"""Drive the actual x64dbg command frontend through its HVM execution fallback."""
from pathlib import Path
import ctypes as C
from ctypes import wintypes as W
import importlib.util
import json
import os
import subprocess
import sys
import time
import uuid

root = Path(sys.argv[1]).resolve()
stage = root / 'frontend-hvm'
stage.mkdir(exist_ok=True)
spec = importlib.util.spec_from_file_location('attach_driver', root / 'x64dbg/src/tests/attach_pause/driver.py')
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)
kernel = C.WinDLL('kernel32', use_last_error=True)
def bind(name, restype, args):
    fn = getattr(kernel, name); fn.restype = restype; fn.argtypes = args; return fn
open_process = bind('OpenProcess', C.c_void_p, [W.DWORD, W.BOOL, W.DWORD])
close = bind('CloseHandle', W.BOOL, [C.c_void_p])
rpm = bind('ReadProcessMemory', W.BOOL, [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)])
wpm = bind('WriteProcessMemory', W.BOOL, rpm.argtypes)
create = bind('CreateFileW', C.c_void_p, [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, C.c_void_p])
read_file = bind('ReadFile', W.BOOL, [C.c_void_p, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p])
def packet(path):
    handle = create(str(path), 0x80000000, 7, None, 3, 0, None)
    if handle == C.c_void_p(-1).value: return []
    try:
        data = C.create_string_buffer(512); count = W.DWORD()
        return data.raw[:count.value].decode().split() if read_file(handle, data, 512, C.byref(count), None) else []
    finally: close(handle)
def wait(predicate, message, seconds=15):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate(): return
        time.sleep(.05)
    raise AssertionError(message)

session = str(uuid.uuid4())
userdir = stage / session
userdir.mkdir()
control = stage / 'control.txt'; state = stage / 'state.txt'
def select(revision, enabled):
    temporary = stage / 'control.tmp'
    temporary.write_text(f'{session} {revision} {enabled}\n')
    os.replace(temporary, control)
    wait(lambda: (values := packet(state)) and values[:4] == [session, str(revision), '0', str(enabled)], 'frontend HVM acknowledgement')
    print(f'PASS frontend mode={enabled} acknowledged', flush=True)
(userdir / 'headless.ini').write_text('[Engine]\nDebugEngine=4\n[Misc]\nNoConsoleWindow=1\n')
env = os.environ.copy()
env.update(KSWORD_DEBUGGER_SESSION_ID=session, KSWORD_DEBUGGER_CONTROL_FILE=str(control),
           KSWORD_DEBUGGER_STATE_FILE=str(state), KSWORD_DEBUGGER_LOG_FILE=str(stage / 'backend.log'))
target = headless = handle = None
transcript = []
try:
    (stage / 'target.json').unlink(missing_ok=True)
    target = subprocess.Popen([str(root / 'DebugTarget.exe'), str(stage)], creationflags=subprocess.CREATE_NO_WINDOW)
    wait(lambda: (stage / 'target.json').exists(), 'target metadata')
    info = json.loads((stage / 'target.json').read_text())
    handle = open_process(0x1f0fff, False, target.pid)
    assert handle and info['pid'] == target.pid
    def read(address):
        value = C.c_uint64(); count = C.c_size_t()
        assert rpm(handle, address, C.byref(value), 8, C.byref(count)) and count.value == 8
        return value.value
    def write(address, value):
        data = C.c_uint64(value); count = C.c_size_t()
        assert wpm(handle, address, C.byref(data), 8, C.byref(count)) and count.value == 8
    # Headless inherits this fixture's environment; no other process is affected.
    previous = {key: os.environ.get(key) for key in env if key.startswith('KSWORD_DEBUGGER_')}
    os.environ.update({key: value for key, value in env.items() if key.startswith('KSWORD_DEBUGGER_')})
    try:
        headless = driver.Headless([str(root / 'x64dbg/bin/x64/headless.exe'), '-userdir', str(userdir)], root / 'x64dbg/bin/x64', subprocess.CREATE_NO_WINDOW)
    finally:
        for key, value in previous.items():
            if value is None: os.environ.pop(key, None)
            else: os.environ[key] = value
    select(1, 1)
    def fresh(command, predicate, message):
        with headless.lock:
            transcript.extend(headless.lines); headless.lines.clear()
        headless.send(command)
        line = headless.wait_for_line(predicate, 15)
        assert line is not None, message
        return line
    def assertion(expression, message):
        line = fresh(f'log "{message}={{x:{expression}}}"', lambda line: message+'=' in line, message)
        assert line.endswith('=1'), line
        print('PASS '+message, flush=True)
    headless.send('settingset Events, SystemBreakpoint, 1')
    fresh(f'attach .{target.pid}', lambda line: 'Attached to process!' in line, 'attach through frontend')
    headless.wait_for_quiescence(1, 5)
    with headless.lock: paused = '[STATE] paused' in headless.lines
    if not paused: fresh('pause', lambda line: line == '[STATE] paused', 'pause through frontend')
    address = info['code']+3
    headless.send(f'bphws {address:X}, x, 1')
    assertion(f'byte:[{address:X}] == 48', 'frontend reads original hidden instruction')
    write(info['control'], 1)
    fresh('run', lambda line: line == '[STATE] paused', 'first frontend execution stop')
    assertion(f'cip == {address:X}', 'frontend exact first RIP')
    assertion('rax == 100', 'frontend first stop before instruction')
    headless.send('mov rax, 1234')
    fresh('sti', lambda line: line == '[STATE] paused', 'frontend StepInto stop')
    assertion(f'cip == {address+3:X}', 'frontend StepInto advances exactly three bytes')
    assertion('rax == 1235', 'frontend edited RAX used by one instruction')
    headless.send('run')
    wait(lambda: read(info['control']+16) == 1, 'first target completion')
    assert read(info['data']) == 0x1235
    with headless.lock:
        transcript.extend(headless.lines); headless.lines.clear()
    write(info['control'], 1)
    assert headless.wait_for_line(lambda line: line == '[STATE] paused', 15), 'repeat frontend execution stop'
    assertion(f'cip == {address:X}', 'frontend repeat exact RIP')
    assertion('rax == 100', 'frontend repeat stops before instruction')
    headless.send(f'bphwc {address:X}')
    assertion(f'byte:[{address:X}] == 48', 'frontend deletion preserves original byte')
    select(2, 0)
    headless.send('run')
    wait(lambda: read(info['control']+16) == 2, 'second target completion')
    assert read(info['data']) == 0x101
    fresh('pause', lambda line: line == '[STATE] paused', 'final frontend pause')
    fresh('detach', lambda line: '[STATE] stopped' in line, 'frontend detach')
    assert target.poll() is None
    write(info['control'], 9); target.wait(timeout=10)
    headless.send('exit'); headless.process.wait(timeout=10)
    assert headless.process.returncode == 0
    print('FINAL PASS actual x64dbg frontend HVM fallback, step, repeat, delete and detach', flush=True)
finally:
    if headless is not None:
        with headless.lock: transcript.extend(headless.lines)
        (stage / 'headless.stdout.txt').write_text('\n'.join(transcript)+'\n', encoding='utf-8')
        if headless.process.poll() is None: headless.process.kill(); headless.process.wait(timeout=10)
    if target is not None and target.poll() is None: target.kill(); target.wait(timeout=10)
    if handle: close(handle)
