// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.games

import android.content.DialogInterface
import android.text.Html
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.edit
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.net.toUri
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import androidx.navigation.NavDirections
import androidx.preference.PreferenceManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.HomeNavigationDirections
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.GamesViewModel
import org.yuzu.yuzu_emu.utils.GameIconUtils

/** Launch logic used by the library screen. */
object GameLauncher {
    fun launch(activity: AppCompatActivity, game: Game, navigate: (NavDirections) -> Unit) {
        val gameExists = DocumentFile.fromSingleUri(
            YuzuApplication.appContext,
            game.path.toUri()
        )?.exists() == true

        if (!gameExists) {
            Toast.makeText(
                YuzuApplication.appContext,
                R.string.loader_error_file_not_found,
                Toast.LENGTH_LONG
            ).show()
            ViewModelProvider(activity)[GamesViewModel::class.java].reloadGames(true)
            return
        }

        val start: () -> Unit = {
            PreferenceManager.getDefaultSharedPreferences(YuzuApplication.appContext).edit {
                putLong(game.keyLastPlayedTime, System.currentTimeMillis())
            }

            activity.lifecycleScope.launch {
                withContext(Dispatchers.IO) {
                    val shortcut = ShortcutInfoCompat.Builder(YuzuApplication.appContext, game.path)
                        .setShortLabel(game.title)
                        .setIcon(GameIconUtils.getShortcutIcon(activity, game))
                        .setIntent(game.launchIntent)
                        .build()
                    ShortcutManagerCompat.pushDynamicShortcut(YuzuApplication.appContext, shortcut)
                }
            }

            navigate(HomeNavigationDirections.actionGlobalEmulationActivity(game, true))
        }

        if (NativeLibrary.gameRequiresFirmware(game.programId) && !NativeLibrary.isFirmwareAvailable()) {
            MaterialAlertDialogBuilder(activity)
                .setTitle(R.string.loader_requires_firmware)
                .setMessage(
                    Html.fromHtml(
                        activity.getString(R.string.loader_requires_firmware_description),
                        Html.FROM_HTML_MODE_LEGACY
                    )
                )
                .setPositiveButton(android.R.string.ok) { _: DialogInterface?, _: Int -> start() }
                .setNegativeButton(android.R.string.cancel) { _, _ -> }
                .show()
        } else {
            start()
        }
    }

    fun openProperties(game: Game, navigate: (NavDirections) -> Unit) =
        navigate(HomeNavigationDirections.actionGlobalPerGamePropertiesFragment(game))
}
