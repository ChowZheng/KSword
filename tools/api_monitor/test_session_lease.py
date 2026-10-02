"""Check exclusive ownership and release from a different thread."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
code=r"""
#include "PROTOCOL_PATH"
#include <thread>
#include <cstdio>
int main() {
 using ks::winapi_monitor::SessionLease;
 DWORD error=0;const auto pid=GetCurrentProcessId();
 SessionLease first, second;
 if(!first.acquire(pid,&error)) return 1;
 if(second.acquire(pid,&error)||error!=ERROR_BUSY) return 2;
 SessionLease moved(std::move(first));
 if(first.pid()!=0||moved.pid()!=pid) return 3;
 std::thread release([&]{moved.reset();}); release.join();
 if(!second.acquire(pid,&error)) return 4;
 printf("PASS: duplicate owner rejected; moved lease released by another thread\n");
}
"""
code=code.replace("PROTOCOL_PATH",(root/"shared/WinApiMonitorProtocol.h").as_posix())
with tempfile.TemporaryDirectory(prefix="ksword_lease_") as temp:
 folder=Path(temp);cpp=folder/"test.cpp";cpp.write_text(code,encoding="utf-8")
 subprocess.run(["cl","/nologo","/EHsc","/std:c++17","/utf-8",str(cpp),"/Fe:"+str(folder/"test.exe"),"/Fo:"+str(folder/"test.obj")],check=True)
 subprocess.run([str(folder/"test.exe")],check=True,timeout=30)
