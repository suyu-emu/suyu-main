"""Dependency-source collector and third-party notices tests. No network access.

The plan is built from the repository's real cpmfile.json files, which is what
--dry-run prints; downloads are never started here.
"""
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/package_policy'))
import collect_dependency_sources as collector  # noqa: E402
import third_party_notices as notices  # noqa: E402

PLAN = collector.build_plan()
ITEMS = PLAN['items']


def find(name):
    return next(i for i in ITEMS if name in i['names'])


class PlanTests(unittest.TestCase):
    def test_source_entry_url_and_hash(self):
        fmt = find('fmt')
        self.assertEqual(fmt['url'], 'https://github.com/fmtlib/fmt/archive/12.1.0.tar.gz')
        self.assertEqual(fmt['kind'], 'source')
        cpm = json.loads((ROOT / 'cpmfile.json').read_text(encoding='utf-8'))
        self.assertEqual(fmt['sha512'], cpm['fmt']['hash'])

    def test_every_root_entry_is_planned_or_excluded(self):
        cpm = json.loads((ROOT / 'cpmfile.json').read_text(encoding='utf-8'))
        planned = {n for i in ITEMS for n in i['names']}
        excluded = {e['name'] for e in PLAN['excluded']}
        for name in cpm:
            self.assertTrue(name in planned or name in excluded, name)

    def test_artifact_entries(self):
        boost = find('boost')
        self.assertEqual(boost['kind'], 'artifact-source')
        self.assertEqual(boost['url'], 'https://github.com/boostorg/boost/releases/download/'
                                       'boost-1.90.0/boost-1.90.0-cmake.tar.xz')
        self.assertTrue(boost['sha512'])
        vvl = find('vulkan-validation-layers')
        self.assertEqual(vvl['kind'], 'artifact-source')
        self.assertEqual(vvl['version'], 'vulkan-sdk-1.4.341.0')  # %NUMERIC_VERSION% substituted
        self.assertTrue(vvl['url'].endswith('/Vulkan-ValidationLayers/archive/vulkan-sdk-1.4.341.0.tar.gz'))
        self.assertIsNone(vvl['sha512'])  # the JSON hash is for the prebuilt zip, not this file
        self.assertTrue(vvl['artifact_sha512'])

    def test_other_git_host(self):
        tzdb = find('tzdb')
        self.assertEqual(tzdb['git_host'], 'git.eden-emu.dev')
        self.assertEqual(tzdb['url'], 'https://git.eden-emu.dev/eden-emu/tzdb_to_nx/archive/230326.tar.gz')

    def test_ci_prebuilt_entries_have_scripts_and_upstream(self):
        scripts = find('ffmpeg-ci')
        self.assertEqual(scripts['url'], 'https://github.com/crueter-ci/FFmpeg/archive/v8.0.1-c7b5f1537d.tar.gz')
        self.assertEqual(scripts['kind'], 'ci-prebuilt')
        self.assertIsNone(scripts['sha512'])
        expected = {
            'openssl-ci-upstream': 'https://github.com/openssl/openssl/archive/11b7b6ea3b.tar.gz',
            'sdl3-ci-upstream': 'https://github.com/libsdl-org/SDL/archive/d57c3b685c.tar.gz',
            'ffmpeg-ci-upstream': 'https://github.com/FFmpeg/FFmpeg/archive/c7b5f1537d.tar.gz',
            'sirit-ci-upstream': 'https://github.com/eden-emulator/sirit/archive/v1.0.5.tar.gz',
        }
        for name, url in expected.items():
            self.assertEqual(find(name)['url'], url, name)
        for name in ('ffmpeg-ci', 'openssl-ci', 'sdl3-ci', 'sirit-ci'):
            self.assertIn('build-scripts', find(name)['roles'])

    def test_duplicate_urls_are_downloaded_once(self):
        urls = [i['url'] for i in ITEMS]
        self.assertEqual(len(urls), len(set(urls)))
        self.assertEqual(len({i['file'] for i in ITEMS}), len(ITEMS))
        self.assertEqual(len(find('biscuit')['origin']), 2)  # root and dynarmic cpmfile
        self.assertIn('ffmpeg-ci-upstream', find('ffmpeg')['names'])

    def test_dynarmic_and_qt_common_entries(self):
        mcl = find('mcl')
        self.assertEqual(mcl['url'], 'https://github.com/azahar-emu/mcl/archive/7b08d83418.tar.gz')
        self.assertTrue(mcl['sha512'])
        self.assertTrue(any(i['url'].endswith('crueter/quazip-qt6/archive/f838774d63.tar.gz') for i in ITEMS))

    def test_exclusions_have_reasons_and_are_not_planned(self):
        self.assertEqual({e['name'] for e in PLAN['excluded']}, {'llvm-mingw'})
        for entry in PLAN['excluded']:
            self.assertTrue(entry['reason'])
        self.assertFalse(any('llvm-mingw' in i['names'] for i in ITEMS))

    def test_only_https_on_allowed_hosts(self):
        for entry in ITEMS:
            host = entry['url'].split('/')[2]
            self.assertTrue(entry['url'].startswith('https://'))
            self.assertIn(host, collector.ALLOWED_HOSTS)

    def test_unknown_ci_entry_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            for rel, _ in collector.CPMFILES:
                path = Path(temp) / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('{}', encoding='utf-8')
            (Path(temp) / 'cpmfile.json').write_text(json.dumps(
                {'mystery-ci': {'ci': True, 'repo': 'x/y', 'version': '1.0-abc'}}), encoding='utf-8')
            with self.assertRaises(SystemExit):
                collector.build_plan(temp)

    def test_disallowed_host_is_not_downloaded(self):
        with self.assertRaises(RuntimeError):
            collector.download({'url': 'https://example.invalid/x.tar.gz'}, Path(tempfile.gettempdir()) / 'never')

    def test_dry_run_prints_json_and_only_filters(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(collector.main(['--dry-run', '--only', 'zlib,sirit-ci']), 0)
        plan = json.loads(out.getvalue())
        self.assertEqual(sorted(i['names'][0] for i in plan['items']), ['sirit', 'zlib'])


class NoticesTests(unittest.TestCase):
    def test_generated_notices_list_dependencies_and_submodules(self):
        text = notices.generate()
        self.assertIn('THIRD-PARTY NOTICES', text)
        self.assertIn('https://github.com/fmtlib/fmt', text)
        self.assertIn('12.1.0', text)
        self.assertIn('MIT (LICENSES/MIT.txt)', text)
        self.assertIn('see the dependency-sources asset', text)
        self.assertIn('externals/breakpad', text)
        self.assertIn('https://github.com/lsalzman/enet.git', text)
        self.assertIn('crueter-ci/FFmpeg', text)
        self.assertNotIn('llvm-mingw', text)
        for entry in ITEMS:
            if entry['kind'] != 'ci-prebuilt':
                self.assertIn(entry['names'][0], text)

    def test_license_hints_point_at_existing_files_or_say_so(self):
        for name, ids in notices.LICENSE_HINTS.items():
            for spdx in ids:
                text = notices.license_text(name)
                exists = (ROOT / 'LICENSES' / (spdx + '.txt')).is_file()
                self.assertEqual('LICENSES/%s.txt' % spdx in text, exists, (name, spdx))

    def test_package_dir_gets_notices_license_and_texts(self):
        with tempfile.TemporaryDirectory() as temp:
            pkg = Path(temp) / 'pkg'
            self.assertEqual(notices.main(['--package-dir', str(pkg)]), 0)
            self.assertTrue((pkg / 'THIRD-PARTY-NOTICES.txt').read_text(encoding='utf-8').startswith('THIRD-PARTY'))
            self.assertEqual((pkg / 'LICENSE.txt').read_bytes(), (ROOT / 'LICENSE.txt').read_bytes())
            copied = sorted(p.name for p in (pkg / 'LICENSES').iterdir())
            self.assertEqual(copied, sorted(p.name for p in (ROOT / 'LICENSES').iterdir()))
            self.assertIn('MIT.txt', copied)


if __name__ == '__main__':
    unittest.main()
