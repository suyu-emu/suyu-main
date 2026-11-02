#!/usr/bin/env python3
"""Build and run the FP differential harness (see README.md)."""
import argparse
import os
import platform
from pathlib import Path
import subprocess
import sys
import tempfile


def call(args, **kwargs):
    result = subprocess.run([str(arg) for arg in args], check=False, **kwargs)
    if result.returncode != 0:
        raise RuntimeError(f"exit {result.returncode}: {args}")
    return result


def low_priority():
    """Child processes run at below-normal priority (idle class on Windows)."""
    if os.name == "nt":
        return {"creationflags": 0x00004000}  # BELOW_NORMAL_PRIORITY_CLASS
    return {"preexec_fn": lambda: os.nice(10)}


def driver_path(build):
    for exe in (build / "fpx_driver", build / "fpx_driver.exe", build / "Release" / "fpx_driver.exe"):
        if exe.is_file():
            return exe
    raise RuntimeError("fpx_driver was not built")


def sharded(driver, args, jobs, log_dir, tag):
    """Runs `driver args --shard i/jobs` in parallel; returns (failed, combined output)."""
    procs = []
    for shard in range(jobs):
        log = open(log_dir / f"{tag}.{shard}.txt", "w")
        procs.append((subprocess.Popen([str(driver), *args, "--shard", f"{shard}/{jobs}"],
                                       stdout=log, stderr=subprocess.STDOUT, **low_priority()), log))
    failed = False
    for proc, log in procs:
        failed |= proc.wait() != 0
        log.close()
    text = "".join((log_dir / f"{tag}.{shard}.txt").read_text() for shard in range(jobs))
    return failed, text


