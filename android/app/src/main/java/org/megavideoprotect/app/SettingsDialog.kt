package org.megavideoprotect.app

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Mirrors the desktop settings dialog: change password + tag management. */
@Composable
fun SettingsDialog(
    currentTags: List<TagEntry>,
    onDismiss: () -> Unit,
    onChanged: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    var current by remember { mutableStateOf("") }
    var newPass by remember { mutableStateOf("") }
    var confirm by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var ok by remember { mutableStateOf<String?>(null) }
    var newTag by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var tagError by remember { mutableStateOf<String?>(null) }

    fun changePassword() {
        if (newPass.length < 8) { error = "New password must be at least 8 characters."; return }
        if (newPass != confirm) { error = "New passwords do not match."; return }
        error = null; ok = null; busy = true
        scope.launch {
            val res = withContext(Dispatchers.Default) {
                CoreBridge.Result.parse(CoreBridge.nativeChangePassword(current, newPass, 256 * 1024, 3, 1))
            }
            busy = false
            if (res.ok) {
                ok = "Password changed."
                current = ""; newPass = ""; confirm = ""
            } else error = res.error.ifBlank { res.detail }
        }
    }

    fun addTag() {
        if (newTag.isBlank()) return
        tagError = null
        scope.launch {
            val res = withContext(Dispatchers.Default) {
                CoreBridge.Result.parse(CoreBridge.nativeAddTag(-1, newTag.trim()))
            }
            if (res.ok) { newTag = ""; onChanged() } else tagError = res.error
        }
    }

    fun deleteTag(id: Long) {
        scope.launch {
            withContext(Dispatchers.Default) { CoreBridge.Result.parse(CoreBridge.nativeDeleteTag(id)) }
            onChanged()
        }
    }

    Dialog(onDismissRequest = onDismiss) {
        Box(
            Modifier
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
        ) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Settings")
                MvpDescription("Change the vault password. The vault stays unlocked; all keys are re-wrapped.")
                MvpInput(value = current, onValueChange = { current = it }, placeholder = "Current password", isPassword = true)
                MvpInput(value = newPass, onValueChange = { newPass = it }, placeholder = "New password", isPassword = true)
                MvpInput(value = confirm, onValueChange = { confirm = it }, placeholder = "Confirm new password", isPassword = true)
                MvpError(error)
                ok?.let { Text(it, color = Mvp.sliderFill, fontSize = 13.sp) }
                MvpButton("Change password", onClick = { changePassword() }, primary = true, enabled = !busy, modifier = Modifier.fillMaxWidth())

                Spacer(Modifier.height(6.dp))
                MvpDescription("Tags")
                Row(verticalAlignment = Alignment.CenterVertically) {
                    MvpInput(value = newTag, onValueChange = { newTag = it }, placeholder = "New tag name", modifier = Modifier.weight(1f))
                    Spacer(Modifier.width(8.dp))
                    MvpButton("Add", onClick = { addTag() }, enabled = !busy)
                }
                MvpError(tagError)
                currentTags.forEach { t ->
                    Row(
                        Modifier.fillMaxWidth().padding(vertical = 2.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        Text("${t.name}  (${t.videoCount})", color = Mvp.text, fontSize = 13.sp)
                        MvpButton("Delete", onClick = { deleteTag(t.id) }, enabled = !busy)
                    }
                }
                Spacer(Modifier.height(4.dp))
                MvpButton("Close", onClick = onDismiss, modifier = Modifier.fillMaxWidth())
            }
        }
    }
}
