package org.megavideoprotect.app

import android.Manifest
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Mirrors the desktop login page: "Unlock Mega Video Protect". */
@Composable
fun LoginScreen(
    vaultLocation: String,
    locationOptions: List<String>,
    onUnlocked: () -> Unit,
    onBack: () -> Unit,
) {
    val context = LocalContext.current
    var location by remember { mutableStateOf(vaultLocation) }
    var showFolderPicker by remember { mutableStateOf(false) }
    var password by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    var pcCode by remember { mutableStateOf("") }
    var pcHint by remember { mutableStateOf<String?>(null) }
    var checkingPc by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()

    fun connectPc() {
        val text = pcCode.trim()
        if (text.isEmpty()) {
            error = "Scan the code on the PC, or paste it."
            return
        }
        if (password.isEmpty()) {
            error = "Enter the PC vault password."
            return
        }
        error = null
        busy = true
        checkingPc = true
        scope.launch {
            val failure = withContext(Dispatchers.IO) { RemoteVault.login(text, password) }
            busy = false
            checkingPc = false
            if (failure == null) {
                GalleryCache.clear()
                onUnlocked()
            } else {
                error = failure
            }
        }
    }

    val scanLauncher = rememberLauncherForActivityResult(ScanContract()) { result ->
        val text = result.contents
        if (!text.isNullOrBlank()) {
            pcCode = text.trim()
            error = null
            pcHint = "Code saved. Enter the PC vault password, then tap Connect."
        }
    }
    val cameraPermission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (!granted) {
            error = "Camera permission is needed to scan the PC."
            return@rememberLauncherForActivityResult
        }
        scanLauncher.launch(
            ScanOptions().apply {
                setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                setPrompt("Point at the code on your PC")
                setBeepEnabled(false)
                setOrientationLocked(false)
            }
        )
    }

    fun unlock() {
        if (password.isEmpty()) {
            error = "Enter the vault password."
            return
        }
        error = null
        busy = true
        scope.launch {
            val res = withContext(Dispatchers.Default) {
                CoreBridge.Result.parse(CoreBridge.nativeOpenVault(location, password))
            }
            busy = false
            if (res.ok) onUnlocked() else error = res.error.ifBlank { res.detail }
        }
    }

    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(
            Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                // The activity is edge-to-edge, so the soft keyboard overlays
                // the window instead of resizing it: without this the keyboard
                // covers the Unlock button and it cannot be tapped.
                .safeDrawingPadding()
                .imePadding()
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            MvpCard(Modifier.widthIn(max = 420.dp).fillMaxWidth()) {
                MvpTitle("Unlock Mega Video Protect")
                MvpDescription("Enter the vault password to unlock the encrypted database.")
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
                MvpError(error)
                if (busy) {
                    // Argon2id takes a few seconds even on the mobile profile —
                    // without this the Unlock button just looks dead.
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp)
                        Text(
                            if (checkingPc) {
                                "Checking the password with the PC…"
                            } else {
                                "Unlocking — deriving the key from your password…"
                            },
                            color = Mvp.description,
                            fontSize = 12.sp,
                        )
                    }
                }
                MvpButton("Unlock vault", onClick = { unlock() }, primary = true, enabled = !busy, modifier = Modifier.fillMaxWidth())
                MvpDescription("Or use the vault shared from your PC. Scan the code, enter that vault's password, then tap Connect. The PC can be locked after you sign in.")
                MvpButton(
                    "Scan PC",
                    onClick = { cameraPermission.launch(Manifest.permission.CAMERA) },
                    enabled = !busy,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpInput(
                    value = pcCode,
                    onValueChange = { pcCode = it },
                    placeholder = "Or paste the code from the PC",
                    enabled = !busy,
                )
                if (pcHint != null) MvpDescription(pcHint!!)
                MvpButton(
                    "Connect",
                    onClick = { connectPc() },
                    enabled = !busy && pcCode.isNotBlank() && password.isNotBlank(),
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpButton("Back", onClick = onBack, enabled = !busy, modifier = Modifier.fillMaxWidth())
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
