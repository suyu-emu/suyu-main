// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <array>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include "core/arm/arm_interface.h"
#include "core/arm/recomp/recomp_guard_gen.h"

namespace Kernel {
class KProcess;
}

namespace Core {

class System;
class DynarmicExclusiveMonitor;
class ExclusiveMonitor;

/**
 * Signature of a recompiled block produced by suyu::recomp::EmitProject.
 *
 * The generated code is plain C operating on its own GuestContext, but the
 * only state it actually needs from us is the register file, PC and NZCV, so
 * the bridge below re-declares a layout-compatible view of that prefix rather
 * than pulling the generated header into the emulator build.
 */
struct RecompGuestRegs {
    u64 x[32];
    u64 pc;
    u8 n, z, c, v;
};

using RecompBlockFn = void (*)(void*);
using RecompLookupFn = RecompBlockFn (*)(u64 pc);

/**
 * Registers the lookup function of a loaded recompiled image.
 *
 * Set before starting a process to have it run on ArmRecomp instead of the
 * JIT; pass nullptr to go back to the normal backend. Kept as a global rather
 * than threaded through the loader because the choice is per-image and has to
 * be visible at KProcess::InitializeInterfaces time, which the loader doesn't
 * own.
 */
void SetRecompLookup(RecompLookupFn lookup);
/// Selects the maximum nonrecursive module-local slice for the loaded image ABI.
/// Legacy direct-call images must remain at 32 to fit the guest fiber stack.
void SetRecompLongSlices(bool enabled);
/// True only after every loaded compiled module negotiated guard version 2.
void SetRecompCodeGuardReady(bool ready);
bool IsRecompCodeGuardReady();

/// ABI 6 (FM1): the arguments the loader passes to each module's
/// recomp_image_fastmem_v1. They describe this host's page table and its view
/// of the generated context; a module that was compiled against anything else
/// answers 0 and the bundle is refused.
struct RecompFastmemLayout {
    u32 page_bits;
    u32 stride_log2;
    u64 pointer_mask;
    u32 off_table;
    u32 off_limit;
};
RecompFastmemLayout GetRecompFastmemLayout();
/// True only after every loaded module is ABI 6 and passed the handshake above.
/// SUYU_RECOMP_FASTMEM=0 still keeps the fast path off at run time.
void SetRecompFastmemReady(bool ready);
bool IsRecompFastmemReady();

/// ABI 6 feature GG1 (generation code guard). The loader calls this after
/// SetRecompLookup, with what each GG1 module's recomp_image_guard_gen_v1
/// returned, and before the process is created. The guard only engages with
/// guard-v2 negotiated and without SUYU_RECOMP_GUARD_GEN=0; otherwise every
/// module verifies on every entry. Returns whether it engaged.
bool SetRecompGuardGenModules(std::vector<RecompGuardGen::Module> modules);

/// ABI 6 feature FPX1 (exact native FP): the arguments the loader passes to each
/// FPX1 module's recomp_image_fpx_v1, this host's view of the generated
/// context's FP fields and of its kill-switch bit.
struct RecompFpxLayout {
    u32 off_fpcr;
    u32 off_fpsr;
    u64 inhibit_bit;
};
RecompFpxLayout GetRecompFpxLayout();
/// True only after every loaded module reports FPX1 and passed that handshake.
/// While set, ArmRecomp keeps the host FP mode the fast paths rely on (MXCSR or
/// FPCR, see core/arm/recomp/guest_fp_env.h) on every guest-core thread;
/// SUYU_RECOMP_FPX=0 turns the fast paths off through the kill-switch bit.
void SetRecompFpxReady(bool ready);
bool IsRecompFpxReady();

/// Called once per loaded module when a process starts, so each recompiled
/// image can be told where its module actually landed. Addresses baked in by
/// the static pass are module-relative - the loader picks the real base at run
/// time - so without this every pointer the guest computes is short by that
/// base and lands near null.
/// `index` is the module's position in load order, lowest base first, and
/// `build_id` its build ID as 64 lower-case hex digits (empty if unknown). The
/// build ID is what identifies a module: names do not (a game's main NSO is
/// named after the game, "cross2_Release.nss", its others "nnSdk" and
/// "multimedia"), and neither does a position among the images, because an
/// export may leave modules out. See RecompGaps::MatchImage.
using RecompBaseFn = void (*)(size_t index, const char* module, const char* build_id, u64 base);
void SetRecompBaseSetter(RecompBaseFn setter);

/// Called once per process after the base setter has seen every loaded module
/// and before the first block runs. Logs any registered image left without a
/// base and returns how many of those have their module loaded; a strict run
/// refuses to start when that is nonzero.
using RecompBindCheckFn = size_t (*)();
void SetRecompBindCheck(RecompBindCheckFn check);

// Installed only for a stopped, explicitly prepared static-image session.
using RecompModules = std::map<u64, std::string>;
using RecompPrepareFn = bool (*)(const RecompModules& modules);
void SetRecompPrepareCallback(RecompPrepareFn callback);
bool HasRecompPrepareCallback();
// Must complete before the application process is published to the applet manager.
bool PrepareRecompProcess(Kernel::KProcess& process, const RecompModules& modules);

/// Returns the registered lookup, or nullptr when no recompiled image is
/// loaded and the JIT should be used.
RecompLookupFn GetRecompLookup();

/// What the CPU is actually doing, for display while a game is running.
///
/// Whether execution is statically recompiled is otherwise only visible in a
/// coverage file written after the fact, which is no use to someone watching
/// the game. `jit_transitions` is the number that settles it: an image that
/// never reaches the JIT reports zero, and one transition is one too many.
struct RecompLiveStats {
    u64 static_blocks;      ///< blocks executed from recompiled images
    u64 jit_transitions;    ///< times execution had to leave them
    u64 forced_cutoff_pc;   ///< diagnostic static-block cutoff handoff PC
    u64 forced_cutoff_blocks;
    bool backend_active;    ///< ArmRecomp is the CPU for this process
    bool jit_available;     ///< false when built without a dynamic recompiler
    /// No JIT fallback is permitted: uncovered code stops execution rather than
    /// handing off. This is what separates a "suyu static AOT" run from a
    /// "Hybrid AOT + JIT" one - both execute recompiled code, but only the
    /// hybrid one is allowed to leave it - so the frontend cannot name the
    /// running backend without it.
    bool strict_mode;
};
RecompLiveStats GetRecompLiveStats();

// Process-wide monotonically accumulated diagnostic counters. Fields are sampled
// independently; callers may subtract a stopped-session baseline.
struct RecompExecutionStats {
    u64 blocks{};
    u64 svc_calls{};
    u64 lookup_misses{};
    u64 unhandled{};
    u64 no_fallback{};
};
RecompExecutionStats GetRecompExecutionStats();
std::array<u64, 4> GetRecompCurrentPcs();

/**
 * CPU backend that executes statically recompiled AArch64 rather than JITing
 * it.
 *
 * The point of routing this through ArmInterface is that everything above the
 * CPU in suyu - the HLE kernel, every service, the GPU - reaches the guest
 * only through this interface. Satisfying it means a recompiled game gets the
 * real HLE and GPU stack instead of the generated runtime's stub SVC handler,
 * which is what stops a recompiled commercial title from getting past its
 * first service call.
 *
 * Execution model: RunThread runs recompiled blocks back to back until the
 * generated code parks on an SVC (it writes the instruction's imm and the
 * resume PC into the context, then returns), at which point we hand control
 * back to the kernel with HaltReason::SupervisorCall exactly as the dynarmic
 * backend does. The kernel then reads arguments through GetSvcArguments,
 * dispatches the real service call, and writes results back through
 * SetSvcArguments before resuming us.
 */
class ArmRecomp final : public ArmInterface {
public:
    /// `process`, `exclusive_monitor` and `core_index` are only used to build a
    /// dynarmic JIT lazily, the first time a PC is reached that the static pass
    /// never covered (an indirect call into code no heuristic found). Without
    /// that fallback such a gap is terminal: the thread is suspended for a
    /// debugger that is not attached and the game hangs on a black screen with
    /// no forward progress.
    explicit ArmRecomp(System& system, bool uses_wall_clock, RecompLookupFn lookup,
                       Kernel::KProcess* process, ExclusiveMonitor* exclusive_monitor,
                       std::size_t core_index);
    ~ArmRecomp() override;

