package com.spectre.holder

import android.content.Context
import android.content.SharedPreferences
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.security.KeyStore
import java.util.Locale
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

enum class Kind(val label: String) { CLIP("Clip"), RECORDING("Grabación"), SCREENSHOT("Captura"), OTHER("Archivo") }

// Algo que el PC mandó al móvil y está en la nube.
data class Clip(val key: String, val size: Long, val modified: Long) {
  val name: String = key.substringAfterLast('/')
  val image: Boolean
  val kind: Kind
  val game: String // vacío si el nombre no lo dice

  init {
    val ext = name.substringAfterLast('.', "").lowercase(Locale.ROOT)
    image = ext in setOf("png", "jpg", "jpeg", "gif", "webp")
    val stem = name.substringBeforeLast('.')
    // Como en Spectre: <Clip|Grabacion|Captura>_<juego>_<aaaa-mm-dd_hh-mm-ss>...
    val m = NAME.find(stem)
    if (m != null) {
      kind = when (m.groupValues[1]) {
        "Clip" -> Kind.CLIP
        "Grabacion" -> Kind.RECORDING
        else -> Kind.SCREENSHOT
      }
      game = m.groupValues[2].replace('_', ' ').trim()
    } else {
      kind = if (image) Kind.SCREENSHOT else if (ext in setOf("mp4", "mkv", "mov", "webm")) Kind.CLIP else Kind.OTHER
      game = ""
    }
  }

  val mime: String
    get() = when (name.substringAfterLast('.', "").lowercase(Locale.ROOT)) {
      "mp4" -> "video/mp4"
      "mkv" -> "video/x-matroska"
      "mov" -> "video/quicktime"
      "webm" -> "video/webm"
      "m4a" -> "audio/mp4"
      "png" -> "image/png"
      "jpg", "jpeg" -> "image/jpeg"
      "gif" -> "image/gif"
      "webp" -> "image/webp"
      else -> "application/octet-stream"
    }

  val video: Boolean get() = mime.startsWith("video/")

  // "Geometry Dash" o "Clip": lo que se lee primero en la lista.
  val title: String get() = if (game.isNotEmpty() && game != "Escritorio") game else kind.label

  companion object {
    private val NAME = Regex("^(Clip|Grabacion|Captura)_(.*?)_\\d{4}-\\d{2}-\\d{2}_\\d{2}-\\d{2}-\\d{2}")
  }
}

// Algo ya descargado en la galería del móvil.
data class Saved(
  val key: String,
  val name: String,
  val uri: String,
  val mime: String,
  val size: Long,
  val at: Long,
  val title: String,
) {
  fun toJson(): JSONObject = JSONObject().put("key", key).put("name", name).put("uri", uri).put("mime", mime)
    .put("size", size).put("at", at).put("title", title)

  companion object {
    fun fromJson(j: JSONObject) = Saved(
      j.optString("key"), j.optString("name"), j.optString("uri"), j.optString("mime"),
      j.optLong("size"), j.optLong("at"), j.optString("title"),
    )
  }
}

// Ajustes y estado que sobreviven a cerrar la app. Las claves de la nube van
// cifradas con una clave del almacén de claves de Android (no sale del móvil).
class Store(context: Context) {
  private val prefs: SharedPreferences = context.getSharedPreferences("holder", Context.MODE_PRIVATE)

  var deleteAfterDownload: Boolean
    get() = prefs.getBoolean("deleteAfter", true)
    set(v) = prefs.edit().putBoolean("deleteAfter", v).apply()

  // Días que algo espera en la nube antes de borrarse solo; 0 = nunca.
  var keepDays: Int
    get() = prefs.getInt("keepDays", 7)
    set(v) = prefs.edit().putInt("keepDays", v).apply()

  var autoDownload: Boolean
    get() = prefs.getBoolean("autoDownload", false)
    set(v) = prefs.edit().putBoolean("autoDownload", v).apply()

  var thumbsOnMobileData: Boolean
    get() = prefs.getBoolean("thumbsMobile", false)
    set(v) = prefs.edit().putBoolean("thumbsMobile", v).apply()

  var askedNotifications: Boolean
    get() = prefs.getBoolean("askedNotifications", false)
    set(v) = prefs.edit().putBoolean("askedNotifications", v).apply()

