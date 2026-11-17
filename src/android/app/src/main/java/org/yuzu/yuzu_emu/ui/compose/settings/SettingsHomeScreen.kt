// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.HomeSetting
import org.yuzu.yuzu_emu.ui.compose.SuyuIcons
import org.yuzu.yuzu_emu.ui.compose.games.LibraryDestination
import org.yuzu.yuzu_emu.ui.compose.games.NavBar
import org.yuzu.yuzu_emu.ui.compose.games.focusHighlight
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors

/** Settings group a [HomeSetting] is listed under, decided from its title resource. */
private enum class SettingsGroup(val titleRes: Int) {
    GENERAL(R.string.settings_group_general),
    GRAPHICS(R.string.settings_group_graphics),
    ONLINE(R.string.settings_group_online),
    DATA(R.string.settings_group_data),
    SUPPORT(R.string.settings_group_support)
}

private fun groupOf(setting: HomeSetting): SettingsGroup = when (setting.titleId) {
    R.string.advanced_settings,
    R.string.app_settings,
    R.string.preferences_controls,
    R.string.profile_manager -> SettingsGroup.GENERAL
    R.string.gpu_driver_manager,
    R.string.freedreno_settings_title -> SettingsGroup.GRAPHICS
    R.string.multiplayer,
    R.string.applets -> SettingsGroup.ONLINE
    R.string.manage_yuzu_data,
    R.string.manage_game_folders,
    R.string.verify_installed_content -> SettingsGroup.DATA
    else -> SettingsGroup.SUPPORT
}

@Composable
fun SettingsHomeScreen(
    settings: List<HomeSetting>,
    firmwareVersion: String?,
    keysPresent: Boolean,
    onBack: () -> Unit,
    onSettingClick: (HomeSetting) -> Unit,
    onNavigate: (LibraryDestination) -> Unit
) {
    val topInset = WindowInsets.statusBars.asPaddingValues().calculateTopPadding()
    val grouped = SettingsGroup.entries.map { group -> group to settings.filter { groupOf(it) == group } }
        .filter { it.second.isNotEmpty() }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(SuyuColors.Background)
    ) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 360.dp),
            modifier = Modifier.weight(1f),
            contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = topInset + 8.dp, bottom = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            item(span = { GridItemSpan(maxLineSpan) }) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Box(
                        modifier = Modifier
                            .size(48.dp)
                            .clip(CircleShape)
                            .background(SuyuColors.SurfaceHigh)
                            .clickable(role = Role.Button, onClick = onBack)
                            .semantics { contentDescription = "back" },
                        contentAlignment = Alignment.Center
                    ) {
                        Icon(
                            painter = painterResource(R.drawable.ic_back),
                            contentDescription = null,
                            tint = SuyuColors.Text
                        )
                    }
                    Text(
                        text = stringResource(R.string.preferences_settings),
                        color = SuyuColors.Text,
                        fontSize = 34.sp,
                        fontWeight = FontWeight.Bold
                    )
                }
            }
            item(span = { GridItemSpan(maxLineSpan) }) {
                StatusCard(firmwareVersion, keysPresent)
            }
            grouped.forEach { (group, entries) ->
                item(span = { GridItemSpan(maxLineSpan) }) {
                    Text(
                        text = stringResource(group.titleRes).uppercase(),
                        color = SuyuColors.Accent,
                        fontSize = 13.sp,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.padding(start = 8.dp, top = 10.dp)
                    )
                }
                items(entries, key = { it.titleId }) { setting ->
                    SettingCard(setting, onClick = { onSettingClick(setting) })
                }
            }
        }

        NavBar(selected = LibraryDestination.SETTINGS, onSelect = onNavigate)
    }
}

@Composable
private fun StatusCard(firmwareVersion: String?, keysPresent: Boolean) {
    val ready = keysPresent && firmwareVersion != null
    val shape = RoundedCornerShape(26.dp)
    val tint = if (ready) SuyuColors.Accent else SuyuColors.CompatPlayable
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(shape)
            .background(Brush.linearGradient(listOf(tint.copy(alpha = 0.16f), SuyuColors.Violet.copy(alpha = 0.12f))))
            .border(1.dp, tint.copy(alpha = 0.3f), shape)
            .padding(16.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        Box(
            modifier = Modifier
                .size(48.dp)
                .clip(RoundedCornerShape(16.dp))
                .background(tint),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                painter = painterResource(if (ready) R.drawable.ic_check_circle else R.drawable.ic_info_outline),
                contentDescription = null,
                tint = SuyuColors.OnAccent,
                modifier = Modifier.size(26.dp)
            )
        }
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                text = stringResource(
                    when {
                        !keysPresent -> R.string.keys_missing
                        firmwareVersion == null -> R.string.settings_no_firmware
                        else -> R.string.settings_ready
                    }
                ),
                color = SuyuColors.Text,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold
            )
            Text(
                text = when {
                    !keysPresent -> stringResource(R.string.keys_missing_description)
                    firmwareVersion == null -> stringResource(R.string.firmware_description_short)
                    else -> stringResource(R.string.settings_ready_detail, firmwareVersion)
                },
                color = SuyuColors.TextSoft,
                fontSize = 13.sp
            )
        }
    }
}

@Composable
private fun SettingCard(setting: HomeSetting, onClick: () -> Unit) {
    val details by setting.details.collectAsState()
    val enabled = setting.isEnabled()
    val shape = RoundedCornerShape(22.dp)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .height(96.dp)
            .focusHighlight(1.03f, innerPadding = 0.dp, ringRadius = 22.dp)
            .clip(shape)
            .background(SuyuColors.Surface)
            .border(1.dp, Color(0xFF232836), shape)
            .clickable(role = Role.Button, onClick = onClick)
            .alpha(if (enabled) 1f else 0.5f)
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        Box(
            modifier = Modifier
                .size(44.dp)
                .clip(RoundedCornerShape(14.dp))
                .background(SuyuColors.Accent.copy(alpha = 0.14f)),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                painter = painterResource(setting.iconId),
                contentDescription = null,
                tint = SuyuColors.Accent,
                modifier = Modifier.size(22.dp)
            )
        }
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = stringResource(setting.titleId),
                color = SuyuColors.Text,
                fontSize = 15.sp,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Text(
                text = stringResource(setting.descriptionId),
                color = SuyuColors.Muted,
                fontSize = 13.sp,
                maxLines = if (details.isNotEmpty()) 1 else 2,
                overflow = TextOverflow.Ellipsis
            )
            if (details.isNotEmpty()) {
                Text(
                    text = details,
                    color = SuyuColors.Accent,
                    fontSize = 12.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
        Icon(
            imageVector = SuyuIcons.Chevron,
            contentDescription = null,
            tint = SuyuColors.Muted,
            modifier = Modifier.size(20.dp)
        )
    }
}
