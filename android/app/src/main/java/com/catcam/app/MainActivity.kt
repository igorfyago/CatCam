package com.catcam.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
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
import android.widget.ImageView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

/**
 * Control surface, camera-app style: full-bleed preview, controls floating
 * over the video. State is never a word that can lie about what you see:
 * the preview shows the live camera, the shutter is a white circle (idle)
 * or a red square (live), the status pill's color is the connection state,
 * and Day/Night is a segmented pair with the active side highlighted.
 * Streaming itself lives in StreamerService.
 *
 * Remote control (tray/camctl) drives the same handlers via intent actions
 * UI_START / UI_STOP / UI_FLIP instead of tapping screen coordinates.
 */
class MainActivity : AppCompatActivity(), TextureView.SurfaceTextureListener {

    companion object {
        const val ACTION_UI_START = "com.catcam.app.UI_START"
        const val ACTION_UI_STOP = "com.catcam.app.UI_STOP"
        const val ACTION_UI_FLIP = "com.catcam.app.UI_FLIP"

        private const val COLOR_LIVE = 0xFFE53935.toInt()   // red: streaming, PC receiving
        private const val COLOR_WAIT = 0xFFDD9C10.toInt()   // amber: streaming, no PC yet
        private const val COLOR_IDLE = 0x66000000           // translucent: idle
        private const val COLOR_SEG_ON = 0xFFFFFFFF.toInt()
        private const val COLOR_SEG_OFF = 0x00FFFFFF
    }

    private val perms = mutableListOf(
        Manifest.permission.CAMERA,
        Manifest.permission.RECORD_AUDIO,
    ).apply {
        if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
    }.toTypedArray()

    private lateinit var statusPill: TextView
    private lateinit var shutter: android.widget.FrameLayout
    private lateinit var shutterInner: android.view.View
    private lateinit var flipBtn: ImageView
    private lateinit var zoomInBtn: TextView
    private lateinit var zoomOutBtn: TextView
    private lateinit var zoomLabel: TextView
    private lateinit var toneCoolBtn: TextView
    private lateinit var toneWarmBtn: TextView
    private lateinit var toneLabel: TextView
    private lateinit var segDay: TextView
    private lateinit var segNight: TextView
    private lateinit var segUsb: TextView
    private lateinit var segWifi: TextView
    private lateinit var segAlways: TextView
    private lateinit var segDemand: TextView
    private lateinit var audioBar: android.widget.ProgressBar
    private lateinit var preview: TextureView

    private var shutterLive = false

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
            updateStatusPill()
            updateShutter()
            updateZoomLabel()
            updateTuningLabels()
            if (StreamerService.glPreviewActive) {
                // GL letterboxes the encoder frame into the view (the "what
                // the PC sees" preview); the matrix must not fight it.
                preview.setTransform(Matrix())
            } else {
                applyPreviewTransform()
            }
            // Camera switch finished: un-dim the stale-frame cover.
            if (StreamerService.statusText.startsWith("Streaming") && preview.alpha < 1f) {
                preview.animate().alpha(1f).setDuration(200).start()
            }
            // READY: the last frame on screen is stale (camera off), dim it
            // so the preview never quietly claims to be live.
            if (!StreamerService.cameraLive && StreamerService.statusText != "Idle"
                    && preview.alpha > 0.31f) {
                preview.animate().alpha(0.3f).setDuration(200).start()
            }
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        // Foreground belt for the service's SCREEN_BRIGHT wake lock.
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // Full-bleed: video draws behind transparent system bars; the control
        // stack and status pill are pushed back inside by the real insets
        // (a fixed margin under-shoots on devices with taller nav bars).
        window.statusBarColor = android.graphics.Color.TRANSPARENT
        window.navigationBarColor = android.graphics.Color.TRANSPARENT
        androidx.core.view.WindowCompat.setDecorFitsSystemWindows(window, false)
        val root = findViewById<android.widget.FrameLayout>(R.id.root)
        val stack = findViewById<android.widget.LinearLayout>(R.id.control_stack)
        val d = resources.displayMetrics.density
        androidx.core.view.ViewCompat.setOnApplyWindowInsetsListener(root) { _, insets ->
            val bars = insets.getInsets(androidx.core.view.WindowInsetsCompat.Type.systemBars())
            (stack.layoutParams as android.widget.FrameLayout.LayoutParams).bottomMargin =
                (26 * d).toInt() + bars.bottom
            (findViewById<android.widget.LinearLayout>(R.id.top_stack).layoutParams
                as android.widget.FrameLayout.LayoutParams).topMargin = (12 * d).toInt() + bars.top
            root.requestLayout()
            insets
        }

