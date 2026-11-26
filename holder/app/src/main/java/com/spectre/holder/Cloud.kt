package com.spectre.holder

import android.util.Xml
import org.json.JSONObject
import org.xmlpull.v1.XmlPullParser
import java.io.IOException
import java.io.InputStream
import java.io.StringReader
import java.net.HttpURLConnection
import java.net.SocketTimeoutException
import java.net.URL
import java.net.UnknownHostException
import java.security.MessageDigest
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

// La nube de Spectre (Ajustes > Nube en el PC): un bucket compatible con S3
// (Cloudflare R2, Backblaze B2...). El PC sube lo que se manda al móvil a
// spectre/movil/<id>-<código>/<nombre del archivo> y esta app lo lista, lo
// descarga y lo borra. Las peticiones van firmadas (AWS Signature V4) con las
// claves que trae el código del PC: sin servidores intermedios.
//
// Al emparejarse, el móvil deja spectre/moviles/<id>.json (su nombre y el
// código "c" del QR que leyó): así el PC sabe que está conectado, cierra el
// QR y lo enseña en Ajustes > Móvil. Se vuelve a dejar de vez en cuando.

data class CloudConfig(
  val host: String,
  val port: Int,
  val region: String,
  val bucket: String,
  val keyId: String,
  val secret: String,
  val prefix: String,
  val pcName: String,
  val nonce: String = "", // el del QR que se leyó: el PC lo busca para saber que fue este móvil
) {
  fun toJson(): JSONObject = JSONObject()
    .put("h", host).put("p", port).put("r", region).put("b", bucket)
    .put("k", keyId).put("s", secret).put("x", prefix).put("n", pcName).put("c", nonce)

  // El mismo bucket (aunque con otras claves): el aviso de este móvil es el mismo archivo.
  fun sameBucket(o: CloudConfig?): Boolean = o != null && o.host == host && o.port == port && o.bucket == bucket

  companion object {
    const val CODE_PREFIX = "spectre-holder:"
    const val PHONES_PREFIX = "spectre/moviles/" // un <id>.json por móvil emparejado

    fun fromJson(j: JSONObject): CloudConfig? {
      val c = CloudConfig(
        host = j.optString("h").trim().lowercase(),
        port = j.optInt("p", 443),
        region = j.optString("r").trim().ifEmpty { "auto" },
        bucket = j.optString("b").trim(),
        keyId = j.optString("k").trim(),
        secret = j.optString("s").trim(),
        prefix = j.optString("x").ifEmpty { "spectre/movil/" },
        pcName = j.optString("n").trim(),
        nonce = j.optString("c").trim().take(64),
      )
      val hostOk = c.host.isNotEmpty() && c.host.none { it == '/' || it == '@' || it == '?' || it == '#' || it == ' ' }
      if (!hostOk || c.port !in 1..65535 || c.bucket.isEmpty() || c.keyId.isEmpty() || c.secret.isEmpty()) return null
      // Solo lo de Spectre para el móvil: la app nunca toca el resto del bucket
      // (ni lo subido con enlace ni los avisos de spectre/moviles/).
      if (!c.prefix.startsWith("spectre/movil/") || !c.prefix.endsWith("/")) return null
      return c
    }

    // "spectre-holder:<base64url del JSON>", lo que enseña el QR de Ajustes > Móvil.
    fun fromCode(text: String): CloudConfig? {
      val at = text.indexOf(CODE_PREFIX)
      if (at < 0) return null
      val data = text.substring(at + CODE_PREFIX.length).trim().takeWhile { !it.isWhitespace() }
      return try {
        val bytes = Base64.getUrlDecoder().decode(data.trimEnd('='))
        fromJson(JSONObject(String(bytes, Charsets.UTF_8)))
      } catch (e: IllegalArgumentException) {
        null
      } catch (e: org.json.JSONException) {
        null
      }
    }
  }
}

data class RemoteObject(val key: String, val size: Long, val modified: Long)

