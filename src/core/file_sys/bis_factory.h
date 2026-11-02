// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {

enum class BisPartitionId : u32 {
    UserDataRoot = 20,
    CalibrationBinary = 27,
    CalibrationFile = 28,
    BootConfigAndPackage2Part1 = 21,
    BootConfigAndPackage2Part2 = 22,
    BootConfigAndPackage2Part3 = 23,
    BootConfigAndPackage2Part4 = 24,
    BootConfigAndPackage2Part5 = 25,
    BootConfigAndPackage2Part6 = 26,
    SafeMode = 29,
    System = 31,
    SystemProperEncryption = 32,
    SystemProperPartition = 33,
    User = 30,
};

class RegisteredCache;
class PlaceholderCache;

/// File system interface to the Built-In Storage
/// This is currently missing accessors to BIS partitions, but seemed like a good place for the NAND
/// registered caches.
class BISFactory {
public:
    /// `system_registered` overrides where installed system content is read from. When null,
    /// it is nand_root/system/Contents/registered. Placeholders and the content directory
    /// stay under nand_root either way, so installs still go to this NAND.
    /// `system_registered` / `user_registered` replace this NAND's own registered content
    /// folders when given (read-only content borrowed from another installation).
    explicit BISFactory(VirtualDir nand_root, VirtualDir load_root, VirtualDir dump_root,
                        VirtualDir system_registered = nullptr,
                        VirtualDir user_registered = nullptr);
    ~BISFactory();

    VirtualDir GetSystemNANDContentDirectory() const;
    VirtualDir GetUserNANDContentDirectory() const;

    RegisteredCache* GetSystemNANDContents() const;
    RegisteredCache* GetUserNANDContents() const;

    PlaceholderCache* GetSystemNANDPlaceholder() const;
    PlaceholderCache* GetUserNANDPlaceholder() const;

    VirtualDir GetModificationLoadRoot(u64 title_id) const;
    VirtualDir GetModificationDumpRoot(u64 title_id) const;

    VirtualDir OpenPartition(BisPartitionId id) const;
    VirtualFile OpenPartitionStorage(BisPartitionId id, VirtualFilesystem file_system) const;

    VirtualDir GetImageDirectory() const;

    u64 GetSystemNANDFreeSpace() const;
    u64 GetSystemNANDTotalSpace() const;
    u64 GetUserNANDFreeSpace() const;
    u64 GetUserNANDTotalSpace() const;
    u64 GetFullNANDTotalSpace() const;

    VirtualDir GetBCATDirectory(u64 title_id) const;

private:
    VirtualDir nand_root;
    VirtualDir load_root;
    VirtualDir dump_root;

    std::unique_ptr<RegisteredCache> sysnand_cache;
    std::unique_ptr<RegisteredCache> usrnand_cache;

    std::unique_ptr<PlaceholderCache> sysnand_placeholder;
    std::unique_ptr<PlaceholderCache> usrnand_placeholder;
};

} // namespace FileSys