        statusPill = findViewById(R.id.status_pill)
        shutter = findViewById(R.id.shutter)
        shutterInner = findViewById(R.id.shutter_inner)
        flipBtn = findViewById(R.id.btn_flip)
        zoomOutBtn = findViewById(R.id.zoom_out)
        zoomInBtn = findViewById(R.id.zoom_in)
        zoomLabel = findViewById(R.id.zoom_label)
        toneCoolBtn = findViewById(R.id.tone_cool)
        toneWarmBtn = findViewById(R.id.tone_warm)
        toneLabel = findViewById(R.id.tone_label)
        segDay = findViewById(R.id.seg_day)
        segNight = findViewById(R.id.seg_night)
        segUsb = findViewById(R.id.seg_usb)
        segWifi = findViewById(R.id.seg_wifi)
        segAlways = findViewById(R.id.seg_always)
        segDemand = findViewById(R.id.seg_demand)
        audioBar = findViewById(R.id.audio_level)
        preview = findViewById(R.id.preview)
        preview.surfaceTextureListener = this
        StreamerService.loadCameraPref(this)

        shutter.setOnClickListener {
            if (StreamerService.statusText != "Idle") stopStreaming() else startOrAskPerms()
        }
        flipBtn.setOnClickListener { flipCamera() }
        zoomOutBtn.setOnClickListener { stepZoom(1f / StreamerService.ZOOM_STEP) }
        zoomInBtn.setOnClickListener { stepZoom(StreamerService.ZOOM_STEP) }
        toneCoolBtn.setOnClickListener { stepTone(-1) }
        toneWarmBtn.setOnClickListener { stepTone(+1) }
        segDay.setOnClickListener { setDay(true) }
        segNight.setOnClickListener { setDay(false) }
        segUsb.setOnClickListener { setTransport(false) }
        segWifi.setOnClickListener { setTransport(true) }
        segAlways.setOnClickListener { setOnDemand(false) }
        segDemand.setOnClickListener { setOnDemand(true) }

        updateZoomLabel()
        updateTuningLabels()
        updateStatusPill()
        updateShutter()

