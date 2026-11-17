// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.emulation

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.ui.compose.SuyuIcons
import org.yuzu.yuzu_emu.ui.compose.games.focusHighlight
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors

data class InGameMenuState(
    val title: String = "",
    val cpuBackend: String = "",
    val gpuDriver: String = "",
    val paused: Boolean = false,
    val overlayVisible: Boolean = true,
    val quickSettingsVisible: Boolean = false,
    val drawerLocked: Boolean = false
)

/**
 * Side menu shown while a game runs. It lists the same options as the legacy navigation drawer;
 * every row reports the id of the matching legacy menu item so the fragment keeps its handlers.
 */
@Composable
fun InGameMenu(state: InGameMenuState, onItem: (Int) -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(SuyuColors.Surface)
            .border(1.dp, Color(0xFF232836))
            .padding(horizontal = 18.dp, vertical = 16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Header(state)

        PrimaryButton(
            icon = if (state.paused) R.drawable.ic_play else R.drawable.ic_pause,
            label = stringResource(if (state.paused) R.string.emulation_unpause else R.string.emulation_pause),
            onClick = { onItem(R.id.menu_pause_emulation) }
        )

        Column(
            modifier = Modifier
                .weight(1f)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            ToggleRow(
                icon = if (state.overlayVisible) R.drawable.ic_controller_disconnected else R.drawable.ic_controller,
                label = stringResource(R.string.emulation_show_overlay),
                checked = state.overlayVisible,
                onClick = { onItem(R.id.menu_quick_overlay) }
            )
            ActionRow(R.drawable.ic_overlay, R.string.emulation_input_overlay) { onItem(R.id.menu_overlay_controls) }
            if (state.quickSettingsVisible) {
                ActionRow(R.drawable.ic_settings, R.string.quick_settings) { onItem(R.id.menu_quick_settings) }
            }
            ActionRow(R.drawable.ic_settings, R.string.preferences_settings) { onItem(R.id.menu_settings) }
            ActionRow(R.drawable.ic_settings_outline, R.string.per_game_settings) { onItem(R.id.menu_settings_per_game) }
            ActionRow(R.drawable.ic_controller, R.string.preferences_controls) { onItem(R.id.menu_controls) }
            ActionRow(R.drawable.ic_two_users, R.string.multiplayer) { onItem(R.id.menu_multiplayer) }
            ActionRow(R.drawable.ic_nfc, R.string.load_amiibo) { onItem(R.id.menu_load_amiibo) }
            ToggleRow(
                icon = if (state.drawerLocked) R.drawable.ic_lock else R.drawable.ic_unlock,
                label = stringResource(if (state.drawerLocked) R.string.unlock_drawer else R.string.lock_drawer),
                checked = state.drawerLocked,
                onClick = { onItem(R.id.menu_lock_drawer) }
            )
        }

        ExitButton(onClick = { onItem(R.id.menu_exit) })
    }
}

@Composable
private fun Header(state: InGameMenuState) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Box(
            modifier = Modifier
                .size(44.dp)
                .clip(RoundedCornerShape(14.dp))
                .background(SuyuColors.Accent.copy(alpha = 0.16f)),
            contentAlignment = Alignment.Center
        ) {
            Icon(SuyuIcons.Play, contentDescription = null, tint = SuyuColors.Accent, modifier = Modifier.size(22.dp))
        }
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = state.title,
                color = SuyuColors.Text,
                fontSize = 17.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis
            )
            val system = listOf(state.cpuBackend, state.gpuDriver).filter { it.isNotEmpty() }.joinToString(" | ")
            if (system.isNotEmpty()) {
                Text(
                    text = stringResource(R.string.system_info_label) + " " + system,
                    color = SuyuColors.Muted,
                    fontSize = 12.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
    }
}

@Composable
private fun PrimaryButton(icon: Int, label: String, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(52.dp)
            .focusHighlight(1.03f, innerPadding = 0.dp, ringRadius = 26.dp)
            .clip(CircleShape)
            .background(SuyuColors.Accent)
            .clickable(role = Role.Button, onClick = onClick),
        contentAlignment = Alignment.Center
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Icon(painterResource(icon), contentDescription = null, tint = SuyuColors.OnAccent, modifier = Modifier.size(22.dp))
            Text(text = label, color = SuyuColors.OnAccent, fontSize = 16.sp, fontWeight = FontWeight.Bold)
        }
    }
}

@Composable
private fun RowShell(
    icon: Int,
    label: String,
    onClick: () -> Unit,
    trailing: @Composable () -> Unit
) {
    val shape = RoundedCornerShape(18.dp)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 52.dp)
            .focusHighlight(1.03f, innerPadding = 0.dp, ringRadius = 18.dp)
            .clip(shape)
            .background(SuyuColors.SurfaceHigh)
            .clickable(role = Role.Button, onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Icon(painterResource(icon), contentDescription = null, tint = SuyuColors.Accent, modifier = Modifier.size(22.dp))
        Text(
            text = label,
            color = SuyuColors.Text,
            fontSize = 14.sp,
            fontWeight = FontWeight.Medium,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f)
        )
        trailing()
    }
}

@Composable
private fun ActionRow(icon: Int, label: Int, onClick: () -> Unit) {
    RowShell(icon, stringResource(label), onClick) {
        Icon(
            imageVector = SuyuIcons.Chevron,
            contentDescription = null,
            tint = SuyuColors.Muted,
            modifier = Modifier.size(18.dp)
        )
    }
}

@Composable
private fun ToggleRow(icon: Int, label: String, checked: Boolean, onClick: () -> Unit) {
    RowShell(icon, label, onClick) {
        Box(
            modifier = Modifier
                .size(width = 46.dp, height = 26.dp)
                .clip(CircleShape)
                .background(if (checked) SuyuColors.Accent else Color(0xFF3A4152)),
            contentAlignment = if (checked) Alignment.CenterEnd else Alignment.CenterStart
        ) {
            Box(
                modifier = Modifier
                    .padding(3.dp)
                    .size(20.dp)
                    .clip(CircleShape)
                    .background(if (checked) SuyuColors.OnAccent else SuyuColors.Muted)
            )
        }
    }
}

@Composable
private fun ExitButton(onClick: () -> Unit) {
    val shape = CircleShape
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(48.dp)
            .focusHighlight(1.03f, innerPadding = 0.dp, ringRadius = 24.dp)
            .clip(shape)
            .border(1.dp, SuyuColors.Heart.copy(alpha = 0.5f), shape)
            .clickable(role = Role.Button, onClick = onClick),
        contentAlignment = Alignment.Center
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Icon(painterResource(R.drawable.ic_exit), contentDescription = null, tint = SuyuColors.Heart, modifier = Modifier.size(20.dp))
            Text(
                text = stringResource(R.string.emulation_exit),
                color = SuyuColors.Heart,
                fontSize = 15.sp,
                fontWeight = FontWeight.SemiBold
            )
        }
    }
}
