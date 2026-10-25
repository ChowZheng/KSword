#!/usr/bin/env python3
"""Test the production GPT-name decoder with Windows-width WCHAR and real QtCore.

The helper is extracted unchanged from DiskEditorBackend.cpp. -fshort-wchar
selects the same 16-bit QString::fromWCharArray inline path as Windows; no
wstring/wcslen ABI calls or native disk operations are used. This is a text
boundary regression, not a Windows GUI or storage-device test.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile


def production_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise ValueError("unterminated production GPT name helper")


FIXTURE = r'''
#include <QString>
#include <QChar>
#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include "production_partition_name.inc"

static_assert(sizeof(wchar_t) == 2, "The fixture must use Windows WCHAR width");
int checks = 0;
void check(bool condition, const char* scenario)
{
    ++checks;
    if (!condition) throw std::runtime_error(scenario);
}
struct Field
{
    wchar_t before = 0x1248;
    wchar_t name[36]{};
    wchar_t after = 0x2490;
};
Field field(const std::u16string& text)
{
    if (text.size() > 36) throw std::runtime_error("fixture name exceeds GPT field");
    Field result;
    std::copy(text.begin(), text.end(), std::begin(result.name));
    return result;
}
void expect(const Field& input, const QString& expected, const char* scenario)
{
    const Field original = input;
    const QString decoded = gptPartitionName(input.name);
    check(decoded == expected, scenario);
    check(!decoded.contains(QChar(u'\0')), "NUL padding must not reach map/table text");
    check(std::equal(std::begin(input.name), std::end(input.name), std::begin(original.name))
        && input.before == original.before && input.after == original.after,
        "decoding must leave the fixed input and adjacent storage unchanged");
}
int main()
{
    const auto basic = field(u"Basic data partition");
    const auto efi = field(u"EFI system partition");
    // Demonstrate the original failure independently of the corrected helper:
    // trimmed() leaves the zero-filled suffix in the QString.
    const QString oldName = QString::fromWCharArray(basic.name, 36).trimmed();
    check(oldName.size() == 36 && oldName.contains(QChar(u'\0')),
        "the previous fixed-length conversion exposes GPT padding");
    expect(basic, QStringLiteral("Basic data partition"), "basic partition label");
    expect(efi, QStringLiteral("EFI system partition"), "EFI partition label");
    expect(field(u"\u6570\u636E\u5206\u533A"), QString::fromUtf16(u"\u6570\u636E\u5206\u533A"),
        "Chinese partition label stays Unicode");
    expect(field(u"\u6570\u636E"), QString::fromUtf16(u"\u6570\u636E"),
        "Unicode bytes must not be guessed as ASCII");
    expect(field(u"A\U0001F4BE\u5206\u533A"), QString::fromUtf16(u"A\U0001F4BE\u5206\u533A"),
        "valid supplementary characters and surrogate pairs");
    expect(field(u""), QString(), "empty field allows existing caller fallback");
    expect(field(std::u16string(36, u'X')), QString(36, QChar(u'X')),
        "full field without a terminator is bounded and preserved");
    expect(field(std::u16string(35, u'Y')), QString(35, QChar(u'Y')),
        "terminator in the last field unit");
    auto garbage = field(u"EFI");
    garbage.name[4] = u'X'; garbage.name[35] = 0xD800;
    expect(garbage, QStringLiteral("EFI"), "ignore all bytes after the first terminator");
    auto emptyGarbage = field(u"");
    emptyGarbage.name[1] = u'Q'; emptyGarbage.name[35] = 0xDC00;
    expect(emptyGarbage, QString(), "leading terminator ignores later garbage");
    expect(field(u"  Data  Partition  "), QStringLiteral("Data  Partition"),
        "trim outer whitespace while preserving interior spacing");
    expect(field(u"  \u6570\u636E \u5206\u533A\t "), QString::fromUtf16(u"\u6570\u636E \u5206\u533A"),
        "existing whitespace trimming semantics remain");
    std::cout << "GPT_PARTITION_NAME_CHECKS=" << checks
        << " WCHAR_BYTES=" << sizeof(wchar_t) << " GUI=0 NATIVE_IO=0\n";
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-include", type=Path)
    parser.add_argument("--compiler", default="g++")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / "Ksword5.1/Ksword5.1/MiscDock/DiskEditor/DiskEditorBackend.cpp").read_text(encoding="utf-8")
    helper = production_function(source, "QString gptPartitionName(")
    if "gptPartitionName(source.Gpt.Name)" not in source:
        raise SystemExit("The production GPT-layout path is not using the tested decoder")
    includes = [options.qt_include] if options.qt_include else [
        Path("/usr/include/x86_64-linux-gnu/qt6"), Path("/usr/include/qt6")]
    qt_include = next((p for p in includes if p and (p / "QtCore/QString").exists()), None)
    if not qt_include:
        parser.error("Qt6 development headers unavailable; pass --qt-include")
    print("PRODUCTION_HELPER_SHA256=" + hashlib.sha256(helper.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="ksword-gpt-name-") as directory:
        work = Path(directory)
        (work / "production_partition_name.inc").write_text(helper, encoding="utf-8")
        (work / "test.cpp").write_text(FIXTURE, encoding="utf-8")
        executable = work / "gpt_partition_name_test"
        subprocess.run([
            options.compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
            "-fPIC", "-fshort-wchar", "-I" + str(qt_include), "-I" + str(qt_include / "QtCore"),
            "-I" + str(work), str(work / "test.cpp"), "-l:libQt6Core.so.6", "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
