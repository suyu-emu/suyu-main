#!/usr/bin/env python3
"""Decide whether the release workflow may publish (stdlib only).

Publishing requires a push of a version tag (never workflow_dispatch, never a
branch push, never a tag deletion) after every required job, including the
package-policy validation jobs, finished with result "success".

Prints the reason and writes "publish=true|false" to $GITHUB_OUTPUT when set.
Exit status is 0 either way (the workflow gates on the output) and 1 when the
needs JSON is malformed.
"""
import argparse
import json
import os
import re
import sys

TAG_REF = re.compile(r'^refs/tags/v[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$')


def decide(event_name, ref, deleted, needs, required=()):
    if event_name != 'push':
        return False, f'event is {event_name or "unset"}, not a tag push'
    if not TAG_REF.match(ref or ''):
        return False, f'ref {ref or "(unset)"} is not a version tag (vX.Y.Z)'
    if deleted is True or str(deleted).strip().lower() == 'true':
        return False, 'the tag push is a deletion'
    if not needs:
        return False, 'no job results were provided'
    for job in required:
        if job not in needs:
            return False, f'required job {job} is missing'
    for job, need in sorted(needs.items()):
        result = need.get('result') if isinstance(need, dict) else None
        if result != 'success':
            return False, f'job {job} finished with result {result}'
    return True, 'version tag push and every required job succeeded'


def write_output(value):
    path = os.environ.get('GITHUB_OUTPUT')
    if path:
        with open(path, 'a', encoding='utf-8') as handle:
            handle.write('publish=' + ('true' if value else 'false') + '\n')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--deleted', default='')
    parser.add_argument('--needs-json', required=True)
    parser.add_argument('--require', default='')
    args = parser.parse_args(argv)
    try:
        needs = json.loads(args.needs_json)
        if not isinstance(needs, dict) or not all(isinstance(v, dict) for v in needs.values()):
            raise ValueError('needs must be an object of objects')
    except ValueError as error:
        print('publish=false: malformed needs JSON: ' + str(error))
        write_output(False)
        return 1
    required = [job.strip() for job in args.require.split(',') if job.strip()]
    publish, reason = decide(os.environ.get('GITHUB_EVENT_NAME', ''), os.environ.get('GITHUB_REF', ''),
                             args.deleted, needs, required)
    print(('publish=true: ' if publish else 'publish=false: ') + reason)
    write_output(publish)
    return 0


if __name__ == '__main__':
    sys.exit(main())
