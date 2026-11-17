// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.common

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.ui.compose.games.focusHighlight
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors

val CardBorder = Color(0xFF232836)

/** Screen frame: back button + title on top, optional floating button, system-bar aware. */
@Composable
fun SuyuScreen(
    title: String,
    onBack: (() -> Unit)?,
    modifier: Modifier = Modifier,
    actions: @Composable RowScope.() -> Unit = {},
    fab: (@Composable () -> Unit)? = null,
    titleContent: (@Composable RowScope.() -> Unit)? = null,
    content: @Composable BoxScope.() -> Unit
) {
    val top = WindowInsets.statusBars.asPaddingValues().calculateTopPadding()
    val bottom = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
    Box(
        modifier = modifier
            .fillMaxSize()
            .background(SuyuColors.Background)
    ) {
        Column(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = 16.dp, end = 16.dp, top = top + 8.dp, bottom = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                if (onBack != null) {
                    SuyuIconButton(R.drawable.ic_back, "back", onBack)
                }
                if (titleContent != null) {
                    titleContent()
                } else {
                    Text(
                        text = title,
                        color = SuyuColors.Text,
                        fontSize = 28.sp,
                        fontWeight = FontWeight.Bold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f)
                    )
                }
                actions()
            }
            Box(modifier = Modifier.weight(1f)) { content() }
        }
        if (fab != null) {
            Box(
                modifier = Modifier
                    .align(Alignment.BottomEnd)
                    .padding(end = 20.dp, bottom = bottom + 20.dp)
            ) { fab() }
        }
    }
}

@Composable
fun SuyuIconButton(icon: Int, description: String, onClick: () -> Unit, size: Dp = 48.dp, enabled: Boolean = true) {
    Box(
        modifier = Modifier
            .size(size)
            .focusHighlight(1.1f, innerPadding = 0.dp, ringRadius = size / 2)
            .clip(CircleShape)
            .background(SuyuColors.SurfaceHigh)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .semantics { contentDescription = description },
        contentAlignment = Alignment.Center
    ) {
        Icon(
            painter = painterResource(icon),
            contentDescription = null,
            tint = if (enabled) SuyuColors.Text else SuyuColors.Muted,
            modifier = Modifier.size(size * 0.45f)
        )
    }
}

@Composable
fun SuyuFab(icon: Int, description: String, onClick: () -> Unit, label: String? = null) {
    val shape = RoundedCornerShape(20.dp)
    Row(
        modifier = Modifier
            .heightIn(min = 56.dp)
            .focusHighlight(1.06f, innerPadding = 0.dp, ringRadius = 20.dp)
            .clip(shape)
            .background(SuyuColors.Accent)
            .clickable(role = Role.Button, onClick = onClick)
            .semantics { contentDescription = description }
            .padding(horizontal = if (label == null) 16.dp else 20.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        Icon(painterResource(icon), contentDescription = null, tint = SuyuColors.OnAccent, modifier = Modifier.size(24.dp))
        if (label != null) {
            Text(text = label, color = SuyuColors.OnAccent, fontSize = 15.sp, fontWeight = FontWeight.Bold)
        }
    }
}

@Composable
fun SuyuCard(
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit
) {
    val shape = RoundedCornerShape(22.dp)
    val base = modifier
        .fillMaxWidth()
        .focusHighlight(1.02f, innerPadding = 0.dp, ringRadius = 22.dp)
        .clip(shape)
        .background(SuyuColors.Surface)
        .border(1.dp, CardBorder, shape)
    Column(
        modifier = (if (onClick != null) base.clickable(role = Role.Button, onClick = onClick) else base)
            .padding(horizontal = 18.dp, vertical = 14.dp),
        content = content
    )
}

/** Icon tile + title + optional description/details, with optional trailing content. */
@Composable
fun SuyuRow(
    title: String,
    modifier: Modifier = Modifier,
    description: String? = null,
    details: String? = null,
    icon: Int? = null,
    onClick: (() -> Unit)? = null,
    leading: (@Composable () -> Unit)? = null,
    trailing: (@Composable () -> Unit)? = null
) {
    SuyuCard(modifier = modifier, onClick = onClick) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            leading?.invoke()
            if (icon != null) {
                Box(
                    modifier = Modifier
                        .size(44.dp)
                        .clip(RoundedCornerShape(14.dp))
                        .background(SuyuColors.Accent.copy(alpha = 0.14f)),
                    contentAlignment = Alignment.Center
                ) {
                    Icon(painterResource(icon), contentDescription = null, tint = SuyuColors.Accent, modifier = Modifier.size(22.dp))
                }
            }
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = title,
                    color = SuyuColors.Text,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Medium,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis
                )
                if (description != null) {
                    Text(text = description, color = SuyuColors.Muted, fontSize = 13.sp)
                }
                if (!details.isNullOrEmpty()) {
                    Text(
                        text = details,
                        color = SuyuColors.Accent,
                        fontSize = 12.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
            trailing?.invoke()
        }
    }
}

@Composable
fun SuyuButton(label: String, onClick: () -> Unit, modifier: Modifier = Modifier, icon: Int? = null, filled: Boolean = true) {
    val shape = CircleShape
    val container = if (filled) {
        modifier.clip(shape).background(SuyuColors.Accent)
    } else {
        modifier.clip(shape).border(1.dp, SuyuColors.Accent.copy(alpha = 0.5f), shape)
    }
    val tint = if (filled) SuyuColors.OnAccent else SuyuColors.Accent
    Row(
        modifier = container
            .focusHighlight(1.04f, innerPadding = 0.dp, ringRadius = 24.dp)
            .heightIn(min = 48.dp)
            .clickable(role = Role.Button, onClick = onClick)
            .padding(horizontal = 20.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally)
    ) {
        if (icon != null) {
            Icon(painterResource(icon), contentDescription = null, tint = tint, modifier = Modifier.size(20.dp))
        }
        Text(text = label, color = tint, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
    }
}

@Composable
fun contentPadding(extraBottom: Dp = 0.dp): PaddingValues {
    val bottom = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
    return PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp, bottom = bottom + 16.dp + extraBottom)
}

@Composable
fun SuyuSectionTitle(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text.uppercase(),
        color = SuyuColors.Accent,
        fontSize = 13.sp,
        fontWeight = FontWeight.SemiBold,
        modifier = modifier.padding(start = 4.dp, top = 12.dp)
    )
}

@Composable
fun SuyuTextField(
    value: String,
    onValueChange: (String) -> Unit,
    hint: String,
    modifier: Modifier = Modifier,
    singleLine: Boolean = true
) {
    val shape = RoundedCornerShape(16.dp)
    Box(
        modifier = modifier
            .heightIn(min = 52.dp)
            .clip(shape)
            .background(SuyuColors.Surface)
            .border(1.dp, CardBorder, shape)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        contentAlignment = Alignment.CenterStart
    ) {
        if (value.isEmpty()) {
            Text(text = hint, color = SuyuColors.Muted, fontSize = 15.sp)
        }
        androidx.compose.foundation.text.BasicTextField(
            value = value,
            onValueChange = onValueChange,
            singleLine = singleLine,
            textStyle = androidx.compose.ui.text.TextStyle(color = SuyuColors.Text, fontSize = 15.sp),
            cursorBrush = androidx.compose.ui.graphics.SolidColor(SuyuColors.Accent),
            modifier = Modifier.fillMaxWidth()
        )
    }
}
