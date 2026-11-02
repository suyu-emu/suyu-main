// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/memory/dmnt_cheat_types.h"

namespace Core {
class System;
}

namespace Service::FileSystem {
class FileSystemController;
}

namespace FileSys {

class ContentProvider;
enum class ContentProviderUnionSlot;
class NCA;
class NACP;

enum class PatchType { Update, DLC, Mod };

/// An update that is installed and enabled but whose content could not be read
/// - the NCA is missing, locked by another process, or fails to decrypt.
///
/// This is worth reporting rather than logging quietly: the base game still
/// boots, just unpatched, which looks exactly like having no update installed.
/// The only outward sign is a different version in the window title, and a user
/// who does not know which version to expect has nothing to compare against.
struct UnappliedUpdate {
    u64 title_id;
    u32 version;
};

/// Record that @p title_id's registered update could not be applied.
void RecordUnappliedUpdate(u64 title_id, u32 version);
/// Every update recorded since the last call, clearing the list.
std::vector<UnappliedUpdate> ConsumeUnappliedUpdates();


enum class PatchSource {
    Unknown,
    NAND,
    SDMC,
    External,
    Packed,
};

struct Patch {
    bool enabled;
    std::string name;
    std::string version;
    PatchType type;
    u64 program_id;
    u64 title_id;
    PatchSource source;
    std::string location;
    u32 numeric_version{0};
    std::optional<std::string> file_path;
    std::optional<std::string> root_path;
};

// A centralized class to manage patches to games.
class PatchManager {
public:
    using BuildID = std::array<u8, 0x20>;
    using Metadata = std::pair<std::unique_ptr<NACP>, VirtualFile>;

    explicit PatchManager(u64 title_id_,
                          const Service::FileSystem::FileSystemController& fs_controller_,
                          const ContentProvider& content_provider_);
    ~PatchManager();

    [[nodiscard]] u64 GetTitleID() const;

    // Currently tracked ExeFS patches:
    // - Game Updates
    [[nodiscard]] VirtualDir PatchExeFS(VirtualDir exefs) const;

    // Currently tracked NSO patches:
    // - IPS
    // - IPSwitch
    [[nodiscard]] std::vector<u8> PatchNSO(const std::vector<u8>& nso,
                                           const std::string& name) const;

    // Checks to see if PatchNSO() will have any effect given the NSO's build ID.
    // Used to prevent expensive copies in NSO loader.
    [[nodiscard]] bool HasNSOPatch(const BuildID& build_id, std::string_view name) const;

    // The enabled patch files in the same order PatchNSO applies them.
    [[nodiscard]] std::vector<VirtualFile> GetNSOPatches(const BuildID& build_id) const;

    // Creates a CheatList object with all
    [[nodiscard]] std::vector<Core::Memory::CheatEntry> CreateCheatList(
        const BuildID& build_id) const;

    // Currently tracked RomFS patches:
    // - Game Updates
    // - LayeredFS
    [[nodiscard]] VirtualFile PatchRomFS(const NCA* base_nca, VirtualFile base_romfs,
                                         ContentRecordType type = ContentRecordType::Program,
                                         VirtualFile packed_update_raw = nullptr,
                                         bool apply_layeredfs = true) const;

    // Returns a vector of patches
    [[nodiscard]] std::vector<Patch> GetPatches(VirtualFile update_raw = nullptr) const;

    struct UpdateSelection {
        /// PatchExeFS will replace the ExeFS with an installed (external, NAND or SD) update.
        bool installed_exefs{};
        /// PatchRomFS treats updates as enabled, so it applies an installed update or, failing
        /// that, one packed in the game file.
        bool romfs_enabled{};
    };
    // Whether the add-on settings let PatchExeFS and PatchRomFS apply an update. Mirrors their
    // own selection rules, which differ slightly; keep the three in sync.
    [[nodiscard]] UpdateSelection GetUpdateSelection() const;

    struct ExeFSUpdate {
        /// The provider the update is read from.
        std::optional<ContentProviderUnionSlot> slot;
        /// Its Program NCA, as PatchExeFS opens it.
        VirtualFile program;
        /// Title version from the provider or, failing that, the update's own CNMT; 0 if
        /// neither says.
        u32 version{};
        /// Display version when the provider records one ("4.0.0"); otherwise empty.
        std::string version_string;
    };
    // The update PatchExeFS replaces the ExeFS with, chosen by the same rules: the external
    // provider's enabled version, then the manual provider's, then the first NAND or SD copy.
    // nullopt when it applies none. Keep in sync with PatchExeFS.
    [[nodiscard]] std::optional<ExeFSUpdate> GetExeFSUpdate() const;

    // If the game update exists, returns the u32 version field in its Meta-type NCA. If that fails,
    // it will fallback to the Meta-type NCA of the base game. If that fails, the result will be
    // std::nullopt
    [[nodiscard]] std::optional<u32> GetGameVersion() const;

    // Given title_id of the program, attempts to get the control data of the update and parse
    // it, falling back to the base control data.
    [[nodiscard]] Metadata GetControlMetadata() const;

    // Version of GetControlMetadata that takes an arbitrary NCA
    [[nodiscard]] Metadata ParseControlNCA(const NCA& nca) const;

private:
    [[nodiscard]] std::vector<VirtualFile> CollectPatches(const std::vector<VirtualDir>& patch_dirs,
                                                          const std::string& build_id) const;

    u64 title_id;
    const Service::FileSystem::FileSystemController& fs_controller;
    const ContentProvider& content_provider;
};

} // namespace FileSys
