// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.games

import android.text.format.Formatter
import androidx.core.content.edit
import androidx.core.net.toUri
import androidx.documentfile.provider.DocumentFile
import androidx.preference.PreferenceManager
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.model.Game

enum class Compat(val labelRes: Int) {
    PERFECT(R.string.compat_perfect),
    GREAT(R.string.compat_great),
    PLAYABLE(R.string.compat_playable)
}

/**
 * Local, per-game library metadata. Favorites are chosen by the user; the average FPS is measured
 * during emulation and the compatibility badge is derived from it, so a game shows neither until
 * it has been played.
 */
object GameMeta {
    private val preferences
        get() = PreferenceManager.getDefaultSharedPreferences(YuzuApplication.appContext)

    private val sizeCache = HashMap<String, Long>()

    fun loadFavorites(games: List<Game>): Set<String> =
        games.filter { preferences.getBoolean(it.keyFavorite, false) }.map { it.path }.toSet()

    fun setFavorite(game: Game, favorite: Boolean) =
        preferences.edit { putBoolean(game.keyFavorite, favorite) }

    fun averageFps(game: Game): Float? =
        preferences.getFloat(game.keyAverageFps, 0f).takeIf { it > 0f }

    fun recordAverageFps(game: Game, fps: Float) {
        if (fps > 0f) {
            preferences.edit { putFloat(game.keyAverageFps, fps) }
        }
    }

    fun compatFor(fps: Float?): Compat? = when {
        fps == null -> null
        fps >= 55f -> Compat.PERFECT
        fps >= 25f -> Compat.GREAT
        else -> Compat.PLAYABLE
    }

    fun lastPlayed(game: Game): Long = preferences.getLong(game.keyLastPlayedTime, 0L)

    fun addedToLibrary(game: Game): Long = preferences.getLong(game.keyAddedToLibraryTime, 0L)

    /** Blocking: call off the main thread. */
    fun sizeLabel(game: Game): String? {
        val bytes = synchronized(sizeCache) { sizeCache[game.path] } ?: run {
            val length = DocumentFile.fromSingleUri(
                YuzuApplication.appContext,
                game.path.toUri()
            )?.length() ?: 0L
            synchronized(sizeCache) { sizeCache[game.path] = length }
            length
        }
        return if (bytes > 0) Formatter.formatShortFileSize(YuzuApplication.appContext, bytes) else null
    }
}
