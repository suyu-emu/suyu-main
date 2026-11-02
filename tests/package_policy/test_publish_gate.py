"""Publish gate decisions; pure functions plus a CLI round trip."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[2] / 'tools/package_policy/publish_gate.py'
sys.path.insert(0, str(TOOL.parent))
import publish_gate  # noqa: E402

REQUIRED = ['build-linux', 'build-windows', 'package-policy-tests']
GOOD = {job: {'result': 'success', 'outputs': {}} for job in REQUIRED}
TAG = 'refs/tags/v1.2.3'


def decide(event='push', ref=TAG, deleted='false', needs=None):
    return publish_gate.decide(event, ref, deleted, GOOD if needs is None else needs, REQUIRED)


class DecideTests(unittest.TestCase):
    def test_valid_tag_push_publishes(self):
        for ref in [TAG, 'refs/tags/v0.0.11', 'refs/tags/v1.2.3-rc.1', 'refs/tags/v1.2.3.4']:
            with self.subTest(ref=ref):
                self.assertTrue(decide(ref=ref)[0])
        self.assertTrue(decide(deleted='')[0])

    def test_workflow_dispatch_never_publishes(self):
        self.assertFalse(decide(event='workflow_dispatch', ref='refs/heads/main')[0])
        self.assertFalse(decide(event='workflow_dispatch', ref=TAG)[0])

    def test_branch_push_and_bad_tags_do_not_publish(self):
        self.assertFalse(decide(ref='refs/heads/main')[0])
        for ref in ['refs/tags/test', 'refs/tags/v1', 'refs/tags/v1.2', 'refs/tags/v1.2.3x',
                    'refs/tags/release-1.2.3', 'v1.2.3', '']:
            with self.subTest(ref=ref):
                self.assertFalse(decide(ref=ref)[0])

    def test_tag_deletion_does_not_publish(self):
        self.assertFalse(decide(deleted='true')[0])
        self.assertFalse(publish_gate.decide('push', TAG, True, GOOD, REQUIRED)[0])

    def test_non_success_needs_block_publishing(self):
        for result in ['failure', 'cancelled', 'skipped', '', None]:
            with self.subTest(result=result):
                needs = dict(GOOD, **{'package-policy-tests': {'result': result}})
                self.assertFalse(decide(needs=needs)[0])

    def test_missing_or_empty_needs_block_publishing(self):
        self.assertFalse(decide(needs={})[0])
        partial = {k: v for k, v in GOOD.items() if k != 'package-policy-tests'}
        publish, reason = decide(needs=partial)
        self.assertFalse(publish)
        self.assertIn('package-policy-tests', reason)
        self.assertFalse(decide(needs={'build-linux': 'success'})[0])


class CliTests(unittest.TestCase):
    def run_gate(self, event, ref, needs_json, deleted='false', require=','.join(REQUIRED)):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / 'github_output'
            env = dict(os.environ, GITHUB_EVENT_NAME=event, GITHUB_REF=ref, GITHUB_OUTPUT=str(output))
            result = subprocess.run([sys.executable, str(TOOL), '--deleted', deleted, '--needs-json', needs_json,
                                     '--require', require], env=env, capture_output=True, text=True)
            written = output.read_text() if output.exists() else ''
        return result, written

    def test_round_trip_writes_github_output(self):
        result, written = self.run_gate('push', TAG, json.dumps(GOOD))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(written, 'publish=true\n')
        result, written = self.run_gate('workflow_dispatch', TAG, json.dumps(GOOD))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(written, 'publish=false\n')
        self.assertIn('publish=false', result.stdout)

    def test_failed_need_is_false_but_exit_zero(self):
        needs = dict(GOOD, **{'build-linux': {'result': 'failure'}})
        result, written = self.run_gate('push', TAG, json.dumps(needs))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(written, 'publish=false\n')

    def test_malformed_json_exits_one(self):
        for text in ['{not json', '[]', '{"a": "success"}']:
            with self.subTest(text=text):
                result, written = self.run_gate('push', TAG, text)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(written, 'publish=false\n')

    def test_works_without_github_output(self):
        env = {k: v for k, v in os.environ.items() if k != 'GITHUB_OUTPUT'}
        env.update(GITHUB_EVENT_NAME='push', GITHUB_REF=TAG)
        result = subprocess.run([sys.executable, str(TOOL), '--needs-json', json.dumps(GOOD)], env=env,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn('publish=true', result.stdout)


if __name__ == '__main__':
    unittest.main()
