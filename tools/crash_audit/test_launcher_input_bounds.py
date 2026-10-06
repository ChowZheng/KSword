#!/usr/bin/env python3
"""Exercise production Launcher PE/JSON parsing without reading system images.

Uses the compiler's Windows PE definitions, synthetic in-memory files, and the
complete Json.cpp. --prove-regression also rejects the corresponding old code.
This is a parser regression, not a full MSVC Launcher build.
"""

from pathlib import Path
import argparse
import shutil
import subprocess
import uuid


ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "Launcher"


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


HARNESS = r'''
#include "Launcher.h"
#include <cassert>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
namespace launcher {
static std::vector<BYTE> input;
bool ReadFileBytes(const std::wstring&, std::vector<BYTE>* bytes) { *bytes = input; return true; }
std::string UpperAscii(std::string value) {
    for (char& ch : value) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return value;
}
/*PRODUCTION*/
}
using namespace launcher;
static unsigned checks;
static void check(bool value) { if (!value) std::abort(); ++checks; }
static IMAGE_NT_HEADERS64* headers() { return reinterpret_cast<IMAGE_NT_HEADERS64*>(input.data() + 64); }
static IMAGE_SECTION_HEADER* section() { return IMAGE_FIRST_SECTION(headers()); }
static IMAGE_DATA_DIRECTORY& directory() { return headers()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]; }
static IMAGE_DEBUG_DIRECTORY* entry() { return reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(input.data() + 512); }
static void reset() {
    input.assign(1024, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(input.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 64;
    auto* nt = headers();
    nt->Signature = IMAGE_NT_SIGNATURE; nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 1; nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->FileHeader.TimeDateStamp = 1234; nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = 0x2000; nt->OptionalHeader.SizeOfHeaders = 512;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    section()->VirtualAddress = 0x1000; section()->Misc.VirtualSize = 512;
    section()->PointerToRawData = 512; section()->SizeOfRawData = 512;
    directory().VirtualAddress = 0x1000; directory().Size = sizeof(IMAGE_DEBUG_DIRECTORY);
    entry()->Type = IMAGE_DEBUG_TYPE_CODEVIEW; entry()->PointerToRawData = 640; entry()->SizeOfData = 40;
    auto* record = reinterpret_cast<RsdsRecord*>(input.data() + 640);
    record->signature = 'SDSR'; record->guid.Data1 = 0x12345678; record->age = 3;
    std::memcpy(record->path, "ntoskrnl.pdb", 12);
}
static bool probe() { PeIdentity identity; return ProbePeIdentity(L"synthetic.sys", &identity); }
static void regression(const std::string& name) {
    reset();
    if (name == "overflow_debug") { directory().Size = 0xFFFFFFF8U; check(!probe()); }
    else if (name == "overflow_rsds") { entry()->PointerToRawData = 0xFFFFFFF0U; entry()->SizeOfData = 48; check(!probe()); }
    else if (name == "section_table") { headers()->FileHeader.NumberOfSections = 0xFFFF; directory().VirtualAddress = 0x9000; check(!probe()); }
    else if (name == "deep_json") {
        JsonValue value; std::string error;
        check(!ParseJson(std::string(100000, '[') + "0" + std::string(100000, ']'), &value, &error));
    } else std::abort();
}
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc == 3 && std::string(argv[1]) == "manifest") {
        std::ifstream file(argv[2], std::ios::binary);
        std::ostringstream text; text << file.rdbuf();
        JsonValue value; std::string error;
        check(file.good() && ParseJson(text.str(), &value, &error) && value.isObject());
        check(value.get("profiles") && value.get("profiles")->isArray() && !value.get("profiles")->array().empty());
        std::cout << "Launcher current support manifest: passed\n";
        return 0;
    }
    if (argc == 2) { regression(argv[1]); return 0; }
    static_assert(sizeof(IMAGE_DOS_HEADER) == 64 && sizeof(IMAGE_OPTIONAL_HEADER64) == 240 && sizeof(IMAGE_SECTION_HEADER) == 40);
    reset(); PeIdentity identity;
    check(ProbePeIdentity(L"synthetic.sys", &identity));
    check(identity.valid && identity.machine == IMAGE_FILE_MACHINE_AMD64 && identity.pdbName == "ntoskrnl.pdb" && identity.pdbAge == 3);
    check(identity.pdbGuid == "12345678000000000000000000000000");
    check(!ProbePeIdentity(L"synthetic.sys", nullptr));
    const auto valid = input;
    for (size_t length = 0; length < 680; ++length) {
        input.assign(valid.begin(), valid.begin() + length); check(!probe());
    }
    reset(); headers()->OptionalHeader.NumberOfRvaAndSizes = 0; check(!probe());
    reset(); headers()->FileHeader.NumberOfSections = 0xFFFF; check(!probe());
    reset(); headers()->FileHeader.SizeOfOptionalHeader = 0xFFFF; check(!probe());
    reset(); reinterpret_cast<IMAGE_DOS_HEADER*>(input.data())->e_lfanew = 0x7FFFFFFF; check(!probe());
    reset(); reinterpret_cast<IMAGE_DOS_HEADER*>(input.data())->e_lfanew = -1; check(!probe());
    reset(); section()->SizeOfRawData = 8; check(!probe());
    reset(); section()->PointerToRawData = 0xFFFFFFF0U; check(!probe());
    reset(); directory().VirtualAddress += 500; check(!probe());
    reset(); directory().VirtualAddress = 700; check(!probe());
    reset(); entry()->SizeOfData = 23; check(!probe());
    reset(); entry()->SizeOfData = 24; check(!probe());
    reset(); entry()->PointerToRawData = 1000; entry()->SizeOfData = 40; check(!probe());
    reset(); std::memcpy(input.data() + 984, input.data() + 640, 40);
    entry()->PointerToRawData = 984; check(probe());
    reset(); std::memcpy(input.data() + 400, input.data() + 512, sizeof(IMAGE_DEBUG_DIRECTORY));
    directory().VirtualAddress = 400; check(probe());
    reset(); entry()->Type = 0; check(!probe());
    for (const char* name : {"overflow_debug", "overflow_rsds", "section_table", "deep_json"}) regression(name);
    JsonValue value; std::string error;
    for (size_t depth : {size_t(0), size_t(1), size_t(32), size_t(64)}) {
        check(ParseJson(std::string(depth, '[') + "0" + std::string(depth, ']'), &value, &error));
        value = JsonValue();
    }
    for (size_t depth : {size_t(65), size_t(128), size_t(100000)}) {
        check(!ParseJson(std::string(depth, '[') + "0" + std::string(depth, ']'), &value, &error));
        check(error.find("nesting limit") != std::string::npos);
    }
    std::string nested;
    for (int depth = 0; depth < 65; ++depth) nested += "{\"a\":";
    check(!ParseJson(nested + "0" + std::string(65, '}'), &value, &error));
    check(!ParseJson("[", &value, &error));
    check(ParseJson("{\"profiles\":[{\"machine\":34404,\"complete\":true}],\"modules\":[]}", &value, &error));
    check(value.get("profiles")->array().size() == 1);
    std::cout << "Launcher input bounds: " << checks << " checks passed\n";
}
'''


