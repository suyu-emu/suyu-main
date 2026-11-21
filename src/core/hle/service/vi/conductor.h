// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <ankerl/unordered_dense.h>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"

#include "core/hle/service/vi/vsync_manager.h"

namespace Core {
class System;
}

namespace Core::Timing {
struct EventType;
}

namespace Service {
class Event;
}

namespace Service::VI {

class Container;
class DisplayList;

class Conductor {
public:
    explicit Conductor(Core::System& system, Container& container, DisplayList& displays);
    ~Conductor();

    //! A frontend that shows frames on its own display clock drives the guest's vsync instead of
    //! the 60 Hz timer. Set before the game loads; multicore only.
    static void SetFrontendDriven(bool driven);
    //! One frontend frame. True if it started a composition (the guest's swap interval skips some).
    static bool FrontendFrame();

    void LinkVsyncEvent(u64 display_id, Event* event);
    void UnlinkVsyncEvent(u64 display_id, Event* event);

private:
    void ProcessVsync();
    void VsyncThread(std::stop_token token);
    s64 GetNextTicks() const;
    bool OnFrontendFrame();

private:
    Core::System& m_system;
    Container& m_container;
    ankerl::unordered_dense::map<u64, VsyncManager> m_vsync_managers;
    std::shared_ptr<Core::Timing::EventType> m_event;
    Common::Event m_signal;
    std::jthread m_thread;

private:
    s32 m_swap_interval = 1;
    f32 m_compose_speed_scale = 1.0f;
    std::atomic<s32> m_frontend_swap_interval{1};
    s32 m_frontend_frames = 0;
};

} // namespace Service::VI
