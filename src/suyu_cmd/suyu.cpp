// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <stop_token>
#include <thread>
#include <vector>

#include <fmt/ostream.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <stb_image_write.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_timer.h>

#include "common/cpu_cache_affinity.h"
#include "common/detached_tasks.h"
#include "common/logging/backend.h"
#include "suyu_cmd/native_status.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/package_policy.h"
#include "common/fs/path_util.h"
#include "common/nvidia_flags.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/arm/recomp/arm_recomp.h"
#include "core/arm/recomp/recomp_gap_session.h"
#include "core/arm/recomp/recomp_gaps.h"
#include "core/arm/recomp/recomp_image_features.h"
#include "core/core.h"
#include "core/perf_stats.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/crypto/key_manager.h"
#include "core/crypto/portable_seal.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/baked_patch_manifest.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/service/library_applet_creator.h"
#include "core/hle/service/filesystem/filesystem.h"
#include <map>
#include "common/lz4_compression.h"
#include "core/loader/nso.h"
#include "core/recompiler/arm64_to_c.h"
#include "core/loader/loader.h"
#include "frontend_common/config.h"
#include "input_common/main.h"
#include "network/network.h"
#include "sdl_config.h"
#include "suyu_cmd/explicit_update.h"
#include "suyu_cmd/emu_window/emu_window_sdl2.h"
#include "suyu_cmd/emu_window/emu_window_sdl2_gl.h"
#ifdef __APPLE__
#include "suyu_cmd/emu_window/emu_window_sdl2_mtl.h"
#endif
#include "suyu_cmd/emu_window/emu_window_sdl2_null.h"
#include "suyu_cmd/emu_window/emu_window_sdl2_vk.h"
#include "video_core/renderer_base.h"

#ifdef USE_DISCORD_PRESENCE
#include <discord_rpc.h>
#endif

#ifdef _WIN32
// windows.h needs to be included before shellapi.h
#include <windows.h>

#include <shellapi.h>

#include "common/windows/timer_resolution.h"
#endif

#undef _UNICODE
#include <getopt.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <climits>
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
extern "C" {
// tells Nvidia and AMD drivers to use the dedicated GPU by default on laptops with switchable
// graphics
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#ifdef __unix__
#include "common/linux/gamemode.h"
#endif

#ifdef USE_DISCORD_PRESENCE
namespace {

struct DiscordPackageSettings {
    bool enabled = true;
    std::string cover_url;
};

// discord.ini beside the executable, written by the game export dialog and editable by the
// user: "enabled=0" keeps the game from contacting Discord, "cover_url" is an https image to
// show instead of the suyu logo. Without the file nothing changes. Unknown keys and comment
// lines are ignored; only the first 4 KiB is read.
DiscordPackageSettings ReadDiscordIni(const std::filesystem::path& exe_dir) {
    DiscordPackageSettings settings;
    std::ifstream file(exe_dir / "discord.ini", std::ios::binary);
    if (!file) {
        return settings;
    }
    std::string text(4096, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(file.gcount()));
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    const auto trim = [](std::string_view value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            return std::string{};
        }
        return std::string(value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1));
    };
    const auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = lower(trim(std::string_view(line).substr(0, equals)));
        const std::string value = trim(std::string_view(line).substr(equals + 1));
        if (key == "enabled") {
            const std::string flag = lower(value);
            settings.enabled = !(flag == "0" || flag == "false" || flag == "no" || flag == "off");
        } else if (key == "cover_url") {
            // Discord takes an image key of at most 256 bytes. A longer or non-https value
            // would only break the presence, so it is dropped rather than cut.
            const bool usable =
                value.size() <= 256 && value.starts_with("https://") &&
                std::none_of(value.begin(), value.end(),
                             [](unsigned char c) { return c <= 0x20 || c == 0x7f; });
            settings.cover_url = usable ? value : std::string{};
        }
    }
    return settings;
}

} // namespace
#endif

// Statically linked recompiled CPU modules.
//
// A per-game build of this executable (see SUYU_CMD_RECOMP_DIR in
// src/suyu_cmd/CMakeLists.txt) compiles in a generated recomp_registration.c
// that lists the game's modules in NSO load order. That makes the exported
// game a single self-contained binary - there is nothing to LoadLibrary and no
// recompiled_*.dll to ship beside it. The layout below must match the struct
// the generator emits.
extern "C" {
struct SuyuRecompStaticModule {
    const char* name;
    void (*(*lookup)(u64))(void*);
    void (*set_base)(u64);
    void (*run_slice)(void*);
    unsigned (*image_abi)();
};
#ifdef SUYU_CMD_STATIC_RECOMP
const SuyuRecompStaticModule* suyu_recomp_static_modules_v4(unsigned* count);
// Each registered module's build ID (lower-case hex), parallel to the module
// list, so an image is bound to the loaded module it was built from rather
// than to a position. Registrations from before it have none; under MSVC (and
// so in the export build kit, whose host objects are linked against any
// export's registry) a default that reports none stands in for the missing
// symbol, and other compilers are told by suyu_cmd/CMakeLists.txt.
#ifdef _MSC_VER
const char* const* suyu_recomp_static_build_ids_v1_absent(unsigned* count) {
    *count = 0;
    return nullptr;
}
#pragma comment(linker, "/alternatename:suyu_recomp_static_build_ids_v1=suyu_recomp_static_build_ids_v1_absent")
#ifndef SUYU_RECOMP_BUILD_IDS_V1
#define SUYU_RECOMP_BUILD_IDS_V1 1
#endif
#endif
#ifdef SUYU_RECOMP_BUILD_IDS_V1
const char* const* suyu_recomp_static_build_ids_v1(unsigned* count);
#endif
#ifdef SUYU_RECOMP_GUARD_V2
int suyu_recomp_static_guard_v2(unsigned version);
#endif
#ifdef SUYU_RECOMP_FASTMEM_V1
// ABI 6 registrations: 1 only if every module reports FM1 and accepts this
// host's layout (see Core::GetRecompFastmemLayout).
int suyu_recomp_static_fastmem_v1(u32 page_bits, u32 stride_log2, u64 pointer_mask,
                                  u32 off_table, u32 off_limit);
#endif
#ifdef SUYU_RECOMP_FEATURES_V1
// ABI 6 registrations: the OR of every module's recomp_image_features().
unsigned suyu_recomp_static_features_v1(void);
#endif
#ifdef SUYU_RECOMP_GUARD_GEN_V1
// ABI 6 GG1 registrations: every module's recomp_image_guard_gen_v1 result, in
// load order. Returns the number filled, or 0 if any module refused.
struct SuyuRecompGuardGenModule {
    u32* word;
    const u64* base;
    u64 code_lo;
    u64 code_end;
};
unsigned suyu_recomp_static_guard_gen_v1(u32 host_version, SuyuRecompGuardGenModule* out,
                                         unsigned max);
#endif
#ifdef SUYU_RECOMP_FPX_V1
// FPX1 registrations: nonzero (the modules' recomp_image_fpx_v1 answer) only if
// every module reports FPX1 and accepts this host's view (Core::GetRecompFpxLayout).
unsigned suyu_recomp_static_fpx_v1(u32 off_fpcr, u32 off_fpsr, u64 inhibit_bit);
#endif
#endif
}

static void PrintHelp(const char* argv0) {
    std::cout << "Usage: " << argv0
              << " [options]\n"
                 "-c, --config          Load the specified configuration file\n"
                 "-d, --content-dir     Folder with update/DLC NSP files to load (repeatable)\n"
                 "-f, --fullscreen      Start in fullscreen mode\n"
                 "-g, --game            File path of the game to load\n"
                 "--app-name            Display name when the loader has no title metadata\n"
                 "--content-base        Read-only base XCI/NSP for an explicit update pair\n"
                 "--content-update      Read-only matching update NSP (both flags required)\n"
                 "--content-probe       Verify that pair and exit without a window or game\n"
                 "--content-dump        New directory for resolved data (requires --content-probe)\n"
                 "-h, --help            Display this help and exit\n"
                 "-m, --multiplayer=nick:password@address:port"
                 " Nickname, password, address and port for multiplayer\n"
                 "-p, --program         Pass following string as arguments to executable\n"
                 "-t, --tas             Replay the TAS script from the user tas directory,\n"
                 "                      starting at the first displayed frame and exiting\n"
                 "                      when the script runs out\n"
                 "-u, --user            Select a specific user profile from 0 to 7\n"
                 "-V, --app-version=<n>[:<display>]\n"
                 "                      Report <n> as the application version and <display>\n"
                 "                      as its version string, for content that carries no\n"
                 "                      control data of its own. Without this a deconstructed\n"
                 "                      ROM directory always reports 1.0.0. Persisted as\n"
                 "                      application_version_override and\n"
                 "                      application_display_version_override in the config.\n"
                 "-v, --version         Output version information and exit\n"
                 "-l, "
                 "--applet-params="
                 "\"program_id,applet_id,applet_type,launch_type,prog_index,prev_prog_index\"\n"
                 "                      Numerical parameters for launching an applet. If no\n"
                 "                      game is provided, then the applet will launch off of\n"
                 "                      the applet_id.\n";
}

static void PrintVersion() {
    std::cout << "suyu" << Common::g_scm_branch << " " << Common::g_scm_desc << std::endl;
}

static void OnStateChanged(const Network::RoomMember::State& state) {
    switch (state) {
    case Network::RoomMember::State::Idle:
        LOG_DEBUG(Network, "Network is idle");
        break;
    case Network::RoomMember::State::Joining:
        LOG_DEBUG(Network, "Connection sequence to room started");
        break;
    case Network::RoomMember::State::Joined:
        LOG_DEBUG(Network, "Successfully joined to the room");
        break;
    case Network::RoomMember::State::Moderator:
        LOG_DEBUG(Network, "Successfully joined the room as a moderator");
        break;
    default:
        break;
    }
}