// offline: sin conexión (se puede volver a probar); status: el código HTTP si respondió.
class CloudException(message: String, val status: Int = 0, val offline: Boolean = false) : IOException(message)

class S3(private val c: CloudConfig) {
  private val hostHeader = if (c.port == 443) c.host else "${c.host}:${c.port}"

  private fun objectPath(key: String): String =
    "/" + uriEncode(c.bucket, false) + if (key.isEmpty()) "" else "/" + uriEncode(key, true)

  private fun signingKey(day: String): ByteArray {
    var k = hmac(("AWS4" + c.secret).toByteArray(Charsets.UTF_8), day)
    k = hmac(k, c.region)
    k = hmac(k, "s3")
    return hmac(k, "aws4_request")
  }

  // Petición firmada, todavía sin conectar. payloadHash: el SHA-256 del cuerpo
  // (sin cuerpo, el de nada: GET, HEAD y DELETE).
  fun request(
    method: String,
    key: String,
    query: List<Pair<String, String>> = emptyList(),
    headers: Map<String, String> = emptyMap(),
    timeoutMs: Int = 20000,
    payloadHash: String = EMPTY_SHA256,
  ): HttpURLConnection {
    val now = Instant.now()
    val amzDate = AMZ_DATE.format(now)
    val day = amzDate.substring(0, 8)
    val path = objectPath(key)
    val q = canonicalQuery(query)
    val scope = "$day/${c.region}/s3/aws4_request"
    val signedNames = "host;x-amz-content-sha256;x-amz-date"
    val canonical = "$method\n$path\n$q\nhost:$hostHeader\nx-amz-content-sha256:$payloadHash\n" +
      "x-amz-date:$amzDate\n\n$signedNames\n$payloadHash"
    val toSign = "AWS4-HMAC-SHA256\n$amzDate\n$scope\n" + hex(sha256(canonical.toByteArray(Charsets.UTF_8)))
    val signature = hex(hmac(signingKey(day), toSign))
    val url = URL("https://$hostHeader$path" + if (q.isEmpty()) "" else "?$q")
    val conn = url.openConnection() as HttpURLConnection
    conn.requestMethod = method
    conn.connectTimeout = timeoutMs
    conn.readTimeout = timeoutMs
    conn.instanceFollowRedirects = false
    conn.useCaches = false
    // Sin gzip: los tamaños y los rangos tienen que ser los del archivo.
    conn.setRequestProperty("Accept-Encoding", "identity")
    conn.setRequestProperty("x-amz-date", amzDate)
    conn.setRequestProperty("x-amz-content-sha256", payloadHash)
    conn.setRequestProperty(
      "Authorization",
      "AWS4-HMAC-SHA256 Credential=${c.keyId}/$scope, SignedHeaders=$signedNames, Signature=$signature",
    )
    for ((k, v) in headers) conn.setRequestProperty(k, v)
    return conn
  }

  // URL firmada para ver un vídeo o sacar su miniatura sin descargarlo.
  fun presign(key: String, seconds: Int): String {
    val amzDate = AMZ_DATE.format(Instant.now())
    val day = amzDate.substring(0, 8)
    val scope = "$day/${c.region}/s3/aws4_request"
    val path = objectPath(key)
    val q = canonicalQuery(
      listOf(
        "X-Amz-Algorithm" to "AWS4-HMAC-SHA256",
        "X-Amz-Credential" to "${c.keyId}/$scope",
        "X-Amz-Date" to amzDate,
        "X-Amz-Expires" to seconds.coerceIn(60, 604800).toString(),
        "X-Amz-SignedHeaders" to "host",
      ),
    )
    val canonical = "GET\n$path\n$q\nhost:$hostHeader\n\nhost\nUNSIGNED-PAYLOAD"
    val toSign = "AWS4-HMAC-SHA256\n$amzDate\n$scope\n" + hex(sha256(canonical.toByteArray(Charsets.UTF_8)))
    val signature = hex(hmac(signingKey(day), toSign))
    return "https://$hostHeader$path?$q&X-Amz-Signature=$signature"
  }

