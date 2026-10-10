package org.megavideoprotect.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.BitmapFactory
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentWidth
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.abs

/**
 * Now-playing page. The player itself lives in [MusicPlaybackService], so the
 * song and its notification keep going after this page closes.
 */
@Composable
fun MusicScreen(
    videoId: Long,
    name: String,
    playlist: List<PlayItem>,
    startFresh: Boolean,
    onBack: () -> Unit,
) {
    val context = LocalContext.current
    val tracks = remember(videoId, name, playlist) {
        playlist.ifEmpty { listOf(PlayItem(videoId, name)) }
    }
    val start = tracks.indexOfFirst { it.id == videoId }.let { if (it < 0) 0 else it }
    var player by remember { mutableStateOf<ExoPlayer?>(null) }
    var index by remember { mutableIntStateOf(start) }
    var playing by remember { mutableStateOf(true) }
    var position by remember { mutableLongStateOf(0L) }
    var duration by remember { mutableLongStateOf(0L) }
    var scrubbing by remember { mutableStateOf(false) }
    var scrub by remember { mutableFloatStateOf(0f) }
    var shuffle by remember { mutableStateOf(false) }
    var repeat by remember { mutableIntStateOf(0) }
    var speed by remember { mutableFloatStateOf(1f) }
    var showRepeat by remember { mutableStateOf(false) }
    var showSpeed by remember { mutableStateOf(false) }
    var showCover by remember { mutableStateOf(false) }
    var failure by remember { mutableStateOf<String?>(null) }
    var cover by remember { mutableStateOf<android.graphics.Bitmap?>(null) }

    val shown = tracks[index.coerceIn(0, tracks.lastIndex)]
    val askNotification = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { }

    LaunchedEffect(Unit) {
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS)
            != PackageManager.PERMISSION_GRANTED
        ) {
            askNotification.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    DisposableEffect(tracks, startFresh) {
        var released = false
        val exo = MusicPlayer.player(context)
        val listener = object : Player.Listener {
            override fun onIsPlayingChanged(isPlaying: Boolean) {
                if (!released) playing = isPlaying
            }

            override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
                if (released) return
                index = exo.currentMediaItemIndex.coerceIn(0, tracks.lastIndex)
            }

            override fun onShuffleModeEnabledChanged(shuffleModeEnabled: Boolean) {
                if (!released) shuffle = shuffleModeEnabled
            }

            override fun onRepeatModeChanged(repeatMode: Int) {
                if (released) return
                repeat = when (repeatMode) {
                    Player.REPEAT_MODE_ALL -> 1
                    Player.REPEAT_MODE_ONE -> 2
                    else -> 0
                }
            }

            override fun onPlayerError(error: androidx.media3.common.PlaybackException) {
                if (released) return
                val detail = error.cause?.message ?: error.message
                failure = detail?.ifBlank { null } ?: "Playback failed"
            }
        }
        exo.addListener(listener)
        index = exo.currentMediaItemIndex.coerceIn(0, tracks.lastIndex)
        playing = exo.isPlaying
        shuffle = exo.shuffleModeEnabled
        repeat = when (exo.repeatMode) {
            Player.REPEAT_MODE_ALL -> 1
            Player.REPEAT_MODE_ONE -> 2
            else -> 0
        }
        val items = tracks.map { track ->
            MediaItem.Builder()
                .setMediaId(track.id.toString())
                .setUri(VaultDataSource.uriFor(track.id))
                .setMediaMetadata(
                    MediaMetadata.Builder()
                        .setTitle(trackTitle(track.name))
                        .build(),
                )
                .build()
        }
        MusicHub.tracks = tracks
        MusicHub.anchorId = tracks[start].id
        val same = exo.mediaItemCount == items.size &&
            items.indices.all { exo.getMediaItemAt(it).mediaId == items[it].mediaId }
        if (!same) {
            exo.setMediaItems(items, start, 0L)
            exo.prepare()
            exo.play()
            index = start
        } else if (startFresh && exo.currentMediaItem?.mediaId != items[start].mediaId) {
            exo.seekTo(start, 0L)
            exo.play()
            index = start
        } else if (startFresh && !exo.isPlaying) {
            exo.play()
        }
        ContextCompat.startForegroundService(
            context,
            Intent(context, MusicPlaybackService::class.java).setAction(MusicPlaybackService.ACTION_ATTACH),
        )
        player = exo
        onDispose {
            released = true
            exo.removeListener(listener)
            player = null
        }
    }

    LaunchedEffect(player) {
        while (true) {
            val exo = player
            if (exo != null && !scrubbing) {
                position = exo.currentPosition.coerceAtLeast(0L)
                val length = exo.duration
                duration = if (length > 0L) length else 0L
                speed = exo.playbackParameters.speed
            }
            delay(200)
        }
    }

    LaunchedEffect(shown.id) {
        cover = withContext(Dispatchers.Default) {
            val bytes = if (RemoteVault.connected()) {
                RemoteVault.getBytes("/v1/thumbnail?id=${shown.id}&max=720")
            } else {
                CoreBridge.nativeThumbnail(shown.id, 720)
            }
            bytes?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
        }
    }

    val fraction = when {
        scrubbing -> scrub
        duration > 0L -> (position.toFloat() / duration.toFloat()).coerceIn(0f, 1f)
        else -> 0f
    }
    val icon = if (repeat == 2) Color(0xFF3390EC) else if (repeat == 1 || shuffle) Mvp.accent else Color.White

    Column(
        Modifier
            .fillMaxSize()
            .background(Color(0xFF1C1C1D))
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                .padding(start = 20.dp, end = 20.dp, top = 20.dp),
            verticalAlignment = Alignment.Top,
        ) {
            Column(Modifier.weight(1f).padding(end = 12.dp, top = 2.dp)) {
                Text(
                    trackTitle(shown.name),
                    color = Color.White,
                    fontSize = 17.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    shown.name.substringAfterLast('.', "Audio").uppercase(),
                    color = Color(0xFF8B9BA8),
                    fontSize = 13.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(top = 6.dp),
                )
            }
            Box(
                Modifier
                    .size(44.dp)
                    .clip(RoundedCornerShape(4.dp))
                    .background(Color(0xFF2C2C2E))
                    .clickable { showCover = true },
                contentAlignment = Alignment.Center,
            ) {
                val art = cover
                if (art == null) {
                    Text("♪", color = Mvp.accent, fontSize = 18.sp)
                } else {
                    Image(
                        bitmap = art.asImageBitmap(),
                        contentDescription = shown.name,
                        modifier = Modifier.fillMaxSize(),
                        contentScale = ContentScale.Crop,
                    )
                }
            }
        }
        failure?.let {
            Text(it, color = Mvp.errorText, fontSize = 13.sp, modifier = Modifier.padding(horizontal = 20.dp, vertical = 4.dp))
        }
        TgSeek(
            fraction = fraction,
            onScrub = {
                scrubbing = true
                scrub = it
            },
            onCommit = { value ->
                val exo = player
                val length = exo?.duration ?: 0L
                if (exo != null && length > 0L) exo.seekTo((value * length).toLong())
                scrubbing = false
            },
        )
        Row(
            Modifier
                .fillMaxWidth()
                .padding(start = 20.dp, end = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                formatClock(if (scrubbing) (scrub * duration).toLong() else position),
                color = Color(0xFF8B9BA8),
                fontSize = 12.sp,
            )
            Spacer(Modifier.weight(1f))
            Box {
                Text(
                    speedLabel(speed),
                    color = Color(0xFF8B9BA8),
                    fontSize = 13.sp,
                    modifier = Modifier
                        .tgPress(
                            onTap = {
                                val next = nextSpeed(speed)
                                player?.setPlaybackSpeed(next)
                                speed = next
                            },
                            onHold = { showSpeed = true },
                            onReleaseHold = {},
                        )
                        .padding(horizontal = 8.dp, vertical = 6.dp),
                )
                DropdownMenu(expanded = showSpeed, onDismissRequest = { showSpeed = false }) {
                    listOf(
                        0.5f to "Slow",
                        1f to "Normal",
                        1.2f to "Medium",
                        1.5f to "Fast",
                        1.7f to "Very fast",
                        2f to "Super fast",
                    ).forEach { (value, label) ->
                        DropdownMenuItem(
                            text = {
                                Text(label, color = if (abs(speed - value) < 0.05f) Mvp.accent else Color.White)
                            },
                            onClick = {
                                player?.setPlaybackSpeed(value)
                                speed = value
                                showSpeed = false
                            },
                        )
                    }
                }
            }
            Text(
                formatClock(duration),
                color = Color(0xFF8B9BA8),
                fontSize = 12.sp,
                modifier = Modifier.padding(end = 8.dp),
            )
        }
        Row(
            Modifier
                .fillMaxWidth()
                .height(66.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                Icon(
                    painterResource(R.drawable.ic_tg_repeat),
                    contentDescription = "Repeat",
                    tint = icon,
                    modifier = Modifier
                        .size(48.dp)
                        .clickable { showRepeat = true }
                        .padding(12.dp),
                )
                DropdownMenu(expanded = showRepeat, onDismissRequest = { showRepeat = false }) {
                    DropdownMenuItem(
                        text = { Text("Repeat song", color = if (repeat == 2) Mvp.accent else Color.White) },
                        onClick = {
                            player?.repeatMode = if (repeat == 2) Player.REPEAT_MODE_OFF else Player.REPEAT_MODE_ONE
                            showRepeat = false
                        },
                    )
                    DropdownMenuItem(
                        text = { Text("Repeat list", color = if (repeat == 1) Mvp.accent else Color.White) },
                        onClick = {
                            player?.repeatMode = if (repeat == 1) Player.REPEAT_MODE_OFF else Player.REPEAT_MODE_ALL
                            showRepeat = false
                        },
                    )
                    DropdownMenuItem(
                        text = { Text("Shuffle", color = if (shuffle) Mvp.accent else Color.White) },
                        onClick = {
                            val enabled = !shuffle
                            shuffle = enabled
                            player?.shuffleModeEnabled = enabled
                            showRepeat = false
                        },
                    )
                    DropdownMenuItem(
                        text = { Text("Reverse order", color = Color.White) },
                        onClick = {
                            val exo = player
                            if (exo != null && tracks.size > 1) {
                                val current = exo.currentMediaItem?.mediaId
                                val reversed = tracks.asReversed()
                                val at = reversed.indexOfFirst { it.id.toString() == current }.let { if (it < 0) 0 else it }
                                val pos = exo.currentPosition
                                exo.setMediaItems(
                                    reversed.map { track ->
                                        MediaItem.Builder()
                                            .setMediaId(track.id.toString())
                                            .setUri(VaultDataSource.uriFor(track.id))
                                            .setMediaMetadata(
                                                MediaMetadata.Builder()
                                                    .setTitle(trackTitle(track.name))
                                                    .build(),
                                            )
                                            .build()
                                    },
                                    at,
                                    pos,
                                )
                                exo.prepare()
                                if (playing) exo.play()
                                MusicHub.tracks = reversed
                                index = at
                            }
                            showRepeat = false
                        },
                    )
                }
                if (repeat == 2) {
                    Text(
                        "1",
                        color = Mvp.accent,
                        fontSize = 9.sp,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier.padding(top = 14.dp),
                    )
                }
            }
            Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                Icon(
                    painterResource(R.drawable.ic_tg_prev),
                    contentDescription = "Previous",
                    tint = Color.White,
                    modifier = Modifier
                        .size(48.dp)
                        .tgPress(
                            onTap = { player?.seekToPreviousMediaItem() },
                            onHold = {
                                val exo = player ?: return@tgPress
                                exo.seekTo((exo.currentPosition - 10000L).coerceAtLeast(0L))
                            },
                            onReleaseHold = {},
                        )
                        .padding(12.dp),
                )
            }
            Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                Icon(
                    painterResource(if (playing) R.drawable.ic_tg_pause else R.drawable.ic_tg_play),
                    contentDescription = "Play",
                    tint = Color.White,
                    modifier = Modifier
                        .size(48.dp)
                        .clickable {
                            val exo = player ?: return@clickable
                            if (exo.isPlaying) exo.pause() else exo.play()
                        }
                        .padding(8.dp),
                )
            }
            Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                Icon(
                    painterResource(R.drawable.ic_tg_next),
                    contentDescription = "Next",
                    tint = Color.White,
                    modifier = Modifier
                        .size(48.dp)
                        .tgPress(
                            onTap = { player?.seekToNextMediaItem() },
                            onHold = { player?.setPlaybackSpeed(4f) },
                            onReleaseHold = { player?.setPlaybackSpeed(speed) },
                        )
                        .padding(12.dp),
                )
            }
            Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                Icon(
                    painterResource(R.drawable.ic_tg_more),
                    contentDescription = "Close",
                    tint = Color.White,
                    modifier = Modifier
                        .size(48.dp)
                        .clickable(onClick = onBack)
                        .padding(12.dp),
                )
            }
        }
        LazyColumn(Modifier.weight(1f).fillMaxWidth()) {
            itemsIndexed(tracks, key = { _, track -> track.id }) { i, track ->
                val current = i == index
                Row(
                    Modifier
                        .fillMaxWidth()
                        .height(56.dp)
                        .clickable {
                            player?.seekTo(i, 0L)
                            player?.play()
                            index = i
                        }
                        .padding(horizontal = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    SongThumb(track.id, Modifier.size(40.dp).clip(RoundedCornerShape(4.dp)))
                    Column(Modifier.weight(1f).padding(start = 10.dp, end = 8.dp)) {
                        Text(
                            trackTitle(track.name),
                            color = if (current) Mvp.accent else Color.White,
                            fontSize = 16.sp,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        Text(
                            track.name.substringAfterLast('.', "Audio").uppercase(),
                            color = Color(0xFF8B9BA8),
                            fontSize = 13.sp,
                            maxLines = 1,
                        )
                    }
                    val known = GalleryCache.videos.firstOrNull { it.id == track.id }?.takeIf { it.durationMs > 0 }?.durationText
                    if (!known.isNullOrBlank()) {
                        Text(known, color = Color(0xFF8B9BA8), fontSize = 13.sp)
                    }
                }
            }
        }
    }

    if (showCover) {
        Box(
            Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = 0.92f))
                .clickable { showCover = false }
                .padding(30.dp),
            contentAlignment = Alignment.Center,
        ) {
            val art = cover
            if (art != null) {
                Image(
                    bitmap = art.asImageBitmap(),
                    contentDescription = shown.name,
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(8.dp)),
                    contentScale = ContentScale.Fit,
                )
            }
        }
    }
}