static void OnNetworkError(const Network::RoomMember::Error& error) {
    switch (error) {
    case Network::RoomMember::Error::LostConnection:
        LOG_DEBUG(Network, "Lost connection to the room");
        break;
    case Network::RoomMember::Error::CouldNotConnect:
        LOG_ERROR(Network, "Error: Could not connect");
        exit(1);
        break;
    case Network::RoomMember::Error::NameCollision:
        LOG_ERROR(
            Network,
            "You tried to use the same nickname as another user that is connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::IpCollision:
        LOG_ERROR(Network, "You tried to use the same fake IP-Address as another user that is "
                           "connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongPassword:
        LOG_ERROR(Network, "Room replied with: Wrong password");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongVersion:
        LOG_ERROR(Network,
                  "You are using a different version than the room you are trying to connect to");
        exit(1);
        break;
    case Network::RoomMember::Error::RoomIsFull:
        LOG_ERROR(Network, "The room is full");
        exit(1);
        break;
    case Network::RoomMember::Error::HostKicked:
        LOG_ERROR(Network, "You have been kicked by the host");
        break;
    case Network::RoomMember::Error::HostBanned:
        LOG_ERROR(Network, "You have been banned by the host");
        break;
    case Network::RoomMember::Error::UnknownError:
        LOG_ERROR(Network, "UnknownError");
        break;
    case Network::RoomMember::Error::PermissionDenied:
        LOG_ERROR(Network, "PermissionDenied");
        break;
    case Network::RoomMember::Error::NoSuchUser:
        LOG_ERROR(Network, "NoSuchUser");
        break;
    }
}

static void OnMessageReceived(const Network::ChatEntry& msg) {
    std::cout << std::endl << msg.nickname << ": " << msg.message << std::endl << std::endl;
}

static void OnStatusMessageReceived(const Network::StatusMessageEntry& msg) {
    std::string message;
    switch (msg.type) {
    case Network::IdMemberJoin:
        message = fmt::format("{} has joined", msg.nickname);
        break;
    case Network::IdMemberLeave:
        message = fmt::format("{} has left", msg.nickname);
        break;
    case Network::IdMemberKicked:
        message = fmt::format("{} has been kicked", msg.nickname);
        break;
    case Network::IdMemberBanned:
        message = fmt::format("{} has been banned", msg.nickname);
        break;
    case Network::IdAddressUnbanned:
        message = fmt::format("{} has been unbanned", msg.nickname);
        break;
    }
    if (!message.empty())
        std::cout << std::endl << "* " << message << std::endl << std::endl;
}

/// True once native recompiled CPU modules are registered — the running
/// process is a standalone game export, not the suyu dev frontend.
bool g_native_export_mode = false;
bool g_export_package = false;
SdlConfig* g_sdl_config = nullptr;

void SaveNativeControls() {
    if (g_sdl_config != nullptr) {
        g_sdl_config->SaveAllValues();
    }
}

/// Application entry point
/// mk8-recomp: report each title's CPU architecture without booting it.
///
/// The static recompiler is AArch64-only, so whether a title is A32 or A64
/// decides whether it can be targeted at all. Nothing in suyu answers that
/// without a full boot, which spins up Vulkan and crashes outright on some
/// titles. This walks the same path the exporter does
/// (XCI -> secure NSP -> Program NCA -> ExeFS -> main.npdm) and reports the
/// NPDM flags.
///
/// Results go to a file rather than stdout because suyu-cmd is linked
/// /SUBSYSTEM:WINDOWS (suyu_cmd/CMakeLists.txt:142), so std::cout is discarded
/// even when redirected. A whole library is processed in one invocation,
/// which also avoids paying process startup 100+ times.
static int ProbeIsaList(const std::string& list_path, const std::string& out_path) {
    std::ifstream list{list_path};
    if (!list) {
        return 1;
    }
    std::ofstream out{out_path, std::ios::trunc};
    if (!out) {
        return 1;
    }

    static const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();

    const auto exefs_from_nsp =
        [](const std::shared_ptr<FileSys::NSP>& nsp) -> FileSys::VirtualDir {
        if (!nsp || nsp->GetStatus() != Loader::ResultStatus::Success) {
            return nullptr;
        }
        if (auto pre_extracted = nsp->GetExeFS()) {
            return pre_extracted;
        }
        const auto tid = nsp->GetProgramTitleID();
        if (const auto nca = nsp->GetNCA(tid, FileSys::ContentRecordType::Program)) {
            if (auto exefs = nca->GetExeFS()) {
                return exefs;
            }
        }
        // An update-only NSP holds its Program NCA under TitleType::Update, and
        // the Base lookup above finds nothing at all. That matters here rather
        // than being a curiosity: where a title ships a 32-bit base, the whole
        // question is whether a later update rebuilt it as AArch64,
        // which cannot be answered without reading the update's own NPDM.
        if (const auto nca = nsp->GetNCA(tid, FileSys::ContentRecordType::Program,
                                         FileSys::TitleType::Update)) {
            return nca->GetExeFS();
        }
        return nullptr;
    };

    std::string rom_path;
    while (std::getline(list, rom_path)) {
        while (!rom_path.empty() && (rom_path.back() == '\r' || rom_path.back() == '\n')) {
            rom_path.pop_back();
        }
        if (rom_path.empty()) {
            continue;
        }

        const auto emit = [&out, &rom_path](std::string_view isa, u64 tid,
                                            std::string_view note) {
            out << isa << '\t' << fmt::format("{:016X}", tid) << '\t' << note << '\t'
                << rom_path << '\n';
            out.flush();
        };

        auto file = vfs->OpenFile(rom_path, FileSys::OpenMode::Read);
        if (!file) {
            emit("ERROR", 0, "open failed");
            continue;
        }

        std::string name = file->GetName();
        std::string ext;
        if (const auto pos = name.rfind('.'); pos != std::string::npos) {
            ext = name.substr(pos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        }

        FileSys::VirtualDir exefs;
        u64 title_id = 0;
        try {
            if (ext == ".nsp") {
                auto nsp = std::make_shared<FileSys::NSP>(file);
                title_id = nsp->GetProgramTitleID();
                exefs = exefs_from_nsp(nsp);
            } else if (ext == ".xci") {
                auto xci = std::make_shared<FileSys::XCI>(file);
                if (xci->GetStatus() != Loader::ResultStatus::Success) {
                    emit("ERROR", 0,
                         fmt::format("xci: {}", Loader::GetResultStatusString(xci->GetStatus())));
                    continue;
                }
                auto secure = xci->GetSecurePartitionNSP();
                if (!secure) {
                    emit("ERROR", 0, "xci: no secure partition");
                    continue;
                }
                title_id = secure->GetProgramTitleID();
                if (secure->GetStatus() != Loader::ResultStatus::Success) {
                    emit("ERROR", title_id,
                         fmt::format("nsp: {}", Loader::GetResultStatusString(secure->GetStatus())));
                    continue;
                }
                exefs = exefs_from_nsp(secure);
                if (!exefs) {
                    // Distinguish "no Program NCA at all" from "found it but it
                    // would not decrypt", which is almost always a missing title
                    // key. Both surface as a null ExeFS otherwise.
                    const auto nca = secure->GetNCA(title_id, FileSys::ContentRecordType::Program);
                    if (!nca) {
                        emit("ERROR", title_id, "no program nca for title id");
                    } else {
                        emit("ERROR", title_id,
                             fmt::format("program nca: {}",
                                         Loader::GetResultStatusString(nca->GetStatus())));
                    }
                    continue;
                }
            } else if (ext == ".nca") {
                auto nca = std::make_shared<FileSys::NCA>(file);
                if (nca->GetStatus() == Loader::ResultStatus::Success) {
                    title_id = nca->GetTitleId();
                    exefs = nca->GetExeFS();
                }
            }
        } catch (const std::exception& e) {
            emit("ERROR", title_id, fmt::format("exception: {}", e.what()));
            continue;
        }

        if (!exefs) {
            emit("ERROR", title_id, "no exefs");
            continue;
        }
        const auto npdm_file = exefs->GetFile("main.npdm");
        if (!npdm_file) {
            emit("ERROR", title_id, "no main.npdm");
            continue;
        }

        FileSys::ProgramMetadata metadata;
        if (metadata.Load(npdm_file) != Loader::ResultStatus::Success) {
            emit("ERROR", title_id, "npdm parse failed");
            continue;
        }

        emit(metadata.Is64BitProgram() ? "ARM64" : "ARM32", title_id,
             fmt::format("flags=0x{:02X}", metadata.Is64BitProgram() ? 1 : 0));
    }
    return 0;
}


// Decode coverage for a whole library, without exporting anything.
//
// Whether a title can run without a JIT has two halves: does the emitter
// understand every instruction in its image, and is every block that executes
// actually emitted. The second needs the title to run. This answers the first,
// which gates the second, and needs no boot, no input and no disk - the .text is
// decompressed in memory and every word is run through the emitter.
//
// Zero unhandled instructions makes a title a candidate for a JIT-free build.
// Any at all rules it out, and the signature says what is missing.
static int ProbeDecodeList(const std::string& list_path, const std::string& out_path) {
    std::ifstream list{list_path};
    std::ofstream out{out_path, std::ios::trunc};
    std::ofstream encodings{out_path + ".encodings.tsv", std::ios::trunc};
    if (!list || !out || !encodings) {
        return 1;
    }

    // The configuration a JIT-free export uses. Two instruction families are off
    // by default because the JIT runs those blocks faster; a build with no JIT
    // has nothing to hand them to, so asking the default question would report a
    // gap in every title that is not a gap for the case being measured.
    suyu::recomp::g_translate_all = true;

    static const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();

    const auto exefs_from_nsp =
        [](const std::shared_ptr<FileSys::NSP>& nsp) -> FileSys::VirtualDir {
        if (!nsp || nsp->GetStatus() != Loader::ResultStatus::Success) {
            return nullptr;
        }
        if (auto pre_extracted = nsp->GetExeFS()) {
            return pre_extracted;
        }
        const auto tid = nsp->GetProgramTitleID();
        if (const auto nca = nsp->GetNCA(tid, FileSys::ContentRecordType::Program)) {
            if (auto exefs = nca->GetExeFS()) {
                return exefs;
            }
        }
        if (const auto nca = nsp->GetNCA(tid, FileSys::ContentRecordType::Program,
                                         FileSys::TitleType::Update)) {
            return nca->GetExeFS();
        }
        return nullptr;
    };

    std::string rom_path;
    while (std::getline(list, rom_path)) {
        while (!rom_path.empty() && (rom_path.back() == '\r' || rom_path.back() == '\n')) {
            rom_path.pop_back();
        }
        if (rom_path.empty()) {
            continue;
        }

        u64 zero_words = 0, udf_words = 0, reserved_low_words = 0;
        const auto emit = [&out, &rom_path, &zero_words, &udf_words, &reserved_low_words](std::string_view status, u64 tid, u64 total,
                                            u64 unhandled, std::string_view note) {
            out << status << '\t' << fmt::format("{:016X}", tid) << '\t' << total << '\t'
                << unhandled << '\t' << note << '\t' << rom_path << '\t' << zero_words
                << '\t' << udf_words << '\t' << reserved_low_words << '\n';
            out.flush();
        };

        FileSys::VirtualDir exefs;
        u64 title_id = 0;
        try {
            auto file = vfs->OpenFile(rom_path, FileSys::OpenMode::Read);
            if (!file) {
                emit("ERROR", 0, 0, 0, "open failed");
                continue;
            }
            // These probes open encrypted archives. Report the prerequisite
            // before the storage layer attempts to use an uninitialized cipher.
            if (!Core::Crypto::KeyManager::Instance().HasKey(Core::Crypto::S256KeyType::Header)) {
                emit("ERROR", 0, 0, 0,
                     fmt::format("{}", fmt::streamed(Loader::ResultStatus::ErrorMissingHeaderKey)));
                return 1;
            }
            std::string name = file->GetName();
            std::string ext;
            if (const auto pos = name.rfind('.'); pos != std::string::npos) {
                ext = name.substr(pos);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }
            if (ext == ".nsp") {
                auto nsp = std::make_shared<FileSys::NSP>(file);
                title_id = nsp->GetProgramTitleID();
                exefs = exefs_from_nsp(nsp);
            } else if (ext == ".xci") {
                auto xci = std::make_shared<FileSys::XCI>(file);
                if (xci->GetStatus() != Loader::ResultStatus::Success) {
                    emit("ERROR", 0, 0, 0, "xci header");
                    continue;
                }
                auto secure = xci->GetSecurePartitionNSP();
                if (secure) {
                    title_id = secure->GetProgramTitleID();
                    exefs = exefs_from_nsp(secure);
                }
            }
        } catch (const std::exception& e) {
            emit("ERROR", title_id, 0, 0, fmt::format("exception: {}", e.what()));
            continue;
        }

        if (!exefs) {
            emit("ERROR", title_id, 0, 0, "no exefs");
            continue;
        }
        if (const auto npdm_file = exefs->GetFile("main.npdm")) {
            FileSys::ProgramMetadata metadata;
            if (metadata.Load(npdm_file) == Loader::ResultStatus::Success &&
                !metadata.Is64BitProgram()) {
                emit("ARM32", title_id, 0, 0, "not translatable");
                continue;
            }
        }

        u64 total = 0;
        u64 unhandled_count = 0;
        std::map<u32, u64> unhandled_sig;
        std::map<u32, u32> unhandled_example;
        std::map<u32, u64> unhandled_encodings;
        try {
            for (const auto& nso_file : exefs->GetFiles()) {
                if (!nso_file || nso_file->GetSize() < sizeof(Loader::NSOHeader)) {
                    continue;
                }
                Loader::NSOHeader header{};
                if (nso_file->ReadObject(&header) != sizeof(Loader::NSOHeader)) {
                    continue;
                }
                if (header.magic != Common::MakeMagic('N', 'S', 'O', '0')) {
                    continue;   // main.npdm and friends live here too
                }
                std::vector<u8> text = nso_file->ReadBytes(header.segments_compressed_size[0],
                                                           header.segments[0].offset);
                if (text.empty()) {
                    continue;
                }
                if (header.IsSegmentCompressed(0)) {
                    text = Common::Compression::DecompressDataLZ4(text, header.segments[0].size);
                    if (text.empty()) {
                        continue;
                    }
                }
                // Discovered blocks, not a linear sweep of .text. A linear
                // sweep also decodes literal pools and alignment padding, which
                // are not instructions and never will be - it would report a gap
                // in every binary ever built. This is the same denominator the
                // exporter uses, so the number is comparable to its coverage.
                const u64 base = header.segments[0].location;
                const auto blocks = suyu::recomp::DiscoverBlocks(text.data(), text.size(), base);
                std::string sink;
                for (const auto& block : blocks) {
                    for (u32 k = 0; k < block.count; ++k) {
                        const u64 pc = block.vaddr + static_cast<u64>(k) * 4;
                        const size_t off = static_cast<size_t>(pc - base);
                        if (off + 4 > text.size()) {
                            break;
                        }
                        u32 insn = 0;
                        std::memcpy(&insn, text.data() + off, sizeof(insn));
                        sink.clear();
                        bool miss = false;
                        suyu::recomp::Translate(insn, pc, sink, &miss);
                        ++total;
                        if (miss) {
                            ++unhandled_count;
                            ++unhandled_sig[insn & 0xFFC00000u];
                            unhandled_example.try_emplace(insn & 0xFFC00000u, insn);
                            ++unhandled_encodings[insn];
                            // Signature zero is wider than UDF's imm16. Keep
                            // its parts separate before drawing padding conclusions.
                            if (insn == 0) ++zero_words;
                            else if ((insn & 0xFFFF0000u) == 0) ++udf_words;
                            else if ((insn & 0xFFC00000u) == 0) ++reserved_low_words;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            emit("ERROR", title_id, total, unhandled_count, fmt::format("decode: {}", e.what()));
            continue;
        }

        if (total == 0) {
            emit("ERROR", title_id, 0, 0, "no nso text");
            continue;
        }

        // Exact encodings permit mnemonic-level ranking and fast decoder-only
        // rechecks without reopening archives. Keep this artifact with the survey.
        for (const auto& [insn, count] : unhandled_encodings) {
            encodings << fmt::format("{:016X}\t{:08X}\t{}\t{}\n", title_id, insn, count, rom_path);
        }
        encodings.flush();
        if (!encodings) return 1;

        // Ranked, so the note says what is missing and not only how much.
        std::vector<std::pair<u32, u64>> ranked{unhandled_sig.begin(), unhandled_sig.end()};
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) {
                      return a.second != b.second ? a.second > b.second : a.first < b.first;
                  });
        std::string note;
        for (size_t i = 0; i < ranked.size(); ++i) {
            note += fmt::format("{}{:08X}:{}:{:08X}", i ? " " : "", ranked[i].first,
                                ranked[i].second, unhandled_example.at(ranked[i].first));
        }
        if (note.empty()) {
            note = "-";
        }
        emit(unhandled_count == 0 ? "CLEAN" : "GAPS", title_id, total, unhandled_count, note);
    }
    return 0;
}

// The exporting suyu records its own executable in the package, so a problem
// it can fix is offered as a button. Empty when nothing was recorded (a Source
// export built elsewhere) or when that suyu is no longer there.
// Each line is one way to find it, first match wins: a path relative to the
// record, then an absolute one that may start with an %ENVIRONMENT% variable.
/// A path as the exporter records it: absolute, relative to `base`, or starting with a
/// %VARIABLE% (the home folder) expanded here. Empty when it cannot be expanded.
static std::filesystem::path ExpandRecordedPath(std::string line, const std::filesystem::path& base) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    std::filesystem::path exe;
    const auto close = line.starts_with('%') ? line.find('%', 1) : std::string::npos;
    if (close != std::string::npos) {
        const std::string name = line.substr(1, close - 1);
#ifdef _WIN32
        const wchar_t* value = _wgetenv(Common::UTF8ToUTF16W(name).c_str());
        if (value == nullptr) {
            return {};
        }
        exe = std::filesystem::path{std::wstring(value) +
                                    Common::UTF8ToUTF16W(line.substr(close + 1))};
#else
        const char* value = std::getenv(name.c_str());
        if (value == nullptr) {
            return {};
        }
        exe = std::filesystem::path{std::string(value) + line.substr(close + 1)};
#endif
    } else {
        exe = std::filesystem::path{Common::FS::ToU8String(line)};
    }
    if (!exe.empty() && exe.is_relative()) {
        exe = (base / exe).lexically_normal();
    }
    return exe;
}

/// The user's own game file (or extracted folder) a validated export starts, as the
/// exporter recorded it in user/config/game-source.ini. The package holds no game data.
static std::filesystem::path RecordedGameSource(const std::filesystem::path& user_root) {
    std::ifstream in(user_root / "config" / "game-source.ini");
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("path=")) {
            return ExpandRecordedPath(line.substr(5), user_root / "config");
        }
    }
    return {};
}

static std::filesystem::path RecordedSuyuExecutable(const std::filesystem::path& user_root) {
    std::ifstream in(user_root / "config" / "suyu-install.txt");
    std::string line;
    while (std::getline(in, line)) {
        const auto exe = ExpandRecordedPath(line, user_root / "config");
        std::error_code ec;
        if (!exe.empty() && std::filesystem::is_regular_file(exe, ec)) {
            return exe;
        }
    }
    return {};
}

// The installed suyu's NAND: <root>/nand, unless its own settings moved it
// (Data Storage in qt-config.ini). Keys have no such setting. Like suyu's own
// reader, any non-empty value counts, whatever its "\default" flag says.
static std::filesystem::path InstalledDataDirectory(const std::filesystem::path& installed_root,
                                                    const std::filesystem::path& config_dir,
                                                    const std::string& setting,
                                                    const char* default_subdir) {
    std::ifstream in(config_dir / "qt-config.ini");
    std::string line;
    bool in_section = false;
    std::string value;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && line.front() == '[') {
            in_section = line == "[Data%20Storage]" || line == "[Data Storage]";
            continue;
        }
        if (!in_section) {
            continue;
        }
        if (line.starts_with(setting + "=")) {
            value = line.substr(line.find('=') + 1);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
        }
    }
    if (value.empty()) {
        return installed_root / default_subdir;
    }
    const std::filesystem::path nand{Common::FS::ToU8String(value)};
    return nand.is_relative() ? installed_root / nand : nand;
}

