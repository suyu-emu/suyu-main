# Provenance

This is a continuation of suyu, which was archived upstream. It keeps the suyu
name and numbering rather than taking a new identity, so the lineage stays
legible.

## Base

| | |
|---|---|
| upstream | `suyu-emu/suyu-v0.0.4` (archived) |
| commit | `d1d09321d7ab84252291e05b3efbc8a8dfa57481` |
| tag | `v0.04-latest` |
| license | GPL-3.0-or-later (with GPL-2.0-or-later files inherited from yuzu) |

Upstream is inconsistent about its own version: the repository is named
`suyu-v0.0.4`, the tag reads `v0.04-latest`, and `BUILD_FULLNAME` was hardcoded
to `v0.04`. This fork normalises to the three-part form, so `0.0.4` becomes
**`0.0.5`** — a direct continuation, not a new numbering scheme. `v0.04` read
literally would mean 0.4, which was evidently not the intent.

`BUILD_FULLNAME` carries `(mk8-recomp)` so a built binary always says which
0.0.5 it is. The archived repository could in principle be picked up by someone
else; the suffix removes the ambiguity without polluting the version number.

## Significant changes

Required by GPL-3 §5(a), and useful regardless. Each is one commit, message
first line quoted:

**Defects fixed in suyu itself** — these are not specific to the recompiler and
affect ordinary emulation:

- `fix: installed updates and DLC in NAND were never indexed` — `GetFileAtID`
  never looked for `.cnmt.nca` inside a two-digit directory, so *no* installed
  update or DLC was ever visible, for any title. A second defect behind it let
  an older update overwrite a newer one by directory scan order.
- `fix: service handler registration dropped most commands` — a typed array was
  walked through a base pointer with a different layout. IpcController
  registered 2 of 6 handlers; `QueryPointerBufferSize` was among the lost, and
  every title stalled in early service setup.
- `fix: RomFS registration was silently dropped` — `emplace` where
  `insert_or_assign` was meant, so re-registration kept the stale entry and the
  title panicked on boot.
- `fix: AOT image dispatch resolved every PC to the wrong module` — double base
  subtraction, plus a four-entry module table for a nine-module title.
- `fix: export the update ExeFS, not the base` — the exporter read base code
  while the emulator ran the update's.

**Additions:**

- `recomp: AArch64 translation, host bridge and execution coverage`
- `feat: suyu-cmd --probe-isa-list reports each title's CPU architecture`
- `diag: identify the NPDM by content hash, and name the update's provider slot`

## Not carried here

One fix lives in a nested submodule and is not part of this repository:
`externals/dynarmic/externals/mcl/include/boost/variant.hpp` has a hardcoded
home directory in its include path. Retiring that patch needs a fork of
`suyu-emu/dynarmic` as well.

## Rebuilding the change set

```
git diff d1d09321d7ab84252291e05b3efbc8a8dfa57481..mk8-recomp
```

That is the authoritative patch set. It replaces the hand-maintained patch files
this work previously carried, which had to be reconstructed by reverse-applying
edits and were a recurring source of error.
