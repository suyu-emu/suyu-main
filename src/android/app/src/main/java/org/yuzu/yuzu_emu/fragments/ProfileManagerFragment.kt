// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.fragments

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.navigation.fragment.findNavController
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.transition.MaterialSharedAxis
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.model.UserProfile
import org.yuzu.yuzu_emu.ui.compose.common.suyuComposeView
import org.yuzu.yuzu_emu.ui.compose.screens.ProfilesScreen
import org.yuzu.yuzu_emu.utils.NativeConfig

class ProfileManagerFragment : Fragment() {
    private val homeViewModel: HomeViewModel by activityViewModels()

    private var profiles by mutableStateOf<List<UserProfile>>(emptyList())
    private var currentUuid by mutableStateOf("")
    private var canCreate by mutableStateOf(true)

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
    ): View = suyuComposeView {
        ProfilesScreen(
            profiles = profiles,
            currentUuid = currentUuid,
            canCreate = canCreate,
            onBack = { requireActivity().onBackPressedDispatcher.onBackPressed() },
            onSelect = ::selectProfile,
            onEdit = ::editProfile,
            onDelete = ::confirmDeleteProfile,
            onAdd = ::addProfile
        )
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(visible = false)
        loadProfiles()
    }

    override fun onResume() {
        super.onResume()
        loadProfiles()
    }

    private fun addProfile() {
        if (NativeLibrary.canCreateUser()) {
            findNavController().navigate(R.id.action_profileManagerFragment_to_newUserDialog)
        } else {
            MaterialAlertDialogBuilder(requireContext())
                .setTitle(R.string.profile_max_users_title)
                .setMessage(R.string.profile_max_users_message)
                .setPositiveButton(android.R.string.ok, null)
                .show()
        }
    }

    private fun loadProfiles() {
        val list = mutableListOf<UserProfile>()
        val userUUIDs = NativeLibrary.getAllUsers() ?: emptyArray()

        for (uuid in userUUIDs) {
            if (uuid.isNotEmpty()) {
                val username = NativeLibrary.getUserUsername(uuid)
                if (!username.isNullOrEmpty()) {
                    val imagePath = NativeLibrary.getUserImagePath(uuid) ?: ""
                    list.add(UserProfile(uuid, username, imagePath))
                }
            }
        }

        profiles = list
        currentUuid = NativeLibrary.getCurrentUser() ?: ""
        canCreate = NativeLibrary.canCreateUser()
    }

    private fun selectProfile(profile: UserProfile) {
        if (NativeLibrary.setCurrentUser(profile.uuid)) {
            loadProfiles()
        }
    }

    private fun editProfile(profile: UserProfile) {
        val bundle = Bundle().apply {
            putString("uuid", profile.uuid)
            putString("username", profile.username)
        }
        findNavController().navigate(R.id.action_profileManagerFragment_to_newUserDialog, bundle)
    }

    private fun confirmDeleteProfile(profile: UserProfile) {
        val isCurrentUser = profile.uuid == NativeLibrary.getCurrentUser()

        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.profile_delete_confirm_title)
            .setMessage(
                if (isCurrentUser) {
                    getString(R.string.profile_delete_current_user_message, profile.username)
                } else {
                    getString(R.string.profile_delete_confirm_message, profile.username)
                }
            )
            .setPositiveButton(R.string.profile_delete) { _, _ -> deleteProfile(profile) }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun deleteProfile(profile: UserProfile) {
        val currentUser = NativeLibrary.getCurrentUser()
        if (!currentUser.isNullOrEmpty() && profile.uuid == currentUser) {
            val users = NativeLibrary.getAllUsers() ?: emptyArray()
            for (uuid in users) {
                if (uuid.isNotEmpty() && uuid != profile.uuid) {
                    NativeLibrary.setCurrentUser(uuid)
                    break
                }
            }
        }

        if (NativeLibrary.removeUser(profile.uuid)) {
            loadProfiles()
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        NativeConfig.saveGlobalConfig()
    }
}
