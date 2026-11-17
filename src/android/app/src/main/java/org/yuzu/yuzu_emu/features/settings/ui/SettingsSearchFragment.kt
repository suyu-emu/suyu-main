// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.features.settings.ui

import android.content.Context
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalView
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.transition.MaterialSharedAxis
import info.debatty.java.stringsimilarity.Cosine
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem
import org.yuzu.yuzu_emu.ui.compose.common.suyuComposeView
import org.yuzu.yuzu_emu.ui.compose.settings.SettingsSearchScreen
import org.yuzu.yuzu_emu.utils.NativeConfig
import org.yuzu.yuzu_emu.utils.collect

class SettingsSearchFragment : Fragment() {
    private var settingsAdapter: SettingsAdapter? = null
    private var revision by mutableIntStateOf(0)
    private var query by mutableStateOf("")

    private val settingsViewModel: SettingsViewModel by activityViewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        exitTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        query = savedInstanceState?.getString(SEARCH_TEXT).orEmpty()
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        val adapter = SettingsAdapter(this, requireContext())
        settingsAdapter = adapter
        adapter.registerAdapterDataObserver(object : RecyclerView.AdapterDataObserver() {
            override fun onChanged() { revision++ }
            override fun onItemRangeChanged(positionStart: Int, itemCount: Int) { revision++ }
            override fun onItemRangeChanged(positionStart: Int, itemCount: Int, payload: Any?) { revision++ }
            override fun onItemRangeInserted(positionStart: Int, itemCount: Int) { revision++ }
            override fun onItemRangeRemoved(positionStart: Int, itemCount: Int) { revision++ }
            override fun onItemRangeMoved(fromPosition: Int, toPosition: Int, itemCount: Int) { revision++ }
        })

        return suyuComposeView {
            val anchor = LocalView.current
            val rows = remember(revision) { SettingsRows.build(adapter, anchor) }
            SettingsSearchScreen(
                query = query,
                onQueryChange = {
                    query = it
                    search()
                },
                rows = rows,
                onBack = { settingsViewModel.setShouldNavigateBack(true) }
            )
        }
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        settingsViewModel.shouldReloadSettingsList.collect(viewLifecycleOwner) {
            if (it) {
                settingsViewModel.setShouldReloadSettingsList(false)
                search()
            }
        }
        search()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(SEARCH_TEXT, query)
    }

    private fun search() {
        val searchTerm = query.lowercase()
        if (searchTerm.isEmpty()) {
            settingsAdapter?.submitList(emptyList())
            return
        }

        val baseList = SettingsItem.settingsItems
        val similarityAlgorithm = if (searchTerm.length > 2) Cosine() else Cosine(1)
        val sortedList: List<SettingsItem> = baseList.mapNotNull { item ->
            val title = item.value.title.lowercase()
            val similarity = similarityAlgorithm.similarity(searchTerm, title)
            if (similarity > 0.08) {
                Pair(similarity, item)
            } else {
                null
            }
        }.sortedByDescending { it.first }.mapNotNull {
            val item = it.second.value
            val pairedSettingKey = item.setting.pairedSettingKey
            val optionalSetting: SettingsItem? = if (pairedSettingKey.isNotEmpty()) {
                val pairedSettingValue = NativeConfig.getBoolean(pairedSettingKey, false)
                if (pairedSettingValue) it.second.value else null
            } else {
                it.second.value
            }
            optionalSetting
        }
        settingsAdapter?.submitList(sortedList)
    }

    companion object {
        const val SEARCH_TEXT = "SearchText"
    }
}
