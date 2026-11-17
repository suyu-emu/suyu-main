// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.unit.dp

/** Stroke icons on a 24x24 grid, tinted by the Icon composable. */
object SuyuIcons {
    private fun icon(name: String, vararg paths: String, filled: Boolean = false): ImageVector {
        val builder = ImageVector.Builder(
            name = name,
            defaultWidth = 24.dp,
            defaultHeight = 24.dp,
            viewportWidth = 24f,
            viewportHeight = 24f
        )
        paths.forEach { d ->
            val nodes = PathParser().parsePathString(d).toNodes()
            if (filled) {
                builder.addPath(pathData = nodes, fill = SolidColor(Color.Black))
            } else {
                builder.addPath(
                    pathData = nodes,
                    stroke = SolidColor(Color.Black),
                    strokeLineWidth = 2f,
                    strokeLineCap = StrokeCap.Round,
                    strokeLineJoin = StrokeJoin.Round
                )
            }
        }
        return builder.build()
    }

    private const val HEART = "M12 20s-7-4.4-7-10a4 4 0 0 1 7-2.5A4 4 0 0 1 19 10c0 5.6-7 10-7 10Z"

    val Library = icon(
        "library",
        "M6 4h3a2 2 0 0 1 2 2v3a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2Z",
        "M15 4h3a2 2 0 0 1 2 2v3a2 2 0 0 1-2 2h-3a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2Z",
        "M6 13h3a2 2 0 0 1 2 2v3a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2v-3a2 2 0 0 1 2-2Z",
        "M15 13h3a2 2 0 0 1 2 2v3a2 2 0 0 1-2 2h-3a2 2 0 0 1-2-2v-3a2 2 0 0 1 2-2Z"
    )
    val Controls = icon(
        "controls",
        "M8 7h8a5 5 0 0 1 5 5v1a5 5 0 0 1-5 5H8a5 5 0 0 1-5-5v-1a5 5 0 0 1 5-5Z",
        "M8 10v5M5.5 12.5h5",
        "M15.5 11.5h.01M17.5 14h.01"
    )
    val Drivers = icon(
        "drivers",
        "M8 6h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2Z",
        "M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"
    )
    val Settings = icon(
        "settings",
        "M4 7h10M18 7h2M4 17h2M10 17h10",
        "M14 7a2 2 0 1 0 4 0a2 2 0 1 0 -4 0",
        "M6 17a2 2 0 1 0 4 0a2 2 0 1 0 -4 0"
    )
    val Search = icon("search", "M4 11a7 7 0 1 0 14 0a7 7 0 1 0 -14 0", "M20 20l-3.5-3.5")
    val Chevron = icon("chevron", "M9 5l7 7-7 7")
    val Close = icon("close", "M6 6l12 12M18 6L6 18")
    val Add = icon("add", "M12 5v14M5 12h14")
    val Play = icon("play", "M8 5.5v13l11-6.5z", filled = true)
    val Heart = icon("heart", HEART)
    val HeartFilled = icon("heartFilled", HEART, filled = true)
}
