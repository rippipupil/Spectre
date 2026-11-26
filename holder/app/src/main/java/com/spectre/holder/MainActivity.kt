package com.spectre.holder

import android.Manifest
import android.app.Activity
import android.content.ClipboardManager
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.window.OnBackInvokedDispatcher

class MainActivity : Activity() {
  private lateinit var ui: Ui
  private lateinit var root: FrameLayout
  private val stack = ArrayList<Screen>()
  private var visible = false
  private var sheet: View? = null
  private val onChange: () -> Unit = { stack.lastOrNull()?.changed() }

  // Una pantalla de la app (inicio, ajustes, lector del QR, visor).
  abstract inner class Screen {
    lateinit var view: View
    abstract fun build(): View
    open fun shown() {}
    open fun hidden() {}
    open fun changed() {}
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    Holder.init(this)
    ui = Ui(this)
    root = FrameLayout(this).apply { setBackgroundColor(C.BG) }
    // Android 15 dibuja la app bajo las barras del sistema: el contenido se aparta de ellas.
    // WindowInsets.Type es de Android 11; en el 10 valen los insets de siempre.
    root.setOnApplyWindowInsetsListener { v, insets ->
      if (Build.VERSION.SDK_INT >= 30) {
        val i = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
        v.setPadding(i.left, i.top, i.right, i.bottom)
        WindowInsets.CONSUMED
      } else {
        @Suppress("DEPRECATION")
        v.setPadding(
          insets.systemWindowInsetLeft,
          insets.systemWindowInsetTop,
          insets.systemWindowInsetRight,
          insets.systemWindowInsetBottom,
        )
        @Suppress("DEPRECATION")
        insets.consumeSystemWindowInsets()
      }
    }
    setContentView(root)
    if (Build.VERSION.SDK_INT >= 33) {
      onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT) { back() }
    }
    push(Home())
    handleLink(intent)
  }

  override fun onNewIntent(intent: Intent) {
    super.onNewIntent(intent)
    handleLink(intent)
  }

  override fun onStart() {
    super.onStart()
    visible = true
    Holder.listen(onChange)
    stack.lastOrNull()?.shown()
    if (Holder.cloud != null) Holder.refresh(auto = true)
  }

  override fun onStop() {
    visible = false
    Holder.unlisten(onChange)
    stack.lastOrNull()?.hidden()
    super.onStop()
  }

  @Deprecated("Android 12 y anteriores")
  override fun onBackPressed() {
    back()
  }

  private fun back() {
    if (sheet != null) {
      closeSheet()
      return
    }
    if (stack.size > 1) pop() else finish()
  }

  private fun push(s: Screen) {
    stack.lastOrNull()?.let {
      it.hidden()
      root.removeView(it.view)
    }
    s.view = s.build()
    stack += s
    root.addView(s.view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
    if (visible) s.shown()
  }

  private fun pop() {
    val s = stack.removeAt(stack.size - 1)
    s.hidden()
    root.removeView(s.view)
    stack.lastOrNull()?.let {
      root.addView(it.view, 0)
      if (visible) it.shown()
      it.changed()
    }
  }

  private fun popToHome() {
    while (stack.size > 1) pop()
  }

  // ------------------------------------------------------------ piezas comunes

  private fun topBar(title: String, subtitle: String? = null, backButton: Boolean, right: View? = null): View {
    val bar = ui.column()
    val r = ui.row().apply { setPadding(ui.dp(if (backButton) 4 else 16), ui.dp(8), ui.dp(8), ui.dp(8)) }
    if (backButton) r.addView(ui.button("Volver", Ui.Style.GHOST) { back() })
    val texts = ui.column().apply { setPadding(ui.dp(if (backButton) 4 else 0), 0, 0, 0) }
    texts.addView(ui.text(title, 18f, bold = true, lines = 1))
    if (subtitle != null) texts.addView(ui.text(subtitle, 12f, C.MUTED, lines = 1), ui.margins(t = 3))
    r.addView(texts, ui.lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
    if (right != null) r.addView(right)
    r.minimumHeight = ui.dp(56)
    bar.addView(r)
    bar.addView(ui.divider())
    return bar
  }

  // Menú o confirmación abajo, plano, sobre la pantalla oscurecida.
  private fun showSheet(title: String, detail: String?, actions: List<Pair<String, Ui.Style>>, onPick: (Int) -> Unit) {
    closeSheet()
    val shade = FrameLayout(this).apply {
      setBackgroundColor(0xB3000000.toInt())
      isClickable = true
      setOnClickListener { closeSheet() }
    }
    val panel = ui.column().apply {
      background = ui.box(C.BG_ALT, C.LINE_STRONG)
      setPadding(ui.dp(16), ui.dp(16), ui.dp(16), ui.dp(16))
      isClickable = true
    }
    panel.addView(ui.text(title, 16f, bold = true))
    if (detail != null) panel.addView(ui.text(detail, 14f, C.MUTED), ui.margins(t = 6))
    actions.forEachIndexed { i, (label, style) ->
      panel.addView(ui.button(label, style) {
        closeSheet()
        onPick(i)
      }, ui.margins(t = if (i == 0) 16 else 8))
    }
    panel.addView(ui.button("Cancelar", Ui.Style.GHOST) { closeSheet() }, ui.margins(t = 8))
    shade.addView(panel, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM))
    root.addView(shade, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
    sheet = shade
  }

  private fun closeSheet() {
    sheet?.let { root.removeView(it) }
    sheet = null
  }

  private fun message(title: String, detail: String) = showSheet(title, detail, emptyList()) {}

  private fun askNotifications() {
    if (Build.VERSION.SDK_INT < 33 || Holder.store.askedNotifications) return
    if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) return
    Holder.store.askedNotifications = true
    requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQ_NOTIFY)
  }

  private fun download(list: List<Clip>) {
    if (list.isEmpty()) return
    askNotifications()
    Holder.download(list)
  }

  private fun scan() {
    if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
      push(Scan())
    } else {
      requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_CAMERA)
    }
  }

  override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
    super.onRequestPermissionsResult(requestCode, permissions, grantResults)
    if (requestCode != REQ_CAMERA) return
    if (grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED) {
      push(Scan())
    } else {
      message(
        "Sin permiso para la cámara",
        "Hace falta para leer el código de Spectre. También puedes copiar el código en el PC " +
          "(Ajustes > Móvil > Copiar el código), pasarlo al móvil y pulsar «Pegar el código».",
      )
    }
  }

  private fun pasteCode() {
    val cm = getSystemService(ClipboardManager::class.java)
    val text = cm?.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(this)?.toString().orEmpty()
    val config = CloudConfig.fromCode(text)
    // Lleva las claves: fuera del portapapeles (y de su historial en teclados como Gboard).
    if (config != null) {
      try {
        cm?.clearPrimaryClip()
      } catch (e: Exception) {
      }
    }
    if (config == null) {
      message(
        "No hay un código en el portapapeles",
        "En el PC: Spectre > Ajustes > Móvil > Copiar el código. Pásalo al móvil y vuelve a pulsar «Pegar el código».",
      )
      return
    }
    paired(config)
  }

  private fun paired(config: CloudConfig) {
    Holder.pair(config)
    popToHome()
    stack.firstOrNull()?.changed()
  }

  // spectre-holder:<código> abierto desde otra app (el lector de QR del sistema).
  private fun handleLink(intent: Intent?) {
    val data = intent?.dataString ?: return
    val config = CloudConfig.fromCode(data) ?: return
    intent.data = null
    // Un enlace puede venir de cualquiera (un chat): se dice a qué nube lleva y
    // si cambia la de ahora.
    val name = config.pcName.ifEmpty { "un PC" }
    val now = Holder.cloud
    val where = "${config.bucket} en ${config.host}"
    val text = if (now == null) {
      "Spectre Holder leerá lo que $name mande a $where. Empareja solo si es tu PC."
    } else if (now.host == config.host && now.bucket == config.bucket) {
      "Es la misma nube de ahora ($where), con las claves de $name."
    } else {
      "Cambia la nube de ahora (${now.bucket} en ${now.host}) por $where, de $name. Si no lo has pedido tú, " +
        "no lo aceptes: verías y descargarías lo que otra persona sube."
    }
    showSheet("¿Emparejar con $name?", text, listOf("Emparejar" to Ui.Style.PRIMARY)) {
      paired(config)
    }
  }

  // ------------------------------------------------------------------ inicio

  inner class Home : Screen() {
    private lateinit var content: LinearLayout
    private lateinit var scroll: ScrollView
    private lateinit var header: FrameLayout
    private var signature = ""
    private val rows = HashMap<String, Row>()
    private var bulk: TextView? = null
    private var status: TextView? = null

    override fun build(): View {
      val col = ui.column()
      header = FrameLayout(this@MainActivity)
      col.addView(header)
      scroll = ScrollView(this@MainActivity).apply { isFillViewport = true }
      content = ui.column()
      scroll.addView(content)
      col.addView(scroll, ui.lp(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
      render()
      return col
    }

    override fun changed() = render()

    private fun sign(): String {
      val c = Holder.cloud ?: return "welcome"
      val saved = Holder.store.saved()
      return buildString {
        append(c.host).append(c.bucket).append('|').append(Holder.listed).append(Holder.refreshing && !Holder.listed)
        append('|').append(Holder.error)
        Holder.clips.forEach { append(it.key).append(',') }
        append('|')
        saved.take(30).forEach { append(it.key).append(',') }
        append('|').append(Holder.busy())
      }
    }

    private fun render() {
      val sig = sign()
      if (sig == signature) {
        tick()
        return
      }
      signature = sig
      rows.clear()
      bulk = null
      status = null
      header.removeAllViews()
      content.removeAllViews()
      val c = Holder.cloud
      if (c == null) welcome() else list(c)
      tick()
    }

    private fun welcome() {
      header.addView(topBar("Spectre Holder", null, backButton = false))
      val box = ui.column().apply { setPadding(ui.dp(24), ui.dp(32), ui.dp(24), ui.dp(24)) }
      val logo = ImageView(this@MainActivity).apply {
        val res = resources.getIdentifier("ic_logo", "drawable", packageName)
        if (res != 0) setImageResource(res)
        adjustViewBounds = true
      }
      box.addView(logo, LinearLayout.LayoutParams(ui.dp(72), ui.dp(72)))
      box.addView(ui.text("Tus clips del PC, en el móvil", 22f, bold = true), ui.margins(t = 24))
      box.addView(
        ui.text(
          "En Spectre, «Enviar al móvil» sube el clip a tu nube (R2 o B2) y aquí lo descargas a la galería. " +
            "Al descargarlo se borra de la nube: solo está allí de paso.",
          15f, C.MUTED,
        ),
        ui.margins(t = 10),
      )
      box.addView(ui.section("Emparejar").apply { setPadding(0, ui.dp(28), 0, ui.dp(8)) })
      box.addView(ui.text("1. En el PC, abre Spectre > Ajustes > Móvil.", 15f), ui.margins(t = 4))
      box.addView(ui.text("2. Pulsa «Enseñar el código».", 15f), ui.margins(t = 8))
      box.addView(ui.text("3. Escanéalo con este móvil.", 15f), ui.margins(t = 8))
      box.addView(ui.button("Escanear el código", Ui.Style.PRIMARY) { scan() }, ui.margins(t = 24))
      box.addView(ui.button("Pegar el código", Ui.Style.SECONDARY) { pasteCode() }, ui.margins(t = 8))
      box.addView(
        ui.text("El código lleva las claves de tu nube: Spectre Holder solo usa la carpeta spectre/movil/ del bucket.", 12f, C.FAINT),
        ui.margins(t = 16),
      )
      content.addView(box)
    }

    private fun list(c: CloudConfig) {
      val sub = if (c.pcName.isNotEmpty()) "Nube de ${c.pcName}" else c.bucket
      header.addView(topBar("Spectre Holder", sub, backButton = false, right = ui.button("Ajustes", Ui.Style.GHOST) { push(Settings()) }))

      val err = Holder.error
      if (err != null) {
        val box = ui.column().apply {
          background = ui.box(C.BG_ALT, C.DANGER)
          setPadding(ui.dp(14), ui.dp(12), ui.dp(14), ui.dp(12))
        }
        box.addView(ui.text("No se pudo leer la nube", 14f, C.DANGER, bold = true))
        box.addView(ui.text(err, 13f, C.MUTED), ui.margins(t = 4))
        box.addView(ui.button("Volver a probar", Ui.Style.SECONDARY) { Holder.refresh() }, ui.margins(t = 10))
        content.addView(box, ui.margins(16, 16, 16, 0))
      }

      val head = ui.row().apply { setPadding(0, 0, ui.dp(8), 0) }
      val title = ui.section(if (Holder.listed) "En la nube · ${Holder.clips.size}" else "En la nube")
      head.addView(title, ui.lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
      head.addView(ui.button("Actualizar", Ui.Style.GHOST) { Holder.refresh() }.apply { minHeight = ui.dp(36) })
      content.addView(head)
      content.addView(ui.divider())

      if (!Holder.listed) {
        status = ui.text(if (Holder.refreshing) "Mirando la nube…" else "", 14f, C.MUTED).apply {
          setPadding(ui.dp(16), ui.dp(16), ui.dp(16), ui.dp(16))
        }
        content.addView(status)
      } else if (Holder.clips.isEmpty()) {
        content.addView(
          ui.text(
            "No hay nada esperando. En el PC, pulsa «Enviar al móvil» en un clip de la biblioteca o del panel Alt+Z.",
            14f, C.MUTED,
          ).apply { setPadding(ui.dp(16), ui.dp(16), ui.dp(16), ui.dp(16)) },
        )
      } else {
        for (clip in Holder.clips) {
          val r = Row(clip)
          rows[clip.key] = r
          content.addView(r.view)
          content.addView(ui.divider())
        }
        bulk = ui.button("", Ui.Style.PRIMARY) {
          if (Holder.busy()) {
            Holder.cancelAll()
          } else {
            download(Holder.clips.filter { !Holder.isSaved(it.key) })
          }
        }
        content.addView(bulk, ui.margins(16, 16, 16, 0))
      }

      val saved = Holder.store.saved()
      if (saved.isNotEmpty()) {
        content.addView(ui.section("En el móvil"))
        content.addView(ui.divider())
        for (s in saved.take(30)) {
          content.addView(savedRow(s))
          content.addView(ui.divider())
        }
      }
      content.addView(
        ui.text("Se guardan en la galería: Movies/Spectre (vídeos) y Pictures/Spectre (capturas).", 12f, C.FAINT)
          .apply { setPadding(ui.dp(16), ui.dp(16), ui.dp(16), ui.dp(24)) },
      )
    }

    // Lo que cambia sin rehacer la lista: progreso, estado y botones.
    private fun tick() {
      rows.values.forEach { it.update() }
      bulk?.let { b ->
        val left = Holder.clips.filter { !Holder.isSaved(it.key) }
        when {
          Holder.busy() -> {
            b.text = "Cancelar las descargas"
            b.isEnabled = true
          }
          left.isEmpty() -> {
            b.text = "Todo descargado"
            b.isEnabled = false
          }
          else -> {
            b.text = "Descargar todo (${left.size} · ${Fmt.bytes(left.sumOf { it.size })})"
            b.isEnabled = true
          }
        }
      }
      status?.text = if (Holder.refreshing) "Mirando la nube…" else ""
    }

    private fun savedRow(s: Saved): View {
      val r = ui.row().apply {
        setPadding(ui.dp(16), ui.dp(10), ui.dp(12), ui.dp(10))
        isClickable = true
        background = android.graphics.drawable.StateListDrawable().apply {
          addState(intArrayOf(android.R.attr.state_pressed), ui.box(C.SURFACE))
          addState(intArrayOf(), android.graphics.drawable.ColorDrawable(0))
        }
      }
      val thumb = Thumb(this@MainActivity, ui)
      thumb.label.text = if (s.mime.startsWith("image/")) "CAPTURA" else "VÍDEO"
      Thumbs.local(s) { b -> if (b != null) thumb.image.setImageBitmap(b) }
      r.addView(thumb, LinearLayout.LayoutParams(ui.dp(96), ViewGroup.LayoutParams.WRAP_CONTENT))
      val texts = ui.column().apply { setPadding(ui.dp(12), 0, ui.dp(8), 0) }
      texts.addView(ui.text(s.title.ifEmpty { s.name }, 15f, bold = true, lines = 1))
      texts.addView(ui.text("${Fmt.ago(s.at)} · ${Fmt.bytes(s.size)}", 13f, C.MUTED, lines = 1), ui.margins(t = 4))
      r.addView(texts, ui.lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
      r.addView(ui.button("Compartir", Ui.Style.GHOST) { share(s) }.apply { minHeight = ui.dp(36) })
      r.setOnClickListener { push(Viewer(local = s, remote = null)) }
      r.setOnLongClickListener {
        showSheet(s.title.ifEmpty { s.name }, s.name, listOf("Abrir con otra app" to Ui.Style.SECONDARY, "Quitar de la lista" to Ui.Style.SECONDARY)) {
          if (it == 0) openWith(s) else {
            Holder.store.removeSaved(s.key)
            changed()
          }
        }
        true
      }
      return r
    }

    // Fila de algo que está en la nube.
    inner class Row(private val clip: Clip) {
      val view: LinearLayout = ui.column()
      private val thumb = Thumb(this@MainActivity, ui)
      private val state = ui.text("", 13f, C.MUTED, lines = 2)
      private val action = ui.button("Descargar", Ui.Style.SECONDARY) { act() }.apply { minHeight = ui.dp(38) }
      private val bar = Bar(this@MainActivity)

      init {
        val r = ui.row().apply {
          setPadding(ui.dp(16), ui.dp(10), ui.dp(12), ui.dp(10))
          isClickable = true
          background = android.graphics.drawable.StateListDrawable().apply {
            addState(intArrayOf(android.R.attr.state_pressed), ui.box(C.SURFACE))
            addState(intArrayOf(), android.graphics.drawable.ColorDrawable(0))
          }
        }
        thumb.label.text = clip.kind.label.uppercase()
        Thumbs.cached(clip.key)?.let { thumb.image.setImageBitmap(it) } ?: Thumbs.remote(clip) { b ->
          if (b != null) thumb.image.setImageBitmap(b)
          showDuration()
        }
        showDuration()
        r.addView(thumb, LinearLayout.LayoutParams(ui.dp(112), ViewGroup.LayoutParams.WRAP_CONTENT))
        val texts = ui.column().apply { setPadding(ui.dp(12), 0, ui.dp(8), 0) }
        texts.addView(ui.text(clip.title, 15f, bold = true, lines = 1))
        val meta = listOf(Fmt.date(clip.modified), Fmt.bytes(clip.size)).filter { it.isNotEmpty() }.joinToString(" · ")
        texts.addView(ui.text(meta, 13f, C.MUTED, lines = 1), ui.margins(t = 4))
        texts.addView(state, ui.margins(t = 4))
        r.addView(texts, ui.lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        r.addView(action)
        r.setOnClickListener { push(Viewer(local = null, remote = clip)) }
        r.setOnLongClickListener {
          menu()
          true
        }
        view.addView(r)
        view.addView(bar, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ui.dp(2)))
      }

      private fun showDuration() {
        val d = Thumbs.duration(clip.key)
        if (d > 0) {
          thumb.badge.text = Fmt.duration(d)
          thumb.badge.visibility = View.VISIBLE
        }
      }

      private fun act() {
        when {
          Holder.current?.key == clip.key || Holder.queued(clip.key) -> Holder.cancel(clip.key)
          Holder.isSaved(clip.key) -> Holder.store.saved().firstOrNull { it.key == clip.key }?.let { push(Viewer(it, null)) }
          else -> download(listOf(clip))
        }
      }

      private fun menu() {
        showSheet(
          clip.title, clip.name,
          listOf("Descargar" to Ui.Style.PRIMARY, "Ver sin descargar" to Ui.Style.SECONDARY, "Borrar de la nube" to Ui.Style.DANGER),
        ) {
          when (it) {
            0 -> download(listOf(clip))
            1 -> push(Viewer(local = null, remote = clip))
            else -> Holder.deleteRemote(clip) { err ->
              if (err != null) message("No se pudo borrar", err) else Thumbs.forget(clip.key)
            }
          }
        }
      }

      fun update() {
        val downloading = Holder.current?.key == clip.key
        val queued = Holder.queued(clip.key)
        val failed = Holder.failed[clip.key]
        val saved = Holder.isSaved(clip.key)
        bar.visibility = if (downloading) View.VISIBLE else View.INVISIBLE
        bar.fraction = if (downloading) Holder.progress else 0f
        state.setTextColor(if (failed != null) C.DANGER else C.MUTED)
        state.text = when {
          downloading -> {
            val speed = if (Holder.speed > 0) " · ${Fmt.bytes(Holder.speed.toLong())}/s" else ""
            "Descargando ${(Holder.progress * 100).toInt()} %$speed"
          }
          queued -> "En la cola"
          failed != null -> failed
          saved -> "En la galería"
          else -> expiry()
        }
        action.text = when {
          downloading || queued -> "Cancelar"
          saved -> "Ver"
          failed != null -> "Reintentar"
          else -> "Descargar"
        }
      }

      private fun expiry(): String {
        val days = Holder.store.keepDays
        if (days <= 0 || clip.seen <= 0) return "Espera en la nube"
        val left = (clip.seen + days * 86_400_000L - System.currentTimeMillis()) / 86_400_000.0
        return when {
          left < 1 -> "Se borra de la nube hoy"
          left < 2 -> "Se borra de la nube mañana"
          else -> "Se borra de la nube en ${left.toInt()} días"
        }
      }
    }
  }

  private fun share(s: Saved) {
    val send = Intent(Intent.ACTION_SEND).apply {
      type = s.mime
      putExtra(Intent.EXTRA_STREAM, Uri.parse(s.uri))
      addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
    try {
      startActivity(Intent.createChooser(send, "Compartir"))
    } catch (e: Exception) {
      message("No se puede compartir", "Ninguna app de este móvil acepta este archivo.")
    }
  }

  private fun openWith(s: Saved) {
    val view = Intent(Intent.ACTION_VIEW).apply {
      setDataAndType(Uri.parse(s.uri), s.mime)
      addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
    try {
      startActivity(view)
    } catch (e: Exception) {
      message("No se puede abrir", "Ninguna app de este móvil abre este archivo.")
    }
  }

  // ----------------------------------------------------------------- ajustes

  inner class Settings : Screen() {
    override fun build(): View {
      val col = ui.column()
      col.addView(topBar("Ajustes", null, backButton = true))
      val scroll = ScrollView(this@MainActivity)
      val c = ui.column()
      val store = Holder.store

      c.addView(ui.section("Descargas"))
      c.addView(
        ui.toggle("Borrar de la nube al descargar", "Lo descargado deja de ocupar sitio en tu bucket.", store.deleteAfterDownload) {
          store.deleteAfterDownload = it
        },
      )
      c.addView(ui.divider())
      c.addView(
        ui.toggle("Descargar lo nuevo al abrir la app", "Sin tener que pulsar «Descargar» en cada uno.", store.autoDownload) {
          store.autoDownload = it
        },
      )
      c.addView(ui.divider())
      c.addView(
        ui.toggle("Miniaturas con datos móviles", "Cada miniatura gasta unos cientos de KB.", store.thumbsOnMobileData) {
          store.thumbsOnMobileData = it
        },
      )

      c.addView(ui.section("Borrar de la nube lo que lleve"))
      val days = listOf(1, 3, 7, 30, 0)
      val labels = listOf("1 día", "3 días", "7 días", "30 días", "Nunca")
      c.addView(
        ui.choices(labels, days.indexOf(store.keepDays).coerceAtLeast(0)) { store.keepDays = days[it] },
        ui.margins(16, 0, 16, 0),
      )
      c.addView(
        ui.text("Se comprueba al abrir la app. Solo borra de la nube, nunca lo que ya está en el móvil.", 12f, C.FAINT)
          .apply { setPadding(ui.dp(16), ui.dp(8), ui.dp(16), 0) },
      )

      c.addView(ui.section("Nube"))
      val cloud = Holder.cloud
      if (cloud != null) {
        val info = ui.column().apply { setPadding(ui.dp(16), 0, ui.dp(16), 0) }
        if (cloud.pcName.isNotEmpty()) info.addView(ui.text("PC: ${cloud.pcName}", 14f))
        info.addView(ui.text("Bucket: ${cloud.bucket}", 14f), ui.margins(t = 4))
        info.addView(ui.text(cloud.host, 12f, C.FAINT, lines = 1), ui.margins(t = 4))
        c.addView(info)
        c.addView(ui.button("Emparejar con otro código", Ui.Style.SECONDARY) { scan() }, ui.margins(16, 16, 16, 0))
        c.addView(
          ui.button("Desemparejar", Ui.Style.DANGER) {
            showSheet(
              "¿Desemparejar?",
              "El móvil olvida las claves de la nube. Lo que hay en la nube y en la galería no se borra.",
              listOf("Desemparejar" to Ui.Style.DANGER),
            ) {
              Holder.unpair()
              popToHome()
            }
          },
          ui.margins(16, 8, 16, 0),
        )
      }
      c.addView(ui.section("Spectre Holder"))
      val version = try {
        packageManager.getPackageInfo(packageName, 0).versionName ?: ""
      } catch (e: Exception) {
        ""
      }
      c.addView(ui.text("Versión $version", 14f, C.MUTED).apply { setPadding(ui.dp(16), 0, ui.dp(16), ui.dp(24)) })
      scroll.addView(c)
      col.addView(scroll, ui.lp(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
      return col
    }
  }

  // ------------------------------------------------------------ lector del QR

  inner class Scan : Screen() {
    private var scanner: Scanner? = null
    private lateinit var hint: TextView

    override fun build(): View {
      val col = ui.column()
      col.addView(topBar("Escanear el código", "Spectre > Ajustes > Móvil > Enseñar el código", backButton = true))
      val frame = FrameLayout(this@MainActivity)
      scanner = Scanner(this@MainActivity, ui, ::read) { err -> hint.text = err }
      frame.addView(scanner, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
      col.addView(frame, ui.lp(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
      val bottom = ui.column().apply { setPadding(ui.dp(16), ui.dp(12), ui.dp(16), ui.dp(16)) }
      hint = ui.text("Apunta al código que enseña Spectre en el PC.", 14f, C.MUTED)
      bottom.addView(hint)
      bottom.addView(ui.button("Pegar el código", Ui.Style.SECONDARY) { pasteCode() }, ui.margins(t = 12))
      col.addView(ui.divider())
      col.addView(bottom)
      return col
    }

    override fun shown() {
      scanner?.start()
    }

    override fun hidden() {
      scanner?.stop()
    }

    private fun read(text: String) {
      val config = CloudConfig.fromCode(text)
      if (config == null) {
        hint.text = "Ese código no es el de Spectre. En el PC: Ajustes > Móvil > Enseñar el código."
        hint.setTextColor(C.WARNING)
        // Vuelve a leer al rato.
        Holder.main.postDelayed({
          if (stack.lastOrNull() === this && visible) {
            scanner?.stop()
            scanner?.start()
          }
        }, 1500)
        return
      }
      scanner?.stop()
      paired(config)
    }
  }

  // ------------------------------------------------------------------- visor

  inner class Viewer(private val local: Saved?, private val remote: Clip?) : Screen() {
    private var video: android.widget.VideoView? = null
    private var seek: android.widget.SeekBar? = null
    private var time: TextView? = null
    private var play: TextView? = null
    private var info: TextView? = null
    private var dragging = false
    private val ticker = object : Runnable {
      override fun run() {
        val v = video ?: return
        if (!dragging && v.duration > 0) {
          seek?.max = v.duration
          seek?.progress = v.currentPosition
          time?.text = "${Fmt.duration(v.currentPosition.toLong())} / ${Fmt.duration(v.duration.toLong())}"
        }
        play?.text = if (v.isPlaying) "Pausa" else "Ver"
        Holder.main.postDelayed(this, 250)
      }
    }

    private val title: String get() = local?.title?.ifEmpty { local.name } ?: remote?.title ?: ""
    private val mime: String get() = local?.mime ?: remote?.mime ?: ""
    private val isImage: Boolean get() = mime.startsWith("image/")

    override fun build(): View {
      val col = ui.column()
      val actionBtn = when {
        local != null -> ui.button("Compartir", Ui.Style.GHOST) { share(local) }
        remote != null -> ui.button("Descargar", Ui.Style.GHOST) {
          download(listOf(remote))
          back()
        }
        else -> null
      }
      val sub = local?.name ?: remote?.let { "${Fmt.date(it.modified)} · ${Fmt.bytes(it.size)} · en la nube" }
      col.addView(topBar(title, sub, backButton = true, right = actionBtn))
      val stage = FrameLayout(this@MainActivity).apply { setBackgroundColor(C.BG) }
      info = ui.text(if (remote != null) "Cargando de la nube…" else "", 14f, C.MUTED).apply { gravity = Gravity.CENTER }
      stage.addView(info, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
      val uri: Uri? = when {
        local != null -> Uri.parse(local.uri)
        remote != null -> Holder.streamUrl(remote)?.let(Uri::parse)
        else -> null
      }
      if (uri == null) {
        info?.text = "No se puede abrir."
      } else if (isImage) {
        val img = ImageView(this@MainActivity).apply { scaleType = ImageView.ScaleType.FIT_CENTER }
        stage.addView(img, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        loadImage(uri, img)
      } else {
        val v = android.widget.VideoView(this@MainActivity)
        video = v
        stage.addView(v, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
        v.setOnPreparedListener { mp ->
          info?.text = ""
          mp.isLooping = false
          v.start()
        }
        v.setOnErrorListener { _, _, _ ->
          info?.text = if (remote != null) "No se puede ver desde la nube. Descárgalo y ábrelo desde la galería." else
            "Este móvil no puede reproducirlo. Prueba con «Abrir con otra app»."
          true
        }
        v.setVideoURI(uri)
      }
      col.addView(stage, ui.lp(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
      if (video != null) col.addView(controls())
      return col
    }

    private fun controls(): View {
      val box = ui.column()
      box.addView(ui.divider())
      val r = ui.row().apply { setPadding(ui.dp(8), ui.dp(8), ui.dp(16), ui.dp(8)) }
      play = ui.button("Pausa", Ui.Style.GHOST) {
        val v = video ?: return@button
        if (v.isPlaying) v.pause() else v.start()
      }.apply { minWidth = ui.dp(72) }
      r.addView(play)
      seek = android.widget.SeekBar(this@MainActivity).apply {
        progressDrawable = android.graphics.drawable.LayerDrawable(
          arrayOf(
            ui.box(C.LINE),
            android.graphics.drawable.ClipDrawable(ui.box(C.ACCENT), Gravity.START, android.graphics.drawable.ClipDrawable.HORIZONTAL),
          ),
        ).apply {
          setId(0, android.R.id.background)
          setId(1, android.R.id.progress)
        }
        thumb = ui.box(C.ACCENT).apply { setSize(ui.dp(4), ui.dp(18)) }
        splitTrack = false
        minHeight = ui.dp(2)
        maxHeight = ui.dp(2)
        setOnSeekBarChangeListener(object : android.widget.SeekBar.OnSeekBarChangeListener {
          override fun onProgressChanged(s: android.widget.SeekBar, p: Int, fromUser: Boolean) {
            if (fromUser) time?.text = "${Fmt.duration(p.toLong())} / ${Fmt.duration(s.max.toLong())}"
          }

          override fun onStartTrackingTouch(s: android.widget.SeekBar) {
            dragging = true
          }

          override fun onStopTrackingTouch(s: android.widget.SeekBar) {
            dragging = false
            video?.seekTo(s.progress)
          }
        })
      }
      r.addView(seek, ui.lp(0, ui.dp(32), 1f))
      time = ui.text("0:00 / 0:00", 12f, C.MUTED).apply { setPadding(ui.dp(8), 0, 0, 0) }
      r.addView(time)
      box.addView(r)
      return box
    }

    private fun loadImage(uri: Uri, into: ImageView) {
      val maxSide = maxOf(resources.displayMetrics.widthPixels, resources.displayMetrics.heightPixels)
      Thread {
        val bmp = try {
          val bytes = if (uri.scheme == "https") java.net.URL(uri.toString()).openStream().use { it.readBytes() }
          else contentResolver.openInputStream(uri)?.use { it.readBytes() }
          if (bytes == null) {
            null
          } else {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
            var sample = 1
            while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= maxSide) sample *= 2
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })
          }
        } catch (e: Exception) {
          null
        }
        Holder.main.post {
          if (bmp != null) {
            info?.text = ""
            into.setImageBitmap(bmp)
          } else {
            info?.text = "No se puede abrir la imagen."
            if (local != null) Holder.store.removeSaved(local.key)
          }
        }
      }.start()
    }

    override fun shown() {
      window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
      if (video != null) Holder.main.post(ticker)
    }

    override fun hidden() {
      window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
      Holder.main.removeCallbacks(ticker)
      video?.pause()
    }
  }

  companion object {
    private const val REQ_CAMERA = 1
    private const val REQ_NOTIFY = 2
  }
}
