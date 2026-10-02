"""Real x86/x64 Release injection, platform selection, PID identity and quoted paths."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from compiler_environment import target_environment
from live_fixture_support import LiveFixture, ROOT, ARCHITECTURE

source = r'''
#include <Windows.h>
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
 if(argc!=2)return 1; printf("%lu\n",GetCurrentProcessId());fflush(stdout);
 for(int c;(c=getchar())!=EOF;) {if(c=='L'&&!LoadLibraryW(argv[1]))return 2;if(c=='Q')return 0;}
 return 0;
}
'''
controller = r'''
#include "ApiMonitorInjection.h"
#include <cstdio>
#include <cstdlib>
#include <shellapi.h>
int wmain(int argc,wchar_t** argv) {
 using namespace ks::winapi_monitor;
 if(argc!=4)return 1;DWORD pid=wcstoul(argv[1],nullptr,10);
 std::wstring path,error;USHORT machine=0;
 if(!queryProcessMachine(pid,&machine,&error))return 2;
 if(!resolveAgentPath(pid,argv[2],&path,&error)){fwprintf(stderr,L"%s\n",error.c_str());return 3;}
 if(path.find(agentFileName(machine))==std::wstring::npos)return 4;
 // An explicit nonstandard DLL must be validated rather than silently replaced.
 if(resolveAgentPath(pid,argv[3],&error,nullptr)||GetLastError()!=ERROR_BAD_EXE_FORMAT)return 5;
 const std::wstring args[]={L"",L"a b",L"测试路径\\",L"a\\\"b",L"a\"b\\\\"};
 for(const auto& arg:args){
  std::wstring command=L"fixture "+quoteWindowsArgument(arg);int count=0;
  auto parsed=CommandLineToArgvW(command.c_str(),&count);bool ok=parsed&&count==2&&arg==parsed[1];
  if(parsed)LocalFree(parsed);if(!ok)return 6;
 }
 if(machine==currentMachine()&&injectAgentNative(pid,path,&error,1))return 7;
 if(!injectAgent(pid,path,&error)){fwprintf(stderr,L"%s\n",error.c_str());return 8;}
 printf("PASS: platform %04x selected and injected\n",machine);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="ksword 跨位数注入 ") as temporary:
    folder=Path(temporary);release=ROOT/"Ksword5.1/x64/Release"
    for arch in ("x86","x64"):
        for name in (f"APIMonitor_{arch}.dll",f"APIMonitorInject_{arch}.exe"):
            shutil.copy2(release/name,folder/name)
    # Copy x64 CRT alongside the copied Agent if the machine lacks the redistributable.
    for name in ("MSVCP140.dll","VCRUNTIME140.dll","VCRUNTIME140_1.dll"):
        if (release/name).exists():shutil.copy2(release/name,folder/name)
    cpp=folder/"controller.cpp";cpp.write_text(controller,encoding="utf-8")
    executable=folder/"controller.exe"
    subprocess.run(["cl","/nologo","/EHsc","/std:c++17","/utf-8","/MT","/I"+str(ROOT/"shared"),str(cpp),
                    "/Fe:"+str(executable),"/Fo:"+str(folder/"controller.obj"),"/link","Shell32.lib"],check=True)
    environments={arch:target_environment(arch) for arch in ("x86","x64")}
    for target in ("x86","x64"):
        opposite="x64" if target=="x86" else "x86"
        wrong=folder/"custom-wrong.dll";shutil.copy2(folder/f"APIMonitor_{opposite}.dll",wrong)
        def inject(pid,path):
            subprocess.run([str(executable),str(pid),str(folder/"APIMonitor_x64.dll"),str(wrong)],check=True)
        with LiveFixture(source,agent=folder/f"APIMonitor_{target}.dll",
                         compiler_environment=environments[target],injector=inject) as fixture:
            fixture.wait_for(lambda f:bool(f.snapshots) and any(e.api=="HooksInstalled" for e in f.events))
            ids={r.api_id for r in fixture.snapshots[-1][1] if r.api_id}
            assert set(range(1,642)).issubset(ids), sorted(set(range(1,642))-ids)
            assert {r.api for r in fixture.snapshots[-1][1] if r.hook_kind==3} == {
                "AcceptEx","ConnectEx","WSARecvMsg","WSASendMsg","TransmitFile","TransmitPackets","GetAcceptExSockaddrs"}
print(f"PASS: {ARCHITECTURE} controller injects both Release Agents; quoted Unicode paths, custom PE mismatch, PID identity, full coverage and stop")
