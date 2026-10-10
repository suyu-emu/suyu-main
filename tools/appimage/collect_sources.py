#!/usr/bin/env python3
"""Collect audited corresponding sources; reject uncovered redistributed ELFs."""
import argparse
import gzip
import json
import os
from pathlib import Path
import subprocess
import tarfile
import sys
from build import HERE, fetch, sha
sys.path.insert(0, str(HERE.parent / 'package_policy'))
from apt_signature import MAX_SIGNATURE_BYTES, validate_detached_signature


def validate(manifest, libraries):
    if manifest.get('schema') != 'suyu-appimage-sources-v1':
        raise ValueError('unsupported source manifest')
    if not manifest.get('runtime_complete') or not manifest.get('runtime_components'):
        raise ValueError('complete runtime dependency sources must be independently audited')
    runtime = json.loads((HERE / 'runtime.lock.json').read_text())
    if manifest.get('runtime_sha256') != runtime['sha256']:
        raise ValueError('runtime source receipt does not bind pinned runtime')
    required = {'type2-runtime', 'libfuse', 'squashfuse', 'musl', 'zlib', 'zstd', 'mimalloc'}
    if not required.issubset({c.get('name') for c in manifest['runtime_components']}):
        raise ValueError('runtime source receipt must cover exact static dependency closure')
    if not manifest.get('runtime_build_receipt'):
        raise ValueError('runtime build dependency versions are not established')
    receipt = manifest['runtime_build_receipt']
    if not isinstance(receipt, dict) or receipt.get('lock_sha256') != sha(HERE / 'runtime.lock.json'):
        raise ValueError('runtime build receipt is not bound to committed source lock')
    for key in ('sha256', 'size', 'image', 'recipe_sha256', 'components', 'sources', 'package_specs', 'apks', 'aports_build_commits'):
        if receipt.get(key) != runtime[key]:
            raise ValueError('runtime receipt differs from committed lock: ' + key)
    if manifest['runtime_components'] != runtime['components']:
        raise ValueError('runtime component source mapping differs from committed lock')
    covered = manifest.get('libraries', {})
    for path, digest in libraries.items():
        record = covered.get(path)
        if not record or record.get('sha256') != digest or not record.get('sources') or not record.get('copyright') or not record.get('source_version'):
            raise ValueError('missing exact corresponding-source coverage: ' + path)
    sources = {s['name']: s for s in manifest['sources']}
    for source in runtime['sources']:
        if sources.get(source['name']) != source:
            raise ValueError('runtime source archive mapping differs from committed lock')
    if not any(s.get('sha256') == runtime['source_sha256'] for s in sources.values()):
        raise ValueError('exact runtime project source missing')
    for record in list(covered.values()) + manifest['runtime_components']:
        if any(name not in sources for name in record['sources']):
            raise ValueError('source reference missing')
    return sources


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--appdir', type=Path, required=True)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    libs = {}
    for path in sorted(a.appdir.rglob('*')):
        if path.is_file() and not path.is_symlink() and '.so' in path.name:
            libs[path.relative_to(a.appdir).as_posix()] = sha(path)
    m = json.loads(a.manifest.read_text()); sources = validate(m, libs)
    a.output.mkdir(parents=True, exist_ok=False)
    for name, source in sources.items():
        if Path(name).name != name or not name.endswith(('.tar.gz', '.tar.xz', '.tar.bz2', '.dsc', '.diff.gz', '.tar.zst', '.asc')):
            raise ValueError('unsafe source filename: ' + repr(name))
        if name.endswith('.asc') and source.get('authentication') != 'apt signed Sources index':
            raise ValueError('detached signature lacks authenticated APT source linkage')
        fetch(source['url'], a.output / name, source['sha256'])
        if name.endswith('.asc'):
            if (a.output / name).stat().st_size > MAX_SIGNATURE_BYTES:
                raise ValueError('detached signature exceeds bounded size')
            validate_detached_signature((a.output / name).read_bytes())
    licenses = a.appdir / 'usr/share/doc/suyu/LICENSES'
    licenses.mkdir(parents=True, exist_ok=True)
    for path, record in m['libraries'].items():
        copyright = Path(record['copyright'])
        if not copyright.is_relative_to('/usr/share/doc') or not copyright.is_file():
            raise ValueError('Ubuntu package copyright missing')
        text = copyright.read_text()
        # Debian copyright files may incorporate full license text by reference.
        import re
        for common in set(re.findall(r'/usr/share/common-licenses/([A-Za-z0-9+-]+(?:\.[0-9]+)?)', text)):
            text += '\n\n' + (Path('/usr/share/common-licenses') / common).read_text()
        name = 'ubuntu-' + record['binary_package'].replace(':', '-') + '.txt'
        dest = licenses / name
        if dest.exists() and dest.read_text() != text:
            raise ValueError('conflicting package copyright')
        dest.write_text(text)
    receipt = dict(m, input_manifest_sha256=sha(a.manifest), bundled_libraries=libs,
                   collected_files={name: sha(a.output / name) for name in sources})
    (a.output / 'MANIFEST.json').write_text(json.dumps(receipt, indent=2) + '\n')
    (a.output / 'README.txt').write_text('Audited AppImage corresponding sources. See MANIFEST.json for exact binary/source mappings.\n')
    version = m['release_name']
    if not version or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-' for c in version):
        raise ValueError('invalid release name')
    archive = a.output.parent / ('suyu-' + version + '-appimage-sources.tar.gz')
    epoch = int(os.environ['SOURCE_DATE_EPOCH'])
    with archive.open('xb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', filename='', mtime=epoch) as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as tar:
            for file in sorted(a.output.iterdir()):
                info = tar.gettarinfo(str(file), arcname='suyu-' + version + '-dependency-sources/' + file.name)
                info.uid = info.gid = 0; info.uname = info.gname = ''; info.mtime = epoch
                with file.open('rb') as stream: tar.addfile(info, stream)
    print(archive)


if __name__ == '__main__':
    main()
