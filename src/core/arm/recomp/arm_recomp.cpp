// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include "common/logging/log.h"
#include "core/arm/debug.h"
#ifndef SUYU_NO_JIT
#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/arm/recomp/arm_recomp.h"
#include "core/core.h"
#include "core/hle/kernel/k_memory_block.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "core/recompiler/nso_dynamic.h"
#include "core/recompiler/nso_relocation_transaction.h"
#include "core/recompiler/nso_relocations.h"
#include "core/recompiler/nso_symbols.h"

namespace Core {

namespace {
// Mirrors the prefix of the GuestContext the recompiler emits. Only the fields
// the emulator needs to observe or mutate are modelled; the generated struct
// carries additional members after these (heap bookkeeping, save-data handles)
// which the recompiled code manages itself and we never touch.
struct GuestContextView {
    u64 x[32];
    u64 pc;
    u8 n, z, c, v;
    u8* mem;
    u64 mem_size;
    u64 mem_base_vaddr;
    int halted;
    u64 pending_svc;
    // The SIMD/FP register file and thread pointer sit immediately after
    // pending_svc in the emitted struct. They have to be modelled here rather
    // than left off the end: the recompiler emits SIMD code, so a context
    // switch that did not carry these would silently lose every floating-point
    // and vector register the guest had live.
    u64 vreg[32][2];
    u64 tpidr_el0;
    const void* host_mem;
    // TPIDRRO_EL0 - the kernel-published thread-local region, whose first
    // 0x100 bytes are the IPC message buffer KServerSession reads a
    // SendSyncRequest out of. Kept apart from tpidr_el0 (the guest's own
    // thread pointer) because the two have different owners and different
    // lifetimes; see the note on GuestContext in core/recompiler/arm64_to_c.h.
    u64 tpidrro_el0;
    // FP control and status. Modelled here so they survive the marshal to and
    // from the fallback JIT - without these fields the guest's rounding mode
    // was silently reset to the host default on every engine transition.
    u64 fpcr;
    u64 fpsr;
    // Must sit here rather than at the end: the emitted struct continues with
    // heap bookkeeping this view does not model, so a field appended after that
    // point would be at a different offset on each side.
    int chain_budget;
    // ABI 6 (FM1): the page table and a page-aligned limit below which the
    // generated memory helpers may read the table directly. fm_limit 0 keeps
    // them on the ABI 5 path. ABI 5 modules end their view at chain_budget and
    // never read these.
    u32 fm_reserved;
    const u8* fm_table;
    u64 fm_limit;
};

// Matches RecompHostMem in the generated runtime. The recompiled code calls
// through this for every guest access, so that it reads and writes the
// emulator's address space rather than the flat buffer the standalone runtime
// would otherwise own - without it the recompiled code and the HLE kernel
// would be looking at two different memories.
struct RecompHostMem {
    void* user;
    u64 (*load)(void* user, u64 va, u32 size);
    void (*store)(void* user, u64 va, u32 size, u64 value);
    // Exclusive access, routed at the kernel's own monitor so recompiled code
    // and the fallback JIT contend correctly against each other.
    u64 (*excl_load)(void* user, u64 va, u32 size);
    u32 (*excl_store)(void* user, u64 va, u32 size, u64 value);
    void (*clear_excl)(void* user);
    // The physical counter, read from the emulator's timing source so the
    // recompiled code and the JIT agree about time.
    u64 (*read_cntpct)(void* user);
    // Exclusive pair forms; `size` is the width of one register, 4 or 8.
    void (*excl_load_pair)(void* user, u64 va, u32 size, u64* lo, u64* hi);
    // Page table, so generated code can resolve a mapped address inline instead
    // of calling out for every load and store. Mirrors GetPointerImpl's fast
    // path; a null backing pointer (unmapped, debug, or GPU-tracked memory)
    // falls through to `load`/`store` so rasterizer invalidation still happens.
    u32 (*excl_store_pair)(void* user, u64 va, u32 size, u64 lo, u64 hi);
    const void* page_entries;
    u64 page_entry_stride;
    u64 page_bits;
    u64 pointer_mask;
    u64 address_space_max;
    // Reserved for layout compatibility; ABI 5 never reads this slot.
    const u64* guard_generation;
};

// This struct is duplicated by hand in the emitter (arm64_to_c.h, RuntimeH's
// RecompHostMem) because the generated project is plain C and shares no headers
// with the emulator. Nothing links the two, so a field added in the middle of
// one and at the end of the other compiles cleanly on both sides and hands the
// generated code a function pointer where it expects data.
//
// That is not hypothetical: inserting the page-table fields after
// excl_load_pair here, while the emitter appended them after excl_store_pair,
// made recompiled code dereference excl_store_pair's code pointer as a page
// table. suyu died during boot with no diagnostic. Hence these.
static_assert(offsetof(RecompHostMem, excl_load_pair) == 56);
static_assert(offsetof(RecompHostMem, excl_store_pair) == 64);
static_assert(offsetof(RecompHostMem, page_entries) == 72);
static_assert(offsetof(RecompHostMem, page_entry_stride) == 80);
static_assert(offsetof(RecompHostMem, page_bits) == 88);
static_assert(offsetof(RecompHostMem, pointer_mask) == 96);
static_assert(offsetof(RecompHostMem, address_space_max) == 104);
static_assert(offsetof(RecompHostMem, guard_generation) == 112);
static_assert(sizeof(RecompHostMem) == 120);

// Nothing links these two builds together, so the shared layout is pinned on
// both sides: the generated runtime asserts the same four offsets against its
// own GuestContext. If a field is ever inserted rather than appended, one of
// the two fails to compile instead of the emulator silently reading the wrong
// registers.
static_assert(offsetof(GuestContextView, pc) == 256);
static_assert(offsetof(GuestContextView, pending_svc) == 304);
static_assert(offsetof(GuestContextView, vreg) == 312);
static_assert(offsetof(GuestContextView, tpidr_el0) == 824);
static_assert(offsetof(GuestContextView, chain_budget) == 864);
static_assert(offsetof(GuestContextView, fm_table) == 872);
static_assert(offsetof(GuestContextView, fm_limit) == 880);
// The FM1 helpers fold the page table layout in as constants
// (RECOMP_FM_PAGE_BITS, RECOMP_FM_STRIDE_LOG2, RECOMP_FM_PTR_MASK). A change
// here must fail the build, not misread the table.
static_assert(sizeof(Common::PageTable::PageEntryData) == 32);
static_assert(Common::PageTable::ATTRIBUTE_BITS == 2);
static_assert(Memory::YUZU_PAGEBITS == 12);
// GG1: the generated store helpers read the watch word at this offset.
static_assert(offsetof(Common::PageTable::PageEntryData, recomp_watch) ==
              RecompGuardGen::kWatchOffset);
static_assert(sizeof(std::atomic<u64>) == sizeof(u64) && std::atomic<u64>::is_always_lock_free);

// Blocks a chain of direct calls may run before returning here. Only this side
// sets it - the generated code just decrements - so the emitter does not need
// to agree on the value.
//
// It bounds two things. How long a guest loop can run without the interrupt and
// SVC checks below getting a look in; and, if the generated calls are not tail
// calls, how deep the host stack goes. Guest threads run on 512 KB fibers
// (common/fiber.cpp) and a block frame carrying SIMD locals is not small.
//
// This comment used to say 256 overflowed that stack and crashed on boot, and
// that raising the budget was only safe when the generated code was built with
// -foptimize-sibling-calls. That flag is still only on the GNU branch - the
// MSVC branch emits "/O1" with no tail-call guarantee, and ChainTo calls into
// another translation unit - so by that reasoning Windows should fall over well
// below the ABI 4 default of 4096.
//
// It does not. Measured on Mario Kart 8 running fully static with zero JIT
// transitions, replaying the same 8861-command fixture at each budget:
//
//     32 -> 0.666x realtime    256 -> 0.696x    1024 -> 0.695x
//   4096 -> 0.678x             8192 -> 0.692x
//
// Every one completed, including 256 and the 8192 ceiling, each executing about
// 8.5 billion blocks. So either MSVC /O1 does tail-call these, or the chains
// never get deep enough to matter. The budget is worth about 4% either way,
// with 32 the slowest, so there is little to gain from tuning it.
//
// Keep the bound: one title not overflowing is not proof that a deeper-
// recursing one cannot. But do not treat a raised budget as known-dangerous on
// MSVC, because that is not what the measurement says.
// Refuse the JIT entirely. Without this, "the JIT was never reached" is an
// observation about one run; with it, reaching the JIT is a loud, fatal failure
// that names the address, which is the difference between evidence and proof.
bool StrictNoFallback() {
    const char* e = std::getenv("SUYU_RECOMP_STRICT");
    return e && *e && *e != '0';
}

const int kChainBudgetOverride = [] {
    const char* e = std::getenv("SUYU_RECOMP_CHAIN_BUDGET");
    if (!e) {
        return 0;
    }
    const int v = std::atoi(e);
    return (v >= 1 && v <= 8192) ? v : 0;
}();
std::atomic<int> g_recomp_chain_budget{32};

int RecompChainBudget() {
    return kChainBudgetOverride ? kChainBudgetOverride
                                : g_recomp_chain_budget.load(std::memory_order_relaxed);
}

const bool kSamplePc = [] {
    const char* e = std::getenv("SUYU_RECOMP_SAMPLE_PC");
    return e && *e && *e != '0';
}();

// How many blocks a thread retires between PC samples, as a power of two.
//
// The default of 18 samples proportionally to *executed blocks*, which is the
// wrong denominator for a stall: a title that runs 260M blocks of boot work in
// four seconds and then sits in a 4,000 block/s poll loop for three minutes
// puts 99.7% of its samples in the four seconds nobody is asking about. Lower
// this to sample the plateau, and use SUYU_RECOMP_SAMPLE_AFTER_SEC to throw
// away the burst entirely.
const unsigned kSamplePcShift = [] {
    const char* e = std::getenv("SUYU_RECOMP_SAMPLE_SHIFT");
    if (!e || !*e) {
        return 18u;
    }
    const int v = std::atoi(e);
    return (v >= 6 && v <= 30) ? static_cast<unsigned>(v) : 18u;
}();

// Seconds of run time to discard before any sample is kept. 0 keeps everything.
const double kSampleAfterSec = [] {
    const char* e = std::getenv("SUYU_RECOMP_SAMPLE_AFTER_SEC");
    return (e && *e) ? std::atof(e) : 0.0;
}();

const std::chrono::steady_clock::time_point kRecompStart = std::chrono::steady_clock::now();

// Naming the instruction that writes a guest field is not possible from the
// store callback, because emitted code resolves mapped memory through the
// inline page-table walk in recomp_host_ptr and never calls back. That walk
// has one runtime-controlled escape: it gives up on any address at or above
// host_mem->address_space_max and takes the callback path instead. Lowering
// that bound sends every access above a chosen address through HostStore,
// where the storing block's PC is visible - c->pc holds the entry address of
// the block currently running, because the emitted code writes it at branches
// rather than per instruction.
//
// The bound applies to loads too, so it is expensive: it is gated on a block
// count so the fast part of boot runs at full speed and only the window around
// the fault is instrumented.
const u64 kSlowPathAbove = [] {
    const char* e = std::getenv("SUYU_RECOMP_SLOWPATH_ABOVE");
    return (e && *e) ? std::strtoull(e, nullptr, 0) : 0ULL;
}();
const u64 kSlowPathAfterBlocks = [] {
    const char* e = std::getenv("SUYU_RECOMP_SLOWPATH_AFTER_BLOCKS");
    return (e && *e) ? std::strtoull(e, nullptr, 0) : 0ULL;
}();
const u64 kTrapStoreLo = [] {
    const char* e = std::getenv("SUYU_RECOMP_TRAP_STORE_LO");
    return (e && *e) ? std::strtoull(e, nullptr, 0) : 0ULL;
}();
const u64 kTrapStoreHi = [] {
    const char* e = std::getenv("SUYU_RECOMP_TRAP_STORE_HI");
    return (e && *e) ? std::strtoull(e, nullptr, 0) : 0ULL;
}();

// How much of the guest's relocation work we do ourselves, and what we do to
// DT_RELASZ/DT_PLTRELSZ afterwards. See ApplyAllRelocations.
//
// Only rtld's own self-relocation actually needs us: it is a hand-written
// bootstrap loop that the recompiled path exits early out of. Everything
// after that is rtld's ordinary C++ relocation pass, which runs correctly
// once rtld itself is relocated.
//
//   rtld-only       - default. Pre-apply and zero rtld alone; leave every
//                     other module untouched for rtld to relocate, exactly as
//                     under dynarmic. Smash reaches an actual match on this.
//   zero-all        - the previous default: pre-apply every module and zero
//                     every module's sizes. Boots, but main's .dynamic then
//                     reports no relocations, so when nn::ro loads a fighter
//                     NRO it cannot bind main's deferred imports. Measured:
//                     0 of main's 909 lua2cpp::create_agent_fighter_* slots
//                     are ever bound, and the match never starts.
//   zero-rtld       - pre-apply every module but zero only rtld's sizes.
//                     Measured not to work: rtld's pass then re-relocates
//                     modules we already did and svcBreaks immediately
//                     (12071 breaks, first at 0.1s after the pre-apply).
//   restore-on-main - zero every module, then put the sizes back on first
//                     execution in main. Measured not to work: rtld caches
//                     the sizes into its own module state when it parses
//                     .dynamic at boot, so a later write is never re-read and
//                     the slots stay unbound just as under zero-all.
const std::string kRelaPolicy = [] {
    const char* e = std::getenv("SUYU_RECOMP_RELA_POLICY");
    return std::string{(e && *e) ? e : "rtld-only"};
}();
// Symbol-name prefix whose unresolved JUMP_SLOT/GLOB_DAT slots are recorded at
// relocation time and re-read periodically, so a log can say whether anything
// ever bound them. Smash's fighter agents are `lua2cpp::create_agent_fighter`.
const std::string kTrackSlotPrefix = [] {
    const char* e = std::getenv("SUYU_RECOMP_TRACK_SLOT_PREFIX");
    return std::string{(e && *e) ? e : ""};
}();
// The original DT_RELASZ/DT_PLTRELSZ values, as (address, size) pairs, for the
// restore-on-main policy. Process-global rather than per-Impl: every CPU Impl
// runs its own relocation pass, but only the first one sees the real sizes -
// the rest parse .dynamic after it has already zeroed them, so a per-Impl copy
// records zeros and a later Impl's restore writes those zeros back over the
// first Impl's correct one. That happened before the first NRO load and put
// the sizes back at 0 exactly where they were needed.
std::mutex g_rela_restore_lock;
std::vector<std::pair<u64, u64>> g_rela_restore;
std::atomic<bool> g_rela_restored{false};

bool SampleWindowOpen() {
    if (kSampleAfterSec <= 0.0) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - kRecompStart).count() >= kSampleAfterSec;
}

static_assert(offsetof(GuestContextView, host_mem) == 832);
static_assert(offsetof(GuestContextView, tpidrro_el0) == 840);
static_assert(offsetof(GuestContextView, fpcr) == 848);
static_assert(offsetof(GuestContextView, fpsr) == 856);

// The generated code signals an SVC by parking with this set. Kept in sync
// with the emitted recomp_svc contract in core/recompiler/arm64_to_c.h.
constexpr u64 kNoPendingSvc = ~0ULL;

// Mirrors RECOMP_HALT_UNHANDLED in the generated runtime: a block that halts
// with this parked its PC on an instruction the decoder cannot translate and
// is asking for that address to be executed by the interpreter fallback.
constexpr int kHaltUnhandled = 2;
} // namespace

