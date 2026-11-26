package com.spectre.holder

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.StateListDrawable
import android.text.TextUtils
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import java.util.Locale

// Colores del tema Negro de Spectre: negro puro, superficies gris oscuro,
// líneas de 1 px y el blanco como acento. El rojo solo para borrar o fallar.
object C {
  const val BG = 0xFF000000.toInt()
  const val BG_ALT = 0xFF0D0D0D.toInt()
  const val SURFACE = 0xFF1A1A1A.toInt()
  const val SURFACE_HOVER = 0xFF242424.toInt()
  const val SURFACE_ACTIVE = 0xFF2E2E2E.toInt()
  const val LINE = 0xFF2A2A2A.toInt()
  const val LINE_STRONG = 0xFF3A3A3A.toInt()
  const val TEXT = 0xFFFFFFFF.toInt()
  const val MUTED = 0xFF9A9A9A.toInt()
  const val FAINT = 0xFF7C7C7C.toInt()
  const val ACCENT = 0xFFFFFFFF.toInt()
  const val ACCENT_ACTIVE = 0xFFCCCCCC.toInt()
  const val ON_ACCENT = 0xFF000000.toInt()
  const val DANGER = 0xFFE5484D.toInt()
  const val WARNING = 0xFFD9A33A.toInt()
}

class Ui(val ctx: Context) {
  private val density = ctx.resources.displayMetrics.density
  fun dp(v: Float): Int = Math.round(v * density)
  fun dp(v: Int): Int = dp(v.toFloat())
  val line: Int get() = maxOf(1, dp(1))

  val medium: Typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
  val regular: Typeface = Typeface.create("sans-serif", Typeface.NORMAL)

  fun text(s: CharSequence, sp: Float = 14f, color: Int = C.TEXT, bold: Boolean = false, lines: Int = 0): TextView =
    TextView(ctx).apply {
      text = s
      setTextColor(color)
      setTextSize(TypedValue.COMPLEX_UNIT_SP, sp)
      typeface = if (bold) medium else regular
      includeFontPadding = false
      setLineSpacing(0f, 1.15f)
      if (lines > 0) {
        maxLines = lines
        ellipsize = TextUtils.TruncateAt.END
      }
    }

  // Rectángulo plano: relleno, borde de 1 px y nada de esquinas redondas.
  fun box(fill: Int, stroke: Int = 0): GradientDrawable = GradientDrawable().apply {
    shape = GradientDrawable.RECTANGLE
    cornerRadius = 0f
    setColor(fill)
    if (stroke != 0) setStroke(line, stroke)
  }

  private fun pressable(normal: Drawable, pressed: Drawable, disabled: Drawable? = null): StateListDrawable =
    StateListDrawable().apply {
      if (disabled != null) addState(intArrayOf(-android.R.attr.state_enabled), disabled)
      addState(intArrayOf(android.R.attr.state_pressed), pressed)
      addState(intArrayOf(), normal)
    }

  enum class Style { PRIMARY, SECONDARY, GHOST, DANGER }

  fun button(label: String, style: Style = Style.SECONDARY, onClick: () -> Unit): TextView = TextView(ctx).apply {
    text = label
    gravity = Gravity.CENTER
    setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
    typeface = medium
    includeFontPadding = false
    minHeight = dp(44)
    setPadding(dp(16), dp(10), dp(16), dp(10))
    isClickable = true
    isFocusable = true
    when (style) {
      Style.PRIMARY -> {
        setTextColor(C.ON_ACCENT)
        background = pressable(box(C.ACCENT), box(C.ACCENT_ACTIVE), box(C.SURFACE_ACTIVE))
      }
      Style.SECONDARY -> {
        setTextColor(C.TEXT)
        background = pressable(box(C.BG, C.LINE_STRONG), box(C.SURFACE_ACTIVE, C.LINE_STRONG))
      }
      Style.GHOST -> {
        setTextColor(C.TEXT)
        background = pressable(ColorDrawable(0), box(C.SURFACE_ACTIVE))
      }
      Style.DANGER -> {
        setTextColor(C.DANGER)
        background = pressable(box(C.BG, C.DANGER), box(C.SURFACE_ACTIVE, C.DANGER))
      }
    }
    setOnClickListener { onClick() }
  }

