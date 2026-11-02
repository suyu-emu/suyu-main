"""Release archive scanner tests. Archives are synthetic and built in temp dirs.

Nothing here is real key material, firmware or game data: key text uses an
obviously synthetic sequence and every payload is a few placeholder bytes.
"""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
import warnings
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'tools/package_policy/scan_release.py'
sys.path.insert(0, str(TOOL.parent))
import scan_release  # noqa: E402

POLICY = scan_release.load_policy()
RULES = scan_release.Rules(POLICY)
SYNTHETIC_HEX = '00112233445566778899aabbccddeeff'
KEY_TEXT = ('master_key_00 = ' + SYNTHETIC_HEX + '\n').encode()
TITLE_KEY_TEXT = (SYNTHETIC_HEX + ' = ' + SYNTHETIC_HEX[::-1] + '\n').encode()


def zip_bytes(members, symlinks=()):
    """members: {name: bytes}; symlinks: {name: target}. Names are stored verbatim."""
    buffer = io.BytesIO()
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
            items = members.items() if isinstance(members, dict) else members
            for name, data in items:
                info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
                info.filename = name  # ZipInfo would rewrite backslashes on Windows
                archive.writestr(info, data)
            for name, target in dict(symlinks).items():
                info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(info, target)
    return buffer.getvalue()


def tar_bytes(members, links=(), dirs=('.',)):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w:gz') as archive:
        for name in dirs:
            info = tarfile.TarInfo(name)
            info.type = tarfile.DIRTYPE
            archive.addfile(info)
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
        for name, kind, target in links:
            info = tarfile.TarInfo(name)
            info.type = kind
            info.linkname = target
            archive.addfile(info)
    return buffer.getvalue()


def kit_files(**overrides):
    files = {'inputs/0123-host.obj': b'host object placeholder', 'strict.cmake': b'set(KIT_OBJECTS)\n',
             'hybrid.cmake': b'set(KIT_OBJECTS)\n', 'revision.txt': b'suyu-aot-kit-test\n'}
    files.update(overrides)
    return files


def kit_members(files=None, manifest_updates=None, listed=None):
    files = kit_files() if files is None else files
    manifest = {
        'revision': 'suyu-aot-kit-test',
        'policy_version': POLICY['policy_version'],
        'producer_source_revision': 'a' * 40,
        'files': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()} if listed is None else listed,
    }
    manifest.update(manifest_updates or {})
    members = {'export-build-kit/' + name: data for name, data in files.items()}
    members['export-build-kit/manifest.json'] = json.dumps(manifest).encode()
    return members


def windows_members(**extra):
    # vc_redist.x64.exe: windeployqt adds the Visual C++ runtime installer on CI.
    members = {'suyu.exe': b'MZ placeholder', 'suyu-cmd.exe': b'MZ placeholder', 'Qt6Core.dll': b'MZ placeholder',
               'platforms/qwindows.dll': b'MZ placeholder', 'LICENSE.txt': b'GPL text placeholder',
               'vc_redist.x64.exe': b'MZ placeholder'}
    members.update(kit_members())
    members.update(extra)
    return members


class ScanCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)

    def write(self, name, data):
        path = self.dir / name
        path.write_bytes(data)
        return path

    def scan(self, name, data, kind, rules=RULES):
        return scan_release.scan_archive(self.write(name, data), kind, rules)

    @staticmethod
    def rules_of(result, member=None):
        return {f['rule'] for f in result['findings'] if member is None or f['member'] == member}

    def assertClean(self, result):
        self.assertEqual(result['findings'], [])


