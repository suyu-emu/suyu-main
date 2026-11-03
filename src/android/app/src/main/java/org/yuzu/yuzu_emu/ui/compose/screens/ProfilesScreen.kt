// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.screens

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.UserProfile
import org.yuzu.yuzu_emu.ui.compose.common.SuyuFab
import org.yuzu.yuzu_emu.ui.compose.common.SuyuIconButton
import org.yuzu.yuzu_emu.ui.compose.common.SuyuRow
import org.yuzu.yuzu_emu.ui.compose.common.SuyuScreen
import org.yuzu.yuzu_emu.ui.compose.common.contentPadding
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors

private fun formatUuid(uuid: String): String {
    if (uuid.length != 32) return uuid
    return listOf(0 to 8, 8 to 12, 12 to 16, 16 to 20, 20 to 32)
        .joinToString("-") { (from, to) -> uuid.substring(from, to) }
}

@Composable
fun ProfilesScreen(
    profiles: List<UserProfile>,
    currentUuid: String,
    canCreate: Boolean,
    onBack: () -> Unit,
    onSelect: (UserProfile) -> Unit,
    onEdit: (UserProfile) -> Unit,
    onDelete: (UserProfile) -> Unit,
    onAdd: () -> Unit
) {
    SuyuScreen(
        title = stringResource(R.string.profile_manager),
        onBack = onBack,
        fab = { SuyuFab(R.drawable.ic_add, stringResource(R.string.profile_manager), onAdd) }
    ) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 420.dp),
            modifier = Modifier.fillMaxSize(),
            contentPadding = contentPadding(extraBottom = 80.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            items(profiles, key = { it.uuid }) { profile ->
                val selected = profile.uuid == currentUuid
                SuyuRow(
                    title = profile.username,
                    description = formatUuid(profile.uuid),
                    details = if (selected) "✓" else null,
                    onClick = { onSelect(profile) },
                    leading = { Avatar(profile, selected) },
                    trailing = {
                        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            SuyuIconButton(R.drawable.ic_edit, stringResource(R.string.edit), { onEdit(profile) }, size = 44.dp)
                            SuyuIconButton(R.drawable.ic_delete, stringResource(R.string.delete), { onDelete(profile) }, size = 44.dp)
                        }
                    }
                )
            }
        }
    }
}

@Composable
private fun Avatar(profile: UserProfile, selected: Boolean) {
    val bitmap by produceState<Bitmap?>(null, profile.imagePath) {
        value = withContext(Dispatchers.IO) {
            if (File(profile.imagePath).exists()) {
                BitmapFactory.decodeFile(profile.imagePath)
            } else {
                val jpeg = NativeLibrary.getDefaultAccountBackupJpeg()
                BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size)
            }
        }
    }
    bitmap?.let {
        Image(
            bitmap = it.asImageBitmap(),
            contentDescription = null,
            contentScale = ContentScale.Crop,
            modifier = Modifier
                .size(56.dp)
                .clip(CircleShape)
                .border(2.dp, if (selected) SuyuColors.Accent else SuyuColors.Outline, CircleShape)
        )
    }
}
