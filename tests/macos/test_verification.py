"""Synthetic regression fixtures; these are not macOS runtime tests."""
import json
import os
import plistlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/macos'))
import verify_bundle as bundle
import verify_startup as startup


class LoaderPaths(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'Folder with spaces/suyu.app'
        self.exe_dir = self.root / 'Contents/MacOS'
        self.image = self.exe_dir / 'suyu'
        self.frameworks = self.root / 'Contents/Frameworks'
        self.frameworks.mkdir(parents=True)
        self.exe_dir.mkdir()
        self.lib = self.frameworks / 'libMoltenVK.dylib'
        self.lib.write_bytes(b'library')

    def resolve(self, value):
        return bundle.resolve_dependency(value, self.image, self.exe_dir,
                                         [self.frameworks], self.root)

    def test_internal_rpath(self):
        self.assertEqual(self.resolve('@rpath/libMoltenVK.dylib'), self.lib.resolve())

    def test_executable_relative(self):
        self.assertEqual(self.resolve('@executable_path/../Frameworks/libMoltenVK.dylib'),
                         self.lib.resolve())

    def test_loader_relative(self):
        self.assertEqual(self.resolve('@loader_path/../Frameworks/libMoltenVK.dylib'),
                         self.lib.resolve())

    def test_system_shared_cache_need_not_exist(self):
        value = '/usr/lib/libSystem.B.dylib'
        self.assertEqual(self.resolve(value), value)

    def test_system_prefix_escape_rejected(self):
        with self.assertRaises(ValueError):
            self.resolve('/usr/lib/../../opt/homebrew/lib/libbad.dylib')

    def test_homebrew_dependency_rejected(self):
        with self.assertRaises(ValueError):
            self.resolve('/opt/homebrew/lib/libMoltenVK.dylib')

    def test_absolute_bundle_dependency_rejected(self):
        with self.assertRaises(ValueError):
            self.resolve(str(self.lib))

    def test_missing_internal_dependency_rejected(self):
        with self.assertRaises(ValueError):
            self.resolve('@rpath/libmissing.dylib')

    def test_external_symlink_rejected(self):
        outside = Path(self.temp.name) / 'external.dylib'
        outside.write_bytes(b'library')
        (self.frameworks / 'escape.dylib').symlink_to(outside)
        with self.assertRaises(ValueError):
            self.resolve('@rpath/escape.dylib')

    def test_otool_paths_preserve_spaces(self):
        text = 'Load command 9\n          cmd LC_RPATH\n      cmdsize 48\n         path @loader_path/../A B (offset 12)\n'
        self.assertEqual(bundle.parse_rpaths(text), ['@loader_path/../A B'])
        libs = 'file:\n\t@rpath/A B.framework/A B (compatibility version 1.0.0, current version 2.0.0)\n'
        self.assertEqual(bundle.parse_dependencies(libs), ['@rpath/A B.framework/A B'])
        identity = 'cmd LC_ID_DYLIB\ncmdsize 64\nname @rpath/A B.framework/A B (offset 24)\n'
        self.assertEqual(bundle.parse_identity(identity), '@rpath/A B.framework/A B')
        self.assertEqual(bundle.parse_identity(text), '')  # Executables have no dylib ID.


class CompleteAudit(unittest.TestCase):
    def setUp(self):
        LoaderPaths.setUp(self)
        self.files = ['Contents/MacOS/suyu', 'Contents/MacOS/suyu-cmd',
                      'Contents/Frameworks/libMoltenVK.dylib',
                      'Contents/PlugIns/platforms/libqcocoa.dylib',
                      'Contents/Frameworks/QtCore.framework/Versions/A/QtCore']
        for name in self.files:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'\xcf\xfa\xed\xfe' + b'fixture')
            path.chmod(0o644)  # Qt framework binaries need not be executable.
        with (self.root / 'Contents/Info.plist').open('wb') as output:
            plistlib.dump({'CFBundleExecutable': 'suyu'}, output)
        self.bad_arch = None
        self.bad_dependency = None

    def fake_tool(self, *args):
        path = Path(args[-1])
        if args[0].endswith('lipo'):
            return 'x86_64' if path == self.bad_arch else 'arm64'
        if '-l' in args:
            return 'cmd LC_RPATH\ncmdsize 48\npath @executable_path/../Frameworks (offset 12)\n'
        if '-L' in args:
            dep = '/opt/homebrew/lib/libbad.dylib' if path == self.bad_dependency else '/usr/lib/libSystem.B.dylib'
            return str(path) + ':\n\t' + dep + ' (compatibility version 1.0.0, current version 1.0.0)\n'
        return str(path) + ':\n'

    def test_framework_without_dylib_suffix_is_audited(self):
        with patch.object(bundle, 'run', self.fake_tool):
            report = bundle.audit(self.root)
        self.assertEqual(report['status'], 'passed')
        self.assertEqual(len(report['images']), 5)

    def test_x86_only_framework_rejected(self):
        self.bad_arch = (self.root / self.files[-1]).resolve()
        with patch.object(bundle, 'run', self.fake_tool), self.assertRaises(ValueError):
            bundle.audit(self.root)

    def test_external_framework_dependency_rejected(self):
        self.bad_dependency = (self.root / self.files[-1]).resolve()
        with patch.object(bundle, 'run', self.fake_tool), self.assertRaises(ValueError):
            bundle.audit(self.root)

    def test_missing_moltenvk_rejected(self):
        self.lib.unlink()
        with patch.object(bundle, 'run', self.fake_tool), self.assertRaises(ValueError):
            bundle.audit(self.root)


