// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <numeric>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/cpu_cache_affinity.h"

namespace Common {
namespace {

constexpr std::uint64_t MiB = 1ULL << 20;

std::vector<std::uint32_t> Range(std::uint32_t first, std::uint32_t last) {
    std::vector<std::uint32_t> cpus(last - first + 1);
    std::iota(cpus.begin(), cpus.end(), first);
    return cpus;
}

} // Anonymous namespace

TEST_CASE("CacheAffinity: symmetric CCDs leave affinity alone", "[common]") {
    const std::vector<CacheDomain> domains{{Range(0, 15), 32 * MiB}, {Range(16, 31), 32 * MiB}};
    const auto choice = ChooseLargestCacheDomain(domains);
    REQUIRE(choice.cpus.empty());
}

TEST_CASE("CacheAffinity: 9950X3D picks the stacked-cache CCD", "[common]") {
    const std::vector<CacheDomain> domains{{Range(0, 15), 96 * MiB}, {Range(16, 31), 32 * MiB}};
    const auto choice = ChooseLargestCacheDomain(domains);
    REQUIRE(choice.cpus == Range(0, 15));
    REQUIRE(choice.size_bytes == 96 * MiB);
}

TEST_CASE("CacheAffinity: stacked cache on the high CPUs", "[common]") {
    const std::vector<CacheDomain> domains{{Range(0, 15), 32 * MiB}, {Range(16, 31), 96 * MiB}};
    const auto choice = ChooseLargestCacheDomain(domains);
    REQUIRE(choice.cpus == Range(16, 31));
}

TEST_CASE("CacheAffinity: one report per CPU is merged", "[common]") {
    // Linux sysfs reports the shared cache once for every CPU that shares it.
    std::vector<CacheDomain> domains;
    for (std::uint32_t cpu = 0; cpu < 32; ++cpu) {
        domains.push_back(cpu < 16 ? CacheDomain{Range(0, 15), 96 * MiB}
                                   : CacheDomain{Range(16, 31), 32 * MiB});
    }
    REQUIRE(ChooseLargestCacheDomain(domains).cpus == Range(0, 15));
}

TEST_CASE("CacheAffinity: a single L3 leaves affinity alone", "[common]") {
    const std::vector<CacheDomain> domains{{Range(0, 15), 32 * MiB}};
    REQUIRE(ChooseLargestCacheDomain(domains).cpus.empty());
}

TEST_CASE("CacheAffinity: missing or malformed data leaves affinity alone", "[common]") {
    REQUIRE(ChooseLargestCacheDomain({}).cpus.empty());

    // Empty CPU list.
    std::vector<CacheDomain> domains{{{}, 96 * MiB}, {Range(16, 31), 32 * MiB}};
    REQUIRE(ChooseLargestCacheDomain(domains).cpus.empty());

    // Zero size.
    domains = {{Range(0, 15), 0}, {Range(16, 31), 32 * MiB}};
    REQUIRE(ChooseLargestCacheDomain(domains).cpus.empty());

    // A CPU claimed by two caches.
    domains = {{Range(0, 16), 96 * MiB}, {Range(16, 31), 32 * MiB}};
    REQUIRE(ChooseLargestCacheDomain(domains).cpus.empty());

    // The same cache reported with two sizes.
    domains = {{Range(0, 15), 96 * MiB}, {Range(0, 15), 32 * MiB}, {Range(16, 31), 32 * MiB}};
    REQUIRE(ChooseLargestCacheDomain(domains).cpus.empty());
}

TEST_CASE("CacheAffinity: a tie for the largest leaves affinity alone", "[common]") {
    const std::vector<CacheDomain> domains{
        {Range(0, 15), 96 * MiB}, {Range(16, 31), 96 * MiB}, {Range(32, 47), 32 * MiB}};
    REQUIRE(ChooseLargestCacheDomain(domains).cpus.empty());
}

TEST_CASE("CacheAffinity: more than 64 CPUs across processor groups", "[common]") {
    // Windows numbers CPUs group * 64 + bit; a domain can sit wholly in group 1.
    const std::vector<CacheDomain> domains{{Range(0, 31), 32 * MiB},
                                           {Range(32, 63), 32 * MiB},
                                           {Range(64, 95), 96 * MiB},
                                           {Range(96, 127), 32 * MiB}};
    REQUIRE(ChooseLargestCacheDomain(domains).cpus == Range(64, 95));

    // One that straddles the group boundary.
    const std::vector<CacheDomain> straddle{{Range(0, 47), 32 * MiB}, {Range(48, 95), 96 * MiB}};
    REQUIRE(ChooseLargestCacheDomain(straddle).cpus == Range(48, 95));

    // Symmetric and large.
    const std::vector<CacheDomain> symmetric{{Range(0, 63), 32 * MiB}, {Range(64, 127), 32 * MiB}};
    REQUIRE(ChooseLargestCacheDomain(symmetric).cpus.empty());
}

} // namespace Common
