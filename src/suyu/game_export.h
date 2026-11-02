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

#include "common/package_policy.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "suyu/wikipedia_cover.h"

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
    /// @param backend_index optional CPU backend to select first, as its RecompileBackend
    ///        value (0 = suyu static, 1 = Hybrid AOT + JIT, 2 = Dynarmic JIT);
    ///        negative leaves the current selection. Without this the harness
    ///        can only ever exercise the default backend, which is why the
    ///        hybrid path went untested.
    void TriggerExportForTesting(const QString& rom_path, const QString& output_dir,
                                 int format_index = -1, int backend_index = -1,
                                 int full_scan = -1, quint32 app_version = 0,
                                 const QString& display_version = {});
    /// Test-only options for the next TriggerExportForTesting() run.
    struct TestExportOptions {
        /// What happens to an existing folder with the package's name. Headless runs never
        /// answer a prompt: the default reports a conflict and changes nothing.
        Common::PackagePolicy::ConflictPolicy conflict = Common::PackagePolicy::ConflictPolicy::Fail;
        /// Optional data checkboxes: -1 leaves them, 0/1 sets them.
        int include_save = -1;
        int include_shader = -1;
        int include_config = -1;
        /// Throws at this step ("precompile", "package", "validate") so tests can prove a
        /// failed export leaves no package and the previous one untouched.
        QString fail_at;
        /// Package type: -1 leaves the dialog's choice, 0 = uses the game file, 1 = portable.
        int package = -1;
    };
    void SetTestExportOptions(const TestExportOptions& options);
    /// True when the last test export stopped because its destination already existed.
    bool ExportConflictForTesting() const;
    bool IsExportInProgressForTesting() const;
    bool HasExportResultForTesting() const;
    bool ExportSucceededForTesting() const;
    int ExportProgressForTesting() const;
    QString ExportStatusForTesting() const;
    QString ExportOutputForTesting() const;
    /// Modules the last export could not recompile, which will run on the
    /// Dynarmic JIT instead. Empty when every module was recompiled.
    QStringList FallbackModulesForTesting() const;
    /// The Coverage row's text: recorded gaps and Hybrid runs for the selected game.
    QString CoverageStatusForTesting() const;

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
    /// Standalone static launchers across every remembered export root, newest first.
    static QStringList FindAllRecompiledExecutables();

    /// Directories that have been used as export output, most recent first.
    static QStringList RecompileOutputRoots();
    /// Default output directory: <repo>/exports in a source checkout, else Downloads.
    static QString DefaultExportRoot();
    /// Remember @p dir as an export output root for future lookups.
    static void RememberOutputRoot(const QString& dir);
    /// The game file an export was made from is kept in this suyu's settings, by the
    /// package's export ID, rather than as a path inside a package that may be moved.
    static void RememberExportSource(const QString& export_id, const QString& rom_path);
    /// The recorded game file for the package in @p package_dir, or empty.
    static QString RecordedExportSource(const QString& package_dir);

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

