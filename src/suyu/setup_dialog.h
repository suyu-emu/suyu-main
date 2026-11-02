// SPDX-FileCopyrightText: Copyright 2025 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>

#include <QDialog>
#include <QString>
#include <QVector>

class QAction;
class QLabel;
class QPushButton;

/**
 * Checklist of everything that has to be in place before a title will boot and
 * before it can be exported.
 *
 * Every step here already had a menu entry, but they were spread across File
 * and Tools in no particular order and nothing said which ones were done, so
 * the only way to find out what was still missing was to try to boot and read
 * the failure. This owns no logic of its own: each step carries a predicate
 * that reports real state and a QAction that is already wired up elsewhere, and
 * the dialog just draws the result and triggers the action.
 */
class SetupDialog : public QDialog {
    Q_OBJECT

public:
    struct Step {
        QString title;
        /// One line on what this is for, shown under the title.
        QString detail;
        /// Re-evaluated every time the dialog refreshes. Must be cheap.
        std::function<bool()> is_done;
        /// Triggered by the step's button; owned by the caller.
        QAction* action{};
        QString button_text;
        /// Shown instead of "Done" when is_done() is true - "3 titles", say.
        std::function<QString()> done_detail;
    };

    explicit SetupDialog(QVector<Step> steps, QWidget* parent = nullptr);
    ~SetupDialog() override;

private:
    /// Re-run every predicate and repaint the rows. Called on construction and
    /// after any step's action returns, since an action is exactly the thing
    /// that changes the answer.
    void Refresh();

    struct Row {
        QLabel* status{};
        QLabel* detail{};
        QPushButton* button{};
    };

    QVector<Step> steps_;
    QVector<Row> rows_;
    QLabel* summary_{};
};