namespace {
std::atomic<RecompLookupFn> g_recomp_lookup{nullptr};
std::atomic<RecompBindFn> g_recomp_binder{nullptr};
} // namespace

RecompExecutionStats GetRecompExecutionStats() {
    return {
        TotalStaticBlocks(),
        g_counters.svc_calls.load(std::memory_order_relaxed),
        g_counters.fallback_from_miss.load(std::memory_order_relaxed),
        g_counters.fallback_from_unhandled.load(std::memory_order_relaxed),
        g_counters.no_fallback_available.load(std::memory_order_relaxed),
    };
}

std::array<u64, 4> GetRecompCurrentPcs() {
    std::array<u64, 4> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = g_current_pcs[index].load(std::memory_order_relaxed);
    }
    return result;
}

void SetRecompLookup(RecompLookupFn lookup) {
    std::scoped_lock lock{g_process_init_lock};
    g_code_guard_ready.store(false, std::memory_order_release);
    g_fastmem_ready.store(false, std::memory_order_release);
    // The previous bundle's images may be unloaded; drop them untouched.
    RecompGuardGen::Forget();
    g_fpx_ready.store(false, std::memory_order_release);
    g_recomp_lookup.store(lookup, std::memory_order_release);
}

void SetRecompBinder(RecompBindFn binder) {
    g_recomp_binder.store(binder, std::memory_order_release);
}

void SetRecompBindCheck(RecompBindCheckFn check) {
    g_recomp_bind_check.store(check, std::memory_order_release);
}

RecompLiveStats GetRecompLiveStats() {
    return RecompLiveStats{
        TotalStaticBlocks(),
        g_counters.fallback_from_miss.load(std::memory_order_relaxed) +
            g_counters.fallback_from_unhandled.load(std::memory_order_relaxed),
        g_forced_cutoff_pc.load(std::memory_order_relaxed),
        g_forced_cutoff_blocks.load(std::memory_order_relaxed),
        g_live_instances.load(std::memory_order_relaxed) > 0,
#ifdef SUYU_NO_JIT
        false,
#else
        true,
#endif
        StrictNoFallback(),
    };
}

void SetRecompPrepareCallback(RecompPrepareFn callback) {
    g_recomp_prepare.store(callback, std::memory_order_release);
}

bool HasRecompPrepareCallback() {
    return g_recomp_prepare.load(std::memory_order_acquire) != nullptr;
}

RecompLookupFn GetRecompLookup() {
    return g_recomp_lookup.load(std::memory_order_acquire);
}

class ArmRecompProcessState {
public:
    enum class Initialization {
        Uninitialized,
        Ready,
        FallbackOnly,
        Fatal,
    };

    std::once_flag initialize_once;
    std::once_flag announce_once;
    Loader::AppLoader::Modules modules;
    Initialization initialization{Initialization::Uninitialized};
    // Published only after the generated images have been identity-checked and
    // (for a single NSO) their relocations committed. Loader-time cache flushes
    // before this point describe the original image and must not disable AOT.
    std::atomic<bool> static_image_published{false};
    // Generated native code is immutable. Once the guest changes executable
    // code, there is no safe per-block repair yet, so every core must remain on
    // Dynarmic for the rest of this process.
    std::atomic<bool> force_jit{false};
    std::string error;
    bool relocations_committed{};
    u64 relocation_writes{};
    u64 finalization_writes{};
};

std::shared_ptr<ArmRecompProcessState> CreateArmRecompProcessState() {
    return std::make_shared<ArmRecompProcessState>();
}

struct ArmRecomp::Impl {
    Impl(System& system_, RecompLookupFn lookup_,
         std::shared_ptr<ArmRecompProcessState> process_state_)
        : system{system_}, lookup{lookup_}, process_state{std::move(process_state_)} {
        std::memset(&ctx, 0, sizeof(ctx));
        ctx.pending_svc = kNoPendingSvc;
        // Point the recompiled code at the emulator's address space.
        bridge.user = this;
        bridge.load = &Impl::HostLoad;
        bridge.store = &Impl::HostStore;
        bridge.excl_load = &Impl::HostExclusiveLoad;
        bridge.excl_store = &Impl::HostExclusiveStore;
        bridge.clear_excl = &Impl::HostClearExclusive;
        bridge.read_cntpct = &Impl::HostReadCntpct;
        bridge.excl_load_pair = &Impl::HostExclusiveLoadPair;
        bridge.excl_store_pair = &Impl::HostExclusiveStorePair;
        // Filled in by RefreshPageTable once a process exists; until then the
        // fields stay null and every access takes the callback path.
        bridge.page_entries = nullptr;
        bridge.guard_generation = nullptr;
        ctx.host_mem = &bridge;
    }

