// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <vector>
#include <QCheckBox>
#include <QDialog>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Core {
class System;
}

/**
 * Dialog for exporting a game as a native-export artifact bundle using
 * ahead-of-time (AOT) static recompilation.
 *
 * Pipeline:
 *   1. Extract ExeFS/RomFS from the ROM container (NSP/XCI/NCA)
 *   2. Translate ARM64 code blocks into Dynarmic IR compiler artifacts
 *   3. Package IR dumps, guest code slices, block maps, and game data
 *   4. Generate a platform-specific export bundle for a future custom runtime
 *
 * Dynarmic does not expose a stable block-serialization API for ready-made
 * host machine code export, so suyu serializes a frontend boundary instead:
 * translated IR dumps plus guest code slices and metadata. These artifacts are
 * the input for a future minimal runtime/codegen stage.
 */
class GameExportDialog : public QDialog {
    Q_OBJECT

public:
    struct LibraryEntry {
        QString title;
        QString path;
        quint64 program_id{};
        QPixmap icon;
    };

    explicit GameExportDialog(Core::System& system_, QWidget* parent = nullptr);
    ~GameExportDialog() override = default;

    /// Set the game ROM path and optional title-id for portable data bundling.
    void SetRomPath(const QString& path, quint64 program_id = 0);
    void SetLibraryEntries(QVector<LibraryEntry> entries);
    /// Provide the game's artwork so it can be embedded as the launcher exe icon.
    void SetGameIcon(const QPixmap& icon);

    /// Test-only: drive a full export run without needing to click through
    /// the dialog's file pickers/combo boxes, so live automation can trigger
    /// and observe the AOT pipeline (including its known hang past ~15%)
    /// directly.
    /// @param format_index optional output-format combo index to select first
    ///        (0 = Source, 1 = Build); negative leaves the current selection.
    void TriggerExportForTesting(const QString& rom_path, const QString& output_dir,
                                 int format_index = -1);

    /// Every standalone recompiled executable that has already been built for
    /// this game, one per recompiled module, newest-looking first. Empty when
    /// the game was never exported, or was exported but never compiled.
    ///
    /// @param game_name  Library title of the game.
    /// @param rom_path   ROM path; its base name is what the exporter actually
    ///                   names the output directory, which is often not the
    ///                   library title, so both are tried.
    static QStringList FindRecompiledExecutables(const QString& game_name,
                                                 const QString& rom_path = {});

    /// Directories that have been used as export output, most recent first.
    static QStringList RecompileOutputRoots();
    /// Remember @p dir as an export output root for future lookups.
    static void RememberOutputRoot(const QString& dir);

    enum class TargetPlatform {
        Windows,
        Linux,
        MacOS,
    };

    enum class RecompileBackend {
        SuyuStatic, ///< In-tree AArch64-to-C ahead-of-time recompiler
        Hybrid,     ///< Static AOT with Dynarmic fallback for uncovered code
        Dynarmic,   ///< JIT baseline for comparison
    };

signals:
    void ExportFinished(bool success, const QString& output_path);

private slots:
    void OnBrowseRom();
    void OnSelectFromLibrary();
    void OnBrowseOutput();
    void OnExport();
    void OnInstallUpdate();
    /// Merge a shared coverage file into this suyu's store for the game.
    void OnImportCoverage();
    /// Save the game's recorded coverage as shareable execution metadata: build IDs, code
    /// offsets and counts. No raw instruction bytes, names or paths are written.
    void OnExportCoverage();

protected:
    // An export pumps the event loop for as long as the compilers take (tens of
    // minutes on a large title) with its whole state on the stack of OnExport().
    // Closing the dialog in that window - Escape, the title-bar X, or anything
    // else that reaches reject() - returns from the exec() that owns this
    // object and destroys it while OnExport() is still running, which takes the
    // process down with no crash log. Both entry points are refused while an
    // export is in flight.
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

protected:
    // An export pumps the event loop for as long as the compilers take (tens of
    // minutes on a large title) with its whole state on the stack of OnExport().
    // Closing the dialog in that window - Escape, the title-bar X, or anything
    // else that reaches reject() - returns from the exec() that owns this
    // object and destroys it while OnExport() is still running, which takes the
    // process down with no crash log. Both entry points are refused while an
    // export is in flight.
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

private:
    void SetupUi();

