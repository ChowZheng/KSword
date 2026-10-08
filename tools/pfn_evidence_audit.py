"""Stream-replay PFN evidence; never execute content from evidence files.

Arithmetic replay does not validate Windows classification semantics. The
three-capture 64 MiB criterion is proposed and requires a capture claim of
validated build semantics. This tool does not certify that claim or create a
supported-Windows-build matrix.
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import json
from pathlib import Path

U64 = (1 << 64) - 1
IN_USE = (3, 4, 6, 7)
MAX_LINE = 8 * 1024 * 1024
MAX_KEYS = 65536
OPERATIONS = {'pages', 'identities', 'ranges_before', 'ranges_after', 'owners_before', 'owners_after'}
KINDS = {'header', 'query', 'identities', 'ledger_chunk', 'owners', 'ranges', 'footer'}


def integer(value, maximum=U64):
    if isinstance(value, bool):
        raise ValueError('boolean is not an integer count')
    if isinstance(value, str):
        number = int(value, 16 if value.lower().startswith('0x') else 10)
    elif isinstance(value, int):
        number = value
    else:
        raise ValueError('count must be an integer or a decimal/hex string')
    if not 0 <= number <= maximum:
        raise ValueError('count is outside its unsigned range')
    return number


def boolean(value):
    if type(value) is not bool:
        raise ValueError('a capture flag must be a JSON boolean')
    return value


def text(value, maximum=1024, empty=False):
    if not isinstance(value, str) or len(value) > maximum or (not empty and not value):
        raise ValueError('missing or excessive bounded text field')
    return value


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON field')
        result[key] = value
    return result


def records(path, hasher=None, byte_progress=None):
    with Path(path).open('rb') as stream:
        line_number = 0
        while True:
            line = stream.readline(MAX_LINE + 1)
            if not line:
                break
            line_number += 1
            if len(line) > MAX_LINE:
                raise ValueError(f'line {line_number} exceeds the record bound')
            if not line.endswith(b'\n'):
                raise ValueError(f'line {line_number} is not finalized')
            if byte_progress is not None:
                byte_progress['offset'] = byte_progress['bytes']
                byte_progress['bytes'] += len(line)
            if hasher is not None:
                hasher.update(line)
            record = json.loads(line, object_pairs_hook=unique_object)
            if not isinstance(record, dict) or record.get('schema') != 'ksword.pfn.raw' or type(record.get('version')) is not int or record['version'] not in (1, 2):
                raise ValueError(f'line {line_number} has an unsupported evidence schema')
            if record.get('kind') not in KINDS:
                raise ValueError('unsupported record kind')
            yield record


def matrix(value, rows=16):
    if not isinstance(value, list) or len(value) != rows or any(not isinstance(row, list) or len(row) != 8 for row in value):
        raise ValueError('invalid use/state matrix dimensions')
    return [[integer(cell) for cell in row] for row in value]


def normalized_ranges(value):
    if not isinstance(value, list) or len(value) > 512:
        raise ValueError('excessive or missing physical ranges')
    ranges = []
    for raw in value:
        first, count = integer(raw['firstPfn'], (1 << 40) - 1), integer(raw['pageCount'], 1 << 40)
        if not count or first + count > 1 << 40:
            raise ValueError('invalid physical range')
        if ranges and first <= ranges[-1][0] + ranges[-1][1]:
            raise ValueError('physical ranges are not normalized/disjoint')
        ranges.append((first, count))
    return ranges


def query_ranges(value):
    # Native endpoints need not be ordered; normalize exactly as the collector.
    if not isinstance(value, list) or len(value) > 512:
        raise ValueError('excessive or missing physical ranges')
    rows = []
    for raw in value:
        first, count = integer(raw['firstPfn'], (1 << 40) - 1), integer(raw['pageCount'], 1 << 40)
        if not count or first + count > 1 << 40:
            raise ValueError('invalid native physical range')
        rows.append((first, count))
    result = []
    for first, count in sorted(rows):
        if result and first <= result[-1][0] + result[-1][1]:
            previous, amount = result[-1]
            result[-1] = (previous, max(previous + amount, first + count) - previous)
        else:
            result.append((first, count))
    return result


def validate_provider(record, operation, status, version):
    failed = bool(status & 0x80000000)
    native = record['nativeAttempts']
    if not isinstance(native, list) or len(native) > 2:
        raise ValueError('excessive native query provenance')
    expected_class = 6 if operation in ('pages', 'identities') else (8 if operation.startswith('owners') else 17)
    versions = []
    for attempt in native:
        if integer(attempt['informationClass']) != expected_class:
            raise ValueError('native information class differs from operation')
        versions.append(integer(attempt['abiVersion']))
        value = integer(attempt['status'], (1 << 32) - 1)
        if not boolean(attempt['invoked']) and value != 0xc00000bb:
            raise ValueError('non-invoked native path claims a result')
    allowed = ([2], [2, 1]) if expected_class == 17 else ([8],) if expected_class == 8 else ([], [1])
    if versions not in allowed:
        raise ValueError('native ABI version/order differs from operation')
    driver = record['driver']
    expected_op = {'pages': 2, 'identities': 4}.get(operation, 3 if expected_class == 8 else 1)
    driver_op = integer(driver['operation'])
    driver_status, transport = (integer(driver[key], (1 << 32) - 1) for key in ('status', 'transportStatus'))
    available, invoked = boolean(driver['available']), boolean(driver['invoked'])
    if driver_op not in (0, expected_op) or invoked and (not available or driver_op != expected_op):
        raise ValueError('driver invocation/operation differs from operation')
    if not driver_op and (invoked or available or driver_status or transport):
        raise ValueError('unused driver path claims a result')
    if driver_op and not invoked and (driver_status != 0xc00000bb or transport):
        raise ValueError('non-invoked driver path claims a result')
    if transport & 0x80000000 and driver_status != transport:
        raise ValueError('driver transport error is hidden')
    path = record['selectedPath']
    if path == 'R3 Native':
        if failed or not native or not native[-1]['invoked'] or integer(native[-1]['status']) != status or driver_op:
            raise ValueError('selected native path contradicts its calls/status')
    elif path == 'R0 fallback':
        if failed or not invoked or transport & 0x80000000 or driver_status != status:
            raise ValueError('selected driver path contradicts its calls/status')
    elif path == 'Unavailable':
        if not failed:
            raise ValueError('unavailable operation claims success')
        possibilities = {0xc000003e}
        if native:
            possibilities.add(integer(native[-1]['status']))
        if driver_op:
            possibilities.add(driver_status)
        if status not in possibilities:
            raise ValueError('final failure differs from observed providers')
    else:
        raise ValueError('unknown provider path')
    if version == 2:
        native_count, driver_count = (integer(record[key], 1) for key in ('nativeAccepted', 'driverAccepted'))
        if native_count + driver_count > 1:
            raise ValueError('one operation claims multiple accepted providers')
        if native_count and (not native or not native[-1]['invoked'] or integer(native[-1]['status']) & 0x80000000):
            raise ValueError('accepted native batch has no successful invocation')
        if driver_count and (not invoked or transport & 0x80000000 or driver_status & 0x80000000):
            raise ValueError('accepted driver batch has no successful invocation')
        if path == 'R3 Native' and (native_count, driver_count) != (1, 0) or path == 'R0 fallback' and (native_count, driver_count) != (0, 1):
            raise ValueError('accepted batch counters differ from selected path')
        return native_count, driver_count
    return int(path == 'R3 Native'), int(path == 'R0 fallback')


def identity_kind(frame, backing, compression_keys):
    native, state = frame & 15, (frame >> 4) & 7
    if state < 2:
        return 14
    if native > 11:
        return 15
    if native == 0 and state in IN_USE and ((frame >> 9) & 0xFFFFFFFFFFFF) in compression_keys:
        return 13
    if native == 1 and backing & 1:
        return 12
    return native


def owner_record(owner, final=False):
    key = integer(owner['key'], (1 << 48) - 1)
    pid = integer(owner['pid'], (1 << 32) - 1)
    name = text(owner['name'], 16, empty=True)
    if any(ord(char) > 255 or char == '\0' for char in name):
        raise ValueError('owner name is not the retained Latin-1 native short name')
    result = {'pid': pid, 'name': name}
    if final:
        result['seenBefore'] = boolean(owner['seenBefore'])
        result['seenAfter'] = boolean(owner['seenAfter'])
        if not result['seenBefore'] and not result['seenAfter']:
            raise ValueError('final owner has no observed endpoint')
    else:
        integer(owner['sessionId'], (1 << 32) - 1)
    return key, result


def audit(path, lookup_pfn=None):
    path = Path(path)
    if lookup_pfn is not None:
        lookup_pfn = integer(lookup_pfn, (1 << 40) - 1)
    header = footer = None
    final_owners, endpoint_owners, conflicts = {}, {}, set()
    first_hash = hashlib.sha256()
    byte_progress = {'bytes': 0, 'offset': 0}
    footer_offset = 0
    for ordinal, record in enumerate(records(path, first_hash, byte_progress)):
        if ordinal == 0:
            if record['kind'] != 'header':
                raise ValueError('first record must be the capture header')
            header = record
            text(header.get('domain'), 256)
            text(header.get('epoch'), 128)
            if integer(header.get('pageBytes')) != 4096:
                raise ValueError('unsupported page size')
            text(header['nativeAbi'])
            boolean(header['semanticsValidated'])
            for key in ('major', 'minor', 'build'):
                integer(header['build'][key], (1 << 32) - 1)
            boolean(header['build']['known'])
            for key in ('process', 'native'):
                text(header['architecture'][key], 128)
        if footer is not None:
            raise ValueError('records follow the final footer')
        if record['version'] != header['version']:
            raise ValueError('mixed evidence policy versions')
        if record.get('domain') != header['domain'] or record.get('epoch') != header['epoch']:
            raise ValueError('mixed observation domain or epoch')
        if ordinal and record['kind'] == 'header':
            raise ValueError('capture contains a second header')
        if record['kind'] == 'owners':
            phase, rows = record.get('phase'), record.get('owners')
            if phase not in ('before', 'after', 'final') or not isinstance(rows, list) or len(rows) > 512:
                raise ValueError('invalid or excessive owner chunk')
            for owner in rows:
                key, observation = owner_record(owner, phase == 'final')
                if phase == 'final':
                    if not key or key in final_owners:
                        raise ValueError('redacted or duplicate final owner key')
                    if header['version'] == 2:
                        held = boolean(owner['leaseHeld'])
                        before, after = integer(owner['createTimeBefore']), integer(owner['createTimeAfter'])
                        verified = bool(observation['pid'] and observation['name'] and observation['seenBefore']
                                        and observation['seenAfter'] and held and before and before == after)
                        if boolean(owner['lifetimeVerified']) != verified or not held and (before or after):
                            raise ValueError('process lifetime claim contradicts its witnesses')
                        observation.update(leaseHeld=held, createTimeBefore=before, createTimeAfter=after, lifetimeVerified=verified)
                    final_owners[key] = observation
                elif key and key not in conflicts:
                    previous = endpoint_owners.get(key)
                    if previous is None:
                        observation.update(seenBefore=phase == 'before', seenAfter=phase == 'after')
                        endpoint_owners[key] = observation
                    elif previous['pid'] != observation['pid'] or previous['name'].lower() != observation['name'].lower():
                        del endpoint_owners[key]
                        conflicts.add(key)
                    else:
                        previous['seenBefore' if phase == 'before' else 'seenAfter'] = True
                if len(final_owners) > MAX_KEYS or len(endpoint_owners) + len(conflicts) > MAX_KEYS:
                    raise ValueError('excessive retained owner keys')
        if record['kind'] == 'footer':
            footer = record
            footer_offset = byte_progress['offset']
    if header is None or footer is None:
        raise ValueError('capture is incomplete: finalized header/footer required')
    endpoint_fields = ('pid', 'name', 'seenBefore', 'seenAfter')
    retained_endpoints = {key: {field: owner[field] for field in endpoint_fields} for key, owner in final_owners.items()}
    if retained_endpoints != endpoint_owners:
        raise ValueError('final owner observations differ from the native endpoints')

    def key_set(name):
        values = footer[name]
        if not isinstance(values, list) or len(values) > MAX_KEYS:
            raise ValueError('excessive retained consumer keys')
        parsed = [integer(key, (1 << 48) - 1) for key in values]
        if 0 in parsed or len(set(parsed)) != len(parsed):
            raise ValueError('redacted or duplicate retained consumer keys')
        return set(parsed)

    compression, resolved = key_set('compressionKeys'), key_set('ownerResolvedKeys')
    if header['version'] == 2 and compression:
        raise ValueError('compression requires an authoritative identity provider absent from this schema')
    for key in resolved:
        owner = final_owners.get(key)
        if not owner or not owner['pid'] or not owner['name'] or header['version'] == 2 and not owner['lifetimeVerified']:
            raise ValueError('resolved key has no final named process observation')
    for key in compression:
        owner = final_owners.get(key)
        if not owner or owner['name'].casefold() != 'memcompression':
            raise ValueError('compression key has no observed MemCompression source')
    ranges = normalized_ranges(header['ranges'])
    starts = [first for first, _ in ranges]

    def inside(pfn):
        index = bisect.bisect_right(starts, pfn) - 1
        return index >= 0 and pfn - ranges[index][0] < ranges[index][1]

    expected = sum(count for _, count in ranges)
    counts, unknown, owners_resolved, objects = ([[0] * 8 for _ in range(16)] for _ in range(4))
    visited = unreadable = chunks = attempts = range_index = range_offset = 0
    last_query = last_end = 0
    pending = span = None
    pending_payload = False
    endpoint_payload_count = 0
    first_span_start = 0
    lookup = None
    retained_groups = set()
    overflow_pages = 0
    second_hash = hashlib.sha256()
    endpoint_ranges = {}
    native_accepted = driver_accepted = 0
    abi_observed = False
    endpoint_operations = set()
    endpoint_statuses = {}

    def check_pending():
        if pending is not None and not pending_payload:
            raise ValueError('query has no retained response payload')

    for record in records(path, second_hash):
        kind = record['kind']
        if kind == 'query':
            check_pending()
            number = integer(record['ordinal'])
            start, end = integer(record['startUs']), integer(record['endUs'])
            if number != last_query + 1 or start > end or start < last_end:
                raise ValueError('query ordinal or monotonic interval is inconsistent')
            last_query, last_end = number, end
            operation = record['operation']
            if operation not in OPERATIONS:
                raise ValueError('unsupported query operation')
            count, first = integer(record['pageCount'], 16384), integer(record['firstPfn'], (1 << 40) - 1)
            status = integer(record['status'], (1 << 32) - 1)
            text(record['started'], 128)
            text(record['finished'], 128)
            accepted_native, accepted_driver = validate_provider(record, operation, status, header['version'])
            native_accepted += accepted_native
            driver_accepted += accepted_driver
            if operation not in ('pages', 'identities'):
                if operation in endpoint_operations or first or count != (16384 if operation.startswith('owners') else 0):
                    raise ValueError('endpoint operation/request metadata is inconsistent')
                required = {'ranges_before': set(), 'owners_before': {'ranges_before'},
                            'owners_after': {'owners_before'}, 'ranges_after': {'owners_after'}}[operation]
                if not required <= endpoint_operations or operation == 'ranges_before' and number != 1:
                    raise ValueError('endpoint query order differs from the sampling interval')
                if operation == 'owners_before' and ('owners_after' in endpoint_operations or 'ranges_after' in endpoint_operations):
                    raise ValueError('owner baseline follows the recheck')
                endpoint_operations.add(operation)
                endpoint_statuses[operation] = status
            elif 'owners_before' not in endpoint_operations or 'owners_after' in endpoint_operations or 'ranges_after' in endpoint_operations:
                raise ValueError('PFN sampling is outside the owner endpoint interval')
            elif not status & 0x80000000:
                abi_observed = True
            if operation in ('pages', 'identities') and not 1 <= count <= 4096:
                raise ValueError('PFN query count is outside the bounded native packet')
            pending = {'ordinal': number, 'operation': operation, 'count': count, 'first': first, 'status': status, 'start': start, 'end': end}
            pending_payload, endpoint_payload_count = False, 0
        elif kind in ('owners', 'ranges'):
            phase = record['phase']
            if kind == 'owners' and phase == 'final':
                check_pending()
                if expected and 'ranges_after' not in endpoint_operations:
                    raise ValueError('final owner proof precedes the endpoint recheck')
                continue
            if pending is None or pending['operation'] != f'{kind}_{phase}' or integer(record['queryOrdinal']) != pending['ordinal'] or integer(record['queryStatus'], (1 << 32) - 1) != pending['status']:
                raise ValueError('endpoint payload differs from its query provenance')
            pending_payload = True
            if kind == 'owners':
                endpoint_payload_count += len(record['owners'])
                if endpoint_payload_count > 16384 or (pending['status'] & 0x80000000 and record['owners']):
                    raise ValueError('owner payload exceeds or contradicts its native query')
            else:
                if phase in endpoint_ranges:
                    raise ValueError('duplicate range-query endpoint')
                rows = record['ranges']
                if pending['status'] & 0x80000000 and rows:
                    raise ValueError('failed range query has a nonempty response')
                endpoint_ranges[phase] = (not bool(pending['status'] & 0x80000000), query_ranges(rows))
        elif kind == 'identities':
            if pending is None or pending['operation'] not in ('pages', 'identities') or pending_payload or record.get('role') != 'query_attempt' or integer(record['queryOrdinal']) != pending['ordinal']:
                raise ValueError('query-attempt payload has no unique PFN query')
            for name, expected_value in (('firstPfn', pending['first']), ('pageCount', pending['count']),
                                         ('startUs', pending['start']), ('endUs', pending['end'])):
                if integer(record[name]) != expected_value:
                    raise ValueError('query-attempt request/interval differs from provenance')
            count = integer(record['requestCount'], 4096)
            rows = record['identities']
            if count != pending['count'] or not isinstance(rows, list) or len(rows) != count:
                raise ValueError('invalid query-attempt identity count')
            if integer(record['status'], (1 << 32) - 1) != pending['status']:
                raise ValueError('query-attempt status differs from provenance')
            synthesized = boolean(record['synthesized'])
            if synthesized != bool(pending['status'] & 0x80000000):
                raise ValueError('synthetic failure identities contradict the query status')
            if span is None:
                if pending['operation'] != 'pages':
                    raise ValueError('a ledger batch must begin with a ranged PFN query')
                span = (pending['first'], [(U64, 0)] * count)
                first_span_start = pending['start']
            first, selected = span
            previous_pfn = None
            for index, row in enumerate(rows):
                if not isinstance(row, list) or len(row) != 3:
                    raise ValueError('identity must retain PFN, frame and backing')
                pfn, frame, backing = (integer(cell) for cell in row)
                if not inside(pfn) or pfn < first or pfn - first >= len(selected):
                    raise ValueError('query identity is outside the capture range/batch')
                if (index == 0 and pfn != pending['first']) or (previous_pfn is not None and pfn <= previous_pfn):
                    raise ValueError('query identity order differs from its request')
                if pending['operation'] == 'pages' and pfn != pending['first'] + index:
                    raise ValueError('ranged PFN payload is not contiguous')
                previous_pfn = pfn
                if synthesized and (frame != U64 or backing != 0):
                    raise ValueError('failed query identities are not explicit synthetic sentinels')
                position = pfn - first
                if pending['operation'] == 'identities' and selected[position][0] != U64:
                    raise ValueError('exact-PFN retry unexpectedly replaces a readable identity')
                if not synthesized and (pending['operation'] == 'pages' or frame != U64):
                    selected[position] = (frame, backing)
            attempts += count
            pending_payload = True
        elif kind == 'ledger_chunk':
            check_pending()
            if span is None or pending is None or integer(record['queryOrdinal']) != pending['ordinal']:
                raise ValueError('ledger chunk has no completed query batch')
            if boolean(record['chunkComplete']) is not True or integer(record['chunkOrdinal']) != chunks:
                raise ValueError('ledger chunk is unfinished or out of order')
            rows, count = record['identities'], integer(record['pageCount'], 4096)
            if integer(record['requestCount'], 4096) != count or integer(record['status'], (1 << 32) - 1) != pending['status']:
                raise ValueError('ledger request/status differs from its completed batch')
            if not count or not isinstance(rows, list) or len(rows) != count or range_index >= len(ranges):
                raise ValueError('ledger chunk does not fit the physical range')
            first = integer(record['firstPfn'])
            if first != ranges[range_index][0] + range_offset or count > ranges[range_index][1] - range_offset or first != span[0] or count != len(span[1]):
                raise ValueError('duplicate, missing or out-of-domain ledger PFN')
            if integer(record['startUs']) > first_span_start or integer(record['endUs']) < pending['end']:
                raise ValueError('ledger interval does not contain its queries')
            for index, row in enumerate(rows):
                if not isinstance(row, list) or len(row) != 3:
                    raise ValueError('identity must retain PFN, frame and backing')
                pfn, frame, backing = (integer(cell) for cell in row)
                if pfn != first + index or (frame, backing) != span[1][index]:
                    raise ValueError('ledger identity differs from its ordered query responses')
                visited += 1
                if frame == U64:
                    unreadable += 1
                    if pfn == lookup_pfn:
                        lookup = {'pfn': hex(pfn), 'frame': hex(frame), 'backing': hex(backing), 'status': 'Unreadable', 'chunkOrdinal': str(chunks)}
                    continue
                state, use = (frame >> 4) & 7, identity_kind(frame, backing, compression)
                counts[use][state] += 1
                if use == 15:
                    unknown[frame & 15][state] += 1
                key = (frame >> 9) & 0xFFFFFFFFFFFF
                if state in IN_USE:
                    original_use = identity_kind(frame, backing, set())
                    group_key = key if original_use == 0 else (backing & ~3 if original_use in (1, 8, 12) else 0)
                    group = (original_use, group_key)
                    if group not in retained_groups:
                        if len(retained_groups) < MAX_KEYS:
                            retained_groups.add(group)
                        else:
                            overflow_pages += 1
                if use in (0, 13) and state in IN_USE and key in resolved:
                    owners_resolved[use][state] += 1
                if use in (1, 8, 12) and backing & ~3:
                    objects[use][state] += 1
                if pfn == lookup_pfn:
                    lookup = {'pfn': hex(pfn), 'frame': hex(frame), 'backing': hex(backing), 'status': 'Observed', 'useIndex': use, 'nativeUse': frame & 15, 'state': state, 'chunkOrdinal': str(chunks)}
            chunks += 1
            range_offset += count
            if range_offset == ranges[range_index][1]:
                range_index += 1
                range_offset = 0
            span = None
        elif kind == 'footer':
            check_pending()
            if span is not None:
                raise ValueError('queried PFNs are missing their final ledger chunk')
    if first_hash.digest() != second_hash.digest():
        raise ValueError('capture changed between metadata and identity replay')
    unscanned = expected - visited
    if counts != matrix(footer['categories']) or unknown != matrix(footer['unknownByNativeUse']):
        raise ValueError('replayed use/state ledger differs from capture footer')
    for key, observed in (('expectedPages', expected), ('visitedPages', visited), ('unreadablePages', unreadable), ('unscannedPages', unscanned), ('ledgerPages', visited), ('attemptPages', attempts), ('chunks', chunks)):
        if integer(footer[key]) != observed:
            raise ValueError(f'{key} differs from raw evidence')
    if integer(footer['rawBytesBeforeFooter']) != footer_offset:
        raise ValueError('raw byte count differs from the serialized capture')
    if integer(footer['groupedOverflowPages']) != overflow_pages:
        raise ValueError('retention overflow differs from the observed backing groups')
    retained_private = {key for use, key in retained_groups if use == 0 and key}
    expected_compression = {key for key in retained_private if key in final_owners and final_owners[key]['name'].lower() == 'memcompression'} if header['version'] == 1 else set()
    expected_resolved = {key for key in retained_private if key in final_owners and final_owners[key]['pid'] and final_owners[key]['name']
                         and (header['version'] == 1 or final_owners[key]['lifetimeVerified'])}
    if compression != expected_compression or resolved != expected_resolved:
        raise ValueError('retained compression/consumer keys differ from group retention and endpoint evidence')
    if not boolean(footer['ledgerRawComplete']):
        raise ValueError('raw ledger was not finalized')
    owner = footer['ownerCoverage']
    owner_resolved = matrix(owner['resolved'])
    owner_unresolved, owner_na, object_known = (matrix(owner[key]) for key in ('unresolved', 'notApplicable', 'objectKeyKnown'))
    if owner_resolved != owners_resolved or object_known != objects:
        raise ValueError('owner/object evidence differs from retained identities and consumer keys')
    for use in range(16):
        for state in range(8):
            if owner_resolved[use][state] + owner_unresolved[use][state] + owner_na[use][state] != counts[use][state]:
                raise ValueError('owner coverage is not a partition of the existing ledger')
            if state not in IN_USE and (owner_resolved[use][state] or owner_unresolved[use][state]):
                raise ValueError('available/bad pages incorrectly require an active consumer')
            if state in IN_USE and owner_na[use][state]:
                raise ValueError('in-use consumers incorrectly labeled not applicable')
    if not boolean(footer['reconciles']):
        raise ValueError('capture does not claim a reconciled ledger')
    known_unresolved = sum(owner_unresolved[use][state] for use in range(15) for state in IN_USE)
    type_unknown = sum(counts[15][state] for state in IN_USE)
    claim_complete = boolean(footer['complete'])
    cancelled = boolean(footer['cancelled'])
    before = endpoint_ranges.get('before')
    after = endpoint_ranges.get('after')
    if before is None or (ranges and (not before[0] or before[1] != ranges)) or not ranges and before[0] and before[1]:
        raise ValueError('capture scope differs from the original range query')
    if expected and after is None:
        raise ValueError('capture has no range recheck')
    changed = bool(expected and (not after[0] or after[1] != ranges))
    if boolean(footer['rangesChanged']) != changed:
        raise ValueError('range-change flag contradicts the actual endpoints')
    if boolean(footer['nativeAbiObserved']) != abi_observed:
        raise ValueError('observed ABI flag differs from successful PFN operations')
    if header['version'] == 2:
        if integer(footer['successfulNativeBatches']) != native_accepted or integer(footer['successfulDriverBatches']) != driver_accepted:
            raise ValueError('successful provider counts differ from query provenance')
        statuses = footer['statuses']
        range_status = endpoint_statuses['ranges_before']
        if not range_status & 0x80000000 and not ranges:
            range_status = 0xc000003e  # Empty successful response is not a usable RAM scope.
        for field, value in (('ranges', range_status), ('ownersBefore', endpoint_statuses.get('owners_before', 0)),
                             ('ownersAfter', endpoint_statuses.get('owners_after', 0))):
            if integer(statuses[field], (1 << 32) - 1) != value:
                raise ValueError('endpoint status summary differs from query provenance')
    complete = expected > 0 and not cancelled and not changed and unreadable == 0 and unscanned == 0
    if claim_complete != complete:
        raise ValueError('complete flag contradicts observed coverage, cancellation or range changes')
    if integer(footer['ownerConflicts']) != len(conflicts):
        raise ValueError('owner conflicts differ from native endpoint observations')
    if complete and not boolean(footer['nativeAbiObserved']):
        raise ValueError('complete capture has no observed PFN ABI')
    if boolean(footer['semanticsValidated']) != header['semanticsValidated']:
        raise ValueError('Windows semantic-validation claims differ within the capture')
    if lookup_pfn is not None and lookup is None:
        lookup = {'pfn': hex(lookup_pfn), 'status': 'Unscanned' if inside(lookup_pfn) else 'Outside observation domain'}
    return {
        'path': str(path.resolve()), 'sha256': first_hash.hexdigest(), 'domain': header['domain'], 'epoch': header['epoch'],
        'build': header['build'], 'architecture': header['architecture'], 'nativeAbi': header['nativeAbi'],
        'semanticsValidated': header['semanticsValidated'], 'semanticValidationSource': 'Capture claim; arithmetic replay does not validate Windows semantics',
        'complete': complete, 'ledgerReplayed': True, 'attributionPolicyValidated': header['version'] == 2,
        'legacyPolicyWarning': 'Legacy name-only compression and endpoint-only consumer claims are arithmetic history, not verified attribution' if header['version'] == 1 else '', 'expectedPages': str(expected), 'visitedPages': str(visited),
        'queryAttemptPages': str(attempts), 'ledgerChunks': str(chunks), 'unknownInUseBytes': str(type_unknown * 4096),
        'knownUseOwnerUnresolvedBytes': str(known_unresolved * 4096), 'unreadableBytes': str(unreadable * 4096),
        'unscannedBytes': str(unscanned * 4096), 'categories': [[str(value) for value in row] for row in counts],
        'observer': footer.get('observer', {}), 'semanticValidation': header.get('semanticValidation', 'Unavailable'),
        **({'lookup': lookup} if lookup_pfn is not None else {})
    }


def criterion(samples):
    blockers = []
    if len(samples) < 3:
        blockers.append('At least three captures are required')
    if len({sample['epoch'] for sample in samples}) != len(samples):
        blockers.append('Capture epochs must be distinct')
    scope = {(sample['domain'], json.dumps(sample['build'], sort_keys=True), json.dumps(sample['architecture'], sort_keys=True), sample['nativeAbi']) for sample in samples}
    if len(scope) > 1:
        blockers.append('Domain, Windows build, architecture and native ABI differ')
    for sample in samples:
        build = sample['build']
        if build.get('known') is not True or not integer(build.get('build', 0), (1 << 32) - 1) or sample['semanticsValidated'] is not True:
            blockers.append(f"{sample['epoch']}: Windows build semantics have not been validated")
        if sample.get('attributionPolicyValidated') is not True:
            blockers.append(f"{sample['epoch']}: legacy attribution policy has no lifetime/provider validation")
        if not sample['complete']:
            blockers.append(f"{sample['epoch']}: failed, unreadable, unscanned or changing capture")
        if int(sample['unknownInUseBytes']) > 64 * 1024 * 1024:
            blockers.append(f"{sample['epoch']}: in-use type-unknown exceeds 64 MiB")
    return {'name': 'Proposed three-capture 64 MiB type-unknown criterion', 'passed': not blockers,
            'blockers': blockers, 'semanticValidationCriterion': 'Capture claim only; no Windows support matrix or real-machine measurement is established',
            'consumerOwnershipCriterion': 'Separate; no owner-unresolved byte target is inferred'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('captures', nargs='+', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--pfn', type=lambda value: integer(value, (1 << 40) - 1), help='Look up one decimal/hex PFN without retaining the whole scan')
    parser.add_argument('--require-proposed-criterion', action='store_true')
    args = parser.parse_args(argv)
    try:
        samples = [audit(path, args.pfn) for path in args.captures]
        report = {'schema': 'ksword.pfn.audit', 'version': 1, 'samples': samples, 'criterion': criterion(samples)}
        if args.output:
            args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    except (ValueError, KeyError, TypeError, OSError, json.JSONDecodeError, RecursionError) as error:
        print(f'PFN_EVIDENCE_AUDIT=FAIL: {error}')
        return 1
    print('PFN_EVIDENCE_AUDIT=PASS')
    print(json.dumps(report['criterion'], ensure_ascii=False))
    return 2 if args.require_proposed_criterion and not report['criterion']['passed'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