// Starts the installed suyu with one flag, such as -install-keys. Detached: suyu keeps
// running after this game exits.
static void StartInstalledSuyu(const std::filesystem::path& suyu_exe, const std::string& flag) {
#ifdef _WIN32
    const std::wstring args(flag.begin(), flag.end());
    ShellExecuteW(nullptr, L"open", suyu_exe.wstring().c_str(), args.c_str(),
                  suyu_exe.parent_path().wstring().c_str(), SW_SHOWNORMAL);
#else
    if (fork() == 0) {
        setsid();
        execl(suyu_exe.c_str(), suyu_exe.c_str(), flag.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
#endif
}

// An exported package is double-clicked by a player with no console and no
// settings UI, so a missing prerequisite is explained in a message box rather
// than left to fail inside the loader. SDL needs no SDL_Init for this box.
// Returns true only when the player chose to continue, which is offered for
// firmware alone: only some screens need it, while nothing runs without keys.
static bool ReportExportProblem(const char* title, const std::string& message,
                                const std::filesystem::path& folder, bool allow_continue,
                                const std::filesystem::path& suyu_exe, const std::string& flag,
                                const char* install_label) {
    LOG_CRITICAL(Frontend, "{}: {}", title, message);
    // Automation has nobody to answer the box; the log carries the message instead.
    if (std::getenv("SUYU_CMD_CAPTURE_HEADLESS") != nullptr) {
        return allow_continue;
    }
    enum Choice : int { Quit, OpenFolder, Continue, InstallInSuyu };
    std::vector<SDL_MessageBoxButtonData> buttons;
    if (!suyu_exe.empty()) {
        buttons.push_back({SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, InstallInSuyu, install_label});
    }
    buttons.push_back({static_cast<SDL_MessageBoxButtonFlags>(
                           suyu_exe.empty() ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0),
                       OpenFolder, "Open folder"});
    if (allow_continue) {
        buttons.push_back({0, Continue, "Continue anyway"});
    }
    buttons.push_back({SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, Quit, "Quit"});
    const SDL_MessageBoxData box{
        (allow_continue ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_ERROR) |
            SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT,
        nullptr,
        title,
        message.c_str(),
        static_cast<int>(buttons.size()),
        buttons.data(),
        nullptr};
    int chosen = Quit;
    if (!SDL_ShowMessageBox(&box, &chosen)) {
        return false;
    }
    switch (chosen) {
    case InstallInSuyu:
        StartInstalledSuyu(suyu_exe, flag);
        return false;
    case OpenFolder: {
        // Created first, so there is somewhere to put the missing files.
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
#ifdef _WIN32
        ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        // SDL hands a file URL to `open` on macOS and to `xdg-open` elsewhere.
        std::string url = "file://";
        for (const unsigned char c : folder.string()) {
            if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
                url += static_cast<char>(c);
            } else {
                url += fmt::format("%{:02X}", c);
            }
        }
        SDL_OpenURL(url.c_str());
#endif
        return false;
    }
    case Continue:
        return true;
    default:
        return false;
    }
}

static bool HasEntries(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::is_directory(dir, ec) &&
           std::filesystem::directory_iterator(dir, ec) != std::filesystem::directory_iterator();
}

// ---- Portable exports ----
// A portable export carries the user's game file and installed update NCAs unchanged, sealed
// with a key derived from the sd_seed of the console it was made with (core/crypto/
// portable_seal.h). It runs only with keys from that console, read from the per-user keys
// folder like every export's keys; keys inside the package are never used.

namespace PortableSeal = Core::Crypto::PortableSeal;

struct PortableSealedFile {
    std::filesystem::path path;
    u64 title_id = 0;
    FileSys::ContentRecordType record_type = FileSys::ContentRecordType::Program;
    u64 size = 0;
    PortableSeal::Nonce nonce{};
};

struct PortableSealInfo {
    std::string export_id;
    PortableSeal::CheckValue check{};
    PortableSealedFile base;
    std::vector<PortableSealedFile> updates;
    std::vector<PortableSealedFile> dlc;
};

static std::string ReadSmallTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string text;
    if (in) {
        text.resize(1 << 20);
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(in.gcount()));
    }
    return text;
}

/// True when the package's manifest declares the portable package type.
static bool IsPortablePackage(const std::filesystem::path& package_dir) {
    return Common::PackagePolicy::ManifestDeclaresPortable(ReadSmallTextFile(
        package_dir / std::string{Common::PackagePolicy::kExportManifestName}));
}

/// The sealed files of a portable export, from game/seal.json. Every name, size and nonce is
/// checked; nothing outside the package's game/ folder can be named.
static std::optional<PortableSealInfo> ReadPortableSeal(const std::filesystem::path& package_dir,
                                                        std::string* error) {
    namespace PP = Common::PackagePolicy;
    const auto fail = [error](std::string why) -> std::optional<PortableSealInfo> {
        *error = std::move(why);
        return std::nullopt;
    };
    const auto manifest = nlohmann::json::parse(
        ReadSmallTextFile(package_dir / std::string{PP::kExportManifestName}), nullptr, false);
    const std::filesystem::path game_dir = package_dir / std::string{PP::kPortableGameDir};
    const auto seal =
        nlohmann::json::parse(ReadSmallTextFile(game_dir / std::string{PP::kPortableSealName}),
                              nullptr, false);
    if (!manifest.is_object() || !seal.is_object()) {
        return fail("export-package.json or game/seal.json is missing or unreadable");
    }
    const auto text = [](const nlohmann::json& object, const char* key) {
        const auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    PortableSealInfo info;
    info.export_id = text(seal, "export_id");
    if (text(seal, "format") != PortableSeal::kSealFormat) {
        return fail("game/seal.json has an unknown format");
    }
    if (info.export_id.empty() || info.export_id != text(manifest, "export_id")) {
        return fail("game/seal.json belongs to another export");
    }
    if (!PortableSeal::FromHex(text(seal, "check"), info.check.data(), info.check.size())) {
        return fail("game/seal.json has no valid check value");
    }
    const auto files = seal.find("files");
    if (files == seal.end() || !files->is_array()) {
        return fail("game/seal.json lists no files");
    }
    static const std::regex kName{R"((base|update-[0-9]{1,2}|dlc-[0-9]{1,4})\.sealed)"};
    bool has_base = false;
    std::set<std::pair<u64, FileSys::ContentRecordType>> dlc_records;
    for (const auto& entry : *files) {
        if (!entry.is_object()) {
            return fail("game/seal.json has a malformed file entry");
        }
        PortableSealedFile file;
        const std::string name = text(entry, "name");
        const std::string role = text(entry, "role");
        const std::string title = text(entry, "title_id");
        const std::string size = text(entry, "size");
        // The role follows from the name: base.sealed, update-<n>.sealed or dlc-<n>.sealed.
        const std::string_view name_role = name == PP::kPortableBaseName ? "base"
                                           : name.starts_with("update-") ? "update"
                                                                         : "dlc";
        if (!std::regex_match(name, kName) || role != name_role || title.size() != 16 ||
            size.empty() || size.size() > 20 ||
            !std::all_of(size.begin(), size.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
            !PortableSeal::FromHex(text(entry, "nonce"), file.nonce.data(), file.nonce.size())) {
            return fail("game/seal.json has a malformed entry for " + name);
        }
        std::array<u8, 8> title_bytes{};
        if (!PortableSeal::FromHex(title, title_bytes.data(), title_bytes.size())) {
            return fail("game/seal.json has a malformed title ID for " + name);
        }
        for (const u8 byte : title_bytes) {
            file.title_id = (file.title_id << 8) | byte;
        }
        // Twenty digits can still exceed u64; report that as damage, not a crash.
        try {
            file.size = std::stoull(size);
        } catch (const std::out_of_range&) {
            return fail("game/seal.json has a malformed size for " + name);
        }
        file.path = game_dir / name;
        if (role != "base") {
            const auto type = entry.find("record_type");
            if (type == entry.end() || !type->is_number_integer() ||
                type->get<std::int64_t>() < 0 ||
                type->get<std::int64_t>() >=
                    static_cast<std::int64_t>(FileSys::ContentRecordType::Count)) {
                return fail("game/seal.json has no content type for " + name);
            }
            file.record_type = static_cast<FileSys::ContentRecordType>(type->get<int>());
            if (role == "update") {
                info.updates.push_back(std::move(file));
            } else if (!dlc_records.emplace(file.title_id, file.record_type).second) {
                return fail("game/seal.json lists the same DLC content twice");
            } else {
                info.dlc.push_back(std::move(file));
            }
        } else {
            if (has_base) {
                return fail("game/seal.json lists two game files");
            }
            has_base = true;
            info.base = std::move(file);
        }
    }
    if (!has_base) {
        return fail("game/seal.json lists no game file");
    }
    // DLC is accepted only for the sealed game itself.
    for (const auto& dlc : info.dlc) {
        if (!PP::IsAddOnContentOf(dlc.title_id, info.base.title_id)) {
            return fail("game/seal.json lists DLC for another game: " +
                        Common::FS::PathToUTF8String(dlc.path.filename()));
        }
    }
    return info;
}

/// The sd_seed in a key file of the usual "name = hex" form, if it has one.
static std::optional<Core::Crypto::Key128> ReadSdSeed(const std::filesystem::path& keys_file) {
    std::ifstream in(keys_file);
    std::string line;
    while (std::getline(in, line)) {
        line.erase(std::remove_if(line.begin(), line.end(),
                                  [](unsigned char c) { return std::isspace(c); }),
                   line.end());
        const auto equals = line.find('=');
        if (equals == std::string::npos || line.starts_with('#')) {
            continue;
        }
        std::string name = line.substr(0, equals);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        Core::Crypto::Key128 seed{};
        if (name == "sd_seed" &&
            PortableSeal::FromHex(line.substr(equals + 1), seed.data(), seed.size())) {
            return seed;
        }
    }
    return std::nullopt;
}

/// True when @p sd_seed is the one the export was sealed with.
static bool SealMatches(const PortableSealInfo& seal, const Core::Crypto::Key128& sd_seed) {
    const auto key = PortableSeal::DeriveKey(sd_seed, seal.export_id);
    const auto check = key ? PortableSeal::ComputeCheck(*key) : std::nullopt;
    return check && PortableSeal::CheckEquals(*check, seal.check);
}

/// Asks for prod.keys with the system's file picker. Empty when the user cancels.
static std::filesystem::path ChooseKeyFile() {
    struct Pick {
        std::atomic<bool> done{false};
        std::string path;
    } pick;
    static const SDL_DialogFileFilter kFilters[] = {{"Key files", "keys"}, {"All files", "*"}};
    const bool events = SDL_InitSubSystem(SDL_INIT_EVENTS);
    SDL_ShowOpenFileDialog(
        [](void* userdata, const char* const* files, int) {
            auto* result = static_cast<Pick*>(userdata);
            if (files != nullptr && files[0] != nullptr) {
                result->path = files[0];
            } else if (files == nullptr) {
                LOG_ERROR(Frontend, "The file picker could not be shown: {}", SDL_GetError());
            }
            result->done = true;
        },
        &pick, nullptr, kFilters, 2, nullptr, false);
    while (!pick.done) {
        if (events) {
            SDL_PumpEvents();
        }
        SDL_Delay(20);
    }
    if (events) {
        SDL_QuitSubSystem(SDL_INIT_EVENTS);
    }
    return pick.path.empty() ? std::filesystem::path{}
                             : std::filesystem::path{Common::FS::ToU8String(pick.path)};
}

enum class KeySetupChoice { Choose, InstallInSuyu, Quit };

static KeySetupChoice AskForKeys(const char* title, const std::string& message,
                                 const std::filesystem::path& suyu_exe) {
    std::vector<SDL_MessageBoxButtonData> buttons;
    buttons.push_back({SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
                       static_cast<int>(KeySetupChoice::Choose), "Choose keys..."});
    if (!suyu_exe.empty()) {
        buttons.push_back({0, static_cast<int>(KeySetupChoice::InstallInSuyu), "Install keys"});
    }
    buttons.push_back({SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
                       static_cast<int>(KeySetupChoice::Quit), "Quit"});
    const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT,
                                 nullptr,
                                 title,
                                 message.c_str(),
                                 static_cast<int>(buttons.size()),
                                 buttons.data(),
                                 nullptr};
    int chosen = static_cast<int>(KeySetupChoice::Quit);
    if (!SDL_ShowMessageBox(&box, &chosen)) {
        return KeySetupChoice::Quit;
    }
    return static_cast<KeySetupChoice>(chosen);
}

/// Installs the key files beside @p chosen (prod.keys, and title.keys and key_retail.bin when
/// present) into @p keys_dir, as suyu's Install Decryption Keys does, after checking that they
/// are from the console the export was sealed with. Keys inside the package are refused.
static bool InstallKeysForPortable(const std::filesystem::path& chosen,
                                   const std::filesystem::path& keys_dir,
                                   const std::filesystem::path& package_dir,
                                   const PortableSealInfo& seal, bool* wrong_console,
                                   std::string* error) {
    std::error_code ec;
    const std::filesystem::path source_dir =
        std::filesystem::is_directory(chosen, ec) ? chosen : chosen.parent_path();
    if (Common::PackagePolicy::IsWithin(package_dir, source_dir)) {
        *error = "Key files inside this export's folder are never used. Choose your key files "
                 "where you keep them, outside the export.";
        return false;
    }
    const auto prod_keys = source_dir / "prod.keys";
    if (!std::filesystem::is_regular_file(prod_keys, ec)) {
        *error = "prod.keys was not found in " + Common::FS::PathToUTF8String(source_dir) + ".";
        return false;
    }
    const auto sd_seed = ReadSdSeed(prod_keys);
    if (!sd_seed) {
        *error = "The chosen prod.keys has no sd_seed. Dump the keys again from your console, "
                 "including sd_seed.";
        return false;
    }
    if (!SealMatches(seal, *sd_seed)) {
        *wrong_console = true;
        *error = "These keys are from a different console than the one this export was made "
                 "with. They were not installed.";
        return false;
    }
    std::filesystem::create_directories(keys_dir, ec);
    for (const char* name : {"prod.keys", "title.keys", "key_retail.bin"}) {
        const auto source = source_dir / name;
        if (!std::filesystem::is_regular_file(source, ec)) {
            continue;
        }
        const auto destination = keys_dir / name;
        if (std::filesystem::equivalent(source, destination, ec)) {
            continue;
        }
        if (!std::filesystem::copy_file(source, destination,
                                        std::filesystem::copy_options::overwrite_existing, ec)) {
            *error = std::string{"Could not install "} + name + ": " + ec.message();
            return false;
        }
    }
    LOG_INFO(Frontend, "Installed key files for this export into {}",
             Common::FS::PathToUTF8String(keys_dir));
    return true;
}

/// The seal key of a portable export, from the keys installed for this user. On a first
/// launch without them, asks for the user's key files and installs them for later launches.
/// Empty after the user has been told why the export cannot start.
static std::optional<Core::Crypto::Key128> UnlockPortableExport(
    const PortableSealInfo& seal, const std::filesystem::path& package_dir,
    const std::filesystem::path& suyu_exe) {
    auto& keys = Core::Crypto::KeyManager::Instance();
    const auto keys_dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::KeysDir);
    const auto keys_text = Common::FS::PathToUTF8String(keys_dir);
    const bool headless = std::getenv("SUYU_CMD_CAPTURE_HEADLESS") != nullptr;
    const auto installed_seed = [&]() -> std::optional<Core::Crypto::Key128> {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(keys_dir / "prod.keys", ec) &&
            !std::filesystem::is_regular_file(keys_dir / "prod.keys_autogenerated", ec)) {
            return std::nullopt;
        }
        if (!keys.HasKey(Core::Crypto::S128KeyType::SDSeed)) {
            return std::nullopt;
        }
        return keys.GetKey(Core::Crypto::S128KeyType::SDSeed);
    };
    const auto wrong_console = [&] {
        ReportExportProblem(
            "Keys from another console",
            fmt::format("These keys are from a different console than the one this export was "
                        "made with,\nso it cannot start. It runs only with the keys of that "
                        "console.\n\nKeys folder: {}",
                        keys_text),
            keys_dir, false, {}, "", "");
    };

    auto sd_seed = installed_seed();
    while (!sd_seed) {
        std::error_code ec;
        const bool has_prod = std::filesystem::is_regular_file(keys_dir / "prod.keys", ec);
        const char* title = has_prod ? "Keys without sd_seed" : "Missing keys";
        const std::string message = fmt::format(
            "{}\n\nThis export runs with the keys of the console it was made with.\n"
            "Choose your key files (prod.keys, and title.keys if you have it).\n\n"
            "They are installed for your user account in\n{}\n"
            "as suyu's Install Decryption Keys does, and used again on later launches.\n"
            "They are never copied into this export.",
            has_prod ? fmt::format("The keys in {} have no sd_seed, which this export needs.",
                                   keys_text)
                     : fmt::format("Missing keys: prod.keys was not found in {}.", keys_text),
            keys_text);
        std::filesystem::path chosen;
        if (headless) {
            LOG_CRITICAL(Frontend, "{}: {}", title, message);
            // Automation answers the setup with a folder or file to install keys from.
            const char* from = std::getenv("SUYU_CMD_SETUP_KEYS_FROM");
            if (from == nullptr || *from == '\0') {
                return std::nullopt;
            }
            chosen = std::filesystem::path{Common::FS::ToU8String(from)};
        } else {
            switch (AskForKeys(title, message, suyu_exe)) {
            case KeySetupChoice::InstallInSuyu:
                StartInstalledSuyu(suyu_exe, "-install-keys");
                return std::nullopt;
            case KeySetupChoice::Quit:
                return std::nullopt;
            case KeySetupChoice::Choose:
                chosen = ChooseKeyFile();
                break;
            }
            if (chosen.empty()) {
                continue;
            }
        }
        bool other_console = false;
        std::string error;
        if (!InstallKeysForPortable(chosen, keys_dir, package_dir, seal, &other_console,
                                    &error)) {
            if (other_console) {
                wrong_console();
                return std::nullopt;
            }
            LOG_CRITICAL(Frontend, "Keys not installed: {}", error);
            if (headless) {
                return std::nullopt;
            }
            const SDL_MessageBoxButtonData ok{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "OK"};
            const SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, nullptr, "Keys not installed",
                                         error.c_str(), 1, &ok, nullptr};
            int ignored = 0;
            SDL_ShowMessageBox(&box, &ignored);
            continue;
        }
        keys.ReloadKeys();
        sd_seed = installed_seed();
        if (!sd_seed && headless) {
            return std::nullopt;
        }
    }
    if (!SealMatches(seal, *sd_seed)) {
        wrong_console();
        return std::nullopt;
    }
    return PortableSeal::DeriveKey(*sd_seed, seal.export_id);
}

