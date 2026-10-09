"""Map deployed Ubuntu libraries to authenticated apt corresponding sources."""
import json
from pathlib import Path
import re
import subprocess
import hashlib
import struct
from build import sha


def elf_code(path):
    """Deployment may alter RPATH/debug sections; substantive code/data must match."""
    data = Path(path).read_bytes()
    if data[:6] != b'\x7fELF\x02\x01': raise ValueError('expected ELF64 little endian library')
    table = struct.unpack_from('<Q', data, 40)[0]
    stride, count, strings = struct.unpack_from('<HHH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, table + n * stride) for n in range(count)]
    names = sections[strings]; names = data[names[4]:names[4] + names[5]]
    result = {}
    for section in sections:
        name = names[section[0]:].split(b'\0', 1)[0].decode()
        if name in ('.text', '.rodata', '.data', '.bss'):
            content = b'' if section[1] == 8 else data[section[4]:section[4] + section[5]]
            result[name] = dict(size=section[5], sha256=hashlib.sha256(content).hexdigest())
    if '.text' not in result: raise ValueError('library has no executable text section')
    return result


def collect(appdir, output, runtime_dir, release_name):
    output.mkdir(parents=True, exist_ok=False)
    runtime = json.loads((runtime_dir / 'runtime-receipt.json').read_text())
    libraries, package_sources, sources = {}, {}, {}
    for path in sorted(appdir.rglob('*')):
        if not path.is_file() or path.is_symlink() or '.so' not in path.name:
            continue
        owners = subprocess.check_output(['dpkg-query', '-S', '*/' + path.name], text=True)
        candidates = [line.split(': ', 1) for line in owners.splitlines() if ': /' in line]
        deployed_code = elf_code(path)
        matched = [(package, original) for package, original in candidates
                   if Path(original).is_file() and elf_code(original) == deployed_code]
        packages = {package for package, original in matched}
        if len(packages) != 1:
            raise ValueError('ambiguous Ubuntu library owner: ' + str(path))
        package = packages.pop()
        source, version = subprocess.check_output(
            ['dpkg-query', '-W', '-f=${source:Package}\t${source:Version}', package], text=True).split('\t')
        key = (source, version)
        if key not in package_sources:
            spec = source + '=' + version
            uris = subprocess.check_output(['apt-get', 'source', '--print-uris', '--download-only', '--only-source', spec],
                                           cwd=output, text=True)
            entries = re.findall(r"^'([^']+)'\s+(\S+)\s+\d+\s+\S+", uris, re.M)
            if not entries: raise ValueError('apt source metadata missing; enable Ubuntu deb-src: ' + spec)
            subprocess.run(['apt-get', 'source', '--download-only', '--only-source', spec], cwd=output, check=True)
            names = []
            for url, name in entries:
                file = output / name
                if not file.is_file() or file.name != name: raise ValueError('invalid apt source filename')
                sources[name] = dict(name=name, url=url, sha256=sha(file), authentication='apt signed Sources index')
                names.append(name)
            package_sources[key] = names
        copyright = Path('/usr/share/doc') / package.split(':')[0] / 'copyright'
        if not copyright.is_file(): raise ValueError('missing package copyright: ' + package)
        libraries[path.relative_to(appdir).as_posix()] = dict(sha256=sha(path), binary_package=package,
              source_package=source, source_version=version, copyright=str(copyright), sources=package_sources[key],
              substantive_elf_sections=deployed_code,
              original_binary_sha256=sha(next(original for owner, original in matched if owner == package)))
    for entry in runtime['sources']:
        sources[entry['name']] = entry
    manifest = dict(schema='suyu-appimage-sources-v1', version=release_name.removeprefix('v'),
                    release_name=release_name, runtime_sha256=runtime['sha256'], runtime_complete=True,
                    runtime_build_receipt=runtime, runtime_components=runtime['components'],
                    libraries=libraries, sources=list(sources.values()))
    destination = output / 'input-manifest.json'
    destination.write_text(json.dumps(manifest, indent=2) + '\n')
    return destination
