package com.spectre.holder

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.ImageFormat
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.util.Size
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.widget.FrameLayout
import com.google.zxing.BarcodeFormat
import com.google.zxing.BinaryBitmap
import com.google.zxing.DecodeHintType
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.ReaderException
import com.google.zxing.common.GlobalHistogramBinarizer
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader
import java.util.concurrent.Executor
import kotlin.math.abs
import kotlin.math.max

// Lector del QR de Spectre (Ajustes > Móvil) con la cámara trasera: Camera2
// para la imagen y ZXing para leer el código, sin servicios de Google.
@SuppressLint("ViewConstructor")
class Scanner(context: Context, private val ui: Ui, private val onText: (String) -> Unit, private val onError: (String) -> Unit) :
  FrameLayout(context), TextureView.SurfaceTextureListener {

  private val texture = TextureView(context)
  private var thread: HandlerThread? = null
  private var handler: Handler? = null
  private var device: CameraDevice? = null
  private var session: CameraCaptureSession? = null
  private var reader: ImageReader? = null
  private var previewSize = Size(1280, 720)
  private val qr = QRCodeReader()
  private val hints = mapOf(DecodeHintType.POSSIBLE_FORMATS to listOf(BarcodeFormat.QR_CODE), DecodeHintType.TRY_HARDER to true)
  @Volatile private var done = false
  private var running = false
  private var luma = ByteArray(0)
  private var frames = 0

  init {
    setBackgroundColor(C.BG)
    texture.surfaceTextureListener = this
    addView(texture, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
    addView(Frame(context, ui), LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
  }

  fun start() {
    if (running) return
    running = true
    done = false
    thread = HandlerThread("camara").apply { start() }
    handler = Handler(thread!!.looper)
    if (texture.isAvailable) open()
  }

  fun stop() {
    running = false
    try {
      session?.close()
    } catch (e: Exception) {
    }
    session = null
    try {
      device?.close()
    } catch (e: Exception) {
    }
    device = null
    reader?.close()
    reader = null
    thread?.quitSafely()
    thread = null
    handler = null
  }

  override fun onSurfaceTextureAvailable(surface: SurfaceTexture, width: Int, height: Int) {
    if (running) open()
  }

  override fun onSurfaceTextureSizeChanged(surface: SurfaceTexture, width: Int, height: Int) = transform(width, height)
  override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean = true
  override fun onSurfaceTextureUpdated(surface: SurfaceTexture) {}

  @SuppressLint("MissingPermission")
  private fun open() {
    val h = handler ?: return
    if (device != null) return
    val mgr = context.getSystemService(CameraManager::class.java) ?: return onError("Este móvil no tiene cámara.")
    try {
      val ids = mgr.cameraIdList
      val id = ids.firstOrNull {
        mgr.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
      } ?: ids.firstOrNull()
      if (id == null) {
        onError("Este móvil no tiene cámara.")
        return
      }
      val map = mgr.getCameraCharacteristics(id).get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
      // Con 1280x720 se lee bien un QR a 20-40 cm y el análisis va rápido.
      val yuv = map?.getOutputSizes(ImageFormat.YUV_420_888)?.toList().orEmpty()
      val pick = yuv.filter { it.width <= 1920 && it.height <= 1080 }
        .minByOrNull { abs(it.width * it.height - 1280 * 720) + abs(it.width * 9 - it.height * 16) } ?: Size(1280, 720)
      previewSize = pick
      reader = ImageReader.newInstance(pick.width, pick.height, ImageFormat.YUV_420_888, 2).apply {
        setOnImageAvailableListener({ r ->
          val img = try {
            r.acquireLatestImage()
          } catch (e: Exception) {
            null
          } ?: return@setOnImageAvailableListener
          try {
            if (!done) decode(img)
          } finally {
            img.close()
          }
        }, h)
      }
      mgr.openCamera(id, object : CameraDevice.StateCallback() {
        override fun onOpened(camera: CameraDevice) {
          if (!running) {
            camera.close()
            return
          }
          device = camera
          startSession(camera)
        }

        override fun onDisconnected(camera: CameraDevice) {
          camera.close()
          device = null
        }

        override fun onError(camera: CameraDevice, error: Int) {
          camera.close()
          device = null
          Holder.main.post { onError("No se pudo abrir la cámara (está en uso por otra app).") }
        }
      }, h)
    } catch (e: Exception) {
      onError("No se pudo abrir la cámara.")
    }
  }

  private fun startSession(camera: CameraDevice) {
    val h = handler ?: return
    val st = texture.surfaceTexture ?: return
    val r = reader ?: return
    st.setDefaultBufferSize(previewSize.width, previewSize.height)
    val preview = Surface(st)
    Holder.main.post { transform(texture.width, texture.height) }
    val executor = Executor { h.post(it) }
    val config = SessionConfiguration(
      SessionConfiguration.SESSION_REGULAR,
      listOf(OutputConfiguration(preview), OutputConfiguration(r.surface)),
      executor,
      object : CameraCaptureSession.StateCallback() {
        override fun onConfigured(s: CameraCaptureSession) {
          if (!running) {
            s.close()
            return
          }
          session = s
          try {
            val req = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
              addTarget(preview)
              addTarget(r.surface)
              set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
            }
            s.setRepeatingRequest(req.build(), null, h)
          } catch (e: Exception) {
            Holder.main.post { onError("No se pudo usar la cámara.") }
          }
        }

        override fun onConfigureFailed(s: CameraCaptureSession) {
          Holder.main.post { onError("No se pudo usar la cámara.") }
        }
      },
    )
    try {
      camera.createCaptureSession(config)
    } catch (e: Exception) {
      onError("No se pudo usar la cámara.")
    }
  }

  // Solo el brillo (plano Y): es lo que necesita ZXing.
  private fun decode(img: android.media.Image) {
    val plane = img.planes[0]
    val w = img.width
    val hgt = img.height
    val buf = plane.buffer
    val rowStride = plane.rowStride
    if (luma.size != w * hgt) luma = ByteArray(w * hgt)
    if (rowStride == w) {
      buf.get(luma, 0, minOf(buf.remaining(), luma.size))
    } else {
      for (y in 0 until hgt) {
        buf.position(y * rowStride)
        buf.get(luma, y * w, minOf(w, buf.remaining()))
      }
    }
    // Cada imagen prueba una forma distinta: el centro (donde está el marco,
    // con los módulos más grandes para ZXing), la imagen entera y el centro
    // con otro umbral (un monitor con reflejos o brillo desigual).
    val attempt = frames++ % 3
    val side = minOf(w, hgt)
    val source = if (attempt == 1) {
      PlanarYUVLuminanceSource(luma, w, hgt, 0, 0, w, hgt, false)
    } else {
      PlanarYUVLuminanceSource(luma, w, hgt, (w - side) / 2, (hgt - side) / 2, side, side, false)
    }
    val binarizer = if (attempt == 2) GlobalHistogramBinarizer(source) else HybridBinarizer(source)
    val text = try {
      qr.decode(BinaryBitmap(binarizer), hints).text
    } catch (e: ReaderException) {
      null
    } catch (e: Exception) {
      null
    } finally {
      qr.reset()
    }
    if (text != null && !done) {
      done = true
      Holder.main.post { onText(text) }
    }
  }

  // La imagen de la cámara llena la vista sin deformarse (recorta lo que sobra).
  private fun transform(viewW: Int, viewH: Int) {
    if (viewW == 0 || viewH == 0) return
    val rotation = display?.rotation ?: Surface.ROTATION_0
    val matrix = Matrix()
    val cx = viewW / 2f
    val cy = viewH / 2f
    if (rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270) {
      val viewRect = RectF(0f, 0f, viewW.toFloat(), viewH.toFloat())
      val bufferRect = RectF(0f, 0f, previewSize.height.toFloat(), previewSize.width.toFloat())
      bufferRect.offset(cx - bufferRect.centerX(), cy - bufferRect.centerY())
      matrix.setRectToRect(viewRect, bufferRect, Matrix.ScaleToFit.FILL)
      val s = max(viewH.toFloat() / previewSize.height, viewW.toFloat() / previewSize.width)
      matrix.postScale(s, s, cx, cy)
      matrix.postRotate(90f * (rotation - 2), cx, cy)
    } else {
      // En vertical la imagen llega girada: ancho y alto cambiados.
      val bw = previewSize.height.toFloat()
      val bh = previewSize.width.toFloat()
      val s = max(viewW / bw, viewH / bh)
      matrix.setScale(bw * s / viewW, bh * s / viewH, cx, cy)
      if (rotation == Surface.ROTATION_180) matrix.postRotate(180f, cx, cy)
    }
    texture.setTransform(matrix)
  }

  // Marco cuadrado de 1 px donde poner el código, con el resto oscurecido.
  private class Frame(context: Context, private val ui: Ui) : View(context) {
    private val paint = Paint()

    override fun onDraw(canvas: Canvas) {
      val w = width.toFloat()
      val h = height.toFloat()
      val side = minOf(w, h) * 0.68f
      val l = (w - side) / 2
      val t = (h - side) / 2 - h * 0.05f
      paint.style = Paint.Style.FILL
      paint.color = 0x99000000.toInt()
      canvas.drawRect(0f, 0f, w, t, paint)
      canvas.drawRect(0f, t + side, w, h, paint)
      canvas.drawRect(0f, t, l, t + side, paint)
      canvas.drawRect(l + side, t, w, t + side, paint)
      paint.style = Paint.Style.STROKE
      paint.color = C.TEXT
      paint.strokeWidth = ui.line.toFloat()
      canvas.drawRect(l, t, l + side, t + side, paint)
      // Esquinas marcadas más gruesas, rectas.
      paint.strokeWidth = ui.dp(3).toFloat()
      val c = side * 0.12f
      canvas.drawLine(l, t, l + c, t, paint); canvas.drawLine(l, t, l, t + c, paint)
      canvas.drawLine(l + side, t, l + side - c, t, paint); canvas.drawLine(l + side, t, l + side, t + c, paint)
      canvas.drawLine(l, t + side, l + c, t + side, paint); canvas.drawLine(l, t + side, l, t + side - c, paint)
      canvas.drawLine(l + side, t + side, l + side - c, t + side, paint)
      canvas.drawLine(l + side, t + side, l + side, t + side - c, paint)
    }
  }
}
