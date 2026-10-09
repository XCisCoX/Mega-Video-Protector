package org.megavideoprotect.app

import android.net.Uri
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL

/**
 * The vault shared from the PC. The QR code is only `mvpvault://host:port`.
 * The phone still has to send the vault password; the session token comes back
 * after that password is accepted.
 */
object RemoteVault {
    @Volatile var baseUrl: String? = null
        private set
    @Volatile var token: String? = null
        private set

    fun connected(): Boolean = !baseUrl.isNullOrBlank() && !token.isNullOrBlank()

    /** Address shown in the gallery footer, without the session token. */
    fun label(): String? {
        val base = baseUrl ?: return null
        return "PC · " + base.removePrefix("http://")
    }

    fun fileUrl(id: Long): String {
        val base = baseUrl ?: return ""
        return "$base/v1/file?id=$id"
    }

    fun disconnect() {
        baseUrl = null
        token = null
    }

    /**
     * Null when the PC accepted the password. Otherwise a sentence for the
     * login screen. The password is not kept after this returns.
     */
    fun login(raw: String, password: String): String? {
        if (password.isEmpty()) return "Enter the PC vault password."
        val text = raw.trim()
        val uri = runCatching { Uri.parse(text) }.getOrNull()
            ?: return "That code is not a PC share."
        if (uri.scheme != "mvpvault") return "That code is not a PC share."
        val host = uri.host ?: return "That code has no address."
        val port = uri.port
        if (port <= 0) return "That code has no port."
        val base = "http://$host:$port"
        val body = password.toByteArray(Charsets.UTF_8)
        val conn = (URL("$base/v1/login").openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"
            doOutput = true
            setRequestProperty("Content-Type", "text/plain; charset=utf-8")
            setFixedLengthStreamingMode(body.size)
            connectTimeout = 8000
            readTimeout = 120000
            instanceFollowRedirects = false
        }
        return try {
            conn.outputStream.use { it.write(body) }
            val code = conn.responseCode
            when (code) {
                401 -> "Wrong password."
                200 -> {
                    val json = conn.inputStream.use { it.readBytes() }.toString(Charsets.UTF_8)
                    val session = JSONObject(json).optString("token")
                    if (session.isBlank()) {
                        "The PC did not accept the password."
                    } else {
                        baseUrl = base
                        token = session
                        null
                    }
                }
                else -> "The PC refused the sign-in. Start sharing on the PC and try again."
            }
        } catch (_: Exception) {
            "Could not reach the PC. Same Wi-Fi, and allow the app through the firewall."
        } finally {
            body.fill(0.toByte())
            conn.disconnect()
        }
    }

    fun getText(path: String): String? = getBytes(path)?.let { String(it, Charsets.UTF_8) }

    fun getBytes(path: String): ByteArray? {
        val base = baseUrl ?: return null
        val secret = token ?: return null
        return get(base, secret, path)
    }

    fun videoSize(id: Long): Long {
        val text = getText("/v1/size?id=$id") ?: return -1L
        return text.trim().toLongOrNull() ?: -1L
    }

    fun readRange(id: Long, offset: Long, size: Int): ByteArray? {
        val ask = size.coerceIn(1, 1 shl 20)
        return getBytes("/v1/read?id=$id&offset=$offset&size=$ask")
    }

    /** Whole plaintext, for a photo. Null when it is missing or larger than `maxBytes`. */
    fun readAll(id: Long, maxBytes: Int): ByteArray? {
        val size = videoSize(id)
        if (size <= 0L || size > maxBytes) return null
        val out = ByteArray(size.toInt())
        var at = 0
        while (at < out.size) {
            val n = minOf(1 shl 20, out.size - at)
            val chunk = readRange(id, at.toLong(), n) ?: return null
            if (chunk.isEmpty()) return null
            chunk.copyInto(out, at, 0, minOf(chunk.size, n))
            at += chunk.size
            if (chunk.size < n) break
        }
        return if (at == out.size) out else null
    }

    private fun get(base: String, secret: String, path: String): ByteArray? {
        val conn = (URL(base + path).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            setRequestProperty("Authorization", "Bearer $secret")
            connectTimeout = 8000
            readTimeout = 60000
            instanceFollowRedirects = false
        }
        return try {
            val code = conn.responseCode
            if (code != 200) return null
            conn.inputStream.use { input ->
                val out = ByteArrayOutputStream()
                input.copyTo(out)
                out.toByteArray()
            }
        } finally {
            conn.disconnect()
        }
    }
}
