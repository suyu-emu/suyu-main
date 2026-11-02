// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.model

import androidx.annotation.StringRes
import androidx.annotation.DrawableRes
import org.yuzu.yuzu_emu.R

data class Installable(
    @StringRes val titleId: Int,
    @StringRes val descriptionId: Int,
    val install: (() -> Unit)? = null,
    val export: (() -> Unit)? = null,
    @DrawableRes val installIconId: Int = R.drawable.ic_import,
    @StringRes val installLabelId: Int = R.string.string_import
)
