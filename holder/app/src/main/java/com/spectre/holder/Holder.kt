package com.spectre.holder

import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.MediaStore
import android.provider.Settings
import org.json.JSONObject
import java.io.FileOutputStream
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList

// Estado de la app: lo que espera en la nube, las descargas a la galería y los
// ajustes. Un solo hilo de descarga mientras hay algo en la cola; nada se
// queda corriendo sin trabajo.
object Holder {
  lateinit var app: Context
    private set
  lateinit var store: Store
    private set
  val main = Handler(Looper.getMainLooper())

  private val listeners = CopyOnWriteArrayList<() -> Unit>()
  fun listen(l: () -> Unit) = listeners.add(l)
  fun unlisten(l: () -> Unit) = listeners.remove(l)

  // Como mucho un aviso cada 200 ms mientras descarga (el progreso).
  @Volatile private var posted = false
  private var lastPost = 0L
  fun changed(progress: Boolean = false) {
    val now = SystemClock.uptimeMillis()
    if (progress && now - lastPost < 200) return
    lastPost = now
    if (posted) return
    posted = true
    main.post {
      posted = false
      listeners.forEach { it() }
    }
  }

  @Volatile var cloud: CloudConfig? = null
    private set

  // Lista de la nube: solo se cambia en el hilo principal.
  var clips: List<Clip> = emptyList()
    private set
  var listed = false // ya hubo una lista (vacía o no) desde que se emparejó
    private set
  var refreshing = false
    private set
  var refreshedAt = 0L
    private set
  var error: String? = null
    private set

  // Emparejar: el aviso al PC (spectre/moviles/<id>.json) recién leído el código.
  enum class Link { NONE, CONNECTING, CONNECTED, FAILED }
  @Volatile var link = Link.NONE
    private set
  var linkError: String? = null
    private set

  // Descargas (lock para la cola).
  private val lock = Any()
  private val queue = ArrayDeque<Clip>()
  private var worker: Thread? = null
  @Volatile var current: Clip? = null
    private set
  @Volatile var progress = 0f
    private set
  @Volatile var speed = 0.0
    private set
  @Volatile private var cancelKey: String? = null
  val failed = ConcurrentHashMap<String, String>()
  // Descargadas en esta sesión que siguen en la nube (si no se borran al descargar).
  private val doneKeys = ConcurrentHashMap.newKeySet<String>()

  fun init(context: Context) {
    if (::app.isInitialized) return
    app = context.applicationContext
    store = Store(app)
    cloud = store.cloud()
  }

  fun pair(config: CloudConfig) {
    val old = cloud
    store.setCloud(config)
    cloud = config
    clips = emptyList()
    listed = false
    error = null
    failed.clear()
    // Otra nube: el aviso de la de antes se quita (allí ya no se lee nada).
    if (old != null && !old.sameCloud(config)) forget(old)
    announce()
    refresh()
  }

  fun unpair() {
    synchronized(lock) { queue.clear() }
    current?.let { cancelKey = it.key }
    cloud?.let { forget(it) }
    store.setCloud(null)
    store.announced(null, 0)
    cloud = null
    clips = emptyList()
    listed = false
    error = null
    failed.clear()
    link = Link.NONE
    linkError = null
    changed()
  }

  // Ya visto el «Conectado con...» (o el fallo) de después de emparejar.
  fun dismissLink() {
    link = Link.NONE
    linkError = null
    changed()
  }

  // Avisa al PC de que este móvil lee la nube. Lo enseña la pantalla de inicio.
  fun announce() {
    val config = cloud ?: return
    link = Link.CONNECTING
    linkError = null
    changed()
    Thread {
      val err = try {
        sendAnnounce(S3(config), config)
        null
      } catch (e: IOException) {
        e.message ?: "No se pudo escribir en la nube."
      } catch (e: Exception) {
        "La nube respondió algo que Spectre Holder no entiende. Prueba otra vez."
      }
      main.post {
        if (cloud != config) return@post
        link = if (err == null) Link.CONNECTED else Link.FAILED
        linkError = err
        changed()
      }
    }.start()
  }

