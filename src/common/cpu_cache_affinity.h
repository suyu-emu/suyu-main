// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Confining the process to the cores that share the largest L3 cache.
//
// On a part with stacked cache on one die only (Ryzen 9 7950X3D/9950X3D), the
// emulated CPU threads run measurably faster on the large-cache die: static
// TOTK went from 32.3 to 44.4 fps on a 9950X3D, and the profile shows the gain
// is instruction-fetch misses served from L3. On a part where every L3 is the
// same size there is nothing to choose, so this does nothing there.
//
// The choice is a pure function so the interesting layouts can be tested on any
// machine; reading the topology and applying the mask are separate.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Common {

// One L3 cache and the logical processors sharing it. CPU numbers are global:
// on Windows, group * 64 + bit; on Linux, the kernel's CPU number.
struct CacheDomain {
    std::vector<std::uint32_t> cpus;
    std::uint64_t size_bytes{};
};

struct CacheAffinityChoice {
    // Empty when the affinity should be left alone.
    std::vector<std::uint32_t> cpus;
    std::uint64_t size_bytes{};
    std::string reason;
};

// Picks the domain with the strictly largest L3. Leaves the affinity alone when
// there is no data, only one L3, every L3 is the same size, two domains tie for
// the largest, or the data is inconsistent (an empty domain, a zero size, or a
// CPU claimed by two different caches). Duplicate reports of one cache are
// merged.
CacheAffinityChoice ChooseLargestCacheDomain(std::span<const CacheDomain> domains);

// Reads the host's L3 topology. Empty where it is unknown or unsupported.
std::vector<CacheDomain> ReadHostCacheDomains();

// Confines the whole process to the largest-L3 cores if the choice above says
// so, the setting allows it, and nothing else has already restricted the
// process's affinity. SUYU_CACHE_AFFINITY=0/1 overrides the setting. Call it
// before the emulated CPU threads are created. Logs one line either way.
void ApplyLargestCacheAffinity(bool enabled);

} // namespace Common
