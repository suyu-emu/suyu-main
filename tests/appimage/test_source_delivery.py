"""Recipe projections preserve complete selected code and immutable input pins."""
import copy
import gzip
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[2] / 'tools/appimage'
sys.path.insert(0, str(TOOLS))
from source_delivery import canonical_gzip, load_delivery_lock, recipe_projection, verify_distribution_records


def snapshot(rows):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w') as archive:
        for name, payload in rows:
            entry = tarfile.TarInfo(name); entry.size = len(payload); entry.mode = 0o644
            archive.addfile(entry, io.BytesIO(payload))
    return stream.getvalue()


class SourceDelivery(unittest.TestCase):
    def test_stored_gzip_is_reproducible_and_roundtrips_boundaries(self):
        for data in (b'', b'fixture', b'x' * 65536, bytes(range(256)) * 600):
            self.assertEqual(gzip.decompress(canonical_gzip(data)), data)
            self.assertEqual(canonical_gzip(data), canonical_gzip(data))

    def test_complete_recipe_projection_and_provenance(self):
        commit = 'a' * 40; top = 'aports-' + commit
        rows = [(top + '/README.md', b'upstream context'),
                (top + '/main/musl/APKBUILD', b'recipe'),
                (top + '/main/musl/patch.diff', b'complete local patch'),
                (top + '/community/unrelated/file', b'unrelated source')]
        original = snapshot(rows)
        source = dict(name='aports.tar.gz', url='https://example.invalid/aports/' + commit,
                      sha256=hashlib.sha256(original).hexdigest())
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'original'; path.write_bytes(original)
            data, receipt = recipe_projection(path, source, 'main/musl')
            with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
                content = {entry.name: archive.extractfile(entry).read() for entry in archive}
            self.assertEqual(content, dict(rows[:3]))
            self.assertEqual(receipt['original_sha256'], source['sha256'])
            self.assertEqual(receipt['original_url'], source['url'])
            self.assertEqual(receipt['output_sha256'], hashlib.sha256(data).hexdigest())
            self.assertEqual(path.read_bytes(), original)
            lock = dict(schema='suyu-source-delivery-v1', projections={source['name']: receipt})
            (Path(d) / 'source-delivery.lock.json').write_text(json.dumps(lock))
            pin = hashlib.sha256(json.dumps(lock, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
            policy = {'appimage': {'source_delivery_lock_canonical_sha256': pin}}
            loaded = load_delivery_lock(Path(d), {'sources': [source]}, policy)
            verify_distribution_records({'source_distributions': loaded['projections']}, loaded)
            changed = copy.deepcopy(loaded['projections'])
            changed[source['name']]['members'].pop()
            with self.assertRaises(ValueError):
                verify_distribution_records({'source_distributions': changed}, loaded)
            with self.assertRaises(ValueError):
                verify_distribution_records({}, loaded)
            wrong_input = dict(source, sha256='0' * 64)
            with self.assertRaises(ValueError):
                recipe_projection(path, wrong_input, 'main/musl')
            with self.assertRaises(ValueError):
                load_delivery_lock(Path(d), {'sources': [wrong_input]}, policy)
            policy['appimage']['source_delivery_lock_canonical_sha256'] = '0' * 64
            with self.assertRaises(ValueError):
                load_delivery_lock(Path(d), {'sources': [source]}, policy)
            missing = snapshot([rows[0], rows[2]])
            path.write_bytes(missing)
            with self.assertRaises(ValueError):
                recipe_projection(path, dict(source, sha256=hashlib.sha256(missing).hexdigest()), 'main/musl')
            (Path(d) / 'source-delivery.lock.json').unlink()
            with self.assertRaises(FileNotFoundError):
                load_delivery_lock(Path(d), {'sources': [source]}, policy)


if __name__ == '__main__':
    unittest.main()
