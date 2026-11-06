// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "core/core.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/filesystem/filesystem.h"

namespace Service::FileSystem {
namespace {

class SaveDirectoryFilesystem final : public FileSys::VfsFilesystem {
public:
    explicit SaveDirectoryFilesystem(FileSys::VirtualDir root_directory)
        : VfsFilesystem{std::move(root_directory)} {}

    FileSys::VirtualDir OpenDirectory(std::string_view, FileSys::OpenMode) override {
        return root;
    }
};

} // namespace

TEST_CASE("FileSystemController replaces a process registration",
          "[core][hle][service][filesystem][main-integration]") {
    Core::System system;
    const auto save_root = std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{}, "save-root");
    system.SetFilesystem(std::make_shared<SaveDirectoryFilesystem>(save_root));

    FileSystemController controller{system};
    constexpr ProcessId Process = 0x44;
    constexpr ProgramId InitialProgram = 0x0100000000001000;
    constexpr ProgramId AuthoritativeProgram = 0x0100000000001001;

    std::shared_ptr<FileSys::RomFSFactory> initial_factory;
    REQUIRE(controller.RegisterProcess(Process, InitialProgram, std::move(initial_factory)) ==
            ResultSuccess);
    std::shared_ptr<FileSys::RomFSFactory> authoritative_factory;
    REQUIRE(controller.RegisterProcess(Process, AuthoritativeProgram,
                                       std::move(authoritative_factory)) == ResultSuccess);

    ProgramId opened_program{};
    std::shared_ptr<SaveDataController> save_data;
    std::shared_ptr<RomFsController> romfs;
    REQUIRE(controller.OpenProcess(&opened_program, &save_data, &romfs, Process) == ResultSuccess);
    CHECK(opened_program == AuthoritativeProgram);
}

} // namespace Service::FileSystem
