// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/deconstructed_rom_directory.h"

namespace Loader {

TEST_CASE("Deconstructed RomFS resolution", "[core][loader][romfs]") {
    CHECK(ResolveDeconstructedRomFS(nullptr) == nullptr);

    const auto root = std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{}, "title");
    const auto exefs = std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{}, "exefs", root);
    const auto extracted = std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{}, "romfs", root);
    root->AddDirectory(exefs);
    root->AddDirectory(extracted);

    const auto nested = std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{}, "nested",
        extracted);
    const std::vector<u8> payload{0x10, 0x20, 0x30, 0x40};
    nested->AddFile(std::make_shared<FileSys::VectorVfsFile>(payload, "asset.bin", nested));
    extracted->AddDirectory(nested);
    extracted->AddDirectory(std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{}, "empty",
        extracted));
    extracted->AddFile(
        std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{}, "zero.bin", extracted));

    const auto image = ResolveDeconstructedRomFS(exefs);
    REQUIRE(image != nullptr);

    const auto round_trip = FileSys::ExtractRomFS(image);
    REQUIRE(round_trip != nullptr);
    REQUIRE(round_trip->GetSubdirectory("empty") != nullptr);
    const auto round_trip_nested = round_trip->GetSubdirectory("nested");
    REQUIRE(round_trip_nested != nullptr);
    const auto round_trip_asset = round_trip_nested->GetFile("asset.bin");
    REQUIRE(round_trip_asset != nullptr);
    CHECK(round_trip_asset->ReadAllBytes() == payload);
    const auto round_trip_empty = round_trip->GetFile("zero.bin");
    REQUIRE(round_trip_empty != nullptr);
    CHECK(round_trip_empty->GetSize() == 0);

    SECTION("packed files take precedence") {
        const auto packed = std::make_shared<FileSys::VectorVfsFile>(
            std::vector<u8>{0xAA}, "romfs.bin", root);
        root->AddFile(packed);
        CHECK(ResolveDeconstructedRomFS(exefs) == packed);
    }

    SECTION("parent lookup is limited to an ExeFS directory") {
        const auto loose = std::make_shared<FileSys::VectorVfsDirectory>(
            std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{}, "loose",
            root);
        root->AddDirectory(loose);
        CHECK(ResolveDeconstructedRomFS(loose) == nullptr);
    }
}

} // namespace Loader
