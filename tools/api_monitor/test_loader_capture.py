"""Tagged loader search paths and unreadable strings must not alter the real API call."""
from pathlib import Path
import subprocess
import tempfile
from live_fixture_support import LiveFixture, ROOT

targets = (ROOT / "APIMonitor_x64/hook/HookTargets.cpp").read_text(encoding="utf-8-sig")
helpers = targets[targets.index("        // Read only the bounded visible prefix"):targets.index("        // AppendObjectNameText")]
unit = r'''
#define NOMINMAX
#include <Windows.h>
#include <winternl.h>
#include <cstdio>
#include "EXPORTS"
using namespace apimon;
HELPERS
#define CHECK(x) do{if(!(x)){printf("FAIL %d\n",__LINE__);return 1;}}while(0)
int main(){
 wchar_t text[320]={};AppendWideText(text,reinterpret_cast<const wchar_t*>(1));CHECK(wcscmp(text,L"<unreadable>")==0);
 text[0]=0;AppendAnsiText(text,reinterpret_cast<const char*>(1));CHECK(wcscmp(text,L"<unreadable>")==0);
 SYSTEM_INFO system{};GetSystemInfo(&system);auto n=system.dwPageSize;
 auto p=static_cast<char*>(VirtualAlloc(nullptr,n*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));CHECK(p);
 DWORD old=0;CHECK(VirtualProtect(p+n,n,PAGE_NOACCESS,&old));
 p[n-2]='A';p[n-1]=0;text[0]=0;AppendAnsiText(text,p+n-2);CHECK(wcscmp(text,L"A")==0);
 p[n-1]='B';text[0]=0;AppendAnsiText(text,p+n-2);CHECK(wcscmp(text,L"AB<unreadable>")==0);
 auto w=reinterpret_cast<wchar_t*>(p+n-2);*w=L'Z';text[0]=0;AppendWideText(text,w);CHECK(wcscmp(text,L"Z<unreadable>")==0);
 text[0]=0;AppendWideText(text,w,1);CHECK(wcscmp(text,L"Z")==0);
 CHECK(VirtualProtect(p+n,n,PAGE_READWRITE|PAGE_GUARD,&old));text[0]=0;AppendAnsiText(text,p+n);
 MEMORY_BASIC_INFORMATION info{};VirtualQuery(p+n,&info,sizeof(info));CHECK((info.Protect&PAGE_GUARD)&&wcscmp(text,L"<unreadable>")==0);
 UNICODE_STRING malformed{3,4,w};text[0]=0;AppendUnicodeStringText(text,&malformed);CHECK(wcscmp(text,L"<unreadable>")==0);
 text[0]=0;AppendUnicodeStringText(text,reinterpret_cast<UNICODE_STRING*>(1));CHECK(wcscmp(text,L"<unreadable>")==0);
 UNICODE_STRING bounded{2,2,w};text[0]=0;AppendUnicodeStringText(text,&bounded);CHECK(wcscmp(text,L"Z")==0);
 VirtualFree(p,0,MEM_RELEASE);puts("PASS: production capture, tagged/unreadable pointers, bounded strings and untouched guard pages");return 0;
}
'''.replace("EXPORTS", (ROOT / "APIMonitor_x64/hook/ExportCatalog.h").as_posix()).replace("HELPERS", helpers)
with tempfile.TemporaryDirectory(prefix="ksword_capture_pages_") as temporary:
    folder = Path(temporary)
    cpp = folder / "fixture.cpp"
    cpp.write_text(unit, encoding="utf-8")
    subprocess.run(["cl", "/nologo", "/EHsc", "/std:c++17", "/utf-8", str(cpp),
                    "/Fe:"+str(folder/"fixture.exe"), "/Fo:"+str(folder/"fixture.obj")], check=True)
    subprocess.run([str(folder/"fixture.exe")], check=True, timeout=30)

source = r'''
#include <Windows.h>
#include <winternl.h>
#include <cstdio>
using Loader=LONG(NTAPI*)(PWSTR,PULONG,PUNICODE_STRING,PHANDLE);
LONG LoadTagged(Loader load){wchar_t name[]=L"version.dll";UNICODE_STRING n={sizeof(name)-2,sizeof(name),name};ULONG flags=0;HANDLE h=nullptr;
 return load(reinterpret_cast<PWSTR>(1),&flags,&n,&h);}
int wmain(int argc,wchar_t**argv){if(argc!=2)return 1;auto load=reinterpret_cast<Loader>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"LdrLoadDll"));
 if(!load||LoadTagged(load)<0)return 2;SetLastError(12345);auto baseline=LoadTagged(load);auto error=GetLastError();
 printf("%lu\n",GetCurrentProcessId());fflush(stdout);
 for(int c;(c=getchar())!=EOF;){if(c=='L'&&!LoadLibraryW(argv[1]))return 3;
  if(c=='X'){SetLastError(12345);auto result=LoadTagged(load);if(result!=baseline||GetLastError()!=error)return 4;puts("DONE");fflush(stdout);}
  if(c=='Q')return 0;}return 0;}
'''
with LiveFixture(source, {"enable_file": 0, "enable_loader": 1}) as fixture:
    fixture.wait_for(lambda f: any(e.api == "HooksInstalled" for e in f.events))
    fixture.command("X")
    fixture.wait_for(lambda f: any(e.api == "LdrLoadDll" and "search=tagged:0x1" in e.detail for e in f.events))
    assert fixture.process.stdout.readline().strip() == b"DONE"
    event = next(e for e in fixture.events if e.api == "LdrLoadDll" and "search=tagged:0x1" in e.detail)
    assert event.result == 0 and "path=version.dll" in event.detail
print("PASS: real loader tagged search path, original result/LastError and useful event detail")
