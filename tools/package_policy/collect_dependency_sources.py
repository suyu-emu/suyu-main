#!/usr/bin/env python3
"""Collect the corresponding source of the third-party dependencies CMake's CPM fetches (stdlib only).

CMake downloads these at configure time, so they are not part of the git source
archive. This tool reads the three cpmfile.json files, works out the exact URL
CPMUtil.cmake would use for every dependency, downloads it, verifies the JSON
SHA-512 where that hash applies to the downloaded file, and packs everything with
a manifest into one uncompressed tar.

The hashes in the manifest identify files. They do not grant permission to use
or redistribute anything; each component stays under its own license.

Use --dry-run to print the plan as JSON without touching the network.
Exit status: 0 success, 1 download or verification failure, 2 usage error.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import tarfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
CPMFILES = (
    ('cpmfile.json', 'cpmfile.json'),
    ('src/dynarmic/externals/cpmfile.json', 'src/dynarmic/externals/cpmfile.json'),
    ('src/qt_common/externals/cpmfile.json', 'src/qt_common/externals/cpmfile.json'),
)
DEFAULT_HOST = 'github.com'
# Downloads are limited to the hosts the cpmfiles use for public source.
ALLOWED_HOSTS = ('github.com', 'git.eden-emu.dev')
DOWNLOAD_ATTEMPTS = 4
TIMEOUT_SECONDS = 120
USER_AGENT = 'suyu-dependency-sources/1'

# Dependencies that are deliberately left out. Each was checked against how CMake
# uses it; anything not listed here is included (when unsure, include).
EXCLUDED = {
    'llvm-mingw': (
        'clang-rt builtins archive linked only when building with clang-cl '
        '(CMakeLists.txt: if (MSVC AND CXX_CLANG)); the release Windows jobs use MSVC cl, '
        'so it is not linked into or shipped with any release binary'),
}

# ci: true entries are PREBUILT binaries from a CI repo whose version is
# "<upstream version>-<upstream commit>" (sirit: just the version). AddCIPackage
# downloads releases/download/v<version>/... from the CI repo, so the CI repo's tag
# v<version> holds the build scripts. The source the prebuilt was built from is
# named here explicitly: name -> (upstream repo, git host, ref template). The
# template may use {version} (whole cpmfile version) and {commit} (text after the
# first '-' in it). A ci entry that is not in this table stops the tool.
CI_UPSTREAM = {
    # crueter-ci/FFmpeg builds FFmpeg/FFmpeg at the commit in the version string.
    'ffmpeg-ci': ('FFmpeg/FFmpeg', DEFAULT_HOST, '{commit}'),
    # crueter-ci/OpenSSL builds openssl/openssl at the commit in the version string.
    'openssl-ci': ('openssl/openssl', DEFAULT_HOST, '{commit}'),
    # crueter-ci/SDL3 builds libsdl-org/SDL at the commit in the version string.
    'sdl3-ci': ('libsdl-org/SDL', DEFAULT_HOST, '{commit}'),
    # eden-emulator/sirit publishes its own prebuilt; source is the same repo's tag v<version>.
    'sirit-ci': ('eden-emulator/sirit', DEFAULT_HOST, 'v{version}'),
}

# Artifact entries whose pinned release artifact is itself a full source bundle
# (git archives of the boost superproject would omit its submodules). For these the
# artifact is downloaded and its JSON hash verified.
ARTIFACT_IS_SOURCE = {'boost'}

SAFE_RE = re.compile(r'[^A-Za-z0-9._+-]+')


def safe(text):
    return SAFE_RE.sub('_', text).strip('._') or 'x'


def load_cpmfiles(root=ROOT):
    for rel, label in CPMFILES:
        yield label, json.loads((Path(root) / rel).read_text(encoding='utf-8'))


def source_ref(entry):
    """The git ref a cpmfile entry pins (mirrors CPMUtil for the root file)."""
    if 'sha' in entry:  # dynarmic / qt_common entries pin a commit
        return entry['sha']
    version = entry['version']
    if 'tag' in entry:
        return entry['tag'].replace('%VERSION%', entry.get('git_version', version))
    numeric = entry.get('numeric_version')
    if numeric:
        version = version.replace('%NUMERIC_VERSION%', numeric)
    return version


def artifact_name(entry, ref):
    artifact = entry.get('artifact')
    if not artifact:
        return None
    numeric = entry.get('numeric_version')
    if numeric:
        artifact = artifact.replace('%NUMERIC_VERSION%', numeric)
    return artifact.replace('%VERSION%', ref)


def archive_url(host, repo, ref):
    return 'https://%s/%s/archive/%s.tar.gz' % (host, repo, ref)


def item(name, repo, host, version, url, kind, role, origin, sha512=None, note=None, extra=None):
    result = {
        'name': name, 'names': [name], 'repo': repo, 'git_host': host, 'version': version,
        'url': url, 'sha512': sha512, 'kind': kind, 'roles': [role], 'origin': [origin],
        'license_hint': None, 'note': note,
    }
    result.update(extra or {})
    return result


def build_plan(root=ROOT):
    """Return {'items': [...], 'excluded': [...]} without any network access."""
    items, excluded = [], []
    for origin, data in load_cpmfiles(root):
        for name, entry in data.items():
            if name in EXCLUDED:
                excluded.append({'name': name, 'origin': origin, 'reason': EXCLUDED[name]})
                continue
            host = entry.get('git_host', DEFAULT_HOST)
            repo = entry['repo']
            ref = source_ref(entry)
            if entry.get('ci'):
                if name not in CI_UPSTREAM:
                    raise SystemExit('collect_dependency_sources: ci entry %r has no upstream mapping' % name)
                version = entry['version']
                items.append(item(
                    name, repo, host, version, archive_url(host, repo, 'v' + version), 'ci-prebuilt',
                    'build-scripts', origin,
                    note='CI repository tag v%s (build scripts for the prebuilt package)' % version))
                up_repo, up_host, template = CI_UPSTREAM[name]
                up_ref = template.format(version=version, commit=version.split('-', 1)[-1])
                items.append(item(
                    name + '-upstream', up_repo, up_host, up_ref, archive_url(up_host, up_repo, up_ref),
                    'ci-prebuilt', 'upstream-source', origin,
                    note='upstream source the prebuilt %s was built from' % name))
                continue
            artifact = artifact_name(entry, ref)
            if artifact is None:
                items.append(item(name, repo, host, ref, archive_url(host, repo, ref), 'source', 'source',
                                  origin, sha512=entry.get('hash')))
            elif name in ARTIFACT_IS_SOURCE:
                url = 'https://%s/%s/releases/download/%s/%s' % (host, repo, ref, artifact)
                items.append(item(name, repo, host, ref, url, 'artifact-source', 'source-artifact', origin,
                                  sha512=entry.get('hash'),
                                  note='pinned release artifact, which is itself a source bundle'))
            else:
                # The pinned artifact is a prebuilt binary; ship the source at the same version.
                items.append(item(
                    name, repo, host, ref, archive_url(host, repo, ref), 'artifact-source', 'source', origin,
                    note='CMake fetches the prebuilt %s; this is the repository source at %s' % (artifact, ref),
                    extra={'artifact': artifact, 'artifact_sha512': entry.get('hash')}))
    return {'items': merge_duplicates(items), 'excluded': excluded}


def merge_duplicates(items):
    """One download per URL; later entries with the same URL are folded into the first."""
    merged, by_url = [], {}
    for entry in items:
        first = by_url.get(entry['url'])
        if first is None:
            by_url[entry['url']] = entry
            merged.append(entry)
            continue
        first['names'] = sorted(set(first['names'] + entry['names']))
        first['roles'] = sorted(set(first['roles'] + entry['roles']))
        first['origin'] = sorted(set(first['origin'] + entry['origin']))
        if first['sha512'] is None:
            first['sha512'] = entry['sha512']
        elif entry['sha512'] and entry['sha512'] != first['sha512']:
            raise SystemExit('collect_dependency_sources: conflicting hashes for %s' % entry['url'])
    for entry in merged:
        repo = entry['repo'].replace('/', '-')
        if '/archive/' in entry['url']:
            entry['file'] = safe('%s-%s' % (repo, entry['version'])) + '.tar.gz'
        else:
            entry['file'] = safe('%s-%s' % (repo, entry['url'].rsplit('/', 1)[-1]))
    return merged


def download(entry, destination):
    """Fetch entry['url'] to destination; return (size, sha256, sha512). Retries transient errors."""
    host = re.match(r'^https://([^/]+)/', entry['url'])
    if not host or host[1] not in ALLOWED_HOSTS:
        raise RuntimeError('refusing to download from a host that is not on the allow list: ' + entry['url'])
    last = None
    for attempt in range(1, DOWNLOAD_ATTEMPTS + 1):
        sha256, sha512, size = hashlib.sha256(), hashlib.sha512(), 0
        try:
            request = urllib.request.Request(entry['url'], headers={'User-Agent': USER_AGENT})
            with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response, \
                    open(destination, 'wb') as out:
                while True:
                    chunk = response.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
                    sha256.update(chunk)
                    sha512.update(chunk)
                    size += len(chunk)
            if size == 0:
                raise RuntimeError('empty download')
            return size, sha256.hexdigest(), sha512.hexdigest()
        except (urllib.error.URLError, OSError, RuntimeError) as error:
            last = error
            code = getattr(error, 'code', None)
            if code in (400, 401, 403, 404, 410):
                break  # not transient
            print('  attempt %d/%d failed: %s' % (attempt, DOWNLOAD_ATTEMPTS, error), file=sys.stderr)
            time.sleep(2 * attempt)
    Path(destination).unlink(missing_ok=True)
    raise RuntimeError('download failed for %s: %s' % (entry['url'], last))


README = """{name}

