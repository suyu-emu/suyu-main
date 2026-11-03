// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Installable
import org.yuzu.yuzu_emu.ui.compose.common.SuyuIconButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuRow
import org.yuzu.yuzu_emu.ui.compose.common.SuyuScreen
import org.yuzu.yuzu_emu.ui.compose.common.contentPadding

@Composable
fun InstallablesScreen(installables: List<Installable>, onBack: () -> Unit) {
    SuyuScreen(title = stringResource(R.string.manage_yuzu_data), onBack = onBack) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 420.dp),
            modifier = Modifier.fillMaxSize(),
            contentPadding = contentPadding(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            items(installables) { item ->
                SuyuRow(
                    title = stringResource(item.titleId),
                    description = stringResource(item.descriptionId),
                    trailing = {
                        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            item.install?.let {
                                SuyuIconButton(item.installIconId, stringResource(item.installLabelId), it, size = 44.dp)
                            }
                            item.export?.let {
                                SuyuIconButton(R.drawable.ic_export, stringResource(R.string.export), it, size = 44.dp)
                            }
                        }
                    }
                )
            }
        }
    }
}
