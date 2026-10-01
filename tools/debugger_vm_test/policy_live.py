"""Guest-only regression of the production Titan policy and Shadow write APIs.

Usage inside the prepared VM: python policy_live.py ROOT --run-in-vm
ROOT contains DebugTarget.exe and x64dbg/bin/x64/KSword/TitanEngine.dll.
No driver is installed or loaded by this script. The guest driver must be ready.
Each phase runs in a bounded subprocess; its test target belongs to a kill-on-close
job, including when a phase times out. Artifacts go to ROOT/policy-regression/.
"""
from pathlib import Path
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
import struct
import subprocess
import sys
import time
import traceback


U32 = C.c_uint32
PHASES = ("offline", "memory", "image-cow", "normal-native", "normal-hvm", "stealth-hvm", "stealth-no-hvm")


class Call(C.Structure):
    _fields_ = [(n, U32) for n in ("version", "size", "command", "reserved")] + [
        ("input", C.c_uint64), ("output", C.c_uint64)] + [
        (n, U32) for n in ("inputBytes", "outputBytes", "error", "returned")]


class Status(C.Structure):
    _fields_ = [(n, U32) for n in ("version", "size", "driverReady", "useHvm",
        "directMemoryWindow", "residentActive", "ownsResident", "attachedProcessId",
        "eptBreakpointProtocol", "lastError")]


class Options(C.Structure):
    _fields_ = [(n, U32) for n in ("version", "size", "mode", "shadowMemoryWrites",
        "allowFallback", "logFallback", "nativeContextFallback", "nativeSuspendFallback",
        "maxShadowPages")] + [("reserved", U32 * 3)]


class Policy(C.Structure):
    _fields_ = [("options", Options)] + [(n, U32) for n in ("activePath", "shadowWritePages",
        "activeBreakpoints", "canChangeOptions", "fallbackCount", "lastFallbackError")]


class Restore(C.Structure):
    _fields_ = [("version", U32), ("size", U32), ("address", C.c_uint64), ("bytes", C.c_uint64)]


class MemoryInfo(C.Structure):
    _fields_ = [("base", C.c_void_p), ("allocation", C.c_void_p), ("allocationProtect", U32),
        ("partition", W.WORD), ("region", C.c_size_t), ("state", U32), ("protect", U32), ("type", U32)]


class WorkingSet(C.Structure):
    _fields_ = [("address", C.c_void_p), ("flags", C.c_size_t)]


class JobBasic(C.Structure):
    _fields_ = [("processTime", C.c_int64), ("jobTime", C.c_int64), ("flags", U32),
        ("minimumWorkingSet", C.c_size_t), ("maximumWorkingSet", C.c_size_t),
        ("activeProcesses", U32), ("affinity", C.c_size_t), ("priority", U32), ("scheduling", U32)]


class JobLimits(C.Structure):
    _fields_ = [("basic", JobBasic), ("io", C.c_uint64 * 6), ("processMemory", C.c_size_t),
        ("jobMemory", C.c_size_t), ("peakProcess", C.c_size_t), ("peakJob", C.c_size_t)]


def fields(value):
    if isinstance(value, C.Array):
        return list(value)
    return {name: fields(item) if isinstance(item, (C.Structure, C.Array)) else item
            for name, _ in value._fields_ for item in [getattr(value, name)]}


def bind(module, name, result, arguments):
    function = getattr(module, name)
    function.restype = result
    function.argtypes = arguments
    return function


