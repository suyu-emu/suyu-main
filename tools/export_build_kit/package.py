"""Freeze Ninja's resolved host link inputs; no source/build paths survive."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys

REVISION = "suyu-aot-kit-abi6-fm1-gg1-fpx1-control-r3"
SOURCE_ROOT = Path(__file__).resolve().parents[2]
POLICY_PATH = SOURCE_ROOT / 'tools/package_policy/policy.json'


def producer_compiler_version(build):
    versions = set()
    for metadata in (build / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'):
        match = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([0-9]+(?:\.[0-9]+)+)"\)',
                          metadata.read_text(encoding='utf-8'))
        if match:
            versions.add(match[1])
    if len(versions) != 1:
        raise ValueError('Build kit requires unambiguous producer C++ compiler version metadata')
    return versions.pop()


def ninja_words(value):
    # Ninja escapes spaces/colons/dollars independently of Windows quoting.
    return [re.sub(r"\$(.)", r"\1", word) for word in
            re.findall(r"(?:\$.|[^\s])+", value)]


def windows_words(value):
    return [re.sub(r"\$(.)", r"\1", word.strip('"')) for word in re.findall(r'"[^"]*"|[^\s]+', value)]


def link_edge(manifest, target):
    lines = manifest.replace("$\n", "").splitlines()
    for index, line in enumerate(lines):
        if not line.startswith("build ") or target not in line:
            continue
        # Rule separator is an unescaped colon followed by whitespace.
        match = re.match(r"build (.*?)(?<!\$): (\S+) (.*)", line)
        if not match or "EXECUTABLE_LINKER" not in match[2]:
            continue
        values = {}
        for following in lines[index + 1:]:
            if not following.startswith("  "):
                break
            key, separator, value = following.strip().partition(" = ")
            if separator:
                values[key] = value
        objects = [word for word in ninja_words(match[3].split(" | ")[0])
                   if word.lower().endswith((".obj", ".res"))]
        return objects, windows_words(values.get("LINK_LIBRARIES", "")), windows_words(values.get("LINK_FLAGS", ""))
    raise ValueError("No resolved executable link edge for " + target)


def cmake_quote(value):
    if any(char in value for char in ';\n\r"'):
        raise ValueError("Unsupported CMake path or flag: " + value)
    return '"' + value.replace('\\', '/') + '"'


def cache_value(build, name):
    cache = build / 'CMakeCache.txt'
    if not cache.is_file():
        return ''
    match = re.search(r'^' + name + r'(?::[A-Za-z]+)?=(.*)$', cache.read_text(encoding='utf-8', errors='replace'), re.M)
    return match[1].strip() if match else ''


def source_revision(root):
    """Commit the packaged host was built from: git HEAD, else a GIT-COMMIT file."""
    try:
        result = subprocess.run(['git', '-C', str(root), 'rev-parse', 'HEAD'], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, timeout=30)
        if result.returncode == 0 and re.fullmatch(r'[0-9a-f]{40,64}', result.stdout.strip()):
            return result.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    marker = Path(root) / 'GIT-COMMIT'
    if marker.is_file():
        value = marker.read_text(encoding='utf-8').strip()
        if re.fullmatch(r'[0-9a-f]{40,64}', value):
            return value
    raise ValueError('Cannot determine the producer source revision (no git checkout or GIT-COMMIT file)')


def is_export_kit(directory):
    revision = directory / 'revision.txt'
    if revision.is_file() and revision.read_text(encoding='utf-8', errors='replace').startswith('suyu-aot-kit-'):
        return True
    manifest = directory / 'manifest.json'
    if manifest.is_file():
        try:
            return 'revision' in json.loads(manifest.read_text(encoding='utf-8'))
        except (ValueError, TypeError):
            return False
    return False


def check_destination(output):
    if not output.exists():
        return
    if not output.is_dir() or (any(output.iterdir()) and not is_export_kit(output)):
        raise ValueError('Refusing to replace a directory that is not an export build kit')


def remove_tree(path):
    def writable(function, target, _error):
        os.chmod(target, stat.S_IWRITE)
        function(target)
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=writable)
    else:
        shutil.rmtree(path, onerror=writable)


def replace_destination(staging, output):
    check_destination(output)
    if not output.exists():
        staging.rename(output)
        return
    old = output.parent / ('.' + output.name + '.old-' + secrets.token_hex(6))
    output.rename(old)
    try:
        staging.rename(output)
    except BaseException:
        old.rename(output)
        raise
    try:
        remove_tree(old)
    except OSError:
        pass  # The kit is already in place; a leftover hidden sibling is harmless.


def package(build, output, revision):
    if revision != REVISION:
        raise ValueError("Unsupported build-kit revision")
    build, output = Path(build).resolve(), Path(output).resolve()
    recomp_dir = cache_value(build, 'SUYU_CMD_RECOMP_DIR')
    if recomp_dir:
        raise ValueError("Refusing to package a build tree configured for a specific game (SUYU_CMD_RECOMP_DIR is set); "
                         "build the generic export host from a clean tree")
    compiler_version = producer_compiler_version(build)
    producer_revision = source_revision(SOURCE_ROOT)
    policy_version = json.loads(POLICY_PATH.read_text(encoding='utf-8'))['policy_version']
    manifest = (build / "build.ninja").read_text(encoding="utf-8")
    check_destination(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = output.parent / ('.' + output.name + '.staging-' + secrets.token_hex(6))
    staging.mkdir()
    try:
        inputs = build_kit(build, staging, manifest, compiler_version, producer_revision, policy_version, recomp_dir)
        actual = {path.relative_to(staging).as_posix() for path in staging.rglob('*') if path.is_file()}
        if actual != set(inputs['files']) | {'manifest.json'}:
            raise ValueError("Staged build kit does not match its manifest: " +
                             ', '.join(sorted(actual ^ (set(inputs['files']) | {'manifest.json'}))))
        replace_destination(staging, output)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def is_inside(path, parent):
    return path == parent or parent in path.parents


def game_specific(text, parts):
    lowered = text.lower()
    return ('recomp_registration' in lowered or 'recomp_static_' in lowered or
            any(part.lower() in ('recomp', 'recomp_modules') for part in parts))


def build_kit(build, staging, manifest, compiler_version, producer_revision, policy_version, recomp_dir):
    copied = {}
    written = []
    cpm_cache = cache_value(build, 'CPM_SOURCE_CACHE') or os.environ.get('CPM_SOURCE_CACHE', '')

    def emit(relative, data):
        destination = staging / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, Path):
            shutil.copy2(data, destination)
        else:
            destination.write_text(data, encoding='ascii' if relative == 'revision.txt' else 'utf-8')
        written.append(relative)

    def allowed_library_location(path):
        parts = [part.lower() for part in path.parts]
        if any(parts[i:i + 2] == ['.cache', 'cpm'] for i in range(len(parts))):
            return True  # CPM package cache holding pinned third-party static libraries
        return bool(cpm_cache) and is_inside(path, Path(cpm_cache).resolve())

    def freeze(raw, mode, is_object):
        path = Path(raw)
        if not path.is_absolute():
            path = build / path
        path = path.resolve()
        if game_specific(raw, ()) or game_specific(path.name, ()):
            raise ValueError("Game-specific link input is not allowed in a generic build kit: " + raw)
        if recomp_dir and is_inside(path, Path(recomp_dir).resolve()):
            raise ValueError("Link input lies under SUYU_CMD_RECOMP_DIR: " + raw)
        if not path.is_file():
            # Only bare names may be resolved by the installed Windows SDK.
            if '/' not in raw and '\\' not in raw and raw.lower().endswith('.lib') and not is_object:
                return raw
            raise ValueError("Missing link input: " + raw)
        inside = is_inside(path, build)
        if inside and game_specific('', path.relative_to(build).parts[:-1]):
            raise ValueError("Game-specific link input is not allowed in a generic build kit: " + raw)
        if is_object:
            parts = path.relative_to(build).parts if inside else ()
            host_dir = 'suyu-export-host-' + mode + '.dir'
            if not any(parts[i:i + 2] == ('CMakeFiles', host_dir) for i in range(len(parts))):
                raise ValueError("Object is not an output of the generic " + host_dir + " target: " + raw)
        elif not inside and not allowed_library_location(path):
            raise ValueError("Link library is outside the build tree: " + raw)
        elif not inside and game_specific('', path.parts[:-1]):
            raise ValueError("Game-specific link input is not allowed in a generic build kit: " + raw)
        key = str(path)
        if key not in copied:
            relative = "inputs/" + hashlib.sha256(key.encode()).hexdigest()[:20] + '-' + path.name
            emit(relative, path)
            copied[key] = relative
        return '${CMAKE_CURRENT_LIST_DIR}/' + copied[key]
    for mode in ('strict', 'hybrid'):
        objects, libraries, flags = link_edge(manifest, 'suyu-export-host-' + mode + '.exe')
        probes = [obj for obj in objects if 'registry_probe.c.obj' in obj]
        if len(probes) != 1:
            raise ValueError("Expected exactly one excluded registry probe object")
        objects = [freeze(obj, mode, True) for obj in objects if obj not in probes]
        if not objects:
            raise ValueError("Host object list is empty")
        libraries = [freeze(lib, mode, False) for lib in libraries]
        for flag in flags:
            if '\\' in flag or re.search(r'[A-Za-z]:[/\\]', flag) or '$' in flag:
                raise ValueError("Nonportable linker flag: " + flag)
        content = '\n'.join('set(KIT_' + key + '\n  ' + '\n  '.join(map(cmake_quote, vals)) + '\n)' for key, vals in
                            [('OBJECTS', objects), ('LIBRARIES', libraries), ('LINK_OPTIONS', flags)])
        emit(mode + '.cmake', content + '\n')
    emit('CMakeLists.txt', Path(__file__).with_name('CMakeLists.txt'))
    emit('src/suyu_cmd/recomp_modules/CMakeLists.txt', SOURCE_ROOT / 'src/suyu_cmd/recomp_modules/CMakeLists.txt')
    emit('revision.txt', REVISION + '\n')
    files = {relative: hashlib.sha256((staging / relative).read_bytes()).hexdigest() for relative in sorted(written)}
    document = {
        'revision': REVISION,
        'producer_compiler_version': compiler_version,
        'policy_version': policy_version,
        'producer_source_revision': producer_revision,
        'inputs': {relative: files[relative] for relative in copied.values()},
        'files': files,
        'note': 'Hashes identify packaged inputs and detect corruption; they do not grant permission or establish legal clearance.',
    }
    (staging / 'manifest.json').write_text(json.dumps(document, indent=2), encoding='utf-8')
    return document


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--revision', default=REVISION)
    args = parser.parse_args()
    package(args.build, args.output, args.revision)
