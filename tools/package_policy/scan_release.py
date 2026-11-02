#!/usr/bin/env python3
"""Package-policy validation of public release archives (stdlib only).

Scans the members of an archive without extracting anything to disk and reports
which policy rules (tools/package_policy/policy.json) they violate. Findings
carry a stable rule id, the member path and a short reason; file contents and
matched key text are never printed or stored.

This is a distribution control for the project's own release assets. Passing it
is not a legal clearance, and the hashes checked for the export build kit
identify and protect packaged inputs; they do not grant permission.

Exit status: 0 clean, 1 findings, 2 usage error.
"""
import argparse
import contextlib
import gzip
import io
import hashlib
import json
from pathlib import Path
import posixpath
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from apt_signature import MAX_SIGNATURE_BYTES, validate_detached_signature
from source_fixtures import load_review

POLICY_PATH = Path(__file__).with_name('policy.json')
sys.path.insert(0, str(POLICY_PATH.parent.parent / 'appimage'))
from source_delivery import load_delivery_lock, verify_distribution_records
KINDS = ('windows', 'linux', 'linux-appimage', 'macos', 'android-apk', 'libretro-linux', 'libretro-windows',
         'libretro-macos', 'libretro-android', 'source', 'dependency-sources')
HEAD_BYTES = 16384 + 8
WHOLE_FILE_LIMIT = 1 << 20
NESTED_MAX_BYTES = 256 << 20
NESTED_MAX_DEPTH = 2
MANIFEST_MAX_BYTES = 8 << 20
SPOOL_BYTES = 32 << 20
ZIP_EXTENSIONS = ('.zip', '.apk', '.jar')
TAR_EXTENSIONS = ('.tar.gz', '.tgz', '.tar.xz', '.txz', '.tar.bz2', '.tbz2', '.tar')
OPAQUE_EXTENSIONS = ('.7z', '.rar', '.xz', '.zst', '.cab', '.msi', '.lz4', '.bz2', '.gz', '.iso')
KIT_PREFIX = 'export-build-kit/'
SHA256_RE = re.compile(r'^[0-9a-f]{64}$')


def load_policy(path=POLICY_PATH):
    return json.loads(Path(path).read_text(encoding='utf-8'))


class Rules:
    def __init__(self, policy):
        self.policy = policy
        self.version = policy['policy_version']
        self.key_names = [re.compile(p, re.I) for p in policy['key_file_name_patterns']]
        self.key_text = [re.compile(p) for p in policy['key_text_patterns']]
        self.firmware_names = [re.compile(p, re.I) for p in policy['firmware_nand_name_patterns']]
        self.firmware_paths = [re.compile(p, re.I) for p in policy['firmware_nand_path_patterns']]
        self.containers = tuple(e.lower() for e in policy['game_container_extensions'])
        self.markers = {n.lower() for n in policy['game_export_marker_names']}
        self.user_data = [re.compile(p) for p in policy['user_data_path_patterns']]
        self.signatures = [(s['name'], s['offset'], bytes.fromhex(s['hex'])) for s in policy['content_signatures']]
        self.text_limit = int(policy['key_text_scan_limit_bytes'])
        self.layouts = {k: [re.compile(p) for p in v] for k, v in policy.get('release_layouts', {}).items()}
        self.exceptions = [
            {'kinds': set(e.get('kinds', [])), 'path': re.compile(e['path_regex']),
             'rules': set(e.get('rules', []))}
            for e in policy.get('approved_exceptions', [])]

    def excused(self, kind, rule, member):
        return any(kind in e['kinds'] and rule in e['rules'] and e['path'].fullmatch(member)
                   for e in self.exceptions)


def clean_label(text):
    return ''.join(c if c.isprintable() else '?' for c in text)


def is_plain_text(prefix):
    """True when bytes are only printable ASCII and whitespace.

    A container magic that sits at a fixed offset follows binary header data (keys,
    hashes, code), so ordinary text that merely has the same four letters at that
    offset is not a container.
    """
    return all(b in (9, 10, 13) or 32 <= b < 127 for b in prefix)


def base_name(path):
    return posixpath.basename(path).rstrip('. ').lower()


def normalize(raw, strip_dot_slash):
    """Return (normalized path, is_dir, list of path-unsafe reasons)."""
    problems = []
    name = raw
    if '\x00' in name:
        problems.append('NUL in name')
        name = name.replace('\x00', '')
    if '\\' in name:
        problems.append('backslash in name')
        name = name.replace('\\', '/')
    if strip_dot_slash:
        while name.startswith('./'):
            name = name[2:]
    is_dir = name.endswith('/')
    stripped = name.rstrip('/')
    if not raw:
        problems.append('empty name')
    if stripped.startswith('/'):
        problems.append('absolute path')
    elif re.match(r'^[A-Za-z]:', stripped):
        problems.append('drive letter')
    parts = stripped.split('/')
    if '..' in parts:
        problems.append('parent-directory component')
    if stripped and '' in parts[1 if stripped.startswith('/') else 0:]:
        problems.append('empty path component')
    kept = [p for p in parts if p not in ('', '.', '..')]
    return '/'.join(kept), is_dir, problems


