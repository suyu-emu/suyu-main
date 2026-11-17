// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

object SuyuColors {
    val Background = Color(0xFF0B0D12)
    val Surface = Color(0xFF12151C)
    val SurfaceHigh = Color(0xFF1A1E28)
    val Outline = Color(0xFF2A2F3C)
    val Text = Color(0xFFECEFF6)
    val TextSoft = Color(0xFFC5CCDA)
    val Muted = Color(0xFF9AA3B5)
    val Accent = Color(0xFF5EEAD4)
    val OnAccent = Color(0xFF04211D)
    val Violet = Color(0xFFA78BFA)
    val Heart = Color(0xFFFF8FA3)
    val CompatPerfect = Color(0xFF5EEAD4)
    val CompatGreat = Color(0xFF93C5FD)
    val CompatPlayable = Color(0xFFFCD34D)
}

private val SuyuDarkScheme = darkColorScheme(
    primary = SuyuColors.Accent,
    onPrimary = SuyuColors.OnAccent,
    secondary = SuyuColors.Violet,
    background = SuyuColors.Background,
    onBackground = SuyuColors.Text,
    surface = SuyuColors.Surface,
    onSurface = SuyuColors.Text,
    surfaceVariant = SuyuColors.SurfaceHigh,
    onSurfaceVariant = SuyuColors.Muted,
    outline = SuyuColors.Outline
)

@Composable
fun SuyuTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = SuyuDarkScheme, content = content)
}