int main(int argc, char** argv) {
#ifdef SUYU_CMD_STATIC_RECOMP_STRICT
#ifdef _WIN32
    _putenv_s("SUYU_RECOMP_STRICT", "1");
#else
    setenv("SUYU_RECOMP_STRICT", "1", 1);
#endif
#elif defined(SUYU_CMD_STATIC_RECOMP_HYBRID)
#ifdef _WIN32
    _putenv_s("SUYU_RECOMP_STRICT", "0");
#else
    setenv("SUYU_RECOMP_STRICT", "0", 1);
#endif
#endif
#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "wb", stdout);
        freopen("CONOUT$", "wb", stderr);
    }
#endif

    try {

    // ── Portable user data for standalone game exports ──────────────────────
    // An exported game is a self-contained folder the user can move or delete
    // as a unit, so its config/saves/NAND/logs/screenshots live in <exe>/user
    // rather than %APPDATA%\suyu. KeysDir must be explicitly pinned at
    // %APPDATA%\suyu\keys (not left to derive on its own): the presence of a
    // sibling "user" folder next to the exe makes the FS layer's own
    // portable-mode auto-detection kick in first and silently rederive
    // KeysDir under <exe>/user/keys instead, so prod.keys/title.keys are
    // never bundled with a distributed export.
    // Must run before Log::Initialize(), which opens a file under LogDir.
    // A JIT baseline package is a plain copy of this executable rather than a
    // static build, so an export is also recognised at runtime by the README
    // the exporter writes beside every package launcher. Without that, the
    // user/ folder the data bundling step creates still switches on the FS
    // layer's auto-detection, which then looks for keys in the empty user/keys.
    // The export carries no system firmware; it reads the installed one, like its keys.
    // Both stay empty when this executable is not an exported package.
    std::filesystem::path installed_nand;
    std::filesystem::path installed_load;
    std::filesystem::path export_user_root;
    // Logged once logging is up: which recorded locations led back into the package.
    bool ignored_package_suyu = false;
    bool ignored_package_nand = false;
    {
        namespace FS = Common::FS;
#ifdef _WIN32
        wchar_t exe_w[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_w, MAX_PATH);
        const std::filesystem::path exe_dir = std::filesystem::path(exe_w).parent_path();
#elif defined(__APPLE__)
        // argv[0] has no directory when started through PATH, which would make
        // the checks below look in the working directory instead.
        char exe_buf[PATH_MAX]{};
        std::uint32_t exe_size = sizeof(exe_buf);
        const std::filesystem::path exe_dir = _NSGetExecutablePath(exe_buf, &exe_size) == 0
                                                  ? std::filesystem::path(exe_buf).parent_path()
                                                  : std::filesystem::path{};
#else
        std::error_code exe_ec;
        const std::filesystem::path exe_dir =
            std::filesystem::canonical("/proc/self/exe", exe_ec).parent_path();
#endif
        const std::filesystem::path user_root = exe_dir / "user";
#ifdef SUYU_CMD_STATIC_RECOMP
        // Never a "user" folder relative to wherever this was started from.
        if (exe_dir.empty()) {
            std::fprintf(stderr, "Cannot locate this executable's folder for its user data\n");
            return EXIT_FAILURE;
        }
        constexpr bool portable_export = true;
#else
        std::error_code readme_ec;
        const bool portable_export =
            !exe_dir.empty() &&
            std::filesystem::is_regular_file(exe_dir / "README_NATIVE_EXPORT.txt", readme_ec);
#endif
        if (portable_export) {
            std::filesystem::create_directories(user_root);
            // SetSuyuPath (path_util.cpp) fails with "is not a directory" if the
            // path doesn't exist yet - most of these get created lazily by
            // whatever subsystem first writes into them, but LoadDir/TASDir are
            // read from (mod scan, TAS script lookup) before anything writes to
            // them, so create every subdir up front instead of relying on that.
            for (const char* sub : {"config", "cache", "cache/shader", "log", "nand", "sdmc", "dump",
                                     "load", "screenshots", "play_time", "crash_dumps", "amiibo", "tas",
                                     "icons", "themes"}) {
                std::filesystem::create_directories(user_root / sub);
            }
            FS::SetSuyuPath(FS::SuyuPath::EdenDir, user_root);
            FS::SetSuyuPath(FS::SuyuPath::ConfigDir, user_root / "config");
            FS::SetSuyuPath(FS::SuyuPath::CacheDir, user_root / "cache");
            FS::SetSuyuPath(FS::SuyuPath::ShaderDir, user_root / "cache" / "shader");
            FS::SetSuyuPath(FS::SuyuPath::LogDir, user_root / "log");
            FS::SetSuyuPath(FS::SuyuPath::NANDDir, user_root / "nand");
            FS::SetSuyuPath(FS::SuyuPath::SaveDir, user_root / "nand");
            FS::SetSuyuPath(FS::SuyuPath::SDMCDir, user_root / "sdmc");
            FS::SetSuyuPath(FS::SuyuPath::DumpDir, user_root / "dump");
            FS::SetSuyuPath(FS::SuyuPath::LoadDir, user_root / "load");
            FS::SetSuyuPath(FS::SuyuPath::ScreenshotsDir, user_root / "screenshots");
            FS::SetSuyuPath(FS::SuyuPath::PlayTimeDir, user_root / "play_time");
            FS::SetSuyuPath(FS::SuyuPath::CrashDumpsDir, user_root / "crash_dumps");
            FS::SetSuyuPath(FS::SuyuPath::AmiiboDir, user_root / "amiibo");
            FS::SetSuyuPath(FS::SuyuPath::TASDir, user_root / "tas");
            FS::SetSuyuPath(FS::SuyuPath::IconsDir, user_root / "icons");
            FS::SetSuyuPath(FS::SuyuPath::ThemesDir, user_root / "themes");
            // Named outright rather than read back from the FS layer, which
            // may already have derived its defaults from this user/ folder.
            // The suyu that made the export decides where that is: a portable
            // install keeps its data in a user/ folder beside its executable,
            // as path_util does for it; otherwise it is the usual location.
            const std::filesystem::path recorded_suyu = RecordedSuyuExecutable(user_root);
            std::error_code portable_ec;
#ifdef _WIN32
            const std::filesystem::path default_root = FS::GetAppDataRoamingDirectory() / "suyu";
            const std::filesystem::path default_config = default_root / "config";
#else
            const std::filesystem::path default_root =
                FS::GetDataDirectory("XDG_DATA_HOME") / "suyu";
            const std::filesystem::path default_config =
                FS::GetDataDirectory("XDG_CONFIG_HOME") / "suyu";
#endif
            std::filesystem::path installed_root = default_root;
            std::filesystem::path installed_config = default_config;
            if (!recorded_suyu.empty() &&
                std::filesystem::is_directory(recorded_suyu.parent_path() / "user", portable_ec)) {
                // Keys and firmware must come from an installation outside this
                // package. A record leading back into it - a copy of suyu placed
                // inside, or an edited path - is ignored rather than letting the
                // package supply its own.
                if (Common::PackagePolicy::AcceptInstalledRoot(
                        exe_dir, recorded_suyu.parent_path() / "user")) {
                    installed_root = recorded_suyu.parent_path() / "user";
                    installed_config = installed_root / "config";
                } else {
                    ignored_package_suyu = true;
                }
            }
            // Coverage gaps are pooled in the installed suyu's user folder, which
            // its exporter reads; with no installed suyu there is nowhere to pool.
            const bool installed_found = std::filesystem::is_directory(installed_root, portable_ec);
            Core::RecompGaps::SetSharedStoreDir(installed_found
                                                    ? installed_root / "recomp" / "gaps"
                                                    : std::filesystem::path{});
            // SetSuyuPath ignores a folder that does not exist, which would
            // leave keys pointing into this package; suyu creates it anyway.
            std::filesystem::create_directories(installed_root / "keys", portable_ec);
            FS::SetSuyuPath(FS::SuyuPath::KeysDir, installed_root / "keys");
            installed_load = InstalledDataDirectory(installed_root, installed_config,
                                                     "load_directory", "load");
            installed_nand = InstalledDataDirectory(installed_root, installed_config,
                                                     "nand_directory", "nand");
            // The installed suyu's own NAND setting may not point back in here either.
            if (!Common::PackagePolicy::AcceptInstalledRoot(exe_dir, installed_nand)) {
                ignored_package_nand = true;
                installed_nand = installed_root / "nand";
            }
            export_user_root = user_root;
            g_export_package = true;
        }
    }

    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    if (ignored_package_suyu) {
        LOG_WARNING(Frontend, "Ignoring a recorded suyu inside this package; keys and firmware "
                              "come from the default installation");
    }
    if (ignored_package_nand) {
        LOG_WARNING(Frontend, "Ignoring an installed NAND folder inside this package");
    }

    // mk8-recomp: --probe-isa <rom> prints the title's CPU architecture and
    // exits, before any config, window or emulation setup.
    for (int i = 1; i + 2 < argc; ++i) {
        if (std::string_view{argv[i]} == "--probe-isa-list") {
            return ProbeIsaList(argv[i + 1], argv[i + 2]);
        }
        if (std::string_view{argv[i]} == "--probe-decode-list") {
            return ProbeDecodeList(argv[i + 1], argv[i + 2]);
        }
    }

    LOG_INFO(Frontend, "suyu-cmd starting up...");
    Common::DetachedTasks detached_tasks;

    int option_index = 0;
#ifdef _WIN32
    int argc_w;
    auto argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w);

    if (argv_w == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to get command line arguments");
        return -1;
    }