    // The monitor is per-core, and core_index is what distinguishes one guest
    // thread's reservation from another's. Without a monitor (no owning process,
    // so no fallback either) these degrade to plain accesses with STXR always
    // succeeding - the old behaviour, and wrong under threads, but that
    // configuration cannot run a real title anyway.
    static u64 HostExclusiveLoad(void* user, u64 va, u32 size) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            return HostLoad(user, va, size);
        }
        const auto core = self->core_index;
        switch (size) {
        case 1: return self->exclusive_monitor->ExclusiveRead8(core, va);
        case 2: return self->exclusive_monitor->ExclusiveRead16(core, va);
        case 4: return self->exclusive_monitor->ExclusiveRead32(core, va);
        case 8: return self->exclusive_monitor->ExclusiveRead64(core, va);
        default: return HostLoad(user, va, size);
        }
    }

    /// Returns 0 on success, 1 when the reservation was lost - the sense of the
    /// status register STXR writes.
    static u32 HostExclusiveStore(void* user, u64 va, u32 size, u64 value) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            HostStore(user, va, size, value);
            return 0;
        }
        const auto core = self->core_index;
        bool ok = false;
        switch (size) {
        case 1: ok = self->exclusive_monitor->ExclusiveWrite8(core, va, static_cast<u8>(value)); break;
        case 2: ok = self->exclusive_monitor->ExclusiveWrite16(core, va, static_cast<u16>(value)); break;
        case 4: ok = self->exclusive_monitor->ExclusiveWrite32(core, va, static_cast<u32>(value)); break;
        case 8: ok = self->exclusive_monitor->ExclusiveWrite64(core, va, value); break;
        default: HostStore(user, va, size, value); return 0;
        }
        return ok ? 0u : 1u;
    }

    /// LDXP/LDAXP. The 64-bit pair takes a real 128-bit reservation; the
    /// 32-bit pair is a 64-bit reservation whose two words are the registers,
    /// which is what the architecture specifies rather than a shortcut.
    static void HostExclusiveLoadPair(void* user, u64 va, u32 size, u64* lo, u64* hi) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            *lo = HostLoad(user, va, size);
            *hi = HostLoad(user, va + size, size);
            return;
        }
        const auto core = self->core_index;
        if (size == 8) {
            const u128 v = self->exclusive_monitor->ExclusiveRead128(core, va);
            *lo = v[0];
            *hi = v[1];
        } else {
            const u64 v = self->exclusive_monitor->ExclusiveRead64(core, va);
            *lo = static_cast<u32>(v);
            *hi = static_cast<u32>(v >> 32);
        }
    }

    /// STXP/STLXP. Returns 0 on success, matching STXR's status sense.
    static u32 HostExclusiveStorePair(void* user, u64 va, u32 size, u64 lo, u64 hi) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            HostStore(user, va, size, lo);
            HostStore(user, va + size, size, hi);
            return 0;
        }
        const auto core = self->core_index;
        bool ok = false;
        if (size == 8) {
            ok = self->exclusive_monitor->ExclusiveWrite128(core, va, u128{lo, hi});
        } else {
            ok = self->exclusive_monitor->ExclusiveWrite64(
                core, va, static_cast<u32>(lo) | (static_cast<u64>(static_cast<u32>(hi)) << 32));
        }
        return ok ? 0u : 1u;
    }

    /// Re-read the page table description into the bridge.
    ///
    /// Called on entry to a run rather than once at construction: the table
    /// belongs to the process, and the pointer is not valid until one exists.
    /// A stale pointer here would have generated code reading another address
    /// space, so it is refreshed rather than cached forever.
    void RefreshPageTable() {
        const auto view = system.ApplicationMemory().GetPageTableView();
        // ABI 5 checks the instruction bytes against this mapping on every entry.
        bridge.page_entries = view.entries;
        bridge.page_entry_stride = view.entry_stride;
        bridge.page_bits = view.page_bits;
        bridge.pointer_mask = view.pointer_mask;
        bridge.address_space_max = view.address_space_max;
        if (kSlowPathAbove != 0 && kSlowPathAbove < bridge.address_space_max &&
            TotalStaticBlocks() >= kSlowPathAfterBlocks) {
            bridge.address_space_max = kSlowPathAbove;
        }
        // ABI 6 (FM1). The generated helpers read the table with its layout
        // folded in as constants, so enable them only for a table that has that
        // layout. The limit is the ABI 5 walk's own limit (after any diagnostic
        // lowering above) rounded down to a page, so the fast path serves a
        // subset of what that walk serves and declines everything else to it.
        ctx.fm_table = nullptr;
        ctx.fm_limit = 0;
        if (!kFastmemDisabled && g_fastmem_ready.load(std::memory_order_acquire) &&
            bridge.page_entries != nullptr && bridge.page_bits == Memory::YUZU_PAGEBITS &&
            bridge.page_entry_stride == sizeof(Common::PageTable::PageEntryData) &&
            bridge.pointer_mask == GetRecompFastmemLayout().pointer_mask &&
            bridge.address_space_max <= (u64{1} << 39)) {
            ctx.fm_table = static_cast<const u8*>(bridge.page_entries);
            ctx.fm_limit = bridge.address_space_max & ~u64{0xfff};
        }
    }

    /// ABI 6 GG1 activation for `process`. Decided entirely from what the
    /// Core::Memory hooks recorded since the process's table was created: a
    /// kernel query here would take the page-table KLightLock, and a contended
    /// KLightLock reschedules, which inside RunThread could enter this core's
    /// RunThread again for another guest thread.
    void ActivateGuardGen(Kernel::KProcess& process, u64 key) {
        // Watch every page of each stable module's span: GG1 stores there go to
        // HostStore, and Core::Memory reports writes and raw pointers there.
        auto& entries = process.GetPageTable().GetImpl().entries;
        const auto watch = [&entries](u64 va, u64 size) {
            const u64 first = va >> Memory::YUZU_PAGEBITS;
            const u64 last = (va + size - 1) >> Memory::YUZU_PAGEBITS;
            for (u64 page = first; page <= last && page < entries.size(); ++page) {
                reinterpret_cast<std::atomic<u64>*>(&entries[page].recomp_watch)
                    ->store(1, std::memory_order_relaxed);
            }
        };
        const void* const activate_table = process.GetMemory().GetPageTableView().entries;
        LOG_INFO(Core_ARM, "recomp generation guard: process {} activating against table={}",
                 process.GetProcessId(), activate_table);
        RecompGuardGen::Activate(key, activate_table, watch);
        const auto stats = RecompGuardGen::GetStats();
        LOG_INFO(Core_ARM,
                 "recomp generation guard: process {} active={} generation={}; {} of {} "
                 "module(s) verify on every entry",
                 process.GetProcessId(), stats.active, stats.generation, stats.sticky,
                 stats.modules);
    }

    /// The same source DynarmicCallbacks64::GetCNTPCT uses, so a guest thread
    /// that migrates between engines sees one monotonic clock.
    static u64 HostReadCntpct(void* user) {
        return static_cast<Impl*>(user)->system.CoreTiming().GetClockTicks();
    }

    static void HostClearExclusive(void* user) {
        auto* self = static_cast<Impl*>(user);
        if (self->exclusive_monitor) {
            self->exclusive_monitor->ClearExclusive(self->core_index);
        }
    }

    /// Report a static-executed access to an address the guest has not mapped.
    ///
    /// The emitted code resolves a mapped access through the page table inline,
    /// so only an unmapped one reaches these callbacks at all. Such an access
    /// is not a guest bug: it means some earlier translated instruction put a
    /// value in a register that the real hardware would never have produced.
    /// The PC trail is the shortest route from the bad address back to the
    /// instruction that computed it, and it is already maintained for misses.
    static void ReportUnmapped(Impl* self, u64 va, u32 size, const char* what) {
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1, std::memory_order_relaxed) >= 8) {
            return;
        }
        std::string trail;
        const size_t count = std::min<size_t>(self->trail_pos, Impl::kTrail);
        for (size_t i = 0; i < count; ++i) {
            trail += fmt::format("{:#x} ",
                                 self->trail[(self->trail_pos - count + i) & (Impl::kTrail - 1)]);
        }
        LOG_ERROR(Core_ARM, "recomp unmapped {}{} @ {:#x} from pc={:#x} after {} blocks", what,
                  size * 8, va, self->ctx.pc, TotalStaticBlocks());
        LOG_ERROR(Core_ARM, "recomp unmapped PC trail (oldest first): {}", trail);
        for (size_t r = 0; r < 32; r += 4) {
            LOG_ERROR(Core_ARM,
                      "recomp unmapped regs x{:<2}={:#018x} x{:<2}={:#018x} x{:<2}={:#018x} "
                      "x{:<2}={:#018x}",
                      r, self->ctx.x[r], r + 1, self->ctx.x[r + 1], r + 2, self->ctx.x[r + 2],
                      r + 3, self->ctx.x[r + 3]);
        }
    }

    static u64 HostLoad(void* user, u64 va, u32 size) {
        va &= 0xffffffffffffULL;
        const u32 bytes = size == 0 ? 4 : size;
        // Size zero is the negotiated guard-v2 instruction read. All other
        // widths must be architectural scalar sizes, with no 48-bit wrap.
        if ((size != 0 && size != 1 && size != 2 && size != 4 && size != 8) ||
            bytes > 0x1000000000000ULL - va) {
            return 0;
        }
        auto* self = static_cast<Impl*>(user);
        auto& memory = self->system.ApplicationMemory();
        if (size == 0) {
            // Validity is per guest page, so the word's first and last bytes
            // cover it; they are the same page unless the word straddles one.
            if (!memory.IsValidVirtualAddress(va) ||
                !memory.IsValidVirtualAddress(va + bytes - 1)) {
                return 0;
            }
            return (u64{1} << 32) | memory.Read32(va);
        }
        if (!memory.IsValidVirtualAddress(va)) {
            ReportUnmapped(self, va, size, "read");
        }
        // Memory's scalar accessors split unaligned accesses across guest
        // pages. Preserve them rather than imposing byte loads on all reads.
        switch (size) {
        case 1: return memory.Read8(va);
        case 2: return memory.Read16(va);
        case 4: return memory.Read32(va);
        default: return memory.Read64(va); // Validated size == 8.
        }
    }

    static void HostStore(void* user, u64 va, u32 size, u64 value) {
        va &= 0xffffffffffffULL;
        if ((size != 1 && size != 2 && size != 4 && size != 8) ||
            size > 0x1000000000000ULL - va) {
            return;
        }
        if (kTrapStoreLo != 0 && va >= kTrapStoreLo && va < kTrapStoreHi) {
            auto* self = static_cast<Impl*>(user);
            std::string trail;
            const size_t count = std::min<size_t>(self->trail_pos, Impl::kTrail);
            for (size_t i = 0; i < count; ++i) {
                trail += fmt::format("{:#x} ",
                                     self->trail[(self->trail_pos - count + i) & (Impl::kTrail - 1)]);
            }
            LOG_ERROR(Core_ARM,
                      "recomp TRAP store {:#x} size={} value={:#x} from block pc={:#x} blocks={} "
                      "x19={:#x} x0={:#x} x1={:#x} x8={:#x} lr={:#x} trail={}",
                      va, size, value, self->ctx.pc, TotalStaticBlocks(), self->ctx.x[19],
                      self->ctx.x[0], self->ctx.x[1], self->ctx.x[8], self->ctx.x[30], trail);
        }
        auto* self = static_cast<Impl*>(user);
        auto& memory = self->system.ApplicationMemory();
        if (!memory.IsValidVirtualAddress(va)) {
            ReportUnmapped(self, va, size, "write");
        }
        switch (size) {
        case 1: memory.Write8(va, static_cast<u8>(value)); break;
        case 2: memory.Write16(va, static_cast<u16>(value)); break;
        case 4: memory.Write32(va, static_cast<u32>(value)); break;
        default: memory.Write64(va, value); break;
        }
    }

    using Initialization = ArmRecompProcessState::Initialization;

    struct LiveModuleMemory {
        Core::Memory::Memory* memory{};
        u64 base{};
        u64 image_size{};
    };

    struct WritableRegion {
        u64 address{};
        u64 end{};

        bool Contains(u64 candidate, u64 size) const {
            return candidate >= address && candidate <= end && size <= end - candidate;
        }
    };

    static bool RangeFits(u64 offset, u64 size, u64 limit) {
        return offset <= limit && size <= limit - offset;
    }

    static bool AddAddress(u64 base, u64 offset, u64& result) {
        if (offset > std::numeric_limits<u64>::max() - base) {
            return false;
        }
        result = base + offset;
        return true;
    }

    static bool ReadModuleMemory(const void* user, u64 module_address, std::span<u8> destination) {
        const auto& module = *static_cast<const LiveModuleMemory*>(user);
        u64 address = 0;
        if (module.memory == nullptr ||
            !RangeFits(module_address, destination.size(), module.image_size) ||
            !AddAddress(module.base, module_address, address)) {
            return false;
        }
        return module.memory->ReadBlock(Kernel::KProcessAddress{address}, destination.data(),
                                        destination.size());
    }

    static bool ReadTransactionMemory(void* user, u64 address, std::span<u8> destination) {
        auto& memory = *static_cast<Core::Memory::Memory*>(user);
        return memory.ReadBlock(Kernel::KProcessAddress{address}, destination.data(),
                                destination.size());
    }

    static bool WriteTransactionMemory(void* user, u64 address, std::span<const u8> source) {
        auto& memory = *static_cast<Core::Memory::Memory*>(user);
        return memory.WriteBlock(Kernel::KProcessAddress{address}, source.data(), source.size());
    }

    static bool HashMappedText(Core::Memory::Memory& memory, u64 base, u64 size,
                               std::array<u8, RecompSha256Size>& digest) {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(),
                                                                       EVP_MD_CTX_free};
        if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
            return false;
        }

        std::array<u8, 64 * 1024> buffer{};
        u64 offset = 0;
        while (offset < size) {
            const std::size_t chunk =
                static_cast<std::size_t>((std::min)(size - offset, u64{buffer.size()}));
            if (!memory.ReadBlock(Kernel::KProcessAddress{base + offset}, buffer.data(), chunk) ||
                EVP_DigestUpdate(context.get(), buffer.data(), chunk) != 1) {
                return false;
            }
            offset += chunk;
        }

        unsigned int digest_size = 0;
        return EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) == 1 &&
               digest_size == digest.size();
    }

    void FailInitialization(Initialization state, std::string error) {
        process_state->initialization = state;
        process_state->error = std::move(error);
        if (state == Initialization::Fatal) {
            LOG_CRITICAL(Core_ARM, "recomp: process initialization is fatal: {}",
                         process_state->error);
        } else {
            LOG_WARNING(Core_ARM, "recomp: using JIT fallback: {}", process_state->error);
        }
    }

    bool PreflightWritableAddress(Kernel::KProcess& process, u64 address,
                                  std::optional<WritableRegion>& cached_region,
                                  std::string& error) {
        constexpr u64 WriteSize = sizeof(u64);
        if ((address & (WriteSize - 1)) != 0 ||
            address > std::numeric_limits<u64>::max() - WriteSize) {
            error = fmt::format("unaligned or overflowing relocation destination {:#x}", address);
            return false;
        }
        if (cached_region && cached_region->Contains(address, WriteSize)) {
            return true;
        }

        Kernel::KMemoryInfo memory_info{};
        Kernel::Svc::PageInfo page_info{};
        const auto query =
            process.GetPageTable().QueryInfo(std::addressof(memory_info), std::addressof(page_info),
                                             Kernel::KProcessAddress{address});
        if (query.IsFailure()) {
            error = fmt::format("could not query relocation destination {:#x}", address);
            return false;
        }
        const Kernel::Svc::MemoryInfo info = memory_info.GetSvcMemoryInfo();
        if (info.size == 0 || info.size > std::numeric_limits<u64>::max() - info.base_address) {
            error =
                fmt::format("relocation destination {:#x} has an invalid memory region", address);
            return false;
        }
        const u64 end = info.base_address + info.size;
        const bool writable_code_data = (info.state == Kernel::Svc::MemoryState::CodeData ||
                                         info.state == Kernel::Svc::MemoryState::AliasCodeData) &&
                                        info.permission == Kernel::Svc::MemoryPermission::ReadWrite;
        if (!writable_code_data || address < info.base_address || address >= end ||
            WriteSize > end - address) {
            error = fmt::format(
                "relocation destination {:#x} is not wholly inside writable NSO data", address);
            return false;
        }
        cached_region = WritableRegion{info.base_address, end};
        return true;
    }

    bool PreflightRelocationWrites(Kernel::KProcess& process,
                                   const suyu::recomp::NsoRelocationPlan& plan,
                                   const suyu::recomp::NsoDynamicInfo& dynamic,
                                   std::string& error) {
        std::optional<WritableRegion> cached_region;
        for (const auto& write : plan.writes) {
            if (!PreflightWritableAddress(process, write.address, cached_region, error)) {
                return false;
            }
        }
        const auto preflight_finalizer =
            [&](const auto& table, const std::optional<u64>& value_address, const char* tag) {
                if (!table || table->byte_size == 0) {
                    return true;
                }
                u64 address = 0;
                if (!value_address || !AddAddress(plan.module_base, *value_address, address)) {
                    error = std::string{tag} + " has no valid mapped value field";
                    return false;
                }
                return PreflightWritableAddress(process, address, cached_region, error);
            };
        return preflight_finalizer(dynamic.rela, dynamic.rela_size_value_address, "DT_RELASZ") &&
               preflight_finalizer(dynamic.plt_rela, dynamic.plt_rela_size_value_address,
                                   "DT_PLTRELSZ");
    }

    void InitializeProcess(Kernel::KProcess* process) {
        if (process == nullptr) {
            FailInitialization(Initialization::FallbackOnly, "guest process is unavailable");
            return;
        }

        Loader::AppLoader::Modules modules;
        const Loader::ResultStatus module_status = system.GetAppLoader().ReadNSOModules(modules);
        if (module_status != Loader::ResultStatus::Success) {
            FailInitialization(Initialization::FallbackOnly,
                               "loader did not expose NSO modules: " +
                                   Loader::GetResultStatusString(module_status));
            return;
        }
        process_state->modules = modules;
        if (modules.empty()) {
            FailInitialization(Initialization::FallbackOnly,
                               "loader reported no NSO modules for the guest process");
            return;
        }

        const auto binder = g_recomp_binder.load(std::memory_order_acquire);
        if (binder == nullptr) {
            FailInitialization(Initialization::FallbackOnly,
                               "recompiled image has no identity-and-base binding callback");
            return;
        }

        // A normal multi-NSO application starts in rtld. Its guest code owns
        // cross-module symbol lookup and relocation, so the host must bind every
        // generated image before the first instruction and then leave the
        // relocation tables intact. The single-NSO path below remains host-
        // relocated because there is no guest dynamic linker to do that work.
        if (modules.size() > 1) {
            if (modules.begin()->second.name != "rtld") {
                FailInitialization(
                    Initialization::FallbackOnly,
                    "a multi-NSO recompiled process must begin with an rtld module");
                return;
            }

            struct PendingBinding {
                u64 base{};
                const Loader::AppLoader::NsoModuleInfo* module{};
                NsoModuleImageLayout layout{};
                std::array<u8, RecompSha256Size> text_sha256{};
            };
            std::vector<PendingBinding> pending;
            pending.reserve(modules.size());

            auto& memory = process->GetMemory();
            for (const auto& [module_base, module] : modules) {
                const auto image_layout =
                    GetNsoModuleImageLayout(process, Kernel::KProcessAddress{module_base});
                if (!image_layout) {
                    FailInitialization(
                        Initialization::FallbackOnly,
                        fmt::format("loaded NSO '{}' memory layout failed structural validation",
                                    module.name));
                    return;
                }

                PendingBinding binding{
                    .base = module_base,
                    .module = &module,
                    .layout = *image_layout,
                };
                if (!HashMappedText(memory, module_base, image_layout->text_size,
                                    binding.text_sha256)) {
                    FailInitialization(
                        Initialization::FallbackOnly,
                        fmt::format("could not hash the live mapped text for NSO '{}'",
                                    module.name));
                    return;
                }
                pending.push_back(binding);
            }

            for (std::size_t index = 0; index < pending.size(); ++index) {
                const PendingBinding& binding = pending[index];
                const auto& module = *binding.module;
                if (!binder(index, pending.size(), module.name.c_str(), binding.base,
                            module.build_id.data(), module.build_id.size(),
                            binding.text_sha256.data(), binding.text_sha256.size(),
                            binding.layout.text_size)) {
                    FailInitialization(
                        Initialization::FallbackOnly,
                        fmt::format("no unique recompiled image matched NSO '{}' identity and "
                                    "live text",
                                    module.name));
                    return;
                }
            }

            process_state->static_image_published.store(true, std::memory_order_release);
            process_state->initialization = Initialization::Ready;
            LOG_INFO(Core_ARM,
                     "recomp: accepted {}-NSO hybrid batch; guest rtld will apply cross-module "
                     "relocations",
                     pending.size());
            return;
        }

        const auto& [module_base, module_info] = *modules.begin();
        const auto image_layout =
            GetNsoModuleImageLayout(process, Kernel::KProcessAddress{module_base});
        if (!image_layout) {
            FailInitialization(Initialization::FallbackOnly,
                               "loaded NSO memory layout failed structural validation");
            return;
        }

        LiveModuleMemory live_memory{
            .memory = &process->GetMemory(),
            .base = module_base,
            .image_size = image_layout->image_size,
        };
        const suyu::recomp::NsoModuleView module{
            .image_size = image_layout->image_size,
            .text_address = 0,
            .user = &live_memory,
            .read = ReadModuleMemory,
        };
        const auto dynamic = suyu::recomp::ParseNsoDynamic(module);
        if (!dynamic) {
            FailInitialization(Initialization::FallbackOnly,
                               "NSO dynamic metadata was rejected: " + dynamic.error);
            return;
        }
        if (!dynamic.warnings.empty()) {
            FailInitialization(Initialization::FallbackOnly,
                               "NSO uses unsupported dynamic metadata: " +
                                   dynamic.warnings.front());
            return;
        }

        const auto symbols = suyu::recomp::ParseNsoDynamicSymbols(module, *dynamic.info);
        if (!symbols) {
            FailInitialization(Initialization::FallbackOnly,
                               "NSO dynamic symbols were rejected: " + symbols.error);
            return;
        }
        const auto relocation_plan = suyu::recomp::PlanNsoRelocations(
            image_layout->image_size, *dynamic.info, *symbols.info, module_base);
        if (!relocation_plan) {
            FailInitialization(Initialization::FallbackOnly,
                               "NSO relocations were rejected: " + relocation_plan.error);
            return;
        }

        std::string preflight_error;
        if (!PreflightRelocationWrites(*process, *relocation_plan.plan, *dynamic.info,
                                       preflight_error)) {
            FailInitialization(Initialization::FallbackOnly,
                               "NSO relocation preflight failed: " + preflight_error);
            return;
        }

        auto& memory = process->GetMemory();
        std::array<u8, RecompSha256Size> text_sha256{};
        if (!HashMappedText(memory, module_base, image_layout->text_size, text_sha256)) {
            FailInitialization(Initialization::FallbackOnly,
                               "could not hash the live mapped NSO text image");
            return;
        }
        if (!binder(0, 1, module_info.name.c_str(), module_base, module_info.build_id.data(),
                    module_info.build_id.size(), text_sha256.data(), text_sha256.size(),
                    image_layout->text_size)) {
            FailInitialization(
                Initialization::FallbackOnly,
                "no unique recompiled image matched the loaded NSO identity and live text");
            return;
        }

        const suyu::recomp::NsoRelocationMemory transaction_memory{
            .user = &memory,
            .read = ReadTransactionMemory,
            .write = WriteTransactionMemory,
        };
        const auto committed = suyu::recomp::CommitNsoRelocationPlan(
            *relocation_plan.plan, *dynamic.info, transaction_memory);
        if (!committed) {
            const Initialization failure =
                committed.state == suyu::recomp::NsoRelocationCommitState::RollbackFailed
                    ? Initialization::Fatal
                    : Initialization::FallbackOnly;
            FailInitialization(failure, "NSO relocation transaction failed: " + committed.error);
            return;
        }
        process_state->relocations_committed = true;
        process_state->relocation_writes = committed.relocation_writes;
        process_state->finalization_writes = committed.finalization_writes;
        process_state->static_image_published.store(true, std::memory_order_release);
        process_state->initialization = Initialization::Ready;
        LOG_INFO(Core_ARM,
                 "recomp: committed {} relocations and {} finalizers for '{}' at {:#x} "
                 "(image size {:#x})",
                 committed.relocation_writes, committed.finalization_writes, module_info.name,
                 module_base, image_layout->image_size);
    }

    /// Discover modules, validate and commit relocations, then publish the
    /// generated image's runtime base as one process-wide initialization step.
    /// Every CPU core calls this gate before dispatch; call_once also publishes
    /// the completed memory transaction and result to all of them.
    Initialization EnsureProcessInitialized(Kernel::KThread* thread) {
        std::call_once(process_state->initialize_once, [this, thread] {
            try {
                Kernel::KProcess* process = owner_process;
                if (process == nullptr && thread != nullptr) {
                    process = thread->GetOwnerProcess();
                }
                InitializeProcess(process);
                if (process_state->initialization == Initialization::Uninitialized) {
                    FailInitialization(Initialization::FallbackOnly,
                                       "runtime initialization produced no result");
                }
            } catch (const std::exception& exception) {
                FailInitialization(
                    process_state->relocations_committed ? Initialization::Fatal
                                                         : Initialization::FallbackOnly,
                    "runtime initialization threw: " + std::string{exception.what()});
            } catch (...) {
                FailInitialization(process_state->relocations_committed
                                       ? Initialization::Fatal
                                       : Initialization::FallbackOnly,
                                   "runtime initialization threw an unknown exception");
            }
        });
        return process_state->initialization;
    }
    System& system;
    // Byte reads of this process's memory for the RecompGaps module parsers.
    const RecompGaps::GuestRead8 read8{
        [this](u64 va) -> std::uint8_t { return system.ApplicationMemory().Read8(va); }};
    RecompLookupFn lookup{};
    GuestContextView ctx{};
    RecompHostMem bridge{};
    u32 fpcr{};
    u32 fpsr{};
    std::atomic<bool> interrupted{false};
    std::shared_ptr<ArmRecompProcessState> process_state;
    static constexpr size_t kTrail = 32;
    u64 trail[kTrail]{};
    size_t trail_pos{0};

    // Interpreter fallback for PCs the static pass never covered. Built on the
    // first miss rather than up front: most runs never need it, and a JIT per
    // core costs a code cache each.
    Kernel::KProcess* owner_process{};
    // The base interface, not dynarmic's implementation of it: the recompiled
    // path only ever calls the virtual methods, and holding the concrete type
    // here is what forced the library into a build that never runs a JIT.
    ExclusiveMonitor* exclusive_monitor{};
    // Last guest thread run on this core, so a reservation is voided on a real
    // context switch and left alone on an ordinary return to the host.
    Kernel::KThread* last_thread{};
    std::size_t core_index{};
    bool uses_wall_clock{};
