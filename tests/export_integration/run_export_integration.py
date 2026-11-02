#!/usr/bin/env python3
"""Drive the real exporter end to end with the synthetic fixture (Windows).

Starts suyu.exe from a release-like folder with an isolated APPDATA, drives
exports through its local automation port, and checks what reaches the output
folder: JIT, Hybrid and strict-AOT packages, Source format, conflicts, backups,
failure injection, rejected input, and the exported launcher itself (its
missing-keys handling, its refusal to read keys or firmware from inside the
package, and a run after the package is moved).

Never touches the developer's suyu data: APPDATA/LOCALAPPDATA point into the
work folder, the exporter's test mode writes nothing to the registry, and the
suyu registry key is compared before and after. Placeholder files named
prod.keys contain no key material; they only satisfy the launcher's existence
check for the unencrypted synthetic input.
"""
import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_fixture  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
# The launcher only checks that prod.keys exists, but suyu-cmd then crashes
# (0xC0000094) building the NCA header cipher when no header_key is loaded,
# even for unencrypted input - a known, pre-existing defect. The test file gets
# a made-up value, assembled here so no key-shaped line sits in the repository.
# It is not a real key and decrypts nothing.
_MADE_UP = '00112233445566778899aabbccddeeff' + 'ffeeddccbbaa99887766554433221100'
PLACEHOLDER_KEYS = ('# synthetic test placeholder; not a real key\n' +
                    'header_key' + ' = ' + _MADE_UP + '\n')
# Made-up sd_seed values for two pretend consoles, A and B, which portable exports are sealed
# to. Assembled the same way; they come from no console.
SEED_A = '5eed' * 8
SEED_B = 'b0b0' * 8
# Synthetic DLC title IDs: two add-ons of the fixture game (0x0100000000E57A00) and one of
# another made-up game. None belongs to a real title.
DLC_TITLES = (0x0100000000E57001, 0x0100000000E57002)
OTHER_GAME_DLC = 0x0100000000F01001
# Synthetic titlekey-encrypted DLC of a third made-up game (0x0100000000F02000), installed
# from an NSP with a ticket, and the made-up title key its ticket carries.
TICKET_DLC = 0x0100000000F02001
TICKET_TITLE_KEY = bytes(range(0x40, 0x50))
# suyu opens an NCA only with some application key-area key loaded, even one with a plaintext
# header and no encrypted section. A made-up value, assembled like the others.
KEY_AREA_PLACEHOLDER = 'key_area_key_application' + '_00' + ' = ' + '7a' * 16 + '\n'


def keys_with_seed(seed):
    return PLACEHOLDER_KEYS + 'sd_seed' + ' = ' + seed + '\n'


def seal_key(seed, export_id):
    digest = hmac.new(bytes.fromhex(seed), b'suyu-portable-seal-v1' + export_id.encode(),
                      hashlib.sha256).digest()
    return digest[:16]


def unseal(data, key, nonce):
    # AES-128-CTR, counter = nonce (8 bytes) || block index (8 bytes, big-endian), from 0.
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    decryptor = Cipher(algorithms.AES(key), modes.CTR(nonce + bytes(8))).decryptor()
    return decryptor.update(data) + decryptor.finalize()


class Mcp:
    def __init__(self, port):
        self.port = port
        self.pending = []

    def _send(self, name, arguments):
        request = {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
                   'params': {'name': name, 'arguments': arguments}}
        sock = socket.create_connection(('127.0.0.1', self.port), timeout=30)
        sock.sendall(json.dumps(request).encode())
        return sock

    def call(self, name, arguments=None, timeout=60):
        sock = self._send(name, arguments or {})
        sock.settimeout(timeout)
        data = b''
        while True:
            chunk = sock.recv(1 << 16)
            if not chunk:
                break
            data += chunk
            try:
                response = json.loads(data)
                break
            except ValueError:
                continue
        sock.close()
        text = response['result']['content'][0]['text']
        return json.loads(text)

    def fire(self, name, arguments=None):
        # For calls whose handler runs a modal dialog: the reply only comes when it closes.
        self.pending.append(self._send(name, arguments or {}))