class Entry:
    __slots__ = ('raw', 'is_dir', 'link', 'link_target', 'size', 'opener', 'source_occurrence', 'source_fact')

    def __init__(self, raw, is_dir, link, link_target, size, opener):
        self.raw, self.is_dir, self.link, self.link_target = raw, is_dir, link, link_target
        self.size, self.opener = size, opener
        self.source_occurrence, self.source_fact = 0, None


def zip_entries(archive):
    for info in archive.infolist():
        is_dir = info.filename.endswith('/')
        link = target = None
        if not is_dir and stat.S_ISLNK(info.external_attr >> 16):
            link = 'symlink'
            if info.file_size <= 4096:
                try:
                    target = archive.read(info).decode('utf-8', 'replace')
                except Exception:
                    target = None
        yield Entry(info.filename, is_dir, link, target, info.file_size,
                    lambda info=info: archive.open(info))


def tar_entries(archive):
    for member in archive:
        link = target = None
        if member.issym():
            link, target = 'symlink', member.linkname
        elif member.islnk():
            link, target = 'hardlink', member.linkname
        elif not (member.isfile() or member.isdir()):
            link = 'special file'
        yield Entry(member.name, member.isdir(), link, target, member.size,
                    lambda member=member: archive.extractfile(member))


def archive_format(name):
    lower = name.lower()
    if lower.endswith(ZIP_EXTENSIONS):
        return 'zip'
    if lower.endswith(TAR_EXTENSIONS):
        return 'tar'
    return None


