package org.megavideoprotect.app

import androidx.compose.material3.darkColorScheme
import androidx.compose.ui.graphics.Color

/**
 * Telegram night glass: near-black field, frosted panels, blue actions.
 * Panels are a light veil with a hairline, the same idea as Telegram's
 * blurred bars, drawn as glass because the phone UI is Compose.
 */
object Mvp {
    val window = Color(0xFF000000)
    val card = Color(0xF21C1C1E)
    val cardBorder = Color(0x24FFFFFF)
    val glass = Color(0x14FFFFFF)
    val glassStroke = Color(0x2EFFFFFF)
    val inputBg = Color(0xFF141416)
    val inputBorder = Color(0x1FFFFFFF)
    val inputFocusBorder = Color(0xFF3390EC)
    val title = Color(0xFFFFFFFF)
    val description = Color(0xFF8E8E93)
    val text = Color(0xFFF2F2F7)
    val placeholder = Color(0xFF6D6D72)
    val button = Color(0x1AFFFFFF)
    val buttonBorder = Color(0x24FFFFFF)
    val buttonHover = Color(0x26FFFFFF)
    val buttonDisabled = Color(0xFF1C1C1E)
    val buttonDisabledText = Color(0xFF636366)
    val primary = Color(0xFF3390EC)
    val primaryBorder = Color(0xFF3390EC)
    val primaryHover = Color(0xFF4BA0F5)
    val accent = Color(0xFF3390EC)
    val selection = Color(0x663390EC)
    val itemHover = Color(0xFF1C1C1E)
    val alternateRow = Color(0xFF101012)
    val headerBg = Color(0xE6101012)
    val headerText = Color(0xFF8E8E93)
    val errorText = Color(0xFFFF6B6B)
    val errorBg = Color(0xFF2C1518)
    val errorBorder = Color(0xFF5C2A30)
    val sliderGroove = Color(0x33FFFFFF)
    val sliderFill = Color(0xFF3390EC)
    val sliderHandle = Color(0xFFFFFFFF)
    val sliderHandleBorder = Color(0xFF3390EC)
    val scrollbarHandle = Color(0xFF3A3A3C)
    val statusText = Color(0xFF8E8E93)
    val destructive = Color(0xFFFF453A)
}

val MvpColorScheme = darkColorScheme(
    primary = Mvp.primary,
    onPrimary = Color.White,
    background = Mvp.window,
    onBackground = Mvp.title,
    surface = Mvp.card,
    onSurface = Mvp.text,
    surfaceVariant = Mvp.inputBg,
    onSurfaceVariant = Mvp.description,
    outline = Mvp.cardBorder,
    outlineVariant = Mvp.inputBorder,
    error = Mvp.errorText,
    onError = Color.White,
    errorContainer = Mvp.errorBg,
    onErrorContainer = Mvp.errorText,
)
