// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.settings

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.features.settings.ui.SettingRow
import org.yuzu.yuzu_emu.ui.compose.SuyuIcons
import org.yuzu.yuzu_emu.ui.compose.common.CardBorder
import org.yuzu.yuzu_emu.ui.compose.common.SuyuButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuIconButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuScreen
import org.yuzu.yuzu_emu.ui.compose.common.contentPadding
import org.yuzu.yuzu_emu.ui.compose.games.focusHighlight
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors

@Composable
fun SettingsListScreen(
    title: String,
    rows: List<SettingRow>,
    onBack: () -> Unit,
    onSearch: (() -> Unit)? = null
) {
    SuyuScreen(
        title = title,
        onBack = onBack,
        actions = {
            if (onSearch != null) {
                SuyuIconButton(R.drawable.ic_search, stringResource(R.string.home_search), onSearch)
            }
        }
    ) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 480.dp),
            modifier = Modifier.fillMaxSize(),
            contentPadding = contentPadding(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            items(
                items = rows,
                span = { row ->
                    if (row is SettingRow.Header) GridItemSpan(maxLineSpan) else GridItemSpan(1)
                }
            ) { row ->
                when (row) {
                    is SettingRow.Header -> Text(
                        text = row.title.uppercase(),
                        color = SuyuColors.Accent,
                        fontSize = 13.sp,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.padding(start = 8.dp, top = 14.dp, bottom = 2.dp)
                    )
                    is SettingRow.Item -> SettingItem(row)
                }
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun SettingItem(row: SettingRow.Item) {
    val shape = RoundedCornerShape(18.dp)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 64.dp)
            .focusHighlight(1.02f, innerPadding = 0.dp, ringRadius = 18.dp)
            .clip(shape)
            .background(SuyuColors.Surface)
            .border(1.dp, CardBorder, shape)
            .combinedClickable(
                role = Role.Button,
                onClick = row.onClick,
                onLongClick = row.onLongClick
            )
            .alpha(if (row.editable) 1f else 0.5f)
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        if (row.iconId != 0) {
            Box(
                modifier = Modifier
                    .size(40.dp)
                    .clip(RoundedCornerShape(12.dp))
                    .background(SuyuColors.Accent.copy(alpha = 0.14f)),
                contentAlignment = Alignment.Center
            ) {
                Icon(painterResource(row.iconId), contentDescription = null, tint = SuyuColors.Accent, modifier = Modifier.size(20.dp))
            }
        }
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = row.title,
                color = SuyuColors.Text,
                fontSize = 15.sp,
                fontWeight = FontWeight.Medium
            )
            if (row.description.isNotEmpty()) {
                Text(text = row.description, color = SuyuColors.Muted, fontSize = 13.sp)
            }
            if (!row.value.isNullOrEmpty()) {
                Text(
                    text = row.value,
                    color = SuyuColors.Accent,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
        if (row.clearable) {
            SuyuButton(
                label = stringResource(row.clearLabelId),
                onClick = row.onClear,
                filled = false
            )
        }
        if (row.showOptions) {
            SuyuIconButton(R.drawable.ic_settings, "options", row.onOptions, size = 44.dp)
        }
        row.checked?.let { SwitchPill(it) }
        if (row.chevron) {
            Icon(SuyuIcons.Chevron, contentDescription = null, tint = SuyuColors.Muted, modifier = Modifier.size(20.dp))
        }
    }
}

@Composable
private fun SwitchPill(checked: Boolean) {
    Box(
        modifier = Modifier
            .size(width = 50.dp, height = 28.dp)
            .clip(CircleShape)
            .background(if (checked) SuyuColors.Accent else androidx.compose.ui.graphics.Color(0xFF3A4152)),
        contentAlignment = if (checked) Alignment.CenterEnd else Alignment.CenterStart
    ) {
        Box(
            modifier = Modifier
                .padding(4.dp)
                .size(20.dp)
                .clip(CircleShape)
                .background(if (checked) SuyuColors.OnAccent else SuyuColors.Muted)
        )
    }
}

@Composable
fun SettingsSearchScreen(
    query: String,
    onQueryChange: (String) -> Unit,
    rows: List<SettingRow>,
    onBack: () -> Unit
) {
    val requester = androidx.compose.runtime.remember { androidx.compose.ui.focus.FocusRequester() }
    androidx.compose.runtime.LaunchedEffect(Unit) {
        runCatching { requester.requestFocus() }
    }
    SuyuScreen(
        title = stringResource(R.string.search_settings),
        onBack = onBack,
        titleContent = {
            Row(
                modifier = Modifier
                    .weight(1f)
                    .heightIn(min = 48.dp)
                    .clip(RoundedCornerShape(24.dp))
                    .background(SuyuColors.Surface)
                    .border(1.dp, CardBorder, RoundedCornerShape(24.dp))
                    .padding(horizontal = 16.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                Icon(SuyuIcons.Search, contentDescription = null, tint = SuyuColors.Muted, modifier = Modifier.size(20.dp))
                Box(modifier = Modifier.weight(1f)) {
                    if (query.isEmpty()) {
                        Text(stringResource(R.string.search_settings), color = SuyuColors.Muted, fontSize = 15.sp)
                    }
                    androidx.compose.foundation.text.BasicTextField(
                        value = query,
                        onValueChange = onQueryChange,
                        singleLine = true,
                        textStyle = androidx.compose.ui.text.TextStyle(color = SuyuColors.Text, fontSize = 15.sp),
                        cursorBrush = androidx.compose.ui.graphics.SolidColor(SuyuColors.Accent),
                        modifier = Modifier
                            .fillMaxWidth()
                            .focusRequester(requester)
                    )
                }
                if (query.isNotEmpty()) {
                    SuyuIconButton(R.drawable.ic_clear, "clear", { onQueryChange("") }, size = 36.dp)
                }
            }
        }
    ) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 480.dp),
            modifier = Modifier.fillMaxSize(),
            contentPadding = contentPadding(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            items(items = rows) { row ->
                if (row is SettingRow.Item) SettingItem(row)
            }
        }
    }
}