class Runtime:
    def __init__(self, root, phase):
        self.root, self.phase = root, phase
        self.folder = root / "policy-regression" / phase
        self.folder.mkdir(parents=True, exist_ok=True)
        self.log = self.folder / "backend.log"
        self.log.write_text("", encoding="utf-8")
        os.environ["KSWORD_DEBUGGER_LOG_FILE"] = str(self.log)
        # A launcher control worker must not race the fixture's explicit C ABI calls.
        for name in ("KSWORD_DEBUGGER_CONTROL_FILE", "KSWORD_DEBUGGER_STATE_FILE", "KSWORD_DEBUGGER_SESSION_ID"):
            os.environ.pop(name, None)
        stage = root / "x64dbg" / "bin" / "x64"
        self.search = os.add_dll_directory(str(stage))
        self.dll = C.CDLL(str(stage / "KSword" / "TitanEngine.dll"), use_last_error=True)
        self.api = bind(self.dll, "KSwordDebuggerCall", U32, [C.POINTER(Call)])
        self.control = bind(self.dll, "KSwordTitanControl", U32, [U32, C.POINTER(Status)])
        self.read = bind(self.dll, "MemoryReadSafe", C.c_bool,
                         [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)])
        self.write = bind(self.dll, "MemoryWriteSafe", C.c_bool, self.read.argtypes)
        self.open = bind(self.dll, "TitanOpenProcess", W.HANDLE, [U32, C.c_bool, U32])
        self.close = bind(self.dll, "TitanCloseHandle", C.c_bool, [W.HANDLE])
        self.kernel = C.WinDLL("kernel32", use_last_error=True)
        self.windows_read = bind(self.kernel, "ReadProcessMemory", W.BOOL, self.read.argtypes)
        self.windows_write = bind(self.kernel, "WriteProcessMemory", W.BOOL, self.read.argtypes)
        self.allocate = bind(self.kernel, "VirtualAllocEx", C.c_void_p,
                             [W.HANDLE, C.c_void_p, C.c_size_t, U32, U32])
        self.free = bind(self.kernel, "VirtualFreeEx", W.BOOL, [W.HANDLE, C.c_void_p, C.c_size_t, U32])
        self.protect = bind(self.kernel, "VirtualProtectEx", W.BOOL,
                            [W.HANDLE, C.c_void_p, C.c_size_t, U32, C.POINTER(U32)])
        self.process = None
        self.ph = self.job = self.extra = None
        self.peers = []
        self.assertions, self.observations = [], {}

    def check(self, condition, message):
        if not condition:
            raise RuntimeError(f"{message}; Win32={C.get_last_error()}")
        self.assertions.append(message)
        print("PASS " + message, flush=True)

    def call(self, command, request, result_type, expected=0):
        result = result_type()
        packet = Call(1, C.sizeof(Call), command, 0, C.addressof(request) if request is not None else 0,
                      C.addressof(result), C.sizeof(request) if request is not None else 0,
                      C.sizeof(result), 0, 0)
        error = self.api(C.byref(packet))
        self.check(error == expected and packet.error == expected,
                   f"C ABI command {command} returns error {expected}")
        self.check(packet.returned == C.sizeof(result), f"command {command} returns complete actual ACK")
        return result

    def options(self):
        return self.call(4, None, Options)

    def policy(self):
        actual = self.call(6, None, Policy)
        self.observations["policy"] = fields(actual)
        return actual

    def set_options(self, expected=0, **changes):
        requested = self.options()
        for name, value in changes.items():
            setattr(requested, name, value)
        actual = self.call(5, requested, Options, expected)
        if expected == 0:
            self.check(bytes(actual) == bytes(requested), "accepted options ACK matches requested bytes")
        else:
            self.check(bytes(actual) == bytes(self.options()), "failed options ACK reports retained actual options")
        return actual

    def select(self, enabled, expected=0):
        actual = Status()
        self.check(self.control(enabled, C.byref(actual)) == expected,
                   f"HVM selection {enabled} returns {expected}")
        if expected == 0:
            self.check(actual.useHvm == enabled, "HVM ACK confirms actual selected mode")
        self.observations["backend"] = fields(actual)
        return actual

    def restore(self, address=0, length=0):
        return self.call(7, Restore(1, C.sizeof(Restore), address, length), Status)

    def rd(self, address, length, windows=False):
        data, done = C.create_string_buffer(length), C.c_size_t()
        function = self.windows_read if windows else self.read
        self.check(function(self.ph, address, data, length, C.byref(done)) and done.value == length,
                   f"{'Windows' if windows else 'Titan'} reads {length} original bytes")
        return data.raw

    def wr(self, address, data, windows=False, expected=0):
        source, done = C.create_string_buffer(data, len(data)), C.c_size_t(0xBAD)
        C.set_last_error(0)
        ok = (self.windows_write if windows else self.write)(self.ph, address, source, len(data), C.byref(done))
        error = C.get_last_error()
        if expected:
            self.check(not ok and error == expected and done.value == 0,
                       f"write rejected before mutation: error {expected}, transferred 0")
        else:
            self.check(ok and done.value == len(data), f"{'Windows setup' if windows else 'Titan'} writes {len(data)} bytes")

    def u64(self, address):
        return struct.unpack("<Q", self.rd(address, 8))[0]

    def command(self, command, expected):
        count = self.u64(self.info["control"] + 16)
        self.wr(self.info["control"], struct.pack("<Q", command), windows=True)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            # Poll without adding thousands of duplicate assertion lines.
            value, done = C.c_uint64(), C.c_size_t()
            if not self.windows_read(self.ph, self.info["control"] + 16, C.byref(value), 8, C.byref(done)):
                raise RuntimeError("target command polling failed")
            if value.value == count + 1:
                break
            if self.process.poll() is not None:
                raise RuntimeError("test target exited before completing command")
            time.sleep(.025)
        else:
            raise TimeoutError("target command did not complete within 8 seconds")
        self.check(self.u64(self.info["control"] + 8) == expected,
                   f"target command {command} actually observed value {expected:#x}")

    def start_target(self):
        create_job = bind(self.kernel, "CreateJobObjectW", W.HANDLE, [C.c_void_p, W.LPCWSTR])
        set_job = bind(self.kernel, "SetInformationJobObject", W.BOOL, [W.HANDLE, C.c_int, C.c_void_p, U32])
        assign = bind(self.kernel, "AssignProcessToJobObject", W.BOOL, [W.HANDLE, W.HANDLE])
        self.job = create_job(None, None)
        limits = JobLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        self.check(self.job and set_job(self.job, 9, C.byref(limits), C.sizeof(limits)), "test-owned kill-on-close job configured")
        infofile = self.folder / "target.json"
        infofile.unlink(missing_ok=True)
        self.process = subprocess.Popen([str(self.root / "DebugTarget.exe"), str(self.folder)],
                                        creationflags=subprocess.CREATE_NO_WINDOW)
        self.ph = self.open(0x1FFFFF, False, self.process.pid)
        self.check(self.ph and assign(self.job, self.ph), "only the new DebugTarget is assigned to fixture job")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                self.info = json.loads(infofile.read_text(encoding="utf-8"))
                break
            except (FileNotFoundError, json.JSONDecodeError):
                time.sleep(.025)
        else:
            raise TimeoutError("DebugTarget metadata was not published")
        self.check(self.info["pid"] == self.process.pid, "metadata belongs to exact test-owned PID")

    def log_contains(self, *parts):
        content = self.log.read_text(encoding="utf-8", errors="replace")
        self.check(all(part in content for part in parts), "backend log confirms " + ", ".join(parts))

    def start_peer(self):
        folder = self.folder / "peer"
        folder.mkdir(parents=True, exist_ok=True)
        infofile = folder / "target.json"
        infofile.unlink(missing_ok=True)
        process = subprocess.Popen([str(self.root / "DebugTarget.exe"), str(folder)],
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        record = [process, None]
        self.peers.append(record)
        handle = record[1] = self.open(0x1FFFFF, False, process.pid)
        assign = bind(self.kernel, "AssignProcessToJobObject", W.BOOL, [W.HANDLE, W.HANDLE])
        self.check(handle and assign(self.job, handle), "second test-owned image joins the same cleanup job")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                info = json.loads(infofile.read_text(encoding="utf-8"))
                break
            except (FileNotFoundError, json.JSONDecodeError):
                time.sleep(.025)
        else:
            raise TimeoutError("second DebugTarget metadata was not published")
        self.check(info["pid"] == process.pid, "second image metadata matches its exact owned PID")
        return process, handle, info

    def cleanup(self):
        errors = []
        if self.phase != "offline":
            try:
                self.restore()
                self.select(0)
            except BaseException:
                errors.append(traceback.format_exc())
        if self.extra and self.ph:
            self.free(self.ph, self.extra, 0, 0x8000)
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=5)
        for process, handle in self.peers:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            if handle:
                self.close(handle)
        if self.ph:
            self.close(self.ph)
        if self.job:
            bind(self.kernel, "CloseHandle", W.BOOL, [W.HANDLE])(self.job)
        return errors