#ifndef SUYU_NO_JIT
    std::unique_ptr<ArmDynarmic64> fallback{};
    std::atomic<ArmDynarmic64*> fallback_signal_target{nullptr};
    bool in_fallback{false};
    bool fallback_unavailable{false};
};

ArmRecomp::ArmRecomp(System& system, bool uses_wall_clock, RecompLookupFn lookup,
                     std::shared_ptr<ArmRecompProcessState> process_state,
                     Kernel::KProcess* process, DynarmicExclusiveMonitor* exclusive_monitor,
                     std::size_t core_index)
    : ArmInterface{uses_wall_clock},
      impl{std::make_unique<Impl>(system, lookup, std::move(process_state))} {
    impl->owner_process = process;
    impl->exclusive_monitor = exclusive_monitor;
    impl->core_index = core_index;
    impl->uses_wall_clock = uses_wall_clock;
    RecompDiagnosticSampler::Config diagnostic_config;
    diagnostic_config.snapshots = kSamplePc;
    const char* lr_env = std::getenv("SUYU_RECOMP_WATCH_LR_OFFSET");
    const char* addr_env = std::getenv("SUYU_RECOMP_WATCH_ADDR");
    diagnostic_config.watch = (lr_env && *lr_env) || (addr_env && *addr_env);
    diagnostic_config.lr_offset = (lr_env && *lr_env) ? std::strtoull(lr_env, nullptr, 0) : 0;
    diagnostic_config.fixed_address = (addr_env && *addr_env) ? std::strtoull(addr_env, nullptr, 0) : 0;
    if (diagnostic_config.watch) {
        const char* path_env = std::getenv("SUYU_RECOMP_WATCH_PATH");
        diagnostic_config.watch_path = (path_env && *path_env)
            ? std::string{path_env}
            : (Common::FS::GetSuyuPath(Common::FS::SuyuPath::LogDir) / "recomp_watch.txt").string();
    }
    impl->diagnostics.Configure(process ? process->GetId() : 0, core_index,
                                std::move(diagnostic_config));
    // Snapshot storage contains copied text only. A new session starts fresh.
    if (g_live_instances.fetch_add(1, std::memory_order_relaxed) == 0) {
        std::scoped_lock lock{g_snapshot_lock};
        g_diagnostic_snapshots.clear();
        // Before the loader places any module: it reports each one as it does.
        g_session_blocks_start.store(TotalStaticBlocks(), std::memory_order_relaxed);
        RecompGaps::BeginSession(process ? process->GetProgramId() : 0, StrictNoFallback());
    }
}

