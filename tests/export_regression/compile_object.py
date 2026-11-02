#!/usr/bin/env python3
"""Compile the exporter object from an existing Ninja manifest without reconfiguring CMake."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TARGET = "src/suyu/CMakeFiles/suyu.dir/game_export.cpp.obj"


def resolve_build(value: str | None) -> Path:
    if value:
        build = Path(value).resolve()
    else:
        candidates = [
            path for path in ROOT.glob("build*")
            if (path / "build.ninja").is_file() and (path / "CMakeCache.txt").is_file()
        ]
        if len(candidates) != 1:
            raise SystemExit(
                "Pass --build or set SUYU_EXPORT_TEST_BUILD to one configured Ninja build "
                f"(found {len(candidates)} candidates)."
            )
        build = candidates[0].resolve()
    if not (build / "build.ninja").is_file() or not (build / "CMakeCache.txt").is_file():
        raise SystemExit(f"Expected an existing CMake/Ninja build at {build}")
    return build


def cache_value(build: Path, key: str) -> str | None:
    prefix = key + ":"
    for line in (build / "CMakeCache.txt").read_text(errors="replace").splitlines():
        if line.startswith(prefix):
            return line.partition("=")[2]
    return None


def msvc_environment(build: Path, devcmd_value: str | None) -> dict[str, str]:
    """The MSVC build environment, with every name in upper case.

    Windows looks environment names up case-insensitively, but a Python dict
    does not, and the two sources disagree on case: os.environ reports names
    upper-cased while `set` keeps their own spelling ("Path", "VCToolsInstallDir").
    """
    if os.environ.get("VCToolsInstallDir") and os.environ.get("INCLUDE"):
        return {name.upper(): value for name, value in os.environ.items()}
    devcmd = Path(devcmd_value) if devcmd_value else None
    if devcmd is None:
        compiler = cache_value(build, "CMAKE_CXX_COMPILER")
        if compiler:
            vc_root = next(
                (parent for parent in Path(compiler).parents if parent.name.lower() == "vc"),
                None,
            )
            if vc_root:
                devcmd = vc_root.parent / "Common7/Tools/VsDevCmd.bat"
    if devcmd is None or not devcmd.is_file():
        raise SystemExit(
            "MSVC environment unavailable. Run in a VS developer shell or pass --vsdevcmd "
            "(or set VSDEVCMD)."
        )
    with tempfile.TemporaryDirectory(prefix="suyu-msvc-env-") as temp:
        batch = Path(temp) / "environment.cmd"
        batch.write_text(f'@echo off\ncall "{devcmd}" -arch=amd64 >nul\nset\n')
        output = subprocess.check_output(
            ["cmd", "/d", "/c", str(batch)], text=True, errors="replace"
        )
    return {
        name.upper(): value
        for name, value in (line.split("=", 1) for line in output.splitlines() if "=" in line)
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=os.environ.get("SUYU_EXPORT_TEST_BUILD"))
    parser.add_argument("--vsdevcmd", default=os.environ.get("VSDEVCMD"))
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("This verifier requires a configured Windows MSVC/Ninja build.")
    build = resolve_build(args.build)
    env = msvc_environment(build, args.vsdevcmd)
    commands = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", TARGET], text=True
    )
    if not commands.splitlines():
        raise SystemExit(f"No compile command found for {TARGET} in {build}")
    result = subprocess.run(
        commands.splitlines()[-1], cwd=build, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
    )
    lines = [line for line in result.stdout.splitlines() if "Note: including file:" not in line]
    print("\n".join(lines[-80:]))
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