class Scanner:
    def __init__(self, rules, kind):
        self.rules, self.kind = rules, kind
        self.findings = []
        self.members = 0
        self.kit = {}
        self.kit_manifest = None
        self.source_top = None
        self.source_hashes = {}
        self.source_manifest = None
        self.source_context = None
        self.source_fact = None
        self.source_review = load_review(POLICY_PATH.parent, rules.policy) if kind == 'dependency-sources' else None

    def add(self, rule, member, reason):
        if self.source_review and self.source_review.excuses(self.source_fact, rule, member):
            return
        self.findings.append({'rule': rule, 'member': clean_label(member), 'reason': reason})

    # ---- archive level -------------------------------------------------
    def scan(self, fileobj, fmt, prefix, depth, source_context=None):
        previous_context, previous_fact = self.source_context, self.source_fact
        if source_context is not None:
            self.source_context = source_context
        try:
            if fmt == 'zip':
                with zipfile.ZipFile(fileobj) as archive:
                    self.scan_entries(zip_entries(archive), False, prefix, depth)
            else:
                with tarfile.open(fileobj=fileobj, mode='r:*') as archive:
                    self.scan_entries(tar_entries(archive), True, prefix, depth)
        except (zipfile.BadZipFile, tarfile.TarError, EOFError, OSError, RuntimeError, ValueError) as error:
            self.add('unreadable', prefix.rstrip('!') or '(archive)', 'archive could not be read: ' + type(error).__name__)
        finally:
            self.source_context, self.source_fact = previous_context, previous_fact

    def scan_entries(self, entries, is_tar, prefix, depth):
        top = depth == 0
        seen = {}
        names, dirs, links = set(), set(), []
        occurrences = {}
        for entry in entries:
            path, is_dir, problems = normalize(entry.raw, is_tar)
            occurrences[entry.raw] = occurrences.get(entry.raw, 0) + 1
            entry.source_occurrence = occurrences[entry.raw]
            entry.source_fact = None
            if self.source_review:
                entry.source_fact = self.source_review.observe(self.source_context, entry,
                    entry.source_occurrence, prefix + (path or entry.raw), prefix + entry.raw)
            self.source_fact = entry.source_fact
            if is_tar and not path and not problems and entry.is_dir:
                continue  # "./" root entry of "tar -C dir ."
            is_dir = is_dir or entry.is_dir
            self.members += 1
            label = prefix + (path or entry.raw)
            for problem in problems:
                self.add('path-unsafe', prefix + entry.raw, problem)
            if path:
                # An APK is installed, never unpacked onto a case-insensitive file
                # system, and Android's resource shrinker emits names that differ
                # only in case (res/0C.xml, res/0c.xml). Only exact repeats count.
                key = path if (top and self.kind == 'android-apk') else path.lower()
                if key in seen and not (is_dir and seen[key]):
                    self.add('path-unsafe', label, 'duplicate member name')
                seen[key] = is_dir and seen.get(key, True)
            if not path:
                continue
            for i in range(1, len(path.split('/'))):
                dirs.add('/'.join(path.split('/')[:i]))
            if is_dir or entry.link:
                if is_dir:
                    dirs.add(path)
            else:
                names.add(path)
            if entry.link:
                links.append((path, label, entry))
                continue
            if is_dir:
                continue
            if top:
                self.check_layout(path, label)
            self.scan_file(path, label, entry, prefix, depth)
        self.check_links(links, names, dirs, top)

    # ---- links -----------------------------------------------------------
    def check_links(self, links, names, dirs, top):
        link_map = {path: entry.link_target for path, _, entry in links if entry.link == 'symlink'}
        for path, label, entry in links:
            self.source_fact = getattr(entry, 'source_fact', None)
            reason = self.link_problem(path, entry, link_map, names, dirs, top)
            if reason:
                self.add('link', label, reason)

    def link_problem(self, path, entry, link_map, names, dirs, top):
        if top and self.kind == 'linux-appimage':
            if entry.link != 'symlink':
                return entry.link + ' entries are not allowed'
            if not any(p.fullmatch(path) for p in self.rules.layouts[self.kind]):
                return 'symlink outside the approved AppDir layout'
            target = entry.link_target
            if not target or '\\' in target or '\x00' in target or target.startswith('/') or re.match(r'^[A-Za-z]:', target):
                return 'symlink target is not a relative AppDir path'
            resolved = self.resolve(posixpath.dirname(path), target, link_map)
            if resolved is None or resolved not in names | dirs:
                return 'symlink target escapes or is absent from AppDir'
            return None
        if not (top and self.kind == 'macos'):
            return entry.link + ' entries are not allowed'
        if entry.link != 'symlink':
            return entry.link + ' entries are not allowed'
        match = re.match(r'^(suyu\.app/.*?[^/]+\.framework)/', path + '/')
        if not match:
            return 'symlink outside a suyu.app framework directory'
        framework = match[1]
        if path == framework:
            return 'symlink replaces a framework directory'
        target = entry.link_target
        if not target or '\x00' in target or '\\' in target or target.startswith('/') or re.match(r'^[A-Za-z]:', target):
            return 'symlink target is empty or not a relative path'
        resolved = self.resolve(posixpath.dirname(path), target, link_map)
        if resolved is None:
            return 'symlink target cannot be resolved inside the archive'
        if not resolved.startswith(framework + '/') or resolved == framework:
            return 'symlink target leaves its framework directory'
        if resolved not in names and resolved not in dirs:
            return 'symlink target is not a member of the archive'
        return None

    @staticmethod
    def resolve(base, target, link_map):
        pending = [p for p in (base + '/' + target).split('/') if p not in ('', '.')]
        current, hops = [], 0
        while pending:
            part = pending.pop(0)
            if part == '..':
                if not current:
                    return None
                current.pop()
                continue
            current.append(part)
            joined = '/'.join(current)
            if joined in link_map:
                hops += 1
                if hops > 40 or not link_map[joined]:
                    return None
                current.pop()
                pending = [p for p in link_map[joined].split('/') if p not in ('', '.')] + pending
        return '/'.join(current)

    # ---- layout ----------------------------------------------------------
    def check_layout(self, path, label):
        if self.kind in ('source', 'dependency-sources'):
            top_dir = path.split('/')[0]
            if self.source_top is None:
                self.source_top = top_dir
            elif top_dir != self.source_top:
                self.add('unexpected', label, 'more than one top-level directory')
                return
        if not any(p.fullmatch(path) for p in self.rules.layouts.get(self.kind, [])):
            self.add('unexpected', label, 'not part of the expected ' + self.kind + ' release layout')

    # ---- file content ----------------------------------------------------
    def scan_name(self, path, label):
        rules = self.rules
        base = base_name(path)
        if any(p.search(base) for p in rules.key_names):
            self.add('key-name', label, 'key file name')
        if any(p.search(base) for p in rules.firmware_names):
            self.add('firmware', label, 'firmware/NAND file name')
        lower = path.lower()
        if self.kind == 'linux-appimage' and 'user' in lower.split('/'):
            self.add('user-data', label, 'user directory is not part of AppDir')
        if any(p.search(lower) for p in rules.firmware_paths):
            self.add('firmware', label, 'firmware/NAND path')
        if base.endswith(rules.containers):
            self.add('game-container', label, 'game container file extension')
        if base in rules.markers:
            self.add('export-marker', label, 'game export marker file name')
        if any(p.search(lower) for p in rules.user_data):
            self.add('user-data', label, 'user data path')
        debian_patch = self.kind == 'dependency-sources' and bool(re.fullmatch(r'suyu-[^/]+-dependency-sources/[A-Za-z0-9._+-]+\.diff\.gz', path))
        if base.endswith(OPAQUE_EXTENSIONS) and not archive_format(base) and not debian_patch:
            self.add('nested-archive', label, 'opaque archive format cannot be inspected')

    def scan_file(self, path, label, entry, prefix, depth):
        rules = self.rules
        self.scan_name(path, label)
        fmt = archive_format(path)
        kit_rel = None
        if depth == 0 and self.kind == 'windows' and path.startswith(KIT_PREFIX):
            kit_rel = path[len(KIT_PREFIX):]
        source_rel = path.split('/', 1)[1] if depth == 0 and self.kind == 'dependency-sources' and '/' in path else None
        debian_patch = source_rel is not None and source_rel.endswith('.diff.gz')
        size = entry.size
        text_limit = size if size <= WHOLE_FILE_LIMIT else rules.text_limit
        first = max(text_limit, HEAD_BYTES)
        nested_ok = fmt and depth < NESTED_MAX_DEPTH and size <= NESTED_MAX_BYTES
        if fmt and not nested_ok:
            self.add('nested-archive', label, 'nested archive too deep or too large to inspect')
        try:
            handle = entry.opener()
            if handle is None:
                raise OSError('no data')
            with handle:
                data = handle.read(first)
                if source_rel is not None and source_rel.endswith('.dsc'):
                    if size > WHOLE_FILE_LIMIT:
                        raise ValueError('Debian source control file exceeds text limit')
                    control = data.decode('utf-8')
                    if not re.search(r'^Format: .+$', control, re.M) or any(ord(c) < 32 and c not in '\r\n\t' for c in control):
                        raise ValueError('invalid Debian source control text')
                for name, offset, magic in rules.signatures:
                    if data[offset:offset + len(magic)] == magic and not (offset and is_plain_text(data[:offset])):
                        self.add('signature', label, 'content signature: ' + name)
                text = data[:text_limit].decode('latin-1')
                if any(p.search(text) for p in rules.key_text):
                    self.add('key-text', label, 'content matches a key text pattern')
                if source_rel is not None and source_rel.endswith('.asc'):
                    if size > MAX_SIGNATURE_BYTES:
                        raise ValueError('detached signature exceeds bounded size')
                    validate_detached_signature(data)
                digest = hashlib.sha256(data) if kit_rel is not None or source_rel is not None or (self.source_context and fmt) else None
                spool = None
                if nested_ok or debian_patch:
                    spool = tempfile.SpooledTemporaryFile(max_size=SPOOL_BYTES)
                    spool.write(data)
                manifest = bytearray(data) if kit_rel == 'manifest.json' or source_rel == 'MANIFEST.json' else None
                total = len(data)
                if digest is not None or spool is not None:
                    while True:
                        chunk = handle.read(1 << 20)
                        if not chunk:
                            break
                        total += len(chunk)
                        if digest is not None:
                            digest.update(chunk)
                        if manifest is not None and len(manifest) <= MANIFEST_MAX_BYTES:
                            manifest.extend(chunk)
                        if spool is not None:
                            if total > NESTED_MAX_BYTES:
                                spool.close()
                                spool = None
                                self.add('nested-archive', label, 'nested archive too large to inspect')
                                if digest is None:
                                    break
                            else:
                                spool.write(chunk)
        except Exception as error:
            self.add('unreadable', label, 'member could not be read: ' + type(error).__name__)
            return
        if digest is not None:
            if kit_rel is not None:
                self.kit[kit_rel] = digest.hexdigest()
            if source_rel is not None and digest is not None:
                self.source_hashes[source_rel] = digest.hexdigest()
            if source_rel == 'MANIFEST.json' and manifest is not None:
                self.source_manifest = bytes(manifest)
            if kit_rel == 'manifest.json' and manifest is not None:
                self.kit_manifest = bytes(manifest)
        if spool is not None:
            try:
                spool.seek(0)
                if self.source_review and self.source_review.opaque(entry.source_fact):
                    return  # Exact reviewed fixture; raw content scans above still applied.
                if debian_patch:
                    # Rolled-over spools have mode w+b; never let gzip inherit it.
                    with gzip.GzipFile(fileobj=spool, mode='rb') as patch:
                        expanded = patch.read(NESTED_MAX_BYTES + 1)
                    if len(expanded) > NESTED_MAX_BYTES:
                        raise ValueError('Debian patch expands beyond limit')
                    text = expanded.decode('utf-8')
                    if any(ord(c) < 32 and c not in '\r\n\t' for c in text):
                        raise ValueError('Debian patch is not text')
                    if any(p.search(text) for p in rules.key_text):
                        self.add('key-text', label + '!patch', 'patch matches a key text pattern')
                    self.scan_file(path[:-3], label + '!patch', Entry(path[:-3], False, None, None, len(expanded), lambda: io.BytesIO(expanded)), '', depth + 1)
                else:
                    context = None
                    if self.kind == 'dependency-sources' and digest is not None:
                        if depth == 0:
                            context = dict(archive_name=source_rel, archive_sha256=digest.hexdigest(), ancestors=[])
                        elif self.source_context is not None:
                            context = dict(self.source_context, ancestors=self.source_context['ancestors'] +
                                [dict(name=entry.raw, occurrence=entry.source_occurrence, sha256=digest.hexdigest())])
                    self.scan(spool, fmt, label + '!', depth + 1, context)
            except (OSError, ValueError, UnicodeError) as error:
                self.add('unreadable', label, 'nested source could not be read: ' + type(error).__name__)
            finally:
                spool.close()

    # ---- export build kit ------------------------------------------------
    def check_kit(self):
        member = KIT_PREFIX + 'manifest.json'
        if self.kit_manifest is None:
            self.add('kit', member, 'export build kit manifest.json is missing')
            return
        try:
            manifest = json.loads(self.kit_manifest.decode('utf-8'))
            if not isinstance(manifest, dict):
                raise ValueError
        except ValueError:
            self.add('kit', member, 'manifest.json is not a valid JSON object')
            return
        if manifest.get('policy_version') != self.rules.version:
            self.add('kit', member, 'manifest policy_version does not match ' + self.rules.version)
        revision = manifest.get('producer_source_revision')
        if not isinstance(revision, str) or not revision.strip():
            self.add('kit', member, 'manifest producer_source_revision is missing or empty')
        files = manifest.get('files')
        if not isinstance(files, dict) or not files:
            self.add('kit', member, 'manifest files table is missing or empty')
            return
        for rel, digest in sorted(self.kit.items()):
            if rel == 'manifest.json':
                continue
            listed = files.get(rel)
            if listed is None:
                self.add('kit', KIT_PREFIX + rel, 'file is not listed in the kit manifest')
            elif not isinstance(listed, str) or not SHA256_RE.match(listed.lower()) or listed.lower() != digest:
                self.add('kit', KIT_PREFIX + rel, 'sha256 does not match the kit manifest')
        for rel in sorted(files):
            if rel != 'manifest.json' and rel not in self.kit:
                self.add('kit', KIT_PREFIX + rel, 'file listed in the kit manifest is missing')


