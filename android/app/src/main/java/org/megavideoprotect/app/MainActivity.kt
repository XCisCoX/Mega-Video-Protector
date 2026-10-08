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

        // The vault lives in the app-private sandbox (filesDir), exactly like
        // the desktop app's QStandardPaths::AppDataLocation fallback on
        // Android. No storage permission needed.
        val vaultRoot = filesDir.absolutePath

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

                // System back / gesture: leave the player, lock the vault.
                BackHandler(enabled = screen !is Screen.Setup) {
                    screen = when (screen) {
                        is Screen.Player -> Screen.Vault
                        is Screen.Vault -> {
                            runCatching { CoreBridge.nativeLock() }
                            Screen.Login
                        }
                        else -> Screen.Setup
                    }
                }

                Box(Modifier.fillMaxSize()) {
                    when (val current = screen) {
                        is Screen.Setup -> SetupScreen(
                            vaultLocation = vaultRoot,
                            onCreated = { screen = Screen.Vault },
                            onOpenExisting = { screen = Screen.Login },
                        )
                        is Screen.Login -> LoginScreen(
                            vaultLocation = vaultRoot,
                            onUnlocked = { screen = Screen.Vault },
                            onBack = { screen = Screen.Setup },
                        )
                        is Screen.Vault -> VaultScreen(
                            vaultLocation = vaultRoot,
                            onLock = {
                                runCatching { CoreBridge.nativeLock() }
                                screen = Screen.Login
                            },
                            onPlay = { id, name -> screen = Screen.Player(id, name) },
                        )
                        is Screen.Player -> PlayerScreen(
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
