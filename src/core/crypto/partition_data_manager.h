// SPDX-FileCopyrightText: Copyright 2025 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Core::Crypto {

enum class Package2Type {
    NormalMain,
    NormalSub,
    SafeModeMain,
    SafeModeSub,
    RepairMain,
    RepairSub,
};

// Raw access to the user's NAND system partitions. It decrypts with keys the user
// installed and never extracts or derives keys from them.
class PartitionDataManager {
public:
    explicit PartitionDataManager(const FileSys::VirtualDir& sysdata_dir);
    ~PartitionDataManager();

    // BOOT0
    bool HasBoot0() const;
    FileSys::VirtualFile GetBoot0Raw() const;

    // Fuses
    bool HasFuses() const;
    FileSys::VirtualFile GetFusesRaw() const;

    // K-Fuses
    bool HasKFuses() const;
    FileSys::VirtualFile GetKFusesRaw() const;

    // Package2
    bool HasPackage2(Package2Type type = Package2Type::NormalMain) const;
    FileSys::VirtualFile GetPackage2Raw(Package2Type type = Package2Type::NormalMain) const;

    // PRODINFO
    bool HasProdInfo() const;
    FileSys::VirtualFile GetProdInfoRaw() const;
    void DecryptProdInfo(std::array<u8, 0x20> bis_key);
    FileSys::VirtualFile GetDecryptedProdInfo() const;

private:
    FileSys::VirtualFile boot0;
    FileSys::VirtualFile fuses;
    FileSys::VirtualFile kfuses;
    std::array<FileSys::VirtualFile, 6> package2;
    FileSys::VirtualFile prodinfo;

    // Processed
    std::array<FileSys::VirtualFile, 6> package2_decrypted;
    FileSys::VirtualFile prodinfo_decrypted;
};

} // namespace Core::Crypto