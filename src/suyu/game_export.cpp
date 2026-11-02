// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "suyu/game_export.h"
#include "suyu/steam_integration.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QCoreApplication>
#include <QPixmap>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QImage>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QCloseEvent>
#include <QEventLoop>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QStorageInfo>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <span>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/settings.h"
#ifndef SUYU_NO_JIT
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/ir/basic_block.h"
#endif

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/package_policy.h"
#include "common/scope_exit.h"
#include "common/scm_rev.h"
#include "common/hex_util.h"
#include "common/lz4_compression.h"
#include "common/swap.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/arm/recomp/recomp_gap_session.h"
#include "core/arm/recomp/recomp_gaps.h"
#include "core/arm/recomp/recomp_image_features.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "core/crypto/portable_seal.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "core/loader/nso.h"
#include "core/recompiler/arm64_to_c.h"
#include "frontend_common/content_manager.h"
#include "frontend_common/firmware_manager.h"

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------
//
// Everything an export copies goes through Common::PackagePolicy: an explicit
// plan of selected files, copied without following links into a staging folder
// this run owns, and checked as a whole before it becomes the package. Nothing
// is swept up by copying a directory wholesale.

namespace PackagePolicy = Common::PackagePolicy;

static std::filesystem::path ToFsPath(const QString& path) {
    return std::filesystem::path{QDir::toNativeSeparators(path).toStdU16String()};
}

static QString FromFsPath(const std::filesystem::path& path) {
    return QDir::fromNativeSeparators(QString::fromStdU16String(path.u16string()));
}

#ifdef _WIN32
// RT_ICON resources contain a DIB, not a PNG file. Build a 32-bit,
// bottom-up DIB with an opaque alpha mask so Explorer and the shell can read
// the icon after it has been attached to the copied launcher executable.
static QByteArray MakeIconDib(const QPixmap& pixmap) {
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull() || image.width() != image.height()) {
        return {};
    }

    const int width = image.width();
    const int height = image.height();
    const qsizetype xor_size = static_cast<qsizetype>(width) * height * 4;
    const qsizetype and_stride = ((width + 31) / 32) * 4;
    const qsizetype and_size = and_stride * height;
    QByteArray dib(static_cast<qsizetype>(sizeof(BITMAPINFOHEADER)) + xor_size + and_size,
                   Qt::Uninitialized);

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = width;
    header.biHeight = height * 2;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(xor_size + and_size);
    std::memcpy(dib.data(), &header, sizeof(header));

    auto* pixels = reinterpret_cast<BYTE*>(dib.data() + sizeof(header));
    for (int y = 0; y < height; ++y) {
        const auto* source = image.constScanLine(height - 1 - y);
        auto* destination = pixels + static_cast<qsizetype>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            const auto* rgba = source + x * 4;
            destination[x * 4 + 0] = rgba[2];
            destination[x * 4 + 1] = rgba[1];
            destination[x * 4 + 2] = rgba[0];
            destination[x * 4 + 3] = rgba[3];
        }
    }
    // A zeroed AND mask means every pixel is taken from the 32-bit XOR image.
    return dib;
}
#endif

#ifdef _WIN32
// RT_ICON resources contain a DIB, not a PNG file. Build a 32-bit,
// bottom-up DIB with an opaque alpha mask so Explorer and the shell can read
// the icon after it has been attached to the copied launcher executable.
static QByteArray MakeIconDib(const QPixmap& pixmap) {
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull() || image.width() != image.height()) {
        return {};
    }

    const int width = image.width();
    const int height = image.height();
    const qsizetype xor_size = static_cast<qsizetype>(width) * height * 4;
    const qsizetype and_stride = ((width + 31) / 32) * 4;
    const qsizetype and_size = and_stride * height;
    QByteArray dib(static_cast<qsizetype>(sizeof(BITMAPINFOHEADER)) + xor_size + and_size,
                   Qt::Uninitialized);

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = width;
    header.biHeight = height * 2;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(xor_size + and_size);
    std::memcpy(dib.data(), &header, sizeof(header));

    auto* pixels = reinterpret_cast<BYTE*>(dib.data() + sizeof(header));
    for (int y = 0; y < height; ++y) {
        const auto* source = image.constScanLine(height - 1 - y);
        auto* destination = pixels + static_cast<qsizetype>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            const auto* rgba = source + x * 4;
            destination[x * 4 + 0] = rgba[2];
            destination[x * 4 + 1] = rgba[1];
            destination[x * 4 + 2] = rgba[0];
            destination[x * 4 + 3] = rgba[3];
        }
    }
    // A zeroed AND mask means every pixel is taken from the 32-bit XOR image.
    return dib;
}
#endif

static bool CopyFileReplacingExisting(const QString& src, const QString& dst) {
    const QFileInfo src_info(src);
    if (!src_info.exists() || !src_info.isFile()) {
        return false;
    }

    const QFileInfo dst_info(dst);
    if (!QDir().mkpath(dst_info.absolutePath())) {
        return false;
    }

    // An unchanged copy is left alone. The bundled payload is the ROM itself -
    // 18.5 GB for Smash Ultimate - and re-copying it on every export dominates
    // the run for no gain when the same file is already sitting there.
    if (dst_info.exists() && dst_info.isFile() && dst_info.size() == src_info.size() &&
        dst_info.lastModified() >= src_info.lastModified()) {
        return true;
    }

    if (QFile::exists(dst) && !QFile::remove(dst)) {
        return false;
    }

    return QFile::copy(src, dst);
}

// True when both paths name the same location on disk, so a "copy" between
// them would be a no-op at best.
static bool IsSamePath(const QString& a, const QString& b) {
    return QDir::cleanPath(QFileInfo(a).absoluteFilePath()).compare(
               QDir::cleanPath(QFileInfo(b).absoluteFilePath()), Qt::CaseInsensitive) == 0;
}

// Copy a directory unless it is already in place.
//
// The AOT cache is now generated straight into the package, so the packaging
// step finds source and destination identical. Copying it anyway meant
// duplicating gigabytes of generated C - Smash Ultimate's cache is about 6 GB -
// onto itself, which is where exports were dying.
static bool CopyDirectoryUnlessInPlace(const QString& src, const QString& dst) {
    if (IsSamePath(src, dst)) {
        return QDir(src).exists();
    }
    return CopyDirectoryRecursive(src, dst);
}

// DLLs suyu-cmd needs beside it (FFmpeg, DXC, OpenSSL; SDL3 is statically linked).
static constexpr const char* kExportRuntimeDlls[] = {
    "avcodec-61.dll",      "avformat-61.dll",  "avutil-59.dll", "swresample-5.dll",
    "swscale-8.dll",       "dxcompiler.dll",   "dxil.dll",      "libcrypto-3-x64.dll",
    "libssl-3-x64.dll",    "discord-rpc.dll",
    // fallback names used by some builds
    "libcrypto.dll",       "libssl.dll",
};

// Export Format combo item data. These keep their meaning (and the automation's
// format_index) whatever the combo's display order.
static constexpr int kFormatSource = 0;
static constexpr int kFormatBuild = 1;

// What every local export says about itself, in its README and the exporter's warning.
static constexpr char kExportContentNotice[] =
    "This is a local game export, made on this computer from a game file you selected.\n"
    "It contains no game data: each time it starts it reads your own game file, and\n"
    "suyu decrypts it with the keys you installed from your own Switch, exactly as when\n"
    "you play it in suyu. Without that file and those keys it does not run. Keys and\n"
    "firmware are never copied into it. Hybrid and static exports also contain code\n"
    "translated from the game (Source exports as C), which only runs that way.\n"
    "It is not an official suyu release. How it was made does not show that anyone owns\n"
    "the game or may redistribute it. Do not upload it, or logs and dumps made with it,\n"
    "to suyu's release or support channels.\n";

// Save data lives at nand/user/save/<save space>/<user ID>/<title ID>. Only
// folders at exactly that depth, named for this title, are copied; a folder of
// that name anywhere else in the NAND is not this title's save.
static bool CopyTitleSaveData(const QString& save_root, const QString& dst_root, quint64 title_id,
                              QString* error) {
    const QString title_id_hex =
        QStringLiteral("%1").arg(title_id, 16, 16, QLatin1Char('0')).toUpper();
    static const QRegularExpression kSpace(QStringLiteral("^[0-9A-Fa-f]{16}$"));
    static const QRegularExpression kUser(QStringLiteral("^[0-9A-Fa-f]{32}$"));
    const QDir root(save_root);
    if (!root.exists()) {
        return true;
    }
    const auto dirs = QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden;
    for (const QFileInfo& space : root.entryInfoList(dirs)) {
        if (!kSpace.match(space.fileName()).hasMatch()) {
            continue;
        }
        for (const QFileInfo& user : QDir(space.filePath()).entryInfoList(dirs)) {
            if (!kUser.match(user.fileName()).hasMatch()) {
                continue;
            }
            const QString title_dir = user.filePath() + QLatin1Char('/') + title_id_hex;
            if (!QFileInfo(title_dir).isDir()) {
                continue;
            }
            const QString target = dst_root + QLatin1Char('/') + space.fileName() +
                                   QLatin1Char('/') + user.fileName() + QLatin1Char('/') +
                                   title_id_hex;
            std::string reason;
            if (!QDir().mkpath(QFileInfo(target).absolutePath()) ||
                !PackagePolicy::CopyTreeNoFollow(ToFsPath(title_dir), ToFsPath(target), &reason)) {
                *error = QStringLiteral("save data: %1").arg(QString::fromStdString(reason));
                return false;
            }
        }
    }
    return true;
}

