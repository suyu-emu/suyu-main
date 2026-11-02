// smoke_gg_host: the ABI 6 generation code guard (GG1) end to end. The generated
// module (through gg_c.c) is driven by the real host manager,
// src/core/arm/recomp/recomp_guard_gen.cpp, whose hooks are called exactly as
// Core::Memory, DeviceMemoryManager and ArmRecomp call them. Every "must bump"
// case verifies a block, changes its code bytes, fires the hook and expects the
// next entry to abort through the unchanged guard (exit 86). Controls expect no
// verification. The race modes run the protocol across threads.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/arm/recomp/recomp_guard_gen.h"

extern "C" {
void ggc_init(void);
std::uint32_t* ggc_handshake(std::uint32_t, std::uint64_t*, std::uint64_t*, const std::uint64_t**);
void* ggc_new_context(void);
unsigned long ggc_enter(void*, std::uint64_t, std::uint64_t);
void ggc_expect_abort_at(std::uint64_t);
void ggc_mutate(void);
void ggc_move_code(std::uint64_t);
std::uint32_t ggc_seen(unsigned);
const void* ggc_table(void);
std::uint64_t ggc_table_limit(void);
void ggc_set_host_write_hook(void (*)(std::uint64_t, std::uint32_t));
void ggc_watch(std::uint64_t, std::uint64_t);
void ggc_run_stores(std::uint64_t, int);
void ggc_host_write(std::uint64_t, std::uint32_t);
}

namespace GG = Core::RecompGuardGen;

#define CHECK(x)                                                                                 \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                            \
            return 1;                                                                            \
        }                                                                                        \
    } while (0)

