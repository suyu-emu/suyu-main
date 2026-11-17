"""Exact reviewed upstream fixture bindings, only for corresponding sources.

No content/key-text/game-signature rule can be excused. An opaque malformed fixture
boundary still receives the ordinary raw-content scans before nested parsing
stops; changing its bytes or any ancestor archive invalidates admission.
"""
import hashlib
import json
from pathlib import Path
import re

ALLOWED_RULES = frozenset({'link', 'user-data', 'path-unsafe', 'unreadable', 'nested-archive', 'key-name'})
FACT_FIELDS = ('archive_name', 'archive_sha256', 'ancestors', 'name', 'raw_name_hex',
               'occurrence', 'type', 'size', 'target', 'target_hex', 'sha256')


def identity(context, name, occurrence):
    return (context['archive_name'], context['archive_sha256'],
            tuple((a['name'], a['occurrence'], a['sha256']) for a in context['ancestors']),
            name, occurrence)


class FixtureReview:
    def __init__(self, document=None, audit_labels=()):
        if document is not None and not isinstance(document, dict):
            raise ValueError('source fixture review must be an object')
        self.records = {}; self.audit_facts = []
        self.audit_labels = set(audit_labels)
        self.audit_prefixes = set()
        for label in self.audit_labels:
            parts = label.split('!')
            self.audit_prefixes.update('!'.join(parts[:i]) for i in range(1, len(parts) + 1))
        for record in (document or {}).get('members', []):
            if (not set(record['rules']).issubset(ALLOWED_RULES) or
                    not set(FACT_FIELDS).issubset(record) or not record.get('review_reason')):
                raise ValueError('source fixture approval exceeds bounded source-only rules')
            if (type(record['occurrence']) is not int or record['occurrence'] < 1 or
                    type(record['size']) is not int or record['size'] < 0 or
                    type(record.get('opaque_fixture', False)) is not bool or
                    record['type'] not in ('file', 'directory', 'symlink', 'hardlink', 'special file') or
                    not isinstance(record['name'], str) or
                    record['raw_name_hex'] != record['name'].encode('utf-8', 'surrogateescape').hex() or
                    record['target'] is not None and not isinstance(record['target'], str) or
                    record['target_hex'] != (record['target'].encode('utf-8', 'surrogateescape').hex()
                                             if record['target'] is not None else None)):
                raise ValueError('invalid exact raw source-member identity')
            if any(not isinstance(record[field], str) or not re.fullmatch('[0-9a-f]{64}', record[field])
                   for field in ('archive_sha256', 'sha256')):
                raise ValueError('invalid source-member digest')
            if not isinstance(record['ancestors'], list) or any(
                    not isinstance(a, dict) or not isinstance(a['name'], str) or type(a['occurrence']) is not int or a['occurrence'] < 1 or
                    not isinstance(a['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', a['sha256']) for a in record['ancestors']):
                raise ValueError('invalid source ancestor identity')
            if 'key-name' in record['rules'] and (record['archive_name'], record['name']) != (
                    'openssl_3.0.13.orig.tar.gz', 'openssl-3.0.13/doc/HOWTO/keys.txt'):
                raise ValueError('key-name admission is limited to the reviewed OpenSSL prose document')
            if record.get('opaque_fixture') and (
                    not set(record['rules']) & {'nested-archive', 'unreadable'} or
                    not record.get('malformed_fixture_evidence')):
                raise ValueError('opaque fixture requires exact malformed decoder-test evidence')
            key = identity(record, record['name'], record['occurrence'])
            if key in self.records:
                raise ValueError('duplicate reviewed raw member occurrence')
            self.records[key] = record

    def observe(self, context, entry, occurrence, label, raw_label):
        if context is None:
            return None
        key = identity(context, entry.raw, occurrence)
        relative_raw = context['archive_name'] + '!' + '!'.join(
            [a['name'] for a in context['ancestors']] + [entry.raw])
        relative_normal = label.split('/', 1)[1] if '/' in label else label
        if key not in self.records and relative_raw not in self.audit_prefixes and relative_normal not in self.audit_prefixes:
            return None
        kind = entry.link or ('directory' if entry.is_dir else 'file')
        target = entry.link_target
        if kind == 'file':
            value = hashlib.sha256()
            with entry.opener() as stream:
                while chunk := stream.read(1 << 20):
                    value.update(chunk)
            digest = value.hexdigest()
        else:
            digest = hashlib.sha256((kind + '\0' + (target or '')).encode('utf-8', 'surrogateescape')).hexdigest()
        fact = dict(context, name=entry.raw, raw_name_hex=entry.raw.encode('utf-8', 'surrogateescape').hex(),
                    occurrence=occurrence, type=kind, size=entry.size, target=target,
                    target_hex=target.encode('utf-8', 'surrogateescape').hex() if target is not None else None,
                    sha256=digest, _label=label, _raw_label=raw_label)
        if self.audit_prefixes:
            self.audit_facts.append(fact)
        return fact

    def matched(self, fact):
        if fact is None:
            return None
        record = self.records.get(identity(fact, fact['name'], fact['occurrence']))
        if record is None or any(record[field] != fact[field] for field in FACT_FIELDS):
            return None
        return record

    def excuses(self, fact, rule, label):
        record = self.matched(fact)
        return (record is not None and rule in ALLOWED_RULES and rule in record['rules'] and
                label in (fact['_label'], fact['_raw_label']))

    def opaque(self, fact):
        record = self.matched(fact)
        return record is not None and record.get('opaque_fixture') is True and fact['type'] == 'file'


def load_review(folder, policy):
    specification = policy.get('source_fixture_review')
    if specification is None:
        return FixtureReview()
    document = json.loads((Path(folder) / specification['file']).read_text())
    digest = hashlib.sha256(json.dumps(document, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    if document.get('schema') != 'suyu-source-fixture-review-v1' or digest != specification['canonical_sha256']:
        raise ValueError('source fixture review differs from committed policy')
    return FixtureReview(document)
