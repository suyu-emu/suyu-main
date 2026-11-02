// SPDX-FileCopyrightText: Copyright 2025 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include <fmt/format.h>

#include "core/hle/kernel/k_lock_trace.h"

namespace Kernel::LockTrace {
namespace {

struct Entry {
    u64 seq;
    u64 addr;
    u32 tid;
    u32 a;
    u32 b;
    u32 c;
    Ev kind;
};

// Large enough to hold every supervisor call a stalled run makes, so this is an
// audit rather than a sample. 64K entries is 2 MB.
constexpr std::size_t kCapacity = 1u << 16;

std::mutex g_lock;
std::array<Entry, kCapacity> g_ring{};
u64 g_next = 0; // Total ever recorded; the ring holds the last kCapacity of them.

const char* Name(Ev kind) {
    switch (kind) {
    case Ev::LockEnter: return "lock-enter";
    case Ev::LockExit: return "lock-exit ";
    case Ev::Unlock: return "unlock    ";
    case Ev::Await: return "await     ";
    case Ev::AwaitExit: return "await-exit";
    case Ev::Signal: return "signal    ";
    }
    return "?         ";
}

std::string Render(const Entry& e) {
    return fmt::format("      #{:<6} tid={:<4} {} addr={:#x} a={:#010x} b={:#010x} c={:#010x}\n",
                       e.seq, e.tid, Name(e.kind), e.addr, e.a, e.b, e.c);
}

} // namespace

bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("SUYU_LOCK_TRACE");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

void Record(Ev kind, u32 tid, u64 addr, u32 a, u32 b, u32 c) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock guard{g_lock};
    const u64 seq = g_next++;
    g_ring[seq % kCapacity] = Entry{seq, addr, tid, a, b, c, kind};
}

std::string DumpForAddress(u64 addr) {
    if (!Enabled()) {
        return {};
    }
    std::scoped_lock guard{g_lock};
    const u64 first = g_next > kCapacity ? g_next - kCapacity : 0;
    std::string out;
    std::size_t shown = 0;
    for (u64 i = first; i < g_next; ++i) {
        const Entry& e = g_ring[i % kCapacity];
        if (e.addr != addr) {
            continue;
        }
        out += Render(e);
        ++shown;
    }
    if (shown == 0) {
        out += "      (no lock events ever recorded for this address)\n";
    }
    return out;
}

std::string DumpSummary(std::size_t tail) {
    if (!Enabled()) {
        return {};
    }
    std::scoped_lock guard{g_lock};
    const u64 first = g_next > kCapacity ? g_next - kCapacity : 0;
    std::array<u64, 6> counts{};
    for (u64 i = first; i < g_next; ++i) {
        counts[static_cast<std::size_t>(g_ring[i % kCapacity].kind)]++;
    }
    std::string out = fmt::format(
        "    {} lock events recorded ({} retained)\n"
        "      lock-enter={} lock-exit={} unlock={} await={} await-exit={} signal={}\n",
        g_next, g_next - first, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]);
    const u64 tail_start = g_next > tail ? g_next - tail : first;
    out += fmt::format("    --- last {} lock events ---\n", g_next - tail_start);
    for (u64 i = tail_start < first ? first : tail_start; i < g_next; ++i) {
        out += Render(g_ring[i % kCapacity]);
    }
    return out;
}

} // namespace Kernel::LockTrace