class WindowsTests(ScanCase):
    def test_clean_archive_passes_and_keeps_dependencies_and_license(self):
        members = windows_members(**{'vcruntime140.dll': b'MZ', 'styles/qmodernwindowsstyle.dll': b'MZ',
                                     'imageformats/qjpeg.dll': b'MZ', 'plugins/tls/qopensslbackend.dll': b'MZ'})
        result = self.scan('suyu-windows-x86_64.zip', zip_bytes(members), 'windows')
        self.assertClean(result)
        self.assertGreater(result['members_scanned'], 10)
        with zipfile.ZipFile(self.dir / 'suyu-windows-x86_64.zip') as archive:
            names = set(archive.namelist())
        self.assertIn('LICENSE.txt', names)
        self.assertIn('vcruntime140.dll', names)

    def check_rejected(self, member, data, rule, kind='windows', prefix='plugins/'):
        name = prefix + member
        archive = zip_bytes(windows_members(**{name: data}))
        result = self.scan('candidate.zip', archive, kind)
        self.assertIn(rule, self.rules_of(result, name), (member, result['findings']))
        return result

    def test_restricted_payloads_are_rejected_with_the_right_rule(self):
        cases = [
            ('prod.keys', b'placeholder', 'key-name'),
            ('PROD.KEYS. ', b'placeholder', 'key-name'),
            ('title.keys_autogenerated', b'placeholder', 'key-name'),
            # Tickets hold (encrypted) title keys; the store suyu keeps them in is NAND data.
            ('0100000000F02001000000000000000A.tik', b'placeholder', 'key-name'),
            ('portable/system/tickets/readme.txt', b'placeholder', 'firmware'),
            ('notes.txt', KEY_TEXT, 'key-text'),
            ('title-notes.txt', TITLE_KEY_TEXT, 'key-text'),
            ('data.bin', b'PFS0' + bytes(64), 'signature'),
            ('hfs.dat', b'HFS0' + bytes(64), 'signature'),
            ('module.dat', b'NSO0' + bytes(64), 'signature'),
            ('cart.dat', bytes(0x100) + b'HEAD' + bytes(64), 'signature'),
            ('homebrew.dat', bytes(16) + b'NRO0' + bytes(64), 'signature'),
            ('table.dat', bytes(16384) + b'NCZSECTN', 'signature'),
            ('BOOT0', b'placeholder', 'firmware'),
            ('nand/system/Contents/registered/x.nca', b'placeholder', 'firmware'),
            ('game.nsp', b'placeholder', 'game-container'),
            ('README_NATIVE_EXPORT.txt', b'placeholder', 'export-marker'),
            ('export-package.json', b'{}', 'export-marker'),
            ('user/nand/user/save/0000000000000000/x', b'placeholder', 'user-data'),
            ('qt-config.ini', b'placeholder', 'user-data'),
        ]
        for member, data, rule in cases:
            with self.subTest(member=member):
                self.check_rejected(member, data, rule)

    def test_nested_archives_are_scanned_and_opaque_ones_rejected(self):
        inner = zip_bytes({'prod.keys': b'placeholder'})
        result = self.scan('c.zip', zip_bytes(windows_members(**{'plugins/inner.zip': inner})), 'windows')
        self.assertEqual(self.rules_of(result, 'plugins/inner.zip!prod.keys'), {'key-name'})
        deep = zip_bytes({'a.zip': zip_bytes({'b.zip': zip_bytes({'c.zip': zip_bytes({'x': b'y'})})})})
        result = self.scan('c.zip', zip_bytes(windows_members(**{'plugins/deep.zip': deep})), 'windows')
        self.assertIn('nested-archive', self.rules_of(result))
        for opaque in ['data.7z', 'data.rar', 'data.xz', 'data.zst', 'data.cab', 'data.msi', 'data.lz4',
                       'data.bz2', 'data.gz', 'data.iso']:
            with self.subTest(opaque=opaque):
                self.check_rejected(opaque, b'placeholder', 'nested-archive')
        tarball = tar_bytes({'notes.txt': KEY_TEXT})
        result = self.scan('c.zip', zip_bytes(windows_members(**{'plugins/inner.tar.gz': tarball})), 'windows')
        self.assertIn('key-text', self.rules_of(result, 'plugins/inner.tar.gz!notes.txt'))

    def test_unsafe_paths_links_and_layout(self):
        names = ['../evil', 'plugins/../../evil', '/abs/evil', 'C:/evil.dll']
        if os.sep == '/':  # zipfile rewrites backslashes to '/' when reading on Windows
            names.append('plugins' + chr(92) + 'evil.dll')
        for name in names:
            with self.subTest(name=name):
                result = self.scan('c.zip', zip_bytes(windows_members(**{name: b'placeholder'})), 'windows')
                self.assertIn('path-unsafe', self.rules_of(result), result['findings'])
        result = self.scan('c.zip', zip_bytes([('suyu.exe', b'a'), ('SUYU.EXE', b'b')]), 'windows')
        self.assertIn('path-unsafe', self.rules_of(result))
        result = self.scan('c.zip', zip_bytes(windows_members(), symlinks={'plugins/link.dll': 'suyu.exe'}), 'windows')
        self.assertIn('link', self.rules_of(result, 'plugins/link.dll'))
        result = self.scan('c.zip', zip_bytes(windows_members(**{'surprise.txt': b'placeholder'})), 'windows')
        self.assertEqual(self.rules_of(result, 'surprise.txt'), {'unexpected'})

    def test_kit_manifest_enforcement(self):
        def scan(members):
            return self.scan('kit.zip', zip_bytes(members), 'windows')

        base = {k: v for k, v in windows_members().items() if not k.startswith('export-build-kit/')}
        # Extra file that the manifest does not list.
        members = dict(base, **kit_members())
        members['export-build-kit/inputs/stale.obj'] = b'stale'
        result = scan(members)
        self.assertEqual(self.rules_of(result, 'export-build-kit/inputs/stale.obj'), {'kit'})
        # Hash mismatch.
        members = dict(base, **kit_members())
        members['export-build-kit/strict.cmake'] = b'tampered'
        self.assertEqual(self.rules_of(scan(members), 'export-build-kit/strict.cmake'), {'kit'})
        # Listed but absent.
        listed = {n: hashlib.sha256(d).hexdigest() for n, d in kit_files().items()}
        listed['inputs/gone.obj'] = '0' * 64
        result = scan(dict(base, **kit_members(listed=listed)))
        self.assertEqual(self.rules_of(result, 'export-build-kit/inputs/gone.obj'), {'kit'})
        # Missing manifest, wrong policy version, empty producer revision.
        no_manifest = {k: v for k, v in dict(base, **kit_members()).items() if not k.endswith('manifest.json')}
        self.assertIn('kit', self.rules_of(scan(no_manifest)))
        self.assertIn('kit', self.rules_of(scan(dict(base, **kit_members(manifest_updates={'policy_version': 'old'})))))
        self.assertIn('kit', self.rules_of(scan(dict(base, **kit_members(manifest_updates={'producer_source_revision': ' '})))))
        self.assertIn('kit', self.rules_of(scan(dict(base, **{'export-build-kit/manifest.json': b'not json'}))))
        # Untouched kit is fine.
        self.assertClean(scan(dict(base, **kit_members())))


