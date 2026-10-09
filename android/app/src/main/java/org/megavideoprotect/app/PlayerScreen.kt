@file:OptIn(androidx.media3.common.util.UnstableApi::class)

package org.megavideoprotect.app

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.view.ViewGroup
import java.nio.ByteBuffer
import java.nio.ByteOrder
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.horizontalDrag
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.pager.VerticalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.toArgb
import androidx.compose.foundation.layout.offset
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
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
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

/**
 * Full-screen vertical feed. Swipe up for the next video and down for the
 * previous one, in gallery order. The current clip loops until you swipe.
 * ExoPlayer reads the vault through [VaultDataSource], so plaintext never
 * lands on disk. A failed codec falls back to a decoded still.
 */
@Composable
fun PlayerScreen(
    videoId: Long,
    name: String,
    playlist: List<PlayItem>,
    onBack: () -> Unit,
) {
    val context = LocalContext.current
    val items = remember(videoId, name, playlist) {
        playlist.ifEmpty { listOf(PlayItem(videoId, name)) }
    }
    val start = items.indexOfFirst { it.id == videoId }.let { if (it < 0) 0 else it }
    val pagerState = rememberPagerState(initialPage = start) { items.size }
    var player by remember { mutableStateOf<ExoPlayer?>(null) }
    var failure by remember { mutableStateOf<String?>(null) }
    var poster by remember { mutableStateOf<Bitmap?>(null) }
    var playing by remember { mutableStateOf(true) }
    var position by remember { mutableLongStateOf(0L) }
    var duration by remember { mutableLongStateOf(0L) }
    var scrubbing by remember { mutableStateOf(false) }
    var scrubFraction by remember { mutableFloatStateOf(0f) }
    // Stays on the thumbnail until ExoPlayer has drawn the first frame.
    var readyIndex by remember { mutableIntStateOf(-1) }
    // Probed clips: missing key = not checked yet, null = play with ExoPlayer,
    // value = a short clip looped from memory so a repeat does not decrypt again.
    var clipById by remember { mutableStateOf(mapOf<Long, LoopClip?>()) }
    var loopPaused by remember { mutableStateOf(false) }
    var loopPosition by remember { mutableLongStateOf(0L) }
    var loopDuration by remember { mutableLongStateOf(0L) }

    val page = pagerState.settledPage
    val shown = items[page.coerceIn(0, items.lastIndex)]

    LaunchedEffect(shown.id, failure) {
        if (failure != null && poster == null) {
            poster = withContext(Dispatchers.Default) {
                if (RemoteVault.connected()) {
                    RemoteVault.getBytes("/v1/thumbnail?id=${shown.id}&max=720")?.let {
                        BitmapFactory.decodeByteArray(it, 0, it.size)
                    }
                } else {
                    CoreBridge.nativeDecodeFrame(shown.id, 0, 960)?.let(::rgbaToBitmap)
                }
            }
        }
    }

    LaunchedEffect(player) {
        while (true) {
            val exo = player
            position = exo?.currentPosition ?: 0L
            val length = exo?.duration ?: 0L
            duration = if (length > 0L) length else 0L
            delay(200)
        }
    }

    DisposableEffect(items) {
        val exo = ExoPlayer.Builder(context)
            .setMediaSourceFactory(ProgressiveMediaSource.Factory(VaultDataSource.Factory()))
            .build()
        exo.setMediaItems(
            items.map { MediaItem.fromUri(VaultDataSource.uriFor(it.id)) },
            start,
            0L,
        )
        // Loop the clip you're watching. Swiping, not the end of the file,
        // is what moves to the next video.
        exo.repeatMode = Player.REPEAT_MODE_ONE
        exo.addListener(object : Player.Listener {
            override fun onPlayerError(error: PlaybackException) {
                val detail = error.cause?.message ?: error.message
                failure = if (detail.isNullOrBlank()) {
                    "Playback failed: ${error.errorCodeName}"
                } else {
                    "Playback failed: ${error.errorCodeName} ($detail)"
                }
            }

            override fun onIsPlayingChanged(isPlaying: Boolean) {
                playing = isPlaying
            }

            override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
                // A repeat of the clip you are watching is not a new video.
                // Covering it again leaves a 1-second clip stuck on its thumbnail.
                if (reason == Player.MEDIA_ITEM_TRANSITION_REASON_REPEAT) return
                if (exo.currentMediaItemIndex != readyIndex) readyIndex = -1
            }

            override fun onRenderedFirstFrame() {
                readyIndex = exo.currentMediaItemIndex
            }
        })
        exo.prepare()
        // Stay paused until we know this page is not a short clip. A short clip
        // is drawn from memory; starting ExoPlayer here would decrypt it too.
        exo.playWhenReady = false
        player = exo
        onDispose {
            player = null
            exo.release()
            clipById.values.forEach { clip -> clip?.frames?.forEach { it.recycle() } }
        }
    }

    LaunchedEffect(shown.id) {
        if (clipById.containsKey(shown.id)) return@LaunchedEffect
        if (RemoteVault.connected()) {
            // The bytes stay on the PC. ExoPlayer streams them; do not decode a local copy.
            clipById = clipById + (shown.id to null)
            return@LaunchedEffect
        }
        val clip = withContext(Dispatchers.IO) {
            CoreBridge.nativeLoopClip(shown.id, 720)?.let(::parseLoopClip)
        }
        val next = clipById.toMutableMap()
        val resident = next.filterValues { it != null }.keys.toList()
        if (clip != null && resident.size >= 6) {
            val drop = resident.first()
            next.remove(drop)?.frames?.forEach { it.recycle() }
        }
        next[shown.id] = clip
        clipById = next
    }

    LaunchedEffect(page) {
        loopPaused = false
        loopPosition = 0L
        loopDuration = 0L
    }

    val currentClip = clipById[shown.id]
    val clipKnown = clipById.containsKey(shown.id)
    LaunchedEffect(page, player, clipKnown, currentClip) {
        val exo = player ?: return@LaunchedEffect
        // Unknown, or a short clip: leave ExoPlayer paused so it does not
        // reopen the package on a loop.
        if (!clipKnown || currentClip != null) {
            exo.pause()
            return@LaunchedEffect
        }
        failure = null
        poster = null
        if (exo.currentMediaItemIndex != page) exo.seekTo(page, 0L)
        exo.play()
    }

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        VerticalPager(
            state = pagerState,
            beyondViewportPageCount = 1,
            modifier = Modifier.fillMaxSize(),
        ) { index ->
            val item = items[index]
            val onThisPage = index == page
            val itemClip = if (clipById.containsKey(item.id)) clipById[item.id] else null
            val itemKnown = clipById.containsKey(item.id)
            val shortClip = itemKnown && itemClip != null
            val loaded = shortClip || (readyIndex == index && failure == null)
            val active = onThisPage && loaded && !shortClip
            Box(
                Modifier
                    .fillMaxSize()
                    .background(Color.Black)
                    .pointerInput(player, index) {
                        // A tap pauses. The down event is left alone so a
                        // vertical swipe still moves the pager to the next video.
                        awaitEachGesture {
                            val down = awaitFirstDown(requireUnconsumed = false)
                            val start = down.position
                            val up = waitForUpOrCancellation() ?: return@awaitEachGesture
                            val moved = up.position - start
                            if (moved.x * moved.x + moved.y * moved.y > 64f * 64f) return@awaitEachGesture
                            if (index != pagerState.settledPage) return@awaitEachGesture
                            if (clipById[items[index].id] != null) {
                                loopPaused = !loopPaused
                                return@awaitEachGesture
                            }
                            val exo = player ?: return@awaitEachGesture
                            if (exo.isPlaying) exo.pause() else exo.play()
                        }
                    },
            ) {
                if (onThisPage && itemClip != null) {
                    ShortLoop(
                        clip = itemClip,
                        paused = loopPaused,
                        scrubbing = scrubbing,
                        scrubFraction = scrubFraction,
                        onProgress = { pos, dur ->
                            loopPosition = pos
                            loopDuration = dur
                        },
                    )
                }
                if (onThisPage && !shortClip && itemKnown && failure == null) {
                    val current = player
                    AndroidView(
                        factory = { ctx ->
                            FeedPlayerView(ctx).apply {
                                useController = false
                                setShowBuffering(PlayerView.SHOW_BUFFERING_NEVER)
                                // Keep the last frame on screen while the clip
                                // seeks back to the start, so the loop does not flash.
                                setKeepContentOnPlayerReset(true)
                                setShutterBackgroundColor(Color.Black.toArgb())
                                keepScreenOn = true
                                layoutParams = ViewGroup.LayoutParams(
                                    ViewGroup.LayoutParams.MATCH_PARENT,
                                    ViewGroup.LayoutParams.MATCH_PARENT,
                                )
                            }
                        },
                        update = { view -> view.player = current },
                        modifier = Modifier.fillMaxSize(),
                    )
                }
                if (!loaded && !shortClip) {
                    if (onThisPage && poster != null) {
                        Image(
                            bitmap = poster!!.asImageBitmap(),
                            contentDescription = null,
                            modifier = Modifier.fillMaxSize(),
                            contentScale = ContentScale.Fit,
                        )
                    } else {
                        ReelPoster(item.id)
                    }
                }
                if ((shortClip && loopPaused && onThisPage) || (!playing && active)) {
                    Text(
                        "▶",
                        color = Color.White,
                        fontSize = 42.sp,
                        modifier = Modifier.align(Alignment.Center),
                    )
                }
                Text(
                    item.name,
                    color = Color.White,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier
                        .align(Alignment.BottomStart)
                        .navigationBarsPadding()
                        .padding(start = 16.dp, end = 16.dp, bottom = 36.dp),
                )
            }
        }

        MvpButton(
            "←",
            onClick = onBack,
            modifier = Modifier
                .align(Alignment.TopStart)
                .statusBarsPadding()
                .padding(12.dp),
        )

        val barDuration = if (loopDuration > 0L) loopDuration else duration
        val barPosition = if (loopDuration > 0L) loopPosition else position
        val fraction = if (barDuration > 0L) (barPosition.toFloat() / barDuration.toFloat()).coerceIn(0f, 1f) else 0f
        val shown = if (scrubbing) scrubFraction else fraction
        if (scrubbing && barDuration > 0L) {
            Text(
                "${formatClock((shown * barDuration).toLong())} / ${formatClock(barDuration)}",
                color = Color.White,
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .navigationBarsPadding()
                    .padding(bottom = 28.dp),
            )
        }
        Box(
            Modifier
                .align(Alignment.BottomCenter)
                .navigationBarsPadding()
                .fillMaxWidth()
                .height(48.dp)
                .pointerInput(player) {
                    fun seekToX(x: Float) {
                        if (size.width <= 0) return
                        scrubFraction = (x / size.width.toFloat()).coerceIn(0f, 1f)
                        if (loopDuration > 0L) return
                        val exo = player ?: return
                        val length = exo.duration
                        if (length <= 0L) return
                        exo.seekTo(exo.currentMediaItemIndex, (scrubFraction * length).toLong())
                    }
                    awaitEachGesture {
                        val down = awaitFirstDown(requireUnconsumed = false)
                        if (loopDuration <= 0L) {
                            val exo = player ?: return@awaitEachGesture
                            if (exo.duration <= 0L) return@awaitEachGesture
                        }
                        scrubbing = true
                        seekToX(down.position.x)
                        try {
                            horizontalDrag(down.id) { change ->
                                change.consume()
                                seekToX(change.position.x)
                            }
                        } finally {
                            scrubbing = false
                        }
                    }
                },
        ) {
            Box(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .height(if (scrubbing) 5.dp else 3.dp)
                    .background(Color.White.copy(alpha = 0.28f)),
            ) {
                Box(
                    Modifier
                        .fillMaxHeight()
                        .fillMaxWidth(shown)
                        .background(Color.White),
                )
            }
            if (scrubbing) {
                Box(
                    Modifier
                        .align(Alignment.BottomStart)
                        .padding(bottom = 0.dp)
                        .fillMaxWidth(shown),
                ) {
                    Box(
                        Modifier
                            .align(Alignment.CenterEnd)
                            .size(14.dp)
                            .offset(x = 7.dp)
                            .clip(CircleShape)
                            .background(Color.White),
                    )
                }
            }
        }

        failure?.let { message ->
            Text(
                message,
                color = Mvp.errorText,
                fontSize = 12.sp,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .navigationBarsPadding()
                    .padding(bottom = 28.dp, start = 16.dp, end = 16.dp),
            )
        }
    }
}

