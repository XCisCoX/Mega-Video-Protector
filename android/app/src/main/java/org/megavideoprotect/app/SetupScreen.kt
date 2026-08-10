package org.megavideoprotect.app

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Mirrors the desktop buildSetupPage(): title, description, location,
 *  password + confirmation, Argon2id profile, Create / Open existing. */
@Composable
fun SetupScreen(
    vaultLocation: String,
    onCreated: () -> Unit,
    onOpenExisting: () -> Unit,
) {
    var password by remember { mutableStateOf("") }
    var confirmation by remember { mutableStateOf("") }
    var profile by remember { mutableStateOf("Balanced — 256 MiB, 3 iterations") }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()

    fun create() {
        if (password.length < 8) {
            error = "Password must be at least 8 characters."
            return
        }
        if (password != confirmation) {
            error = "Passwords do not match."
            return
        }
        error = null
        busy = true
        val memKib = if (profile.startsWith("High")) 512 * 1024 else 256 * 1024
        val iters = if (profile.startsWith("High")) 4 else 3
        scope.launch {
            val res = withContext(Dispatchers.Default) {
                CoreBridge.Result.parse(CoreBridge.nativeCreateVault(vaultLocation, password, memKib, iters, 1))
            }
            busy = false
            if (res.ok) onCreated() else error = res.error.ifBlank { res.detail }
        }
    }

    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(
            Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            MvpCard(Modifier.widthIn(max = 420.dp).fillMaxWidth()) {
                MvpTitle("Create your encrypted vault")
                MvpDescription(
                    "Choose a private storage location and a strong password. " +
                        "The password is never stored."
                )
                MvpInput(
                    value = vaultLocation,
                    onValueChange = {},
                    placeholder = "Vault storage folder",
                    enabled = false,
                )
                MvpInput(value = password, onValueChange = { password = it }, placeholder = "Password", isPassword = true)
                MvpInput(value = confirmation, onValueChange = { confirmation = it }, placeholder = "Confirm password", isPassword = true)
                MvpDescription("Argon2id security profile")
                MvpCombo(
                    selected = profile,
                    items = listOf(
                        "Balanced — 256 MiB, 3 iterations",
                        "High security — 512 MiB, 4 iterations",
                    ),
                    onSelect = { profile = it },
                )
                MvpError(error)
                MvpButton("Create vault", onClick = { create() }, primary = true, enabled = !busy, modifier = Modifier.fillMaxWidth())
                MvpButton("Open an existing vault", onClick = onOpenExisting, enabled = !busy, modifier = Modifier.fillMaxWidth())
            }
        }
    }
}