# Type-2 format: https://github.com/AppImage/AppImageSpec/blob/master/draft.md
# Use the host's SquashFS reader only. Never execute/mount/extract the AppImage.
APPIMAGE_LIST_LIMIT = 16 << 20
APPIMAGE_FILE_LIMIT = 256 << 20
APPIMAGE_TOTAL_LIMIT = 2 << 30
APPIMAGE_ENTRY_LIMIT = 20000


def appimage_offset(path, rules, runtime_hash, runtime_size):
    approved = rules.policy['appimage']
    if runtime_hash != approved['runtime_sha256'] or runtime_size != approved['runtime_size']:
        raise ValueError('runtime pin does not match approved AppImage runtime')
    with open(path, 'rb') as handle:
        prefix = handle.read(runtime_size)
        if (len(prefix) != runtime_size or prefix[:6] != b'\x7fELF\x02\x01' or
                prefix[8:11] != b'AI\x02' or prefix[18:20] != b'\x3e\x00' or
                hashlib.sha256(prefix).hexdigest() != runtime_hash):
            raise ValueError('invalid or unapproved type-2 x86_64 runtime prefix')
        header = handle.read(96)
        if len(header) != 96 or header[:4] != b'hsqs' or struct.unpack_from('<HH', header, 28) != (4, 0):
            raise ValueError('missing SquashFS v4 payload')
        used = struct.unpack_from('<Q', header, 40)[0]
        remaining = path.stat().st_size - runtime_size
        if used < 96 or used > remaining or remaining - used > 4095:
            raise ValueError('truncated or oversized SquashFS payload')
        handle.seek(runtime_size + used)
        if any(handle.read()):
            raise ValueError('unexpected data after SquashFS payload')
    return runtime_size


