package com.catcam.app

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.AudioAttributes
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaRecorder
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import android.util.Range
import android.view.Surface
import androidx.core.app.ActivityCompat
import java.io.DataOutputStream
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.LinkedBlockingDeque
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * CatCam streaming service.
 *
 * Camera2 -> MediaCodec (H.264) --\
 *                                 +--> length-prefixed TCP mux on :9000
 * AudioRecord (PCM 16k mono) ----/
 *
 * Wire protocol (all multi-byte ints big-endian):
 *   [1 byte type][4 byte length][payload]
 *   type 0x01 = video config (SPS+PPS from MediaCodec INFO_OUTPUT_FORMAT_CHANGED, raw csd-0/csd-1)
 *   type 0x02 = video frame (Annex-B-ish raw MediaCodec output buffer)
 *   type 0x03 = audio PCM chunk (16kHz, 16-bit, mono)
 *
 * The PC connects to us. USB mode: `adb forward tcp:9000 tcp:9000`, PC connects to localhost:9000.
 * WiFi mode: PC connects to <tablet-ip>:9000.
 */
class StreamerService : Service() {

    companion object {
        private const val TAG = "CatCam"
        const val PORT = 9000
        const val ACTION_START = "com.catcam.app.START"
        const val ACTION_STOP = "com.catcam.app.STOP"
        private const val CHANNEL_ID = "catcam_stream"

        // Tunables
        private const val VIDEO_WIDTH = 1280
        private const val VIDEO_HEIGHT = 720
        private const val VIDEO_FPS = 30
        private const val VIDEO_BITRATE = 10_000_000 // USB transport: bandwidth is
        // free, and dark/noisy scenes fall apart below ~8M at 720x1280x30
        private const val VIDEO_IFRAME_SEC = 2
        private const val AUDIO_RATE = 16000
        // ~2s of stream (30 video + 10 audio pkts/s). The queue exists so
        // the network can NEVER back-pressure capture (HANDOFF lesson 37).
        private const val SEND_QUEUE_PACKETS = 60

        @Volatile var preferFrontCamera = true

        // Last user camera choice survives process restarts (tray auto-restart,
        // tablet reboot); the in-memory flag alone resets to front.
        private const val PREFS = "catcam"
        private const val KEY_PREFER_FRONT = "prefer_front_camera"

        fun loadCameraPref(ctx: Context) {
            preferFrontCamera = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getBoolean(KEY_PREFER_FRONT, true)
        }

        fun saveCameraPref(ctx: Context) {
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit().putBoolean(KEY_PREFER_FRONT, preferFrontCamera).apply()
        }

        @Volatile var clientConnected = false
            private set
        @Volatile var statusText = "Idle"
            private set

        // Preview surface owned by MainActivity's TextureView; null when app backgrounded.
        @Volatile var previewSurface: Surface? = null
        @Volatile var sensorOrientation: Int = 90
        @Volatile var usingFrontCamera: Boolean = false
    }

    private val running = AtomicBoolean(false)
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var serverSocket: ServerSocket? = null
    @Volatile private var out: DataOutputStream? = null