  private fun announceKey(): String = CloudConfig.PHONES_PREFIX + store.deviceId + ".json"

  // spectre/moviles/<id>.json: quién es este móvil (lo que enseña el PC) y el
  // código que leyó (así sabe que fue este, aunque tenga más emparejados).
  private fun sendAnnounce(s3: S3, config: CloudConfig) {
    val version = try {
      app.packageManager.getPackageInfo(app.packageName, 0).versionName ?: ""
    } catch (e: Exception) {
      ""
    }
    val body = JSONObject()
      .put("v", 1)
      .put("id", store.deviceId)
      .put("name", deviceName())
      .put("app", version)
      .put("c", config.nonce)
      .put("pc", config.pcName)
      .put("t", System.currentTimeMillis())
      .toString()
    s3.put(announceKey(), body.toByteArray(Charsets.UTF_8), "application/json")
    store.announced(config, System.currentTimeMillis())
  }

  // Sin esperar: al cambiar de nube o desemparejar (sin red, se queda allí).
  private fun forget(config: CloudConfig) {
    val key = announceKey()
    Thread {
      try {
        S3(config).delete(key)
      } catch (e: Exception) {
      }
    }.start()
  }

  // El nombre que el usuario le puso al móvil (Ajustes > Información) o la marca y el modelo.
  fun deviceName(): String {
    val named = try {
      Settings.Global.getString(app.contentResolver, Settings.Global.DEVICE_NAME)
    } catch (e: Exception) {
      null
    }?.trim().orEmpty()
    if (named.isNotEmpty()) return named.take(60)
    val maker = Build.MANUFACTURER.orEmpty().trim().replaceFirstChar { it.titlecase() }
    val model = Build.MODEL.orEmpty().trim()
    val full = if (model.startsWith(maker, ignoreCase = true) || maker.isEmpty()) model else "$maker $model"
    return full.ifEmpty { "Android" }.take(60)
  }

  // Vuelve a pedir la lista. auto: al abrir la app (descarga sola si está activado).
  fun refresh(auto: Boolean = false) {
    val config = cloud ?: return
    if (refreshing) return
    refreshing = true
    changed()
    Thread {
      var list: List<Clip>? = null
      var err: String? = null
      try {
        val s3 = S3(config)
        // El PC enseña cuándo se vio el móvil: el aviso se repite si pasó más de una hora.
        if (link == Link.NONE && store.announceDue(config, System.currentTimeMillis())) {
          try {
            sendAnnounce(s3, config)
          } catch (e: Exception) {
          }
        }
        val all = s3.list()
        val now = System.currentTimeMillis()
        val firstSeen = store.markSeen(all.map { it.key }, now)
        // Lo que este móvil vio hace más días de los elegidos se borra de la nube (no del móvil).
        val days = store.keepDays
        val old = if (days > 0) {
          val limit = now - days * 86_400_000L
          all.filter { (firstSeen[it.key] ?: now) < limit && it.key != current?.key && !queued(it.key) }
        } else {
          emptyList()
        }
        val removed = HashSet<String>()
        for (o in old) {
          try {
            s3.delete(o.key)
            removed += o.key
          } catch (e: Exception) {
            break // sin permiso o sin red: se intenta la próxima vez
          }
        }
        list = all.filter { it.key !in removed }.map { Clip(it.key, it.size, it.modified, firstSeen[it.key] ?: now) }
      } catch (e: IOException) {
        err = e.message ?: "No se pudo leer la nube."
      } catch (e: Exception) {
        // Una respuesta rota (XML a medias, un proxy): no cierra la app.
        err = "La nube respondió algo que Spectre Holder no entiende. Prueba otra vez."
      }
      main.post {
        refreshing = false
        if (cloud != config) return@post // se desemparejó mientras tanto
        if (list != null) {
          clips = list.sortedByDescending { it.modified }
          listed = true
          refreshedAt = System.currentTimeMillis()
          error = null
          if (auto && store.autoDownload) download(clips.filter { !isSaved(it.key) })
        } else {
          error = err
        }
        changed()
      }
    }.start()
  }

