"""Invoke blocked native clipboard exports in an owned process without changing the clipboard."""
from live_fixture_support import LiveFixture

source = r'''
#include <Windows.h>
#include <cstdio>
int wmain(int argc,wchar_t** argv){if(argc!=2)return 1;LoadLibraryW(L"user32.dll");auto module=LoadLibraryW(L"win32u.dll");
 printf("%lu\n",GetCurrentProcessId());fflush(stdout);
 for(int c;(c=getchar())!=EOF;){if(c=='L'){if(!LoadLibraryW(argv[1]))return 2;}if(c=='X'){
  auto read=(HANDLE(WINAPI*)(UINT,void*))GetProcAddress(module,"NtUserGetClipboardData");
  auto write=(HANDLE(WINAPI*)(UINT,HANDLE,void*))GetProcAddress(module,"NtUserSetClipboardData");
  auto empty=(BOOL(WINAPI*)())GetProcAddress(module,"NtUserEmptyClipboard");if(!read||!write||!empty)return 3;
  for(unsigned i=0;i<100;++i){SetLastError(0);if(read(CF_UNICODETEXT,nullptr)||GetLastError()!=ERROR_ACCESS_DENIED)return 10;
   SetLastError(0);if(write(CF_UNICODETEXT,nullptr,nullptr)||GetLastError()!=ERROR_ACCESS_DENIED)return 11;
   SetLastError(0);if(empty()||GetLastError()!=ERROR_ACCESS_DENIED)return 12;}
  printf("DONE\n");fflush(stdout);}if(c=='Q')return 0;}return 0;}
'''
with LiveFixture(source, {"enable_file": 0, "enable_clipboard": 1, "clipboard_read_action": 1, "clipboard_write_action": 1}) as fixture:
    fixture.wait_for(lambda f: bool(f.snapshots) and any(e.api == "HooksInstalled" for e in f.events))
    for name in ("NtUserGetClipboardData", "NtUserSetClipboardData", "NtUserEmptyClipboard"):
        assert any(r.api == name and r.state in (0, 1) for r in fixture.snapshots[-1][1])
    fixture.command("X")
    fixture.wait_for(lambda f: any(e.api == "NtUserEmptyClipboard" for e in f.events))
    assert fixture.process.stdout.readline().strip() == b"DONE"
print("PASS: native clipboard HANDLE/BOOL failures, errors and x86 callee stack cleanup")
