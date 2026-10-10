package org.megavideoprotect.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import kotlin.concurrent.thread
import androidx.core.app.ServiceCompat
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.ProgressiveMediaSource

const val EXTRA_OPEN_MUSIC = "org.megavideoprotect.app.OPEN_MUSIC"

/** The queue the notification reopens. Lives with the process, not the page. */
object MusicHub {
    var tracks: List<PlayItem> = emptyList()
    var anchorId: Long = 0L
    var active: Boolean = false
}

/** One player for the page and the notification. Closing the page does not release it. */
object MusicPlayer {
    var exo: ExoPlayer? = null

    fun player(context: android.content.Context): ExoPlayer {
        exo?.let { return it }
        val created = ExoPlayer.Builder(context.applicationContext)
            .setMediaSourceFactory(ProgressiveMediaSource.Factory(VaultDataSource.Factory()))
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(C.USAGE_MEDIA)
                    .setContentType(C.AUDIO_CONTENT_TYPE_MUSIC)
                    .build(),
                /* handleAudioFocus = */ true,
            )
            .setHandleAudioBecomingNoisy(true)
            .setWakeMode(C.WAKE_MODE_LOCAL)
            .build()
        exo = created
        return created
    }

    fun release() {
        exo?.release()
        exo = null
        MusicHub.active = false
    }
}

/**
 * Keeps the song playing after the page closes and posts the status-bar
 * controls: previous, play or pause, next.
 */
