// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.fragments

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.appcompat.app.AppCompatActivity
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import com.google.android.material.transition.MaterialSharedAxis
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.model.License
import org.yuzu.yuzu_emu.ui.compose.common.suyuComposeView
import org.yuzu.yuzu_emu.ui.compose.screens.LicensesScreen

class LicensesFragment : Fragment() {
    private val homeViewModel: HomeViewModel by activityViewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        val licenses = listOf(
            License(
                R.string.license_fidelityfx_fsr,
                R.string.license_fidelityfx_fsr_description,
                R.string.license_fidelityfx_fsr_link,
                R.string.license_fidelityfx_fsr_copyright,
                R.string.license_fidelityfx_fsr_text
            ),
            License(
                R.string.license_cubeb,
                R.string.license_cubeb_description,
                R.string.license_cubeb_link,
                R.string.license_cubeb_copyright,
                R.string.license_cubeb_text
            ),
            License(
                R.string.license_dynarmic,
                R.string.license_dynarmic_description,
                R.string.license_dynarmic_link,
                R.string.license_dynarmic_copyright,
                R.string.license_dynarmic_text
            ),
            License(
                R.string.license_ffmpeg,
                R.string.license_ffmpeg_description,
                R.string.license_ffmpeg_link,
                R.string.license_ffmpeg_copyright,
                R.string.license_ffmpeg_text
            ),
            License(
                R.string.license_opus,
                R.string.license_opus_description,
                R.string.license_opus_link,
                R.string.license_opus_copyright,
                R.string.license_opus_text
            ),
            License(
                R.string.license_sirit,
                R.string.license_sirit_description,
                R.string.license_sirit_link,
                R.string.license_sirit_copyright,
                R.string.license_sirit_text
            ),
            License(
                R.string.license_adreno_tools,
                R.string.license_adreno_tools_description,
                R.string.license_adreno_tools_link,
                R.string.license_adreno_tools_copyright,
                R.string.license_adreno_tools_text
            )
        )

        return suyuComposeView {
            LicensesScreen(
                licenses = licenses,
                onBack = { requireActivity().onBackPressedDispatcher.onBackPressed() },
                onOpen = { license ->
                    LicenseBottomSheetDialogFragment.newInstance(license).show(
                        (requireActivity() as AppCompatActivity).supportFragmentManager,
                        LicenseBottomSheetDialogFragment.TAG
                    )
                }
            )
        }
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        homeViewModel.setStatusBarShadeVisibility(visible = false)
    }
}
