"""Synthetic raw-schema regressions; these are not native Windows PFN scans."""
from __future__ import annotations
import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import pfn_evidence_audit as subject

U64 = (1 << 64) - 1
ZERO = lambda: [['0'] * 8 for _ in range(16)]


def frame(native, state, key=0):
    return native | (state << 4) | (key << 9)


def row(pfn, value, backing=0):
    return [hex(pfn), hex(value), hex(backing)]


def capture(epoch='fixture-epoch', sentinel=True, partial=False, recovery=False, exact_retry=False,
            validated=False, retention=False, legacy=False):
    """Independently specified cells and consumers, not expected values decoded by the auditor."""
    common = {'schema': 'ksword.pfn.raw', 'version': 1 if legacy else 2, 'domain': 'fixture-NT-domain', 'epoch': epoch}
    output = []
    def record(kind, **values):
        value = dict(common, kind=kind, **values)
        output.append(value)
        return value
    ranges = [{'firstPfn': '0x64', 'pageCount': '65537' if retention else '4'}]
    if not retention:
        ranges.append({'firstPfn': '0xc8', 'pageCount': '4'})
    record('header', started='2026-10-07T12:00:00.000Z', pageBytes=4096, ranges=ranges,
           build={'major': 10, 'minor': 0, 'build': 26100, 'known': True},
           architecture={'process': 'x64', 'native': 'x64'},
           nativeAbi='x64 Superfetch=45 PFN=1 PrivateSource=8 identityBytes=24',
           semanticsValidated=validated, semanticValidation='Synthetic fixture only; no OS validation',
           limits={'identityBatchPages': 4096, 'recoveryQueriesPerBatch': 64, 'groups': 65536,
                   'samplesPerUse': 256, 'timingRecords': 4096, 'rawEncodedRecordBytes': 8388608},
           observer={'workingSetBefore': '0', 'privateBefore': '0', 'known': True})
    query_number = 0
    def query(operation, count=0, first=0, status=0):
        nonlocal query_number
        query_number += 1
        native_class = 6 if operation in ('pages', 'identities') else (8 if operation.startswith('owners') else 17)
        abi = 1 if native_class == 6 else (8 if native_class == 8 else 2)
        driver_op = {'pages': 2, 'identities': 4}.get(operation, 3 if native_class == 8 else 1)
        return record('query', ordinal=str(query_number), operation=operation, pageCount=str(count),
                      firstPfn=hex(first), startUs=str(query_number * 10), endUs=str(query_number * 10 + 5),
                      started='2026-10-07T12:00:00.001Z', finished='2026-10-07T12:00:00.002Z',
                      status=hex(status), selectedPath='Unavailable' if status & 0x80000000 else 'R3 Native',
                      nativeAccepted=0 if status & 0x80000000 else 1, driverAccepted=0,
                      nativeAttempts=[{'informationClass': native_class, 'abiVersion': abi,
                                       'status': hex(status), 'invoked': True}],
                      driver={'operation': driver_op if status & 0x80000000 else 0,
                              'status': '0xc00000bb' if status & 0x80000000 else '0x0',
                              'transportStatus': '0x0', 'available': False, 'invoked': False})
    owners = [{'key': '0xaa', 'pid': 41, 'sessionId': 0, 'name': 'worker.exe'},
              {'key': '0xbb', 'pid': 0, 'sessionId': 0, 'name': 'MemCompression'}]
    if retention:
        owners.append({'key': '0xcc', 'pid': 42, 'sessionId': 0, 'name': 'MemCompression'})
    q = query('ranges_before')
    record('ranges', phase='before', queryOrdinal=q['ordinal'], queryStatus=q['status'], ranges=copy.deepcopy(ranges))
    q = query('owners_before', count=16384)
    record('owners', phase='before', queryOrdinal=q['ordinal'], queryStatus=q['status'], owners=copy.deepcopy(owners))
    cells = []
    if retention:
        rows = [row(100, frame(0, 6, 0xaa)), row(101, frame(0, 3, 0xbb))]
        rows.extend(row(102 + index, frame(1, 6), (index + 1) << 12) for index in range(65534))
        rows.append(row(100 + 65536, frame(0, 6, 0xcc)))
        spans = [rows[offset:offset + 4096] for offset in range(0, len(rows), 4096)]
        cells = [(0, 6, 2), (13 if legacy else 0, 3, 1), (1, 6, 65534)]
    else:
        rows_a = [row(100, frame(0, 6, 0xaa)), row(101, frame(1, 6), 0x1001),
                  row(102, frame(12, 3)), row(103, U64 if sentinel and not exact_retry else frame(11, 7))]
        rows_b = [row(200, frame(0, 3, 0xbb)), row(201, frame(0, 6, 0xcc)),
                  row(202, frame(1, 2), 0x2000), row(203, frame(15, 1))]
        spans = [rows_a] if partial or recovery else [rows_a, rows_b]
        cells = [(0, 6, 1), (12, 6, 1)]
        if not recovery:
            cells.append((15, 3, 1))
            if not sentinel or exact_retry:
                cells.append((11, 7, 1))
        if not partial and not recovery:
            cells.extend([(13 if legacy else 0, 3, 1), (0, 6, 1), (1, 2, 1), (14, 1, 1)])
    attempts = 0
    for chunk_number, final_rows in enumerate(spans):
        first = int(final_rows[0][0], 16)
        request_rows = copy.deepcopy(final_rows)
        if recovery:
            request_rows = [row(first + index, U64) for index in range(4)]
        elif exact_retry and chunk_number == 0:
            request_rows[3] = row(103, U64)
        q = query('pages', len(request_rows), first, 0xc000003e if recovery else 0)
        start = q['startUs']
        record('identities', role='query_attempt', queryOrdinal=q['ordinal'], firstPfn=hex(first),
               pageCount=str(len(request_rows)), requestCount=str(len(request_rows)),
               startUs=q['startUs'], endUs=q['endUs'], status=q['status'],
               synthesized=recovery, identities=request_rows)
        attempts += len(request_rows)
        if recovery:
            q = query('pages', 2, 100)
            record('identities', role='query_attempt', queryOrdinal=q['ordinal'], firstPfn='0x64',
                   pageCount='2', requestCount='2', startUs=q['startUs'], endUs=q['endUs'],
                   status=q['status'], synthesized=False, identities=copy.deepcopy(final_rows[:2]))
            q = query('pages', 2, 102, 0xc0000022)
            record('identities', role='query_attempt', queryOrdinal=q['ordinal'], firstPfn='0x66',
                   pageCount='2', requestCount='2', startUs=q['startUs'], endUs=q['endUs'],
                   status=q['status'], synthesized=True, identities=[row(102, U64), row(103, U64)])
            final_rows = final_rows[:2] + [row(102, U64), row(103, U64)]
            attempts += 4
        elif exact_retry and chunk_number == 0:
            q = query('identities', 1, 103)
            record('identities', role='query_attempt', queryOrdinal=q['ordinal'], firstPfn='0x67',
                   pageCount='1', requestCount='1', startUs=q['startUs'], endUs=q['endUs'],
                   status=q['status'], synthesized=False, identities=[row(103, frame(11, 7))])
            attempts += 1
        record('ledger_chunk', queryOrdinal=q['ordinal'], chunkOrdinal=str(chunk_number),
               firstPfn=hex(first), pageCount=str(len(final_rows)), requestCount=str(len(final_rows)),
               startUs=start, endUs=str(int(q['endUs']) + 1), status=q['status'], chunkComplete=True,
               classificationPhase='Native identity; deferred compression keys are listed in the footer',
               identities=copy.deepcopy(final_rows))
    q = query('owners_after', count=16384)
    record('owners', phase='after', queryOrdinal=q['ordinal'], queryStatus=q['status'], owners=copy.deepcopy(owners))
    q = query('ranges_after')
    record('ranges', phase='after', queryOrdinal=q['ordinal'], queryStatus=q['status'], ranges=copy.deepcopy(ranges))
    record('owners', phase='final', owners=[dict(key=owner['key'], pid=owner['pid'], name=owner['name'],
                                              seenBefore=True, seenAfter=True,
                                              **({} if legacy else dict(leaseHeld=bool(owner['pid']),
                                                  createTimeBefore='100' if owner['pid'] else '0',
                                                  createTimeAfter='100' if owner['pid'] else '0',
                                                  lifetimeVerified=bool(owner['pid'])))) for owner in owners])
    counts, unknown, resolved, unresolved, na, objects = (ZERO() for _ in range(6))
    for use, state, amount in cells:
        counts[use][state] = str(int(counts[use][state]) + amount)
        (unresolved if state in (3, 4, 6, 7) else na)[use][state] = counts[use][state]
    resolved[0][6] = '1'
    unresolved[0][6] = str(int(unresolved[0][6]) - 1)
    if retention:
        objects[1][6] = '65534'
    else:
        objects[12][6] = '1'
        if not recovery:
            unknown[12][3] = '1'
        if not partial and not recovery:
            objects[1][2] = '1'
    visited = sum(len(span) for span in spans)
    unreadable = 0 if retention or exact_retry or not sentinel else (2 if recovery else 1)
    expected = 65537 if retention else 8
    unscanned = expected - visited
    record('footer', complete=not unreadable and not unscanned and not partial,
           cancelled=partial, rangesChanged=False, reconciles=True, ledgerRawComplete=True,
           expectedPages=str(expected), visitedPages=str(visited), unreadablePages=str(unreadable),
           unscannedPages=str(unscanned), ledgerPages=str(visited), attemptPages=str(attempts), chunks=str(len(spans)),
           categories=counts, unknownByNativeUse=unknown,
           ownerCoverage={'resolved': resolved, 'unresolved': unresolved, 'notApplicable': na, 'objectKeyKnown': objects},
           compressionKeys=['0xbb'] if legacy and (retention or not partial and not recovery) else [],
           ownerResolvedKeys=['0xaa'], nativeAbiObserved=True, semanticsValidated=validated,
           successfulNativeBatches=str(sum(x['kind'] == 'query' and x['selectedPath'] == 'R3 Native' for x in output)),
           successfulDriverBatches='0',
           statuses=dict(ranges='0x0', ownersBefore='0x0', ownersAfter='0x0', lastPage='0x0'),
           ownerConflicts='0', groupedOverflowPages='1' if retention else '0',
           batchTimingOverflow='0', rawBytesBeforeFooter='0',
           observer={'workingSetBefore': '0', 'workingSetAfter': '0', 'workingSetMax': '0',
                     'privateBefore': '0', 'privateAfter': '0', 'privateMax': '0', 'known': True,
                     'beforeKnown': True, 'afterKnown': True, 'samples': '6'})
    return output


class EvidenceAuditTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1] / '.codex-build-logs' / 'pfn-evidence-audit-tests'
        root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='fixture-', dir=root)
        self.folder = Path(self.temp.name).resolve()
        assert self.folder.parent == root.resolve()  # Only this generated directory is cleaned.
    def tearDown(self):
        self.temp.cleanup()
    def write(self, values, name='capture.jsonl'):
        path = self.folder / name
        total = 0
        with path.open('wb') as stream:
            for value in values:
                if value['kind'] == 'footer':
                    value['rawBytesBeforeFooter'] = str(total)
                encoded = (json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n').encode('utf-8')
                stream.write(encoded)
                total += len(encoded)
        return path
    def check_rejected(self, values):
        with self.assertRaises((ValueError, KeyError, TypeError)):
            subject.audit(self.write(values))

    def test_exact_schema_multiple_ranges_future_sentinel_owner_and_real_zero(self):
        result = subject.audit(self.write(capture()), '0xc8')
        self.assertEqual(result['expectedPages'], '8')
        self.assertEqual(result['visitedPages'], '8')
        self.assertEqual(result['unreadableBytes'], '4096')
        self.assertEqual(result['unknownInUseBytes'], '4096')
        self.assertEqual(result['knownUseOwnerUnresolvedBytes'], str(3 * 4096))
        self.assertFalse(result['complete'])
        self.assertEqual(result['categories'][0][3], '1')  # PID0 compression source is not a resolved consumer.
        self.assertEqual(result['lookup']['useIndex'], 0)
        self.assertEqual(result['observer']['privateBefore'], '0')  # Valid zero, not unavailable.
        values = capture()
        for value in values:
            if value['kind'] in ('identities', 'ledger_chunk'):
                for item in value['identities']:
                    if item[0] == '0x65':
                        item[2] = '0x1'  # Image flag exists, object key does not.
        values[-1]['ownerCoverage']['objectKeyKnown'][12][6] = '0'
        self.assertTrue(subject.audit(self.write(values))['ledgerReplayed'])

    def test_partial_cancel_and_lookup_boundaries(self):
        path = self.write(capture(partial=True))
        result = subject.audit(path, 200)
        self.assertEqual(result['unscannedBytes'], str(4 * 4096))
        self.assertEqual(result['lookup']['status'], 'Unscanned')
        self.assertEqual(subject.audit(path, 103)['lookup']['status'], 'Unreadable')
        self.assertEqual(subject.audit(path, 150)['lookup']['status'], 'Outside observation domain')
        self.assertFalse(result['complete'])

    def test_failed_bulk_sparse_recovery_and_exact_sentinel_retry(self):
        result = subject.audit(self.write(capture(recovery=True)))
        self.assertEqual(result['queryAttemptPages'], '8')
        self.assertEqual(result['unreadableBytes'], '8192')
        self.assertEqual(result['unknownInUseBytes'], '0')  # Failed future-code page is not counted as known-unknown.
        self.assertEqual(result['unscannedBytes'], '16384')
        result = subject.audit(self.write(capture(exact_retry=True)), 103)
        self.assertEqual(result['queryAttemptPages'], '9')
        self.assertEqual(result['unreadableBytes'], '0')
        self.assertEqual(result['lookup']['useIndex'], 11)
        self.assertTrue(result['complete'])

    def test_retention_boundary_keeps_name_only_compression_private(self):
        result = subject.audit(self.write(capture(retention=True)), 100 + 65536)
        self.assertEqual(result['expectedPages'], '65537')
        self.assertEqual(result['lookup']['useIndex'], 0)  # Named compression source whose group was not retained.
        self.assertEqual(result['categories'][0][3], '1')
        self.assertEqual(result['categories'][0][6], '2')
        self.assertTrue(result['complete'])

    def test_malformed_footer_counters_flags_and_keys(self):
        mutations = {
            'expected': lambda v: v[-1].update(expectedPages='7'),
            'visited': lambda v: v[-1].update(visitedPages='7'),
            'failed': lambda v: v[-1].update(unreadablePages='0'),
            'unscanned': lambda v: v[-1].update(unscannedPages='1'),
            'raw ledger': lambda v: v[-1].update(ledgerPages='7'),
            'raw attempts': lambda v: v[-1].update(attemptPages='7'),
            'raw chunks': lambda v: v[-1].update(chunks='1'),
            'raw complete': lambda v: v[-1].update(ledgerRawComplete=False),
            'reconcile': lambda v: v[-1].update(reconciles=False),
            'false complete': lambda v: v[-1].update(complete=True),
            'truthy complete': lambda v: v[-1].update(complete='true'),
            'truthy version': lambda v: v[0].update(version=True),
            'unsupported schema': lambda v: v[0].update(version=3),
            'duplicate compression': lambda v: v[-1].update(compressionKeys=['0xbb', '0xbb']),
            'unobserved compression': lambda v: v[-1].update(compressionKeys=['0xcc']),
            'invented resolved': lambda v: v[-1].update(ownerResolvedKeys=['0xaa', '0xbb']),
            'omitted resolved': lambda v: v[-1].update(ownerResolvedKeys=[]),
            'forged overflow': lambda v: v[-1].update(groupedOverflowPages='1'),
            'semantic discrepancy': lambda v: v[-1].update(semanticsValidated=True),
            'unknown becomes private': lambda v: v[-1]['categories'][15].__setitem__(3, '0'),
            'invented object clue': lambda v: v[-1]['ownerCoverage']['objectKeyKnown'][0].__setitem__(6, '1'),
            'missing object clue': lambda v: v[-1]['ownerCoverage']['objectKeyKnown'][12].__setitem__(6, '0'),
            'in-use N/A': lambda v: (v[-1]['ownerCoverage']['unresolved'][12].__setitem__(6, '0'), v[-1]['ownerCoverage']['notApplicable'][12].__setitem__(6, '1')),
            'standby owner': lambda v: (v[-1]['ownerCoverage']['notApplicable'][1].__setitem__(2, '0'), v[-1]['ownerCoverage']['unresolved'][1].__setitem__(2, '1')),
        }
        for name, change in mutations.items():
            with self.subTest(name=name):
                values = capture()
                change(values)
                self.check_rejected(values)

    def test_chunk_domain_order_and_raw_response_integrity(self):
        mutations = {
            'first ordinal': lambda v: next(x for x in v if x['kind'] == 'ledger_chunk').update(chunkOrdinal='1'),
            'chunk unfinished': lambda v: next(x for x in v if x['kind'] == 'ledger_chunk').update(chunkComplete=False),
            'chunk wrong count': lambda v: next(x for x in v if x['kind'] == 'ledger_chunk').update(pageCount='3'),
            'chunk wrong first': lambda v: next(x for x in v if x['kind'] == 'ledger_chunk').update(firstPfn='0x65'),
            'query wrong ordinal': lambda v: next(x for x in v if x['kind'] == 'query').update(ordinal='2'),
            'backwards time': lambda v: next(x for x in v if x['kind'] == 'query').update(endUs='1'),
            'mixed epoch': lambda v: next(x for x in v if x['kind'] == 'ledger_chunk').update(epoch='another-epoch'),
            'mixed domain': lambda v: next(x for x in v if x['kind'] == 'ledger_chunk').update(domain='another-domain'),
            'unordered ranges': lambda v: v[0]['ranges'].reverse(),
            'overlapping ranges': lambda v: v[0]['ranges'][1].update(firstPfn='0x66'),
            'adjacent unmerged': lambda v: v[0]['ranges'][1].update(firstPfn='0x68'),
            'zero range': lambda v: v[0]['ranges'][0].update(pageCount='0'),
            'attempt wrong status': lambda v: next(x for x in v if x['kind'] == 'identities').update(status='0xc0000022'),
            'attempt synthesis': lambda v: next(x for x in v if x['kind'] == 'identities').update(synthesized=True),
            'attempt out of range': lambda v: next(x for x in v if x['kind'] == 'identities')['identities'][0].__setitem__(0, '0x96'),
            'attempt reordered': lambda v: next(x for x in v if x['kind'] == 'identities')['identities'].reverse(),
            'attempt missing': lambda v: next(x for x in v if x['kind'] == 'identities')['identities'].pop(),
            'attempt duplicate': lambda v: next(x for x in v if x['kind'] == 'identities')['identities'].__setitem__(1, row(100, frame(0, 6, 0xaa))),
            'ledger forged raw': lambda v: next(x for x in v if x['kind'] == 'ledger_chunk')['identities'][0].__setitem__(1, '0x60'),
            'ledger duplicate': lambda v: next(x for x in v if x['kind'] == 'ledger_chunk')['identities'][1].__setitem__(0, '0x64'),
            'ledger reordered': lambda v: next(x for x in v if x['kind'] == 'ledger_chunk')['identities'].reverse(),
            'unknown record': lambda v: next(x for x in v if x['kind'] == 'query').update(kind='execute'),
        }
        for name, change in mutations.items():
            with self.subTest(name=name):
                values = capture()
                change(values)
                self.check_rejected(values)
        values = capture()
        values.remove(next(x for x in values if x['kind'] == 'ledger_chunk'))
        self.check_rejected(values)

    def test_owner_endpoint_conflicts_and_retention_bounds(self):
        values = capture()
        after = next(x for x in values if x['kind'] == 'owners' and x['phase'] == 'after')
        after['owners'][0]['pid'] = 51
        final = next(x for x in values if x['kind'] == 'owners' and x['phase'] == 'final')
        final['owners'].pop(0)
        footer = values[-1]
        footer['ownerResolvedKeys'] = []
        footer['ownerConflicts'] = '1'
        footer['ownerCoverage']['resolved'][0][6] = '0'
        footer['ownerCoverage']['unresolved'][0][6] = '2'
        self.assertTrue(subject.audit(self.write(values))['ledgerReplayed'])
        footer['ownerConflicts'] = '0'
        self.check_rejected(values)
        for kind in ('before', 'after', 'final'):
            values = capture()
            target = next(x for x in values if x['kind'] == 'owners' and x['phase'] == kind)
            target['owners'][0]['name'] = 'x' * 17
            self.check_rejected(values)
        values = capture()
        next(x for x in values if x['kind'] == 'owners' and x['phase'] == 'final')['owners'].append(
            {'key': '0xaa', 'pid': 41, 'name': 'worker.exe', 'seenBefore': True, 'seenAfter': True})
        self.check_rejected(values)

    def test_missing_footer_line_bound_duplicate_fields_and_instructions_are_data(self):
        values = capture()
        self.check_rejected(values[:-1])
        self.check_rejected(values + [copy.deepcopy(values[-1])])
        path = self.write(capture())
        path.write_bytes(path.read_bytes().rstrip(b'\n'))
        with self.assertRaisesRegex(ValueError, 'not finalized'):
            subject.audit(path)
        path.write_bytes(b'x' * (subject.MAX_LINE + 1) + b'\n')
        with self.assertRaisesRegex(ValueError, 'record bound'):
            subject.audit(path)
        path.write_bytes(b'{"schema":"ksword.pfn.raw","version":1,"kind":"header","kind":"execute"}\n')
        with self.assertRaisesRegex(ValueError, 'duplicate JSON'):
            subject.audit(path)
        values = capture()
        marker = self.folder / 'must-not-exist'
        values[0]['semanticValidation'] = f"__import__('pathlib').Path({str(marker)!r}).write_text('executed')"
        self.assertTrue(subject.audit(self.write(values))['ledgerReplayed'])
        self.assertFalse(marker.exists())

    def test_forced_empty_complete_cannot_pass(self):
        values = capture()
        values[0]['ranges'] = []
        self.check_rejected(values)
        values = [values[0], values[-1]]
        footer = values[-1]
        for key in ('expectedPages', 'visitedPages', 'unreadablePages', 'unscannedPages',
                    'ledgerPages', 'attemptPages', 'chunks'):
            footer[key] = '0'
        footer['categories'] = ZERO()
        footer['unknownByNativeUse'] = ZERO()
        footer['ownerCoverage'] = {key: ZERO() for key in ('resolved', 'unresolved', 'notApplicable', 'objectKeyKnown')}
        footer['compressionKeys'] = footer['ownerResolvedKeys'] = []
        footer['complete'] = True
        self.check_rejected(values)

    def test_three_capture_criterion_is_claim_based_and_does_not_hide_failures(self):
        samples = [subject.audit(self.write(capture(epoch=f'epoch-{index}', sentinel=False), f'{index}.jsonl')) for index in range(3)]
        self.assertFalse(subject.criterion(samples)['passed'])
        for sample in samples:
            sample['semanticsValidated'] = True  # Synthetic mock claim, never a real Windows measurement.
        self.assertTrue(subject.criterion(samples)['passed'])
        for key, changed in (('epoch', samples[0]['epoch']), ('domain', 'another-domain'), ('nativeAbi', 'different-ABI'),
                             ('complete', False), ('unknownInUseBytes', str(64 * 1024 * 1024 + 4096))):
            mutated = copy.deepcopy(samples)
            mutated[2][key] = changed
            self.assertFalse(subject.criterion(mutated)['passed'])
        mutated = copy.deepcopy(samples)
        mutated[2]['build']['build'] += 1
        self.assertFalse(subject.criterion(mutated)['passed'])
        mutated = copy.deepcopy(samples)
        mutated[2]['architecture']['native'] = 'arm64'
        self.assertFalse(subject.criterion(mutated)['passed'])
        self.assertFalse(subject.criterion(samples[:2])['passed'])

    def test_range_endpoints_and_attempt_provenance_are_recomputed(self):
        mutations = {
            'range after changed': lambda v: next(x for x in v if x['kind'] == 'ranges' and x['phase'] == 'after')['ranges'][0].update(pageCount='5'),
            'range before differs': lambda v: next(x for x in v if x['kind'] == 'ranges' and x['phase'] == 'before')['ranges'][0].update(pageCount='5'),
            'empty after success': lambda v: next(x for x in v if x['kind'] == 'ranges' and x['phase'] == 'after').update(ranges=[]),
            'native not called': lambda v: next(x for x in v if x['kind'] == 'query')['nativeAttempts'][0].update(invoked=False),
            'wrong native class': lambda v: next(x for x in v if x['kind'] == 'query')['nativeAttempts'][0].update(informationClass=6),
            'wrong ABI': lambda v: next(x for x in v if x['kind'] == 'query')['nativeAttempts'][0].update(abiVersion=8),
            'wrong native status': lambda v: next(x for x in v if x['kind'] == 'query')['nativeAttempts'][0].update(status='0xc00000bb'),
            'wrong endpoint request': lambda v: next(x for x in v if x['kind'] == 'query').update(pageCount='1'),
            'wrong accepted count': lambda v: next(x for x in v if x['kind'] == 'query').update(nativeAccepted=0),
            'wrong native total': lambda v: v[-1].update(successfulNativeBatches='0'),
            'false range status': lambda v: v[-1]['statuses'].update(ranges='0xc0000022'),
            'false owner status': lambda v: v[-1]['statuses'].update(ownersAfter='0xc0000022'),
            'endpoint swapped': lambda v: next(x for x in v if x['kind'] == 'query' and x['operation'] == 'owners_before').update(operation='owners_after'),
            'invented driver total': lambda v: v[-1].update(successfulDriverBatches='1'),
            'wrong observed ABI': lambda v: v[-1].update(nativeAbiObserved=False),
            'first PFN': lambda v: next(x for x in v if x['kind'] == 'identities').update(firstPfn='0x65'),
            'page count': lambda v: next(x for x in v if x['kind'] == 'identities').update(pageCount='1'),
            'attempt start': lambda v: next(x for x in v if x['kind'] == 'identities').update(startUs='0'),
            'attempt end': lambda v: next(x for x in v if x['kind'] == 'identities').update(endUs='999'),
            'driver not called': lambda v: next(x for x in v if x['kind'] == 'query').update(selectedPath='R0 fallback'),
            'driver fake call': lambda v: next(x for x in v if x['kind'] == 'query')['driver'].update(invoked=True),
        }
        for name, mutation in mutations.items():
            with self.subTest(name=name):
                values = capture(sentinel=False, validated=True)
                mutation(values)
                self.check_rejected(values)
        # Changed ranges are valid partial evidence when both claims agree.
        values = capture(sentinel=False)
        next(x for x in values if x['kind'] == 'ranges' and x['phase'] == 'after')['ranges'][0]['pageCount'] = '5'
        values[-1].update(rangesChanged=True, complete=False)
        self.assertFalse(subject.audit(self.write(values))['complete'])
        # An unordered native payload normalizes to the same header scope.
        values = capture(sentinel=False)
        for x in values:
            if x['kind'] == 'ranges': x['ranges'].reverse()
        self.assertTrue(subject.audit(self.write(values))['complete'])
        # Successful driver-only PFN operation, after switching to fallback.
        values = capture(sentinel=False)
        q = next(x for x in values if x['kind'] == 'query' and x['operation'] == 'pages')
        q.update(nativeAttempts=[], nativeAccepted=0, driverAccepted=1, selectedPath='R0 fallback',
                 driver=dict(operation=2, status='0x0', transportStatus='0x0', available=True, invoked=True))
        values[-1]['successfulNativeBatches'] = str(int(values[-1]['successfulNativeBatches']) - 1)
        values[-1]['successfulDriverBatches'] = '1'
        self.assertTrue(subject.audit(self.write(values))['complete'])
        # Failed range recheck remains partial; a forged complete flag is rejected.
        values = capture(sentinel=False, validated=True)
        q = next(x for x in values if x['kind'] == 'query' and x['operation'] == 'ranges_after')
        q.update(status='0xc0000022', selectedPath='Unavailable', nativeAccepted=0)
        q['nativeAttempts'][0]['status'] = q['status']
        r = next(x for x in values if x['kind'] == 'ranges' and x['phase'] == 'after')
        r.update(queryStatus=q['status'], ranges=[])
        values[-1].update(rangesChanged=True, complete=False)
        values[-1]['successfulNativeBatches'] = str(int(values[-1]['successfulNativeBatches']) - 1)
        self.assertFalse(subject.audit(self.write(values))['complete'])
        values[-1].update(rangesChanged=False, complete=True)
        self.check_rejected(values)

    def test_lifetime_witnesses_and_legacy_history_do_not_certify_consumers(self):
        for field, value in (('leaseHeld', False), ('seenAfter', False), ('createTimeBefore', '0'),
                             ('createTimeAfter', '101'), ('lifetimeVerified', False)):
            values = capture(sentinel=False)
            final = next(x for x in values if x['kind'] == 'owners' and x['phase'] == 'final')
            final['owners'][0][field] = value
            self.check_rejected(values)
        values = capture(sentinel=False)
        next(x for x in values if x['kind'] == 'owners' and x['phase'] == 'after')['owners'].pop(0)
        final = next(x for x in values if x['kind'] == 'owners' and x['phase'] == 'final')
        final['owners'][0].update(seenAfter=False, lifetimeVerified=False)
        values[-1]['ownerResolvedKeys'] = []
        values[-1]['ownerCoverage']['resolved'][0][6] = '0'
        values[-1]['ownerCoverage']['unresolved'][0][6] = '2'
        result = subject.audit(self.write(values))
        self.assertTrue(result['complete'])
        self.assertEqual(result['knownUseOwnerUnresolvedBytes'], str(5 * 4096))
        samples = [subject.audit(self.write(capture(sentinel=False, validated=True, legacy=True, epoch=f'legacy-{i}'), f'legacy-{i}.jsonl')) for i in range(3)]
        self.assertEqual(samples[0]['categories'][13][3], '1')
        self.assertFalse(samples[0]['attributionPolicyValidated'])
        self.assertFalse(subject.criterion(samples)['passed'])

    def test_byte_count_and_cli_exit_and_lookup(self):
        path = self.write(capture())
        content = path.read_bytes()
        values = capture()
        self.write(values)
        footer = values[-1]
        footer['rawBytesBeforeFooter'] = str(int(footer['rawBytesBeforeFooter']) + 1)
        lines = [json.dumps(value, separators=(',', ':')).encode() + b'\n' for value in values]
        path.write_bytes(b''.join(lines))
        with self.assertRaisesRegex(ValueError, 'raw byte count'):
            subject.audit(path)
        path.write_bytes(content)
        report = self.folder / 'report.json'
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(subject.main([str(path), '--pfn', '0xc8', '--output', str(report)]), 0)
            self.assertEqual(subject.main([str(path), '--require-proposed-criterion']), 2)
            self.assertEqual(subject.main([str(path), '--output', str(self.folder / 'missing-parent' / 'report.json')]), 1)
        self.assertEqual(json.loads(report.read_text())['samples'][0]['lookup']['useIndex'], 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