        requestBatteryExemption()
        handler.post(ticker)
        handler.post(levelTicker)
        handleUiAction(intent)
        // On demand: opening the app is enough. It goes READY by itself so
        // the PC can turn the camera on later with nobody at the tablet
        // (the whole point on Wi-Fi, where the PC cannot launch the app).
        if (StreamerService.onDemand && StreamerService.statusText == "Idle" && hasPerms())
            startStreaming()
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        handleUiAction(intent)
    }

    // Tray/camctl entry: same handlers as the on-screen controls, so the UI
    // can never disagree with what remote control did. Idempotent: START
    // while streaming and STOP while idle are no-ops.
    private fun handleUiAction(intent: Intent?) {
        when (intent?.action) {
            ACTION_UI_START -> if (StreamerService.statusText == "Idle") startOrAskPerms()
            ACTION_UI_STOP -> if (StreamerService.statusText != "Idle") stopStreaming()
            ACTION_UI_FLIP -> flipCamera()
        }
    }

    // ------------------------------------------------------------- actions

    private fun startOrAskPerms() {
        if (hasPerms()) startStreaming() else
            ActivityCompat.requestPermissions(this, perms, 1)
    }

    private fun startStreaming() {
        val i = Intent(this, StreamerService::class.java).setAction(StreamerService.ACTION_START)
        ContextCompat.startForegroundService(this, i)
    }

    private fun stopStreaming() {
        startService(Intent(this, StreamerService::class.java).setAction(StreamerService.ACTION_STOP))
    }

    private fun flipCamera() {
        StreamerService.preferFrontCamera = !StreamerService.preferFrontCamera
        StreamerService.saveCameraPref(this)
        // Each camera keeps its own zoom and tone (calls vs cat duty want
        // different framing): pull the new camera's saved values.
        StreamerService.loadCameraPref(this)
        updateZoomLabel()
        updateTuningLabels()
        if (StreamerService.statusText != "Idle") {
            // Dim the last frame of the OLD camera while the switch runs, so
            // the preview never quietly claims to be the new camera. The
            // ticker restores alpha once the new camera is streaming.
            preview.animate().alpha(0.3f).setDuration(150).start()
            stopStreaming()
            handler.postDelayed({ startStreaming() }, 800)
        }
    }

    // Zoom is applied at the camera HAL, so the stream to the PC and the
    // preview change together; the value persists per camera.
    private fun stepZoom(factor: Float) {
        StreamerService.setZoom(this, StreamerService.zoomRatio * factor)
        updateZoomLabel()
    }

    private fun stepTone(delta: Int) {
        StreamerService.setTone(this, StreamerService.toneStep + delta)
        updateTuningLabels()
    }

    private fun setDay(on: Boolean) {
        StreamerService.setDayMode(this, on)
        updateTuningLabels()
    }

    // A REQUEST: the beacon carries it to the tray, which restarts the host
    // on the chosen transport (a few seconds). The pill shows the actual
    // transport once connected, and the switch snaps to reality then.
    private fun setTransport(wifi: Boolean) {
        StreamerService.setTransport(this, wifi)
        updateTuningLabels()
    }

    // Always on = Start means camera on (cat cam). On demand = Start means
    // READY, the PC turns the camera on when an app pulls frames.
    private fun setOnDemand(on: Boolean) {
        StreamerService.setOnDemand(this, on)
        updateTuningLabels()
    }

    // ------------------------------------------------------------- state UI

    private fun updateStatusPill() {
        val s = StreamerService.statusText
        val (text, color) = when {
            s.startsWith("Streaming") && StreamerService.clientConnected ->
                // Actual transport, read from where the connection came from.
                ("LIVE · " + if (StreamerService.clientViaWifi == true) "Wi-Fi" else "USB") to COLOR_LIVE
            s == "Idle" -> "Idle" to COLOR_IDLE
            // READY: connected, camera off, the PC decides. Amber like
            // waiting (nothing is being sent), the words say why.
            s.startsWith("Ready") ->
                ("READY · " + (if (StreamerService.clientViaWifi == true) "Wi-Fi" else "USB")
                    + " · camera off") to COLOR_WAIT
            // While waiting, show this tablet's address: it is what the PC
            // side needs for cable-free (direct TCP) mode.
            s.startsWith("Waiting") || s.startsWith("Streaming") ->
                (lanIp()?.let { "Waiting for PC · $it" } ?: "Waiting for PC") to COLOR_WAIT
            else -> s to COLOR_WAIT   // error strings surface as-is
        }
        statusPill.text = text
        statusPill.backgroundTintList = ColorStateList.valueOf(color)
        // First-run onboarding: while waiting and no PC has EVER connected,
        // point at the PC app. Once any PC has connected, never show again.
        if (StreamerService.clientConnected) everConnected = true
        findViewById<TextView>(R.id.get_pc_hint).visibility =
            if (!everConnected && text.startsWith("Waiting")) android.view.View.VISIBLE
            else android.view.View.GONE
    }

    private var everConnected = false

    private var cachedIp: String? = null
    private var cachedIpMs = 0L
    private fun lanIp(): String? {
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - cachedIpMs < 5000) return cachedIp
        cachedIpMs = now
        cachedIp = try {
            java.net.NetworkInterface.getNetworkInterfaces().asSequence()
                .flatMap { it.inetAddresses.asSequence() }
                .firstOrNull { it is java.net.Inet4Address && it.isSiteLocalAddress }
                ?.hostAddress
        } catch (e: Exception) { null }
        return cachedIp
    }

    private fun updateShutter() {
        val live = StreamerService.statusText != "Idle"
        if (live == shutterLive) return
        shutterLive = live
        shutterInner.setBackgroundResource(if (live) R.drawable.shutter_live else R.drawable.shutter_idle)
        val size = (resources.displayMetrics.density * (if (live) 30 else 60)).toInt()
        shutterInner.layoutParams = shutterInner.layoutParams.apply { width = size; height = size }
    }

    private fun updateZoomLabel() {
        zoomLabel.text = String.format(java.util.Locale.US, "%.1f×", StreamerService.zoomRatio)
    }

    // Segmented pair styling: both options visible, the active one highlighted.
    private fun styleSeg(v: TextView, active: Boolean) {
        v.setBackgroundResource(R.drawable.pill_solid)
        v.backgroundTintList = ColorStateList.valueOf(if (active) COLOR_SEG_ON else COLOR_SEG_OFF)
        v.setTextColor(if (active) 0xFF000000.toInt() else 0xB3FFFFFF.toInt())
    }

    private fun updateTuningLabels() {
        val t = StreamerService.toneStep
        toneLabel.text = if (t > 0) "+$t" else "$t"
        styleSeg(segDay, StreamerService.dayMode)
        styleSeg(segNight, !StreamerService.dayMode)
        styleSeg(segUsb, !StreamerService.transportWifi)
        styleSeg(segWifi, StreamerService.transportWifi)
        styleSeg(segAlways, !StreamerService.onDemand)
        styleSeg(segDemand, StreamerService.onDemand)
    }

    // ------------------------------------------------------------- plumbing

    private fun hasPerms() = perms.all {
        ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (hasPerms()) startStreaming()
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
            stopStreaming()
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

    // Rotate the preview so it matches how the tablet is held (direct/
    // landscape path only; the GL preview letterboxes itself).
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
        // Empirical on SM-T220 (lesson 12): raw sensor output is already
        // display-upright in portrait, so rotate by display only.
        val rotateDeg = displayDeg
        val swapped = rotateDeg == 90 || rotateDeg == 270
        val contentW = if (swapped) 720f else 1280f
        val contentH = 720f
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
