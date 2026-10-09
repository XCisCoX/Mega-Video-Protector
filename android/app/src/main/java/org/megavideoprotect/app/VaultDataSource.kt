package org.megavideoprotect.app

import android.net.Uri
import androidx.media3.common.C
import androidx.media3.datasource.BaseDataSource
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.DataSpec
import androidx.media3.datasource.TransferListener
import java.io.IOException

/**
 * Media3 DataSource over the vault. Every byte is served by the core's
 * streaming, per-chunk-authenticated reader (`Vault::read_video_range`) through
 * JNI, so playback never decrypts the video to disk — the desktop player's
 * security model, on the phone.
 *
 * Media items are addressed as `mvpvault://video/<id>`; the plaintext length is
 * the imported file's original size.
 */
class VaultDataSource private constructor() : BaseDataSource(/* isNetwork= */ false) {

    private var videoId = -1L
    private var streamSize = 0L
    private var position = 0L
    private var bytesRemaining = 0L
    private var currentUri: Uri? = null
    private var opened = false

    // Read-ahead window: one JNI call per window instead of one per read.
    private val window = ByteArray(WINDOW_BYTES)
    private var windowLength = 0
    private var windowConsumed = 0

    private fun fillWindow(at: Long) {
        val want = minOf(WINDOW_BYTES.toLong(), streamSize - at).toInt()
        if (want <= 0) {
            windowLength = 0
            windowConsumed = 0
            return
        }
        val data = if (RemoteVault.connected()) {
            RemoteVault.readRange(videoId, at, want)
        } else {
            CoreBridge.nativeReadRange(videoId, at, want)
        }
            ?: throw IOException("vault read failed at offset $at")
        val copy = minOf(data.size, window.size)
        System.arraycopy(data, 0, window, 0, copy)
        windowLength = copy
        windowConsumed = 0
    }

    override fun open(dataSpec: DataSpec): Long {
        transferInitializing(dataSpec)
        val id = dataSpec.uri.lastPathSegment?.toLongOrNull()
            ?: throw IOException("malformed vault URI: ${dataSpec.uri}")
        val size = if (RemoteVault.connected()) RemoteVault.videoSize(id) else CoreBridge.nativeVideoSize(id)
        if (size < 0L) throw IOException("video $id is not in this vault")
        videoId = id
        streamSize = size
        currentUri = dataSpec.uri
        position = dataSpec.position
        bytesRemaining = if (dataSpec.length != C.LENGTH_UNSET.toLong()) {
            dataSpec.length
        } else {
            (size - position).coerceAtLeast(0L)
        }
        windowLength = 0
        windowConsumed = 0
        opened = true
        transferStarted(dataSpec)
        return bytesRemaining
    }

    override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
        if (length == 0) return 0
        if (bytesRemaining == 0L) return C.RESULT_END_OF_INPUT
        if (windowConsumed >= windowLength) {
            fillWindow(position)
            if (windowLength <= 0) return C.RESULT_END_OF_INPUT
        }
        val take = minOf(
            length.toLong(),
            (windowLength - windowConsumed).toLong(),
            bytesRemaining,
        ).toInt()
        if (take <= 0) return C.RESULT_END_OF_INPUT
        System.arraycopy(window, windowConsumed, buffer, offset, take)
        windowConsumed += take
        position += take
        bytesRemaining -= take
        bytesTransferred(take)
        return take
    }

    override fun getUri(): Uri? = currentUri

    override fun close() {
        windowLength = 0
        windowConsumed = 0
        if (opened) {
            opened = false
            transferEnded()
        }
    }

    class Factory : DataSource.Factory {
        override fun createDataSource(): DataSource = VaultDataSource()
    }

    companion object {
        private const val WINDOW_BYTES = 1 shl 20

        fun uriFor(videoId: Long): Uri = Uri.parse("mvpvault://video/$videoId")

        /** Transfer listener is unused: reads come from the local vault. */
        @Suppress("unused")
        fun factory(listener: TransferListener?): DataSource.Factory = Factory()
    }
}
