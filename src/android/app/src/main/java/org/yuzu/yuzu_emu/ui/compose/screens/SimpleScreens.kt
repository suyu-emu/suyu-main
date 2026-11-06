// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.screens

import android.net.Uri
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.DirectoryType
import org.yuzu.yuzu_emu.model.GameDir
import org.yuzu.yuzu_emu.model.License
import org.yuzu.yuzu_emu.ui.compose.common.SuyuButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuFab
import org.yuzu.yuzu_emu.ui.compose.common.SuyuIconButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuRow
import org.yuzu.yuzu_emu.ui.compose.common.SuyuScreen
import org.yuzu.yuzu_emu.ui.compose.common.contentPadding
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors

@Composable
fun GameFoldersScreen(
    folders: List<GameDir>,
    onBack: () -> Unit,
    onAdd: () -> Unit,
    onEdit: (GameDir) -> Unit,
    onDelete: (GameDir) -> Unit
) {
    SuyuScreen(
        title = stringResource(R.string.game_folders),
        onBack = onBack,
        fab = { SuyuFab(R.drawable.ic_add, stringResource(R.string.add_games), onAdd) }
    ) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 360.dp),
            modifier = Modifier.fillMaxSize(),
            contentPadding = contentPadding(extraBottom = 80.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            items(folders) { folder ->
                SuyuRow(
                    title = Uri.parse(folder.uriString).path ?: folder.uriString,
                    description = stringResource(
                        when (folder.type) {
                            DirectoryType.GAME -> R.string.games
                            DirectoryType.EXTERNAL_CONTENT -> R.string.external_content
                        }
                    ),
                    trailing = {
                        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            SuyuIconButton(R.drawable.ic_edit, stringResource(R.string.edit), { onEdit(folder) }, size = 44.dp)
                            SuyuIconButton(R.drawable.ic_delete, stringResource(R.string.delete), { onDelete(folder) }, size = 44.dp)
                        }
                    }
                )
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun AboutScreen(
    versionText: String,
    onBack: () -> Unit,
    onLogoLongPress: () -> Unit,
    onContributors: () -> Unit,
    onLicenses: () -> Unit,
    onCopyVersion: () -> Unit,
    onGithub: () -> Unit,
    onReddit: () -> Unit
) {
    SuyuScreen(title = stringResource(R.string.about), onBack = onBack) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 8.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            Spacer(Modifier.height(8.dp))
            Image(
                painter = painterResource(R.drawable.ic_suyu_full),
                contentDescription = null,
                modifier = Modifier
                    .size(112.dp)
                    .combinedClickable(onClick = {}, onLongClick = onLogoLongPress)
            )
            Text(
                text = stringResource(R.string.app_name),
                color = SuyuColors.Text,
                fontSize = 34.sp,
                fontWeight = FontWeight.Bold
            )
            Text(
                text = stringResource(R.string.about_app_description),
                color = SuyuColors.Muted,
                fontSize = 14.sp,
                textAlign = TextAlign.Center
            )
            Spacer(Modifier.height(8.dp))
            SuyuRow(
                title = stringResource(R.string.contributors),
                description = stringResource(R.string.contributors_description),
                icon = R.drawable.ic_two_users,
                onClick = onContributors
            )
            SuyuRow(
                title = stringResource(R.string.licenses),
                description = stringResource(R.string.licenses_description),
                icon = R.drawable.ic_code,
                onClick = onLicenses
            )
            SuyuRow(
                title = stringResource(R.string.build),
                description = versionText,
                icon = R.drawable.ic_info_outline,
                onClick = onCopyVersion
            )
            Row(
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp, Alignment.CenterHorizontally)
            ) {
                SuyuButton("GitHub", onGithub, icon = R.drawable.ic_github, filled = false)
                SuyuButton("Reddit", onReddit, icon = R.drawable.ic_website, filled = false)
            }
        }
    }
}

@Composable
fun LicensesScreen(licenses: List<License>, onBack: () -> Unit, onOpen: (License) -> Unit) {
    SuyuScreen(title = stringResource(R.string.licenses), onBack = onBack) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 360.dp),
            modifier = Modifier.fillMaxSize(),
            contentPadding = contentPadding(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            items(licenses) { license ->
                SuyuRow(
                    title = stringResource(license.titleId),
                    description = stringResource(license.descriptionId),
                    onClick = { onOpen(license) }
                )
            }
        }
    }
}