private:
    void SetupUi();

    /// What the export will run for the selected game's update.
    enum class UpdateState {
        NoGame,    ///< No ROM selected, or its title ID could not be read
        NotApplicable, ///< NCA/NRO/extracted input: the exporter never applies an update
        None,      ///< Neither installed nor included in the ROM file
        Disabled,  ///< Installed, but turned off in the game's add-on settings
        Installed, ///< Installed in suyu, and used by the export
        Bundled,   ///< Included in the selected NSP/XCI itself
        BundledDisabled, ///< Included in the file, but updates are off: the export would refuse
        Unreadable,      ///< Turned on but its code cannot be read (missing keys or damage)
    };
    /// Title ID of the selected ROM: the library's when known, else read from the file.
    quint64 SelectedProgramId() const;
    /// @p version gets a used update's display version ("4.0.0") and @p title_version its
    /// numeric title version, as the game will be told them. @p source says where it is read
    /// from: the install location (with the file for NAND), or the game file itself.
    UpdateState CurrentUpdateState(QString* version = nullptr, QString* source = nullptr,
                                   quint32* title_version = nullptr) const;
    /// Refresh the Update row after the ROM changes or an update is installed.
    void RefreshUpdateStatus();
    /// Refresh the Keys, Firmware and Game file rows and what the package type combo offers.
    void RefreshPackageStatus();
    /// Whether a portable package can be made for the current selection; when it cannot,
    /// @p reason says why.
    bool PortableAvailable(QString* reason) const;
    /// The game file and installed update and DLC NCAs a portable export carries, with their
    /// size.
    struct SealSource {
        QString name;        ///< file name inside game/
        QString role;        ///< "base", "update" or "dlc"
        quint64 title_id{};
        int record_type{-1}; ///< ContentRecordType of an update or DLC NCA
        FileSys::VirtualFile file;
    };
    std::vector<SealSource> PortableSources(QString* error) const;
    /// The selected game's DLC installed in suyu's NAND, every NCA of each DLC title.
    std::vector<SealSource> InstalledDlcSources() const;
    /// Copies @p sources into <package_root>/game, sealed, and writes seal.json. Returns false
    /// with @p error set on failure, or with @p cancelled set when the user stopped it.
    bool SealGameFiles(const std::vector<SealSource>& sources, const QString& package_root,
                       const std::string& export_id, quint64 program_id, QString* error,
                       bool* cancelled);
    /// Ask for an update NSP, check it belongs to the selected game, and install it.
    /// Returns true when an update was installed.
    bool PromptAndInstallUpdate();
    /// Refresh the Coverage row from <suyu user dir>/recomp/gaps/<title>.json.
    void RefreshCoverageStatus();
    /// Build IDs of the selected game's ExeFS modules, read once per ROM path.
    QStringList SelectedModuleBuildIds();

    /// True from the moment OnExport() starts until it returns. Guards both
    /// dialog teardown and re-entry into OnExport() itself: the automation RPC
    /// can call TriggerExportForTesting() from the event loop that the export
    /// is pumping, and two exports writing the same cache directory corrupt it.
    bool export_in_progress{false};
    bool test_driven_export{false};
    bool test_export_has_result{false};
    bool test_export_succeeded{false};
    QString test_export_output;
    quint32 test_app_version{};
    QString test_display_version;
    TestExportOptions test_options_;
    bool test_export_conflict{false};
    /// Modules the last export routed to the Dynarmic JIT after a recompile or
    /// compile failure. Reported to the user when the export finishes, so a
    /// partially-degraded package cannot look like a clean one.
    QStringList last_fallback_modules;

    /// Compiler that built the last export's recompiled modules into the
    /// single-file launcher (clang-cl or MSVC), or empty when nothing was
    /// compiled. Written to the export log, aot_manifest.json and the README.
    QString last_recomp_compiler;

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
    QCheckBox* steam_wikipedia_checkbox{};
    /// Writes discord.ini beside a Windows package's launcher: enabled, with a cover URL
    /// found on Wikipedia, or disabled.
    QCheckBox* discord_checkbox{};
    /// Export format: index 0 = source only, index 1 = build to a native binary.
    /// "Build" is a promise, not a hint - when it is selected the export runs
    /// cmake to completion and reports a hard error if a binary cannot be
    /// produced, rather than quietly degrading to a folder of C.
    QComboBox* output_format_combo{};
    /// True when output_format_combo selects the build-to-binary format.
    bool WantsCompiledOutput() const;
    /// Add the finished package's launcher to Steam when asked to. Returns a note for the
    /// completion message, empty when Steam was not requested.
    QString MaybeAddToSteam(const QString& game_title, const QString& exe_path,
                            const QString& backend_label, bool replace, bool use_wikipedia,
                            const std::optional<WikipediaCover::CoverUrls>& known_cover);
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
