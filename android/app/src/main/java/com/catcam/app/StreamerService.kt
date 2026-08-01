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

        // Adaptive bitrate (WiFi PoC). The send queue is the bandwidth probe:
        // drops or a deep queue mean the link is smaller than the stream, so
        // halve the encoder bitrate (down to ABR_FLOOR, then shed frames);
        // a clean queue for ABR_CLEAN_SECS earns a 25% raise back toward
        // VIDEO_BITRATE. On USB the queue never backs up (measured depth 0),
        // so the controller never fires and behavior is bit-identical.
        private const val ABR_FLOOR = 150_000
        private const val ABR_TICK_MS = 1000L
        private const val ABR_CLEAN_SECS = 5

        // Discovery beacon: "CATCAM1 <video-port>" as a UDP broadcast on
        // this port every 2s while the service runs, so the PC tray can
        // find the tablet with no adb, no cable and no typed IPs.
        const val BEACON_PORT = 9001
        private const val BEACON_INTERVAL_MS = 2000L

        @Volatile var preferFrontCamera = true

        // Last user camera choice survives process restarts (tray auto-restart,
        // tablet reboot); the in-memory flag alone resets to front.
        private const val PREFS = "catcam"
        private const val KEY_PREFER_FRONT = "prefer_front_camera"
        private const val KEY_ZOOM_FRONT = "zoom_front"
        private const val KEY_ZOOM_BACK = "zoom_back"
        private const val KEY_TONE_FRONT = "tone_front"
        private const val KEY_TONE_BACK = "tone_back"
        private const val KEY_DAY_MODE = "day_mode"
        private const val KEY_TRANSPORT_WIFI = "transport_wifi"
        private const val KEY_TRANSPORT_GEN = "transport_gen"

        // Multiplicative per tap: even perceived steps across the whole range
        // (1x to this HAL's 10x in ~10 taps). Additive steps feel huge near 1x
        // and useless near max.
        const val ZOOM_STEP = 1.25f

        // Digital zoom, per camera (front = calls, back = cat duty; they want
        // different framing). Applied at the HAL (CONTROL_ZOOM_RATIO): the
        // sensor is cropped BEFORE the downscale to the output streams, so
        // detail survives zooming far better than blowing up delivered frames,
        // and every output (encoder AND preview) zooms in lockstep.
        @Volatile var zoomRatio = 1f
            private set
        @Volatile var zoomMax = 1f
            private set

        // Color tone: small warm/cool bias in [-2, 2] (positive = warm),
        // per camera, applied in the GL pass so the stream and the preview
        // grade identically by construction.
        @Volatile var toneStep = 0
            private set

        // Day/Night capture tuning. Night (default) = the measured lesson-34
        // balance: NR HIGH_QUALITY, EDGE OFF (sharpening turns high-ISO grain
        // into dotted static), temporal blend 0.55. Day = daylight detail:
        // NR FAST, EDGE FAST, no temporal smoothing. Global: it is about the
        // room light, not the camera.
        @Volatile var dayMode = false
            private set

        // Mic level 0..1 (RMS with a sqrt curve so speech sits mid-bar),
        // published ~10/s while streaming; the UI draws it as a small bar.
        @Volatile var audioLevel = 0f
            private set

        // Transport switch. The tablet only REQUESTS a transport: the PC
        // does the connecting, so the request rides the beacon with a
        // change counter and the tray applies it (last user action wins,
        // tablet switch or tray menu). On every accepted connection the
        // request snaps to the ACTUAL transport, so the UI can never claim
        // USB while streaming over Wi-Fi.
        @Volatile var transportWifi = false
            private set
        @Volatile private var transportGen = 0

        // Actual transport of the current connection: loopback source =
        // adb-forwarded USB, LAN source = Wi-Fi. null = no client.
        @Volatile var clientViaWifi: Boolean? = null
            private set

        fun setTransport(ctx: Context, wifi: Boolean) {
            if (wifi == transportWifi) return
            transportWifi = wifi
            transportGen += 1
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putBoolean(KEY_TRANSPORT_WIFI, wifi)
                .putInt(KEY_TRANSPORT_GEN, transportGen)
                .apply()
        }

        fun loadCameraPref(ctx: Context) {
            val sp = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            preferFrontCamera = sp.getBoolean(KEY_PREFER_FRONT, true)
            zoomMax = queryMaxZoom(ctx)
            zoomRatio = sp.getFloat(if (preferFrontCamera) KEY_ZOOM_FRONT else KEY_ZOOM_BACK, 1f)
                .coerceIn(1f, zoomMax)
            toneStep = sp.getInt(if (preferFrontCamera) KEY_TONE_FRONT else KEY_TONE_BACK, 0)
            dayMode = sp.getBoolean(KEY_DAY_MODE, false)
            transportWifi = sp.getBoolean(KEY_TRANSPORT_WIFI, false)
            transportGen = sp.getInt(KEY_TRANSPORT_GEN, 0)
        }

        // UI entry: clamp, persist for this camera, and push into the live
        // capture session if one is running (applies within a frame or two,
        // no restart, no stream blip — dims don't change, the PC never knows).
        fun setZoom(ctx: Context, ratio: Float) {
            zoomRatio = ratio.coerceIn(1f, zoomMax)
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putFloat(if (preferFrontCamera) KEY_ZOOM_FRONT else KEY_ZOOM_BACK, zoomRatio)
                .apply()
            instance?.pushCaptureTuning()
        }

        // Tone lives entirely in the GL pass: no capture-request rebuild.
        fun setTone(ctx: Context, step: Int) {
            toneStep = step.coerceIn(-2, 2)
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putInt(if (preferFrontCamera) KEY_TONE_FRONT else KEY_TONE_BACK, toneStep)
                .apply()
            instance?.glRotator?.setTone(toneStep)
        }

        // Day/Night touches both worlds: HAL (NR/EDGE via the repeating
        // request) and GL (temporal blend weight). Both apply live.
        fun setDayMode(ctx: Context, on: Boolean) {
            dayMode = on
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putBoolean(KEY_DAY_MODE, on).apply()
            instance?.glRotator?.setDayMode(on)
            instance?.pushCaptureTuning()
        }

        // Characteristics query only, no camera open. Both SM-T220 cameras
        // report CONTROL_ZOOM_RATIO_RANGE [1.0, 10.0] (measured 2026-08-01).
        private fun queryMaxZoom(ctx: Context): Float = try {
            val mgr = ctx.getSystemService(Context.CAMERA_SERVICE) as CameraManager
            val facing = if (preferFrontCamera)
                CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK
            var max = 1f
            for (id in mgr.cameraIdList) {
                val ch = mgr.getCameraCharacteristics(id)
                if (ch.get(CameraCharacteristics.LENS_FACING) != facing) continue
                max = if (Build.VERSION.SDK_INT >= 30)
                    ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)?.upper ?: 1f
                else
                    ch.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1f
                break
            }
            max
        } catch (e: Exception) {
            Log.w(TAG, "zoom caps: ${e.message}"); 1f
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

        // True while the GL pipeline mirrors the encoder output into the
        // preview (the "what the PC sees" view): the activity must then leave
        // its TextureView transform at identity and let the GL letterbox rule.
        @Volatile var glPreviewActive = false
            private set

        @Volatile private var instance: StreamerService? = null

        // The activity hands its TextureView surface here (null to detach).
        // A live GL pipeline adopts it on the spot — no session rebuild, no
        // stream blip — and returns true. False: surface is only parked in
        // previewSurface for the next session build (idle, or direct path).
        fun attachPreview(surface: Surface?): Boolean {
            previewSurface = surface
            val rot = instance?.glRotator ?: return false
            rot.setPreviewSurface(surface)
            glPreviewActive = surface != null
            return true
        }
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

    // ABR state. Owned by the sender thread except dropTally (incremented
    // from capture threads on overflow).
    private val dropTally = java.util.concurrent.atomic.AtomicInteger(0)
    private var abrBitrate = VIDEO_BITRATE
    private var abrCleanSecs = 0
    private var abrDivisor = 1

    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var reqBuilder: CaptureRequest.Builder? = null
    private var activeArray: android.graphics.Rect? = null
    // Capability gates read at camera open: never SET what the HAL does not
    // LIST (unlisted values are no-video or a crash on stricter devices).
    private var availNrModes: IntArray? = null
    private var availEdgeModes: IntArray? = null
    private var availAeRanges: Array<Range<Int>>? = null
    private var chosenChars: CameraCharacteristics? = null
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
        thread(name = "CatCamBeacon") { beaconLoop() }
        openCameraAndEncoder()
    }

    private fun stopStreaming() {
        running.set(false)
        out = null
        sendQ.clear()
        glPreviewActive = false
        reqBuilder = null
        audioLevel = 0f
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

    override fun onCreate() { super.onCreate(); instance = this }

    override fun onDestroy() { stopStreaming(); instance = null; super.onDestroy() }

    // ---------------------------------------------------------- capture tuning

    // Everything user-adjustable that lives in the CaptureRequest: zoom and
    // the Day/Night NR+EDGE pair. One owner so the initial request and every
    // live re-push are guaranteed identical.
    private fun applyTuningToRequest(req: CaptureRequest.Builder) {
        // HAL zoom. API 30+ has the first-class control; older devices get
        // the same effect via a centered SCALER_CROP_REGION over the active
        // array.
        val z = zoomRatio.coerceIn(1f, zoomMax)
        if (Build.VERSION.SDK_INT >= 30) {
            req.set(CaptureRequest.CONTROL_ZOOM_RATIO, z)
        } else {
            val arr = activeArray
            if (arr != null) {
                val cw = (arr.width() / z).toInt()
                val ch = (arr.height() / z).toInt()
                val cx = arr.left + (arr.width() - cw) / 2
                val cy = arr.top + (arr.height() - ch) / 2
                req.set(CaptureRequest.SCALER_CROP_REGION,
                    android.graphics.Rect(cx, cy, cx + cw, cy + ch))
            }
        }
        // Night (default) = the measured lesson-34 balance: sensor-stage
        // cleanup beats encoder heroics, and EDGE stays OFF because any
        // sharpening gives high-ISO noise dots hard outlines the encoder
        // then spends bits preserving. Day = daylight detail: light NR keeps
        // texture, EDGE FAST was part of the original working balance.
        // Both are set ONLY when this HAL lists the mode (OFF in particular
        // is often absent on LIMITED-tier devices); otherwise the HAL keeps
        // its default, which beats no video at all.
        val nr = if (dayMode) CaptureRequest.NOISE_REDUCTION_MODE_FAST
                 else CaptureRequest.NOISE_REDUCTION_MODE_HIGH_QUALITY
        if (availNrModes?.contains(nr) == true)
            req.set(CaptureRequest.NOISE_REDUCTION_MODE, nr)
        val edge = if (dayMode) CaptureRequest.EDGE_MODE_FAST
                   else CaptureRequest.EDGE_MODE_OFF
        if (availEdgeModes?.contains(edge) == true)
            req.set(CaptureRequest.EDGE_MODE, edge)
        Log.i(TAG, "tuning: day=$dayMode zoom=${"%.2f".format(java.util.Locale.US, z)} tone=$toneStep")
    }

    private fun pushCaptureTuning() {
        val session = captureSession ?: return
        val req = reqBuilder ?: return
        cameraHandler?.post {
            try {
                applyTuningToRequest(req)
                session.setRepeatingRequest(req.build(), null, cameraHandler)
            } catch (e: Exception) {
                // Session mid-teardown (stop/flip race): harmless, the next
                // session build re-applies the persisted tuning anyway.
                Log.w(TAG, "tuning apply: ${e.message}")
            }
        }
    }

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
                    // Ground truth: loopback = adb-forwarded USB, LAN = Wi-Fi.
                    val viaWifi = client.inetAddress?.isLoopbackAddress == false
                    clientViaWifi = viaWifi
                    // Snap the request switch to reality (bool only, no gen
                    // bump: this is observation, not a user action).
                    if (transportWifi != viaWifi) {
                        transportWifi = viaWifi
                        getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                            .putBoolean(KEY_TRANSPORT_WIFI, viaWifi).apply()
                    }
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
                clientViaWifi = null
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
            dropTally.incrementAndGet()
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
        var lastAbr = lastStat
        // Fresh client, fresh link: start optimistic and let AIMD find it.
        abrReset()
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
                if (now - lastAbr >= ABR_TICK_MS) {
                    lastAbr = now
                    abrTick()
                }
                if (now - lastStat >= 10_000) {
                    Log.i(TAG, "sender: $sent pkts/10s, queue depth ${sendQ.size}, " +
                        "abr ${abrBitrate / 1000}k div=$abrDivisor")
                    sent = 0; lastStat = now
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "sender: ${e.message}")
        }
    }

    // ------------------------------------------------------------------ ABR

    private fun abrReset() {
        dropTally.set(0)
        abrCleanSecs = 0
        if (abrBitrate != VIDEO_BITRATE) applyBitrate(VIDEO_BITRATE)
        if (abrDivisor != 1) applyDivisor(1)
    }

    // AIMD, once per second on the sender thread. Congestion = any overflow
    // drop or a queue past half. Cut bitrate first (halve, floor 150k);
    // only at the floor start shedding frames (30 -> 15 -> 10 fps). Recovery
    // is the mirror: frames first, then bitrate, each step gated on
    // ABR_CLEAN_SECS of clean queue.
    private fun abrTick() {
        val drops = dropTally.getAndSet(0)
        val depth = sendQ.size
        if (drops > 0 || depth > SEND_QUEUE_PACKETS / 2) {
            abrCleanSecs = 0
            when {
                abrBitrate > ABR_FLOOR ->
                    applyBitrate((abrBitrate / 2).coerceAtLeast(ABR_FLOOR))
                abrDivisor < 3 -> applyDivisor(abrDivisor + 1)
                // Floor everywhere and still choking: the link is smaller
                // than the minimum stream; keep limping, nothing left to shed.
            }
        } else if (depth <= 2) {
            if (++abrCleanSecs >= ABR_CLEAN_SECS) {
                abrCleanSecs = 0
                when {
                    abrDivisor > 1 -> applyDivisor(abrDivisor - 1)
                    abrBitrate < VIDEO_BITRATE ->
                        applyBitrate((abrBitrate * 5 / 4).coerceAtMost(VIDEO_BITRATE))
                }
            }
        } else {
            abrCleanSecs = 0
        }
    }

    private fun applyBitrate(b: Int) {
        abrBitrate = b
        try {
            encoder?.setParameters(android.os.Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, b)
            })
            Log.i(TAG, "abr: bitrate -> ${b / 1000}k")
        } catch (e: Exception) {
            // Encoder rejects live bitrate changes: ABR can only log it.
            Log.w(TAG, "abr: bitrate change rejected: ${e.message}")
        }
    }

    private fun applyDivisor(n: Int) {
        abrDivisor = n
        glRotator?.setRenderDivisor(n)
        Log.i(TAG, "abr: frame divisor -> $n (~${VIDEO_FPS / n}fps)")
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
        activeArray = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
        availNrModes = chars.get(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES)
        availEdgeModes = chars.get(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES)
        availAeRanges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
        chosenChars = chars

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

    // Choose a capture size this camera LISTS (an unlisted size is no-video
    // or an exception on stricter HALs). Preference order keeps the measured
    // SM-T220 modes exactly: front = 1600x1200 if listed, else the largest
    // 4:3 near 2MP; back = 1280x720 if listed, else the largest 16:9 up to
    // 1080p-ish; last resort = the largest listed size around 2MP.
    private fun pickCaptureSize(front: Boolean): Pair<Int, Int> {
        val sizes = chosenChars
            ?.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?.getOutputSizes(android.graphics.SurfaceTexture::class.java)
        if (sizes.isNullOrEmpty()) return VIDEO_WIDTH to VIDEO_HEIGHT
        val exact = if (front) sizes.firstOrNull { it.width == 1600 && it.height == 1200 }
                    else sizes.firstOrNull { it.width == VIDEO_WIDTH && it.height == VIDEO_HEIGHT }
        if (exact != null) return exact.width to exact.height
        val wantRatio = if (front) 4f / 3f else 16f / 9f
        val match = sizes.filter {
            Math.abs(it.width.toFloat() / it.height - wantRatio) < 0.05f &&
            it.width * it.height <= 2_100_000
        }.maxByOrNull { it.width * it.height }
        val pick = match
            ?: sizes.filter { it.width * it.height <= 2_200_000 }.maxByOrNull { it.width * it.height }
            ?: sizes.minByOrNull { it.width * it.height }!!
        Log.i(TAG, "capture size negotiated: ${pick.width}x${pick.height} (front=$front)")
        return pick.width to pick.height
    }

    private fun startEncoderAndSession(device: CameraDevice) {
        try {
            // Decide output orientation from how the device is currently held,
            // by the display's ACTUAL shape. The old ROTATION_0-means-portrait
            // shortcut was only true on portrait-natural hardware; a
            // landscape-natural tablet (Pixel Tablet, most 10-inch devices)
            // reports ROTATION_0 while lying sideways.
            val dm = getSystemService(Context.DISPLAY_SERVICE) as android.hardware.display.DisplayManager
            val disp = dm.getDisplay(android.view.Display.DEFAULT_DISPLAY)
            val dispRot = disp?.rotation ?: Surface.ROTATION_0
            val metrics = android.util.DisplayMetrics()
            @Suppress("DEPRECATION") disp?.getRealMetrics(metrics)
            val heldPortrait = metrics.heightPixels >= metrics.widthPixels

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
                try {
                    enc.configure(makeFormat(false), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                } catch (e2: Exception) {
                    // Third rung: some vendor encoders reject VBR or the
                    // quality keys wholesale. Baseline keys only, which the
                    // CDD guarantees for an AVC encoder.
                    Log.w(TAG, "Tuned format rejected (${e2.message}), using minimal format")
                    try { enc.release() } catch (_: Exception) {}
                    enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
                    val minimal = MediaFormat.createVideoFormat(
                        MediaFormat.MIMETYPE_VIDEO_AVC, encWidth, encHeight).apply {
                        setInteger(MediaFormat.KEY_COLOR_FORMAT,
                            MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                        setInteger(MediaFormat.KEY_BIT_RATE, VIDEO_BITRATE)
                        setInteger(MediaFormat.KEY_FRAME_RATE, VIDEO_FPS)
                        setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, VIDEO_IFRAME_SEC)
                    }
                    enc.configure(minimal, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                }
            }
            val codecInput: Surface = enc.createInputSurface()
            enc.start()
            encoder = enc

            // GL rotation pipeline camera->encoder. The capture size is
            // NEGOTIATED against what this camera actually lists (an unlisted
            // size is no-video or an exception on stricter HALs): front
            // prefers the full 4:3 sensor near 2MP (real detail downscaled
            // beats a pre-cropped feed, measured), back prefers 16:9 720p.
            // On the SM-T220 this resolves to the exact previous values
            // (1600x1200 front, 1280x720 back).
            val (capW, capH) = pickCaptureSize(usingFrontCamera)
            val captureTarget: Surface = if (heldPortrait) {
                val rot = GLRotator(codecInput, encWidth, encHeight)
                rot.setInputBufferSize(capW, capH)
                rot.setTransform(rotateDeg, mirror = mirrorFrame)
                // Orientation is classified from the live stMatrix on the
                // first frame (PREROT keeps the measured SM-T220 geometry;
                // STANDARD derives the textbook rotation).
                val dispDeg = when (dispRot) {
                    Surface.ROTATION_90 -> 90; Surface.ROTATION_180 -> 180
                    Surface.ROTATION_270 -> 270; else -> 0
                }
                rot.setOrientationHints(sensorOrientation, dispDeg, usingFrontCamera)
                rot.inputSurfaceTexture.setOnFrameAvailableListener(
                    { rot.renderFrame() }, cameraHandler)
                glRotator = rot
                rot.setTone(toneStep)
                rot.setDayMode(dayMode)
                // Preview = the encoder's exact output (same rotation, crop,
                // denoise, zoom, tone — what the PC sees), mirrored by the
                // rotator. The old separate HAL preview target had its own
                // geometry and showed framing the PC never got (e.g. the full
                // 4:3 front sensor while the PC gets the 9:16 center-crop).
                previewSurface?.let { rot.setPreviewSurface(it) }
                glPreviewActive = previewSurface != null
                Surface(rot.inputSurfaceTexture)
            } else codecInput

            // Only the direct/landscape path still feeds the TextureView from
            // the HAL; in GL mode the rotator owns the preview.
            val halPreview = if (glRotator == null) previewSurface else null

            device.createCaptureSession(
                listOfNotNull(captureTarget, halPreview),
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(session: CameraCaptureSession) {
                        captureSession = session
                        try {
                        val req = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                            addTarget(captureTarget)
                            halPreview?.let { addTarget(it) }
                            set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                            // NR/EDGE live in applyTuningToRequest (Day/Night).
                            // Brightness vs motion-smoothness dial, per camera
                            // (measured: AE floor 15 in a dark room ran the
                            // sensor at ~17fps and 41% of delivered frames
                            // were duplicates = visible judder in calls).
                            // Front = face calls: floor 24, near-smooth motion,
                            // modestly darker. Back = cat duty: floor 15, max
                            // brightness, judder irrelevant for a sleeping cat.
                            // Clamped to the ranges this HAL actually lists
                            // (on the SM-T220 both wanted ranges are listed,
                            // so behavior is unchanged there).
                            val wanted = Range(if (usingFrontCamera) 24 else 15, VIDEO_FPS)
                            val ae = availAeRanges?.minByOrNull { r ->
                                Math.abs(r.lower - wanted.lower) * 2 + Math.abs(r.upper - wanted.upper)
                            } ?: wanted
                            set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, ae)
                        }
                        applyTuningToRequest(req)
                        reqBuilder = req
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

    // ---------------------------------------------------------------- beacon

    // Broadcast to every up interface's broadcast address (falls back to the
    // global broadcast). Failures are quiet: WiFi being off just means the
    // tray discovers nothing until it is back.
    private fun beaconLoop() {
        var sock: java.net.DatagramSocket? = null
        try {
            sock = java.net.DatagramSocket().apply { broadcast = true }
            while (running.get()) {
                // Recomputed per send: the transport request rides along
                // ("CATCAM1 <port> <usb|wifi> <gen>"; the gen counter lets
                // the tray tell a new user action from a repeated beacon).
                val payload = ("CATCAM1 $PORT " +
                    (if (transportWifi) "wifi" else "usb") + " $transportGen").toByteArray()
                try {
                    val targets = ArrayList<java.net.InetAddress>()
                    java.net.NetworkInterface.getNetworkInterfaces()?.let { nis ->
                        for (ni in nis) {
                            if (!ni.isUp || ni.isLoopback) continue
                            for (ia in ni.interfaceAddresses) ia.broadcast?.let { targets.add(it) }
                        }
                    }
                    if (targets.isEmpty())
                        targets.add(java.net.InetAddress.getByName("255.255.255.255"))
                    for (t in targets)
                        sock.send(java.net.DatagramPacket(payload, payload.size, t, BEACON_PORT))
                } catch (_: Exception) {
                }
                Thread.sleep(BEACON_INTERVAL_MS)
            }
        } catch (e: Exception) {
            Log.w(TAG, "beacon: ${e.message}")
        } finally {
            try { sock?.close() } catch (_: Exception) {}
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

    // Mic level for the UI bar: math on the chunk already in hand, ~10/s,
    // never touches the capture cadence. Logged 1/s: "no audio" complaints
    // are diagnosable from logcat alone (the audio path fails silently
    // otherwise, lesson 33).
    private var lastLevelLogMs = 0L
    private fun publishLevel(chunk: ByteArray, n: Int) {
        var sum = 0L
        var i = 0
        while (i + 1 < n) {
            // PCM 16-bit little-endian
            val s = ((chunk[i + 1].toInt() shl 8) or (chunk[i].toInt() and 0xff)).toShort().toInt()
            sum += s.toLong() * s
            i += 2
        }
        val rms = kotlin.math.sqrt(sum.toDouble() / (n / 2))
        // dB meter, 50dB window: speech RMS (~1k-8k = -30..-12 dBFS) lands
        // mid-to-high bar. The first cut used sqrt(amplitude), which put
        // LOUD speech at ~25% of the bar (measured, user feedback 2026-08-01).
        val db = 20.0 * kotlin.math.log10(rms.coerceAtLeast(1.0) / 32768.0)
        // 1.4 visual gain on the window: loud speech ~70% like consumer
        // meters, not the technically-correct-but-flat 50%.
        val instant = (((db + 50.0) / 50.0) * 1.4).toFloat().coerceIn(0f, 1f)
        // Fast attack, slow decay (~0.8s full fall): speech pumps the bar
        // instead of flickering it at chunk rate.
        audioLevel = kotlin.math.max(instant, audioLevel * 0.75f)
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - lastLevelLogMs >= 1000) {
            lastLevelLogMs = now
            Log.i(TAG, "mic level=${"%.2f".format(java.util.Locale.US, audioLevel)}")
        }
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
                    publishLevel(chunk, n)
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
