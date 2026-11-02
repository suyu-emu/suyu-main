// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace Core {
class System;
struct PerfStatsResults;
}

namespace SuyuCmd {

/// What the main game process is really executing on.
///
/// `Pending` exists because registration, backend selection and actual
/// execution are three different states: a recompiled image can be registered
/// while the application process has not yet been handed it. Reporting that as
/// static AOT would be a claim about something that has not happened.
enum class NativeBackendClass {
    Pending,
    StrictAot,
    HybridAot,
    DynarmicJit,
    Nce,
    Unknown,
};

/// One timestamped reading of everything the status display needs.
///
/// Filled by exactly one sampler per process. The title, the F12 panel and any
/// benchmark read the same snapshot; none of them sample independently, because
/// the underlying performance counters are destructive to read.
struct NativeStatusSnapshot {
    std::string game_name;
    std::string display_version;
    std::uint64_t title_id{};

    bool game_running{};

    // Performance is published by whichever sampler owns the counters and
    // expires if that sampler stops reporting.
    bool perf_available{};
    std::uint64_t perf_sample_monotonic_ms{};
    double average_game_fps{};
    double system_fps{};
    double emulation_speed{};
    double frametime_ms{};

    bool recomp_registered{};
    bool backend_active{};
    std::uint64_t static_blocks{};
    bool jit_available{};
    bool strict_requested{};
    std::uint64_t jit_transitions{};
    // The configured CPU backend is Dynarmic, which is what runs the game when
    // no recompiled image is registered (a JIT baseline package).
    bool dynarmic_backend{};
    // In multicore mode guest time follows the host clock, so the emulation speed reads ~100%
    // whatever the frame rate; it only means something in single-core mode.
    bool speed_meaningful{};

    // Filled in by the window's status refresh alone: ShaderNotify's reporting
    // bookkeeping is not synchronised, so exactly one thread may query it.
    int shaders_building{};

    bool applet_running{};
    std::string applet_name;
    NativeBackendClass applet_backend{NativeBackendClass::Unknown};

    std::uint64_t sample_monotonic_ms{};
};

inline const char* NativeBackendName(NativeBackendClass kind) {
    switch (kind) {
    case NativeBackendClass::Pending:
        return "PENDING";
    case NativeBackendClass::StrictAot:
        return "suyu static AOT";
    case NativeBackendClass::HybridAot:
        return "suyu Hybrid JIT + AOT";
    case NativeBackendClass::DynarmicJit:
        return "suyu Dynarmic JIT";
    case NativeBackendClass::Nce:
        return "NCE";
    default:
        return "UNKNOWN";
    }
}

inline NativeBackendClass ClassifyGameBackend(const NativeStatusSnapshot& s) {
    if (!s.game_running) {
        return NativeBackendClass::Pending;
    }
    if (!s.recomp_registered) {
        return s.dynarmic_backend ? NativeBackendClass::DynarmicJit : NativeBackendClass::Unknown;
    }
    if (!s.backend_active || s.static_blocks == 0) {
        return NativeBackendClass::Pending;
    }
    return s.strict_requested ? NativeBackendClass::StrictAot : NativeBackendClass::HybridAot;
}

/// `strict_requested` is the policy asked for, `jit_available` is what the build
/// can do. A JIT-capable binary running a strict game is not a JIT-free
/// executable, so the two are never folded into one word.
inline std::string GameBackendQualifier(const NativeStatusSnapshot& s) {
    const NativeBackendClass kind = ClassifyGameBackend(s);
    if (kind == NativeBackendClass::StrictAot) {
        return s.jit_available ? " (JIT-capable)" : " (no-JIT build)";
    }
    return {};
}

inline std::string FormatNativeTitle(const NativeStatusSnapshot& s) {
    std::string out;
    out += s.game_name.empty() ? std::string{"Game"} : s.game_name;
    if (!s.display_version.empty()) {
        out += " ";
        out += s.display_version;
    }
    out += " | ";
    out += NativeBackendName(ClassifyGameBackend(s));
    out += GameBackendQualifier(s);

    if (!s.perf_available) {
        out += s.speed_meaningful ? " | FPS: -- | speed: --" : " | FPS: --";
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf), " | %.1f FPS (%.1f ms)", s.average_game_fps,
                      s.frametime_ms);
        out += buf;
        if (s.speed_meaningful) {
            std::snprintf(buf, sizeof(buf), " | %.0f%%", s.emulation_speed * 100.0);
            out += buf;
        }
    }

    // Shown only while it is happening, so a stutter can be matched to it.
    if (s.shaders_building > 0) {
        out += " | Building " + std::to_string(s.shaders_building) +
               (s.shaders_building == 1 ? " shader" : " shaders");
    }

    if (s.recomp_registered) {
        char jit[96];
        std::snprintf(jit, sizeof(jit), " | JIT transitions: %llu",
                      static_cast<unsigned long long>(s.jit_transitions));
        out += jit;
    }
    out += " | F12 Controls";
    return out;
}

/// Samples the live state. The SDL thread alone calls with `consume_perf=true`.
/// While benchmarking owns the counters, it calls with false and the benchmark
/// sampler publishes performance independently.
NativeStatusSnapshot SampleNativeStatus(Core::System& system, bool consume_perf);
void SetNativeLaunchName(std::string name);
void SetNativeLaunchVersion(std::string version);
void StoreNativePerfStats(const Core::PerfStatsResults& results);

void StoreNativeStatusSnapshot(const NativeStatusSnapshot& snapshot);
bool TryGetNativeStatusSnapshot(NativeStatusSnapshot& out);

} // namespace SuyuCmd
