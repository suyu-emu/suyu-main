#!/usr/bin/env python3
"""Compile real Qt graphics widgets and verify global/per-game INI round trips.

Uses an existing MSVC/Ninja build for compiler flags and dependency libraries.
All fixture objects and config files live in temporary directories.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/export_regression"))
from compile_object import cache_value, msvc_environment, resolve_build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True)
    parser.add_argument("--vsdevcmd")
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("This fixture requires an existing Windows MSVC/Qt Ninja build.")
    build = resolve_build(args.build)
    env = msvc_environment(build, args.vsdevcmd)
    qt = Path(cache_value(build, "Qt6Core_DIR")).parents[2]
    # Preserve production compiler definitions, includes and ABI flags.
    target = "src/suyu/CMakeFiles/suyu.dir/configuration/shared_widget.cpp.obj"
    template = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", target], text=True
    ).splitlines()[-1]
    flags = template[:template.index(" /showIncludes")]
    manifest = (build / "build.ninja").read_text(encoding="utf-8")
    executable = re.search(r"^build bin[\\/]suyu\.exe:.*?(?=\n\n)", manifest, re.M | re.S)[0]
    libraries = re.search(r"^  LINK_LIBRARIES = (.*)$", executable, re.M)[1]
    with tempfile.TemporaryDirectory(prefix="suyu-graphics-fixture-") as temporary:
        directory = Path(temporary)
        moc = directory / "moc_shared_widget.cpp"
        subprocess.run([str(qt / "bin/moc.exe"), str(ROOT / "src/suyu/configuration/shared_widget.h"),
                        "-o", str(moc)], env=env, check=True)
        sources = [ROOT / "tests/graphics_settings/widget_ini_unit.cpp", moc,
                   ROOT / "src/suyu/configuration/shared_widget.cpp",
                   ROOT / "src/suyu/configuration/shared_translation.cpp",
                   ROOT / "src/suyu/configuration/qt_config.cpp", ROOT / "src/suyu/uisettings.cpp"]
        objects = []
        for index, source in enumerate(sources):
            obj = directory / f"fixture_{index}.obj"
            objects.append(obj)
            command = flags + f' /Fo"{obj}" /Fd"{directory}/fixture.pdb" /c "{source}"'
            result = subprocess.run(command, cwd=build, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
            if result.returncode:
                print(result.stdout)
                return result.returncode
        exe = directory / "graphics-fixture.exe"
        response = directory / "link.rsp"
        response.write_text("\n".join(f'"{obj}"' for obj in objects) + "\n" + libraries,
                            encoding="utf-8")
        linker = Path(env["VCTOOLSINSTALLDIR"]) / "bin/Hostx64/x64/link.exe"
        result = subprocess.run([str(linker), "/nologo", f"@{response}", "/SUBSYSTEM:CONSOLE",
                                 f"/OUT:{exe}"], cwd=build, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            print(result.stdout)
            return result.returncode
        env["PATH"] = str(qt / "bin") + os.pathsep + env.get("PATH", "")
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["QT_PLUGIN_PATH"] = str(qt / "plugins")
        return subprocess.run([str(exe)], env=env).returncode


if __name__ == "__main__":
    sys.exit(main())
