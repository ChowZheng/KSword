"""All categories plus the real default Raw module set under concurrent COM/heap/thread activity."""
import time
from live_fixture_support import LiveFixture
source = r'''

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>
#include <string>
#pragma comment(lib,"Advapi32.lib")
std::atomic<bool> quit{false};
DWORD WINAPI Activity(void*){
 while(!quit){
  auto heap=GetProcessHeap();void* p=HeapAlloc(heap,0,128);if(p){memset(p,1,128);HeapFree(heap,0,p);}
  HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(e){SetEvent(e);WaitForSingleObject(e,0);CloseHandle(e);}
  HKEY k;if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software",0,KEY_READ,&k)==ERROR_SUCCESS){DWORD type=0,len=0;RegQueryValueExW(k,L"MissingFixture",nullptr,&type,nullptr,&len);RegCloseKey(k);}
  wchar_t path[MAX_PATH];GetTempPathW(MAX_PATH,path);GetModuleFileNameW(nullptr,path,MAX_PATH);
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);GUID g;CoCreateGuid(&g);CoUninitialize();
  GetVersion();GetProcessId(GetCurrentProcess());Sleep(2);
 }return 0;
}
int wmain(int argc,wchar_t**argv){
 if(argc!=2)return 1;
 const wchar_t* modules[]={L"user32.dll",L"gdi32.dll",L"gdi32full.dll",L"ws2_32.dll",L"wininet.dll",L"winhttp.dll",L"iphlpapi.dll",L"dnsapi.dll",L"netapi32.dll",L"secur32.dll",L"rpcrt4.dll",L"ole32.dll",L"oleaut32.dll",L"combase.dll",L"shell32.dll",L"shlwapi.dll",L"crypt32.dll",L"bcrypt.dll",L"ncrypt.dll",L"wintrust.dll",L"urlmon.dll",L"psapi.dll",L"wtsapi32.dll",L"version.dll",L"userenv.dll",L"profapi.dll",L"samcli.dll",L"wldap32.dll",L"setupapi.dll",L"cfgmgr32.dll",L"wevtapi.dll",L"tdh.dll"};
 for(auto m:modules)LoadLibraryW(m);
 std::vector<HANDLE> threads;for(unsigned i=0;i<4;++i)threads.push_back(CreateThread(nullptr,0,Activity,nullptr,0,nullptr));
 printf("%lu\n",GetCurrentProcessId());fflush(stdout);
 for(int c;(c=getchar())!=EOF;){if(c=='L'){if(!LoadLibraryW(argv[1]))return 2;}if(c=='Q')break;}
 quit=true;for(auto t:threads){WaitForSingleObject(t,5000);CloseHandle(t);}return 0;
}

'''
raw = 'KernelBase.dll;kernel32.dll;advapi32.dll;user32.dll;gdi32.dll;gdi32full.dll;ws2_32.dll;wininet.dll;winhttp.dll;iphlpapi.dll;dnsapi.dll;netapi32.dll;secur32.dll;rpcrt4.dll;ole32.dll;oleaut32.dll;combase.dll;shell32.dll;shlwapi.dll;crypt32.dll;bcrypt.dll;ncrypt.dll;wintrust.dll;urlmon.dll;psapi.dll;wtsapi32.dll;version.dll;userenv.dll;profapi.dll;samcli.dll;wldap32.dll;setupapi.dll;cfgmgr32.dll;wevtapi.dll;tdh.dll'

config={f'enable_{s}':1 for s in ['file','registry','network','process','loader','clipboard']}
config.update(enable_raw_fallback=1,raw_modules=raw,raw_use_default_denylist=1)
with LiveFixture(source,config) as fixture:
 fixture.wait_for(lambda f:bool(f.snapshots) and any(e.api=='HooksInstalled' for e in f.events),timeout=60)
 rows=fixture.snapshots[-1][1]
 assert len([r for r in rows if r.hook_kind==1 and r.state in (0,1)])>=100
 excluded=[r for r in rows if r.module.lower()=='gdi32.dll' and r.api=='EngAcquireSemaphore' and r.hook_kind==1]
 assert excluded and all(r.state==5 and 'ntdll' in r.detail for r in excluded)
 end=time.monotonic()+8;observed=0
 while time.monotonic()<end:
  fixture.pump()
  assert fixture.process.poll() is None,fixture.process.returncode
  observed+=len(fixture.events);fixture.events.clear()
  time.sleep(.01)
 assert observed>=100
 # Stop while callers continue; close() waits for HooksRemoved and then retires the owned process normally.
print('PASS: all categories/default Raw, native aliases excluded, concurrent COM/thread activity, '+str(observed)+' events and stop')