class OtherKindTests(ScanCase):
    def test_linux_tar_with_dot_slash_names_passes(self):
        data = tar_bytes({'./suyu': b'ELF', './suyu-cmd': b'ELF', './LICENSE.txt': b'GPL'})
        self.assertClean(self.scan('suyu-linux-x86_64.tar.gz', data, 'linux'))
        result = self.scan('bad.tar.gz', tar_bytes({'./suyu': b'ELF', './extra': b'x'}), 'linux')
        self.assertEqual(self.rules_of(result, 'extra'), {'unexpected'})

    def test_tar_links_are_rejected(self):
        members = {'./suyu': b'ELF', './suyu-cmd': b'ELF', './LICENSE.txt': b'GPL'}
        for kind in [tarfile.SYMTYPE, tarfile.LNKTYPE]:
            data = tar_bytes(members, links=[('./sneaky', kind, 'suyu')])
            result = self.scan('l.tar.gz', data, 'linux')
            self.assertIn('link', self.rules_of(result, 'sneaky'))

    def macos_members(self):
        fw = 'suyu.app/Contents/Frameworks/QtCore.framework/'
        return ({'suyu.app/Contents/MacOS/suyu': b'MACHO', 'LICENSE.txt': b'GPL',
                 fw + 'Versions/A/QtCore': b'MACHO', fw + 'Versions/A/Resources/Info.plist': b'plist'},
                {fw + 'Versions/Current': 'A', fw + 'QtCore': 'Versions/Current/QtCore',
                 fw + 'Resources': 'Versions/Current/Resources'})

    def test_macos_framework_symlinks(self):
        members, links = self.macos_members()
        self.assertClean(self.scan('suyu-macos-arm64.zip', zip_bytes(members, links), 'macos'))
        fw = 'suyu.app/Contents/Frameworks/QtCore.framework/'
        bad_links = {
            'escape': {fw + 'evil': '../../../MacOS/suyu'},
            'absolute': {fw + 'evil': '/etc/hosts'},
            'outside framework': {'suyu.app/Contents/MacOS/link': 'suyu'},
            'other framework': {fw + 'evil': '../QtGui.framework/QtGui'},
            'missing target': {fw + 'evil': 'Versions/A/Nope'},
        }
        for name, extra in bad_links.items():
            with self.subTest(name):
                result = self.scan('bad.zip', zip_bytes(members, dict(links, **extra)), 'macos')
                self.assertIn('link', self.rules_of(result), result['findings'])
        result = self.scan('linux-link.zip', zip_bytes(members, links), 'windows')
        self.assertIn('link', self.rules_of(result))

    def test_apk_and_libretro_and_source_layouts(self):
        apk = zip_bytes({'AndroidManifest.xml': b'x', 'classes.dex': b'dex', 'classes2.dex': b'dex',
                         'resources.arsc': b'x', 'res/layout/a.xml': b'x', 'lib/arm64-v8a/libsuyu.so': b'ELF',
                         'META-INF/CERT.RSA': b'x', 'assets/a.txt': b'x', 'kotlin/kotlin.kotlin_builtins': b'x',
                         'app.properties': b'x', 'DebugProbesKt.bin': b'x'})
        self.assertClean(self.scan('suyu.apk', apk, 'android-apk'))
        bad = zip_bytes({'AndroidManifest.xml': b'x', 'lib/x86_64/libsuyu.so': b'ELF'})
        self.assertIn('unexpected', self.rules_of(self.scan('bad.apk', bad, 'android-apk')))

    def test_apk_resource_names_and_bundled_library_data(self):
        import gzip
        base = [('AndroidManifest.xml', b'x'), ('classes.dex', b'dex')]
        # The resource shrinker emits names that differ only in case.
        shrunk = base + [('res/0C.xml', b'x'), ('res/0c.xml', b'y'),
                         ('kotlin-tooling-metadata.json', b'{}'),
                         ('okhttp3/internal/publicsuffix/NOTICE', b'x'),
                         ('okhttp3/internal/publicsuffix/publicsuffixes.gz', gzip.compress(b'com\n')),
                         ('org/commonmark/internal/util/entities.properties', b'x')]
        self.assertClean(self.scan('shrunk.apk', zip_bytes(shrunk), 'android-apk'))
        exact = base + [('res/0c.xml', b'x'), ('res/0c.xml', b'y')]
        self.assertIn('path-unsafe', self.rules_of(self.scan('dup.apk', zip_bytes(exact), 'android-apk')))
        # Archives that get unpacked still reject names that collide ignoring case.
        cased = zip_bytes([('suyu.exe', b'MZ'), ('LICENSE.txt', b'x'), ('license.txt', b'y')])
        self.assertIn('path-unsafe', self.rules_of(self.scan('cased.zip', cased, 'windows')))

    def test_libretro_and_source_layouts(self):
        self.assertClean(self.scan('l.tar.gz', tar_bytes({'./suyu_libretro.so': b'ELF', './LICENSE.txt': b'x'}), 'libretro-linux'))
        self.assertClean(self.scan('w.zip', zip_bytes({'suyu_libretro.dll': b'MZ', 'LICENSE.txt': b'x'}), 'libretro-windows'))
        self.assertClean(self.scan('m.zip', zip_bytes({'suyu_libretro.dylib': b'M', 'LICENSE.txt': b'x',
                                                       'suyu_libretro_libs/libx.dylib': b'M'}), 'libretro-macos'))
        self.assertClean(self.scan('a.zip', zip_bytes({'suyu_libretro_android.so': b'ELF', 'LICENSE.txt': b'x'}), 'libretro-android'))
        source = tar_bytes({'suyu-v1.2.3-source/src/main.cpp': b'int main(){}', 'suyu-v1.2.3-source/README.md': b'hi'}, dirs=())
        self.assertClean(self.scan('suyu-v1.2.3-source.tar.gz', source, 'source'))
        two = tar_bytes({'suyu-v1.2.3-source/a.cpp': b'x', 'other/b.cpp': b'y'}, dirs=())
        self.assertIn('unexpected', self.rules_of(self.scan('two.tar.gz', two, 'source')))
        keyed = tar_bytes({'suyu-v1.2.3-source/docs/notes.txt': KEY_TEXT}, dirs=())
        self.assertIn('key-text', self.rules_of(self.scan('k.tar.gz', keyed, 'source')))

    def test_license_texts_and_notices_are_allowed_in_every_package_kind(self):
        extra = {'LICENSE.txt': b'x', 'LICENSES/MIT.txt': b'MIT', 'THIRD-PARTY-NOTICES.txt': b'notices'}
        self.assertClean(self.scan('l.tar.gz', tar_bytes({'./suyu': b'ELF', './suyu-cmd': b'ELF', **{'./' + k: v for k, v in extra.items()}}), 'linux'))
        self.assertClean(self.scan('w.zip', zip_bytes(windows_members(**extra)), 'windows'))
        self.assertClean(self.scan('m.zip', zip_bytes({'suyu.app/Contents/Info.plist': b'x', **extra}), 'macos'))
        for kind, core in (('libretro-linux', 'suyu_libretro.so'), ('libretro-windows', 'suyu_libretro.dll'),
                           ('libretro-macos', 'suyu_libretro.dylib'), ('libretro-android', 'suyu_libretro_android.so')):
            self.assertClean(self.scan(kind + '.zip', zip_bytes({core: b'bin', **extra}), kind))
        result = self.scan('bad.zip', zip_bytes({'suyu_libretro.so': b'bin', 'LICENSES/sub/MIT.txt': b'x',
                                                 'LICENSES/run.exe': b'x'}), 'libretro-linux')
        self.assertEqual(self.rules_of(result, 'LICENSES/sub/MIT.txt'), {'unexpected'})
        self.assertEqual(self.rules_of(result, 'LICENSES/run.exe'), {'unexpected'})

    def test_dependency_sources_layout_and_nested_archives(self):
        top = 'suyu-v1.2.3-dependency-sources/'
        upstream = tar_bytes({'zlib-1.3/zlib.h': b'/* header */', 'zlib-1.3/doc/notes.txt': b'notes'}, dirs=())
        members = {top + 'MANIFEST.json': b'{}', top + 'README.txt': b'readme', top + 'madler-zlib-v1.3.tar.gz': upstream,
                   top + 'boost.tar.xz': self.xz_tar({'boost/a.hpp': b'x'})}
        self.assertClean(self.scan('suyu-v1.2.3-dependency-sources.tar', self.plain_tar(members), 'dependency-sources'))
        bad = dict(members)
        bad[top + 'extra.bin'] = b'x'
        bad[top + 'sub/inner.tar.gz'] = upstream
        bad['other/file.tar.gz'] = upstream
        result = self.scan('bad.tar', self.plain_tar(bad), 'dependency-sources')
        self.assertEqual(self.rules_of(result, top + 'extra.bin'), {'unexpected'})
        self.assertEqual(self.rules_of(result, top + 'sub/inner.tar.gz'), {'unexpected'})
        self.assertIn('unexpected', self.rules_of(result, 'other/file.tar.gz'))

    def test_dependency_sources_nested_content_is_still_scanned(self):
        top = 'suyu-v1.2.3-dependency-sources/'
        dirty = tar_bytes({'lib-1/docs/notes.txt': KEY_TEXT, 'lib-1/data.zst': b'zstd', 'lib-1/link': b''}, dirs=(),
                          links=[('lib-1/sym', tarfile.SYMTYPE, 'data.zst')])
        xz = self.xz_tar({'boost/notes.txt': KEY_TEXT})
        result = self.scan('d.tar', self.plain_tar({top + 'lib.tar.gz': dirty, top + 'boost.tar.xz': xz}), 'dependency-sources')
        self.assertEqual(self.rules_of(result, top + 'lib.tar.gz!lib-1/docs/notes.txt'), {'key-text'})
        self.assertEqual(self.rules_of(result, top + 'lib.tar.gz!lib-1/data.zst'), {'nested-archive'})
        self.assertEqual(self.rules_of(result, top + 'lib.tar.gz!lib-1/sym'), {'link'})
        self.assertEqual(self.rules_of(result, top + 'boost.tar.xz!boost/notes.txt'), {'key-text'})

    def test_dependency_sources_exceptions_are_exact_and_scoped(self):
        entries = [e for e in POLICY['approved_exceptions'] if 'dependency-sources' in e['kinds']]
        self.assertTrue(entries)
        for entry in entries:
            self.assertEqual(entry['kinds'], ['dependency-sources'])
            self.assertTrue(entry['path_regex'].startswith('suyu-[^/]+-dependency-sources/'))
            self.assertIn('!', entry['path_regex'])  # always inside one named upstream archive
            # key-name only for the documentation file checked in the shipped-exceptions test.
            self.assertLessEqual(set(entry['rules']), {'user-data', 'link', 'nested-archive', 'key-name'})
        top = 'suyu-v1.2.3-dependency-sources/'
        other = tar_bytes({'zstd-1/tests/golden-decompression/other.zst': b'zstd'}, dirs=())
        result = self.scan('e.tar', self.plain_tar({top + 'facebook-zstd-b8d6101fba.tar.gz': other}), 'dependency-sources')
        self.assertEqual(self.rules_of(result), {'nested-archive'})

    @staticmethod
    def plain_tar(members):
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode='w') as archive:
            for name, data in members.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
        return buffer.getvalue()

    @staticmethod
    def xz_tar(members):
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode='w:xz') as archive:
            for name, data in members.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
        return buffer.getvalue()

    def test_large_files_are_only_partially_read(self):
        big = b'A' * (3 << 20) + KEY_TEXT  # key text beyond the scan limit of a large file is not read
        result = self.scan('big.zip', zip_bytes(windows_members(**{'plugins/big.dat': big})), 'windows')
        self.assertNotIn('key-text', self.rules_of(result))
        small_late = b'A' * 100000 + bytes([10]) + KEY_TEXT  # under 1 MiB: scanned in full
        result = self.scan('late.zip', zip_bytes(windows_members(**{'plugins/late.txt': small_late})), 'windows')
        self.assertIn('key-text', self.rules_of(result))

    def test_plain_text_with_a_magic_at_its_offset_is_not_a_container(self):
        text = b'#' * 256 + b'HEAD_REF and more build script text' + bytes([10])
        result = self.scan('t.zip', zip_bytes(windows_members(**{'plugins/portfile.cmake': text})), 'windows')
        self.assertNotIn('signature', self.rules_of(result))
        binary = bytes(range(1, 128)) * 2 + bytes(2) + b'HEAD'  # non-text bytes before the offset
        result = self.scan('b.zip', zip_bytes(windows_members(**{'plugins/portfile.cmake': binary})), 'windows')
        self.assertIn('signature', self.rules_of(result))

    def test_shipped_exceptions_are_exact_and_do_not_cover_containers(self):
        for entry in POLICY['approved_exceptions']:
            self.assertTrue(entry['reason'] and entry['provenance'], entry)
            self.assertNotIn('.*', entry['path_regex'])
            self.assertFalse(set(entry['rules']) & {'key-text', 'firmware', 'game-container', 'signature'})
            # A name-only key match may be waived for one documentation file per exact
            # path, never for key-shaped contents, and never outside the dependency sources.
            if 'key-name' in entry['rules']:
                self.assertEqual(entry['rules'], ['key-name'])
                self.assertEqual(entry['kinds'], ['dependency-sources'])
                self.assertTrue(entry['path_regex'].endswith('/doc/HOWTO/keys\.txt'), entry)

    def test_unreadable_archive_is_a_finding(self):
        result = self.scan('broken.zip', b'not a zip at all', 'windows')
        self.assertIn('unreadable', self.rules_of(result))


