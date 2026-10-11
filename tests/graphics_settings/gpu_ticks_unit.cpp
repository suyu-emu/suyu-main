// SPDX-License-Identifier: GPL-2.0-or-later
// Execute the production GetTicks method with a controllable clock and real settings.
#include <cstdlib>
#include <iostream>
#include <limits>
#include "common/settings.h"

struct FakeTiming {
    u64 ticks = 16384;
    u64 GetGPUTicks() const { return ticks; }
};
struct FakeSystem {
    FakeTiming timing;
    FakeTiming& CoreTiming() { return timing; }
};
struct TickProbe {
    FakeSystem& system;
#include "gpu_ticks_method.inc"
};

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main() {
    Settings::SetConfiguringGlobal(true);
    FakeSystem system;
    TickProbe probe{system};
    Settings::values.use_fast_gpu_time = false;
    Settings::values.fast_gpu_time = Settings::GpuOverclock::Medium;
    Check(probe.GetTicks() == 16384, "Fast GPU Time off must leave ticks unchanged");
    Settings::values.fast_gpu_time = Settings::GpuOverclock::High;
    Check(probe.GetTicks() == 16384, "off must ignore the stored acceleration level");
    Settings::values.use_fast_gpu_time = true;
    Settings::values.fast_gpu_time = Settings::GpuOverclock::Normal;
    Check(probe.GetTicks() == 16384, "Normal must leave ticks unchanged");
    Settings::values.fast_gpu_time = Settings::GpuOverclock::Medium;
    Check(probe.GetTicks() == 64, "enabled Medium must retain its existing divisor");
    Settings::values.fast_gpu_time = Settings::GpuOverclock::High;
    Check(probe.GetTicks() == 32, "enabled High must retain its existing divisor");
    system.timing.ticks = 511;
    Check(probe.GetTicks() == 0, "integer rounding at the High divisor boundary");
    Settings::values.fast_gpu_time = Settings::GpuOverclock::Medium;
    Check(probe.GetTicks() == 1, "integer rounding at the Medium divisor boundary");
    system.timing.ticks = std::numeric_limits<u64>::max();
    Check(probe.GetTicks() == 0x00ffffffffffffffULL, "large tick values must not overflow");
    Settings::values.use_fast_gpu_time = false;
    Check(probe.GetTicks() == std::numeric_limits<u64>::max(), "off preserves the full clock range");
    std::cout << "PASS: production GPU tick getter respects the off switch\n";
}