def offline(runtime):
    options = runtime.options()
    expected = Options(1, 48, 0, 0, 1, 1, 1, 1, 32, (U32 * 3)(0, 0, 0))
    runtime.check(bytes(options) == bytes(expected), "offline defaults preserve native writes and required fallback logging")
    status = runtime.call(1, None, Status)
    runtime.check(status.driverReady == 0 and status.useHvm == 0, "options/status queries do not initialize the driver")
    runtime.set_options(shadowMemoryWrites=1, maxShadowPages=2)
    before = runtime.options()
    actual = runtime.set_options(87, logFallback=0)
    runtime.check(bytes(before) == bytes(actual), "invalid offline request returns original actual ACK")
    runtime.check(runtime.policy().canChangeOptions == 1, "offline policy is editable")
    runtime.restore()
    runtime.check(runtime.call(1, None, Status).driverReady == 0, "offline options and empty restore keep driver closed")


def memory(runtime):
    runtime.set_options(shadowMemoryWrites=1, allowFallback=1, maxShadowPages=1)
    status = runtime.select(1)
    runtime.check(status.driverReady == 1 and status.directMemoryWindow == 1, "guest driver and private HVM memory window are ready")
    runtime.start_target()
    code, data = runtime.info["code"], runtime.info["data"]
    original = runtime.rd(code, 17, windows=True)
    runtime.check(original[5] == 0xC0, "fixture original instruction is inc rax")
    runtime.command(1, 0x101)
    runtime.wr(code + 5, b"\xc8")  # inc rax -> dec rax in execution view only
    runtime.check(runtime.rd(code, 17) == original and runtime.rd(code, 17, windows=True) == original,
                   "both frontend and ordinary Windows reads retain all original code")
    runtime.command(1, 0xFF)
    runtime.check(runtime.u64(data) == 0xFF, "patched instruction executed and stored the changed result")
    policy = runtime.policy()
    runtime.check(policy.activePath == 2 and policy.shadowWritePages == 1 and policy.canChangeOptions == 0,
                   "actual policy reports owned Shadow page and locks changed options")
    runtime.set_options(170, maxShadowPages=2)
    runtime.set_options()  # An identical request remains idempotent while active.
    runtime.select(0, 170)

    runtime.wr(code + 0x80, b"\x90" * 1233, expected=50)
    runtime.check(runtime.rd(code + 0x80, 1233, windows=True) == bytes(1233), "oversized request leaves original range untouched")
    runtime.command(1, 0xFF)
    runtime.extra = runtime.allocate(runtime.ph, None, 8192, 0x3000, 0x40)
    runtime.check(bool(runtime.extra), "two private executable fixture pages allocated")
    baseline = bytes((offset * 17 + 3) % 251 for offset in range(8192))
    runtime.wr(runtime.extra, baseline, windows=True)
    runtime.wr(runtime.extra + 32, b"\x90", expected=8)
    runtime.check(runtime.rd(runtime.extra, 8192, windows=True) == baseline, "page-budget rejection preserves the new page")
    runtime.wr(runtime.extra + 4095, b"\x90\x90\x90", expected=50)
    runtime.check(runtime.rd(runtime.extra, 8192, windows=True) == baseline, "cross-page rejection leaves both pages untouched")
    previous = U32()
    runtime.check(runtime.protect(runtime.ph, runtime.extra + 4096, 4096, 0x04, C.byref(previous)), "second fixture page changed to ordinary RW data")
    runtime.wr(runtime.extra + 4095, b"\x90\x90\x90", expected=50)
    runtime.check(runtime.rd(runtime.extra, 8192, windows=True) == baseline, "mixed code/data rejection does not half-write either page")
    runtime.command(1, 0xFF)
    runtime.restore(code + 5, 1)
    runtime.check(runtime.policy().shadowWritePages == 0, "range restore retires last owned memory patch")
    runtime.command(1, 0x101)
    runtime.check(runtime.rd(code, 17, windows=True) == original, "restore preserves original backing bytes")

    before = runtime.policy().fallbackCount
    runtime.wr(data, struct.pack("<Q", 0xAABBCCDDEEFF1122))
    runtime.command(3, 0xAABBCCDDEEFF1122)
    runtime.check(runtime.policy().fallbackCount > before, "ordinary RW data fallback increments actual diagnostic counter")
    runtime.log_contains("Fallback Shadow Page memory write -> HVM ordinary data write",
                         "Ordinary data fallback completed", "execution view only", "before mutation")
    runtime.set_options(allowFallback=0)
    runtime.wr(data, struct.pack("<Q", 0x1122), expected=50)
    runtime.command(3, 0xAABBCCDDEEFF1122)
    runtime.log_contains("Shadow data write/freeze refused", "fallback is disabled")
    runtime.set_options(mode=1, shadowMemoryWrites=0, allowFallback=1)
    runtime.wr(data, struct.pack("<Q", 0x2233))
    runtime.command(3, 0x2233)
    runtime.check(runtime.policy().activePath == 0, "stealth ordinary data fallback installs no fake execution view")
    runtime.set_options(allowFallback=0)
    runtime.wr(data, struct.pack("<Q", 0x4455), expected=50)
    runtime.command(3, 0x2233)
    runtime.restore()
    runtime.select(0)


