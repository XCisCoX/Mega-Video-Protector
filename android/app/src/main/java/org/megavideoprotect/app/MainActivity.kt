package org.megavideoprotect.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.core.view.WindowCompat

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)

        // The vault location is user-selectable (see VaultLocation); fall back
        // to the private sandbox when nothing has been chosen yet. No storage
        // permission is needed for either root.
        val vaultRoot = VaultLocation.saved(this) ?: filesDir.absolutePath
        val locationOptions = runCatching { VaultLocation.options(this) }
            .getOrDefault(listOf(filesDir.absolutePath))

        // Deterministic: no network / IO in composition — the vault check runs
        // once here, before the first frame.
        val vaultExists = runCatching { CoreBridge.nativeVaultExists(vaultRoot) }.getOrDefault(false)

        setContent {
            MaterialTheme(colorScheme = MvpColorScheme) {
                // Resume where the user left off: the unlock page when a vault
                // already exists in the sandbox, setup otherwise.
                var screen by remember {
                    mutableStateOf<Screen>(if (vaultExists) Screen.Login else Screen.Setup)
                }

                // System back / gesture: leave the media player or the image
                // viewer, lock the vault.
                BackHandler(enabled = screen !is Screen.Setup) {
                    screen = when (screen) {
                        is Screen.Player -> Screen.Vault
                        is Screen.Image -> Screen.Vault
                        is Screen.Vault -> {
                            runCatching { CoreBridge.nativeLock() }
                            GalleryCache.clear()
                            Screen.Login
                        }
                        else -> Screen.Setup
                    }
                }

                Box(Modifier.fillMaxSize()) {
                    when (val current = screen) {
                        is Screen.Setup -> SetupScreen(
                            vaultLocation = vaultRoot,
                            locationOptions = locationOptions,
                            onCreated = { screen = Screen.Vault },
                            onOpenExisting = { screen = Screen.Login },
                        )
                        is Screen.Login -> LoginScreen(
                            vaultLocation = vaultRoot,
                            locationOptions = locationOptions,
                            onUnlocked = { screen = Screen.Vault },
                            onBack = { screen = Screen.Setup },
                        )
                        is Screen.Vault -> VaultScreen(
                            vaultLocation = vaultRoot,
                            onLock = {
                                runCatching { CoreBridge.nativeLock() }
                                screen = Screen.Login
                            },
                            // A still image has no stream to play, so it opens in
                            // the image viewer; everything else in the player.
                            onPlay = { id, name ->
                                screen = if (isImageName(name)) {
                                    Screen.Image(id, name)
                                } else {
                                    Screen.Player(id, name)
                                }
                            },
                        )
                        is Screen.Player -> PlayerScreen(
                            videoId = current.videoId,
                            name = current.name,
                            onBack = { screen = Screen.Vault },
                        )
                        is Screen.Image -> ImageViewerScreen(
                            videoId = current.videoId,
                            name = current.name,
                            onBack = { screen = Screen.Vault },
                        )
                    }
                }
            }
        }
    }

    override fun onDestroy() {
        runCatching { CoreBridge.nativeClose() }
        super.onDestroy()
    }
}
