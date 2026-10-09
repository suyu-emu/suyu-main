#!/usr/bin/env python3
"""Build an Ubuntu 24.04 x86_64 AppImage from an already built release package."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import urllib.request

HERE = Path(__file__).resolve().parent


def apprun_script():
    return '''#!/bin/sh
set -eu
APPDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export APPDIR
export XDG_CURRENT_DESKTOP="${XDG_CURRENT_DESKTOP-}"
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
for hook in "$APPDIR"/apprun-hooks/*.sh; do [ ! -f "$hook" ] || . "$hook"; done
command=suyu
if [ "${1-}" = --suyu-cmd ]; then command=suyu-cmd; shift; fi
exec "$APPDIR/usr/bin/$command" "$@"
'''


def deployment_env(qmake):
    return dict(os.environ, QMAKE=qmake, ARCH='x86_64', QT_QPA_PLATFORM='offscreen',
                EXTRAPLATFORM_PLUGINS='libqoffscreen.so', DISABLE_COPYRIGHT_FILES_DEPLOYMENT='1')


def retain_optional_translations(app, work):
    """Keep the tool's optional localization output outside the release AppDir."""
    translations = app / 'usr/translations'
    if not translations.exists(): return {}
    if translations.is_symlink() or not translations.is_dir():
        raise ValueError('unexpected Qt translation deployment')
    hashes = {}
    for path in sorted(translations.rglob('*')):
        if path.is_symlink() or not path.is_file() or path.suffix != '.qm' or path.parent != translations:
            raise ValueError('unexpected file in optional Qt translation subtree')
        hashes[path.name] = sha(path)
    retained = work / 'omitted-qt-translations'
    if retained.exists(): raise ValueError('optional translation retention destination already exists')
    shutil.move(str(translations), retained)
    return hashes


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def fetch(url, path, digest):
    if not path.exists():
        with urllib.request.urlopen(url, timeout=120) as source, path.open('xb') as target:
            shutil.copyfileobj(source, target)
    if sha(path) != digest:
        raise ValueError('SHA256 mismatch: ' + str(path))
    return path