namespace {

// Stand-ins for the emulator's page tables (only their identity matters) and
// for the physical page behind the module's code.
int kTable;
int kOtherTable;
// The module's own table is the real entry array gg_c.c uses (watch words
// included); set at the start of main.
const void* table = &kTable;
const void* const other_table = &kOtherTable;
constexpr std::uint64_t kCodePa = 0x80001000;
constexpr std::uint64_t kBlock = 0x1000; // add x0,x0,#1; b 0x1000 (two words)

std::uint32_t* word;
GG::Module module_info;
void* ctx;

// The host's watch callback, as ArmRecomp passes it to Activate.
void Watch(std::uint64_t va, std::uint64_t size) {
    ggc_watch(va, size);
}

// What Core::Memory does after every write it performs, and when it hands out
// a writable raw pointer (NoteRecompWrite / NoteRecompPointer).
void HostWriteHook(std::uint64_t va, std::uint32_t size) {
    if (!GG::Watching()) {
        return;
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (GG::AnyWatched(ggc_table(), 32, 12, ggc_table_limit(), va, size)) {
        GG::OnWatchedWrite(table, va, size);
    }
}
void ExposeHook(std::uint64_t va, std::uint64_t size) {
    if (GG::Watching() &&
        (GG::Prelogging() || GG::AnyWatched(ggc_table(), 32, 12, ggc_table_limit(), va, size))) {
        GG::OnPointerExposed(table, va, size);
    }
}

// What Core::Memory reports while the loader builds the process: a new table,
// the code pages mapped writable, then the page holding the blocks made RX.
// (The protect splits the mapping, so activation must see through the split.)
void CreateProcess() {
    GG::OnPageTableSwap(table);
    GG::OnMap(table, 0x0, 0x3000, kCodePa - 0x1000, true);
    GG::OnProtect(table, 0x1000, 0x1000, false);
}

std::uint32_t Word() {
    return reinterpret_cast<std::atomic<std::uint32_t>*>(word)->load();
}

// Loads the module, registers it and activates it with a stable probe.
int Setup(bool activate = true) {
    ggc_init();
    std::uint64_t lo = 0, end = 0;
    const std::uint64_t* base = nullptr;
    CHECK(ggc_handshake(1, &lo, &end, &base) == nullptr); // v1 host: no store watch
    word = ggc_handshake(GG::kHostVersion, &lo, &end, &base);
    CHECK(word != nullptr && base != nullptr);
    CHECK(lo == 0x1000 && end > lo);
    CHECK(Word() == GG::kVerifyAlways);
    module_info = {word, base, lo, end};
    GG::SetModules({module_info}, true);
    CHECK(GG::Watching());
    CHECK(Word() == GG::kVerifyAlways);
    ctx = ggc_new_context();
    // Not active yet: every entry verifies.
    CHECK(ggc_enter(ctx, kBlock, 0) == 2);
    CHECK(ggc_enter(ctx, kBlock, 0) == 2);
    CreateProcess();
    CHECK(ggc_enter(ctx, kBlock, 0) == 2);
    if (activate) {
        GG::Activate(7, table, Watch);
        CHECK(GG::IsActive(7) && !GG::IsActive(8));
        CHECK(Word() != GG::kVerifyAlways && Word() != 0);
        CHECK(GG::GetStats().sticky == 0);
    }
    return 0;
}

// Verified once, then skipped: the state every "must bump" case starts from.
int VerifiedThenSkipping() {
    CHECK(ggc_enter(ctx, kBlock, 0) == 2);
    CHECK(ggc_seen(0) == Word());
    CHECK(ggc_enter(ctx, kBlock, 0) == 0);
    CHECK(ggc_enter(ctx, kBlock, 0) == 0);
    return 0;
}

// Mutate, fire `hook`, and require the next entry to be rejected.
template <typename Hook>
int MustCatch(Hook&& hook) {
    if (VerifiedThenSkipping()) return 1;
    ggc_mutate();
    hook();
    ggc_expect_abort_at(kBlock);
    ggc_enter(ctx, kBlock, 99);
    std::fprintf(stderr, "FAIL: a changed block ran after the hook\n");
    return 1;
}

// `change` alters the block's code through some path; the next entry must be
// rejected.
template <typename Change>
int MustCatchChange(Change&& change) {
    if (VerifiedThenSkipping()) return 1;
    change();
    ggc_expect_abort_at(kBlock);
    ggc_enter(ctx, kBlock, 99);
    std::fprintf(stderr, "FAIL: a block changed through a watched path still ran\n");
    return 1;
}

// Fire `hook`; the next entry must still skip.
template <typename Hook>
int MustNotBump(Hook&& hook) {
    if (VerifiedThenSkipping()) return 1;
    const std::uint32_t before = Word();
    hook();
    CHECK(Word() == before);
    CHECK(ggc_enter(ctx, kBlock, 0) == 0);
    return 0;
}

int RaceBump() {
    // One thread keeps moving the generation; three enter the same block.
    // Nothing changes the code, so no entry may abort. Check the observed
    // generation after the bumper stops: a separate publication counter is
    // not synchronized with what another thread can already read from the
    // module's generation word.
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> verifications{0}, entries{0};
    std::thread bumper([&] {
        for (int i = 0; i < 200000; ++i) {
            GG::OnInvalidateAll();
        }
        stop.store(true);
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&] {
            void* c = ggc_new_context();
            while (!stop.load()) {
                verifications += ggc_enter(c, kBlock, 0) != 0;
                ++entries;
            }
        });
    }
    bumper.join();
    for (auto& t : threads) t.join();
    CHECK(ggc_seen(0) <= Word());
    // Quiescent: one more entry verifies at the final generation, then skips.
    ggc_enter(ctx, kBlock, 0);
    CHECK(ggc_seen(0) == Word());
    CHECK(ggc_enter(ctx, kBlock, 0) == 0);
    std::printf("race-bump: %llu entries, %llu verified, generation %u\n",
                (unsigned long long)entries.load(), (unsigned long long)verifications.load(),
                Word());
    return 0;
}

int RaceMutate(unsigned delay_us) {
    // An entry thread runs the block continuously (skipping). The mutator
    // changes the code, bumps, then publishes with release. An entry that
    // starts after it acquired the flag happens after the bump, so it must
    // verify and abort; completing it is the failure this test exists for.
    // One entry thread: the MSVC CRT resets SIGABRT to its default before
    // running the handler, so a second thread aborting at the same moment
    // would end the process with 3 instead of 86. race-bump covers several
    // verifiers racing on one block.
    std::atomic<bool> published{false};
    std::atomic<int> late_runs{0};
    ggc_expect_abort_at(kBlock);
    std::vector<std::thread> threads;
    for (int t = 0; t < 1; ++t) {
        threads.emplace_back([&] {
            void* c = ggc_new_context();
            for (;;) {
                const bool after = published.load(std::memory_order_acquire);
                ggc_enter(c, kBlock, 99);
                if (after) {
                    ++late_runs;
                    std::fprintf(stderr, "FAIL: entry after the bump skipped verification\n");
                    std::_Exit(1);
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
    ggc_mutate();
    GG::OnInvalidate(kBlock, 4); // the guest's IC IVAU after its store
    published.store(true, std::memory_order_release);
    for (auto& t : threads) t.join(); // never returns: an entry aborts
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    table = ggc_table();
    ggc_set_host_write_hook(HostWriteHook);
    const bool activate = mode != "log-alias" && mode != "log-trim" && mode != "untracked" &&
                          mode != "unstable" && mode != "unmapped-code" && mode != "overflow" &&
                          mode != "pointer-prelog" &&
                          mode != "disabled";
    if (mode == "disabled") {
        ggc_init();
        std::uint64_t lo, end;
        const std::uint64_t* base;
        word = ggc_handshake(GG::kHostVersion, &lo, &end, &base);
        CHECK(word);
        GG::SetModules({{word, base, lo, end}}, false); // e.g. SUYU_RECOMP_GUARD_GEN=0
        CHECK(!GG::Watching());
        GG::OnPageTableSwap(table);
        GG::OnMap(table, 0x1000, 0x1000, kCodePa, false);
        GG::Activate(7, table, Watch);
        GG::OnInvalidateAll();
        CHECK(Word() == GG::kVerifyAlways);
        ctx = ggc_new_context();
        for (int i = 0; i < 4; ++i) CHECK(ggc_enter(ctx, kBlock, 0) == 2);
        CHECK(ggc_seen(0) == 0);
        std::printf("PASS %s\n", mode.c_str());
        return 0;
    }
    if (Setup(activate)) return 1;

    // ---- the protocol itself ----
    if (mode == "skip") {
        if (VerifiedThenSkipping()) return 1;
        const std::uint32_t g = Word();
        GG::OnInvalidateAll();
        CHECK(Word() == g + 1);
        CHECK(ggc_enter(ctx, kBlock, 0) == 2); // moved: verifies once
        CHECK(ggc_seen(0) == g + 1);
        CHECK(ggc_enter(ctx, kBlock, 0) == 0);
        GG::Forget(); // the loader drops the bundle; the word is left alone
        CHECK(!GG::Watching() && Word() == g + 1);
    } else if (mode == "mutate-no-bump") {
        // The contract, stated as a test: without an event that moves the
        // generation, a changed word is not re-read. DESIGN.md section 2 lists
        // every path that must move it.
        if (VerifiedThenSkipping()) return 1;
        ggc_mutate();
        CHECK(ggc_enter(ctx, kBlock, 0) == 0);
    }
    // ---- hooks that must end skipping (exit 86) ----
    else if (mode == "hook-map") {
        return MustCatch([] { GG::OnMap(table, 0x1000, 0x1000, 0x90000000, false); });
    } else if (mode == "hook-unmap") {
        return MustCatch([] { GG::OnUnmap(table, 0x0, 0x2000); });
    } else if (mode == "hook-protect-rx") {
        return MustCatch([] { GG::OnProtect(table, 0x1000, 0x1000, false); });
    } else if (mode == "hook-protect-rw") {
        return MustCatch([] {
            GG::OnProtect(table, 0x1000, 0x1000, true);
            GG::OnInvalidateAll(); // still verify-always afterwards
        });
    } else if (mode == "hook-alias") {
        return MustCatch([] { GG::OnMap(other_table, 0x7000000, 0x1000, kCodePa, true); });
    } else if (mode == "hook-device") {
        return MustCatch([] { GG::OnDeviceMap(table, 0x1004, 4); });
    } else if (mode == "hook-ivau") {
        return MustCatch([] { GG::OnInvalidate(0x1004, 4); });
    } else if (mode == "hook-ivau-all") {
        return MustCatch([] { GG::OnInvalidateAll(); });
    } else if (mode == "hook-new-table") {
        return MustCatch([] { GG::OnPageTableSwap(other_table); });
    } else if (mode == "hook-new-process") {
        return MustCatch([] { GG::Activate(9, table, Watch); });
    }
    // ---- writes to a watched code page (exit 86) ----
    else if (mode == "guest-store") {
        // A guest STR/STP/STUR/STLR into the code page, FM1 fast path on: the
        // watch word must send every one of them to the host callback.
        return MustCatchChange([] { ggc_run_stores(0x1000, 1); });
    } else if (mode == "guest-store-slow") {
        // The same with FM1 off (SUYU_RECOMP_FASTMEM=0): the ABI 5 walk.
        return MustCatchChange([] { ggc_run_stores(0x1000, 0); });
    } else if (mode == "guest-store-cross") {
        // Stores that start on the unwatched page below and cross into code.
        return MustCatchChange([] { ggc_run_stores(0x0ffc, 1); });
    } else if (mode == "host-write") {
        // An HLE, loader or cheat write through Core::Memory.
        return MustCatchChange([] { ggc_host_write(0x1000, 0xd503201fu); });
    } else if (mode == "pointer-exposed") {
        // A writable raw pointer into the code page, then a write through it.
        return MustCatchChange([] {
            ExposeHook(0x1000, 0x1000);
            ggc_mutate();
        });
    } else if (mode == "jit-fallback") {
        return MustCatch([] { GG::OnJitFallback(); });
    } else if (mode == "pointer-prelog") {
        // A raw pointer into the code handed out before the first run: pinned.
        ExposeHook(0x1004, 4);
        GG::Activate(7, table, Watch);
        CHECK(GG::IsActive(7) && GG::GetStats().sticky == 1);
        CHECK(ggc_enter(ctx, kBlock, 0) == 2 && ggc_enter(ctx, kBlock, 0) == 2);
    } else if (mode == "ctl-writes-elsewhere") {
        // Stores, host writes and pointers outside the code: nothing moves.
        return MustNotBump([] {
            ggc_run_stores((0x10ull << 12) | 0x100, 1);
            ggc_host_write(0x10200, 1);
            ExposeHook(0x10000, 0x1000);
        });
    } else if (mode == "hook-rebase") {
        // The module moves; its own set_base drops it to verify-always, and
        // the code at the new base no longer matches.
        if (VerifiedThenSkipping()) return 1;
        ggc_move_code(0x41000);
        CHECK(Word() == GG::kVerifyAlways);
        GG::OnInvalidateAll(); // the host notices the new base: sticky from here
        CHECK(Word() == GG::kVerifyAlways && GG::GetStats().sticky == 1);
        CHECK(ggc_enter(ctx, 0x41000, 0) == 2);
        CHECK(ggc_enter(ctx, 0x41000, 0) == 2);
        ggc_mutate();
        ggc_expect_abort_at(0x41000);
        ggc_enter(ctx, 0x41000, 99);
        return 1;
    }
    // ---- activation decides sticky ----
    else if (mode == "log-alias") {
        // An alias of the code's physical page that already exists when the
        // process first runs.
        GG::OnPageTableSwap(other_table);
        GG::OnMap(other_table, 0x7000000, 0x2000, kCodePa - 0x1000, true);
        GG::Activate(7, table, Watch);
        CHECK(GG::GetStats().sticky == 1 && Word() == GG::kVerifyAlways);
        CHECK(ggc_enter(ctx, kBlock, 0) == 2 && ggc_enter(ctx, kBlock, 0) == 2);
    } else if (mode == "log-trim") {
        // The same alias, unmapped again before the first run: no longer a
        // reason. A mapping of the module's own range is not an alias either.
        GG::OnMap(other_table, 0x7000000, 0x3000, kCodePa - 0x1000, true);
        GG::OnMap(table, 0x1000, 0x1000, kCodePa, false); // the code's own mapping again
        GG::OnUnmap(other_table, 0x7000000, 0x1000);
        GG::OnUnmap(other_table, 0x7001000, 0x2000);
        GG::Activate(7, table, Watch);
        CHECK(GG::GetStats().sticky == 0 && Word() != GG::kVerifyAlways);
        CHECK(ggc_enter(ctx, kBlock, 0) == 2 && ggc_enter(ctx, kBlock, 0) == 0);
    } else if (mode == "unstable") {
        // Code the guest may store to (W+X, or made writable again).
        GG::OnProtect(table, 0x1000, 0x1000, true);
        GG::Activate(7, table, Watch);
        CHECK(GG::IsActive(7) && GG::GetStats().sticky == 1);
        CHECK(ggc_enter(ctx, kBlock, 0) == 2 && ggc_enter(ctx, kBlock, 0) == 2);
    } else if (mode == "unmapped-code") {
        // Part of the guarded span is not mapped at all.
        GG::OnUnmap(table, 0x1000, 0x1000);
        GG::Activate(7, table, Watch);
        CHECK(GG::IsActive(7) && GG::GetStats().sticky == 1);
    } else if (mode == "untracked") {
        // A table created before the modules were registered: its log is
        // incomplete, so nothing about it is proven.
        GG::OnMap(other_table, 0x1000, 0x1000, kCodePa, false);
        GG::Activate(7, other_table, Watch);
        CHECK(GG::IsActive(7) && GG::GetStats().sticky == 1);
        CHECK(ggc_enter(ctx, kBlock, 0) == 2 && ggc_enter(ctx, kBlock, 0) == 2);
    } else if (mode == "overflow") {
        // More live mappings than the log keeps: nothing is proven.
        GG::OnPageTableSwap(other_table);
        for (std::uint64_t i = 0; i <= GG::kMaxMapLog; ++i) {
            GG::OnMap(other_table, 0x100000000 + i * 0x2000, 0x1000, 0x200000000 + i * 0x1000, true);
        }
        CHECK(GG::GetStats().map_log_overflow);
        GG::Activate(7, table, Watch);
        CHECK(GG::IsActive(7) && GG::GetStats().sticky == 1);
    }
    // ---- hooks that must not bump ----
    else if (mode == "ctl-ivau-elsewhere") {
        return MustNotBump([] { GG::OnInvalidate(0x9000, 64); });
    } else if (mode == "ctl-map-elsewhere") {
        return MustNotBump([] {
            GG::OnMap(table, 0x9000, 0x1000, 0x90000000, true);
            GG::OnMap(other_table, 0x1000, 0x1000, 0x90000000, true); // same VA, other process
            GG::OnProtect(other_table, 0x1000, 0x1000, true);
            GG::OnUnmap(other_table, 0x1000, 0x1000);
            GG::OnDeviceMap(other_table, 0x1000, 0x1000);
        });
    }
    // ---- concurrency ----
    else if (mode == "race-bump") {
        if (RaceBump()) return 1;
    } else if (mode.rfind("race-mutate-", 0) == 0) {
        return RaceMutate(static_cast<unsigned>(std::stoul(mode.substr(12))));
    } else {
        std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
        return 2;
    }
    std::printf("PASS %s\n", mode.c_str());
    return 0;
}
