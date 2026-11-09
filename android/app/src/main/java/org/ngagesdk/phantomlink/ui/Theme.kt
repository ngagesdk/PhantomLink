/*
 * Colours taken from the PhantomLink logo: purple, lime, black and white.
 *
 * SPDX-License-Identifier: MIT
 */
package org.ngagesdk.phantomlink.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import org.ngagesdk.phantomlink.ThemeMode

private val Light = lightColorScheme(
    primary = Color(0xFF8A12D6),
    onPrimary = Color.White,
    primaryContainer = Color(0xFFF1E3FF),
    onPrimaryContainer = Color(0xFF2E0050),
    secondary = Color(0xFF4E6B00),
    onSecondary = Color.White,
    secondaryContainer = Color(0xFFE8FBB4),
    onSecondaryContainer = Color(0xFF162100),
    background = Color(0xFFFAF8FD),
    onBackground = Color(0xFF1C1A1F),
    surface = Color(0xFFFAF8FD),
    onSurface = Color(0xFF1C1A1F),
    surfaceContainerLowest = Color.White,
    surfaceContainerLow = Color.White,
    surfaceContainer = Color(0xFFF4F0F8),
    surfaceContainerHigh = Color(0xFFEFEAF4),
    surfaceVariant = Color(0xFFEDE7F3),
    onSurfaceVariant = Color(0xFF5D5866),
    outline = Color(0xFF8E8796),
    outlineVariant = Color(0xFFE2DCE8),
    error = Color(0xFFBA1A1A),
)

private val Dark = darkColorScheme(
    primary = Color(0xFFD7A6FF),
    onPrimary = Color(0xFF3D0068),
    primaryContainer = Color(0xFF5A1194),
    onPrimaryContainer = Color(0xFFF1E3FF),
    secondary = Color(0xFFB9EE3A),
    onSecondary = Color(0xFF243500),
    secondaryContainer = Color(0xFF3A4F00),
    onSecondaryContainer = Color(0xFFE8FBB4),
    background = Color(0xFF131016),
    onBackground = Color(0xFFECE6F0),
    surface = Color(0xFF131016),
    onSurface = Color(0xFFECE6F0),
    surfaceContainerLowest = Color(0xFF0E0C10),
    surfaceContainerLow = Color(0xFF1C1820),
    surfaceContainer = Color(0xFF211D25),
    surfaceContainerHigh = Color(0xFF2B2630),
    surfaceVariant = Color(0xFF2B2630),
    onSurfaceVariant = Color(0xFFCBC3D3),
    outline = Color(0xFF958E9D),
    outlineVariant = Color(0xFF3A3540),
    error = Color(0xFFFFB4AB),
)

/** Status dot colours. */
@Immutable
data class StatusColors(val ok: Color, val busy: Color, val off: Color, val bad: Color)

private val LightStatus = StatusColors(ok = Color(0xFF5E9900), busy = Color(0xFFE09A00), off = Color(0xFFB9B2C1), bad = Color(0xFFD32F2F))
private val DarkStatus = StatusColors(ok = Color(0xFFB9EE3A), busy = Color(0xFFFFC94A), off = Color(0xFF5D5866), bad = Color(0xFFFF8A80))

val LocalStatusColors = staticCompositionLocalOf { LightStatus }

@Composable
fun isDark(mode: ThemeMode): Boolean = when (mode) {
    ThemeMode.LIGHT -> false
    ThemeMode.DARK -> true
    ThemeMode.SYSTEM -> isSystemInDarkTheme()
}

@Composable
fun PhantomTheme(dark: Boolean, content: @Composable () -> Unit) {
    androidx.compose.runtime.CompositionLocalProvider(LocalStatusColors provides if (dark) DarkStatus else LightStatus) {
        MaterialTheme(
            colorScheme = if (dark) Dark else Light,
            shapes = Shapes(
                small = RoundedCornerShape(10.dp),
                medium = RoundedCornerShape(16.dp),
                large = RoundedCornerShape(24.dp),
            ),
            content = content,
        )
    }
}
