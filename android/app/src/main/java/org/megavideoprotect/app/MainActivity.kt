package org.megavideoprotect.app

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
        if (RemoteVault.connected()) RemoteVault.disconnect()
        else runCatching { CoreBridge.nativeLock() }
        GalleryCache.clear()
        show(Screen.Login)
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

        setContent {
            MaterialTheme(colorScheme = MvpColorScheme) {
                // Back leaves the player for the library. From the library or
                // the login screen it leaves the app without locking.
                BackHandler(enabled = screen !is Screen.Setup) {
                    when (screen) {
                        is Screen.Player, is Screen.Image -> show(Screen.Vault)
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
                            onPlay = { id, name, playlist ->
                                show(
                                    if (isImageName(name)) {
                                        Screen.Image(id, name)
                                    } else {
                                        val playable = playlist
                                            .filter { !isImageName(it.name) }
                                            .map { PlayItem(it.id, it.name) }
                                        Screen.Player(id, name, playable)
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
                    }
                }
            }
        }
    }

    override fun onStart() {
        super.onStart()
        val inside = screen is Screen.Vault || screen is Screen.Player || screen is Screen.Image
        if (inside && awayTooLong()) lockNow()
        AppSession.leftAt = 0L
    }

    override fun onStop() {
        AppSession.leftAt = SystemClock.elapsedRealtime()
        super.onStop()
    }
}
