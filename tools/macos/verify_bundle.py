#!/usr/bin/env python3
"""Audit a deployed macOS bundle. Actual Mach-O inspection requires macOS."""
import argparse
import json
import platform
import plistlib
import posixpath
import re
import subprocess
from pathlib import Path

MACHO_MAGIC = {
    b'\xfe\xed\xfa\xce', b'\xce\xfa\xed\xfe',
    b'\xfe\xed\xfa\xcf', b'\xcf\xfa\xed\xfe',
    b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca',
    b'\xca\xfe\xba\xbf', b'\xbf\xba\xfe\xca',
}


def system_path(value):
    normalized = posixpath.normpath(value)
    return normalized.startswith('/usr/lib/') or normalized.startswith('/System/Library/')


def inside(path, root):
    return path.resolve().is_relative_to(root.resolve())


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def parse_rpaths(text):
    return re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset \d+\)', text)


def parse_identity(text):
    match = re.search(r'cmd LC_ID_DYLIB\s+cmdsize \d+\s+name (.*?) \(offset \d+\)', text)
    return match.group(1) if match else ''


def parse_dependencies(text):
    return [line.strip().split(' (compatibility version ', 1)[0]
            for line in text.splitlines()[1:] if line.startswith((' ', '\t'))]


def expand_path(value, image, executable_dir):
    for token, base in (('@loader_path', image.parent), ('@executable_path', executable_dir)):
        if value == token:
            return base.resolve()
        if value.startswith(token + '/'):
            return (base / value[len(token) + 1:]).resolve()
    if value.startswith('/'):
        return Path(value).resolve()
    raise ValueError('unsupported or relative load path: ' + value)


def resolve_dependency(value, image, executable_dir, search_paths, bundle):
    if system_path(value):
        return value  # System libraries may live only in Apple's dyld shared cache.
    if value.startswith('@rpath/'):
        candidates = [p / value[len('@rpath/'):] for p in search_paths]
    elif value.startswith(('@loader_path/', '@executable_path/')):
        candidates = [expand_path(value, image, executable_dir)]
    else:
        raise ValueError('non-relocatable dependency: ' + value)
    for candidate in candidates:
        if candidate.is_file() and inside(candidate, bundle):
            return candidate.resolve()
    raise ValueError('dependency missing from bundle: ' + value)


def audit(bundle):
    bundle = bundle.resolve()
    executable_dir = bundle / 'Contents/MacOS'
    required = ['Contents/MacOS/suyu', 'Contents/MacOS/suyu-cmd',
                'Contents/Frameworks/libMoltenVK.dylib',
                'Contents/PlugIns/platforms/libqcocoa.dylib']
    for name in required:
        if not (bundle / name).is_file():
            raise ValueError('required bundled runtime missing: ' + name)
    with (bundle / 'Contents/Info.plist').open('rb') as source:
        info = plistlib.load(source)
    if info.get('CFBundleExecutable') != 'suyu':
        raise ValueError('Info.plist does not select the suyu executable')

    images = {}
    for path in sorted(bundle.rglob('*')):
        if path.is_symlink() and (not path.exists() or not inside(path, bundle)):
            raise ValueError('broken or external bundle symlink: ' + str(path))
        if not path.is_file():
            continue
        with path.open('rb') as stream:
            magic = stream.read(4)
        if magic not in MACHO_MAGIC:
            continue
        image = path.resolve()
        if image in images:
            continue
        architectures = run('/usr/bin/lipo', '-archs', str(image)).strip().split()
        if 'arm64' not in architectures:
            raise ValueError('no arm64 slice: ' + str(path.relative_to(bundle)))
        load_commands = run('/usr/bin/otool', '-arch', 'arm64', '-l', str(image))
        rpaths = parse_rpaths(load_commands)
        search_paths = []
        for entry in rpaths:
            if entry.startswith('/'):
                raise ValueError('absolute runtime search path: ' + entry + ' in ' + str(path))
            expanded = expand_path(entry, image, executable_dir)
            if not inside(expanded, bundle):
                raise ValueError('external runtime search path: ' + entry + ' in ' + str(path))
            search_paths.append(expanded)
        dependencies = parse_dependencies(
            run('/usr/bin/otool', '-arch', 'arm64', '-L', str(image)))
        identity = parse_identity(load_commands)
        if identity.startswith('/'):
            raise ValueError('absolute dynamic-library identity: ' + identity)
        images[image] = {'path': str(image.relative_to(bundle)),
                         'architectures': architectures, 'rpaths': rpaths,
                         'search_paths': search_paths,
                         'dependencies': [d for d in dependencies if d != identity]}

    for name in required:
        if (bundle / name).resolve() not in images:
            raise ValueError('required file is not Mach-O: ' + name)
    # dlopen roots: the driver and Qt plugins inherit the application's runpaths.
    # Inspect both executables and every image, including framework binaries that
    # have neither an executable permission bit nor a .dylib suffix.
    for executable in ('suyu', 'suyu-cmd'):
        main = images[(executable_dir / executable).resolve()]
        seen = set()

        def visit(image, inherited):
            record = images[image]
            search = tuple(dict.fromkeys(record['search_paths'] + list(inherited)))
            key = (image, search)
            if key in seen:
                return
            seen.add(key)
            for dep in record['dependencies']:
                target = resolve_dependency(dep, image, executable_dir, search, bundle)
                if isinstance(target, Path):
                    if target not in images:
                        raise ValueError('dependency is not a bundled Mach-O image: ' + dep)
                    visit(target, search)

        for image in images:
            visit(image, tuple(main['search_paths']))
    run('/usr/bin/codesign', '--verify', '--deep', '--strict', str(bundle))
    return {'status': 'passed', 'architecture': 'arm64',
            'moltenvk_bundled': True, 'cocoa_plugin_bundled': True,
            'signature_verified': True, 'notarized': False,
            'images': [{k: v for k, v in record.items() if k != 'search_paths'}
                       for record in images.values()]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    result = {'host': platform.platform(), 'app': str(args.app.resolve())}
    try:
        if platform.system() != 'Darwin':
            raise ValueError('not run: a real macOS host is required')
        result.update(audit(args.app))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        result.update(status='not_run' if platform.system() != 'Darwin' else 'failed',
                      error=str(error))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2) + '\n')
    print(result['status'])
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
