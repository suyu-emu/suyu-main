// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.screens

import android.text.Spanned
import android.text.style.StyleSpan
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.text.HtmlCompat
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.ButtonState
import org.yuzu.yuzu_emu.model.PageButton
import org.yuzu.yuzu_emu.model.PageState
import org.yuzu.yuzu_emu.model.SetupPage
import org.yuzu.yuzu_emu.ui.compose.common.SuyuButton
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors

private fun htmlToAnnotated(html: String): AnnotatedString {
    val spanned: Spanned = HtmlCompat.fromHtml(html, HtmlCompat.FROM_HTML_MODE_LEGACY)
    return buildAnnotatedString {
        append(spanned.toString().trim())
        spanned.getSpans(0, spanned.length, StyleSpan::class.java).forEach { span ->
            if (span.style == android.graphics.Typeface.BOLD || span.style == android.graphics.Typeface.BOLD_ITALIC) {
                val start = spanned.getSpanStart(span)
                val end = minOf(spanned.getSpanEnd(span), length)
                if (start in 0 until end) {
                    addStyle(SpanStyle(fontWeight = FontWeight.Bold, color = SuyuColors.Text), start, end)
                }
            }
        }
    }
}

@Composable
fun SetupScreen(
    pages: List<SetupPage>,
    page: Int,
    revision: Int,
    onBack: () -> Unit,
    onNext: () -> Unit,
    onButton: (PageButton) -> Unit
) {
    val top = WindowInsets.statusBars.asPaddingValues().calculateTopPadding()
    val bottom = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
    val current = pages.getOrNull(page)

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(SuyuColors.Background)
    ) {
        if (current != null) {
            // Reading revision makes the completion state below recompute after each step.
            val pageDone = revision.let { current.pageSteps() == PageState.COMPLETE }
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(top = top + 16.dp, bottom = bottom + 96.dp)
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 32.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(14.dp, Alignment.CenterVertically)
            ) {
                Box(
                    modifier = Modifier
                        .size(88.dp)
                        .clip(CircleShape)
                        .background(SuyuColors.Accent.copy(alpha = 0.14f)),
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        painter = painterResource(current.iconId),
                        contentDescription = null,
                        tint = SuyuColors.Accent,
                        modifier = Modifier.size(44.dp)
                    )
                }
                Text(
                    text = stringResource(current.titleId),
                    color = SuyuColors.Text,
                    fontSize = 28.sp,
                    fontWeight = FontWeight.Bold,
                    textAlign = TextAlign.Center
                )
                Text(
                    text = htmlToAnnotated(stringResource(current.descriptionId)),
                    color = SuyuColors.Muted,
                    fontSize = 15.sp,
                    textAlign = TextAlign.Center
                )
                if (pageDone) {
                    Text(
                        text = stringResource(R.string.step_complete),
                        color = SuyuColors.Accent,
                        fontSize = 16.sp,
                        fontWeight = FontWeight.SemiBold
                    )
                } else {
                    current.pageButtons?.forEach { button ->
                        val complete = revision.let { button.buttonState() == ButtonState.BUTTON_ACTION_COMPLETE }
                        SuyuButton(
                            label = stringResource(button.titleId),
                            onClick = { if (!complete) onButton(button) },
                            modifier = Modifier
                                .fillMaxWidth()
                                .alpha(if (complete) 0.38f else 1f),
                            icon = if (complete) R.drawable.ic_check else button.iconId,
                            filled = !complete
                        )
                    }
                }
            }
        }

        Row(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .padding(start = 24.dp, end = 24.dp, bottom = bottom + 20.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            if (page > 0) {
                SuyuButton(label = stringResource(R.string.back), onClick = onBack, filled = false)
            } else {
                Box(modifier = Modifier.size(1.dp))
            }
            Text(
                text = "${page + 1} / ${pages.size}",
                color = SuyuColors.Muted,
                fontSize = 13.sp
            )
            if (page < pages.size - 1) {
                SuyuButton(label = stringResource(R.string.next), onClick = onNext)
            } else {
                Box(modifier = Modifier.size(1.dp))
            }
        }
    }
}
