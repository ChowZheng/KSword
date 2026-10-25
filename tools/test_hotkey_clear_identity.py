#!/usr/bin/env python3
"""Compile production hotkey-clear identity gates with value-only C++ stubs.

This harness extracts three unchanged function bodies from HotkeyClearController.cpp.
It never compiles or calls ClearHotkey, ClearRegisteredHotkey, WinAPI, or a driver.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER = ROOT / "Ksword5.1/Ksword5.1/ProcessDock/HotkeyClearController.cpp"
PROTOCOL = ROOT / "shared/driver/KswordArkKeyboardIoctl.h"


def extract_function(source: str, name: str) -> str:
    match = re.search(rf"(?m)^[ \t]*bool[ \t]+{re.escape(name)}[ \t]*\(", source)
    if match is None:
        raise ValueError(f"Production function unavailable: {name}")
    opening = source.find("{", match.end())
    if opening < 0:
        raise ValueError(f"Production function has no body: {name}")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():index + 1]
    raise ValueError(f"Unterminated production function: {name}")


def protocol_constants(source: str) -> str:
    names = (
        "KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION",
        "KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK",
        "KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL",
        "KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED",
        "KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_HOTKEY_TABLE",
    )
    result: list[str] = []
    for name in names:
        match = re.search(rf"(?m)^#define[ \t]+{name}[ \t]+([^\r\n]+)$", source)
        if match is None:
            raise ValueError(f"Protocol constant unavailable: {name}")
        result.append(f"#define {name} {match.group(1).strip()}")
    return "\n".join(result)


HARNESS = r"""
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

