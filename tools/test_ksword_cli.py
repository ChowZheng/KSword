"""Run the production CLI against an in-process Windows transport fixture.

No driver is loaded and no service or target process is changed. Run from an
x64 VS developer shell: python tools/test_ksword_cli.py.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#define NOMINMAX
#include <WinSock2.h>
#include <Windows.h>
#include <cstring>
#include <string>
static unsigned reads = 0;
static DWORD openError = 0, ioctlError = ERROR_NOT_SUPPORTED;
static HANDLE WINAPI testOpen(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
    if (openError) { SetLastError(openError); return INVALID_HANDLE_VALUE; }
    return reinterpret_cast<HANDLE>(123);
}
static BOOL WINAPI testClose(HANDLE) { return TRUE; }
static BOOL WINAPI testRead(HANDLE, LPVOID data, DWORD capacity, LPDWORD bytes, LPOVERLAPPED) {
    ++reads;
    const std::string frame = reads == 1 ? "fixture log!\n" : "END_OF_LOG";
    if (capacity < frame.size()) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    std::memcpy(data, frame.data(), frame.size()); *bytes = static_cast<DWORD>(frame.size()); return TRUE;
}
static BOOL WINAPI testIoctl(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD bytes, LPOVERLAPPED) {
    *bytes = 0; SetLastError(ioctlError); return FALSE;
}
#define CreateFileW testOpen
#define CloseHandle testClose
#define ReadFile testRead
#define DeviceIoControl testIoctl
#define wmain productionMain
#include "CLI_SOURCE"
#undef wmain
int commandArkDriverExtended(int, wchar_t*[]) { return 3; }
int commandArkDriverCallbackMonitor(int, wchar_t*[]) { return 3; }
int wmain(int argc, wchar_t* argv[]) {
    if (argc > 1 && std::wstring(argv[1]) == L"--protocol") {
        std::wcout << IOCTL_KSWORD_ARK_ENUM_PROCESS << L" " << IOCTL_KSWORD_ARK_QUERY_DRIVER_CAPABILITIES << L"\n"; return 0;
    }
    if (argc > 1 && std::wstring(argv[1]) == L"--missing-device") { openError = ERROR_FILE_NOT_FOUND; ++argv; --argc; }
    if (argc > 1 && std::wstring(argv[1]) == L"--unsupported") { ++argv; --argc; }
    return productionMain(argc, argv);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", action="store_true", help="Observe the pre-fix log fastfail")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ksword-cli-") as temp:
        directory = Path(temp)
        source = directory / "regression.cpp"
        source.write_text(HARNESS.replace("CLI_SOURCE", (ROOT / "KswordCLI/KswordCLI.cpp").as_posix()), encoding="utf-8")
        binary = directory / "regression.exe"
        subprocess.run(["cl", "/nologo", "/std:c++17", "/EHsc", "/utf-8", "/O2", str(source),
                        "/Fe:" + str(binary), "/link", "Iphlpapi.lib", "Ws2_32.lib", "Setupapi.lib"], cwd=temp, check=True)
        cases = [("log",), ("log", "--max-frames", "0"), ("log", "--max-frames", "1"),
                 ("log", "--max-frames", "2"), ("log", "--max-frames", "100")]
        for command in cases:
            result = subprocess.run([str(binary), *command], capture_output=True, timeout=10)
            if args.baseline:
                print(command, hex(result.returncode & 0xffffffff), repr(result.stdout))
            else:
                assert result.returncode == 0, (command, result.returncode, result.stderr)
                assert result.stdout.replace(b"\r\n", b"\n") == (b"" if command[-1] == "0" else b"fixture log!\n"), (command, result.stdout)
        if not args.baseline:
            protocol = subprocess.run([str(binary), "--protocol"], capture_output=True, check=True)
            assert protocol.stdout.decode().split() == [str(0x222014), str(0x222028)], protocol.stdout
            unsupported = subprocess.run([str(binary), "capability", "query-driver-capabilities"], capture_output=True)
            assert b"preflight query" in unsupported.stderr and b"r0 ioctl-registry" in unsupported.stderr
            for command, option in [("process terminate", "--pid"), ("driver detail", "--driver"),
                                    ("kernel object-summary", "--target-kind"), ("handle enum --limit 4", "--pid")]:
                result = subprocess.run([str(binary), *command.split()], capture_output=True)
                assert result.returncode == 1 and ("missing option " + option).encode() in result.stderr, result.stderr
                assert b"usage: KswordCLI.exe" in result.stderr, result.stderr
            empty = subprocess.run([str(binary), "process", "terminate", "--pid"], capture_output=True)
            assert b"missing value for option --pid" in empty.stderr, empty.stderr
            unknown = subprocess.run([str(binary), "driver", "detail", "--name", "SkaProtect"], capture_output=True)
            assert unknown.returncode == 1 and b"unknown option --name (use --driver)" in unknown.stderr, unknown.stderr
            assert b"missing option" not in unknown.stderr
            valid = subprocess.run([str(binary), "driver", "detail", "--driver", "SkaProtect"], capture_output=True)
            assert valid.returncode == 5 and b"IOCTL_KSWORD_ARK_QUERY_DRIVER_OBJECT" in valid.stderr, valid.stderr
            for command in ["process enum --limit 4", "thread enum --limit 4", "kernel cid", "kernel ipc",
                            "driver integrity --limit 4", "callback runtime-state", "misc vbs", "dyn status"]:
                result = subprocess.run([str(binary), *command.split()], capture_output=True)
                assert result.returncode == 5 and b"win32=50" in result.stderr, (command, result.returncode, result.stderr)
            for command in ["process enum", "kernel ipc", "callback runtime-state", "misc vbs"]:
                result = subprocess.run([str(binary), "--missing-device", *command.split()], capture_output=True)
                assert result.returncode == 2 and b"win32=2" in result.stderr, (command, result.returncode, result.stderr)
            print("CLI regression: log default/0/1/2/100 passed (real CRT UTF-8 redirection)")


if __name__ == "__main__":
    main()
