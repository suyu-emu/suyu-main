#!/usr/bin/env bash
# Build-host packaging only. End users do not run this script.
set -euo pipefail
if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
    echo 'A real Apple Silicon Mac build host is required.' >&2
    exit 1
fi
repo=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:?Usage: package.sh BUILD_DIRECTORY OUTPUT_DIRECTORY MACDEPLOYQT}
out=${2:?Missing output directory}
deploy=${3:?Missing full macdeployqt path}
mkdir -p "$out"
out=$(cd "$out" && pwd)
stage=$(mktemp -d)
pending="$out/.suyu-macos-arm64.pending.zip"
trap 'rm -rf "$stage"; rm -f "$pending"' EXIT
APP="$stage/suyu.app"
ditto "$build/bin/suyu.app" "$APP"
cp "$build/bin/suyu-cmd" "$APP/Contents/MacOS/"
python3 - "$repo" "$APP" <<'PY'
import plistlib
import re
import sys
from pathlib import Path
repo, app = map(Path, sys.argv[1:])
if not (app / 'Contents/MacOS/suyu').is_file():
    raise SystemExit('The compiled suyu executable is missing')
version = re.search(r'Current version:\s+\*\*v(\d+\.\d+\.\d+)\*\*',
                    (repo / 'README.md').read_text())
if not version:
    raise SystemExit('Cannot determine bundle version from README.md')
plist = app / 'Contents/Info.plist'
with plist.open('rb') as stream:
    info = plistlib.load(stream)
# Repair old builds too: EXECUTABLE_NAME is not CMake's bundle template variable.
info.update(CFBundleExecutable='suyu', CFBundleName='suyu',
            CFBundleShortVersionString=version[1], CFBundleVersion=version[1],
            LSMinimumSystemVersion='15.0', NSHighResolutionCapable=True)
info.pop('LSRequiresCarbon', None)
with plist.open('wb') as stream:
    plistlib.dump(info, stream)
PY
# Homebrew splits Qt modules across formulae; qtbase's own rpaths do not
# locate QtSvg, QtPdf and QtVirtualKeyboard when deploying their plugins.
brew_prefix=$(brew --prefix)
qt_prefix=$(brew --prefix qt)
deploy_args=(-executable="$APP/Contents/MacOS/suyu-cmd" -always-overwrite
             -no-plugins -no-codesign -verbose=2
             -libpath="$qt_prefix/lib" -libpath="$brew_prefix/lib")
qt_libs=("$qt_prefix/lib" "$brew_prefix/lib")
for qt_lib in "$brew_prefix"/opt/qt*/lib; do
    [[ -d "$qt_lib" && "$qt_lib" != "$brew_prefix/opt/qt@5/lib" ]] || continue
    qt_libs+=("$qt_lib")
    deploy_args+=(-libpath="$qt_lib")
