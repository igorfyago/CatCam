package com.catcam.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.view.Surface
import android.view.TextureView
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

/**
 * Control surface: live preview (only while app is in foreground), status,
 * Start/Stop, front/back flip. Streaming itself lives in StreamerService.
 */
class MainActivity : AppCompatActivity(), TextureView.SurfaceTextureListener {

    private val perms = mutableListOf(
        Manifest.permission.CAMERA,
        Manifest.permission.RECORD_AUDIO,
    ).apply {
        if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
    }.toTypedArray()

    private lateinit var status: TextView
    private lateinit var startBtn: Button
    private lateinit var stopBtn: Button
    private lateinit var flipBtn: Button
    private lateinit var zoomInBtn: Button
    private lateinit var zoomOutBtn: Button
    private lateinit var zoomLabel: TextView
    private lateinit var toneCoolBtn: Button
    private lateinit var toneWarmBtn: Button
    private lateinit var toneLabel: TextView
    private lateinit var dayNightBtn: Button
    private lateinit var audioBar: android.widget.ProgressBar
    private lateinit var preview: TextureView

    private val handler = Handler(Looper.getMainLooper())

    // The mic bar needs to feel live: 100ms, ~matching the 10/s PCM chunk
    // rate. Kept separate from the 500ms state ticker on purpose.
    private val levelTicker = object : Runnable {
        override fun run() {
            audioBar.progress = (StreamerService.audioLevel * 100).toInt()
            handler.postDelayed(this, 100)
        }
    }