@contextlib.contextmanager
def squashfs_output(tool, image, offset, args, limit):
    # POSIX rlimits bound reader output/memory/CPU, including malicious payloads.
    import resource
    def bounded_child():
        resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit))
        resource.setrlimit(resource.RLIMIT_AS, (2 << 30, 2 << 30))
        resource.setrlimit(resource.RLIMIT_CPU, (60, 60))
    with tempfile.TemporaryFile() as output:
        env = {'PATH': '/usr/bin:/bin', 'LC_ALL': 'C', 'TZ': 'UTC'}
        reader_args = [*args[:-1], str(image), args[-1]] if '-cat' in args else [*args, str(image)]
        result = subprocess.run([str(tool), '-processors', '1', '-o', str(offset), *reader_args],
                                stdout=output, stderr=subprocess.DEVNULL, timeout=90,
                                env=env, preexec_fn=bounded_child)
        if result.returncode or output.tell() > limit:
            raise ValueError('SquashFS reader rejected payload or exceeded limits')
        output.seek(0)
        yield output


def squashfs_listing(data):
    entries = []
    for line in data.decode('utf-8', errors='strict').splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r'([dl-][rwxstST-]{9})\s+\d+/\d+\s+(\d+)\s+\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}\s+(squashfs-root(?:/.*)?)', line)
        if not match:
            raise ValueError('unrecognized or special SquashFS listing member')
        mode, size, name = match.groups()
        target = None
        if mode[0] == 'l':
            if name.count(' -> ') != 1:
                raise ValueError('ambiguous SquashFS symlink listing')
            name, target = name.split(' -> ')
        elif ' -> ' in name:
            raise ValueError('ambiguous SquashFS member name')
        name = name.removeprefix('squashfs-root').removeprefix('/')
        if not name and mode[0] == 'd':
            continue
        if not name or any(c in name for c in '\r\n\x00'):
            raise ValueError('invalid SquashFS member path')
        entries.append((name, mode, int(size), target))
    if len(entries) > APPIMAGE_ENTRY_LIMIT:
        raise ValueError('too many AppDir members')
    if sum(size for _, mode, size, _ in entries if mode[0] == '-') > APPIMAGE_TOTAL_LIMIT:
        raise ValueError('AppDir expanded size exceeds limit')
    return entries