class ExceptionAndCliTests(ScanCase):
    def test_approved_exception_is_narrow(self):
        policy = copy.deepcopy(POLICY)
        policy['approved_exceptions'] = [{
            'kinds': ['source'], 'path_regex': r'suyu-v1-source/vendor/testdata/vector\.bin',
            'rules': ['signature'], 'reason': 'synthetic test vector', 'provenance': 'unit test'}]
        rules = scan_release.Rules(policy)
        members = {'suyu-v1-source/vendor/testdata/vector.bin': b'PFS0' + bytes(8),
                   'suyu-v1-source/vendor/testdata/other.bin': b'PFS0' + bytes(8)}
        result = self.scan('s.tar.gz', tar_bytes(members, dirs=()), 'source', rules)
        self.assertEqual([f['member'] for f in result['findings']], ['suyu-v1-source/vendor/testdata/other.bin'])
        result = self.scan('s2.tar.gz', tar_bytes(members, dirs=()), 'source')
        self.assertEqual(len(result['findings']), 2)

    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True)

    def test_cli_exit_codes_and_report_never_contain_key_text(self):
        clean = self.write('suyu-windows-x86_64.zip', zip_bytes(windows_members()))
        bad_members = windows_members(**{'plugins/notes.txt': KEY_TEXT + TITLE_KEY_TEXT})
        bad = self.write('bad-windows.zip', zip_bytes(bad_members))
        report = self.dir / 'report.json'
        result = self.run_cli('--kind', 'windows', '--report', str(report), str(clean))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.run_cli('--kind', 'windows', '--report', str(report), str(clean), str(bad))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        text = report.read_text()
        data = json.loads(text)
        self.assertEqual(data['policy_version'], POLICY['policy_version'])
        self.assertEqual([a['path'] for a in data['archives']], ['suyu-windows-x86_64.zip', 'bad-windows.zip'])
        self.assertEqual(data['archives'][0]['findings'], [])
        self.assertIn('key-text', {f['rule'] for f in data['archives'][1]['findings']})
        for output in (text, result.stdout, result.stderr):
            self.assertNotIn(SYNTHETIC_HEX, output)
            self.assertNotIn(SYNTHETIC_HEX[::-1], output)
            self.assertNotIn(self.temp.name, output)
        self.assertEqual(self.run_cli('--kind', 'nonsense', str(clean)).returncode, 2)
        self.assertEqual(self.run_cli('--kind', 'windows', str(self.dir / 'missing.zip')).returncode, 2)
        self.assertEqual(self.run_cli('--kind', 'windows').returncode, 2)


if __name__ == '__main__':
    unittest.main()
