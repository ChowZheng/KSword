"""Compare coherent AMD snapshots: exit deltas and sampled root software cycles.

Cycles exclude guest execution and pure hardware VMEXIT/VMRUN latency.
Flight-record timestamps and lifecycle counters are not graph weights.
"""
import argparse
import glob
import json
from collections import Counter
from pathlib import Path

BUCKETS = ('MSR', 'XSETBV', 'VMLOAD', 'VMSAVE', 'STGI', 'CLGI', 'VMRUN', 'CPUID', 'NPF', 'IO', 'IRQ', 'other')
STAGES = ('xstate_save', 'host_restore', 'dispatch', 'entry_coordination', 'guest_restore')
DETAILS = ('fetch', 'operand', 'permissions', 'npt12_sync', 'writeback', 'npt_walk', 'shadow_install')


def load(path):
    raw = Path(path).read_bytes()
    text = raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig')
    try:
        data = json.loads(text)
        return data if isinstance(data, list) else [data]
    except json.JSONDecodeError:
        return [json.loads(line) for line in text.splitlines() if line.strip()]


def number(value):
    return int(value, 16) if isinstance(value, str) and value.lower().startswith('0x') else int(value)


def valid(row):
    seq = number(row.get('sequence', 0))
    return row.get('valid') == 1 and seq > 0 and seq % 2 == 0 and row.get('saturated', 0) == 0


def delta(a, b):
    n = number(b) - number(a)
    if n < 0:
        raise ValueError('counter reset')
    return n


def indexed(snapshot):
    rows = snapshot['svmProcessors']
    result = {(r['group'], r['number']): r for r in rows}
    if not rows or len(result) != len(rows):
        raise ValueError('empty or duplicate CPU set')
    return result


def exit_delta(a, b):
    if not valid(a) or not valid(b):
        raise ValueError('incoherent hotspots')
    result = Counter()
    if len(a['levels']) != 2 or len(b['levels']) != 2:
        raise ValueError('incomplete hotspot levels')
    for level, (x, y) in enumerate(zip(a['levels'], b['levels']), 1):
        left = {number(k): number(v) for k, v in x['codes'].items()}
        right = {number(k): number(v) for k, v in y['codes'].items()}
        for code in left.keys() | right.keys():
            result[(level, f'exit_0x{code:x}')] = delta(left.get(code, 0), right.get(code, 0))
        for key, name in (('npf', 'exit_0x400'), ('invalid', 'invalid'), ('other', 'other')):
            result[(level, name)] += delta(x.get(key, 0), y.get(key, 0))
        if delta(x['total'], y['total']) != sum(n for (l, _), n in result.items() if l == level):
            raise ValueError('exit accounting mismatch')
    return result


def cycle_delta(a, b):
    if not valid(a) or not valid(b):
        raise ValueError('incoherent or saturated perf')
    mask = number(a['sampleMask'])
    if mask != number(b['sampleMask']) or mask < 0 or mask & (mask + 1):
        raise ValueError('sampling configuration changed')
    if len(a['levels']) != 2 or len(b['levels']) != 2:
        raise ValueError('incomplete perf levels')
    weights, samples = Counter(), Counter()
    for level, (left, right) in enumerate(zip(a['levels'], b['levels']), 1):
        if len(left) != len(BUCKETS) or len(right) != len(BUCKETS):
            raise ValueError('incomplete perf buckets')
        for bucket, x, y in zip(BUCKETS, left, right):
            if any(len(r['stages']) != len(STAGES) or len(r['details']) != len(DETAILS) for r in (x, y)):
                raise ValueError('incomplete stage timing')
            n, ticks = delta(x['samples'], y['samples']), delta(x['cycles'], y['cycles'])
            stages = [delta(i, j) for i, j in zip(x['stages'], y['stages'])]
            details = [delta(i, j) for i, j in zip(x['details'], y['details'])]
            if sum(stages) != ticks or sum(details) > stages[2] or (n == 0 and ticks):
                raise ValueError('cycle accounting mismatch')
            samples[(level, bucket)] = n
            for stage, value in zip(STAGES, stages):
                if stage != 'dispatch':
                    weights[(level, bucket, stage)] = value
            for stage, value in zip(DETAILS, details):
                weights[(level, bucket, 'dispatch;' + stage)] = value
            weights[(level, bucket, 'dispatch;remainder')] = stages[2] - sum(details)
    if len(a['tlbIssued']) != 4 or len(b['tlbIssued']) != 4:
        raise ValueError('incomplete hardware flush counters')
    return weights, samples, [delta(i, j) for i, j in zip(a['tlbIssued'], b['tlbIssued'])]


