// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string_view>

#include <fmt/format.h>

#include "common/cpu_cache_affinity.h"
#include "common/logging.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <filesystem>
#include <fstream>
#include <sched.h>
#endif

namespace Common {

namespace {

[[maybe_unused]] std::string FormatCpuList(const std::vector<std::uint32_t>& cpus) {
    std::string out;
    for (std::size_t i = 0; i < cpus.size();) {
        std::size_t j = i;
        while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) {
            ++j;
        }
        if (!out.empty()) {
            out += ',';
        }
        out += j == i ? fmt::format("{}", cpus[i]) : fmt::format("{}-{}", cpus[i], cpus[j]);
        i = j + 1;
    }
    return out;
}

std::uint64_t ToMiB(std::uint64_t bytes) {
    return bytes >> 20;
}

} // Anonymous namespace

CacheAffinityChoice ChooseLargestCacheDomain(std::span<const CacheDomain> domains) {
    CacheAffinityChoice choice;
    if (domains.empty()) {
        choice.reason = "no L3 cache information";
        return choice;
    }

    // Normalise, merge repeated reports of one cache, and reject anything that
    // does not describe disjoint caches.
    std::vector<CacheDomain> unique;
    for (const auto& domain : domains) {
        CacheDomain normal{domain.cpus, domain.size_bytes};
        std::sort(normal.cpus.begin(), normal.cpus.end());
        normal.cpus.erase(std::unique(normal.cpus.begin(), normal.cpus.end()), normal.cpus.end());
        if (normal.cpus.empty() || normal.size_bytes == 0) {
            choice.reason = "malformed L3 cache information";
            return choice;
        }
        const auto same = std::find_if(unique.begin(), unique.end(), [&](const CacheDomain& d) {
            return d.cpus == normal.cpus;
        });
        if (same != unique.end()) {
            if (same->size_bytes != normal.size_bytes) {
                choice.reason = "malformed L3 cache information";
                return choice;
            }
            continue;
        }
        unique.push_back(std::move(normal));
    }

    std::map<std::uint32_t, std::size_t> owner;
    for (std::size_t i = 0; i < unique.size(); ++i) {
        for (const auto cpu : unique[i].cpus) {
            if (!owner.emplace(cpu, i).second) {
                choice.reason = "malformed L3 cache information";
                return choice;
            }
        }
    }

    if (unique.size() == 1) {
        choice.reason = "only one L3 cache";
        return choice;
    }

    const auto [min_it, max_it] = std::minmax_element(
        unique.begin(), unique.end(),
        [](const CacheDomain& a, const CacheDomain& b) { return a.size_bytes < b.size_bytes; });
    if (min_it->size_bytes == max_it->size_bytes) {
        choice.reason = fmt::format("all {} L3 caches are the same size", unique.size());
        return choice;
    }
    const auto largest = max_it->size_bytes;
    if (std::count_if(unique.begin(), unique.end(), [&](const CacheDomain& d) {
            return d.size_bytes == largest;
        }) > 1) {
        choice.reason = "more than one L3 cache shares the largest size";
        return choice;
    }

    choice.cpus = max_it->cpus;
    choice.size_bytes = largest;
    choice.reason = fmt::format("largest of {} L3 caches (smallest {} MB)", unique.size(),
                                ToMiB(min_it->size_bytes));
    return choice;
}

#if defined(_WIN32)

std::vector<CacheDomain> ReadHostCacheDomains() {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &length);
    if (length == 0) {
        return {};
    }
    std::vector<char> buffer(length);
    if (!GetLogicalProcessorInformationEx(
            RelationCache,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length)) {
        return {};
    }

    std::vector<CacheDomain> domains;
    for (DWORD offset = 0; offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= length;) {
        const auto* entry =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (entry->Size == 0) {
            break;
        }
        const auto& cache = entry->Cache;
        if (entry->Relationship == RelationCache && cache.Level == 3 &&
            cache.Type == CacheUnified) {
            CacheDomain domain{{}, cache.CacheSize};
            const WORD groups = cache.GroupCount == 0 ? 1 : cache.GroupCount;
            for (WORD g = 0; g < groups; ++g) {
                const GROUP_AFFINITY& affinity = cache.GroupMasks[g];
                for (std::uint32_t bit = 0; bit < 64; ++bit) {
                    if ((static_cast<std::uint64_t>(affinity.Mask) >> bit) & 1) {
                        domain.cpus.push_back(affinity.Group * 64U + bit);
                    }
                }
            }
            domains.push_back(std::move(domain));
        }
        offset += entry->Size;
    }
    return domains;
}

#elif defined(__linux__) && !defined(__ANDROID__)

namespace {

std::string ReadLine(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return line;
}

// "0-7,16-23" -> {0..7, 16..23}. Empty on anything unexpected.
std::vector<std::uint32_t> ParseCpuList(std::string_view text) {
    std::vector<std::uint32_t> cpus;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto part = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        const auto dash = part.find('-');
        char* end = nullptr;
        const std::string first_text{part.substr(0, dash)};
        const auto first = std::strtoul(first_text.c_str(), &end, 10);
        if (first_text.empty() || *end != '\0') {
            return {};
        }
        auto last = first;
        if (dash != std::string_view::npos) {
            const std::string last_text{part.substr(dash + 1)};
            last = std::strtoul(last_text.c_str(), &end, 10);
            if (last_text.empty() || *end != '\0' || last < first) {
                return {};
            }
        }
        for (auto cpu = first; cpu <= last; ++cpu) {
            cpus.push_back(static_cast<std::uint32_t>(cpu));
        }
    }
    return cpus;
}

} // Anonymous namespace

