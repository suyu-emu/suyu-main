// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys {

TEST_CASE("RegisteredCache discovers nested CNMT NCA files",
          "[core][filesystem][cache][main-integration]") {
    const auto root = std::make_shared<VectorVfsDirectory>(
        std::vector<VirtualFile>{}, std::vector<VirtualDir>{}, "registered");
    // SHA-256 over the all-zero 16-byte NCA ID starts with 0x37, which selects this bucket.
    const auto bucket = std::make_shared<VectorVfsDirectory>(
        std::vector<VirtualFile>{}, std::vector<VirtualDir>{}, "00000037", root);
    const auto nested_cnmt = std::make_shared<VectorVfsFile>(
        std::vector<u8>{0}, "00000000000000000000000000000000.cnmt.nca", bucket);
    bucket->AddFile(nested_cnmt);
    root->AddDirectory(bucket);

    std::size_t parse_calls{};
    VirtualFile parsed_file;
    NcaID parsed_id{};
    const RegisteredCache cache{
        root, [&](const VirtualFile& file, const NcaID& id) -> VirtualFile {
            ++parse_calls;
            parsed_file = file;
            parsed_id = id;
            // File discovery is the behavior under test. A null parsed file makes NCA stop with
            // ErrorNullFile, before encrypted-header setup needs real keys or fixture data.
            return nullptr;
        }};

    CHECK(parse_calls == 1);
    CHECK(parsed_file == nested_cnmt);
    CHECK(parsed_id == NcaID{});
}

} // namespace FileSys
