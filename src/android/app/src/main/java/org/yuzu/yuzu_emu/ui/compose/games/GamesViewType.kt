// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.games

/** How the library lists games, and the preference keys that remember the choice. */
object GamesViewType {
    const val GRID = 0
    const val GRID_COMPACT = 1
    const val LIST = 2
    const val CAROUSEL = 3

    const val PREF_PORTRAIT = "GamesViewTypePortrait"
    const val PREF_LANDSCAPE = "GamesViewTypeLandscape"
}
