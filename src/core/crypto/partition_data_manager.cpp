// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/string_util.h"
#include "core/crypto/key_manager.h"
#include "core/crypto/partition_data_manager.h"
#include "core/crypto/xts_encryption_layer.h"
#include "core/file_sys/vfs/vfs.h"

namespace Core::Crypto {

static FileSys::VirtualFile FindFileInDirWithNames(const FileSys::VirtualDir& dir,
                                                   const std::string& name) {
    const auto upper = Common::ToUpper(name);

    for (const auto& fname : {name, name + ".bin", upper, upper + ".BIN"}) {
        if (dir->GetFile(fname) != nullptr) {
            return dir->GetFile(fname);
        }
    }

    return nullptr;
}

PartitionDataManager::PartitionDataManager(const FileSys::VirtualDir& sysdata_dir)
    : boot0(FindFileInDirWithNames(sysdata_dir, "BOOT0")),
      fuses(FindFileInDirWithNames(sysdata_dir, "fuses")),
      kfuses(FindFileInDirWithNames(sysdata_dir, "kfuses")),
      package2({
          FindFileInDirWithNames(sysdata_dir, "BCPKG2-1-Normal-Main"),
          FindFileInDirWithNames(sysdata_dir, "BCPKG2-2-Normal-Sub"),
          FindFileInDirWithNames(sysdata_dir, "BCPKG2-3-SafeMode-Main"),
          FindFileInDirWithNames(sysdata_dir, "BCPKG2-4-SafeMode-Sub"),
          FindFileInDirWithNames(sysdata_dir, "BCPKG2-5-Repair-Main"),
          FindFileInDirWithNames(sysdata_dir, "BCPKG2-6-Repair-Sub"),
      }),
      prodinfo(FindFileInDirWithNames(sysdata_dir, "PRODINFO")) {
}

PartitionDataManager::~PartitionDataManager() = default;

bool PartitionDataManager::HasBoot0() const {
    return boot0 != nullptr;
}

FileSys::VirtualFile PartitionDataManager::GetBoot0Raw() const {
    return boot0;
}

bool PartitionDataManager::HasFuses() const {
    return fuses != nullptr;
}

FileSys::VirtualFile PartitionDataManager::GetFusesRaw() const {
    return fuses;
}

bool PartitionDataManager::HasKFuses() const {
    return kfuses != nullptr;
}

FileSys::VirtualFile PartitionDataManager::GetKFusesRaw() const {
    return kfuses;
}

bool PartitionDataManager::HasPackage2(Package2Type type) const {
    return package2.at(static_cast<size_t>(type)) != nullptr;
}

FileSys::VirtualFile PartitionDataManager::GetPackage2Raw(Package2Type type) const {
    return package2.at(static_cast<size_t>(type));
}

bool PartitionDataManager::HasProdInfo() const {
    return prodinfo != nullptr;
}

FileSys::VirtualFile PartitionDataManager::GetProdInfoRaw() const {
    return prodinfo;
}

void PartitionDataManager::DecryptProdInfo(std::array<u8, 0x20> bis_key) {
    if (prodinfo == nullptr)
        return;

    prodinfo_decrypted = std::make_shared<XTSEncryptionLayer>(prodinfo, bis_key);
}

FileSys::VirtualFile PartitionDataManager::GetDecryptedProdInfo() const {
    return prodinfo_decrypted;
}

} // namespace Core::Crypto