def scan_appimage(scanner, path, runtime_hash, runtime_size, source_revision):
    try:
        if path.stat().st_size > APPIMAGE_TOTAL_LIMIT:
            raise ValueError('compressed AppImage exceeds size limit')
        # Scan a private immutable-by-path snapshot; never follow a replaced input.
        with tempfile.TemporaryDirectory(prefix='suyu-appimage-scan-') as folder:
            snapshot = Path(folder) / 'payload.AppImage'
            with open(path, 'rb') as original, open(snapshot, 'wb') as copied:
                total = 0
                while chunk := original.read(1 << 20):
                    total += len(chunk)
                    if total > APPIMAGE_TOTAL_LIMIT:
                        raise ValueError('compressed AppImage exceeds size limit')
                    copied.write(chunk)
            offset = appimage_offset(snapshot, scanner.rules, runtime_hash, runtime_size)
            repository = scanner.appimage_pins.get('repository', '')
            if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*', repository):
                raise ValueError('expected repository slug is required')
            if not source_revision or not re.fullmatch(r'[0-9a-f]{40}', source_revision):
                raise ValueError('expected source revision is required')
            if not sys.platform.startswith('linux'):
                raise ValueError('AppImage inspection requires Linux SquashFS tooling')
            # Trust a root-owned system executable; no PATH-supplied/embedded reader.
            tool = next((Path(p) for p in ('/usr/bin/unsquashfs', '/bin/unsquashfs') if Path(p).is_file()), None)
            if tool is None or tool.resolve().stat().st_uid != 0 or tool.resolve().stat().st_mode & 0o022:
                raise ValueError('trusted system unsquashfs is unavailable')
            with squashfs_output(tool, snapshot, offset, ['-lln'], APPIMAGE_LIST_LIMIT) as listing:
                members = squashfs_listing(listing.read())
            return scan_appdir(scanner, members, lambda name, size: squashfs_output(
                tool, snapshot, offset, ['-cat', '-no-wildcards', name], min(size + 1, APPIMAGE_FILE_LIMIT)))
    except (OSError, ValueError, UnicodeError, subprocess.TimeoutExpired) as error:
        scanner.add('appimage', path.name, 'AppImage inspection failed: ' + type(error).__name__)