def analyze(snapshots):
    records = sorted(snapshots, key=lambda s: number(s['snapshotBeginQpc']))
    counts, cycles, samples = Counter(), Counter(), Counter()
    excluded, intervals, flushes = [], [], [0]*4
    for i, (a, b) in enumerate(zip(records, records[1:])):
        try:
            if a.get('backend') != 2 or b.get('backend') != 2 or a.get('version') != b.get('version'):
                raise ValueError('incompatible AMD metrics versions')
            hz, ticks = number(a['qpcFrequency']), number(b['snapshotBeginQpc'])-number(a['snapshotBeginQpc'])
            if hz <= 0 or hz != number(b['qpcFrequency']) or ticks <= 0:
                raise ValueError('incomparable clock interval')
            left, right = indexed(a), indexed(b)
            if left.keys() != right.keys():
                raise ValueError('CPU topology changed')
        except (ValueError, KeyError) as error:
            excluded.append({'interval': i, 'reason': str(error)})
            continue
        included, timed = [], []
        for cpu in sorted(left):
            label, x, y = f'g{cpu[0]}p{cpu[1]}', left[cpu], right[cpu]
            try:
                identities = ('generation', 'stableVmcb01Pa', 'stableVmcb02Pa', 'asid') if a['version'] == 11 else ('generation', 'vmcbPa', 'asid')
                for identity in identities:
                    if x[identity] != y[identity]:
                        raise ValueError(identity+' changed')
                changes = exit_delta(x['hotspots'], y['hotspots'])
                counts.update({f'svm;{label};L{l};{code}': n for (l, code), n in changes.items()})
                included.append(label)
            except (ValueError, KeyError) as error:
                excluded.append({'interval': i, 'cpu': label, 'reason': str(error)})
                continue
            if a['version'] in (10, 11):
                try:
                    weights, ns, controls = cycle_delta(x.get('perf', {}), y.get('perf', {}))
                    cycles.update({f'svm;{label};L{l};{code};{stage}': n for (l, code, stage), n in weights.items()})
                    samples.update({f'{label}/L{l}/{code}': n for (l, code), n in ns.items()})
                    flushes = [j+k for j, k in zip(flushes, controls)]
                    timed.append(label)
                except (ValueError, KeyError) as error:
                    excluded.append({'interval': i, 'cpu': label, 'timingReason': str(error)})
        intervals.append({'seconds': ticks/hz, 'includedCpus': included, 'timedCpus': timed,
                          'completeCpuCoverage': len(included)==len(left), 'completeTimingCoverage': len(timed)==len(left)})
    result = {'kind': 'ksword-amd-exit-profile', 'snapshots': len(records), 'intervals': intervals, 'excluded': excluded,
              'totalExits': sum(counts.values()), 'sampledRootCycles': sum(cycles.values()),
              'samplesByCpuLevelBucket': dict(samples), 'hardwareTlbIssued': dict(zip(('0','1','3','7'), flushes)),
              'instructionCycleSampling': False, 'pureHardwareTransitionTiming': False,
              'note': 'Cycles are sampled root software work including instrumentation. Dispatch remainder includes events, routing and diagnostics; counts are separate weights.'}
    return result, counts, cycles


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', nargs='+')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    files = sorted({name for item in args.inputs for name in (glob.glob(item, recursive=True) or [item])})
    result, counts, cycles = analyze([r for name in files for r in load(name)])
    for suffix, values, field in (('.collapsed', counts, 'collapsedPath'), ('.cycles.collapsed', cycles, 'cycleCollapsedPath')):
        path = args.out.with_suffix(suffix)
        path.write_text(''.join(f'{stack} {n}\n' for stack, n in sorted(values.items()) if n), encoding='utf-8')
        result[field] = str(path)
    args.out.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
