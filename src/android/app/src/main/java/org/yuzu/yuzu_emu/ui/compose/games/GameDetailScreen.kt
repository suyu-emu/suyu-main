// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.games

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyGridScope
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.io.File
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.GameProperty
import org.yuzu.yuzu_emu.model.InstallableProperty
import org.yuzu.yuzu_emu.model.SubmenuProperty
import org.yuzu.yuzu_emu.ui.compose.SuyuIcons
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors
import org.yuzu.yuzu_emu.utils.DirectoryInitialization
import org.yuzu.yuzu_emu.utils.MemoryUtil

/**
 * Per-game page: cover, real stats (size, play time, measured FPS, shader cache) and the same
 * properties the legacy screen listed. Tapping the play time edits it, as before.
 */
@Composable
fun GameDetailScreen(
    game: Game,
    playTimeSeconds: Long,
    properties: List<GameProperty>,
    startEnabled: Boolean,
    shortcutEnabled: Boolean,
    onBack: () -> Unit,
    onShortcut: () -> Unit,
    onEditPlayTime: () -> Unit,
    onStart: () -> Unit
) {
    val topInset = WindowInsets.statusBars.asPaddingValues().calculateTopPadding()
    val bottomInset = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
    val fps = remember(game.path, playTimeSeconds) { GameMeta.averageFps(game) }

    BoxWithConstraints(
        modifier = Modifier
            .fillMaxSize()
            .background(SuyuColors.Background)
    ) {
        val wide = maxWidth >= 600.dp && maxWidth > maxHeight
        val summary: @Composable () -> Unit = {
            Summary(
                game = game,
                fps = fps,
                playTimeSeconds = playTimeSeconds,
                onEditPlayTime = onEditPlayTime
            )
        }
        val propertyGrid: LazyGridScope.() -> Unit = {
            item(span = { GridItemSpan(maxLineSpan) }) {
                Text(
                    text = stringResource(R.string.detail_properties),
                    color = SuyuColors.Text,
                    fontSize = 18.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.padding(start = 4.dp, top = 6.dp)
                )
            }
            items(properties) { property ->
                when (property) {
                    is SubmenuProperty -> SubmenuCard(property)
                    is InstallableProperty -> InstallableCard(property)
                }
            }
        }

        if (wide) {
            Row(modifier = Modifier.fillMaxSize()) {
                Column(
                    modifier = Modifier
                        .width(380.dp)
                        .fillMaxHeight()
                        .padding(start = 16.dp, top = topInset + 8.dp, bottom = bottomInset + 16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    TopButtons(onBack, onShortcut, shortcutEnabled)
                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .verticalScroll(rememberScrollState())
                    ) { summary() }
                    PlayButton(onStart, startEnabled)
                }
                LazyVerticalGrid(
                    columns = GridCells.Adaptive(minSize = 320.dp),
                    modifier = Modifier
                        .weight(1f)
                        .padding(bottom = bottomInset),
                    contentPadding = PaddingValues(
                        start = 16.dp,
                        end = 16.dp,
                        top = topInset + 8.dp,
                        bottom = 16.dp
                    ),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                    content = propertyGrid
                )
            }
        } else {
            Column(modifier = Modifier.fillMaxSize()) {
                LazyVerticalGrid(
                    columns = GridCells.Adaptive(minSize = 320.dp),
                    modifier = Modifier.weight(1f),
                    contentPadding = PaddingValues(
                        start = 16.dp,
                        end = 16.dp,
                        top = topInset + 8.dp,
                        bottom = 16.dp
                    ),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                            TopButtons(onBack, onShortcut, shortcutEnabled)
                            summary()
                        }
                    }
                    propertyGrid()
                }
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(SuyuColors.Background)
                        .padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = bottomInset + 12.dp)
                ) { PlayButton(onStart, startEnabled) }
            }
        }
    }
}

@Composable
private fun TopButtons(onBack: () -> Unit, onShortcut: () -> Unit, shortcutEnabled: Boolean) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        RoundButton(onClick = onBack, description = "back") {
            Icon(painterResource(R.drawable.ic_back), contentDescription = null, tint = SuyuColors.Text)
        }
        RoundButton(onClick = onShortcut, description = "shortcut", enabled = shortcutEnabled) {
            Icon(painterResource(R.drawable.ic_add), contentDescription = null, tint = SuyuColors.Text)
        }
    }
}

