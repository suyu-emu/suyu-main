"""Partitioning must preserve generated bodies, lookup entries, and execution."""
from pathlib import Path
import argparse
import re
import subprocess


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def executable(directory, name):
    for path in [directory / (name + '.exe'), directory / name, directory / 'Release' / (name + '.exe')]:
        if path.is_file():
            return path
    raise AssertionError('Missing ' + name)


def inspect(directory, module, limit):
    bodies, entries = {}, []
    units = sorted((directory / 'src').glob('recompiled_' + module + '_*.c'))
    for unit in units:
        text = unit.read_text()
        functions = re.findall(r'(void blk_.*?)(?=\nvoid blk_|\nconst struct _recomp_ent|\Z)', text, re.S)
        slots = [int(slot) for slot in re.findall(r'RECOMP_GG_GUARD\(_expected,(\d+)U\)', text)]
        assert slots == list(range(len(functions))), (unit, slots)
        seen = re.search(r'recomp_gg_seen\[(\d+)\]', text)
        assert int(seen[1]) == len(functions)
        count = re.search(r'const unsigned _segn_\w+ = (\d+)U;', text)
        assert int(count[1]) == len(functions)
        for function in functions:
            name = function.split('(')[0]
            assert name not in bodies
            bodies[name] = re.sub(r'RECOMP_GG_GUARD\(_expected,\d+U\)', 'RECOMP_GG_GUARD(_expected,SLOT)', function).rstrip()
        table = re.search(r'const struct _recomp_ent _seg_.*?\[\] = \{(.*?)\n\};', text, re.S)[1]
        entries.extend(re.findall(r'\{0x[0-9a-f]+ULL, 0x[0-9a-f]+ULL, blk_\w+\}', table))
        # A single oversized block is intentionally retained alone.
        if len(functions) > 1:
            assert unit.stat().st_size <= limit, unit
    return bodies, sorted(entries), len(units)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    build = Path(args.build).resolve()
    emitter = build / 'emitter'
    generated = build / 'generated'
    run('cmake', '-S', Path(__file__).parent, '-B', emitter, '-G', 'Ninja', '-DSUYU_SOURCE=' + str(source), '-DCMAKE_BUILD_TYPE=Release')
    run('cmake', '--build', emitter)
    run(executable(emitter, 'partition_export'), generated)
    counted_bodies, counted_entries, counted_units = inspect(generated / 'counted', 'counted', 32 << 20)
    assert len(counted_bodies) == len(counted_entries) == 20001
    assert counted_units == 2
    counted_first = (generated / 'counted/src/recompiled_counted_0.c').read_text()
    assert 'const unsigned _segn_counted_0 = 20000U;' in counted_first
    for module in ['smoke', 'second']:
        baseline = generated / 'baseline' / ('second' if module == 'second' else '')
        partitioned = generated / 'partitioned' / ('second' if module == 'second' else '')
        original_bodies, original_entries, original_count = inspect(baseline, module, 32 << 20)
        bodies, entries, count = inspect(partitioned, module, 4096)
        assert bodies == original_bodies, module
        assert entries == original_entries, module
        assert count > original_count, module
    for mode in ['baseline', 'partitioned']:
        consumer = build / mode
        run('cmake', '-S', source / 'tests/recompiler_smoke', '-B', consumer, '-G', 'Ninja', '-DSUYU_SOURCE=' + str(source), '-DGENERATED_DIR=' + str(generated / mode), '-DCMAKE_BUILD_TYPE=Release')
        run('cmake', '--build', consumer, '--target', 'smoke_run', 'smoke_static')
        run(executable(consumer, 'smoke_run'))
        run(executable(consumer, 'smoke_static'))
    print('PASS: exact bodies/dispatch entries, GG slots, single- and multi-module execution')