def appimage_runtime_lock(rules):
    folder = POLICY_PATH.parent.parent / 'appimage'
    raw = (folder / 'runtime.lock.json').read_bytes()
    lock = json.loads(raw)
    approved = rules.policy['appimage']
    canonical = hashlib.sha256(json.dumps(lock, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    if (canonical != approved['runtime_lock_canonical_sha256'] or
            lock.get('schema') != 'suyu-appimage-runtime-v1' or
            lock.get('sha256') != approved['runtime_sha256'] or lock.get('size') != approved['runtime_size'] or
            lock.get('recipe_sha256') != approved['runtime_recipe_sha256'] or
            hashlib.sha256((folder / 'runtime-build.sh').read_bytes()).hexdigest() != lock['recipe_sha256']):
        raise ValueError('runtime lock or source recipe differs from approved policy')
    return lock, hashlib.sha256(raw).hexdigest()


def check_appimage_sources(scanner, document, lock, lock_hash, files):
    member = 'usr/share/suyu/distribution-sources.json'
    if not isinstance(document, dict) or document.get('schema') != 'suyu-appimage-sources-v1':
        scanner.add('source-provenance', member, 'source manifest missing or invalid')
        return
    if document.get('runtime_complete') is not True or document.get('runtime_sha256') != lock['sha256']:
        scanner.add('source-provenance', member, 'complete pinned runtime source identity missing')
        return
    try:
        delivery = load_delivery_lock(POLICY_PATH.parent.parent / 'appimage', lock, scanner.rules.policy)
        verify_distribution_records(document, delivery)
    except (KeyError, ValueError):
        scanner.add('source-provenance', member, 'source recipe distribution differs from reviewed delivery lock')
        return
    receipt = document.get('runtime_build_receipt')
    if not isinstance(receipt, dict) or any(receipt.get(k) != v for k, v in lock.items()):
        scanner.add('source-provenance', member, 'runtime receipt does not match reviewed source closure')
        return
    if (receipt.get('lock_sha256') != lock_hash or receipt.get('recipe_sha256') != lock['recipe_sha256'] or
            any(not SHA256_RE.fullmatch(str(receipt.get(k, ''))) for k in ('linker_map_sha256', 'installed_package_db_sha256'))):
        scanner.add('source-provenance', member, 'runtime recipe/build evidence is not hash-linked')
    archives = ['/usr/lib/libc.a', '/usr/lib/libfuse3.a', '/usr/lib/libmimalloc.a', '/usr/lib/libz.a',
                '/usr/lib/libzstd.a', '/usr/local/lib/libsquashfuse.a', '/usr/local/lib/libsquashfuse_ll.a']
    if receipt.get('contributed_archives') != archives or document.get('runtime_components') != lock['components']:
        scanner.add('source-provenance', member, 'static archive/source closure mismatch')
    sources = document.get('sources')
    if not isinstance(sources, list) or any(not isinstance(s, dict) for s in sources):
        scanner.add('source-provenance', member, 'source records missing')
        return
    by_name = {s.get('name'): s for s in sources}
    if len(by_name) != len(sources) or any(by_name.get(s['name']) != s for s in lock['sources']):
        scanner.add('source-provenance', member, 'exact runtime sources missing or changed')
    libraries = document.get('libraries', {})
    if not isinstance(libraries, dict):
        scanner.add('source-provenance', member, 'library source mappings missing')
        return
    for name, digest in files.items():
        if name.startswith(('usr/lib/', 'usr/plugins/')) and '.so' in name:
            record = libraries.get(name)
            if (not isinstance(record, dict) or record.get('sha256') != digest or
                    not record.get('source_version') or not record.get('copyright') or
                    not isinstance(record.get('sources'), list) or not record['sources'] or
                    any(s not in by_name for s in record['sources'])):
                scanner.add('source-provenance', name, 'bundled library has no exact corresponding-source mapping')


def check_source_distribution_archive(scanner):
    try:
        lock, lock_hash = appimage_runtime_lock(scanner.rules)
        expected_appimage = (getattr(scanner, 'appimage_source_bundle', False) or
                             bool(set(scanner.source_hashes) & {row['name'] for row in lock['sources']}))
        if scanner.source_manifest is None and not expected_appimage:
            return
        document = json.loads(scanner.source_manifest)
        if not isinstance(document, dict):
            raise ValueError('source manifest must be an object')
        if document.get('schema') != 'suyu-appimage-sources-v1':
            if expected_appimage:
                raise ValueError('AppImage source schema cannot be downgraded')
            return  # Clearly unrelated project collectors have different schemas.
        folder = POLICY_PATH.parent.parent / 'appimage'
        delivery = load_delivery_lock(folder, lock, scanner.rules.policy)
        verify_distribution_records(document, delivery)
        check_appimage_sources(scanner, document, lock, lock_hash, {})
        records = {row['name']: row for row in document['sources']}
        if len(records) != len(document['sources']):
            raise ValueError('duplicate source input identity')
        if any(records.get(row['name']) != row for row in lock['sources']):
            raise ValueError('immutable runtime upstream inputs differ')
        if set(scanner.source_hashes) != set(records) | {'MANIFEST.json', 'README.txt'}:
            raise ValueError('physical source distribution inventory differs')
        for name, record in records.items():
            projection = delivery['projections'].get(name)
            expected = projection['output_sha256'] if projection else record['sha256']
            if scanner.source_hashes.get(name) != expected:
                raise ValueError('source distribution body does not match declared exact hash')
    except (TypeError, ValueError, KeyError):
        scanner.add('source-provenance', 'MANIFEST.json', 'exact source distribution/recipe linkage invalid')


def scan_appdir(scanner, members, read_member):
    approved = scanner.rules.policy['appimage']
    provenance_path = approved['provenance_path']
    files, links, provenance, source_document = {}, {}, None, None
    def inspected_entries():
        nonlocal provenance, source_document
        for name, mode, size, target in members:
            if name in ('AppRun', 'usr/bin/suyu', 'usr/bin/suyu-cmd') and (mode[0] != '-' or 'x' not in mode[1:]):
                scanner.add('appimage', name, 'required executable is not an executable regular file')
            normal, _, problems = normalize(name, False)
            if problems or normal != name:
                scanner.add('path-unsafe', name, 'unsafe AppDir member path')
                continue # never pass hostile paths to a filesystem reader
            if mode[0] == 'd':
                scanner.scan_name(name, name)
                if not any(re.fullmatch(pattern, name) for pattern in approved['approved_directories']):
                    scanner.add('unexpected', name, 'unexpected AppDir directory')
            elif mode[0] == 'l':
                scanner.check_layout(name, name)
                scanner.scan_name(name, name)
                links[name] = target
            else:
                if size > APPIMAGE_FILE_LIMIT:
                    scanner.add('appimage', name, 'expanded member exceeds limit')
                    continue
                with read_member(name, size) as handle:
                    # Content scanning uses the exact same entries/nested-archive rules.
                    data = handle.read(size + 1)
                if len(data) != size:
                    raise ValueError('SquashFS member size mismatch')
                import io
                disguised = ('zip' if data.startswith(b'PK\x03\x04') else
                             'tar' if data.startswith((b'\x1f\x8b', b'\xfd7zXZ\x00')) else None)
                if disguised and archive_format(name) is None:
                    scanner.add('nested-archive', name, 'archive payload disguised as an AppDir file')
                    scanner.scan(io.BytesIO(data), disguised, name + '!', 1)
                elif data.startswith((b'7z\xbc\xaf\x27\x1c', b'Rar!')):
                    scanner.add('nested-archive', name, 'opaque archive payload cannot be inspected')
                if name == 'usr/share/suyu/distribution-sources.json':
                    if size > MANIFEST_MAX_BYTES:
                        raise ValueError('source provenance too large')
                    source_document = json.loads(data)
                if name == provenance_path:
                    if size > MANIFEST_MAX_BYTES:
                        raise ValueError('AppImage provenance too large')
                    provenance = json.loads(data)
                else:
                    files[name] = hashlib.sha256(data).hexdigest()
                import io
                yield Entry(name, False, None, None, size, lambda data=data: io.BytesIO(data))
                continue
            yield Entry(name, mode[0] == 'd', 'symlink' if mode[0] == 'l' else None, target, size, None)
    scanner.scan_entries(inspected_entries(), False, '', 0)
    if not isinstance(provenance, dict):
        scanner.add('appimage', provenance_path, 'embedded provenance is missing or invalid')
        return
    lock, lock_hash = appimage_runtime_lock(scanner.rules)
    check_appimage_sources(scanner, source_document, lock, lock_hash, files)
    pins = getattr(scanner, 'appimage_pins', {})
    for key, expected in {'schema': approved['schema'], 'policy_version': scanner.rules.version, 'runtime_recipe_sha256': lock['recipe_sha256'], 'runtime_lock_sha256': lock_hash, **{k:v for k,v in pins.items() if k != 'repository'}}.items():
        if provenance.get(key) != expected:
            scanner.add('appimage', provenance_path, 'provenance mismatch: ' + key)
    if provenance.get('files') != files or provenance.get('links') != links:
        scanner.add('appimage', provenance_path, 'embedded inventory does not match inspected final payload')
    for required in approved['required_files']:
        if required not in files:
            scanner.add('appimage', required, 'required regular AppDir file missing')
    for required in ('.DirIcon', 'suyu.desktop', 'suyu.svg'):
        if required not in files and required not in links:
            scanner.add('appimage', required, 'AppImage desktop integration member missing')
    if not any(name.startswith('usr/share/doc/suyu/LICENSES/') for name in files):
        scanner.add('appimage', 'usr/share/doc/suyu/LICENSES', 'license texts missing')
    if not any(name.startswith('usr/share/doc/suyu/packaging-tool-licenses/') for name in files):
        scanner.add('appimage', 'usr/share/doc/suyu/packaging-tool-licenses', 'packaging tool licenses missing')
    source_url = provenance.get('source_url', '')
    expected_url = 'https://github.com/' + pins.get('repository', '') + '/tree/' + pins.get('source_revision', '')
    if source_url != expected_url:
        scanner.add('appimage', provenance_path, 'source URL does not identify expected source revision')
    if provenance.get('runtime_source_url') != lock['source_url']:
        scanner.add('appimage', provenance_path, 'official runtime source provenance missing')


def scan_archive(path, kind, rules, appimage_pins=None):
    scanner = Scanner(rules, kind)
    path = Path(path)
    scanner.appimage_source_bundle = path.name.endswith('-appimage-sources.tar.gz')
    fmt = archive_format(path.name)
    if kind == 'linux-appimage':
        scanner.appimage_pins = appimage_pins or {}
        scan_appimage(scanner, path, scanner.appimage_pins.get('runtime_sha256'),
                      scanner.appimage_pins.get('runtime_size'), scanner.appimage_pins.get('source_revision'))
    elif fmt is None:
        scanner.add('unreadable', path.name, 'unsupported archive type')
    else:
        with open(path, 'rb') as handle:
            scanner.scan(handle, fmt, '', 0)
        if kind == 'windows':
            scanner.check_kit()
    if kind == 'dependency-sources' and any(n.endswith(('.dsc', '.diff.gz', '.asc')) for n in scanner.source_hashes):
        try:
            manifest = json.loads(scanner.source_manifest)
            claimed = {item['name']: item['sha256'] for item in manifest['sources']}
            for name, digest in scanner.source_hashes.items():
                if name.endswith(('.dsc', '.diff.gz', '.asc')) and claimed.get(name) != digest:
                    scanner.add('source-provenance', name, 'Debian source is not hash-linked to MANIFEST.json')
                if name.endswith('.asc'):
                    item = next((item for item in manifest['sources'] if item['name'] == name), {})
                    if item.get('authentication') != 'apt signed Sources index':
                        scanner.add('source-provenance', name, 'signature lacks authenticated APT source linkage')
        except (TypeError, ValueError, KeyError):
            scanner.add('source-provenance', 'MANIFEST.json', 'Debian source manifest missing or invalid')
    if kind == 'dependency-sources':
        check_source_distribution_archive(scanner)
    findings = [f for f in scanner.findings if not rules.excused(kind, f['rule'], f['member'])]
    findings.sort(key=lambda f: (f['member'], f['rule'], f['reason']))
    return {'path': path.name, 'kind': kind, 'members_scanned': scanner.members, 'findings': findings}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--kind', required=True, choices=KINDS)
    parser.add_argument('--appimage-runtime-sha256')
    parser.add_argument('--appimage-runtime-size', type=int)
    parser.add_argument('--appimage-source-revision')
    parser.add_argument('--appimage-repository')
    parser.add_argument('--report', help='write a JSON report (archive base names only)')
    parser.add_argument('--policy', default=str(POLICY_PATH), help=argparse.SUPPRESS)
    parser.add_argument('archives', nargs='+', metavar='ARCHIVE')
    args = parser.parse_args(argv)
    sys.stdout.reconfigure(errors='backslashreplace')
    for archive in args.archives:
        if not Path(archive).is_file():
            parser.error('not a file: ' + archive)
    rules = Rules(load_policy(args.policy))
    pins = {'runtime_sha256': args.appimage_runtime_sha256, 'runtime_size': args.appimage_runtime_size,
            'source_revision': args.appimage_source_revision, 'repository': args.appimage_repository}
    results = [scan_archive(a, args.kind, rules, pins) for a in args.archives]
    if args.report:
        Path(args.report).write_text(json.dumps(
            {'policy_version': rules.version, 'archives': results}, indent=2) + '\n', encoding='utf-8')
    failed = False
    for result in results:
        count = len(result['findings'])
        print(f"{result['path']} [{result['kind']}]: {result['members_scanned']} members, "
              f"{count} finding(s)")
        for finding in result['findings']:
            print(f"  {finding['rule']}: {finding['member']}: {finding['reason']}")
        failed = failed or bool(count)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
