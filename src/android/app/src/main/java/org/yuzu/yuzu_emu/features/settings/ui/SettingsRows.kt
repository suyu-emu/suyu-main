// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.features.settings.ui

import android.view.View
import java.time.Instant
import java.time.ZoneId
import java.time.ZonedDateTime
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.features.input.NativeInput
import org.yuzu.yuzu_emu.features.settings.model.view.AnalogInputSetting
import org.yuzu.yuzu_emu.features.settings.model.view.ButtonInputSetting
import org.yuzu.yuzu_emu.features.settings.model.view.DateTimeSetting
import org.yuzu.yuzu_emu.features.settings.model.view.GpuUnswizzleSetting
import org.yuzu.yuzu_emu.features.settings.model.view.HeaderSetting
import org.yuzu.yuzu_emu.features.settings.model.view.InputProfileSetting
import org.yuzu.yuzu_emu.features.settings.model.view.InputSetting
import org.yuzu.yuzu_emu.features.settings.model.view.IntSingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.model.view.LaunchableSetting
import org.yuzu.yuzu_emu.features.settings.model.view.ModifierInputSetting
import org.yuzu.yuzu_emu.features.settings.model.view.PathSetting
import org.yuzu.yuzu_emu.features.settings.model.view.RunnableSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem
import org.yuzu.yuzu_emu.features.settings.model.view.SingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SliderSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SpinBoxSetting
import org.yuzu.yuzu_emu.features.settings.model.view.StringInputSetting
import org.yuzu.yuzu_emu.features.settings.model.view.StringSingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SubmenuSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SwitchSetting
import org.yuzu.yuzu_emu.utils.PathUtil

/** What the Compose list needs to draw one row of the settings screen. */
sealed interface SettingRow {
    data class Header(val title: String) : SettingRow

    class Item(
        val title: String,
        val description: String = "",
        val value: String? = null,
        val iconId: Int = 0,
        val editable: Boolean = true,
        val clearable: Boolean = false,
        val clearLabelId: Int = R.string.clear,
        val checked: Boolean? = null,
        val chevron: Boolean = false,
        val showOptions: Boolean = false,
        val onClick: () -> Unit = {},
        val onLongClick: () -> Unit = {},
        val onClear: () -> Unit = {},
        val onToggle: (Boolean) -> Unit = {},
        val onOptions: () -> Unit = {}
    ) : SettingRow
}

/**
 * Maps the existing [SettingsItem] list to [SettingRow]s. Every action goes through the same
 * [SettingsAdapter] handlers the RecyclerView version used, so dialogs and saving are unchanged.
 */
object SettingsRows {
    fun build(adapter: SettingsAdapter, anchor: View): List<SettingRow> =
        adapter.currentList.mapIndexed { index, item -> toRow(adapter, anchor, item, index) }

