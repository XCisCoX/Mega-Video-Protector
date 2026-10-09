@file:OptIn(androidx.media3.common.util.UnstableApi::class)

package org.megavideoprotect.app

import android.net.Uri
import androidx.media3.common.C
import androidx.media3.datasource.BaseDataSource
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.DataSpec
import java.io.IOException
import java.io.InputStream
import java.net.URL
import javax.net.ssl.HttpsURLConnection

/**
 * Plays a shared video over the pinned TLS connection. ExoPlayer's own HTTP
 * source trusts the system certificates and rejects the PC's certificate.
 */
class RemoteHttpDataSource private constructor() : BaseDataSource(/* isNetwork= */ true) {

    private var connection: HttpsURLConnection? = null
    private var input: InputStream? = null
    private var currentUri: Uri? = null
    private var bytesRemaining = 0L
    private var opened = false

    override fun open(dataSpec: DataSpec): Long {
        transferInitializing(dataSpec)
        if (!RemoteVault.connected()) throw IOException("The PC connection is closed.")
        val conn = (URL(dataSpec.uri.toString()).openConnection() as HttpsURLConnection).apply {
            RemoteVault.applySecurity(this)
            requestMethod = "GET"
            connectTimeout = 8_000
            readTimeout = 60_000
            instanceFollowRedirects = false
            setRequestProperty("Authorization", "Bearer ${RemoteVault.token.orEmpty()}")
            if (dataSpec.position != 0L || dataSpec.length != C.LENGTH_UNSET.toLong()) {
                val end = if (dataSpec.length == C.LENGTH_UNSET.toLong()) {
                    ""
                } else {
                    (dataSpec.position + dataSpec.length - 1).toString()
                }
                setRequestProperty("Range", "bytes=${dataSpec.position}-$end")
            }
        }
        try {
            conn.connect()
        } catch (error: IOException) {
            conn.disconnect()
            throw error
        }
        val code = conn.responseCode
        if (code != 200 && code != 206) {
            val reason = conn.responseMessage.orEmpty()
            conn.disconnect()
            throw IOException("The PC refused the video ($code $reason).".trim())
        }
        val advertised = conn.getHeaderField("Content-Length")?.toLongOrNull()
        bytesRemaining = when {
            dataSpec.length != C.LENGTH_UNSET.toLong() -> dataSpec.length
            advertised != null && advertised >= 0L -> advertised
            else -> C.LENGTH_UNSET.toLong()
        }
        input = conn.inputStream
        connection = conn
        currentUri = dataSpec.uri
        opened = true
        transferStarted(dataSpec)
        return bytesRemaining
    }

    override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
        if (length == 0) return 0
        if (bytesRemaining == 0L) return C.RESULT_END_OF_INPUT
        val want = if (bytesRemaining == C.LENGTH_UNSET.toLong()) {
            length
        } else {
            minOf(length.toLong(), bytesRemaining).toInt()
        }
        if (want <= 0) return C.RESULT_END_OF_INPUT
        val read = input?.read(buffer, offset, want) ?: -1
        if (read == -1) return C.RESULT_END_OF_INPUT
        if (bytesRemaining != C.LENGTH_UNSET.toLong()) bytesRemaining -= read
        bytesTransferred(read)
        return read
    }

    override fun getUri(): Uri? = currentUri

    override fun close() {
        try {
            input?.close()
        } catch (_: IOException) {
        }
        input = null
        connection?.disconnect()
        connection = null
        if (opened) {
            opened = false
            transferEnded()
        }
    }

    class Factory : DataSource.Factory {
        override fun createDataSource(): DataSource = RemoteHttpDataSource()
    }
}