  fun divider(): View = View(ctx).apply {
    setBackgroundColor(C.LINE)
    layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, line)
  }

  fun column(): LinearLayout = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
  fun row(): LinearLayout = LinearLayout(ctx).apply {
    orientation = LinearLayout.HORIZONTAL
    gravity = Gravity.CENTER_VERTICAL
  }

  fun lp(w: Int = ViewGroup.LayoutParams.MATCH_PARENT, h: Int = ViewGroup.LayoutParams.WRAP_CONTENT, weight: Float = 0f) =
    LinearLayout.LayoutParams(w, h, weight)

  fun margins(l: Int = 0, t: Int = 0, r: Int = 0, b: Int = 0): LinearLayout.LayoutParams =
    lp().apply { setMargins(dp(l), dp(t), dp(r), dp(b)) }

  // Título de sección en mayúsculas pequeñas, como en Ajustes de Spectre.
  fun section(s: String): TextView = text(s.uppercase(Locale.ROOT), 11f, C.FAINT, bold = true).apply {
    letterSpacing = 0.08f
    setPadding(dp(16), dp(20), dp(16), dp(8))
  }

  // Fila de ajuste con una casilla cuadrada a la derecha.
  fun toggle(title: String, detail: String?, checked: Boolean, onChange: (Boolean) -> Unit): View {
    val check = CheckBox(ctx, this).apply { this.checked = checked }
    val r = row().apply {
      setPadding(dp(16), dp(12), dp(16), dp(12))
      background = pressable(ColorDrawable(0), box(C.SURFACE))
      isClickable = true
    }
    val texts = column()
    texts.addView(text(title, 15f))
    if (detail != null) texts.addView(text(detail, 13f, C.MUTED), margins(t = 4))
    r.addView(texts, lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
    r.addView(check, LinearLayout.LayoutParams(dp(20), dp(20)).apply { leftMargin = dp(16) })
    r.setOnClickListener {
      check.checked = !check.checked
      onChange(check.checked)
    }
    return r
  }

  // Opciones en fila, una elegida (como los botones de Spectre: planos y con borde).
  fun choices(options: List<String>, selected: Int, onSelect: (Int) -> Unit): LinearLayout {
    val r = row()
    val views = ArrayList<TextView>()
    fun paint(sel: Int) = views.forEachIndexed { i, v ->
      v.setTextColor(if (i == sel) C.ON_ACCENT else C.TEXT)
      v.background = if (i == sel) box(C.ACCENT) else pressable(box(C.BG, C.LINE_STRONG), box(C.SURFACE_ACTIVE, C.LINE_STRONG))
    }
    options.forEachIndexed { i, label ->
      val v = TextView(ctx).apply {
        text = label
        gravity = Gravity.CENTER
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
        typeface = medium
        includeFontPadding = false
        minHeight = dp(38)
        setPadding(dp(4), dp(8), dp(4), dp(8))
        isClickable = true
        setOnClickListener {
          paint(i)
          onSelect(i)
        }
      }
      views += v
      // Bordes pegados: el de la derecha de una opción es el de la izquierda de la siguiente.
      r.addView(v, lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { if (i > 0) leftMargin = -line })
    }
    paint(selected)
    return r
  }
}

// Casilla cuadrada de 1 px; marcada: blanca con la marca en negro.
class CheckBox(context: Context, private val ui: Ui) : View(context) {
  var checked = false
    set(v) {
      field = v
      invalidate()
    }
  private val paint = Paint(Paint.ANTI_ALIAS_FLAG)

