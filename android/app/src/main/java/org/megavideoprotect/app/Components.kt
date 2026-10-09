package org.megavideoprotect.app

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Card replicating the Qt #card frame (bg #171c25, 1px #2b3444, radius 16). */
@Composable
fun MvpCard(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier = modifier
            .background(Mvp.card, RoundedCornerShape(16.dp))
            .border(1.dp, Mvp.cardBorder, RoundedCornerShape(16.dp))
            .padding(horizontal = 28.dp, vertical = 26.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
        content = content
    )
}

/** Page title: #f3f6fa, 25sp, weight 600 (QLabel#pageTitle). */
@Composable
fun MvpTitle(text: String) {
    Text(text, color = Mvp.title, fontSize = 25.sp, fontWeight = FontWeight.SemiBold)
}

/** Description: #9eabba, 13sp (QLabel#description). */
@Composable
fun MvpDescription(text: String) {
    Text(text, color = Mvp.description, fontSize = 13.sp)
}

/** Input replicating QLineEdit (bg #10151d, border #364154, radius 8, focus #6f8cff). */
@Composable
fun MvpInput(
    value: String,
    onValueChange: (String) -> Unit,
    placeholder: String,
    isPassword: Boolean = false,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        modifier = modifier.fillMaxWidth(),
        placeholder = { Text(placeholder, color = Mvp.placeholder, fontSize = 14.sp) },
        visualTransformation = if (isPassword) PasswordVisualTransformation() else VisualTransformation.None,
        singleLine = true,
        enabled = enabled,
        textStyle = TextStyle(color = Mvp.title, fontSize = 14.sp),
        keyboardOptions = KeyboardOptions.Default,
        shape = RoundedCornerShape(8.dp),
        colors = OutlinedTextFieldDefaults.colors(
            focusedTextColor = Mvp.title,
            unfocusedTextColor = Mvp.title,
            disabledTextColor = Mvp.title.copy(alpha = 0.6f),
            cursorColor = Mvp.accent,
            focusedBorderColor = Mvp.inputFocusBorder,
            unfocusedBorderColor = Mvp.inputBorder,
            disabledBorderColor = Mvp.inputBorder.copy(alpha = 0.6f),
            focusedContainerColor = Mvp.inputBg,
            unfocusedContainerColor = Mvp.inputBg,
            disabledContainerColor = Mvp.inputBg,
            focusedPlaceholderColor = Mvp.placeholder,
            unfocusedPlaceholderColor = Mvp.placeholder,
            disabledPlaceholderColor = Mvp.placeholder,
        ),
    )
}

/** Button replicating QPushButton (secondary: #252d3b/#3a465a; primary: #536fe8). */
@Composable
fun MvpButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    primary: Boolean = false,
    enabled: Boolean = true,
) {
    val bg = when {
        !enabled -> Mvp.buttonDisabled
        primary -> Mvp.primary
        else -> Mvp.button
    }
    val border = when {
        !enabled -> Mvp.buttonDisabled
        primary -> Mvp.primaryBorder
        else -> Mvp.buttonBorder
    }
    val fg = when {
        !enabled -> Mvp.buttonDisabledText
        primary -> Color.White
        else -> Mvp.title
    }
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.height(39.dp),
        shape = RoundedCornerShape(8.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = bg,
            contentColor = fg,
            disabledContainerColor = Mvp.buttonDisabled,
            disabledContentColor = Mvp.buttonDisabledText,
        ),
        border = BorderStroke(1.dp, border),
        contentPadding = PaddingValues(horizontal = 16.dp),
    ) {
        Text(text, fontSize = 14.sp, fontWeight = if (primary) FontWeight.SemiBold else FontWeight.Normal)
    }
}

/** Error banner replicating QLabel#errorLabel (#ff9a9a on #321d24, 1px #6a303a, radius 7). */
@Composable
fun MvpError(text: String?, modifier: Modifier = Modifier) {
    if (text.isNullOrBlank()) return
    Text(
        text,
        color = Mvp.errorText,
        fontSize = 13.sp,
        modifier = modifier
            .fillMaxWidth()
            .background(Mvp.errorBg, RoundedCornerShape(7.dp))
            .border(1.dp, Mvp.errorBorder, RoundedCornerShape(7.dp))
            .padding(9.dp),
    )
}

/** Dropdown replicating QComboBox (bg #10151d, border #364154, radius 8). */
@Composable
fun MvpCombo(
    selected: String,
    items: List<String>,
    onSelect: (String) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    var open by remember { mutableStateOf(false) }
    Box(modifier) {
        MvpButton(text = selected, onClick = { open = true }, enabled = enabled)
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            items.forEach { item ->
                DropdownMenuItem(
                    text = { Text(item, color = Mvp.text, fontSize = 13.sp) },
                    onClick = { open = false; onSelect(item) },
                )
            }
        }
    }
}

internal const val ChooseFolder = "Choose a folder…"

/** App-private and shared storage, with a folder picker as the other mode. */
@Composable
fun VaultPlaceField(
    location: String,
    options: List<String>,
    enabled: Boolean,
    onLocation: (String) -> Unit,
    onChooseFolder: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val standard = options.distinct()
    MvpCombo(
        selected = VaultLocation.shortName(context, location),
        items = standard.map { VaultLocation.shortName(context, it) } + ChooseFolder,
        onSelect = { label ->
            if (label == ChooseFolder) {
                onChooseFolder()
            } else {
                val index = standard.indexOfFirst { VaultLocation.shortName(context, it) == label }
                if (index >= 0) onLocation(standard[index])
            }
        },
        enabled = enabled,
        modifier = modifier.fillMaxWidth(),
    )
}

/** The scan mark, pinned to the top-right of the screen. */
@Composable
fun BoxScope.ScanCorner(enabled: Boolean, onClick: () -> Unit) {
    IconButton(
        onClick = onClick,
        enabled = enabled,
        modifier = Modifier
            .align(Alignment.TopEnd)
            .safeDrawingPadding()
            .padding(end = 4.dp),
    ) {
        Icon(
            painter = painterResource(R.drawable.ic_scan_qr),
            contentDescription = "Scan",
            tint = Mvp.title,
            modifier = Modifier.size(26.dp),
        )
    }
}

/** One toolbar action row (like the Qt gallery toolbar). */
@Composable
fun RowScope.ToolbarSpacer(weight: Float = 1f) {
    Spacer(Modifier.weight(weight))
}

@Composable
fun RowScope.ToolbarButton(
    text: String,
    onClick: () -> Unit,
    primary: Boolean = false,
) {
    Spacer(Modifier.width(4.dp))
    MvpButton(text = text, onClick = onClick, primary = primary, modifier = Modifier.weight(1f))
}
