"""The exporter's C++ policy tables must say what tools/package_policy/policy.json says.

Release tooling reads policy.json; the exporter compiles its own tables. This keeps
the two from drifting apart, and checks that every setting an exported package may
carry is a real setting in a category that is allowed to travel.
"""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
POLICY = json.loads((ROOT / 'tools/package_policy/policy.json').read_text(encoding='utf-8'))
CPP = (ROOT / 'src/common/package_policy.cpp').read_text(encoding='utf-8')
HEADER = (ROOT / 'src/common/package_policy.h').read_text(encoding='utf-8')
GAPS = (ROOT / 'src/core/arm/recomp/recomp_gaps.h').read_text(encoding='utf-8')
SETTINGS = (ROOT / 'src/common/settings.h').read_text(encoding='utf-8')


def cpp_array(name):
    match = re.search(r'constexpr[^\n]*\b' + name + r'\b[^{]*\{(.*?)\};', CPP, re.S)
    if not match:
        raise AssertionError('table not found: ' + name)
    body = re.sub(r'//[^\n]*', '', match[1])
    raw = re.findall(r'R"\((.*?)\)"', body)
    return raw if raw else re.findall(r'"((?:[^"\\]|\\.)*)"', body)


def alternation(pattern):
    """The names in the first group after '^\s*(' of a key-text pattern."""
    start = pattern.index('^\s*(') + len('^\s*(')
    depth, current, names = 0, '', []
    for char in pattern[start:]:
        if char == '(':
            depth += 1
        elif char == ')':
            if depth == 0:
                break
            depth -= 1
        elif char == '|' and depth == 0:
            names.append(current)
            current = ''
            continue
        current += char
    names.append(current)
    expanded = []
    for name in names:
        group = re.fullmatch(r'([a-z_]+)\(([a-z|]+)\)', name)
        if group:
            expanded += [group[1] + option for option in group[2].split('|')]
        else:
            expanded.append(name)
    return expanded


class PolicySync(unittest.TestCase):
    def test_version(self):
        self.assertIn('kPolicyVersion = "%s"' % POLICY['policy_version'], HEADER)

    def test_name_tables(self):
        self.assertEqual(cpp_array('kKeyFileNamePatterns'), POLICY['key_file_name_patterns'])
        self.assertEqual(cpp_array('kFirmwareNandNamePatterns'),
                         POLICY['firmware_nand_name_patterns'])
        self.assertEqual(cpp_array('kFirmwareNandPathPatterns'),
                         POLICY['firmware_nand_path_patterns'])
        self.assertEqual(cpp_array('kGameContainerExtensions'),
                         POLICY['game_container_extensions'])

    def test_exefs_roles(self):
        exefs = POLICY['exefs']
        self.assertEqual(cpp_array('kModuleNames'), exefs['module_names'])
        self.assertEqual(cpp_array('kOsMetadataNames'), exefs['ignored_os_metadata'])
        self.assertIn('kNacpSize = 0x%X' % exefs['nacp_size'], CPP)
        self.assertIn('kRomFsHeaderSize = 0x%X' % exefs['romfs_header_size'], CPP)

    def test_signatures(self):
        found = re.findall(r'Signature\{"([^"]+)", (\d+), "([^"]+)"\}', CPP)
        cpp = [(name, int(offset), text.encode().hex()) for name, offset, text in found]
        json_signatures = [(s['name'], s['offset'], s['hex']) for s in POLICY['content_signatures']]
        self.assertEqual(cpp, json_signatures)

    def test_key_text_names(self):
        indexed, plain, rights = POLICY['key_text_patterns']
        self.assertEqual(sorted(cpp_array('kIndexedKeyNames')), sorted(alternation(indexed)))
        # bis_key_0[0-3] is spelled out in C++.
        expected = [n for n in alternation(plain) if not n.startswith('bis_key_0[')]
        expected += ['bis_key_0%d' % i for i in range(4)]
        self.assertEqual(sorted(cpp_array('kPlainKeyNames')), sorted(expected))
        self.assertIn('[0-9a-f]{32}\\s*=\\s*[0-9a-f]{32}', rights)

    def test_shared_coverage_schema(self):
        shared = POLICY['shared_coverage']
        self.assertIn('kSharedSchemaName = "%s"' % shared['schema'], GAPS)
        self.assertIn('kSharedSchemaVersion = %d' % shared['schema_version'], GAPS)
        self.assertIn('"%s"' % shared['description'], GAPS)

    def test_exportable_settings_are_real_and_allowed(self):
        section_of = {
            'Core': 'Core', 'Cpu': 'Cpu', 'Renderer': 'Renderer', 'RendererAdvanced': 'Renderer',
            'RendererHacks': 'Renderer', 'RendererExtensions': 'Renderer', 'Audio': 'Audio',
            'System': 'System', 'SystemAudio': 'System', 'LibraryApplet': 'LibraryApplet',
        }
        declared = {}
        for label, category in re.findall(r'"([a-z0-9_]+)"\s*,\s*Category::(\w+)', SETTINGS):
            declared.setdefault(label, set()).add(category)
        allowlist = cpp_array('kExportableSettings')
        self.assertEqual(len(allowlist), len(set(allowlist)), 'duplicate allowlist entries')
        for key in allowlist:
            section, label = key.split('/')
            with self.subTest(key=key):
                self.assertIn(label, declared, 'not a setting in settings.h')
                sections = {section_of.get(c) for c in declared[label]}
                self.assertIn(section, sections, 'category not allowed or wrong section')
        never = {'vulkan_device', 'output_device', 'input_device', 'device_name', 'current_user',
                 'program_args', 'rng_seed', 'rng_seed_enabled', 'custom_rtc', 'custom_rtc_enabled',
                 'custom_rtc_offset', 'dump_audio_commands', 'application_version_override',
                 'application_display_version_override'}
        self.assertFalse(never & {key.split('/')[1] for key in allowlist})


if __name__ == '__main__':
    unittest.main()
