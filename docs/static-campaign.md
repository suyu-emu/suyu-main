# Static recompilation checkpoint

v0.0.10 keeps the suyu HLE, GPU, audio and service stack while running ahead-of-time AArch64 code. Static is experimental and can be substantially slower. Use Hybrid AOT + JIT for best performance; retain Dynarmic JIT as the baseline.

## What the campaign added

- Expanded decoder coverage and floating-point state handling, checked with synthetic instruction, arithmetic and differential suites.
- Stopped block discovery walking long padding runs, while retaining real roots and explicit traps.
- Exposed every aligned instruction within discovered blocks as a guarded side entry. This removed the manual computed-target root requirement for the fixed-module paths tested so far.
- Added manifest-validated automatic title bundles and routed source-backed library entries through the current host. This avoids stale detached executables bypassing the active bundle.
- Retained per-CPU relocation initialization and serialized its guest-memory writes. Collapsing that lifecycle to one raw process pointer had changed later CPU state and caused a boot regression.
- Added ABI 4's bounded, nonrecursive module-local dispatcher. Ordinary branch boundaries can remain inside the module loop instead of returning to the host each time. Full byte guards still run; no temporary guard bypass/cache experiment is part of the release.
- Added separate no-JIT binary builds, symbol/input audits, and boot-armed TAS recording and playback.

## What went wrong during validation

An older replay consumed every command and ended on an expected title image. That established the title path only; it was incorrectly treated as race coverage. Later changes could therefore pass the test while missing the controller prompt or menu route.

Some diagnostic DLL combinations fell back immediately and executed no static blocks. Their successful images were incorrectly attributed to static optimizations. The original positive branch-patch claims were withdrawn. Nonzero static work, zero fallback attempts and a genuinely no-JIT host are now separate checks.

Boot synchronization aligns input origin, but it cannot make different CPU/rendering workloads advance identically. A slower path can enter attract mode, consume inputs at a different screen, or still be introducing a course when the faster run reaches the grid. Neither an EOF counter nor an approximate image hash is sufficient alone.

## How to prevent repeats

1. Pin the source revision, executable and module hashes, generated ABI, update identity and replay hash. Reject stale module builds.
2. Validate the reference recording visually at the actual requested milestone before accepting it as a fixture.
3. Keep three verdicts: exact command completion, visual milestone reached, and no-JIT execution. Retain exact-EOF failures even if later idle observation reaches the milestone.
4. Use separate functional timing fixtures when necessary, but never compare different recordings or different milestones as an identical-work benchmark.
5. Review interval screenshots: controller prompt, menu path, attract rendering, course selection and race entry. Full-race testing is a separate milestone.
6. Gate releases on synthetic slice/guard/branch tests and both regular and no-JIT platform builds. Code changes must still reject before guest effects; large slices must remain bounded and nonrecursive.
7. Profile current artifacts on an idle machine. Treat dispatcher, instruction guards, memory helpers, floating-point emulation and host rendering as candidates until samples identify their cost.

## Current limits

Local guarded no-JIT tests reach controller prompts, menus, attract rendering and the race starting grid. The grid was visually verified during bounded idle observation after EOF, with zero fallback attempts and no JIT available. Race entry takes longer than the JIT fixture allows; frame-exact timing and full-race validation are still open. A completed replay proves only the paths observed. Fixed NSO coverage does not include arbitrary runtime-loaded/generated code, and AArch32 is unsupported.

Historical speedup numbers predate the current guards and expanded floating-point semantics and used a retired title-screen fixture. They are not v0.0.10 gameplay performance measurements. Release notes recommend Hybrid while static optimization continues.