def build(compiler: str, work: Path, compatibility: str, json: str, name: str) -> Path:
    production = "\n".join([
        between(compatibility, "#pragma pack(push, 1)", "struct SystemModuleEntry"),
        between(compatibility, "std::wstring Basename(", "int ModuleClassForName("),
        between(compatibility, "bool ProbePeIdentity(", "bool QueryKernelModules("),
    ])
    source = work / f"{name}.cpp"
    source.write_text(HARNESS.replace("/*PRODUCTION*/", production), encoding="utf-8")
    json_file = work / f"{name}_json.cpp"
    json_file.write_text(json, encoding="utf-8")
    executable = work / f"{name}.exe"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-multichar",
                    "-I", str(LAUNCHER), str(source), str(json_file), "-o", str(executable)], check=True, cwd=ROOT)
    return executable


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--prove-regression", action="store_true")
    parser.add_argument("--baseline", default="b2154a0d")
    args = parser.parse_args()
    compiler = shutil.which(args.compiler)
    if not compiler:
        raise SystemExit(f"Compiler unavailable: {args.compiler}")
    scratch = ROOT / ".codex-tmp"
    work = scratch / f"launcher-input-{uuid.uuid4().hex}"
    work.mkdir(parents=True)
    try:
        executable = build(compiler, work, (LAUNCHER / "Compatibility.cpp").read_text(encoding="utf-8-sig"),
                           (LAUNCHER / "Json.cpp").read_text(encoding="utf-8-sig"), "fixed")
        subprocess.run([str(executable)], check=True, cwd=ROOT, timeout=30)
        subprocess.run([str(executable), "manifest", "Launcher/launcher_support_manifest.json"], check=True, cwd=ROOT, timeout=30)
        if args.prove_regression:
            old = [subprocess.check_output(["git", "show", f"{args.baseline}:Launcher/{file}"], cwd=ROOT).decode("utf-8-sig")
                   for file in ("Compatibility.cpp", "Json.cpp")]
            executable = build(compiler, work, *old, "old")
            for case in ("overflow_debug", "overflow_rsds", "deep_json"):
                try:
                    result = subprocess.run([str(executable), case], cwd=ROOT, timeout=20, capture_output=True)
                except subprocess.TimeoutExpired as error:
                    raise AssertionError(f"Old parser hung instead of completing: {case}") from error
                if result.returncode == 0:
                    raise AssertionError(f"Old parser unexpectedly passed: {case}")
                print(f"Old parser rejected by regression: {case} (exit={result.returncode})")
    finally:
        if not work.resolve().is_relative_to(scratch.resolve()):
            raise RuntimeError("Scratch path escaped workspace")
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
