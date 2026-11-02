// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/arm/recomp/recomp_guard_gen.h"

#include <algorithm>
#include <mutex>

namespace Core::RecompGuardGen {

namespace detail {
std::atomic<bool> g_watching{false};
std::atomic<bool> g_prelog{false};
}

namespace {

// The generated code reads and writes these words with 32/64-bit atomics of its
// own; the host goes through std::atomic views of the same objects.
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint64_t>) == sizeof(std::uint64_t));
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

using Runs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

struct ModuleState {
    Module m;
    bool sticky = false;
    bool pin_logged = false; // whether SetPinLogger's callback has fired for it
    std::uint64_t active_base = 0; // base at activation
    Runs pa_runs;                  // [pa, pa + size) backing the guarded span
};

// One live mapping, as Core::Memory made it: `size` bytes at `va` in `table`,
// backed by physical `pa`, with the guest write permission it has now.
struct MapRecord {
    const void* table;
    std::uint64_t va, size, pa;
    bool writable;
};

struct State {
    std::mutex lock;
    std::vector<ModuleState> modules;
    bool enabled = false;
    bool active = false;
    const void* active_table = nullptr;
    std::atomic<std::uint64_t> active_key{0};
    // Monotonic for the host process lifetime; 0 is never used, so a seen word
    // still at its zero initialiser never matches.
    std::uint32_t counter = 0;
    bool exhausted = false;
    // Every live mapping of every table created while watching, kept current by
    // the map, unmap and protect hooks. A table not in `tables` was created
    // before the modules were registered, so its log is incomplete.
    std::vector<MapRecord> map_log;
    std::vector<const void*> tables;
    bool map_log_overflow = false;
    // Raw pointers handed out before the first activation (table, va, size).
    std::vector<MapRecord> exposures;
    bool exposures_overflow = false;
    // Activations since registration; only the first can prove anything.
    unsigned activations = 0;
    std::uint64_t bumps[static_cast<unsigned>(Reason::Count)]{};
    // Diagnostic only; never cleared by SetModules/Forget.
    PinLogFn pin_logger;
    TableSeenFn table_seen_logger;
};

State& S() {
    static State state;
    return state;
}

std::uint64_t LoadBase(const Module& m) {
    return reinterpret_cast<const std::atomic<std::uint64_t>*>(m.base)->load(
        std::memory_order_relaxed);
}

void StoreWord(const Module& m, std::uint32_t value) {
    reinterpret_cast<std::atomic<std::uint32_t>*>(m.word)->store(value, std::memory_order_release);
}

bool Overlaps(std::uint64_t a, std::uint64_t a_size, std::uint64_t b, std::uint64_t b_size) {
    return a_size != 0 && b_size != 0 && a < b + b_size && b < a + a_size;
}

std::uint64_t SpanStart(const ModuleState& s) {
    return LoadBase(s.m) + s.m.code_lo;
}

std::uint64_t SpanSize(const ModuleState& s) {
    return s.m.code_end - s.m.code_lo;
}

bool OverlapsModuleVa(const ModuleState& s, std::uint64_t va, std::uint64_t size) {
    return Overlaps(va, size, SpanStart(s), SpanSize(s));
}

bool OverlapsModulePa(const ModuleState& s, std::uint64_t pa, std::uint64_t size) {
    return std::any_of(s.pa_runs.begin(), s.pa_runs.end(), [&](const auto& run) {
        return Overlaps(pa, size, run.first, run.second);
    });
}

// Caller holds the lock. Sets `module` sticky and, the first time this module
// ever goes sticky, reports why through the registered pin logger.
void MarkSticky(State& s, std::size_t index, ModuleState& module, PinCause cause,
                std::uint64_t addr) {
    module.sticky = true;
    if (!module.pin_logged) {
        module.pin_logged = true;
        if (s.pin_logger) {
            s.pin_logger(index, cause, addr);
        }
    }
}

// Caller holds the lock. Stores the new generation into every module word, or
// kVerifyAlways for a module that is not provably stable. Serialised by the
// lock, so each word's modification order follows bump order.
void Bump(State& s, Reason reason) {
    ++s.bumps[static_cast<unsigned>(reason)];
    if (!s.enabled) {
        return;
    }
    if (s.counter >= kVerifyAlways - 1) {
        s.exhausted = true;
    } else {
        ++s.counter;
    }
    for (std::size_t i = 0; i < s.modules.size(); ++i) {
        auto& module = s.modules[i];
        if (s.active && LoadBase(module.m) != module.active_base) {
            MarkSticky(s, i, module, PinCause::Rebased, 0); // rebased since activation
        }
        const bool verify_always = !s.active || s.exhausted || module.sticky;
        StoreWord(module.m, verify_always ? kVerifyAlways : s.counter);
    }
}