class QString {
public:
    std::string text;
    QString() = default;
    explicit QString(const char* value) : text(value ? value : "") {}
    explicit QString(std::string value) : text(std::move(value)) {}
    static QString fromLatin1(const char* value) { return QString(value); }
    QString trimmed() const {
        std::size_t first = 0, last = text.size();
        while (first < last && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
        while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
        return QString(text.substr(first, last - first));
    }
    bool isEmpty() const { return text.empty(); }
    void clear() { text.clear(); }
};
class QLatin1String {
public:
    explicit QLatin1String(const char* value) : text(value) {}
    const char* text;
};

/*PROTOCOL*/

namespace ks::process {
enum class HotkeySourceKind { Unknown, WindowActivation, ShellLink, MenuShortcut, PeAccelerator };
struct HotkeyFileIdentity {
    bool valid = false;
    std::uint32_t volumeSerialNumber = 0;
    std::uint64_t fileIndex = 0, fileSize = 0, lastWriteTime100ns = 0;
};
struct UserModeHotkeyRecord {
    HotkeySourceKind sourceKind = HotkeySourceKind::Unknown;
    std::uint32_t processId = 0, threadId = 0;
    std::uint64_t processCreationTime100ns = 0, windowHandle = 0;
    std::uint16_t expectedWindowHotkeyValue = 0, expectedShortcutHotkeyValue = 0;
    QString objectText, shortcutTargetPath;
    HotkeyFileIdentity shortcutFileIdentity;
};
struct RegisteredHotkeyClearTarget {
    std::uint64_t hotkeyObject = 0, sessionGlobals = 0, expectedThreadInfo = 0;
    std::uint64_t expectedWindowHandle = 0, expectedSnapshotHash = 0;
    std::uint32_t processId = 0, threadId = 0, hotkeyId = 0, modifiers = 0, virtualKey = 0;
};
/*CAN_CLEAR*/
}

namespace ksword::ark {
struct IoResult { bool ok = false; };
struct KeyboardHotkeyEntry {
    std::uint32_t source = 0, processId = 0, threadId = 0, hotkeyId = 0;
    std::uint32_t modifiers = 0, virtualKey = 0;
    std::uint64_t hotkeyObject = 0, sessionGlobals = 0, threadInfo = 0;
    std::uint64_t windowHandle = 0, snapshotHash = 0;
};
struct KeyboardHotkeyEnumResult {
    IoResult io;
    std::uint32_t version = 0, status = 0, returnedCount = 0, totalCount = 0;
    std::vector<KeyboardHotkeyEntry> entries;
};
}
/*COMPLETE_SNAPSHOT*/
/*MATCH_TARGET*/

static int cases = 0, failures = 0;
static void expect(bool actual, const char* name) {
    ++cases;
    if (!actual) { ++failures; std::printf("FAIL %s\n", name); }
}

static ks::process::UserModeHotkeyRecord validWindow() {
    ks::process::UserModeHotkeyRecord row;
    row.sourceKind = ks::process::HotkeySourceKind::WindowActivation;
    row.processId = 101; row.processCreationTime100ns = 202;
    row.windowHandle = 303; row.threadId = 404; row.expectedWindowHotkeyValue = 0x0141;
    return row;
}
static ks::process::UserModeHotkeyRecord validShortcut() {
    auto row = validWindow();
    row.sourceKind = ks::process::HotkeySourceKind::ShellLink;
    row.objectText = QString("C:/test/shortcut.lnk");
    row.shortcutTargetPath = QString("C:/test/app.exe");
    row.expectedShortcutHotkeyValue = 0x0242;
    row.shortcutFileIdentity = {true, 55, 66, 77, 88};
    return row;
}
static ks::process::RegisteredHotkeyClearTarget validRegistered() {
    ks::process::RegisteredHotkeyClearTarget target;
    target.hotkeyObject = 101; target.processId = 202; target.threadId = 303;
    target.hotkeyId = 404; target.modifiers = 2; target.virtualKey = 65;
    target.sessionGlobals = 505; target.expectedThreadInfo = 606;
    target.expectedWindowHandle = 707; target.expectedSnapshotHash = 808;
    return target;
}
static ksword::ark::KeyboardHotkeyEntry matchingEntry(
    const ks::process::RegisteredHotkeyClearTarget& target) {
    ksword::ark::KeyboardHotkeyEntry entry;
    entry.source = KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_HOTKEY_TABLE;
    entry.hotkeyObject = target.hotkeyObject; entry.processId = target.processId;
    entry.threadId = target.threadId; entry.hotkeyId = target.hotkeyId;
    entry.modifiers = target.modifiers; entry.virtualKey = target.virtualKey;
    entry.sessionGlobals = target.sessionGlobals; entry.threadInfo = target.expectedThreadInfo;
    entry.windowHandle = target.expectedWindowHandle; entry.snapshotHash = target.expectedSnapshotHash;
    return entry;
}

int main() {
    using namespace ks::process;
    QString reason;
    auto window = validWindow();
    expect(CanClearHotkey(window, &reason), "window valid");
    auto complete = validShortcut();
    expect(CanClearHotkey(complete, &reason), "shortcut valid");
    for (auto kind : {HotkeySourceKind::MenuShortcut, HotkeySourceKind::PeAccelerator,
                      HotkeySourceKind::Unknown}) {
        auto row = complete; row.sourceKind = kind;
        expect(!CanClearHotkey(row, &reason) && reason.text == "unsupported_source",
               "unsupported source remains denied with complete fields");
    }
    { auto row = window; row.processId = 0; expect(!CanClearHotkey(row, &reason), "window PID missing"); }
    { auto row = window; row.processCreationTime100ns = 0; expect(!CanClearHotkey(row, &reason), "window creation missing"); }
    { auto row = window; row.windowHandle = 0; expect(!CanClearHotkey(row, &reason), "window HWND missing"); }
    { auto row = window; row.threadId = 0; expect(!CanClearHotkey(row, &reason), "window TID missing"); }
    { auto row = window; row.expectedWindowHotkeyValue = 0; expect(!CanClearHotkey(row, &reason), "window raw value missing"); }
    { auto row = complete; row.processId = 0; expect(!CanClearHotkey(row, &reason), "shortcut PID missing"); }
    { auto row = complete; row.processCreationTime100ns = 0; expect(!CanClearHotkey(row, &reason), "shortcut creation missing"); }
    { auto row = complete; row.objectText.clear(); expect(!CanClearHotkey(row, &reason), "shortcut path missing"); }
    { auto row = complete; row.shortcutTargetPath.clear(); expect(!CanClearHotkey(row, &reason), "shortcut target missing"); }
    { auto row = complete; row.expectedShortcutHotkeyValue = 0; expect(!CanClearHotkey(row, &reason), "shortcut raw value missing"); }
    { auto row = complete; row.shortcutFileIdentity.valid = false; expect(!CanClearHotkey(row, &reason), "shortcut file identity invalid"); }
    { auto row = complete; row.shortcutFileIdentity.volumeSerialNumber = 0; expect(!CanClearHotkey(row, &reason), "shortcut volume missing"); }
    { auto row = complete; row.shortcutFileIdentity.fileIndex = 0; expect(!CanClearHotkey(row, &reason), "shortcut file index missing"); }
    { auto row = complete; row.shortcutFileIdentity.fileSize = 0; expect(!CanClearHotkey(row, &reason), "shortcut file size missing"); }
    // FILETIME zero is a representable captured timestamp; executor comparison still guards changes.
    { auto row = complete; row.shortcutFileIdentity.lastWriteTime100ns = 0; expect(CanClearHotkey(row, &reason), "shortcut zero mtime remains valid"); }

    auto target = validRegistered();
    auto entry = matchingEntry(target);
    expect(matchesRegisteredTarget(entry, target), "registered identity matches");
    { auto row = entry; row.source = 0; expect(!matchesRegisteredTarget(row, target), "registered source mismatch"); }
    { auto row = entry; row.processId++; expect(!matchesRegisteredTarget(row, target), "registered PID mismatch"); }
    { auto row = entry; row.threadId++; expect(!matchesRegisteredTarget(row, target), "registered TID mismatch"); }
    { auto row = entry; row.hotkeyId++; expect(!matchesRegisteredTarget(row, target), "registered ID mismatch"); }
    { auto row = entry; row.virtualKey++; expect(!matchesRegisteredTarget(row, target), "registered VK mismatch"); }
    { auto row = entry; row.modifiers++; expect(!matchesRegisteredTarget(row, target), "registered modifiers mismatch"); }
    { auto row = entry; row.hotkeyObject++; expect(!matchesRegisteredTarget(row, target), "registered object mismatch"); }
    { auto row = entry; row.sessionGlobals++; expect(!matchesRegisteredTarget(row, target), "registered session mismatch"); }
    { auto row = entry; row.threadInfo++; expect(!matchesRegisteredTarget(row, target), "registered threadInfo mismatch"); }
    { auto row = entry; row.windowHandle++; expect(!matchesRegisteredTarget(row, target), "registered HWND mismatch"); }
    { auto row = entry; row.snapshotHash++; expect(!matchesRegisteredTarget(row, target), "registered hash mismatch"); }
    { auto row = entry; auto optional = target; optional.expectedSnapshotHash = 0;
      row.snapshotHash++; expect(matchesRegisteredTarget(row, optional), "registered optional hash omitted"); }

    ksword::ark::KeyboardHotkeyEnumResult snapshot;
    snapshot.io.ok = true; snapshot.version = KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION;
    snapshot.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK;
    snapshot.returnedCount = snapshot.totalCount = 1;
    snapshot.entries.push_back(entry);
    expect(completeKeyboardSnapshot(snapshot), "complete keyboard snapshot");
    { auto row = snapshot; row.io.ok = false; expect(!completeKeyboardSnapshot(row), "snapshot IO failed"); }
    { auto row = snapshot; row.version++; expect(!completeKeyboardSnapshot(row), "snapshot version mismatch"); }
    { auto row = snapshot; row.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL;
      expect(!completeKeyboardSnapshot(row), "snapshot partial"); }
    { auto row = snapshot; row.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED;
      expect(!completeKeyboardSnapshot(row), "snapshot truncated"); }
    { auto row = snapshot; row.totalCount++; expect(!completeKeyboardSnapshot(row), "snapshot count truncated"); }
    { auto row = snapshot; row.entries.clear(); expect(!completeKeyboardSnapshot(row), "snapshot rows missing"); }
    std::printf("hotkey-clear identity regression: %d cases, %d failures\n", cases, failures);
    return failures == 0 ? 0 : 1;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="C++ compiler path; defaults to g++ then clang++")
    args = parser.parse_args()
    compiler = shutil.which(args.compiler) if args.compiler else (shutil.which("g++") or shutil.which("clang++"))
    if compiler is None:
        print("C++ compiler unavailable: g++/clang++", flush=True)
        return 2

