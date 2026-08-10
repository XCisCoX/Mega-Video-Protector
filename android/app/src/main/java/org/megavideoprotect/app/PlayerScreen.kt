package org.megavideoprotect.app

import android.graphics.BitmapFactory
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.shape.RoundedCornerShape
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Player: mirrors the desktop player window. Decodes a frame at the seek
 * position through the JNI bridge (FFmpeg over the encrypted stream — nothing
 * touches disk). Audio playback is not wired up in this first build.
 */
@Composable
fun PlayerScreen(
    videoId: Long,
    name: String,
    durationMs: Long,
    onBack: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    var frame by remember { mutableStateOf<android.graphics.Bitmap?>(null) }
    var positionMs by remember { mutableLongStateOf(0L) }
    var seeking by remember { mutableStateOf(false) }
    var info by remember { mutableStateOf("") }
    var decodeError by remember { mutableStateOf<String?>(null) }

    fun decode(at: Long) {
        seeking = true
        decodeError = null
        scope.launch {
            val (bmp, note) = withContext(Dispatchers.Default) {
                val bytes = CoreBridge.nativeDecodeFrame(videoId, at, 720)
                val meta = runCatching { CoreBridge.nativeMediaInfo(videoId) }.getOrNull() ?: ""
                if (bytes != null) {
                    BitmapFactory.decodeByteArray(bytes, 0, bytes.size) to meta
                } else null to meta
            }
            if (bmp != null) {
                frame = bmp
                info = note
            } else {
                decodeError = "Could not decode a frame at this position."
            }
            seeking = false
        }
    }

    Column(
        Modifier
            .fillMaxSize()
            .background(Mvp.window)
            .padding(12.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            MvpButton("← Back", onClick = onBack)
            Text(name, color = Mvp.title, fontSize = 17.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f).padding(horizontal = 12.dp), textAlign = TextAlign.Center)
            Spacer(Modifier.size(70.dp))
        }

        Box(
            Modifier
                .weight(1f)
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .background(Mvp.card),
            contentAlignment = Alignment.Center,
        ) {
            val f = frame
            if (f != null) {
                Image(
                    bitmap = f.asImageBitmap(),
                    contentDescription = null,
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Fit,
                )
            } else if (seeking) {
                CircularProgressIndicator(color = Mvp.accent)
            } else {
                Text("Decode a frame with the slider", color = Mvp.description, fontSize = 13.sp)
            }
        }

        Row(
            Modifier.fillMaxWidth().padding(top = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            MvpButton("Seek 5s −", onClick = { decode((positionMs - 5000).coerceAtLeast(0)) }, enabled = !seeking)
            Slider(
                value = positionMs.toFloat(),
                onValueChange = { positionMs = it.toLong() },
                onValueChangeFinished = { decode(positionMs) },
                valueRange = 0f..durationMs.coerceAtLeast(1).toFloat(),
                enabled = durationMs > 0 && !seeking,
                modifier = Modifier.weight(1f).padding(horizontal = 12.dp),
                colors = SliderDefaults.colors(
                    thumbColor = Mvp.sliderHandle,
                    activeTrackColor = Mvp.sliderFill,
                    inactiveTrackColor = Mvp.sliderGroove,
                ),
            )
            MvpButton("Seek 5s +", onClick = { decode((positionMs + 5000).coerceAtMost(durationMs)) }, enabled = !seeking)
        }
        Text(
            "%d:%02d / %d:%02d".format(positionMs / 60000, (positionMs / 1000) % 60, durationMs / 60000, (durationMs / 1000) % 60),
            color = Mvp.description,
            fontSize = 12.sp,
        )
        if (decodeError != null) MvpError(decodeError, Modifier.padding(top = 6.dp))
    }
}
