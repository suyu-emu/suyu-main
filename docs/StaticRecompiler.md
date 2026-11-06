# Static recompiler roadmap

## Product direction

Build a title-specialized, static-first Switch runtime:

```text
lawfully supplied title image
  -> format/architecture probe
  -> NSO module analysis and build-ID verification
  -> ahead-of-time native blocks
  -> suyu kernel/HLE/GPU/audio/input/filesystem runtime
  -> Dynarmic fallback for uncovered or dynamic code
```

Pure whole-program static recompilation is a per-title optimization goal, not an MVP requirement.
Indirect calls, dynamically loaded NROs, runtime-generated code, atomics, and missing instruction
semantics make a correctness-preserving fallback necessary.

The GPU command stream is not CPU code and should continue through suyu's existing GPU engines,
rasterizer, and runtime shader translation.

## Current foundation

- `core/recompiler/arm64_to_c.h` discovers AArch64 blocks and emits portable C.
- `core/arm/recomp/arm_recomp.cpp` implements the normal CPU interface and bridges generated code
  to suyu memory, SVC dispatch, and a lazy Dynarmic fallback.
- Live instruction-cache invalidation now fails safe: once executable code changes, every core in
  that process stays on Dynarmic instead of dispatching stale immutable AOT blocks.
- `core/recompiler/nso_image.{h,cpp}` now provides checked, non-Qt NSO0 inspection and segment
  decoding to both the command-line tool and in-app exporter.
- `core/recompiler/npdm_info.{h,cpp}` provides a dependency-light, structurally checked NPDM
  architecture probe; AArch32 is rejected before bytes reach the AArch64 decoder.
- `suyu/game_export.cpp` retains container/VFS work but now consumes the shared NSO decoder and
  canonical block discovery instead of maintaining a second parser and branch sweep.
- `tools/static_recompiler` provides deterministic `emit-raw`, `inspect-nso`, and `emit-nso`
  commands plus synthetic parser, JSON, LZ4, generation, native-build, and execution tests.
- Generated standalone projects require a 64-bit host, preserve page-aligned NSO segment virtual
  addresses, validate bundled file sizes, and explicitly map zeroed BSS instead of overlaying every
  segment at the text base.

## Milestones

### Primary-runtime promotion gate

The repository should be renamed to `suyu-recompiler` only after the static-first path is the
normal, continuously tested way to launch a compatible title. “Primary” does not mean deleting
Dynarmic: dynamic code, invalidated executable pages, and untranslated instructions still need a
correct fallback. It does require all of the following:

- [x] Desktop builds include the canonical `suyu_recomp` compiler by default.
- [x] Every frontend launch path selects a matching per-title AOT cache before process creation.
- [x] Executable-code invalidation permanently and safely moves that process to Dynarmic.
- [ ] Version the complete generated-image/host ABI and reject incompatible images before calling
  any image-owned function.
- [ ] Account for AOT instructions in CoreTiming and bound each dispatch slice so guest scheduling,
  interrupts, and timers remain deterministic.
- [ ] Differentially test representative scalar, SIMD/FP, memory, atomic, SVC, and AOT/JIT handoff
  cases against Dynarmic with synthetic inputs.
- [ ] Report per-process AOT instructions, fallback instructions/transitions, and uncovered PCs so
  “static-first” is measured rather than inferred from successful compilation.
- [ ] Pass an end-to-end open-homebrew launch test through the hosted runtime on Windows and Linux,
  including input, graphics, audio, filesystem/save I/O, and clean shutdown.
- [ ] Keep the AOT path within an agreed performance envelope of Dynarmic on the same workload; a
  default path that is pathologically slower is not ready for promotion.

These are correctness and product gates, not coverage theater. A proprietary title compiling to a
large C tree, or reaching a menu once, is useful evidence but cannot by itself satisfy them.

### M0: canonical emitter CLI

- One frontend for the canonical emitter; no duplicated decoder/runtime.
- Strict input/address/module validation and checked output writes.
- Cross-platform generation/build/run smoke test with no proprietary inputs.

### M1: shared executable analysis

