// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <string>

#include "common/hex_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "core/loader/nso.h"
#include "suyu_cmd/verified_data_dump.h"

namespace SuyuCli {

// Lifetime: declare this before Core::System, and the VFS before this provider.
// Only this process sees these entries. No NAND installation is performed.
class ExplicitUpdateProvider final : public FileSys::ManualContentProvider {
public:
    std::optional<u32> GetEntryVersion(u64 title_id) const override {
        const auto it = versions.find(title_id);
        return it == versions.end() ? std::nullopt : std::optional<u32>{it->second};
    }
    std::map<u64, u32> versions;
};

inline bool ConfigureExplicitUpdate(Core::System& system, ExplicitUpdateProvider& provider,
                                    const std::string& base_path,
                                    const std::string& update_path,
                                    const std::string& dump_directory = {}) {
    using FileSys::ContentRecordType;
    using FileSys::TitleType;
    const auto fail = [](const char* message) {
        LOG_ERROR(Frontend, "CLI explicit content rejected: {}", message);
        return false;
    };
    auto& vfs = *system.GetFilesystem();
    const auto base_file = vfs.OpenFile(base_path, FileSys::OpenMode::Read);
    const auto update_file = vfs.OpenFile(update_path, FileSys::OpenMode::Read);
    if (!base_file || !update_file) {
        return fail("base or update file could not be opened read-only");
    }
    const auto loader = Loader::GetLoader(system, base_file);
    if (!loader || (loader->GetFileType() != Loader::FileType::XCI &&
                    loader->GetFileType() != Loader::FileType::NSP)) {
        return fail("content base must be a bootable XCI/NSP, not an extracted module");
    }
    u64 title_id{};
    if (loader->ReadProgramId(title_id) != Loader::ResultStatus::Success || title_id == 0 ||
        FileSys::GetBaseTitleID(title_id) != title_id) {
        return fail("content base does not identify a base application");
    }
    const u64 update_id = FileSys::GetUpdateTitleID(title_id);
    // Do not silently choose among installed/external and explicitly supplied versions.
    for (const u64 id : {title_id, update_id}) {
        if (!system.GetContentProvider().ListEntriesFilter({}, {}, id).empty()) {
            return fail("profile already exposes this title/update; use a clean private content profile");
        }
    }
    FileSys::ManualContentProvider base_entries;
    FileSys::ManualContentProvider update_entries;
    if (!base_entries.AddEntriesFromContainer(base_file, false, title_id) ||
        !update_entries.AddEntriesFromContainer(update_file, true, title_id)) {
        return fail("could not read matching container content using the existing local keys");
    }
    const auto versions = update_entries.ListUpdateVersions(update_id);
    if (versions.size() != 1 || versions.front().version == 0) {
        return fail("explicit update must contain one readable, nonzero CNMT version for this title");
    }
    const auto& selected = versions.front();
    const auto update_program = selected.files[static_cast<size_t>(ContentRecordType::Program)];
    if (!update_program) {
        return fail("matching update Program NCA is missing");
    }
    const auto& disabled = Settings::values.disabled_addons[title_id];
    const auto version_key = "Update@" + std::to_string(selected.version);
    for (const auto& name : disabled) {
        if (name == "Update" || name == version_key) {
            return fail("selected update is disabled in this profile");
        }
    }
    // Register only Application records from the base, never an older on-cart update.
    for (const auto& entry : base_entries.ListEntriesFilter(TitleType::Application, {}, title_id)) {
        const auto file = base_entries.GetEntryRaw(entry.title_id, entry.type);
        if (file) {
            provider.AddEntry(TitleType::Application, entry.type, entry.title_id, file);
        }
    }
    for (size_t i = 0; i < selected.files.size(); ++i) {
        if (selected.files[i]) {
            provider.AddEntryWithVersion(TitleType::Update, static_cast<ContentRecordType>(i),
                                         update_id, selected.version, selected.version_string,
                                         selected.files[i]);
        }
    }
    provider.versions[update_id] = selected.version;
    auto base_program = provider.GetEntry(title_id, ContentRecordType::Program);
    if (!base_program || base_program->GetStatus() != Loader::ResultStatus::Success ||
        !base_program->GetExeFS() || !base_program->GetRomFS()) {
        return fail("base Program NCA / ExeFS / RomFS is not readable");
    }
    // Use the real NCA patch implementation, not a concatenation of filesystem files.
    const FileSys::NCA combined{update_program, base_program.get()};
    if (combined.GetStatus() != Loader::ResultStatus::Success || !combined.GetExeFS() ||
        !combined.GetRomFS() || combined.GetRomFS()->GetSize() == 0) {
        return fail("update could not be resolved against the supplied base NCA");
    }
    const auto expected_exefs = combined.GetExeFS();
    FileSys::ProgramMetadata metadata;
    const auto npdm = expected_exefs->GetFile("main.npdm");
    if (!expected_exefs->GetFile("main")) {
        return fail("updated main executable is missing");
    }
    if (!npdm || metadata.Load(npdm) != Loader::ResultStatus::Success ||
        !metadata.Is64BitProgram() || metadata.GetTitleID() != title_id) {
        return fail("updated NPDM is not a matching AArch64 application");
    }
    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, &provider);
    const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto effective_exefs = pm.PatchExeFS(base_program->GetExeFS());
    if (!effective_exefs) {
        return fail("normal ExeFS patch selection failed");
    }
    const std::array names = {"rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3",
                             "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"};
    for (const char* name : names) {
        const auto expected = expected_exefs->GetFile(name);
        const auto effective = effective_exefs->GetFile(name);
        if (static_cast<bool>(expected) != static_cast<bool>(effective)) {
            return fail("normal loader selected a different executable module set");
        }
        if (!expected) {
            continue;
        }
        Loader::NSOHeader want{}, got{};
        if (expected->Read(reinterpret_cast<u8*>(&want), sizeof(want), 0) != sizeof(want) ||
            effective->Read(reinterpret_cast<u8*>(&got), sizeof(got), 0) != sizeof(got) ||
            std::memcmp(&want, "NSO0", 4) != 0 || std::memcmp(&got, "NSO0", 4) != 0 ||
            want.build_id != got.build_id || expected->GetSize() != effective->GetSize()) {
            return fail("normal loader's module identity disagrees with explicit update");
        }
        LOG_INFO(Frontend, "CLI paired module name={} build_id={}", name,
                 Common::HexToString(want.build_id));
    }
    const auto effective_romfs = pm.PatchRomFS(base_program.get(), base_program->GetRomFS(),
                                              ContentRecordType::Program, nullptr, false);
    if (!effective_romfs || !FileSys::ConsumeUnappliedUpdates().empty() ||
        effective_romfs->GetSize() != combined.GetRomFS()->GetSize() ||
        effective_romfs->ReadBytes(0x50, 0) != combined.GetRomFS()->ReadBytes(0x50, 0)) {
        return fail("normal RomFS patch selection failed to match explicit update view");
    }
    LOG_INFO(Frontend, "CLI paired content verified title_id={:016X} update_id={:016X} "
                      "version={} display={} is64=1 romfs_bytes={}", title_id, update_id,
             selected.version, selected.version_string, effective_romfs->GetSize());
    if (!dump_directory.empty()) {
        try {
            DumpVerifiedGameData(effective_exefs, effective_romfs, dump_directory);
            LOG_INFO(Frontend, "CLI verified game data dump complete romfs_bytes={}",
                     effective_romfs->GetSize());
        } catch (const std::exception& error) {
            LOG_ERROR(Frontend, "CLI verified game data dump failed: {}", error.what());
            return false;
        }
    }
    return true;
}

} // namespace SuyuCli