@Composable
private fun RoundButton(
    onClick: () -> Unit,
    description: String,
    enabled: Boolean = true,
    content: @Composable () -> Unit
) {
    Box(
        modifier = Modifier
            .size(48.dp)
            .focusHighlight(1.1f, innerPadding = 0.dp, ringRadius = 24.dp)
            .clip(CircleShape)
            .background(SuyuColors.SurfaceHigh)
            .alpha(if (enabled) 1f else 0.4f)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .semantics { contentDescription = description },
        contentAlignment = Alignment.Center
    ) { content() }
}

@Composable
private fun Summary(
    game: Game,
    fps: Float?,
    playTimeSeconds: Long,
    onEditPlayTime: () -> Unit
) {
    val context = LocalContext.current
    val size by produceState<String?>(null, game.path) {
        value = withContext(Dispatchers.IO) { GameMeta.sizeLabel(game) }
    }
    val shaders by produceState<String?>(null, game.path) {
        value = withContext(Dispatchers.IO) {
            val dir = File(
                DirectoryInitialization.userDirectory + "/cache/shader/" + game.settingsName.lowercase()
            )
            if (dir.exists()) {
                MemoryUtil.bytesToSizeUnit(
                    dir.walkTopDown().filter { it.isFile }.map { it.length() }.sum().toFloat()
                )
            } else {
                null
            }
        }
    }
    val dash = "—"

    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(16.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            GameCover(
                game,
                Modifier
                    .size(140.dp)
                    .clip(RoundedCornerShape(26.dp))
                    .background(SuyuColors.SurfaceHigh)
            )
            Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                CompatBadge(fps)
                Text(
                    text = cleanTitle(game.title),
                    color = SuyuColors.Text,
                    fontSize = 24.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 4,
                    overflow = TextOverflow.Ellipsis
                )
                if (game.developer.isNotEmpty()) {
                    Text(text = game.developer, color = SuyuColors.TextSoft, fontSize = 14.sp, maxLines = 1)
                }
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Stat(
                label = stringResource(R.string.detail_stat_fps),
                value = fps?.let { String.format(Locale.US, "%.1f", it) } ?: dash,
                accent = fps != null,
                modifier = Modifier.weight(1f)
            )
            Stat(
                label = stringResource(R.string.detail_stat_played),
                value = if (playTimeSeconds > 0) formatPlayTime(context, playTimeSeconds) else dash,
                modifier = Modifier
                    .weight(1f)
                    .focusHighlight(1.04f, innerPadding = 0.dp, ringRadius = 18.dp)
                    .clip(RoundedCornerShape(18.dp))
                    .clickable(role = Role.Button, onClick = onEditPlayTime)
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Stat(
                label = stringResource(R.string.detail_stat_size),
                value = size ?: dash,
                modifier = Modifier.weight(1f)
            )
            Stat(
                label = stringResource(R.string.detail_stat_shaders),
                value = shaders ?: dash,
                modifier = Modifier.weight(1f)
            )
        }
    }
}

private fun formatPlayTime(context: android.content.Context, seconds: Long): String {
    val hours = seconds / 3600
    val minutes = (seconds % 3600) / 60
    return when {
        hours > 0 -> "$hours${context.getString(R.string.hours_abbr)} $minutes${context.getString(R.string.minutes_abbr)}"
        minutes > 0 -> "$minutes${context.getString(R.string.minutes_abbr)}"
        else -> "$seconds${context.getString(R.string.seconds_abbr)}"
    }
}

