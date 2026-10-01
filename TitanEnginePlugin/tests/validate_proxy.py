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
            if len(fields) == 7 and fields[0] == session_id and int(fields[1]) == revision: return fields
            time.sleep(0.01)
        raise AssertionError(f'log control acknowledgment timeout for {revision}')
    assert ack(0)[2:] == ['0', '0', '0', '0', '0']
    request('stale-session 1 0\n')
    time.sleep(0.15)
    assert ack(0)[1] == '0'
    request(f'{session_id} 2 2\n')
    assert ack(2)[2:] == ['87', '0', '0', '0', '0']
    request(f'{session_id} 3 0\n')
    assert ack(3)[2:] == ['0', '0', '0', '0', '0']
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
        wow64 = function(proxy, 'ProcessIsWow64', C.c_bool, [C.c_void_p, C.POINTER(W.BOOL)])
        result = W.BOOL(1); assert wow64(process, C.byref(result)) and result.value == 0
        replay = function(proxy, 'ReplayGetPosition', C.c_bool, [C.POINTER(C.c_uint64 * 2)])
        position = (C.c_uint64 * 2)(1, 1)
        assert not replay(C.byref(position)) and C.get_last_error() == 50 and list(position) == [0, 0]
    finally:
        if address: assert free(process, address, 0, 0x8000)
        assert close(process)
    print(f'PASS: real native64 ABI/session/context-size({accepted[0]})/memory/protect/handles/LastError/replay/idle-control acknowledgments; HVM disabled, no driver opened')


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
