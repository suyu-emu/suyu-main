#!/usr/bin/env python3
"""Separate synthetic IPS/pchtxt export and strict-launch regression (Windows).

Uses isolated user data and the existing synthetic fixture. Nothing is taken
from a game or console. Run after the ordinary export integration gates, with
a new short work directory and an unused MCP port. Never runs implicitly.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_fixture
from run_export_integration import Run, PLACEHOLDER_KEYS, registry_dump

# PatchManager locates mods under GetBaseTitleID(program_id). Use an aligned
# synthetic application ID so exporter and launcher share that directory.
# This changes only this process's generated fixture, not the existing suite.
make_fixture.TITLE_ID &= ~0x1FFF


PATCHED_WORD = 0xD2800301  # movz x1, #24 (original debug-string length is 23)
PATCHED_MARKER = make_fixture.MARKER + '!'


def synthetic_patches():
    """Offsets address the runtime's 0x100-byte header plus flat image."""
    return [(0x100 + make_fixture.TEXT_LOC + make_fixture.CODE_OFF + 8,
             struct.pack('<I', PATCHED_WORD)),
            (0x100 + make_fixture.RODATA_LOC + make_fixture.MSG_OFF + 22, b'!\n')]


def make_ips(records):
    out = bytearray(b'PATCH')
    for offset, data in records:
        out += offset.to_bytes(3, 'big') + struct.pack('>H', len(data)) + data
    return bytes(out + b'EOF')


def make_pchtxt(build, records):
    lines = ['@nsobid-' + build, '@enabled']
    lines.extend('%08X %s' % (offset, data.hex().upper()) for offset, data in records)
    lines.append('@stop')
    return ('\n'.join(lines) + '\n').encode('ascii')


