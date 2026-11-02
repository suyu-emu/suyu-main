#!/usr/bin/env python3
"""Build and run the FM1 differential test (ABI 6 fast path vs. slow path vs. ABI 5)."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def call(args, timeout=1800):
    result = subprocess.run([str(arg) for arg in args], check=False, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"exit {result.returncode}: {args}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator", help="CMake generator, e.g. Ninja")
    parser.add_argument("--cc", help="explicit C compiler path, if needed")
    parser.add_argument("--cxx", help="explicit C++ compiler path, if needed")
    parser.add_argument("--sanitize", action="store_true", help="ASan + UBSan (GCC/Clang)")
    parser.add_argument("--operations", type=int, default=20_000_000)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    test_source = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="suyu-fmdiff-") as tmp:
        build = Path(tmp) / "build"
        options = [f"-DSUYU_SOURCE={args.source.resolve()}", "-DCMAKE_BUILD_TYPE=Release",
                   f"-DFMDIFF_SANITIZE={'ON' if args.sanitize else 'OFF'}"]
        if args.generator:
            options += ["-G", args.generator]
        for key, value in (("C", args.cc), ("CXX", args.cxx)):
            if value:
                options.append(f"-DCMAKE_{key}_COMPILER={value}")
        call([args.cmake, "-S", test_source, "-B", build, *options])
        call([args.cmake, "--build", build, "--config", "Release", "--parallel", "2"])
        for exe in (build / "fmdiff", build / "fmdiff.exe", build / "Release" / "fmdiff.exe"):
            if exe.is_file():
                call([exe, args.operations, args.seed])
                return
        raise RuntimeError("fmdiff was not built")


if __name__ == "__main__":
    main()