def image_cow(runtime):
    runtime.set_options(shadowMemoryWrites=1, allowFallback=1, maxShadowPages=2)
    runtime.select(1)
    runtime.start_target()
    peer_process, peer_handle, peer_info = runtime.start_peer()
    runtime.check("imageProbe" in runtime.info and "imageProbe" in peer_info,
                   "both fixtures publish the rebuilt noinline image probe")
    address, peer_address = runtime.info["imageProbe"], peer_info["imageProbe"]
    query = bind(runtime.kernel, "VirtualQueryEx", C.c_size_t,
                 [W.HANDLE, C.c_void_p, C.POINTER(MemoryInfo), C.c_size_t])
    working_set = bind(runtime.kernel, "K32QueryWorkingSetEx", W.BOOL,
                       [W.HANDLE, C.c_void_p, U32])

    def peer_read(where, length):
        data, done = C.create_string_buffer(length), C.c_size_t()
        runtime.check(runtime.windows_read(peer_handle, where, data, length, C.byref(done)) and done.value == length,
                       "second image original read completed")
        return data.raw

    def peer_command(expected):
        count = struct.unpack("<Q", peer_read(peer_info["control"] + 16, 8))[0]
        for where, value in ((peer_info["control"] + 24, 0x100), (peer_info["control"], 4)):
            data, done = C.c_uint64(value), C.c_size_t()
            runtime.check(runtime.windows_write(peer_handle, where, C.byref(data), 8, C.byref(done)) and done.value == 8,
                           "second target receives its own image-probe command")
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            value, done = C.c_uint64(), C.c_size_t()
            if not runtime.windows_read(peer_handle, peer_info["control"] + 16, C.byref(value), 8, C.byref(done)):
                raise RuntimeError("second target command polling failed")
            if value.value == count + 1:
                break
            if peer_process.poll() is not None:
                raise RuntimeError("second image exited before completing its probe")
            time.sleep(.025)
        else:
            raise TimeoutError("second image probe did not complete")
        actual = struct.unpack("<Q", peer_read(peer_info["control"] + 8, 8))[0]
        runtime.check(actual == expected, f"second image actually executes original code and returns {expected:#x}")

    def backing(handle, where):
        info, entry = MemoryInfo(), WorkingSet(where, 0)
        runtime.check(query(handle, where, C.byref(info), C.sizeof(info)) == C.sizeof(info),
                       "image virtual-memory classification completed")
        runtime.check(info.state == 0x1000 and info.type == 0x1000000 and
                      info.protect & (0x10 | 0x20 | 0x40 | 0x80) != 0,
                       "probe remains committed executable MEM_IMAGE")
        runtime.check(working_set(handle, C.byref(entry), C.sizeof(entry)), "image working-set query completed")
        runtime.check(entry.flags & 1 != 0, "image working-set entry is valid")
        return info, entry.flags

    runtime.wr(runtime.info["control"] + 24, struct.pack("<Q", 0x100), windows=True)
    runtime.command(4, 0x101)
    peer_command(0x101)
    original = runtime.rd(address, 5, windows=True)
    peer_original = peer_read(peer_address, 5)
    runtime.observations["imageProbeCode"] = {"main": runtime.rd(address, 16, windows=True).hex(),
        "peer": peer_read(peer_address, 16).hex()}
    print("IMAGE_PROBE_BYTES=" + json.dumps(runtime.observations["imageProbeCode"]), flush=True)
    runtime.check(original == b"\x48\x8d\x41\x01\xc3" and peer_original == original,
                   "Release/x64 image probe is verified lea rax,[rcx+1];ret in both images")
    runtime.check((address & 4095) + 5 <= 4096, "known image instruction fits one backing page")
    before, flags = backing(runtime.ph, address)
    peer_before, peer_flags = backing(peer_handle, peer_address)
    runtime.check(address - before.allocation == peer_address - peer_before.allocation,
                   "both targets use the same image file and probe RVA")
    runtime.observations["imageBeforeCow"] = {"main": fields(before), "mainFlags": flags,
        "peer": fields(peer_before), "peerFlags": peer_flags, "originalCode": original.hex()}
    runtime.check(flags & 0x8000 != 0 and peer_flags & 0x8000 != 0,
                   "both executable image pages are physically shared before COW")
    runtime.wr(address + 3, b"\x02", expected=50)
    runtime.check(runtime.rd(address, 5, windows=True) == original and peer_read(peer_address, 5) == original,
                   "shared-image Shadow refusal changes neither original mapping")
    runtime.check(runtime.policy().shadowWritePages == 0, "shared-image refusal publishes no owned Shadow patch")
    runtime.command(4, 0x101)
    peer_command(0x101)
    runtime.log_contains("backing is still shared", "no automatic same-byte write into a running target")

    # Test setup only: explicitly request Windows COW by writing the exact existing
    # byte. This does not assert that the production backend does so automatically.
    runtime.wr(address + 3, original[3:4], windows=True)
    after, private_flags = backing(runtime.ph, address)
    _, peer_flags_after = backing(peer_handle, peer_address)
    runtime.observations["imageAfterCow"] = {"main": fields(after), "mainFlags": private_flags,
        "peerFlags": peer_flags_after}
    runtime.check(private_flags & 0x8000 == 0 and peer_flags_after & 0x8000 != 0,
                   "explicit same-byte Windows write creates private COW backing for only the first image")
    runtime.check(after.protect == before.protect and runtime.rd(address, 5, windows=True) == original,
                   "Windows COW setup preserves original instruction bytes and protection")
    runtime.wr(address + 3, b"\x02")
    runtime.check(runtime.rd(address, 5) == original and runtime.rd(address, 5, windows=True) == original,
                   "private COW image Shadow edit remains absent from frontend and ordinary reads")
    runtime.check(peer_read(peer_address, 5) == original, "peer original code remains unchanged during Shadow execution")
    policy = runtime.policy()
    runtime.check(policy.activePath == 2 and policy.shadowWritePages == 1,
                   "private COW MEM_IMAGE edit reports one actual Shadow page")
    runtime.command(4, 0x102)
    peer_command(0x101)
    runtime.restore(address + 3, 1)
    runtime.check(runtime.policy().shadowWritePages == 0, "COW image range restore retires the memory patch")
    runtime.command(4, 0x101)
    peer_command(0x101)
    runtime.check(runtime.rd(address, 5, windows=True) == original and peer_read(peer_address, 5) == original,
                   "restore preserves original bytes in both images")
    runtime.select(0)