// Caller holds the lock. Makes every module `hit` selects sticky (reporting
// `addr` as the reason's address) and, if there was one, bumps so that its
// word drops to kVerifyAlways now.
template <typename Hit>
void StickyWhere(State& s, Reason reason, PinCause cause, Hit&& hit) {
    bool any = false;
    for (std::size_t i = 0; i < s.modules.size(); ++i) {
        auto& module = s.modules[i];
        std::uint64_t addr = 0;
        if (hit(module, addr)) {
            MarkSticky(s, i, module, cause, addr);
            any = true;
        }
    }
    if (any) {
        Bump(s, reason);
    }
}

// Caller holds the lock. Splits every record of `table` at the edges of
// [va, va + size) and hands each piece inside it to `inside`, which returns
// whether to keep it (possibly changed).
template <typename Inside>
void EditLog(State& s, const void* table, std::uint64_t va, std::uint64_t size, Inside&& inside) {
    if (std::none_of(s.map_log.begin(), s.map_log.end(), [&](const MapRecord& rec) {
            return rec.table == table && Overlaps(rec.va, rec.size, va, size);
        })) {
        return;
    }
    std::vector<MapRecord> out;
    out.reserve(s.map_log.size() + 2);
    const std::uint64_t end = va + size;
    for (const auto& rec : s.map_log) {
        if (rec.table != table || !Overlaps(rec.va, rec.size, va, size)) {
            out.push_back(rec);
            continue;
        }
        const std::uint64_t rec_end = rec.va + rec.size;
        if (rec.va < va) {
            out.push_back({rec.table, rec.va, va - rec.va, rec.pa, rec.writable});
        }
        const std::uint64_t lo = std::max(rec.va, va), hi = std::min(rec_end, end);
        MapRecord mid{rec.table, lo, hi - lo, rec.pa + (lo - rec.va), rec.writable};
        if (inside(mid)) {
            out.push_back(mid);
        }
        if (end < rec_end) {
            out.push_back({rec.table, end, rec_end - end, rec.pa + (end - rec.va), rec.writable});
        }
    }
    s.map_log.swap(out);
    if (s.map_log.size() > kMaxMapLog) {
        s.map_log_overflow = true;
    }
}

// Caller holds the lock. Whether [va, va + size) is wholly mapped in `table`
// without write permission; fills the physical runs behind it.
bool StableSpan(const State& s, const void* table, std::uint64_t va, std::uint64_t size,
                Runs& runs) {
    std::vector<MapRecord> parts;
    for (const auto& rec : s.map_log) {
        if (rec.table == table && Overlaps(rec.va, rec.size, va, size)) {
            parts.push_back(rec);
        }
    }
    std::sort(parts.begin(), parts.end(),
              [](const MapRecord& a, const MapRecord& b) { return a.va < b.va; });
    std::uint64_t at = va;
    const std::uint64_t end = va + size;
    for (const auto& rec : parts) {
        if (rec.va > at || rec.writable) {
            return false; // a hole, or code the guest may store to
        }
        const std::uint64_t hi = std::min(rec.va + rec.size, end);
        const std::uint64_t pa = rec.pa + (at - rec.va);
        if (!runs.empty() && runs.back().first + runs.back().second == pa) {
            runs.back().second += hi - at;
        } else {
            runs.emplace_back(pa, hi - at);
        }
        at = hi;
        if (at >= end) {
            return true;
        }
    }
    return false;
}

} // namespace

void SetPinLogger(PinLogFn fn) {
    auto& s = S();
    std::scoped_lock lk{s.lock};
    s.pin_logger = std::move(fn);
}

void SetTableSeenLogger(TableSeenFn fn) {
    auto& s = S();
    std::scoped_lock lk{s.lock};
    s.table_seen_logger = std::move(fn);
}

void SetModules(std::vector<Module> modules, bool enabled) {
    auto& s = S();
    std::scoped_lock lk{s.lock};
    s.modules.clear();
    for (const auto& m : modules) {
        StoreWord(m, kVerifyAlways);
        s.modules.push_back(ModuleState{m});
    }
    s.enabled = enabled && !s.modules.empty();
    s.active = false;
    s.active_table = nullptr;
    s.active_key.store(0, std::memory_order_release);
    s.map_log.clear();
    s.tables.clear();
    s.map_log_overflow = false;
    s.exposures.clear();
    s.exposures_overflow = false;
    s.activations = 0;
    detail::g_prelog.store(s.enabled, std::memory_order_release);
    detail::g_watching.store(s.enabled, std::memory_order_seq_cst);
}

