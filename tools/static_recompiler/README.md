# suyu static recompiler CLI

This directory contains the low-level command-line frontend for suyu's experimental
AArch64-to-C recompiler. It can inspect a plaintext NSO0 module, determine its architecture from a
validated `main.npdm`, and emit a portable CMake project from either the complete NSO or a raw,
little-endian AArch64 text segment. It shares the executable parser and block/emission code with
the in-app exporter under `src/core/recompiler`.

This is an engineering tool, not a finished one-click game converter. The generated standalone
runtime has stub services. Switch software that uses Horizon services, the GPU, audio, input, or
dynamic modules must run through suyu's hosted `ArmRecomp` backend, which can fall back to
Dynarmic for uncovered code.

See [the static recompiler roadmap](../../docs/StaticRecompiler.md) for the hosted-runtime
architecture and milestones.

## Build

Use a 64-bit C/C++ toolchain:

```sh
cmake -S tools/static_recompiler -B build/static_recompiler -G Ninja
cmake --build build/static_recompiler
```

LZ4 is optional. If CMake can find an `lz4::lz4` package target, the NSO commands also decode
compressed segments. Without it, header inspection and uncompressed NSO decoding still work;
`emit-nso` rejects compressed input with a clear error. For example, a vcpkg install can be exposed
with `-DCMAKE_PREFIX_PATH=<vcpkg-installed-triplet>`.

On Windows, select an x64 MSVC environment or pass x64 Clang explicitly. The checked-in `.exe`
and `.obj` files in this legacy directory are not used by the CMake build; rebuild from source.

## Emit a raw text segment

```sh
build/static_recompiler/suyu_recomp emit-raw \
  --input path/to/text.bin \
  --base 0x1000 \
  --entry 0x1000 \
  --module main \
  --output build/generated-main
```

Run `suyu_recomp --help` for all options. The older positional form remains accepted for
compatibility.

The tool is also available from the full suyu build with `-DSUYU_RECOMPILER_CLI=ON`.

## Emit a plaintext NSO

```sh
build/static_recompiler/suyu_recomp emit-nso \
  --input path/to/main \
  --npdm path/to/main.npdm \
  --module main \
  --output build/generated-main
```

`emit-nso` validates META, ACI0, and ACID structure before trusting NPDM's instruction-set bit. It
refuses AArch32 because the current translator is AArch64-only. `--assume-aarch64` is available for
controlled experiments without an NPDM and prints an explicit warning. The generated standalone
loader requires a 64-bit host, enforces the page-aligned text/rodata/data order, preserves those
virtual addresses, resolves module-relative MOD0 pointers across all three segments, checks bundled
file sizes, verifies every required NSO0 segment hash after decompression, and zeros BSS. The
standalone tool builds the small SHA-256 component from the repository's mbedTLS submodule; no
system crypto package is required.

This output is a translation project, not yet a self-contained replacement for a Switch runtime.
Imports and relocations still require the hosted suyu loader, and the standalone services remain
test stubs.

## Inspect a plaintext NSO

```sh
build/static_recompiler/suyu_recomp inspect-nso \
  --input path/to/main \
  --npdm path/to/main.npdm \
  --json
```

Inspection reports checked file and memory ranges, build ID, segment compression and expected
hashes. NSO0 does not encode whether its instructions are AArch32 or AArch64, so architecture is
reported as unknown unless `--npdm` supplies a validated `main.npdm`. `--assume-aarch64` explicitly
opts into entry-point, block, MOD0, ELF64 dynamic-table, RELA, and bounded dynamic-symbol discovery
for controlled experimentation. Symbol names and relocation indices are checked against the exact
dynstr/dynsym extents in the NSO header. Missing or malformed dynamic metadata is reported as a
nonfatal diagnostic so basic NSO inspection remains available.

Every NSO0 segment whose header requires SHA-256 validation is checked after decompression. Both
human and JSON output distinguish verified hashes from formats that the current build could not
decode.

`inspect-nso` consumes already-decrypted module bytes. It neither accepts nor manages console
keys, NSP/XCI/NCA containers, updates, or mods.

## Package a local deconstructed title on Windows

`package_deconstructed.ps1` creates a reproducible, local-only hosted-runtime package from a
plaintext deconstructed title tree that you supplied. The minimum input is:

```text
<input>/
  exefs/
    main
    main.npdm
  romfs/              # optional extracted RomFS
```

A packed sibling file named `romfs` or `romfs.bin`, or `exefs/romfs.bin`, is also supported when
no extracted `romfs` directory is present. The packager discovers the canonical NSO set (`rtld`,
`main`, `subsdk0` through `subsdk9`, then `sdk`), inspects and emits each module, generates static
registration, and either builds or copies a hosted `suyu-cmd-static` executable. It never launches
the host or any title code.

Use fresh work and output paths. Both must be outside the input tree and, by default, must either
be outside the source checkout or be ignored by Git:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  tools/static_recompiler/package_deconstructed.ps1 `
  -InputRoot "D:\legal-dump\extracted" `
  -WorkRoot "build\local-title\work" `
  -OutputRoot "build\local-title\package" `
  -RecompilerPath "build\static_recompiler\Release\suyu_recomp.exe" `
  -HostBuildRoot "build\suyu-msvc"
```

For a deconstructed system applet such as qlaunch, add the explicit frontend launch tuple:

```powershell
  -LaunchMode Applet -AppletId 3 `
  -AppletType SystemApplet -LaunchType FrontendInitiated
```

`-HostBuildRoot` must name an already configured 64-bit suyu build; the script reconfigures it with
the generated module directory and builds `suyu-cmd-static`. Builds use four parallel jobs by
default; set `-BuildJobs` to choose a different bounded value. For development or synthetic tests,
`-StaticHostPath` can instead copy an already-built matching host. That mode records a warning in
the manifest because the packager cannot prove which generated modules were linked into an
arbitrary supplied executable.

The output contains `suyu-recompiled.exe`, a quoted `launch.cmd`, the input `exefs` and optional
RomFS, `recomp_package.json`, and `SHA256SUMS`. The manifest records the program ID, NPDM identity,
module build IDs and mapped-text identities, launch parameters, tool/source provenance, and host
identity. Runtime registration independently refuses a mismatched live module identity.

This script does not decrypt content, accept keys, or produce a distributable artifact. Generated
code, the title-specialized executable, and copied title content must remain local and must not be
committed.

## Test

```sh
ctest --test-dir build/static_recompiler --output-on-failure
```

The tests validate malformed NSO and NPDM ranges, architecture gating, MOD0 probing, optional LZ4
behavior, JSON inspection, raw emission, complete NSO emission, and deterministic local package
assembly. Both emission tests build and run the generated native project. The package test uses a
synthetic NSO/NPDM, copies a deliberately non-executable fake host, and never launches that host.
The NSO test loads values from separated rodata/data addresses, computes `5 + 7 = 12`, and writes
through the zeroed BSS mapping. It runs with and without save-data initialization and verifies that
no whole-process autosave can replace the freshly loaded module.

Only use executable content that you are legally allowed to analyze. Do not commit console keys,
game dumps, extracted assets, or generated code derived from proprietary titles.
