"""Headless production proxy tests with the actual pinned native Titan DLL.

This deliberately does not load a driver, enable HVM, create a GUI, or claim
native debug-loop/runtime EPT coverage. Fixture copies live inside the repo.
"""
from __future__ import annotations
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


class Session(C.Structure):
    _pack_ = 1
    _fields_ = [('structSize', W.DWORD), ('kind', C.c_int), ('capabilities', C.c_uint64),
                ('machine', W.DWORD), ('pid', W.DWORD), ('tid', W.DWORD), ('reserved', W.DWORD)]


class MemoryInfo(C.Structure):
    _fields_ = [('base', C.c_void_p), ('allocation', C.c_void_p), ('allocationProtect', W.DWORD),
                ('partitionId', W.WORD), ('region', C.c_size_t), ('state', W.DWORD),
                ('protect', W.DWORD), ('type', W.DWORD)]


class Options(C.Structure):
    _fields_ = [(name, W.DWORD) for name in
                ['version', 'size', 'mode', 'shadowMemoryWrites', 'allowFallback', 'logFallback',
                 'nativeContextFallback', 'nativeSuspendFallback', 'maxShadowPages']] + [('reserved', W.DWORD * 3)]


class Policy(C.Structure):
    _fields_ = [('options', Options)] + [(name, W.DWORD) for name in
                ['activePath', 'shadowWritePages', 'activeBreakpoints', 'canChangeOptions', 'fallbackCount', 'lastFallbackError']]


class Call(C.Structure):
    _fields_ = [('version', W.DWORD), ('size', W.DWORD), ('command', W.DWORD), ('reserved', W.DWORD),
                ('input', C.c_uint64), ('output', C.c_uint64), ('inputBytes', W.DWORD), ('outputBytes', W.DWORD),
                ('error', W.DWORD), ('bytesReturned', W.DWORD)]


class BackendStatus(C.Structure):
    _fields_ = [(name, W.DWORD) for name in ['version', 'size', 'driverReady', 'useHvm',
                'directWindow', 'resident', 'ownsResident', 'attachedPid', 'eptProtocol', 'lastError']]


class EngineInfo(C.Structure):
    _fields_ = [(name, W.DWORD) for name in ['version', 'size', 'provider', 'kind']] + [
        ('capabilities', C.c_uint64)] + [(name, W.DWORD) for name in ['pid', 'configuredHvm', 'activeHvm',
        'windowsTransport', 'hardwareSlots', 'eptGranularity']] + [('backend', BackendStatus), ('policy', Policy)]


class BreakpointQuery(C.Structure):
    _fields_ = [('version', W.DWORD), ('size', W.DWORD), ('address', C.c_uint64), ('type', W.DWORD), ('reserved', W.DWORD)]


class BreakpointInfo(C.Structure):
    _fields_ = [('version', W.DWORD), ('size', W.DWORD), ('address', C.c_uint64),
               ('requestedBytes', C.c_uint64), ('effectiveBytes', C.c_uint64)] + [
        (name, W.DWORD) for name in ['type', 'mechanism', 'access', 'slot', 'flags', 'fallbackError']]


class ProcessInfo(C.Structure):
    _fields_ = [('process', W.HANDLE), ('thread', W.HANDLE), ('pid', W.DWORD), ('tid', W.DWORD)]


def function(dll, name, restype, arguments):
    fn = getattr(dll, name); fn.restype = restype; fn.argtypes = arguments
    return fn


