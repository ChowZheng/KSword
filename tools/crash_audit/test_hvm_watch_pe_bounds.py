#!/usr/bin/env python3
"""Compile the production disk export parser with Qt I/O shims and real PE types."""
from pathlib import Path
import argparse
import re
import shutil
import subprocess

from test_registry_search_lifetime import balanced_end

ROOT = Path(__file__).resolve().parents[2]
RELATIVE = "Ksword5.1/Ksword5.1/UI/HvmWatch.cpp"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-revision", help="Read a Git revision for a negative baseline; the checkout is unchanged")
    args = parser.parse_args()
    if args.source_revision:
        source = subprocess.run(["git", "show", f"{args.source_revision}:{RELATIVE}"], cwd=ROOT,
            check=True, capture_output=True, text=True, encoding="utf-8").stdout
    else:
        source = (ROOT / RELATIVE).read_text(encoding="utf-8-sig")
    begin = source.index("        ExportTable loadExportTable(")
    opening = source.index("{", begin)
    production = source[begin:balanced_end(source, opening, "{", "}")]
    harness = r'''
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using qsizetype = std::intptr_t;
using quint64 = std::uint64_t;
using quint32 = std::uint32_t;
using quint16 = std::uint16_t;
struct QString {
    std::string value;
    QString(const char* text = "") : value(text) {}
    bool isEmpty() const { return value.empty(); }
    static QString fromLatin1(const char* text, int size) {
        QString result;
        result.value.assign(text, static_cast<std::size_t>(size));
        return result;
    }
};
template<class A, class B> using QPair = std::pair<A, B>;
template<class T> struct QVector : std::vector<T> {
    void append(const T& value) { this->push_back(value); }
    bool isEmpty() const { return this->empty(); }
};
std::vector<unsigned char> fixture;
struct QByteArray {
    std::vector<unsigned char> bytes;
    const char* constData() const { return reinterpret_cast<const char*>(bytes.data()); }
    qsizetype size() const { return static_cast<qsizetype>(bytes.size()); }
};
namespace QIODevice { constexpr int ReadOnly = 0; }
struct QFile {
    explicit QFile(const QString&) {}
    bool open(int) { return true; }
    QByteArray readAll() { return QByteArray{fixture}; }
    void close() {}
};
QString toWin32Path(const QString& input) { return input; }
struct ExportTable {
    bool valid = false;
    QVector<QPair<quint32, QString>> entries;
};
__PRODUCTION__

int checks = 0;
void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
    ++checks;
}
template<class T> void put(std::size_t offset, const T& value) {
    if (offset > fixture.size() || sizeof(T) > fixture.size() - offset)
        throw std::runtime_error("fixture write range");
    std::memcpy(fixture.data() + offset, &value, sizeof(value));
}
constexpr std::size_t ntOffset = 64;
constexpr std::size_t sectionOffset = ntOffset + sizeof(IMAGE_NT_HEADERS64);
constexpr std::size_t exportOffset = 512;
IMAGE_NT_HEADERS64 makeNt() {
    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.FileHeader.NumberOfSections = 1;
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {0x1000, 0x80};
    return nt;
}
IMAGE_SECTION_HEADER makeSection() {
    IMAGE_SECTION_HEADER section{};
    section.VirtualAddress = 0x1000;
    section.SizeOfRawData = 1024;
    section.PointerToRawData = 512;
    return section;
}
IMAGE_EXPORT_DIRECTORY makeExports() {
    IMAGE_EXPORT_DIRECTORY exports{};
    exports.NumberOfFunctions = 2;
    exports.NumberOfNames = 2;
    exports.AddressOfNames = 0x1080;
    exports.AddressOfNameOrdinals = 0x1090;
    exports.AddressOfFunctions = 0x10A0;
    return exports;
}
void validFixture() {
    fixture.assign(1536, 0);
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = ntOffset;
    put(0, dos);
    put(ntOffset, makeNt());
    put(sectionOffset, makeSection());
    put(exportOffset, makeExports());
    const quint32 names[]{0x10B0, 0x10C0};
    const quint16 ordinals[]{0, 1};
    const quint32 functions[]{0x2100, 0x2000};
    std::memcpy(fixture.data() + 640, names, sizeof(names));
    std::memcpy(fixture.data() + 656, ordinals, sizeof(ordinals));
    std::memcpy(fixture.data() + 672, functions, sizeof(functions));
    std::memcpy(fixture.data() + 688, "Zulu", 5);
    std::memcpy(fixture.data() + 704, "Alpha", 6);
}
ExportTable parse() { return loadExportTable(QString("fixture.sys")); }
void rejected(const char* reason) { check(!parse().valid, reason); }
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); // Negative baselines must not open a Windows fault dialog.
    validFixture();
    auto result = parse();
    check(result.valid && result.entries.size() == 2, "valid x64 exports rejected");
    check(result.entries[0].first == 0x2000 && result.entries[0].second.value == "Alpha", "RVA sort changed");
    check(result.entries[1].second.value == "Zulu", "valid symbol text changed");

    // The old 32-bit e_lfanew + header comparison wrapped and dereferenced
    // base + 0x7FFFFFF0 even though only a DOS header existed.
    validFixture();
    auto dos = IMAGE_DOS_HEADER{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x7FFFFFF0;
    put(0, dos);
    rejected("signed NT offset overflow accepted");
    validFixture();
    fixture.resize(sectionOffset + sizeof(IMAGE_SECTION_HEADER) - 1);
    rejected("truncated section header accepted");
    validFixture();
    auto nt = makeNt(); nt.FileHeader.NumberOfSections = 0xFFFF;
    put(ntOffset, nt);
    rejected("huge section table accepted");
    validFixture();
    nt = makeNt(); nt.FileHeader.SizeOfOptionalHeader = 16;
    put(ntOffset, nt);
    rejected("short optional header accepted");
    validFixture();
    nt = makeNt(); nt.OptionalHeader.NumberOfRvaAndSizes = 0;
    put(ntOffset, nt);
    rejected("missing data directory accepted");

    for (int kind = 0; kind < 3; ++kind) {
        validFixture();
        auto exports = makeExports();
        if (kind == 0) exports.AddressOfNames = 0x13FC;
        if (kind == 1) exports.AddressOfNameOrdinals = 0x13FF;
        if (kind == 2) exports.AddressOfFunctions = 0x13FC;
        put(exportOffset, exports);
        rejected("truncated export array accepted");
    }
    validFixture();
    auto exports = makeExports(); exports.NumberOfNames = 0xFFFFFFFF;
    put(exportOffset, exports);
    rejected("huge names count reserved memory");
    validFixture();
    exports = makeExports(); exports.NumberOfFunctions = 0xFFFFFFFF;
    put(exportOffset, exports);
    rejected("huge functions array accepted");
    validFixture();
    auto section = makeSection(); section.PointerToRawData = 0xFFFFFFF0;
    put(sectionOffset, section);
    rejected("raw file offset overflow accepted");

    validFixture();
    exports = makeExports(); exports.NumberOfNames = 1;
    put(exportOffset, exports);
    const quint32 tailName = 0x13FC;
    put(640, tailName);
    std::memcpy(fixture.data() + 1532, "ABCD", 4);
    rejected("unterminated name at EOF accepted");
    validFixture();
    const quint16 invalidOrdinal = 2;
    put(656, invalidOrdinal); put(658, invalidOrdinal);
    rejected("invalid function ordinal accepted");
    validFixture();
    exports = makeExports(); exports.NumberOfNames = 0;
    put(exportOffset, exports);
    rejected("empty export names became valid table");
    for (std::size_t cut = 0; cut < 710; ++cut) {
        validFixture(); fixture.resize(cut); (void)parse();
        ++checks; // every byte truncation through the headers and export arrays must remain safe
    }
    std::cout << "HVM_WATCH_PE_BOUNDARY_CHECKS=" << checks << "\n";
}
'''.replace("__PRODUCTION__", production)
    compiler = shutil.which("g++")
    if compiler is None: raise SystemExit("g++ unavailable")
    work = ROOT / ".codex-tmp/crash-audit/hvm-watch-pe"
    work.mkdir(parents=True, exist_ok=True)
    cpp, executable = work / "hvm_watch_pe.cpp", work / "hvm_watch_pe.exe"
    cpp.write_text(harness, encoding="utf-8")
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-static",
        str(cpp), "-o", str(executable)], check=True, timeout=60)
    result = subprocess.run([str(executable)], timeout=15, capture_output=True, text=True)
    if result.returncode:
        print(f"HVM_WATCH_PE_EXIT={result.returncode}")
        if result.stderr: print(result.stderr.strip())
        raise SystemExit(1)
    print(result.stdout.strip())


if __name__ == "__main__": main()
