# Static recompiler smoke tests

Run from the repository root:

```sh
python tests/recompiler_smoke/run.py
```

Requires Python 3, CMake 3.20+, and a C/C++20 compiler (MSVC or GCC).
The runner uses CMake's normal compiler discovery; `--cc`, `--cxx`, and
`--cmake` accept explicit tool paths. `--source` can select another suyu tree.
All generated files and binaries live in a temporary directory.

The suite exports only synthetic arithmetic, branch and memory instructions
using the current emitter and runtime: as ABI 5, as ABI 6 with the FM1
page-table fast path (`SUYU_RECOMP_AB_FASTMEM=1`), and as ABI 6 with FM1 and
the generation code guard. The ABI 5 and ABI 6 outputs must each match a golden
SHA-256 of the whole generated tree, so any change to them is deliberate; the
ABI 6 output may differ from ABI 5 only in the runtime, export and CMake files.
Every runtime case below runs against all three builds.

It checks the image ABI, exact slice budgets of 1/3/4096,
bounded stack use, SVC and breakpoint stops, interior entries, BLR through x30,
and ordinary, cross-page, special, mutated, and unmapped code guards. Expected
guard aborts run in separate processes and must identify the rejected PC before
any guest effect. Each runtime case has a 15-second timeout.

The `mem-*` modes run one block of loads and stores (every width, pairs, Q,
LDAR/STLR, LDXR/STXR) against a table shaped like `Common::PageTable`, with a
logging host callback. Ordinary, unmapped, GPU-tracked and debug pages, page
crossings into non-adjacent host pages, unaligned accesses, a non-page-aligned
address limit, high and tagged address bits, and a guest read-only page are
covered. Each case is checked against a byte model, and on ABI 6 is run with
the fast path on and off, which must give the same registers, memory and
callback sequence. `mem-ordinary` also proves that the fast path, not the ABI
5 walk, serves ordinary accesses.

A third export turns FPX1 (exact native FP) on as well
(`SUYU_RECOMP_AB_FPX=1`). Its tree is pinned by its own golden hash, as is the
FM1-only tree, and it may differ from the FM1 tree only in the runtime header,
export and CMake files. Every runtime case runs against it too, and it checks
the FPX1 handshake.

For ABI 6, `smoke_features` checks the host's feature refusal
(`core/arm/recomp/recomp_image_features.h`) against the image's
`recomp_image_features()`: this host accepts exactly the bits its emitter
produces, and a host lacking any of them refuses the image.

A third export, `abi6gg`, adds the generation code guard (GG1,
`SUYU_RECOMP_AB_GUARD_GEN=1`). It may differ from ABI 6 only in the block units
and the four top-level files, and every mode above runs against it as well,
with the generation word left at "verify always", which must behave exactly as
ABI 5. `smoke_gg_host` then drives the generated module with the real host
manager (`core/arm/recomp/recomp_guard_gen.cpp`):

- the handshake and skip-after-verify protocol;
- writes to a watched code page: guest stores through the FM1 fast path, the
  ABI 5 walk and page-crossing stores, a host write, a writable raw pointer
  (before and after activation) and the JIT fallback, each followed by an
  entry that must abort; and writes elsewhere that must not bump;
- for every hook that must move the generation (map, unmap, protect, alias in
  another table, device map, IC IVAU, whole-cache invalidation, new table, new
  process, rebase): verify, change the code, fire the hook, and require the
  next entry to abort (exit 86);
- activation, decided from what the hooks recorded since the table was
  created, that must pin "verify always" (an alias existing before the first
  run, writable or partly unmapped code, a table created before registration,
  an overflowing log), and controls that must not bump;
- `race-bump`, three verifying threads against a thread bumping 200,000 times,
  and `race-mutate-N`, a thread that changes the code, bumps and publishes while
  another enters the block, at 50 different delays.

Pass `-DSMOKE_SANITIZE=thread` to build `smoke_gg_host` under ThreadSanitizer.

A second executable links two generated static libraries using their emitted
CMake projects and one shared runtime. It checks that lookup indexes, load bases,
guard negotiation, and execution remain independent, including after rebasing
one module. Missing per-module symbol aliases therefore fail at the actual link.

The block-count assertions also exercise the host accounting formula: the first
block is counted on entry; a budget decrement to zero parks the next PC without
executing another block. This suite checks generated-code behavior, not full
emulator integration or title compatibility.

## Image binding

`smoke_binding_unit` checks `RecompGaps::MatchImage`, which suyu-cmd uses to
give each loaded module its recompiled image: by build ID, so a Hybrid
registration that leaves a module out (here main) still binds every other
image to its own module, the omitted module gets none, and an image whose
module is not loaded stays unbound. Registrations without build IDs bind by
NSO slot name instead. Images were once bound by position, which this fails.

## Literal loads

`smoke_literal_unit` translates LDR and LDRSW (literal) for GPRs and Q
registers and checks that each reads from `g_module_base` plus the module
offset, like ADR/ADRP. The suite above runs at base 0, where a missing base
goes unnoticed; TOTK main faulted on exactly that.

## Coverage loop (recomp_gaps.json)

`smoke_gaps_unit` checks `core/arm/recomp/recomp_gaps.cpp` on its own: the
schema (exact round trip, a wrong or newer schema refused, unknown keys
ignored), merging and deduplication across runs, the per-module offset bound,
exact build-ID matching (case and zero padding aside; an all-zero ID matches
nothing), that no directory survives into a file, and how the session recorder
classifies a miss: an offset in a module with an image, a hit on a module
without one, or unattributed.

The loop itself uses a synthetic module (`loop_code.h`) whose first function
calls a second through a pointer loaded from memory. Block discovery indexes
every nonzero word, so the only code it leaves out is a zero word straight
after an unconditional branch or return; the callee starts on one. The first
export has no block there, and `smoke_loop record` sees the call miss exactly
as ArmRecomp would, records it with the real recorder and merges it into a
gaps file. Exporting again with an empty gaps file, or with gaps recorded
against another build ID, must reproduce the first tree byte for byte; with
the recorded file the tree must change, and `smoke_loop static` then finds a
block at the callee and the same call stays in recompiled code (stopping on
the callee's own `udf #0`, reported as an unimplemented opcode, not a miss).