  fun cloud(): CloudConfig? {
    val sealed = prefs.getString("cloud", null) ?: return null
    val plain = open(sealed) ?: return null
    return try {
      CloudConfig.fromJson(JSONObject(plain))
    } catch (e: org.json.JSONException) {
      null
    }
  }

  fun setCloud(config: CloudConfig?) {
    if (config == null) {
      prefs.edit().remove("cloud").apply()
      return
    }
    val sealed = seal(config.toJson().toString()) ?: return
    prefs.edit().putString("cloud", sealed).apply()
  }

  // Descargas a medias: clave del objeto -> {uri, bytes}. Siguen donde iban.
  fun partial(key: String): Pair<String, Long>? {
    val j = JSONObject(prefs.getString("partial", "{}") ?: "{}").optJSONObject(key) ?: return null
    return j.optString("uri") to j.optLong("size")
  }

  @Synchronized
  fun setPartial(key: String, uri: String?, size: Long) {
    val all = JSONObject(prefs.getString("partial", "{}") ?: "{}")
    if (uri == null) all.remove(key) else all.put(key, JSONObject().put("uri", uri).put("size", size))
    prefs.edit().putString("partial", all.toString()).apply()
  }

  fun saved(): List<Saved> {
    val arr = JSONArray(prefs.getString("saved", "[]") ?: "[]")
    return (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.let(Saved::fromJson) }
  }

  @Synchronized
  fun addSaved(s: Saved) {
    val list = listOf(s) + saved().filter { it.key != s.key }
    val arr = JSONArray()
    list.take(MAX_SAVED).forEach { arr.put(it.toJson()) }
    prefs.edit().putString("saved", arr.toString()).apply()
  }

  @Synchronized
  fun removeSaved(key: String) {
    val arr = JSONArray()
    saved().filter { it.key != key }.forEach { arr.put(it.toJson()) }
    prefs.edit().putString("saved", arr.toString()).apply()
  }

  @Synchronized
  fun clearSaved() {
    prefs.edit().putString("saved", "[]").apply()
  }

  private fun key(): SecretKey? = try {
    val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
    (ks.getKey(ALIAS, null) as? SecretKey) ?: KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
      .apply {
        init(
          KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .build(),
        )
      }.generateKey()
  } catch (e: Exception) {
    null
  }

  // "k:" + iv + cifrado (AES-GCM con la clave del almacén). Si el almacén no
  // funciona en este móvil, en claro dentro de los datos privados de la app.
  private fun seal(plain: String): String? {
    val k = key() ?: return "p:" + Base64.encodeToString(plain.toByteArray(Charsets.UTF_8), Base64.NO_WRAP)
    return try {
      val cipher = Cipher.getInstance("AES/GCM/NoPadding")
      cipher.init(Cipher.ENCRYPT_MODE, k)
      val data = cipher.doFinal(plain.toByteArray(Charsets.UTF_8))
      "k:" + Base64.encodeToString(cipher.iv, Base64.NO_WRAP) + ":" + Base64.encodeToString(data, Base64.NO_WRAP)
    } catch (e: Exception) {
      "p:" + Base64.encodeToString(plain.toByteArray(Charsets.UTF_8), Base64.NO_WRAP)
    }
  }

  private fun open(sealed: String): String? = try {
    when {
      sealed.startsWith("p:") -> String(Base64.decode(sealed.substring(2), Base64.NO_WRAP), Charsets.UTF_8)
      sealed.startsWith("k:") -> {
        val parts = sealed.substring(2).split(':')
        val k = key()
        if (parts.size != 2 || k == null) {
          null
        } else {
          val cipher = Cipher.getInstance("AES/GCM/NoPadding")
          cipher.init(Cipher.DECRYPT_MODE, k, GCMParameterSpec(128, Base64.decode(parts[0], Base64.NO_WRAP)))
          String(cipher.doFinal(Base64.decode(parts[1], Base64.NO_WRAP)), Charsets.UTF_8)
        }
      }
      else -> null
    }
  } catch (e: Exception) {
    null
  }

  companion object {
    private const val ALIAS = "spectre-holder-cloud"
    private const val MAX_SAVED = 200
  }
}