  override fun onDraw(canvas: Canvas) {
    val w = width.toFloat()
    val h = height.toFloat()
    val l = ui.line.toFloat()
    if (checked) {
      paint.style = Paint.Style.FILL
      paint.color = C.ACCENT
      canvas.drawRect(0f, 0f, w, h, paint)
      paint.color = C.ON_ACCENT
      paint.style = Paint.Style.STROKE
      paint.strokeWidth = l * 2
      paint.strokeCap = Paint.Cap.SQUARE
      paint.strokeJoin = Paint.Join.MITER
      val path = android.graphics.Path().apply {
        moveTo(w * 0.24f, h * 0.52f)
        lineTo(w * 0.42f, h * 0.70f)
        lineTo(w * 0.77f, h * 0.32f)
      }
      canvas.drawPath(path, paint)
    } else {
      paint.style = Paint.Style.STROKE
      paint.strokeWidth = l
      paint.color = C.LINE_STRONG
      canvas.drawRect(l / 2, l / 2, w - l / 2, h - l / 2, paint)
    }
  }
}

// Barra de progreso plana de 2 px.
class Bar(context: Context) : View(context) {
  var fraction = 0f
    set(v) {
      field = v.coerceIn(0f, 1f)
      invalidate()
    }
  var color = C.ACCENT
  private val paint = Paint()

  override fun onDraw(canvas: Canvas) {
    paint.color = C.LINE
    canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)
    paint.color = color
    canvas.drawRect(0f, 0f, width * fraction, height.toFloat(), paint)
  }
}

// Hueco de la miniatura: negro con una línea y el tipo escrito mientras no hay imagen.
class Thumb(context: Context, ui: Ui) : FrameLayout(context) {
  val image = android.widget.ImageView(context).apply { scaleType = android.widget.ImageView.ScaleType.CENTER_CROP }
  val label = ui.text("", 11f, C.FAINT, bold = true).apply { gravity = Gravity.CENTER }
  val badge = ui.text("", 11f, C.TEXT, bold = true).apply {
    setBackgroundColor(0xCC000000.toInt())
    setPadding(ui.dp(5), ui.dp(2), ui.dp(5), ui.dp(2))
    visibility = GONE
  }

  init {
    background = ui.box(C.SURFACE, C.LINE)
    addView(label, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
    addView(image, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT).apply {
      setMargins(ui.line, ui.line, ui.line, ui.line)
    })
    addView(badge, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.BOTTOM or Gravity.END).apply {
      setMargins(0, 0, ui.dp(4), ui.dp(4))
    })
  }

  // 16:9 al ancho que le den.
  override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
    val w = MeasureSpec.getSize(widthMeasureSpec)
    super.onMeasure(
      MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(w * 9 / 16, MeasureSpec.EXACTLY),
    )
  }
}

object Fmt {
  private val ES: Locale = Locale.forLanguageTag("es-ES")

  fun bytes(n: Long): String {
    if (n < 1024) return "$n B"
    val kb = n / 1024.0
    if (kb < 1024) return String.format(ES, "%.0f KB", kb)
    val mb = kb / 1024.0
    if (mb < 1024) return String.format(ES, if (mb < 10) "%.1f MB" else "%.0f MB", mb)
    return String.format(ES, "%.2f GB", mb / 1024.0)
  }

  fun duration(ms: Long): String {
    val s = (ms + 500) / 1000
    return if (s >= 3600) String.format(Locale.ROOT, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60)
    else String.format(Locale.ROOT, "%d:%02d", s / 60, s % 60)
  }

  fun date(ms: Long): String {
    if (ms <= 0) return ""
    val f = java.text.SimpleDateFormat("d MMM, HH:mm", ES)
    return f.format(java.util.Date(ms))
  }

  // "hace 5 min", "hace 2 h", "hace 3 días".
  fun ago(ms: Long, now: Long = System.currentTimeMillis()): String {
    val d = (now - ms).coerceAtLeast(0) / 1000
    return when {
      d < 60 -> "ahora"
      d < 3600 -> "hace ${d / 60} min"
      d < 86400 -> "hace ${d / 3600} h"
      d < 2 * 86400 -> "ayer"
      else -> "hace ${d / 86400} días"
    }
  }
}
