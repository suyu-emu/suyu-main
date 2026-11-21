#!/usr/bin/env python3
"""Launch a relocated suyu.app with isolated settings; keep evidence, not games."""
import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import tempfile
from pathlib import Path

from verify_bundle import inside, system_path


def parse_check(stdout):
    matches = [line[len('SUYU_STARTUP_CHECK='):]
               for line in stdout.splitlines() if line.startswith('SUYU_STARTUP_CHECK=')]
    if len(matches) != 1:
        raise ValueError('the real frontend did not emit exactly one startup result')
    result = json.loads(matches[0])
    expected = ('success', 'window_exposed', 'surface_present_modes', 'screenshot_saved')
    if any(result.get(field) is not True for field in expected):
        raise ValueError('the frontend window/Vulkan/screenshot check failed')
    if result.get('qt_platform') != 'cocoa' or not result.get('vulkan_devices'):
        raise ValueError('a native Cocoa window and Vulkan device are required')
    if result.get('games_tested') is not False:
        raise ValueError('this probe must run without games')
    return result


def loaded_images(stderr):
    return [match.group(1) for line in stderr.splitlines()
            if line.startswith('dyld[') and (match := re.search(r'\s(/.+)$', line))]


def clean_environment(root, evidence):
    # Preserve the existing login identity, including HOME, unchanged. suyu's
    # data/config/cache and Qt settings are redirected to private test paths.
    keep = ('HOME', 'USER', 'LOGNAME', 'TMPDIR', '__CF_USER_TEXT_ENCODING')
    env = {name: os.environ[name] for name in keep if name in os.environ}
    env.update(PATH='/usr/bin:/bin:/usr/sbin:/sbin', LANG='en_US.UTF-8',
               QT_QPA_PLATFORM='cocoa', DYLD_PRINT_LIBRARIES='1',
               XDG_DATA_HOME=str(root / 'data'), XDG_CONFIG_HOME=str(root / 'config'),
               XDG_CACHE_HOME=str(root / 'cache'),
               SUYU_STARTUP_TEST_CONFIG=str(root / 'config'),
               SUYU_STARTUP_SCREENSHOT=str(evidence / 'startup-window.png'))
    return env


def verify(app, evidence):
    app = app.resolve()
    with tempfile.TemporaryDirectory(prefix='suyu startup ') as temp:
        # macOS can name the same temporary directory through /var or
        # /private/var. Keep relocation paths in their resolved form.
        root = Path(temp).resolve()
        copied = root / 'Relocated application/suyu.app'
        copied.parent.mkdir()
        subprocess.run(['/usr/bin/ditto', str(app), str(copied)], check=True)
        subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(copied)],
                       check=True, capture_output=True)
        env = clean_environment(root, evidence)
        for folder in ('data', 'config', 'cache'):
            (root / folder).mkdir()
        exe = copied / 'Contents/MacOS/suyu'
        stdout_path, stderr_path = evidence / 'startup-stdout.txt', evidence / 'startup-stderr.txt'
        with stdout_path.open('w') as stdout, stderr_path.open('w') as stderr:
            process = subprocess.Popen([str(exe), '--verify-startup'], cwd=copied.parent,
                                       env=env, stdout=stdout, stderr=stderr, start_new_session=True)
            try:
                code = process.wait(timeout=45)
            except subprocess.TimeoutExpired:
                import signal
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise ValueError('startup did not finish within 45 seconds; check stderr')
        if code != 0:
            raise ValueError('the real frontend exited with status ' + str(code))
        check = parse_check(stdout_path.read_text(errors='replace'))
        loaded = loaded_images(stderr_path.read_text(errors='replace'))
        driver = copied / 'Contents/Frameworks/libMoltenVK.dylib'
        if not any(Path(name).resolve() == driver.resolve() for name in loaded):
            raise ValueError('dyld evidence does not confirm the bundled MoltenVK was loaded')
        external = [name for name in loaded if not system_path(name) and not inside(Path(name), copied)]
        if external:
            raise ValueError('startup loaded libraries outside the app/system: ' + ', '.join(external))
        png = evidence / 'startup-window.png'
        if not png.is_file() or png.read_bytes()[:8] != b'\x89PNG\r\n\x1a\n':
            raise ValueError('the frontend did not save a PNG of its window')
        # Resolve both sides, as dyld may use a different symlink spelling.
        # Avoid embedding a removed temporary path in the useful image list.
        loaded = [str(Path(p).resolve().relative_to(copied.resolve())) if inside(Path(p), copied) else p
                  for p in loaded]
        return {'status': 'passed', 'exit_code': code, 'frontend': check,
                'bundled_moltenvk_loaded': True, 'loaded_images': loaded,
                'screenshot_sha256': hashlib.sha256(png.read_bytes()).hexdigest(),
                'scope': 'Qt window, Vulkan instance/device and macOS surface enumeration; no gameplay',
                'gatekeeper_tested': False, 'target_mac_tested': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    png = evidence / 'startup-window.png'
    png.unlink(missing_ok=True)  # Never accept a screenshot from a previous run.
    report = {'host': platform.platform(), 'architecture': platform.machine(),
              'app': str(args.app.resolve())}
    try:
        if platform.system() != 'Darwin' or platform.machine() != 'arm64':
            raise ValueError('not run: an Apple Silicon macOS host with a GUI session is required')
        report.update(verify(args.app, evidence))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        report.update(status='not_run' if platform.system() != 'Darwin' else 'failed',
                      error=str(error))
    (evidence / 'startup-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(report['status'])
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
