package com.catcam.app

import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.Matrix
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * GLRotator: Camera2 (SurfaceTexture / external OES texture) -> GL render with
 * rotation/mirror/crop -> MediaCodec input Surface (encoder), via EGL14 + GLES20.
 *
 * Usage:
 *   val rotator = GLRotator(encoder.createInputSurface(), outW, outH)
 *   rotator.setInputBufferSize(1280, 720)              // sensor frame size
 *   rotator.setTransform(90, mirror = true)            // e.g. front camera portrait
 *   val captureTarget = Surface(rotator.inputSurfaceTexture)
 *   rotator.inputSurfaceTexture.setOnFrameAvailableListener { rotator.renderFrame() }
 *   ...
 *   rotator.release()
 *
 * Threading: renderFrame() may be called from any thread (typically the
 * OnFrameAvailableListener handler thread). EGL context is made current per call.
 */
class GLRotator(
    outputSurface: Surface,
    private val outWidth: Int,
    private val outHeight: Int
) {
    companion object {
        private const val TAG = "GLRotator"

        private const val VERTEX_SHADER = """
            uniform mat4 uMVPMatrix;
            uniform mat4 uTexMatrix;
            attribute vec4 aPosition;
            attribute vec4 aTextureCoord;
            varying vec2 vTextureCoord;
            void main() {
                gl_Position = uMVPMatrix * aPosition;
                vTextureCoord = (uTexMatrix * aTextureCoord).xy;
            }
        """

        private const val FRAGMENT_SHADER = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTextureCoord;
            uniform samplerExternalOES sTexture;
            void main() {
                gl_FragColor = texture2D(sTexture, vTextureCoord);
            }
        """

        // Temporal denoise: blend the fresh camera frame over a running
        // accumulator (ping-pong FBO), then copy the accumulator to the
        // encoder surface. NEW_WEIGHT balances grain removal against motion
        // trails: steady-state noise variance is a/(2-a) of the raw frame
        // (0.55 -> ~0.38x variance, ~0.6x sigma), trails decay 45%/frame,
        // invisible on a talking head. The camera samples through the
        // texMatrix; the accumulator is ALREADY in output space, so it
        // samples through the plain quad coord (vPlainCoord) — sampling it
        // through the texMatrix would re-rotate the oriented frame.
        private const val NEW_WEIGHT = 0.55f

        private const val BLEND_VERTEX_SHADER = """
            uniform mat4 uMVPMatrix;
            uniform mat4 uTexMatrix;
            attribute vec4 aPosition;
            attribute vec4 aTextureCoord;
            varying vec2 vTextureCoord;
            varying vec2 vPlainCoord;
            void main() {
                gl_Position = uMVPMatrix * aPosition;
                vTextureCoord = (uTexMatrix * aTextureCoord).xy;
                vPlainCoord = aTextureCoord.xy;
            }
        """

        private const val BLEND_FRAGMENT_SHADER = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTextureCoord;
            varying vec2 vPlainCoord;
            uniform samplerExternalOES sTexture;   // fresh camera frame
            uniform sampler2D sAccum;              // previous blended output
            uniform float uNewWeight;
            void main() {
                vec4 cam = texture2D(sTexture, vTextureCoord);
                vec4 acc = texture2D(sAccum, vPlainCoord);
                gl_FragColor = mix(acc, cam, uNewWeight);
            }
        """

        private const val COPY_FRAGMENT_SHADER = """
            precision mediump float;
            varying vec2 vTextureCoord;
            uniform sampler2D sTexture;
            void main() {
                gl_FragColor = texture2D(sTexture, vTextureCoord);
            }
        """

        // Full-screen quad, interleaved: x, y, tx, ty (CCW, triangle strip order).
        private val FULL_RECT = floatArrayOf(
            -1f, -1f,  0f, 0f,
             1f, -1f,  1f, 0f,
            -1f,  1f,  0f, 1f,
             1f,  1f,  1f, 1f
        )
    }

    /** Give this (wrapped in a Surface) to Camera2 as a capture target. */
    val inputSurfaceTexture: SurfaceTexture

    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    private var eglConfig: EGLConfig? = null

    // Optional second output: the activity's preview TextureView. It gets the
    // EXACT frame the encoder gets (rotation, crop, denoise, zoom), letterboxed,
    // so the tablet preview shows what the PC sees instead of a separate HAL
    // feed with its own geometry. Attach/detach any time; a preview failure
    // only ever drops the preview, never the encoder path.
    private var previewEglSurface: EGLSurface = EGL14.EGL_NO_SURFACE

    private var program = 0
    private var oesTextureId = 0
    private var aPositionLoc = 0
    private var aTextureCoordLoc = 0
    private var uMVPMatrixLoc = 0
    private var uTexMatrixLoc = 0

    // Temporal denoise (two-pass, ping-pong FBO). denoiseOk=false at any
    // init failure falls back to the original direct path forever.
    private var blendProgram = 0
    private var copyProgram = 0
    private var fboId = 0
    private val accumTex = IntArray(2)
    private var accumWrite = 0
    private var accumValid = false
    private var denoiseOk = false
    private var bAPosition = 0
    private var bATexCoord = 0
    private var bUMVP = 0
    private var bUTexMatrix = 0
    private var bSTexture = 0
    private var bSAccum = 0
    private var bUNewWeight = 0
    private var cAPosition = 0
    private var cATexCoord = 0
    private var cUMVP = 0
    private var cUTexMatrix = 0
    private var cSTexture = 0
    private val identityMatrix = FloatArray(16).also { Matrix.setIdentityM(it, 0) }

    private val vertexBuffer: FloatBuffer =
        ByteBuffer.allocateDirect(FULL_RECT.size * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
            .apply { put(FULL_RECT); position(0) }

    private val mvpMatrix = FloatArray(16)
    private val texMatrix = FloatArray(16)
    private val stMatrix = FloatArray(16)   // from SurfaceTexture (fixes V-flip, crop)

    // Transform state
    @Volatile private var rotateDeg = 0
    @Volatile private var mirror = false
    @Volatile private var inWidth = outWidth
    @Volatile private var inHeight = outHeight

    // Matrix instrumentation: dump stMatrix/texMatrix to logcat for the first
    // few frames after every config change. The stMatrix content is per-camera
    // HAL behavior that cannot be derived from docs; logging it is the only
    // ground truth (handoff step 1: instrument, don't guess).
    @Volatile private var logFrames = 3

    init {
        initEgl(outputSurface)
        program = buildProgram()
        oesTextureId = createOesTexture()
        inputSurfaceTexture = SurfaceTexture(oesTextureId)
        Matrix.setIdentityM(mvpMatrix, 0)
        Matrix.setIdentityM(texMatrix, 0)
        initDenoise()
    }

    private fun initDenoise() {
        try {
            blendProgram = linkProgram(BLEND_VERTEX_SHADER, BLEND_FRAGMENT_SHADER)
            bAPosition = GLES20.glGetAttribLocation(blendProgram, "aPosition")
            bATexCoord = GLES20.glGetAttribLocation(blendProgram, "aTextureCoord")
            bUMVP = GLES20.glGetUniformLocation(blendProgram, "uMVPMatrix")
            bUTexMatrix = GLES20.glGetUniformLocation(blendProgram, "uTexMatrix")
            bSTexture = GLES20.glGetUniformLocation(blendProgram, "sTexture")
            bSAccum = GLES20.glGetUniformLocation(blendProgram, "sAccum")
            bUNewWeight = GLES20.glGetUniformLocation(blendProgram, "uNewWeight")

            copyProgram = linkProgram(VERTEX_SHADER, COPY_FRAGMENT_SHADER)
            cAPosition = GLES20.glGetAttribLocation(copyProgram, "aPosition")
            cATexCoord = GLES20.glGetAttribLocation(copyProgram, "aTextureCoord")
            cUMVP = GLES20.glGetUniformLocation(copyProgram, "uMVPMatrix")
            cUTexMatrix = GLES20.glGetUniformLocation(copyProgram, "uTexMatrix")
            cSTexture = GLES20.glGetUniformLocation(copyProgram, "sTexture")

            GLES20.glGenTextures(2, accumTex, 0)
            for (t in accumTex) {
                GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, t)
                GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA,
                    outWidth, outHeight, 0, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null)
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
            }
            val fbos = IntArray(1)
            GLES20.glGenFramebuffers(1, fbos, 0)
            fboId = fbos[0]
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fboId)
            GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0,
                GLES20.GL_TEXTURE_2D, accumTex[0], 0)
            val complete = GLES20.glCheckFramebufferStatus(GLES20.GL_FRAMEBUFFER) ==
                GLES20.GL_FRAMEBUFFER_COMPLETE
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
            denoiseOk = complete
            Log.i(TAG, if (complete) "temporal denoise on (newWeight=$NEW_WEIGHT)"
                       else "FBO incomplete: denoise off, direct path")
        } catch (e: Exception) {
            Log.w(TAG, "denoise init failed (${e.message}): direct path")
            denoiseOk = false
        }
    }

    /** Set the size of frames the camera will deliver (sensor buffer size). */
    fun setInputBufferSize(w: Int, h: Int) {
        inWidth = w
        inHeight = h
        inputSurfaceTexture.setDefaultBufferSize(w, h)
        accumValid = false // stale accumulator would ghost the old framing
    }

    /**
     * @param rotateDeg clockwise rotation applied to the image: 0, 90, 180, or 270.
     * @param mirror horizontal mirror applied AFTER rotation (front-camera selfie style).
     */
    fun setTransform(rotateDeg: Int, mirror: Boolean) {
        this.rotateDeg = ((rotateDeg % 360) + 360) % 360
        this.mirror = mirror
        logFrames = 3
        accumValid = false // stale accumulator would ghost the old orientation
    }

    /**
     * Attach (or detach with null) a live mirror of the encoder output — the
     * activity's preview surface. Safe from any thread: this only creates or
     * destroys the EGL window surface, and never touches EGL current-ness,
     * which stays owned by the render thread (eglMakeCurrent binds the context
     * to the calling thread; doing it here would break renderFrame).
     */
    @Synchronized
    fun setPreviewSurface(surface: Surface?) {
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) return
        if (previewEglSurface != EGL14.EGL_NO_SURFACE) {
            EGL14.eglDestroySurface(eglDisplay, previewEglSurface)
            previewEglSurface = EGL14.EGL_NO_SURFACE
        }
        if (surface != null && surface.isValid) {
            try {
                val s = EGL14.eglCreateWindowSurface(
                    eglDisplay, eglConfig, surface, intArrayOf(EGL14.EGL_NONE), 0)
                if (s != null && s != EGL14.EGL_NO_SURFACE) previewEglSurface = s
                else Log.w(TAG, "preview eglCreateWindowSurface failed: 0x" +
                    Integer.toHexString(EGL14.eglGetError()))
            } catch (e: Exception) {
                Log.w(TAG, "preview attach failed: ${e.message}")
            }
        }
        Log.i(TAG, "preview surface ${if (previewEglSurface != EGL14.EGL_NO_SURFACE) "attached" else "detached"}")
    }

    /**
     * Call when a new camera frame is available (from OnFrameAvailableListener).
     * Pulls the latest frame from the SurfaceTexture and draws one frame to the
     * encoder surface.
     */
    @Synchronized
    fun renderFrame() {
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) return
        makeCurrent()

        inputSurfaceTexture.updateTexImage()
        inputSurfaceTexture.getTransformMatrix(stMatrix)

        computeTexMatrix()

        if (logFrames > 0) {
            logFrames--
            Log.i(TAG, "cfg rot=$rotateDeg mirror=$mirror in=${inWidth}x$inHeight out=${outWidth}x$outHeight")
            Log.i(TAG, "stMatrix : ${fmt(stMatrix)}")
            Log.i(TAG, "texMatrix: ${fmt(texMatrix)}")
        }

        GLES20.glViewport(0, 0, outWidth, outHeight)

        // Which texture holds the finished output frame (for the preview
        // mirror). -1 = direct path, re-sample the OES texture instead.
        var previewTex = -1
        if (denoiseOk) {
            // Pass 1: blend the camera frame over the accumulator (ping-pong).
            val write = accumWrite
            val read = 1 - write
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fboId)
            GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0,
                GLES20.GL_TEXTURE_2D, accumTex[write], 0)
            GLES20.glUseProgram(blendProgram)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId)
            GLES20.glUniform1i(bSTexture, 0)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE1)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, accumTex[read])
            GLES20.glUniform1i(bSAccum, 1)
            // First frame after (re)config seeds the accumulator raw.
            GLES20.glUniform1f(bUNewWeight, if (accumValid) NEW_WEIGHT else 1f)
            GLES20.glUniformMatrix4fv(bUMVP, 1, false, mvpMatrix, 0)
            GLES20.glUniformMatrix4fv(bUTexMatrix, 1, false, texMatrix, 0)
            drawQuad(bAPosition, bATexCoord)

            // Pass 2: copy the fresh accumulator to the encoder surface.
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
            GLES20.glClearColor(0f, 0f, 0f, 1f)
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
            GLES20.glUseProgram(copyProgram)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, accumTex[write])
            GLES20.glUniform1i(cSTexture, 0)
            GLES20.glUniformMatrix4fv(cUMVP, 1, false, identityMatrix, 0)
            GLES20.glUniformMatrix4fv(cUTexMatrix, 1, false, identityMatrix, 0)
            drawQuad(cAPosition, cATexCoord)

            previewTex = accumTex[write]
            accumWrite = read
            accumValid = true
        } else {
            GLES20.glClearColor(0f, 0f, 0f, 1f)
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
            GLES20.glUseProgram(program)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId)
            GLES20.glUniformMatrix4fv(uMVPMatrixLoc, 1, false, mvpMatrix, 0)
            GLES20.glUniformMatrix4fv(uTexMatrixLoc, 1, false, texMatrix, 0)
            drawQuad(aPositionLoc, aTextureCoordLoc)
        }

        // Stamp the encoder frame with the camera frame's timestamp so the
        // encoder sees monotonic presentation times.
        EGLExt.eglPresentationTimeANDROID(eglDisplay, eglSurface, inputSurfaceTexture.timestamp)
        EGL14.eglSwapBuffers(eglDisplay, eglSurface)

        // Encoder frame is on its way; now mirror it to the preview, if attached.
        drawPreview(previewTex)
    }

    // Draw the finished output frame letterboxed into the preview surface.
    // srcTex >= 0: the denoise accumulator that was just copied to the encoder.
    // srcTex < 0: direct path, re-draw the OES texture with the same texMatrix
    // (updateTexImage content persists until the next update, so this is the
    // same frame the encoder just received).
    private fun drawPreview(srcTex: Int) {
        val ps = previewEglSurface
        if (ps == EGL14.EGL_NO_SURFACE) return
        try {
            if (!EGL14.eglMakeCurrent(eglDisplay, ps, ps, eglContext))
                throw RuntimeException("makeCurrent 0x" + Integer.toHexString(EGL14.eglGetError()))
            // Query size per frame: the TextureView can resize under us.
            val w = IntArray(1); val h = IntArray(1)
            EGL14.eglQuerySurface(eglDisplay, ps, EGL14.EGL_WIDTH, w, 0)
            EGL14.eglQuerySurface(eglDisplay, ps, EGL14.EGL_HEIGHT, h, 0)
            if (w[0] <= 0 || h[0] <= 0) return
            // Letterbox (aspect-fit, like the tray preview): the user must see
            // the WHOLE frame the PC gets, so never crop here.
            val scale = minOf(w[0].toFloat() / outWidth, h[0].toFloat() / outHeight)
            val vw = (outWidth * scale).toInt().coerceAtLeast(1)
            val vh = (outHeight * scale).toInt().coerceAtLeast(1)
            GLES20.glClearColor(0f, 0f, 0f, 1f)
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
            GLES20.glViewport((w[0] - vw) / 2, (h[0] - vh) / 2, vw, vh)
            if (srcTex >= 0) {
                GLES20.glUseProgram(copyProgram)
                GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
                GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, srcTex)
                GLES20.glUniform1i(cSTexture, 0)
                GLES20.glUniformMatrix4fv(cUMVP, 1, false, identityMatrix, 0)
                GLES20.glUniformMatrix4fv(cUTexMatrix, 1, false, identityMatrix, 0)
                drawQuad(cAPosition, cATexCoord)
            } else {
                GLES20.glUseProgram(program)
                GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
                GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId)
                GLES20.glUniformMatrix4fv(uMVPMatrixLoc, 1, false, mvpMatrix, 0)
                GLES20.glUniformMatrix4fv(uTexMatrixLoc, 1, false, texMatrix, 0)
                drawQuad(aPositionLoc, aTextureCoordLoc)
            }
            EGL14.eglSwapBuffers(eglDisplay, ps)
        } catch (e: Exception) {
            // Preview surface died (activity teardown race, etc): drop the
            // preview and keep streaming. Never let it touch the encoder path.
            Log.w(TAG, "preview draw dropped: ${e.message}")
            try { EGL14.eglDestroySurface(eglDisplay, ps) } catch (_: Exception) {}
            previewEglSurface = EGL14.EGL_NO_SURFACE
        }
    }

    @Synchronized
    fun release() {
        inputSurfaceTexture.release()
        if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            if (previewEglSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(eglDisplay, previewEglSurface)
            if (eglSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(eglDisplay, eglSurface)
            if (eglContext != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(eglDisplay, eglContext)
            EGL14.eglTerminate(eglDisplay)
        }
        previewEglSurface = EGL14.EGL_NO_SURFACE
        eglDisplay = EGL14.EGL_NO_DISPLAY
        eglContext = EGL14.EGL_NO_CONTEXT
        eglSurface = EGL14.EGL_NO_SURFACE
        if (oesTextureId != 0) {
            val t = intArrayOf(oesTextureId)
            GLES20.glDeleteTextures(1, t, 0)
            oesTextureId = 0
        }
        if (program != 0) {
            GLES20.glDeleteProgram(program)
            program = 0
        }
        if (blendProgram != 0) { GLES20.glDeleteProgram(blendProgram); blendProgram = 0 }
        if (copyProgram != 0) { GLES20.glDeleteProgram(copyProgram); copyProgram = 0 }
        if (fboId != 0) { GLES20.glDeleteFramebuffers(1, intArrayOf(fboId), 0); fboId = 0 }
        if (accumTex[0] != 0) { GLES20.glDeleteTextures(2, accumTex, 0); accumTex.fill(0) }
    }

    // ---------------------------------------------------------------- EGL setup

    private fun initEgl(outputSurface: Surface) {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        check(eglDisplay != EGL14.EGL_NO_DISPLAY) { "eglGetDisplay failed" }
        val version = IntArray(2)
        check(EGL14.eglInitialize(eglDisplay, version, 0, version, 1)) { "eglInitialize failed" }

        val attribs = intArrayOf(
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGLExt.EGL_RECORDABLE_ANDROID, 1,   // required by MediaCodec input surfaces
            EGL14.EGL_NONE
        )
        val configs = arrayOfNulls<EGLConfig>(1)
        val num = IntArray(1)
        check(EGL14.eglChooseConfig(eglDisplay, attribs, 0, configs, 0, 1, num, 0) && num[0] > 0) {
            "eglChooseConfig failed"
        }
        eglConfig = configs[0]

        eglContext = EGL14.eglCreateContext(
            eglDisplay, eglConfig, EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0
        )
        check(eglContext != EGL14.EGL_NO_CONTEXT) { "eglCreateContext failed" }

        eglSurface = EGL14.eglCreateWindowSurface(
            eglDisplay, eglConfig, outputSurface, intArrayOf(EGL14.EGL_NONE), 0
        )
        check(eglSurface != EGL14.EGL_NO_SURFACE) { "eglCreateWindowSurface failed" }

        makeCurrent()
    }

    private fun makeCurrent() {
        if (!EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            throw RuntimeException("eglMakeCurrent failed: 0x" + Integer.toHexString(EGL14.eglGetError()))
        }
    }

    // ------------------------------------------------------------- GL resources

    private fun createOesTexture(): Int {
        val ids = IntArray(1)
        GLES20.glGenTextures(1, ids, 0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, ids[0])
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        return ids[0]
    }

    private fun buildProgram(): Int {
        val p = linkProgram(VERTEX_SHADER, FRAGMENT_SHADER)
        aPositionLoc = GLES20.glGetAttribLocation(p, "aPosition")
        aTextureCoordLoc = GLES20.glGetAttribLocation(p, "aTextureCoord")
        uMVPMatrixLoc = GLES20.glGetUniformLocation(p, "uMVPMatrix")
        uTexMatrixLoc = GLES20.glGetUniformLocation(p, "uTexMatrix")
        return p
    }

    private fun linkProgram(vsSrc: String, fsSrc: String): Int {
        val vs = compileShader(GLES20.GL_VERTEX_SHADER, vsSrc)
        val fs = compileShader(GLES20.GL_FRAGMENT_SHADER, fsSrc)
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, vs)
        GLES20.glAttachShader(p, fs)
        GLES20.glLinkProgram(p)
        val ok = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0)
        if (ok[0] == 0) {
            val log = GLES20.glGetProgramInfoLog(p)
            GLES20.glDeleteProgram(p)
            throw RuntimeException("Program link failed: $log")
        }
        GLES20.glDeleteShader(vs)
        GLES20.glDeleteShader(fs)
        return p
    }

    private fun drawQuad(posLoc: Int, texLoc: Int) {
        vertexBuffer.position(0)
        GLES20.glVertexAttribPointer(posLoc, 2, GLES20.GL_FLOAT, false, 16, vertexBuffer)
        GLES20.glEnableVertexAttribArray(posLoc)
        vertexBuffer.position(2)
        GLES20.glVertexAttribPointer(texLoc, 2, GLES20.GL_FLOAT, false, 16, vertexBuffer)
        GLES20.glEnableVertexAttribArray(texLoc)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
    }

    private fun compileShader(type: Int, src: String): Int {
        val s = GLES20.glCreateShader(type)
        GLES20.glShaderSource(s, src)
        GLES20.glCompileShader(s)
        val ok = IntArray(1)
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0)
        if (ok[0] == 0) {
            val log = GLES20.glGetShaderInfoLog(s)
            GLES20.glDeleteShader(s)
            throw RuntimeException("Shader compile failed (${if (type == GLES20.GL_VERTEX_SHADER) "vertex" else "fragment"}): $log")
        }
        return s
    }

    // ------------------------------------------------------------- Transform math

    /**
     * texMatrix = stMatrix (SurfaceTexture flip/crop) * rotation * mirror * aspect-crop.
     * Aspect-crop: scale texture coords around the center so the input aspect
     * fills the output aspect (center-crop, no distortion, no letterbox).
     */
    private fun computeTexMatrix() {
        val m = FloatArray(16)
        Matrix.setIdentityM(m, 0)

        // Rotate around texture center (0.5, 0.5).
        // Order matters: mirror BEFORE rotation. scaleM after rotateM flips
        // the already-rotated frame (wrong axis) — measured on-device:
        // 90+mirror-after == 270 (hair still left). Mirror first, then rotate.
        Matrix.translateM(m, 0, 0.5f, 0.5f, 0f)
        if (mirror) Matrix.scaleM(m, 0, -1f, 1f, 1f)
        Matrix.rotateM(m, 0, rotateDeg.toFloat(), 0f, 0f, 1f)

        // Effective input aspect after orientation. m operates in CONTENT
        // space (the quad is the output frame; stMatrix then maps content
        // coords to raw buffer sampling), so the crop scale is applied on
        // content axes directly.
        // At rotateDeg==0 the stMatrix itself delivers the content already
        // rotated to portrait (measured on SM-T220, lessons 21/24: both
        // cams are 90-degree-family stMatrices), so the CONTENT dims are
        // the SWAPPED buffer dims. For 16:9 buffers that makes the crop a
        // no-op (720x1280 content == 9:16 output, factor 1.0 — identical
        // to the old skip-crop behavior). For 4:3 full-sensor buffers
        // (1600x1200 -> content 1200x1600) it center-crops width to 9:16
        // instead of squeezing. A future device with an identity stMatrix
        // at rotate 0 would need this revisited — check the logged stMatrix
        // (the instrumentation below) before trusting it there.
        run {
            val contentW: Float
            val contentH: Float
            if (rotateDeg == 0 || rotateDeg == 180) {
                if (rotateDeg == 0) {
                    contentW = inHeight.toFloat()   // stMatrix pre-rotated (measured)
                    contentH = inWidth.toFloat()
                } else {
                    contentW = inWidth.toFloat()
                    contentH = inHeight.toFloat()
                }
            } else {
                contentW = inHeight.toFloat()
                contentH = inWidth.toFloat()
            }
            val inAspect = contentW / contentH
            val outAspect = outWidth.toFloat() / outHeight.toFloat()
            if (inAspect > outAspect) {
                Matrix.scaleM(m, 0, outAspect / inAspect, 1f, 1f)
            } else if (inAspect < outAspect) {
                Matrix.scaleM(m, 0, 1f, inAspect / outAspect, 1f)
            }
        }

        Matrix.translateM(m, 0, -0.5f, -0.5f, 0f)

        // Pre-multiply the SurfaceTexture's own matrix (handles producer flip).
        Matrix.multiplyMM(texMatrix, 0, stMatrix, 0, m, 0)
    }

    // Column-major FloatArray(16) rendered as 4 display rows separated by |.
    // Locale.US: keep dot decimals in logcat regardless of device locale.
    private fun fmt(m: FloatArray) = (0..3).joinToString(" | ") { r ->
        (0..3).joinToString(" ") { c -> "%.3f".format(java.util.Locale.US, m[c * 4 + r]) }
    }
}