def breakpoint(runtime):
    stealth = runtime.phase.startswith("stealth")
    hvm = runtime.phase.endswith("hvm") and runtime.phase != "stealth-no-hvm"
    runtime.set_options(mode=int(stealth), shadowMemoryWrites=0, allowFallback=1)
    runtime.start_target()
    callback_type, attached_type = C.CFUNCTYPE(None, C.c_void_p), C.CFUNCTYPE(None)
    sethw = bind(runtime.dll, "SetHardwareBreakPoint", C.c_bool,
                 [C.c_size_t, U32, C.c_int, C.c_int, callback_type])
    deletehw = bind(runtime.dll, "DeleteHardwareBreakPoint", C.c_bool, [U32])
    attach = bind(runtime.dll, "AttachDebugger", C.c_bool, [U32, C.c_bool, C.c_void_p, attached_type])
    detach = bind(runtime.dll, "DetachDebuggerEx", C.c_bool, [U32])
    stop = bind(runtime.dll, "StopDebug", C.c_bool, [])
    getreg = bind(runtime.dll, "GetContextDataEx", C.c_size_t, [W.HANDLE, C.c_int])
    open_thread = bind(runtime.dll, "TitanOpenThread", W.HANDLE, [U32, C.c_bool, U32])
    thread = open_thread(0x1FFFFF, False, runtime.info["tid"])
    runtime.check(bool(thread), "exact fixture main thread opened")
    failures, seen = [], {"attached": 0, "hits": 0}
    address = runtime.info["code"] + 3
    original = runtime.rd(runtime.info["code"], 17, windows=True)

    def safe(function):
        def wrapper(*args):
            try:
                function(*args)
            except BaseException:
                failures.append(traceback.format_exc())
                print(failures[-1], flush=True)
                stop()
        return wrapper

    @callback_type
    @safe
    def hit(_):
        seen["hits"] += 1
        runtime.check(getreg(thread, 25) == address, "real execute breakpoint stops at exact instruction RIP")
        runtime.check(getreg(thread, 17) == 0x100, "execute breakpoint stops before instruction changes RAX")
        if stealth:
            runtime.observations["heldBeforeRestore"] = fields(runtime.policy())
            runtime.restore()
            policy = runtime.policy()
            runtime.observations["heldAfterRestore"] = fields(policy)
            print("HELD_RESTORE_POLICY=" + json.dumps(fields(policy)), flush=True)
            runtime.check(policy.shadowWritePages == 0 and policy.activeBreakpoints == 1 and policy.activePath == 2,
                           "restoring code mask retains the hidden INT3 breakpoint")
            runtime.check(runtime.rd(runtime.info["code"], 17, windows=True) == original,
                           "INT3 and masked patch remain absent from ordinary reads after restore")
            if seen["hits"] == 1:
                runtime.wr(runtime.info["control"], struct.pack("<Q", 1), windows=True)
                return  # The native one-instruction retry must rearm the logical binding.
        runtime.check(deletehw(11), "hardware slot retired through its actual adapter route")
        runtime.check(runtime.policy().activeBreakpoints == 0, "actual binding count returns to zero")
        runtime.set_options(maxShadowPages=1)
        runtime.select(0)
        runtime.check(detach(runtime.process.pid), "native debug loop detaches test target")

    @attached_type
    @safe
    def attached():
        seen["attached"] += 1
        state = runtime.select(int(hvm))
        if stealth and not hvm:
            C.set_last_error(0)
            runtime.check(not sethw(address, 11, 4, 7, hit) and C.get_last_error() == 21,
                           "stealth without HVM rejects visible hardware DR fallback")
            policy = runtime.policy()
            runtime.check(policy.activePath == 0 and policy.activeBreakpoints == 0,
                           "rejected stealth binding reports no installed breakpoint")
            runtime.check(detach(runtime.process.pid), "rejected stealth fixture detaches cleanly")
            return
        if stealth:
            # Stealth forces Shadow writes even when the explicit checkbox is off.
            runtime.wr(runtime.info["code"] + 5, b"\xc8")
        runtime.check(sethw(address, 11, 4, 7, hit), "execute slot installed using selected normal/stealth route")
        policy = runtime.policy()
        expected_path = 2 if stealth else (1 if state.eptBreakpointProtocol else 2) if hvm else 0
        runtime.observations["installedPath"] = policy.activePath
        runtime.check(policy.activePath == expected_path and policy.activeBreakpoints >= 1 and policy.canChangeOptions == 0,
                       f"actual breakpoint route and guard report path {expected_path}")
        runtime.set_options(170, maxShadowPages=1)
        runtime.set_options()
        if hvm:
            runtime.select(0, 170)
            runtime.check(runtime.rd(runtime.info["code"], 17, windows=True) == original,
                           "HVM breakpoint installation keeps original code unchanged")
        runtime.wr(runtime.info["control"], struct.pack("<Q", 1), windows=True)

    try:
        runtime.check(attach(runtime.process.pid, False, None, attached), "real native attach loop completed")
        runtime.check(not failures and seen["attached"] == 1, "attached callback completed without swallowed ctypes exceptions")
        expected_hits = 0 if stealth and not hvm else 2 if stealth else 1
        runtime.check(seen["hits"] == expected_hits, "exact expected breakpoint hit count observed")
        if not (stealth and not hvm):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and runtime.u64(runtime.info["control"] + 16) < expected_hits:
                time.sleep(.025)
            runtime.check(runtime.u64(runtime.info["control"] + 16) == expected_hits,
                           "each breakpoint invocation completed its resumed target command")
            runtime.check(runtime.u64(runtime.info["data"]) == 0x101,
                           "retired/restored instruction resumes and stores original result")
        if stealth:
            runtime.log_contains("Windows debug-event transport remains active")
            runtime.log_contains("actual=ShadowPage hidden INT3" if hvm else "visible hardware debug registers are forbidden")
        elif hvm and runtime.observations["installedPath"] == 2:
            runtime.log_contains("ShadowPage hidden INT3", "installed successfully")
    finally:
        runtime.close(thread)
    runtime.observations["callbacks"] = seen