std::vector<CacheDomain> ReadHostCacheDomains() {
    namespace fs = std::filesystem;
    std::vector<CacheDomain> domains;
    std::error_code ec;
    for (const auto& cpu_dir : fs::directory_iterator("/sys/devices/system/cpu", ec)) {
        const auto name = cpu_dir.path().filename().string();
        if (name.size() < 4 || name.compare(0, 3, "cpu") != 0 ||
            name.find_first_not_of("0123456789", 3) != std::string::npos) {
            continue;
        }
        std::error_code cache_ec;
        for (const auto& index : fs::directory_iterator(cpu_dir.path() / "cache", cache_ec)) {
            if (ReadLine(index.path() / "level") != "3" ||
                ReadLine(index.path() / "type") != "Unified") {
                continue;
            }
            const auto size_text = ReadLine(index.path() / "size");
            char* end = nullptr;
            std::uint64_t size = std::strtoull(size_text.c_str(), &end, 10);
            if (*end == 'K') {
                size <<= 10;
            } else if (*end == 'M') {
                size <<= 20;
            }
            domains.push_back({ParseCpuList(ReadLine(index.path() / "shared_cpu_list")), size});
        }
    }
    return domains;
}

#else

std::vector<CacheDomain> ReadHostCacheDomains() {
    return {};
}

#endif

void ApplyLargestCacheAffinity(bool enabled) {
    const char* env = std::getenv("SUYU_CACHE_AFFINITY");
    const std::string_view env_value = env ? env : "";
    if (env_value == "0" || env_value == "1") {
        enabled = env_value == "1";
    }

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    // The GUI loads one game after another in the same process; remember what
    // the affinity was before this pinned it, so turning the setting off undoes
    // it and turning it on again does not mistake its own pin for a user's.
    static bool pinned = false;
#if defined(_WIN32)
    static DWORD_PTR original_mask = 0;
#else
    static cpu_set_t original_set{};

    const auto set_all_threads = [](const cpu_set_t& set) {
        // sched_setaffinity is per thread; the GUI and loader threads already
        // exist, so apply it to every one of them.
        bool ok = sched_setaffinity(0, sizeof(set), &set) == 0;
        std::error_code ec;
        for (const auto& task : std::filesystem::directory_iterator("/proc/self/task", ec)) {
            const auto tid = std::atoi(task.path().filename().c_str());
            if (tid > 0) {
                sched_setaffinity(tid, sizeof(set), &set);
            }
        }
        return ok;
    };
#endif

    if (pinned) {
        if (enabled) {
            LOG_INFO(Common, "Cache affinity: process is still on the largest-L3 cores");
            return;
        }
#if defined(_WIN32)
        SetProcessAffinityMask(GetCurrentProcess(), original_mask);
#else
        set_all_threads(original_set);
#endif
        pinned = false;
        LOG_INFO(Common, "Cache affinity: disabled, restored the previous CPU affinity");
        return;
    }

    if (!enabled) {
        LOG_INFO(Common, "Cache affinity: off (setting or SUYU_CACHE_AFFINITY=0)");
        return;
    }

    const auto domains = ReadHostCacheDomains();
    const auto choice = ChooseLargestCacheDomain(domains);
    if (choice.cpus.empty()) {
        LOG_INFO(Common, "Cache affinity: no change, {}", choice.reason);
        return;
    }
    const auto cpu_text = FormatCpuList(choice.cpus);

#if defined(_WIN32)
    if (GetActiveProcessorGroupCount() > 1) {
        LOG_INFO(Common, "Cache affinity: no change, more than one processor group");
        return;
    }
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        LOG_INFO(Common, "Cache affinity: no change, could not read the process affinity");
        return;
    }
    if (process_mask != system_mask) {
        LOG_INFO(Common, "Cache affinity: no change, affinity already restricted to {:#x}",
                 static_cast<std::uint64_t>(process_mask));
        return;
    }
    DWORD_PTR mask = 0;
    for (const auto cpu : choice.cpus) {
        if (cpu >= 64) {
            LOG_INFO(Common, "Cache affinity: no change, CPUs outside processor group 0");
            return;
        }
        mask |= DWORD_PTR{1} << cpu;
    }
    if ((mask & system_mask) != mask || mask == system_mask) {
        LOG_INFO(Common, "Cache affinity: no change, CPUs {} do not narrow the system mask {:#x}",
                 cpu_text, static_cast<std::uint64_t>(system_mask));
        return;
    }
    if (!SetProcessAffinityMask(GetCurrentProcess(), mask)) {
        LOG_WARNING(Common, "Cache affinity: SetProcessAffinityMask failed ({})", GetLastError());
        return;
    }
    original_mask = process_mask;
#else
    cpu_set_t current{};
    if (sched_getaffinity(0, sizeof(current), &current) != 0) {
        LOG_INFO(Common, "Cache affinity: no change, could not read the process affinity");
        return;
    }
    // Restricted means some CPU the caches report is not allowed already.
    for (const auto& domain : domains) {
        for (const auto cpu : domain.cpus) {
            if (cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &current)) {
                LOG_INFO(Common, "Cache affinity: no change, affinity already restricted");
                return;
            }
        }
    }
    cpu_set_t wanted{};
    CPU_ZERO(&wanted);
    for (const auto cpu : choice.cpus) {
        CPU_SET(cpu, &wanted);
    }
    if (!set_all_threads(wanted)) {
        LOG_WARNING(Common, "Cache affinity: sched_setaffinity failed");
        return;
    }
    original_set = current;
#endif
    pinned = true;
    LOG_INFO(Common, "Cache affinity: pinned to CPUs {} ({} MB L3), {}", cpu_text,
             ToMiB(choice.size_bytes), choice.reason);
#else
    (void)enabled;
    LOG_INFO(Common, "Cache affinity: no change, not supported on this platform");
#endif
}

} // namespace Common