class MusicPlaybackService : Service() {
    private var session: MediaSession? = null
    private var listening = false
    private val handler = Handler(Looper.getMainLooper())
    private var coverId: Long = Long.MIN_VALUE
    private var coverBitmap: Bitmap? = null
    private var coverLoading: Long = Long.MIN_VALUE
    private var noteBitmap: Bitmap? = null
    private val ticker = object : Runnable {
        override fun run() {
            val player = MusicPlayer.exo ?: return
            if (!player.isPlaying) return
            push(player, rebuild = false)
            handler.postDelayed(this, 1000L)
        }
    }
    private val listener = object : Player.Listener {
        override fun onEvents(player: Player, events: Player.Events) {
            val changed = events.contains(Player.EVENT_IS_PLAYING_CHANGED) ||
                events.contains(Player.EVENT_MEDIA_ITEM_TRANSITION) ||
                events.contains(Player.EVENT_PLAYBACK_STATE_CHANGED) ||
                events.contains(Player.EVENT_MEDIA_METADATA_CHANGED) ||
                events.contains(Player.EVENT_SHUFFLE_MODE_ENABLED_CHANGED) ||
                events.contains(Player.EVENT_REPEAT_MODE_CHANGED)
            push(player, rebuild = changed)
            handler.removeCallbacks(ticker)
            if (player.isPlaying) handler.postDelayed(ticker, 1000L)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopPlayback()
            return START_NOT_STICKY
        }
        val player = MusicPlayer.player(this)
        ensureSession(player)
        if (!listening) {
            player.addListener(listener)
            listening = true
        }
        val title = player.mediaMetadata.title?.toString()?.ifBlank { null } ?: "Music"
        val current = buildNotification(player, title, player.isPlaying)
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIFICATION_ID, current, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(NOTIFICATION_ID, current)
        }
        when (intent?.action) {
            ACTION_TOGGLE -> if (player.isPlaying) player.pause() else player.play()
            ACTION_NEXT -> player.seekToNextMediaItem()
            ACTION_PREV -> player.seekToPreviousMediaItem()
            ACTION_SHUFFLE -> player.shuffleModeEnabled = !player.shuffleModeEnabled
            ACTION_REPEAT -> player.repeatMode = when (player.repeatMode) {
                Player.REPEAT_MODE_OFF -> Player.REPEAT_MODE_ALL
                Player.REPEAT_MODE_ALL -> Player.REPEAT_MODE_ONE
                else -> Player.REPEAT_MODE_OFF
            }
        }
        push(player, rebuild = true)
        return START_STICKY
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        val player = MusicPlayer.exo
        if (player == null || !player.playWhenReady) stopPlayback()
    }

    override fun onDestroy() {
        handler.removeCallbacks(ticker)
        MusicPlayer.exo?.removeListener(listener)
        listening = false
        session?.isActive = false
        session?.release()
        session = null
        super.onDestroy()
    }

    private fun ensureSession(player: Player) {
        if (session != null) return
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Music",
            NotificationManager.IMPORTANCE_LOW,
        )
        channel.setShowBadge(false)
        channel.lockscreenVisibility = Notification.VISIBILITY_PUBLIC
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        val created = MediaSession(this, "mvp-music")
        created.setCallback(object : MediaSession.Callback() {
            override fun onPlay() {
                MusicPlayer.exo?.play()
            }

            override fun onPause() {
                MusicPlayer.exo?.pause()
            }

            override fun onSkipToNext() {
                MusicPlayer.exo?.seekToNextMediaItem()
            }

            override fun onSkipToPrevious() {
                MusicPlayer.exo?.seekToPreviousMediaItem()
            }

            override fun onSeekTo(pos: Long) {
                MusicPlayer.exo?.seekTo(pos)
            }

            override fun onStop() {
                stopPlayback()
            }
        })
        created.isActive = true
        session = created
        player.currentMediaItem?.mediaId?.toLongOrNull()?.let { MusicHub.anchorId = it }
    }

    private fun push(player: Player, rebuild: Boolean) {
        val session = session ?: return
        val playing = player.isPlaying
        val state = when {
            player.playbackState == Player.STATE_BUFFERING -> PlaybackState.STATE_BUFFERING
            playing -> PlaybackState.STATE_PLAYING
            player.playbackState == Player.STATE_ENDED -> PlaybackState.STATE_STOPPED
            else -> PlaybackState.STATE_PAUSED
        }
        var actions = PlaybackState.ACTION_PLAY or
            PlaybackState.ACTION_PAUSE or
            PlaybackState.ACTION_PLAY_PAUSE or
            PlaybackState.ACTION_SEEK_TO or
            PlaybackState.ACTION_STOP
        if (player.mediaItemCount > 1) {
            actions = actions or PlaybackState.ACTION_SKIP_TO_NEXT or PlaybackState.ACTION_SKIP_TO_PREVIOUS
        }
        session.setPlaybackState(
            PlaybackState.Builder()
                .setActions(actions)
                .setState(state, player.currentPosition.coerceAtLeast(0L), if (playing) 1f else 0f)
                .build(),
        )
        val title = player.mediaMetadata.title?.toString()?.ifBlank { null } ?: "Music"
        val artist = artistOf(player)
        val duration = player.duration
        val metadata = MediaMetadata.Builder()
            .putString(MediaMetadata.METADATA_KEY_TITLE, title)
            .putString(MediaMetadata.METADATA_KEY_ARTIST, artist)
            .putString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST, artist)
        if (duration > 0L) metadata.putLong(MediaMetadata.METADATA_KEY_DURATION, duration)
        val id = player.currentMediaItem?.mediaId?.toLongOrNull()
        val cover = coverFor(id)
        if (cover != null) {
            metadata.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, cover)
            metadata.putBitmap(MediaMetadata.METADATA_KEY_ART, cover)
        }
        session.setMetadata(metadata.build())
        player.currentMediaItem?.mediaId?.toLongOrNull()?.let { MusicHub.anchorId = it }
        MusicHub.active = player.mediaItemCount > 0
        if (!rebuild) return
        val notification = buildNotification(player, title, playing)
        if (playing) {
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(
                    NOTIFICATION_ID,
                    notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK,
                )
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
        } else {
            ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_DETACH)
            getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification)
        }
    }

    private fun buildNotification(player: Player, title: String, playing: Boolean): Notification {
        val open = PendingIntent.getActivity(
            this,
            1,
            Intent(this, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
                putExtra(EXTRA_OPEN_MUSIC, true)
            },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val stop = PendingIntent.getService(
            this,
            2,
            Intent(this, MusicPlaybackService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val playIcon = if (playing) R.drawable.ic_tg_pause else R.drawable.ic_tg_play
        val playLabel = if (playing) "Pause" else "Play"
        val repeatIcon = if (player.repeatMode == Player.REPEAT_MODE_ONE) {
            R.drawable.ic_notify_repeat_one
        } else {
            R.drawable.ic_tg_repeat
        }
        val artist = artistOf(player)
        return Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_music)
            .setLargeIcon(coverFor(player.currentMediaItem?.mediaId?.toLongOrNull()))
            .setContentTitle(title)
            .setContentText(artist)
            .setSubText(null)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_TRANSPORT)
            .setColor(0xFF3390EC.toInt())
            .setContentIntent(open)
            .setDeleteIntent(stop)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setOngoing(playing)
            .setOnlyAlertOnce(true)
            .addAction(action(ACTION_SHUFFLE, R.drawable.ic_notify_shuffle, "Shuffle", 6))
            .addAction(action(ACTION_PREV, R.drawable.ic_tg_prev, "Previous", 3))
            .addAction(action(ACTION_TOGGLE, playIcon, playLabel, 4))
            .addAction(action(ACTION_NEXT, R.drawable.ic_tg_next, "Next", 5))
            .addAction(action(ACTION_REPEAT, repeatIcon, "Repeat", 7))
            .setStyle(
                Notification.MediaStyle()
                    .setMediaSession(session?.sessionToken)
                    .setShowActionsInCompactView(0, 1, 2, 3, 4),
            )
            .build()
    }

    private fun artistOf(player: Player): String {
        val tagged = player.mediaMetadata.artist?.toString()?.trim().orEmpty()
        if (tagged.isNotEmpty() && tagged != "Mega Vault Protect") return tagged
        val file = MusicHub.tracks.firstOrNull {
            it.id.toString() == player.currentMediaItem?.mediaId
        }?.name.orEmpty()
        val base = file.substringBeforeLast('.', file)
        val split = base.split(" - ", limit = 2)
        if (split.size == 2 && split[0].isNotBlank() && split[1].isNotBlank()) return split[0]
        return ""
    }

    private fun coverFor(id: Long?): Bitmap {
        val note = noteCover()
        if (id == null) return note
        if (coverId == id) return coverBitmap ?: note
        if (coverLoading != id) {
            coverLoading = id
            thread(name = "mvp-cover") {
                val bytes = runCatching { CoreBridge.nativeThumbnail(id, 320) }.getOrNull()
                val decoded = bytes?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
                handler.post {
                    val player = MusicPlayer.exo ?: return@post
                    if (player.currentMediaItem?.mediaId?.toLongOrNull() != id) return@post
                    coverId = id
                    coverBitmap = decoded
                    coverLoading = Long.MIN_VALUE
                    push(player, rebuild = true)
                }
            }
        }
        return note
    }

    private fun noteCover(): Bitmap {
        noteBitmap?.let { return it }
        val side = 256
        val bitmap = Bitmap.createBitmap(side, side, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        canvas.drawColor(Color.rgb(28, 28, 30))
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(51, 144, 236) }
        val s = side.toFloat()
        canvas.drawOval(s * 0.25f, s * 0.60f, s * 0.51f, s * 0.80f, paint)
        canvas.drawOval(s * 0.55f, s * 0.52f, s * 0.81f, s * 0.72f, paint)
        canvas.drawRect(s * 0.46f, s * 0.22f, s * 0.53f, s * 0.70f, paint)
        canvas.drawRect(s * 0.76f, s * 0.16f, s * 0.83f, s * 0.62f, paint)
        canvas.drawRect(s * 0.46f, s * 0.20f, s * 0.83f, s * 0.28f, paint)
        noteBitmap = bitmap
        return bitmap
    }

    private fun action(action: String, icon: Int, title: String, request: Int): Notification.Action {
        val pending = PendingIntent.getForegroundService(
            this,
            request,
            Intent(this, MusicPlaybackService::class.java).setAction(action),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        return Notification.Action.Builder(icon, title, pending).build()
    }

    private fun stopPlayback() {
        handler.removeCallbacks(ticker)
        MusicPlayer.exo?.removeListener(listener)
        listening = false
        MusicPlayer.release()
        session?.isActive = false
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    companion object {
        const val ACTION_ATTACH = "org.megavideoprotect.app.ATTACH_MUSIC"
        const val ACTION_TOGGLE = "org.megavideoprotect.app.TOGGLE_MUSIC"
        const val ACTION_NEXT = "org.megavideoprotect.app.NEXT_MUSIC"
        const val ACTION_PREV = "org.megavideoprotect.app.PREV_MUSIC"
        const val ACTION_SHUFFLE = "org.megavideoprotect.app.SHUFFLE_MUSIC"
        const val ACTION_REPEAT = "org.megavideoprotect.app.REPEAT_MUSIC"
        const val ACTION_STOP = "org.megavideoprotect.app.STOP_MUSIC"
        private const val CHANNEL_ID = "mvp_music"
        private const val NOTIFICATION_ID = 41
    }
}
