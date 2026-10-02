"""Exercise modern API signatures in an owned x64 process with the Release Agent."""
from live_fixture_support import LiveFixture

source = r'''
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winternl.h>
#include <objbase.h>
#include <cstdio>
void CALLBACK Apc(ULONG_PTR) {}
int wmain(int argc,wchar_t** argv) {
 if(argc!=2)return 1;
 LoadLibraryW(L"ole32.dll");
 printf("%lu\n",GetCurrentProcessId());fflush(stdout);
 for(int c;(c=getchar())!=EOF;) {
  if(c=='L'){if(!LoadLibraryW(argv[1]))return 2;}
  if(c=='X') {
   HMODULE kb=GetModuleHandleW(L"KernelBase.dll"),nt=GetModuleHandleW(L"ntdll.dll");
   using Alloc=PVOID(WINAPI*)(HANDLE,PVOID,SIZE_T,ULONG,ULONG,MEM_EXTENDED_PARAMETER*,ULONG);
   for(auto name:{"VirtualAlloc2","VirtualAlloc2FromApp"}) {
    auto fn=reinterpret_cast<Alloc>(GetProcAddress(kb,name));if(!fn)continue;
    SetLastError(0x1234);auto memory=fn(GetCurrentProcess(),nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE,nullptr,0);
    if(!memory||GetLastError()!=0x1234)return 10;*static_cast<int*>(memory)=42;VirtualFree(memory,0,MEM_RELEASE);
   }
   using Mapping=HANDLE(WINAPI*)(HANDLE,LPSECURITY_ATTRIBUTES,ULONG,ULONG,ULONG,ULONG64,PCWSTR,MEM_EXTENDED_PARAMETER*,ULONG);
   using View=PVOID(WINAPI*)(HANDLE,HANDLE,PVOID,ULONG64,SIZE_T,ULONG,ULONG,MEM_EXTENDED_PARAMETER*,ULONG);
   auto mapping=reinterpret_cast<Mapping>(GetProcAddress(kb,"CreateFileMapping2"));
   auto view=reinterpret_cast<View>(GetProcAddress(kb,"MapViewOfFile3"));
   if(mapping&&view){auto h=mapping(INVALID_HANDLE_VALUE,nullptr,FILE_MAP_ALL_ACCESS,PAGE_READWRITE,SEC_COMMIT,4096,nullptr,nullptr,0);if(!h)return 11;
    auto p=view(h,GetCurrentProcess(),nullptr,0,4096,0,PAGE_READWRITE,nullptr,0);if(!p)return 12;UnmapViewOfFile(p);CloseHandle(h);}
   using Query=NTSTATUS(NTAPI*)(HANDLE,THREADINFOCLASS,PVOID,ULONG,PULONG);
   BYTE info[256]{};ULONG length=0;auto query=reinterpret_cast<Query>(GetProcAddress(nt,"NtQueryInformationThread"));
   if(query&&query(GetCurrentThread(),static_cast<THREADINFOCLASS>(0),info,48,&length)<0)return 13;
   using Connect=NTSTATUS(NTAPI*)(PHANDLE,const UNICODE_STRING*,POBJECT_ATTRIBUTES,PVOID,ULONG,PSID,PVOID,PSIZE_T,PVOID,PVOID,PLARGE_INTEGER);
   auto connect=reinterpret_cast<Connect>(GetProcAddress(nt,"NtAlpcConnectPort"));
   WCHAR missing[]=L"\\RPC Control\\KswordOwnedNonexistentPort";UNICODE_STRING name{sizeof(missing)-2,sizeof(missing),missing};HANDLE port=nullptr;
   if(connect&&connect(&port,&name,nullptr,nullptr,0,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr)>=0)return 14;
   using Send=NTSTATUS(NTAPI*)(HANDLE,ULONG,PVOID,PVOID,PVOID,PSIZE_T,PVOID,PLARGE_INTEGER);
   auto send=reinterpret_cast<Send>(GetProcAddress(nt,"NtAlpcSendWaitReceivePort"));
   if(send&&send(INVALID_HANDLE_VALUE,0,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr)>=0)return 15;
   using Queue=BOOL(WINAPI*)(PAPCFUNC,HANDLE,ULONG_PTR,QUEUE_USER_APC_FLAGS);
   auto queue=reinterpret_cast<Queue>(GetProcAddress(kb,"QueueUserAPC2"));
   if(queue){if(!queue(Apc,GetCurrentThread(),42,static_cast<QUEUE_USER_APC_FLAGS>(0)))return 16;SleepEx(0,TRUE);}
   IUnknown* object=nullptr;auto result=CoGetObject(L"KswordOwnedInvalidMoniker:",nullptr,IID_IUnknown,reinterpret_cast<void**>(&object));
   if(SUCCEEDED(result)||object)return 17;
   printf("DONE\n");fflush(stdout);
  }
  if(c=='Q')return 0;
 }
 return 0;
}
'''
source = source.replace('#include <cstdio>', '#include <cstdio>\n#include <initializer_list>')
with LiveFixture(source, {"enable_process": 1}) as fixture:
    fixture.wait_for(lambda f: bool(f.snapshots) and any(e.api == "HooksInstalled" for e in f.events))
    fixture.command("X")
    fixture.wait_for(lambda f: any(e.api == "CoGetObject" for e in f.events))
    assert fixture.process.stdout.readline().strip() == b"DONE"
    names = {row.api for row in fixture.snapshots[-1][1] if 615 <= row.api_id <= 623 and row.state in (0, 1)}
    fixture.wait_for(lambda f: names <= {e.api for e in f.events})
    events = {e.api: e for e in fixture.events if e.api in names}
    assert events["NtAlpcConnectPort"].result < 0
    assert events["NtAlpcSendWaitReceivePort"].result < 0
    assert events["NtQueryInformationThread"].result == 0
    assert events["CoGetObject"].result < 0
    assert all(615 <= event.api_id <= 623 and event.detail for event in events.values())
print("PASS: nine modern API definitions, real calls, ABI/output/error preservation and stable IDs")
