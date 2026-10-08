"""Copy files and run scripts in an already-running VMware CLI test guest.

Uses the installed VIX SDK in-process; credentials never appear in a process
command line. The default account has the user-confirmed empty password.
"""
import argparse
import ctypes as c
import getpass
import os
from pathlib import Path
import time


class Guest:
    def __init__(self, dll, vmx, user, password):
        self.directory = os.add_dll_directory(str(Path(dll).parent))
        self.api = c.CDLL(str(dll))
        for name in ("VixJob_CheckCompletion", "VixJob_Wait", "VixJob_GetError"):
            getattr(self.api, name).restype = c.c_uint64
        self.api.Vix_GetErrorText.restype = c.c_char_p
        self.api.Vix_GetErrorText.argtypes = [c.c_uint64, c.c_char_p]
        self.host = self.wait(self.api.VixHost_Connect(-1, 3, None, 0, None, None, 0, 0, None, None), 3010)
        self.vm = self.wait(self.api.VixHost_OpenVM(self.host, vmx.encode(), 0, 0, None, None), 3010)
        self.wait(self.api.VixVM_LoginInGuest(self.vm, user.encode(), password.encode(), 0x08, None, None))

    def wait(self, job, property_id=0, timeout=60):
        end = time.monotonic() + timeout
        try:
            complete = c.c_int()
            while not complete.value:
                error = self.api.VixJob_CheckCompletion(job, c.byref(complete))
                self.check(error)
                if time.monotonic() > end:
                    raise TimeoutError("VIX operation timed out; do not retry a guest mutation without checking its state")
                if not complete.value:
                    time.sleep(.1)
            value = c.c_int64() if property_id == 3018 else c.c_int()
            error = self.api.VixJob_Wait(job, property_id, c.byref(value), 0) if property_id else self.api.VixJob_Wait(job, 0)
            self.check(error)
            return c.c_int(value.value).value if property_id == 3018 else value.value
        finally:
            self.api.Vix_ReleaseHandle(job)

    def check(self, error):
        if error:
            raise RuntimeError(f"VIX {error}: {self.api.Vix_GetErrorText(error, None).decode(errors='replace')}")

    def copy(self, source, destination, upload=True):
        method = self.api.VixVM_CopyFileFromHostToGuest if upload else self.api.VixVM_CopyFileFromGuestToHost
        self.wait(method(self.vm, source.encode(), destination.encode(), 0, 0, None, None))

    def run(self, program, arguments, timeout=60):
        return self.wait(self.api.VixVM_RunProgramInGuest(self.vm, program.encode(), arguments.encode(), 0, 0, None, None), 3018, timeout)

    def close(self):
        self.api.Vix_ReleaseHandle(self.vm)
        self.api.VixHost_Disconnect(self.host)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", required=True)
    parser.add_argument("--vmx", required=True)
    parser.add_argument("--user", default="Administrator")
    parser.add_argument("--ask-password", action="store_true")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("upload", "download"):
        command = commands.add_parser(name)
        command.add_argument("source")
        command.add_argument("destination")
    run = commands.add_parser("run")
    run.add_argument("program")
    run.add_argument("arguments")
    run.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()
    guest = Guest(args.dll, args.vmx, args.user, getpass.getpass("Guest password: ") if args.ask_password else "")
    try:
        if args.command == "run":
            result = guest.run(args.program, args.arguments, args.timeout)
            print(f"GUEST_EXIT_CODE={result}")
            raise SystemExit(result)
        guest.copy(args.source, args.destination, args.command == "upload")
        print("COPY_OK")
    finally:
        guest.close()


if __name__ == "__main__":
    main()