private fun trackTitle(name: String): String = name.substringBeforeLast('.', name)

private fun speedLabel(speed: Float): String = when {
    abs(speed - 0.5f) < 0.05f -> "0.5X"
    abs(speed - 1.2f) < 0.05f -> "1.2X"
    abs(speed - 1.5f) < 0.05f -> "1.5X"
    abs(speed - 1.7f) < 0.05f -> "1.7X"
    abs(speed - 2f) < 0.05f -> "2X"
    else -> "1X"
}

private fun nextSpeed(speed: Float): Float = when {
    speed < 1.25f -> 1.5f
    speed < 1.75f -> 2f
    else -> 1f
}

@Composable
private fun TgSeek(
    fraction: Float,
    onScrub: (Float) -> Unit,
    onCommit: (Float) -> Unit,
) {
    Box(
        Modifier
            .fillMaxWidth()
            .height(38.dp)
            .padding(horizontal = 12.dp)
            .pointerInput(onScrub, onCommit) {
                awaitEachGesture {
                    val down = awaitFirstDown()
                    var value = (down.position.x / size.width).coerceIn(0f, 1f)
                    onScrub(value)
                    drag(down.id) { change ->
                        value = (change.position.x / size.width).coerceIn(0f, 1f)
                        onScrub(value)
                        change.consume()
                    }
                    onCommit(value)
                }
            },
        contentAlignment = Alignment.CenterStart,
    ) {
        Box(
            Modifier
                .fillMaxWidth()
                .height(4.dp)
                .clip(RoundedCornerShape(2.dp))
                .background(Color(0xFF3A4550)),
        )
        Box(
            Modifier
                .fillMaxWidth(fraction.coerceIn(0f, 1f))
                .height(4.dp)
                .background(Mvp.accent),
        )
        Box(
            Modifier
                .fillMaxWidth(fraction.coerceIn(0f, 1f))
                .wrapContentWidth(Alignment.End)
                .size(12.dp)
                .clip(CircleShape)
                .background(Color.White),
        )
    }
}

