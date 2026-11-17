#!/usr/bin/env python3
"""Verify the production GPU clock getter, including a pre-fix failing control."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/export_regression'))
from compile_object import cache_value, msvc_environment, resolve_build


def method(source):
    start = source.index('[[nodiscard]] u64 GetTicks() const {')
    brace = source.index('{', start)
    depth = 1
    position = brace + 1
    while depth:
        depth += (source[position] == '{') - (source[position] == '}')
        position += 1
    return source[start:position]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True)
    args = parser.parse_args()
    build = resolve_build(args.build)
    env = msvc_environment(build, None)
    target = 'src/suyu/CMakeFiles/suyu.dir/configuration/shared_widget.cpp.obj'
    command = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True).splitlines()[-1]
    flags = command[:command.index(' /showIncludes')]
    block = re.search(r'^build bin[\\/]suyu\.exe:.*?(?=\n\n)', (build / 'build.ninja').read_text(), re.M | re.S)[0]
    libraries = re.search(r'^  LINK_LIBRARIES = (.*)$', block, re.M)[1]
    current = method((ROOT / 'src/video_core/gpu.cpp').read_text())
    # Remove precisely the new gate; the rest of the production getter stays identical.
    old = current.replace('Settings::values.use_fast_gpu_time.GetValue() &&', '')
    if old == current:
        raise RuntimeError('Production getter is missing its Fast GPU Time gate')
    with tempfile.TemporaryDirectory(prefix='suyu-gpu-clock-') as temporary:
        folder = Path(temporary)
        for label, body in (('before', old), ('after', current)):
            (folder / 'gpu_ticks_method.inc').write_text(body)
            obj = folder / (label + '.obj')
            compile_command = flags + f' /I"{folder}" /Fo"{obj}" /Fd"{folder}/fixture.pdb" /c "{ROOT / "tests/graphics_settings/gpu_ticks_unit.cpp"}"'
            subprocess.run(compile_command, cwd=build, env=env, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            response = folder / 'link.rsp'
            response.write_text(f'"{obj}"\n' + libraries)
            exe = folder / (label + '.exe')
            linker = Path(env['VCTOOLSINSTALLDIR']) / 'bin/Hostx64/x64/link.exe'
            subprocess.run([str(linker), '/nologo', f'@{response}', '/SUBSYSTEM:CONSOLE', f'/OUT:{exe}'], cwd=build, env=env, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            qt = Path(cache_value(build, 'Qt6Core_DIR')).parents[2]
            env['PATH'] = str(qt / 'bin') + os.pathsep + env.get('PATH', '')
            result = subprocess.run([str(exe)], env=env, capture_output=True, text=True)
            if label == 'before':
                if result.returncode == 0 or 'Fast GPU Time off must leave ticks unchanged' not in result.stderr:
                    raise RuntimeError('Pre-fix negative control did not fail for the expected reason')
                print('PASS: pre-fix getter fails the off-switch control')
            elif result.returncode != 0:
                raise RuntimeError(result.stdout + result.stderr)
            else:
                print(result.stdout.strip())
    return 0


if __name__ == '__main__':
    sys.exit(main())
