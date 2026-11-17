// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.fragments

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.core.os.bundleOf
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.navigation.fragment.findNavController
import com.google.android.material.transition.MaterialSharedAxis
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.model.Applet
import org.yuzu.yuzu_emu.model.AppletInfo
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.ui.compose.common.suyuComposeView
import org.yuzu.yuzu_emu.ui.compose.screens.AppletsScreen

class AppletLauncherFragment : Fragment() {
    private val homeViewModel: HomeViewModel by activityViewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        val applets = listOf(
            Applet(
                R.string.album_applet,
                R.string.album_applet_description,
                R.drawable.ic_album,
                AppletInfo.PhotoViewer
            ),
            Applet(
                R.string.cabinet_applet,
                R.string.cabinet_applet_description,
                R.drawable.ic_nfc,
                AppletInfo.Cabinet
            ),
            Applet(
                R.string.mii_edit_applet,
                R.string.mii_edit_applet_description,
                R.drawable.ic_mii,
                AppletInfo.MiiEdit
            ),
            Applet(
                R.string.qlaunch_applet,
                R.string.qlaunch_description,
                R.drawable.ic_home,
                AppletInfo.QLaunch
            )
        )
        return suyuComposeView {
            AppletsScreen(
                applets = applets,
                onBack = { requireActivity().onBackPressedDispatcher.onBackPressed() },
                onLaunch = ::launchApplet
            )
        }
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(visible = false)
    }

    private fun launchApplet(applet: Applet) {
        val appletPath = NativeLibrary.getAppletLaunchPath(applet.appletInfo.entryId)
        if (appletPath.isEmpty()) {
            Toast.makeText(requireContext(), R.string.applets_error_applet, Toast.LENGTH_SHORT).show()
            return
        }

        if (applet.appletInfo == AppletInfo.Cabinet) {
            findNavController().navigate(R.id.action_appletLauncherFragment_to_cabinetLauncherDialogFragment)
            return
        }

        NativeLibrary.setCurrentAppletId(applet.appletInfo.appletId)
        val appletGame = Game(
            title = YuzuApplication.appContext.getString(applet.titleId),
            path = appletPath
        )
        findNavController().navigate(
            R.id.action_global_emulationActivity,
            bundleOf(
                "game" to appletGame,
                "custom" to false
            )
        )
    }
}
