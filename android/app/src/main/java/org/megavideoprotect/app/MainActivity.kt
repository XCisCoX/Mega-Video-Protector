package org.megavideoprotect.app

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.core.view.WindowCompat

/** Survives leaving the app, as long as the process is still alive. */
private object AppSession {
    var screen: Screen = Screen.Login
    var leftAt: Long = 0L
}

class MainActivity : ComponentActivity() {
    private var screen by mutableStateOf<Screen>(Screen.Login)
    /** Files handed over from another app's Share / Send menu. Held until the vault is open. */
    private var pendingShare by mutableStateOf<List<Uri>>(emptyList())

    private fun show(next: Screen) {
        screen = next
        AppSession.screen = next
    }

    private fun unlocked(): Boolean =
        RemoteVault.connected() || runCatching { CoreBridge.nativeIsUnlocked() }.getOrDefault(false)

    private fun awayTooLong(): Boolean {
        val limit = VaultLocation.idleLockMs(this)
        if (limit <= 0L || AppSession.leftAt == 0L) return false
        return SystemClock.elapsedRealtime() - AppSession.leftAt >= limit
    }

    private fun lockNow() {
        if (MusicHub.active) {
            startService(
                Intent(this, MusicPlaybackService::class.java)
                    .setAction(MusicPlaybackService.ACTION_STOP),
            )
        }
        if (RemoteVault.connected()) RemoteVault.disconnect()
        else runCatching { CoreBridge.nativeLock() }
        GalleryCache.clear()
        show(Screen.Login)
    }

    private fun openMusicFromNotification(source: Intent?) {
        if (source?.getBooleanExtra(EXTRA_OPEN_MUSIC, false) != true) return
        if (!unlocked() || MusicHub.tracks.isEmpty()) return
        val songs = MusicHub.tracks
        val id = MusicHub.anchorId.takeIf { anchor -> songs.any { it.id == anchor } } ?: songs.first().id
        val title = songs.firstOrNull { it.id == id }?.name ?: songs.first().name
        show(Screen.Music(id, title, songs, startFresh = false))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)

        val vaultRoot = VaultLocation.saved(this) ?: filesDir.absolutePath
        val locationOptions = runCatching { VaultLocation.options(this) }
            .getOrDefault(listOf(filesDir.absolutePath))
        val vaultExists = runCatching { CoreBridge.nativeVaultExists(vaultRoot) }.getOrDefault(false)
        val open = unlocked()
        if (open && awayTooLong()) {
            if (RemoteVault.connected()) RemoteVault.disconnect()
            else runCatching { CoreBridge.nativeLock() }
            GalleryCache.clear()
            show(Screen.Login)
        } else if (open && AppSession.screen !is Screen.Login && AppSession.screen !is Screen.Setup) {
            show(AppSession.screen)
        } else if (vaultExists) {
            show(Screen.Login)
        } else {
            show(Screen.Setup)
        }
        acceptShare(intent)
        openMusicFromNotification(intent)