ArmRecomp::~ArmRecomp() {
    if (impl->core_index < g_current_pcs.size()) {
        g_current_pcs[impl->core_index].store(0, std::memory_order_relaxed);
    }
    // Reports only copied samples and counters; never dereferences a process
    // after its page table has been finalized. Report once per session, not once
    // per host-process lifetime.
    if (g_live_instances.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        ReportRecompCoverage();
        RecompGaps::EndSession(TotalStaticBlocks() >
                               g_session_blocks_start.load(std::memory_order_relaxed));
    }
}

bool PrepareRecompProcess(Kernel::KProcess& process, const RecompModules& modules) {
    const auto prepare = g_recomp_prepare.load(std::memory_order_acquire);
    if (!prepare) return true;
    // KProcess::InitializeInterfaces chooses ArmRecomp for every application
    // core while this lookup is installed. RTTI is disabled in core builds.
    if (!GetRecompLookup() || !process.IsApplication() || !process.Is64Bit() ||
        modules.empty() || modules.begin()->second != "rtld" ||
        modules.begin()->first != GetInteger(process.GetEntryPoint())) {
        LOG_ERROR(Core_ARM, "Static preparation requires an AArch64 rtld entry point");
        return false;
    }
    for (size_t i = 0; i < Hardware::NUM_CPU_CORES; ++i) {
        const auto* cpu = static_cast<ArmRecomp*>(process.GetArmInterface(i));
        if (!cpu || cpu->impl->explicitly_prepared) return false;
    }
    if (!prepare(modules)) return false;
    for (size_t i = 0; i < Hardware::NUM_CPU_CORES; ++i) {
        auto& state = *static_cast<ArmRecomp*>(process.GetArmInterface(i))->impl;
        state.modules = modules;
        state.modules_read = true;
        // Run real rtld relocation. The desktop host pre-relocator substitutes
        // unresolved imports and must never modify this strict session's memory.
        state.rela_applied = true;
        state.explicitly_prepared = true;
    }
    g_counters.RecordModules(modules);
    return true;
}

bool ArmRecomp::EnterFallback(Kernel::KThread* thread) {
    if (impl->fallback_unavailable) {
        return false;
    }
    if (StrictNoFallback()) {
        // Latched, so the caller's own critical log naming the PC is what gets
        // read, and the second thread to arrive does not repeat this one.
        impl->fallback_unavailable = true;
        LOG_CRITICAL(Core_ARM, "recomp: strict mode - refusing to fall back to the JIT");
        return false;
    }
#ifdef SUYU_NO_JIT
    // Built without a dynamic recompiler at all. There is nothing to fall back
    // to, by construction rather than by configuration.
    impl->fallback_unavailable = true;
    LOG_CRITICAL(Core_ARM, "recomp: built without a JIT; uncovered code cannot run");
    return false;
#else
    if (!impl->fallback) {
        if (!impl->owner_process || !impl->exclusive_monitor) {
            impl->fallback_unavailable = true;
            return false;
        }
        impl->fallback = std::make_unique<ArmDynarmic64>(
            impl->system, impl->uses_wall_clock, impl->owner_process, *impl->exclusive_monitor,
            impl->core_index);
        impl->fallback->SetWatchpointArray(m_watchpoints);
        impl->fallback_signal_target.store(impl->fallback.get(), std::memory_order_release);
        if (impl->interrupted.load(std::memory_order_acquire)) {
            impl->fallback->SignalInterrupt(thread);
        }
        LOG_WARNING(Core_ARM, "recomp: created JIT fallback for uncovered code");
    }
    impl->in_fallback = true;
    return true;
#endif
}

