#!/usr/bin/env python3
"""Rebuild the source-pinned static runtime in an isolated two-CPU container."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
from build import HERE, fetch, sha


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work-dir', type=Path, required=True)
    p.add_argument('--docker', default='docker')
    a = p.parse_args()
    lock = json.loads((HERE / 'runtime.lock.json').read_text())
    if sha(HERE / 'runtime-build.sh') != lock['recipe_sha256']:
        raise ValueError('runtime build recipe differs from reviewed lock')
    a.work_dir.mkdir(parents=True, exist_ok=False)
    work = a.work_dir.resolve()
    inputs = work / 'inputs'; inputs.mkdir()
    apks = work / 'apks'; apks.mkdir()
    for entry in lock['sources']:
        fetch(entry['url'], inputs / entry['name'], entry['sha256'])
    for entry in lock['apks']:
        fetch(entry['url'], apks / entry['name'], entry['sha256'])
    recipe = work / 'runtime-build.sh'; recipe.write_bytes((HERE / 'runtime-build.sh').read_bytes())
    with (work / 'build.log').open('w') as log:
        subprocess.run([a.docker, 'run', '--rm', '--cpus', '2', '-e', 'RUN_NAME=result',
                        '-v', str(work) + ':/proof', lock['image'], 'sh', '-c',
                        'apk add --no-network /proof/apks/*.apk && bash /proof/runtime-build.sh'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    binary = work / 'result/runtime-x86_64'
    if sha(binary) != lock['sha256'] or binary.stat().st_size != lock['size']:
        raise ValueError('runtime reproducibility pin mismatch; preserve build receipt')
    # Docker owns result/, so an unprivileged host cannot unlink from it.
    # Preserve the container receipt and create a host-owned executable instead.
    staged = work / 'runtime-x86_64'
    shutil.copy2(binary, staged)
    if sha(staged) != lock['sha256'] or staged.stat().st_size != lock['size']:
        raise ValueError('staged runtime differs from reproducibility pin')
    mapping = (work / 'result/runtime.map').read_text()
    records = {}
    for block in (work / 'result/apk-installed-db.txt').read_text().split('\n\n'):
        record = {line[0]: line[2:] for line in block.splitlines() if len(line) > 1 and line[1] == ':'}
        if 'P' in record: records[record['P']] = record
    for component, expected_package in lock['aports_build_commits'].items():
        installed = records[expected_package['package']]
        if installed['V'] != expected_package['version'] or installed['c'] != expected_package['commit']:
            raise ValueError('installed runtime library recipe differs: ' + component)
    contributed = sorted(set(re.findall(r'(/[^\s()]+\.a)\(', mapping)))
    expected = ['/usr/lib/libc.a', '/usr/lib/libfuse3.a', '/usr/lib/libmimalloc.a',
                '/usr/lib/libz.a', '/usr/lib/libzstd.a', '/usr/local/lib/libsquashfuse.a',
                '/usr/local/lib/libsquashfuse_ll.a']
    if contributed != expected:
        raise ValueError('unexpected static archive closure: ' + repr(contributed))
    receipt = dict(lock, recipe_sha256=sha(recipe), lock_sha256=sha(HERE / 'runtime.lock.json'),
                   linker_map_sha256=sha(work / 'result/runtime.map'),
                   installed_package_db_sha256=sha(work / 'result/apk-installed-db.txt'),
                   contributed_archives=contributed, reproducibility_pin_verified=True)
    (work / 'runtime-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(work / 'runtime-x86_64')


if __name__ == '__main__': main()
