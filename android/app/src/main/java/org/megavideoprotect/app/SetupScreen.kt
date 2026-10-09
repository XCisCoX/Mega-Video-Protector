package org.megavideoprotect.app

import android.Manifest
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
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

/** Create a vault here, or scan the PC the same way as the unlock screen. */
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
    var scanning by remember { mutableStateOf(false) }
    var password by remember { mutableStateOf("") }
    var confirmation by remember { mutableStateOf("") }
    var profile by remember { mutableStateOf("Balanced — 256 MiB, 3 iterations") }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    var checkingPc by remember { mutableStateOf(false) }
    var pcCode by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()
    val standard = remember(locationOptions) { locationOptions.distinct() }

    val cameraPermission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) {
            scanning = true
            error = null
        } else {
            error = "Camera permission is needed to scan the PC."
        }
    }

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
        val memKib = when {
            profile.startsWith("High") -> 512 * 1024
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

    fun connect() {
        val code = pcCode
        if (code == null) return
        if (password.isEmpty()) {
            error = "Enter the vault password."
            return
        }
        error = null
        busy = true
        checkingPc = true
        scope.launch {
            val failure = withContext(Dispatchers.IO) { RemoteVault.login(code, password) }
            busy = false
            checkingPc = false
            if (failure == null) {
                GalleryCache.clear()
                onCreated()
            } else {
                error = failure
            }
        }
    }

    if (scanning) {
        QrScanScreen(
            onCode = { text ->
                pcCode = text
                scanning = false
                error = null
            },
            onClose = { scanning = false },
        )
        return
    }

    Box(Modifier.fillMaxSize()) {
        Column(
            Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .safeDrawingPadding()
                .imePadding()
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            MvpCard(Modifier.widthIn(max = 420.dp).fillMaxWidth()) {
                MvpTitle(if (pcCode == null) "Create your encrypted vault" else "Unlock")
                MvpDescription(
                    if (pcCode == null) {
                        "Choose where the vault lives and a strong password. The password is never stored."
                    } else {
                        "PC code ready. This password opens the shared vault."
                    }
                )
                if (pcCode != null) {
                    Text(
                        "Use this phone",
                        color = Mvp.accent,
                        fontSize = 13.sp,
                        modifier = Modifier.clickable(enabled = !busy) {
                            pcCode = null
                            error = null
                        },
                    )
                } else {
                    VaultPlaceField(
                        location = location,
                        options = standard,
                        enabled = !busy,
                        onLocation = { picked ->
                            location = picked
                            VaultLocation.save(context, picked)
                        },
                        onChooseFolder = { showFolderPicker = true },
                    )
                }
                MvpInput(
                    value = password,
                    onValueChange = { password = it },
                    placeholder = "Password",
                    isPassword = true,
                )
                if (pcCode == null) {
                    MvpInput(
                        value = confirmation,
                        onValueChange = { confirmation = it },
                        placeholder = "Confirm password",
                        isPassword = true,
                    )
                    MvpCombo(
                        selected = profile,
                        items = listOf(
                            "Balanced — 256 MiB, 3 iterations",
                            "High security — 512 MiB, 4 iterations",
                        ),
                        onSelect = { profile = it },
                        enabled = !busy,
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
                MvpError(error)
                if (busy) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp)
                        Text(
                            if (checkingPc) "Checking the password with the PC…"
                            else "Creating the vault — deriving the key…",
                            color = Mvp.description,
                            fontSize = 12.sp,
                        )
                    }
                }
                MvpButton(
                    if (pcCode == null) "Create vault" else "Connect",
                    onClick = { if (pcCode == null) create() else connect() },
                    primary = true,
                    enabled = !busy,
                    modifier = Modifier.fillMaxWidth(),
                )
                if (pcCode == null) {
                    MvpButton(
                        "Open an existing vault",
                        onClick = onOpenExisting,
                        enabled = !busy,
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
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
        ScanCorner(enabled = !busy) {
            cameraPermission.launch(Manifest.permission.CAMERA)
        }
    }
}
