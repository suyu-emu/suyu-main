// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/loader/nup.h"

#include <cstring>
#include <fstream>

#include "common/logging.h"

namespace Core::Loader {

NupLoader::NupLoader() = default;
NupLoader::~NupLoader() = default;

bool NupLoader::Load(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        LOG_ERROR(Loader, "NUP: Cannot open file: {}", path);
        return false;
    }

    const auto file_size = static_cast<std::uint64_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    // Read superblock
    file.read(reinterpret_cast<char*>(&superblock_), sizeof(Superblock));
    if (!file) {
        LOG_ERROR(Loader, "NUP: Cannot read superblock");
        return false;
    }

    // Validate magic
    if (std::memcmp(superblock_.magic, kMagic, 4) != 0) {
        LOG_ERROR(Loader, "NUP: Invalid magic");
        return false;
    }

    // Validate header size
    if (superblock_.header_size < kHeaderSize) {
        LOG_ERROR(Loader, "NUP: Header size too small");
        return false;
    }

    // Validate file size
    if (superblock_.file_size != file_size) {
        LOG_ERROR(Loader, "NUP: File size mismatch");
        return false;
    }

    // Validate flags
    if (superblock_.flags != 0) {
        LOG_ERROR(Loader, "NUP: Unknown flags");
        return false;
    }

    loaded_ = true;
    LOG_INFO(Loader, "NUP: Loaded {} ({} packages)", path, superblock_.package_count);
    return true;
}

bool NupLoader::IsValidNupFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    char magic[4];
    file.read(magic, 4);
    return std::memcmp(magic, kMagic, 4) == 0;
}

} // namespace Core::Loader