private class LoopClip(
    val frames: List<Bitmap>,
    val ptsUs: LongArray,
    val durationUs: Long,
    val pcm: ByteArray?,
    val sampleRate: Int,
    val channels: Int,
) {
    fun frameAt(timeUs: Long): Bitmap {
        val span = durationUs.coerceAtLeast(1L)
        val t = ((timeUs % span) + span) % span
        var index = 0
        for (i in ptsUs.indices) {
            if (ptsUs[i] <= t) index = i else break
        }
        return frames[index]
    }
}

private fun parseLoopClip(data: ByteArray): LoopClip? {
    return runCatching {
        val buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        if (buf.remaining() < 8) return null
        if (buf.int != 0x504F4F4C) return null
        val count = buf.int
        if (count <= 0 || count > 48) return null
        val frames = ArrayList<Bitmap>(count)
        val pts = LongArray(count)
        for (i in 0 until count) {
            if (buf.remaining() < 8) return null
            pts[i] = buf.int.toLong() and 0xffffffffL
            val len = buf.int
            if (len <= 0 || len > buf.remaining()) return null
            val jpeg = ByteArray(len)
            buf.get(jpeg)
            frames += BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size) ?: return null
        }
        if (buf.remaining() < 12) return null
        val sampleRate = buf.int
        val channels = buf.int
        val pcmLen = buf.int
        if (pcmLen < 0 || pcmLen > buf.remaining()) return null
        val pcm = if (pcmLen > 0 && sampleRate > 0 && channels > 0) {
            ByteArray(pcmLen).also { buf.get(it) }
        } else {
            null
        }
        val lastGap = if (count >= 2) (pts[count - 1] - pts[count - 2]).coerceAtLeast(1L) else 33_000L
        val durationUs = (pts[count - 1] + lastGap).coerceAtLeast(1L)
        LoopClip(frames, pts, durationUs, pcm, sampleRate, channels)
    }.getOrNull()
}