This archive holds the source of third-party components that suyu's build fetches
at configure time (through CMake's CPM, see cpmfile.json and
CMakeModules/CPMUtil.cmake). They are not part of suyu's own source archive, so
they are published here.

- suyu's own source is the separate suyu-<tag>-source archive of the same release.
- MANIFEST.json lists every file: repository, version, download URL, size, SHA-256
  and SHA-512, and whether the SHA-512 pinned in cpmfile.json was verified.
- Files ending in -upstream, or of kind ci-prebuilt, are the build scripts and the
  upstream source behind prebuilt packages that CMake downloads for some platforms.
- The hashes identify files. They do not grant permission to use or redistribute
  anything: each component remains under its own license, which is found in its
  own source tree. See also THIRD-PARTY-NOTICES.txt in the release packages.
"""


def collect(plan, output, name, only=None):
    package = Path(output) / name
    package.mkdir(parents=True, exist_ok=True)
    selected = [e for e in plan['items'] if not only or only & set(e['names'])]
    if only and not selected:
        raise SystemExit('collect_dependency_sources: --only matched nothing')
    records, failed = [], False
    for entry in selected:
        target = package / entry['file']
        print('%s: %s' % (','.join(entry['names']), entry['url']))
        record = dict(entry)
        try:
            size, sha256, sha512 = download(entry, target)
        except RuntimeError as error:
            print('  FAILED: %s' % error, file=sys.stderr)
            failed = True
            continue
        expected = entry['sha512']
        record.update(size=size, sha256=sha256, sha512=sha512, expected_sha512=expected,
                      verified=bool(expected) and expected == sha512)
        if expected and expected != sha512:
            print('  HASH MISMATCH: expected %s got %s' % (expected, sha512), file=sys.stderr)
            target.unlink(missing_ok=True)
            failed = True
            continue
        records.append(record)
    if failed:
        raise RuntimeError('one or more dependency sources failed; nothing was packed')
    manifest = {'name': name, 'items': records, 'excluded': plan['excluded']}
    (package / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (package / 'README.txt').write_text(README.format(name=name), encoding='utf-8', newline='\n')
    tar_path = Path(output) / (name + '.tar')

    def reset(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ''
        info.mtime = 0
        return info

    with tarfile.open(tar_path, 'w') as archive:
        archive.add(package, arcname=name, recursive=False, filter=reset)
        for path in sorted(package.iterdir()):
            archive.add(path, arcname=name + '/' + path.name, filter=reset)
    return tar_path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--output', help='directory for the package folder and the .tar')
    parser.add_argument('--name', help='package name, e.g. suyu-v1.2.3-dependency-sources')
    parser.add_argument('--dry-run', action='store_true', help='print the plan as JSON and exit')
    parser.add_argument('--only', help='comma-separated dependency names to collect')
    args = parser.parse_args(argv)
    only = {n for n in (args.only or '').split(',') if n} or None
    plan = build_plan()
    if args.dry_run:
        if only:
            plan['items'] = [e for e in plan['items'] if only & set(e['names'])]
        json.dump(plan, sys.stdout, indent=2)
        print()
        return 0
    if not args.output or not args.name:
        parser.error('--output and --name are required unless --dry-run is given')
    if not re.fullmatch(r'suyu-[A-Za-z0-9._-]+-dependency-sources', args.name):
        parser.error('--name must look like suyu-<ref>-dependency-sources')
    try:
        tar_path = collect(plan, args.output, args.name, only)
    except RuntimeError as error:
        print('collect_dependency_sources: %s' % error, file=sys.stderr)
        return 1
    print(tar_path)
    return 0


if __name__ == '__main__':
    sys.exit(main())
