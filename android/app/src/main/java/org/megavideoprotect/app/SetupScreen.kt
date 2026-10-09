package org.megavideoprotect.app

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
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
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Mirrors the desktop buildSetupPage(): title, description, location,
 *  password + confirmation, Argon2id profile, Create / Open existing. */
@Composable
fun SetupScreen(
    vaultLocation: String,
    locationOptions: List<String>,
    onCreated: () -> Unit,
    onOpenExisting: () -> Unit,
) {
    val context = LocalContext.current
    var location by remember { mutableStateOf(vaultLocation) }
    var showFolderPicker by remember { mutableStateOf(false) }
    var password by remember { mutableStateOf("") }
    var confirmation by remember { mutableStateOf("") }
    // Same profiles as the desktop client (no phone-specific downgrade).
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
        // A phone cannot afford the desktop profile: Argon2id at 256 MiB x 3
        // took ~45 s to unlock on a Redmi Note 13 Pro, during which the UI looks
        // frozen. The mobile profile keeps the same iteration count at 64 MiB
        // (still far above OWASP's Argon2id floor) and unlocks in a few seconds.
        val memKib = when {
            profile.startsWith("High") -> 512 * 1024
            profile.startsWith("Mobile") -> 64 * 1024
            else -> 256 * 1024
        }
        val iters = if (profile.startsWith("High")) 4 else 3
        scope.launch {
            val res = withContext(Dispatchers.Default) {
                CoreBridge.Result.parse(CoreBridge.nativeCreateVault(location, password, memKib, iters, 1))
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
                // Edge-to-edge activity: pad by the IME inset so the soft
                // keyboard does not cover the Create vault button.
                .safeDrawingPadding()
                .imePadding()
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
                MvpDescription("Vault storage folder")
                MvpCombo(
                    selected = VaultLocation.label(context, location),
                    items = locationOptions.map { VaultLocation.label(context, it) },
                    onSelect = { label ->
                        val index = locationOptions.map { VaultLocation.label(context, it) }.indexOf(label)
                        if (index >= 0) {
                            location = locationOptions[index]
                            VaultLocation.save(context, location)
                        }
                    },
                )
                MvpButton(
                    "Choose folder…",
                    onClick = { showFolderPicker = true },
                    enabled = !busy,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpInput(
                    value = location,
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
                if (busy) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp)
                        Text("Creating the vault — deriving the key…", color = Mvp.description, fontSize = 12.sp)
                    }
                }
                MvpButton("Create vault", onClick = { create() }, primary = true, enabled = !busy, modifier = Modifier.fillMaxWidth())
                MvpButton("Open an existing vault", onClick = onOpenExisting, enabled = !busy, modifier = Modifier.fillMaxWidth())
                if (showFolderPicker) {
                    FolderPickerDialog(
                        onDismiss = { showFolderPicker = false },
                        onPicked = { picked ->
                            location = picked
                            VaultLocation.save(context, picked)
                            showFolderPicker = false
                        },
                    )
                }
            }
        }
    }
}
