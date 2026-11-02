"""Link both consumers against the actual release kit, outside shipped content."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def run(command, *, env=None):
    result = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors='replace', timeout=60)
    print(result.stdout, end='', flush=True)
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}): {command}')
    return result.stdout


def check_manifest(kit):
    """The kit manifest must name its policy and producer and hash every other file."""
    manifest = json.loads((kit / 'manifest.json').read_text(encoding='utf-8'))
    policy = json.loads((Path(__file__).resolve().parents[2] / 'tools/package_policy/policy.json')
                        .read_text(encoding='utf-8'))
    if manifest.get('policy_version') != policy['policy_version']:
        raise AssertionError('Kit manifest policy_version does not match policy.json')
    if not str(manifest.get('producer_source_revision', '')).strip():
        raise AssertionError('Kit manifest has no producer_source_revision')
    files = manifest.get('files')
    actual = {p.relative_to(kit).as_posix() for p in kit.rglob('*') if p.is_file()}
    if not isinstance(files, dict) or actual != set(files) | {'manifest.json'}:
        raise AssertionError('Kit files differ from the manifest file table')
    for relative, digest in files.items():
        if hashlib.sha256((kit / relative).read_bytes()).hexdigest() != digest:
            raise AssertionError('Kit file hash mismatch: ' + relative)


def check_release_kit(kit, output):
    kit, output = kit.resolve(), output.resolve()
    if kit == output or kit in output.parents or output in kit.parents:
        raise ValueError('Test output must be separate from the production build kit')
    for tool in ('cmake', 'ninja', 'cl'):
        if not shutil.which(tool):
            raise RuntimeError(f'Missing {tool}; run in an x64 Visual Studio developer environment')
    for name in ('manifest.json', 'revision.txt', 'CMakeLists.txt', 'strict.cmake', 'hybrid.cmake'):
        if not (kit / name).is_file():
            raise FileNotFoundError(kit / name)
    check_manifest(kit)
    revision = (kit / 'revision.txt').read_text(encoding='utf-8').strip()
    export = output / 'synthetic-export'
    module = export / 'main'
    module.mkdir(parents=True, exist_ok=True)
    for header in ('recomp_abi_v4.h', 'recomp_guard_v2.h', 'recomp_fastmem_v1.h',
                   'recomp_features_v1.h', 'recomp_guard_gen_v1.h', 'recomp_fpx_v1.h'):
        (export / header).write_text('/* Link-only release-kit fixture. */\n', encoding='utf-8')
    (export / 'recomp_modules.cmake').write_text('set(SUYU_RECOMP_MODULES main)\n', encoding='utf-8')
    (module / 'recomp_runtime.h').write_text('#define RECOMP_IMAGE_ABI 6\n/* tpidrro_el0 */\n', encoding='utf-8')
    (module / 'module.c').write_text('int release_kit_fixture(void) { return 0; }\n', encoding='utf-8')
    (module / 'CMakeLists.txt').write_text(
        'add_library(recomp_static_main STATIC module.c)\n'
        'target_compile_options(recomp_static_main PRIVATE /O2)\n', encoding='utf-8')
    probe = Path(__file__).resolve().parents[2] / 'tools/export_build_kit/registry_probe.c'
    shutil.copyfile(probe, export / 'recomp_registration.c')
    env = os.environ.copy()
    env['PATH'] = str(kit.parent) + os.pathsep + env.get('PATH', '')
    for mode in ('strict', 'hybrid'):
        consumer = output / mode
        run(['cmake', '-S', str(kit), '-B', str(consumer), '-G', 'Ninja',
             '-DSUYU_CMD_RECOMP_DIR=' + str(export),
             '-DSUYU_EXPORT_BUILD_KIT_REVISION=' + revision,
             '-DSUYU_RECOMP_HYBRID=' + ('ON' if mode == 'hybrid' else 'OFF')])
        cache = (consumer / 'CMakeCache.txt').read_text(encoding='utf-8')
        if 'CMAKE_BUILD_TYPE:STRING=Release' not in cache:
            raise AssertionError(f'{mode} consumer did not default to Release')
        run(['cmake', '--build', str(consumer)])
        # The Windows frontend attaches to the parent's console and redirects
        # help there, bypassing these pipes. Its clean exit still proves the CRT,
        # DLL imports and argument parser initialized successfully.
        run([str(consumer / 'bin/suyu-cmd-static.exe'), '--help'], env=env)
        print(f'{mode}: production kit linked and --help exited cleanly', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kit', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    check_release_kit(args.kit, args.output)