@Composable
private fun Stat(label: String, value: String, modifier: Modifier = Modifier, accent: Boolean = false) {
    val shape = RoundedCornerShape(18.dp)
    Column(
        modifier = modifier
            .clip(shape)
            .background(SuyuColors.Surface)
            .border(1.dp, Color(0xFF232836), shape)
            .padding(horizontal = 12.dp, vertical = 10.dp)
    ) {
        Text(text = label, color = SuyuColors.Muted, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Text(
            text = value,
            color = if (accent) SuyuColors.Accent else SuyuColors.Text,
            fontSize = 20.sp,
            fontWeight = FontWeight.SemiBold,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

@Composable
private fun PlayButton(onStart: () -> Unit, enabled: Boolean) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(60.dp)
            .focusHighlight(1.03f, innerPadding = 0.dp, ringRadius = 30.dp)
            .clip(CircleShape)
            .background(SuyuColors.Accent)
            .alpha(if (enabled) 1f else 0.4f)
            .clickable(enabled = enabled, role = Role.Button, onClick = onStart),
        contentAlignment = Alignment.Center
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Icon(SuyuIcons.Play, contentDescription = null, tint = SuyuColors.OnAccent, modifier = Modifier.size(24.dp))
            Text(
                text = stringResource(R.string.play),
                color = SuyuColors.OnAccent,
                fontSize = 18.sp,
                fontWeight = FontWeight.Bold
            )
        }
    }
}

@Composable
private fun PropertyShell(
    icon: Int,
    title: Int,
    description: Int,
    onClick: (() -> Unit)?,
    trailing: @Composable () -> Unit,
    details: String? = null
) {
    val shape = RoundedCornerShape(22.dp)
    val base = Modifier
        .fillMaxWidth()
        .height(96.dp)
        .focusHighlight(1.03f, innerPadding = 0.dp, ringRadius = 22.dp)
        .clip(shape)
        .background(SuyuColors.Surface)
        .border(1.dp, Color(0xFF232836), shape)
    Row(
        modifier = (if (onClick != null) base.clickable(role = Role.Button, onClick = onClick) else base)
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
            Icon(painterResource(icon), contentDescription = null, tint = SuyuColors.Accent, modifier = Modifier.size(22.dp))
        }
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = stringResource(title),
                color = SuyuColors.Text,
                fontSize = 15.sp,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Text(
                text = stringResource(description),
                color = SuyuColors.Muted,
                fontSize = 13.sp,
                maxLines = if (details.isNullOrEmpty()) 2 else 1,
                overflow = TextOverflow.Ellipsis
            )
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
        trailing()
    }
}

@Composable
private fun SubmenuCard(property: SubmenuProperty) {
    val flowDetails = property.detailsFlow?.collectAsState()?.value
    val computed by produceState<String?>(null, property) {
        value = property.details?.let { withContext(Dispatchers.IO) { it() } }
    }
    val actions = property.secondaryActions?.filter { it.isShown }.orEmpty()
    PropertyShell(
        icon = property.iconId,
        title = property.titleId,
        description = property.descriptionId,
        onClick = property.action,
        details = flowDetails ?: computed,
        trailing = {
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically) {
                actions.forEach { action ->
                    IconAction(
                        icon = action.iconId,
                        description = stringResource(action.descriptionId),
                        onClick = action.action
                    )
                }
                Icon(
                    imageVector = SuyuIcons.Chevron,
                    contentDescription = null,
                    tint = SuyuColors.Muted,
                    modifier = Modifier.size(20.dp)
                )
            }
        }
    )
}

@Composable
private fun InstallableCard(property: InstallableProperty) {
    PropertyShell(
        icon = property.iconId,
        title = property.titleId,
        description = property.descriptionId,
        onClick = null,
        trailing = {
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically) {
                property.install?.let {
                    IconAction(R.drawable.ic_install, stringResource(R.string.detail_action_import), it)
                }
                property.export?.let {
                    IconAction(R.drawable.ic_export, stringResource(R.string.detail_action_export), it)
                }
            }
        }
    )
}

@Composable
private fun IconAction(icon: Int, description: String, onClick: () -> Unit, size: Dp = 44.dp) {
    Box(
        modifier = Modifier
            .size(size)
            .focusHighlight(1.12f, innerPadding = 0.dp, ringRadius = size / 2)
            .clip(CircleShape)
            .background(SuyuColors.SurfaceHigh)
            .clickable(role = Role.Button, onClick = onClick)
            .semantics { contentDescription = description },
        contentAlignment = Alignment.Center
    ) {
        Icon(painterResource(icon), contentDescription = null, tint = SuyuColors.Text, modifier = Modifier.size(20.dp))
    }
}
