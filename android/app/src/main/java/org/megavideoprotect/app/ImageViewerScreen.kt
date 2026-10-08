package org.megavideoprotect.app

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.rememberTransformableState
import androidx.compose.foundation.gestures.transformable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.nio.ByteBuffer

/**
 * Image extensions the bundled core can actually decode (FFmpeg's image2 demuxer
 * plus the compiled decoders). Anything here opens in the image viewer: the media
 * player has no stream to play for a single frame, so an image "playing" was a
 * dead 0-length video.
 */
private val IMAGE_EXTENSIONS =
    setOf("png", "jpg", "jpeg", "webp", "gif", "bmp", "tif", "tiff")

fun isImageName(name: String): Boolean =
    name.substringAfterLast('.', "").lowercase() in IMAGE_EXTENSIONS

/**
 * Full-screen viewer for an image inside the vault. The frame comes from the
 * core's decoder over the encrypted package (nothing is written to disk) at
 * screen resolution, capped so a huge photo cannot exhaust the heap. Pinch to
 * zoom, drag to pan, double-tap to toggle zoom, Back to return to the gallery.
 */
@Composable
fun ImageViewerScreen(
    videoId: Long,
    name: String,
    onBack: () -> Unit,
) {
    val context = LocalContext.current
    var bitmap by remember(videoId) { mutableStateOf<Bitmap?>(null) }
    var failed by remember(videoId) { mutableStateOf(false) }
    var scale by remember(videoId) { mutableStateOf(1f) }
    var offset by remember(videoId) { mutableStateOf(Offset.Zero) }

    LaunchedEffect(videoId) {
        bitmap = null
        failed = false
        val metrics = context.resources.displayMetrics
        val maxDimension = maxOf(metrics.widthPixels, metrics.heightPixels).coerceIn(1024, 2560)
        val decoded = withContext(Dispatchers.Default) {
            runCatching {
                CoreBridge.nativeDecodeFrame(videoId, 0, maxDimension)?.let(::rgbaToBitmap)
            }.getOrNull()
        }
        bitmap = decoded
        failed = decoded == null
    }

    val transformState = rememberTransformableState { zoomChange, panChange, _ ->
        scale = (scale * zoomChange).coerceIn(1f, 6f)
        offset = if (scale <= 1f) Offset.Zero else offset + panChange
    }

    Column(Modifier.fillMaxSize().background(Mvp.window)) {
        Row(
            Modifier
                .fillMaxWidth()
                .background(Mvp.headerBg)
                .padding(horizontal = 12.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            MvpButton("← Back", onClick = onBack)
            Text(
                name,
                color = Mvp.title,
                fontSize = 15.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
                modifier = Modifier.weight(1f),
            )
            if (scale > 1f) {
                MvpButton("Fit", onClick = {
                    scale = 1f
                    offset = Offset.Zero
                })
            } else {
                Text("${(scale * 100).toInt()}%", color = Mvp.description, fontSize = 12.sp)
            }
        }

        Box(
            Modifier
                .weight(1f)
                .fillMaxWidth()
                .background(Mvp.card)
                .pointerInput(videoId) {
                    detectTapGestures(
                        onDoubleTap = {
                            if (scale > 1f) {
                                scale = 1f
                                offset = Offset.Zero
                            } else {
                                scale = 3f
                            }
                        },
                    )
                }
                .transformable(transformState),
            contentAlignment = Alignment.Center,
        ) {
            val image = bitmap
            when {
                image != null -> Image(
                    bitmap = image.asImageBitmap(),
                    contentDescription = name,
                    modifier = Modifier
                        .fillMaxSize()
                        .graphicsLayer(
                            scaleX = scale,
                            scaleY = scale,
                            translationX = offset.x,
                            translationY = offset.y,
                        ),
                    contentScale = ContentScale.Fit,
                )
                failed -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(
                        "Could not decode this image",
                        color = Mvp.errorText,
                        fontSize = 14.sp,
                        textAlign = TextAlign.Center,
                    )
                }
                else -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        CircularProgressIndicator(color = Mvp.accent, strokeWidth = 3.dp)
                        Text(
                            "Decrypting…",
                            color = Mvp.description,
                            fontSize = 12.sp,
                            modifier = Modifier.padding(top = 10.dp),
                        )
                    }
                }
            }
        }

        Text(
            if (failed) {
                "The vault stores the file but the picture could not be decoded."
            } else {
                "Pinch to zoom · drag to pan · double-tap to toggle"
            },
            color = Mvp.statusText,
            fontSize = 11.sp,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
        )
    }
}

/**
 * The bridge returns raw RGBA plus an 8-byte little-endian width/height prefix
 * (jni_bridge.cpp, decode_frame_at). BitmapFactory only understands *encoded*
 * image data, which is why decoding always failed before — rebuild the bitmap
 * from the pixels instead.
 */
internal fun rgbaToBitmap(bytes: ByteArray): Bitmap? {
    if (bytes.size < 8) return null
    fun u32(at: Int): Int =
        (bytes[at].toInt() and 0xFF) or
            ((bytes[at + 1].toInt() and 0xFF) shl 8) or
            ((bytes[at + 2].toInt() and 0xFF) shl 16) or
            ((bytes[at + 3].toInt() and 0xFF) shl 24)

    val width = u32(0)
    val height = u32(4)
    if (width <= 0 || height <= 0) return null
    val pixelBytes = width.toLong() * height.toLong() * 4L
    if (bytes.size - 8 < pixelBytes) return null
    return runCatching {
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(bytes, 8, pixelBytes.toInt()))
        bitmap
    }.getOrNull()
}