done
# Select the desktop plugins explicitly. Automatic plugin deployment retries
# optional PDF/QML/keyboard modules without honoring the supplied library roots.
qt_plugins=$("$qt_prefix/bin/qmake" -query QT_INSTALL_PLUGINS)
[[ -d "$qt_plugins" ]] || { echo 'Cannot locate the installed Qt plugins' >&2; exit 1; }
rm -rf "$APP/Contents/PlugIns"
shopt -s nullglob
for category in platforms styles imageformats iconengines tls networkinformation; do
    for plugin in "$qt_plugins/$category"/*.dylib; do
        base=$(basename "$plugin")
        [[ "$base" != *_debug.dylib && "$base" != libqpdf.dylib ]] || continue
        [[ "$category" != platforms || "$base" == libqcocoa.dylib ]] || continue
        mkdir -p "$APP/Contents/PlugIns/$category"
        target="$APP/Contents/PlugIns/$category/$base"
        cp -p "$plugin" "$target"
        deploy_args+=(-executable="$target")
    done
done
shopt -u nullglob
for plugin in platforms/libqcocoa.dylib iconengines/libqsvgicon.dylib imageformats/libqsvg.dylib; do
    [[ -f "$APP/Contents/PlugIns/$plugin" ]] || {
        echo "Required desktop Qt plugin is missing: $plugin" >&2; exit 1;
    }
done
# Give each copied plugin an actual load-command search path for its dependencies.
# This also handles QtSvg when macdeployqt's plugin scan ignores -libpath.
python3 - "$repo" "$APP" "${qt_libs[@]}" <<'PY'
import subprocess
import sys
from pathlib import Path
repo, app = map(Path, sys.argv[1:3])
roots = list(dict.fromkeys(Path(value) for value in sys.argv[3:]))
sys.path.insert(0, str(repo / 'tools/macos'))
from verify_bundle import parse_dependencies, parse_identity, parse_rpaths, run
for plugin in sorted((app / 'Contents/PlugIns').rglob('*.dylib')):
    commands = run('/usr/bin/otool', '-arch', 'arm64', '-l', str(plugin))
    identity, rpaths = parse_identity(commands), parse_rpaths(commands)
    dependencies = parse_dependencies(run('/usr/bin/otool', '-arch', 'arm64', '-L', str(plugin)))
    for dep in dependencies:
        if dep == identity or not dep.startswith('@rpath/'):
            continue
        relative = dep[len('@rpath/'):]
        source = next((root for root in roots if (root / relative).is_file()), None)
        if source is None:
            raise SystemExit(f'Cannot locate {dep} required by {plugin.name}')
        if str(source) not in rpaths:
            subprocess.run(['/usr/bin/install_name_tool', '-add_rpath', str(source), str(plugin)], check=True)
            rpaths.append(str(source))
PY
"$deploy" "$APP" "${deploy_args[@]}"
python3 - "$repo" "$APP" <<'PY'
import os
import subprocess
import sys
from pathlib import Path
repo, app = map(Path, sys.argv[1:])
sys.path.insert(0, str(repo / 'tools/macos'))
from verify_bundle import MACHO_MAGIC, expand_path, inside, parse_dependencies, parse_identity, parse_rpaths, run, system_path
app = app.resolve()
frameworks = app / 'Contents/Frameworks'
executable_dir = app / 'Contents/MacOS'
seen = set()
for candidate in sorted(app.rglob('*')):
    if not candidate.is_file():
        continue
    with candidate.open('rb') as stream:
        if stream.read(4) not in MACHO_MAGIC:
            continue
    image = candidate.resolve()
    if image in seen:
        continue
    if not inside(image, app):
        raise SystemExit('External image in staged bundle: ' + str(candidate))
    seen.add(image)
    commands = run('/usr/bin/otool', '-arch', 'arm64', '-l', str(image))
    rpaths, identity = parse_rpaths(commands), parse_identity(commands)
    dependencies = parse_dependencies(run('/usr/bin/otool', '-arch', 'arm64', '-L', str(image)))
    for dependency in dependencies:
        if dependency == identity or not dependency.startswith('/') or system_path(dependency):
            continue
        if '.framework/' in dependency:
            before, after = dependency.split('.framework/', 1)
            target = frameworks / (Path(before).name + '.framework') / after
        else:
            target = frameworks / Path(dependency).name
        if not target.is_file() or not inside(target, app):
            raise SystemExit('Unbundled runtime dependency: ' + dependency)
        replacement = '@rpath/' + target.relative_to(frameworks).as_posix()
        subprocess.run(['/usr/bin/install_name_tool', '-change', dependency, replacement, str(image)], check=True)
    for path in rpaths:
        # Homebrew plugins also carry relative paths that escape the app.
        # Retain only supported paths that stay inside the relocated bundle.
        try:
            portable = not path.startswith('/') and inside(expand_path(path, image, executable_dir), app)
        except ValueError:
            portable = False
        if not portable:
            subprocess.run(['/usr/bin/install_name_tool', '-delete_rpath', path, str(image)], check=True)
    relative = Path(os.path.relpath(frameworks, image.parent)).as_posix()
    bundled = '@loader_path' + (('/' + relative) if relative != '.' else '')
    if bundled not in rpaths:
        subprocess.run(['/usr/bin/install_name_tool', '-add_rpath', bundled, str(image)], check=True)
    if identity:
        replacement = ('@rpath/' + image.relative_to(frameworks).as_posix()
                       if inside(image, frameworks) else '@loader_path/' + image.name)
        if identity != replacement:
            subprocess.run(['/usr/bin/install_name_tool', '-id', replacement, str(image)], check=True)
resources = app / 'Contents/Resources'
resources.mkdir(parents=True, exist_ok=True)
(resources / 'qt.conf').write_text('[Paths]\nPrefix = .\nPlugins = PlugIns\nLibraries = Frameworks\n')
PY
codesign --force --deep --sign - "$APP"
if ! python3 "$repo/tools/macos/verify_bundle.py" "$APP" --report "$out/bundle-audit.json"; then
    echo 'Bundle verification failed; details follow:' >&2
    python3 -m json.tool "$out/bundle-audit.json" >&2
    exit 1
fi
if ! python3 "$repo/tools/macos/verify_startup.py" "$APP" --evidence "$out/startup-evidence"; then
    echo 'Startup verification failed; details follow:' >&2
    python3 -m json.tool "$out/startup-evidence/startup-report.json" >&2
    exit 1
fi
# No application ZIP is emitted if the audit or startup check fails.
python3 "$repo/tools/package_policy/third_party_notices.py" --package-dir "$stage"
cp "$repo/docs/macos-first-run.md" "$stage/FIRST-RUN.md"
ditto -c -k --sequesterRsrc "$stage" "$pending"
python3 "$repo/tools/package_policy/scan_release.py" --kind macos "$pending"
mv "$pending" "$out/suyu-macos-arm64.zip"
python3 - "$repo" "$out" <<'PY'
import hashlib
import json
import platform
import subprocess
import sys
from pathlib import Path
repo, out = map(Path, sys.argv[1:])
archive = out / 'suyu-macos-arm64.zip'
record = {'source_commit': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
          'source_modified': bool(subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain'], text=True).strip()),
          'host': platform.platform(), 'architecture': platform.machine(),
          'compiler': subprocess.check_output(['/usr/bin/xcrun', 'clang', '--version'], text=True).strip(),
          'sdk_version': subprocess.check_output(['/usr/bin/xcrun', '--sdk', 'macosx', '--show-sdk-version'], text=True).strip(),
          'archive': archive.name, 'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
          'signing': 'ad hoc', 'notarized': False, 'games_included': False,
          'target_mac_tested': False}
(out / 'build-record.json').write_text(json.dumps(record, indent=2) + '\n')
(out / 'SHA256SUMS.txt').write_text(record['sha256'] + '  ' + archive.name + '\n')
PY
