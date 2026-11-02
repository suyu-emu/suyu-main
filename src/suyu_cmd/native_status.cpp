// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cmath>
#include <mutex>

#include <SDL3/SDL.h>

#include "core/arm/recomp/arm_recomp.h"
#include "core/core.h"
#include "core/perf_stats.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/am/am_types.h"
#include "common/settings.h"
#include "suyu_cmd/native_status.h"

namespace SuyuCmd {
namespace {

std::mutex g_snapshot_mutex;
NativeStatusSnapshot g_snapshot;
bool g_have_snapshot = false;
std::string g_launch_name;
std::string g_launch_version;
constexpr u64 kPerfSampleExpiryMs = 3000;

void AssignPerf(NativeStatusSnapshot& snapshot, const Core::PerfStatsResults& results) {
    snapshot.average_game_fps = results.average_game_fps;
    snapshot.system_fps = results.system_fps;
    snapshot.emulation_speed = results.emulation_speed;
    snapshot.frametime_ms =
        std::isfinite(results.frametime) ? results.frametime * 1000.0 : 0.0;
    snapshot.perf_available = std::isfinite(results.average_game_fps) &&
                              std::isfinite(results.system_fps) &&
                              std::isfinite(results.emulation_speed);
    snapshot.perf_sample_monotonic_ms = SDL_GetTicks();
}

std::string ReadDisplayVersion() {
    const std::string override = Settings::values.application_display_version_override.GetValue();
    if (!override.empty()) {
        return override;
    }
    const u32 version = Settings::values.application_version_override.GetValue();
    if (version == 0) {
        return {};
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "v%u.%u.%u", (version >> 24) & 0xFF, (version >> 16) & 0xFF,
                  (version >> 8) & 0xFF);
    return buf;
}

// The applet's actual engine is not exposed here. Preserve unknown rather than
// inferring its backend from the executable's available backends.
void CollectApplet(Kernel::KernelCore& kernel, NativeStatusSnapshot& s) {
    auto* application = kernel.ApplicationProcess();
    if (application == nullptr) {
        return;
    }

    const u64 application_pid = application->GetProcessId();
    for (const auto& handle : kernel.GetProcessList()) {
        Kernel::KProcess* process = handle.GetPointerUnsafe();
        if (process == nullptr || process->GetProcessId() == application_pid ||
            (process->GetState() != Kernel::KProcess::State::Running &&
             process->GetState() != Kernel::KProcess::State::RunningAttached)) {
            continue;
        }
        const u64 program_id = process->GetProgramId();
        if (program_id < static_cast<u64>(Service::AM::AppletProgramId::QLaunch) ||
            program_id > static_cast<u64>(Service::AM::AppletProgramId::MaxProgramId)) {
            continue;
        }

        switch (static_cast<Service::AM::AppletProgramId>(program_id)) {
        case Service::AM::AppletProgramId::MiiEdit:
            s.applet_name = "Mii editor";
            break;
        case Service::AM::AppletProgramId::SoftwareKeyboard:
            s.applet_name = "Software keyboard";
            break;
        case Service::AM::AppletProgramId::PhotoViewer:
            s.applet_name = "Photo viewer";
            break;
        case Service::AM::AppletProgramId::Controller:
            s.applet_name = "Controller";
            break;
        case Service::AM::AppletProgramId::ProfileSelect:
            s.applet_name = "Player select";
            break;
        case Service::AM::AppletProgramId::Error:
            s.applet_name = "Error";
            break;
        default:
            s.applet_name = "guest applet";
            break;
        }
        s.applet_running = true;
        break;
    }
}

} // namespace

NativeStatusSnapshot SampleNativeStatus(Core::System& system, bool consume_perf) {
    NativeStatusSnapshot s;
    s.sample_monotonic_ms = SDL_GetTicks();

    s.game_running = system.ApplicationProcess() != nullptr;
    s.title_id = system.GetApplicationProcessProgramID();
    (void)system.GetGameName(s.game_name);
    if (s.game_name.empty()) {
        std::scoped_lock lock{g_snapshot_mutex};
        // An extracted ExeFS carries no title, so the name the launch path gives stands in.
        s.game_name = g_launch_name.empty() ? "Unknown title" : g_launch_name;
    }
    s.display_version = ReadDisplayVersion();
    {
        std::scoped_lock lock{g_snapshot_mutex};
        if (!g_launch_version.empty()) {
            s.display_version = g_launch_version;
        }
    }

    const auto live = Core::GetRecompLiveStats();
    s.backend_active = live.backend_active;
    s.static_blocks = live.static_blocks;
    s.jit_available = live.jit_available;
    s.strict_requested = live.strict_mode;
    s.jit_transitions = live.jit_transitions;
    s.recomp_registered = Core::GetRecompLookup() != nullptr;
    s.dynarmic_backend = Settings::values.cpu_backend.GetValue() == Settings::CpuBackend::Dynarmic;
    s.speed_meaningful = !Settings::values.use_multi_core.GetValue();

    if (consume_perf) {
        AssignPerf(s, system.GetAndResetPerfStats());
    }

    CollectApplet(system.Kernel(), s);
    return s;
}

void StoreNativeStatusSnapshot(const NativeStatusSnapshot& snapshot) {
    std::scoped_lock lock{g_snapshot_mutex};
    const auto previous = g_snapshot;
    g_snapshot = snapshot;
    if (!g_snapshot.perf_available && snapshot.perf_sample_monotonic_ms == 0 &&
        g_have_snapshot && previous.perf_available &&
        SDL_GetTicks() - previous.perf_sample_monotonic_ms <= kPerfSampleExpiryMs) {
        g_snapshot.perf_available = true;
        g_snapshot.average_game_fps = previous.average_game_fps;
        g_snapshot.system_fps = previous.system_fps;
        g_snapshot.emulation_speed = previous.emulation_speed;
        g_snapshot.frametime_ms = previous.frametime_ms;
        g_snapshot.perf_sample_monotonic_ms = previous.perf_sample_monotonic_ms;
    }
    g_have_snapshot = true;
}

void SetNativeLaunchName(std::string name) {
    std::scoped_lock lock{g_snapshot_mutex};
    g_launch_name = std::move(name);
}

void SetNativeLaunchVersion(std::string version) {
    std::scoped_lock lock{g_snapshot_mutex};
    g_launch_version = std::move(version);
}

void StoreNativePerfStats(const Core::PerfStatsResults& results) {
    std::scoped_lock lock{g_snapshot_mutex};
    AssignPerf(g_snapshot, results);
    g_have_snapshot = true;
}

bool TryGetNativeStatusSnapshot(NativeStatusSnapshot& out) {
    std::scoped_lock lock{g_snapshot_mutex};
    if (!g_have_snapshot) {
        return false;
    }
    out = g_snapshot;
    if (out.perf_available && SDL_GetTicks() - out.perf_sample_monotonic_ms > kPerfSampleExpiryMs) {
        out.perf_available = false;
    }
    return true;
}

} // namespace SuyuCmd
