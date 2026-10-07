#!/usr/bin/env python3
"""Run the actual Startup SCM action with deterministic fake Win32 transport."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess


def extract_function(source: str, name: str) -> str:
    # Mask comments/literals while preserving positions so braces in messages
    # cannot shorten or extend the extracted production function.
    masked = re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match.group()), source, flags=re.DOTALL)
    pattern = (r"^    (?:std::string|std::wstring|bool|ks::startup::ActionResult|"
               r"ks::startup::StartupActionStatus) " + re.escape(name) + r"\(")
    matches = list(re.finditer(pattern, masked, re.MULTILINE))
    if len(matches) != 1:
        raise ValueError(f"Expected one production definition of {name}; found {len(matches)}")
    start = matches[0].start()
    opening = masked.index("{", matches[0].end())
    depth = 1
    for index in range(opening + 1, len(masked)):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1] + "\n"
    raise ValueError(f"Unterminated production definition of {name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--startup-source", type=Path,
                        help="Optional baseline source used to prove the regression detects the original bug")
    parser.add_argument("--output-directory", type=Path)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parent.parent
    startup_source = args.startup_source or repository / "Ksword5.1/Ksword5.1/ksword/startup/startup.cpp"
    source = startup_source.read_text(encoding="utf-8-sig")
    output = args.output_directory or repository / ".codex-tmp/storage-controller-startup-tests"
    output.mkdir(parents=True, exist_ok=True)
    generated = output / "startup_scm_production.inc"
    helpers = ("FromWide", "ToWide", "MakeActionResult", "StatusFromWin32",
               "QueryScmStartType", "SetScmEntryEnabled")
    prelude = "\n".join((
        '#include "storage_controller_startup_stubs.h"',
        '#include "Ksword5.1/Ksword5.1/ksword/startup/startup.h"',
        '#include "Ksword5.1/Ksword5.1/ksword/string/string.h"',
        '#include "shared/usermode/KswordArkServiceMode.h"',
        "#include <cstdint>", "#include <string>", "#include <vector>", "namespace {", ""))
    generated.write_text(prelude + "".join(extract_function(source, name) for name in helpers) + "}\n",
                         encoding="utf-8")
    compiler = shutil.which("g++")
    if not compiler:
        raise RuntimeError("g++ is required to run the fake-SCM production regression")
    executable = output / "storage_controller_startup_tests.exe"
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unknown-pragmas",
               "-O2", "-static", "-DUNICODE", "-D_UNICODE", "-I", str(repository),
               "-I", str(repository / "tools"), "-I", str(output),
               str(repository / "tools/storage_controller_startup_tests.cpp"),
               str(repository / "Ksword5.1/Ksword5.1/ksword/string/string.cpp"),
               "-ladvapi32", "-o", str(executable)]
    subprocess.run(command, cwd=repository, check=True, timeout=60)
    return subprocess.run([str(executable)], cwd=repository, timeout=15).returncode


if __name__ == "__main__":
    raise SystemExit(main())