    HaltReason RunThread(Kernel::KThread* thread) override;
    HaltReason StepThread(Kernel::KThread* thread) override;

    void ClearInstructionCache() override;
    void InvalidateCacheRange(u64 addr, std::size_t size) override;

    Architecture GetArchitecture() const override {
        return Architecture::AArch64;
    }

    void GetContext(Kernel::Svc::ThreadContext& ctx) const override;
    void SetContext(const Kernel::Svc::ThreadContext& ctx) override;
    void SetTpidrroEl0(u64 value) override;

    void GetSvcArguments(std::span<uint64_t, 8> args) const override;
    void SetSvcArguments(std::span<const uint64_t, 8> args) override;
    u32 GetSvcNumber() const override;

    void SignalInterrupt(Kernel::KThread* thread) override;

    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override;
    void RewindBreakpointInstruction() override;

friend bool PrepareRecompProcess(Kernel::KProcess&, const RecompModules&);

private:
    /// Builds the JIT fallback if needed and marks this thread as running on
    /// it. Returns false when no JIT can be built (no process/monitor).
    bool EnterFallback();
    /// Runs the JIT fallback for one scheduling slice, syncing guest state in
    /// and back out, and returns to recompiled execution once the PC is covered
    /// again.
    HaltReason RunFallback(Kernel::KThread* thread);

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Core
