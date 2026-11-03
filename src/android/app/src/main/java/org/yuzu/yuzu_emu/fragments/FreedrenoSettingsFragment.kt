// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.fragment.app.Fragment
import androidx.navigation.fragment.navArgs
import com.google.android.material.snackbar.Snackbar
import com.google.android.material.transition.MaterialSharedAxis
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.ui.compose.common.suyuComposeView
import org.yuzu.yuzu_emu.ui.compose.screens.FreedrenoScreen
import org.yuzu.yuzu_emu.utils.FreedrenoPreset
import org.yuzu.yuzu_emu.utils.FreedrenoPresets
import org.yuzu.yuzu_emu.utils.NativeFreedrenoConfig

class FreedrenoSettingsFragment : Fragment() {
    private val args by navArgs<FreedrenoSettingsFragmentArgs>()
    private val game: Game? get() = args.game
    private val isPerGameConfig: Boolean get() = game != null

    private var variables by mutableStateOf<List<FreedrenoVariable>>(emptyList())

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        exitTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View = suyuComposeView {
        FreedrenoScreen(
            title = getString(
                if (isPerGameConfig) R.string.freedreno_per_game_title else R.string.freedreno_settings_title
            ),
            presets = FreedrenoPresets.ALL_PRESETS,
            variables = variables,
            onBack = { requireActivity().onBackPressedDispatcher.onBackPressed() },
            onPreset = ::applyPreset,
            onDelete = { variable ->
                NativeFreedrenoConfig.clearFreedrenoEnv(variable.name)
                loadCurrentSettings()
            },
            onAdd = ::addVariable,
            onClearAll = {
                NativeFreedrenoConfig.clearAllFreedrenoEnv()
                showSnackbar(getString(R.string.freedreno_cleared_all))
                loadCurrentSettings()
            },
            onSave = {
                if (isPerGameConfig) {
                    NativeFreedrenoConfig.savePerGameConfig(game!!.programIdHex)
                    showSnackbar(getString(R.string.freedreno_per_game_saved))
                } else {
                    NativeFreedrenoConfig.saveFreedrenoConfig()
                    showSnackbar(getString(R.string.freedreno_saved))
                }
            }
        )
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        NativeFreedrenoConfig.setFreedrenoBasePath(requireContext().cacheDir.absolutePath)
        NativeFreedrenoConfig.initializeFreedrenoConfig()
        if (isPerGameConfig) {
            NativeFreedrenoConfig.loadPerGameConfig(game!!.programIdHex)
        } else {
            NativeFreedrenoConfig.reloadFreedrenoConfig()
        }
        loadCurrentSettings()
    }

    private fun loadCurrentSettings() {
        // Load all currently set environment variables
        val list = mutableListOf<FreedrenoVariable>()
        val commonVars = listOf(
            "TU_DEBUG", "FD_MESA_DEBUG", "IR3_SHADER_DEBUG",
            "FD_RD_DUMP", "FD_RD_DUMP_FRAMES", "FD_RD_DUMP_TESTNAME",
            "TU_BREADCRUMBS", "FD_DEV_FEATURES"
        )
        for (varName in commonVars) {
            if (NativeFreedrenoConfig.isFreedrenoEnvSet(varName)) {
                list.add(FreedrenoVariable(varName, NativeFreedrenoConfig.getFreedrenoEnv(varName)))
            }
        }
        variables = list
    }

    private fun addVariable(name: String, value: String): Boolean {
        if (name.isEmpty()) {
            showSnackbar(getString(R.string.freedreno_error_empty_name))
            return false
        }
        return if (NativeFreedrenoConfig.setFreedrenoEnv(name, value)) {
            showSnackbar(getString(R.string.freedreno_variable_added, name))
            loadCurrentSettings()
            true
        } else {
            showSnackbar(getString(R.string.freedreno_error_setting_variable))
            false
        }
    }

    private fun applyPreset(preset: FreedrenoPreset) {
        // Clear all first for consistency, then apply every variable in the preset
        NativeFreedrenoConfig.clearAllFreedrenoEnv()
        for ((varName, varValue) in preset.variables) {
            NativeFreedrenoConfig.setFreedrenoEnv(varName, varValue)
        }
        showSnackbar(getString(R.string.freedreno_preset_applied, preset.name))
        loadCurrentSettings()
    }

    private fun showSnackbar(message: String) {
        Snackbar.make(requireView(), message, Snackbar.LENGTH_SHORT).show()
    }
}

/**
 * Data class representing a Freedreno environment variable.
 */
data class FreedrenoVariable(
    val name: String,
    val value: String
)