    controller = CONTROLLER.read_text(encoding="utf-8-sig")
    constants = protocol_constants(PROTOCOL.read_text(encoding="utf-8-sig"))
    production = {
        "/*PROTOCOL*/": constants,
        "/*CAN_CLEAR*/": extract_function(controller, "CanClearHotkey"),
        "/*COMPLETE_SNAPSHOT*/": extract_function(controller, "completeKeyboardSnapshot"),
        "/*MATCH_TARGET*/": extract_function(controller, "matchesRegisteredTarget"),
    }
    source_text = HARNESS
    for marker, implementation in production.items():
        source_text = source_text.replace(marker, implementation)

    root = ROOT / ".codex-build-logs" / "hotkey-clear-identity"
    root.mkdir(parents=True, exist_ok=True)
    run_dir = root / f"case-{uuid.uuid4().hex}"
    if not run_dir.resolve().is_relative_to(ROOT.resolve()):
        raise RuntimeError("Test output escaped the repository")
    run_dir.mkdir()
    try:
        source = run_dir / "identity.cpp"
        binary = run_dir / ("identity.exe" if os.name == "nt" else "identity")
        source.write_text(source_text, encoding="utf-8")
        env = os.environ.copy()
        env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
        print(f"compiler: {compiler}", flush=True)
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(binary)], check=True, env=env)
        return subprocess.run([str(binary)], check=False, env=env).returncode
    finally:
        if run_dir.resolve().is_relative_to(root.resolve()) and root.resolve().is_relative_to(ROOT.resolve()):
            shutil.rmtree(run_dir)


if __name__ == "__main__":
    raise SystemExit(main())
