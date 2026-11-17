// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Core::Loader {

// NUP (NX Package) format reader.
// Clean-room container format for unencrypted NX (Switch 1/2) game dumps.
// See tools/nup/SPEC.md for the full specification.
class NupLoader {
public:
    struct Superblock {
        char magic[4];           // 'N','U','P', format_major_version (1)
        std::uint16_t header_size;  // = 0x80 for v1
        std::uint16_t header_version; // = 1
        std::uint64_t toc_offset;   // == header_size in v1
        std::uint64_t toc_size;     // bytes
        std::uint8_t toc_sha256[32];
        std::uint64_t blob_offset;   // start of blob region, 16-byte aligned
        std::uint64_t file_size;    // total size of this .nup file
        std::uint32_t package_count;
        std::uint32_t flags;        // 0 for v1
        std::uint8_t uuid[16];
        std::uint8_t reserved[32];
    };

    static constexpr std::uint8_t kMagic[4] = {'N', 'U', 'P', 1};
    static constexpr std::uint16_t kHeaderSize = 0x80;
    static constexpr std::uint16_t kHeaderVersion = 1;

    NupLoader();
    ~NupLoader();

    // Load and validate a NUP file. Returns true on success.
    bool Load(const std::string& path);

    // Get the superblock.
    const Superblock& GetSuperblock() const { return superblock_; }

    // Check if a file is a valid NUP file.
    static bool IsValidNupFile(const std::string& path);

private:
    Superblock superblock_{};
    bool loaded_ = false;
};

} // namespace Core::Loader