void Forget() {
    auto& s = S();
    std::scoped_lock lk{s.lock};
    detail::g_watching.store(false, std::memory_order_seq_cst);
    detail::g_prelog.store(false, std::memory_order_release);
    s.exposures.clear();
    s.exposures_overflow = false;
    s.activations = 0;
    s.modules.clear();
    s.enabled = false;
    s.active = false;
    s.active_table = nullptr;
    s.active_key.store(0, std::memory_order_release);
    s.map_log.clear();
    s.tables.clear();
    s.map_log_overflow = false;
}

bool IsActive(std::uint64_t key) {
    return S().active_key.load(std::memory_order_acquire) == key;
}

void Activate(std::uint64_t key, const void* table, const WatchFn& watch) {
    auto& s = S();
    std::scoped_lock lk{s.lock};
    if (!s.enabled || s.active_key.load(std::memory_order_relaxed) == key) {
        return;
    }
    // Only a table whose every mapping the hooks saw can prove anything, and
    // only for the first process: raw pointers are recorded before the first
    // activation, not before a later one.
    const bool tracked = !s.map_log_overflow && !s.exposures_overflow && s.activations == 0 &&
                         std::find(s.tables.begin(), s.tables.end(), table) != s.tables.end();
    // Which of the four AND'ed conditions above actually failed, for
    // diagnostics only: `tracked` alone can't say which one.
    PinCause untracked_cause = PinCause::Untracked;
    if (s.map_log_overflow) {
        untracked_cause = PinCause::MapLogOverflow;
    } else if (s.exposures_overflow) {
        untracked_cause = PinCause::ExposuresOverflow;
    } else if (s.activations != 0) {
        untracked_cause = PinCause::NotFirstActivation;
    }
    ++s.activations;
    for (std::size_t i = 0; i < s.modules.size(); ++i) {
        auto& module = s.modules[i];
        module.active_base = LoadBase(module.m);
        module.pa_runs.clear();
        if (!tracked) {
            MarkSticky(s, i, module, untracked_cause, reinterpret_cast<std::uint64_t>(table));
            continue;
        }
        if (SpanSize(module) == 0 ||
            !StableSpan(s, table, SpanStart(module), SpanSize(module), module.pa_runs)) {
            MarkSticky(s, i, module, PinCause::Hole, SpanStart(module));
            continue;
        }
        // Any other live mapping of the same physical pages, in any table, is
        // an alias through which this code could change.
        for (const auto& rec : s.map_log) {
            const bool own =
                rec.table == table && Overlaps(rec.va, rec.size, SpanStart(module), SpanSize(module));
            if (!own && OverlapsModulePa(module, rec.pa, rec.size)) {
                MarkSticky(s, i, module, PinCause::AliasAtActivation, rec.va);
                break;
            }
        }
        if (module.sticky) {
            continue;
        }
        // A raw pointer into the span handed out before now may still be
        // written through.
        for (const auto& rec : s.exposures) {
            if (rec.table == table && OverlapsModuleVa(module, rec.va, rec.size)) {
                MarkSticky(s, i, module, PinCause::ExposedBeforeActivation, rec.va);
                break;
            }
        }
    }
    // Watch the stable spans before their words leave kVerifyAlways: from the
    // bump on, a store there must reach the host. Then end the pre-activation
    // pointer log; Core::Memory reads the watch words once it sees that.
    for (const auto& module : s.modules) {
        if (!module.sticky) {
            watch(SpanStart(module), SpanSize(module));
        }
    }
    s.exposures.clear();
    detail::g_prelog.store(false, std::memory_order_release);
    s.active = true;
    s.active_table = table;
    Bump(s, Reason::Activate);
    s.active_key.store(key, std::memory_order_release);
}

void OnMap(const void* table, std::uint64_t va, std::uint64_t size, std::uint64_t pa,
           bool writable) {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    // A new mapping replaces whatever the log had there.
    EditLog(s, table, va, size, [](MapRecord&) { return false; });
    if (s.map_log.size() < kMaxMapLog) {
        s.map_log.push_back({table, va, size, pa, writable});
    } else {
        s.map_log_overflow = true;
    }
    if (!s.active) {
        return;
    }
    StickyWhere(s, Reason::Map, PinCause::Map, [&](const ModuleState& module, std::uint64_t& addr) {
        if (table == s.active_table && OverlapsModuleVa(module, va, size)) {
            addr = va;
            return true;
        }
        if (OverlapsModulePa(module, pa, size)) {
            addr = pa;
            return true;
        }
        return false;
    });
}