HaltReason ArmRecomp::RunFallback(Kernel::KThread* thread, bool single_step) {
    // The recompiled context is the single source of truth; the JIT is loaded
    // from it on the way in and drained back on the way out, so every accessor
    // on this interface (SVC arguments, thread context save/restore) keeps
    // working unchanged no matter which engine actually ran.
    impl->ctx.pending_svc = kNoPendingSvc;
    impl->ctx.halted = 0;

    Kernel::Svc::ThreadContext tctx{};
    this->GetContext(tctx);
    impl->fallback->SetContext(tctx);
    impl->fallback->SetTpidrroEl0(impl->ctx.tpidrro_el0);

    const HaltReason hr =
        single_step ? impl->fallback->StepThread(thread) : impl->fallback->RunThread(thread);

    impl->fallback->GetContext(tctx);
    this->SetContext(tctx);
    if (True(hr & HaltReason::SupervisorCall)) {
        impl->ctx.pending_svc = impl->fallback->GetSvcNumber();
    }
    if (True(hr & HaltReason::BreakLoop)) {
        impl->interrupted.exchange(false, std::memory_order_acq_rel);
    }

    if (True(hr & HaltReason::CacheInvalidation) &&
        impl->process_state->static_image_published.load(std::memory_order_acquire)) {
        // IC IVAU/IC IALLU executed by the guest is raised from inside the
        // nested Dynarmic backend, so the ordinary kernel invalidation fanout
        // never reaches this wrapper. Make the AOT decision sticky here and
        // clear every other core's fallback cache as well. The current
        // fallback already invalidated its own requested range before halting.
        const bool first_invalidation =
            !impl->process_state->force_jit.exchange(true, std::memory_order_acq_rel);
        if (first_invalidation) {
            LOG_WARNING(Core_ARM,
                        "recomp: guest invalidated executable code; switching this process "
                        "permanently to JIT");
            // Broadcast only the first invalidation. If two fallback cores
            // raise cache-invalidation halts together, rebroadcasting both can
            // make them repeatedly clear and halt one another's JIT caches.
            if (impl->owner_process != nullptr) {
                for (std::size_t index = 0; index < Core::Hardware::NUM_CPU_CORES; ++index) {
                    auto* interface = impl->owner_process->GetArmInterface(index);
                    if (interface != nullptr && interface != this) {
                        interface->ClearInstructionCache();
                    }
                }
            }
        }
    }

    if (True(hr & HaltReason::SupervisorCall)) {
        g_counters.svc_calls.fetch_add(1, std::memory_order_relaxed);
    }

    // Return to recompiled execution as soon as the PC is covered again, so a
    // single uncovered function costs only the time spent inside it. A process
    // whose image failed validation stays entirely on the JIT.
    if (!impl->process_state->force_jit.load(std::memory_order_acquire) &&
        impl->process_state->initialization == ArmRecompProcessState::Initialization::Ready &&
        impl->lookup && impl->lookup(impl->ctx.pc)) {
        impl->in_fallback = false;
        g_counters.jit_to_static.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
#endif
}

HaltReason ArmRecomp::RunThread(Kernel::KThread* thread) {
    // A context switch voids any outstanding reservation. A thread preempted
    // between its LDXR and STXR would otherwise have the STXR succeed against a
    // word another thread changed on this core meanwhile, silently losing an
    // update to a mutex or condition variable and deadlocking the guest.
    //
    // Only on an actual switch: this backend returns to the host constantly
    // (chain budget, SVCs), so clearing on every entry would void reservations
    // the same thread is still in the middle of.
    if (impl->last_thread != thread) {
        impl->last_thread = thread;
        if (impl->exclusive_monitor) {
            impl->exclusive_monitor->ClearExclusive(impl->core_index);
        }
    }
    // Logged once so it is obvious from a log whether the backend was ever
    // entered at all. A run with no errors is otherwise indistinguishable from
    // a run where the guest thread was never scheduled onto it.
    std::call_once(impl->process_state->announce_once, [this] {
        LOG_INFO(Core_ARM, "ArmRecomp::RunThread entered, pc={:#x}", impl->ctx.pc);
    });
    if (!impl->lookup) {
        LOG_ERROR(Core_ARM, "No recompiled code registered; cannot run thread");
        return HaltReason::BreakLoop;
    }

    const auto initialization = impl->EnsureProcessInitialized(thread);
    if (initialization == ArmRecompProcessState::Initialization::Fatal) {
        return HaltReason::PrefetchAbort;
    }
    if (initialization == ArmRecompProcessState::Initialization::FallbackOnly) {
        if (!EnterFallback(thread)) {
            LOG_CRITICAL(Core_ARM,
                         "recomp: runtime image initialization failed and no JIT fallback is "
                         "available");
            return HaltReason::PrefetchAbort;
        }
        return RunFallback(thread);
    }
    if (initialization != ArmRecompProcessState::Initialization::Ready) {
        LOG_CRITICAL(Core_ARM, "recomp: runtime image initialization has no terminal state");
        return HaltReason::PrefetchAbort;
    }

    if (impl->process_state->force_jit.load(std::memory_order_acquire)) {
        if (!EnterFallback(thread)) {
            LOG_CRITICAL(Core_ARM,
                         "recomp: executable code was invalidated and no JIT fallback is "
                         "available");
            return HaltReason::PrefetchAbort;
        }
        return RunFallback(thread);
    }

    // A previous miss handed this thread to the JIT; keep running there until
    // the PC lands back inside recompiled code.
    if (impl->in_fallback) {
        return RunFallback(thread);
    }

    impl->ctx.halted = 0;

    while (!impl->ctx.halted) {
        if (impl->interrupted.exchange(false, std::memory_order_acq_rel)) {
            return HaltReason::BreakLoop;
        }

        // Another core can invalidate executable memory while this one is
        // between generated blocks. Stop dispatching immutable AOT code at the
        // next block boundary rather than waiting for this thread to yield.
        if (impl->process_state->force_jit.load(std::memory_order_acquire)) {
            if (!EnterFallback(thread)) {
                return HaltReason::PrefetchAbort;
            }
            return RunFallback(thread);
        }

        // An SVC parked us last time round; the kernel has now serviced it and
        // resumed, so clear it before continuing.
        if (impl->ctx.pending_svc != kNoPendingSvc) {
            impl->ctx.pending_svc = kNoPendingSvc;
        }

        // The host-side dispatcher picks the image that owns this absolute PC.
        // That image then subtracts the runtime base registered during process
        // initialization before looking up its module-relative block.
        // Rolling trail of the last few PCs. A wild indirect branch reports
        // only the address it landed on, which says nothing about which block
        // computed it; without the predecessors there is no way to tell a bad
        // GOT read from a bad emitted branch.
        impl->trail[impl->trail_pos++ & (Impl::kTrail - 1)] = impl->ctx.pc;

        RecompBlockFn block = impl->lookup(impl->ctx.pc);
        if (first_run) {
            LOG_INFO(Core_ARM, "First static lookup: pc={:#x}, covered={}", impl->ctx.pc,
                     block != nullptr);
        }
        // Test hook: forces every lookup past the Nth to miss, so the JIT
        // fallback below can be exercised on a title that would otherwise never
        // hit a gap. Unset in normal runs.
        {
            static const char* const force_miss = std::getenv("SUYU_RECOMP_FORCE_MISS_AFTER");
            static const char* const force_miss_static_blocks =
                std::getenv("SUYU_RECOMP_FORCE_MISS_AFTER_STATIC_BLOCKS");
            static std::atomic<int> blocks_run{0};
            if (force_miss) {
                const int n = blocks_run.fetch_add(1, std::memory_order_relaxed);
                const int limit = std::atoi(force_miss);
                // Name the blocks either side of the cutoff. Bisecting on the
                // cutoff tells you which index first breaks the run; this turns
                // that index into the actual guest address to look at.
                if (n >= limit - 4 && n <= limit + 4) {
                    LOG_ERROR(Core_ARM, "recomp: block #{} pc={:#x}", n, impl->ctx.pc);
                }
                if (n >= limit) {
                    block = nullptr;
                }
            }
            // Same hook keyed on a module-relative guest PC. Block counts and
            // wall clocks both bracket a fault loosely; this hands over at one
            // named instruction, which is what turns a bracket into a bisect.
            // Module bases are picked by the loader and differ per run, so the
            // offset is matched against every loaded base rather than an
            // absolute address the caller could not know in advance.
            static const char* const force_miss_at_offset =
                std::getenv("SUYU_RECOMP_FORCE_MISS_AT_MODULE_OFFSET");
            if (force_miss_at_offset) {
                static const u64 target = std::strtoull(force_miss_at_offset, nullptr, 0);
                // Snapshotted rather than read under the counters' lock on every
                // block, which would cost more than the whole dispatch loop.
                // Retried while empty because the first guest blocks can run
                // before the loader has finished registering module bases.
                static std::vector<u64> bases;
                if (bases.empty()) {
                    std::scoped_lock lk{g_counters.hist_lock};
                    for (const auto& [base, name] : g_counters.modules) {
                        bases.push_back(base);
                    }
                }
                for (const u64 base : bases) {
                    if (impl->ctx.pc - base == target) {
                        u64 unset = 0;
                        if (g_forced_cutoff_pc.compare_exchange_strong(
                                unset, impl->ctx.pc, std::memory_order_relaxed)) {
                            g_forced_cutoff_blocks.store(TotalStaticBlocks(),
                                                         std::memory_order_relaxed);
                            LOG_ERROR(Core_ARM,
                                      "recomp: forced offset cutoff {:#x} reached pc={:#x} "
                                      "after {} blocks",
                                      target, impl->ctx.pc,
                                      g_forced_cutoff_blocks.load(std::memory_order_relaxed));
                        }
                        block = nullptr;
                        break;
                    }
                }
            }
            // Address-range hole. Every guest PC whose offset in `main` falls
            // in [lo, hi) misses the lookup and runs under the JIT while the
            // rest of the module stays static, which narrows a fault inside a
            // module the way bisect-modules narrows it to a module. Read at
            // runtime, so an arm needs neither a re-export nor a rebuild.
            //
            // Module-relative for the same reason as the cutoff above. Scoped
            // to `main` because the same offset exists in every other image.
            //
            // Only meaningful when fallback is allowed: under
            // SUYU_RECOMP_STRICT a miss is a PrefetchAbort, not a handover.
            static const char* const main_hole = std::getenv("SUYU_RECOMP_MAIN_HOLE");
            if (main_hole) {
                static u64 hole_lo = 0;
                static u64 hole_hi = 0;
                static const bool hole_valid = [] {
                    char* end = nullptr;
                    hole_lo = std::strtoull(main_hole, &end, 0);
                    if (!end || *end != '-') {
                        LOG_ERROR(Core_ARM, "recomp: SUYU_RECOMP_MAIN_HOLE={} is not <lo>-<hi>",
                                  main_hole);
                        return false;
                    }
                    hole_hi = std::strtoull(end + 1, nullptr, 0);
                    if (hole_hi <= hole_lo) {
                        LOG_ERROR(Core_ARM, "recomp: SUYU_RECOMP_MAIN_HOLE={} is empty", main_hole);
                        return false;
                    }
                    return true;
                }();
                // Which module the offsets are relative to, as a substring of
                // its name. Modules are registered under the NSO's own name,
                // not the exefs file name - Smash's `main` is
                // `cross2_Release.nss` and its `sdk` is `nnSdk` - so matching
                // "main" finds nothing. Unset applies the hole to every module,
                // which is what the all-to-JIT control wants.
                static const char* const hole_module = std::getenv("SUYU_RECOMP_HOLE_MODULE");
                // Snapshotted, and retried while empty, because the first guest
                // blocks can run before the loader registers module bases.
                static std::vector<u64> hole_bases;
                if (hole_valid && hole_bases.empty()) {
                    std::scoped_lock lk{g_counters.hist_lock};
                    for (const auto& [base, name] : g_counters.modules) {
                        if (!hole_module || name.find(hole_module) != std::string::npos) {
                            hole_bases.push_back(base);
                        }
                    }
                    if (!hole_bases.empty()) {
                        LOG_ERROR(Core_ARM, "recomp: main hole {:#x}-{:#x} armed on {} module(s)",
                                  hole_lo, hole_hi, hole_bases.size());
                    } else if (!g_counters.modules.empty()) {
                        // Named a module that is not loaded. Silence here would
                        // look exactly like a range that never executes.
                        LOG_ERROR(Core_ARM, "recomp: SUYU_RECOMP_HOLE_MODULE={} matched no module",
                                  hole_module);
                    }
                }
                for (const u64 base : hole_bases) {
                    if (impl->ctx.pc < base) {
                        continue;
                    }
                    const u64 off = impl->ctx.pc - base;
                    if (off >= hole_lo && off < hole_hi) {
                        // Logged once so a run proves the hole was applied. A
                        // silently ignored hole reports the same counters as no
                        // hole at all, which makes every arm of a bisection look
                        // uninformative for the wrong reason.
                        static std::atomic<bool> hole_announced{false};
                        if (!hole_announced.exchange(true, std::memory_order_relaxed)) {
                            LOG_ERROR(Core_ARM,
                                      "recomp: main hole {:#x}-{:#x} active (base {:#x}), first "
                                      "handover at pc={:#x}",
                                      hole_lo, hole_hi, base, impl->ctx.pc);
                        }
                        block = nullptr;
                        break;
                    }
                }
            }
            // Same hook keyed on wall time instead of block count. A stalled
            // title stops retiring blocks, so a block-count cutoff placed
            // inside the stall is never reached; seconds get there regardless.
            static const char* const force_miss_after_sec =
                std::getenv("SUYU_RECOMP_FORCE_MISS_AFTER_SEC");
            if (force_miss_after_sec) {
                const double limit = std::atof(force_miss_after_sec);
                const double elapsed = std::chrono::duration<double>(
                                           std::chrono::steady_clock::now() - kRecompStart)
                                           .count();
                if (elapsed >= limit) {
                    u64 unset = 0;
                    if (g_forced_cutoff_pc.compare_exchange_strong(
                            unset, impl->ctx.pc, std::memory_order_relaxed)) {
                        g_forced_cutoff_blocks.store(TotalStaticBlocks(),
                                                     std::memory_order_relaxed);
                        LOG_ERROR(Core_ARM, "recomp: forced time cutoff {}s reached pc={:#x}",
                                  limit, impl->ctx.pc);
                    }
                    block = nullptr;
                }
            }
            if (force_miss_static_blocks) {
                const u64 limit = std::strtoull(force_miss_static_blocks, nullptr, 10);
                const u64 executed = TotalStaticBlocks();
                if (executed >= limit) {
                    u64 unset = 0;
                    if (g_forced_cutoff_pc.compare_exchange_strong(
                            unset, impl->ctx.pc, std::memory_order_relaxed)) {
                        g_forced_cutoff_blocks.store(executed, std::memory_order_relaxed);
                    }
                    static std::atomic<int> static_cutoff_logs{0};
                    if (static_cutoff_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
                        LOG_ERROR(Core_ARM,
                                  "recomp: forced static-block cutoff {} reached at {} pc={:#x}",
                                  limit, executed, impl->ctx.pc);
                    }
                    block = nullptr;
                }
            }
        }
        // A miss is now recoverable, so it can happen many times per second;
        // the full diagnostic dump is kept for the first few only, where it is
        // still useful for finding which indirect call went uncovered.
        static std::atomic<int> miss_count{0};
        const int miss_index = block ? 0 : miss_count.fetch_add(1, std::memory_order_relaxed);
        if (!block && miss_index < 8) {
            std::string trail;
            const size_t count = std::min<size_t>(impl->trail_pos, Impl::kTrail);
            for (size_t i = 0; i < count; ++i) {
                const u64 p = impl->trail[(impl->trail_pos - count + i) & (Impl::kTrail - 1)];
                trail += fmt::format("{:#x} ", p);
            }
            LOG_ERROR(Core_ARM, "recomp PC trail (oldest first): {}", trail);
            LOG_ERROR(Core_ARM, "recomp regs x16={:#x} x17={:#x} x30={:#x} sp={:#x}",
                      impl->ctx.x[16], impl->ctx.x[17], impl->ctx.x[30], impl->ctx.x[31]);
            LOG_ERROR(Core_ARM, "recomp regs x0={:#x} x15={:#x} x18={:#x} x19={:#x}",
                      impl->ctx.x[0], impl->ctx.x[15], impl->ctx.x[18], impl->ctx.x[19]);
            // The whole file, four per line. A miss is almost always a bad value
            // in some register the previous block computed, and guessing which
            // one to print in advance costs a rebuild per guess.
            for (size_t r = 0; r < 32; r += 4) {
                LOG_ERROR(Core_ARM, "recomp regs x{:<2}={:#018x} x{:<2}={:#018x} x{:<2}={:#018x} x{:<2}={:#018x}",
                          r, impl->ctx.x[r], r + 1, impl->ctx.x[r + 1], r + 2, impl->ctx.x[r + 2],
                          r + 3, impl->ctx.x[r + 3]);
            }
            // Dump guest memory around the registers that look like pointers.
            // A miss caused by a bad *value* and one caused by the wrong data
            // being mapped at the right address look identical from the
            // register file alone.
            for (u32 r : {8u, 22u, 25u}) {
                const u64 p = impl->ctx.x[r];
                if (p < 0x1000 || p > 0x0000'FFFF'FFFF'FFFFULL) {
                    continue;
                }
                std::string dump;
                for (s64 d = -0x20; d < 0x30; d += 4) {
                    dump += fmt::format("{:08x} ", (u32)Impl::HostLoad(impl.get(), p + d, 4));
                }
                LOG_ERROR(Core_ARM, "recomp mem @x{} ({:#x}) [-0x20..+0x30): {}", r, p, dump);
            }
            {
                const auto& modules = impl->process_state->modules;
                const u64 mbase = modules.empty() ? 0 : modules.begin()->first;
                for (u64 seg : {0x0ULL, 0x2000ULL, 0x3000ULL}) {
                    std::string dump;
                    for (u64 i = 0; i < 0x40; i += 4) {
                        dump += fmt::format("{:08x} ", Impl::HostLoad(impl.get(), mbase + seg + i, 4));
                    }
                    LOG_ERROR(Core_ARM, "recomp mem mod+{:#x} (base {:#x}): {}", seg, mbase, dump);
                }
            }
        }
        if (!block) {
            // No recompiled block covers this address: an indirect branch into
            // code the static pass never reached. The guest's own instructions
            // are still mapped in guest memory, so hand the thread to a JIT and
            // keep going instead of returning PrefetchAbort - that halt reason
            // makes the kernel suspend the thread for a debugger that is not
            // attached, which is a permanent, silent black-screen hang.
            if (miss_index < 64) {
                LOG_ERROR(Core_ARM, "No recompiled block at PC {:#x}; falling back to JIT",
                          impl->ctx.pc);
            } else {
                LOG_DEBUG(Core_ARM, "No recompiled block at PC {:#x}; falling back to JIT",
                          impl->ctx.pc);
            }
            if (!EnterFallback(thread)) {
                LOG_CRITICAL(Core_ARM,
                             "recomp: no JIT fallback available at PC {:#x}; thread cannot "
                             "continue",
                             impl->ctx.pc);
                return HaltReason::PrefetchAbort;
            }
            return RunFallback(thread);
        }

        // Dump periodically: teardown is not guaranteed to run (the emulated
        // process can outlive shutdown), and a run with no report is a run with
        // no measurement. One compare per block against a power-of-two mask.
        const u64 seen = t_blocks.slot->n.load(std::memory_order_relaxed) + 1;
        t_blocks.slot->n.store(seen, std::memory_order_relaxed);
        if ((seen & 0x3FFFFULL) == 0x3FFFFULL) {
            WriteRecompCoverageFile(FormatRecompCoverage());
            // Time-limited inside; teardown is not guaranteed, as above.
            RecompGaps::Flush(true, false);
        }
        // Generated code calls a direct branch's target itself rather than
        // coming back here, so one call below can run a whole chain of blocks.
        // The budget bounds that chain, and what is left of it afterwards says
        // how many blocks actually ran - without which every count here would
        // report chains rather than blocks.
        const int chain_budget = RecompChainBudget();
        impl->ctx.chain_budget = chain_budget;
        if (first_run) {
            LOG_INFO(Core_ARM, "Calling first signed static block at pc={:#x}", impl->ctx.pc);
        }
        block(&impl->ctx);
        if (early_dispatch < 64) {
            LOG_INFO(Core_ARM, "Early static return #{}: pc={:#x}, svc={}, halted={}",
                     early_dispatch, impl->ctx.pc, impl->ctx.pending_svc, impl->ctx.halted);
        }
        if (first_run) {
            LOG_INFO(Core_ARM, "First signed static block returned: pc={:#x}, svc={}, halted={}",
                     impl->ctx.pc, impl->ctx.pending_svc, impl->ctx.halted);
            first_run = false;
        }
        {
            const int spent = chain_budget - impl->ctx.chain_budget;
            u64 after = seen;
            if (spent > 0) {
                // The first block is already in `seen`. A decrement to zero
                // parks the next PC without entering it, so that final attempt
                // contributes no executed block.
                after += static_cast<u64>(spent - (impl->ctx.chain_budget == 0 ? 1 : 0));
                t_blocks.slot->n.store(after, std::memory_order_relaxed);
            }
            if (kSamplePc && (seen >> kSamplePcShift) != (after >> kSamplePcShift) &&
                SampleWindowOpen()) {
                g_counters.RecordSample(impl->ctx.pc);
            }
        }

        if (impl->ctx.halted == kHaltIcIvau) {
            if (!g_code_guard_ready.load(std::memory_order_acquire)) {
                LOG_CRITICAL(Core_ARM, "recomp: IC IVAU requires guard-v2 host and all guarded modules");
                std::abort();
            }
            const u64 address = impl->ctx.pending_svc;
            impl->ctx.pending_svc = kNoPendingSvc;
            // Forward the guest instruction-cache operation to each CPU interface.
            for (size_t core = 0; core < Hardware::NUM_CPU_CORES; ++core) {
                if (auto* cpu = thread->GetOwnerProcess()->GetArmInterface(core)) cpu->InvalidateCacheRange(address, 64);
            }
            impl->ctx.pc += 4;
            impl->ctx.halted = 0;
            continue;
        }
        if (impl->ctx.halted == kHaltBreakpoint) {
            return HaltReason::InstructionBreakpoint;
        }

        // The block stopped on an instruction the decoder has no translation
        // for, having parked the PC on that instruction. Running it on the JIT
        // instead keeps guest state exact: the alternative the generated code
        // used to take - step over it and zero x0 - silently produced a
        // plausible-looking null that only surfaced as a crash much later, in
        // whatever code eventually dereferenced it.
        if (impl->ctx.halted == kHaltUnhandled) {
            if ((static_cast<u32>(Impl::HostLoad(impl.get(), impl->ctx.pc, 4)) & 0xffffffe0U) == 0xd50b7520U) {
                LOG_CRITICAL(Core_ARM, "recomp: legacy unguarded IC IVAU refused at {:#x}", impl->ctx.pc);
                std::abort();
            }
            impl->ctx.halted = 0;
            // The generated code knows the encoding but cannot pass it back
            // through the halt contract, so read it out of guest memory - the
            // PC is parked exactly on the offending instruction. This is what
            // ranks the missing opcodes by execution rather than by how often
            // they appear in the image.
            g_counters.fallback_from_unhandled.fetch_add(1, std::memory_order_relaxed);
            const u32 unsupported_opcode =
                static_cast<u32>(Impl::HostLoad(impl.get(), impl->ctx.pc, 4));
            g_counters.RecordUnhandled(unsupported_opcode);
            RecompGaps::RecordUnimplemented(unsupported_opcode);
            static std::atomic<int> unhandled_count{0};
            if (unhandled_count.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_WARNING(Core_ARM, "recomp: unimplemented opcode {:#010x} at {:#x}; checking fallback policy",
                            unsupported_opcode, impl->ctx.pc);
            }
            if (!EnterFallback(thread)) {
                LOG_CRITICAL(Core_ARM, "recomp: unimplemented opcode at {:#x} and no JIT fallback",
                             impl->ctx.pc);
                return HaltReason::PrefetchAbort;
            }
            return RunFallback(thread);
        }

        if (impl->ctx.pending_svc != kNoPendingSvc) {
            // Log every SVC call from rtld (first few hundred only to avoid spam)
            LOG_TRACE(Core_ARM, "recomp SVC {} at pc={:#x} x0={:#x} x1={:#x} x2={:#x} x3={:#x}",
                      impl->ctx.pending_svc, impl->ctx.pc, impl->ctx.x[0], impl->ctx.x[1],
                      impl->ctx.x[2], impl->ctx.x[3]);
            g_counters.svc_calls.fetch_add(1, std::memory_order_relaxed);
            g_counters.RecordSvc(static_cast<u32>(impl->ctx.pending_svc));
            return HaltReason::SupervisorCall;
        }
    }

    return HaltReason::BreakLoop;
}

HaltReason ArmRecomp::StepThread(Kernel::KThread* thread) {
    impl->SampleDiagnostics(thread);
    // A context switch voids any outstanding reservation. A thread preempted
    // between its LDXR and STXR would otherwise have the STXR succeed against a
    // word another thread changed on this core meanwhile, silently losing an
    // update to a mutex or condition variable and deadlocking the guest.
    //
    // Only on an actual switch: this backend returns to the host constantly
    // (chain budget, SVCs), so clearing on every entry would void reservations
    // the same thread is still in the middle of.
    if (impl->last_thread != thread) {
        impl->last_thread = thread;
        if (impl->exclusive_monitor) {
            impl->exclusive_monitor->ClearExclusive(impl->core_index);
        }
    }
    // Block granularity is the finest this backend can step: recompiled blocks
    // are straight-line C with no per-instruction re-entry point.
    if (!impl->lookup) {
        return HaltReason::BreakLoop;
    }
    const auto initialization = impl->EnsureProcessInitialized(thread);
    if (initialization == ArmRecompProcessState::Initialization::Fatal) {
        return HaltReason::PrefetchAbort;
    }
    if (initialization == ArmRecompProcessState::Initialization::FallbackOnly ||
        impl->process_state->force_jit.load(std::memory_order_acquire) || impl->in_fallback) {
        if (!EnterFallback(thread)) {
            return HaltReason::PrefetchAbort;
        }
        return RunFallback(thread, true);
    }
    if (initialization != ArmRecompProcessState::Initialization::Ready) {
        return HaltReason::PrefetchAbort;
    }
    const RecompBlockFn block = impl->lookup(impl->ctx.pc);
    if (!block) {
        if (!EnterFallback(thread)) {
            return HaltReason::PrefetchAbort;
        }
        return RunFallback(thread, true);
    }
    impl->ctx.pending_svc = kNoPendingSvc;
    impl->ctx.halted = 0;
    block(&impl->ctx);
    if (impl->ctx.halted == kHaltUnhandled) {
        impl->ctx.halted = 0;
        if (!EnterFallback(thread)) {
            return HaltReason::PrefetchAbort;
        }
        return RunFallback(thread, true);
    }
    if (impl->ctx.pending_svc != kNoPendingSvc) {
        return HaltReason::SupervisorCall;
    }
    return HaltReason::StepThread;
}

void ArmRecomp::ClearInstructionCache() {
    if (impl->process_state->static_image_published.load(std::memory_order_acquire) &&
        !impl->process_state->force_jit.exchange(true, std::memory_order_acq_rel)) {
        LOG_WARNING(Core_ARM,
                    "recomp: executable code cache was cleared; switching this process "
                    "permanently to JIT");
    }
    if (auto* fallback = impl->fallback_signal_target.load(std::memory_order_acquire)) {
        fallback->ClearInstructionCache();
    }
}

void ArmRecomp::InvalidateCacheRange(u64 addr, std::size_t size) {
    if (impl->process_state->static_image_published.load(std::memory_order_acquire) &&
        !impl->process_state->force_jit.exchange(true, std::memory_order_acq_rel)) {
        LOG_WARNING(Core_ARM,
                    "recomp: executable code at {:#x} (size {:#x}) was invalidated; switching "
                    "this process permanently to JIT",
                    addr, size);
    }
    if (auto* fallback = impl->fallback_signal_target.load(std::memory_order_acquire)) {
        fallback->InvalidateCacheRange(addr, size);
    }
}

void ArmRecomp::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    std::memset(&ctx, 0, sizeof(ctx));
    for (size_t i = 0; i < 29; ++i) {
        ctx.r[i] = impl->ctx.x[i];
    }
    ctx.fp = impl->ctx.x[29];
    ctx.lr = impl->ctx.x[30];
    ctx.sp = impl->ctx.x[31];
    ctx.pc = impl->ctx.pc;
    ctx.pstate = (static_cast<u32>(impl->ctx.n) << 31) |
                 (static_cast<u32>(impl->ctx.z) << 30) |
                 (static_cast<u32>(impl->ctx.c) << 29) |
                 (static_cast<u32>(impl->ctx.v) << 28);
    // u128 here is a pair of 64-bit halves, matching how the generated
    // context stores each vector register.
    for (size_t i = 0; i < 32; ++i) {
        ctx.v[i][0] = impl->ctx.vreg[i][0];
        ctx.v[i][1] = impl->ctx.vreg[i][1];
    }
    ctx.fpcr = impl->fpcr;
    ctx.fpsr = impl->fpsr;
    ctx.tpidr = impl->ctx.tpidr_el0;
}