def summarize(text, hits):
    """Adds up the per-shard LEG and TOTAL lines of `driver diff` output."""
    legs, total, bad, lines, hit = {}, 0, 0, [], [0, 0, 0, 0]
    for line in text.splitlines():
        words = line.split()
        if line.startswith("LEG "):
            key = " ".join(words[1:5]).rstrip(":")
            counts = legs.setdefault(key, [0, 0, 0])
            for i, at in enumerate((6, 8, 10)):
                counts[i] += int(words[at])
        elif line.startswith("TOTAL "):
            total += int(words[2])
            bad += int(words[4])
        elif line.startswith("HITS "):
            for i, at in enumerate((2, 3, 5, 6)):
                hit[i] += int(words[at])
        elif line.startswith("FPX hit rate") or line.startswith("  ") or line.startswith("     "):
            continue
        elif "fpx hit" in line:
            if hits:
                lines.append(line)
        elif line.strip():
            lines.append(line)
    for key, (cases, value, fpsr) in sorted(legs.items()):
        lines.append(f"LEG {key}: cases {cases} value-mismatch {value} fpsr-mismatch {fpsr}")
    lines.append(f"TOTAL cases {total} mismatches {bad}")
    for name, kept, ops in (("FPCR 0 (L1)", hit[0], hit[1]), ("FPCR FZ (L1+L3)", hit[2], hit[3])):
        if ops:
            lines.append(f"FPX hit rate {name}, IXC set: {100.0 * kept / ops:.2f}% ({kept} of {ops} lane ops)")
    return "".join(line + "\n" for line in lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator", help="CMake generator, e.g. Ninja")
    parser.add_argument("--cc", help="explicit C compiler path, if needed")
    parser.add_argument("--cxx", help="explicit C++ compiler path, if needed")
    parser.add_argument("--cflags", default="", help="extra flags for the generated code and driver")
    parser.add_argument("--build", type=Path, help="keep the build here instead of a temporary directory")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--cases", type=int, default=20000, help="L1 cases per word per FPCR")
    parser.add_argument("--adv", type=int, default=20000, help="L3 cases per word per FPCR")
    parser.add_argument("--l4-step", type=int, default=4099, help="1 walks all 2^32 inputs")
    parser.add_argument("--legs", default="123", help="any of 1234")
    parser.add_argument("--golden", type=Path, help="L5: hash file written on an AArch64 host")
    parser.add_argument("--write-golden", type=Path, help="AArch64 only: write the hardware hashes")
    parser.add_argument("--controls", action="store_true", help="L6 negative controls")
    parser.add_argument("--hits", action="store_true", help="print the fast-path hit rate per word")
    args = parser.parse_args()
    test_source = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="suyu-fpx-") as tmp:
        build = args.build or Path(tmp) / "build"
        options = [f"-DSUYU_SOURCE={args.source.resolve()}", "-DCMAKE_BUILD_TYPE=Release",
                   f"-DFPX_EXTRA_C_FLAGS={args.cflags}"]
        if args.generator:
            options += ["-G", args.generator]
        for key, value in (("C", args.cc), ("CXX", args.cxx)):
            if value:
                options.append(f"-DCMAKE_{key}_COMPILER={value}")
        call([args.cmake, "-S", test_source, "-B", build, *options])
        call([args.cmake, "--build", build, "--config", "Release", "--parallel", str(args.jobs)])
        driver = driver_path(build)
        logs = Path(tmp)
        common = ["--cases", str(args.cases), "--adv", str(args.adv), "--l4-step", str(args.l4_step)]
        status = 0
        if args.write_golden:
            failed, text = sharded(driver, ["hash", "hw", "--legs", "124", *common], args.jobs, logs, "gold")
            args.write_golden.write_text(text)
            print(f"wrote {len(text.splitlines())} hashes to {args.write_golden}")
            status |= failed
        failed, text = sharded(driver, ["diff", "--legs", args.legs, *common], args.jobs, logs, "diff")
        print(summarize(text, args.hits), end="")
        status |= failed
        if args.golden:
            failed, text = sharded(driver, ["check", args.golden.resolve(), "--legs", "124", *common],
                                   args.jobs, logs, "check")
            print("".join(line + "\n" for line in text.splitlines()
                          if line.startswith(("CHECK", "MISMATCH"))), end="")
            status |= failed
        if args.controls:
            for name in ("nokeep", "nomid", "nofz", "mxcsr"):
                failed, text = sharded(driver, ["control", name, "--legs", "13", *common], args.jobs,
                                       logs, name)
                print("".join(line + "\n" for line in summarize(text, False).splitlines()
                              if line.startswith("LEG")), end="")
                found = sum(int(line.split()[2]) for line in text.splitlines()
                            if line.startswith("CONTROL"))
                # AArch64 fast paths use the hardware FMA, so there is no
                # midpoint test for the nomid control to take away.
                exempt = name == "nomid" and platform.machine().lower() in ("arm64", "aarch64")
                print(f"CONTROL {name}: {found} mismatches",
                      "(fails as designed)" if found else
                      "(not applicable on AArch64)" if exempt else "(DID NOT FAIL)")
                status |= failed or (not found and not exempt)
            result = subprocess.run([str(driver), "inhibit", "--cases", str(args.cases)], check=False,
                                    capture_output=True, text=True, **low_priority())
            print("".join(line + "\n" for line in result.stdout.splitlines()
                          if line.startswith("INHIBIT")), end="")
            status |= result.returncode != 0
            # The shadow instrumentation: exact results, and its own count of
            # fast-path mismatches must be 0 with fast-path hits recorded.
            shadow_log = logs / "shadow.log"
            env = dict(os.environ, SUYU_RECOMP_FPX_SHADOW_LOG=str(shadow_log))
            result = subprocess.run([str(driver), "shadow", "--legs", "13", *common], check=False,
                                    capture_output=True, text=True, env=env, **low_priority())
            summary = Path(str(shadow_log) + ".sum")
            rows = [line.split() for line in summary.read_text().splitlines()[1:]
                    if line and not line.startswith("fpsr")] if summary.is_file() else []
            kept = sum(int(row[3]) for row in rows)
            shadow_bad = sum(int(row[4]) for row in rows)
            print(f"SHADOW exact-result mismatches {'none' if result.returncode == 0 else 'FOUND'}, "
                  f"fast results kept {kept}, fast-vs-exact mismatches {shadow_bad}")
            status |= result.returncode != 0 or not rows or kept == 0 or shadow_bad != 0
            # L6(d): the same poisoning, repaired by the host's own check first.
            result = subprocess.run([str(driver), "env", "--legs", "13", *common], check=False,
                                    capture_output=True, text=True, **low_priority())
            print("".join(line + "\n" for line in result.stdout.splitlines()
                          if line.startswith(("ENV", "LEG", "TOTAL"))), end="")
            status |= result.returncode != 0
        print("FPX harness:", "FAILED" if status else "passed")
        sys.exit(1 if status else 0)


if __name__ == "__main__":
    main()
