"""Delivery-only aports recipe projections; immutable runtime inputs stay intact."""
import binascii
import hashlib
import io
import json
from pathlib import Path
import struct
import tarfile

CONTEXT_FILES = ('README.md', 'CODINGSTYLE.md', 'COMMITSTYLE.md')


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        while chunk := stream.read(1 << 20):
            value.update(chunk)
    return value.hexdigest()


def canonical_gzip(data):
    """Stored DEFLATE blocks avoid compressor-version-dependent output hashes."""
    result = bytearray(b'\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff')
    for start in range(0, max(1, len(data)), 65535):
        block = data[start:start + 65535]
        result.append(1 if start + len(block) == len(data) else 0)
        result.extend(struct.pack('<HH', len(block), len(block) ^ 0xFFFF))
        result.extend(block)
    result.extend(struct.pack('<II', binascii.crc32(data), len(data) & 0xFFFFFFFF))
    return bytes(result)


def recipe_projection(input_path, source, recipe_root, metadata_only=False):
    if digest(input_path) != source['sha256']:
        raise ValueError('aports projection input differs from immutable runtime pin')
    commit = source['url'].rsplit('/', 1)[1]
    if len(commit) != 40 or any(c not in '0123456789abcdef' for c in commit):
        raise ValueError('exact aports snapshot commit required')
    top = 'aports-' + commit
    selected = [top + '/' + name for name in CONTEXT_FILES]
    prefix = top + '/' + recipe_root + '/' if recipe_root else None
    members = []; contents = {}; occurrences = {}
    with tarfile.open(input_path, 'r:*') as archive:
        for entry in archive:
            occurrences[entry.name] = occurrences.get(entry.name, 0) + 1
            included = entry.name in selected or (prefix and entry.name.startswith(prefix))
            if not included or entry.isdir():
                continue
            if not entry.isfile() or occurrences[entry.name] != 1:
                raise ValueError('recipe projection requires unique regular source members')
            data = archive.extractfile(entry).read()
            contents[entry.name] = data
            members.append(dict(name=entry.name, occurrence=1, type='file', size=len(data),
                                mode=entry.mode & 0o777, sha256=hashlib.sha256(data).hexdigest()))
    if not metadata_only and (prefix + 'APKBUILD') not in contents:
        raise ValueError('complete corresponding aports APKBUILD required')
    if not members:
        raise ValueError('empty aports projection')
    buffer = io.BytesIO()
    members.sort(key=lambda row: row['name'])
    with tarfile.open(fileobj=buffer, mode='w', format=tarfile.GNU_FORMAT) as archive:
        for row in members:
            entry = tarfile.TarInfo(row['name']); entry.size = row['size']; entry.mode = row['mode']
            entry.uid = entry.gid = entry.mtime = 0; entry.uname = entry.gname = ''
            archive.addfile(entry, io.BytesIO(contents[row['name']]))
    data = canonical_gzip(buffer.getvalue())
    receipt = dict(name=source['name'], original_sha256=source['sha256'], original_url=source['url'],
                   snapshot_commit=commit, recipe_root=recipe_root, metadata_only=metadata_only,
                   members=members, output_sha256=hashlib.sha256(data).hexdigest(), output_size=len(data))
    return data, receipt


def load_delivery_lock(folder, runtime_lock, policy):
    lock = json.loads((folder / 'source-delivery.lock.json').read_text())
    canonical = hashlib.sha256(json.dumps(lock, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    if (lock.get('schema') != 'suyu-source-delivery-v1' or
            canonical != policy['appimage'].get('source_delivery_lock_canonical_sha256')):
        raise ValueError('source delivery lock differs from reviewed policy')
    originals = {row['name']: row for row in runtime_lock['sources']}
    for name, record in lock['projections'].items():
        original = originals.get(name)
        if (not original or record['name'] != name or record['original_sha256'] != original['sha256'] or
                record['original_url'] != original['url']):
            raise ValueError('source projection is not linked to immutable runtime input')
    return lock


def verify_distribution_records(document, delivery_lock):
    if document.get('source_distributions') != delivery_lock['projections']:
        raise ValueError('source distribution recipe completeness differs from reviewed lock')
