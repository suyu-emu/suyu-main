// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.horizontalScroll
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.fragments.FreedrenoVariable
import org.yuzu.yuzu_emu.ui.compose.common.SuyuButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuCard
import org.yuzu.yuzu_emu.ui.compose.common.SuyuIconButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuRow
import org.yuzu.yuzu_emu.ui.compose.common.SuyuScreen
import org.yuzu.yuzu_emu.ui.compose.common.SuyuSectionTitle
import org.yuzu.yuzu_emu.ui.compose.common.SuyuTextField
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors
import org.yuzu.yuzu_emu.utils.FreedrenoPreset

@Composable
fun FreedrenoScreen(
    title: String,
    presets: List<FreedrenoPreset>,
    variables: List<FreedrenoVariable>,
    onBack: () -> Unit,
    onPreset: (FreedrenoPreset) -> Unit,
    onDelete: (FreedrenoVariable) -> Unit,
    onAdd: (name: String, value: String) -> Boolean,
    onClearAll: () -> Unit,
    onSave: () -> Unit
) {
    var name by rememberSaveable { mutableStateOf("") }
    var value by rememberSaveable { mutableStateOf("") }

    SuyuScreen(title = title, onBack = onBack) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            SuyuSectionTitle(stringResource(R.string.freedreno_presets))
            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                presets.forEach { preset ->
                    SuyuButton(label = preset.name, onClick = { onPreset(preset) }, filled = false)
                }
            }

            SuyuSectionTitle(stringResource(R.string.freedreno_current_settings))
            variables.forEach { variable ->
                SuyuRow(
                    title = variable.name,
                    description = variable.value,
                    trailing = {
                        SuyuIconButton(R.drawable.ic_delete, stringResource(R.string.delete), { onDelete(variable) }, size = 44.dp)
                    }
                )
            }

            SuyuSectionTitle(stringResource(R.string.freedreno_debug))
            SuyuTextField(name, { name = it }, stringResource(R.string.freedreno_var_name), Modifier.fillMaxWidth())
            SuyuTextField(value, { value = it }, stringResource(R.string.freedreno_var_value), Modifier.fillMaxWidth())
            SuyuButton(
                label = stringResource(R.string.freedreno_add_variable),
                onClick = {
                    if (onAdd(name.trim(), value.trim())) {
                        name = ""
                        value = ""
                    }
                },
                modifier = Modifier.fillMaxWidth(),
                filled = false
            )
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.fillMaxWidth()) {
                SuyuButton(
                    label = stringResource(R.string.freedreno_clear_all),
                    onClick = onClearAll,
                    modifier = Modifier.weight(1f),
                    filled = false
                )
                SuyuButton(
                    label = stringResource(R.string.save),
                    onClick = onSave,
                    modifier = Modifier.weight(1f)
                )
            }
            SuyuCard {
                Text(
                    text = stringResource(R.string.freedreno_info_title),
                    color = SuyuColors.Text,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Medium
                )
                Text(
                    text = stringResource(R.string.freedreno_info_description),
                    color = SuyuColors.Muted,
                    fontSize = 13.sp
                )
            }
        }
    }
}
