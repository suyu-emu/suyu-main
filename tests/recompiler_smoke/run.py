#!/usr/bin/env python3
"""Compile and execute synthetic ABI 5, ABI 6 and ABI 6 + GG1 tests, and the coverage loop;
requires only Python, CMake, C/C++."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

# SHA-256 of the whole generated smoke tree (every file, by relative path) with
# the fast-path emit option off. That output must stay byte-identical to ABI 5;
# update this only for a deliberate ABI 5 emitter change, never for ABI 6 work.
ABI5_GOLDEN = "63c1ed5bafc2c68e2b6a53282c304cf1edc2c43d899a73371b49f41de4d1597a"
# The same for ABI 6 with FM1 alone (GG1 and FPX1 both off), which neither
# feature may move, and with FM1 and FPX1 (exact native FP) on, tracked so any
# change is deliberate. Recomputed against this integration tree, since the FP
# exactness fixes touch the shared emitter and so this text too; GG1's own
# switch-off contract (DESIGN.md sec 0) guarantees it does not move these.
FM1_GOLDEN = "371f92332302eb6e3b2d62dc94c6720b1812aeb9af515255c44cff892bca3fc7"
FPX_GOLDEN = "e59b462663b60fd4dd9ece6474e02d28f13084314595091569827d3be0c414d8"

# ABI 6 changes only these files; the block sources must be identical.
ABI6_CHANGED = {"CMakeLists.txt", "recomp_export.c", "recomp_runtime.c", "recomp_runtime.h"}
# FPX1 on top of FM1 changes only these: the smoke code has no FP instructions.
# The runtime C gains the out-of-line exact FP bodies (recomp_fpe_*).
FPX_CHANGED = {"CMakeLists.txt", "recomp_export.c", "recomp_runtime.c", "recomp_runtime.h"}
# GG1 (the generation code guard) changes the block units as well, never the
# dispatch table (src/recompiled_<module>.c) or the file set.
GG1_CHANGED = ABI6_CHANGED | {"recompiled_smoke_0.c", "recompiled_second_0.c"}

# name -> SUYU_RECOMP_AB_* switches. abi6gg and ggfpx are GG1's on-top-of-FM1
# and GG1+FPX1-together combinations; neither is golden-hashed (GG1 is not
# switch-off text, so DESIGN.md checks it structurally via check_guard_gen).
VARIANTS = (("abi5", {}),
            ("abi6", {"SUYU_RECOMP_AB_FASTMEM": "1"}),
            ("abi6gg", {"SUYU_RECOMP_AB_FASTMEM": "1", "SUYU_RECOMP_AB_GUARD_GEN": "1"}),
            ("fpx", {"SUYU_RECOMP_AB_FASTMEM": "1", "SUYU_RECOMP_AB_FPX": "1"}),
            ("ggfpx", {"SUYU_RECOMP_AB_FASTMEM": "1", "SUYU_RECOMP_AB_GUARD_GEN": "1",
                       "SUYU_RECOMP_AB_FPX": "1"}))

# smoke_gg_host modes: exit 0 (protocol, activation, controls, races) or 86
# (a hook that must end skipping, followed by a changed block).
GG_HOST_PASS = ("skip", "mutate-no-bump", "log-alias", "log-trim", "unstable", "unmapped-code",
                "untracked", "overflow", "disabled", "ctl-ivau-elsewhere", "ctl-map-elsewhere",
                "pointer-prelog", "ctl-writes-elsewhere", "race-bump")
GG_HOST_ABORT = ("hook-map", "hook-unmap", "hook-protect-rx", "hook-protect-rw", "hook-alias",
                 "hook-device", "hook-ivau", "hook-ivau-all", "hook-new-table",
                 "hook-new-process", "hook-rebase", "guest-store", "guest-store-slow",
                 "guest-store-cross", "host-write", "pointer-exposed", "jit-fallback")


def call(args, expected=0, timeout=120, env=None):
    result = subprocess.run([str(arg) for arg in args], check=False, timeout=timeout, env=env)
    if result.returncode != expected:
        raise RuntimeError(f"exit {result.returncode}, expected {expected}: {args}")


def executable(build, name):
    for candidate in (build / name, build / (name + ".exe"), build / "Release" / (name + ".exe")):
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"missing executable {name} under {build}")


def tree_hash(root):
    digest = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        data = path.read_bytes()
        digest.update(path.relative_to(root).as_posix().encode() + b"\0" +
                      str(len(data)).encode() + b"\0" + data)
    return digest.hexdigest()


def check_differences(base, other, changed, what):
    files_base = sorted(p.relative_to(base) for p in base.rglob("*") if p.is_file())
    files_other = sorted(p.relative_to(other) for p in other.rglob("*") if p.is_file())
    if files_base != files_other:
        raise RuntimeError(f"{what} export produced a different file set")
    for rel in files_base:
        same = (base / rel).read_bytes() == (other / rel).read_bytes()
        if same == (rel.name in changed):
            raise RuntimeError(f"unexpected {what} difference state for {rel.as_posix()}")


def check_guard_gen(abi6, gg):
    check_differences(abi6, gg, GG1_CHANGED, "GG1")
    for module in (gg, gg / "second"):
        header = (module / "recomp_runtime.h").read_text()
        export = (module / "recomp_export.c").read_text()
        if "#define RECOMP_IMAGE_ABI 6\n" not in header or "RECOMP_FEATURE_GUARD_GEN1" not in header:
            raise RuntimeError(f"{module} is not ABI 6 with GG1")
        if "recomp_image_guard_gen_v1" not in export or "recomp_image_fastmem_v1" not in export:
            raise RuntimeError(f"{module}: GG1 handshake presence is wrong")
        units = list((module / "src").glob("*_0.c"))
        if not units or "recomp_code_guard(" in units[0].read_text():
            raise RuntimeError(f"{module}: blocks still call the per-entry guard directly")


def check_outputs(abi5, abi6, fpx):
    for root, golden, what in ((abi5, ABI5_GOLDEN, "ABI 5"), (abi6, FM1_GOLDEN, "ABI 6 FM1"),
                               (fpx, FPX_GOLDEN, "ABI 6 FM1+FPX1")):
        actual = tree_hash(root)
        if actual != golden:
            raise RuntimeError(f"{what} output changed: {actual} != {golden}")
    check_differences(abi5, abi6, ABI6_CHANGED, "ABI 6")
    check_differences(abi6, fpx, FPX_CHANGED, "FPX1")
    for root, abi, fastmem, fpx_on in ((abi5, 5, False, False), (abi6, 6, True, False),
                                       (fpx, 6, True, True)):
        for module in (root, root / "second"):
            header = (module / "recomp_runtime.h").read_text()
            export = (module / "recomp_export.c").read_text()
            if f"#define RECOMP_IMAGE_ABI {abi}\n" not in header:
                raise RuntimeError(f"{module} is not ABI {abi}")
            if ("recomp_image_fastmem_v1" in export) != fastmem:
                raise RuntimeError(f"{module}: fastmem handshake presence is wrong")
            if ("recomp_image_fpx_v1" in export) != fpx_on:
                raise RuntimeError(f"{module}: FPX1 handshake presence is wrong")


def check_ggfpx(abi6gg, ggfpx):
    # FM1+GG1+FPX1 together: both features negotiated on top of the same FM1
    # base, neither one disabling the other. Structural only, like GG1 alone.
    # GG1 is already on in both trees, so only FPX1's own delta should show,
    # exactly as it does going from plain FM1 to FM1+FPX1 (FPX_CHANGED).
    check_differences(abi6gg, ggfpx, FPX_CHANGED, "GG1+FPX1")
    for module in (ggfpx, ggfpx / "second"):
        header = (module / "recomp_runtime.h").read_text()
        export = (module / "recomp_export.c").read_text()
        if ("RECOMP_FEATURE_GUARD_GEN1" not in header) or ("RECOMP_FEATURE_FPX1" not in header):
            raise RuntimeError(f"{module} is missing GG1 or FPX1 in the runtime header")
        if "recomp_image_guard_gen_v1" not in export or "recomp_image_fpx_v1" not in export:
            raise RuntimeError(f"{module}: GG1+FPX1 handshake presence is wrong")


def check_coverage_loop(root, exporter, build):
    """recomp_gaps.json end to end: record a miss, re-export with it, run statically."""
    env = dict(os.environ)
    for key in ("SUYU_RECOMP_AB_FASTMEM", "SUYU_RECOMP_AB_GUARD_GEN", "SUYU_RECOMP_AB_FPX"):
        env.pop(key, None)
    gaps = root / "loop-gaps.json"

    def export(name, gaps_file=None):
        out = root / name
        call([exporter, "--loop", out] + ([gaps_file] if gaps_file else []), env=env)
        return out

    first = export("loop-1")
    call([executable(build("run-loop-1", f"-DLOOP_DIR={first.as_posix()}"), "smoke_loop"), "record", gaps])
    # No usable gaps must leave the output byte-identical: an empty file, and
    # gaps recorded against another build of the module.
    empty = root / "loop-empty.json"
    empty.write_text('{"schema": "suyu-recomp-gaps", "schema_version": 1, "runs": 3, '
                     '"clean_runs": 3}\n')
    other = root / "loop-other.json"
    other.write_text(gaps.read_text().replace("5eedc0de", "5eedc0df"))
    for name, gaps_file in (("loop-empty", empty), ("loop-other", other)):
        if tree_hash(export(name, gaps_file)) != tree_hash(first):
            raise RuntimeError(f"{name}: output changed without a matching gap")
    second = export("loop-2", gaps)
    if tree_hash(second) == tree_hash(first):
        raise RuntimeError("the recorded gap did not reach block discovery")
    call([executable(build("run-loop-2", f"-DLOOP_DIR={second.as_posix()}"), "smoke_loop"), "static", gaps])
    print("Coverage loop checks passed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--cc", help="explicit C compiler path, if needed")
    parser.add_argument("--cxx", help="explicit C++ compiler path, if needed")
    args = parser.parse_args()
    source = args.source.resolve()
    test_source = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="suyu-recompiler-smoke-") as tmp:
        root = Path(tmp)
        compiler_options = []
        for key, value in (("C", args.cc), ("CXX", args.cxx)):
            if value:
                compiler_options.append(f"-DCMAKE_{key}_COMPILER={Path(value).as_posix()}")

        def build(name, *options):
            directory = root / name
            call([args.cmake, "-S", test_source, "-B", directory,
                  f"-DSUYU_SOURCE={source.as_posix()}", "-DCMAKE_BUILD_TYPE=Release", *compiler_options,
                  *options])
            call([args.cmake, "--build", directory, "--config", "Release", "--parallel", "2"])
            return directory

        export_build = build("export")
        exporter = executable(export_build, "smoke_export")
        call([executable(export_build, "smoke_gaps_unit")])
        call([executable(export_build, "smoke_binding_unit")])
        call([executable(export_build, "smoke_module_name_unit")])
        call([executable(export_build, "smoke_import_resolve_unit")])
        call([executable(export_build, "smoke_literal_unit")])
        call([executable(export_build, "smoke_shl_unit")])
        check_coverage_loop(root, exporter, build)
        generated = {}
        for name, switches in VARIANTS:
            generated[name] = root / f"generated-{name}"
            generated[name].mkdir()
            env = dict(os.environ)
            env.pop("SUYU_RECOMP_AB_FASTMEM", None)
            env.pop("SUYU_RECOMP_AB_GUARD_GEN", None)
            env.pop("SUYU_RECOMP_AB_FPX", None)
            env.update(switches)
            call([exporter, generated[name]], env=env)
        check_outputs(generated["abi5"], generated["abi6"], generated["fpx"])
        check_guard_gen(generated["abi6"], generated["abi6gg"])
        check_ggfpx(generated["abi6gg"], generated["ggfpx"])

        for name, switches in VARIANTS:
            guard_gen = "SUYU_RECOMP_AB_GUARD_GEN" in switches
            run = build(f"run-{name}", f"-DGENERATED_DIR={generated[name].as_posix()}")
            runner = executable(run, "smoke_run")
            hybrid_env = dict(os.environ, SUYU_RECOMP_STRICT="0")
            for mode in ("hybrid-mutated-entry", "hybrid-unmapped-entry",
                         "hybrid-middle-entry", "hybrid-direct-chain"):
                call([runner, mode], timeout=15, env=hybrid_env)
            for mode in ("slice", "ordinary-page", "cross-page", "special-page",
                         "mem-ordinary", "mem-unmapped", "mem-special", "mem-cross",
                         "mem-unaligned", "mem-limit", "mem-protected"):
                call([runner, mode], timeout=15)
            for mode in ("mutated", "mutated-entry", "unmapped-zero",
                         "cross-page-mutated", "cross-page-unmapped"):
                call([runner, mode], expected=86, timeout=15,
                     env=dict(os.environ, SUYU_RECOMP_STRICT="1"))
            call([executable(run, "smoke_static")], timeout=15)
            if name != "abi5":
                call([executable(run, "smoke_features")], timeout=15)
            if guard_gen:
                host = executable(run, "smoke_gg_host")
                for mode in GG_HOST_PASS:
                    call([host, mode], timeout=120)
                for mode in GG_HOST_ABORT:
                    call([host, mode], expected=86, timeout=15,
                         env=dict(os.environ, SUYU_RECOMP_STRICT="1"))
                # The cross-thread catch, at a spread of mutation times.
                for delay in range(0, 2000, 40):
                    call([host, f"race-mutate-{delay}"], expected=86, timeout=15,
                         env=dict(os.environ, SUYU_RECOMP_STRICT="1"))
            print(f"All synthetic {name.upper()} smoke checks passed.")


if __name__ == "__main__":
    main()
