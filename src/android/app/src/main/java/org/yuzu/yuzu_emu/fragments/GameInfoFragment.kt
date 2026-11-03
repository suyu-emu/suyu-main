// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.navigation.fragment.navArgs
import com.google.android.material.transition.MaterialSharedAxis
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.ui.compose.common.suyuComposeView
import org.yuzu.yuzu_emu.ui.compose.screens.GameInfoField
import org.yuzu.yuzu_emu.ui.compose.screens.GameInfoScreen
import org.yuzu.yuzu_emu.model.GameVerificationResult
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.utils.GameMetadata
import org.yuzu.yuzu_emu.utils.ViewUtils.setVisible
import org.yuzu.yuzu_emu.utils.ViewUtils.updateMargins

class GameInfoFragment : Fragment() {
    private val homeViewModel: HomeViewModel by activityViewModels()

    private val args by navArgs<GameInfoFragmentArgs>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)

        // Check for an up-to-date version string
        args.game.version = GameMetadata.getVersion(args.game.path, true)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        val game = args.game
        val pathString = Uri.parse(game.path).path ?: ""
        val fields = buildList {
            add(GameInfoField(R.string.path, pathString))
            add(GameInfoField(R.string.program_id, game.programIdHex))
            if (game.developer.isNotEmpty()) {
                add(GameInfoField(R.string.developer, game.developer))
            }
            add(GameInfoField(R.string.version, game.version))
        }
        return suyuComposeView {
            GameInfoScreen(
                title = game.title,
                fields = fields,
                onBack = { requireActivity().onBackPressedDispatcher.onBackPressed() },
                onCopyField = { copyToClipboard(getString(it.label), it.value) },
                onCopyAll = {
                    val details = """
                        ${game.title}
                        ${getString(R.string.path)} - $pathString
                        ${getString(R.string.program_id)} - ${game.programIdHex}
                        ${getString(R.string.developer)} - ${game.developer}
                        ${getString(R.string.version)} - ${game.version}
                    """.trimIndent()
                    copyToClipboard(game.title, details)
                },
                onVerify = ::verifyIntegrity
            )
        }
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(false)
    }

    private fun verifyIntegrity() {
        ProgressDialogFragment.newInstance(
            requireActivity(),
            R.string.verifying,
            true
        ) { progressCallback, _ ->
            val result = GameVerificationResult.from(
                NativeLibrary.verifyGameContents(
                    args.game.path,
                    progressCallback
                )
            )
            return@newInstance when (result) {
                GameVerificationResult.Success ->
                    MessageDialogFragment.newInstance(
                        titleId = R.string.verify_success,
                        descriptionId = R.string.operation_completed_successfully
                    )

                GameVerificationResult.Failed ->
                    MessageDialogFragment.newInstance(
                        titleId = R.string.verify_failure,
                        descriptionId = R.string.verify_failure_description
                    )

                GameVerificationResult.NotImplemented ->
                    MessageDialogFragment.newInstance(
                        titleId = R.string.verify_no_result,
                        descriptionId = R.string.verify_no_result_description
                    )
            }
        }.show(parentFragmentManager, ProgressDialogFragment.TAG)
    }

    private fun copyToClipboard(label: String, body: String) {
        val clipBoard =
            requireContext().getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val clip = ClipData.newPlainText(label, body)
        clipBoard.setPrimaryClip(clip)

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            Toast.makeText(
                requireContext(),
                R.string.copied_to_clipboard,
                Toast.LENGTH_SHORT
            ).show()
        }
    }
}
