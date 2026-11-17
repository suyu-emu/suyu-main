// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Applet
import org.yuzu.yuzu_emu.ui.compose.common.SuyuButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuRow
import org.yuzu.yuzu_emu.ui.compose.common.SuyuScreen
import org.yuzu.yuzu_emu.ui.compose.common.contentPadding

@Composable
fun AppletsScreen(applets: List<Applet>, onBack: () -> Unit, onLaunch: (Applet) -> Unit) {
    SuyuScreen(title = stringResource(R.string.applets), onBack = onBack) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 360.dp),
            modifier = Modifier.fillMaxSize(),
            contentPadding = contentPadding(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            items(applets) { applet ->
                SuyuRow(
                    title = stringResource(applet.titleId),
                    description = stringResource(applet.descriptionId),
                    icon = applet.iconId,
                    onClick = { onLaunch(applet) }
                )
            }
        }
    }
}

class GameInfoField(val label: Int, val value: String)

@Composable
fun GameInfoScreen(
    title: String,
    fields: List<GameInfoField>,
    onBack: () -> Unit,
    onCopyField: (GameInfoField) -> Unit,
    onCopyAll: () -> Unit,
    onVerify: () -> Unit
) {
    SuyuScreen(title = title, onBack = onBack) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            fields.forEach { field ->
                SuyuRow(
                    title = field.value,
                    description = stringResource(field.label),
                    onClick = { onCopyField(field) }
                )
            }
            SuyuButton(
                label = stringResource(R.string.copy_details),
                onClick = onCopyAll,
                modifier = Modifier.fillMaxWidth(),
                filled = false
            )
            SuyuButton(
                label = stringResource(R.string.verify_integrity),
                onClick = onVerify,
                modifier = Modifier.fillMaxWidth()
            )
        }
    }
}
