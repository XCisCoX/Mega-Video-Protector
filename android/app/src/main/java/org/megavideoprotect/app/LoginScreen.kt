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
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Mirrors the desktop login page: "Unlock Mega Video Protect". */
@Composable
fun LoginScreen(
    vaultLocation: String,
    onUnlocked: () -> Unit,
    onBack: () -> Unit,
) {
    var password by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()

    fun unlock() {
        if (password.isEmpty()) {
            error = "Enter the vault password."
            return
        }
        error = null
        busy = true
        scope.launch {
            val res = withContext(Dispatchers.Default) {
                CoreBridge.Result.parse(CoreBridge.nativeOpenVault(vaultLocation, password))
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
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            MvpCard(Modifier.widthIn(max = 420.dp).fillMaxWidth()) {
                MvpTitle("Unlock Mega Video Protect")
                MvpDescription("Enter the vault password to unlock the encrypted database.")
                MvpInput(
                    value = vaultLocation,
                    onValueChange = {},
                    placeholder = "Vault storage folder",
                    enabled = false,
                )
                MvpInput(value = password, onValueChange = { password = it }, placeholder = "Password", isPassword = true)
                MvpError(error)
                MvpButton("Unlock vault", onClick = { unlock() }, primary = true, enabled = !busy, modifier = Modifier.fillMaxWidth())
                MvpButton("Back", onClick = onBack, enabled = !busy, modifier = Modifier.fillMaxWidth())
            }
        }
    }
}