def snapshot(root):
    files = {}
    for path in sorted(Path(root).rglob('*')):
        if path.is_file():
            files[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return files


def registry_dump():
    result = subprocess.run(['reg', 'query', r'HKCU\Software\suyu', '/s'], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, errors='replace')
    return result.stdout


class Run:
    def __init__(self, args):
        self.args = args
        self.work = Path(args.work)
        self.results = []
        self.env = dict(os.environ)
        self.env['APPDATA'] = str(self.work / 'appdata')
        self.env['LOCALAPPDATA'] = str(self.work / 'localappdata')
        self.env['SUYU_MCP_PORT'] = str(args.port)
        self.env['SUYU_CMD_CAPTURE_HEADLESS'] = '1'
        self.out = self.work / 'out'
        self.fixture = self.work / 'SynthFixture'
        self.process = None
        self.mcp = Mcp(args.port)

    def record(self, name, ok, detail=''):
        self.results.append({'test': name, 'ok': bool(ok), 'detail': detail})
        print(('PASS ' if ok else 'FAIL ') + name + (': ' + detail if detail else ''), flush=True)

    def start_suyu(self):
        exe = Path(self.args.suyu_dir) / 'suyu.exe'
        self.process = subprocess.Popen([str(exe)], env=self.env, cwd=str(self.work),
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 180
        while time.time() < deadline:
            try:
                self.mcp.call('get_firmware_status', timeout=10)
                return
            except (OSError, ValueError, KeyError):
                time.sleep(2)
        raise SystemExit('suyu did not open its automation port')

    def open_dialog(self):
        status = self.mcp.call('get_aot_export_status')
        if status.get('available'):
            return
        self.mcp.fire('trigger_ui_action', {'action': 'export_game'})
        for _ in range(60):
            time.sleep(1)
            if self.mcp.call('get_aot_export_status').get('available'):
                return
        raise SystemExit('the export dialog did not open')

    def export(self, backend, fmt, rom=None, output=None, timeout=5400, **options):
        self.open_dialog()
        arguments = {'action': 'aot_test_export', 'rom_path': str(rom or self.fixture),
                     'output_dir': str(output or self.out), 'backend': backend, 'format': fmt,
                     'include_save': False, 'include_shader': False, 'include_config': False,
                     # Named outright: the dialog otherwise uses the choice saved in suyu's
                     # settings, which belong to the developer's own suyu.
                     'package': 'reference'}
        arguments.update(options)
        self.mcp.call('trigger_ui_action', arguments)
        deadline = time.time() + timeout
        time.sleep(2)
        while time.time() < deadline:
            status = self.mcp.call('get_aot_export_status', timeout=120)
            if status.get('done') and not status.get('running'):
                return status
            time.sleep(5)
        raise SystemExit('export timed out: %s %s' % (backend, fmt))

    def staging_leftovers(self, output=None):
        return sorted(p.name for p in Path(output or self.out).glob('.suyu-export-*'))

    # ---- launcher ----

    def launch(self, package, exe_name, keys=True, timeout=90, keys_text=None, env=None):
        keys_dir = self.work / 'appdata' / 'suyu' / 'keys'
        keys_file = keys_dir / 'prod.keys'
        if keys:
            keys_dir.mkdir(parents=True, exist_ok=True)
            keys_file.write_text(keys_text or PLACEHOLDER_KEYS)
        elif keys_file.exists():
            keys_file.unlink()
        log_dir = package / 'user' / 'log'
        shutil.rmtree(log_dir, ignore_errors=True)
        try:
            result = subprocess.run([str(package / exe_name)], env=dict(self.env, **(env or {})),
                                    cwd=str(package),
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=timeout, text=True, errors='replace')
            code, output = result.returncode, result.stdout
        except subprocess.TimeoutExpired as expired:
            code, output = None, (expired.output or '') if isinstance(expired.output, str) else ''
            subprocess.run(['taskkill', '/F', '/T', '/IM', exe_name], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        log = ''
        for path in log_dir.glob('*.txt') if log_dir.exists() else []:
            log += path.read_text(errors='replace')
        return code, output, log

    # ---- scenarios ----

    def check_package(self, name, package, expect_aot_cache, expect_launcher):
        manifest = json.loads((package / 'export-package.json').read_text())
        files = snapshot(package)
        names = set(files)
        self.record(name + ': package manifest', manifest.get('classification') == 'LOCAL_GAME_EXPORT'
                    and manifest.get('title_id') == '%016X' % self.fixture_info['title_id']
                    and manifest['contains']['nintendo_keys'] is False
                    and manifest['contains']['system_firmware'] is False,
                    json.dumps(manifest.get('format')))
        # The default package type, unchanged: it runs the user's game file with their keys.
        self.record(name + ': default package type', manifest.get('package_type') == 'reference'
                    and manifest['contains'].get('original_game_file_sealed') is False
                    and manifest.get('requires_at_launch') == ['user_game_file', 'user_keys']
                    and not any(n.startswith('game/') for n in names))
        # No game data at all: the package names the user's game file instead.
        self.record(name + ': no game files in the package',
                    not any(n.startswith('exefs/') or n.endswith('romfs.bin') for n in names)
                    and manifest['contains']['extracted_exefs'] is False
                    and manifest['contains']['decrypted_romfs'] is False)
        if expect_launcher:
            self.record(name + ': records the game file', 'user/config/game-source.ini' in names and
                        str(self.fixture.name) in (package / 'user/config/game-source.ini').read_text())
        # Source projects keep no module copies, segments, runner or guest dumps.
        self.record(name + ': no module copies or standalone runner',
                    not any(n.startswith('aot_cache/exefs/nso/') or '/data/' in n or
                            n.endswith('/main.c') or n.startswith('aot_cache/debug/')
                            for n in names))
        self.record(name + ': licenses', {'LICENSES/LICENSE.txt', 'LICENSES/THIRD-PARTY-NOTICES.txt',
                                          'LICENSES/SOURCE.txt'} <= names)
        self.record(name + ': aot_cache ' + ('kept' if expect_aot_cache else 'absent'),
                    any(n.startswith('aot_cache/') for n in names) == expect_aot_cache)
        if expect_launcher:
            self.record(name + ': launcher', any(n.endswith('.exe') and '/' not in n for n in names))
        bad = [n for n in names if n.lower().endswith(('.keys', '.tik', '.nsp', '.xci', '.nca'))
               and n != 'exefs/control.nca']
        self.record(name + ': no keys or containers', not bad, ', '.join(bad))
        source_note = (package / 'game_source.txt')
        if source_note.exists():
            text = source_note.read_text()
            self.record(name + ': no source path in package', str(self.work) not in text and
                        'Users' not in text)
        manifest_text = (package / 'aot_manifest.json').read_text() if (package / 'aot_manifest.json').exists() else ''
        self.record(name + ': no machine paths in manifests', ':\\\\' not in manifest_text and
                    ':/' not in manifest_text and str(self.work) not in json.dumps(manifest))
        (self.work / 'scan-export.json').unlink(missing_ok=True)
        scan = subprocess.run([sys.executable, str(ROOT / 'tools/package_policy/scan_release.py'),
                               '--kind', 'windows', '--report', str(self.work / 'scan-export.json'),
                               str(self.zip_of(package))], stdout=subprocess.PIPE, text=True)
        report = self.work / 'scan-export.json'
        rules = set()
        if report.exists():
            rules = {f['rule'] for a in json.loads(report.read_text())['archives']
                     for f in a['findings']}
        self.record(name + ': public-release scan rejects the export',
                    scan.returncode == 1 and 'export-marker' in rules, ','.join(sorted(rules)))

    def missing_key_check(self, package, exe):
        # The configuration that used to crash (divide-by-zero in the AES layer): an
        # encrypted file, a prod.keys present but without header_key. The launcher must
        # now say the keys do not fit and stop.
        nca = self.work / 'encrypted-looking.nca'
        nca.write_bytes(os.urandom(0x8000))
        record = package / 'user' / 'config' / 'game-source.ini'
        original = record.read_text()
        record.write_text('path=' + str(nca) + '\n')
        keys = self.work / 'appdata' / 'suyu' / 'keys' / 'prod.keys'
        keys.write_text('# placeholder without header_key\n')
        log_dir = package / 'user' / 'log'
        shutil.rmtree(log_dir, ignore_errors=True)
        try:
            code = subprocess.run([str(package / exe)], env=self.env, cwd=str(package),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  timeout=90).returncode
        except subprocess.TimeoutExpired:
            code = None
        log = ''.join(p.read_text(errors='replace') for p in log_dir.glob('*.txt'))             if log_dir.exists() else ''
        self.record('missing header key is reported, not a crash',
                    code == 2 and 'could not be decrypted' in log, 'exit=%s' % code)
        record.write_text(original)
        keys.write_text(PLACEHOLDER_KEYS)

    def portable_scenarios(self):
        """Portable package: the synthetic NSP copied in, sealed to console A's sd_seed."""
        nsp = self.work / 'games' / 'SynthNsp.nsp'
        nsp_info = make_fixture.write_nsp(nsp)
        out = self.work / 'out-portable'
        status = self.export('dynarmic', 'build', rom=nsp, output=out, package='portable')
        self.record('portable: export succeeds', status.get('success'), status.get('status', ''))
        if not status.get('success'):
            return
        package = Path(status['output_path'])
        exe = package.name + '.exe'
        self.record('portable: no staging left behind', not self.staging_leftovers(out))
        manifest = json.loads((package / 'export-package.json').read_text())
        self.record('portable: manifest', manifest.get('package_type') == 'portable'
                    and manifest['contains'].get('original_game_file_sealed') is True
                    and manifest['contains']['nintendo_keys'] is False
                    and manifest['contains']['system_firmware'] is False
                    and manifest.get('requires_at_launch') == ['user_keys_for_this_console'],
                    json.dumps(manifest.get('requires_at_launch')))
        self.record('portable: manifest counts the DLC', manifest['contains'].get('dlc_count') == 2,
                    str(manifest['contains'].get('dlc_count')))
        names = set(snapshot(package))
        seal = json.loads((package / 'game' / 'seal.json').read_text())
        game_files = sorted(n for n in names if n.startswith('game/'))
        dlc_names = ['dlc-%d.sealed' % i for i in range(len(self.dlc))]
        self.record('portable: sealed layout',
                    game_files == sorted(['game/base.sealed', 'game/seal.json'] +
                                         ['game/' + n for n in dlc_names])
                    and [f['name'] for f in seal['files']] == ['base.sealed'] + dlc_names,
                    ', '.join(game_files))
        sealed = (package / 'game' / 'base.sealed').read_bytes()
        original = nsp.read_bytes()
        self.record('portable: sealed size matches the game file',
                    len(sealed) == nsp_info['size'] == int(seal['files'][0]['size']))
        self.record('portable: no container signature in the sealed file',
                    sealed[:4] != b'PFS0' and sealed[0x100:0x104] != b'HEAD' and sealed != original)
        export_id = manifest['export_id']
        key = seal_key(SEED_A, export_id)
        check = hmac.new(key, b'check', hashlib.sha256).digest()[:16].hex()
        self.record('portable: seal check value', seal.get('check') == check
                    and seal.get('export_id') == export_id)
        try:
            unsealed = unseal(sealed, key, bytes.fromhex(seal['files'][0]['nonce']))
            self.record('portable: unsealing gives the game file byte for byte',
                        unsealed == original)
        except ImportError:
            self.record('portable: unsealing gives the game file byte for byte', False,
                        'the cryptography package is needed for this check')
        # DLC: each installed NCA of the game's DLC, sealed with its own nonce, byte for byte;
        # the other game's DLC is left out.
        dlc_entries = seal['files'][1:]
        self.record('portable: DLC entries in seal.json',
                    [e.get('role') for e in dlc_entries] == ['dlc'] * len(self.dlc)
                    and [int(e['title_id'], 16) for e in dlc_entries] == self.dlc_titles
                    and [e.get('record_type') for e in dlc_entries] == [t for t, _, _ in self.dlc]
                    and [int(e['size']) for e in dlc_entries] == [len(d) for _, _, d in self.dlc]
                    and len({e['nonce'] for e in seal['files']}) == len(seal['files']),
                    json.dumps(dlc_entries))
        self.record('portable: other games\' DLC left out',
                    '%016X' % OTHER_GAME_DLC not in json.dumps(seal))
        try:
            same = all(unseal((package / 'game' / e['name']).read_bytes(), key,
                              bytes.fromhex(e['nonce'])) == data
                       for e, (_, _, data) in zip(dlc_entries, self.dlc))
            self.record('portable: unsealing gives each DLC file byte for byte', same)
        except ImportError:
            self.record('portable: unsealing gives each DLC file byte for byte', False,
                        'the cryptography package is needed for this check')
        self.record('portable: DLC files are sealed, not plain',
                    all((package / 'game' / n).read_bytes()[:13] != b'synthetic dlc'
                        for n in dlc_names))
        stored = ''.join((package / n).read_bytes().decode('latin-1').lower()
                         for n in names if not n.endswith('.sealed'))
        self.record('portable: no seed or seal key stored',
                    SEED_A not in stored and key.hex() not in stored)
        bad = [n for n in names if n.lower().endswith(('.keys', '.tik', '.nsp', '.xci', '.nca'))
               or n.lower().startswith(('user/nand/system', 'user/keys'))]
        self.record('portable: no keys, firmware or plain containers', not bad, ', '.join(bad))
        self.record('portable: records no game file path',
                    'user/config/game-source.ini' not in names and
                    str(self.work) not in json.dumps(manifest) + json.dumps(seal))

        marker = self.fixture_info['marker']
        keys_file = self.work / 'appdata' / 'suyu' / 'keys' / 'prod.keys'
        # Console A's keys installed for this user: the package runs.
        code, output, log = self.launch(package, exe, keys_text=keys_with_seed(SEED_A))
        self.record('portable launcher: runs with this console\'s keys', marker in log,
                    'exit=%s' % code)
        self.record('portable launcher: registers the sealed DLC',
                    'Registered 0 sealed update entries and %d sealed DLC entries' % len(self.dlc)
                    in log, 'exit=%s' % code)
        self.dlc_launch_checks(package, exe, marker)
        # Moved elsewhere, it still runs: it needs no game file outside itself.
        moved = self.work / 'moved' / 'Relocated portable'
        moved.parent.mkdir(exist_ok=True)
        shutil.move(str(package), str(moved))
        hidden = nsp.with_suffix('.hidden')
        nsp.rename(hidden)
        code, output, log = self.launch(moved, exe, keys_text=keys_with_seed(SEED_A))
        self.record('portable launcher: runs after moving, without the original game file',
                    marker in log, 'exit=%s' % code)
        hidden.rename(nsp)
        shutil.move(str(moved), str(package))
        # Another console's keys are refused, and nothing is decrypted.
        code, output, log = self.launch(package, exe, keys_text=keys_with_seed(SEED_B))
        self.record('portable launcher: refuses keys from another console',
                    code == 2 and 'different console' in log and marker not in log,
                    'exit=%s' % code)
        # Keys without an sd_seed: a clear refusal.
        code, output, log = self.launch(package, exe, keys_text=PLACEHOLDER_KEYS)
        self.record('portable launcher: refuses keys without sd_seed',
                    code == 2 and 'Keys without sd_seed' in log and marker not in log,
                    'exit=%s' % code)
        # Keys put inside the package are never used...
        for folder in (package, package / 'user' / 'keys'):
            folder.mkdir(parents=True, exist_ok=True)
            (folder / 'prod.keys').write_text(keys_with_seed(SEED_A))
        code, output, log = self.launch(package, exe, keys=False)
        self.record('portable launcher: ignores keys inside the package',
                    code == 2 and 'Missing keys' in log and marker not in log
                    and not keys_file.exists(), 'exit=%s' % code)
        # ...nor installed from there by the first-run setup.
        code, output, log = self.launch(package, exe, keys=False,
                                        env={'SUYU_CMD_SETUP_KEYS_FROM': str(package)})
        self.record('portable setup: refuses key files inside the package',
                    code == 2 and 'never used' in log and not keys_file.exists(),
                    'exit=%s' % code)
        (package / 'prod.keys').unlink()
        shutil.rmtree(package / 'user' / 'keys')
        # The first-run setup checks the chosen keys belong to the console the export was made
        # with, then installs them into the per-user keys folder, never into the package.
        other = self.work / 'other-console-keys'
        other.mkdir()
        (other / 'prod.keys').write_text(keys_with_seed(SEED_B))
        code, output, log = self.launch(package, exe, keys=False,
                                        env={'SUYU_CMD_SETUP_KEYS_FROM': str(other / 'prod.keys')})
        self.record('portable setup: refuses keys from another console',
                    code == 2 and 'different console' in log and not keys_file.exists(),
                    'exit=%s' % code)
        mine = self.work / 'my-console-keys'
        mine.mkdir()
        (mine / 'prod.keys').write_text(keys_with_seed(SEED_A))
        code, output, log = self.launch(package, exe, keys=False,
                                        env={'SUYU_CMD_SETUP_KEYS_FROM': str(mine / 'prod.keys')})
        self.record('portable setup: installs the user\'s keys for this user and runs',
                    marker in log and keys_file.exists()
                    and keys_file.read_text() == keys_with_seed(SEED_A)
                    and not any(n.lower().endswith('.keys') for n in snapshot(package)),
                    'exit=%s' % code)
        # Later launches use the installed keys without asking again.
        code, output, log = self.launch(package, exe, keys_text=keys_file.read_text())
        self.record('portable launcher: later launches use the installed keys',
                    marker in log and 'Missing keys' not in log, 'exit=%s' % code)
        keys_file.write_text(PLACEHOLDER_KEYS)

    def dlc_launch_checks(self, package, exe, marker):
        """A missing DLC file leaves that DLC out; a seal.json that misdescribes DLC is refused."""
        game = package / 'game'
        seal_path = game / 'seal.json'
        original_seal = seal_path.read_text()
        held = game / 'dlc-1.held'
        (game / 'dlc-1.sealed').rename(held)
        code, output, log = self.launch(package, exe, keys_text=keys_with_seed(SEED_A))
        self.record('portable launcher: runs without a missing DLC file',
                    marker in log and 'continuing without DLC' in log
                    and 'Registered 0 sealed update entries and %d sealed DLC entries'
                    % (len(self.dlc) - 1) in log, 'exit=%s' % code)
        held.rename(game / 'dlc-1.sealed')

        def tampered(change):
            seal = json.loads(original_seal)
            change(seal)
            seal_path.write_text(json.dumps(seal, indent=4))
            try:
                return self.launch(package, exe, keys_text=keys_with_seed(SEED_A))
            finally:
                seal_path.write_text(original_seal)

        def dlc_entry(change):
            return lambda seal: change(seal['files'][1])

        cases = [
            ('DLC of another game',
             dlc_entry(lambda e: e.update(title_id='%016X' % OTHER_GAME_DLC)),
             'DLC for another game'),
            ('DLC whose title ID is the game itself',
             dlc_entry(lambda e: e.update(title_id='%016X' % self.fixture_info['title_id'])),
             'DLC for another game'),
            ('DLC without a content type', dlc_entry(lambda e: e.pop('record_type')),
             'no content type'),
            ('DLC with a content type out of range',
             dlc_entry(lambda e: e.update(record_type=7)), 'no content type'),
            ('a DLC file listed as an update', dlc_entry(lambda e: e.update(role='update')),
             'malformed entry'),
            ('the same DLC content listed twice',
             lambda seal: seal['files'].append(dict(seal['files'][1], name='dlc-9.sealed')),
             'same DLC content twice'),
        ]
        for name, change, expected in cases:
            code, output, log = tampered(change)
            self.record('portable launcher: refuses ' + name,
                        code == 2 and expected in log + output and marker not in log,
                        'exit=%s' % code)

    def zip_of(self, package):
        target = self.work / 'zips' / (package.name + '.zip')
        target.parent.mkdir(exist_ok=True)
        if target.exists():
            target.unlink()
        shutil.make_archive(str(target.with_suffix('')), 'zip', root_dir=package)
        return target

    def run(self):
        if self.work.exists():
            raise SystemExit('work folder exists; pass a new one')
        self.work.mkdir(parents=True)
        (self.work / 'appdata').mkdir()
        (self.work / 'localappdata').mkdir()
        self.fixture_info = make_fixture.write_fixture(self.fixture)
        # suyu itself hits the same header-cipher crash while probing a game with no
        # header_key loaded, so the placeholder is installed before it starts.
        keys_dir = self.work / 'appdata' / 'suyu' / 'keys'
        keys_dir.mkdir(parents=True)
        # suyu reads its keys once, at start; console A's sd_seed lets it make portable exports.
        (keys_dir / 'prod.keys').write_text(keys_with_seed(SEED_A) + KEY_AREA_PLACEHOLDER)
        # Synthetic DLC installed in this suyu's NAND before it starts: two DLC titles of the
        # fixture game (one larger than a 4 MiB copy chunk) and one of another game, which
        # exports must leave out. Made-up bytes; see make_fixture.install_dlc.
        registered = self.work / 'appdata' / 'suyu' / 'nand' / 'user' / 'Contents' / 'registered'
        base = self.fixture_info['title_id']
        self.dlc = (make_fixture.install_dlc(registered, DLC_TITLES[0], [(2, (5 << 20) + 123)]) +
                    make_fixture.install_dlc(registered, DLC_TITLES[1], [(2, 4096), (3, 777)]))
        self.dlc_titles = [DLC_TITLES[0], DLC_TITLES[1], DLC_TITLES[1]]
        make_fixture.install_dlc(registered, OTHER_GAME_DLC, [(2, 2048)])
        assert all((t & ~0x1FFF) == (base & ~0x1FFF) for t in DLC_TITLES)
        registry_before = registry_dump()
        self.start_suyu()
        try:
            self.scenarios()
        finally:
            try:
                self.process.terminate()
                self.process.wait(30)
            except Exception:
                self.process.kill()
        self.record('registry unchanged by test exports', registry_dump() == registry_before)
        # suyu and the launchers only read keys; nothing may be written beside them.
        keys = sorted(p.name for p in (self.work / 'appdata' / 'suyu' / 'keys').iterdir())
        self.record('no key files created', keys == ['prod.keys'], ', '.join(keys))
        report = self.work / 'integration-report.json'
        report.write_text(json.dumps(self.results, indent=2))
        failed = [r for r in self.results if not r['ok']]
        print('%d passed, %d failed' % (len(self.results) - len(failed), len(failed)))
        return 1 if failed else 0

    def stop_suyu(self):
        try:
            self.process.terminate()
            self.process.wait(30)
        except Exception:
            self.process.kill()
            self.process.wait(30)

    def ticket_scenarios(self):
        """Installing an NSP keeps its ticket in the NAND; a restarted suyu loads it again."""
        nsp = self.work / 'games' / 'SynthTicketDlc.nsp'
        info = make_fixture.write_titlekey_dlc_nsp(nsp, TICKET_DLC, TICKET_DLC & ~0x1FFF,
                                                   TICKET_TITLE_KEY)
        self.ticket_info = info
        nand = self.work / 'appdata' / 'suyu' / 'nand'
        store = nand / 'system' / 'tickets'
        keys_dir = self.work / 'appdata' / 'suyu' / 'keys'
        before = self.mcp.call('get_keys_status')
        result = self.mcp.call('install_content_from_path', {'path': str(nsp)}, timeout=120)
        stored = sorted(p.name for p in store.iterdir()) if store.exists() else []
        expected = info['rights_id'] + '.tik'
        self.record('install: NSP with a ticket installs', result.get('result') == 'success',
                    json.dumps(result))
        self.record('install: only the valid ticket is kept, as shipped',
                    stored == [expected] and (store / expected).read_bytes() == info['ticket'],
                    ', '.join(stored))
        self.record('install: no key files created',
                    sorted(p.name for p in keys_dir.iterdir()) == ['prod.keys'])
        self.record('install: nothing loaded from the store before a restart',
                    before.get('installed_tickets') == 0, json.dumps(before))
        stamp = (store / expected).stat().st_mtime_ns if stored else None
        time.sleep(1.5)
        again = self.mcp.call('install_content_from_path', {'path': str(nsp)}, timeout=120)
        self.record('install again: identical ticket left alone',
                    again.get('result') == 'overwrite' and stored and
                    (store / expected).stat().st_mtime_ns == stamp, json.dumps(again))
        self.stop_suyu()
        self.start_suyu()
        status = self.mcp.call('get_keys_status')
        log = ''.join(p.read_text(errors='replace')
                      for p in (self.work / 'appdata' / 'suyu' / 'log').glob('*.txt'))
        self.record('restart: installed ticket loaded with its title key',
                    status.get('installed_tickets') == 1 and
                    status.get('installed_ticket_title_keys') == 1 and
                    'Loaded 1 installed tickets (1 with a title key)' in log, json.dumps(status))

    def launcher_ticket_check(self, package, exe):
        """The launcher reads tickets from the installed NAND, never from its own package."""
        decoy = package / 'user' / 'nand' / 'system' / 'tickets'
        decoy.mkdir(parents=True, exist_ok=True)
        decoy_rights = struct.pack('>Q', TICKET_DLC + 1) + bytes(7) + b'\x0a'
        (decoy / (decoy_rights.hex() + '.tik')).write_bytes(
            make_fixture.make_ticket(decoy_rights, TICKET_TITLE_KEY))
        code, output, log = self.launch(package, exe, keys=True)
        installed = str(self.work / 'appdata' / 'suyu' / 'nand' / 'system' / 'tickets')
        loaded = [line for line in log.splitlines() if 'installed tickets' in line]
        lines = [line.replace('/', os.sep).lower() for line in loaded]
        self.record('launcher: loads tickets from the installed NAND only',
                    len(loaded) == 1 and 'Loaded 1 installed tickets (1 with a title key)'
                    in loaded[0] and installed.lower() in lines[0] and
                    str(package).lower() not in lines[0],
                    loaded[0] if loaded else 'exit=%s' % code)
        shutil.rmtree(package / 'user' / 'nand' / 'system')

    def scenarios(self):
        a = self.args
        jit_name = 'SynthFixture - Dynarmic JIT'
        jit = self.out / jit_name

        self.ticket_scenarios()

        # JIT baseline package.
        status = self.export('dynarmic', 'build')
        self.record('JIT export succeeds', status.get('success'), status.get('status', ''))
        if status.get('success'):
            self.check_package('JIT', jit, False, True)
        self.record('JIT: no staging left behind', not self.staging_leftovers())

        # Launcher behaviour: keys missing, then present; firmware prompt is headless.
        exe = jit_name + '.exe'
        code, output, log = self.launch(jit, exe, keys=False)
        self.record('launcher: missing keys stop the game (exit 2)', code == 2 and 'Missing keys' in log,
                    'exit=%s' % code)
        code, output, log = self.launch(jit, exe, keys=True)
        marker = self.fixture_info['marker']
        self.record('launcher: synthetic program runs (JIT)', marker in log, 'exit=%s' % code)
        self.launcher_ticket_check(jit, exe)

        # Relocation after promotion: move the whole package and run it again.
        moved = self.work / 'moved' / 'Relocated JIT'
        moved.parent.mkdir()
        shutil.move(str(jit), str(moved))
        code, output, log = self.launch(moved, exe, keys=True)
        self.record('launcher: runs after the package is moved', marker in log, 'exit=%s' % code)
        shutil.move(str(moved), str(jit))

        # The package runs only with the user's own game file present.
        hidden = self.work / 'SynthFixture-hidden'
        self.fixture.rename(hidden)
        code, output, log = self.launch(jit, exe, keys=True)
        self.record('launcher: refuses to start without the user\'s game file',
                    code == 2 and 'Game file not found' in log and marker not in log,
                    'exit=%s' % code)
        hidden.rename(self.fixture)

        # Keys or firmware placed inside the package are not used.
        fake = jit / 'fakesuyu'
        (fake / 'user' / 'keys').mkdir(parents=True)
        (fake / 'suyu.exe').write_bytes(b'')
        (fake / 'user' / 'keys' / 'prod.keys').write_text(PLACEHOLDER_KEYS)
        record = jit / 'user' / 'config' / 'suyu-install.txt'
        original_record = record.read_text() if record.exists() else None
        record.parent.mkdir(parents=True, exist_ok=True)
        record.write_text('../../fakesuyu/suyu.exe\n')
        code, output, log = self.launch(jit, exe, keys=False)
        self.record('launcher: ignores a recorded suyu inside the package',
                    code == 2 and 'Missing keys' in log and 'Ignoring a recorded suyu' in log,
                    'exit=%s' % code)
        shutil.rmtree(fake)
        if original_record is None:
            record.unlink()
        else:
            record.write_text(original_record)
        firmware = jit / 'user' / 'nand' / 'system' / 'Contents' / 'registered'
        # The launcher creates this folder, empty, on its first run.
        firmware.mkdir(parents=True, exist_ok=True)
        (firmware / 'placeholder.nca').write_bytes(b'\0' * 16)
        code, output, log = self.launch(jit, exe, keys=True)
        self.record('launcher: refuses firmware inside a validated export',
                    code == 2 and 'Firmware inside the export' in log, 'exit=%s' % code)
        shutil.rmtree(firmware)
        firmware.mkdir()

        self.missing_key_check(jit, exe)

        # Conflicts: a headless export never merges or replaces by default.
        (jit / 'user' / 'nand' / 'user' / 'save' / '0000000000000000' /
         '00112233445566778899aabbccddeeff' / ('%016X' % self.fixture_info['title_id'])).mkdir(parents=True)
        save = (jit / 'user' / 'nand' / 'user' / 'save' / '0000000000000000' /
                '00112233445566778899aabbccddeeff' / ('%016X' % self.fixture_info['title_id']) / 'save.bin')
        save.write_bytes(b'precious synthetic save')
        (jit / 'prod.keys').write_text(PLACEHOLDER_KEYS)
        (jit / 'BOOT0').write_bytes(b'\0' * 16)
        before = snapshot(jit)
        status = self.export('dynarmic', 'build')
        self.record('conflict: reported, nothing changed', not status.get('success') and status.get('conflict')
                    and snapshot(jit) == before and not self.staging_leftovers(), status.get('status', ''))

        # Failure after a complete build: the old package is untouched, no backup is made.
        status = self.export('dynarmic', 'build', conflict='replace', fail_at='validate')
        backups = sorted(p.name for p in self.out.glob(jit_name + '.previous-*'))
        self.record('failure: previous package and saves unchanged', not status.get('success')
                    and snapshot(jit) == before and not backups and not self.staging_leftovers())

        # Replace keeps the old package (and its save) as a backup; nothing carries over.
        status = self.export('dynarmic', 'build', conflict='replace')
        backups = sorted(self.out.glob(jit_name + '.previous-*'))
        new = snapshot(jit)
        self.record('replace: new package holds only this export', status.get('success') and
                    not any(n in new for n in ('prod.keys', 'BOOT0')) and
                    not any(n.startswith('user/nand') for n in new))
        self.record('replace: previous package kept with its save', len(backups) == 1 and
                    snapshot(backups[0]) == before)

        # Keep both: a new folder, the existing one untouched.
        current = snapshot(jit)
        status = self.export('dynarmic', 'build', conflict='keep_both')
        self.record('keep both: new folder', status.get('success') and
                    status.get('output_path', '').endswith(' (2)') and snapshot(jit) == current)

        # Dirty extracted input is refused before anything is produced.
        dirty = self.work / 'DirtyFixture'
        shutil.copytree(self.fixture, dirty)
        (dirty / 'notes.txt').write_text('unrelated file')
        (dirty / 'prod.keys').write_text(PLACEHOLDER_KEYS)
        dirty_out = self.work / 'out-dirty'
        for backend in ('dynarmic', 'hybrid'):
            status = self.export(backend, 'source' if backend == 'hybrid' else 'build', rom=dirty,
                                 output=dirty_out)
            leftovers = [p.name for p in dirty_out.iterdir()] if dirty_out.exists() else []
            self.record('dirty input refused (%s)' % backend, not status.get('success') and not leftovers
                        and 'notes.txt' in status.get('status', '') and 'prod.keys' in status.get('status', ''),
                        status.get('status', ''))

        self.portable_scenarios()

        if a.skip_aot:
            return
        # Source format: the generated C project is kept and marked game-derived.
        status = self.export('hybrid', 'source')
        source_pkg = self.out / 'SynthFixture - Hybrid AOT + JIT'
        self.record('Hybrid Source export succeeds', status.get('success'), status.get('status', ''))
        if status.get('success'):
            self.check_package('Hybrid Source', source_pkg, True, False)
            readme = (source_pkg / 'README_NATIVE_EXPORT.txt').read_text(encoding='utf-8')
            self.record('Source README says it is game-derived', 'game-derived material' in readme and 'no standalone program' in readme)
            shutil.rmtree(source_pkg)

        for backend, name in (('hybrid', 'SynthFixture - Hybrid AOT + JIT'), ('static', 'SynthFixture')):
            status = self.export(backend, 'build')
            package = self.out / name
            self.record('%s Build export succeeds' % backend, status.get('success'), status.get('status', ''))
            if not status.get('success'):
                continue
            self.check_package('%s Build' % backend, package, False, True)
            code, output, log = self.launch(package, name + '.exe', keys=True, timeout=120)
            self.record('launcher: synthetic program runs (%s)' % backend, marker in log, 'exit=%s' % code)
            # A run that only reached the JIT fallback would not show that the image works.
            self.record('launcher: recompiled code runs (%s)' % backend,
                        'covered=true' in log and 'falling back to JIT' not in log)
            moved = self.work / 'moved' / ('Relocated ' + backend)
            shutil.move(str(package), str(moved))
            code, output, log = self.launch(moved, name + '.exe', keys=True, timeout=120)
            self.record('launcher: %s runs after the package is moved' % backend, marker in log,
                        'exit=%s' % code)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suyu-dir', required=True, help='release-like folder with suyu.exe')
    parser.add_argument('--work', required=True, help='new, short work folder')
    parser.add_argument('--port', type=int, default=9761)
    parser.add_argument('--skip-aot', action='store_true', help='JIT and policy scenarios only')
    if os.name != 'nt':
        print('SKIP: the exporter integration test drives a Windows build')
        return 0
    return Run(parser.parse_args()).run()


if __name__ == '__main__':
    sys.exit(main())