#endif
    std::string filepath;
    std::optional<std::string> config_path;
    std::optional<std::string> explicit_content_base;
    std::optional<std::string> explicit_content_update;
    bool explicit_content_probe = false;
    std::string explicit_content_dump;
    std::string program_args;
    std::optional<int> selected_user;

    bool use_multiplayer = false;
    bool fullscreen = false;
    bool tas_playback = false;
    std::optional<u32> app_version_override;
    std::string app_display_version_override;
    std::optional<std::string> app_name_override;
    Service::AM::FrontendAppletParameters load_parameters{};
    std::string nickname{};
    std::string password{};
    std::string address{};
    u16 port = Network::DefaultRoomPort;

    static struct option long_options[] = {
        // clang-format off
        {"config", required_argument, 0, 'c'},
        {"content-dir", required_argument, 0, 'd'},
        {"fullscreen", no_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {"game", required_argument, 0, 'g'},
        {"applet-params", optional_argument, 0, 'l'},
        {"multiplayer", required_argument, 0, 'm'},
        {"program", optional_argument, 0, 'p'},
        {"tas", no_argument, 0, 't'},
        {"user", required_argument, 0, 'u'},
        {"version", no_argument, 0, 'v'},
        {"app-version", required_argument, 0, 'V'},
        {"app-name", required_argument, 0, 'N'},
        {"content-base", required_argument, 0, 'B'},
        {"content-update", required_argument, 0, 'U'},
        {"content-probe", no_argument, 0, 'P'},
        {"content-dump", required_argument, 0, 'D'},
        {0, 0, 0, 0},
        // clang-format on
    };

    while (optind < argc) {
        int arg = getopt_long(argc, argv, "g:fhvp::c:d:u:l::tV:N:B:U:PD:", long_options, &option_index);
        if (arg != -1) {
            switch (static_cast<char>(arg)) {
            case 'B':
                if (explicit_content_base) {
                    LOG_ERROR(Frontend, "Duplicate --content-base argument");
                    return 2;
                }
                explicit_content_base = optarg;
                break;
            case 'U':
                if (explicit_content_update) {
                    LOG_ERROR(Frontend, "Duplicate --content-update argument");
                    return 2;
                }
                explicit_content_update = optarg;
                break;
            case 'D':
                if (!explicit_content_dump.empty() || !optarg || !*optarg) {
                    LOG_ERROR(Frontend, "Invalid or repeated --content-dump argument");
                    return 2;
                }
                explicit_content_dump = optarg;
                break;
            case 'P':
                explicit_content_probe = true;
                break;
            case 'c':
                config_path = optarg;
                break;
            case 'f':
                fullscreen = true;
                LOG_INFO(Frontend, "Starting in fullscreen mode...");
                break;
            case 'h':
                PrintHelp(argv[0]);
                return 0;
            case 'g': {
                const std::string str_arg(optarg);
                filepath = str_arg;
                break;
            }
            case 'd':
                Settings::values.external_content_dirs.emplace_back(optarg);
                break;
            case 'l': {
                std::string str_arg(argv[optind++]);
                str_arg.append(",0"); // FALLBACK: if string is partially completed ("1234,3")
                                      // this will set all those unset to 0. otherwise we get
                                      // all 3s.
                std::stringstream stream(str_arg);
                std::string sub;
                std::getline(stream, sub, ',');
                load_parameters.program_id = std::stoull(sub);
                std::getline(stream, sub, ',');
                load_parameters.applet_id = static_cast<Service::AM::AppletId>(std::stoul(sub));
                std::getline(stream, sub, ',');
                load_parameters.applet_type = static_cast<Service::AM::AppletType>(std::stoi(sub));
                std::getline(stream, sub, ',');
                load_parameters.launch_type = static_cast<Service::AM::LaunchType>(std::stoi(sub));
                std::getline(stream, sub, ',');
                load_parameters.program_index = std::stoi(sub);
                std::getline(stream, sub, ',');
                load_parameters.previous_program_index = std::stoi(sub);
                break;
            }
            case 'm': {
                use_multiplayer = true;
                const std::string str_arg(optarg);
                // regex to check if the format is nickname:password@ip:port
                // with optional :password
                const std::regex re("^([^:]+)(?::(.+))?@([^:]+)(?::([0-9]+))?$");
                if (!std::regex_match(str_arg, re)) {
                    std::cout << "Wrong format for option --multiplayer\n";
                    PrintHelp(argv[0]);
                    return 0;
                }

                std::smatch match;
                std::regex_search(str_arg, match, re);
                ASSERT(match.size() == 5);
                nickname = match[1];
                password = match[2];
                address = match[3];
                if (!match[4].str().empty()) {
                    port = static_cast<u16>(std::strtoul(match[4].str().c_str(), nullptr, 0));
                }
                std::regex nickname_re("^[a-zA-Z0-9._\\- ]+$");
                if (!std::regex_match(nickname, nickname_re)) {
                    std::cout
                        << "Nickname is not valid. Must be 4 to 20 alphanumeric characters.\n";
                    return 0;
                }
                if (address.empty()) {
                    std::cout << "Address to room must not be empty.\n";
                    return 0;
                }
                break;
            }
            case 't':
                tas_playback = true;
                break;
            case 'p':
                program_args = argv[optind];
                ++optind;
                break;
            case 'u':
                selected_user = atoi(optarg);
                break;
            case 'v':
                PrintVersion();
                return 0;
            case 'V': {
                // <numeric>[:<display>]. The numeric part is what the guest sees through
                // the application version, the display part is the string a title prints
                // for itself.
                const std::string str_arg(optarg);
                const auto colon = str_arg.find(':');
                const std::string numeric = str_arg.substr(0, colon);
                try {
                    app_version_override = static_cast<u32>(std::stoul(numeric));
                } catch (const std::exception&) {
                    std::cout << "Invalid --app-version: " << str_arg
                              << " (expected <number>[:<display>])\n";
                    return 0;
                }
                if (colon != std::string::npos) {
                    app_display_version_override = str_arg.substr(colon + 1);
                }
                break;
            }
            case 'N':
                if (app_name_override || !optarg || !*optarg) {
                    LOG_ERROR(Frontend, "Invalid or repeated --app-name argument");
                    return 2;
                }
                app_name_override = optarg;
                break;
            }
        } else {
#ifdef _WIN32
            filepath = Common::UTF16ToUTF8(argv_w[optind]);
#else
            filepath = argv[optind];
#endif
            optind++;
        }
    }

    if (explicit_content_base.has_value() != explicit_content_update.has_value() ||
        (explicit_content_probe && !explicit_content_base) ||
        (!explicit_content_dump.empty() && !explicit_content_probe)) {
        LOG_ERROR(Frontend, "Supply both --content-base and --content-update");
        return 2;
    }
    if (explicit_content_base && !filepath.empty()) {
        LOG_ERROR(Frontend, "--content-base owns the launch path; do not also supply -g or a positional game");
        return 2;
    }
    if (explicit_content_base) {
        filepath = *explicit_content_base;
    }
#ifdef SUYU_CMD_STATIC_RECOMP
    if (explicit_content_base || explicit_content_probe) {
        LOG_ERROR(Frontend, "Explicit content comparison is only supported by the ordinary CLI");
        return 2;
    }
#endif
    SdlConfig config{config_path};
    g_sdl_config = &config;

    // apply the log_filter setting
    // the logger was initialized before and doesn't pick up the filter on its own
    Common::Log::Filter filter;
    filter.ParseFilterString(Settings::values.log_filter.GetValue());
    Common::Log::SetGlobalFilter(filter);

    if (!program_args.empty()) {
        Settings::values.program_args = program_args;
    }

    if (selected_user.has_value()) {
        Settings::values.current_user = std::clamp(*selected_user, 0, 7);
    }

    if (tas_playback) {
        const auto script_path = Common::FS::GetSuyuPath(Common::FS::SuyuPath::TASDir) /
                                 "script0-1.txt";
        std::error_code tas_error;
        const auto script_size = std::filesystem::file_size(script_path, tas_error);
        LOG_INFO(Frontend, "TAS script path={} size={} valid={}", Common::FS::PathToUTF8String(script_path),
                 tas_error ? 0 : script_size, !tas_error && script_size > 0);
        if (tas_error || script_size == 0) {
            LOG_ERROR(Frontend, "TAS playback requires a nonempty script0-1.txt");
            return 2;
        }
        // Must be set before the input subsystem is constructed: the TAS driver
        // only reads the scripts out of the TAS directory when it sees this
        // enabled, and it is applied here so the config file cannot clear it.
        Settings::values.tas_enable.SetValue(true);
    }

#ifdef _WIN32
    LocalFree(argv_w);
#endif

    MicroProfileOnThreadCreate("EmuThread");
    SCOPE_EXIT {
        MicroProfileShutdown();
    };

    Common::ConfigureNvidiaEnvironmentFlags();

    // A portable package carries the user's game file, sealed to the console it was made
    // with. It is opened once the keys for that console are known, further down.
    std::optional<PortableSealInfo> portable_seal;
    if (filepath.empty() && !export_user_root.empty() &&
        IsPortablePackage(export_user_root.parent_path())) {
        std::string seal_error;
        portable_seal = ReadPortableSeal(export_user_root.parent_path(), &seal_error);
        if (!portable_seal) {
            ReportExportProblem(
                "Export damaged",
                fmt::format("This portable export cannot be opened: {}.\n\nExport the game "
                            "again.",
                            seal_error),
                export_user_root.parent_path(), false, {}, "", "");
            return 2;
        }
        filepath = Common::FS::PathToUTF8String(portable_seal->base.path);
        LOG_INFO(Frontend, "Export: portable package, game file sealed in {}", filepath);
    }

    // A package made by the validated exporter carries no game data. It starts the
    // user's own game file, which is read and decrypted with the keys installed in suyu,
    // exactly as suyu itself would; nothing inside the package is used in its place.
    if (filepath.empty() && !export_user_root.empty() &&
        std::filesystem::is_regular_file(
            export_user_root.parent_path() / Common::PackagePolicy::kExportManifestName)) {
        const auto source = RecordedGameSource(export_user_root);
        std::error_code source_ec;
        std::filesystem::path launch = source;
        if (!source.empty() && std::filesystem::is_directory(source, source_ec)) {
            launch = std::filesystem::is_directory(source / "exefs", source_ec)
                         ? source / "exefs" / "main"
                         : source / "main";
        }
        if (source.empty() || !std::filesystem::is_regular_file(launch, source_ec)) {
            ReportExportProblem(
                "Game file not found",
                fmt::format("This exported game starts your own copy of the game, which was not "
                            "found{}{}.\n\nPut the game file back, or export the game again "
                            "from where it is now.",
                            source.empty() ? "" : " at\n",
                            source.empty() ? "" : Common::FS::PathToUTF8String(source)),
                source.empty() ? export_user_root / "config" : source.parent_path(), false, {},
                "", "");
            return 2;
        }
        filepath = Common::FS::PathToUTF8String(launch);
        LOG_INFO(Frontend, "Export: starting the user's game file {}", filepath);
    }

    // Auto-detect ROM / exefs alongside the executable when no -g flag is given
    if (filepath.empty() && !static_cast<u32>(load_parameters.applet_id)) {
#ifdef _WIN32
        wchar_t exe_path_w[MAX_PATH];
        GetModuleFileNameW(nullptr, exe_path_w, MAX_PATH);
        const std::filesystem::path exe_dir = std::filesystem::path(exe_path_w).parent_path();
#else
        const std::filesystem::path exe_dir =
            std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
        // Prefer deconstructed exefs dir (Switch ROM viewer structure: exefs/main)
        const std::filesystem::path exefs_main = exe_dir / "exefs" / "main";
        if (std::filesystem::exists(exefs_main)) {
#ifdef _WIN32
            filepath = Common::UTF16ToUTF8(exefs_main.wstring());
#else
            filepath = exefs_main.string();
#endif
            LOG_INFO(Frontend, "Auto-detected exefs/main: {}", filepath);
            goto rom_found;
        }
        // Fall back to packed ROM files (XCI/NSP/NCA)
        static constexpr std::array<std::string_view, 3> exts{".xci", ".nsp", ".nca"};
        for (const auto& entry : std::filesystem::directory_iterator(exe_dir)) {
            const auto ext = Common::ToLower(entry.path().extension().string());
            for (const auto e : exts) {
                if (ext == e) {
#ifdef _WIN32
                    filepath = Common::UTF16ToUTF8(entry.path().wstring());
#else
                    filepath = entry.path().string();
#endif
                    LOG_INFO(Frontend, "Auto-detected ROM: {}", filepath);
                    goto rom_found;
                }
            }
        }
        LOG_CRITICAL(Frontend, "No ROM specified, no exefs/main found, and no XCI/NSP/NCA next to exe");
        return -1;
        rom_found:;
    }

    // Native recompiled CPU modules. An export may leave modules out (Hybrid
    // runs them on the JIT), so each image is bound to the loaded module with
    // its build ID, never to a position. Whichever way they arrive, registering
    // any of them makes ArmRecomp run the game's CPU natively instead of dynarmic.
    struct RecompModule {
        Core::RecompBlockFn (*lookup)(u64){};
        void (*set_base)(u64){};
        Core::RecompBlockFn run_slice{};
        unsigned image_abi{};
        unsigned (*guard_v2)(unsigned){};
        std::string name;     // export name: rtld, main, subsdk0, sdk
        std::string build_id; // empty for registrations from before build IDs
        bool bound{};         // given its loaded module's base
    };
    static std::vector<RecompModule> s_recomp_modules;
    bool recomp_guard_ready = false;
    // ABI 5 and ABI 6 (FM1 fast path) images are both accepted, but never
    // mixed: ABI 6 extends the shared GuestContext, so one bundle has one ABI.
    unsigned recomp_bundle_abi = 0;
    // FPX1 (exact native FP) is all or nothing across a bundle: its kill
    // switch is a context bit that only FPX1 modules keep from the guest.
    // Nonzero: the handshake answer, whose low byte is the compiled fast path.
    unsigned recomp_fpx = 0;
    [[maybe_unused]] int recomp_fpx_modules = -1;
    [[maybe_unused]] const auto fpx_layout = Core::GetRecompFpxLayout();
    [[maybe_unused]] const auto accept_recomp_abi = [&recomp_bundle_abi](unsigned abi) {
        if (abi != 5 && abi != 6) {
            LOG_CRITICAL(Frontend, "Recompiled image ABI {} is not 5 or 6; re-export all modules",
                         abi);
            return false;
        }
        if (recomp_bundle_abi != 0 && abi != recomp_bundle_abi) {
            LOG_CRITICAL(Frontend,
                         "Recompiled images mix ABI {} and {}; re-export all modules together",
                         recomp_bundle_abi, abi);
            return false;
        }
        recomp_bundle_abi = abi;
        return true;
    };
    [[maybe_unused]] const auto fastmem_layout = Core::GetRecompFastmemLayout();
    // ABI 6 GG1: filled by the handshakes below, handed over after SetRecompLookup.
    std::vector<Core::RecompGuardGen::Module> recomp_guard_gen_modules;

    // Preferred path: modules compiled straight into this executable. Nothing
    // to find on disk, nothing to load, and no version skew between the exe and
    // its modules.
#ifdef SUYU_CMD_STATIC_RECOMP
    {
        unsigned count = 0;
        const SuyuRecompStaticModule* mods = suyu_recomp_static_modules_v4(&count);
        unsigned build_id_count = 0;
        const char* const* build_ids = nullptr;
#ifdef SUYU_RECOMP_BUILD_IDS_V1
        build_ids = suyu_recomp_static_build_ids_v1(&build_id_count);
#endif
        if (!build_ids || build_id_count != count) {
            build_ids = nullptr;
            LOG_WARNING(Frontend, "Static recompiled modules carry no build IDs (registration "
                                  "predates them); binding images by NSO slot name");
        }
        for (unsigned i = 0; i < count; ++i) {
            if (!mods[i].image_abi) {
                LOG_CRITICAL(Frontend, "Static image predates correctness ABI 5; re-export all modules");
                return EXIT_FAILURE;
            }
            if (!accept_recomp_abi(mods[i].image_abi())) {
                return EXIT_FAILURE;
            }
            s_recomp_modules.push_back({mods[i].lookup, mods[i].set_base, mods[i].run_slice,
                                        mods[i].image_abi(), nullptr,
                                        mods[i].name ? mods[i].name : "",
                                        build_ids && build_ids[i] ? build_ids[i] : ""});
            LOG_INFO(Frontend, "Static recompiled module [{}] {} build_id={} — ArmRecomp active",
                     i, mods[i].name ? mods[i].name : "?",
                     s_recomp_modules.back().build_id.substr(0, 16));
        }
#ifdef SUYU_RECOMP_GUARD_V2
        recomp_guard_ready = suyu_recomp_static_guard_v2(2) != 0;
#endif
        if (recomp_bundle_abi == 6) {
            // A feature bit is a requirement on the host: refuse any this host
            // does not implement before trusting any other handshake.
            bool features_known = false;
            unsigned features = 0;
#ifdef SUYU_RECOMP_FEATURES_V1
            features = suyu_recomp_static_features_v1();
            features_known = true;
#endif
            if (!features_known) {
                LOG_CRITICAL(Frontend, "ABI 6 static modules predate the feature registry; "
                                       "re-export all modules with this build");
                return EXIT_FAILURE;
            }
            if (const u32 unknown = Core::RecompImageFeature::Unsupported(features)) {
                LOG_CRITICAL(Frontend,
                             "Static recompiled modules require image features {:#x} that this "
                             "host does not implement; update suyu or re-export with this build",
                             unknown);
                return EXIT_FAILURE;
            }
            // GG1 images complete the FM1 handshake only after this one.
            if (features & Core::RecompImageFeature::GuardGen1) {
                bool guard_gen_ok = false;
#ifdef SUYU_RECOMP_GUARD_GEN_V1
                std::vector<SuyuRecompGuardGenModule> gg(count);
                const unsigned filled = suyu_recomp_static_guard_gen_v1(
                    Core::RecompGuardGen::kHostVersion, gg.data(), count);
                guard_gen_ok = filled == count;
                for (unsigned i = 0; guard_gen_ok && i < filled; ++i) {
                    recomp_guard_gen_modules.push_back(
                        {gg[i].word, gg[i].base, gg[i].code_lo, gg[i].code_end});
                }
#endif
                if (!guard_gen_ok) {
                    LOG_CRITICAL(Frontend, "ABI 6 static modules refused the generation guard "
                                           "handshake; re-export all modules with this build");
                    return EXIT_FAILURE;
                }
            }
            bool fastmem_ok = false;
#ifdef SUYU_RECOMP_FASTMEM_V1
            fastmem_ok = suyu_recomp_static_fastmem_v1(
                             fastmem_layout.page_bits, fastmem_layout.stride_log2,
                             fastmem_layout.pointer_mask, fastmem_layout.off_table,
                             fastmem_layout.off_limit) == 1;
#endif
            if (!fastmem_ok) {
                LOG_CRITICAL(Frontend, "ABI 6 static modules do not match this host's fastmem "
                                       "layout; re-export all modules with this build");
                return EXIT_FAILURE;
            }
            if (features & Core::RecompImageFeature::ExactFpX1) {
#ifdef SUYU_RECOMP_FPX_V1
                recomp_fpx = suyu_recomp_static_fpx_v1(fpx_layout.off_fpcr, fpx_layout.off_fpsr,
                                                       fpx_layout.inhibit_bit);
#endif
                if (!recomp_fpx) {
                    LOG_CRITICAL(Frontend, "FPX1 static modules are mixed or do not match this "
                                           "host's FP layout; re-export all modules with this build");
                    return EXIT_FAILURE;
                }
            }
        }
    }
#endif

    // Compatibility path for exports that ship recompiled_*.dll beside the exe:
    // recompiled_rtld.dll, recompiled_image.dll (main), recompiled_subsdk0.dll,
    // recompiled_sdk.dll. Skipped entirely when modules are already linked in.
#ifdef _WIN32
    if (s_recomp_modules.empty()) {
        wchar_t _exe_w[MAX_PATH]{};
        GetModuleFileNameW(nullptr, _exe_w, MAX_PATH);
        const auto _exe_dir = std::filesystem::path(_exe_w).parent_path();

        // Load in standard NSO load order: rtld, main (recompiled_image), subsdk0..9, sdk
        std::vector<std::wstring> dll_order = {
            L"recompiled_rtld.dll",
            L"recompiled_image.dll",  // main
            L"recompiled_subsdk0.dll", L"recompiled_subsdk1.dll", L"recompiled_subsdk2.dll",
            L"recompiled_subsdk3.dll", L"recompiled_subsdk4.dll", L"recompiled_subsdk5.dll",
            L"recompiled_subsdk6.dll", L"recompiled_subsdk7.dll", L"recompiled_subsdk8.dll",
            L"recompiled_subsdk9.dll",
            L"recompiled_sdk.dll",
        };

        for (const auto& dll_name : dll_order) {
            const auto p = _exe_dir / dll_name;
            if (!std::filesystem::exists(p)) continue;
            HMODULE h = LoadLibraryW(p.wstring().c_str());
            if (!h) {
                LOG_WARNING(Frontend, "Found {} but LoadLibrary failed (err={})",
                            Common::UTF16ToUTF8(dll_name), GetLastError());
                continue;
            }
            using LookupFn = Core::RecompBlockFn (*)(u64);
            using SetBaseFn = void (*)(u64);
            auto lkp = reinterpret_cast<LookupFn>(GetProcAddress(h, "recomp_image_lookup"));
            auto sbf = reinterpret_cast<SetBaseFn>(GetProcAddress(h, "recomp_image_set_base"));
            if (lkp) {
                auto run_slice = reinterpret_cast<Core::RecompBlockFn>(
                    GetProcAddress(h, "recomp_image_run_slice"));
                auto image_abi = reinterpret_cast<unsigned (*)()>(
                    GetProcAddress(h, "recomp_image_abi"));
                const unsigned abi = image_abi ? image_abi() : 0;
                if (!accept_recomp_abi(abi)) {
                    FreeLibrary(h);
                    return EXIT_FAILURE;
                }
                if (abi == 6) {
                    auto features = reinterpret_cast<unsigned (*)()>(
                        GetProcAddress(h, "recomp_image_features"));
                    auto fastmem_v1 = reinterpret_cast<unsigned (*)(u32, u32, u64, u32, u32)>(
                        GetProcAddress(h, "recomp_image_fastmem_v1"));
                    // A feature bit is a requirement on the host: refuse any
                    // this host does not implement.
                    if (const u32 unknown = features ? Core::RecompImageFeature::Unsupported(
                                                           features())
                                                     : 0) {
                        LOG_CRITICAL(Frontend,
                                     "{} requires image features {:#x} that this host does not "
                                     "implement; update suyu or re-export with this build",
                                     Common::UTF16ToUTF8(dll_name), unknown);
                        FreeLibrary(h);
                        return EXIT_FAILURE;
                    }
                    // GG1 images complete the FM1 handshake only after this one.
                    if (features && (features() & Core::RecompImageFeature::GuardGen1)) {
                        using GuardGenFn = u32* (*)(u32, u64*, u64*, const u64**);
                        auto guard_gen_v1 = reinterpret_cast<GuardGenFn>(
                            GetProcAddress(h, "recomp_image_guard_gen_v1"));
                        Core::RecompGuardGen::Module gg{};
                        gg.word = guard_gen_v1 ? guard_gen_v1(Core::RecompGuardGen::kHostVersion,
                                                              &gg.code_lo, &gg.code_end, &gg.base)
                                               : nullptr;
                        if (!gg.word) {
                            LOG_CRITICAL(Frontend,
                                         "{} refused the generation guard handshake; re-export "
                                         "all modules with this build",
                                         Common::UTF16ToUTF8(dll_name));
                            FreeLibrary(h);
                            return EXIT_FAILURE;
                        }
                        recomp_guard_gen_modules.push_back(gg);
                    }
                    if (!features || !(features() & 1u) || !fastmem_v1 ||
                        fastmem_v1(fastmem_layout.page_bits, fastmem_layout.stride_log2,
                                   fastmem_layout.pointer_mask, fastmem_layout.off_table,
                                   fastmem_layout.off_limit) != 1) {
                        LOG_CRITICAL(Frontend,
                                     "{} does not match this host's fastmem layout; re-export "
                                     "all modules with this build",
                                     Common::UTF16ToUTF8(dll_name));
                        FreeLibrary(h);
                        return EXIT_FAILURE;
                    }
                    const bool has_fpx = (features() & Core::RecompImageFeature::ExactFpX1) != 0;
                    unsigned fpx = 0;
                    if (has_fpx) {
                        auto fpx_v1 = reinterpret_cast<unsigned (*)(u32, u32, u64)>(
                            GetProcAddress(h, "recomp_image_fpx_v1"));
                        fpx = fpx_v1 ? fpx_v1(fpx_layout.off_fpcr, fpx_layout.off_fpsr,
                                              fpx_layout.inhibit_bit)
                                     : 0;
                    }
                    if ((has_fpx && !fpx) ||
                        (recomp_fpx_modules >= 0 && recomp_fpx_modules != (has_fpx ? 1 : 0))) {
                        LOG_CRITICAL(Frontend,
                                     "{} does not match this host's FP layout or the FPX1 "
                                     "feature of the other modules; re-export all modules "
                                     "with this build",
                                     Common::UTF16ToUTF8(dll_name));
                        FreeLibrary(h);
                        return EXIT_FAILURE;
                    }
                    recomp_fpx_modules = has_fpx ? 1 : 0;
                    recomp_fpx = fpx;
                }
                auto guard = reinterpret_cast<unsigned (*)(unsigned)>(GetProcAddress(h, "recomp_image_guard_v2"));
                std::string module_name = Common::UTF16ToUTF8(dll_name);
                module_name = module_name.substr(11, module_name.size() - 15); // recompiled_*.dll
                s_recomp_modules.push_back({lkp, sbf, run_slice, abi, guard,
                                            module_name == "image" ? "main" : module_name});
                LOG_INFO(Frontend, "Native recompiled module [{}] loaded from {} — ArmRecomp active",
                         s_recomp_modules.size() - 1, Common::UTF16ToUTF8(dll_name));
            }
        }
        recomp_guard_ready = !s_recomp_modules.empty();
        for (const auto& module : s_recomp_modules) {
            if (!module.guard_v2 || module.guard_v2(0) != 2) recomp_guard_ready = false;
        }
        for (const auto& module : s_recomp_modules) {
            if (module.guard_v2) module.guard_v2(recomp_guard_ready ? 2 : 0);
        }
    }
#endif

    if (!s_recomp_modules.empty()) {
        // Combined lookup: try each module's lookup until one returns non-null.
        Core::SetRecompLookup([](u64 pc) -> Core::RecompBlockFn {
            for (const auto& m : s_recomp_modules) {
                if (auto fn = m.lookup(pc)) return m.run_slice ? m.run_slice : fn;
            }
            return nullptr;
        });
        const bool long_slices = std::all_of(
            s_recomp_modules.cbegin(), s_recomp_modules.cend(), [](const RecompModule& module) {
                return module.image_abi >= 4 && module.run_slice != nullptr;
            });
        Core::SetRecompLongSlices(long_slices);
        Core::SetRecompCodeGuardReady(recomp_guard_ready);
        LOG_INFO(Frontend, "Recompiled instruction guard-v2: {}",
                 recomp_guard_ready ? "ready" : "not negotiated");
        // Every ABI 6 module passed the handshake above, or loading stopped.
        Core::SetRecompFastmemReady(recomp_bundle_abi == 6);
        LOG_INFO(Frontend, "Recompiled image ABI {}; page-table fastmem: {}", recomp_bundle_abi,
                 recomp_bundle_abi == 6 ? "negotiated" : "not used");
        // After SetRecompLookup, which forgets any earlier modules, and before
        // the process exists. Logs its own outcome.
        Core::SetRecompGuardGenModules(std::move(recomp_guard_gen_modules));
        // Every FPX1 module passed its handshake above, or loading stopped.
        Core::SetRecompFpxReady(recomp_fpx != 0);
        LOG_INFO(Frontend, "Recompiled FPX1 native FP: {} (handshake {:#x})",
                 recomp_fpx ? "negotiated" : "not used", recomp_fpx);
        // Bind each loaded module to the image built from it, by build ID (see
        // RecompGaps::MatchImage). Never by position: a Hybrid export that
        // leaves main to the JIT registers [rtld, subsdk0, sdk], and handing
        // those the bases of loaded modules 0, 1, 2 gave subsdk0's image main's
        // base and sdk's image multimedia's. A module with no image runs on the
        // JIT (or, strict, stops there); an image with no module stays unbound.
        Core::SetRecompBaseSetter([](size_t index, const char* module, const char* build_id,
                                     u64 base) {
            std::vector<Core::RecompGaps::ImageIdentity> images;
            for (const auto& m : s_recomp_modules) {
                images.push_back({m.name, m.build_id});
            }
            const std::string_view id = build_id ? build_id : "";
            const auto image = Core::RecompGaps::MatchImage(images, index, module ? module : "", id);
            if (!image || !s_recomp_modules[*image].set_base) {
                LOG_WARNING(Frontend,
                            "No recompiled image for module '{}' (#{}, build_id={}, base {:#x})",
                            module ? module : "?", index, id.substr(0, 16), base);
                return;
            }
            auto& m = s_recomp_modules[*image];
            m.set_base(base);
            m.bound = true;
            // Misses in this module are gaps a re-export can close.
            Core::RecompGaps::NoteImage(base, m.name);
            LOG_INFO(Frontend, "Recompiled image '{}' bound to module '{}' (#{}, build_id={}, base {:#x})",
                     m.name, module ? module : "?", index, id.substr(0, 16), base);
        });
        // Before the first block: an image left unbound runs with base 0, so its
        // first module-relative address faults instead of saying why.
        Core::SetRecompBindCheck([]() -> size_t {
            std::string unbound;
            size_t loaded = 0;
            for (const auto& m : s_recomp_modules) {
                if (m.bound) {
                    continue;
                }
                const bool module_loaded = Core::RecompGaps::HasLoadedModule(m.build_id, m.name);
                loaded += module_loaded ? 1 : 0;
                unbound += fmt::format("{}{} (build_id={}{})", unbound.empty() ? "" : ", ",
                                       m.name, m.build_id.substr(0, 16),
                                       module_loaded ? ", its module is loaded" : "");
            }
            if (!unbound.empty()) {
                LOG_WARNING(Frontend,
                            "Recompiled image(s) not bound to any loaded module, so their code "
                            "cannot run statically: {}",
                            unbound);
            }
            return loaded;
        });
        // A window running native recompiled code is a standalone game export,
        // not the suyu dev frontend — the window chrome (title/icon) should
        // read as the game, not the emulator.
        g_native_export_mode = true;

        // Decode video on the CPU unless the user has chosen otherwise.
        // Handing the guest's VP9 streams to a hardware decoder (d3d11va on
        // Windows) deadlocks partway through the first movie on at least Intel
        // integrated graphics: the process stays alive and the log stops
        // mid-line, which presents as a permanently black window right after
        // boot. An export is something a player double-clicks with no
        // settings UI in front of them, so it defaults to the path that always
        // finishes over the one that is faster when it works.
        if (Settings::values.nvdec_emulation.UsingGlobal() &&
            Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
            Settings::values.nvdec_emulation.SetValue(Settings::NvdecEmulation::Cpu);
            LOG_INFO(Frontend, "Native export: using CPU video decoding");
        }
    }

    // Mods/patches: a standalone export is a self-contained folder, so a "mods"
    // directory beside the executable is where users will drop things. Point
    // the existing load directory at it and the normal PatchManager path
    // (LayeredFS, IPS/pchtxt patches, cheats) picks it up unchanged - same
    // <title_id>/<mod name>/ layout the Qt frontend uses.
    {
#ifdef _WIN32
        wchar_t exe_w[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_w, MAX_PATH);
        const auto exe_dir = std::filesystem::path(exe_w).parent_path();
#else
        const auto exe_dir = std::filesystem::path(argv[0]).parent_path();
#endif
        auto local_mods = exe_dir / "mods";
        if (!export_user_root.empty()) {
            const auto manifest_text = ReadSmallTextFile(exe_dir / "aot_manifest.json");
            const auto manifest = nlohmann::json::parse(manifest_text, nullptr, false);
#ifdef SUYU_CMD_STATIC_RECOMP
            if (!manifest.is_object()) {
                ReportExportProblem("Missing AOT metadata",
                                    "The game's aot_manifest.json is missing or damaged. Re-export this game.",
                                    exe_dir, false, RecordedSuyuExecutable(export_user_root), "", "Open suyu");
                return 2;
            }
#endif
            if (!manifest.is_discarded() && manifest.contains("baked_patches")) {
                std::string title;
                std::ifstream source(export_user_root / "config" / "game-source.ini");
                for (std::string line; std::getline(source, line);) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.starts_with("title_id=")) title = line.substr(9);
                }
                u64 title_id = 0;
                if (title.size() == 16 && title.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
                    title_id = FileSys::GetBaseTitleID(std::stoull(title, nullptr, 16));
                    title = fmt::format("{:016X}", title_id);
                }
                const auto digest = [](const std::string& bytes) {
                    std::array<unsigned char, 32> hash{};
                    unsigned length = 0;
                    if (!EVP_Digest(bytes.data(), bytes.size(), hash.data(), &length, EVP_sha256(), nullptr) || length != hash.size()) {
                        return std::string{};
                    }
                    return PortableSeal::ToHex(hash.data(), hash.size());
                };
                const auto baked = FileSys::VerifyBakedPatches(manifest, installed_load, exe_dir, title, digest);
                if (!baked.error.empty()) {
                    ReportExportProblem("Baked mods changed", baked.error, installed_load, false,
                                        RecordedSuyuExecutable(export_user_root), "", "Open suyu");
                    return 2;
                }
                local_mods = baked.load_root;
                auto& disabled = Settings::values.disabled_addons[title_id];
                disabled.clear();
                for (const auto& entry : std::filesystem::directory_iterator(local_mods / title)) {
                    const auto utf8_name = entry.path().filename().u8string();
                    const std::string name(utf8_name.begin(), utf8_name.end());
                    if (entry.is_directory() && !baked.mods.contains(name)) disabled.push_back(name);
                }
                LOG_INFO(Frontend, "Verified {} baked mod(s) from {}", baked.mods.size(), Common::FS::PathToUTF8String(local_mods));
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(local_mods, ec);
        if (std::filesystem::is_directory(local_mods)) {
            Common::FS::SetSuyuPath(Common::FS::SuyuPath::LoadDir, local_mods);
            LOG_INFO(Frontend, "Using local mod directory: {}",
                     Common::FS::PathToUTF8String(local_mods));
        }
        LOG_INFO(Frontend, "Keys directory (never bundled): {}",
                 Common::FS::GetSuyuPathString(Common::FS::SuyuPath::KeysDir));
    }

    // An export reads keys and firmware from the installed suyu only, so check
    // for them before the loader needs them. Existence checks only: the loader
    // still reports keys that are present but unusable, handled further down.
    const std::filesystem::path suyu_exe =
        installed_nand.empty() ? std::filesystem::path{} : RecordedSuyuExecutable(export_user_root);
    std::optional<Core::Crypto::Key128> portable_key;
    if (portable_seal) {
        portable_key =
            UnlockPortableExport(*portable_seal, export_user_root.parent_path(), suyu_exe);
        if (!portable_key) {
            return 2;
        }
    }
    if (!installed_nand.empty()) {
        const auto keys_dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::KeysDir);
        std::error_code keys_ec;
        // The same two names KeyManager loads production keys from. There is no
        // running on without them, even from extracted exefs/main with no
        // firmware: loading still decrypts, and fails inside the AES layer.
        if (!std::filesystem::is_regular_file(keys_dir / "prod.keys", keys_ec) &&
            !std::filesystem::is_regular_file(keys_dir / "prod.keys_autogenerated", keys_ec)) {
            ReportExportProblem(
                "Missing keys",
                fmt::format("Missing keys: prod.keys was not found in {}.\n\nInstall your keys "
                            "in suyu (Tools > Install Decryption Keys), then start the game "
                            "again. Copying prod.keys into that folder works just as well.",
                            Common::FS::PathToUTF8String(keys_dir)),
                keys_dir, false, suyu_exe, "-install-keys", "Install keys");
            return 2;
        }
        // Same rule as the filesystem fallback: this package's own NAND wins,
        // otherwise the installed one is read. A package made by the validated
        // exporter (export-package.json) never contains firmware, so firmware
        // found in its NAND was put there afterwards and is not used in place of
        // the installed firmware.
        const auto registered = std::filesystem::path("system") / "Contents" / "registered";
        std::error_code manifest_ec;
        const auto package_nand_firmware = export_user_root / "nand" / registered;
        if (std::filesystem::is_regular_file(
                export_user_root.parent_path() / Common::PackagePolicy::kExportManifestName,
                manifest_ec) &&
            HasEntries(package_nand_firmware)) {
            ReportExportProblem(
                "Firmware inside the export",
                fmt::format("This exported game has system firmware in its own folder ({}).\n\n"
                            "Exported games read firmware only from the installed suyu. Remove "
                            "that folder, then start the game again.",
                            Common::FS::PathToUTF8String(package_nand_firmware)),
                package_nand_firmware, false, suyu_exe, "-install-firmware",
                "Install firmware");
            return 2;
        }
        if (!HasEntries(export_user_root / "nand" / registered) &&
            !HasEntries(installed_nand / registered)) {
            const auto firmware_dir = installed_nand / registered;
            const auto message = fmt::format(
                "Missing firmware: no system firmware was found in {}.\n\nInstall firmware in "
                "suyu (Tools > Install Firmware), then start the game again.\n\nYou can "
                "continue anyway, but Mii screens and some menus may fail.",
                Common::FS::PathToUTF8String(firmware_dir));
            if (!ReportExportProblem("Missing firmware", message, firmware_dir, true, suyu_exe,
                                     "-install-firmware", "Install firmware")) {
                return 2;
            }
        }
    }

    LOG_INFO(Frontend, "suyu-cmd: Initializing system...");
    // The VFS and explicit provider outlive System, which retains their file references.
    const auto explicit_vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    SuyuCli::ExplicitUpdateProvider explicit_provider;
    Core::System system{};
    system.Initialize();
    if (!installed_nand.empty()) {
        system.GetFileSystemController().SetSystemContentFallback(installed_nand,
                                                                 export_user_root.parent_path());
    }
    LOG_INFO(Frontend, "suyu-cmd: System initialized.");
    if (explicit_content_base) {
        system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system.SetFilesystem(explicit_vfs);
        system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
        if (!SuyuCli::ConfigureExplicitUpdate(system, explicit_provider,
                                              *explicit_content_base, *explicit_content_update,
                                              explicit_content_dump)) {
            return 2;
        }
        if (explicit_content_probe) {
            LOG_INFO(Frontend, "CLI explicit content probe complete (no guest executed)");
            return 0;
        }
    }

    InputCommon::InputSubsystem input_subsystem{};

    // Apply the command line arguments
    system.ApplySettings();

    std::unique_ptr<EmuWindow_SDL2> emu_window;
    switch (Settings::values.renderer_backend.GetValue()) {
    case Settings::RendererBackend::OpenGL_GLSL:
    case Settings::RendererBackend::OpenGL_GLASM:
    case Settings::RendererBackend::OpenGL_SPIRV:
        emu_window = std::make_unique<EmuWindow_SDL2_GL>(&input_subsystem, system, fullscreen);
        break;
    case Settings::RendererBackend::Vulkan:
        emu_window = std::make_unique<EmuWindow_SDL2_VK>(&input_subsystem, system, fullscreen);
        break;
    case Settings::RendererBackend::Null:
        emu_window = std::make_unique<EmuWindow_SDL2_Null>(&input_subsystem, system, fullscreen);
        break;
    default:
        emu_window = std::make_unique<EmuWindow_SDL2_VK>(&input_subsystem, system, fullscreen);
        break;
    }

    if (tas_playback) {
        emu_window->EnableTasPlayback();
    }

