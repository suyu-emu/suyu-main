// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.padding
import androidx.compose.ui.draw.clip
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Driver
import org.yuzu.yuzu_emu.ui.compose.common.SuyuButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuFab
import org.yuzu.yuzu_emu.ui.compose.common.SuyuIconButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuRow
import org.yuzu.yuzu_emu.ui.compose.common.SuyuScreen
import org.yuzu.yuzu_emu.ui.compose.common.contentPadding
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors

@Composable
fun DriverManagerScreen(
    drivers: List<Driver>,
    systemDriverTitle: String,
    showUseGlobal: Boolean,
    onBack: () -> Unit,
    onUseGlobal: () -> Unit,
    onSelect: (Int) -> Unit,
    onRemove: (Int) -> Unit,
    onInstall: () -> Unit,
    onFetch: () -> Unit
) {
    SuyuScreen(
        title = stringResource(R.string.gpu_driver_manager),
        onBack = onBack,
        actions = {
            if (showUseGlobal) {
                SuyuButton(
                    label = stringResource(R.string.use_global_setting),
                    onClick = onUseGlobal,
                    filled = false
                )
            }
        },
        fab = {
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                SuyuFab(R.drawable.ic_install, stringResource(R.string.fetch), onFetch, label = stringResource(R.string.fetch))
                SuyuFab(R.drawable.ic_add, stringResource(R.string.install), onInstall, label = stringResource(R.string.install))
            }
        }
    ) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 360.dp),
            modifier = Modifier.fillMaxSize(),
            contentPadding = contentPadding(extraBottom = 80.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            itemsIndexed(drivers) { index, driver ->
                SuyuRow(
                    title = driver.title,
                    description = driver.version.ifEmpty { null },
                    details = driver.description.ifEmpty { null },
                    onClick = { onSelect(index) },
                    leading = { SelectionDot(driver.selected) },
                    trailing = if (driver.title != systemDriverTitle) {
                        {
                            SuyuIconButton(
                                R.drawable.ic_delete,
                                stringResource(R.string.delete),
                                { onRemove(index) },
                                size = 44.dp
                            )
                        }
                    } else {
                        null
                    }
                )
            }
        }
    }
}

@Composable
private fun SelectionDot(selected: Boolean) {
    val color = if (selected) SuyuColors.Accent else SuyuColors.Muted
    Box(
        modifier = Modifier
            .size(24.dp)
            .clip(CircleShape)
            .border(2.dp, color, CircleShape),
        contentAlignment = androidx.compose.ui.Alignment.Center
    ) {
        if (selected) {
            Box(
                modifier = Modifier
                    .padding(0.dp)
                    .size(12.dp)
                    .clip(CircleShape)
                    .background(SuyuColors.Accent)
            )
        }
    }
}