/** Draws a sub-second clip from frames already in memory, so the repeat never reopens the vault. */
@Composable
private fun ShortLoop(
    clip: LoopClip,
    paused: Boolean,
    scrubbing: Boolean,
    scrubFraction: Float,
    onProgress: (Long, Long) -> Unit,
) {
    var frame by remember(clip) { mutableStateOf(clip.frames.first()) }
    var elapsedUs by remember(clip) { mutableLongStateOf(0L) }
    val track = remember(clip) { mutableStateOf<AudioTrack?>(null) }
    DisposableEffect(clip) {
        val pcm = clip.pcm
        val audio = if (pcm != null && clip.sampleRate > 0 && (clip.channels == 1 || clip.channels == 2)) {
            runCatching {
                val mask = if (clip.channels == 1) {
                    AudioFormat.CHANNEL_OUT_MONO
                } else {
                    AudioFormat.CHANNEL_OUT_STEREO
                }
                AudioTrack.Builder()
                    .setAudioAttributes(
                        AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                            .build(),
                    )
                    .setAudioFormat(
                        AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setSampleRate(clip.sampleRate)
                            .setChannelMask(mask)
                            .build(),
                    )
                    .setTransferMode(AudioTrack.MODE_STATIC)
                    .setBufferSizeInBytes(pcm.size)
                    .build()
                    .also { audioTrack ->
                        audioTrack.write(pcm, 0, pcm.size)
                        val frameBytes = clip.channels * 2
                        val frames = pcm.size / frameBytes
                        if (frames > 0) audioTrack.setLoopPoints(0, frames, -1)
                    }
            }.getOrNull()
        } else {
            null
        }
        track.value = audio
        onDispose {
            audio?.pause()
            audio?.flush()
            audio?.release()
            track.value = null
        }
    }
    LaunchedEffect(clip, paused, scrubbing, if (scrubbing) scrubFraction else -1f) {
        val durMs = (clip.durationUs / 1000L).coerceAtLeast(1L)
        val audio = track.value
        if (scrubbing) {
            audio?.pause()
            val us = (scrubFraction.coerceIn(0f, 1f) * clip.durationUs).toLong()
            elapsedUs = us
            frame = clip.frameAt(us)
            if (audio != null && clip.sampleRate > 0) {
                val sample = (us * clip.sampleRate / 1_000_000L).toInt().coerceAtLeast(0)
                runCatching { audio.setPlaybackHeadPosition(sample) }
            }
            onProgress(us / 1000L, durMs)
            return@LaunchedEffect
        }
        if (paused) {
            audio?.pause()
            return@LaunchedEffect
        }
        audio?.play()
        val baseUs = elapsedUs
        val baseNs = System.nanoTime()
        while (true) {
            withFrameNanos { now ->
                val us = baseUs + (now - baseNs) / 1000L
                elapsedUs = us
                frame = clip.frameAt(us)
                onProgress((us % clip.durationUs) / 1000L, durMs)
            }
        }
    }
    Image(
        bitmap = frame.asImageBitmap(),
        contentDescription = null,
        modifier = Modifier.fillMaxSize(),
        contentScale = ContentScale.Fit,
    )
}