  // Todo lo que espera al móvil (spectre/movil/...), de una vez.
  fun list(): List<RemoteObject> {
    val out = ArrayList<RemoteObject>()
    var token: String? = null
    var pages = 0
    do {
      val q = mutableListOf("list-type" to "2", "prefix" to c.prefix)
      token?.let { q += "continuation-token" to it }
      val body = call(request("GET", "", q))
      token = parseList(body, out)
    } while (token != null && ++pages < 50)
    return out
  }

  fun delete(key: String) {
    call(request("DELETE", key))
  }

  // Un archivo pequeño de una vez (el aviso de spectre/moviles/).
  fun put(key: String, body: ByteArray, contentType: String) {
    val conn = request("PUT", key, headers = mapOf("Content-Type" to contentType), payloadHash = hex(sha256(body)))
    conn.doOutput = true
    conn.setFixedLengthStreamingMode(body.size)
    try {
      conn.outputStream.use { it.write(body) }
    } catch (e: IOException) {
      conn.disconnect()
      throw offline(e)
    }
    call(conn)
  }

  // Abre la descarga desde `offset`. Devuelve la conexión ya respondida (200 o 206).
  fun get(key: String, offset: Long): HttpURLConnection {
    val headers = if (offset > 0) mapOf("Range" to "bytes=$offset-") else emptyMap()
    val conn = request("GET", key, headers = headers, timeoutMs = 30000)
    val code = try {
      conn.responseCode
    } catch (e: IOException) {
      conn.disconnect()
      throw offline(e)
    }
    if (code == 200 || code == 206) return conn
    val body = try {
      conn.errorStream?.use { readText(it) } ?: ""
    } catch (e: IOException) {
      ""
    }
    conn.disconnect()
    throw failure(code, body)
  }

  private fun call(conn: HttpURLConnection): String {
    try {
      val code = conn.responseCode
      if (code in 200..299) return conn.inputStream.use { readText(it) }
      val body = conn.errorStream?.use { readText(it) } ?: ""
      throw failure(code, body)
    } catch (e: CloudException) {
      throw e
    } catch (e: IOException) {
      throw offline(e)
    } finally {
      conn.disconnect()
    }
  }

  private fun offline(e: IOException): CloudException = when (e) {
    is UnknownHostException -> CloudException("Sin conexión a internet (no se encuentra ${c.host}).", offline = true)
    is SocketTimeoutException -> CloudException("La nube no responde. Se puede volver a probar.", offline = true)
    is javax.net.ssl.SSLException -> CloudException("No se pudo abrir una conexión segura con la nube.", offline = true)
    else -> CloudException("Sin conexión con la nube: ${e.message ?: e.javaClass.simpleName}", offline = true)
  }

  private fun failure(code: Int, body: String): CloudException {
    val s3 = xmlTag(body, "Code")
    val text = when {
      s3 == "RequestTimeTooSkewed" -> "La hora del móvil no está bien: ponla en automática y vuelve a probar."
      s3 == "InvalidAccessKeyId" || s3 == "SignatureDoesNotMatch" ->
        "La nube no acepta las claves. Empareja otra vez con el código de Spectre (Ajustes > Móvil)."
      s3 == "NoSuchBucket" -> "El bucket ya no existe. Empareja otra vez con el código de Spectre."
      s3 == "NoSuchKey" || code == 404 -> "Ya no está en la nube."
      code == 401 || code == 403 -> "Las claves no dan permiso para esto (código $code)."
      code >= 500 -> "La nube falló (código $code). Se puede volver a probar."
      else -> "La nube respondió con un error (código $code" + (if (s3.isEmpty()) ")." else ", $s3).")
    }
    return CloudException(text, status = code)
  }