    /// True from the moment OnExport() starts until it returns. Guards both
    /// dialog teardown and re-entry into OnExport() itself: the automation RPC
    /// can call TriggerExportForTesting() from the event loop that the export
    /// is pumping, and two exports writing the same cache directory corrupt it.
    bool export_in_progress{false};

    /// AOT export: scan ARM code and serialize translated compiler artifacts.
    /// Returns path to the generated cache directory, or empty string on failure.
    QString RunAotPrecompile(const QString& exefs_dir, const QString& cache_dir,
                             RecompileBackend backend, const QString& game_name);

    /// Package the translated output into a platform-specific export bundle rooted at
    /// @p package_root, the staging folder that becomes the package once validated.
    bool PackageNativeExport(const QString& rom_path, const QString& cache_dir,
                             const QString& package_root, const QString& game_name,
                             TargetPlatform platform, RecompileBackend backend);

    QLineEdit* rom_path_edit{};
    QLineEdit* output_path_edit{};
    QComboBox* platform_combo{};
    QComboBox* backend_combo{};
    QCheckBox* include_save_data_checkbox{};
    QCheckBox* include_shader_cache_checkbox{};
    QCheckBox* include_custom_config_checkbox{};
    QCheckBox* aot_full_scan_checkbox{};
    /// When checked and a module fails to recompile, emit a stub that falls back
    /// to the dynarmic interpreter for that module instead of aborting the export.
    QCheckBox* fallback_to_interpreter_checkbox{};
    QCheckBox* steam_shortcut_checkbox{};
    QCheckBox* steam_replace_rom_checkbox{};
    /// Export format: index 0 = source only, index 1 = build to a native binary.
    /// "Build" is a promise, not a hint - when it is selected the export runs
    /// cmake to completion and reports a hard error if a binary cannot be
    /// produced, rather than quietly degrading to a folder of C.
    QComboBox* output_format_combo{};
    /// True when output_format_combo selects the build-to-binary format.
    bool WantsCompiledOutput() const;
    void MaybeAddToSteam(const QString& game_name, const QString& exe_path);
    QProgressBar* progress_bar{};
    QPushButton* export_button{};
    QLabel* status_label{};
    /// The stages of one export, each weighted by the share of the work it does, so the
    /// progress bar follows the work rather than a few fixed steps.
    enum class ExportStage { Extract, Lift, Compile, Link, Package, Count };
    /// Where each stage starts on the bar (0..1); the last entry is the end, 1.
    std::array<double, static_cast<std::size_t>(ExportStage::Count) + 1> stage_bounds_{};
    /// Weights the stages this export actually runs: Source exports skip compiling and
    /// linking, and the JIT baseline is packaging alone.
    void SetupExportStages(bool uses_aot, bool compiled);
    /// Moves the bar to @p fraction of @p stage - never backwards - and, when given, shows
    /// @p status.
    void ReportStage(ExportStage stage, double fraction, const QString& status = {});
    QLabel* keys_status_label{};
    QLabel* firmware_status_label{};
    QLabel* game_file_status_label{};
    /// "Uses your game file" (reference) or "Portable" (sealed game file in the package).
    QComboBox* package_type_combo{};
    QLabel* package_note_label{};
    /// The general note on what an export contains, which depends on the package type.
    QLabel* content_label{};
    QLabel* update_status_label{};
    QLabel* update_source_label{};
    /// The selected game's installed DLC, which portable exports carry.
    QLabel* dlc_status_label{};
    QPushButton* install_update_button{};
    QLabel* coverage_status_label{};
    QPushButton* export_coverage_button{};
    QStringList coverage_build_ids;
    QString coverage_build_ids_path;
    quint64 rom_program_id{};
    /// ROM path rom_program_id belongs to; a hand-typed different path invalidates it.
    QString rom_program_id_path;
    /// Title ID parsed from a ROM file, and the path it belongs to (SelectedProgramId's cache).
    mutable quint64 cached_program_id{};
    mutable QString cached_program_id_path;
    QVector<LibraryEntry> library_entries_;
    Core::System& system_;
    QPixmap game_icon_;
};
