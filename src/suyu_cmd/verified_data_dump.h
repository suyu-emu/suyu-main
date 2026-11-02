// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace SuyuCli {

// The caller has already resolved and validated the real base/update view.
// A fresh directory and a final completion marker prevent partial output reuse.
// Template file handles need only GetSize() and Read(); no replacement VFS.
template <typename FilePtr>
void WriteVerifiedDataFile(const FilePtr& source, const std::filesystem::path& path) {
    if (!source || source->GetSize() == 0) {
        throw std::runtime_error("empty or missing verified input file");
    }
    auto temporary = path;
    temporary += ".partial";
    if (std::filesystem::exists(path) || std::filesystem::exists(temporary)) {
        throw std::runtime_error("verified output already exists; refusing overwrite");
    }
    const std::uint64_t size = source->GetSize();
    if (size > static_cast<std::uint64_t>(SIZE_MAX)) {
        throw std::runtime_error("verified file does not fit host addressing");
    }
    std::ofstream output(temporary, std::ios::binary | std::ios::out);
    if (!output) throw std::runtime_error("cannot create verified data output");
    std::vector<std::uint8_t> buffer(8U * 1024U * 1024U);
    std::uint64_t offset = 0;
    std::uint64_t next_progress = 256ULL * 1024ULL * 1024ULL;
    while (offset < size) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), size - offset));
        if (source->Read(buffer.data(), count, static_cast<std::size_t>(offset)) != count) {
            throw std::runtime_error("short read from resolved game content");
        }
        output.write(reinterpret_cast<const char*>(buffer.data()),
                     static_cast<std::streamsize>(count));
        if (!output) throw std::runtime_error("short write/disk full while writing game content");
        offset += count;
        if (offset >= next_progress) {
            std::cerr << "[verified-data] copied " << offset << " / " << size << " bytes\n";
            next_progress += 256ULL * 1024ULL * 1024ULL;
        }
    }
    output.flush();
    if (!output) throw std::runtime_error("game content flush failed");
    output.close();
    if (!output || std::filesystem::file_size(temporary) != size || source->GetSize() != size) {
        throw std::runtime_error("resolved content size changed or output is incomplete");
    }
    std::filesystem::rename(temporary, path);
}

template <typename DirPtr, typename FilePtr>
void DumpVerifiedGameData(const DirPtr& exefs, const FilePtr& romfs,
                          const std::string& output_utf8) {
    namespace fs = std::filesystem;
    const fs::path output{std::u8string(output_utf8.cbegin(), output_utf8.cend())};
    if (!exefs || !romfs || !exefs->GetFile("main") || !exefs->GetFile("main.npdm") ||
        !output.is_absolute() || !fs::is_directory(output.parent_path())) {
        throw std::runtime_error("verified data needs complete inputs and an absolute new output directory");
    }
    // File names are a fixed executable-module allowlist, never archive paths.
    constexpr std::array names = {"main.npdm", "rtld", "main", "subsdk0", "subsdk1", "subsdk2",
                                  "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7",
                                  "subsdk8", "subsdk9", "sdk"};
    std::uint64_t total = romfs->GetSize();
    if (!total || total > 256ULL * 1024ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error("resolved RomFS size exceeds diagnostic limit");
    }
    for (const char* name : names) {
        if (const auto file = exefs->GetFile(name)) {
            if (!file->GetSize() || file->GetSize() > 1024ULL * 1024ULL * 1024ULL) {
                throw std::runtime_error("resolved executable size exceeds diagnostic limit");
            }
            total += file->GetSize();
        }
    }
    if (fs::space(output.parent_path()).available < total + 64ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error("insufficient disk space for resolved game data");
    }
    if (!fs::create_directory(output)) {
        throw std::runtime_error("verified output directory already exists; refusing overwrite");
    }
    fs::create_directory(output / "exefs");
    for (const char* name : names) {
        if (const auto file = exefs->GetFile(name)) {
            WriteVerifiedDataFile(file, output / "exefs" / name);
        }
    }
    // Deconstructed loader reads RomFS next to 'main', not above ExeFS.
    WriteVerifiedDataFile(romfs, output / "exefs" / "romfs.bin");
    std::ofstream marker(output / "verified-data.complete", std::ios::binary);
    marker << "suyu-verified-data-v1\nromfs_bytes=" << romfs->GetSize() << "\n";
    marker.flush();
    if (!marker) throw std::runtime_error("verified completion marker write failed");
    marker.close();
    if (!marker) throw std::runtime_error("verified completion marker close failed");
}

} // namespace SuyuCli