class StartupResult(unittest.TestCase):
    def valid(self):
        return {'success': True, 'window_exposed': True, 'surface_present_modes': True,
                'screenshot_saved': True, 'qt_platform': 'cocoa',
                'vulkan_devices': ['Synthetic Apple GPU'], 'games_tested': False}

    def text(self, record):
        return 'SUYU_STARTUP_CHECK=' + json.dumps(record) + '\n'

    def test_explicit_success_record(self):
        self.assertTrue(startup.parse_check(self.text(self.valid()))['success'])

    def test_process_liveness_alone_is_not_success(self):
        with self.assertRaises(ValueError):
            startup.parse_check('suyu started\n')

    def test_failure_record_rejected(self):
        for name in ('success', 'window_exposed', 'surface_present_modes', 'screenshot_saved'):
            with self.subTest(field=name), self.assertRaises(ValueError):
                record = self.valid()
                record[name] = False
                startup.parse_check(self.text(record))

    def test_offscreen_qt_rejected(self):
        record = self.valid()
        record['qt_platform'] = 'offscreen'
        with self.assertRaises(ValueError):
            startup.parse_check(self.text(record))

    def test_empty_device_list_rejected(self):
        record = self.valid()
        record['vulkan_devices'] = []
        with self.assertRaises(ValueError):
            startup.parse_check(self.text(record))

    def test_duplicate_results_rejected(self):
        with self.assertRaises(ValueError):
            startup.parse_check(self.text(self.valid()) * 2)

    def test_loaded_image_paths_with_spaces(self):
        text = 'dyld[123]: <AB-CD> /private/tmp/A B/suyu.app/Contents/Frameworks/libMoltenVK.dylib\n'
        self.assertEqual(startup.loaded_images(text),
                         ['/private/tmp/A B/suyu.app/Contents/Frameworks/libMoltenVK.dylib'])

    def test_loader_overrides_removed_from_environment(self):
        with patch.dict(os.environ, {'LIBVULKAN_PATH': '/bad', 'DYLD_LIBRARY_PATH': '/bad',
                                     'QT_PLUGIN_PATH': '/bad', 'SUYU_RECOMP_DIR': '/bad'}):
            env = startup.clean_environment(Path('/test'), Path('/evidence'))
        for name in ('LIBVULKAN_PATH', 'DYLD_LIBRARY_PATH', 'QT_PLUGIN_PATH', 'SUYU_RECOMP_DIR'):
            self.assertNotIn(name, env)
        self.assertEqual(env['QT_QPA_PLATFORM'], 'cocoa')


if __name__ == '__main__':
    unittest.main()