  companion object {
    const val EMPTY_SHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    private val AMZ_DATE: DateTimeFormatter =
      DateTimeFormatter.ofPattern("yyyyMMdd'T'HHmmss'Z'").withZone(ZoneOffset.UTC)

    fun sha256(data: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(data)

    fun hmac(key: ByteArray, text: String): ByteArray {
      val mac = Mac.getInstance("HmacSHA256")
      mac.init(SecretKeySpec(key, "HmacSHA256"))
      return mac.doFinal(text.toByteArray(Charsets.UTF_8))
    }

    fun hex(data: ByteArray): String {
      val digits = "0123456789abcdef"
      val sb = StringBuilder(data.size * 2)
      for (b in data) {
        val v = b.toInt() and 0xff
        sb.append(digits[v shr 4]).append(digits[v and 15])
      }
      return sb.toString()
    }

    // RFC 3986: letras, números y - _ . ~ tal cual; lo demás %XX (en UTF-8).
    fun uriEncode(s: String, keepSlash: Boolean): String {
      val digits = "0123456789ABCDEF"
      val sb = StringBuilder()
      for (b in s.toByteArray(Charsets.UTF_8)) {
        val c = b.toInt() and 0xff
        val ch = c.toChar()
        if ((ch in 'A'..'Z') || (ch in 'a'..'z') || (ch in '0'..'9') || ch == '-' || ch == '_' || ch == '.' ||
          ch == '~' || (keepSlash && ch == '/')
        ) {
          sb.append(ch)
        } else {
          sb.append('%').append(digits[c shr 4]).append(digits[c and 15])
        }
      }
      return sb.toString()
    }

    fun canonicalQuery(q: List<Pair<String, String>>): String =
      q.map { uriEncode(it.first, false) to uriEncode(it.second, false) }
        .sortedWith(compareBy({ it.first }, { it.second }))
        .joinToString("&") { "${it.first}=${it.second}" }

    private fun readText(input: InputStream): String = String(input.readBytes(), Charsets.UTF_8)

    fun xmlTag(xml: String, tag: String): String {
      val open = "<$tag>"
      val a = xml.indexOf(open)
      if (a < 0) return ""
      val b = xml.indexOf("</$tag>", a)
      return if (b < 0) "" else xml.substring(a + open.length, b)
    }

    // ListObjectsV2: añade los objetos y devuelve el token de la página siguiente.
    fun parseList(xml: String, out: MutableList<RemoteObject>): String? {
      val p = Xml.newPullParser()
      p.setFeature(XmlPullParser.FEATURE_PROCESS_NAMESPACES, false)
      p.setInput(StringReader(xml))
      var truncated = false
      var next: String? = null
      var key: String? = null
      var size = 0L
      var modified = 0L
      var inContents = false
      var event = p.eventType
      while (event != XmlPullParser.END_DOCUMENT) {
        if (event == XmlPullParser.START_TAG) {
          when (p.name) {
            "Contents" -> {
              inContents = true
              key = null
              size = 0
              modified = 0
            }
            "Key" -> if (inContents) key = p.nextText()
            "Size" -> if (inContents) size = p.nextText().trim().toLongOrNull() ?: 0
            "LastModified" -> if (inContents) modified = parseTime(p.nextText().trim())
            "IsTruncated" -> truncated = p.nextText().trim() == "true"
            "NextContinuationToken" -> next = p.nextText().trim()
          }
        } else if (event == XmlPullParser.END_TAG && p.name == "Contents") {
          inContents = false
          val k = key
          // Las "carpetas" vacías (claves que acaban en /) no son archivos.
          if (k != null && !k.endsWith("/")) out += RemoteObject(k, size, modified)
        }
        event = p.next()
      }
      return if (truncated && !next.isNullOrEmpty()) next else null
    }

    private fun parseTime(text: String): Long = try {
      Instant.parse(text).toEpochMilli()
    } catch (e: Exception) {
      0L
    }
  }
}