void ArmRecomp::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    for (size_t i = 0; i < 29; ++i) {
        impl->ctx.x[i] = ctx.r[i];
    }
    impl->ctx.x[29] = ctx.fp;
    impl->ctx.x[30] = ctx.lr;
    impl->ctx.x[31] = ctx.sp;
    impl->ctx.pc = ctx.pc;
    impl->ctx.n = static_cast<u8>((ctx.pstate >> 31) & 1);
    impl->ctx.z = static_cast<u8>((ctx.pstate >> 30) & 1);
    impl->ctx.c = static_cast<u8>((ctx.pstate >> 29) & 1);
    impl->ctx.v = static_cast<u8>((ctx.pstate >> 28) & 1);
    for (size_t i = 0; i < 32; ++i) {
        impl->ctx.vreg[i][0] = ctx.v[i][0];
        impl->ctx.vreg[i][1] = ctx.v[i][1];
    }
    impl->fpcr = ctx.fpcr;
    impl->fpsr = ctx.fpsr;
    // Only the guest-owned thread pointer travels in ThreadContext. The
    // read-only one is republished separately by PhysicalCore::LoadContext
    // on every switch-in, so writing it from here would overwrite the
    // kernel's TLS pointer with the guest's.
    impl->ctx.tpidr_el0 = ctx.tpidr;
}

