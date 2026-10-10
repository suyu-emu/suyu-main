"""AppImage source archives cannot downgrade schema or omit exact receipt links."""
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


def tar_bytes(files):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w') as archive:
        for name, data in files.items():
            entry = tarfile.TarInfo(name); entry.size = len(data)
            archive.addfile(entry, io.BytesIO(data))
    return stream.getvalue()


class SourceDistribution(unittest.TestCase):
    def setUp(self):
        self.rules = scan.Rules(scan.load_policy())
        self.lock, _ = scan.appimage_runtime_lock(self.rules)
        self.lock = copy.deepcopy(self.lock)
        payload = tar_bytes({'upstream/LICENSE.txt': b'fixture source text'})
        self.files = {row['name']: payload for row in self.lock['sources']}
        for row in self.lock['sources']:
            row['sha256'] = hashlib.sha256(payload).hexdigest()
        self.lock_hash = 'a' * 64
        archives = ['/usr/lib/libc.a', '/usr/lib/libfuse3.a', '/usr/lib/libmimalloc.a',
                    '/usr/lib/libz.a', '/usr/lib/libzstd.a', '/usr/local/lib/libsquashfuse.a',
                    '/usr/local/lib/libsquashfuse_ll.a']
        receipt = dict(self.lock, lock_sha256=self.lock_hash, linker_map_sha256='b' * 64,
                       installed_package_db_sha256='c' * 64, contributed_archives=archives)
        self.document = dict(schema='suyu-appimage-sources-v1', runtime_complete=True,
                             runtime_sha256=self.lock['sha256'], runtime_build_receipt=receipt,
                             runtime_components=self.lock['components'], sources=self.lock['sources'],
                             libraries={}, source_distributions={})

    def inspect(self, document, files=None):
        files = dict(self.files if files is None else files)
        files['README.txt'] = b'fixture distribution description'
        if document is not None:
            files['MANIFEST.json'] = json.dumps(document).encode()
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'suyu-fixture-appimage-sources.tar.gz'
            path.write_bytes(tar_bytes({'suyu-fixture-dependency-sources/' + n: b for n, b in files.items()}))
            with mock.patch.object(scan, 'appimage_runtime_lock', return_value=(self.lock, self.lock_hash)), \
                 mock.patch.object(scan, 'load_delivery_lock', return_value={'projections': {}}):
                return scan.scan_archive(path, 'dependency-sources', self.rules)['findings']

    def test_complete_source_fixture_and_schema_downgrades(self):
        self.assertEqual(self.inspect(self.document), [])
        changes = [None, [], dict(self.document, schema='unrelated'),
                   {k: v for k, v in self.document.items() if k != 'schema'}]
        for document in changes:
            with self.subTest(document_type=type(document).__name__):
                self.assertIn('source-provenance', {f['rule'] for f in self.inspect(document)})

    def test_receipt_distribution_and_physical_inventory_cannot_be_omitted(self):
        for field in ('source_distributions', 'runtime_build_receipt', 'runtime_complete', 'runtime_sha256'):
            changed = {k: v for k, v in self.document.items() if k != field}
            with self.subTest(field=field):
                self.assertIn('source-provenance', {f['rule'] for f in self.inspect(changed)})
        files = dict(self.files); files.pop('type2-runtime.tar.gz')
        self.assertIn('source-provenance', {f['rule'] for f in self.inspect(self.document, files)})
        files = dict(self.files); files['extra.tar.gz'] = files['type2-runtime.tar.gz']
        self.assertIn('source-provenance', {f['rule'] for f in self.inspect(self.document, files)})
        changed = copy.deepcopy(self.document)
        changed['runtime_build_receipt']['lock_sha256'] = '0' * 64
        self.assertIn('source-provenance', {f['rule'] for f in self.inspect(changed)})


if __name__ == '__main__':
    unittest.main()
