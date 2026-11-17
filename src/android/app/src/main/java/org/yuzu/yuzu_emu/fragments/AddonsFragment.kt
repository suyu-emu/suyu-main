// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.documentfile.provider.DocumentFile
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.navigation.fragment.navArgs
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.transition.MaterialSharedAxis
import org.yuzu.yuzu_emu.R
import androidx.compose.runtime.getValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.yuzu.yuzu_emu.ui.compose.common.suyuComposeView
import org.yuzu.yuzu_emu.ui.compose.screens.AddonsScreen
import org.yuzu.yuzu_emu.model.AddonViewModel
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.model.PatchType
import org.yuzu.yuzu_emu.utils.AddonUtil
import org.yuzu.yuzu_emu.utils.FileUtil.copyFilesTo
import org.yuzu.yuzu_emu.utils.InstallableActions
import org.yuzu.yuzu_emu.utils.ViewUtils.updateMargins
import org.yuzu.yuzu_emu.utils.collect
import java.io.File

class AddonsFragment : Fragment() {
    private val homeViewModel: HomeViewModel by activityViewModels()
    private val addonViewModel: AddonViewModel by activityViewModels()

    private val args by navArgs<AddonsFragmentArgs>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        addonViewModel.onAddonsViewCreated(args.game)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View = suyuComposeView {
        val addons by addonViewModel.addonList.collectAsStateWithLifecycle()
        AddonsScreen(
            title = getString(R.string.addons_game, args.game.title),
            addons = addons,
            onBack = { requireActivity().onBackPressedDispatcher.onBackPressed() },
            onToggle = { patch, checked ->
                if (PatchType.from(patch.type) == PatchType.Update && checked) {
                    addonViewModel.enableOnlyThisUpdate(patch)
                } else {
                    patch.enabled = checked
                }
            },
            onDelete = { addonViewModel.setAddonToDelete(it) },
            onInstall = {
                ContentTypeSelectionDialogFragment().show(
                    parentFragmentManager,
                    ContentTypeSelectionDialogFragment.TAG
                )
            }
        )
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(false)

        addonViewModel.showModInstallPicker.collect(
            viewLifecycleOwner,
            resetState = { addonViewModel.showModInstallPicker(false) }
        ) { if (it) installAddon.launch(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).data) }
        addonViewModel.showModNoticeDialog.collect(
            viewLifecycleOwner,
            resetState = { addonViewModel.showModNoticeDialog(false) }
        ) {
            if (it) {
                MessageDialogFragment.newInstance(
                    requireActivity(),
                    titleId = R.string.addon_notice,
                    descriptionId = R.string.addon_notice_description,
                    dismissible = false,
                    positiveAction = { addonViewModel.showModInstallPicker(true) },
                    negativeAction = {},
                    negativeButtonTitleId = R.string.close
                ).show(parentFragmentManager, MessageDialogFragment.TAG)
            }
        }
        addonViewModel.addonToDelete.collect(
            viewLifecycleOwner,
            resetState = { addonViewModel.setAddonToDelete(null) }
        ) {
            if (it != null) {
                MessageDialogFragment.newInstance(
                    requireActivity(),
                    titleId = R.string.confirm_uninstall,
                    descriptionId = R.string.confirm_uninstall_description,
                    positiveAction = { addonViewModel.onDeleteAddon(it) },
                    negativeAction = {}
                ).show(parentFragmentManager, MessageDialogFragment.TAG)
            }
        }
        parentFragmentManager.setFragmentResultListener(
            ContentTypeSelectionDialogFragment.REQUEST_INSTALL_GAME_UPDATE,
            viewLifecycleOwner
        ) { _, _ ->
            installGameUpdate.launch(arrayOf("*/*"))
        }
    }

    override fun onResume() {
        super.onResume()
        addonViewModel.onAddonsViewStarted(args.game)
    }

    override fun onDestroy() {
        if (activity?.isChangingConfigurations != true) {
            addonViewModel.onCloseAddons()
        }
        super.onDestroy()
    }

    private val installAddon =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { result ->
            if (result == null) {
                return@registerForActivityResult
            }

            val externalAddonDirectory = DocumentFile.fromTreeUri(requireContext(), result)
            if (externalAddonDirectory == null) {
                MessageDialogFragment.newInstance(
                    requireActivity(),
                    titleId = R.string.invalid_directory,
                    descriptionId = R.string.invalid_directory_description
                ).show(parentFragmentManager, MessageDialogFragment.TAG)
                return@registerForActivityResult
            }

            val isValid = externalAddonDirectory.listFiles()
                .any { AddonUtil.validAddonDirectories.contains(it.name?.lowercase()) }
            val errorMessage = MessageDialogFragment.newInstance(
                requireActivity(),
                titleId = R.string.invalid_directory,
                descriptionId = R.string.invalid_directory_description
            )
            if (isValid) {
                ProgressDialogFragment.newInstance(
                    requireActivity(),
                    R.string.installing_game_content,
                    false
                ) { progressCallback, _ ->
                    val parentDirectoryName = externalAddonDirectory.name
                    val internalAddonDirectory =
                        File(args.game.addonDir + parentDirectoryName)
                    try {
                        externalAddonDirectory.copyFilesTo(internalAddonDirectory, progressCallback)
                    } catch (_: Exception) {
                        return@newInstance errorMessage
                    }
                    addonViewModel.refreshAddons(force = true)
                    return@newInstance getString(R.string.addon_installed_successfully)
                }.show(parentFragmentManager, ProgressDialogFragment.TAG)
            } else {
                errorMessage.show(parentFragmentManager, MessageDialogFragment.TAG)
            }
        }

    private val installGameUpdate =
        registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { documents ->
            InstallableActions.verifyAndInstallContent(
                activity = requireActivity(),
                fragmentManager = parentFragmentManager,
                addonViewModel = addonViewModel,
                documents = documents,
                programId = args.game.programId
            )
        }
}