def inventory(root):
    files, links = {}, {}
    for path in sorted(root.rglob('*')):
        name = path.relative_to(root).as_posix()
        if name == 'usr/share/suyu/appimage-provenance.json':
            continue
        if path.is_symlink():
            # A scanner must verify these too, rather than following external paths.
            if not path.resolve().is_relative_to(root.resolve()):
                raise ValueError('AppDir symlink escapes: ' + name)
            links[name] = os.readlink(path)
        elif path.is_file():
            files[name] = sha(path)
    return files, links


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package-dir', type=Path, required=True)
    p.add_argument('--work-dir', type=Path, required=True)
    p.add_argument('--source-revision', required=True)
    p.add_argument('--runtime-dir', type=Path, required=True, help='verified runtime.py output')
    p.add_argument('--release-name', required=True, help='v0.0.14 or a safe workflow-dispatch reference')
    p.add_argument('--repository', required=True, help='GitHub owner/repository from GITHUB_REPOSITORY')
    p.add_argument('--qmake', default='qmake6')
    a = p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', a.repository) or any(x in ('.', '..') for x in a.repository.split('/')):
        p.error('repository must be a GitHub owner/repository')
    a.release_name = re.sub(r'[^A-Za-z0-9._-]', '-', a.release_name)
    if not a.release_name or a.release_name in ('.', '..'):
        p.error('invalid release name')
    release = platform.freedesktop_os_release()
    if release.get('ID') != 'ubuntu' or release.get('VERSION_ID') != '24.04' or platform.machine() != 'x86_64':
        p.error('supported build environment is Ubuntu 24.04 x86_64')
    if len(a.source_revision) != 40 or any(c not in '0123456789abcdef' for c in a.source_revision):
        p.error('source revision must be a full lowercase git commit')
    if not os.environ.get('SOURCE_DATE_EPOCH', '').isdigit():
        p.error('SOURCE_DATE_EPOCH must be the release commit timestamp')
    a.work_dir.mkdir(parents=True, exist_ok=False)
    work = a.work_dir.resolve()
    lock = json.loads((HERE / 'tools.lock.json').read_text())
    tool_dir = work / 'tools'; tool_dir.mkdir()
    downloads = tool_dir / 'downloads'; downloads.mkdir()
    tools = {}
    for entry in lock['tools']:
        if entry['repo'] == 'AppImage/type2-runtime':
            continue  # Release runtime is built locally from the pinned full source closure.
        binary = fetch(entry['url'], downloads / entry['name'], entry['sha256'])
        binary.chmod(0o755)
        if entry['name'].endswith('.AppImage'):
            extract = tool_dir / entry['repo'].split('/')[-1]; extract.mkdir()
            subprocess.run([str(binary), '--appimage-extract'], cwd=extract, check=True)
            tools[entry['repo']] = extract / 'squashfs-root/AppRun'
        else:
            tools[entry['repo']] = binary
    app = work / 'AppDir'; (app / 'usr/bin').mkdir(parents=True)
    for name in ('suyu', 'suyu-cmd'):
        shutil.copy2(a.package_dir / name, app / 'usr/bin' / name)
    docs = app / 'usr/share/doc/suyu'; docs.mkdir(parents=True)
    for name in ('LICENSE.txt', 'THIRD-PARTY-NOTICES.txt', 'LICENSES'):
        source = a.package_dir / name
        if not source.exists():
            raise ValueError('release license bundle missing: ' + name)
        if source.is_dir(): shutil.copytree(source, docs / name)
        else: shutil.copy2(source, docs / name)
    licenses = docs / 'packaging-tool-licenses'; licenses.mkdir()
    for entry in lock['tools']:
        fetch(entry['license_url'], licenses / (entry['repo'].replace('/', '-') + '.txt'), entry['license_sha256'])
    desktop = work / 'suyu.desktop'
    desktop.write_text('[Desktop Entry]\nType=Application\nName=suyu\nExec=suyu %f\nIcon=suyu\nCategories=Game;Emulator;\n')
    env = deployment_env(a.qmake)
    # Extracted plugin avoids FUSE and prevents discovery of an unpinned PATH plugin.
    plugin = tool_dir / 'linuxdeploy-plugin-qt'
    plugin.write_text('#!/bin/sh\nexec "' + str(tools['linuxdeploy/linuxdeploy-plugin-qt']) + '" "$@"\n')
    plugin.chmod(0o755); env['PATH'] = str(tool_dir) + os.pathsep + env['PATH']
    subprocess.run([str(tools['linuxdeploy/linuxdeploy']), '--appdir', str(app),
                    '--executable', str(app / 'usr/bin/suyu'), '--executable', str(app / 'usr/bin/suyu-cmd'),
                    '--desktop-file', str(desktop), '--icon-file', str(HERE.parents[1] / 'dist/suyu.svg'),
                    '--plugin', 'qt'], env=env, check=True)
    omitted_translations = retain_optional_translations(app, work)
    apprun = app / 'AppRun'
    if apprun.is_symlink(): apprun.unlink()
    apprun.write_text(apprun_script())
    apprun.chmod(0o755)
    if not (app / 'apprun-hooks/linuxdeploy-plugin-qt-hook.sh').is_file():
        raise ValueError('pinned Qt deployment hook missing')
    if not (app / 'usr/plugins/platforms/libqoffscreen.so').is_file():
        raise ValueError('offscreen Qt platform plugin was not bundled')
    if not (app / 'usr/plugins/platforminputcontexts').is_dir():
        raise ValueError('Qt platform input contexts missing')
    # The collector is invoked after deployment, so closure is verified against actual payload.
    from payload_sources import collect
    source_manifest = collect(app, work / 'apt-sources', a.runtime_dir.resolve(), a.release_name)
    subprocess.run(['python3', str(HERE / 'collect_sources.py'), '--appdir', str(app),
                    '--manifest', str(source_manifest), '--output', str(work / 'sources')], check=True)
    meta = app / 'usr/share/suyu'; meta.mkdir(parents=True, exist_ok=True)
    shutil.copy2(work / 'sources/MANIFEST.json', meta / 'distribution-sources.json')
    runtime = json.loads((a.runtime_dir / 'runtime-receipt.json').read_text())
    runtime_lock = json.loads((HERE / 'runtime.lock.json').read_text())
    if runtime['sha256'] != runtime_lock['sha256'] or runtime['lock_sha256'] != sha(HERE / 'runtime.lock.json'):
        raise ValueError('runtime receipt is not bound to reviewed runtime lock')
    tools['AppImage/type2-runtime'] = a.runtime_dir.resolve() / 'runtime-x86_64'
    if sha(tools['AppImage/type2-runtime']) != runtime['sha256']:
        raise ValueError('runtime artifact receipt mismatch')
    files, links = inventory(app)
    proof = dict(schema='suyu-appimage-v1', policy_version='suyu-package-policy-1',
                 source_revision=a.source_revision, source_url='https://github.com/' + a.repository + '/tree/' + a.source_revision,
                 runtime_sha256=runtime['sha256'], runtime_size=runtime['size'],
                 runtime_source_url=runtime['source_url'], files=files, links=links,
                 runtime_recipe_sha256=runtime['recipe_sha256'], runtime_lock_sha256=runtime['lock_sha256'],
                 omitted_optional_qt_translations=omitted_translations,
                 tools_lock_sha256=sha(HERE / 'tools.lock.json'), compatibility='Ubuntu 24.04 x86_64')
    (meta / 'appimage-provenance.json').write_text(json.dumps(proof, indent=2) + '\n')
    epoch = int(os.environ['SOURCE_DATE_EPOCH'])
    for path in sorted(app.rglob('*')) + [app]:
        os.utime(path, (epoch, epoch), follow_symlinks=False)
    artifact = work / 'suyu-linux-x86_64.AppImage'
    subprocess.run([str(tools['AppImage/appimagetool']), '--runtime-file', str(tools['AppImage/type2-runtime']),
                    str(app), str(artifact)], env=env, check=True)
    with artifact.open('rb') as stream:
        if hashlib.sha256(stream.read(runtime['size'])).hexdigest() != runtime['sha256'] or stream.read(4) != b'hsqs':
            raise ValueError('runtime prefix or SquashFS offset mismatch')
    proof['artifact_sha256'] = sha(artifact)
    (work / 'appimage-provenance.json').write_text(json.dumps(proof, indent=2) + '\n')
    print(artifact)


if __name__ == '__main__':
    main()
