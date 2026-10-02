"""Inject into an owned executable with the same 256 KiB default stack as x86 Notepad."""
from live_fixture_support import LiveFixture

source = r'''
#include <Windows.h>
#include <cstdio>
int wmain(int argc,wchar_t**argv){if(argc!=2)return 1;
 auto base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
 auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
 if(nt->OptionalHeader.SizeOfStackReserve!=262144)return 2;
 printf("%lu\n",GetCurrentProcessId());fflush(stdout);
 for(int c;(c=getchar())!=EOF;){if(c=='L'&&!LoadLibraryW(argv[1]))return 3;
  if(c=='X'){for(unsigned i=0;i<400;++i)GetFileAttributesW(L"C:\\Windows\\win.ini");puts("DONE");fflush(stdout);}
  if(c=='Q')return 0;}return 0;}
'''
with LiveFixture(source, link_arguments=("/STACK:262144,4096",)) as fixture:
    fixture.wait_for(lambda f: bool(f.snapshots) and any(e.api == "HooksInstalled" for e in f.events))
    fixture.command("X")
    fixture.wait_for(lambda f: any(e.api == "GetFileAttributesW" for e in f.events))
    assert fixture.process.stdout.readline().strip() == b"DONE"
print("PASS: 256 KiB inherited worker/sender stacks, real event batches, complete coverage and clean stop")
