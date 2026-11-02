#!/usr/bin/env python3
"""Generate THIRD-PARTY-NOTICES.txt for release packages (stdlib only).

Lists every third-party dependency CMake's CPM fetches (from the cpmfile.json
files, through collect_dependency_sources.py) and the git submodules from
.gitmodules, with the license names that can be mapped to texts under LICENSES/.
The listing is a pointer for readers; it is not a substitute for the license
files that ship in each component's own source, which are in the release's
dependency-sources asset.

Modes:
  --output FILE       write the notices file
  --package-dir DIR   create DIR if needed, then write DIR/THIRD-PARTY-NOTICES.txt and
                      copy LICENSES/ and LICENSE.txt into it
"""
import argparse
from pathlib import Path
import re
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import collect_dependency_sources as collector  # noqa: E402

ROOT = collector.ROOT

# SPDX identifiers as declared by the upstream projects, for the components where
# this is well established. Only used to name files under LICENSES/; a component
# missing here is listed as "see the dependency-sources asset". Informational.
LICENSE_HINTS = {
    'biscuit': ['MIT'], 'boost': ['BSL-1.0'], 'boost_headers': ['BSL-1.0'], 'catch2': ['BSL-1.0'],
    'cpp-jwt': ['MIT'], 'discord-rpc': ['MIT'], 'enet': ['MIT'], 'fmt': ['MIT'],
    'frozen': ['Apache-2.0'], 'httplib': ['MIT'], 'libadrenotools': ['BSD-2-Clause'],
    'lz4': ['BSD-2-Clause'], 'mcl': ['MIT'], 'moltenvk': ['Apache-2.0'], 'nlohmann': ['MIT'],
    'oaknut': ['MIT'], 'oboe': ['Apache-2.0'], 'openssl': ['Apache-2.0'], 'openssl-cmake': ['MIT'],
    'opus': ['BSD-3-Clause'], 'sdl3': ['Zlib'], 'simpleini': ['MIT'], 'spirv-headers': ['MIT'],
    'unordered-dense': ['MIT'], 'vulkan-headers': ['Apache-2.0'],
    'vulkan-memory-allocator': ['MIT'], 'vulkan-utility-libraries': ['Apache-2.0'],
    'vulkan-validation-layers': ['Apache-2.0'], 'xbyak': ['BSD-3-Clause'], 'zlib': ['Zlib'],
    'zstd': ['BSD-3-Clause'],
}
FALLBACK = 'see the dependency-sources asset'

HEADER = """THIRD-PARTY NOTICES

suyu is distributed under its own license, see LICENSE.txt. It is built with third-party
components that keep their own licenses. This file lists them so the notices travel
with every package.

- The corresponding source of the components CMake downloads at build time, with a
  MANIFEST.json of exact versions and hashes, is published as the release asset
  suyu-<tag>-dependency-sources.tar. Each component's own license file is in there.
- suyu's own source, including its git submodules, is the release asset
  suyu-<tag>-source.tar.gz.
- License texts that suyu's repository itself uses are in the LICENSES/ folder of
  this package. License names below are those declared by the upstream projects and
  are informational; the upstream source is authoritative.
- Which components are linked into a given package depends on the platform and build
  options; this list covers everything the build can fetch.
"""


def license_text(name):
    ids = LICENSE_HINTS.get(name)
    if not ids:
        return FALLBACK
    parts = []
    for spdx in ids:
        present = (ROOT / 'LICENSES' / (spdx + '.txt')).is_file()
        parts.append('%s (LICENSES/%s.txt)' % (spdx, spdx) if present else '%s (%s)' % (spdx, FALLBACK))
    return ', '.join(parts)


def read_submodules(root=ROOT):
    text = (Path(root) / '.gitmodules').read_text(encoding='utf-8')
    modules = []
    for block in re.split(r'(?m)^(?=\[submodule )', text):
        name = re.match(r'\[submodule "([^"]+)"\]', block)
        if not name:
            continue
        path = re.search(r'(?m)^\s*path\s*=\s*(\S+)', block)
        url = re.search(r'(?m)^\s*url\s*=\s*(\S+)', block)
        modules.append((name[1], path[1] if path else '', url[1] if url else ''))
    return modules


def generate(root=ROOT):
    plan = collector.build_plan(root)
    lines = [HEADER, 'DEPENDENCIES FETCHED BY THE BUILD', '']
    for entry in sorted(plan['items'], key=lambda e: (e['names'][0].lower(), e['url'])):
        if entry['kind'] == 'ci-prebuilt':
            continue  # covered by the source entry of the same upstream, or listed below
        name = entry['names'][0]
        lines += [name,
                  '  repository: https://%s/%s' % (entry['git_host'], entry['repo']),
                  '  version:    %s' % entry['version'],
                  '  license:    %s' % license_text(name), '']
    ci = [e for e in plan['items'] if e['kind'] == 'ci-prebuilt']
    if ci:
        lines += ['PREBUILT PACKAGES USED ON SOME PLATFORMS', '']
        for entry in sorted(ci, key=lambda e: e['names'][0].lower()):
            lines += [entry['names'][0],
                      '  repository: https://%s/%s' % (entry['git_host'], entry['repo']),
                      '  version:    %s' % entry['version'],
                      '  license:    %s' % license_text(entry['names'][0].replace('-upstream', '').replace('-ci', '')),
                      '  role:       %s' % ', '.join(entry['roles']), '']
    lines += ['GIT SUBMODULES OF THE SUYU SOURCE TREE', '',
              '(their source is part of the source asset)', '']
    for name, path, url in read_submodules(root):
        lines += ['%s' % name, '  path: %s' % path, '  url:  %s' % url, '']
    return '\n'.join(lines).rstrip('\n') + '\n'


def write_package(directory, root=ROOT):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'THIRD-PARTY-NOTICES.txt').write_text(generate(root), encoding='utf-8', newline='\n')
    shutil.copy2(Path(root) / 'LICENSE.txt', directory / 'LICENSE.txt')
    target = directory / 'LICENSES'
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(Path(root) / 'LICENSES', target)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--output', help='write THIRD-PARTY-NOTICES.txt to this file')
    parser.add_argument('--package-dir', help='write the notices and copy LICENSES/ and LICENSE.txt here')
    args = parser.parse_args(argv)
    if not args.output and not args.package_dir:
        parser.error('give --output and/or --package-dir')
    if args.output:
        Path(args.output).write_text(generate(), encoding='utf-8', newline='\n')
    if args.package_dir:
        write_package(args.package_dir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
