"""Exact source-fixture treatment cannot admit altered or forbidden payloads."""
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/package_policy'))
import scan_release as scan
from source_fixtures import FixtureReview
from source_fixtures import load_review


def archive_bytes(rows):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w') as archive:
        for name, payload, target in rows:
            entry = tarfile.TarInfo(name)
            if target is not None:
                entry.type = tarfile.SYMTYPE; entry.linkname = target
            else:
                entry.size = len(payload)
            archive.addfile(entry, None if target is not None else io.BytesIO(payload))
    return stream.getvalue()


class SourceFixtures(unittest.TestCase):
    def inspect(self, inner, review, kind='dependency-sources'):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'sources.tar.gz'
            path.write_bytes(archive_bytes([('suyu-test-dependency-sources/upstream.tar.gz', inner, None)]))
            with mock.patch.object(scan, 'load_review', return_value=review):
                return scan.scan_archive(path, kind, scan.Rules(scan.load_policy()))['findings']

    def record(self, rows, name, rules, opaque=False):
        inner = archive_bytes(rows)
        audit = FixtureReview(audit_labels=['upstream.tar.gz!' + name])
        self.inspect(inner, audit)
        fact = next(f for f in audit.audit_facts if f['name'] == name)
        result = {k: v for k, v in fact.items() if not k.startswith('_')}
        result.update(rules=rules, review_reason='Known immutable upstream regression fixture', opaque_fixture=opaque)
        if opaque:
            result['malformed_fixture_evidence'] = [{'rule': 'unreadable', 'reason': 'specific malformed tar header'}]
        return inner, fact, result

    def test_static_source_fixture_and_archive_mutations(self):
        name = 'upstream/tests/log/reference.txt'
        rows = [(name, b'known static fixture\n', None)]
        inner, fact, record = self.record(rows, name, ['user-data'])
        review = FixtureReview({'members': [record]})
        self.assertEqual(self.inspect(inner, review), [])
        for altered in ([(name, b'changed fixture\n', None)],
                        rows + [('upstream/injected/prod.keys', b'forbidden', None)],
                        rows + rows):
            self.assertTrue(self.inspect(archive_bytes(altered), review))
        self.assertIn('user-data', {f['rule'] for f in self.inspect(inner, review, 'source')})
        changed = dict(fact, occurrence=2)
        self.assertIsNone(review.matched(changed))
        changed = dict(fact, archive_sha256='0' * 64)
        self.assertIsNone(review.matched(changed))
        with self.assertRaises(ValueError):
            FixtureReview({'members': [record, record]})

    def test_link_target_raw_path_and_type_are_bound(self):
        name = 'upstream/tests/alias'
        rows = [('upstream/tests/real', b'fixture', None), (name, b'', 'real')]
        inner, fact, record = self.record(rows, name, ['link'])
        review = FixtureReview({'members': [record]})
        self.assertEqual(self.inspect(inner, review), [])
        for field, value in (('target', '../../foreign'), ('target_hex', '00'),
                             ('name', name.upper()), ('raw_name_hex', '00'), ('type', 'hardlink')):
            self.assertIsNone(review.matched(dict(fact, **{field: value})))
        changed = [('upstream/tests/real', b'fixture', None), (name, b'', '../../foreign')]
        self.assertIn('link', {f['rule'] for f in self.inspect(archive_bytes(changed), review)})

    def test_opaque_fixture_keeps_unwaivable_raw_content_scans(self):
        name = 'upstream/tests/archive/test_bad.tar'
        for payload, expected in ((b'not a tar header', None),
                (b'master_key_00 = ' + b'01' * 16, 'key-text'),
                (b'PFS0' + b'\0' * 32, 'signature')):
            inner, fact, record = self.record([(name, payload, None)], name,
                                               ['nested-archive', 'unreadable'], True)
            found = self.inspect(inner, FixtureReview({'members': [record]}))
            if expected:
                self.assertIn(expected, {f['rule'] for f in found})
            else:
                self.assertEqual(found, [])
        for rule in ('key-text', 'signature', 'game-container'):
            bad = dict(record, rules=[rule])
            with self.assertRaises(ValueError):
                FixtureReview({'members': [bad]})
        with self.assertRaises(ValueError):
            FixtureReview({'members': [dict(record, malformed_fixture_evidence=[])]})

    def test_ancestor_identity_and_occurrence_are_bound(self):
        name = 'upstream/tests/log/reference.txt'
        inner, fact, record = self.record([(name, b'fixture', None)], name, ['user-data'])
        ancestor = dict(name='upstream.tar', occurrence=1, sha256='a' * 64)
        record['ancestors'] = [ancestor]
        nested_fact = dict(fact, ancestors=[ancestor])
        review = FixtureReview({'members': [record]})
        self.assertIsNotNone(review.matched(nested_fact))
        for field, value in (('name', 'different.tar'), ('occurrence', 2), ('sha256', 'b' * 64)):
            changed = dict(nested_fact, ancestors=[dict(ancestor, **{field: value})])
            self.assertIsNone(review.matched(changed))

    def test_invalid_binding_types_and_missing_review_lock_fail_closed(self):
        name = 'upstream/tests/log/reference.txt'
        _, _, record = self.record([(name, b'fixture', None)], name, ['user-data'])
        for field, value in (('occurrence', True), ('size', 7.0), ('opaque_fixture', 'yes')):
            with self.subTest(field=field), self.assertRaises(ValueError):
                FixtureReview({'members': [dict(record, **{field: value})]})
        with tempfile.TemporaryDirectory() as d, self.assertRaises(FileNotFoundError):
            load_review(Path(d), {'source_fixture_review': {'file': 'missing.json', 'canonical_sha256': '0' * 64}})


if __name__ == '__main__':
    unittest.main()
