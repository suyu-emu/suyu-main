// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.fragments

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.getValue
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.transition.MaterialSharedAxis
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.DirectoryType
import org.yuzu.yuzu_emu.model.GameDir
import org.yuzu.yuzu_emu.model.GamesViewModel
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.ui.compose.common.suyuComposeView
import org.yuzu.yuzu_emu.ui.compose.screens.GameFoldersScreen

class GameFoldersFragment : Fragment() {
    private val homeViewModel: HomeViewModel by activityViewModels()
    private val gamesViewModel: GamesViewModel by activityViewModels()

    private val getGamesDirectory =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { result ->
            if (result != null) {
                processGamesDir(result)
            }
        }

    private val getExternalContentDirectory =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { result ->
            if (result != null) {
                processExternalContentDir(result)
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)

        gamesViewModel.onOpenGameFoldersFragment()
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View = suyuComposeView {
        val folders by gamesViewModel.folders.collectAsStateWithLifecycle()
        GameFoldersScreen(
            folders = folders,
            onBack = { requireActivity().onBackPressedDispatcher.onBackPressed() },
            onAdd = ::showAddFolderChooser,
            onEdit = { folder ->
                GameFolderPropertiesDialogFragment.newInstance(folder)
                    .show(requireActivity().supportFragmentManager, GameFolderPropertiesDialogFragment.TAG)
            },
            onDelete = { gamesViewModel.removeFolder(it) }
        )
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(visible = false)
    }

    override fun onStop() {
        super.onStop()
        gamesViewModel.onCloseGameFoldersFragment()
    }

    private fun showAddFolderChooser() {
        // Choose between a games folder and an external content folder
        val options = arrayOf(
            getString(R.string.games),
            getString(R.string.external_content)
        )

        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.add_folders)
            .setItems(options) { _, which ->
                when (which) {
                    0 -> getGamesDirectory.launch(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).data)
                    1 -> getExternalContentDirectory.launch(null)
                }
            }
            .show()
    }

    private fun processGamesDir(result: Uri) {
        requireContext().contentResolver.takePersistableUriPermission(
            result,
            Intent.FLAG_GRANT_READ_URI_PERMISSION
        )

        val uriString = result.toString()
        val folder = gamesViewModel.folders.value.firstOrNull { it.uriString == uriString }
        if (folder != null) {
            Toast.makeText(
                requireContext().applicationContext,
                R.string.folder_already_added,
                Toast.LENGTH_SHORT
            ).show()
            return
        }

        AddGameFolderDialogFragment.newInstance(uriString, calledFromGameFragment = false)
            .show(parentFragmentManager, AddGameFolderDialogFragment.TAG)
    }

    private fun processExternalContentDir(result: Uri) {
        requireContext().contentResolver.takePersistableUriPermission(
            result,
            Intent.FLAG_GRANT_READ_URI_PERMISSION
        )

        val uriString = result.toString()
        val folder = gamesViewModel.folders.value.firstOrNull {
            it.uriString == uriString && it.type == DirectoryType.EXTERNAL_CONTENT
        }
        if (folder != null) {
            Toast.makeText(
                requireContext().applicationContext,
                R.string.folder_already_added,
                Toast.LENGTH_SHORT
            ).show()
            return
        }

        val externalContentDir = GameDir(uriString, deepScan = false, DirectoryType.EXTERNAL_CONTENT)
        gamesViewModel.addFolder(externalContentDir, savedFromGameFragment = false)
    }
}