// Saves and the shader cache are optional, chosen per export, and always
// scoped to this title. The per-game configuration is not copied as a file:
// SeedPortableConfig takes only allowlisted settings from it.
static bool CopyPortableSupportData(quint64 program_id, const QString& package_root,
                                    bool include_save, bool include_shader, QString* error) {
    const QString title_id_hex = QStringLiteral("%1").arg(program_id, 16, 16, QLatin1Char('0')).toUpper();
    const QString output_user_root = package_root + QStringLiteral("/user");

    if (include_save) {
        const QString nand_save_root =
            FromFsPath(Common::FS::GetSuyuPath(Common::FS::SuyuPath::NANDDir)) +
            QStringLiteral("/user/save");
        if (!CopyTitleSaveData(nand_save_root, output_user_root + QStringLiteral("/nand/user/save"),
                               program_id, error)) {
            return false;
        }
    }

    if (include_shader) {
        // The package reads ShaderDir as user/cache/shader, and the renderers name the
        // per-title folder in lowercase hex; any other destination is never loaded.
        // A shader cache is derived from this game's shaders: it stays in this local
        // package and is never part of shared coverage or a public release.
        const QString shader_title_dir = title_id_hex.toLower();
        const QString shader_src =
            FromFsPath(Common::FS::GetSuyuPath(Common::FS::SuyuPath::ShaderDir)) +
            QLatin1Char('/') + shader_title_dir;
        const QString shader_dst =
            output_user_root + QStringLiteral("/cache/shader/") + shader_title_dir;
        if (QFileInfo(shader_src).isDir()) {
            std::string reason;
            if (!QDir().mkpath(QFileInfo(shader_dst).absolutePath()) ||
                !PackagePolicy::CopyTreeNoFollow(ToFsPath(shader_src), ToFsPath(shader_dst),
                                                 &reason)) {
                *error = QStringLiteral("shader cache: %1").arg(QString::fromStdString(reason));
                return false;
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Dialog setup
// ---------------------------------------------------------------------------

GameExportDialog::GameExportDialog(Core::System& system, QWidget* parent)
    : QDialog(parent), system_(system) {
    setWindowTitle(tr("Export Game — AOT Static Recompilation"));
    setMinimumSize(540, 420);
    SetupUi();
}

void GameExportDialog::SetupUi() {
    auto* layout = new QVBoxLayout(this);

    // ROM path row
    auto* rom_row = new QHBoxLayout();
    rom_row->addWidget(new QLabel(tr("ROM:"), this));
    rom_path_edit = new QLineEdit(this);
    rom_path_edit->setReadOnly(false);
    rom_path_edit->setPlaceholderText(tr("Select a ROM file (.nsp, .xci, .nca, .nro) ..."));
    rom_row->addWidget(rom_path_edit);
    auto* rom_library_btn = new QPushButton(tr("From Library..."), this);
    rom_row->addWidget(rom_library_btn);
    auto* rom_browse_btn = new QPushButton(tr("Browse..."), this);
    rom_row->addWidget(rom_browse_btn);
    layout->addLayout(rom_row);

    // What an export runs with: the keys and firmware installed in suyu, and the selected game
    // file. Keys and firmware are never copied into a package.
    const auto add_status_row = [this, layout](const QString& caption) {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(caption, this));
        auto* value = new QLabel(this);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(value, 1);
        layout->addLayout(row);
        return value;
    };
    keys_status_label = add_status_row(tr("Keys:"));
    firmware_status_label = add_status_row(tr("Firmware:"));
    game_file_status_label = add_status_row(tr("Game file:"));
    // Read once: firmware is not installed from this dialog.
    QString firmware_text = tr("Not installed (optional; Mii screens and some menus need it)");
    if (FirmwareManager::CheckFirmwarePresence(system_)) {
        const auto [firmware, result] = FirmwareManager::GetFirmwareVersion(system_);
        firmware_text = result.IsSuccess() ? tr("Installed, version %1.%2.%3")
                                                 .arg(firmware.major)
                                                 .arg(firmware.minor)
                                                 .arg(firmware.micro)
                                           : tr("Installed");
    }
    firmware_status_label->setText(firmware_text);

    // Update row: exports use the game's installed update, so say which one, and let a fresh
    // suyu install add it here instead of sending the user to File > Install first.
    auto* update_row = new QHBoxLayout();
    update_row->addWidget(new QLabel(tr("Update:"), this));
    update_status_label = new QLabel(this);
    update_status_label->setWordWrap(true);
    update_row->addWidget(update_status_label, 1);
    install_update_button = new QPushButton(tr("Install Update File..."), this);
    install_update_button->setToolTip(
        tr("Install a game update (.nsp) into suyu so the export uses that version."));
    update_row->addWidget(install_update_button);
    layout->addLayout(update_row);
    // Where that update comes from, in smaller type; long paths are elided, in full on hover.
    update_source_label = new QLabel(this);
    QFont source_font = update_source_label->font();
    source_font.setPointSizeF(source_font.pointSizeF() * 0.85);
    update_source_label->setFont(source_font);
    update_source_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    update_source_label->setVisible(false);
    layout->addWidget(update_source_label);
    // DLC row: the game's DLC installed in suyu. Portable exports carry it, sealed; the default
    // export reads it from the installed suyu at each launch.
    dlc_status_label = add_status_row(tr("DLC:"));

    // Coverage row: what earlier Hybrid runs of this game recorded (recomp_gaps.json in the
    // suyu user folder), which the export feeds back in and which says whether a static
    // export can be expected to run. The files carry no game code, so players can pool them.
    auto* coverage_row = new QHBoxLayout();
    coverage_row->addWidget(new QLabel(tr("Coverage:"), this));
    coverage_status_label = new QLabel(this);
    coverage_status_label->setWordWrap(true);
    coverage_row->addWidget(coverage_status_label, 1);
    auto* import_coverage_button = new QPushButton(tr("Import coverage file..."), this);
    import_coverage_button->setToolTip(
        tr("Add a coverage file from another player's Hybrid runs of this game: execution "
           "metadata (build IDs, code offsets and counts). Older files are converted on import "
           "and any instruction encodings in them are dropped."));
    coverage_row->addWidget(import_coverage_button);
    export_coverage_button = new QPushButton(tr("Export coverage file..."), this);
    export_coverage_button->setToolTip(
        tr("Save what your Hybrid runs of this game recorded, to share: execution metadata "
           "(build IDs, code offsets and counts). No raw instruction bytes, module names or file "
           "paths are included."));
    coverage_row->addWidget(export_coverage_button);
    layout->addLayout(coverage_row);
    connect(import_coverage_button, &QPushButton::clicked, this,
            &GameExportDialog::OnImportCoverage);
    connect(export_coverage_button, &QPushButton::clicked, this,
            &GameExportDialog::OnExportCoverage);

    // Output path row
    auto* out_row = new QHBoxLayout();
    out_row->addWidget(new QLabel(tr("Output:"), this));
    output_path_edit = new QLineEdit(this);
    output_path_edit->setPlaceholderText(tr("Select output directory..."));
    out_row->addWidget(output_path_edit);
    auto* browse_btn = new QPushButton(tr("Browse..."), this);
    out_row->addWidget(browse_btn);
    layout->addLayout(out_row);

    const QString default_output = DefaultExportRoot();
    if (!default_output.isEmpty()) {
        output_path_edit->setText(QDir::toNativeSeparators(default_output));
    }

    // Target platform
    auto* plat_row = new QHBoxLayout();
    plat_row->addWidget(new QLabel(tr("Target:"), this));
    platform_combo = new QComboBox(this);
    platform_combo->addItem(tr("Windows artifact bundle"), static_cast<int>(TargetPlatform::Windows));
    platform_combo->addItem(tr("Linux artifact bundle"), static_cast<int>(TargetPlatform::Linux));
    platform_combo->addItem(tr("macOS artifact bundle"), static_cast<int>(TargetPlatform::MacOS));
#ifdef _WIN32
    platform_combo->setCurrentIndex(0);
#elif defined(__APPLE__)
    platform_combo->setCurrentIndex(2);
#else
    platform_combo->setCurrentIndex(1);
#endif
    plat_row->addWidget(platform_combo);
    layout->addLayout(plat_row);

    // CPU backend used by the exported comparison build.
    auto* backend_row = new QHBoxLayout();
    backend_row->addWidget(new QLabel(tr("CPU Backend:"), this));
    // Items are told apart by their RecompileBackend data, never by position. The JIT
    // baseline comes first and is the default: it needs no compiler and is the reference
    // the recompiled backends are measured against.
    backend_combo = new QComboBox(this);
#ifndef SUYU_NO_JIT
    backend_combo->addItem(tr("suyu Dynarmic JIT (Baseline)"),
                           static_cast<int>(RecompileBackend::Dynarmic));
    backend_combo->addItem(tr("suyu Hybrid JIT + AOT"),
                           static_cast<int>(RecompileBackend::Hybrid));
#endif
    backend_combo->addItem(tr("suyu static AOT (Experimental)"),
                           static_cast<int>(RecompileBackend::SuyuStatic));
    backend_combo->setCurrentIndex(0);
    backend_row->addWidget(backend_combo);
    layout->addLayout(backend_row);

    // Package type. The default records the game file and reads it at each launch. Portable
    // copies the game file and its installed update into the package unchanged and seals them
    // to this console's keys; the package then runs only with that console's keys.
    auto* package_row = new QHBoxLayout();
    package_row->addWidget(new QLabel(tr("Package type:"), this));
    package_type_combo = new QComboBox(this);
    package_type_combo->addItem(tr("Uses your game file (default)"), QStringLiteral("reference"));
    package_type_combo->addItem(tr("Portable — includes your game file, sealed to this console"),
                                QStringLiteral("portable"));
    package_row->addWidget(package_type_combo, 1);
    layout->addLayout(package_row);
    package_note_label = new QLabel(this);
    package_note_label->setWordWrap(true);
    package_note_label->setStyleSheet(QStringLiteral("color: #888;"));
    layout->addWidget(package_note_label);
    {
        const QSettings settings(QStringLiteral("suyu"), QStringLiteral("suyu"));
        if (settings.value(QStringLiteral("recompile/package_type")).toString() ==
            QStringLiteral("portable")) {
            package_type_combo->setCurrentIndex(1);
        }
    }
    // Remembered only when the user picks one; automation sets its own per export.
    connect(package_type_combo, &QComboBox::activated, this, [this](int) {
        QSettings settings(QStringLiteral("suyu"), QStringLiteral("suyu"));
        settings.setValue(QStringLiteral("recompile/package_type"),
                          package_type_combo->currentData().toString());
        RefreshPackageStatus();
    });

    // AOT options
    aot_full_scan_checkbox = new QCheckBox(
        tr("Full code scan (currently no additional generated code)"), this);
    aot_full_scan_checkbox->setChecked(false);
    aot_full_scan_checkbox->setToolTip(
        tr("This mode currently produces the same generated code and does not improve cold boot."));
    layout->addWidget(aot_full_scan_checkbox);
    // It produces the same generated code as a normal export, so offering it only suggests a
    // speed-up that does not exist. Kept, hidden, for TriggerExportForTesting's full_scan.
    aot_full_scan_checkbox->setVisible(false);

    steam_shortcut_checkbox = new QCheckBox(tr("Add to Steam library when the export finishes"), this);
    steam_shortcut_checkbox->setChecked(false);
    steam_shortcut_checkbox->setToolTip(
        tr("Creates a non-Steam shortcut for the exported game and pulls its store artwork "
           "into Steam, so it shows up in the library with a proper grid image."));
    layout->addWidget(steam_shortcut_checkbox);

    steam_replace_rom_checkbox =
        new QCheckBox(tr("...and replace an existing shortcut for the same game"), this);
    steam_replace_rom_checkbox->setChecked(true);
    steam_replace_rom_checkbox->setToolTip(
        tr("A shortcut added earlier for this title - pointing at the ROM through the emulator - "
           "is removed first, so the recompiled build takes its place instead of appearing "
           "alongside it. Uncheck to keep both entries."));
    layout->addWidget(steam_replace_rom_checkbox);
    connect(steam_shortcut_checkbox, &QCheckBox::toggled, steam_replace_rom_checkbox,
            &QWidget::setEnabled);
    steam_replace_rom_checkbox->setEnabled(false);

    fallback_to_interpreter_checkbox = new QCheckBox(
        tr("Fall back to interpreter if a module fails to recompile"), this);
    fallback_to_interpreter_checkbox->setChecked(true);
    fallback_to_interpreter_checkbox->setToolTip(
        tr("When checked: if a module cannot be recompiled (e.g. too complex, "
           "unsupported instructions), the export continues and that module will "
           "use the dynarmic JIT at runtime instead of the static recompiled code. "
           "When unchecked: any recompile failure aborts the entire export."));
    layout->addWidget(fallback_to_interpreter_checkbox);

    // Source vs Build is a real, explicit choice rather than an easily-missed
    // checkbox, because the two produce completely different deliverables and
    // "I picked build and got a folder of C" was the reported complaint. Source
    // stays the default: a large title lifts to gigabytes of C - Smash
    // Ultimate's main module alone is ~3 GB across 139 translation units - and
    // compiling that is hours of C-compiler work, so it must be asked for, not
    // stumbled into. When Build IS chosen the export compiles all the way to a
    // binary and fails loudly if it cannot, instead of silently degrading.
    auto* format_row = new QHBoxLayout();
    format_row->addWidget(new QLabel(tr("Export Format:"), this));
    output_format_combo = new QComboBox(this);
    output_format_combo->addItem(
        tr("Source — generate C project only (fast, compile it yourself)"));
    output_format_combo->addItem(
        tr("Build — compile to a standalone executable (slow, needs CMake + a C compiler)"));
    output_format_combo->setToolTip(
        tr("Source writes the recompiled C plus a CMakeLists.txt and a build script, and stops "
           "there. Build additionally runs CMake to completion, producing the standalone "
           "'recompiled' executable and the shared library suyu loads to run the game on its own "
           "recompiler. Build can take hours on large titles; the window stays responsive while "
           "it works."));
    output_format_combo->setCurrentIndex(0);
    format_row->addWidget(output_format_combo, 1);
    layout->addLayout(format_row);

    include_save_data_checkbox = new QCheckBox(tr("Include save data for this game"), this);
    include_save_data_checkbox->setChecked(false);
    include_save_data_checkbox->setToolTip(
        tr("Copies this game's own save folders into the package's user/ folder."));
    layout->addWidget(include_save_data_checkbox);

    include_shader_cache_checkbox = new QCheckBox(tr("Include transferable shader cache"), this);
    include_shader_cache_checkbox->setChecked(false);
    include_shader_cache_checkbox->setToolTip(
        tr("Copies this game's shader cache. It is built from the game's shaders, so it stays "
           "with this local package."));
    layout->addWidget(include_shader_cache_checkbox);

    include_custom_config_checkbox = new QCheckBox(tr("Include custom game configuration"), this);
    include_custom_config_checkbox->setChecked(false);
    include_custom_config_checkbox->setToolTip(
        tr("Applies this game's own gameplay, graphics and audio settings. The settings file is "
           "not copied: paths, devices, accounts and debugging options stay behind."));
    layout->addWidget(include_custom_config_checkbox);

    content_label = new QLabel(this);
    content_label->setWordWrap(true);
    layout->addWidget(content_label);

    auto* note_label = new QLabel(
          tr("Translates the game's ARM64 code into C. Output mirrors the ROM structure: "
             "exefs/ holds one C project per module (main, rtld, sdk, ...). "
             "Build has suyu compile it for you (slow on large titles). "
             "Source gives you the C + CMakeLists.txt to compile yourself. "
             "With fallback enabled, modules that fail recompile use the dynarmic JIT at runtime."),
        this);
    note_label->setWordWrap(true);
    note_label->setStyleSheet(QStringLiteral("color: #888;"));
    // A word-wrapped QLabel reports a single line as its size hint, so the
    // layout hands it one line of height and clips the rest. This is the label
    // that explains what the selected backend actually does, and it was being
    // cut off mid-sentence - so the one piece of text telling the user what
    // "Hybrid AOT + JIT" means was unreadable. Reserve the tallest of the
    // three notes up front rather than letting the dialog resize as the combo
    // changes.
    note_label->setMinimumHeight(4 * note_label->fontMetrics().lineSpacing());
    note_label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    layout->addWidget(note_label);

    layout->addStretch();

    // Progress
    progress_bar = new QProgressBar(this);
    progress_bar->setRange(0, 1000);
    progress_bar->setValue(0);
    progress_bar->setVisible(false);
    // A hidden widget occupies no space, so the progress bar appearing when the
    // export starts used to take its height from whatever was above it - the
    // note label - and clip it exactly when the user was reading it. Keep the
    // space reserved from the start so nothing moves when the export begins.
    {
        QSizePolicy sp = progress_bar->sizePolicy();
        sp.setRetainSizeWhenHidden(true);
        progress_bar->setSizePolicy(sp);
    }
    layout->addWidget(progress_bar);

    status_label = new QLabel(this);
    status_label->setStyleSheet(QStringLiteral("color: #888;"));
    // Same reasoning: this is empty until the export runs, and the status text
    // is one line, so reserve one line.
    status_label->setMinimumHeight(status_label->fontMetrics().lineSpacing());
    layout->addWidget(status_label);

    // Export button
    export_button = new QPushButton(tr("Export Game"), this);
    layout->addWidget(export_button);

    connect(rom_library_btn, &QPushButton::clicked, this, &GameExportDialog::OnSelectFromLibrary);
    connect(rom_browse_btn, &QPushButton::clicked, this, &GameExportDialog::OnBrowseRom);
    connect(browse_btn, &QPushButton::clicked, this, &GameExportDialog::OnBrowseOutput);
    connect(export_button, &QPushButton::clicked, this, &GameExportDialog::OnExport);
    connect(install_update_button, &QPushButton::clicked, this,
            &GameExportDialog::OnInstallUpdate);
    connect(rom_path_edit, &QLineEdit::editingFinished, this, [this] {
        if (rom_path_edit->text() != rom_program_id_path) {
            rom_program_id = 0;
        }
        if (!export_in_progress) {
            RefreshUpdateStatus();
        }
    });
    RefreshUpdateStatus();
    const auto update_options = [this, note_label](int) {
        const bool is_windows = platform_combo->currentData().toInt() ==
                                static_cast<int>(TargetPlatform::Windows);
#ifndef SUYU_NO_JIT
        // The exporter has no non-Windows JIT package path. Disable the
        // selectable item as well as the later export guard.
        const int jit_index = backend_combo->findData(static_cast<int>(RecompileBackend::Dynarmic));
        if (jit_index >= 0) {
            if (auto* model = qobject_cast<QStandardItemModel*>(backend_combo->model())) {
                model->item(jit_index)->setEnabled(is_windows);
            }
            if (!is_windows && backend_combo->currentIndex() == jit_index) {
                backend_combo->setCurrentIndex(
                    backend_combo->findData(static_cast<int>(RecompileBackend::Hybrid)));
            }
        }
#endif
        const auto backend =
            static_cast<RecompileBackend>(backend_combo->currentData().toInt());
        const bool uses_aot = backend != RecompileBackend::Dynarmic;
        const bool is_hybrid = backend == RecompileBackend::Hybrid;
        aot_full_scan_checkbox->setEnabled(false);
        fallback_to_interpreter_checkbox->setEnabled(is_hybrid);
        fallback_to_interpreter_checkbox->setChecked(is_hybrid);
        // Only the Windows package path currently bundles a runtime executable.
        // Do not offer Build where it would produce source artifacts and then
        // report success as though a standalone program had been made.
        output_format_combo->setEnabled(uses_aot && is_windows);
        if (!is_windows) {
            output_format_combo->setCurrentIndex(output_format_combo->findData(kFormatSource));
            output_format_combo->setToolTip(
                tr("Build is available for Windows exports only. Linux and macOS exports "
                   "currently contain source artifacts and no bundled runtime."));
        } else {
            output_format_combo->setToolTip(
                tr("Source writes the recompiled C plus a CMakeLists.txt and a build script, and stops "
                   "there. Build additionally runs CMake to completion, producing the standalone "
                   "'recompiled' executable and the shared library suyu loads to run the game on its own "
                   "recompiler. Build can take hours on large titles; the window stays responsive while "
                   "it works."));
        }
        // Only a Windows package has a launcher executable to point a shortcut at: the JIT
        // baseline, or a static or Hybrid Build. A Source export deletes the launcher.
        const bool has_launcher = is_windows && (!uses_aot || WantsCompiledOutput());
        steam_shortcut_checkbox->setEnabled(has_launcher);
        if (!has_launcher) {
            steam_shortcut_checkbox->setChecked(false);
        }
        steam_shortcut_checkbox->setToolTip(
            has_launcher ? tr("Adds the exported game to Steam as a non-Steam shortcut that runs "
                              "its own executable. Restart Steam to see it.")
                         : tr("Needs a Windows export with a launcher: choose Build, or the JIT "
                              "baseline."));
        steam_replace_rom_checkbox->setEnabled(has_launcher &&
                                               steam_shortcut_checkbox->isChecked());
        steam_wikipedia_checkbox->setEnabled(has_launcher && steam_shortcut_checkbox->isChecked());
        discord_checkbox->setEnabled(has_launcher);
        if (backend == RecompileBackend::SuyuStatic) {
            note_label->setText(
                tr("Experimental: translates the game's ARM64 code ahead of time with no JIT "
                   "fallback. Loading and gameplay can be slower. Uncovered code stops execution; "
                   "compatibility must be checked for each title. suyu Hybrid JIT + AOT falls back "
                   "to the JIT instead; performance varies by game."));
        } else if (is_hybrid) {
            note_label->setText(
                tr("Runs recompiled code first and falls back to the Dynarmic JIT for uncovered "
                   "code. Performance varies by game; compare it with the suyu Dynarmic JIT "
                   "export."));
        } else {
            note_label->setText(
                tr("Runs the game with the Dynarmic JIT as a baseline for direct comparison. "
                   "No AOT source is generated."));
        }
        RefreshPackageStatus();
    };
    connect(backend_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            update_options);
    connect(platform_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (platform_combo->currentData().toInt() == static_cast<int>(TargetPlatform::Windows)) {
            output_format_combo->setCurrentIndex(output_format_combo->findData(kFormatBuild));
        }
    });
    connect(platform_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            update_options);
    connect(output_format_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            update_options);
    update_options(backend_combo->currentIndex());
}

// Box art for the Steam step: the lead image of the game's article, from Wikipedia's public
// page summary API (see WikipediaCover::FindCoverUrls for how a page is matched). The lookup
// and the image download share one short deadline; any failure just leaves the icon-based
// artwork. A lookup already made for this export, found or not, is reused instead of repeated.
static QImage FetchWikipediaCover(const QString& title,
                                  const std::optional<WikipediaCover::CoverUrls>& known) {
    constexpr qint64 kBudgetMs = 6000;
    const QString user_agent = QStringLiteral("suyu-game-export (Steam artwork)");
    QNetworkAccessManager network;
    QElapsedTimer clock;
    clock.start();
    const WikipediaCover::CoverUrls urls =
        known ? *known
              : WikipediaCover::FindCoverUrls(network, title, clock, kBudgetMs, user_agent);
    return urls.original.isEmpty()
               ? QImage{}
               : QImage::fromData(WikipediaCover::GetWithin(network, QUrl(urls.original), clock,
                                                            kBudgetMs, user_agent));
}

QString GameExportDialog::MaybeAddToSteam(const QString& game_title, const QString& exe_path,
                                          const QString& backend_label, bool replace,
                                          bool use_wikipedia,
                                          const std::optional<WikipediaCover::CoverUrls>& known) {
    SteamIntegration steam;
    if (!steam.IsSteamInstalled()) {
        return tr("\n\nSteam was not found, so no Steam shortcut was added.");
    }
    if (!QFileInfo(exe_path).isFile()) {
        return tr("\n\nNo launcher executable was found in the package, so no Steam shortcut "
                  "was added.");
    }
    // Always named after the backend, so exports of one game with different backends can be
    // told apart. Replacing removes only the library's own shortcut, which has the plain title.
    const QString app_name = QStringLiteral("%1 (%2)").arg(game_title, backend_label);
    if (!steam.AddLauncherShortcut(app_name, exe_path, replace ? game_title : QString())) {
        return tr("\n\nThe Steam shortcut could not be written. Steam's shortcut list was left "
                  "unchanged.");
    }

    status_label->setText(tr("Adding Steam library artwork..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    const QImage cover = use_wikipedia ? FetchWikipediaCover(game_title, known) : QImage{};
    QString artwork_note;
    if (!steam.WriteLauncherArtwork(app_name, exe_path, game_icon_.toImage(), cover)) {
        artwork_note = tr(" Its library artwork could not be written.");
    }
    const QString added =
        replace ? tr("\n\nAdded to Steam as \"%1\", replacing any \"%2\" shortcut that started the "
                     "game through suyu. Restart Steam to see it.")
                      .arg(app_name, game_title)
                : tr("\n\nAdded to Steam as \"%1\". Restart Steam to see it.").arg(app_name);
    return added + artwork_note;
}

// Deconstructed titles keep the multi-gigabyte RomFS beside the NSOs. Stage
// only the files the loaders read, by role and validated format; RomFS is
// copied once during packaging. Anything else in the folder - notes, key
// files, firmware, links - stops the export with the reason instead of being
// carried along.
static bool CopyDeconstructedExeFs(const QString& src, const QString& dst,
                                   QString* error = nullptr) {
    const auto fail = [error](const QString& reason) {
        LOG_ERROR(Frontend, "Extracted ExeFS rejected: {}", reason.toStdString());
        if (error) {
            *error = reason;
        }
        return false;
    };
    const auto source = ToFsPath(src);
    const auto destination = ToFsPath(dst);
    if (PackagePolicy::Overlaps(source, destination)) {
        return fail(QStringLiteral("the ExeFS folder and the export folder overlap"));
    }
    const PackagePolicy::ExeFsPlan plan = PackagePolicy::PlanExtractedExeFs(source);
    if (!plan.Ok()) {
        return fail(QString::fromStdString(PackagePolicy::Describe(plan.rejected)));
    }
    for (const auto& ignored : plan.ignored) {
        LOG_INFO(Frontend, "Extracted ExeFS: not copying {}", ignored);
    }
    std::string reason;
    if (!PackagePolicy::RemoveTreeNoFollow(destination, &reason) || !QDir().mkpath(dst)) {
        return fail(QStringLiteral("cannot prepare %1").arg(dst));
    }
    for (const auto& file : plan.files) {
        if (file.role == PackagePolicy::ExeFsRole::RomFs ||
            file.role == PackagePolicy::ExeFsRole::ControlNca) {
            continue;
        }
        if (!PackagePolicy::CopyFileNoFollow(source, file.source, destination / file.name,
                                             &reason)) {
            return fail(QString::fromStdString(reason));
        }
    }
    return true;
}

// An extracted ExeFS names its title in main.npdm. When the selected title is
// known, an ExeFS belonging to another one is not packaged under its name.
static bool NpdmMatchesTitle(const QString& exefs_dir, u64 program_id, QString* error) {
    if (program_id == 0) {
        return true;
    }
    static const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    const auto npdm = vfs->OpenFile(QString(exefs_dir + QStringLiteral("/main.npdm")).toStdString(),
                                    FileSys::OpenMode::Read);
    if (!npdm) {
        return true;
    }
    FileSys::ProgramMetadata metadata;
    if (metadata.Load(npdm) != Loader::ResultStatus::Success) {
        *error = QStringLiteral("main.npdm cannot be read");
        return false;
    }
    if (FileSys::GetBaseTitleID(metadata.GetTitleID()) != FileSys::GetBaseTitleID(program_id)) {
        *error = QStringLiteral("main.npdm belongs to title %1, not the selected %2")
                     .arg(metadata.GetTitleID(), 16, 16, QLatin1Char('0'))
                     .arg(program_id, 16, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

// Fingerprint the effective files the recompiler will consume, including the
// file names. A ROM path or mtime alone does not identify an installed update.
static QString HashExeFsFiles(const FileSys::VirtualDir& vdir, const QString& directory) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr u64 chunk_size = 4ULL * 1024 * 1024;
    const auto add_file_identity = [&hash](const QByteArray& name, u64 size) {
        hash.addData(QByteArray::number(name.size()));
        hash.addData(QByteArrayView(":", 1));
        hash.addData(name);
        hash.addData(QByteArrayView(":", 1));
        hash.addData(QByteArray::number(size));
        hash.addData(QByteArrayView(":", 1));
    };
    if (vdir) {
        auto files = vdir->GetFiles();
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return a->GetName() < b->GetName();
        });
        if (files.empty()) {
            return {};
        }
        for (const auto& file : files) {
            const QByteArray name = QByteArray::fromStdString(file->GetName());
            add_file_identity(name, file->GetSize());
            for (u64 offset = 0; offset < file->GetSize(); offset += chunk_size) {
                const u64 length = std::min(chunk_size, file->GetSize() - offset);
                const auto bytes = file->ReadBytes(length, offset);
                if (bytes.size() != length) {
                    return {};
                }
                hash.addData(QByteArrayView(reinterpret_cast<const char*>(bytes.data()),
                                            static_cast<qsizetype>(bytes.size())));
            }
        }
    } else {
        const QDir dir(directory);
        if (!dir.exists()) {
            return {};
        }
        const auto files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        bool found = false;
        for (const auto& info : files) {
            if (info.fileName().compare(QStringLiteral("romfs.bin"), Qt::CaseInsensitive) == 0) {
                continue;
            }
            found = true;
            const QByteArray name = info.fileName().toUtf8();
            add_file_identity(name, info.size());
            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) {
                return {};
            }
            qint64 remaining = info.size();
            while (remaining > 0) {
                const QByteArray bytes = file.read(std::min<qint64>(remaining, chunk_size));
                if (bytes.isEmpty()) {
                    return {};
                }
                hash.addData(bytes);
                remaining -= bytes.size();
            }
        }
        if (!found) {
            return {};
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

static bool PrepareAotSourcePackage(const QString& pkg_dir, const QString& package_name,
                                    const QString& cache_dir) {
    // A prior Build can leave a launcher in the same package. Source is C only.
    const auto remove_file = [](const QString& path) {
        return !QFile::exists(path) || QFile::remove(path);
    };
    if (!remove_file(pkg_dir + QDir::separator() + package_name + QStringLiteral(".exe")) ||
        !remove_file(pkg_dir + QStringLiteral("/launch.bat"))) {
        return false;
    }
    for (const char* dll : {"avcodec-61.dll", "avformat-61.dll", "avutil-59.dll",
                            "dxcompiler.dll", "dxil.dll", "libcrypto.dll", "libssl.dll",
                            "libcrypto-3-x64.dll", "libssl-3-x64.dll", "swresample-5.dll",
                            "swscale-8.dll"}) {
        if (!remove_file(pkg_dir + QDir::separator() + QString::fromLatin1(dll))) {
            return false;
        }
    }
    const QString stale_launcher_dir = cache_dir + QStringLiteral("/launcher");
    if (QDir(stale_launcher_dir).exists() && !QDir(stale_launcher_dir).removeRecursively()) {
        return false;
    }
    QFile readme(pkg_dir + QDir::separator() + QStringLiteral("README_NATIVE_EXPORT.txt"));
    if (!readme.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream out(&readme);
    out << "AOT source export (local game export); no compiled launcher is included.\n\n"
           "Generated C, CMake project files, and build scripts are in aot_cache/. The C is\n"
           "translated from the game's code: it is game-derived material, not a content-free\n"
           "project. It carries none of the game's own files - no modules, data segments,\n"
           "ExeFS or RomFS - and builds only libraries that suyu loads alongside your game\n"
           "file, read with your keys. There is no standalone program to build from it.\n\n"
        << kExportContentNotice;
    return out.status() == QTextStream::Ok;
}

static bool ReadCachedFallbackPolicy(const QByteArray& manifest, bool requested,
                                     QStringList& cached_modules) {
    const auto document = QJsonDocument::fromJson(manifest);
    if (!document.isObject()) {
        return false;
    }
    const auto object = document.object();
    if (!object.contains(QStringLiteral("fallback_enabled")) ||
        !object.value(QStringLiteral("fallback_enabled")).isBool() ||
        object.value(QStringLiteral("fallback_enabled")).toBool() != requested ||
        !object.value(QStringLiteral("fallback_modules")).isArray()) {
        return false;
    }
    QStringList modules;
    for (const auto& value : object.value(QStringLiteral("fallback_modules")).toArray()) {
        if (!value.isString()) {
            return false;
        }
        modules.append(value.toString());
    }
    if (!requested && !modules.isEmpty()) {
        return false;
    }
    cached_modules = std::move(modules);
    return true;
}

// SUYU_AOT_JIT_MODULES=main,sdk leaves the named modules to the JIT in a Hybrid
// export without recompiling them. A module whose code links past the 2 GB
// image limit (TOTK's main) can't be in the image at all.
static QStringList RequestedJitModules() {
    QStringList names;
    for (const auto& name : qEnvironmentVariable("SUYU_AOT_JIT_MODULES")
                                .split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        names.append(name.trimmed());
    }
    return names;
}

static bool WritePortableVersionOverride(const QString& config_path, u32 app_version,
                                         const QString& display_version) {
    const bool has_override = app_version != 0 || !display_version.isEmpty();
    if (!has_override && !QFile::exists(config_path)) {
        return true;
    }
    if (has_override && !QDir().mkpath(QFileInfo(config_path).absolutePath())) {
        return false;
    }
    // QSettings writes "a/b" as the "a\b" key frontend_common reads. A literal backslash in
    // the name never becomes that key (Qt 6.10 writes "application_version_overridedefault"),
    // so the "\default" flag was lost and an existing "\default=true" made suyu-cmd ignore
    // the value.
    QSettings portable(config_path, QSettings::IniFormat);
    const QString number = QStringLiteral("System/application_version_override");
    const QString display = QStringLiteral("System/application_display_version_override");
    if (has_override) {
        portable.setValue(number, app_version);
        portable.setValue(number + QStringLiteral("/default"), false);
        portable.setValue(number + QStringLiteral("/use_global"), true);
        portable.setValue(display, display_version);
        portable.setValue(display + QStringLiteral("/default"), false);
        portable.setValue(display + QStringLiteral("/use_global"), true);
    } else {
        for (const auto& key : {number, display}) {
            portable.remove(key);
            portable.remove(key + QStringLiteral("/default"));
            portable.remove(key + QStringLiteral("/use_global"));
        }
    }
    portable.sync();
    return portable.status() == QSettings::NoError;
}

// A package reads only its own sdl2-config.ini, so it used to play on suyu-cmd's defaults
// (FIFO vsync among them) whatever the user had chosen in suyu. Both frontends store settings
// under the same keys, so the global values that decide how the game runs are copied across,
// then this game's own overrides when its custom configuration is included: the package never
// reads config/custom itself, and that file is never copied. Only the settings in
// PackagePolicy::ExportableSettings() are read from either file, so paths, devices, accounts,
// web credentials and debugging or unsafe options stay behind even when they share a section.
static bool SeedPortableConfig(const QString& config_path, const QString& global_path,
                               const QString& custom_path) {
    if (!QFile::exists(global_path) && (custom_path.isEmpty() || !QFile::exists(custom_path))) {
        return true;
    }
    if (!QDir().mkpath(QFileInfo(config_path).absolutePath())) {
        return false;
    }
    // "Section/label" as QSettings names them; it reads a key's "label\default" companion
    // as "label/default".
    QStringList keys;
    for (const std::string_view key : PackagePolicy::ExportableSettings()) {
        keys.append(QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size())));
    }

    QSettings portable(config_path, QSettings::IniFormat);
    const QString is_default = QStringLiteral("/default");
    if (QFile::exists(global_path)) {
        const QSettings global(global_path, QSettings::IniFormat);
        for (const QString& key : keys) {
            if (global.contains(key)) {
                portable.setValue(key, global.value(key));
            }
            if (global.contains(key + is_default)) {
                portable.setValue(key + is_default, global.value(key + is_default));
            }
        }
    }

    if (!custom_path.isEmpty() && QFile::exists(custom_path)) {
        // A per-game file stores a value only where "use_global" is false.
        const QSettings custom(custom_path, QSettings::IniFormat);
        for (const QString& key : keys) {
            if (custom.value(key + QStringLiteral("/use_global")).toString() ==
                    QStringLiteral("false") &&
                custom.contains(key)) {
                portable.setValue(key, custom.value(key));
                portable.setValue(key + is_default,
                                  custom.value(key + is_default, QStringLiteral("false")));
            }
        }
    }
    portable.sync();
    return portable.status() == QSettings::NoError;
}

// Every key a shipped config holds must be on the allowlist, or be one of the version
// keys WritePortableVersionOverride owns. Checked on the written file, not on intent.
static bool PortableConfigIsClean(const QString& config_path, QStringList* stray) {
    if (!QFile::exists(config_path)) {
        return true;
    }
    static const QStringList kVersionKeys{
        QStringLiteral("System/application_version_override"),
        QStringLiteral("System/application_display_version_override")};
    const QSettings ini(config_path, QSettings::IniFormat);
    for (const QString& key : ini.allKeys()) {
        QString base = key;
        for (const QString& suffix : {QStringLiteral("/default"), QStringLiteral("/use_global")}) {
            if (base.endsWith(suffix)) {
                base.chop(suffix.size());
                break;
            }
        }
        const QByteArray utf8 = base.toUtf8();
        if (kVersionKeys.contains(base) ||
            PackagePolicy::IsExportableSettingKey(std::string_view{utf8.constData(),
                                                                   static_cast<size_t>(utf8.size())})) {
            continue;
        }
        stray->append(key);
    }
    return stray->isEmpty();
}

void GameExportDialog::SetLibraryEntries(QVector<LibraryEntry> entries) {
    library_entries_ = std::move(entries);
}

void GameExportDialog::SetGameIcon(const QPixmap& icon) {
    game_icon_ = icon;
}

void GameExportDialog::SetRomPath(const QString& path, quint64 program_id) {
    rom_path_edit->setText(path);
    rom_program_id = program_id;
    rom_program_id_path = path;
    RefreshUpdateStatus();
}

void GameExportDialog::TriggerExportForTesting(const QString& rom_path, const QString& output_dir,
                                               int format_index) {
    if (format_index >= 0 && output_format_combo &&
        format_index < output_format_combo->count()) {
        output_format_combo->setCurrentIndex(format_index);
    }
    SetRomPath(rom_path);
    test_app_version = app_version;
    test_display_version = display_version;
    if (full_scan >= 0 && aot_full_scan_checkbox) {
        aot_full_scan_checkbox->setChecked(full_scan != 0);
    }
    output_path_edit->setText(output_dir);
    test_driven_export = true;
    test_export_has_result = false;
    test_export_succeeded = false;
    test_export_output.clear();
    OnExport();
    test_driven_export = false;
    test_app_version = 0;
    test_display_version.clear();
}

bool GameExportDialog::IsExportInProgressForTesting() const {
    return export_in_progress;
}

bool GameExportDialog::HasExportResultForTesting() const {
    return test_export_has_result;
}

bool GameExportDialog::ExportSucceededForTesting() const {
    return test_export_succeeded;
}

int GameExportDialog::ExportProgressForTesting() const {
    return progress_bar ? progress_bar->value() : 0;
}

QString GameExportDialog::ExportStatusForTesting() const {
    return status_label ? status_label->text() : QString{};
}

QString GameExportDialog::ExportOutputForTesting() const {
    return test_export_output;
}

QStringList GameExportDialog::FallbackModulesForTesting() const {
    return last_fallback_modules;
}

QString GameExportDialog::CoverageStatusForTesting() const {
    return coverage_status_label ? coverage_status_label->text() : QString{};
}

void GameExportDialog::OnBrowseRom() {
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Select ROM"), QString(),
        tr("Switch game files (*.nsp *.xci *.nca *.nro);;All files (*.*)"), nullptr,
        QFileDialog::ReadOnly | QFileDialog::DontUseNativeDialog);
    if (!file.isEmpty()) {
        rom_path_edit->setText(file);
        rom_program_id = 0;
        RefreshUpdateStatus();
    }
}

// Cartridge dumps and some NSPs carry the update next to the base game; the ExeFS
// extractor applies it from there, so such a game needs nothing installed.
// The NSP, or an XCI's secure partition, of a game file; null when it cannot be read.
static std::shared_ptr<FileSys::NSP> OpenRomContainer(const QString& rom_path) {
    static const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    const auto file = vfs->OpenFile(rom_path.toStdString(), FileSys::OpenMode::Read);
    if (!file) {
        return nullptr;
    }
    std::shared_ptr<FileSys::NSP> nsp;
    if (rom_path.endsWith(QStringLiteral(".nsp"), Qt::CaseInsensitive)) {
        nsp = std::make_shared<FileSys::NSP>(file);
    } else if (rom_path.endsWith(QStringLiteral(".xci"), Qt::CaseInsensitive)) {
        const FileSys::XCI xci{file};
        nsp = xci.GetSecurePartitionNSP();
    }
    return nsp && nsp->GetStatus() == Loader::ResultStatus::Success ? nsp : nullptr;
}

// The version of the update packed in a game file: the title version from its CNMT and the
// display version from its control data. Each is left alone when the file does not say.
static void ReadRomFileUpdateVersion(const QString& rom_path, const FileSys::PatchManager& pm,
                                     u32& title_version, QString& display) {
    const auto nsp = OpenRomContainer(rom_path);
    if (!nsp) {
        return;
    }
    const u64 update_tid = FileSys::GetUpdateTitleID(pm.GetTitleID());
    if (const auto meta = nsp->GetNCA(update_tid, FileSys::ContentRecordType::Meta,
                                      FileSys::TitleType::Update);
        meta && !meta->GetSubdirectories().empty()) {
        for (const auto& file : meta->GetSubdirectories()[0]->GetFiles()) {
            if (file->GetExtension() == "cnmt") {
                title_version = FileSys::CNMT{file}.GetTitleVersion();
                break;
            }
        }
    }
    if (const auto control = nsp->GetNCA(update_tid, FileSys::ContentRecordType::Control,
                                         FileSys::TitleType::Update)) {
        const auto metadata = pm.ParseControlNCA(*control);
        if (metadata.first) {
            display = QString::fromStdString(metadata.first->GetVersionString());
        }
    }
}

static bool RomFileIncludesUpdate(const QString& rom_path, u64 program_id) {
    const auto nsp = OpenRomContainer(rom_path);
    if (!nsp) {
        return false;
    }
    const auto update = nsp->GetNCA(FileSys::GetUpdateTitleID(program_id),
                                    FileSys::ContentRecordType::Program,
                                    FileSys::TitleType::Update);
    // An update whose code cannot be opened is skipped by the extractors, so it does not count.
    // Its status is not checked: an update NCA opened without its base always reports
    // ErrorMissingBKTRBaseRomFS, yet its ExeFS - all the extractors take from it - is fine.
    return update != nullptr && update->GetExeFS() != nullptr;
}

quint64 GameExportDialog::SelectedProgramId() const {
    if (rom_program_id != 0) {
        return rom_program_id;
    }
    const QString path = rom_path_edit->text();
    if (path.isEmpty() || !QFile::exists(path)) {
        return 0;
    }
    static const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    // An extracted folder names its title in main.npdm, read through its main module.
    QString file_path = path;
    if (QFileInfo(path).isDir()) {
        file_path = QDir(path).exists(QStringLiteral("exefs"))
                        ? path + QStringLiteral("/exefs/main")
                        : path + QStringLiteral("/main");
    }
    const auto file = vfs->OpenFile(file_path.toStdString(), FileSys::OpenMode::Read);
    if (!file) {
        return 0;
    }
    // Parsing opens and decrypts the container headers, so remember the answer per path.
    if (path == cached_program_id_path) {
        return cached_program_id;
    }
    const auto loader = Loader::GetLoader(system_, file);
    u64 program_id{};
    if (!loader || loader->ReadProgramId(program_id) != Loader::ResultStatus::Success) {
        program_id = 0;
    }
    cached_program_id_path = path;
    cached_program_id = program_id;
    return program_id;
}

GameExportDialog::UpdateState GameExportDialog::CurrentUpdateState(QString* version,
                                                                   QString* source,
                                                                   quint32* title_version) const {
    const QString rom_path = rom_path_edit->text();
    if (rom_path.isEmpty()) {
        return UpdateState::NoGame;
    }
    // Only NSP and XCI exports go through the update: a standalone NCA exports its own ExeFS,
    // and pairing an installed update's RomFS with it is refused as inconsistent.
    if (!rom_path.endsWith(QStringLiteral(".nsp"), Qt::CaseInsensitive) &&
        !rom_path.endsWith(QStringLiteral(".xci"), Qt::CaseInsensitive)) {
        return UpdateState::NotApplicable;
    }
    const u64 program_id = SelectedProgramId();
    if (program_id == 0) {
        return UpdateState::NoGame;
    }
    // Ask PatchManager the question the extractors will: whether the add-on settings let an
    // update replace the ExeFS and the RomFS. Reading the Add-Ons list instead gets this wrong,
    // since one "Update (SDMC)" flag turns off a NAND copy too.
    const FileSys::PatchManager pm{program_id, system_.GetFileSystemController(),
                                   system_.GetContentProvider()};
    const auto selection = pm.GetUpdateSelection();
    const u64 update_id = FileSys::GetUpdateTitleID(program_id);
    const auto& content_provider = system_.GetContentProvider();
    // Includes updates the game list registered from game files, which PatchManager uses too.
    const bool any_update = content_provider.HasEntry(update_id, FileSys::ContentRecordType::Program);
    // An enabled installed update wins over one packed in the file for both ExeFS and RomFS.
    if (selection.installed_exefs) {
        // The very update PatchExeFS will apply, so the source and version shown are its.
        const auto update = pm.GetExeFSUpdate();
        // PatchExeFS silently keeps the base code when the update's ExeFS cannot be opened.
        // Status is not checked: without its base an update NCA always reports
        // ErrorMissingBKTRBaseRomFS, even when its ExeFS reads fine.
        if (!update || FileSys::NCA{update->program}.GetExeFS() == nullptr) {
            return UpdateState::Unreadable;
        }
        if (version) {
            // The display version ("4.0.0") from the control data, with the update applied the
            // same way the patcher applies it.
            auto metadata = pm.GetControlMetadata();
            // That needs the base game's control data in a provider, which a browsed file with
            // its update in NAND lacks; the update carries control data of its own.
            if (!metadata.first) {
                if (const auto control = content_provider.GetEntry(
                        update_id, FileSys::ContentRecordType::Control)) {
                    metadata = pm.ParseControlNCA(*control);
                }
            }
            *version = metadata.first
                           ? QString::fromStdString(metadata.first->GetVersionString())
                           : QString::fromStdString(update->version_string);
        }
        if (title_version) {
            *title_version = update->version;
        }
        if (source) {
            // Only a NAND copy is a file of its own worth naming; the others live inside a
            // container file or a directory listing, so the source is named instead.
            using Slot = FileSys::ContentProviderUnionSlot;
            const auto slot = update->slot;
            const bool nand = slot && (*slot == Slot::SysNAND || *slot == Slot::UserNAND);
            const QString where = !slot                     ? tr("installed content")
                                  : *slot == Slot::SysNAND  ? tr("suyu's system NAND")
                                  : *slot == Slot::UserNAND ? tr("suyu's NAND")
                                  : *slot == Slot::SDMC     ? tr("suyu's SD card")
                                  : *slot == Slot::External ? tr("an external content folder")
                                                            : tr("a game in the game list");
            // The game list registers the NCAs of every game file it scans, so an update found
            // there or in a content folder is this very file's own when the file carries one.
            const bool this_file = slot &&
                                   (*slot == Slot::FrontendManual || *slot == Slot::External) &&
                                   RomFileIncludesUpdate(rom_path, program_id);
            *source = this_file ? tr("this game file (bundled)")
                      : nand    ? QStringLiteral("%1: %2").arg(
                                   where, QDir::toNativeSeparators(QString::fromStdString(
                                              update->program->GetFullPath())))
                                : where;
        }
        return UpdateState::Installed;
    }
    // A packed update reaches the ExeFS unconditionally but the RomFS only while updates are
    // enabled; otherwise the pair would not match and the export refuses it.
    if (RomFileIncludesUpdate(rom_path, program_id)) {
        if (source) {
            *source = tr("this game file (bundled)");
        }
        if (version || title_version) {
            u32 number = 0;
            QString display;
            ReadRomFileUpdateVersion(rom_path, pm, number, display);
            if (version) {
                *version = display;
            }
            if (title_version) {
                *title_version = number;
            }
        }
        return selection.romfs_enabled ? UpdateState::Bundled : UpdateState::BundledDisabled;
    }
    return any_update ? UpdateState::Disabled : UpdateState::None;
}

void GameExportDialog::RefreshUpdateStatus() {
    if (!update_status_label) {
        return;
    }
    // Save data, shaders and per-game config are all keyed by title ID; the library may not
    // supply one (cartridge dumps, browsed files), so read it the same way the Update row does.
    // Only disable them: the dialog can refresh before the game is known, and unticking
    // here left save data, shaders and config out of every later export. The export
    // itself skips portable data when there is no title ID.
    const bool allow_portable_data = SelectedProgramId() != 0;
    for (QCheckBox* box : {include_save_data_checkbox, include_shader_cache_checkbox,
                           include_custom_config_checkbox}) {
        box->setEnabled(allow_portable_data);
    }
    QString version;
    QString source;
    const UpdateState state = CurrentUpdateState(&version, &source);
    install_update_button->setEnabled(state != UpdateState::NoGame &&
                                      state != UpdateState::NotApplicable);
    const bool used = state == UpdateState::Installed || state == UpdateState::Bundled;
    if (update_source_label) {
        const QString line = tr("From: %1").arg(source);
        update_source_label->setText(update_source_label->fontMetrics().elidedText(
            line, Qt::ElideMiddle, std::max(width() - 40, 400)));
        update_source_label->setToolTip(line);
        update_source_label->setVisible(used && !source.isEmpty());
    }
    switch (state) {
    case UpdateState::NoGame:
        update_status_label->setText(tr("Select a game to check for an update."));
        break;
    case UpdateState::NotApplicable:
        update_status_label->setText(
            tr("Updates apply to .nsp and .xci games. This file exports as it is."));
        break;
    case UpdateState::None:
        update_status_label->setText(
            tr("No update installed. The export will use the base game version."));
        break;
    case UpdateState::Disabled:
        update_status_label->setText(
            tr("Updates are turned off for this game (Properties > Add-Ons), so the export "
               "will not use its update."));
        break;
    case UpdateState::Installed:
        update_status_label->setText(
            version.isEmpty()
                ? tr("An update is available and turned on. The export will use it.")
                : tr("Update %1 is available and turned on. The export will use it.")
                      .arg(version));
        break;
    case UpdateState::Bundled:
        update_status_label->setText(
            version.isEmpty()
                ? tr("Update included in this game file. The export will use it.")
                : tr("Update %1 included in this game file. The export will use it.")
                      .arg(version));
        break;
    case UpdateState::BundledDisabled:
        update_status_label->setText(
            tr("This game file includes an update, but updates are turned off for this game "
               "(Properties > Add-Ons). Turn them on to export it."));
        break;
    case UpdateState::Unreadable:
        update_status_label->setText(
            tr("The installed update cannot be read. It may need keys that are not installed, "
               "or be damaged. Reinstall it, or the export will use the base game version."));
        break;
    }
    RefreshCoverageStatus();
    RefreshPackageStatus();
}

// The keys installed in suyu: whether prod.keys is there, and whether they include an sd_seed,
// which seals a portable export to this console.
static bool InstalledProdKeysPresent() {
    const auto keys_dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::KeysDir);
    std::error_code ec;
    return std::filesystem::is_regular_file(keys_dir / "prod.keys", ec) ||
           std::filesystem::is_regular_file(keys_dir / "prod.keys_autogenerated", ec);
}

static bool InstalledKeysHaveSdSeed() {
    return Core::Crypto::KeyManager::Instance().HasKey(Core::Crypto::S128KeyType::SDSeed);
}

bool GameExportDialog::PortableAvailable(QString* reason) const {
    const auto unavailable = [reason](const QString& why) {
        if (reason) {
            *reason = why;
        }
        return false;
    };
    const QString rom_path = rom_path_edit ? rom_path_edit->text() : QString{};
    const QFileInfo rom_info(rom_path);
    if (rom_path.isEmpty() || !rom_info.isFile()) {
        return unavailable(tr("select an .nsp or .xci game file first."));
    }
    if (!rom_path.endsWith(QStringLiteral(".nsp"), Qt::CaseInsensitive) &&
        !rom_path.endsWith(QStringLiteral(".xci"), Qt::CaseInsensitive)) {
        return unavailable(tr("it needs an .nsp or .xci game file."));
    }
    if (platform_combo &&
        platform_combo->currentData().toInt() != static_cast<int>(TargetPlatform::Windows)) {
        return unavailable(tr("it is available for Windows exports only."));
    }
    if (backend_combo &&
        backend_combo->currentData().toInt() != static_cast<int>(RecompileBackend::Dynarmic) &&
        !WantsCompiledOutput()) {
        return unavailable(tr("a Source export has no launcher to run it."));
    }
    if (!InstalledProdKeysPresent()) {
        return unavailable(tr("no keys are installed in suyu."));
    }
    if (!InstalledKeysHaveSdSeed()) {
        return unavailable(tr("the installed keys have no sd_seed, which the seal is made from."));
    }
    return true;
}

std::vector<GameExportDialog::SealSource> GameExportDialog::PortableSources(QString* error) const {
    std::vector<SealSource> sources;
    const QString rom_path = rom_path_edit->text();
    static const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    const u64 program_id = SelectedProgramId();
    auto base = vfs->OpenFile(rom_path.toStdString(), FileSys::OpenMode::Read);
    if (!base || program_id == 0) {
        *error = tr("the game file could not be opened");
        return {};
    }
    sources.push_back({QString::fromLatin1(PackagePolicy::kPortableBaseName.data(),
                                           static_cast<qsizetype>(
                                               PackagePolicy::kPortableBaseName.size())),
                       QStringLiteral("base"), program_id, -1, std::move(base)});
    // The installed update the export uses, as suyu installed it: its NCAs from the NAND, still
    // in their original encryption. An update packed in the game file travels inside it.
    const auto add_update = [&]() -> bool {
        const FileSys::PatchManager pm{program_id, system_.GetFileSystemController(),
                                       system_.GetContentProvider()};
        if (!pm.GetUpdateSelection().installed_exefs) {
            return true;
        }
        const auto update = pm.GetExeFSUpdate();
        using Slot = FileSys::ContentProviderUnionSlot;
        if (!update || !update->slot || (*update->slot != Slot::UserNAND &&
                                         *update->slot != Slot::SysNAND)) {
            return true;
        }
        const auto* provider = system_.GetContentProviderUnion().GetSlotProvider(*update->slot);
        if (provider == nullptr) {
            return true;
        }
        const u64 update_id = FileSys::GetUpdateTitleID(program_id);
        int index = 0;
        for (const auto type :
             {FileSys::ContentRecordType::Meta, FileSys::ContentRecordType::Program,
              FileSys::ContentRecordType::Data, FileSys::ContentRecordType::Control,
              FileSys::ContentRecordType::HtmlDocument,
              FileSys::ContentRecordType::LegalInformation}) {
            if (auto file = provider->GetEntryRaw(update_id, type)) {
                sources.push_back({QStringLiteral("update-%1.sealed").arg(index++),
                                   QStringLiteral("update"), update_id, static_cast<int>(type),
                                   std::move(file)});
            } else if (type == FileSys::ContentRecordType::Program) {
                *error = tr("the installed update's program could not be opened");
                return false;
            }
        }
        return true;
    };
    if (!add_update()) {
        return {};
    }
    auto dlc = InstalledDlcSources();
    sources.insert(sources.end(), std::make_move_iterator(dlc.begin()),
                   std::make_move_iterator(dlc.end()));
    return sources;
}

std::vector<GameExportDialog::SealSource> GameExportDialog::InstalledDlcSources() const {
    // Each DLC title of the game installed in suyu's NAND, and all of its NCAs as suyu installed
    // them, in their original encryption. Nothing is decrypted or extracted here.
    std::vector<SealSource> sources;
    const u64 program_id = SelectedProgramId();
    if (program_id == 0) {
        return sources;
    }
    using Slot = FileSys::ContentProviderUnionSlot;
    const auto& content = system_.GetContentProviderUnion();
    std::map<u64, const FileSys::ContentProvider*> titles;
    for (const auto slot : {Slot::UserNAND, Slot::SysNAND}) {
        const auto* provider = content.GetSlotProvider(slot);
        if (provider == nullptr) {
            continue;
        }
        for (const auto& entry : provider->ListEntriesFilter(FileSys::TitleType::AOC)) {
            if (PackagePolicy::IsAddOnContentOf(entry.title_id, program_id)) {
                titles.try_emplace(entry.title_id, provider);
            }
        }
    }
    int index = 0;
    for (const auto& [title_id, provider] : titles) {
        for (int type = 0; type < static_cast<int>(FileSys::ContentRecordType::Count); ++type) {
            if (auto file =
                    provider->GetEntryRaw(title_id, static_cast<FileSys::ContentRecordType>(type))) {
                sources.push_back({QStringLiteral("dlc-%1.sealed").arg(index++),
                                   QStringLiteral("dlc"), title_id, type, std::move(file)});
            }
        }
    }
    return sources;
}

void GameExportDialog::RefreshPackageStatus() {
    if (!package_type_combo || !keys_status_label || !game_file_status_label) {
        return;
    }
    const bool has_keys = InstalledProdKeysPresent();
    const bool has_seed = has_keys && InstalledKeysHaveSdSeed();
    keys_status_label->setText(
        !has_keys  ? tr("Not installed (Tools > Install Decryption Keys)")
        : has_seed ? tr("Installed, including this console's sd_seed")
                   : tr("Installed, without an sd_seed (needed only for Portable)"));

    const QString rom_path = rom_path_edit->text();
    const QFileInfo rom_info(rom_path);
    if (rom_path.isEmpty()) {
        game_file_status_label->setText(tr("No game selected"));
    } else if (rom_info.isDir()) {
        game_file_status_label->setText(tr("Extracted folder: %1").arg(rom_info.fileName()));
    } else if (rom_info.isFile()) {
        game_file_status_label->setText(
            QStringLiteral("%1 (%2)").arg(rom_info.fileName(),
                                          QLocale().formattedDataSize(rom_info.size())));
    } else {
        game_file_status_label->setText(tr("Not found: %1").arg(rom_path));
    }
    if (dlc_status_label) {
        std::set<quint64> dlc_titles;
        quint64 dlc_size = 0;
        for (const auto& source : InstalledDlcSources()) {
            dlc_titles.insert(source.title_id);
            dlc_size += source.file->GetSize();
        }
        dlc_status_label->setText(
            dlc_titles.empty()
                ? tr("None installed")
                : tr("%1 installed (%2) — included in portable exports")
                      .arg(dlc_titles.size())
                      .arg(QLocale().formattedDataSize(static_cast<qint64>(dlc_size))));
    }

    QString reason;
    const bool available = PortableAvailable(&reason);
    if (auto* model = qobject_cast<QStandardItemModel*>(package_type_combo->model())) {
        model->item(1)->setEnabled(available);
    }
    if (!available && package_type_combo->currentIndex() == 1) {
        package_type_combo->setCurrentIndex(0);
    }
    const bool portable = package_type_combo->currentIndex() == 1;
    if (content_label) {
        content_label->setText(
            portable ? tr("This export contains your game file and any installed update and "
                          "DLC, sealed to this console's keys. Keys and system firmware are "
                          "never copied. Do not upload exports or their logs to suyu's release "
                          "or support channels.")
                     : tr("An export does not contain the game. Each launch reads the game file "
                          "you selected and decrypts it with your own installed keys; AOT "
                          "exports (Source too) contain code translated from it. Keys and "
                          "system firmware are never copied. Do not upload exports or their "
                          "logs to suyu's release or support channels."));
    }
    if (!portable) {
        package_note_label->setText(
            available ? tr("The export reads your game file from where it is now each time it "
                           "starts.")
                      : tr("The export reads your game file from where it is now each time it "
                           "starts. Portable is not available: %1")
                            .arg(reason));
        return;
    }
    QString error;
    const auto sources = PortableSources(&error);
    quint64 extra = 0;
    for (const auto& source : sources) {
        extra += source.file->GetSize();
    }
    const auto has_role = [&sources](const QString& role) {
        return std::any_of(sources.begin(), sources.end(),
                           [&role](const SealSource& source) { return source.role == role; });
    };
    const bool with_update = has_role(QStringLiteral("update"));
    const bool with_dlc = has_role(QStringLiteral("dlc"));
    package_note_label->setText(
        tr("The game file%1 is copied as it is, in its original encryption, and sealed to this "
           "console's keys. On first launch the export asks for your keys and remembers them on "
           "that PC. Keys and firmware are never copied into the export and must come from a "
           "Switch you own. Adds about %2 to the export.")
            .arg(with_update && with_dlc ? tr(" and its installed update and DLC")
                 : with_update           ? tr(" and its installed update")
                 : with_dlc              ? tr(" and its installed DLC")
                                         : QString{},
                 QLocale().formattedDataSize(static_cast<qint64>(extra))) +
        (error.isEmpty() ? QString{} : QStringLiteral(" ") + tr("Note: %1.").arg(error)));
}

bool GameExportDialog::SealGameFiles(const std::vector<SealSource>& sources,
                                     const QString& package_root, const std::string& export_id,
                                     quint64 program_id, QString* error, bool* cancelled) {
    namespace Seal = Core::Crypto::PortableSeal;
    *cancelled = false;
    auto& keys = Core::Crypto::KeyManager::Instance();
    if (!keys.HasKey(Core::Crypto::S128KeyType::SDSeed)) {
        *error = tr("the installed keys have no sd_seed");
        return false;
    }
    const auto seal_key =
        Seal::DeriveKey(keys.GetKey(Core::Crypto::S128KeyType::SDSeed), export_id);
    const auto check = seal_key ? Seal::ComputeCheck(*seal_key) : std::nullopt;
    if (!check) {
        *error = tr("the seal key could not be computed");
        return false;
    }
    const QString game_dir =
        package_root + QLatin1Char('/') +
        QString::fromLatin1(PackagePolicy::kPortableGameDir.data(),
                            static_cast<qsizetype>(PackagePolicy::kPortableGameDir.size()));
    if (!QDir().mkpath(game_dir)) {
        *error = tr("cannot create the game folder in the package");
        return false;
    }
    quint64 total = 0;
    for (const auto& source : sources) {
        total += source.file->GetSize();
    }
    // The copy is as large as the game; say so up front rather than fail part way.
    const QStorageInfo storage(game_dir);
    constexpr quint64 kMargin = 64ull << 20;
    if (storage.isValid() && storage.bytesAvailable() >= 0 &&
        static_cast<quint64>(storage.bytesAvailable()) < total + kMargin) {
        *error = tr("not enough free space in the output folder: the game file needs %1, %2 is "
                    "free")
                     .arg(QLocale().formattedDataSize(static_cast<qint64>(total)),
                          QLocale().formattedDataSize(storage.bytesAvailable()));
        return false;
    }

    QProgressDialog progress(tr("Copying the game file into the export..."), tr("Cancel"), 0,
                             1000, this);
    progress.setWindowTitle(tr("Portable Export"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(1000);
    progress.setAutoClose(false);
    progress.setAutoReset(false);

    constexpr std::size_t kChunk = 4u << 20;
    std::vector<u8> buffer(kChunk);
    quint64 done = 0;
    QJsonArray files;
    for (const auto& source : sources) {
        const auto nonce = Seal::RandomNonce();
        if (!nonce) {
            *error = tr("no random nonce could be generated");
            return false;
        }
        QFile out(game_dir + QLatin1Char('/') + source.name);
        if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            *error = tr("cannot create %1 in the package").arg(source.name);
            return false;
        }
        Seal::Sealer sealer(*seal_key, *nonce);
        const u64 size = source.file->GetSize();
        u64 offset = 0;
        while (offset < size) {
            const std::size_t length =
                static_cast<std::size_t>(std::min<u64>(kChunk, size - offset));
            const std::size_t got = source.file->Read(buffer.data(), length, offset);
            if (got != length) {
                *error = tr("the game file could not be read at offset %1").arg(offset);
                return false;
            }
            sealer.Apply(buffer.data(), got, offset);
            if (out.write(reinterpret_cast<const char*>(buffer.data()),
                          static_cast<qint64>(got)) != static_cast<qint64>(got)) {
                *error = tr("writing %1 failed: %2").arg(source.name, out.errorString());
                return false;
            }
            offset += got;
            done += got;
            const double fraction = total == 0 ? 1.0 : static_cast<double>(done) / total;
            progress.setValue(static_cast<int>(fraction * 1000));
            ReportStage(ExportStage::Package, 0.1 + 0.75 * fraction,
                        tr("Copying the game file, sealed: %1 of %2")
                            .arg(QLocale().formattedDataSize(static_cast<qint64>(done)),
                                 QLocale().formattedDataSize(static_cast<qint64>(total))));
            // User input is let through here so the Cancel button works. The
            // progress dialog is window-modal, so the export dialog and the
            // main window behind it still receive no clicks or keys.
            QApplication::processEvents();
            if (progress.wasCanceled()) {
                *cancelled = true;
                return false;
            }
        }
        if (!out.flush()) {
            *error = tr("writing %1 failed: %2").arg(source.name, out.errorString());
            return false;
        }
        out.close();
        QJsonObject entry{
            {QStringLiteral("name"), source.name},
            {QStringLiteral("role"), source.role},
            {QStringLiteral("title_id"),
             QStringLiteral("%1").arg(source.title_id, 16, 16, QLatin1Char('0')).toUpper()},
            {QStringLiteral("size"), QString::number(size)},
            {QStringLiteral("nonce"),
             QString::fromStdString(Seal::ToHex(nonce->data(), nonce->size()))},
        };
        if (source.record_type >= 0) {
            entry[QStringLiteral("record_type")] = source.record_type;
        }
        files.append(entry);
    }
    // What the launcher needs to open the sealed files. It holds neither the seal key nor
    // the sd_seed; the check value only tells the right console's keys apart from others.
    QJsonObject seal{
        {QStringLiteral("format"), QString::fromLatin1(Seal::kSealFormat.data(),
                                                       static_cast<qsizetype>(
                                                           Seal::kSealFormat.size()))},
        {QStringLiteral("cipher"), QStringLiteral("AES-128-CTR")},
        {QStringLiteral("export_id"), QString::fromStdString(export_id)},
        {QStringLiteral("title_id"),
         QStringLiteral("%1").arg(program_id, 16, 16, QLatin1Char('0')).toUpper()},
        {QStringLiteral("check"),
         QString::fromStdString(Seal::ToHex(check->data(), check->size()))},
        {QStringLiteral("files"), files},
    };
    QSaveFile seal_file(game_dir + QLatin1Char('/') +
                        QString::fromLatin1(PackagePolicy::kPortableSealName.data(),
                                            static_cast<qsizetype>(
                                                PackagePolicy::kPortableSealName.size())));
    if (!seal_file.open(QIODevice::WriteOnly) ||
        seal_file.write(QJsonDocument(seal).toJson(QJsonDocument::Indented)) <= 0 ||
        !seal_file.commit()) {
        *error = tr("cannot write seal.json");
        return false;
    }
    return true;
}

void GameExportDialog::OnInstallUpdate() {
    if (!export_in_progress) {
        PromptAndInstallUpdate();
    }
}

bool GameExportDialog::PromptAndInstallUpdate() {
    const u64 program_id = SelectedProgramId();
    if (program_id == 0) {
        QMessageBox::warning(this, tr("No Game Selected"),
                             tr("Select the game first, then install its update."));
        return false;
    }
    const QString update_path = QFileDialog::getOpenFileName(
        this, tr("Select Update File"), QString(),
        tr("Switch update (*.nsp);;All files (*.*)"), nullptr,
        QFileDialog::ReadOnly | QFileDialog::DontUseNativeDialog);
    if (update_path.isEmpty()) {
        return false;
    }
    if (!update_path.endsWith(QStringLiteral(".nsp"), Qt::CaseInsensitive)) {
        QMessageBox::warning(this, tr("Not an Update File"),
                             tr("Game updates are .nsp files. Choose the update .nsp for this "
                                "game."));
        return false;
    }

    // Check the file before copying gigabytes into the NAND: it must be an update, and for
    // this game rather than another one.
    const auto vfs = system_.GetFilesystem();
    const auto file = vfs->OpenFile(update_path.toStdString(), FileSys::OpenMode::Read);
    const u64 update_id = FileSys::GetUpdateTitleID(program_id);
    bool readable = false;
    bool is_this_update = false;
    if (file) {
        const FileSys::NSP nsp{file};
        readable = nsp.GetStatus() == Loader::ResultStatus::Success && !nsp.IsExtractedType();
        is_this_update = readable && nsp.GetNCA(update_id, FileSys::ContentRecordType::Program,
                                                FileSys::TitleType::Update) != nullptr;
    }
    if (!readable) {
        QMessageBox::warning(this, tr("Update Not Readable"),
                             tr("This update file could not be read. It may be damaged, or it "
                                "may need keys that are not installed."));
        return false;
    }
    if (!is_this_update) {
        QMessageBox::warning(
            this, tr("Not an Update for This Game"),
            tr("This file is not an update for the selected game (update title ID %1).\n\n"
               "Choose the update .nsp for this game. Base games and DLC are installed with "
               "File > Install Files to NAND.")
                .arg(QStringLiteral("%1").arg(update_id, 16, 16, QLatin1Char('0')).toUpper()));
        return false;
    }

    // No Cancel button: InstallEntry removes a previously installed update before copying,
    // so stopping partway would leave neither version usable.
    QProgressDialog progress(tr("Installing update..."), QString(), 0, 1000, this);
    progress.setCancelButton(nullptr);
    progress.setWindowTitle(tr("Install Update"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    install_update_button->setEnabled(false);
    export_button->setEnabled(false);
    const auto result = ContentManager::InstallNSP(
        system_, *vfs, update_path.toStdString(), [&progress](size_t total, size_t done) {
            progress.setValue(total == 0 ? 0 : static_cast<int>(done * 1000 / total));
            QCoreApplication::processEvents();
            return false;
        });
    progress.close();
    export_button->setEnabled(true);
    // A failed install can leave the cache listing files it no longer has.
    system_.GetFileSystemController().GetUserNANDContents()->Refresh();
    RefreshUpdateStatus();

    switch (result) {
    case ContentManager::InstallResult::Success:
    case ContentManager::InstallResult::Overwrite: {
        // Judge by what the export will actually get, not by the install succeeding.
        const UpdateState after = CurrentUpdateState();
        if (after == UpdateState::Installed || after == UpdateState::Bundled) {
            QMessageBox::information(this, tr("Update Installed"),
                                     tr("The update was installed. The export will use it."));
            return true;
        }
        QMessageBox::information(
            this, tr("Update Installed"),
            after == UpdateState::Unreadable
                ? tr("The update was installed, but suyu still cannot read the update it would "
                     "use for this game. It may need keys that are not installed.")
                : tr("The update was installed, but updates are turned off for this game. Turn "
                     "them on in the game's Properties > Add-Ons to export the updated game."));
        return false;
    }
    case ContentManager::InstallResult::BaseInstallAttempted:
        QMessageBox::warning(this, tr("Not an Update"),
                             tr("This file is a base game, not an update."));
        return false;
    case ContentManager::InstallResult::Failure:
    default:
        QMessageBox::warning(this, tr("Update Not Installed"),
                             tr("The update could not be installed. The file may be "
                                "damaged or need keys that are not installed."));
        return false;
    }
}

void GameExportDialog::OnSelectFromLibrary() {
    if (library_entries_.isEmpty()) {
        QMessageBox::information(this, tr("Library"),
                                 tr("No launchable local games were found in your library."));
        return;
    }

    QDialog chooser(this);
    chooser.setWindowTitle(tr("Select Game from Library"));
    chooser.resize(680, 420);

    auto* layout = new QVBoxLayout(&chooser);
    auto* list = new QListWidget(&chooser);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setAlternatingRowColors(true);

    for (const auto& entry : library_entries_) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n%2").arg(entry.title, entry.path), list);
        item->setData(Qt::UserRole, entry.path);
        item->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(entry.program_id));
        item->setToolTip(entry.path);
    }

    if (list->count() > 0) {
        list->setCurrentRow(0);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &chooser);
    connect(buttons, &QDialogButtonBox::accepted, &chooser, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &chooser, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &chooser, [&chooser](QListWidgetItem*) {
        chooser.accept();
    });

    layout->addWidget(list);
    layout->addWidget(buttons);

    if (chooser.exec() != QDialog::Accepted || !list->currentItem()) {
        return;
    }

    const QString selected_path = list->currentItem()->data(Qt::UserRole).toString();
    const quint64 selected_program_id =
        static_cast<quint64>(list->currentItem()->data(Qt::UserRole + 1).toULongLong());
    SetRomPath(selected_path, selected_program_id);

    for (const auto& entry : library_entries_) {
        if (entry.path == selected_path && !entry.icon.isNull()) {
            SetGameIcon(entry.icon);
            break;
        }
    }
}

void GameExportDialog::OnBrowseOutput() {
    const QString start_dir = output_path_edit->text().isEmpty() ? QString() : output_path_edit->text();
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Output Directory"), start_dir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
    if (!dir.isEmpty()) {
        output_path_edit->setText(dir);
    }
}

// ---------------------------------------------------------------------------
// ARM64 basic block analysis
// ---------------------------------------------------------------------------

namespace {

/// Represents a discovered ARM64 basic block in the NSO .text segment.
struct Arm64BasicBlock {
    u32 vaddr;       ///< Virtual address offset within the segment
    u32 size;        ///< Block size in bytes
    u32 instruction_count;
    bool is_entry;   ///< Whether this is the segment entry point
};

/// Classify ARM64 instructions to detect basic block boundaries.
/// Returns true if the instruction is a block-terminating branch/system call.
static bool IsBlockTerminator(u32 insn) {
    // B  (unconditional branch immediate)
    if ((insn & 0xFC000000) == 0x14000000) return true;
    // BL (branch with link — call, but still ends the block)
    if ((insn & 0xFC000000) == 0x94000000) return true;
    // BR (branch register — indirect jump)
    if ((insn & 0xFFFFFC1F) == 0xD61F0000) return true;
    // BLR (branch with link register)
    if ((insn & 0xFFFFFC1F) == 0xD63F0000) return true;
    // RET
    if ((insn & 0xFFFFFC1F) == 0xD65F0000) return true;
    // CBZ
    if ((insn & 0x7F000000) == 0x34000000) return true;
    // CBNZ
    if ((insn & 0x7F000000) == 0x35000000) return true;
    // TBZ
    if ((insn & 0x7F000000) == 0x36000000) return true;
    // TBNZ
    if ((insn & 0x7F000000) == 0x37000000) return true;
    // B.cond (conditional branch)
    if ((insn & 0xFF000010) == 0x54000000) return true;
    // SVC (supervisor call — system call boundary)
    if ((insn & 0xFFE0001F) == 0xD4000001) return true;
    return false;
}

/// Returns true if the instruction is a direct branch (B or BL) and extracts the target offset.
static bool GetDirectBranchTarget(u32 insn, u32 pc, u32& target_out) {
    // B: imm26 is a signed offset in instructions
    if ((insn & 0xFC000000) == 0x14000000) {
        s32 imm26 = static_cast<s32>(insn << 6) >> 6; // sign-extend 26 bits
        target_out = pc + static_cast<u32>(imm26 * 4);
        return true;
    }
    // BL: same encoding
    if ((insn & 0xFC000000) == 0x94000000) {
        s32 imm26 = static_cast<s32>(insn << 6) >> 6;
        target_out = pc + static_cast<u32>(imm26 * 4);
        return true;
    }
    return false;
}

/// Perform a linear sweep over ARM64 .text to identify basic blocks.
/// This discovers block boundaries by looking for branch instructions and branch targets.
static std::vector<Arm64BasicBlock> AnalyzeArm64BasicBlocks(std::span<const u8> text_data,
                                                             u32 base_vaddr, bool full_scan) {
    if (text_data.size() < 4) {
        return {};
    }

    const u32 num_instructions = static_cast<u32>(text_data.size() / 4);
    const u32* insn_ptr = reinterpret_cast<const u32*>(text_data.data());

    // First pass: identify all branch targets so we know where blocks start
    std::vector<bool> is_block_start(num_instructions, false);
    is_block_start[0] = true; // Entry point of the segment

    for (u32 i = 0; i < num_instructions; ++i) {
        u32 insn = insn_ptr[i];
        u32 pc = base_vaddr + i * 4;

        if (IsBlockTerminator(insn)) {
            // The instruction after a terminator starts a new block
            if (i + 1 < num_instructions) {
                is_block_start[i + 1] = true;
            }

            // If it's a direct branch, the target also starts a block
            u32 target = 0;
            if (GetDirectBranchTarget(insn, pc, target)) {
                u32 target_index = (target - base_vaddr) / 4;
                if (target_index < num_instructions) {
                    is_block_start[target_index] = true;
                }
            }
        }
    }

    // Second pass: build block list from boundaries
    std::vector<Arm64BasicBlock> blocks;
    blocks.reserve(num_instructions / 8); // Heuristic: average 8 instructions per block

    u32 block_start_idx = 0;
    for (u32 i = 1; i <= num_instructions; ++i) {
        if (i == num_instructions || is_block_start[i]) {
            Arm64BasicBlock block{};
            block.vaddr = base_vaddr + block_start_idx * 4;
            block.size = (i - block_start_idx) * 4;
            block.instruction_count = i - block_start_idx;
            block.is_entry = (block_start_idx == 0);
            blocks.push_back(block);
            block_start_idx = i;
        }
    }

    return blocks;
}

/// Compute a hex string from a build ID array.
static QString BuildIdToHex(const std::array<u8, 0x20>& build_id) {
    QString hex;
    hex.reserve(0x40);
    for (u8 byte : build_id) {
        hex.append(QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0')));
    }
    return hex;
}

/// Information extracted from a single NSO module.
struct NsoAnalysisResult {
    QString name;
    QString build_id_hex;
    u32 text_vaddr{};
    u32 text_size{};
    u32 rodata_vaddr{};
    u32 rodata_size{};
    u32 data_vaddr{};
    u32 data_size{};
    u32 total_blocks{};
    u32 total_instructions{};
    /// Guest address of the first real instruction. An NSO's .text does not
    /// start with code, so this is not simply text_vaddr.
    u64 entry_vaddr{};
    std::vector<u8> text_bytes;
    std::vector<u8> rodata_bytes;
    std::vector<u8> data_bytes;
    std::vector<Arm64BasicBlock> blocks;
};

/// Offset of the first real instruction within a decompressed .text.
///
/// An NSO's .text opens with a branch word, then the MOD0 header the offset at
/// +4 points at, then zero padding - none of which is code. Starting a
/// recompiled image at .text+0 therefore begins mid-header, and because
/// nothing branches to the true entry it never becomes a block start either.
static u32 FindNsoEntryOffset(std::span<const u8> text) {
    if (text.size() < 0x10) {
        return 0;
    }
    u32 mod0_off = 0;
    std::memcpy(&mod0_off, text.data() + 4, sizeof(mod0_off));

    // MOD0 itself is 0x1C bytes; walk past it and then over the padding to the
    // first non-zero word.
    size_t off = (static_cast<size_t>(mod0_off) + 0x1C + 3) & ~size_t{3};
    if (off >= text.size()) {
        return 0;
    }
    while (off + 4 <= text.size()) {
        u32 word = 0;
        std::memcpy(&word, text.data() + off, sizeof(word));
        if (word != 0) {
            return static_cast<u32>(off);
        }
        off += 4;
    }
    return 0;
}

/// Reads bytes at a module-relative virtual address out of whichever of the
/// three decompressed segments actually contains it. .dynamic/.dynsym/.dynstr
/// can live in text or rodata depending on toolchain, so callers walking them
/// need one accessor spanning all three rather than assuming a segment.
static bool ReadModuleBytes(const NsoAnalysisResult& mod, u64 vaddr, u8* out, size_t len) {
    auto try_seg = [&](u64 seg_vaddr, const std::vector<u8>& bytes) {
        if (vaddr < seg_vaddr) return false;
        const u64 off = vaddr - seg_vaddr;
        if (off + len > bytes.size()) return false;
        std::memcpy(out, bytes.data() + off, len);
        return true;
    };
    return try_seg(mod.text_vaddr, mod.text_bytes) ||
           try_seg(mod.rodata_vaddr, mod.rodata_bytes) ||
           try_seg(mod.data_vaddr, mod.data_bytes);
}

static u32 ReadModuleU32(const NsoAnalysisResult& mod, u64 vaddr) {
    u32 v = 0;
    ReadModuleBytes(mod, vaddr, reinterpret_cast<u8*>(&v), sizeof(v));
    return v;
}

static u64 ReadModuleU64(const NsoAnalysisResult& mod, u64 vaddr) {
    u64 v = 0;
    ReadModuleBytes(mod, vaddr, reinterpret_cast<u8*>(&v), sizeof(v));
    return v;
}

/// Collects every defined dynsym symbol's address from a module's .dynamic
/// section, mirroring ArmRecomp::Impl::ParseDynamic/IndexExports at runtime
/// (src/core/arm/recomp/arm_recomp.cpp) but reading from the decompressed
/// export-time buffers rather than live guest memory. These become extra
/// block-discovery roots: a function reached only via another module's
/// resolved GOT/PLT entry (e.g. nn::init::Start) never gets a direct branch
/// inside its own module, so without this its real entry address can fall
/// mid-block - behind alignment padding after the previous function's return
/// - and the runtime dispatcher can never resolve a call landing exactly on it.
/// Conservatively scans .rodata and .data for 8-byte-aligned values that look
/// like a pointer into this module's own .text - vtables, HIPC/service
/// dispatch tables, and other function-pointer tables the SDK builds at
/// static-init time, all of which reach their target only through an
/// indirect call (BLR) with no direct branch anywhere in .text pointing at
/// it. Exported dynsym symbols (CollectExportedSymbolAddresses) don't cover
/// these - a table entry like this is never exported, it's purely an
/// implementation detail the module reads out of its own data section - so
/// without also seeding roots from here, any function reached exclusively
/// through such a table falls mid-block (typically right behind whichever
/// neighboring function's RET happened to end the previous block) and the
/// runtime dispatcher can never resolve a BLR landing exactly on its real
/// entry address ("No recompiled block at PC").
///
/// This is deliberately over-inclusive: ordinary data that happens to look
/// like a text address becomes a spurious extra block boundary, which only
/// costs a slightly smaller block - never a wrong one - so there's no need to
/// separate real function-pointer tables from incidental matches.
static std::vector<u64> ScanDataForCodePointers(const NsoAnalysisResult& mod) {
    std::vector<u64> out;
    const u64 text_lo = mod.text_vaddr;
    const u64 text_hi = mod.text_vaddr + mod.text_bytes.size();
    const auto scan = [&](const std::vector<u8>& bytes) {
        if (bytes.size() < 8) return;
        for (size_t off = 0; off + 8 <= bytes.size(); off += 8) {
            u64 v = 0;
            std::memcpy(&v, bytes.data() + off, sizeof(v));
            if (v >= text_lo && v < text_hi && (v & 3) == 0) {
                out.push_back(v);
            }
        }
    };
    scan(mod.rodata_bytes);
    scan(mod.data_bytes);
    return out;
}

/// Function-pointer roots recovered from the module's relocation tables.
///
/// ScanDataForCodePointers reads .rodata/.data as they sit in the file, which
/// is *before* relocation: a vtable slot or static function-pointer table entry
/// is written by the linker as a relocation, and the slot itself usually holds
/// nothing (or a bare addend). The real target only exists in the RELA entry -
/// R_AARCH64_RELATIVE carries it in the addend, ABS64 in symbol value + addend.
/// So the addresses reached exclusively through those tables - virtual methods,
/// service dispatch entries, and every function handed to the OS as a callback,
/// thread entry points above all - are invisible to a raw data scan.
///
/// Missing them is what leaves a brand-new guest thread starting at an address
/// no block covers, so every thread the game spawns drops straight to the
/// interpreter. Seeding them here keeps that work in statically recompiled
/// code. Over-inclusive on purpose: a value that merely looks like a text
/// address only ever costs one extra block boundary, never a wrong block.
static std::vector<u64> ScanRelocationsForCodePointers(const NsoAnalysisResult& mod) {
    std::vector<u64> out;
    if (mod.text_bytes.size() < 8) return out;

    u32 mod0_off = 0;
    std::memcpy(&mod0_off, mod.text_bytes.data() + 4, sizeof(mod0_off));
    const u64 mod0_va = mod.text_vaddr + mod0_off;
    if (ReadModuleU32(mod, mod0_va) != 0x30444F4Du) return out;

    const u32 dyn_rel_off = ReadModuleU32(mod, mod0_va + 4);
    const u64 dyn_va = mod0_va + dyn_rel_off;

    constexpr u32 DT_NULL = 0, DT_PLTRELSZ = 2, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
                   DT_JMPREL = 23;
    u64 rela_va = 0, rela_sz = 0, jmprel_va = 0, jmprel_sz = 0, symtab_va = 0;
    for (u64 p = dyn_va, guard = 0; guard < 0x1000; p += 16, guard += 16) {
        const u64 tag = ReadModuleU64(mod, p);
        const u64 val = ReadModuleU64(mod, p + 8);
        if (tag == DT_NULL) break;
        if (tag == DT_RELA) rela_va = val;
        if (tag == DT_RELASZ) rela_sz = val;
        if (tag == DT_JMPREL) jmprel_va = val;
        if (tag == DT_PLTRELSZ) jmprel_sz = val;
        if (tag == DT_SYMTAB) symtab_va = val;
    }

    const u64 text_lo = mod.text_vaddr;
    const u64 text_hi = mod.text_vaddr + mod.text_bytes.size();
    constexpr u32 R_AARCH64_ABS64 = 0x101, R_AARCH64_GLOB_DAT = 0x401,
                   R_AARCH64_JUMP_SLOT = 0x402, R_AARCH64_RELATIVE = 0x403;
    const auto scan_table = [&](u64 table_va, u64 table_sz) {
        for (u64 p = table_va; p + 24 <= table_va + table_sz; p += 24) {
            const u64 r_info = ReadModuleU64(mod, p + 8);
            const u64 r_addend = ReadModuleU64(mod, p + 16);
            const u32 r_type = static_cast<u32>(r_info & 0xFFFFFFFF);
            const u32 r_sym = static_cast<u32>(r_info >> 32);
            u64 target = 0;
            if (r_type == R_AARCH64_RELATIVE) {
                target = r_addend;
            } else if ((r_type == R_AARCH64_ABS64 || r_type == R_AARCH64_GLOB_DAT ||
                        r_type == R_AARCH64_JUMP_SLOT) &&
                       symtab_va != 0 && r_sym != 0) {
                target = ReadModuleU64(mod, symtab_va + static_cast<u64>(r_sym) * 24 + 8) +
                         (r_type == R_AARCH64_ABS64 ? r_addend : 0);
            } else {
                continue;
            }
            if (target >= text_lo && target < text_hi && (target & 3) == 0) {
                out.push_back(target);
            }
        }
    };
    if (rela_va && rela_sz) scan_table(rela_va, rela_sz);
    if (jmprel_va && jmprel_sz) scan_table(jmprel_va, jmprel_sz);
    return out;
}

static std::vector<u64> CollectExportedSymbolAddresses(const NsoAnalysisResult& mod) {
    std::vector<u64> out;
    if (mod.text_bytes.size() < 8) return out;

    // MOD0 offset is stored at text+4, relative to the start of .text (same
    // field FindNsoEntryOffset walks past to find the real entry point).
    u32 mod0_off = 0;
    std::memcpy(&mod0_off, mod.text_bytes.data() + 4, sizeof(mod0_off));
    const u64 mod0_va = mod.text_vaddr + mod0_off;
    if (ReadModuleU32(mod, mod0_va) != 0x30444F4Du) return out; // "MOD0"

    const u32 dyn_rel_off = ReadModuleU32(mod, mod0_va + 4);
    const u64 dyn_va = mod0_va + dyn_rel_off;

    constexpr u32 DT_NULL = 0, DT_STRTAB = 5, DT_SYMTAB = 6;
    u64 symtab_va = 0, strtab_va = 0;
    for (u64 p = dyn_va, guard = 0; guard < 0x1000; p += 16, guard += 16) {
        const u64 tag = ReadModuleU64(mod, p);
        const u64 val = ReadModuleU64(mod, p + 8);
        if (tag == DT_NULL) break;
        if (tag == DT_SYMTAB) symtab_va = val;
        if (tag == DT_STRTAB) strtab_va = val;
    }
    if (!symtab_va || !strtab_va || strtab_va <= symtab_va) return out;

    // .dynsym/.dynstr are laid out back to back, so the gap between them
    // bounds the entry count (no explicit count exists for a plain DT_SYMTAB).
    const u64 span = strtab_va - symtab_va;
    const u32 max_index = static_cast<u32>(std::min<u64>(span / 24, 65536));
    for (u32 i = 1; i < max_index; ++i) { // index 0 is always the null symbol
        const u64 sym_va = symtab_va + static_cast<u64>(i) * 24;
        const u16 shndx = static_cast<u16>(ReadModuleU32(mod, sym_va + 6) & 0xFFFF);
        if (shndx == 0) continue; // SHN_UNDEF - an import, not an export
        const u64 value = ReadModuleU64(mod, sym_va + 8);
        if (value) out.push_back(value);
    }
    return out;
}

/// Parse and analyze a single NSO file using the VFS.
static std::optional<NsoAnalysisResult> AnalyzeNsoFile(const FileSys::VirtualFile& nso_file,
                                                        bool full_scan,
                                                        const FileSys::PatchManager& patch_manager,
                                                        bool& patch_failed) {
    patch_failed = false;
    if (!nso_file || nso_file->GetSize() < sizeof(Loader::NSOHeader)) {
        return std::nullopt;
    }

    Loader::NSOHeader header{};
    if (nso_file->ReadObject(&header) != sizeof(Loader::NSOHeader)) {
        return std::nullopt;
    }

    if (header.magic != Common::MakeMagic('N', 'S', 'O', '0')) {
        return std::nullopt;
    }

    NsoAnalysisResult result{};
    result.name = QString::fromStdString(nso_file->GetName());
    result.build_id_hex = BuildIdToHex(header.build_id);

    // Extract segment metadata
    result.text_vaddr = header.segments[0].location;
    result.text_size = header.segments[0].size;
    result.rodata_vaddr = header.segments[1].location;
    result.rodata_size = header.segments[1].size;
    result.data_vaddr = header.segments[2].location;
    result.data_size = header.segments[2].size;

    // Read and decompress .text segment (segment 0)
    std::vector<u8> text_data = nso_file->ReadBytes(
        header.segments_compressed_size[0], header.segments[0].offset);

    if (text_data.empty()) {
        return std::nullopt;
    }

    if (header.IsSegmentCompressed(0)) {
        text_data = Common::Compression::DecompressDataLZ4(text_data, header.segments[0].size);
        if (text_data.empty()) {
            return std::nullopt;
        }
    }

    result.text_bytes = text_data;
    result.entry_vaddr = static_cast<u64>(result.text_vaddr) + FindNsoEntryOffset(result.text_bytes);

    // Read and decompress .rodata segment (segment 1)
    {
        std::vector<u8> seg = nso_file->ReadBytes(
            header.segments_compressed_size[1], header.segments[1].offset);
        if (!seg.empty() && header.IsSegmentCompressed(1)) {
            seg = Common::Compression::DecompressDataLZ4(seg, header.segments[1].size);
        }
        result.rodata_bytes = std::move(seg);
    }

    // Read and decompress .data segment (segment 2)
    {
        std::vector<u8> seg = nso_file->ReadBytes(
            header.segments_compressed_size[2], header.segments[2].offset);
        if (!seg.empty() && header.IsSegmentCompressed(2)) {
            seg = Common::Compression::DecompressDataLZ4(seg, header.segments[2].size);
        }
        result.data_bytes = std::move(seg);
    }

    if (patch_manager.HasNSOPatch(header.build_id, nso_file->GetName())) {
        std::array<std::vector<u8>, 3> segments{
            std::move(result.text_bytes), std::move(result.rodata_bytes), std::move(result.data_bytes)};
        const bool patched = SuyuExport::BakeNsoPatches(
            {reinterpret_cast<const u8*>(&header), sizeof(header)},
            {header.segments[0].location, header.segments[1].location, header.segments[2].location},
            segments, header.segments[2].bss_size,
            [&](const std::vector<u8>& image) {
                const bool dump_nso = Settings::values.dump_nso.GetValue();
                Settings::values.dump_nso.SetValue(false);
                SCOPE_EXIT { Settings::values.dump_nso.SetValue(dump_nso); };
                return patch_manager.PatchNSO(image, nso_file->GetName());
            });
        if (!patched) {
            patch_failed = true;
            LOG_ERROR(Frontend, "Cannot bake NSO patches for {}: invalid patched image", nso_file->GetName());
            return std::nullopt;
        }
        result.text_bytes = std::move(segments[0]);
        result.rodata_bytes = std::move(segments[1]);
        result.data_bytes = std::move(segments[2]);
    }
    result.entry_vaddr = static_cast<u64>(result.text_vaddr) + FindNsoEntryOffset(result.text_bytes);

    // Analyze ARM64 basic blocks in the patched .text segment
    result.blocks = AnalyzeArm64BasicBlocks(
        std::span<const u8>{result.text_bytes.data(), result.text_bytes.size()},
        header.segments[0].location, full_scan);

    result.total_blocks = static_cast<u32>(result.blocks.size());
    result.total_instructions = 0;
    for (const auto& block : result.blocks) {
        result.total_instructions += block.instruction_count;
    }

    return result;
}

// Record only the files selected by the runtime patch manager for these build IDs.
// Patch files outside LayeredExeFS still contribute explicitly to the cache key.
static std::optional<QJsonObject> BakedPatchManifest(
    const std::vector<FileSys::VirtualFile>& modules, const FileSys::PatchManager& manager) {
    std::map<QString, std::map<QString, QJsonObject>> selected;
    std::map<QString, FileSys::VirtualDir> selected_directories;
    for (const auto& module : modules) {
        Loader::NSOHeader header{};
        if (module->ReadObject(&header) != sizeof(header) ||
            header.magic != Common::MakeMagic('N', 'S', 'O', '0')) continue;
        for (const auto& patch : manager.GetNSOPatches(header.build_id)) {
            const auto exefs = patch->GetContainingDirectory();
            const auto mod = exefs->GetParentDirectory();
            selected_directories[QString::fromStdString(mod->GetName())] = exefs;
            // A patch-only export must not advertise a folder whose other payloads
            // replace modules or install runtime hooks when the launcher enables it.
            for (const auto& file : exefs->GetFiles()) {
                if (file->GetExtension() != "ips" && file->GetExtension() != "pchtxt") {
                    LOG_ERROR(Frontend, "Cannot bake mod {}: patch-only exports do not support {}",
                              mod->GetName(), file->GetName());
                    return std::nullopt;
                }
            }
            if (patch->GetSize() > (16u << 20)) {
                LOG_ERROR(Frontend, "Cannot bake mod {}: patch {} exceeds the 16 MiB limit",
                          mod->GetName(), patch->GetName());
                return std::nullopt;
            }
            const auto bytes = patch->ReadAllBytes();
            if (bytes.size() != patch->GetSize()) return std::nullopt;
            const auto digest = QCryptographicHash::hash(
                QByteArrayView(reinterpret_cast<const char*>(bytes.data()),
                               static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256).toHex();
            const QString path = QStringLiteral("exefs/") + QString::fromStdString(patch->GetName());
            selected[QString::fromStdString(mod->GetName())][path] = QJsonObject{
                {QStringLiteral("path"), path}, {QStringLiteral("sha256"), QString::fromLatin1(digest)},
                {QStringLiteral("size"), static_cast<qint64>(bytes.size())},
                {QStringLiteral("target_build_id"), BuildIdToHex(header.build_id).toUpper()}};
        }
    }
    QJsonArray mods;
    for (const auto& [name, files] : selected) {
        const auto& directory = selected_directories.at(name);
        if (!directory->GetSubdirectories().empty()) {
            LOG_ERROR(Frontend, "Cannot bake mod {}: use a dedicated patch-only ExeFS folder", name.toStdString());
            return std::nullopt;
        }
        for (const auto& file : directory->GetFiles()) {
            // QString, not auto: auto would keep a QStringBuilder that refers to the
            // temporary from fromStdString after it is destroyed.
            const QString path = QStringLiteral("exefs/") + QString::fromStdString(file->GetName());
            if (!files.contains(path)) {
                LOG_ERROR(Frontend, "Cannot bake mod {}: {} does not target this export. Move the selected patches into a dedicated mod folder",
                          name.toStdString(), file->GetName());
                return std::nullopt;
            }
        }
        QJsonArray entries;
        for (const auto& [path, entry] : files) entries.append(entry);
        mods.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("files"), entries}});
    }
    const auto fingerprint = mods.isEmpty() ? QByteArray{} : QCryptographicHash::hash(
        QJsonDocument(mods).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    return QJsonObject{{QStringLiteral("schema"), 1}, {QStringLiteral("mods"), mods},
                       {QStringLiteral("fingerprint"), QString::fromLatin1(fingerprint)}};
}

/// Attempt to get the ExeFS VirtualDir from a ROM file using the VFS infrastructure.
static FileSys::VirtualDir ExtractExeFsFromRom(const std::string& rom_path) {
    // RealVfsFile holds a raw RealVfsFilesystem& (not a shared_ptr), so a
    // locally-scoped vfs would dangle once files it opened outlive this
    // function - keep one filesystem instance alive for the process.
    static const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    auto file = vfs->OpenFile(rom_path, FileSys::OpenMode::Read);
    if (!file) {
        return nullptr;
    }

    const std::string name = file->GetName();
    const std::string ext = [&name]() {
        auto pos = name.rfind('.');
        if (pos == std::string::npos) return std::string{};
        std::string e = name.substr(pos);
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return e;
    }();

    // Extract the Program NCA's ExeFS from an NSP. NSP::GetExeFS() only
    // returns a populated result for pre-extracted directory-style NSPs;
    // for a real packed/encrypted NSP (the normal case) its exefs/romfs
    // members are never set by the constructor, so it always returns null
    // there regardless of whether the NSP parsed successfully. The actual
    // content lives on the Program-type NCA, keyed by the NSP's own program
    // title ID.
    // The loader never runs an ExeFS it has not put through PatchManager: an
    // installed update REPLACES the ExeFS outright rather than patching it
    // (patch_manager.cpp:301, `exefs = update->GetExeFS()`). Reading only the
    // container's own ExeFS therefore recompiles code the emulator will never
    // execute whenever the update lives in installed content rather than on
    // the cartridge. That is not hypothetical: a Mario Kart 8 Deluxe export
    // taken from the XCI came back as the cart's 32-bit (NX32) base build
    // while the emulator was running the 64-bit update, so the AArch64
    // recompiler "translated" A32 words - 86% of them unhandled, 25k blocks
    // instead of 1.08M - and the resulting images killed the emulator the
    // moment rtld's static image was entered.
    const auto apply_installed_update =
        [&system](u64 title_id, FileSys::VirtualDir exefs) -> FileSys::VirtualDir {
        if (!exefs) {
            return exefs;
        }
        const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                       system.GetContentProvider()};
        if (auto patched = pm.PatchExeFS(exefs)) {
            return patched;
        }
        return exefs;
    };

    const auto exefs_from_nsp = [&apply_installed_update](
                                   const std::shared_ptr<FileSys::NSP>& nsp) -> FileSys::VirtualDir {
        if (nsp->GetStatus() != Loader::ResultStatus::Success) {
            return nullptr;
        }
        const auto base_tid = nsp->GetProgramTitleID();
        if (auto exefs = nsp->GetExeFS()) {
            // Pre-extracted NSP - already populated.
            return apply_installed_update(base_tid, exefs);
        }
        // An update replaces the ExeFS outright rather than patching it -
        // PatchManager::PatchExeFS does `exefs = update->GetExeFS()`
        // (patch_manager.cpp:301). So whenever an update is present, the base
        // ExeFS is code the game will never execute, and recompiling it
        // produces an image whose every module has the wrong build ID.
        //
        // Cartridge dumps routinely carry an update in the same secure
        // partition, which is why this is not an edge case: a second title
        // ships one, and recompiling the base gave eight modules that diverged
        // into unmapped memory 153 blocks into boot.
        const auto base_tid = nsp->GetProgramTitleID();
        const auto update_tid = FileSys::GetUpdateTitleID(base_tid);
        // TitleType::Update, not the Application default: NSP::GetNCA keys on
        // {title_type, content_type}, so asking for an update under the
        // Application type silently finds nothing.
        if (const auto update_nca = nsp->GetNCA(update_tid, FileSys::ContentRecordType::Program,
                                                FileSys::TitleType::Update)) {
            if (auto update_exefs = update_nca->GetExeFS()) {
                LOG_INFO(Frontend,
                         "AOT: using update ExeFS ({:016X}) rather than the base ({:016X})",
                         update_tid, base_tid);
                return update_exefs;
            }
        }

        const auto t_nca_start = std::chrono::steady_clock::now();
        const auto program_nca =
            nsp->GetNCA(base_tid, FileSys::ContentRecordType::Program);
        const auto t_nca_got = std::chrono::steady_clock::now();
        const auto exefs = program_nca ? program_nca->GetExeFS() : nullptr;
        const auto t_exefs_got = std::chrono::steady_clock::now();
        LOG_INFO(Frontend, "AOT diag: GetNCA took {} ms, NCA::GetExeFS took {} ms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(t_nca_got - t_nca_start).count(),
                 std::chrono::duration_cast<std::chrono::milliseconds>(t_exefs_got - t_nca_got).count());
        return apply_installed_update(base_tid, exefs);
    };

    // Try NSP
    if (ext == ".nsp") {
        const auto t_ctor_start = std::chrono::steady_clock::now();
        auto nsp = std::make_shared<FileSys::NSP>(file);
        const auto t_ctor_end = std::chrono::steady_clock::now();
        LOG_INFO(Frontend, "AOT diag: NSP ctor took {} ms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(t_ctor_end - t_ctor_start).count());
        if (auto exefs = exefs_from_nsp(nsp)) {
            return exefs;
        }
    }

    // Try XCI
    if (ext == ".xci") {
        auto xci = std::make_shared<FileSys::XCI>(file);
        if (xci->GetStatus() == Loader::ResultStatus::Success) {
            auto secure_nsp = xci->GetSecurePartitionNSP();
            if (secure_nsp) {
                if (auto exefs = exefs_from_nsp(secure_nsp)) {
                    return exefs;
                }
            }
        }
    }

    // Try NCA
    if (ext == ".nca") {
        auto nca = std::make_shared<FileSys::NCA>(file);
        if (nca->GetStatus() == Loader::ResultStatus::Success) {
            auto exefs = nca->GetExeFS();
            if (exefs) return exefs;
        }
    }

    // Standalone NSO — wrap in a synthetic directory
    if (ext == ".nso" || ext == "") {
        // Check if it's an NSO by magic
        u32 magic = 0;
        if (file->ReadObject(&magic) == sizeof(magic) &&
            magic == Common::MakeMagic('N', 'S', 'O', '0')) {
            // Return nullptr — caller will handle single NSO files
        }
    }

    return nullptr;
}

// Save every file in a VirtualDir to disk recursively.
// Returns number of bytes written, -1 on failure.
static qint64 DumpVirtualDir(const FileSys::VirtualDir& vdir, const QString& dest_dir) {
    if (!vdir) return 0;
    QDir().mkpath(dest_dir);
    qint64 total = 0;
    for (const auto& f : vdir->GetFiles()) {
        const auto data = f->ReadAllBytes();
        QFile out(dest_dir + QLatin1Char('/') + QString::fromStdString(f->GetName()));
        if (!out.open(QIODevice::WriteOnly)) return -1;
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<qint64>(data.size()));
        total += static_cast<qint64>(data.size());
    }
    for (const auto& sub : vdir->GetSubdirectories()) {
        const qint64 r = DumpVirtualDir(
            sub, dest_dir + QLatin1Char('/') + QString::fromStdString(sub->GetName()));
        if (r < 0) return -1;
        total += r;
    }
    return total;
}

// Extract the romfs VirtualFile from a ROM (NSP/XCI/NCA). Returns nullptr if not available.
static FileSys::VirtualFile ExtractRomFsFromRom(const std::string& rom_path) {
    // RealVfsFile holds a raw RealVfsFilesystem& (not a shared_ptr), so a
    // locally-scoped vfs would dangle once files it opened outlive this
    // function - keep one filesystem instance alive for the process.
    static const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    auto file = vfs->OpenFile(rom_path, FileSys::OpenMode::Read);
    if (!file) return nullptr;
    const std::string name = file->GetName();
    auto pos = name.rfind('.'); std::string ext;
    if (pos != std::string::npos) { ext = name.substr(pos); std::transform(ext.begin(),ext.end(),ext.begin(),::tolower); }

    const auto romfs_from_nsp = [](const std::shared_ptr<FileSys::NSP>& nsp) -> FileSys::VirtualFile {
        if (nsp->GetStatus() != Loader::ResultStatus::Success) return nullptr;
        const auto nca = nsp->GetNCA(nsp->GetProgramTitleID(), FileSys::ContentRecordType::Program);
        return nca ? nca->GetRomFS() : nullptr;
    };

    if (ext == ".nsp") {
        auto nsp = std::make_shared<FileSys::NSP>(file);
        if (auto r = romfs_from_nsp(nsp)) return r;
    }
    if (ext == ".xci") {
        auto xci = std::make_shared<FileSys::XCI>(file);
        if (xci->GetStatus() == Loader::ResultStatus::Success) {
            auto sec = xci->GetSecurePartitionNSP();
            if (sec) if (auto r = romfs_from_nsp(sec)) return r;
        }
    }
    if (ext == ".nca") {
        auto nca = std::make_shared<FileSys::NCA>(file);
        if (nca->GetStatus() == Loader::ResultStatus::Success)
            return nca->GetRomFS();
    }
    return nullptr;
}

static std::optional<u32> ReadArm64InstructionAt(std::span<const u8> text, u32 text_vaddr,
                                                 u64 vaddr) {
    if (vaddr < text_vaddr) {
        return std::nullopt;
    }

    const size_t offset = static_cast<size_t>(vaddr - text_vaddr);
    if (offset + sizeof(u32) > text.size()) {
        return std::nullopt;
    }

    u32 instruction = 0;
    std::memcpy(&instruction, text.data() + offset, sizeof(instruction));
    return instruction;
}
#endif

// Writes a Dynarmic IR dump per block, for eyeballing what the JIT would have
// made of code the emitter is being asked about. Debug material only - nothing
// in the export pipeline reads it - and the only reason this file needs
// dynarmic at all, so it goes when dynarmic does.
#ifdef SUYU_NO_JIT
static bool SerializeTranslatedBlocks(const NsoAnalysisResult&, const QString&, const QString&,
                                      u32*, u32*) {
    return false;
}
#else
static bool SerializeTranslatedBlocks(const NsoAnalysisResult& mod, const QString& ir_root,
                                      const QString& code_root, u32* serialized_blocks,
                                      u32* failed_blocks) {
    const QString module_ir_dir = ir_root + QDir::separator() + mod.name;
    const QString module_code_dir = code_root + QDir::separator() + mod.name;
    QDir().mkpath(module_ir_dir);
    QDir().mkpath(module_code_dir);

    const std::span<const u8> text_span{mod.text_bytes.data(), mod.text_bytes.size()};

    // This phase dumps two files per basic block - the raw bytes and a
    // Dynarmic IR listing - purely as debugging material. It is off by default
    // because the counts involved make it unusable otherwise: Smash Ultimate
    // has about 2.68 million blocks, so it wants roughly 5.4 million files, and
    // the filesystem becomes the entire cost of an export that otherwise takes
    // seconds. Measured before this was gated: 187,000 files written in a few
    // minutes with no end in sight, which is what the long-standing report of
    // the exporter "hanging past 15%" actually was. Nothing in the recompiler
    // path reads these - EmitProject works from mod.text_bytes directly.
    //
    // An earlier comment here put the block count at "90,000+" and explained
    // the symptom as the window merely failing to repaint. Both were wrong.
    const bool dump_blocks =
        !qEnvironmentVariableIsEmpty("SUYU_AOT_DUMP_BLOCKS");
    if (!dump_blocks) {
        // Leave the count at zero rather than reporting the block total: the
        // manifest publishes this as "ir_blocks_serialized", and nothing was
        // serialized. The number that matters for the recompiler is
        // recompiled_c_blocks, which is counted separately.
        return true;
    }

    size_t blocks_processed = 0;
    for (const auto& block : mod.blocks) {
        if (++blocks_processed % 250 == 0) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
        const QString stem = QStringLiteral("%1_%2")
                                 .arg(mod.name)
                                 .arg(block.vaddr, 8, 16, QLatin1Char('0'));

        const size_t offset = static_cast<size_t>(block.vaddr - mod.text_vaddr);
        if (offset + block.size > mod.text_bytes.size()) {
            ++(*failed_blocks);
            continue;
        }

        QFile guest_code_file(module_code_dir + QDir::separator() + stem + QStringLiteral(".guest.bin"));
        if (guest_code_file.open(QIODevice::WriteOnly)) {
            guest_code_file.write(reinterpret_cast<const char*>(mod.text_bytes.data() + offset),
                                  static_cast<qint64>(block.size));
            guest_code_file.close();
        }

        const u64 block_begin = block.vaddr;
        const u64 block_end = block.vaddr + block.size;
        auto read_code = [&](u64 vaddr) -> std::optional<u32> {
            if (vaddr < block_begin || vaddr + sizeof(u32) > block_end) {
                return std::nullopt;
            }
            return ReadArm64InstructionAt(text_span, mod.text_vaddr, vaddr);
        };

        Dynarmic::A64::TranslationOptions options{};
        options.hook_hint_instructions = false;
        const Dynarmic::A64::LocationDescriptor descriptor{static_cast<u64>(block.vaddr),
                                                           Dynarmic::FP::FPCR{}};
        Dynarmic::IR::Block ir_block{descriptor};
        Dynarmic::A64::Translate(ir_block, descriptor, read_code, options);

        QFile ir_file(module_ir_dir + QDir::separator() + stem + QStringLiteral(".ir.txt"));
        if (!ir_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            ++(*failed_blocks);
            continue;
        }

        QTextStream ir_out(&ir_file);
        ir_out << "module=" << mod.name << "\n";
        ir_out << "block_vaddr=0x" << QStringLiteral("%1").arg(block.vaddr, 8, 16, QLatin1Char('0')) << "\n";
        ir_out << "block_size=" << block.size << "\n";
        ir_out << "instruction_count=" << block.instruction_count << "\n\n";
        ir_out << QString::fromStdString(Dynarmic::IR::DumpBlock(ir_block));
        ir_file.close();
        ++(*serialized_blocks);
    }

    return true;
}
#endif

#ifdef _WIN32
// Every Visual Studio installation that carries the x64 C++ toolset, newest
// first.
//
// This used to be a directory walk of C:/Program Files/Microsoft Visual Studio,
// which finds an installation only when it is the default edition, on the
// default drive, of a year the code was written to expect. vswhere ships with
// every VS installer since 2017 and is the supported way to ask, so it answers
// for Community, Professional and Build Tools alike wherever they were put.
static QStringList VisualStudioInstallRoots() {
    static const QStringList roots = [] {
        QStringList found;
        const QString installer_dir = QString::fromLocal8Bit(qgetenv("ProgramFiles(x86)"));
        if (installer_dir.isEmpty()) {
            return found;
        }
        const QString vswhere =
            installer_dir + QStringLiteral("/Microsoft Visual Studio/Installer/vswhere.exe");
        if (!QFile::exists(vswhere)) {
            return found;
        }
        QProcess p;
        p.start(vswhere,
                {QStringLiteral("-products"), QStringLiteral("*"), QStringLiteral("-requires"),
                 QStringLiteral("Microsoft.VisualStudio.Component.VC.Tools.x86.x64"),
                 QStringLiteral("-sort"), QStringLiteral("-property"),
                 QStringLiteral("installationPath")});
        if (!p.waitForFinished(15000)) {
            p.kill();
            p.waitForFinished(2000);
            return found;
        }
        const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
        for (const auto& line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QString root = QDir::fromNativeSeparators(line.trimmed());
            if (!root.isEmpty()) {
                found.append(root);
            }
        }
        return found;
    }();
    return roots;
}

// vcvars64.bat for the newest such installation, or empty when there is none.
// SUYU_VCVARS overrides the search outright for layouts vswhere cannot see.
static QString FindVcVars64() {
    const QString from_env = QString::fromLocal8Bit(qgetenv("SUYU_VCVARS"));
    if (!from_env.isEmpty() && QFile::exists(from_env)) {
        return from_env;
    }
    for (const auto& root : VisualStudioInstallRoots()) {
        const QString bat = root + QStringLiteral("/VC/Auxiliary/Build/vcvars64.bat");
        if (QFile::exists(bat)) {
            return bat;
        }
    }
    return {};
}
#endif

// suyu's own build (and the module/launcher builds this dialog spawns) needs a
// newer CMake than most systems have first on PATH. Prefer the VS-bundled one.
static QString FindBestCmakeExecutable() {
    const QString path_cmake = QStandardPaths::findExecutable(QStringLiteral("cmake"));
#ifdef _WIN32
    for (const auto& root : VisualStudioInstallRoots()) {
        const QString c =
            root + QStringLiteral(
                       "/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe");
        if (QFile::exists(c)) {
            return c;
        }
    }
#endif
    return path_cmake;
}

// Run a child process to completion while keeping the GUI responsive.
//
// The output MUST be drained on every poll iteration. A child that writes more
// than the OS pipe buffer (~64 KiB on Windows) blocks forever in its own
// write() if nobody reads, and QProcess only buffers what it has actually
// read - QApplication::processEvents() alone does not pump a QProcess that the
// caller is simultaneously blocking on inside waitForFinished(). A cmake build
// of a recompiled module emits far more than 64 KiB (100+ translation units,
// each with MSVC C4127/C4723 warnings), so an undrained loop deadlocks: the
// parent hangs in waitForFinished, the child hangs in write, and no compiler
// ever gets spawned for the remaining files. Everything read is accumulated
// into `captured` so callers still get the full log for diagnostics.
static int RunProcessDrained(QProcess& proc, const QString& program, const QStringList& args,
                             QString* captured = nullptr,
                             const std::function<void(const QString&)>& on_output = {}) {
    QString sink;
    QString& out = captured ? *captured : sink;
    out.clear();

    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(program, args);
    if (!proc.waitForStarted(30000)) {
        out += QStringLiteral("<process failed to start: %1>").arg(program);
        return -1;
    }

    const auto drain = [&] {
        const QByteArray chunk = proc.readAllStandardOutput();
        if (!chunk.isEmpty()) {
            const QString text = QString::fromLocal8Bit(chunk);
            if (on_output) {
                on_output(text);
            }
            out += text;
            // Keep the retained log bounded; only the tail is ever reported.
            if (out.size() > 1 << 20) {
                out = out.right(1 << 19);
            }
        }
    };

    while (proc.state() != QProcess::NotRunning) {
        // waitForReadyRead returns immediately once the child has closed its
        // stdout, so it cannot be the only thing throttling this loop - fall
        // back to waiting on the process itself when no data arrived, or the
        // loop spins a core for the rest of the build.
        if (!proc.waitForReadyRead(100)) {
            if (proc.waitForFinished(50)) {
                break;
            }
        }
        drain();
        // ExcludeUserInputEvents is load-bearing, not tidiness. This pump runs
        // for the whole of a multi-minute child build with the caller's state
        // on the stack; delivering user input here lets a stray click or an
        // Escape keypress close the dialog (destroying the object this code is
        // running inside) or press a button that re-enters the export. Timers,
        // socket notifiers and repaints still run, so the window stays alive
        // and responsive to the OS.
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    // The child may have exited with data still sitting in the pipe.
    proc.waitForFinished(5000);
    drain();
    return proc.exitCode();
}

// The last "[done/total]" step counter ninja prints in @p text, if any.
static bool LastBuildStep(const QString& text, int& done, int& total) {
    static const QRegularExpression kStep(QStringLiteral("\\[(\\d+)/(\\d+)\\]"));
    bool found = false;
    for (auto it = kStep.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        done = match.captured(1).toInt();
        total = match.captured(2).toInt();
        found = true;
    }
    return found;
}

#ifdef _WIN32
// clang-cl for the generated modules of a Windows Build export. On MK8D the
// same generated C races ~45% faster built with clang-cl /O2 than with MSVC
// /O2, and clang-cl's objects link into the MSVC-built host with link.exe as
// they are. Search order: SUYU_CLANG_CL (final when set, even if it names a
// missing file, so an A/B can hide an installed LLVM), the LLVM installer's
// default location, Visual Studio's "C++ Clang tools for Windows", then PATH.
static QString FindClangCl(QString& searched) {
    const QString from_env = qEnvironmentVariable("SUYU_CLANG_CL").trimmed();
    if (!from_env.isEmpty()) {
        searched = QStringLiteral("SUYU_CLANG_CL=") + from_env;
        return QFileInfo(from_env).isFile() ? QDir::fromNativeSeparators(from_env) : QString{};
    }
    QStringList candidates{
        QDir::fromNativeSeparators(qEnvironmentVariable("ProgramFiles", QStringLiteral("C:/Program Files"))) +
        QStringLiteral("/LLVM/bin/clang-cl.exe")};
    for (const auto& root : VisualStudioInstallRoots()) {
        candidates.append(root + QStringLiteral("/VC/Tools/Llvm/x64/bin/clang-cl.exe"));
    }
    searched = candidates.join(QStringLiteral(", ")) + QStringLiteral(", PATH");
    for (const auto& c : candidates) {
        if (QFileInfo(c).isFile()) {
            return c;
        }
    }
    return QDir::fromNativeSeparators(QStandardPaths::findExecutable(QStringLiteral("clang-cl")));
}

// Compiles a small C file with the export's own environment (vcvars64's INCLUDE
// is what finds <stdint.h>), so a broken or non-x64 clang-cl is caught here
// rather than partway through a module build.
static bool ClangClSelfTest(const QString& clang, const QProcessEnvironment& env,
                            const QString& dir, QString& version, QString& error) {
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    const QString src = dir + QStringLiteral("/selftest.c");
    const QString obj = dir + QStringLiteral("/selftest.obj");
    {
        QFile f(src);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            error = QStringLiteral("cannot write ") + src;
            return false;
        }
        f.write("#include <stdint.h>\n"
                "uint64_t suyu_clang_selftest(uint64_t x) { return x * 3u + 1u; }\n");
    }
    QString out;
    QProcess ver;
    ver.setProcessEnvironment(env);
    if (RunProcessDrained(ver, clang, {QStringLiteral("--version")}, &out) != 0) {
        error = out.right(2000);
        return false;
    }
    version = out.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (!out.contains(QStringLiteral("Target: x86_64"))) {
        error = QStringLiteral("not an x86_64 compiler: ") + out.right(2000);
        return false;
    }
    QProcess cc;
    cc.setProcessEnvironment(env);
    const int rc = RunProcessDrained(cc, clang,
                                     {QStringLiteral("/nologo"), QStringLiteral("/c"),
                                      QStringLiteral("/O2"), src, QStringLiteral("/Fo") + obj},
                                     &out);
    if (rc != 0 || !QFile::exists(obj)) {
        error = QStringLiteral("test compile failed (rc=%1): ").arg(rc) + out.right(2000);
        return false;
    }
    return true;
}
#endif

#ifdef _WIN32
// Every Visual Studio installation that carries the x64 C++ toolset, newest
// first.
//
// This used to be a directory walk of C:/Program Files/Microsoft Visual Studio,
// which finds an installation only when it is the default edition, on the
// default drive, of a year the code was written to expect. vswhere ships with
// every VS installer since 2017 and is the supported way to ask, so it answers
// for Community, Professional and Build Tools alike wherever they were put.
static QStringList VisualStudioInstallRoots() {
    static const QStringList roots = [] {
        QStringList found;
        const QString installer_dir = QString::fromLocal8Bit(qgetenv("ProgramFiles(x86)"));
        if (installer_dir.isEmpty()) {
            return found;
        }
        const QString vswhere =
            installer_dir + QStringLiteral("/Microsoft Visual Studio/Installer/vswhere.exe");
        if (!QFile::exists(vswhere)) {
            return found;
        }
        QProcess p;
        p.start(vswhere,
                {QStringLiteral("-products"), QStringLiteral("*"), QStringLiteral("-requires"),
                 QStringLiteral("Microsoft.VisualStudio.Component.VC.Tools.x86.x64"),
                 QStringLiteral("-sort"), QStringLiteral("-property"),
                 QStringLiteral("installationPath")});
        if (!p.waitForFinished(15000)) {
            p.kill();
            p.waitForFinished(2000);
            return found;
        }
        const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
        for (const auto& line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QString root = QDir::fromNativeSeparators(line.trimmed());
            if (!root.isEmpty()) {
                found.append(root);
            }
        }
        return found;
    }();
    return roots;
}

// vcvars64.bat for the newest such installation, or empty when there is none.
// SUYU_VCVARS overrides the search outright for layouts vswhere cannot see.
static QString FindVcVars64() {
    const QString from_env = QString::fromLocal8Bit(qgetenv("SUYU_VCVARS"));
    if (!from_env.isEmpty() && QFile::exists(from_env)) {
        return from_env;
    }
    for (const auto& root : VisualStudioInstallRoots()) {
        const QString bat = root + QStringLiteral("/VC/Auxiliary/Build/vcvars64.bat");
        if (QFile::exists(bat)) {
            return bat;
        }
    }
    return {};
}
#endif

// suyu's own build (and the module/launcher builds this dialog spawns) needs a
// newer CMake than most systems have first on PATH. Prefer the VS-bundled one.
static QString FindBestCmakeExecutable() {
    const QString path_cmake = QStandardPaths::findExecutable(QStringLiteral("cmake"));
#ifdef _WIN32
    for (const auto& root : VisualStudioInstallRoots()) {
        const QString c =
            root + QStringLiteral(
                       "/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe");
        if (QFile::exists(c)) {
            return c;
        }
    }
#endif
    return path_cmake;
}

// Run a child process to completion while keeping the GUI responsive.
//
// The output MUST be drained on every poll iteration. A child that writes more
// than the OS pipe buffer (~64 KiB on Windows) blocks forever in its own
// write() if nobody reads, and QProcess only buffers what it has actually
// read - QApplication::processEvents() alone does not pump a QProcess that the
// caller is simultaneously blocking on inside waitForFinished(). A cmake build
// of a recompiled module emits far more than 64 KiB (100+ translation units,
// each with MSVC C4127/C4723 warnings), so an undrained loop deadlocks: the
// parent hangs in waitForFinished, the child hangs in write, and no compiler
// ever gets spawned for the remaining files. Everything read is accumulated
// into `captured` so callers still get the full log for diagnostics.
static int RunProcessDrained(QProcess& proc, const QString& program, const QStringList& args,
                             QString* captured = nullptr) {
    QString sink;
    QString& out = captured ? *captured : sink;
    out.clear();

    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(program, args);
    if (!proc.waitForStarted(30000)) {
        out += QStringLiteral("<process failed to start: %1>").arg(program);
        return -1;
    }

    const auto drain = [&] {
        const QByteArray chunk = proc.readAllStandardOutput();
        if (!chunk.isEmpty()) {
            out += QString::fromLocal8Bit(chunk);
            // Keep the retained log bounded; only the tail is ever reported.
            if (out.size() > 1 << 20) {
                out = out.right(1 << 19);
            }
        }
    };

    while (proc.state() != QProcess::NotRunning) {
        // waitForReadyRead returns immediately once the child has closed its
        // stdout, so it cannot be the only thing throttling this loop - fall
        // back to waiting on the process itself when no data arrived, or the
        // loop spins a core for the rest of the build.
        if (!proc.waitForReadyRead(100)) {
            if (proc.waitForFinished(50)) {
                break;
            }
        }
        drain();
        // ExcludeUserInputEvents is load-bearing, not tidiness. This pump runs
        // for the whole of a multi-minute child build with the caller's state
        // on the stack; delivering user input here lets a stray click or an
        // Escape keypress close the dialog (destroying the object this code is
        // running inside) or press a button that re-enters the export. Timers,
        // socket notifiers and repaints still run, so the window stays alive
        // and responsive to the OS.
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    // The child may have exited with data still sitting in the pipe.
    proc.waitForFinished(5000);
    drain();
    return proc.exitCode();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// AOT Pre-compilation — Real Implementation
// ---------------------------------------------------------------------------

// Optionally publish the finished build to Steam. A recompiled export is an
// ordinary executable, so the shortcut needs no launch options and no emulator
// behind it - which also means any shortcut the user already has for this title
// (the ROM, launched through suyu) now points at the worse of the two.
// Replacing it is the default, with a checkbox for anyone who wants both.
void GameExportDialog::MaybeAddToSteam(const QString& game_name, const QString& exe_path) {
    if (steam_shortcut_checkbox == nullptr || !steam_shortcut_checkbox->isChecked()) {
        return;
    }
    SteamIntegration steam;
    if (!steam.IsSteamInstalled()) {
        if (status_label != nullptr) {
            status_label->setText(tr("Steam not found - skipped library shortcut."));
        }
        return;
    }
    if (steam_replace_rom_checkbox != nullptr && steam_replace_rom_checkbox->isChecked()) {
        steam.RemoveGameShortcut(game_name);
    }
    const bool added = steam.AddGameShortcut(game_name, exe_path, exe_path);
    if (added) {
        // Grid art is what makes the entry read as a real game rather than a
        // generic shortcut; fetched by title from Steam's public endpoints.
        const QString userdata = steam.GetSteamUserdataPath();
        if (!userdata.isEmpty()) {
            steam.FetchArtwork(game_name, userdata);
        }
    }
    if (status_label != nullptr) {
        status_label->setText(added ? tr("Added \"%1\" to the Steam library.").arg(game_name)
                                    : tr("Could not add \"%1\" to Steam.").arg(game_name));
    }
}

bool GameExportDialog::WantsCompiledOutput() const {
    return output_format_combo && output_format_combo->currentIndex() == 1;
}

QString GameExportDialog::RunAotPrecompile(const QString& exefs_dir,
                                           const QString& cache_dir,
                                           RecompileBackend backend,
                                           const QString& game_name) {
    QDir().mkpath(cache_dir);

    const QString manifest_path = cache_dir + QDir::separator() + QStringLiteral("aot_manifest.json");

    // blockmaps/, ir/ and code/ are debugging material for a codegen stage that
    // no longer exists: nothing in suyu or in the generated project reads any of
    // them, and EmitProject works from the module's text bytes directly. They
    // used to be created (and blockmaps written) on every export, which put
    // three dead directories at the top of every package. They are now produced
    // only when explicitly asked for, by the same switch that gates the
    // per-block dumps.
    const bool dump_debug_artifacts = !qEnvironmentVariableIsEmpty("SUYU_AOT_DUMP_BLOCKS");
    const QString debug_root = cache_dir + QDir::separator() + QStringLiteral("debug");
    const QString blockmap_dir = debug_root + QDir::separator() + QStringLiteral("blockmaps");
    const QString code_dir = debug_root + QDir::separator() + QStringLiteral("code");
    const QString ir_dir = debug_root + QDir::separator() + QStringLiteral("ir");
    if (dump_debug_artifacts) {
        QDir().mkpath(blockmap_dir);
        QDir().mkpath(code_dir);
        QDir().mkpath(ir_dir);
    }

    // blockmaps/, ir/ and code/ are debugging material for a codegen stage that
    // no longer exists: nothing in suyu or in the generated project reads any of
    // them, and EmitProject works from the module's text bytes directly. They
    // used to be created (and blockmaps written) on every export, which put
    // three dead directories at the top of every package. They are now produced
    // only when explicitly asked for, by the same switch that gates the
    // per-block dumps.
    const bool dump_debug_artifacts = !qEnvironmentVariableIsEmpty("SUYU_AOT_DUMP_BLOCKS");

    const bool translate_all = suyu::recomp::TranslateAllForExport(
        backend == RecompileBackend::SuyuStatic,
        !qEnvironmentVariableIsEmpty("SUYU_AOT_TRANSLATE_ALL"));
    suyu::recomp::g_translate_all = translate_all;
    // ABI 6 is the default: the FM1 page-table fast path for guest memory, the
    // GG1 generation code guard and FPX1 exact native FP. Together they more
    // than doubled the MK8D race on Windows (clang-cl: 34.6 -> 56.3 fps) with
    // identical results. Setting one to 0 leaves it out; SUYU_AOT_FASTMEM=0
    // gives ABI 5 output, byte-identical to earlier releases.
    const auto enabled = [](const char* name) {
        return qEnvironmentVariable(name) != QStringLiteral("0");
    };
    // No standalone runner and no bundled segments: the translated code runs only inside
    // suyu, against the user's own game file.
    suyu::recomp::g_emit_standalone_runner = false;
    suyu::recomp::g_emit_fastmem = enabled("SUYU_AOT_FASTMEM");
    // GG1 and FPX1 are ABI 6 features, so they need FM1.
    suyu::recomp::g_emit_guard_gen = enabled("SUYU_AOT_GUARD_GEN");
    if (suyu::recomp::g_emit_guard_gen && !suyu::recomp::g_emit_fastmem) {
        LOG_WARNING(Frontend, "The generation code guard needs SUYU_AOT_FASTMEM; exporting "
                              "without it");
        suyu::recomp::g_emit_guard_gen = false;
    }
    suyu::recomp::g_emit_fpx = enabled("SUYU_AOT_FPX");
    if (suyu::recomp::g_emit_fpx && !suyu::recomp::g_emit_fastmem) {
        LOG_WARNING(Frontend, "Exact native FP (FPX1) needs SUYU_AOT_FASTMEM; exporting "
                              "without it");
        suyu::recomp::g_emit_fpx = false;
    }
    // What the images will report from recomp_image_features(), recorded in the
    // manifest so a cached export with other features is not reused.
    const unsigned image_features =
        suyu::recomp::g_emit_fastmem
            ? (Core::RecompImageFeature::FastmemPT1 |
               (suyu::recomp::EmitGuardGen() ? Core::RecompImageFeature::GuardGen1 : 0u) |
               (suyu::recomp::g_emit_fpx ? Core::RecompImageFeature::ExactFpX1 : 0u))
            : 0u;
    const QString debug_root = cache_dir + QDir::separator() + QStringLiteral("debug");
    const QString blockmap_dir = debug_root + QDir::separator() + QStringLiteral("blockmaps");
    const QString code_dir = debug_root + QDir::separator() + QStringLiteral("code");
    const QString ir_dir = debug_root + QDir::separator() + QStringLiteral("ir");
    const bool full_scan = aot_full_scan_checkbox->isChecked();
    const bool is_hybrid = backend == RecompileBackend::Hybrid;
    const bool fallback_enabled = is_hybrid && fallback_to_interpreter_checkbox &&
                                  fallback_to_interpreter_checkbox->isChecked();
    const QString requested_backend_name =
        is_hybrid ? QStringLiteral("suyu-hybrid") : QStringLiteral("suyu-static");
    const QString effective_backend_name = requested_backend_name;

    // Addresses earlier runs of this title reached with no recompiled block
    // (recomp_gaps.json in this suyu's user folder, where Hybrid runs and
    // imported coverage files are pooled). They become extra block-discovery
    // roots below, but only in a module whose build ID matches exactly. With no
    // recorded addresses nothing changes and the generated code is identical.
    Core::RecompGaps::GapData recorded_gaps;
    {
        std::string gaps_error;
        const auto program_id = SelectedProgramId();
        const auto gaps_path = Core::RecompGaps::SharedStoreFile(program_id);
        std::error_code gaps_ec;
        if (!gaps_path.empty() && std::filesystem::exists(gaps_path, gaps_ec)) {
            auto loaded = Core::RecompGaps::ReadFile(gaps_path, &gaps_error);
            if (loaded && loaded->title_id == Core::RecompGaps::TitleIdHex(program_id)) {
                recorded_gaps = std::move(*loaded);
                LOG_INFO(Frontend,
                         "AOT coverage: {} recorded address(es) over {} run(s) from {}",
                         recorded_gaps.GapOffsets(), recorded_gaps.runs,
                         Common::FS::PathToUTF8String(gaps_path));
            } else {
                LOG_WARNING(Frontend, "AOT coverage: ignoring {}: {}",
                            Common::FS::PathToUTF8String(gaps_path),
                            loaded ? std::string{"recorded for another title"} : gaps_error);
            }
        }
    }
    const QString coverage_fingerprint =
        QString::fromStdString(Core::RecompGaps::Fingerprint(recorded_gaps));

    // A completed export is immutable for a given game/output directory and
    // scan mode. Reusing it makes re-opening the export dialog or packaging
    // the same title again effectively instant instead of decompressing every
    // NSO and regenerating gigabytes of C.
    if (QFile::exists(manifest_path)) {
        QFile manifest(manifest_path);
        if (manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QByteArray manifest_bytes = manifest.readAll();
            const QString contents = QString::fromUtf8(manifest_bytes);
            const bool same_scan = contents.contains(
                QStringLiteral("\"full_scan\": ") + (full_scan ? QStringLiteral("true")
                                                                  : QStringLiteral("false")));
            const bool same_backend = contents.contains(
                QStringLiteral("\"effective_backend\": \"") + effective_backend_name +
                QStringLiteral("\""));
            const bool has_recompiled_project =
                QDir(cache_dir + QDir::separator() + QStringLiteral("exefs")).exists();
            const bool has_required_launcher =
                !WantsCompiledOutput() ||
                QFile::exists(cache_dir + QDir::separator() + QStringLiteral("launcher") +
                              QDir::separator() + QStringLiteral("static_launcher.exe"));
            const bool same_image_abi = contents.contains(
                suyu::recomp::g_emit_fastmem ? QStringLiteral("\"image_abi\": 6,")
                                             : QStringLiteral("\"image_abi\": 5,"));
            // ABI 6 images also have to carry the same feature set.
            const bool same_image_features =
                !suyu::recomp::g_emit_fastmem ||
                contents.contains(QStringLiteral("\"image_features\": %1,").arg(image_features));
            const bool same_correctness_revision = contents.contains(
                QStringLiteral("\"correctness_revision\": \"20261010-graphics-safe-defaults-r3\","));
            const auto cached_manifest = QJsonDocument::fromJson(manifest_bytes).object();
            const bool same_patches = cached_manifest.value(QStringLiteral("baked_patches"))
                                          .toObject().value(QStringLiteral("fingerprint")).toString() ==
                                      patch_fingerprint;
            const bool same_translate_all = contents.contains(
                QStringLiteral("\"translate_all\": ") +
                (translate_all ? QStringLiteral("true,") : QStringLiteral("false,")));
            const bool same_source = contents.contains(
                QStringLiteral("\"source_exefs_sha256\": \"") + source_hash +
                QStringLiteral("\""));
            // Recorded coverage roots change the generated code; a cache from
            // before this field existed had none.
            const bool same_coverage =
                contents.contains(QStringLiteral("\"coverage_fingerprint\": \"") +
                                  coverage_fingerprint + QStringLiteral("\"")) ||
                (coverage_fingerprint.isEmpty() &&
                 !contents.contains(QStringLiteral("\"coverage_fingerprint\"")));
            QStringList cached_fallback_modules;
            bool same_fallback_policy = ReadCachedFallbackPolicy(
                manifest_bytes, fallback_enabled, cached_fallback_modules);
            for (const auto& name : RequestedJitModules()) {
                same_fallback_policy =
                    same_fallback_policy && cached_fallback_modules.contains(name);
            }
            if (same_scan && same_backend && same_image_abi && same_image_features &&
                same_correctness_revision && same_coverage && same_patches &&
                same_translate_all && same_source && same_fallback_policy &&
                has_recompiled_project && has_required_launcher) {
                last_fallback_modules = std::move(cached_fallback_modules);
                LOG_INFO(Frontend, "Reusing completed AOT cache at {}", cache_dir.toStdString());
                return cache_dir;
            }
        }
    }

    // A changed input may have removed modules. Rebuild the owned cache from
    // an empty tree so no generated C or launcher survives from that input.
    if (QDir(cache_dir).exists() && !QDir(cache_dir).removeRecursively()) {
        LOG_ERROR(Frontend, "Could not clear stale AOT cache at {}", cache_dir.toStdString());
        return {};
    }
    if (!QDir().mkpath(cache_dir)) {
        return {};
    }
    if (dump_debug_artifacts) {
        QDir().mkpath(blockmap_dir);
        QDir().mkpath(code_dir);
        QDir().mkpath(ir_dir);
    }

    // A completed export is immutable for a given game/output directory and
    // scan mode. Reusing it makes re-opening the export dialog or packaging
    // the same title again effectively instant instead of decompressing every
    // NSO and regenerating gigabytes of C.
    if (QFile::exists(manifest_path)) {
        QFile manifest(manifest_path);
        if (manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString contents = QString::fromUtf8(manifest.readAll());
            const bool same_scan = contents.contains(
                QStringLiteral("\"full_scan\": ") + (full_scan ? QStringLiteral("true")
                                                                  : QStringLiteral("false")));
            const bool same_backend = contents.contains(
                QStringLiteral("\"effective_backend\": \"") + effective_backend_name +
                QStringLiteral("\""));
            const bool has_recompiled_project =
                QDir(cache_dir + QDir::separator() + QStringLiteral("exefs")).exists();
            const bool has_required_launcher =
                !WantsCompiledOutput() ||
                QFile::exists(cache_dir + QDir::separator() + QStringLiteral("launcher") +
                              QDir::separator() + QStringLiteral("static_launcher.exe"));
            if (same_scan && same_backend && has_recompiled_project && has_required_launcher) {
                LOG_INFO(Frontend, "Reusing completed AOT cache at {}", cache_dir.toStdString());
                return cache_dir;
            }
        }
    }

    // Collect NSO files to analyze — either from VFS (ROM containers) or from extracted ExeFS
    std::vector<NsoAnalysisResult> module_results;
    bool used_vfs = false;

    // First, try to open the ROM via VFS to extract ExeFS directly
    if (!rom_path.isEmpty() && QFile::exists(rom_path) && QFileInfo(rom_path).isFile()) {
        const auto t_extract_start = std::chrono::steady_clock::now();
        auto exefs_vdir = source_exefs;
        const auto t_extract_end = std::chrono::steady_clock::now();
        LOG_INFO(Frontend, "AOT diag: ExtractExeFsFromRom took {} ms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(t_extract_end - t_extract_start).count());
        if (exefs_vdir) {
            used_vfs = true;
            const auto t_getfiles_start = std::chrono::steady_clock::now();
            const auto nso_files = exefs_vdir->GetFiles();
            const auto t_getfiles_end = std::chrono::steady_clock::now();
            LOG_INFO(Frontend, "AOT diag: GetFiles() took {} ms, {} entries",
                     std::chrono::duration_cast<std::chrono::milliseconds>(t_getfiles_end - t_getfiles_start).count(),
                     nso_files.size());

            // The recompiler only understands AArch64. A 32-bit ARM title (or
            // the 32-bit base build of a title whose update went 64-bit) fed
            // to it does not fail - every A32 word decodes as *something*, so
            // the export "succeeds" with ~86% of instructions unhandled and
            // produces images that crash the emulator on entry. main.npdm
            // carries the answer in one bit, so read it and stop here instead.
            for (const auto& f : nso_files) {
                if (f->GetName() != "main.npdm") {
                    continue;
                }
                std::vector<u8> npdm = f->ReadAllBytes();
                if (npdm.size() < 0x10 || std::memcmp(npdm.data(), "META", 4) != 0) {
                    break;
                }
                // META flags, bit 0 = Is64BitInstruction.
                if ((npdm[0x0C] & 1) == 0) {
                    LOG_ERROR(Frontend,
                              "AOT: refusing to recompile a 32-bit ARM ExeFS (main.npdm flags "
                              "{:#04x}). The AArch64 recompiler cannot translate A32 code. If "
                              "this title's 64-bit build ships in an update, install the update "
                              "or point the export at it directly.",
                              npdm[0x0C]);
                    return QString();
                }
                break;
            }

            // Standard NSO module names in load order
            static const std::vector<std::string> module_names = {
                "rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3",
                "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"
            };

            for (const auto& nso_file : nso_files) {
                ReportStage(ExportStage::Extract,
                            static_cast<double>(module_results.size()) / nso_files.size(),
                            tr("Reading %1...").arg(QString::fromStdString(nso_file->GetName())));
                const auto t_file_start = std::chrono::steady_clock::now();
                bool patch_failed = false;
                auto result = AnalyzeNsoFile(nso_file, full_scan, patch_manager, patch_failed);
                if (patch_failed) return {};
                const auto t_file_end = std::chrono::steady_clock::now();
                LOG_INFO(Frontend, "AOT diag: AnalyzeNsoFile({}) took {} ms, size={}",
                         nso_file->GetName(),
                         std::chrono::duration_cast<std::chrono::milliseconds>(t_file_end - t_file_start).count(),
                         nso_file->GetSize());
                if (result.has_value()) {
                    module_results.push_back(std::move(*result));
                }
            }

            // Also save extracted ExeFS content to cache
            const QString exefs_cache = cache_dir + QDir::separator() + QStringLiteral("exefs");
            QDir().mkpath(exefs_cache);
            for (const auto& f : nso_files) {
                const auto t_cache_start = std::chrono::steady_clock::now();
                const QString out_path = exefs_cache + QDir::separator() +
                                         QString::fromStdString(f->GetName());
                QFile out_file(out_path);
                if (out_file.open(QIODevice::WriteOnly)) {
                    std::vector<u8> nso_bytes = f->ReadAllBytes();
                    out_file.write(reinterpret_cast<const char*>(nso_bytes.data()),
                                static_cast<qint64>(nso_bytes.size()));
                    out_file.close();
                }
                const auto t_cache_end = std::chrono::steady_clock::now();
                LOG_INFO(Frontend, "AOT diag: cache copy of {} took {} ms", f->GetName(),
                         std::chrono::duration_cast<std::chrono::milliseconds>(t_cache_end - t_cache_start).count());
            }
        }
    }

    // Fallback: read NSO files from the extracted exefs directory on disk
    if (!used_vfs && QDir(exefs_dir).exists()) {
        if (!CopyDeconstructedExeFs(exefs_dir,
                                    cache_dir + QDir::separator() + QStringLiteral("exefs"))) {
            return QString();
        }

        // RealVfsFile holds a raw RealVfsFilesystem& (not a shared_ptr), so a
    // locally-scoped vfs would dangle once files it opened outlive this
    // function - keep one filesystem instance alive for the process.
    static const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
        QDirIterator it(exefs_dir, QDir::Files | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            it.next();
            auto nso_file = vfs->OpenFile(it.filePath().toStdString(), FileSys::OpenMode::Read);
            if (nso_file) {
                bool patch_failed = false;
                auto result = AnalyzeNsoFile(nso_file, full_scan, patch_manager, patch_failed);
                if (patch_failed) return {};
                if (result.has_value()) {
                    module_results.push_back(std::move(*result));
                }
            }
        }
    }

    if (module_results.empty()) {
        return QString();
    }
    ReportStage(ExportStage::Extract, 1.0);

    // Compute totals
    u32 total_blocks = 0;
    u32 total_instructions = 0;
    u32 total_text_bytes = 0;
    u32 total_ir_blocks = 0;
    u32 total_ir_failures = 0;
    for (const auto& mod : module_results) {
        total_blocks += mod.total_blocks;
        total_instructions += mod.total_instructions;
        total_text_bytes += mod.text_size;
        if (dump_debug_artifacts) {
            SerializeTranslatedBlocks(mod, ir_dir, code_dir, &total_ir_blocks, &total_ir_failures);
        }
    }

    // Static recompilation: lift each module's AArch64 .text into a buildable, cross-platform C
    // project. Output mirrors the ROM's exefs structure (hactool/NxFileViewer convention):
    //   exefs/
    //     nso/       <- raw NSO binaries (already extracted above)
    //     main/      <- C project for the main module
    //     rtld/      <- C project for rtld
    //     sdk/       <- C project for sdk
    //     ...
    //     CMakeLists.txt  <- top-level: builds all modules
    const QString recomp_root = cache_dir + QDir::separator() + QStringLiteral("exefs");
    QDir().mkpath(recomp_root);

    // Move raw NSOs into exefs/nso/ so the layout stays clean
    const QString nso_raw_dir = recomp_root + QDir::separator() + QStringLiteral("nso");
    QDir().mkpath(nso_raw_dir);
    {
        const QString old_exefs = cache_dir + QDir::separator() + QStringLiteral("exefs");
        // Snapshot the listing before renaming anything: renaming an entry out
        // of the same directory a QDirIterator is actively walking invalidates
        // its cursor, so only the first file (alphabetically "main") ever got
        // moved and the rest - rtld/sdk/subsdk0 - were left behind as raw
        // blobs sitting at exefs/<name>, colliding with the mkpath() below
        // that needs that same path to be a directory for the module's C
        // project ("is a file, not a directory" cmake configure failure).
        QStringList to_move;
        QDirIterator nso_it(old_exefs, QDir::Files | QDir::NoDotAndDotDot);
        while (nso_it.hasNext()) {
            to_move << nso_it.next();
        }
        for (const QString& src : to_move) {
            const QString dst = nso_raw_dir + QDir::separator() + QFileInfo(src).fileName();
            if (src != dst) {
                QFile::rename(src, dst);
            }
        }
    }

    const bool fallback_enabled = fallback_to_interpreter_checkbox &&
                                  fallback_to_interpreter_checkbox->isChecked();

    u64 recomp_total_blocks = 0;
    QStringList recomp_module_dirs;
    QStringList fallback_modules;

    for (const auto& mod : module_results) {
        if (mod.text_bytes.empty()) {
            continue;
        }
        ReportStage(ExportStage::Lift,
                    total_code_bytes ? static_cast<double>(lifted_code_bytes) / total_code_bytes
                                     : 0.0,
                    tr("Lifting module %1 (%2 of %3) to C...")
                        .arg(mod.name)
                        .arg(++lifted_modules)
                        .arg(module_results.size()));
        lifted_code_bytes += mod.text_bytes.size();
        const QString mod_dir = recomp_root + QDir::separator() + mod.name;
        QDir().mkpath(mod_dir);

        std::vector<u64> exported_roots = CollectExportedSymbolAddresses(mod);
        {
            std::vector<u64> data_ptr_roots = ScanDataForCodePointers(mod);
            exported_roots.insert(exported_roots.end(), data_ptr_roots.begin(), data_ptr_roots.end());
            std::vector<u64> reloc_roots = ScanRelocationsForCodePointers(mod);
            exported_roots.insert(exported_roots.end(), reloc_roots.begin(), reloc_roots.end());
            std::sort(exported_roots.begin(), exported_roots.end());
            exported_roots.erase(std::unique(exported_roots.begin(), exported_roots.end()),
                                 exported_roots.end());
        }

        suyu::recomp::RecompileStats stats{};
        bool emit_ok = false;
        try {
            stats = suyu::recomp::EmitProject(
                mod.name.toStdString(), mod.text_bytes.data(), mod.text_bytes.size(),
                mod.text_vaddr, mod_dir.toStdString(), /*source_only=*/false,
                mod.rodata_bytes.empty() ? nullptr : mod.rodata_bytes.data(),
                mod.rodata_bytes.size(),
                mod.data_bytes.empty() ? nullptr : mod.data_bytes.data(), mod.data_bytes.size(),
                mod.entry_vaddr, game_name.toStdString(), &exported_roots);
            emit_ok = true;

            // Report coverage, not just volume. Block and instruction counts say
            // how much code was walked; only this says how much of it the decoder
            // actually understood. A high unhandled fraction means the AOT image
            // is mostly a shim that bounces straight back into the JIT - and a
            // fraction near 100% means the input was not the ISA we think it is.
            const double unhandled_pct = stats.UnhandledFraction() * 100.0;
            LOG_INFO(Frontend,
                     "AOT coverage [{}]: {} blocks, {} instructions emitted, {} unhandled "
                     "({:.2f}%), {} distinct opcode signatures",
                     mod.name.toStdString(), stats.blocks, stats.emitted, stats.unhandled,
                     unhandled_pct, stats.unhandled_by_signature.size());
            for (const auto& [group, count] : stats.unhandled_by_group) {
                LOG_INFO(Frontend, "AOT coverage [{}]:   unhandled {}: {}",
                         mod.name.toStdString(), suyu::recomp::EncodingGroupName(group), count);
            }
            if (unhandled_pct > 50.0) {
                LOG_WARNING(Frontend,
                            "AOT coverage [{}]: over half of all instructions are unhandled. "
                            "Either the decoder is missing something very common, or this module "
                            "is not AArch64 at all.",
                            mod.name.toStdString());
            }
        } catch (const std::exception& e) {
            LOG_ERROR(Frontend, "EmitProject failed for module {}: {}", mod.name.toStdString(),
                      e.what());
        } catch (...) {
            LOG_ERROR(Frontend, "EmitProject failed for module {} (unknown error)",
                      mod.name.toStdString());
        }

        if (!emit_ok) {
            if (!fallback_enabled) {
                QMessageBox::critical(
                    this, tr("Export Failed"),
                    tr("Module '%1' could not be recompiled.\n\nEnable 'Fall back to interpreter' "
                       "to skip failed modules and use the dynarmic JIT for them at runtime.")
                        .arg(mod.name));
                return {};
            }
            // Emit a stub CMakeLists so the top-level build doesn't break on this dir
            {
                QFile stub(mod_dir + QDir::separator() + QStringLiteral("CMakeLists.txt"));
                if (stub.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    QTextStream o(&stub);
                    o << "# Module " << mod.name
                      << " fell back to dynarmic JIT — no static recompilation available.\n"
                         "# At runtime suyu will use the interpreter for this module.\n"
                         "message(STATUS \"[fallback] " << mod.name << " uses dynarmic\")\n";
                }
            }
            fallback_modules.append(mod.name);
            LOG_WARNING(Frontend, "Module {} fell back to dynarmic interpreter", mod.name.toStdString());
            continue;
        }

        recomp_total_blocks += stats.blocks;
        recomp_module_dirs.append(mod.name);

        // Actually build it (only in Build mode).
        const QString cmake = WantsCompiledOutput() ? FindBestCmakeExecutable() : QString();
        if (WantsCompiledOutput() && cmake.isEmpty()) {
            LOG_ERROR(Frontend, "Build export requested but cmake was not found");
            QMessageBox::critical(
                this, tr("Export Failed"),
                tr("Export Format is set to Build, but CMake could not be found, so "
                   "no executable can be produced.\n\nInstall CMake (and a C compiler) and try "
                   "again, or choose the Source export format if you only want the generated C."));
            return {};
        }
        if (!cmake.isEmpty()) {
            status_label->setText(tr("Compiling %1 (this takes a while)...").arg(mod.name));
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

            const QString build_dir = mod_dir + QDir::separator() + QStringLiteral("build");
            QString configure_log;
            QString build_log;

            QProcess configure;
            const int configure_rc = RunProcessDrained(
                configure, cmake,
                {QStringLiteral("-S"), mod_dir, QStringLiteral("-B"), build_dir,
                 // The package ships one self-contained exe, so only the static
                 // library is ever consumed. Without this the generated project
                 // also builds a standalone exe and a loadable DLL from the same
                 // sources - three full compiles of a translation unit that can
                 // take 40 minutes each on a large title.
                 QStringLiteral("-DRECOMP_STATIC_ONLY=ON")},
                &configure_log);
            if (configure_rc == 0) {
                QProcess build;
                const int build_rc =
                    RunProcessDrained(build, cmake,
                                      {QStringLiteral("--build"), build_dir,
                                       QStringLiteral("--config"), QStringLiteral("Release"),
                                       QStringLiteral("--parallel")},
                                      &build_log);
                if (build_rc != 0) {
                    LOG_ERROR(Frontend, "Recompiled module {} failed to compile:\n{}",
                              mod.name.toStdString(), build_log.right(4000).toStdString());
                    if (fallback_enabled) {
                        LOG_WARNING(Frontend, "Module {} compile failed, falling back to dynarmic",
                                    mod.name.toStdString());
                        fallback_modules.append(mod.name);
                        continue;
                    }
                    QMessageBox::critical(
                        this, tr("Build Failed"),
                        tr("Compiling module '%1' failed.\n\nThe generated sources are still in:\n"
                           "%2\n\nEnable 'Fall back to interpreter' to continue despite build "
                           "failures. See the suyu log for compiler output.")
                            .arg(mod.name, mod_dir));
                    return {};
                }
            } else {
                LOG_ERROR(Frontend, "cmake could not configure recompiled module {}:\n{}",
                          mod.name.toStdString(), configure_log.right(4000).toStdString());
                if (fallback_enabled) {
                    LOG_WARNING(Frontend, "Module {} cmake configure failed, falling back to dynarmic",
                                mod.name.toStdString());
                    fallback_modules.append(mod.name);
                    continue;
                }
                QMessageBox::critical(
                    this, tr("Build Failed"),
                    tr("CMake could not configure module '%1'.\n\nThe generated sources are in:\n"
                       "%2\n\nEnable 'Fall back to interpreter' to continue despite failures.")
                        .arg(mod.name, mod_dir));
                return {};
            }

            const QStringList produced =
                // With RECOMP_STATIC_ONLY the only artifact is the static library,
                // so accept that as proof the module compiled.
                QDir(build_dir).entryList({QStringLiteral("*.exe"), QStringLiteral("*.dll"),
                                           QStringLiteral("*.so"), QStringLiteral("*.dylib"),
                                           QStringLiteral("*.lib"), QStringLiteral("*.a"),
                                           QStringLiteral("recompiled")},
                                          QDir::Files, QDir::Name) +
                QDir(build_dir + QDir::separator() + QStringLiteral("Release"))
                    .entryList({QStringLiteral("*.exe"), QStringLiteral("*.dll"),
                                QStringLiteral("*.lib"), QStringLiteral("*.a")},
                               QDir::Files, QDir::Name);
            if (produced.isEmpty()) {
                LOG_ERROR(Frontend, "Build of module {} reported success but produced no binary",
                          mod.name.toStdString());
                if (fallback_enabled) {
                    fallback_modules.append(mod.name);
                    continue;
                }
                QMessageBox::critical(
                    this, tr("Build Failed"),
                    tr("CMake reported success for module '%1' but no binary was found in:\n%2")
                        .arg(mod.name, build_dir));
                return {};
            }
            LOG_INFO(Frontend, "Built recompiled module {}: {}", mod.name.toStdString(),
                     produced.join(QStringLiteral(", ")).toStdString());
        }
    }

    // Top-level CMakeLists.txt: includes all module subdirs so `cmake -S exefs -B build`
    // builds everything in one shot.
    {
        QFile top_cmake(recomp_root + QDir::separator() + QStringLiteral("CMakeLists.txt"));
        if (top_cmake.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream o(&top_cmake);
            o << "cmake_minimum_required(VERSION 3.13)\n"
                 "project(" << game_name << "_recompiled C)\n\n"
                 "# Add each recompiled module as a subdirectory.\n"
                 "# Each module builds its own 'recompiled' exe and 'recompiled_image' shared lib.\n";
            for (const auto& m : recomp_module_dirs) {
                o << "if(EXISTS \"${CMAKE_CURRENT_SOURCE_DIR}/" << m << "/CMakeLists.txt\")\n"
                  << "  add_subdirectory(" << m << ")\n"
                  << "endif()\n";
            }
            if (!fallback_modules.isEmpty()) {
                o << "\n# Modules using dynarmic JIT fallback (not recompiled):\n";
                for (const auto& m : fallback_modules) {
                    o << "# " << m << "\n";
                }
            }
        }
    }

    // ── Single self-contained executable ────────────────────────────────────
    // Every module also builds as a static library (see arm64_to_c.h's
    // EmitProject). Linking those into a private copy of suyu-cmd produces one
    // exe that carries the recompiled CPU code inside it, so the package has no
    // recompiled_*.dll siblings at all. The registration file below is what
    // tells that build which modules exist and in what order they load.
    if (WantsCompiledOutput() && !recomp_module_dirs.isEmpty()) {
        // NSO load order, which is also the index order Core's base setter uses.
        QStringList ordered;
        const auto take = [&](const QString& name) {
            if (recomp_module_dirs.contains(name)) {
                ordered.append(name);
            }
        };
        take(QStringLiteral("rtld"));
        take(QStringLiteral("main"));
        for (int i = 0; i < 10; ++i) {
            take(QStringLiteral("subsdk%1").arg(i));
        }
        take(QStringLiteral("sdk"));
        for (const auto& m : recomp_module_dirs) {
            if (!ordered.contains(m)) {
                ordered.append(m);
            }
        }

        QFile reg(recomp_root + QDir::separator() + QStringLiteral("recomp_registration.c"));
        if (reg.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream o(&reg);
            o << "/* auto-generated by suyu game export - DO NOT EDIT */\n"
                 "/* Lists this game's statically linked recompiled modules in NSO load\n"
                 "   order. Consumed by src/suyu_cmd/suyu.cpp. */\n"
                 "#include <stdint.h>\n\n"
                 "typedef void (*SuyuRecompBlockFn)(void*);\n\n";
            for (const auto& m : ordered) {
                o << "extern SuyuRecompBlockFn recomp_image_lookup_" << m << "(uint64_t);\n"
                  << "extern void recomp_image_set_base_" << m << "(uint64_t);\n"
                  << "extern uint64_t g_module_base_" << m << ";\n";
            }
            o << "\ntypedef struct {\n"
                 "    const char* name;\n"
                 "    SuyuRecompBlockFn (*lookup)(uint64_t);\n"
                 "    void (*set_base)(uint64_t);\n"
                 "} SuyuRecompStaticModule;\n\n"
                 "static const SuyuRecompStaticModule s_modules[] = {\n";
            for (const auto& m : ordered) {
                o << "    { \"" << m << "\", recomp_image_lookup_" << m
                  << ", recomp_image_set_base_" << m << " },\n";
            }
            o << "};\n\n"
                 "const SuyuRecompStaticModule* suyu_recomp_static_modules(unsigned* count) {\n"
                 "    *count = (unsigned)(sizeof(s_modules) / sizeof(s_modules[0]));\n"
                 "    return s_modules;\n"
                 "}\n";
            reg.close();
        }

        // Locate the suyu build tree this frontend was built from. The static
        // variant is an extra target inside it, so all of core/video_core/... is
        // already compiled and only the new target has to link.
        QString build_tree;
        QString source_tree;
        // The suyu tree needs a newer CMake than is typically first on PATH, so
        // reuse whichever one configured it.
        QString tree_cmake;
        {
            QDir up(QCoreApplication::applicationDirPath());
            for (int level = 0; level < 5; ++level) {
                const QString cache = up.absoluteFilePath(QStringLiteral("CMakeCache.txt"));
                if (QFile::exists(cache)) {
                    build_tree = up.absolutePath();
                    QFile cf(cache);
                    if (cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                        QTextStream in(&cf);
                        while (!in.atEnd()) {
                            const QString line = in.readLine();
                            if (line.startsWith(QStringLiteral("CMAKE_HOME_DIRECTORY:"))) {
                                source_tree = line.section(QLatin1Char('='), 1);
                            } else if (line.startsWith(QStringLiteral("CMAKE_COMMAND:"))) {
                                tree_cmake = line.section(QLatin1Char('='), 1);
                            }
                        }
                    }
                    break;
                }
                if (!up.cdUp()) {
                    break;
                }
            }
        }

        const QString cmake = QFile::exists(tree_cmake)
                                  ? tree_cmake
                                  : QStandardPaths::findExecutable(QStringLiteral("cmake"));
        if (build_tree.isEmpty() || source_tree.isEmpty() || cmake.isEmpty()) {
            LOG_WARNING(Frontend,
                        "No suyu build tree found next to this executable — falling back to the "
                        "generic launcher; the export will use recompiled DLLs instead of a single "
                        "static executable");
        } else {
            status_label->setText(tr("Linking the single-file executable..."));
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

            QString conf_log;
            QString link_log;

            // suyu requires a newer CMake than most systems have first on PATH.
            // The VS-bundled CMake is what actually configured this tree (via
            // vcvars64.bat) even when CMakeCache.txt's own CMAKE_COMMAND
            // record points at an older system-wide install (that record
            // reflects whichever cmake first touched the cache, not
            // necessarily the one capable of building it now) - so search
            // VS-bundled installs FIRST and only fall back to the
            // cache/PATH ones after.
            QStringList cmake_candidates;
#ifdef _WIN32
            for (const auto& root : VisualStudioInstallRoots()) {
                cmake_candidates.append(
                    root + QStringLiteral("/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/"
                                          "bin/cmake.exe"));
            }
#endif
            cmake_candidates.append(cmake);
            const QString path_cmake = QStandardPaths::findExecutable(QStringLiteral("cmake"));
            if (!path_cmake.isEmpty()) {
                cmake_candidates.append(path_cmake);
            }
            cmake_candidates.removeDuplicates();

            // Reconfiguring the tree outside a Developer Command Prompt (as
            // this QProcess launch is) leaves cl.exe/link.exe off PATH and
            // INCLUDE/LIB unset, so CMake's compiler-id detection falls back
            // to a "GENERIC" architecture and later steps that need to know
            // the target platform (e.g. the bundled-OpenSSL fetch) reject it.
            // Capture vcvars64.bat's environment once and apply it to both
            // the configure and build subprocesses.
            QProcessEnvironment vs_env = QProcessEnvironment::systemEnvironment();
            {
#ifdef _WIN32
                const QString vcvars = FindVcVars64();
                if (vcvars.isEmpty()) {
                    LOG_WARNING(Frontend,
                                "no Visual Studio C++ toolset found - cl.exe and link.exe will "
                                "not be on PATH for this build, which fails compiler detection. "
                                "Set SUYU_VCVARS to a vcvars64.bat to override.");
                } else {
                    QProcess env_proc;
                    QString out;
                    // `set`'s output for a Developer Command Prompt easily exceeds
                    // the OS pipe buffer (hundreds of vars, a huge PATH) - same
                    // undrained-pipe deadlock as the build subprocesses, so this
                    // uses the same drain-while-waiting helper rather than a bare
                    // waitForFinished()+readAllStandardOutput() after the fact.
                    RunProcessDrained(env_proc, QStringLiteral("cmd.exe"),
                                       {QStringLiteral("/c"), QStringLiteral("call"), vcvars,
                                        QStringLiteral("&&"), QStringLiteral("set")},
                                       &out);
                    for (const auto& line : out.split(QStringLiteral("\n"))) {
                        const int eq = line.indexOf(QLatin1Char('='));
                        if (eq > 0) {
                            vs_env.insert(line.left(eq).trimmed(),
                                          line.mid(eq + 1).trimmed());
                        }
                    }
                }
#endif
            }

            QString cmake_exe;
            int conf_rc = -1;
            QProcess conf;
            conf.setProcessEnvironment(vs_env);
            for (const auto& candidate : cmake_candidates) {
                if (!QFile::exists(candidate)) {
                    continue;
                }
                conf_rc = RunProcessDrained(
                    conf, candidate,
                    {QStringLiteral("-S"), source_tree, QStringLiteral("-B"), build_tree,
                     QStringLiteral("-DSUYU_CMD_RECOMP_DIR=") +
                         QDir::fromNativeSeparators(recomp_root)},
                    &conf_log);
                if (conf_rc == 0) {
                    cmake_exe = candidate;
                    break;
                }
                // Each candidate reuses `conf`, so without this only the last
                // failure's output survives to the summary log below.
                LOG_WARNING(Frontend, "cmake candidate {} failed to configure (rc={}):\n{}",
                            candidate.toStdString(), conf_rc,
                            conf_log.right(3000).toStdString());
            }
            int link_rc = -1;
            if (conf_rc == 0) {
                QProcess bld;
                bld.setProcessEnvironment(vs_env);
                link_rc = RunProcessDrained(bld, cmake_exe,
                                            {QStringLiteral("--build"), build_tree,
                                             QStringLiteral("--target"),
                                             QStringLiteral("suyu-cmd-static"),
                                             QStringLiteral("--config"), QStringLiteral("Release"),
                                             QStringLiteral("--parallel")},
                                            &link_log);
                if (link_rc != 0) {
                    LOG_ERROR(Frontend, "suyu-cmd-static failed to link:\n{}",
                              link_log.right(4000).toStdString());
                }
            } else {
                LOG_ERROR(Frontend, "cmake could not configure the static launcher:\n{}",
                          conf_log.right(4000).toStdString());
            }

            if (link_rc == 0) {
                // The target lands wherever suyu-cmd does; look in the usual spots.
                const QStringList candidates = {
                    build_tree + QStringLiteral("/bin/suyu-cmd-static.exe"),
                    build_tree + QStringLiteral("/bin/Release/suyu-cmd-static.exe"),
                    build_tree + QStringLiteral("/bin/suyu-cmd-static"),
                    QCoreApplication::applicationDirPath() +
                        QStringLiteral("/suyu-cmd-static.exe"),
                    QCoreApplication::applicationDirPath() + QStringLiteral("/suyu-cmd-static"),
                };
                for (const auto& c : candidates) {
                    if (!QFile::exists(c)) {
                        continue;
                    }
                    const QString dst_dir = cache_dir + QDir::separator() +
                                            QStringLiteral("launcher");
                    QDir().mkpath(dst_dir);
                    const QString dst =
                        dst_dir + QDir::separator() + QStringLiteral("static_launcher.exe");
                    QFile::remove(dst);
                    if (QFile::copy(c, dst)) {
                        LOG_INFO(Frontend, "Built single-file launcher from {}", c.toStdString());
                    }
                    break;
                }
            }
        }
    }

    // One-command build scripts.
    {
        QFile bw(recomp_root + QDir::separator() + QStringLiteral("build_native_windows.cmd"));
        if (bw.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream o(&bw);
            o << "@echo off\r\n"
                 "rem Build all recompiled modules.\r\n"
                 "rem Find cmake: PATH first, then any Visual Studio install.\r\n"
                 "rem\r\n"
                 "rem vswhere ships with every VS installer since 2017, so this finds\r\n"
                 "rem Community, Professional and Build Tools of any year, on any drive.\r\n"
                 "rem A directory glob of one hardcoded year and edition did not.\r\n"
                 "set \"CMAKE=cmake\"\r\n"
                 "where cmake >nul 2>&1\r\n"
                 "if %errorlevel% equ 0 goto have_cmake\r\n"
                 "set \"VSWHERE=%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"\r\n"
                 "if not exist \"%VSWHERE%\" goto no_cmake\r\n"
                 "for /f \"usebackq tokens=*\" %%V in (`\"%VSWHERE%\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set \"CMAKE=%%V\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe\"\r\n"
                 "if not exist \"%CMAKE%\" goto no_cmake\r\n"
                 "echo Using Visual Studio cmake: %CMAKE%\r\n"
                 "goto have_cmake\r\n"
                 ":no_cmake\r\n"
                 "echo ERROR: cmake not found. Install CMake or Visual Studio.\r\n"
                 "pause & exit /b 1\r\n"
                 ":have_cmake\r\n"
                 "for /d %%M in (*) do (\r\n"
                 "  if exist \"%%M\\CMakeLists.txt\" (\r\n"
                 "    echo Building %%M ...\r\n"
                 "    \"%CMAKE%\" -S \"%%M\" -B \"%%M\\build\" && \"%CMAKE%\" --build \"%%M\\build\" --config Release\r\n"
                 "  )\r\n"
                 ")\r\n"
                 "echo Done.\r\n"
                 "pause\r\n";
            bw.close();
        }
        QFile bs(recomp_root + QDir::separator() + QStringLiteral("build_native_unix.sh"));
        if (bs.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream o(&bs);
            o << "#!/bin/sh\n"
                 "# Build all recompiled modules.\n"
                 "cmake -S . -B build && cmake --build build\n";
            bs.close();
        }
    }

    // Write block map files for each module (binary format, debugging only -
    // see the note by dump_debug_artifacts above).
    // Format per entry: [u32 vaddr][u32 size][u32 instruction_count][u32 flags]
    for (const auto& mod : module_results) {
        if (!dump_debug_artifacts) {
            break;
        }
        const QString map_path = blockmap_dir + QDir::separator() + mod.name +
                                 QStringLiteral(".blockmap");
        QFile map_file(map_path);
        if (map_file.open(QIODevice::WriteOnly)) {
            // Header: magic "AOTB", version, block_count
            const u32 magic = 0x42544F41; // "AOTB"
            const u32 version = 1;
            const u32 block_count = static_cast<u32>(mod.blocks.size());
            map_file.write(reinterpret_cast<const char*>(&magic), 4);
            map_file.write(reinterpret_cast<const char*>(&version), 4);
            map_file.write(reinterpret_cast<const char*>(&block_count), 4);

            size_t map_blocks_written = 0;
            for (const auto& block : mod.blocks) {
                if (++map_blocks_written % 1000 == 0) {
                    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                }
                map_file.write(reinterpret_cast<const char*>(&block.vaddr), 4);
                map_file.write(reinterpret_cast<const char*>(&block.size), 4);
                map_file.write(reinterpret_cast<const char*>(&block.instruction_count), 4);
                u32 flags = block.is_entry ? 1u : 0u;
                map_file.write(reinterpret_cast<const char*>(&flags), 4);
            }
            map_file.close();
        }
    }

    // The raw modules were only read to translate them; no copy of the game's own
    // executables stays in the project or the package.
    if (!QDir(recomp_root + QStringLiteral("/nso")).removeRecursively()) {
        LOG_ERROR(Frontend, "Could not remove the analysed modules from the AOT cache");
        return {};
    }

    const auto current_patches = BakedPatchManifest(patch_modules, patch_manager);
    if (!current_patches || *current_patches != *baked_patches) {
        LOG_ERROR(Frontend, "Enabled NSO patches changed during export; re-export with unchanged mods");
        return {};
    }

    // Write the final fallback outcome, including compile-time module failures.
    last_fallback_modules = fallback_modules;
    QJsonArray fallback_array;
    for (const auto& module : fallback_modules) {
        fallback_array.append(module);
    }

    // Write the AOT manifest with real analysis results
    QFile manifest(manifest_path);
    if (manifest.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&manifest);
        out << "{\n";
        out << "  \"version\": 2,\n";
        out << "  \"image_abi\": " << (suyu::recomp::g_emit_fastmem ? 6 : 5) << ",\n";
        if (suyu::recomp::g_emit_fastmem) {
            out << "  \"image_features\": " << image_features << ",\n";
        }
        out << "  \"correctness_revision\": \"20261010-graphics-safe-defaults-r3\",\n";
        if (!patch_fingerprint.isEmpty()) {
            out << "  \"baked_patches\": " << QString::fromUtf8(
                QJsonDocument(*baked_patches).toJson(QJsonDocument::Compact)) << ",\n";
        }
        out << "  \"source_exefs_sha256\": \"" << source_hash << "\",\n";
        out << "  \"translate_all\": " << (translate_all ? "true" : "false") << ",\n";
        out << "  \"requested_backend\": \"" << requested_backend_name << "\",\n";
        out << "  \"effective_backend\": \"" << effective_backend_name << "\",\n";
        out << "  \"full_scan\": " << (full_scan ? "true" : "false") << ",\n";
        out << "  \"fallback_enabled\": " << (fallback_enabled ? "true" : "false") << ",\n";
        out << "  \"fallback_modules\": "
            << QString::fromUtf8(QJsonDocument(fallback_array).toJson(QJsonDocument::Compact))
            << ",\n";
        out << "  \"total_modules\": " << module_results.size() << ",\n";
        // Recorded coverage gaps fed back as block roots (recomp_gaps.json).
        out << "  \"coverage_roots\": " << coverage_roots_used << ",\n";
        out << "  \"coverage_modules_ignored\": " << coverage_modules_ignored << ",\n";
        out << "  \"coverage_fingerprint\": \"" << coverage_fingerprint << "\",\n";
        out << "  \"total_blocks_analyzed\": " << total_blocks << ",\n";
        out << "  \"total_instructions\": " << total_instructions << ",\n";
        out << "  \"total_text_bytes\": " << total_text_bytes << ",\n";
        out << "  \"ir_blocks_serialized\": " << total_ir_blocks << ",\n";
        out << "  \"ir_translation_failures\": " << total_ir_failures << ",\n";
        out << "  \"host_machine_code_blocks\": 0,\n";
        out << "  \"recompiled_c_blocks\": " << recomp_total_blocks << ",\n";
        out << "  \"recompiled_project\": \"recompiled/<module>/ (buildable C, cross-platform CMake; "
               "generated units in <module>/src, build output in <module>/build)\",\n";
        out << "  \"native_build_scripts\": [\"recompiled/build_native_windows.cmd\", \"recompiled/build_native_unix.sh\"],\n";
        if (!last_recomp_compiler.isEmpty()) {
            // Through QJsonArray for the escaping; the path has backslashes.
            const QString quoted = QString::fromUtf8(
                QJsonDocument(QJsonArray{last_recomp_compiler}).toJson(QJsonDocument::Compact));
            out << "  \"recomp_compiler\": " << quoted.mid(1, quoted.size() - 2) << ",\n";
        }
        out << "  \"requires_runtime_codegen\": false,\n";
        out << "  \"modules\": [\n";
        for (size_t i = 0; i < module_results.size(); ++i) {
            const auto& mod = module_results[i];
            out << "    {\n";
            out << "      \"name\": \"" << mod.name << "\",\n";
            out << "      \"build_id\": \"" << mod.build_id_hex << "\",\n";
            out << "      \"text_vaddr\": " << mod.text_vaddr << ",\n";
            out << "      \"text_size\": " << mod.text_size << ",\n";
            out << "      \"rodata_vaddr\": " << mod.rodata_vaddr << ",\n";
            out << "      \"rodata_size\": " << mod.rodata_size << ",\n";
            out << "      \"data_vaddr\": " << mod.data_vaddr << ",\n";
            out << "      \"data_size\": " << mod.data_size << ",\n";
            out << "      \"blocks\": " << mod.total_blocks << ",\n";
            out << "      \"instructions\": " << mod.total_instructions << ",\n";
            out << "      \"project_directory\": \"recompiled/" << mod.name << "\",\n";
            out << "      \"sources_directory\": \"recompiled/" << mod.name << "/src\"";
            if (dump_debug_artifacts) {
                out << ",\n      \"blockmap_file\": \"debug/blockmaps/" << mod.name
                    << ".blockmap\",\n";
                out << "      \"ir_directory\": \"debug/ir/" << mod.name << "\",\n";
                out << "      \"guest_code_directory\": \"debug/code/" << mod.name << "\"";
            }
            out << "\n";
            out << "    }" << (i + 1 < module_results.size() ? "," : "") << "\n";
        }
        out << "  ],\n";
             out << "  \"comment\": \"suyu static recompiler export. The in-tree AArch64-to-C "
                 "recompiler emits buildable native source and module metadata.\"\n";
        out << "}\n";
        manifest.close();
    }

    return cache_dir;
}

// ---------------------------------------------------------------------------
// Standalone packaging
// ---------------------------------------------------------------------------

// Where the AOT cache lives inside a finished package, per target platform.
// PackageNativeExport lays the package out; this has to agree with it exactly,
// because the export now generates the cache directly at this path instead of
// staging it elsewhere and copying it in.
static QString AotCacheDirFor(const QString& output_dir, const QString& game_name,
                              GameExportDialog::TargetPlatform platform) {
    switch (platform) {
    case GameExportDialog::TargetPlatform::Linux:
        return output_dir + QDir::separator() + game_name + QStringLiteral(".AppDir") +
               QDir::separator() + QStringLiteral("usr/bin") + QDir::separator() +
               QStringLiteral("aot_cache");
    case GameExportDialog::TargetPlatform::MacOS:
        return output_dir + QDir::separator() + game_name + QStringLiteral(".app") +
               QDir::separator() + QStringLiteral("Contents") + QDir::separator() +
               QStringLiteral("Resources") + QDir::separator() + QStringLiteral("aot_cache");
    case GameExportDialog::TargetPlatform::Windows:
    default:
        return output_dir + QDir::separator() + game_name + QDir::separator() +
               QStringLiteral("aot_cache");
    }
}

bool GameExportDialog::PackageNativeExport(const QString& rom_path, const QString& cache_dir,
                                           const QString& output_dir, const QString& game_name,
                                           TargetPlatform platform) {
    // The original ROM used to be copied into every package. It is not needed
    // there: the recompiled project carries the guest segments it executes in
    // <module>/data, and nothing in the package or in suyu ever opened the copy.
    // All it did was add the ROM's full size - several gigabytes for an XCI - to
    // an output that is otherwise tens of megabytes of C. The source is recorded
    // by path instead, so the export can still be traced back to it.
    const auto write_source_reference = [&rom_path](const QString& dir) {
        QFile ref(dir + QDir::separator() + QStringLiteral("game_source.txt"));
        if (!ref.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return;
        }
        QTextStream out(&ref);
        out << "Recompiled from: " << QDir::toNativeSeparators(rom_path) << "\n"
            << "The ROM is referenced, not bundled - the generated project runs from the guest\n"
            << "segments in aot_cache/recompiled/<module>/data and does not read this file.\n";
        ref.close();
    };

    switch (platform) {
    case TargetPlatform::Windows: {
        const QString pkg_dir = package_root;
        if (!QDir().mkpath(pkg_dir)) {
            return false;
        }

        // A prior export into this same folder may have been "Build" format
        // and left a compiled launcher exe + its DLLs sitting in pkg_dir. If
        // this run is "Source" (or otherwise not producing a compiled
        // launcher), that stale exe is never touched by anything below - it
        // just sits there looking like part of the new export. Strip it so a
        // format switch on the same output folder doesn't leave orphaned
        // binaries next to freshly generated C source.
        if (!WantsCompiledOutput()) {
            QFile::remove(pkg_dir + QDir::separator() + game_name + QStringLiteral(".exe"));
            for (const char* dll : {"avcodec-61.dll", "avformat-61.dll", "avutil-59.dll",
                                     "dxcompiler.dll", "dxil.dll", "libcrypto.dll", "libssl.dll",
                                     "swresample-5.dll", "swscale-8.dll"}) {
                QFile::remove(pkg_dir + QDir::separator() + QString::fromLatin1(dll));
            }
        }

        // ── Extract exefs (NSO executables) → <pkg>/exefs/ ──────────────────────
        // This gives the package the same structure as Switch ROM viewers show.
        // suyu-cmd auto-detects exefs/main next to it and loads from there.
        const QString exefs_dst = pkg_dir + QStringLiteral("/exefs");
        auto exefs_vdir = ExtractExeFsFromRom(rom_path.toStdString());
        if (exefs_vdir) {
            DumpVirtualDir(exefs_vdir, exefs_dst);
        }

        // ── Extract romfs → <pkg>/exefs/romfs.bin ────────────────────────────────
        // DeconstructedRomDirectory loader looks for a .romfs file alongside the NSOs.
        // Romfs can be several GB — only extract if VFS gives it without copying the file.
        if (exefs_vdir) {
            auto romfs_vf = ExtractRomFsFromRom(rom_path.toStdString());
            if (romfs_vf) {
                // Always extract the decrypted romfs into the package so the
                // export is genuinely standalone: no original ROM file and no
                // keys required at runtime (only at export time, on this
                // machine, to read the source ROM once). Streamed in chunks -
                // romfs can be many GB and ReadAllBytes() would double the
                // package's peak memory use for no benefit.
                QFile rf(exefs_dst + QStringLiteral("/romfs.bin"));
                if (rf.open(QIODevice::WriteOnly)) {
                    constexpr u64 kChunk = 64ULL * 1024 * 1024;
                    const u64 total = romfs_vf->GetSize();
                    for (u64 off = 0; off < total; off += kChunk) {
                        const u64 len = std::min(kChunk, total - off);
                        const auto bytes = romfs_vf->ReadBytes(len, off);
                        rf.write(reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<qint64>(bytes.size()));
                    }
                    rf.close();
                }
            }
        }

        // Bundle suyu-cmd.exe (renamed to the game name) and its runtime DLLs so the
        // package runs standalone — suyu-cmd provides the HLE+GPU+audio stack.
        const QString bin_dir = QCoreApplication::applicationDirPath();
        // Prefer the per-game build that has this game's recompiled modules
        // linked in: one file, no recompiled_*.dll beside it. The generic
        // suyu-cmd is the fallback for source-only exports and for machines
        // where the static link could not be produced.
        const QString static_launcher =
            cache_dir + QStringLiteral("/launcher/static_launcher.exe");
        const bool has_static_launcher = QFile::exists(static_launcher);

        // The generated C source and per-module build trees under aot_cache/
        // are compile-time-only: once the static launcher exists, everything
        // this package needs to run is already linked into that one exe.
        // Shipping the source tree alongside it (often hundreds of MB, plus
        // it visually screams "this is an emulator with a debug build in
        // it" rather than a native game) only makes sense for Source-format
        // exports, where the user asked for the C project instead of a
        // compiled binary.
        if (!has_static_launcher) {
            write_source_reference(pkg_dir);
            if (!CopyDirectoryUnlessInPlace(
                    cache_dir, pkg_dir + QDir::separator() + QStringLiteral("aot_cache"))) {
                return false;
            }
        }
        // has_static_launcher's aot_cache cleanup happens further down, after
        // static_launcher.exe has been copied out of it to its final path -
        // deleting cache_dir here would remove that file before the copy runs.
        // A Build export must never silently downgrade to the generic emulator.
        // That produces the misleading "game.exe + ROM" bundle which still
        // depends on recompiled DLLs (or falls back to JIT), rather than the
        // self-contained executable promised by this export mode.
        if (WantsCompiledOutput() && !has_static_launcher) {
            LOG_ERROR(Frontend, "Static recompiled launcher was not produced: {}",
                      static_launcher.toStdString());
            // Built explicitly rather than via QMessageBox::critical so the text
            // format can be pinned to plain text — Qt's auto rich-text detection
            // otherwise renders parts of the message as a styled/highlighted block.
            QMessageBox box(this);
            box.setIcon(QMessageBox::Critical);
            box.setWindowTitle(tr("Export Failed"));
            box.setTextFormat(Qt::PlainText);
            box.setText(tr("The static recompiled executable was not produced."));
            box.setInformativeText(
                tr("The export was stopped instead of packaging the generic emulator launcher. "
                   "Check the build log and ensure the recompiler modules compiled successfully."));
            box.setStandardButtons(QMessageBox::Ok);
            box.exec();
            return false;
        }
        const QString launcher_src = has_static_launcher
                                         ? static_launcher
                                         : bin_dir + QStringLiteral("/suyu-cmd.exe");
        const QString launcher_dst = pkg_dir + QDir::separator() + game_name + QStringLiteral(".exe");
        if (QFile::exists(launcher_src)) {
            QFile::remove(launcher_dst);
            QFile::copy(launcher_src, launcher_dst);

            // Embed the game's icon into the launcher exe via Windows resource update API.
            if (!game_icon_.isNull()) {
#ifdef _WIN32
                // Scale icon to 256x256 for best Explorer display quality
                const QPixmap icon256 = game_icon_.scaled(256, 256,
                    Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                    .copy(0, 0, 256, 256);
                const QByteArray icon_dib = MakeIconDib(icon256);
                if (!icon_dib.isEmpty()) {
#pragma pack(push,1)
                    struct GrpEntry { BYTE w,h,cc,res; WORD pl,bpp; DWORD sz; WORD id; };
                    struct GrpDir  { WORD reserved,type,count; GrpEntry e[1]; };
#pragma pack(pop)
                    GrpDir grp{};
                    grp.type=1; grp.count=1;
                    // w=h=0 signals 256x256 in ICO/GRPICONDIR convention
                    grp.e[0].w=0; grp.e[0].h=0;
                    grp.e[0].pl=1; grp.e[0].bpp=32;
                    grp.e[0].sz=(DWORD)icon_dib.size(); grp.e[0].id=1;
                    const std::wstring dstW = launcher_dst.toStdWString();
                    // FALSE = keep existing resources (manifests, version info, etc.)
                    HANDLE h = BeginUpdateResourceW(dstW.c_str(), FALSE);
                    if (h) {
                        const bool icon_ok = UpdateResourceW(h, RT_ICON, MAKEINTRESOURCEW(1),
                            MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                            (LPVOID)icon_dib.data(), (DWORD)icon_dib.size()) != FALSE;
                        const bool group_ok = UpdateResourceW(h, RT_GROUP_ICON, MAKEINTRESOURCEW(1),
                            MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                            (LPVOID)&grp, (DWORD)(sizeof(WORD)*3 + sizeof(GrpEntry)));
                        if (!icon_ok || !group_ok || !EndUpdateResourceW(h, FALSE)) {
                            LOG_WARNING(Frontend, "Failed to embed game icon in {}",
                                        launcher_dst.toStdString());
                        }
                    }
                }
#endif
            }

            // DLLs required by suyu-cmd (FFmpeg, DXC, OpenSSL — SDL3 is statically linked)
            static constexpr const char* kRuntimeDlls[] = {
                "avcodec-61.dll", "avformat-61.dll", "avutil-59.dll",
                "swresample-5.dll", "swscale-8.dll",
                "dxcompiler.dll", "dxil.dll",
                "libcrypto-3-x64.dll", "libssl-3-x64.dll",
                // fallback names used by some builds
                "libcrypto.dll", "libssl.dll",
            };
            for (const char* dll : kRuntimeDlls) {
                const QString src = bin_dir + QLatin1Char('/') + QLatin1String(dll);
                if (QFile::exists(src)) {
                    const QString dst = pkg_dir + QDir::separator() + QLatin1String(dll);
                    QFile::remove(dst);
                    QFile::copy(src, dst);
                }
            }
        }

        // No -g argument: the exe auto-detects exefs/main (and romfs.bin
        // alongside it) next to itself, both already extracted and decrypted
        // into this package above. Passing the original ROM path here would
        // bypass that and force suyu-cmd to re-open the encrypted source ROM
        // instead - needing keys and the original file present, exactly what
        // bundling exefs/romfs was meant to avoid.
        QFile bat(pkg_dir + QDir::separator() + QStringLiteral("launch.bat"));
        if (bat.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&bat);
            out << "@echo off\n";
            out << "\"" << game_name << ".exe\"\n";
            bat.close();
        }

        // cache_dir is pkg_dir/aot_cache itself (RunAotPrecompile generates
        // straight into the package, it was never copied in from elsewhere),
        // so once static_launcher.exe has been copied out to the package
        // root above, the whole generated-C build tree - per-module source,
        // object files, .lib artifacts, often several GB - is now dead
        // weight sitting in what's supposed to be a tidy, standalone game
        // folder. Delete it; a repeat export just recompiles (fast, thanks
        // to the /O1 + /MP codegen flags) rather than reusing this cache.
        if (has_static_launcher) {
            QDir(cache_dir).removeRecursively();
        } else {
            QDir(pkg_dir + QStringLiteral("/aot_cache/launcher")).removeRecursively();
        }

        // Mods/patches live beside the exe (see SetSuyuPath(LoadDir) in
        // suyu_cmd/suyu.cpp); create it so the layout is discoverable.
        QDir().mkpath(pkg_dir + QStringLiteral("/mods"));

        QFile readme(pkg_dir + QDir::separator() + QStringLiteral("README_NATIVE_EXPORT.txt"));
        if (readme.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&readme);
            out << "Recompiled native build — fully standalone, no ROM or keys needed to run\n\n";
            out << "Run: double-click launch.bat (or " << game_name << ".exe directly)\n\n";
            out << "This is the game itself, statically recompiled to x86 machine code and\n";
            out << "linked into " << game_name << ".exe alongside suyu's HLE/GPU/audio backend\n";
            out << "- no emulator install and no separate DLLs for the game code.\n\n";
            out << "What runs native vs emulated:\n";
            out << "- Native  : the game's own CPU code, translated ahead of time to C and\n";
            out << "            compiled into this exe. No instruction decoding at run time.\n";
            out << "- Emulated: system calls, OS services (filesystem, input, audio, sockets)\n";
            out << "            and the GPU, all served by suyu's HLE backend built into the\n";
            out << "            same exe. A console game cannot run without these.\n";
            out << "- Fallback: a small interpreter covers the few instructions the static\n";
            out << "            recompiler cannot translate yet (mostly rare SIMD forms) and\n";
            out << "            code only reachable through computed branches. It runs on\n";
            out << "            demand and hands control straight back; correctness never\n";
            out << "            depends on how much of the program it covers.\n\n";
            out << "Contents:\n";
            out << "- " << game_name << ".exe : the game (recompiled code + HLE/GPU backend, one file)\n";
            out << "- launch.bat      : one-click launcher\n";
            out << "- exefs/          : the game's own executables and data, extracted once at\n";
            out << "                    export time so no ROM or decryption keys are needed to run\n";
            out << "- *.dll           : runtime libraries (FFmpeg, Vulkan, OpenSSL)\n";
            out << "- mods/           : optional; drop <title_id>/<mod name>/ folders here\n";
            out << "- user/           : this game's own config, saves, and logs (not suyu's)\n\n";
            out << "Press F12 in-game for the debug panel (status, mods, folders).\n";
            readme.close();
        }

        MaybeAddToSteam(game_name, launcher_dst);

        return true;
    }

    case TargetPlatform::Linux: {
        const QString appdir = package_root;
        const QString bin_dir = appdir + QStringLiteral("/usr/bin");
        if (!QDir().mkpath(bin_dir)) {
            return false;
        }

        write_source_reference(bin_dir);

        // Copy AOT cache
        if (!CopyDirectoryUnlessInPlace(cache_dir,
                                    bin_dir + QDir::separator() + QStringLiteral("aot_cache"))) {
            return false;
        }

        QFile readme(appdir + QDir::separator() + QStringLiteral("README_NATIVE_EXPORT.txt"));
        if (readme.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&readme);
            out << "suyu local game export: recompiler artifacts\n\n";
            out << "Compiler artifacts are under usr/bin/aot_cache.\n";
            out << "No frontend runtime binary is bundled in this export.\n\n";
            out << kExportContentNotice;
            readme.close();
        }

        return true;
    }

    case TargetPlatform::MacOS: {
        const QString app_bundle = package_root;
        const QString contents_dir = app_bundle + QStringLiteral("/Contents");
        const QString res_dir = contents_dir + QStringLiteral("/Resources");
        if (!QDir().mkpath(contents_dir) || !QDir().mkpath(res_dir)) {
            return false;
        }

        write_source_reference(res_dir);

        // Copy AOT cache
        if (!CopyDirectoryUnlessInPlace(cache_dir,
                                    res_dir + QDir::separator() + QStringLiteral("aot_cache"))) {
            return false;
        }

        QFile readme(contents_dir + QDir::separator() + QStringLiteral("README_NATIVE_EXPORT.txt"));
        if (readme.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&readme);
            out << "suyu local game export: recompiler artifacts\n\n";
            out << "Compiler artifacts are under Contents/Resources/aot_cache.\n";
            out << "No frontend runtime binary is bundled in this export.\n\n";
            out << kExportContentNotice;
            readme.close();
        }

        return true;
    }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Locating already-built standalone recompiled executables
// ---------------------------------------------------------------------------

namespace {
constexpr char kOutputRootsKey[] = "recompile/output_roots";
} // namespace

QStringList GameExportDialog::RecompileOutputRoots() {
    QSettings settings(QStringLiteral("suyu"), QStringLiteral("suyu"));
    QStringList roots = settings.value(QString::fromLatin1(kOutputRootsKey)).toStringList();

    // The dialog defaults to Downloads, and the test harness writes into an
    // aot_test_output next to the working directory, so both are worth
    // checking even before the user has ever completed an export.
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (!downloads.isEmpty()) {
        roots.append(downloads);
    }
    roots.append(QDir::currentPath() + QDir::separator() + QStringLiteral("aot_test_output"));

    // In a development tree suyu runs out of build/bin, so the export root the
    // test harness writes to sits a couple of levels above the executable.
    QDir up(QCoreApplication::applicationDirPath());
    for (int level = 0; level < 4; ++level) {
        roots.append(up.absoluteFilePath(QStringLiteral("aot_test_output")));
        if (!up.cdUp()) {
            break;
        }
    }

    roots.removeDuplicates();
    return roots;
}

void GameExportDialog::RememberOutputRoot(const QString& dir) {
    if (dir.isEmpty()) {
        return;
    }
    QSettings settings(QStringLiteral("suyu"), QStringLiteral("suyu"));
    QStringList roots = settings.value(QString::fromLatin1(kOutputRootsKey)).toStringList();
    roots.removeAll(dir);
    roots.prepend(dir);
    while (roots.size() > 8) {
        roots.removeLast();
    }
    settings.setValue(QString::fromLatin1(kOutputRootsKey), roots);
}

QStringList GameExportDialog::FindRecompiledExecutables(const QString& game_name,
                                                        const QString& rom_path) {
    // The export directory is named after the ROM's base name, which for a
    // library entry is usually but not always the display title.
    QStringList names;
    if (!game_name.isEmpty()) {
        names.append(game_name);
    }
    if (!rom_path.isEmpty()) {
        const QString base = QFileInfo(rom_path).completeBaseName();
        if (!base.isEmpty()) {
            names.append(base);
        }
    }
    names.removeDuplicates();

    // A package can be laid out for any of the three target platforms, and the
    // build directory is single- or multi-config depending on the generator.
    static const QStringList package_suffixes = {
        QStringLiteral("/aot_cache/recompiled"),
        QStringLiteral(".AppDir/usr/bin/aot_cache/recompiled"),
        QStringLiteral(".app/Contents/Resources/aot_cache/recompiled"),
    };
    static const QStringList exe_candidates = {
        QStringLiteral("build/Release/recompiled.exe"),
        QStringLiteral("build/Debug/recompiled.exe"),
        QStringLiteral("build/recompiled.exe"),
        QStringLiteral("build/recompiled"),
    };

    QStringList found;
    for (const QString& root : RecompileOutputRoots()) {
        for (const QString& name : names) {
            // New layout: <root>/<GameName>/<GameName>.exe (suyu-cmd launcher)
            const QString pkg_launcher = root + QDir::separator() + name +
                                         QDir::separator() + name +
#ifdef _WIN32
                                         QStringLiteral(".exe");
#else
                                         QString{};
#endif
            if (QFile::exists(pkg_launcher)) {
                found.prepend(QDir::toNativeSeparators(pkg_launcher));
            }

            // Legacy layout: deep in aot_cache/recompiled/<module>/build/
            for (const QString& suffix : package_suffixes) {
                const QString recomp_root = root + QDir::separator() + name + suffix;
                QDir dir(recomp_root);
                if (!dir.exists()) {
                    continue;
                }
                const QStringList modules =
                    dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
                for (const QString& mod : modules) {
                    for (const QString& rel : exe_candidates) {
                        const QString exe =
                            recomp_root + QDir::separator() + mod + QDir::separator() + rel;
                        const QFileInfo info(exe);
                        if (info.exists() && info.isFile()) {
                            found.append(QDir::toNativeSeparators(info.absoluteFilePath()));
                            break;
                        }
                    }
                }
            }
        }
    }
    found.removeDuplicates();
    return found;
}

// ---------------------------------------------------------------------------
// Main export entry point
// ---------------------------------------------------------------------------

void GameExportDialog::closeEvent(QCloseEvent* event) {
    if (export_in_progress) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void GameExportDialog::reject() {
    if (export_in_progress) {
        return;
    }
    QDialog::reject();
}

namespace {
// Holds `flag` for its lifetime and, on Windows, tells the OS the machine is
// busy. A Build export runs for tens of minutes with no user input, so the
// idle timer would otherwise be free to sleep the system out from under the
// child compilers mid-build. ES_DISPLAY_REQUIRED is deliberately not set: the
// screen may blank, only sleep is held off.
class ExportRunGuard {
public:
    explicit ExportRunGuard(bool& flag) : flag_{flag} {
        flag_ = true;
#ifdef _WIN32
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
#endif
    }
    ~ExportRunGuard() {
        flag_ = false;
#ifdef _WIN32
        SetThreadExecutionState(ES_CONTINUOUS);
#endif
    }
    ExportRunGuard(const ExportRunGuard&) = delete;
    ExportRunGuard& operator=(const ExportRunGuard&) = delete;

private:
    bool& flag_;
};
} // namespace

/// Direct-from-ROM icon+title fallback for when the export ran without ever
/// matching a scanned library entry - a freshly downloaded ROM the library
/// hasn't indexed yet, or one outside the configured game directories
/// entirely. Without this, such an export silently kept the default suyu
/// icon and fell back to the filename for its title, since every other path
/// to a name/icon (OnSelectFromLibrary, the library_entries_ match in
/// OnExport) depends on the game already being in the scanned library.
/// Mirrors GameLibraryWorker::GetGameIcon (src/suyu/game_library.cpp).
static bool ReadIconAndTitleFromRom(Core::System& system, const QString& rom_path,
                                    QPixmap& out_icon, QString& out_title) {
    static const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    auto file = vfs->OpenFile(rom_path.toStdString(), FileSys::OpenMode::Read);
    if (!file) {
        return false;
    }
    auto loader = Loader::GetLoader(system, file);
    if (!loader) {
        return false;
    }
    bool got_anything = false;
    std::vector<u8> icon_data;
    if (loader->ReadIcon(icon_data) == Loader::ResultStatus::Success && !icon_data.empty()) {
        QPixmap pixmap;
        if (pixmap.loadFromData(icon_data.data(), static_cast<uint>(icon_data.size())) &&
            !pixmap.isNull()) {
            out_icon = pixmap;
            got_anything = true;
        }
    }
    std::string title;
    if (loader->ReadTitle(title) == Loader::ResultStatus::Success && !title.empty()) {
        out_title = QString::fromStdString(title);
        got_anything = true;
    }
    return got_anything;
}

void GameExportDialog::OnExport() {
    // Re-entry would run two exports over one cache directory. See the comment
    // on export_in_progress: processEvents() inside the export can deliver an
    // automation RPC that calls straight back in here.
    if (export_in_progress) {
        LOG_WARNING(Frontend, "Export already in progress; ignoring re-entrant request");
        return;
    }
    const ExportRunGuard run_guard{export_in_progress};

    const QString rom_path = rom_path_edit->text();
    const QString output_dir = output_path_edit->text();

    if (rom_path.isEmpty()) {
        QMessageBox::warning(this, tr("No ROM"), tr("Please set a ROM path first."));
        return;
    }
    if (output_dir.isEmpty()) {
        QMessageBox::warning(this, tr("No Output"),
                             tr("Please select an output directory first."));
        return;
    }
    if (!QFile::exists(rom_path)) {
        QMessageBox::warning(this, tr("ROM Not Found"),
                             tr("The specified ROM file does not exist."));
        return;
    }

#ifdef _WIN32
    // Windows refuses paths past 260 characters unless long paths are enabled,
    // and the build scripts this export writes nest deeper than anything else
    // in the package:
    //   <out>\<game> - Hybrid AOT + JIT\aot_cache\exefs\main\build\CMakeFiles\<target>
    //   CMakeScratch\TryCompile-xxxxxx\cmTC_xxxxx.dir\Debug\cmTC_xxxxx.tlog\<file>
    //   link-cvtres.write.1.tlog
    // which lands roughly 190 characters below the output directory. Over the
    // limit, MSBuild fails with "FTK1011: could not create the new file
    // tracking log file" - which never mentions path length, and sends people
    // looking for a compiler bug instead of a shorter folder.
    {
        constexpr int kMaxPath = 260;
        constexpr int kGeneratedTail = 190;
        constexpr int kBudget = kMaxPath - kGeneratedTail;
        if (output_dir.length() > kBudget) {
            const auto answer = QMessageBox::warning(
                this, tr("Output Path May Be Too Long"),
                tr("The output directory is %1 characters long. The build scripts this "
                   "export writes nest about %2 characters below it, and Windows rejects "
                   "paths longer than %3 unless long paths are enabled. Compiling would "
                   "fail with \"FTK1011: could not create the new file tracking log "
                   "file\", which does not mention the path length.\n\n"
                   "Pick an output directory shorter than about %4 characters - "
                   "C:\\exports, for instance - and this cannot happen.\n\n"
                   "Export here anyway?")
                    .arg(output_dir.length())
                    .arg(kGeneratedTail)
                    .arg(kMaxPath)
                    .arg(kBudget),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                return;
            }
        }
    }
#endif

    // An exported game never contains firmware; it reads the firmware installed in suyu when
    // it runs. Without it, games still boot but anything built on firmware data - Mii
    // selection in Mario Kart 8 Deluxe, for one - can stop a strict static game thread.
    if (!test_driven_export && !FirmwareManager::CheckFirmwarePresence(system_)) {
        const auto answer = QMessageBox::warning(
            this, tr("System Firmware Not Installed"),
            tr("suyu has no system firmware installed.\n\n"
               "Exported games do not include firmware. They use the firmware installed in "
               "suyu on this computer. Without it, parts of a game that rely on firmware "
               "data, such as choosing a Mii, can fail.\n\n"
               "Install firmware with Tools > Install Firmware, then export again.\n\n"
               "Export anyway?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    // A fresh suyu install may have the game but not its update. Offer to install it here
    // rather than silently exporting the base version.
    if (!test_driven_export) {
        const UpdateState update_state = CurrentUpdateState();
        if (update_state == UpdateState::BundledDisabled) {
            // The ExeFS would come from the file's update and the RomFS from the base game,
            // which packaging refuses - after the whole compile. Stop before it starts.
            QMessageBox::warning(
                this, tr("Updates Turned Off"),
                tr("This game file includes an update, but updates are turned off for this "
                   "game. The export cannot mix the update's code with the base game's data.\n\n"
                   "Turn updates on in the game's Properties > Add-Ons, then export again."));
            return;
        }
        if (update_state == UpdateState::None || update_state == UpdateState::Disabled ||
            update_state == UpdateState::Unreadable) {
            QMessageBox box(QMessageBox::Question,
                            update_state == UpdateState::None ? tr("No Game Update")
                                                              : tr("Game Update Not Used"),
                            update_state == UpdateState::None
                                ? tr("No update is installed for this game, so the export "
                                     "will use the base game version.\n\nIf you have the "
                                     "update file (.nsp), install it now so the export uses "
                                     "the updated game.")
                            : update_state == UpdateState::Disabled
                                ? tr("Updates are turned off for this game, so the export will "
                                     "not use its update.\n\nTurn updates on in the game's "
                                     "Properties > Add-Ons to export the updated game.")
                                : tr("The installed update cannot be read, so the export will "
                                     "use the base game version.\n\nIt may need keys that are "
                                     "not installed, or be damaged. Reinstalling it may help."),
                            QMessageBox::NoButton, this);
            QPushButton* install_button = nullptr;
            if (update_state == UpdateState::None || update_state == UpdateState::Unreadable) {
                install_button =
                    box.addButton(tr("Install Update File..."), QMessageBox::AcceptRole);
            }
            QPushButton* base_button =
                box.addButton(tr("Export Without Update"), QMessageBox::DestructiveRole);
            box.addButton(QMessageBox::Cancel);
            box.setDefaultButton(QMessageBox::Cancel);
            box.exec();
            if (install_button && box.clickedButton() == install_button) {
                if (!PromptAndInstallUpdate()) {
                    return;
                }
            } else if (box.clickedButton() != base_button) {
                return;
            }
        }
    }

    const auto platform =
        static_cast<TargetPlatform>(platform_combo->currentData().toInt());
    const auto backend =
        static_cast<RecompileBackend>(backend_combo->currentData().toInt());
    const bool uses_aot = backend != RecompileBackend::Dynarmic;
    const bool is_hybrid = backend == RecompileBackend::Hybrid;
    if (!uses_aot && platform != TargetPlatform::Windows) {
        QMessageBox::warning(
            this, tr("Unsupported JIT Target"),
            tr("The Dynarmic comparison package is currently available for Windows targets only."));
        return;
    }
    const bool include_save_data = include_save_data_checkbox->isChecked();
    const bool include_shader_cache = include_shader_cache_checkbox->isChecked();
    const bool include_custom_config = include_custom_config_checkbox->isChecked();
    // Automation chooses the package type per export; otherwise the dialog's choice applies.
    const bool portable = test_driven_export && test_options_.package >= 0
                              ? test_options_.package == 1
                              : package_type_combo->currentIndex() == 1;
    // Read once: a Build can run for hours with the dialog responsive, and the ROM field or
    // these options may change meanwhile.
    const u64 export_program_id = SelectedProgramId();
    // The update this export is built from, read now for the same reason. Every package
    // loads a deconstructed ExeFS with no control data, so unless the package config
    // names a version the game reports 0 and "1.0.0" while the update's code runs.
    u32 update_version = 0;
    QString update_display;
    if (export_program_id != 0) {
        const UpdateState update_state =
            CurrentUpdateState(&update_display, nullptr, &update_version);
        if (update_state != UpdateState::Installed && update_state != UpdateState::Bundled) {
            update_version = 0;
            update_display.clear();
        }
    }
    const bool add_to_steam =
        steam_shortcut_checkbox->isEnabled() && steam_shortcut_checkbox->isChecked();
    const bool steam_replace = steam_replace_rom_checkbox->isChecked();
    const bool steam_wikipedia = steam_wikipedia_checkbox->isChecked();
    const bool show_in_discord = discord_checkbox->isChecked();
    const QFileInfo rom_info(rom_path);
    // Prefer the NACP/library title; fall back to filename if not found.
    QString game_name = rom_info.completeBaseName();
    bool matched_library_entry = false;
    for (const auto& entry : library_entries_) {
        if (QFileInfo(entry.path) == rom_info) {
            matched_library_entry = true;
            if (!entry.title.trimmed().isEmpty()) {
                game_name = entry.title.trimmed();
                // Strip characters that are illegal in Windows filenames
                static const QRegularExpression kIllegal(QStringLiteral("[\\\\/:*?\"<>|]"));
                game_name.replace(kIllegal, QStringLiteral("_"));
            }
            // OnSelectFromLibrary sets this already, but OnBrowseRom and a
            // hand-typed ROM path never do - only the "From Library" picker
            // called SetGameIcon, so any other way of choosing a ROM that
            // still matches a scanned library entry silently shipped with
            // suyu's own icon instead of the game's. Set it here too, once,
            // wherever the entry lookup already happens for the game name.
            if (!entry.icon.isNull()) {
                SetGameIcon(entry.icon);
            }
            break;
        }
    }
    // A ROM the library hasn't scanned yet (freshly downloaded, or outside
    // the configured game directories) never matches library_entries_ at
    // all, so the icon/title stayed at their defaults above. Read them
    // straight from the ROM's own control data instead of depending on the
    // library ever having indexed this file.
    if (!matched_library_entry || game_icon_.isNull()) {
        QPixmap rom_icon;
        QString rom_title;
        if (ReadIconAndTitleFromRom(system_, rom_path, rom_icon, rom_title)) {
            if (!rom_icon.isNull() && game_icon_.isNull()) {
                SetGameIcon(rom_icon);
            }
            if (!matched_library_entry && !rom_title.trimmed().isEmpty()) {
                game_name = rom_title.trimmed();
                static const QRegularExpression kIllegal(QStringLiteral("[\\\\/:*?\"<>|]"));
                game_name.replace(kIllegal, QStringLiteral("_"));
            }
        }
    }

    // Recorded before the run rather than after: even a half-finished export
    // leaves buildable output here, and this is how the library later finds it.
    RememberOutputRoot(output_dir);

    export_button->setEnabled(false);
    progress_bar->setVisible(true);
    progress_bar->setValue(0);
    status_label->setText(tr("Preparing AOT export..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    if (backend == RecompileBackend::Ballistic) {
        status_label->setText(tr("Ballistic export is not wired yet; using Dynarmic export artifacts for this run."));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    struct StagingCleanup {
        const PackagePolicy::Staging& staging;
        ~StagingCleanup() {
            std::string ignored;
            PackagePolicy::RemoveStaging(staging, &ignored);
        }
    } staging_cleanup{*staging};
    const QString staging_root = FromFsPath(staging->root);
    const QString work_dir = FromFsPath(staging->work);

    try {
    // Step 1: Create temporary working directories
    const QString work_dir = output_dir + QDir::separator() +
                             QStringLiteral(".aot_work_") + game_name;
    const QString exefs_work = work_dir + QDir::separator() + QStringLiteral("exefs");
    // The AOT cache is generated straight into the package it belongs in
    // rather than into the work area. It is by far the largest thing an export
    // produces - about 6 GB of C for Smash Ultimate - and staging it only to
    // copy it across meant writing 12 GB and holding both at once, which is
    // where large exports were failing during packaging.
    const QString cache_work = AotCacheDirFor(output_dir, game_name, platform);
    QDir().mkpath(exefs_work);
    QDir().mkpath(cache_work);

    // Step 2: ExeFS extraction is handled inside RunAotPrecompile via VFS.
    // For extracted directories, we copy them to the work area here.
    status_label->setText(tr("Scanning for ExeFS content..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    QFileInfo rom_fi(rom_path);
    if (rom_fi.isDir()) {
        const QString exefs_sub = rom_path + QDir::separator() + QStringLiteral("exefs");
        if (QDir(exefs_sub).exists()) {
            if (!CopyDirectoryRecursive(exefs_sub, exefs_work)) {
                throw std::runtime_error("Failed to copy ExeFS staging directory");
            }
        } else {
            if (!CopyDirectoryRecursive(rom_path, exefs_work)) {
                throw std::runtime_error("Failed to copy ROM directory into export staging area");
            }
        }
    }

    // Step 3: AOT pre-compilation
    status_label->setText(tr("Running AOT pre-compilation (%1)...")
                              .arg(backend == RecompileBackend::Ballistic
                                       ? QStringLiteral("Ballistic")
                                       : QStringLiteral("Dynarmic")));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    const QString cache_result = RunAotPrecompile(exefs_work, cache_work, backend, game_name);
    if (cache_result.isEmpty()) {
        status_label->setText(tr("AOT pre-compilation failed."));
        progress_bar->setValue(0);
        export_button->setEnabled(true);
        emit ExportFinished(false, {});
        return;
    }

    if (uses_aot) {
        // Step 2: ExeFS extraction is handled inside RunAotPrecompile via VFS.
        // An extracted folder is copied to the work area here, by role only.
        status_label->setText(tr("Scanning for ExeFS content..."));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        QFileInfo rom_fi(rom_path);
        if (rom_fi.isDir()) {
            const QString exefs_sub = rom_path + QStringLiteral("/exefs");
            const QString exefs_source = QDir(exefs_sub).exists() ? exefs_sub : rom_path;
            QString reason;
            if (!CopyDeconstructedExeFs(exefs_source, exefs_work, &reason) ||
                !NpdmMatchesTitle(exefs_work, export_program_id, &reason)) {
                finish_failed(tr("Export stopped: %1").arg(reason));
                return;
            }
        }

        // For packaged ROM files (NSP/XCI/NCA), the AOT step uses VFS to extract ExeFS directly.
        ReportStage(ExportStage::Extract, 0.1);
        status_label->setText(
            is_hybrid ? tr("Running AOT pre-compilation (suyu Hybrid JIT + AOT)...")
                      : tr("Running AOT pre-compilation: suyu static AOT (Experimental)..."));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        if (RunAotPrecompile(exefs_work, cache_work, backend, game_name).isEmpty()) {
            finish_failed(tr("AOT pre-compilation failed."));
            return;
        }
    }
    injected("precompile");
    // Step 4: Package native export artifacts
    status_label->setText(tr("Packaging native export artifacts..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    if (!PackageNativeExport(rom_path, cache_work, staging_root, game_name, platform, backend)) {
        finish_failed(tr("Packaging failed."));
        return;
    }
    progress_bar->setValue(75);

    // Step 5: Bundle portable data (save, shader, config)
    if ((include_save_data || include_shader_cache || include_custom_config) &&
        rom_program_id != 0) {
        status_label->setText(tr("Bundling portable data..."));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        // Determine the package root (platform-specific)
        QString pkg_root;
        switch (platform) {
        case TargetPlatform::Windows:
            pkg_root = output_dir + QDir::separator() + game_name;
            break;
        case TargetPlatform::Linux:
            pkg_root = output_dir + QDir::separator() + game_name + QStringLiteral(".AppDir");
            break;
        case TargetPlatform::MacOS:
            pkg_root = output_dir + QDir::separator() + game_name + QStringLiteral(".app");
            break;
        }

        if (!CopyPortableSupportData(rom_program_id, pkg_root, include_save_data,
                                     include_shader_cache, include_custom_config)) {
            throw std::runtime_error("Failed to bundle portable support data");
        }
    }
    ReportStage(ExportStage::Package, 0.9);

    // Step 5: Optional, title-scoped support data. Only what was selected for this
    // export is copied; nothing survives from an earlier package.
    if ((include_save_data || include_shader_cache) && export_program_id != 0) {
        status_label->setText(tr("Bundling portable data..."));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QString support_error;
        if (!CopyPortableSupportData(export_program_id, staging_root, include_save_data,
                                     include_shader_cache, &support_error)) {
            throw std::runtime_error("Failed to bundle portable support data: " +
                                     support_error.toStdString());
        }
    }
    ReportStage(ExportStage::Package, 0.95);

    // Deconstructed ExeFS has no update CNMT, so preserve an explicitly
    // selected application version in the portable CLI config. This keeps
    // the game's reported version aligned with the code/data in the export.
    if (platform == TargetPlatform::Windows) {
        u32 app_version = test_app_version != 0
                              ? test_app_version
                              : Settings::values.application_version_override.GetValue();
        QString display_version = !test_display_version.isEmpty()
                                      ? test_display_version
                                      : QString::fromStdString(Settings::values
                                                                    .application_display_version_override
                                                                    .GetValue());
        // With no explicit override, the version of the update the export was built from.
        // suyu-cmd applies these keys for every package type, JIT baseline included.
        if (app_version == 0 && display_version.isEmpty() &&
            (update_version != 0 || !update_display.isEmpty())) {
            app_version = update_version;
            display_version = update_display;
            LOG_INFO(Frontend, "Export: reporting the update's application version {} ({})",
                     app_version, display_version.toStdString());
        }
        const QString config_path = staging_root + QStringLiteral("/user/config/sdl2-config.ini");
        const QString config_dir =
            FromFsPath(Common::FS::GetSuyuPath(Common::FS::SuyuPath::ConfigDir));
        const QString custom_path =
            include_custom_config && export_program_id != 0
                ? config_dir + QStringLiteral("/custom/") +
                      QStringLiteral("%1").arg(export_program_id, 16, 16, QLatin1Char('0'))
                          .toUpper() +
                      QStringLiteral(".ini")
                : QString{};
        if (!SeedPortableConfig(config_path, config_dir + QStringLiteral("/qt-config.ini"),
                                custom_path)) {
            throw std::runtime_error("Could not write portable settings config");
        }
        if (!WritePortableVersionOverride(config_path, app_version, display_version)) {
            throw std::runtime_error("Could not write portable version config");
        }
        QStringList stray_settings;
        if (!PortableConfigIsClean(config_path, &stray_settings)) {
            throw std::runtime_error("The package config holds settings outside the allowlist: " +
                                     stray_settings.join(QStringLiteral(", ")).toStdString());
        }
        // Lets the package's missing keys/firmware dialog open this suyu's installer. A Source
        // export is built into a launcher on some other machine, where this path means nothing.
        const QString record_dir = QFileInfo(config_path).absolutePath();
        const QString install_record = record_dir + QStringLiteral("/suyu-install.txt");
        if (!uses_aot || WantsCompiledOutput()) {
            // One line per way of finding suyu again, tried in order: relative to this file,
            // for a package that stays put, then absolute, for one moved elsewhere on this
            // computer. Neither names the account: the relative line is left out when it
            // would pass through the profile folder, and the absolute one names the home
            // folder as %USERPROFILE% (%HOME% elsewhere), expanded when it is read. The
            // staging folder sits beside the final one, so the relative line holds for both.
            const QString suyu_path = QDir::cleanPath(QCoreApplication::applicationFilePath());
            const QString home = QDir::cleanPath(QDir::homePath());
            QStringList lines;
            const QString relative = QDir(record_dir).relativeFilePath(suyu_path);
            if (QDir::isRelativePath(relative) &&
                !relative.split(QLatin1Char('/')).contains(QDir(home).dirName(),
                                                           Qt::CaseInsensitive)) {
                lines.append(relative);
            }
#ifdef _WIN32
            const QString home_token = QStringLiteral("%USERPROFILE%");
            constexpr auto home_case = Qt::CaseInsensitive;
#else
            const QString home_token = QStringLiteral("%HOME%");
            constexpr auto home_case = Qt::CaseSensitive;
#endif
            lines.append(suyu_path.startsWith(home + QLatin1Char('/'), home_case)
                             ? home_token + suyu_path.mid(home.size())
                             : suyu_path);
            QSaveFile record(install_record);
            if (!QDir().mkpath(record_dir) || !record.open(QIODevice::WriteOnly) ||
                record.write(QDir::toNativeSeparators(lines.join(QLatin1Char('\n')))
                                 .toUtf8()
                                 .append('\n')) < 0 ||
                !record.commit()) {
                LOG_WARNING(Frontend, "Could not record the suyu installation in the package");
            }
        }
    }

    // The Discord switch for the package's launcher, which suyu-cmd reads at start. The
    // Wikipedia lookup made for it is handed to the Steam step, which needs the same page.
    // Written into the staged package, so it is validated with everything else.
    std::optional<WikipediaCover::CoverUrls> cover_urls;
    if (platform == TargetPlatform::Windows &&
        QFileInfo(staging_root + QLatin1Char('/') + export_name + QStringLiteral(".exe")).isFile()) {
        QString discord_cover;
        if (show_in_discord && !test_driven_export) {
            status_label->setText(tr("Looking up cover art for Discord..."));
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            QNetworkAccessManager network;
            QElapsedTimer clock;
            clock.start();
            cover_urls = WikipediaCover::FindCoverUrls(
                network, game_title, clock, 6000,
                QStringLiteral("suyu-game-export (Discord cover art)"));
            discord_cover = WikipediaCover::DiscordImageUrl(*cover_urls);
        }
        if (!WikipediaCover::WriteDiscordIni(staging_root, show_in_discord, discord_cover)) {
            LOG_WARNING(Frontend, "Could not write discord.ini in the package");
        }
    }

    const QString export_id = QString::fromStdString(staging->id);
    std::set<quint64> dlc_titles;
    for (const auto& source : seal_sources) {
        if (source.role == QStringLiteral("dlc")) {
            dlc_titles.insert(source.title_id);
        }
    }
    if (!WriteLicenseNotices(staging_root, uses_aot) ||
        !WriteExportManifest(staging_root, export_id, export_program_id, backend,
                             uses_aot && WantsCompiledOutput(), include_save_data,
                             include_shader_cache, include_custom_config, portable,
                             static_cast<int>(dlc_titles.size()))) {
        throw std::runtime_error("Could not write the package notices");
    }

    // The whole staged package is checked against what the exporter produces
    // before it is allowed to become the output.
    ReportStage(ExportStage::Package, 0.97, tr("Checking the package..."));
    PackagePolicy::LocalExportExpectation expect;
    expect.platform = platform == TargetPlatform::Linux   ? PackagePolicy::Platform::Linux
                      : platform == TargetPlatform::MacOS ? PackagePolicy::Platform::MacOS
                                                          : PackagePolicy::Platform::Windows;
    expect.package_name = export_name.toStdString();
    expect.title_id_hex =
        QStringLiteral("%1").arg(export_program_id, 16, 16, QLatin1Char('0')).toUpper().toStdString();
    expect.allow_aot_cache =
        uses_aot && (platform != TargetPlatform::Windows || !WantsCompiledOutput());
    for (const char* dll : kExportRuntimeDlls) {
        expect.runtime_dlls.emplace_back(dll);
    }
    expect.portable = portable;
    const auto findings = PackagePolicy::ValidateLocalExport(staging->root, expect);
    if (!findings.empty()) {
        for (const auto& finding : findings) {
            LOG_ERROR(Frontend, "Package check: {}: {} [{}]", finding.path, finding.detail,
                      finding.rule);
        }
        throw std::runtime_error("The package failed its content check: " +
                                 PackagePolicy::Describe(findings));
    }
    injected("validate");

    const auto promoted = PackagePolicy::Promote(*staging, ToFsPath(destination), conflict_policy);
    if (!promoted.ok) {
        test_export_conflict = promoted.conflict;
        finish_failed(promoted.conflict
                          ? tr("Export not saved: %1 (\"%2\").")
                                .arg(QString::fromStdString(promoted.error), destination_name)
                          : tr("Export failed: %1").arg(QString::fromStdString(promoted.error)));
        return;
    }
    const QString final_path = FromFsPath(promoted.final_path);
    const QString backup_note =
        promoted.backup_path.empty()
            ? QString{}
            : tr("\n\nThe previous export, with any saves played from it, was kept at:\n%1")
                  .arg(QDir::toNativeSeparators(FromFsPath(promoted.backup_path)));
    if (!test_driven_export) {
        RememberExportSource(export_id, rom_path);
    }
    ReportStage(ExportStage::Package, 1.0);

    const QString launcher_exe = final_path + QLatin1Char('/') + export_name + QStringLiteral(".exe");
    QString steam_note;
    if (!test_driven_export && add_to_steam && platform == TargetPlatform::Windows) {
        steam_note = MaybeAddToSteam(game_title, launcher_exe,
                                     !uses_aot    ? tr("suyu Dynarmic JIT")
                                     : is_hybrid ? tr("suyu Hybrid JIT + AOT")
                                                 : tr("suyu static AOT"),
                                     steam_replace, steam_wikipedia, cover_urls);
    }

    export_button->setEnabled(true);
    status_label->setText(tr("Export completed: %1").arg(final_path));
    emit ExportFinished(true, final_path);

    if (WantsCompiledOutput()) {
        QMessageBox::information(
            this, tr("AOT Export Complete"),
            tr("Game exported and compiled to a standalone executable at:\n%1\n\n"
               "The package contains:\n"
               "- %2.exe — the recompiled game, statically linked with suyu's HLE/GPU backend "
               "(no separate DLLs, no emulator installation required)\n"
               "- mods/ — drop patch/mod folders here\n"
               "- user/ — this export's own config, save data, and logs (independent of suyu's)\n"
               "- Runtime DLLs (FFmpeg, Vulkan, OpenSSL) alongside the exe\n\n"
               "Just run %2.exe.")
                .arg(final_path, game_name));
    } else {
        QMessageBox::information(
            this, tr("AOT Export Complete"),
            tr("Game exported as C source to:\n%1\n\n"
               "The package contains:\n"
               "- Recompiled C source (buildable standalone PC executable)\n"
               "- Runtime with save/load support (save_data/ directory next to exe)\n"
               "- Bundled data segments (text, rodata, data)\n"
               "- Build scripts for Windows (.cmd) and Unix (.sh)\n\n"
               "Run build_native_windows.cmd (or build_native_unix.sh) in aot_cache/recompiled/ to compile.\n"
               "The resulting executable runs independently — no emulator required.\n\n"
               "(Choose \"Build\" instead of \"Source\" as the Export Format to have suyu compile "
               "this for you automatically.)")
                .arg(final_path));
    }
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "Exception during game export: {}", e.what());
        finish_failed(tr("Export failed."));
        test_export_has_result = true;
        if (!test_driven_export) {
            QMessageBox::critical(this, tr("Export Failed"),
                                  tr("An error occurred during game export:\n%1")
                                      .arg(QString::fromUtf8(e.what())));
        }
    } catch (...) {
        LOG_ERROR(Frontend, "Unknown exception during game export");
        finish_failed(tr("Export failed."));
        test_export_has_result = true;
        if (!test_driven_export) {
            QMessageBox::critical(this, tr("Export Failed"),
                                  tr("An unexpected error occurred during game export."));
        }
    }
}
