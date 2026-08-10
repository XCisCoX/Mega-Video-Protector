package org.megavideoprotect.app

import androidx.compose.material3.darkColorScheme
import androidx.compose.ui.graphics.Color

/**
 * Exact color tokens from the desktop Qt theme (qt-app/.../theme.hpp):
 * window #0c0f15, cards #171c25 (border #2b3444, radius 16), inputs #10151d
 * (border #364154, focus #6f8cff), primary button #536fe8 (hover #627cf0,
 * border #6f88f4), accent #5b7cfa, selection #2b3a63, error #ff9a9a on
 * #321d24 with border #6a303a.
 */
object Mvp {
    val window = Color(0xFF0C0F15)
    val card = Color(0xFF171C25)
    val cardBorder = Color(0xFF2B3444)
    val inputBg = Color(0xFF10151D)
    val inputBorder = Color(0xFF364154)
    val inputFocusBorder = Color(0xFF6F8CFF)
    val title = Color(0xFFF3F6FA)
    val description = Color(0xFF9EABBA)
    val text = Color(0xFFE2E7EF)
    val placeholder = Color(0xFF707C8E)
    val button = Color(0xFF252D3B)
    val buttonBorder = Color(0xFF3A465A)
    val buttonHover = Color(0xFF303A4B)
    val buttonDisabled = Color(0xFF1D232D)
    val buttonDisabledText = Color(0xFF697587)
    val primary = Color(0xFF536FE8)
    val primaryBorder = Color(0xFF6F88F4)
    val primaryHover = Color(0xFF627CF0)
    val accent = Color(0xFF5B7CFA)
    val selection = Color(0xFF2B3A63)
    val itemHover = Color(0xFF232C3D)
    val alternateRow = Color(0xFF161C27)
    val headerBg = Color(0xFF1A202C)
    val headerText = Color(0xFFAAB6C8)
    val errorText = Color(0xFFFF9A9A)
    val errorBg = Color(0xFF321D24)
    val errorBorder = Color(0xFF6A303A)
    val sliderGroove = Color(0xFF2B3444)
    val sliderFill = Color(0xFF536FE8)
    val sliderHandle = Color(0xFF7F97F5)
    val sliderHandleBorder = Color(0xFF9DB1F8)
    val scrollbarHandle = Color(0xFF3A465A)
    val statusText = Color(0xFF666666)
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