void OnUnmap(const void* table, std::uint64_t va, std::uint64_t size) {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    EditLog(s, table, va, size, [](MapRecord&) { return false; });
    if (!s.active || table != s.active_table) {
        return;
    }
    StickyWhere(s, Reason::Unmap, PinCause::Unmap,
                [&](const ModuleState& module, std::uint64_t& addr) {
                    addr = va;
                    return OverlapsModuleVa(module, va, size);
                });
}

void OnProtect(const void* table, std::uint64_t va, std::uint64_t size, bool writable) {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    EditLog(s, table, va, size, [writable](MapRecord& rec) {
        rec.writable = writable;
        return true;
    });
    if (!s.active || table != s.active_table) {
        return;
    }
    bool any = false;
    for (std::size_t i = 0; i < s.modules.size(); ++i) {
        auto& module = s.modules[i];
        if (OverlapsModuleVa(module, va, size)) {
            any = true;
            if (writable) {
                MarkSticky(s, i, module, PinCause::Protect, va); // guest stores to it are not hooked
            }
        }
    }
    if (any) {
        Bump(s, Reason::Protect);
    }
}

void OnWatchedWrite(const void* table, std::uint64_t va, std::uint64_t size) {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    if (!s.active || table != s.active_table) {
        return;
    }
    // Called after the bytes are written, so any entry ordered after this bump
    // re-reads them and rejects a changed block exactly as ABI 5 would.
    const bool hit = std::any_of(s.modules.begin(), s.modules.end(), [&](const auto& module) {
        return OverlapsModuleVa(module, va, size);
    });
    if (hit) {
        Bump(s, Reason::CodeWrite);
    }
}

void OnPointerExposed(const void* table, std::uint64_t va, std::uint64_t size) {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    if (s.activations == 0) {
        if (s.exposures.size() < kMaxMapLog) {
            s.exposures.push_back({table, va, size, 0, true});
        } else {
            s.exposures_overflow = true;
        }
        return;
    }
    if (!s.active || table != s.active_table) {
        return;
    }
    StickyWhere(s, Reason::PointerExposed, PinCause::PointerExposed,
                [&](const ModuleState& module, std::uint64_t& addr) {
                    addr = va;
                    return OverlapsModuleVa(module, va, size);
                });
}

void OnJitFallback() {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    StickyWhere(s, Reason::JitFallback, PinCause::JitFallback,
                [](const ModuleState&, std::uint64_t&) { return true; });
}

void OnDeviceMap(const void* table, std::uint64_t va, std::uint64_t size) {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    if (!s.active || table != s.active_table) {
        return;
    }
    // Device writes land in the backing without any CPU hook.
    StickyWhere(s, Reason::DeviceMap, PinCause::DeviceMap,
                [&](const ModuleState& module, std::uint64_t& addr) {
                    addr = va;
                    return OverlapsModuleVa(module, va, size);
                });
}

void OnInvalidate(std::uint64_t va, std::uint64_t size) {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    // Outside every module span this cannot concern a guarded word: a module
    // with a writable, aliased or device-mapped page is already sticky.
    const bool hit = std::any_of(s.modules.begin(), s.modules.end(), [&](const auto& module) {
        return OverlapsModuleVa(module, va, size);
    });
    if (hit) {
        Bump(s, Reason::Invalidate);
    }
}

void OnInvalidateAll() {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    Bump(s, Reason::InvalidateAll);
}

void OnPageTableSwap(const void* table) {
    if (!Watching()) {
        return;
    }
    auto& s = S();
    std::scoped_lock lk{s.lock};
    // A reused address is a new table: drop what the log still says about it.
    s.map_log.erase(std::remove_if(s.map_log.begin(), s.map_log.end(),
                                   [table](const MapRecord& rec) { return rec.table == table; }),
                    s.map_log.end());
    if (std::find(s.tables.begin(), s.tables.end(), table) == s.tables.end()) {
        s.tables.push_back(table);
    }
    if (s.table_seen_logger) {
        s.table_seen_logger(table, s.tables.size());
    }
    Bump(s, Reason::PageTableSwap);
}

Stats GetStats() {
    auto& s = S();
    std::scoped_lock lk{s.lock};
    Stats out{};
    out.enabled = s.enabled;
    out.active = s.active;
    out.generation = s.exhausted ? kVerifyAlways : s.counter;
    out.modules = static_cast<std::uint32_t>(s.modules.size());
    out.sticky = static_cast<std::uint32_t>(
        std::count_if(s.modules.begin(), s.modules.end(), [](const auto& m) { return m.sticky; }));
    out.map_log = s.map_log.size();
    out.map_log_overflow = s.map_log_overflow;
    std::copy(std::begin(s.bumps), std::end(s.bumps), std::begin(out.bumps));
    return out;
}

} // namespace Core::RecompGuardGen
