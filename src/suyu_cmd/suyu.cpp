// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
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
#include "common/fs/path_util.h"
#include "common/nvidia_flags.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/arm/recomp/arm_recomp.h"
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
};
#ifdef SUYU_CMD_STATIC_RECOMP
const SuyuRecompStaticModule* suyu_recomp_static_modules(unsigned* count);
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
#ifdef SUYU_CMD_STATIC_RECOMP
    {
        namespace FS = Common::FS;
#ifdef _WIN32
        wchar_t exe_w[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_w, MAX_PATH);
        const std::filesystem::path user_root =
            std::filesystem::path(exe_w).parent_path() / L"user";
#else
        const std::filesystem::path user_root =
            std::filesystem::path(argv[0]).parent_path() / "user";
#endif
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
        FS::SetSuyuPath(FS::SuyuPath::KeysDir, FS::GetAppDataRoamingDirectory() / "suyu" / "keys");
    }
#endif

    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();

    // mk8-recomp: --probe-isa <rom> prints the title's CPU architecture and
    // exits, before any config, window or emulation setup.
    for (int i = 1; i + 2 < argc; ++i) {
        if (std::string_view{argv[i]} == "--probe-isa-list") {
            return ProbeIsaList(argv[i + 1], argv[i + 2]);
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

    // Native recompiled CPU modules, in NSO load order: rtld(0), main(1),
    // subsdk0-N(2..N+1), sdk(last). Whichever way they arrive, registering any
    // of them makes ArmRecomp run the game's CPU natively instead of dynarmic.
    struct RecompModule {
        Core::RecompBlockFn (*lookup)(u64){};
        void (*set_base)(u64){};
    };
    static std::vector<RecompModule> s_recomp_modules;

    // Preferred path: modules compiled straight into this executable. Nothing
    // to find on disk, nothing to load, and no version skew between the exe and
    // its modules.
#ifdef SUYU_CMD_STATIC_RECOMP
    {
        unsigned count = 0;
        const SuyuRecompStaticModule* mods = suyu_recomp_static_modules(&count);
        for (unsigned i = 0; i < count; ++i) {
            s_recomp_modules.push_back({mods[i].lookup, mods[i].set_base});
            LOG_INFO(Frontend, "Static recompiled module [{}] {} — ArmRecomp active", i,
                     mods[i].name ? mods[i].name : "?");
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
                s_recomp_modules.push_back({lkp, sbf});
                LOG_INFO(Frontend, "Native recompiled module [{}] loaded from {} — ArmRecomp active",
                         s_recomp_modules.size() - 1, Common::UTF16ToUTF8(dll_name));
            }
        }
    }
#endif

    if (!s_recomp_modules.empty()) {
        // Combined lookup: try each module's lookup until one returns non-null.
        Core::SetRecompLookup([](u64 pc) -> Core::RecompBlockFn {
            for (const auto& m : s_recomp_modules) {
                if (auto fn = m.lookup(pc)) return fn;
            }
            return nullptr;
        });
        // Route base to the module at the same index in load order.
        // rtld=index0, main=index1, subsdk0=index2, ..., sdk=last.
        Core::SetRecompBaseSetter([](size_t index, const char*, u64 base) {
            if (index < s_recomp_modules.size() && s_recomp_modules[index].set_base) {
                s_recomp_modules[index].set_base(base);
            }
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
        const auto local_mods = exe_dir / "mods";
        std::error_code ec;
        std::filesystem::create_directories(local_mods, ec);
        if (std::filesystem::is_directory(local_mods)) {
            Common::FS::SetSuyuPath(Common::FS::SuyuPath::LoadDir, local_mods);
            LOG_INFO(Frontend, "Using local mod directory: {}", local_mods.string());
        }
        LOG_INFO(Frontend, "Keys directory (never bundled): {}",
                 Common::FS::GetSuyuPathString(Common::FS::SuyuPath::KeysDir));
    }

    LOG_INFO(Frontend, "suyu-cmd: Initializing system...");
    Core::System system{};
    system.Initialize();
    LOG_INFO(Frontend, "suyu-cmd: System initialized.");

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

#ifdef _WIN32
    Common::Windows::SetCurrentTimerResolutionToMaximum();
    system.CoreTiming().SetTimerResolutionNs(Common::Windows::GetCurrentTimerResolution());
#endif

    LOG_INFO(Frontend, "suyu-cmd: Window created, loading game...");
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
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
    LOG_INFO(Frontend, "suyu-cmd: Calling system.Load for '{}'...", filepath);
    const Core::SystemResultStatus load_result{system.Load(*emu_window, filepath, load_parameters)};
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

    // A game export is launched over and over by a player, always for the same
    // title, so the disk shader cache is the difference between a long black
    // screen on every single run and one slow first run. Keep it for exports
    // and keep the old blanket disable for the plain dev frontend, where the
    // startup instability it works around was originally seen.
    if (!g_native_export_mode && Settings::values.use_disk_shader_cache.GetValue()) {
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