    private val ticker = object : Runnable {
        override fun run() {
            val streaming = StreamerService.statusText != "Idle"
            status.text = "${StreamerService.statusText}\n" +
                (if (StreamerService.clientConnected) "PC connected" else "No PC connected")
            startBtn.isEnabled = !streaming
            stopBtn.isEnabled = streaming
            flipBtn.text = if (StreamerService.preferFrontCamera) "Front" else "Back"
            updateZoomLabel()
            updateTuningLabels()
            if (StreamerService.glPreviewActive) {
                // GL letterboxes the encoder frame into the view (the "what
                // the PC sees" preview); the matrix must not fight it.
                preview.setTransform(Matrix())
            } else {
                applyPreviewTransform()
            }
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        // Foreground belt for the service's SCREEN_BRIGHT wake lock.
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        status = findViewById(R.id.status)
        startBtn = findViewById(R.id.start)
        stopBtn = findViewById(R.id.stop)
        flipBtn = findViewById(R.id.flip)
        zoomOutBtn = findViewById(R.id.zoom_out)
        zoomInBtn = findViewById(R.id.zoom_in)
        zoomLabel = findViewById(R.id.zoom_label)
        toneCoolBtn = findViewById(R.id.tone_cool)
        toneWarmBtn = findViewById(R.id.tone_warm)
        toneLabel = findViewById(R.id.tone_label)
        dayNightBtn = findViewById(R.id.day_night)
        audioBar = findViewById(R.id.audio_level)
        preview = findViewById(R.id.preview)
        preview.surfaceTextureListener = this
        StreamerService.loadCameraPref(this)
        updateZoomLabel()
        updateTuningLabels()

        startBtn.setOnClickListener {
            if (hasPerms()) startStreaming() else
                ActivityCompat.requestPermissions(this, perms, 1)
        }
        stopBtn.setOnClickListener {
            startService(Intent(this, StreamerService::class.java).setAction(StreamerService.ACTION_STOP))
        }
        flipBtn.setOnClickListener {
            StreamerService.preferFrontCamera = !StreamerService.preferFrontCamera
            StreamerService.saveCameraPref(this)
            // Each camera keeps its own zoom and tone (calls vs cat duty want
            // different framing): pull the new camera's saved values.
            StreamerService.loadCameraPref(this)
            updateZoomLabel()
            updateTuningLabels()
            if (StreamerService.statusText != "Idle") {
                startService(Intent(this, StreamerService::class.java).setAction(StreamerService.ACTION_STOP))
                handler.postDelayed({ startStreaming() }, 800)
            }
        }
        zoomOutBtn.setOnClickListener { stepZoom(1f / StreamerService.ZOOM_STEP) }
        zoomInBtn.setOnClickListener { stepZoom(StreamerService.ZOOM_STEP) }
        toneCoolBtn.setOnClickListener { stepTone(-1) }
        toneWarmBtn.setOnClickListener { stepTone(+1) }
        dayNightBtn.setOnClickListener {
            StreamerService.setDayMode(this, !StreamerService.dayMode)
            updateTuningLabels()
        }

        requestBatteryExemption()
        handler.post(ticker)
        handler.post(levelTicker)
    }

    private fun hasPerms() = perms.all {
        ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (hasPerms()) startStreaming()
    }

    private fun startStreaming() {
        val i = Intent(this, StreamerService::class.java).setAction(StreamerService.ACTION_START)
        ContextCompat.startForegroundService(this, i)
    }

    // Zoom is applied at the camera HAL, so the stream to the PC and the
    // preview change together; the value persists per camera.
    private fun stepZoom(factor: Float) {
        StreamerService.setZoom(this, StreamerService.zoomRatio * factor)
        updateZoomLabel()
    }

    private fun updateZoomLabel() {
        zoomLabel.text = String.format(java.util.Locale.US, "%.1f×", StreamerService.zoomRatio)
    }

    private fun stepTone(delta: Int) {
        StreamerService.setTone(this, StreamerService.toneStep + delta)
        updateTuningLabels()
    }

    private fun updateTuningLabels() {
        val t = StreamerService.toneStep
        toneLabel.text = if (t > 0) "+$t" else "$t"
        // Like Flip, the button names the CURRENT state.
        dayNightBtn.text = if (StreamerService.dayMode) "Day" else "Night"
    }

    private fun requestBatteryExemption() {
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        if (!pm.isIgnoringBatteryOptimizations(packageName)) {
            startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                Uri.parse("package:$packageName")))
        }
    }

    // ------------------------------------------------------- preview surface

    override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
        // A live GL pipeline adopts the surface on the spot (no restart, no
        // stream blip) and mirrors the encoder output into it.
        if (StreamerService.attachPreview(Surface(st))) return
        // Direct/landscape path: the preview is a HAL target, which can only
        // join by rebuilding the capture session.
        if (StreamerService.statusText != "Idle") {
            startService(Intent(this, StreamerService::class.java).setAction(StreamerService.ACTION_STOP))
            handler.postDelayed({ startStreaming() }, 800)
        }
    }

    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
        // Must detach BEFORE the SurfaceTexture is released (returning true
        // releases it); attachPreview(null) blocks until GL forgot the surface.
        StreamerService.attachPreview(null)
        return true
    }

    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {}
    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}

    // Rotate the preview so it matches how the tablet is held, and mirror the
    // front camera so it behaves like a mirror (natural for self-view).
    private fun applyPreviewTransform() {
        val w = preview.width.toFloat(); val h = preview.height.toFloat()
        if (w == 0f || h == 0f) return
        val displayRot = if (Build.VERSION.SDK_INT >= 30)
            display?.rotation ?: Surface.ROTATION_0
        else @Suppress("DEPRECATION") windowManager.defaultDisplay.rotation
        val displayDeg = when (displayRot) {
            Surface.ROTATION_90 -> 90; Surface.ROTATION_180 -> 180
            Surface.ROTATION_270 -> 270; else -> 0
        }
        // Degrees the sensor image must rotate to be upright.
        val sensorDeg = StreamerService.sensorOrientation
        // Empirical on SM-T220 front cam: raw sensor output is already display-upright
        // in portrait -> no rotation needed. Keep the formula for other devices:
        // val rotateDeg = ((sensorDeg - displayDeg) + 360) % 360
        val rotateDeg = displayDeg

        // Content aspect AFTER rotation (the encoded frame is 1280x720 landscape)
        val swapped = rotateDeg == 90 || rotateDeg == 270
        val contentW = if (swapped) 720f else 1280f
        val contentH = 720f
        // Uniform center-crop: single scale factor, preserves proportions exactly
        val scale = kotlin.math.max(w / contentW, h / contentH)

        val m = Matrix()
        m.postRotate(rotateDeg.toFloat(), w / 2, h / 2)
        m.postScale(scale, scale, w / 2, h / 2)
        preview.setTransform(m)
    }

    override fun onDestroy() {
        handler.removeCallbacks(ticker)
        handler.removeCallbacks(levelTicker)
        StreamerService.attachPreview(null)
        super.onDestroy()
    }
}