        setContent {
            MaterialTheme(colorScheme = MvpColorScheme) {
                // Back leaves the player for the library. From the library or
                // the login screen it leaves the app without locking.
                BackHandler(enabled = screen !is Screen.Setup) {
                    when (screen) {
                        is Screen.Player, is Screen.Image, is Screen.Music -> show(Screen.Vault)
                        else -> moveTaskToBack(true)
                    }
                }

                Box(Modifier.fillMaxSize()) {
                    when (val current = screen) {
                        is Screen.Setup -> SetupScreen(
                            vaultLocation = vaultRoot,
                            locationOptions = locationOptions,
                            onCreated = { show(Screen.Vault) },
                            onOpenExisting = { show(Screen.Login) },
                        )
                        is Screen.Login -> LoginScreen(
                            vaultLocation = vaultRoot,
                            locationOptions = locationOptions,
                            onUnlocked = { show(Screen.Vault) },
                            onBack = { show(Screen.Setup) },
                        )
                        is Screen.Vault -> VaultScreen(
                            vaultLocation = RemoteVault.label() ?: vaultRoot,
                            onLock = { lockNow() },
                            shareUris = pendingShare,
                            onShareHandled = { pendingShare = emptyList() },
                            onOpenMusic = {
                                val songs = MusicHub.tracks
                                if (songs.isNotEmpty()) {
                                    val id = MusicHub.anchorId.takeIf { anchor -> songs.any { it.id == anchor } }
                                        ?: songs.first().id
                                    val title = songs.firstOrNull { it.id == id }?.name ?: songs.first().name
                                    show(Screen.Music(id, title, songs, startFresh = false))
                                }
                            },
                            onPlay = { id, name, playlist ->
                                show(
                                    when {
                                        isImageName(name) -> Screen.Image(id, name)
                                        isAudioName(name) -> {
                                            val songs = playlist
                                                .filter { isAudioName(it.name) }
                                                .map { PlayItem(it.id, it.name) }
                                            Screen.Music(id, name, songs)
                                        }
                                        else -> {
                                            val playable = playlist
                                                .filter { !isImageName(it.name) && !isAudioName(it.name) }
                                                .map { PlayItem(it.id, it.name) }
                                            Screen.Player(id, name, playable)
                                        }
                                    },
                                )
                            },
                        )
                        is Screen.Player -> PlayerScreen(
                            videoId = current.videoId,
                            name = current.name,
                            playlist = current.playlist,
                            onBack = { show(Screen.Vault) },
                        )
                        is Screen.Image -> ImageViewerScreen(
                            videoId = current.videoId,
                            name = current.name,
                            onBack = { show(Screen.Vault) },
                        )
                        is Screen.Music -> MusicScreen(
                            videoId = current.videoId,
                            name = current.name,
                            playlist = current.playlist,
                            startFresh = current.startFresh,
                            onBack = { show(Screen.Vault) },
                        )
                    }
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        acceptShare(intent)
        openMusicFromNotification(intent)
    }

    /** Pulls the files out of a Share / Send intent. Ignored for a normal launch. */
    private fun acceptShare(intent: Intent?) {
        val uris = intent?.sharedStreams().orEmpty()
        if (uris.isEmpty()) return
        pendingShare = uris
        val inside = screen is Screen.Vault || screen is Screen.Player
            || screen is Screen.Image || screen is Screen.Music
        if (inside && unlocked()) show(Screen.Vault)
    }

    override fun onStart() {
        super.onStart()
        val inside = screen is Screen.Vault || screen is Screen.Player
            || screen is Screen.Image || screen is Screen.Music
        if (inside && awayTooLong()) lockNow()
        AppSession.leftAt = 0L
    }

    override fun onStop() {
        AppSession.leftAt = SystemClock.elapsedRealtime()
        super.onStop()
    }
}

/** Content URIs attached to ACTION_SEND or ACTION_SEND_MULTIPLE. */
private fun Intent.sharedStreams(): List<Uri> {
    if (action != Intent.ACTION_SEND && action != Intent.ACTION_SEND_MULTIPLE) return emptyList()
    val found = LinkedHashSet<Uri>()
    fun add(uri: Uri?) {
        if (uri != null) found.add(uri)
    }
    if (Build.VERSION.SDK_INT >= 33) {
        add(getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java))
        getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java)?.forEach { add(it) }
    } else {
        @Suppress("DEPRECATION")
        add(getParcelableExtra(Intent.EXTRA_STREAM))
        @Suppress("DEPRECATION")
        getParcelableArrayListExtra<Uri>(Intent.EXTRA_STREAM)?.forEach { add(it) }
    }
    val clip = clipData
    if (clip != null) {
        for (index in 0 until clip.itemCount) add(clip.getItemAt(index).uri)
    }
    return found.toList()
}