  fun isSaved(key: String): Boolean = key in doneKeys || store.saved().any { it.key == key }

  fun queued(key: String): Boolean = synchronized(lock) { queue.any { it.key == key } }

  fun pending(): Int = synchronized(lock) { queue.size } + if (current != null) 1 else 0

  fun busy(): Boolean = pending() > 0

  // A la cola (las que ya están o se están descargando no se repiten).
  fun download(list: List<Clip>) {
    if (list.isEmpty() || cloud == null) return
    synchronized(lock) {
      for (c in list) {
        if (c.key == current?.key || queue.any { it.key == c.key }) continue
        failed.remove(c.key)
        queue.addLast(c)
      }
      if (worker == null && queue.isNotEmpty()) {
        worker = Thread({ work() }, "descargas").apply { start() }
      }
    }
    // Mantiene viva la descarga con la pantalla apagada o la app en segundo plano.
    try {
      app.startForegroundService(Intent(app, TransferService::class.java))
    } catch (e: Exception) {
      // Sin permiso para arrancarlo ahora (la app no está delante): sigue igual mientras viva.
    }
    changed()
  }

  fun cancel(key: String) {
    synchronized(lock) { queue.removeAll { it.key == key } }
    if (current?.key == key) cancelKey = key
    changed()
  }

  fun cancelAll() {
    synchronized(lock) { queue.clear() }
    current?.let { cancelKey = it.key }
    changed()
  }

  // Lo quita de la nube sin descargarlo.
  fun deleteRemote(clip: Clip, done: (String?) -> Unit) {
    val config = cloud ?: return
    cancel(clip.key)
    Thread {
      val err = try {
        S3(config).delete(clip.key)
        null
      } catch (e: IOException) {
        e.message ?: "No se pudo borrar."
      } catch (e: Exception) {
        "No se pudo borrar."
      }
      main.post {
        if (err == null) clips = clips.filter { it.key != clip.key }
        done(err)
        changed()
      }
    }.start()
  }

  // URL para verlo sin descargarlo (vale una hora).
  fun streamUrl(clip: Clip): String? = cloud?.let { S3(it).presign(clip.key, 3600) }