def exercise(stage: Path, missing: bool) -> None:
    session_id = '00112233-4455-6677-8899-aabbccddeeff'
    control_path = stage / 'control.txt'; state_path = stage / 'state.txt'
    if not missing:
        state_path.unlink(missing_ok=True)
        control_path.write_text(f'{session_id} 0 0\n')
        os.environ['KSWORD_DEBUGGER_CONTROL_FILE'] = str(control_path)
        os.environ['KSWORD_DEBUGGER_STATE_FILE'] = str(state_path)
        os.environ['KSWORD_DEBUGGER_SESSION_ID'] = session_id
    proxy = C.CDLL(str(stage / 'KSword/TitanEngine.dll'), use_last_error=True)
    initialize = function(proxy, 'KSwordTitanInitialize', W.DWORD, [])
    if missing:
        assert initialize() == 127, 'missing canonical export must fail ERROR_PROC_NOT_FOUND'
        assert initialize() == 127, 'failed initialization result must remain stable'
        print('PASS: missing native GetSessionInfo export is rejected')
        return
    assert initialize() == 0, f'proxy initialize failed: {C.get_last_error()}'
    # Match the production log page's delete-sharing reader. Python's ordinary
    # open() can briefly block the worker's atomic MoveFileEx acknowledgement.
    files = C.WinDLL('kernel32', use_last_error=True)
    create = function(files, 'CreateFileW', C.c_void_p,
                      [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, C.c_void_p])
    read_file = function(files, 'ReadFile', W.BOOL,
                         [C.c_void_p, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p])
    close_file = function(files, 'CloseHandle', W.BOOL, [C.c_void_p])
    def read_ack():
        handle = create(str(state_path), 0x80000000, 7, None, 3, 0, None)
        if handle == C.c_void_p(-1).value: return []
        try:
            buffer = C.create_string_buffer(512); count = W.DWORD()
            if not read_file(handle, buffer, 512, C.byref(count), None): return []
            return buffer.raw[:count.value].decode('ascii').split()
        finally:
            assert close_file(handle)
    def request(text):
        temporary = control_path.with_suffix('.tmp')
        temporary.write_text(text)
        os.replace(temporary, control_path)
    def ack(revision):
        until = time.monotonic() + 3
        while time.monotonic() < until:
            fields = read_ack()
            if len(fields) >= 7 and fields[0] == session_id and int(fields[1]) == revision: return fields
            time.sleep(0.01)
        raise AssertionError(f'log control acknowledgment timeout for {revision}')
    assert ack(0)[2:7] == ['0', '0', '0', '0', '0']
    request('stale-session 1 0\n')
    time.sleep(0.15)
    assert ack(0)[1] == '0'
    request(f'{session_id} 2 2\n')
    assert ack(2)[2:7] == ['87', '0', '0', '0', '0']
    request(f'{session_id} 3 0\n')
    assert ack(3)[2:7] == ['0', '0', '0', '0', '0']
    assert ack(3)[7:] == ['v2', '0', '0', '1', '1', '1', '32', '0', '0', '0', '1', '0', '0']
    call_api = function(proxy, 'KSwordDebuggerCall', W.DWORD, [C.POINTER(Call)])
    def dispatch(command, output, source=None):
        packet = Call(1, C.sizeof(Call), command, 0, C.addressof(source) if source is not None else 0,
                      C.addressof(output), C.sizeof(source) if source is not None else 0, C.sizeof(output), 0, 0)
        result = call_api(C.byref(packet))
        assert result == packet.error and packet.bytesReturned == C.sizeof(output)
        return result
    original = Options(); assert C.sizeof(original) == 48 and C.sizeof(Policy) == 72 and dispatch(4, original) == 0
    engine = EngineInfo(); assert C.sizeof(engine) == 160 and dispatch(8, engine) == 0
    assert engine.version == 1 and engine.size == 160 and engine.provider == 0 and engine.kind == 0
    assert engine.configuredHvm == 0 and engine.activeHvm == 0 and engine.hardwareSlots == 4 and engine.eptGranularity == 4096
    assert not engine.backend.driverReady, 'engine information must not initialize/open the driver'
    absent = BreakpointQuery(1, 24, 0x1234, 2, 0); binding = BreakpointInfo()
    assert C.sizeof(binding) == 56 and dispatch(9, binding, absent) == 1168 and not (binding.flags & 1)
    short = Call(1, 48, 8, 0, 0, C.addressof(engine), 0, 159, 0, 0)
    assert call_api(C.byref(short)) == 122 and short.bytesReturned == 0
    expected = [1, 48, 0, 0, 1, 1, 1, 1, 32, 0, 0, 0]
    assert list(C.cast(C.byref(original), C.POINTER(W.DWORD * 12)).contents) == expected
    for field, value, error in [('mode', 2, 87), ('shadowMemoryWrites', 2, 87), ('allowFallback', 2, 87),
                                ('logFallback', 0, 87), ('maxShadowPages', 0, 87), ('maxShadowPages', 33, 87),
                                ('version', 2, 1306), ('size', 44, 1306)]:
        invalid = Options.from_buffer_copy(original); setattr(invalid, field, value); actual = Options()
        assert dispatch(5, actual, invalid) == error and bytes(actual) == bytes(original), field
    invalid = Options.from_buffer_copy(original); invalid.reserved[2] = 1; actual = Options()
    assert dispatch(5, actual, invalid) == 87 and bytes(actual) == bytes(original)
    request(f'{session_id} 4 0 v2 1 1 0 0 0 8\n')
    confirmed = ack(4)
    assert confirmed[2:7] == ['0', '0', '0', '0', '0'] and confirmed[7:14] == ['v2', '1', '1', '0', '0', '0', '8']
    request(f'{session_id} 5 0 v2 1 1 0 0 0 33\n')
    assert ack(5)[2] == '87' and ack(5)[7:14] == confirmed[7:14], 'failed packet keeps actual options'
    request(f'{session_id} 6 0\n')
    assert ack(6)[7:14] == confirmed[7:14], 'legacy HVM request preserves actual configured options'
    assert dispatch(5, actual, original) == 0 and bytes(actual) == bytes(original)
    native = C.CDLL(str(stage / 'TitanEngine.dll'), use_last_error=True)
    debug_data = function(proxy, 'GetDebugData', C.c_void_p, [])
    native_debug_data = function(native, 'GetDebugData', C.c_void_p, [])
    observations = []
    for fn in [native_debug_data, debug_data]:
        C.set_last_error(0xA11CE)
        observations.append((fn(), C.get_last_error()))
    assert observations[0] == observations[1] and observations[1][1] == 0xA11CE
    session = function(proxy, 'GetSessionInfo', C.c_bool, [C.POINTER(Session)])
    info = Session()
    C.set_last_error(0xBEEF)
    assert C.sizeof(info) == 32 and session(C.byref(info))
    assert C.get_last_error() == 0xBEEF, 'successful canonical call must preserve native LastError'
    assert info.structSize == 32 and info.kind == 0 and not info.pid and not info.tid and not info.reserved
    assert not session(None)
    alignment = function(proxy, 'EngineCheckStructAlignment', C.c_bool, [C.c_int, C.c_size_t])
    native_alignment = function(native, 'EngineCheckStructAlignment', C.c_bool, [C.c_int, C.c_size_t])
    accepted = [i for i in range(2049) if native_alignment(16, i)]
    assert len(accepted) == 1 and alignment(16, accepted[0]) and not alignment(16, accepted[0] + 1)
    open_process = function(proxy, 'TitanOpenProcess', C.c_void_p, [W.DWORD, C.c_bool, W.DWORD])
    kernel = C.WinDLL('kernel32', use_last_error=True)
    process = open_process(0x1F0FFF, False, kernel.GetCurrentProcessId())
    assert process, 'real process handle required'
    close = function(proxy, 'TitanCloseHandle', C.c_bool, [C.c_void_p])
    alloc = function(proxy, 'MemoryAllocSafe', C.c_void_p, [C.c_void_p, C.c_void_p, C.c_size_t, W.DWORD, W.DWORD])
    free = function(proxy, 'MemoryFreeSafe', C.c_bool, [C.c_void_p, C.c_void_p, C.c_size_t, W.DWORD])
    query = function(proxy, 'MemoryQuerySafe', C.c_size_t, [C.c_void_p, C.c_void_p, C.POINTER(MemoryInfo), C.c_size_t])
    write = function(proxy, 'MemoryWriteSafe', C.c_bool, [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)])
    read = function(proxy, 'MemoryReadUnsafe', C.c_bool, [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)])
    native_read = function(native, 'MemoryReadUnsafe', C.c_bool, read.argtypes)
    protect = function(proxy, 'MemoryProtectSafe', C.c_bool, [C.c_void_p, C.c_void_p, C.c_size_t, W.DWORD, C.POINTER(W.DWORD)])
    address = None
    try:
        address = alloc(process, None, 4096, 0x3000, 4)
        assert address
        mbi = MemoryInfo()
        assert query(process, address, C.byref(mbi), C.sizeof(mbi)) == C.sizeof(mbi)
        assert mbi.state == 0x1000 and mbi.region >= 4096
        source = C.create_string_buffer(b'KSword native forwarding\0'); size = len(source)
        transferred = C.c_size_t(); destination = C.create_string_buffer(size)
        assert write(process, address, source, size, C.byref(transferred)) and transferred.value == size
        assert read(process, address, destination, size, C.byref(transferred)) and transferred.value == size
        assert destination.raw == source.raw
        assert read(process, address, destination, size, None)
        old = W.DWORD()
        assert protect(process, address, 4096, 2, C.byref(old)) and old.value == 4
        assert protect(process, address, 4096, 4, C.byref(old)) and old.value == 2
        errors = []
        for fn in [native_read, read]:
            C.set_last_error(0xBEEF); transferred.value = 0xFFFF
            assert not fn(process, 1, destination, 8, C.byref(transferred))
            errors.append((C.get_last_error(), transferred.value))
        assert errors[0] == errors[1] and errors[0][0] != 0
        # A real native breakpoint table and a private, never-executed self page
        # exercise adapter binding protection without attaching a debugger.
        # This setup helper belongs to the original native engine's historical
        # exports. The production proxy intentionally exposes only canonical64.
        get_process_info = function(native, 'TitanGetProcessInformation', C.POINTER(ProcessInfo), [])
        engine_info = get_process_info(); saved_info = bytes(engine_info.contents)
        callback = C.CFUNCTYPE(None)(lambda: None)
        set_bp = function(proxy, 'SetBPX', C.c_bool, [C.c_size_t, W.DWORD, C.c_void_p])
        delete_bp = function(proxy, 'DeleteBPX', C.c_bool, [C.c_size_t])
        set_hw = function(proxy, 'SetHardwareBreakPoint', C.c_bool, [C.c_size_t, W.DWORD, C.c_int, C.c_int, C.c_void_p])
        set_mem = function(proxy, 'SetMemoryBPXEx', C.c_bool, [C.c_size_t, C.c_size_t, C.c_int, C.c_bool, C.c_void_p])
        memory_callback = C.CFUNCTYPE(None, C.c_void_p)(lambda _: None)
        stealth = Options.from_buffer_copy(original); stealth.mode = 1; stealth.shadowMemoryWrites = 1; stealth.allowFallback = 0
        installed = False
        try:
            engine_info.contents.process = process; engine_info.contents.pid = kernel.GetCurrentProcessId()
            stub = C.create_string_buffer(b'\x90\xc3' + b'\x90' * 30)
            assert write(process, address, stub, len(stub), C.byref(transferred))
            assert protect(process, address, 4096, 0x40, C.byref(old))
            assert set_bp(address, 0x10000000, callback); installed = True
            query = BreakpointQuery(1, 24, address, 1, 0)
            assert dispatch(9, binding, query) == 0 and binding.mechanism == 4 and binding.flags & 1
            assert binding.requestedBytes == 1 and binding.effectiveBytes == 1 and binding.fallbackError == 0
            assert dispatch(8, engine) == 0 and engine.policy.activeBreakpoints >= 1 and engine.windowsTransport == 1
            policy = Policy(); assert dispatch(6, policy) == 0 and policy.activeBreakpoints >= 1 and policy.canChangeOptions == 0
            assert dispatch(5, actual, stealth) == 170 and bytes(actual) == bytes(original), 'live native binding protects policy'
            assert dispatch(5, actual, original) == 0, 'identical options remain idempotent'
            assert delete_bp(address); installed = False
            assert dispatch(9, binding, query) == 1168 and not (binding.flags & 1), 'deleted native binding must not be inferred from policy'
            assert dispatch(5, actual, stealth) == 0 and bytes(actual) == bytes(stealth)
            assert not set_bp(address, 0x10000000, callback) and C.get_last_error() == 21
            assert not set_hw(address, 0, 4, 7, callback) and C.get_last_error() == 21
            assert not set_mem(address, 4096, 3, True, memory_callback) and C.get_last_error() == 50
            assert dispatch(6, policy) == 0 and policy.activeBreakpoints == 0 and policy.fallbackCount >= 3
            assert dispatch(5, actual, original) == 0
            check_bytes = C.create_string_buffer(2)
            assert read(process, address, check_bytes, 2, C.byref(transferred)) and check_bytes.raw == b'\x90\xc3', 'stealth refusal cannot change original code'
        finally:
            if installed: assert delete_bp(address)
            assert dispatch(5, actual, original) == 0
            C.memmove(engine_info, saved_info, len(saved_info))
        wow64 = function(proxy, 'ProcessIsWow64', C.c_bool, [C.c_void_p, C.POINTER(W.BOOL)])
        result = W.BOOL(1); assert wow64(process, C.byref(result)) and result.value == 0
        replay = function(proxy, 'ReplayGetPosition', C.c_bool, [C.POINTER(C.c_uint64 * 2)])
        position = (C.c_uint64 * 2)(1, 1)
        assert not replay(C.byref(position)) and C.get_last_error() == 50 and list(position) == [0, 0]
    finally:
        if address: assert free(process, address, 0, 0x8000)
        assert close(process)
    print(f'PASS: real native64 ABI/session/context-size({accepted[0]})/memory/protect/handles/LastError/replay/control v1-v2/options rollback/native binding guard/stealth visible-breakpoint rejection; HVM disabled, no driver opened')


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--proxy', type=Path)
    parser.add_argument('--native', type=Path)
    parser.add_argument('--fixture-root', type=Path)
    parser.add_argument('--case-stage', type=Path)
    parser.add_argument('--missing', action='store_true')
    args = parser.parse_args()
    if args.case_stage:
        exercise(args.case_stage.resolve(), args.missing); return
    repository = Path(__file__).resolve().parents[2]
    stage = args.fixture_root.resolve()
    assert stage.is_relative_to(repository), 'fixture output must remain inside repository'
    native_bytes = args.native.read_bytes()
    assert b'GetSessionInfo\0' in native_bytes
    for case in ['complete', 'missing']:
        path = stage / case; (path / 'KSword').mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.proxy, path / 'KSword/TitanEngine.dll')
        data = native_bytes if case == 'complete' else native_bytes.replace(b'GetSessionInfo\0', b'BadSessionInfo\0', 1)
        (path / 'TitanEngine.dll').write_bytes(data)
        command = [sys.executable, str(Path(__file__).resolve()), '--case-stage', str(path)]
        if case == 'missing': command.append('--missing')
        subprocess.run(command, check=True, timeout=30)
    print('PASS: production proxy native forwarding and incompatible native dependency rejection')


if __name__ == '__main__': main()