#ifdef _WIN32
    Common::Windows::SetCurrentTimerResolutionToMaximum();
    system.CoreTiming().SetTimerResolutionNs(Common::Windows::GetCurrentTimerResolution());
#endif

    LOG_INFO(Frontend, "suyu-cmd: Window created, loading game...");
    if (!explicit_content_base) {
        system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    }
    // The command line wins over the configuration file, so a one-off run can differ
    // from the persisted setting without editing it.
    if (!app_version_override) {
        const u32 configured = Settings::values.application_version_override.GetValue();
        const std::string& configured_display =
            Settings::values.application_display_version_override.GetValue();
        if (configured != 0 || !configured_display.empty()) {
            app_version_override = configured;
            if (app_display_version_override.empty()) {
                app_display_version_override = configured_display;
            }
        }
    }

    if (app_version_override) {
        SuyuCmd::SetNativeLaunchVersion(app_display_version_override);
        // Deconstructed ROM directories carry no control data, so GetDisplayVersion has
        // nothing to read and falls back to a hard-coded 1.0.0. Titles that report their
        // own version, and anything that checks version compatibility, then see a value
        // that does not match the code actually loaded.
        LOG_INFO(Frontend, "suyu-cmd: reporting application version {} ({})",
                 *app_version_override,
                 app_display_version_override.empty() ? "no display version"
                                                      : app_display_version_override);
        system.SetApplicationVersionOverride(*app_version_override,
                                             app_display_version_override);
    }
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    system.GetUserChannel().clear();

    // Register the booted container's NCAs, like the Qt frontend's game list does, so the patch
    // manager can find the base program NCA. Without it an update's BKTR RomFS has no base to
    // patch and the game is handed the unpatched base RomFS alongside the updated code.
    static auto manual_provider = std::make_unique<FileSys::ManualContentProvider>();
    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                   manual_provider.get());
    if (!filepath.empty()) {
        const auto extension = Common::ToLower(std::filesystem::path(filepath).extension().string());
        if (extension == ".nsp" || extension == ".xci") {
            if (const auto game_file =
                    system.GetFilesystem()->OpenFile(filepath, FileSys::OpenMode::Read)) {
                if (!manual_provider->AddEntriesFromContainer(game_file)) {
                    LOG_WARNING(Frontend, "Could not register the contents of {}", filepath);
                }
            }
        }
    }

    if (static_cast<u32>(load_parameters.applet_id)) {
        // code below based off of suyu/main.cpp : GMainWindow::OnHomeMenu()
        // Inline minimal mapping (AppletIdToProgramId is in an anonymous namespace)
        const auto applet_id_to_prog_id = [](Service::AM::AppletId id) -> Service::AM::AppletProgramId {
            using namespace Service::AM;
            switch (id) {
            case AppletId::QLaunch:        return AppletProgramId::QLaunch;
            case AppletId::Starter:        return AppletProgramId::Starter;
            case AppletId::Auth:           return AppletProgramId::Auth;
            case AppletId::OverlayDisplay: return AppletProgramId::OverlayDisplay;
            default:                       return static_cast<AppletProgramId>(0);
            }
        };
        Service::AM::AppletProgramId applet_prog_id = applet_id_to_prog_id(load_parameters.applet_id);
        auto sysnand = system.GetFileSystemController().GetSystemNANDContents();
        if (!sysnand) {
            LOG_CRITICAL(Frontend, "Failed to load applet: Firmware not installed.");
            return -1;
        }

        auto user_applet_nca = sysnand->GetEntry(static_cast<u64>(applet_prog_id),
                                                 FileSys::ContentRecordType::Program);
        if (!user_applet_nca) {
            LOG_CRITICAL(Frontend, "Failed to load applet: applet cannot be found.");
            return -1;
        }
        if (filepath.empty())
            filepath = user_applet_nca->GetFullPath();
    } else {
        load_parameters.applet_id = Service::AM::AppletId::Application;
    }
    // A portable package's sealed files are opened through views that remove the seal. The
    // game file and update NCAs underneath are as the user's console made them, and the loader
    // decrypts them with the user's keys like any game file.
    FileSys::VirtualFile portable_game;
    if (portable_seal) {
        const auto open_sealed = [&](const PortableSealedFile& sealed) -> FileSys::VirtualFile {
            auto file = explicit_vfs->OpenFile(Common::FS::PathToUTF8String(sealed.path),
                                               FileSys::OpenMode::Read);
            if (!file || file->GetSize() != sealed.size) {
                return nullptr;
            }
            return PortableSeal::OpenSealed(std::move(file), *portable_key, sealed.nonce);
        };
        portable_game = open_sealed(portable_seal->base);
        bool complete = portable_game != nullptr;
        for (const auto& update : portable_seal->updates) {
            auto file = open_sealed(update);
            if (!file) {
                complete = false;
                break;
            }
            explicit_provider.AddEntry(FileSys::TitleType::Update, update.record_type,
                                       update.title_id, std::move(file));
        }
        if (!complete) {
            ReportExportProblem("Export damaged",
                                "A sealed file in this export's game folder is missing or has the "
                                "wrong size. Export the game again.",
                                export_user_root.parent_path() / "game", false, {}, "", "");
            return 2;
        }
        // DLC is optional: a sealed DLC file that cannot be opened leaves that DLC out, and
        // the game runs without it. A wrong key never gets here; it is refused above.
        std::size_t dlc_entries = 0;
        for (const auto& dlc : portable_seal->dlc) {
            auto file = open_sealed(dlc);
            if (!file) {
                LOG_WARNING(Frontend, "Sealed DLC file {} is missing or has the wrong size; "
                                      "continuing without DLC {:016X}",
                            Common::FS::PathToUTF8String(dlc.path.filename()), dlc.title_id);
                continue;
            }
            explicit_provider.AddEntry(FileSys::TitleType::AOC, dlc.record_type, dlc.title_id,
                                       std::move(file));
            ++dlc_entries;
        }
        if (!portable_seal->updates.empty() || dlc_entries != 0) {
            system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                           &explicit_provider);
            LOG_INFO(Frontend, "Registered {} sealed update entries and {} sealed DLC entries",
                     portable_seal->updates.size(), dlc_entries);
        }
    }
    if (!explicit_content_base) {
        // Match the Qt frontend's launch registration: a container can carry
        // control data and additional content besides its primary program.
        // This provider outlives System and does not install anything into NAND.
        const auto launch_file =
            portable_game ? portable_game
                          : system.GetFilesystem()->OpenFile(filepath, FileSys::OpenMode::Read);
        const auto launch_loader = launch_file ? Loader::GetLoader(system, launch_file) : nullptr;
        u64 program_id{};
        if (launch_loader && launch_loader->ReadProgramId(program_id) == Loader::ResultStatus::Success) {
            const auto type = launch_loader->GetFileType();
            if (type == Loader::FileType::NCA) {
                explicit_provider.AddEntry(FileSys::TitleType::Application,
                    FileSys::GetCRTypeFromNCAType(FileSys::NCA{launch_file}.GetType()),
                    program_id, launch_file);
                system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                               &explicit_provider);
            } else if (type == Loader::FileType::XCI || type == Loader::FileType::NSP) {
                const auto nsp = type == Loader::FileType::NSP
                    ? std::make_shared<FileSys::NSP>(launch_file)
                    : FileSys::XCI{launch_file}.GetSecurePartitionNSP();
                if (nsp) {
                    size_t entries{};
                    for (const auto& [title_id, content] : nsp->GetNCAs()) {
                        for (const auto& [record, nca] : content) {
                            explicit_provider.AddEntry(record.first, record.second, title_id,
                                                       nca->GetBaseFile());
                            ++entries;
                        }
                    }
                    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                                   &explicit_provider);
                    LOG_INFO(Frontend, "Registered {} launch-container content entries", entries);
                }
            } else if (type == Loader::FileType::DeconstructedRomDirectory) {
                const auto launch_path = std::filesystem::u8path(filepath);
                const auto control_path = launch_path.parent_path() / "control.nca";
                const auto manifest_path = launch_path.parent_path().parent_path() / "aot_manifest.json";
                bool required = false;
                std::error_code manifest_ec;
                const bool has_manifest = std::filesystem::exists(manifest_path, manifest_ec);
                std::ifstream manifest{manifest_path};
                if (manifest_ec || (has_manifest && !manifest.is_open())) {
                    LOG_CRITICAL(Frontend, "Packaged manifest could not be opened: {}",
                                 Common::FS::PathToUTF8String(manifest_path));
                    return 2;
                }
                if (manifest.is_open()) {
                    const auto metadata = nlohmann::json::parse(manifest, nullptr, false);
                    if (!metadata.is_object()) {
                        LOG_CRITICAL(Frontend, "Packaged manifest is unreadable: {}",
                                     Common::FS::PathToUTF8String(manifest_path));
                        return 2;
                    }
                    const auto entry = metadata.find("control_metadata");
                    if (entry != metadata.end()) {
                        if (!entry->is_string() || entry->get<std::string>() != "exefs/control.nca") {
                            LOG_CRITICAL(Frontend, "Packaged control metadata declaration is invalid");
                            return 2;
                        }
                        required = true;
                    }
                }
                const auto control_file = system.GetFilesystem()->OpenFile(
                    Common::FS::PathToUTF8String(control_path), FileSys::OpenMode::Read);
                if (!control_file && required) {
                    LOG_CRITICAL(Frontend, "Packaged control metadata is missing: {}; re-export the game",
                                 Common::FS::PathToUTF8String(control_path));
                    return 2;
                }
                if (control_file) {
                    const FileSys::NCA control{control_file};
                    if (!FileSys::IsValidControlMetadata(control, program_id)) {
                        LOG_CRITICAL(Frontend,
                                     "Packaged control metadata is unreadable or belongs to another game: {}; "
                                     "check installed keys and re-export the game",
                                     Common::FS::PathToUTF8String(control_path));
                        return 2;
                    }
                    explicit_provider.AddEntry(FileSys::TitleType::Application,
                                               FileSys::ContentRecordType::Control,
                                               program_id, control_file);
                    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                                   &explicit_provider);
                    LOG_INFO(Frontend, "Registered packaged control metadata for {:016X}", program_id);
                }
            }
        }
    }
    LOG_INFO(Frontend, "suyu-cmd: Calling system.Load for '{}'...", filepath);
    // A deconstructed ROM may have no control metadata for GetGameName.
    // Preserve a reusable name from the chosen launch path for the status UI.
    // UTF-8 both ways: path::string() throws on Windows for names outside the ANSI code
    // page, and a Japanese title is exactly that.
    const std::filesystem::path launch_path{Common::FS::ToU8String(filepath)};
    const auto launch_stem = Common::FS::PathToUTF8String(launch_path.stem());
    std::string fallback_name =
        (launch_stem == "main" && launch_path.parent_path().filename() == "exefs")
            ? Common::FS::PathToUTF8String(launch_path.parent_path().parent_path().filename())
            : launch_stem;
    // An export's folder is "<game> - <backend>"; the window title names the backend itself.
    for (const std::string_view suffix : {" - Dynarmic JIT", " - Hybrid AOT + JIT"}) {
        if (fallback_name.ends_with(suffix) && fallback_name.size() > suffix.size()) {
            fallback_name.resize(fallback_name.size() - suffix.size());
        }
    }
    if (portable_seal) {
        // The sealed file's name says nothing; the package folder is named after the game.
        fallback_name = Common::FS::PathToUTF8String(export_user_root.parent_path().filename());
        for (const std::string_view suffix : {" - Dynarmic JIT", " - Hybrid AOT + JIT"}) {
            if (fallback_name.ends_with(suffix) && fallback_name.size() > suffix.size()) {
                fallback_name.resize(fallback_name.size() - suffix.size());
            }
        }
    }
    SuyuCmd::SetNativeLaunchName(app_name_override.value_or(fallback_name));
    // Before Load creates the emulated CPU threads.
    Common::ApplyLargestCacheAffinity(Settings::values.cache_affinity.GetValue());
    const Core::SystemResultStatus load_result{
        portable_game ? system.Load(*emu_window, portable_game, load_parameters)
                      : system.Load(*emu_window, filepath, load_parameters)};
    LOG_INFO(Frontend, "suyu-cmd: system.Load returned: {}", static_cast<int>(load_result));

    switch (load_result) {
    case Core::SystemResultStatus::ErrorGetLoader:
        LOG_CRITICAL(Frontend, "Failed to obtain loader for {}!", filepath);
        return -1;
    case Core::SystemResultStatus::ErrorLoader:
        LOG_CRITICAL(Frontend, "Failed to load ROM!");
        return -1;
    case Core::SystemResultStatus::ErrorNotInitialized:
        LOG_CRITICAL(Frontend, "CPUCore not initialized");
        return -1;
    case Core::SystemResultStatus::ErrorVideoCore:
        LOG_CRITICAL(Frontend, "Failed to initialize VideoCore!");
        return -1;
    case Core::SystemResultStatus::Success:
        break; // Expected case
    default:
        if (static_cast<u32>(load_result) >
            static_cast<u32>(Core::SystemResultStatus::ErrorLoader)) {
            const u16 loader_id = static_cast<u16>(Core::SystemResultStatus::ErrorLoader);
            const u16 error_id = static_cast<u16>(load_result) - loader_id;
            // Keys that exist but do not fit this game or firmware surface
            // here; an export names the keys folder instead of carrying on.
            using Loader::ResultStatus;
            switch (static_cast<ResultStatus>(error_id)) {
            // A wrong header key decrypts the NCA header to garbage, which the
            // loader reports as a bad header rather than as a key error.
            case ResultStatus::ErrorBadNCAHeader:
            case ResultStatus::ErrorMissingProductionKeyFile:
            case ResultStatus::ErrorMissingHeaderKey:
            case ResultStatus::ErrorIncorrectHeaderKey:
            case ResultStatus::ErrorMissingTitlekey:
            case ResultStatus::ErrorMissingTitlekek:
            case ResultStatus::ErrorInvalidRightsID:
            case ResultStatus::ErrorMissingKeyAreaKey:
            case ResultStatus::ErrorIncorrectKeyAreaKey:
            case ResultStatus::ErrorIncorrectTitlekeyOrTitlekek:
                if (!installed_nand.empty()) {
                    const auto keys_dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::KeysDir);
                    ReportExportProblem(
                        "Keys problem",
                        fmt::format("The game could not be decrypted with the keys in {} ({}).\n\n"
                                    "Install current keys in suyu (Tools > Install Decryption "
                                    "Keys), then start the game again. Copying prod.keys and "
                                    "title.keys into that folder works just as well.",
                                    Common::FS::PathToUTF8String(keys_dir),
                                    static_cast<ResultStatus>(error_id)),
                        keys_dir, false, suyu_exe, "-install-keys", "Install keys");
                    return 2;
                }
                break;
            default:
                break;
            }
            LOG_CRITICAL(Frontend,
                         "While attempting to load the ROM requested, an error occurred. Please "
                         "refer to the suyu wiki for more information or the suyu discord for "
                         "additional help.\n\nError Code: {:04X}-{:04X}\nError Description: {}",
                         loader_id, error_id, static_cast<Loader::ResultStatus>(error_id));
        }
        return -1;
    }

