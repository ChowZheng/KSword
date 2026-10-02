"""Read actual exports and reject unreadable pointers and malformed image ranges."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#include <cstdio>
#include "SOURCE"
extern "C" __declspec(dllexport) DWORD FixtureExport(){return 42;}
#define CHECK(x) do{if(!(x)){printf("FAIL %d\n",__LINE__);return 1;}}while(0)
int main(){using namespace apimon;std::vector<std::string> names;
 CHECK(EnumerateNamedExports(GetModuleHandleW(L"ntdll.dll"),&names));CHECK(std::binary_search(names.begin(),names.end(),"NtQueryInformationThread"));
 auto module=GetModuleHandleW(nullptr);CHECK(EnumerateNamedExports(module,&names));CHECK(std::binary_search(names.begin(),names.end(),"FixtureExport"));
 auto* base=reinterpret_cast<BYTE*>(module);auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
 auto* exports=reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
 DWORD previous=0;LONG oldOffset=dos->e_lfanew;VirtualProtect(dos,sizeof(*dos),PAGE_READWRITE,&previous);dos->e_lfanew=0x7fffffff;
 CHECK(!EnumerateNamedExports(module,&names));dos->e_lfanew=oldOffset;DWORD ignored=0;VirtualProtect(dos,sizeof(*dos),previous,&ignored);
 IMAGE_EXPORT_DIRECTORY saved=*exports;VirtualProtect(exports,sizeof(*exports),PAGE_READWRITE,&previous);
 exports->NumberOfNames=0xffffffff;CHECK(!EnumerateNamedExports(module,&names));*exports=saved;
 exports->AddressOfNames=nt->OptionalHeader.SizeOfImage-1;CHECK(!EnumerateNamedExports(module,&names));*exports=saved;
 VirtualProtect(exports,sizeof(*exports),previous,&ignored);CHECK(EnumerateNamedExports(module,&names));
 auto* unreadable=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_NOACCESS);DWORD value=0;
 CHECK(!ReadExportMemory(unreadable,&value,sizeof(value)));CHECK(!EnumerateNamedExports(reinterpret_cast<HMODULE>(unreadable),&names));VirtualFree(unreadable,0,MEM_RELEASE);
 CHECK(!ReadExportMemory(reinterpret_cast<void*>(static_cast<std::uintptr_t>(-2)),&value,8));
 printf("PASS: real sorted exports, module pinning, malformed PE ranges/counts, inaccessible pages and address overflow\n");
}
'''.replace("SOURCE", (root / "APIMonitor_x64/hook/ExportCatalog.h").as_posix())
with tempfile.TemporaryDirectory(prefix="ksword_exports_") as temp:
    folder = Path(temp)
    cpp = folder / "fixture.cpp"
    cpp.write_text(code, encoding="utf-8")
    subprocess.run(["cl", "/nologo", "/EHsc", "/std:c++17", "/utf-8", str(cpp),
                    "/Fe:" + str(folder / "fixture.exe"), "/Fo:" + str(folder / "fixture.obj")], check=True)
    subprocess.run([str(folder / "fixture.exe")], check=True, timeout=30)
