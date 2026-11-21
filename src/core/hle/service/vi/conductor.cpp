// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/vi/conductor.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/display_list.h"
#include "core/hle/service/vi/vsync_manager.h"

#include <algorithm>
#include <mutex>

constexpr auto FrameNs = std::chrono::nanoseconds{1000000000 / 60};

namespace Service::VI {

namespace {
std::atomic<bool> g_frontend_driven{false};
std::mutex g_frontend_mutex;
Conductor* g_frontend_conductor = nullptr;
} // namespace

Conductor::Conductor(Core::System& system, Container& container, DisplayList& displays)
    : m_system(system), m_container(container) {
    displays.ForEachDisplay([&](Display& display) {
        m_vsync_managers.insert({display.GetId(), VsyncManager{}});
    });

    if (system.IsMulticore()) {
        m_event = Core::Timing::CreateEvent(
            "ScreenComposition",
            [this](s64 time,
                   std::chrono::nanoseconds ns_late) -> std::optional<std::chrono::nanoseconds> {
                m_signal.Set();
                return std::chrono::nanoseconds(this->GetNextTicks());
            });

        if (g_frontend_driven) {
            std::scoped_lock lock{g_frontend_mutex};
            g_frontend_conductor = this;
        } else {
            system.CoreTiming().ScheduleLoopingEvent(FrameNs, FrameNs, m_event);
        }
        m_thread = std::jthread([this](std::stop_token token) { this->VsyncThread(token); });
    } else {
        m_event = Core::Timing::CreateEvent(
            "ScreenComposition",
            [this](s64 time,
                   std::chrono::nanoseconds ns_late) -> std::optional<std::chrono::nanoseconds> {
                this->ProcessVsync();
                return std::chrono::nanoseconds(this->GetNextTicks());
            });

        system.CoreTiming().ScheduleLoopingEvent(FrameNs, FrameNs, m_event);
    }
}

Conductor::~Conductor() {
    {
        std::scoped_lock lock{g_frontend_mutex};
        if (g_frontend_conductor == this) {
            g_frontend_conductor = nullptr;
        }
    }

    m_system.CoreTiming().UnscheduleEvent(m_event);

    if (m_system.IsMulticore()) {
        m_thread.request_stop();
        m_signal.Set();
    }
}

void Conductor::LinkVsyncEvent(u64 display_id, Event* event) {
    if (auto it = m_vsync_managers.find(display_id); it != m_vsync_managers.end()) {
        it->second.LinkVsyncEvent(event);
    }
}

void Conductor::UnlinkVsyncEvent(u64 display_id, Event* event) {
    if (auto it = m_vsync_managers.find(display_id); it != m_vsync_managers.end()) {
        it->second.UnlinkVsyncEvent(event);
    }
}

void Conductor::ProcessVsync() {
    for (auto& [display_id, manager] : m_vsync_managers) {
        m_container.ComposeOnDisplay(&m_swap_interval, &m_compose_speed_scale, display_id);
        manager.SignalVsync(m_system.Kernel());
    }
    m_frontend_swap_interval.store(m_swap_interval, std::memory_order_relaxed);
}

void Conductor::SetFrontendDriven(bool driven) {
    g_frontend_driven = driven;
}

bool Conductor::FrontendFrame() {
    std::scoped_lock lock{g_frontend_mutex};
    return g_frontend_conductor && g_frontend_conductor->OnFrontendFrame();
}

bool Conductor::OnFrontendFrame() {
    if (++m_frontend_frames <
        std::max(1, m_frontend_swap_interval.load(std::memory_order_relaxed))) {
        return false;
    }
    m_frontend_frames = 0;
    m_signal.Set();
    return true;
}

void Conductor::VsyncThread(std::stop_token token) {
    Common::SetCurrentThreadName("VSyncThread");

    while (!token.stop_requested()) {
        m_signal.Wait();

        if (m_system.IsShuttingDown()) {
            return;
        }

        this->ProcessVsync();
    }
}

s64 Conductor::GetNextTicks() const {
    const auto& settings = Settings::values;
    auto speed_scale = 1.f;
    if (settings.use_multi_core.GetValue()) {
        if (settings.use_speed_limit.GetValue()) {
            // Scales the speed based on speed_limit setting on MC. SC is handled by
            // SpeedLimiter::DoSpeedLimiting.
            speed_scale = 100.f / Settings::SpeedLimit();
        } else {
            // Run at unlocked framerate.
            speed_scale = 0.01f;
        }
    }

    // Adjust by speed limit determined during composition.
    speed_scale /= m_compose_speed_scale;

    if (m_system.GetNVDECActive() && settings.use_video_framerate.GetValue()) {
        // Run at intended presentation rate during video playback.
        speed_scale = 1.f;
    }

    const f32 effective_fps = 60.f / static_cast<f32>(m_swap_interval);
    return static_cast<s64>(speed_scale * (1000000000.f / effective_fps));
}

} // namespace Service::VI