  private fun work() {
    android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND)
    while (true) {
      val c = synchronized(lock) {
        val next = queue.removeFirstOrNull()
        if (next == null) worker = null
        current = next
        next
      } ?: break
      val config = cloud ?: continue
      progress = 0f
      speed = 0.0
      cancelKey = null
      changed()
      var err: String? = null
      var ok = false
      try {
        ok = fetch(S3(config), c)
      } catch (e: IOException) {
        err = e.message ?: "La descarga falló."
      } catch (e: SecurityException) {
        err = "Android no deja guardar en la galería."
      } catch (e: IllegalStateException) {
        err = "Android no deja guardar en la galería."
      } catch (e: Exception) {
        // Cualquier otra cosa (MediaStore, una respuesta rota) es un fallo de esta
        // descarga: el hilo sigue con la cola y se quita al acabarla.
        err = "La descarga falló: ${e.javaClass.simpleName}."
      }
      if (ok) {
        doneKeys += c.key
        if (store.deleteAfterDownload) {
          try {
            S3(config).delete(c.key)
            main.post {
              clips = clips.filter { it.key != c.key }
              changed()
            }
          } catch (e: Exception) {
            // Se queda en la nube; se borra sola al pasar los días elegidos.
          }
        }
      } else if (err != null) {
        failed[c.key] = err
      }
      current = null
      changed()
    }
    changed()
  }

  // Descarga a la galería (Movies/Spectre o Pictures/Spectre). Sigue donde se
  // quedó si se cortó. false: cancelada.
  private fun fetch(s3: S3, c: Clip): Boolean {
    val resolver = app.contentResolver
    var uri: Uri? = store.partial(c.key)?.first?.let(Uri::parse)
    var offset = 0L
    if (uri != null) {
      offset = try {
        resolver.openFileDescriptor(uri, "r")?.use { it.statSize } ?: -1L
      } catch (e: Exception) {
        -1L
      }
      if (offset < 0) {
        uri = null
        offset = 0
      }
    }
    if (uri == null) {
      val (collection, folder) = when {
        c.image -> MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY) to Environment.DIRECTORY_PICTURES
        c.video -> MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY) to Environment.DIRECTORY_MOVIES
        c.mime.startsWith("audio/") -> MediaStore.Audio.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY) to Environment.DIRECTORY_MUSIC
        else -> MediaStore.Downloads.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY) to Environment.DIRECTORY_DOWNLOADS
      }
      val values = ContentValues().apply {
        put(MediaStore.MediaColumns.DISPLAY_NAME, c.name)
        put(MediaStore.MediaColumns.MIME_TYPE, c.mime)
        put(MediaStore.MediaColumns.RELATIVE_PATH, "$folder/Spectre")
        put(MediaStore.MediaColumns.IS_PENDING, 1)
        if (c.modified > 0) put(MediaStore.MediaColumns.DATE_TAKEN, c.modified)
      }
      uri = resolver.insert(collection, values) ?: throw IOException("Android no deja crear el archivo en la galería.")
      offset = 0
      store.setPartial(c.key, uri.toString(), 0)
    }
    if (offset > c.size) offset = 0

    if (offset < c.size || c.size == 0L) {
      val conn = s3.get(c.key, offset)
      try {
        if (conn.responseCode == 200) offset = 0 // la nube no hizo caso del rango: desde el principio
        val pfd = resolver.openFileDescriptor(uri, "rw") ?: throw IOException("No se pudo abrir el archivo de la galería.")
        pfd.use {
          FileOutputStream(it.fileDescriptor).use { out ->
            val ch = out.channel
            ch.truncate(offset)
            ch.position(offset)
            val buf = ByteArray(256 * 1024)
            var t0 = SystemClock.elapsedRealtime()
            var bytes0 = offset
            var lastSave = t0
            conn.inputStream.use { input ->
              while (true) {
                if (cancelKey == c.key) {
                  cancelKey = null
                  out.flush()
                  resolver.delete(uri, null, null)
                  store.setPartial(c.key, null, 0)
                  return false
                }
                val n = input.read(buf)
                if (n < 0) break
                out.write(buf, 0, n)
                offset += n
                val now = SystemClock.elapsedRealtime()
                if (now - t0 >= 1000) {
                  speed = (offset - bytes0) * 1000.0 / (now - t0)
                  t0 = now
                  bytes0 = offset
                }
                if (now - lastSave >= 3000) {
                  store.setPartial(c.key, uri.toString(), offset)
                  lastSave = now
                }
                progress = if (c.size > 0) offset.toFloat() / c.size else 0f
                changed(progress = true)
              }
            }
          }
        }
      } catch (e: CloudException) {
        throw e
      } catch (e: IOException) {
        store.setPartial(c.key, uri.toString(), offset)
        throw CloudException("La descarga se cortó al ${(progress * 100).toInt()} %. «Descargar» sigue donde iba.", offline = true)
      } finally {
        conn.disconnect()
      }
      if (c.size > 0 && offset != c.size) {
        store.setPartial(c.key, uri.toString(), offset)
        throw CloudException("La descarga se cortó al ${(progress * 100).toInt()} %. «Descargar» sigue donde iba.", offline = true)
      }
    }

    val done = ContentValues().apply { put(MediaStore.MediaColumns.IS_PENDING, 0) }
    resolver.update(uri, done, null, null)
    store.setPartial(c.key, null, 0)
    store.addSaved(Saved(c.key, c.name, uri.toString(), c.mime, c.size, System.currentTimeMillis(), c.title))
    progress = 1f
    return true
  }
}
