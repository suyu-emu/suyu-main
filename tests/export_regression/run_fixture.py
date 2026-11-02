#!/usr/bin/env python3
"""Exercise the exporter's real ExeFS helpers with small synthetic files."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from compile_object import ROOT, cache_value, msvc_environment, resolve_build

SOURCE = ROOT / "src/suyu/game_export.cpp"


def extract(text: str, start: str, end: str) -> str:
    return text[text.index(start):text.index(end, text.index(start))]


def resolve_qt_root(build: Path, value: str | None) -> Path:
    if value:
        root = Path(value).resolve()
    else:
        core_dir = cache_value(build, "Qt6Core_DIR")
        if not core_dir:
            raise SystemExit("Pass --qt-root or set QT_ROOT; Qt6Core_DIR is absent from CMakeCache.txt")
        root = Path(core_dir).resolve().parents[2]
    if not (root / "include/QtCore/QCoreApplication").is_file() or not (
        root / "lib/Qt6Core.lib"
    ).is_file() or not (root / "bin/Qt6Core.dll").is_file():
        raise SystemExit(f"Qt root lacks Qt6Core headers, import library, or DLL: {root}")
    return root


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=os.environ.get("SUYU_EXPORT_TEST_BUILD"))
    parser.add_argument("--qt-root", default=os.environ.get("QT_ROOT"))
    parser.add_argument("--vsdevcmd", default=os.environ.get("VSDEVCMD"))
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("The fixture requires a configured Windows Qt/MSVC toolchain.")
    build = resolve_build(args.build)
    qt_root = resolve_qt_root(build, args.qt_root)
    source = SOURCE.read_text(encoding="utf-8")
    helpers = "\n".join((
        extract(source, "namespace PackagePolicy = Common::PackagePolicy;", "#ifdef _WIN32"),
        # Includes IsSamePath and the runtime DLL list and content notice after it.
        extract(source, "static bool CopyFileReplacingExisting(", "// Save data lives at"),
        # Stops before NpdmMatchesTitle, which needs suyu's loaders.
        extract(source, "static bool CopyDeconstructedExeFs(",
                "// An extracted ExeFS names its title in main.npdm."),
        extract(source, "static QString HashExeFsFiles(", "void GameExportDialog::SetLibraryEntries("),
        extract(source, "static bool HasUnpairedStandaloneNcaUpdate(",
                "static FileSys::VirtualFile ExtractRomFsFromRom("),
    ))
    pair_guard = extract(
        source, "const auto validate_base_fallback =", "const auto romfs_from_nsp ="
    )
    registration = extract(
        source, "const auto write_registration =", "if (!write_registration(recomp_module_dirs))"
    )
    registration_helper = (
        "static bool WriteRegistration(const QString& recomp_root, const QStringList& mods) {\n"
        + registration + "\nreturn write_registration(mods);\n}\n"
    )
    prefix = r'''
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "common/package_policy.h"
#define LOG_ERROR(...) do {} while (false)
#define LOG_INFO(...) do {} while (false)
using u64 = std::uint64_t;
using u32 = std::uint32_t;
namespace suyu::recomp {
bool g_emit_fastmem = false;
bool g_emit_fpx = false;
bool EmitGuardGen() { return false; }
}
namespace FileSys {
struct VfsFile {
    std::string name;
    std::vector<unsigned char> contents;
    std::string GetName() const { return name; }
    u64 GetSize() const { return contents.size(); }
    std::vector<unsigned char> ReadBytes(u64 size, u64 offset) const {
        if (offset + size > contents.size()) return {};
        return {contents.begin() + offset, contents.begin() + offset + size};
    }
};
struct VfsDir {
    std::vector<std::shared_ptr<VfsFile>> files;
    std::vector<std::shared_ptr<VfsFile>> GetFiles() const { return files; }
};
using VirtualDir = std::shared_ptr<VfsDir>;
using VirtualFile = std::shared_ptr<VfsFile>;
}
'''
    body = r'''
static bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
// Synthetic formats: an NSO header with a payload, and a RomFS header.
static QByteArray nso(const QByteArray& payload) {
    QByteArray bytes(0x100, '\0');
    bytes.replace(0, 4, "NSO0");
    return bytes + payload;
}
static QByteArray romfs() {
    QByteArray bytes(0x80, '\0');
    bytes[0] = 0x50;
    return bytes;
}
static QByteArray readAll(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    const QString root = QString::fromLocal8Bit(argv[1]);
    const QString src = root + "/source";
    const QString dst = root + "/export/exefs";
    if (!QDir().mkpath(src) || !writeFile(src + "/main", nso("code-v1")) ||
        !writeFile(src + "/subsdk0", nso("old-module")) ||
        !writeFile(src + "/romfs.bin", romfs())) return 3;
    const QString first = HashExeFsFiles({}, src);
    if (first.isEmpty() || !CopyDeconstructedExeFs(src, dst) ||
        !QFile::exists(dst + "/main") || !QFile::exists(dst + "/subsdk0") ||
        QFile::exists(dst + "/romfs.bin")) return 4;
    if (!writeFile(src + "/main", nso("code-v2")) || !QFile::remove(src + "/subsdk0")) return 5;
    const QString second = HashExeFsFiles({}, src);
    if (second.isEmpty() || second == first || !CopyDeconstructedExeFs(src, dst) ||
        !QFile::exists(dst + "/main") || QFile::exists(dst + "/subsdk0")) return 6;
    if (readAll(dst + "/main") != nso("code-v2")) return 7;
    if (HashExeFsFiles({}, src) != second) return 8;
    if (CopyDeconstructedExeFs(src, src) || !QFile::exists(src + "/main")) return 9;
    if (CopyDeconstructedExeFs(src.toUpper(), src) || !QFile::exists(src + "/main")) return 9;
    // Only files a loader reads are copied, and anything else stops the copy: an
    // unknown sibling, a key file, or a file named like a module that is not one.
    for (const auto& [name, bytes] : std::vector<std::pair<QString, QByteArray>>{
             {"notes.txt", "unrelated"},
             {"prod.keys", "master_key_00 = 00112233445566778899aabbccddeeff\n"},
             {"sdk", "not an executable"}}) {
        const QString dirty = root + "/dirty-" + QString(name).replace('.', '_');
        const QString dirty_dst = root + "/dirty-out-" + QString(name).replace('.', '_');
        if (!writeFile(dirty + "/main", nso("code")) || !writeFile(dirty + "/" + name, bytes) ||
            CopyDeconstructedExeFs(dirty, dirty_dst) ||
            QFile::exists(dirty_dst + "/" + name)) return 30;
    }
    const QString pkg = root + "/export";
    const QString cache = pkg + "/aot_cache";
    if (!QDir().mkpath(cache + "/launcher") ||
        !writeFile(pkg + "/Synthetic.exe", "old-build") ||
        !writeFile(pkg + "/launch.bat", "old-launch") ||
        !writeFile(pkg + "/libcrypto-3-x64.dll", "old-dll") ||
        !writeFile(cache + "/launcher/static_launcher.exe", "old-static")) return 10;
    if (!PrepareAotSourcePackage(pkg, "Synthetic", cache) ||
        QFile::exists(pkg + "/Synthetic.exe") || QFile::exists(pkg + "/launch.bat") ||
        QFile::exists(pkg + "/libcrypto-3-x64.dll") || QDir(cache + "/launcher").exists()) return 11;
    QFile readme(pkg + "/README_NATIVE_EXPORT.txt");
    if (!readme.open(QIODevice::ReadOnly) ||
        !readme.readAll().contains("no compiled launcher is included")) return 12;
    auto base_dir = std::make_shared<FileSys::VfsDir>();
    auto current_dir = std::make_shared<FileSys::VfsDir>();
    auto base_main = std::make_shared<FileSys::VfsFile>();
    base_main->name = "main"; base_main->contents = {'b','a','s','e'};
    auto current_main = std::make_shared<FileSys::VfsFile>();
    current_main->name = "main"; current_main->contents = {'u','p','d','t'};
    base_dir->files.push_back(base_main);
    current_dir->files.push_back(current_main);
    FileSys::VirtualDir effective_exefs = base_dir;
'''+pair_guard+r'''
    auto base_romfs = std::make_shared<FileSys::VfsFile>();
    auto updated_romfs = std::make_shared<FileSys::VfsFile>();
    if (validate_base_fallback(base_dir, base_romfs, base_romfs) != base_romfs) return 13;
    effective_exefs = current_dir;
    if (validate_base_fallback(base_dir, base_romfs, base_romfs) != nullptr ||
        validate_base_fallback(current_dir, base_romfs, updated_romfs) != updated_romfs) return 13;
    if (HasUnpairedStandaloneNcaUpdate(base_romfs, base_romfs) ||
        !HasUnpairedStandaloneNcaUpdate(base_romfs, updated_romfs)) return 14;
    const QByteArray fallback_manifest = R"({"fallback_enabled":true,"fallback_modules":["sdk"]})";
    QStringList cached_modules;
    if (!ReadCachedFallbackPolicy(fallback_manifest, true, cached_modules) ||
        cached_modules != QStringList{QStringLiteral("sdk")} ||
        ReadCachedFallbackPolicy(fallback_manifest, false, cached_modules) ||
        ReadCachedFallbackPolicy(R"({"fallback_enabled":true,"fallback_modules":[5]})",
                                 true, cached_modules) ||
        ReadCachedFallbackPolicy(R"({"fallback_enabled":false,"fallback_modules":["sdk"]})",
                                 false, cached_modules)) return 15;
    const QString config_path = pkg + "/user/config/sdl2-config.ini";
    if (!WritePortableVersionOverride(config_path, 42, "1.0")) return 16;
    {
        QSettings ini(config_path, QSettings::IniFormat);
        if (ini.value("System/application_version_override").toUInt() != 42) return 17;
        ini.setValue("Other/retained", "yes");
        ini.sync();
    }
    if (!WritePortableVersionOverride(config_path, 0, {})) return 18;
    {
        QSettings ini(config_path, QSettings::IniFormat);
        if (ini.contains("System/application_version_override") ||
            ini.contains("System/application_display_version_override") ||
            ini.value("Other/retained").toString() != "yes") return 19;
    }
    // One allowlist decides every setting a package config carries, from the global file
    // and the per-game file alike; the per-game file itself is never shipped.
    const QString global_ini = root + "/settings/qt-config.ini";
    const QString custom_ini = root + "/settings/custom/0100000000010000.ini";
    {
        QSettings global(global_ini, QSettings::IniFormat);
        global.setValue("Renderer/backend", 1);
        global.setValue("Renderer/backend/default", false);
        global.setValue("Renderer/vulkan_device", 3);
        global.setValue("Renderer/renderer_debug", true);
        global.setValue("Cpu/cpu_debug_mode", true);
        global.setValue("System/device_name", "Someone's PC");
        global.setValue("Data%20Storage/nand_directory", "C:/Users/someone/nand");
        global.setValue("WebService/suyu_token", "synthetic-token");
        global.setValue("UI/Paths/romsPath", "C:/Users/someone/games");
        global.sync();
        QSettings custom(custom_ini, QSettings::IniFormat);
        custom.setValue("Renderer/use_vsync", 0);
        custom.setValue("Renderer/use_vsync/use_global", false);
        custom.setValue("System/current_user", 5);
        custom.setValue("System/current_user/use_global", false);
        custom.setValue("Renderer/evil_key", "x");
        custom.setValue("Renderer/evil_key/use_global", false);
        custom.setValue("Controls/player_0_guid", "synthetic");
        custom.sync();
    }
    const QString seeded = root + "/seed/user/config/sdl2-config.ini";
    QStringList stray;
    if (!SeedPortableConfig(seeded, global_ini, custom_ini) ||
        !PortableConfigIsClean(seeded, &stray) || !stray.isEmpty()) return 40;
    {
        QSettings ini(seeded, QSettings::IniFormat);
        if (ini.value("Renderer/backend").toInt() != 1 || ini.value("Renderer/use_vsync").toInt() != 0)
            return 41;
        for (const auto* key : {"Renderer/vulkan_device", "Renderer/renderer_debug",
                                "Cpu/cpu_debug_mode", "System/device_name",
                                "Data%20Storage/nand_directory", "WebService/suyu_token",
                                "UI/Paths/romsPath", "System/current_user", "Renderer/evil_key",
                                "Controls/player_0_guid"}) {
            if (ini.contains(key)) return 42;
        }
        const QByteArray raw = readAll(seeded);
        if (raw.contains("someone") || raw.contains("synthetic")) return 43;
    }
    // Deselected per-game settings leave only the global allowlisted values.
    const QString seeded_global = root + "/seed-global/sdl2-config.ini";
    if (!SeedPortableConfig(seeded_global, global_ini, {})) return 44;
    if (QSettings(seeded_global, QSettings::IniFormat).contains("Renderer/use_vsync")) return 45;
    // Anything written outside the allowlist afterwards is caught on the file itself.
    {
        QSettings ini(seeded, QSettings::IniFormat);
        ini.setValue("Data%20Storage/sdmc_directory", "C:/elsewhere");
        ini.sync();
    }
    stray.clear();
    if (PortableConfigIsClean(seeded, &stray) || stray.isEmpty()) return 46;
    const QString registry_root = root + "/registry";
    const QStringList selected{QStringLiteral("main"), QStringLiteral("subsdk1")};
    if (!QDir().mkpath(registry_root) || !WriteRegistration(registry_root, selected)) return 20;
    {
        QFile selection(registry_root + "/recomp_modules.cmake");
        QFile registry(registry_root + "/recomp_registration.c");
        if (!selection.open(QIODevice::ReadOnly) || !registry.open(QIODevice::ReadOnly)) return 21;
        const auto selection_text = selection.readAll();
        const auto registry_text = registry.readAll();
        if (!selection_text.contains("set(SUYU_RECOMP_MODULES main subsdk1)") ||
            !registry_text.contains("recomp_image_lookup_main(") ||
            !registry_text.contains("recomp_image_lookup_subsdk1(") ||
            registry_text.contains("recomp_image_lookup_sdk(")) return 22;
    }
    // A false result must stop the caller before it can configure or link.
    int builds_started = 0;
    const auto try_build = [&](const QString& path) {
        if (!WriteRegistration(path, selected)) return false;
        ++builds_started;
        return true;
    };
    const QString registry_failure = root + "/registry-failure";
    if (!QDir().mkpath(registry_failure + "/recomp_registration.c") ||
        try_build(registry_failure) || builds_started != 0 ||
        !QFile::exists(registry_failure + "/recomp_modules.cmake")) return 23;
    const QString selection_failure = root + "/selection-failure";
    if (!QDir().mkpath(selection_failure + "/recomp_modules.cmake") ||
        try_build(selection_failure) || builds_started != 0 ||
        QFile::exists(selection_failure + "/recomp_registration.c")) return 24;
    return 0;
}
'''
    env = msvc_environment(build, args.vsdevcmd)
    with tempfile.TemporaryDirectory(prefix="suyu-export-fixture-") as temp:
        directory = Path(temp)
        cpp = directory / "fixture.cpp"
        exe = directory / "fixture.exe"
        cpp.write_text(prefix + helpers + registration_helper + body, encoding="utf-8")
        command = [
            str(Path(env["VCTOOLSINSTALLDIR"]) / "bin/Hostx64/x64/cl.exe"),
            "/nologo", "/EHsc", "/std:c++20", "/Zc:__cplusplus", "/utf-8", "/MD",
            f"/I{qt_root / 'include'}", f"/I{qt_root / 'include/QtCore'}",
            f"/I{qt_root / 'mkspecs/win32-msvc'}", str(cpp),
            f"/I{ROOT / 'src'}", str(ROOT / "src/common/package_policy.cpp"),
            "/link", f"/LIBPATH:{qt_root / 'lib'}", "Qt6Core.lib", f"/OUT:{exe}",
        ]
        subprocess.run(command, cwd=directory, env=env, check=True)
        env["PATH"] = str(qt_root / "bin") + os.pathsep + env.get("PATH", "")
        subprocess.run([exe, directory / "fixture"], env=env, check=True)
    print("PASS: ExeFS roles/cache, rejected siblings, Source cleanup, NCA pairing, fallback policy, "
          "settings allowlist, version reset, registry write failures")
    return 0


if __name__ == "__main__":
    sys.exit(main())
