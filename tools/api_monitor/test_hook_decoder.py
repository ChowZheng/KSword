"""Compile the production conservative decoder and check instruction boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "APIMonitor_x64/hook/HookEngine.cpp").read_text(encoding="utf-8-sig")
decoder = source[source.index("        std::size_t ModRmLength("):source.index("        // BuildAbsoluteJump")]
start = source.index("        std::size_t CalculatePatchSize(")
patch = source[start:source.index("\n    }\n", start)]
main = r'''
int main() {
 struct Case { const char* name; std::initializer_list<unsigned char> bytes; size_t length; };
 Case cases[] = {
  {"byte immediate", {0x80,0xF9,0x90},3},
  {"truncated byte immediate", {0x80,0xF9},0},
  {"16-bit group immediate", {0x66,0x81,0xC0,1,0},5},
  {"16-bit mov immediate", {0x66,0xC7,0xC0,1,0},5},
  {"REX.W overrides 66", {0x66,0x48,0x81,0xC0,1,0,0,0},8},
  {"16-bit push", {0x66,0x68,1,0},4},
  {"address override rejected", {0x67,0x8B,0x05,1,0,0,0},0},
  {"return rejected", {0xC3},0}, {"padding rejected", {0xCC},0},
  {"RIP relative rejected", {0x48,0x8B,0x05,1,0,0,0},0},
  {"relative branch rejected", {0xE8,0,0,0,0},0},
  {"syscall test", {0xF6,0x04,0x25,8,3,0xFE,0x7F,1},8}
 };
 for(const auto& c: cases) {
  auto actual=DecodeInstructionLength(c.bytes.begin(),c.bytes.size());
  if(actual!=c.length) { printf("FAIL %s: %zu != %zu\n",c.name,actual,c.length); return 1; }
 }
 unsigned char code[32]; memset(code,0x90,sizeof(code)); code[12]=0x80;code[13]=0xF9;
 if(CalculatePatchSize(code)!=15) return 2;
 printf("PASS: 12 decoder cases and complete patch boundary\n");
}
'''
with tempfile.TemporaryDirectory(prefix="ksword_decoder_") as temp:
    folder = Path(temp)
    cpp = folder / "decoder.cpp"
    cpp.write_text("#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n#include <cstring>\n#include <initializer_list>\nconstexpr size_t kAbsoluteJumpSize=14;\n" + decoder + patch + main, encoding="utf-8")
    subprocess.run(["cl", "/nologo", "/EHsc", "/std:c++17", "/utf-8", str(cpp), "/Fe:" + str(folder / "decoder.exe"), "/Fo:" + str(folder / "decoder.obj")], check=True)
    subprocess.run([str(folder / "decoder.exe")], check=True)