private fun formatClock(ms: Long): String {
    val total = (ms / 1000L).coerceAtLeast(0L)
    return "%d:%02d".format(total / 60L, total % 60L)
}

/** Player surface that does not eat swipes, so the feed can move to the next video. */
private class FeedPlayerView(context: android.content.Context) : PlayerView(context) {
    override fun onTouchEvent(event: android.view.MotionEvent): Boolean = false
    override fun onInterceptTouchEvent(event: android.view.MotionEvent): Boolean = false
}

@Composable
private fun ReelPoster(id: Long) {
    var bmp by remember(id) { mutableStateOf<Bitmap?>(null) }
    LaunchedEffect(id) {
        bmp = withContext(Dispatchers.Default) {
            val jpeg = if (RemoteVault.connected()) {
                RemoteVault.getBytes("/v1/thumbnail?id=$id&max=720")
            } else {
                CoreBridge.nativeThumbnail(id, 720)
            }
            jpeg?.let {
                BitmapFactory.decodeByteArray(it, 0, it.size)
            }
        }
    }
    val image = bmp
    if (image == null) {
        Box(Modifier.fillMaxSize().background(Color.Black))
    } else {
        Image(
            bitmap = image.asImageBitmap(),
            contentDescription = null,
            modifier = Modifier.fillMaxSize(),
            contentScale = ContentScale.Crop,
        )
    }
}
