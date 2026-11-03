// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.text.HtmlCompat
import org.commonmark.parser.Parser
import org.commonmark.renderer.html.HtmlRenderer
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.fragments.DriverFetcherFragment.Artifact
import org.yuzu.yuzu_emu.fragments.DriverFetcherFragment.DriverGroup
import org.yuzu.yuzu_emu.fragments.DriverFetcherFragment.Release
import org.yuzu.yuzu_emu.ui.compose.common.SuyuButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuCard
import org.yuzu.yuzu_emu.ui.compose.common.SuyuRow
import org.yuzu.yuzu_emu.ui.compose.common.SuyuScreen
import org.yuzu.yuzu_emu.ui.compose.common.contentPadding
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors

@Composable
fun DriverFetcherScreen(
    gpuModel: String?,
    recommendedDriver: String,
    loading: Boolean,
    groups: List<DriverGroup>,
    isInstalled: (Artifact) -> Boolean,
    onInstall: (Artifact, onDone: () -> Unit) -> Unit,
    onBack: () -> Unit
) {
    // Bumped after an install so the buttons re-check which drivers are present.
    var installRevision by remember { mutableIntStateOf(0) }

    SuyuScreen(title = stringResource(R.string.gpu_driver_fetcher), onBack = onBack) {
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = contentPadding(),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            item {
                SuyuCard {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(stringResource(R.string.gpu_model), color = SuyuColors.Muted, fontSize = 14.sp)
                        Text(gpuModel ?: "-", color = SuyuColors.Accent, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.padding(top = 4.dp)) {
                        Text(stringResource(R.string.recommended_driver), color = SuyuColors.Muted, fontSize = 14.sp)
                        Text(recommendedDriver, color = SuyuColors.Accent, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                    }
                }
            }
            if (loading) {
                item {
                    LinearProgressIndicator(
                        modifier = Modifier.fillMaxWidth(),
                        color = SuyuColors.Accent,
                        trackColor = SuyuColors.SurfaceHigh
                    )
                }
            }
            items(groups) { group ->
                DriverGroupCard(group, installRevision, isInstalled) { artifact ->
                    onInstall(artifact) { installRevision++ }
                }
            }
        }
    }
}

@Composable
private fun DriverGroupCard(
    group: DriverGroup,
    installRevision: Int,
    isInstalled: (Artifact) -> Boolean,
    onInstall: (Artifact) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    SuyuCard(onClick = { expanded = !expanded }) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Text(
                text = group.name,
                color = SuyuColors.Text,
                fontSize = 17.sp,
                fontWeight = FontWeight.SemiBold
            )
            Text(
                text = if (expanded) "▲" else "▼",
                color = SuyuColors.Muted,
                fontSize = 14.sp
            )
        }
        Text(
            text = "${group.releases.size}",
            color = SuyuColors.Muted,
            fontSize = 12.sp
        )
    }
    if (expanded) {
        Column(
            modifier = Modifier.padding(start = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            group.releases.forEach { release ->
                ReleaseCard(release, installRevision, isInstalled, onInstall)
            }
        }
    }
}

private val markdownParser: Parser by lazy { Parser.builder().build() }
private val htmlRenderer: HtmlRenderer by lazy { HtmlRenderer.builder().build() }

private fun previewText(body: String): String {
    val raw = body.take(150).replace("#", "").removeSurrounding(" ")
    return raw.replace("\\r\\n", "\n").replace("\\n", "\n")
}

private fun fullText(body: String): String {
    val md = body.replace("\\r\\n", "\n\n").replace("\\n", "\n\n")
    return try {
        val html = htmlRenderer.render(markdownParser.parse(md))
        HtmlCompat.fromHtml(html, HtmlCompat.FROM_HTML_MODE_COMPACT).toString().trim()
    } catch (_: Exception) {
        md
    }
}

@Composable
private fun ReleaseCard(
    release: Release,
    installRevision: Int,
    isInstalled: (Artifact) -> Boolean,
    onInstall: (Artifact) -> Unit
) {
    var fullBody by remember { mutableStateOf(false) }
    var showDownloads by remember { mutableStateOf(false) }
    SuyuCard(onClick = { fullBody = !fullBody }) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(
                text = release.title,
                color = SuyuColors.Text,
                fontSize = 15.sp,
                fontWeight = FontWeight.Medium,
                modifier = Modifier.weight(1f, fill = false)
            )
            if (release.latest) {
                Text(
                    text = stringResource(R.string.latest),
                    color = SuyuColors.OnAccent,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier
                        .clip(RoundedCornerShape(8.dp))
                        .background(SuyuColors.Accent)
                        .padding(horizontal = 8.dp, vertical = 2.dp)
                )
            }
        }
        Text(
            text = if (fullBody) fullText(release.body) else previewText(release.body),
            color = SuyuColors.Muted,
            fontSize = 13.sp,
            maxLines = if (fullBody) Int.MAX_VALUE else 3,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(top = 4.dp)
        )
        SuyuButton(
            label = stringResource(if (showDownloads) R.string.hide_downloads else R.string.show_downloads),
            onClick = { showDownloads = !showDownloads },
            modifier = Modifier.padding(top = 8.dp),
            filled = false
        )
        if (showDownloads) {
            Column(
                modifier = Modifier.padding(top = 8.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp)
            ) {
                release.artifacts.forEach { artifact ->
                    val installed = installRevision.let { isInstalled(artifact) }
                    SuyuButton(
                        label = if (installed) {
                            stringResource(R.string.installed_label, artifact.name)
                        } else {
                            artifact.name
                        },
                        onClick = { if (!installed) onInstall(artifact) },
                        modifier = Modifier.fillMaxWidth(),
                        icon = if (installed) R.drawable.ic_check else R.drawable.ic_import,
                        filled = !installed
                    )
                }
            }
        }
    }
}