#ifdef USE_DISCORD_PRESENCE
    // Both suyu-cmd and the renamed executable shipped by a game export run
    // through this path, including when Steam starts the exported shortcut.
#ifdef _WIN32
    wchar_t discord_exe_buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, discord_exe_buffer, MAX_PATH);
    const std::filesystem::path discord_exe_path(discord_exe_buffer);
#else
    const std::filesystem::path discord_exe_path(argv[0]);
#endif
    const DiscordPackageSettings discord_settings =
        ReadDiscordIni(discord_exe_path.parent_path());
    const bool discord_enabled = discord_settings.enabled;
    if (!discord_enabled) {
        LOG_INFO(Frontend, "Discord presence disabled by discord.ini");
    }
    std::string discord_title;
    system.GetAppLoader().ReadTitle(discord_title);
    if (discord_title.empty()) {
        // Extracted ExeFS exports may not carry the control data used for a
        // title. Their executable is already named after the game by the exporter.
#ifdef _WIN32
        discord_title = Common::UTF16ToUTF8(discord_exe_path.stem().wstring());
#else
        discord_title = discord_exe_path.stem().string();
#endif
        if (discord_title == "suyu-cmd" || discord_title == "suyu-cmd-static") {
            discord_title = std::filesystem::path(filepath).stem().string();
        }
    }
    const auto limit_discord_text = [](std::string& value) {
        if (value.size() <= 128) {
            return;
        }
        size_t length = 128;
        while (length > 0 && (static_cast<unsigned char>(value[length]) & 0xC0) == 0x80) {
            --length;
        }
        value.resize(length);
    };
    limit_discord_text(discord_title);
    DiscordRichPresence discord_presence{};
    std::string discord_state = discord_title;
    if (discord_enabled) {
        DiscordEventHandlers discord_handlers{};
        // Share the Suyu application ID used by the Qt frontend.
        Discord_Initialize("1221314350216646828", &discord_handlers, 0, nullptr);
        discord_presence.details = "Playing a Nintendo Switch game";
        discord_presence.state = discord_state.c_str();
        // Discord proxies an https image given as the key. The suyu logo then moves to the
        // small image, as in the Qt frontend.
        if (discord_settings.cover_url.empty()) {
            discord_presence.largeImageKey = "suyu_logo";
        } else {
            discord_presence.largeImageKey = discord_settings.cover_url.c_str();
            discord_presence.smallImageKey = "suyu_logo";
        }
        discord_presence.largeImageText = discord_title.c_str();
        discord_presence.startTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();
        LOG_INFO(Frontend, "Discord presence: publishing \"{}\" with image key {}", discord_title,
                 discord_presence.largeImageKey);
        Discord_UpdatePresence(&discord_presence);
    }
    SCOPE_EXIT {
        if (discord_enabled) {
            Discord_ClearPresence();
            Discord_Shutdown();
        }
    };