    // All socket writes happen on ONE sender (the server thread); capture
    // and encode only enqueue. A blocking write on the capture path over
    // WiFi wedged the whole chain (lesson 37): drainEncoder stopped
    // dequeuing, the encoder ran out of buffers, GLRotator blocked in
    // eglSwapBuffers, the camera HAL starved and the CameraDevice died.
    private val sendQ = LinkedBlockingDeque<Pair<Byte, ByteArray>>(SEND_QUEUE_PACKETS)
    @Volatile private var lastSyncReqMs = 0L
    @Volatile private var droppedVideo = false

    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var encoder: MediaCodec? = null
    @Volatile private var lastConfig: ByteArray? = null
    private var encWidth = VIDEO_WIDTH
    private var encHeight = VIDEO_HEIGHT
    private var glRotator: GLRotator? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> { stopStreaming(); stopSelf(); return START_NOT_STICKY }
            else -> startStreaming()
        }
        return START_STICKY
    }

    private fun startStreaming() {
        if (!running.compareAndSet(false, true)) return
        loadCameraPref(this)
        Log.i(TAG, "Starting streamer")
        startForegroundWithNotification()
        acquireWakeLock()

        thread(name = "CatCamServer") { serverLoop() }
        thread(name = "CatCamAudio") { audioLoop() }
        openCameraAndEncoder()
    }

    private fun stopStreaming() {
        running.set(false)
        out = null
        sendQ.clear()
        try { serverSocket?.close() } catch (_: Exception) {}
        try { captureSession?.close() } catch (_: Exception) {}
        try { cameraDevice?.close() } catch (_: Exception) {}
        try { glRotator?.release() } catch (_: Exception) {}
        glRotator = null
        try { encoder?.stop(); encoder?.release() } catch (_: Exception) {}
        cameraThread?.quitSafely()
        wakeLock?.let { if (it.isHeld) it.release() }
        wifiLock?.let { if (it.isHeld) it.release() }
        clientConnected = false
        statusText = "Idle"
    }

    override fun onDestroy() { stopStreaming(); super.onDestroy() }

    // ---------------------------------------------------------------- server

    private fun serverLoop() {
        while (running.get()) {
            try {
                statusText = "Waiting for PC on :$PORT"
                ServerSocket(PORT).use { server ->
                    serverSocket = server
                    Log.i(TAG, "Listening on :$PORT")
                    val client: Socket = server.accept()
                    client.tcpNoDelay = true
                    Log.i(TAG, "PC connected: ${client.inetAddress}")
                    val o = DataOutputStream(client.getOutputStream())
                    sendQ.clear()
                    out = o
                    clientConnected = true
                    statusText = "Streaming to ${client.inetAddress.hostAddress}"
                    // New client joined mid-GOP: re-send SPS/PPS and force a keyframe
                    // so the decoder has a valid starting point.
                    resendConfigAndSyncFrame()
                    // This thread becomes the single sender until the socket
                    // dies. Blocking HERE is fine: only the network waits on
                    // the network; capture keeps producing into the queue.
                    senderLoop(o)
                }
            } catch (e: Exception) {
                Log.w(TAG, "serverLoop: ${e.message}")
            } finally {
                out = null
                clientConnected = false
                if (running.get()) statusText = "Waiting for PC on :$PORT"
            }
        }
    }

    fun notifyClientGone() {
        clientConnected = false
        try { serverSocket?.close() } catch (_: Exception) {}
    }

    // Re-send SPS/PPS and force the encoder to emit an IDR immediately,
    // so a client that connected mid-GOP gets a decodable stream.
    private fun resendConfigAndSyncFrame() {
        lastConfig?.let { sendPacket(0x01, it) }
        try {
            encoder?.setParameters(android.os.Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            })
            Log.i(TAG, "Requested sync frame for new client")
        } catch (e: Exception) {
            Log.w(TAG, "sync frame request failed: ${e.message}")
        }
    }

    // Enqueue only, never blocks on the network (lesson 37). Overflow means
    // the link is slower than the stream right now: video frames drop (the
    // client's decoder is then mid-GOP broken, so ask for a sync frame,
    // rate-limited) and audio just gaps. A config packet supersedes
    // everything still queued: without it later packets are undecodable.
    private fun sendPacket(type: Byte, payload: ByteArray) {
        if (out == null) return
        if (type == 0x01.toByte()) {
            sendQ.clear()
            sendQ.offerFirst(type to payload)
            return
        }
        if (!sendQ.offerLast(type to payload) && type == 0x02.toByte()) {
            // Don't request the sync frame NOW: while the queue is full a
            // fresh IDR would drop too, and each request fattens the stream
            // (measured on a 20KB/s link: 1/s requests turned every frame
            // into a ~100KB IDR, one whole frame arrived per ~30s). Flag it;
            // the sender asks once the queue has drained room for an IDR.
            droppedVideo = true
        }
    }

    private fun requestSyncFrameRateLimited() {
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - lastSyncReqMs < 1000) return
        lastSyncReqMs = now
        try {
            encoder?.setParameters(android.os.Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            })
            Log.i(TAG, "send queue overflow: requested sync frame")
        } catch (_: Exception) {}
    }

    // The single owner of socket writes; runs on the server thread for the
    // lifetime of one client connection. Death is detected by a write
    // throwing (audio enqueues 10/s, so traffic is guaranteed).
    private fun senderLoop(o: DataOutputStream) {
        var sent = 0
        var lastStat = android.os.SystemClock.elapsedRealtime()
        try {
            while (running.get() && clientConnected) {
                val pkt = sendQ.pollFirst(500, TimeUnit.MILLISECONDS) ?: continue
                o.writeByte(pkt.first.toInt())
                o.writeInt(pkt.second.size)
                o.write(pkt.second)
                o.flush()
                sent++
                if (droppedVideo && sendQ.size < SEND_QUEUE_PACKETS / 4) {
                    droppedVideo = false
                    requestSyncFrameRateLimited()
                }
                val now = android.os.SystemClock.elapsedRealtime()
                if (now - lastStat >= 10_000) {
                    Log.i(TAG, "sender: $sent pkts/10s, queue depth ${sendQ.size}")
                    sent = 0; lastStat = now
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "sender: ${e.message}")
        }
    }

    // ---------------------------------------------------------------- camera

    private fun openCameraAndEncoder() {
        cameraThread = HandlerThread("CatCamCamera").also { it.start() }
        cameraHandler = Handler(cameraThread!!.looper)

        val mgr = getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val wantedFacing = if (preferFrontCamera)
            CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK
        var chosenId = mgr.cameraIdList.first()
        for (id in mgr.cameraIdList) {
            val ch = mgr.getCameraCharacteristics(id)
            if (ch.get(CameraCharacteristics.LENS_FACING) == wantedFacing) { chosenId = id; break }
        }
        val chars = mgr.getCameraCharacteristics(chosenId)
        sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
        usingFrontCamera = wantedFacing == CameraCharacteristics.LENS_FACING_FRONT

        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED) {
            Log.e(TAG, "Camera permission not granted")
            statusText = "Camera permission missing"
            return
        }

        mgr.openCamera(chosenId, object : CameraDevice.StateCallback() {
            override fun onOpened(device: CameraDevice) {
                cameraDevice = device
                startEncoderAndSession(device)
            }
            override fun onDisconnected(device: CameraDevice) { device.close() }
            override fun onError(device: CameraDevice, error: Int) {
                Log.e(TAG, "Camera error $error"); device.close()
            }
        }, cameraHandler)
    }

    private fun startEncoderAndSession(device: CameraDevice) {
        try {
            // Decide output orientation from how the tablet is currently held.
            // Portrait hold -> encode 720x1280 (video itself rotated via GLRotator).
            val dm = getSystemService(Context.DISPLAY_SERVICE) as android.hardware.display.DisplayManager
            val dispRot = dm.getDisplay(android.view.Display.DEFAULT_DISPLAY)?.rotation ?: Surface.ROTATION_0
            val heldPortrait = dispRot == Surface.ROTATION_0 || dispRot == Surface.ROTATION_180

            encWidth = if (heldPortrait) 720 else 1280
            encHeight = if (heldPortrait) 1280 else 720
            // Measured via dumped PC frames (lessons 21 and 24, HANDOFF.md):
            // on SM-T220 the stMatrix already delivers portrait-upright content
            // at rotate 0 for BOTH cameras; added rotation only spins it
            // (90/180/270 progressed RIGHT/DOWN/LEFT). The old back-cam
            // "0 -> hair RIGHT" rows were stale dumps, see lesson 24.
            val rotateDeg = 0
            // Both cameras unmirrored in the GL pass (lesson 31):
            // - Back: true-to-scene, text readable (lesson 24).
            // - Front: the stMatrix pure rotation ALREADY renders mirror-feel
            //   (look right, image looks right). The old extra mirror flipped
            //   it to text-readable, which was calibrated on chair text, the
            //   wrong target for a selfie view (user preference, 2026-07-26).
            val mirrorFrame = false

            fun makeFormat(highProfile: Boolean) =
                MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, encWidth, encHeight).apply {
                    setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                    setInteger(MediaFormat.KEY_BIT_RATE, VIDEO_BITRATE)
                    setInteger(MediaFormat.KEY_BITRATE_MODE,
                        MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
                    setInteger(MediaFormat.KEY_FRAME_RATE, VIDEO_FPS)
                    // 2s keyframes: I-frames are expensive, and each one steals
                    // bits from detail. 1s was never intended (unused constant).
                    setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, VIDEO_IFRAME_SEC)
                    // Latency/quality balance (2026-07-26): LATENCY 1, not
                    // 0. Zero strips all encoder lookahead for ~33ms saved,
                    // invisible on this chain; one frame of slack buys back
                    // rate-control quality. PRIORITY stays realtime.
                    setInteger(MediaFormat.KEY_LATENCY, 1)
                    setInteger(MediaFormat.KEY_PRIORITY, 0)
                    setInteger(MediaFormat.KEY_OPERATING_RATE, VIDEO_FPS)
                    // Cap the ugliest quantizers under VBR; silently ignored
                    // where unsupported.
                    setInteger(MediaFormat.KEY_VIDEO_QP_MAX, 36)
                    if (highProfile) {
                        // CABAC etc: ~10-15% better quality per bit when the
                        // vendor encoder accepts it.
                        setInteger(MediaFormat.KEY_PROFILE,
                            MediaCodecInfo.CodecProfileLevel.AVCProfileHigh)
                        setInteger(MediaFormat.KEY_LEVEL,
                            MediaCodecInfo.CodecProfileLevel.AVCLevel31)
                    }
                }
            var enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            try {
                enc.configure(makeFormat(true), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                Log.i(TAG, "Encoder configured: High profile, ${VIDEO_BITRATE / 1_000_000}Mbps VBR")
            } catch (e: Exception) {
                // Budget SoCs can reject High profile at configure time; a
                // failed configure can leave vendor codecs sour, so recreate.
                Log.w(TAG, "High profile rejected (${e.message}), using encoder default")
                try { enc.release() } catch (_: Exception) {}
                enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
                enc.configure(makeFormat(false), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            }
            val codecInput: Surface = enc.createInputSurface()
            enc.start()
            encoder = enc

            // GL rotation pipeline camera->encoder. Needed whenever portrait
            // even at rotateDeg 0: handles aspect-crop into 720x1280 + mirror.
            // Front cam: request the FULL 4:3 sensor (1600x1200; the 16:9
            // 1280x720 we used to ask for is not even a native mode, the HAL
            // synthesized it by discarding sensor rows). GLRotator center-crops
            // portrait 9:16 from the full array, so the encoder's 720x1280 is
            // built by DOWNSCALING real detail (plus mild supersampling
            // denoise) instead of enlarging a pre-cropped feed. Back cam keeps
            // 1280x720 (cat duty, untouched tonight).
            val capW = if (usingFrontCamera) 1600 else VIDEO_WIDTH
            val capH = if (usingFrontCamera) 1200 else VIDEO_HEIGHT
            val captureTarget: Surface = if (heldPortrait) {
                val rot = GLRotator(codecInput, encWidth, encHeight)
                rot.setInputBufferSize(capW, capH)
                rot.setTransform(rotateDeg, mirror = mirrorFrame)
                rot.inputSurfaceTexture.setOnFrameAvailableListener(
                    { rot.renderFrame() }, cameraHandler)
                glRotator = rot
                Surface(rot.inputSurfaceTexture)
            } else codecInput

            device.createCaptureSession(
                listOfNotNull(captureTarget, previewSurface),
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(session: CameraCaptureSession) {
                        captureSession = session
                        try {
                        val req = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                            addTarget(captureTarget)
                            previewSurface?.let { addTarget(it) }
                            set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                            // Quality pass: sensor-stage cleanup beats encoder
                            // heroics, especially on the 2MP front cam at night.
                            set(CaptureRequest.NOISE_REDUCTION_MODE,
                                CaptureRequest.NOISE_REDUCTION_MODE_HIGH_QUALITY)
                            // EDGE OFF (was HIGH_QUALITY -> FAST -> OFF, review
                            // consensus): on a 2MP sensor at night there is no
                            // fine detail to enhance, only grain, and any
                            // sharpening gives noise dots hard outlines the
                            // encoder then spends bits preserving.
                            set(CaptureRequest.EDGE_MODE,
                                CaptureRequest.EDGE_MODE_OFF)
                            // Brightness vs motion-smoothness dial, per camera
                            // (measured: AE floor 15 in a dark room ran the
                            // sensor at ~17fps and 41% of delivered frames
                            // were duplicates = visible judder in calls).
                            // Front = face calls: floor 24, near-smooth motion,
                            // modestly darker. Back = cat duty: floor 15, max
                            // brightness, judder irrelevant for a sleeping cat.
                            set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
                                Range(if (usingFrontCamera) 24 else 15, VIDEO_FPS))
                        }
                        session.setRepeatingRequest(req.build(), null, cameraHandler)
                        thread(name = "CatCamEncoder") { drainEncoder(enc) }
                        Log.i(TAG, "Camera+encoder running ${encWidth}x$encHeight@$VIDEO_FPS rot=$rotateDeg")
                        } catch (e: Exception) {
                            // Fast stop/start races this callback against the
                            // camera closing ("CameraDevice was already
                            // closed", crashed the process 2026-07-26,
                            // lesson 37). A dead device here is survivable:
                            // the next service start rebuilds everything.
                            Log.w(TAG, "onConfigured raced camera close: ${e.message}")
                            statusText = "Camera closed during setup"
                        }
                    }
                    override fun onConfigureFailed(session: CameraCaptureSession) {
                        Log.e(TAG, "Capture session configure failed")
                        statusText = "Camera session failed"
                    }
                },
                cameraHandler
            )
        } catch (e: Exception) {
            Log.e(TAG, "startEncoderAndSession", e)
            statusText = "Encoder error: ${e.message}"
        }
    }

    private fun drainEncoder(enc: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (running.get()) {
            try {
                when (val idx = enc.dequeueOutputBuffer(info, 100_000)) {
                    MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        // Extract SPS/PPS and send as config packet
                        val fmt = enc.outputFormat
                        val sps = fmt.getByteBuffer("csd-0")
                        val pps = fmt.getByteBuffer("csd-1")
                        if (sps != null && pps != null) {
                            val s = ByteArray(sps.remaining()).also { sps.get(it) }
                            val p = ByteArray(pps.remaining()).also { pps.get(it) }
                            // Wire config: [w:4][h:4][SPS][PPS] so the PC can size its pipeline
                            val dim = java.nio.ByteBuffer.allocate(8)
                                .putInt(encWidth).putInt(encHeight).array()
                            lastConfig = dim + s + p
                            sendPacket(0x01, dim + s + p)
                            Log.i(TAG, "Sent config: ${encWidth}x$encHeight SPS ${s.size}B PPS ${p.size}B")
                        }
                    }
                    else -> if (idx >= 0) {
                        val buf = enc.getOutputBuffer(idx)!!
                        val data = ByteArray(info.size)
                        buf.position(info.offset); buf.limit(info.offset + info.size)
                        buf.get(data)
                        sendPacket(0x02, data)
                        enc.releaseOutputBuffer(idx, false)
                    }
                }
            } catch (e: Exception) {
                if (running.get()) Log.w(TAG, "drainEncoder: ${e.message}")
            }
        }
    }

    // ---------------------------------------------------------------- audio

    // Wired monitor (wired-jack path): while a 3.5mm
    // cable is plugged the mic PCM ALSO renders to the jack, and an aux
    // cable into the PC's mic-in makes the tablet mic a real, selectable
    // Windows input with zero drivers. No cable = no render, and the track
    // is pinned to the wired device, so the tablet speaker can never carry
    // the mic (feedback-safe even if routing glitches).
    @Volatile private var wiredDev: AudioDeviceInfo? = null
    private var jackTrack: AudioTrack? = null
    private var deviceCb: AudioDeviceCallback? = null

    private fun isWiredOut(d: AudioDeviceInfo) =
        d.type == AudioDeviceInfo.TYPE_WIRED_HEADSET ||
        d.type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES ||
        d.type == AudioDeviceInfo.TYPE_LINE_ANALOG ||
        d.type == AudioDeviceInfo.TYPE_AUX_LINE

    private fun watchWiredOut() {
        val am = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        wiredDev = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull { isWiredOut(it) }
        deviceCb = object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>) {
                added.firstOrNull { isWiredOut(it) }?.let { wiredDev = it }
            }
            override fun onAudioDevicesRemoved(removed: Array<out AudioDeviceInfo>) {
                if (removed.any { it.id == wiredDev?.id }) wiredDev = null
            }
        }
        am.registerAudioDeviceCallback(deviceCb, null)
    }

    private fun renderToJack(chunk: ByteArray, n: Int) {
        val dev = wiredDev
        if (dev == null) {
            jackTrack?.let { try { it.stop(); it.release() } catch (_: Exception) {} }
            jackTrack = null
            return
        }
        var t = jackTrack
        if (t == null || t.preferredDevice?.id != dev.id) {
            jackTrack?.release()
            t = AudioTrack.Builder()
                .setAudioAttributes(AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                .setAudioFormat(AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(AUDIO_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
                .setBufferSizeInBytes(3200 * 4)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            t.preferredDevice = dev
            t.play()
            jackTrack = t
            Log.i(TAG, "Wired monitor ON -> ${dev.productName}")
        }
        // Non-blocking: a full render buffer drops audio locally rather than
        // ever stalling the capture/stream cadence.
        t.write(chunk, 0, n, AudioTrack.WRITE_NON_BLOCKING)
    }

    private fun audioLoop() {
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
            Log.e(TAG, "Mic permission not granted"); return
        }
        val minBuf = AudioRecord.getMinBufferSize(
            AUDIO_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val rec = AudioRecord(
            MediaRecorder.AudioSource.MIC, AUDIO_RATE,
            AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, minBuf * 2)
        val chunk = ByteArray(3200) // 100ms @ 16kHz 16-bit mono
        try {
            watchWiredOut()
            rec.startRecording()
            Log.i(TAG, "Audio recording @${AUDIO_RATE}Hz")
            while (running.get()) {
                val n = rec.read(chunk, 0, chunk.size)
                if (n > 0) {
                    sendPacket(0x03, chunk.copyOf(n))
                    renderToJack(chunk, n)
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "audioLoop: ${e.message}")
        } finally {
            try { rec.stop(); rec.release() } catch (_: Exception) {}
            try { jackTrack?.stop(); jackTrack?.release() } catch (_: Exception) {}
            jackTrack = null
            deviceCb?.let {
                (getSystemService(Context.AUDIO_SERVICE) as AudioManager)
                    .unregisterAudioDeviceCallback(it)
            }
            deviceCb = null
        }
    }

    // ------------------------------------------------------------- plumbing

    private fun startForegroundWithNotification() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "CatCam streaming", NotificationManager.IMPORTANCE_LOW))

        val stopIntent = PendingIntent.getService(
            this, 0,
            Intent(this, StreamerService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE)

        val notif: Notification = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("CatCam")
            .setContentText("Streaming camera + mic to PC")
            .setSmallIcon(android.R.drawable.presence_video_online)
            .addAction(Notification.Action.Builder(null, "Stop", stopIntent).build())
            .setOngoing(true)
            .build()

        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(1, notif,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
        } else {
            startForeground(1, notif)
        }
    }

    private fun acquireWakeLock() {
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        // SCREEN_BRIGHT (not PARTIAL): while streaming the tablet behaves
        // like a video player, screen on at full brightness, never dims,
        // never sleeps, app minimized or not. The screen never turning off
        // also means the keyguard never auto-engages. Deprecated API but the
        // only app-level way to hold the screen from a service; still works.
        @Suppress("DEPRECATION")
        wakeLock = pm.newWakeLock(
            PowerManager.SCREEN_BRIGHT_WAKE_LOCK or PowerManager.ON_AFTER_RELEASE,
            "catcam:stream")
            .also { it.acquire(12 * 60 * 60 * 1000L) } // 12h cap, re-acquired on restart
        // WiFi transport: without a low-latency lock the radio power-saves
        // between beacons and every burst eats 100ms+ stalls (lesson 37).
        // Harmless in USB mode.
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "catcam:wifi")
            .also { it.acquire() }
    }
}