void ArmRecomp::SetTpidrroEl0(u64 value) {
    // The emitted MRS handler for TPIDRRO_EL0 reads this out of the guest
    // context, so it has to land there and nowhere else: writing it into
    // tpidr_el0 (as this used to) destroyed the guest's own thread pointer on
    // every context switch and made the guest emit its IPC header outside the
    // TLS region the kernel parses it from.
    impl->ctx.tpidrro_el0 = value;
}

void ArmRecomp::GetSvcArguments(std::span<uint64_t, 8> args) const {
    for (size_t i = 0; i < 8; ++i) {
        args[i] = impl->ctx.x[i];
    }
}

void ArmRecomp::SetSvcArguments(std::span<const uint64_t, 8> args) {
    for (size_t i = 0; i < 8; ++i) {
        impl->ctx.x[i] = args[i];
    }
}

u32 ArmRecomp::GetSvcNumber() const {
    return static_cast<u32>(impl->ctx.pending_svc);
}

void ArmRecomp::SignalInterrupt(Kernel::KThread* thread) {
    impl->interrupted.store(true, std::memory_order_release);
    // While the JIT is running this thread it is the one that has to be woken;
    // the flag above is only read by the recompiled dispatch loop.
    if (auto* fallback = impl->fallback_signal_target.load(std::memory_order_acquire)) {
        fallback->SignalInterrupt(thread);
    }
#else
    (void)thread;
#endif
}

const Kernel::DebugWatchpoint* ArmRecomp::HaltedWatchpoint() const {
    if (const auto* fallback = impl->fallback_signal_target.load(std::memory_order_acquire)) {
        return static_cast<const ArmInterface*>(fallback)->HaltedWatchpoint();
    }
    return nullptr;
}

void ArmRecomp::RewindBreakpointInstruction() {
    if (auto* fallback = impl->fallback_signal_target.load(std::memory_order_acquire)) {
        static_cast<ArmInterface*>(fallback)->RewindBreakpointInstruction();
        Kernel::Svc::ThreadContext ctx{};
        fallback->GetContext(ctx);
        SetContext(ctx);
    }
}

} // namespace Core
