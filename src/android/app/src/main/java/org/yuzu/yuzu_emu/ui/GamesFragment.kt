// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.ViewCompositionStrategy
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LifecycleEventEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavDirections
import androidx.navigation.fragment.findNavController
import org.yuzu.yuzu_emu.HomeNavigationDirections
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.features.settings.model.BooleanSetting
import org.yuzu.yuzu_emu.features.settings.model.Settings
import org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreen
import org.yuzu.yuzu_emu.model.AppletInfo
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.GamesViewModel
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.ui.compose.games.GameLauncher
import org.yuzu.yuzu_emu.ui.compose.games.GameMeta
import org.yuzu.yuzu_emu.ui.compose.games.GamesScreen
import org.yuzu.yuzu_emu.ui.compose.games.LibraryDestination
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuTheme
import org.yuzu.yuzu_emu.ui.main.MainActivity

/**
 * Game library. The UI is Jetpack Compose (see [GamesScreen]); this fragment only wires it to the
 * existing view models and navigation graph.
 */
class GamesFragment : Fragment() {
    private val gamesViewModel: GamesViewModel by activityViewModels()
    private val homeViewModel: HomeViewModel by activityViewModels()

    private lateinit var mainActivity: MainActivity

    private val getGamesDirectory =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { result ->
            if (result != null) {
                mainActivity.processGamesDir(result, true)
            }
        }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        homeViewModel.setStatusBarShadeVisibility(true)
        mainActivity = requireActivity() as MainActivity

        return ComposeView(requireContext()).apply {
            setViewCompositionStrategy(ViewCompositionStrategy.DisposeOnViewTreeLifecycleDestroyed)
            setContent {
                SuyuTheme {
                    val games by gamesViewModel.games.collectAsStateWithLifecycle()
                    val isReloading by gamesViewModel.isReloading.collectAsStateWithLifecycle()
                    var favorites by remember(games) { mutableStateOf(GameMeta.loadFavorites(games)) }
                    var refreshTick by remember { mutableIntStateOf(0) }

                    // Played time, FPS and favorites change while the emulator is in front.
                    LifecycleEventEffect(Lifecycle.Event.ON_RESUME) { refreshTick++ }

                    val navigate: (NavDirections) -> Unit = { findNavController().navigate(it) }

                    GamesScreen(
                        games = games,
                        favorites = favorites,
                        isRefreshing = isReloading,
                        refreshTick = refreshTick,
                        showQLaunch = BooleanSetting.ENABLE_QLAUNCH_BUTTON.getBoolean() &&
                            NativeLibrary.isFirmwareAvailable(),
                        showAddFolder = BooleanSetting.ENABLE_FOLDER_BUTTON.getBoolean(),
                        onRefresh = { gamesViewModel.reloadGames(false) },
                        onAddFolder = {
                            getGamesDirectory.launch(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).data)
                        },
                        onQLaunch = ::launchQLaunch,
                        onPlay = { GameLauncher.launch(requireActivity() as AppCompatActivity, it, navigate) },
                        onProperties = { GameLauncher.openProperties(it, navigate) },
                        onToggleFavorite = { game ->
                            val makeFavorite = game.path !in favorites
                            GameMeta.setFavorite(game, makeFavorite)
                            favorites = if (makeFavorite) favorites + game.path else favorites - game.path
                        },
                        onNavigate = { destination ->
                            when (destination) {
                                LibraryDestination.LIBRARY -> Unit
                                LibraryDestination.CONTROLS -> navigate(
                                    HomeNavigationDirections.actionGlobalSettingsActivity(
                                        null,
                                        Settings.MenuTag.SECTION_INPUT
                                    )
                                )
                                LibraryDestination.DRIVERS -> navigate(
                                    HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(
                                        SettingsSubscreen.DRIVER_MANAGER,
                                        null
                                    )
                                )
                                LibraryDestination.SETTINGS -> findNavController()
                                    .navigate(R.id.action_gamesFragment_to_homeSettingsFragment)
                            }
                        }
                    )
                }
            }
        }
    }

    private fun launchQLaunch() {
        try {
            val appletPath = NativeLibrary.getAppletLaunchPath(AppletInfo.QLaunch.entryId)
            if (appletPath.isEmpty()) {
                Toast.makeText(
                    requireContext(),
                    R.string.applets_error_applet,
                    Toast.LENGTH_SHORT
                ).show()
                return
            }

            NativeLibrary.setCurrentAppletId(AppletInfo.QLaunch.appletId)

            val qlaunchGame = Game(
                title = getString(R.string.qlaunch_applet),
                path = appletPath
            )

            findNavController().navigate(HomeNavigationDirections.actionGlobalEmulationActivity(qlaunchGame))
        } catch (e: Exception) {
            Toast.makeText(
                requireContext(),
                "Failed to launch QLaunch: ${e.message}",
                Toast.LENGTH_SHORT
            ).show()
        }
    }
}
