package org.megavideoprotect.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.padding
import androidx.compose.ui.Modifier
import androidx.core.view.WindowCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)

        // The vault lives in the app-private sandbox (filesDir), exactly like
        // the desktop app's QStandardPaths::AppDataLocation fallback on
        // Android. No storage permission needed.
        val vaultRoot = filesDir.absolutePath

        setContent {
            MaterialTheme(colorScheme = MvpColorScheme) {
                var screen by remember { mutableStateOf<Screen>(Screen.Setup) }
                var unlocked by remember {
                    mutableStateOf(
                        // Deterministic: no network / IO in composition; the
                        // vault check runs synchronously on first composition.
                        CoreBridge.nativeVaultExists(vaultRoot)
                    )
                }
                var playerArgs by remember { mutableStateOf<Pair<Long, String>?>(null) }
                var playerDuration by remember { mutableLongStateOf(0L) }

                fun enterVault() {
                    unlocked = true
                    screen = Screen.Vault
                }

                fun openPlayer(id: Long, name: String) {
                    val dur = runCatching {
                        JSONObject(CoreBridge.nativeMediaInfo(id)).optLong("durationMs", 0)
                    }.getOrDefault(0L)
                    playerDuration = dur
                    playerArgs = id to name
                    screen = Screen.Player(id, name)
                }

                Box(Modifier.fillMaxSize()) {
                    when (screen) {
                        is Screen.Setup -> SetupScreen(
                            vaultLocation = vaultRoot,
                            onCreated = { enterVault() },
                            onOpenExisting = { screen = Screen.Login },
                        )
                        is Screen.Login -> LoginScreen(
                            vaultLocation = vaultRoot,
                            onUnlocked = { enterVault() },
                            onBack = { screen = Screen.Setup },
                        )
                        is Screen.Vault -> VaultScreen(
                            vaultLocation = vaultRoot,
                            onLock = {
                                unlocked = false
                                screen = Screen.Login
                            },
                            onPlay = { id, name -> openPlayer(id, name) },
                        )
                        is Screen.Player -> PlayerScreen(
                            videoId = (screen as Screen.Player).videoId,
                            name = (screen as Screen.Player).name,
                            durationMs = playerDuration,
                            onBack = {
                                playerArgs = null
                                screen = Screen.Vault
                            },
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
