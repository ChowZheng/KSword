"""Meaningful profile regressions: resets, coverage, nonduplicated weights and encoding."""
import copy
import json
import tempfile
import unittest
from pathlib import Path
from analyze_svm_profile import analyze, load, cycle_delta


def snapshot(tick, n):
    hot = {'total': str(n), 'npf': str(n), 'invalid': '0', 'other': '0', 'codes': {}}
    sample = {'samples': str(n), 'cycles': str(n*50), 'maximum': '50', 'stages': [str(n*10)]*5, 'details': [str(n)]*7}
    empty = dict(sample, samples='0', cycles='0', stages=['0']*5, details=['0']*7)
    perf = {'valid': 1, 'saturated': 0, 'sequence': str(n*2), 'sampleMask': '63', 'tlbIssued': [str(n)]*4,
            'levels': [[copy.deepcopy(sample)]+[copy.deepcopy(empty) for _ in range(11)] for _ in range(2)]}
    return {'version': 10, 'backend': 2, 'qpcFrequency': '100', 'snapshotBeginQpc': str(tick),
            'svmProcessors': [{'group': 0, 'number': 0, 'generation': 4, 'vmcbPa': '0x1000', 'asid': 2,
              'hotspots': {'valid': 1, 'saturated': 0, 'sequence': str(n*2), 'levels': [copy.deepcopy(hot) for _ in range(2)]}, 'perf': perf}]}


class ProfileTests(unittest.TestCase):
    def test_delta_not_cumulative(self):
        result, counts, cycles = analyze([snapshot(100, 10), snapshot(200, 13), snapshot(300, 15)])
        self.assertEqual(result['totalExits'], 10)
        self.assertEqual(sum(counts.values()), 10)
        self.assertEqual(sum(cycles.values()), 500)
        self.assertEqual(result['hardwareTlbIssued']['1'], 5)
        self.assertTrue(all(i['completeTimingCoverage'] for i in result['intervals']))

    def test_invalid_and_changed_lifetime(self):
        for field, value in [('generation', 5), ('vmcbPa', '0x2000'), ('asid', 3)]:
            a, b = snapshot(100, 10), snapshot(200, 15)
            b['svmProcessors'][0][field] = value
            self.assertTrue(analyze([a, b])[0]['excluded'])
        a, b = snapshot(100, 10), snapshot(200, 15)
        b['svmProcessors'][0]['hotspots']['valid'] = 0
        result = analyze([a, b])[0]
        self.assertEqual(result['totalExits'], 0)
        self.assertFalse(result['intervals'][0]['completeCpuCoverage'])
        self.assertTrue(analyze([snapshot(100, 15), snapshot(200, 10)])[0]['excluded'])

    def test_timing_rejection_preserves_exit_evidence(self):
        for field, value in [('sequence', '3'), ('valid', 0), ('saturated', 1), ('sampleMask', '127')]:
            a, b = snapshot(100, 10), snapshot(200, 15)
            b['svmProcessors'][0]['perf'][field] = value
            result, _, cycles = analyze([a, b])
            self.assertEqual(result['totalExits'], 10)
            self.assertFalse(cycles)
            self.assertTrue(result['excluded'])

    def test_bad_accounting_clock_and_topology(self):
        a, b = snapshot(100, 10), snapshot(200, 15)
        b['svmProcessors'][0]['perf']['levels'][0][0]['details'][0] = '200'
        with self.assertRaises(ValueError):
            cycle_delta(a['svmProcessors'][0]['perf'], b['svmProcessors'][0]['perf'])
        b = snapshot(200, 15)
        b['qpcFrequency'] = '200'
        self.assertTrue(analyze([a, b])[0]['excluded'])
        b = snapshot(200, 15)
        b['svmProcessors'] *= 2
        self.assertTrue(analyze([a, b])[0]['excluded'])

    def test_all_jsonl_lines_and_utf16(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'metrics.jsonl'
            text = '\n'.join(json.dumps(snapshot(i*100, i)) for i in (1, 2, 3))
            for encoding in ('utf-8-sig', 'utf-16'):
                p.write_text(text, encoding=encoding)
                self.assertEqual(len(load(p)), 3)


if __name__ == '__main__':
    unittest.main()
