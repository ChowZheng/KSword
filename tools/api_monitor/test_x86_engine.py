"""Execute the x86 backend against benign 32-bit machine code and patch lifecycle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = r'''
#include <cstdio>
#include "ENGINE"
using namespace apimon;
using Function=int(__cdecl*)();
static void* original=nullptr;
int Detour(){return reinterpret_cast<Function>(original)()+1;}
int Other(){return 0;}
#define CHECK(x) do{if(!(x)){printf("FAIL %d %s\n",__LINE__,#x);return 1;}}while(0)
void put(unsigned char* p,std::initializer_list<unsigned char> b){memcpy(p,b.begin(),b.size());}
void rel(unsigned char* p,unsigned char* destination,size_t n,size_t offset){uint32_t d=(uintptr_t)destination-((uintptr_t)p+n);memcpy(p+offset,&d,4);}
int main(){
 static_assert(sizeof(void*)==4);
 struct Case{std::initializer_list<unsigned char> bytes;size_t length;};
 Case cases[]={{{0x8B,0xFF},2},{{0x55},1},{{0x8B,0xEC},2},{{0x40},1},{{0x48},1},{{0xA1,0,0,0,0},5},
 {{0x64,0xA1,0x18,0,0,0},6},{{0x8B,0x05,0,0,0,0},6},{{0x67,0x8B,0},0},{{0x66,0xE8,0,0},0},
 {{0xC5,0xF8,0x77},0},{{0xF3,0x0F,0x1E,0xFB},4},{{0xFF,0xD2},2},{{0xFF,0x15,0,0,0,0},6}};
 for(auto& c:cases){auto d=DecodeInstruction(c.bytes.begin(),c.bytes.size());CHECK(d.length==c.length&&!d.ripRelative);}
 auto* memory=(unsigned char*)VirtualAlloc(nullptr,65536,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);CHECK(memory);
 std::wstring error;
 for(int index=0;index<7;++index){auto* p=memory+index*1024;memset(p,0x90,1024);int expected=42;
  switch(index){
   case 0:put(p,{0x8B,0xFF,0x55,0x8B,0xEC,0xB8,42,0,0,0,0x5D,0xC3});break;
   case 1:put(p,{0xA1,0,0,0,0,0xC3});{uint32_t address=(uintptr_t)(p+128),value=42;memcpy(p+1,&address,4);memcpy(p+128,&value,4);}break;
   case 2:put(p,{0xE8,0,0,0,0,0x83,0xC0,1,0xC3});rel(p,p+128,5,1);put(p+128,{0xB8,42,0,0,0,0xC3});expected=43;break;
   case 3:put(p,{0x31,0xC0,0x74,0,0xB8,42,0,0,0,0xC3});break;
   case 4:put(p,{0x31,0xC0,0x75,0xFC,0xB8,42,0,0,0,0xC3});break;
   case 5:put(p,{0xF3,0x0F,0x1E,0xFB,0xB8,42,0,0,0,0xC3});break;
   case 6:put(p,{0x31,0xC0,0x0F,0x84,0,0,0,0,0xB8,1,0,0,0,0xC3});rel(p+2,p+128,6,2);put(p+128,{0xB8,42,0,0,0,0xC3});break;
  }
  CHECK(((Function)p)()==expected);InlineHookRecord hook;
  auto status=InstallInlineHookAtAddress(p,(void*)&Detour,&hook,&original,&error);
  if(status!=InlineHookInstallResult::Installed){wprintf(L"case %d: %s\n",index,error.c_str());return 1;}
  CHECK(hook.patchSize>=5&&((Function)p)()==expected+1&&((Function)original)()==expected);
  if(index==5)CHECK(memcmp(p,kLandingPad,4)==0&&memcmp(original,kLandingPad,4)==0);
  CHECK(UninstallInlineHook(&hook)&&((Function)p)()==expected&&((Function)original)()==expected);
 }
 auto* p=memory+8192;put(p,{0xB8,42,0,0,0,0xC3});InlineHookRecord owner,shared,conflict;void* second=nullptr,*bad=nullptr;
 CHECK(InstallInlineHookAtAddress(p,(void*)&Detour,&owner,&original,&error)==InlineHookInstallResult::Installed);
 CHECK(InstallInlineHookAtAddress(p,(void*)&Detour,&shared,&second,&error)==InlineHookInstallResult::Installed&&shared.sharedEntry&&second==original);
 CHECK(InstallInlineHookAtAddress(p,(void*)&Other,&conflict,&bad,&error)==InlineHookInstallResult::PermanentFailure);
 CHECK(UninstallInlineHook(&owner)&&((Function)p)()==43);CHECK(UninstallInlineHook(&shared)&&((Function)p)()==42);
 auto* entry=memory+9216;put(entry,{0xFF,0x25,0,0,0,0});uint32_t slot=(uintptr_t)(entry+128),target=(uintptr_t)p;memcpy(entry+2,&slot,4);memcpy(entry+128,&target,4);
 InlineHookRecord indirect;CHECK(InstallInlineHookAtAddress(entry,(void*)&Detour,&indirect,&original,&error)==InlineHookInstallResult::Installed&&((Function)entry)()==43);CHECK(UninstallInlineHook(&indirect));
 put(entry,{0x74,1,0xB8,42,0,0,0,0xC3});InlineHookRecord middle;CHECK(InstallInlineHookAtAddress(entry,(void*)&Detour,&middle,&bad,&error)==InlineHookInstallResult::PermanentFailure&&error.find(L"middle")!=std::wstring::npos);
 put(entry,{0xEB,0xFE});InlineHookRecord cycle;CHECK(InstallInlineHookAtAddress(entry,(void*)&Detour,&cycle,&bad,&error)==InlineHookInstallResult::PermanentFailure);
 DWORD prior=0;VirtualProtect(memory+16384,4096,PAGE_NOACCESS,&prior);InlineHookRecord unreadable;CHECK(InstallInlineHookAtAddress(memory+16384,(void*)&Detour,&unreadable,&bad,&error)==InlineHookInstallResult::PermanentFailure);
 CHECK(FitsRel32(0xFFFFFFFFLL));
 printf("PASS: x86 decoder, absolute operands, CALL/Jcc/internal branches, ENDBR32, indirect exports, conflicts and retired trampoline\n");
}
'''.replace("ENGINE", (root / "APIMonitor_x86/hook/HookEngineX86.cpp").as_posix())
with tempfile.TemporaryDirectory(prefix="ksword_x86_engine_") as temp:
    directory = Path(temp)
    cpp = directory / "fixture.cpp"
    cpp.write_text(source, encoding="utf-8")
    subprocess.run(["cl", "/nologo", "/EHsc", "/std:c++17", "/utf-8", "/I" + str(root / "APIMonitor_x64"),
                    str(cpp), "/Fe:" + str(directory / "fixture.exe"), "/Fo:" + str(directory / "fixture.obj")], check=True)
    subprocess.run([str(directory / "fixture.exe")], check=True, timeout=30)