private fun Modifier.tgPress(
    onTap: () -> Unit,
    onHold: () -> Unit,
    onReleaseHold: () -> Unit,
): Modifier = pointerInput(onTap, onHold, onReleaseHold) {
    detectTapGestures(
        onPress = {
            coroutineScope {
                val job = launch {
                    delay(300)
                    onHold()
                }
                tryAwaitRelease()
                val held = job.isCompleted
                job.cancel()
                if (held) onReleaseHold() else onTap()
            }
        },
    )
}

@Composable
private fun SongThumb(id: Long, modifier: Modifier) {
    var bmp by remember(id) { mutableStateOf(GalleryCache.thumb(id, 80)) }
    LaunchedEffect(id) {
        if (bmp != null) return@LaunchedEffect
        val decoded = withContext(Dispatchers.Default) {
            val jpeg = if (RemoteVault.connected()) {
                RemoteVault.getBytes("/v1/thumbnail?id=$id&max=160")
            } else {
                CoreBridge.nativeThumbnail(id, 160)
            }
            jpeg?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
        }
        if (decoded != null) {
            GalleryCache.putThumb(id, 80, decoded)
            bmp = decoded
        }
    }
    Box(modifier.background(Color(0xFF2C2C2E)), contentAlignment = Alignment.Center) {
        val art = bmp
        if (art == null) {
            Text("♪", color = Mvp.accent, fontSize = 14.sp)
        } else {
            Image(
                bitmap = art.asImageBitmap(),
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop,
            )
        }
    }
}