- Completed slice: checked NSO0 layout/build-ID/segment inspection, bounded decoding, optional LZ4,
  explicit ZBIC detection, unverified-hash signaling, structurally validated NPDM architecture
  probing, and architecture-honest human/JSON CLI output.
- Completed slice: `emit-nso` decodes the three segments, validates a conventional MOD0/AArch64
  entry, rejects AArch32, and emits an address-correct C project behind an NPDM or explicit
  assumption gate.
- Real-module validation resolves the entry stub's module-relative MOD0 pointer across text,
  rodata, and data; MOD0 is not assumed to reside beside the entry code.
- Required NSO0 SHA-256 hashes are checked after decompression before analysis or emission.
- Move exported-symbol roots, relocation roots, and code-pointer scanning out of the Qt exporter.
- Key output by build ID plus hashes of the post-update, post-mod decompressed segments.
- Keep decryption/key management outside the recompiler core.

### M2: correctness oracle and telemetry

- Differentially execute synthetic AArch64 blocks under AOT and vendored Dynarmic.
- Compare GPRs, vector registers, NZCV, FPCR/FPSR, TLS registers, memory writes, SVC exits, and
  exceptions.
- Record compile-time unsupported-opcode histograms and runtime fallback PCs/transitions.
- Report static execution ratio per module; prioritize semantics by measured frequency.

### M3: hosted hybrid proof

- Start with a small open-source AArch64 libnx program.
- Let guest `rtld` run under Dynarmic first; transition into compiled `main`/SDK blocks after the
  real loader has applied relocations.
- Preserve complete CPU state across every AOT/JIT transition.
- Add code-page invalidation and safe handling for dynamically loaded modules.
- Define success as deterministic boot, first frame, input, audio, save I/O, and useful coverage
  metrics—not merely successful C compilation.

### M4: scalable backend

- Use the vendored Dynarmic AArch64 frontend and optimized IR as the instruction-semantics source
  of truth.
- First implement an IR-to-C backend behind the existing `ArmRecomp` ABI.
- Then evaluate relocatable native object or LLVM emission to avoid multi-gigabyte C output and
  long compiler passes.
- Keep Dynarmic as the reference engine and fallback even as static coverage grows.

### M5: specialized runtime packaging

- Link verified per-module AOT objects into a per-title `suyu-cmd` runtime.
- Refuse module/build-ID mismatches instead of attempting to run stale output.
- Provide a local Windows packager for a user-supplied plaintext deconstructed tree. Emit an
  audited manifest, deterministic checksums, explicit application/applet launch parameters, and a
  title-specialized hosted runtime without ever launching it during packaging.
- Distribute runtime code and user configuration only. Generated title code, title-specialized
  executables, and copied title content remain local.

## Immediate correctness work

1. Add hosted AOT-vs-Dynarmic differential tests before expanding instruction coverage.
2. Add coverage/fallback counters and first-failure diagnostics.
3. Fix or deliberately delegate exclusives/LSE atomics and remaining indirect-target,
   TLS/relocation, and dynamically loaded-code cases.

Completed correctness gates include loader-provided module discovery, full
GPR/vector/NZCV/FPCR/FPSR/TLS state transfer across AOT/JIT handoffs, and conservative
instruction-cache invalidation: generated code is immutable, so the first live invalidation
permanently delegates that process to Dynarmic.

## Development and distribution boundary

Develop with synthetic fixtures and open homebrew. Accept only locally supplied inputs that the
user is entitled to analyze. Never commit or distribute console keys, firmware, game images,
extracted assets, instruction-containing traces, or generated code/binaries derived from
proprietary titles. Preserve suyu's GPL notices and audit the license/provenance of external work
before incorporating it.

Useful references include [N64Recomp](https://github.com/N64Recomp/N64Recomp) for the generated-C
and per-title runtime model, and the rapidly evolving
[mk8-recomp experiment](https://github.com/dougchansan/mk8-recomp) for recent work on this exact
suyu AOT/hybrid path. Audit and selectively integrate compatible changes instead of copying an
unreviewed fork wholesale.
