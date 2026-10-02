"""Verify x86 Raw/Fake calling conventions and preserved x87 state using production stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = r'''
#include <cstdio>
#include "STUBS"
using namespace apimon;
static unsigned calls=0;
void Enter(void* context){if(context!=(void*)42)throw 1;++calls;SetLastError(1234);__asm {pxor xmm0,xmm0} }
std::uint64_t Fake(void* context){SetLastError(4567);return context==(void*)42?0x1122334455667788ULL:0;}
int __stdcall Add(int a,int b){return a*10+b;}
int __cdecl AddC(int a,int b){return a*10+b;}
int __fastcall AddF(int a,int b,int c){return a*100+b*10+c;}
__declspec(naked) double __cdecl ReturnX87(){__asm {ret}}
#define CHECK(x) do{if(!(x)){printf("FAIL %d %s\n",__LINE__,#x);return 1;}}while(0)
int main(){static_assert(sizeof(void*)==4);
 void* original=(void*)&Add;auto raw=BuildX86RawStub((void*)42,(void*)&Enter,&original);CHECK(raw);
 auto add=(int(__stdcall*)(int,int))raw;
 for(unsigned i=0;i<10000;++i)CHECK(add(3,4)==34);
 original=(void*)&AddC;CHECK(((int(__cdecl*)(int,int))raw)(5,6)==56);
 original=(void*)&AddF;CHECK(((int(__fastcall*)(int,int,int))raw)(3,4,5)==345);
 original=(void*)&ReturnX87;double value=3.25,observed=0;__asm {fld value}
 observed=((double(__cdecl*)())raw)();CHECK(observed==value);
 auto fake=BuildX86FakeStub((void*)42,(void*)&Fake,8);CHECK(fake);
 for(unsigned i=0;i<10000;++i)CHECK(((std::uint64_t(__stdcall*)(int,int))fake)(1,2)==0x1122334455667788ULL);
 CHECK(GetLastError()==4567);
 auto cdeclFake=BuildX86FakeStub((void*)42,(void*)&Fake,0);CHECK(cdeclFake);
 CHECK(((std::uint64_t(__cdecl*)(int,int))cdeclFake)(1,2)==0x1122334455667788ULL);
 auto wide=BuildX86FakeStub((void*)42,(void*)&Fake,12);CHECK(wide);
 CHECK(((std::uint64_t(__stdcall*)(std::uint64_t,int))wide)(0xABCDEFFEDCBAULL,2)==0x1122334455667788ULL);
 CHECK(!BuildX86FakeStub((void*)42,(void*)&Fake,3));
 CHECK(calls==10003);
 printf("PASS: x86 cdecl/stdcall/fastcall Raw, x87/XMM preservation, Fake stack cleanup and EDX:EAX return\n");
 return 0;
}
'''.replace("STUBS", (root / "APIMonitor_x86/hook/EntryStubs.h").as_posix())
with tempfile.TemporaryDirectory(prefix="ksword_x86_stubs_") as temp:
    folder = Path(temp)
    cpp = folder / "fixture.cpp"
    cpp.write_text(source, encoding="utf-8")
    subprocess.run(["cl", "/nologo", "/EHsc", "/std:c++17", str(cpp),
                    "/Fe:" + str(folder / "fixture.exe"), "/Fo:" + str(folder / "fixture.obj")], check=True)
    subprocess.run([str(folder / "fixture.exe")], check=True, timeout=30)
