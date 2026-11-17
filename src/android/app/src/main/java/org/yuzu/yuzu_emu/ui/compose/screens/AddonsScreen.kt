// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Patch
import org.yuzu.yuzu_emu.ui.compose.common.SuyuFab
import org.yuzu.yuzu_emu.ui.compose.common.SuyuIconButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuRow
import org.yuzu.yuzu_emu.ui.compose.common.SuyuScreen
import org.yuzu.yuzu_emu.ui.compose.common.contentPadding
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors

@Composable
fun AddonsScreen(
    title: String,
    addons: List<Patch>,
    onBack: () -> Unit,
    onToggle: (Patch, Boolean) -> Unit,
    onDelete: (Patch) -> Unit,
    onInstall: () -> Unit
) {
    // Patch.enabled is a plain field, so toggling bumps this to redraw the switches.
    var revision by remember { mutableIntStateOf(0) }

    SuyuScreen(
        title = title,
        onBack = onBack,
        fab = { SuyuFab(R.drawable.ic_add, stringResource(R.string.install), onInstall) }
    ) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 420.dp),
            modifier = Modifier.fillMaxSize(),
            contentPadding = contentPadding(extraBottom = 80.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            items(addons) { patch ->
                val enabled = revision.let { patch.enabled }
                SuyuRow(
                    title = patch.name,
                    description = patch.version.ifEmpty { null },
                    onClick = {
                        onToggle(patch, !enabled)
                        revision++
                    },
                    trailing = {
                        Box(modifier = Modifier.alpha(if (patch.isRemovable) 1f else 0.38f)) {
                            SuyuIconButton(
                                R.drawable.ic_delete,
                                stringResource(R.string.delete),
                                { onDelete(patch) },
                                size = 44.dp,
                                enabled = patch.isRemovable
                            )
                        }
                        Box(
                            modifier = Modifier
                                .padding(start = 8.dp)
                                .size(width = 50.dp, height = 28.dp)
                                .clip(CircleShape)
                                .background(if (enabled) SuyuColors.Accent else androidx.compose.ui.graphics.Color(0xFF3A4152)),
                            contentAlignment = if (enabled) Alignment.CenterEnd else Alignment.CenterStart
                        ) {
                            Box(
                                modifier = Modifier
                                    .padding(4.dp)
                                    .size(20.dp)
                                    .clip(CircleShape)
                                    .background(if (enabled) SuyuColors.OnAccent else SuyuColors.Muted)
                            )
                        }
                    }
                )
            }
        }
    }
}
