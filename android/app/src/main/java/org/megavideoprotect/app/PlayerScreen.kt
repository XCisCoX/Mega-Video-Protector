@file:OptIn(androidx.media3.common.util.UnstableApi::class)

package org.megavideoprotect.app

import android.graphics.Bitmap
import android.view.ViewGroup
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
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.media3.common.MediaItem
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.ProgressiveMediaSource
import androidx.media3.ui.PlayerView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.nio.ByteBuffer

/**
 * Player: ExoPlayer (platform hardware decoders) reading the vault through
 * [VaultDataSource], so video *and* audio play straight from the encrypted
 * packages — the plaintext never lands on disk. When the platform decoder
 * cannot handle the codec, the core's FFmpeg decoder supplies a representative
 * frame instead of a black screen. Rotate the phone for landscape; seek/volume
 * live in the player controls.
 */
@Composable
fun PlayerScreen(
    videoId: Long,
    name: String,
    onBack: () -> Unit,
) {
    val context = LocalContext.current
    var player by remember { mutableStateOf<ExoPlayer?>(null) }
    var failure by remember { mutableStateOf<String?>(null) }
    var poster by remember { mutableStateOf<Bitmap?>(null) }

    // Fallback poster: decoded through the core when ExoPlayer gives up.
    LaunchedEffect(videoId, failure) {
        if (failure != null && poster == null) {
            poster = withContext(Dispatchers.Default) {
                CoreBridge.nativeDecodeFrame(videoId, 0, 960)?.let(::rgbaToBitmap)
            }
        }
    }

    DisposableEffect(videoId) {
        val exo = ExoPlayer.Builder(context)
            .setMediaSourceFactory(ProgressiveMediaSource.Factory(VaultDataSource.Factory()))
            .build()
        exo.setMediaItem(MediaItem.fromUri(VaultDataSource.uriFor(videoId)))
        // Loop the single video by default: the vault is a curated library, and
        // restarting the same clip is the common case on a phone.
        exo.repeatMode = Player.REPEAT_MODE_ONE
        exo.addListener(object : Player.Listener {
            override fun onPlayerError(error: PlaybackException) {
                failure = "Playback failed: ${error.errorCodeName}"
            }
        })
        exo.prepare()
        exo.playWhenReady = true
        player = exo
        onDispose {
            player = null
            exo.release()
        }
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
            Spacer(Modifier.size(70.dp))
        }

        Box(
            Modifier
                .weight(1f)
                .fillMaxWidth()
                .background(Mvp.card),
            contentAlignment = Alignment.Center,
        ) {
            val posterFrame = poster
            if (posterFrame != null) {
                Image(
                    bitmap = posterFrame.asImageBitmap(),
                    contentDescription = null,
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Fit,
                )
            } else {
                AndroidView(
                    factory = { ctx ->
                        PlayerView(ctx).apply {
                            useController = true
                            setShowBuffering(PlayerView.SHOW_BUFFERING_WHEN_PLAYING)
                            setShutterBackgroundColor(Mvp.card.toArgb())
                            keepScreenOn = true
                            layoutParams = ViewGroup.LayoutParams(
                                ViewGroup.LayoutParams.MATCH_PARENT,
                                ViewGroup.LayoutParams.MATCH_PARENT,
                            )
                        }
                    },
                    update = { view -> view.player = player },
                    modifier = Modifier.fillMaxSize(),
                )
            }
        }

        failure?.let { message ->
            Text(
                if (poster == null) message else "$message — showing a decoded frame",
                color = Mvp.errorText,
                fontSize = 12.sp,
                modifier = Modifier.fillMaxWidth().padding(8.dp),
            )
        }
    }
}

// rgbaToBitmap lives in ImageViewerScreen.kt: the player, the viewer and the
// thumbnails all rebuild bitmaps from the bridge's raw RGBA the same way.