private fun formatClock(ms: Long): String {
    val total = (ms / 1000L).coerceAtLeast(0L)
    val hours = total / 3600L
    val minutes = (total / 60L) % 60L
    val seconds = total % 60L
    return if (hours > 0L) "%d:%02d:%02d".format(hours, minutes, seconds)
    else "%d:%02d".format(minutes, seconds)
}

/** The 36dp bar Telegram keeps under the title while a song is playing. */
@Composable
fun TelegramMusicBar(onOpen: () -> Unit) {
    val context = LocalContext.current
    var title by remember { mutableStateOf("") }
    var playing by remember { mutableStateOf(false) }
    var fraction by remember { mutableFloatStateOf(0f) }
    var speedText by remember { mutableStateOf("1X") }
    var visible by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        while (true) {
            val exo = MusicPlayer.exo
            val shown = exo != null && exo.mediaItemCount > 0
            visible = shown
            if (exo != null && shown) {
                title = exo.mediaMetadata.title?.toString()?.ifBlank { null } ?: "Music"
                playing = exo.isPlaying
                val length = exo.duration
                fraction = if (length > 0L) (exo.currentPosition.toFloat() / length.toFloat()).coerceIn(0f, 1f) else 0f
                speedText = speedLabel(exo.playbackParameters.speed)
            }
            delay(200)
        }
    }
    if (!visible) return
    Box(
        Modifier
            .fillMaxWidth()
            .height(36.dp)
            .background(Color(0xFF1C1C1E))
            .clickable(onClick = onOpen),
    ) {
        Row(Modifier.fillMaxSize(), verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier
                    .size(36.dp)
                    .clickable {
                        ContextCompat.startForegroundService(
                            context,
                            Intent(context, MusicPlaybackService::class.java)
                                .setAction(MusicPlaybackService.ACTION_TOGGLE),
                        )
                    },
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    painterResource(if (playing) R.drawable.ic_tg_pause else R.drawable.ic_tg_play),
                    contentDescription = "Play",
                    tint = Color.White,
                    modifier = Modifier.size(18.dp),
                )
            }
            Text(
                title,
                color = Color.White,
                fontSize = 15.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
            Text(
                speedText,
                color = Color(0xFF8B9BA8),
                fontSize = 13.sp,
                modifier = Modifier
                    .clickable {
                        val exo = MusicPlayer.exo ?: return@clickable
                        exo.setPlaybackSpeed(nextSpeed(exo.playbackParameters.speed))
                    }
                    .padding(horizontal = 8.dp),
            )
            Box(
                Modifier
                    .size(36.dp)
                    .clickable {
                        context.startService(
                            Intent(context, MusicPlaybackService::class.java)
                                .setAction(MusicPlaybackService.ACTION_STOP),
                        )
                    },
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    painterResource(R.drawable.ic_tg_close),
                    contentDescription = "Close",
                    tint = Color(0xFF8B9BA8),
                    modifier = Modifier.size(16.dp),
                )
            }
        }
        Box(
            Modifier
                .align(Alignment.BottomStart)
                .fillMaxWidth(fraction.coerceIn(0.001f, 1f))
                .height(2.dp)
                .background(Mvp.accent),
        )
    }
}