    private fun toRow(adapter: SettingsAdapter, anchor: View, item: SettingsItem, index: Int): SettingRow {
        val context = anchor.context
        val resources = context.resources
        val editable = item.isEditable
        val clearable = item.clearable
        val onClear = { adapter.onClearClick(item, index) }
        val onLongClick = { if (editable) adapter.onLongClick(item, index) }

        return when (item) {
            is HeaderSetting -> SettingRow.Header(item.title)

            is SwitchSetting -> SettingRow.Item(
                title = item.title,
                description = item.description,
                checked = item.getIsChecked(item.needsRuntimeGlobal),
                editable = editable,
                clearable = clearable,
                onToggle = { adapter.onBooleanClick(item, it, index) },
                onClear = { onClear() },
                onClick = { if (editable) adapter.onBooleanClick(item, !item.getIsChecked(item.needsRuntimeGlobal), index) },
                onLongClick = { onLongClick() }
            )

            is SingleChoiceSetting -> {
                val values = resources.getIntArray(item.valuesId)
                val choices = resources.getStringArray(item.choicesId)
                val selected = values.indexOf(item.getSelectedValue())
                SettingRow.Item(
                    title = item.title,
                    description = item.description,
                    value = if (selected >= 0) choices[selected] else null,
                    editable = editable,
                    clearable = clearable,
                    onClick = { if (editable) adapter.onSingleChoiceClick(item, index) },
                    onLongClick = { onLongClick() },
                    onClear = { onClear() }
                )
            }

            is StringSingleChoiceSetting -> SettingRow.Item(
                title = item.title,
                description = item.description,
                value = item.getSelectedValue(),
                editable = editable,
                clearable = clearable,
                onClick = { if (editable) adapter.onStringSingleChoiceClick(item, index) },
                onLongClick = { onLongClick() },
                onClear = { onClear() }
            )

            is IntSingleChoiceSetting -> SettingRow.Item(
                title = item.title,
                description = item.description,
                value = item.getChoiceAt(item.getSelectedValue()),
                editable = editable,
                clearable = clearable,
                onClick = { if (editable) adapter.onIntSingleChoiceClick(item, index) },
                onLongClick = { onLongClick() },
                onClear = { onClear() }
            )

            is SliderSetting -> SettingRow.Item(
                title = item.title,
                description = item.description,
                value = String.format(
                    context.getString(R.string.value_with_units),
                    item.getSelectedValue(),
                    item.units
                ),
                editable = editable,
                clearable = clearable,
                onClick = { if (editable) adapter.onSliderClick(item, index) },
                onLongClick = { onLongClick() },
                onClear = { onClear() }
            )

            is SpinBoxSetting -> SettingRow.Item(
                title = item.title,
                description = item.description,
                value = item.getSelectedValue().toString(),
                editable = editable,
                clearable = clearable,
                onClick = { if (editable) adapter.onSpinBoxClick(item, index) },
                onLongClick = { onLongClick() },
                onClear = { onClear() }
            )

            is SubmenuSetting -> SettingRow.Item(
                title = item.title,
                description = item.description,
                iconId = item.iconId,
                chevron = true,
                onClick = { adapter.onSubmenuClick(item) }
            )

            is DateTimeSetting -> {
                val instant = Instant.ofEpochMilli(item.getValue() * 1000)
                val zoned = ZonedDateTime.ofInstant(instant, ZoneId.of("UTC"))
                SettingRow.Item(
                    title = item.title,
                    description = item.description,
                    value = DateTimeFormatter.ofLocalizedDateTime(FormatStyle.MEDIUM).format(zoned),
                    editable = editable,
                    clearable = clearable,
                    onClick = { if (editable) adapter.onDateTimeClick(item, index) },
                    onLongClick = { onLongClick() },
                    onClear = { onClear() }
                )
            }

            is RunnableSetting -> SettingRow.Item(
                title = item.title,
                description = item.description,
                iconId = item.iconId,
                onClick = { if (item.isRunnable) item.runnable.invoke() }
            )

            is InputSetting -> {
                val showOptions = when (item) {
                    is AnalogInputSetting -> {
                        val param = NativeInput.getStickParam(item.playerIndex, item.nativeAnalog)
                        param.get("engine", "") == "analog_from_button" ||
                            param.has("axis_x") || param.has("axis_y")
                    }
                    is ButtonInputSetting -> {
                        val param = NativeInput.getButtonParam(item.playerIndex, item.nativeButton)
                        param.has("code") || param.has("button") || param.has("hat") ||
                            param.has("axis")
                    }
                    is ModifierInputSetting ->
                        NativeInput.getStickParam(item.playerIndex, item.nativeAnalog).has("modifier")
                    else -> false
                }
                SettingRow.Item(
                    title = item.title,
                    value = item.getSelectedValue(),
                    showOptions = showOptions,
                    onClick = { adapter.onInputClick(item, index) },
                    onLongClick = { adapter.onLongClick(item, index) },
                    onOptions = { adapter.onInputOptionsClick(anchor, item, index) }
                )
            }

            is InputProfileSetting -> SettingRow.Item(
                title = item.title,
                value = item.getCurrentProfile().ifEmpty { context.getString(R.string.not_set) },
                onClick = { adapter.onInputProfileClick(item, index) }
            )

            is StringInputSetting -> SettingRow.Item(
                title = item.title,
                description = item.description,
                value = item.getSelectedValue(),
                editable = editable,
                clearable = clearable,
                onClick = { if (editable) adapter.onStringInputClick(item, index) },
                onLongClick = { onLongClick() },
                onClear = { onClear() }
            )

            is LaunchableSetting -> SettingRow.Item(
                title = item.title,
                description = item.description,
                chevron = true,
                onClick = { adapter.onLaunchableClick(item) }
            )

            is PathSetting -> {
                val usingDefault = item.isUsingDefaultPath()
                SettingRow.Item(
                    title = item.title,
                    description = item.description,
                    iconId = item.iconId,
                    value = if (usingDefault) {
                        context.getString(R.string.default_string)
                    } else {
                        PathUtil.truncatePathForDisplay(item.getCurrentPath())
                    },
                    clearable = !usingDefault,
                    clearLabelId = R.string.reset_to_default,
                    onClick = { adapter.onPathClick(item, index) },
                    onClear = { adapter.onPathReset(item, index) }
                )
            }

            is GpuUnswizzleSetting -> {
                val summary = if (item.isEnabled()) {
                    fun label(entries: Int, values: Int, current: Int): String {
                        val i = resources.getIntArray(values).indexOf(current)
                        return if (i >= 0) resources.getStringArray(entries)[i] else "?"
                    }
                    listOf(
                        label(item.textureSizeChoicesId, item.textureSizeValuesId, item.getTextureSize()),
                        label(item.streamSizeChoicesId, item.streamSizeValuesId, item.getStreamSize()),
                        label(item.chunkSizeChoicesId, item.chunkSizeValuesId, item.getChunkSize())
                    ).joinToString(" ⋅ ")
                } else {
                    context.getString(R.string.gpu_unswizzle_disabled)
                }
                SettingRow.Item(
                    title = item.title,
                    description = item.description,
                    value = summary,
                    editable = editable,
                    clearable = clearable,
                    onClick = { if (editable) adapter.onGpuUnswizzleClick(item, index) },
                    onLongClick = { onLongClick() },
                    onClear = { onClear() }
                )
            }

            else -> SettingRow.Header(item.title)
        }
    }
}
