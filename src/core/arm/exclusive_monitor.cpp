// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#if (defined(ARCHITECTURE_x86_64) || defined(ARCHITECTURE_arm64)) && !defined(SUYU_NO_JIT)
#include "core/arm/dynarmic/dynarmic_exclusive_monitor.h"
#endif
#include "core/arm/exclusive_monitor.h"
#include "core/arm/standalone_exclusive_monitor.h"
#include "core/memory.h"

namespace Core {

ExclusiveMonitor::~ExclusiveMonitor() = default;

std::unique_ptr<Core::ExclusiveMonitor> MakeExclusiveMonitor(Memory::Memory& memory,
                                                             std::size_t num_cores) {
#ifdef SUYU_NO_JIT
    // Every process builds one of these regardless of which engine runs, so
    // while dynarmic owns the only implementation the library cannot be
    // unlinked - this is what breaks that.
    return std::make_unique<Core::StandaloneExclusiveMonitor>(memory, num_cores);
#elif defined(ARCHITECTURE_x86_64) || defined(ARCHITECTURE_arm64)
    return std::make_unique<Core::DynarmicExclusiveMonitor>(memory, num_cores);
#else
    return std::make_unique<Core::StandaloneExclusiveMonitor>(memory, num_cores);
#endif
}

} // namespace Core