#endif

    if (use_multiplayer) {
        if (auto member = system.GetRoomNetwork().GetRoomMember().lock()) {
            member->BindOnChatMessageReceived(OnMessageReceived);
            member->BindOnStatusMessageReceived(OnStatusMessageReceived);
            member->BindOnStateChanged(OnStateChanged);
            member->BindOnError(OnNetworkError);
            LOG_DEBUG(Network, "Start connection to {}:{} with nickname {}", address, port,
                      nickname);
            member->Join(nickname, address.c_str(), port, 0, Network::NoPreferredIP, password);
        } else {
            LOG_ERROR(Network, "Could not access RoomMember");
            return 0;
        }
    }

    // Core is loaded, start the GPU (makes the GPU contexts current to this thread)
    system.GPU().Start();
    system.GetCpuManager().OnGpuReady();

    // Ordinary Vulkan CLI games can reuse recorded pipelines just like game exports.
    // Keep the old guard for other backends until their plain-CLI startup is tested.
    if (Settings::values.renderer_backend.GetValue() != Settings::RendererBackend::Vulkan &&
        !g_native_export_mode && installed_nand.empty() &&
        Settings::values.use_disk_shader_cache.GetValue()) {
        LOG_WARNING(Frontend,
                    "suyu-cmd: disabling disk shader cache for untested non-Vulkan CLI startup");
        Settings::values.use_disk_shader_cache.SetValue(false);
    }
    if (Settings::values.use_disk_shader_cache.GetValue()) {
        // Build the cached shaders on their own thread, as the Qt frontend does from its
        // emulation thread. The progress callback runs on the shader workers, so it only
        // records counts; the window belongs to this thread, which keeps it responding and
        // draws the progress. Closing the window stops the precompile.
        std::atomic<std::size_t> built{0};
        std::atomic<std::size_t> total{0};
        std::atomic<bool> finished{false};
        std::stop_source stop_loading;
        std::exception_ptr load_error;
        std::thread loader([&] {
            try {
                system.Renderer().ReadRasterizer()->LoadDiskResources(
                    system.GetApplicationProcessProgramID(), stop_loading.get_token(),
                    [&](VideoCore::LoadCallbackStage stage, size_t value, size_t count) {
                        if (stage == VideoCore::LoadCallbackStage::Build) {
                            total.store(count, std::memory_order_relaxed);
                            built.store(value, std::memory_order_relaxed);
                        }
                    });
            } catch (...) {
                load_error = std::current_exception();
            }
            finished.store(true, std::memory_order_release);
        });
        while (!finished.load(std::memory_order_acquire)) {
            emu_window->ShowBuildProgress(built.load(std::memory_order_relaxed),
                                          total.load(std::memory_order_relaxed));
            if (!emu_window->PumpEventsWhileLoading()) {
                stop_loading.request_stop();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        loader.join();
        emu_window->HideBuildProgress();
        try {
            if (load_error) {
                std::rethrow_exception(load_error);
            }
        } catch (const std::exception& e) {
            LOG_ERROR(Frontend, "Failed to load disk shader cache: {}", e.what());
        } catch (...) {
            LOG_ERROR(Frontend, "Failed to load disk shader cache due to unknown exception");
        }
    }

    system.RegisterExitCallback([&] {
        // Just exit right away.
        exit(0);
    });

#ifdef __unix__
    Common::Linux::StartGamemode();
#endif

    void(system.Run());
    if (system.DebuggerEnabled()) {
        system.InitializeDebugger();
    }

    // Periodic performance samples for benchmarking.
    //
    // Timing a replay end to end says little when a run can stall partway
    // and still finish: the stall is averaged in invisibly, and a run that
    // never finishes yields no number at all. A series lets a measurement
    // pick a window, and a stall shows up in it as a gap.
    //
    // On a thread of its own because the loop below blocks in WaitEvent:
    // samples driven from there would stop arriving exactly when the
    // emulator stops making progress, which is the case worth seeing. The
    // status sampler hands these counters no sample at all while this owns
    // them, so there is still only one reader of them.
    const bool perf_sampling = std::getenv("SUYU_CMD_PERF_SAMPLE") != nullptr;
    std::atomic<bool> perf_sampling_run{perf_sampling};
    std::thread perf_sampler;
    if (perf_sampling) {
        perf_sampler = std::thread([&system, &perf_sampling_run] {
            while (perf_sampling_run.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::seconds{1});
                if (!perf_sampling_run.load(std::memory_order_relaxed)) {
                    break;
                }
                const auto r = system.GetAndResetPerfStats();
                SuyuCmd::StoreNativePerfStats(r);
                LOG_INFO(Frontend,
                         "PERF game_fps={:.3f} system_fps={:.3f} frametime_ms={:.3f} "
                         "speed={:.4f}",
                         r.average_game_fps, r.system_fps, r.frametime * 1000.0,
                         r.emulation_speed);
            }
        });
    } else {
        LOG_INFO(Frontend,
                 "PERF sampling off: the window status refresh owns the perf counters");
    }

    const char* capture_dir_env = std::getenv("SUYU_CMD_CAPTURE_DIR");
    const std::filesystem::path capture_dir = capture_dir_env ? capture_dir_env : "";
    if (!capture_dir.empty()) {
        std::filesystem::create_directories(capture_dir);
    }
    const auto capture_start = std::chrono::steady_clock::now();
    const auto capture_seconds = [](const char* name, int fallback) {
        const char* value = std::getenv(name);
        return std::chrono::seconds{value ? std::max(1, std::atoi(value)) : fallback};
    };
    auto next_capture = capture_seconds("SUYU_CMD_CAPTURE_FIRST_SEC", 30);
    const auto capture_interval = capture_seconds("SUYU_CMD_CAPTURE_INTERVAL_SEC", 60);
    unsigned capture_index = 0;
    while (emu_window->IsOpen()) {
        // The wait is bounded (see EmuWindow_SDL2::WaitEvent), so this loop also
        // runs with no input, which keeps a NetPlay join or leave current in Discord.
        emu_window->WaitEvent();
#ifdef USE_DISCORD_PRESENCE
        if (discord_enabled) {
            std::string next_state = discord_title;
            if (const auto member = system.GetRoomNetwork().GetRoomMember().lock();
                member && member->IsConnected()) {
                const auto room_name = member->GetRoomInformation().name;
                next_state = room_name.empty() ? "In a NetPlay room" : "NetPlay: " + room_name;
            }
            limit_discord_text(next_state);
            if (next_state != discord_state) {
                discord_state = std::move(next_state);
                discord_presence.state = discord_state.c_str();
                Discord_UpdatePresence(&discord_presence);
            }
        }
#endif
        if (capture_dir.empty() || system.Renderer().IsScreenshotPending() ||
            std::chrono::steady_clock::now() - capture_start < next_capture) {
            continue;
        }
        constexpr int width = 1280;
        constexpr int height = 720;
        auto pixels = std::make_shared<std::vector<u8>>(width * height * 4);
        const std::string output =
            (capture_dir / ("frame-" + std::to_string(++capture_index) + ".png")).string();
        system.Renderer().RequestScreenshot(
            pixels->data(),
            [pixels, output](bool invert_y) {
                std::vector<u8> rgba(pixels->size());
                for (int y = 0; y < height; ++y) {
                    const int source_y = invert_y ? height - 1 - y : y;
                    for (int x = 0; x < width; ++x) {
                        const size_t src = (static_cast<size_t>(source_y) * width + x) * 4;
                        const size_t dst = (static_cast<size_t>(y) * width + x) * 4;
                        rgba[dst] = (*pixels)[src + 2];
                        rgba[dst + 1] = (*pixels)[src + 1];
                        rgba[dst + 2] = (*pixels)[src];
                        rgba[dst + 3] = (*pixels)[src + 3];
                    }
                }
                LOG_INFO(Frontend, "CAPTURE {} success={}", output,
                         stbi_write_png(output.c_str(), width, height, 4, rgba.data(),
                                        width * 4) != 0);
            },
            Layout::DefaultFrameLayout(width, height));
        next_capture += capture_interval;
    }

    perf_sampling_run.store(false, std::memory_order_relaxed);
    if (perf_sampler.joinable()) {
        perf_sampler.join();
    }

    system.DetachDebugger();
    void(system.Pause());
    system.ShutdownMainProcess();

#ifdef __unix__
    Common::Linux::StopGamemode();
#endif

    detached_tasks.WaitForAllTasks();
    return 0;
    } catch (const std::exception& e) {
        LOG_CRITICAL(Frontend, "Unhandled fatal exception in suyu-cmd: {}", e.what());
        return -1;
    } catch (...) {
        LOG_CRITICAL(Frontend, "Unhandled unknown fatal exception in suyu-cmd");
        return -1;
    }
}
