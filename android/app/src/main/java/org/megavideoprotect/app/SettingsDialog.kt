package org.megavideoprotect.app

import android.content.Intent
import android.graphics.drawable.ColorDrawable
import android.net.Uri
import android.os.Build
import android.view.WindowManager
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private val Sheet = Color(0xFF121214)
private val Group = Color(0xFF1C1C1E)
private val Line = Color(0x14FFFFFF)

/** Phone settings. The library behind the sheet is blurred; the sheet itself is solid. */
@Composable
fun SettingsDialog(
    currentTags: List<TagEntry>,
    remote: Boolean = false,
    onDismiss: () -> Unit,
    onChanged: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var idleLabel by remember { mutableStateOf(VaultLocation.idleLockLabel(context)) }
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
                CoreBridge.Result.parse(CoreBridge.nativeCreateTag(newTag.trim()))
            }
            if (res.ok) { newTag = ""; onChanged() } else tagError = res.error.ifBlank { res.detail }
        }
    }

    fun deleteTag(id: Long) {
        scope.launch {
            withContext(Dispatchers.Default) { CoreBridge.Result.parse(CoreBridge.nativeDeleteTag(id)) }
            onChanged()
        }
    }

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false),
    ) {
        val view = LocalView.current
        SideEffect {
            val window = (view.parent as? DialogWindowProvider)?.window ?: return@SideEffect
            window.setDimAmount(0.55f)
            window.setBackgroundDrawable(ColorDrawable(android.graphics.Color.TRANSPARENT))
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                window.addFlags(WindowManager.LayoutParams.FLAG_BLUR_BEHIND)
                window.setBackgroundBlurRadius(70)
            }
        }
        Box(Modifier.fillMaxSize()) {
            Box(
                Modifier
                    .fillMaxSize()
                    .clickable(
                        interactionSource = remember { MutableInteractionSource() },
                        indication = null,
                        onClick = onDismiss,
                    ),
            )
            Column(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .imePadding()
                    .clip(RoundedCornerShape(topStart = 22.dp, topEnd = 22.dp))
                    .background(Sheet)
                    .clickable(
                        interactionSource = remember { MutableInteractionSource() },
                        indication = null,
                    ) {}
                    .navigationBarsPadding(),
            ) {
                Box(
                    Modifier
                        .align(Alignment.CenterHorizontally)
                        .padding(top = 8.dp)
                        .size(width = 36.dp, height = 4.dp)
                        .clip(RoundedCornerShape(2.dp))
                        .background(Color.White.copy(alpha = 0.22f)),
                )
                Row(
                    Modifier
                        .fillMaxWidth()
                        .padding(start = 20.dp, end = 12.dp, top = 6.dp, bottom = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("Settings", color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
                    Spacer(Modifier.weight(1f))
                    Text(
                        "Done",
                        color = Mvp.accent,
                        fontSize = 16.sp,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier
                            .clickable(onClick = onDismiss)
                            .padding(horizontal = 8.dp, vertical = 8.dp),
                    )
                }
                Column(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(max = 560.dp)
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = 16.dp)
                        .padding(bottom = 20.dp),
                    verticalArrangement = Arrangement.spacedBy(18.dp),
                ) {
                    if (remote) {
                        Text(
                            "This phone is using the PC vault. Password and tags stay on the PC.",
                            color = Mvp.description,
                            fontSize = 13.sp,
                        )
                    }

                    SettingsBlock(title = "Lock after leaving") {
                        VaultLocation.idleChoices.forEachIndexed { index, (ms, label) ->
                            if (index > 0) Box(Modifier.fillMaxWidth().height(1.dp).background(Line))
                            Row(
                                Modifier
                                    .fillMaxWidth()
                                    .clickable {
                                        VaultLocation.saveIdleLock(context, ms)
                                        idleLabel = label
                                    }
                                    .padding(horizontal = 16.dp, vertical = 14.dp),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Text(label, color = Color.White, fontSize = 16.sp, modifier = Modifier.weight(1f))
                                if (idleLabel == label) {
                                    Text("✓", color = Mvp.accent, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
                                }
                            }
                        }
                    }

                    if (!remote) {
                        SettingsBlock(title = "Password") {
                            SettingsField(current, { current = it }, "Current password", password = true)
                            Box(Modifier.fillMaxWidth().height(1.dp).background(Line))
                            SettingsField(newPass, { newPass = it }, "New password", password = true)
                            Box(Modifier.fillMaxWidth().height(1.dp).background(Line))
                            SettingsField(confirm, { confirm = it }, "Confirm new password", password = true)
                            if (!error.isNullOrBlank() || !ok.isNullOrBlank()) {
                                Box(Modifier.fillMaxWidth().height(1.dp).background(Line))
                                Text(
                                    error ?: ok.orEmpty(),
                                    color = if (error != null) Mvp.errorText else Mvp.accent,
                                    fontSize = 13.sp,
                                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 10.dp),
                                )
                            }
                            Box(Modifier.fillMaxWidth().height(1.dp).background(Line))
                            Box(
                                Modifier
                                    .fillMaxWidth()
                                    .clickable(enabled = !busy) { changePassword() }
                                    .padding(vertical = 14.dp),
                                contentAlignment = Alignment.Center,
                            ) {
                                Text(
                                    if (busy) "Changing…" else "Change password",
                                    color = if (busy) Mvp.placeholder else Mvp.accent,
                                    fontSize = 16.sp,
                                    fontWeight = FontWeight.SemiBold,
                                )
                            }
                        }

                        SettingsBlock(title = "Tags") {
                            Column(
                                Modifier
                                    .fillMaxWidth()
                                    .heightIn(max = 220.dp)
                                    .verticalScroll(rememberScrollState()),
                            ) {
                                if (currentTags.isEmpty()) {
                                    Text(
                                        "No tags yet",
                                        color = Mvp.description,
                                        fontSize = 15.sp,
                                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 14.dp),
                                    )
                                }
                                currentTags.forEachIndexed { index, tag ->
                                    if (index > 0) Box(Modifier.fillMaxWidth().height(1.dp).background(Line))
                                    Row(
                                        Modifier
                                            .fillMaxWidth()
                                            .padding(start = 16.dp, end = 8.dp),
                                        verticalAlignment = Alignment.CenterVertically,
                                    ) {
                                        Text(tag.name, color = Color.White, fontSize = 16.sp, modifier = Modifier.weight(1f))
                                        Text("${tag.videoCount}", color = Mvp.description, fontSize = 14.sp)
                                        Spacer(Modifier.width(4.dp))
                                        Text(
                                            "Remove",
                                            color = Mvp.destructive,
                                            fontSize = 14.sp,
                                            modifier = Modifier
                                                .clickable(enabled = !busy) { deleteTag(tag.id) }
                                                .padding(horizontal = 8.dp, vertical = 14.dp),
                                        )
                                    }
                                }
                            }
                            if (!tagError.isNullOrBlank()) {
                                Box(Modifier.fillMaxWidth().height(1.dp).background(Line))
                                Text(
                                    tagError.orEmpty(),
                                    color = Mvp.errorText,
                                    fontSize = 13.sp,
                                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 10.dp),
                                )
                            }
                            Box(Modifier.fillMaxWidth().height(1.dp).background(Line))
                            Row(
                                Modifier.fillMaxWidth(),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Box(Modifier.weight(1f)) {
                                    SettingsField(newTag, { newTag = it }, "New tag")
                                }
                                Text(
                                    "Add",
                                    color = if (newTag.isBlank() || busy) Mvp.placeholder else Mvp.accent,
                                    fontSize = 16.sp,
                                    fontWeight = FontWeight.SemiBold,
                                    modifier = Modifier
                                        .clickable(enabled = newTag.isNotBlank() && !busy) { addTag() }
                                        .padding(horizontal = 16.dp, vertical = 14.dp),
                                )
                            }
                        }
                    }

                    Text(
                        "github.com/XCisCoX/Mega-Vault-Protector",
                        color = Mvp.description,
                        fontSize = 12.sp,
                        textAlign = TextAlign.Center,
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable {
                                context.startActivity(
                                    Intent(
                                        Intent.ACTION_VIEW,
                                        Uri.parse("https://github.com/XCisCoX/Mega-Vault-Protector"),
                                    )
                                )
                            }
                            .padding(top = 4.dp, bottom = 8.dp),
                    )
                }
            }
        }
    }
}

@Composable
private fun SettingsBlock(title: String, content: @Composable () -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(
            title,
            color = Mvp.description,
            fontSize = 13.sp,
            fontWeight = FontWeight.Medium,
            modifier = Modifier.padding(start = 4.dp),
        )
        Column(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(14.dp))
                .background(Group),
        ) {
            content()
        }
    }
}

@Composable
private fun SettingsField(
    value: String,
    onValueChange: (String) -> Unit,
    placeholder: String,
    password: Boolean = false,
) {
    BasicTextField(
        value = value,
        onValueChange = onValueChange,
        singleLine = true,
        textStyle = TextStyle(color = Color.White, fontSize = 16.sp),
        visualTransformation = if (password) PasswordVisualTransformation() else VisualTransformation.None,
        cursorBrush = SolidColor(Mvp.accent),
        modifier = Modifier.fillMaxWidth(),
        decorationBox = { inner ->
            Box(
                Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 14.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                if (value.isEmpty()) {
                    Text(placeholder, color = Mvp.placeholder, fontSize = 16.sp)
                }
                inner()
            }
        },
    )
}