class BakedRun(Run):
    def launch_owned(self, package, timeout=120):
        """Only terminate this launch on timeout; never kill by executable name."""
        log_dir = package / 'user/log'
        for path in log_dir.glob('*.txt'):
            path.unlink()  # Only logs in this test's owned synthetic package.
        process = subprocess.Popen([str(package / 'SynthFixture.exe')], env=self.env,
                                   cwd=package, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, errors='replace',
                                   creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
        try:
            output, _ = process.communicate(timeout=timeout)
            code = process.returncode
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate()
            code = None
        log = ''
        for path in log_dir.glob('*.txt'):
            data = path.read_bytes()
            log += data.decode('utf-8', errors='replace')
        return code, output, log

    def check_manifest(self, package, patch, mod_name, kind, source=False):
        manifest = package / ('aot_cache/aot_manifest.json' if source else 'aot_manifest.json')
        metadata = json.loads(manifest.read_text(encoding='utf-8'))
        baked = metadata.get('baked_patches', {})
        mods = baked.get('mods', [])
        expected_hash = hashlib.sha256(patch.read_bytes()).hexdigest()
        expected_build = make_fixture.make_nso()[0x40:0x60].hex().upper()
        expected = [{'name': mod_name, 'files': [{'path': 'exefs/' + patch.name,
                    'sha256': expected_hash, 'size': patch.stat().st_size,
                    'target_build_id': expected_build}]}]
        self.record(kind + ': exact recorded patch set', baked.get('schema') == 1 and mods == expected)
        canonical = json.dumps(mods, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()
        self.record(kind + ': fingerprint', baked.get('fingerprint') == hashlib.sha256(canonical).hexdigest())
        self.record(kind + ': no patch payload bundled',
                    not any(p.suffix in ('.ips', '.pchtxt') for p in package.rglob('*') if p.is_file()))

    def scenarios(self):
        build = make_fixture.make_nso()[0x40:0x60].hex().upper()
        load = self.work / 'appdata/suyu/load' / ('%016X' % make_fixture.TITLE_ID)
        for kind in ('ips', 'pchtxt'):
            mod_name = 'Synthetic ' + kind
            exefs = load / mod_name / 'exefs'
            exefs.mkdir(parents=True)
            patch = exefs / ((build + '.ips') if kind == 'ips' else 'fixture.pchtxt')
            payload = make_ips(synthetic_patches()) if kind == 'ips' else make_pchtxt(build, synthetic_patches())
            patch.write_bytes(payload)
            source_output = self.work / (kind + '-source')
            status = self.export('static', 'source', output=source_output)
            self.record(kind + ': Source export', status.get('success'), status.get('status', ''))
            if not status.get('success'):
                patch.unlink()
                continue
            source = source_output / 'SynthFixture'
            self.check_manifest(source, patch, mod_name, kind + ' Source', source=True)
            generated = '\n'.join(p.read_text(errors='replace') for p in source.rglob('*.c')).lower()
            self.record(kind + ': patched TEXT reaches emitted guard words', '0xd2800301u' in generated)
            self.record(kind + ': original TEXT is absent from emitted guard words', '0xd28002e1u' not in generated)

            output = self.work / (kind + '-build')
            status = self.export('static', 'build', output=output)
            self.record(kind + ': Build export', status.get('success'), status.get('status', ''))
            if not status.get('success'):
                patch.unlink()
                continue
            package = output / 'SynthFixture'
            self.check_manifest(package, patch, mod_name, kind + ' Build')
            code, stdout, log = self.launch_owned(package)
            self.record(kind + ': unchanged patch boots modified TEXT/RODATA',
                        code == 0 and PATCHED_MARKER in log, 'exit=%s' % code)
            self.record(kind + ': strict AOT with guards and no JIT',
                        'covered=true' in log and 'falling back to JIT' not in log and
                        'unsupported code change' not in stdout + log)

            for change in ('drift', 'missing'):
                if change == 'drift':
                    patch.write_bytes(bytes([payload[0] ^ 1]) + payload[1:])
                else:
                    patch.unlink()
                code, stdout, log = self.launch_owned(package)
                self.record(kind + ': ' + change + ' refuses before emulation',
                            code == 2 and mod_name in stdout + log and
                            'suyu-cmd: Initializing system' not in stdout + log,
                            'exit=%s' % code)
                patch.write_bytes(payload)
            code, stdout, log = self.launch_owned(package)
            self.record(kind + ': restored patch boots', code == 0 and PATCHED_MARKER in log, 'exit=%s' % code)
            aot_manifest = package / 'aot_manifest.json'
            saved_manifest = aot_manifest.read_bytes()
            for damage in ('missing', 'invalid'):
                if damage == 'missing':
                    aot_manifest.unlink()
                else:
                    aot_manifest.write_bytes(b'{invalid JSON')
                try:
                    code, stdout, log = self.launch_owned(package)
                    self.record(kind + ': ' + damage + ' AOT metadata refuses before emulation',
                                code == 2 and 'missing or damaged' in stdout + log and
                                'suyu-cmd: Initializing system' not in stdout + log,
                                'exit=%s' % code)
                finally:
                    aot_manifest.write_bytes(saved_manifest)
            source_record = package / 'user/config/game-source.ini'
            saved_source = source_record.read_text(encoding='utf-8')
            raw_title = make_fixture.TITLE_ID + 1
            changed_source = saved_source.replace('title_id=%016X' % make_fixture.TITLE_ID,
                                                 'title_id=%016X' % raw_title)
            if changed_source == saved_source:
                raise RuntimeError('synthetic source record lacks expected title ID')
            source_record.write_text(changed_source, encoding='utf-8')
            try:
                code, stdout, log = self.launch_owned(package)
                self.record(kind + ': raw program-index title uses base mod root',
                            code == 0 and PATCHED_MARKER in log and 'Verified 1 baked mod(s)' in log,
                            'exit=%s' % code)
            finally:
                source_record.write_text(saved_source, encoding='utf-8')
            patch.unlink()  # Leave no patch enabled for the next format.

    def run(self):
        if self.work.exists():
            raise SystemExit('work directory exists; choose a new one')
        # Refuse a occupied port before contacting an unrelated running GUI.
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', self.args.port))
        self.work.mkdir(parents=True)
        (self.work / 'appdata').mkdir()
        (self.work / 'localappdata').mkdir()
        self.fixture_info = make_fixture.write_fixture(self.fixture)
        keys = self.work / 'appdata/suyu/keys/prod.keys'
        keys.parent.mkdir(parents=True)
        keys.write_text(PLACEHOLDER_KEYS)
        (self.work / 'appdata/suyu/load').mkdir()
        before = registry_dump()
        try:
            self.start_suyu()
            self.scenarios()
        finally:
            if self.process is not None:
                self.stop_suyu()
        self.record('registry unchanged', registry_dump() == before)
        (self.work / 'baked-patch-report.json').write_text(json.dumps(self.results, indent=2))
        failed = sum(not result['ok'] for result in self.results)
        print('%d passed, %d failed' % (len(self.results) - failed, failed))
        return int(failed != 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suyu-dir', required=True)
    parser.add_argument('--work', required=True)
    parser.add_argument('--port', type=int, default=9787)
    args = parser.parse_args()
    if os.name != 'nt':
        print('SKIP: Windows exporter integration')
        return 0
    return BakedRun(args).run()


if __name__ == '__main__':
    sys.exit(main())
