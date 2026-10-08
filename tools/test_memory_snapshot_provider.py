"""Compile the production snapshot adapter against a small QByteArray shim.

This checks captured-range bounds and change-reference semantics without Qt or
target I/O. Real Qt/editor event tests remain in memory_editor_ui_tests.cpp.
Only the adapter's dependency include is replaced; its declarations and method
bodies are compiled unchanged. Requires G++ (or Clang++).
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "Ksword5.1/Ksword5.1/UI"
SHIM = r'''#pragma once
#include <cstdint>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
using qsizetype = std::int64_t;
class QByteArray {
    std::string bytes;
public:
    QByteArray() = default;
    explicit QByteArray(std::string value) : bytes(std::move(value)) {}
    QByteArray(qsizetype count, char value) : bytes(static_cast<std::size_t>(count), value) {}
    qsizetype size() const { return static_cast<qsizetype>(bytes.size()); }
    bool isEmpty() const { return bytes.empty(); }
    char at(qsizetype at) const { return bytes.at(static_cast<std::size_t>(at)); }
    void clear() { bytes.clear(); }
};
#include "MemoryDiffOverlay.h"
namespace ks::ui {
struct WorkbenchByteWindow {
    bool ok = false;
    std::uint64_t address = 0;
    std::vector<std::uint8_t> bytes, validMask, baselineBytes, baselineValidMask,
        previousBytes, previousValidMask;
    std::vector<ksword::memwb::ByteChangeKind> changeKinds;
};
class IWorkbenchBytesProvider {
public:
    virtual ~IWorkbenchBytesProvider() = default;
    virtual WorkbenchByteWindow FetchWindow(std::uint64_t, std::uint64_t) const = 0;
    virtual int AddressBits() const = 0;
    virtual bool HasPreviousRead() const = 0;
};
}
'''

TEST = r'''
#include "MemorySnapshotBytesProvider.h"
#include <cstdlib>
#include <iostream>
#include <limits>
unsigned checks = 0;
void require(bool value, const char* why) {
    ++checks;
    if (!value) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}
int main() {
    using ks::ui::MemorySnapshotBytesProvider;
    using Kind = ksword::memwb::ByteChangeKind;
    MemorySnapshotBytesProvider p;
    require(!p.FetchWindow(0, 1).ok, "uninitialized evidence is unavailable");
    require(p.FetchWindow(123, 0).ok, "empty requests are explicit empty success");
    constexpr std::uint64_t base = 0x1000;
    std::string baseline(257, '\0'), current(257, '\0'), previous(257, '\0');
    for (std::size_t i = 0; i < current.size(); ++i) {
        baseline[i] = static_cast<char>(i);
        current[i] = i % 3 == 0 ? static_cast<char>(i + 1) : baseline[i];
        previous[i] = i % 5 == 0 ? static_cast<char>(i + 2) : baseline[i];
    }
    p.setSnapshot(base, QByteArray(current), QByteArray(baseline), QByteArray(previous), 32, true);
    require(p.AddressBits() == 32 && p.HasPreviousRead(), "identity metadata available");
    for (std::uint64_t start = 0; start < current.size(); ++start) {
        for (const std::uint64_t length : {1ULL, 7ULL, 64ULL, 65536ULL}) {
            const auto w = p.FetchWindow(base + start, length);
            const auto count = std::min<std::uint64_t>(length, current.size() - start);
            require(w.ok && w.address == base + start && w.bytes.size() == count, "requested range clips at evidence tail");
            require(w.validMask == std::vector<std::uint8_t>(count, 1), "captured bytes have full validity");
            for (std::size_t i = 0; i < w.bytes.size(); ++i) {
                const auto at = start + i;
                require(w.bytes[i] == static_cast<std::uint8_t>(current[at]), "current byte maps exactly");
                require(w.baselineBytes[i] == static_cast<std::uint8_t>(baseline[at])
                    && w.previousBytes[i] == static_cast<std::uint8_t>(previous[at]), "references share the same offset");
                const auto kind = current[at] != baseline[at] ? Kind::Pending
                    : baseline[at] != previous[at] ? Kind::ExternalChange : Kind::Unchanged;
                require(w.changeKinds[i] == kind, "pending and external changes retain priority");
            }
        }
    }
    require(!p.FetchWindow(base - 1, 2).ok, "prefix outside evidence is unavailable");
    require(!p.FetchWindow(base + current.size(), 1).ok, "first byte after evidence is unavailable");
    p.setSnapshot(base, QByteArray(current), QByteArray(baseline), QByteArray(previous), 64, false);
    for (const auto kind : p.FetchWindow(base, current.size()).changeKinds)
        require(kind == Kind::Unchanged, "suppressing highlights leaves content untouched");
    p.setSnapshot(base, QByteArray(current), QByteArray(baseline), {}, 64, true);
    require(!p.HasPreviousRead(), "missing previous read never appears available");
    require(p.FetchWindow(base, 3).previousValidMask == std::vector<std::uint8_t>(3, 0), "missing reference validity is zero");
    p.setSnapshot(base, QByteArray(current), {}, QByteArray(previous), 48, true);
    require(p.AddressBits() == 64, "invalid architecture metadata normalizes to 64 bits");
    require(p.FetchWindow(base, 1).changeKinds[0] == Kind::Unchanged, "no baseline means no fabricated difference");
    p.setSnapshot(base, QByteArray(2 * 1024 * 1024, 'a'), {}, {}, 64, false);
    require(p.FetchWindow(base, std::numeric_limits<std::uint64_t>::max()).bytes.size() == 1024 * 1024,
        "oversized requests have a bounded allocation");
    const auto max = std::numeric_limits<std::uint64_t>::max();
    p.setSnapshot(max - 1, QByteArray(std::string("ab")), {}, {}, 64, true);
    require(p.FetchWindow(max, 1).ok && p.FetchWindow(max, 1).bytes[0] == 'b', "inclusive address-space tail does not wrap");
    p.setSnapshot(max, QByteArray(std::string("ab")), {}, {}, 64, true);
    require(!p.FetchWindow(max, 1).ok, "overflowing snapshots clear old evidence");
    p.clear();
    require(!p.HasPreviousRead() && !p.FetchWindow(base, 1).ok, "clear releases references and evidence");
    std::cout << "PASS: " << checks << " production snapshot provider checks (Qt shim)\n";
}
'''


def main():
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise SystemExit("G++ or Clang++ is required.")
    build_root = ROOT / ".codex-tmp/snapshot-provider"
    build_root.mkdir(parents=True, exist_ok=True)
    if not build_root.resolve().is_relative_to(ROOT.resolve()):
        raise SystemExit("Snapshot test output must remain inside the repository.")
    with tempfile.TemporaryDirectory(prefix="run-", dir=build_root) as temporary:
        build = Path(temporary)
        (build / "SnapshotShim.h").write_text(SHIM, encoding="utf-8")
        header = (UI / "MemorySnapshotBytesProvider.h").read_text(encoding="utf-8-sig")
        header = header.replace('#include "MemoryWorkbench/WorkbenchDisasmView.h"', '#include "SnapshotShim.h"')
        (build / "MemorySnapshotBytesProvider.h").write_text(header, encoding="utf-8")
        (build / "MemorySnapshotBytesProvider.cpp").write_text(
            (UI / "MemorySnapshotBytesProvider.cpp").read_text(encoding="utf-8-sig"), encoding="utf-8")
        (build / "test.cpp").write_text(TEST, encoding="utf-8")
        exe = build / "snapshot-provider-tests.exe"
        subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O2",
            "-I", str(ROOT / "shared/evidence/memory_workbench"),
            str(build / "MemorySnapshotBytesProvider.cpp"), str(build / "test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
