// SPDX-FileCopyrightText: Copyright 2025 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "suyu/setup_dialog.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

SetupDialog::SetupDialog(QVector<Step> steps, QWidget* parent)
    : QDialog(parent), steps_(std::move(steps)) {
    setWindowTitle(tr("Setup"));
    setModal(true);

    auto* outer = new QVBoxLayout(this);

    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    outer->addWidget(summary_);

    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    outer->addWidget(line);

    auto* grid = new QGridLayout();
    grid->setColumnStretch(1, 1);
    outer->addLayout(grid);

    for (int i = 0; i < steps_.size(); ++i) {
        const Step& step = steps_[i];
        Row row;

        // Fixed width so the titles line up whether the mark is a tick or a
        // dash; the column would otherwise jump around on every refresh.
        row.status = new QLabel(this);
        row.status->setFixedWidth(20);
        row.status->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

        auto* title = new QLabel(QStringLiteral("<b>%1</b>").arg(step.title.toHtmlEscaped()), this);
        row.detail = new QLabel(this);
        row.detail->setWordWrap(true);

        auto* text = new QVBoxLayout();
        text->setSpacing(2);
        text->addWidget(title);
        text->addWidget(row.detail);

        row.button = new QPushButton(step.button_text, this);
        row.button->setEnabled(step.action != nullptr && step.action->isEnabled());

        grid->addWidget(row.status, i, 0, Qt::AlignTop);
        grid->addLayout(text, i, 1);
        grid->addWidget(row.button, i, 2, Qt::AlignTop);

        // The action opens its own modal dialog, so by the time trigger()
        // returns the user has finished with it and the predicate can be asked
        // again. Refreshing every row rather than this one is deliberate:
        // installing firmware can change what the key check reports.
        connect(row.button, &QPushButton::clicked, this, [this, i]() {
            if (auto* action = steps_[i].action) {
                action->trigger();
            }
            Refresh();
        });

        rows_.append(row);
    }

    outer->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* refresh = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    connect(refresh, &QPushButton::clicked, this, &SetupDialog::Refresh);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);

    Refresh();
}

SetupDialog::~SetupDialog() = default;

void SetupDialog::Refresh() {
    int done = 0;

    for (int i = 0; i < steps_.size(); ++i) {
        const Step& step = steps_[i];
        Row& row = rows_[i];

        const bool ok = step.is_done && step.is_done();
        if (ok) {
            ++done;
        }

        // Colour is not the only signal: the mark differs too, so this still
        // reads on a monochrome or colour-blind-unfriendly theme.
        row.status->setText(ok ? QStringLiteral("<span style='color:#2e9e4f'>&#10003;</span>")
                               : QStringLiteral("<span style='color:#b0882c'>&#8212;</span>"));

        QString detail = step.detail;
        if (ok && step.done_detail) {
            const QString extra = step.done_detail();
            if (!extra.isEmpty()) {
                detail = extra;
            }
        }
        row.detail->setText(detail);

        if (step.action) {
            row.button->setEnabled(step.action->isEnabled());
        }
    }

    if (done == steps_.size()) {
        summary_->setText(tr("Everything is in place."));
    } else {
        summary_->setText(tr("%1 of %2 steps done. Work down the list - later steps need the "
                             "earlier ones.")
                              .arg(done)
                              .arg(steps_.size()));
    }
}