def run_phase(root, phase):
    runtime, error, cleanup_errors = None, None, []
    folder = root / "policy-regression" / phase
    folder.mkdir(parents=True, exist_ok=True)
    try:
        runtime = Runtime(root, phase)
        {"offline": offline, "memory": memory, "image-cow": image_cow}.get(phase, breakpoint)(runtime)
    except BaseException:
        error = traceback.format_exc()
        print(error, flush=True)
    finally:
        if runtime:
            try:
                cleanup_errors = runtime.cleanup()
            except BaseException:
                cleanup_errors.append(traceback.format_exc())
        result = {"phase": phase, "passed": error is None and not cleanup_errors, "error": error,
                  "cleanupErrors": cleanup_errors, "assertions": runtime.assertions if runtime else [],
                  "observations": runtime.observations if runtime else {}}
        (folder / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return 0 if result["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--run-in-vm", action="store_true", help="explicitly enable live guest policy tests")
    parser.add_argument("--phase", choices=PHASES, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    if not arguments.run_in_vm or os.name != "nt" or C.sizeof(C.c_void_p) != 8:
        parser.error("run only in the prepared Windows x64 VM with --run-in-vm")
    if (C.sizeof(Call), C.sizeof(Status), C.sizeof(Options), C.sizeof(Policy), C.sizeof(Restore), C.sizeof(JobLimits)) != (48, 40, 48, 72, 24, 144):
        raise RuntimeError("unexpected x64 public API or Windows job ABI layout")
    root = arguments.root.resolve()
    if arguments.phase:
        return run_phase(root, arguments.phase)
    artifact = root / "policy-regression"
    artifact.mkdir(parents=True, exist_ok=True)
    results = []
    for phase in PHASES:
        (artifact / phase / "result.json").unlink(missing_ok=True)
        command = [sys.executable, str(Path(__file__).resolve()), str(root), "--run-in-vm", "--phase", phase]
        try:
            child = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   timeout=65, creationflags=subprocess.CREATE_NO_WINDOW)
            output, exit_code = child.stdout.decode("utf-8", errors="replace"), child.returncode
        except subprocess.TimeoutExpired as failure:
            output = (failure.stdout or b"").decode("utf-8", errors="replace") + "\nFAIL phase timed out after 65 seconds\n"
            exit_code = 124
        (artifact / f"{phase}.log").write_text(output, encoding="utf-8")
        print(output, end="", flush=True)
        result_file = artifact / phase / "result.json"
        result = json.loads(result_file.read_text(encoding="utf-8")) if result_file.exists() else {"phase": phase, "passed": False}
        result["exitCode"] = exit_code
        result["passed"] = result["passed"] and exit_code == 0
        results.append(result)
        # A failed cleanup may leave HVM state requiring investigation. Stop here.
        if not result["passed"]:
            break
    passed = len(results) == len(PHASES) and all(result["passed"] for result in results)
    (artifact / "result.json").write_text(json.dumps({"passed": passed, "phases": results}, indent=2), encoding="utf-8")
    print("FINAL " + ("PASS" if passed else "FAIL") + " policy regression", flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
