package com.spectre.holder

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.MediaMetadataRetriever
import android.net.ConnectivityManager
import android.net.Uri
import android.util.LruCache
import android.util.Size
import java.io.File
import java.io.FileOutputStream
import java.net.URL
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors

// Miniaturas: de la nube, sacando un fotograma sin descargar el vídeo entero
// (los clips de Spectre llevan el índice al principio); de la galería, la del
// sistema. Se guardan en la caché de la app para no pedirlas otra vez.
object Thumbs {
  private const val W = 384
  private const val H = 216
  private val pool = Executors.newFixedThreadPool(2)
  private val memory = object : LruCache<String, Bitmap>(16 * 1024 * 1024) {
    override fun sizeOf(key: String, value: Bitmap) = value.byteCount
  }
  private val asked = ConcurrentHashMap.newKeySet<String>()
  private val durations = ConcurrentHashMap<String, Long>()

  fun duration(key: String): Long = durations[key] ?: 0L

  private fun file(key: String): File {
    val dir = File(Holder.app.cacheDir, "miniaturas").apply { mkdirs() }
    return File(dir, S3.hex(S3.sha256(key.toByteArray(Charsets.UTF_8))).substring(0, 32) + ".jpg")
  }

  fun cached(key: String): Bitmap? = memory.get(key)

  // Con datos móviles no se piden miniaturas de la nube (salvo que se active en Ajustes).
  private fun allowed(): Boolean {
    if (Holder.store.thumbsOnMobileData) return true
    val cm = Holder.app.getSystemService(ConnectivityManager::class.java) ?: return true
    return !cm.isActiveNetworkMetered
  }

  // done llega en el hilo principal (también con null si no se pudo).
  fun remote(clip: Clip, done: (Bitmap?) -> Unit) {
    memory.get(clip.key)?.let { return done(it) }
    if (!asked.add(clip.key)) return
    pool.execute {
      var bmp: Bitmap? = null
      val f = file(clip.key)
      if (f.exists()) bmp = BitmapFactory.decodeFile(f.path)
      if (bmp == null && allowed()) {
        val url = Holder.streamUrl(clip)
        if (url != null) bmp = if (clip.image) image(url, clip.size) else video(clip.key, url)
        if (bmp != null) save(f, bmp)
      }
      if (bmp != null) memory.put(clip.key, bmp) else asked.remove(clip.key)
      Holder.main.post { done(bmp) }
    }
  }

  fun local(saved: Saved, done: (Bitmap?) -> Unit) {
    val id = "local:" + saved.uri
    memory.get(id)?.let { return done(it) }
    if (!asked.add(id)) return
    pool.execute {
      val bmp = try {
        Holder.app.contentResolver.loadThumbnail(Uri.parse(saved.uri), Size(W, H), null)
      } catch (e: Exception) {
        null
      }
      if (bmp != null) memory.put(id, bmp) else asked.remove(id)
      Holder.main.post { done(bmp) }
    }
  }

  private fun video(key: String, url: String): Bitmap? {
    val r = MediaMetadataRetriever()
    return try {
      r.setDataSource(url, HashMap())
      r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull()?.let { durations[key] = it }
      r.getScaledFrameAtTime(500_000L, MediaMetadataRetriever.OPTION_CLOSEST_SYNC, W, H)
    } catch (e: Exception) {
      null
    } finally {
      try {
        r.release()
      } catch (e: Exception) {
      }
    }
  }

  private fun image(url: String, size: Long): Bitmap? {
    if (size > 40L * 1024 * 1024) return null
    return try {
      val bytes = URL(url).openStream().use { it.readBytes() }
      val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
      BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
      var sample = 1
      while (bounds.outWidth / (sample * 2) >= W && bounds.outHeight / (sample * 2) >= H) sample *= 2
      BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })
    } catch (e: Exception) {
      null
    }
  }

  private fun save(f: File, bmp: Bitmap) {
    try {
      FileOutputStream(f).use { bmp.compress(Bitmap.CompressFormat.JPEG, 82, it) }
    } catch (e: Exception) {
    }
  }

  fun forget(key: String) {
    memory.remove(key)
    asked.remove(key)
    file(key).delete()
  }
}
