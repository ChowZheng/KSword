"""Read-only Git/Qt entry-marker audit; candidates require call-chain review.

Does not execute project binaries, checkout revisions, build, or edit sources.
Outputs only the explicitly selected analysis JSON file.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
from functools import lru_cache
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PREFIX = "Ksword5.1/Ksword5.1/"
AREAS = ("ProcessDock/", "FileDock/", "NetworkDock/", "KernelDock/", "MonitorDock/")
EXACT = {"MainWindow.cpp", "MainWindow.h", "PluginHost.cpp", "PluginHost.h"}


def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], cwd=ROOT).decode("utf-8", "replace")


def selected(path: str) -> bool:
    return path.startswith(PREFIX) and path.endswith((".cpp", ".h", ".hpp")) and (
        path[len(PREFIX):] in EXACT or path[len(PREFIX):].startswith(AREAS))


class Blobs:
    def __init__(self) -> None:
        self.process = subprocess.Popen(["git", "cat-file", "--batch"], cwd=ROOT,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    @lru_cache(maxsize=None)
    def read(self, revision: str, path: str) -> tuple[str | None, str]:
        assert self.process.stdin is not None and self.process.stdout is not None
        self.process.stdin.write(f"{revision}:{path}\n".encode("utf-8"))
        self.process.stdin.flush()
        header = self.process.stdout.readline().decode("utf-8", "replace").split()
        if header[-1] == "missing":
            return None, ""
        blob, kind, size = header
        if kind != "blob":
            raise ValueError(f"Expected source blob: {revision}:{path}")
        data = self.process.stdout.read(int(size))
        if self.process.stdout.read(1) != b"\n":
            raise ValueError("Malformed cat-file boundary")
        return blob, data.decode("utf-8-sig", "replace")

    def close(self) -> None:
        assert self.process.stdin is not None
        self.process.stdin.close()
        self.process.wait()


def uncomment(source: str) -> str:
    # Preserve strings and their contents; discard comments to avoid dead examples.
    pattern = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/'
    return re.sub(pattern, lambda m: "" if m[0].startswith(("//", "/*")) else m[0], source)


def arguments(source: str, start: int) -> list[str]:
    depth = 1
    brace = bracket = 0
    quote = ""
    escaped = False
    parts: list[str] = []
    part_start = start
    for index in range(start, len(source)):
        char = source[index]
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
            continue
        if char in "\"'":
            quote = char
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                parts.append(source[part_start:index].strip())
                return parts
        elif char == "{":
            brace += 1
        elif char == "}":
            brace -= 1
        elif char == "[":
            bracket += 1
        elif char == "]":
            bracket -= 1
        elif char == "," and depth == 1 and brace == 0 and bracket == 0:
            parts.append(source[part_start:index].strip())
            part_start = index + 1
    return []


def compact(text: str) -> str:
    return re.sub(r"\s+", "", text)


@lru_cache(maxsize=None)
def markers(source: str) -> frozenset[tuple[str, str]]:
    source = uncomment(source)
    found: set[tuple[str, str]] = set()
    for match in re.finditer(r"\b(connect|addTab|createDockWidget|install\w*Menu)\s*\(", source):
        name = match[1]
        args = arguments(source, match.end())
        if not args:
            continue
        if name == "connect" and len(args) >= 3:
            # For lambdas record invoked names, not presentation string contents.
            target = args[-1]
            if "{" in target:
                calls = sorted(set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", target)))
                refs = sorted(set(re.findall(r"&([A-Za-z_]\w*(?:::\w+)+)", target)))
                target = "lambda:" + ",".join(calls + refs)
            key = "|".join(compact(arg) for arg in [*args[:-1], target])
            found.add(("connect", key))
        elif name == "addTab":
            found.add(("page", compact(args[0])))
        elif name == "createDockWidget" and len(args) >= 4:
            found.add(("dock", compact(args[0]) + "|" + compact(args[3])))
        elif name.startswith("install"):
            found.add(("menu-helper", name + "|" + "|".join(map(compact, args))))
    for match in re.finditer(r"\bnew\s+([A-Za-z_]\w*(?:Dock|Window|Dialog))\s*\(", source):
        if not match[1].startswith("Q"):
            found.add(("instantiate", match[1]))
    for match in re.finditer(r"(\w+)->setContextMenuPolicy\s*\(\s*(Qt::\w+)\s*\)", source):
        found.add(("menu-policy", match[1] + "|" + match[2]))
    return frozenset(found)


def records(items: set[tuple[str, str]] | frozenset[tuple[str, str]]) -> list[dict[str, str]]:
    return [{"kind": kind, "marker": value} for kind, value in sorted(items)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    options = parser.parse_args()
    output = options.output.resolve()
    if not output.is_relative_to(ROOT / ".codex-build-logs"):
        parser.error("Analysis output must remain in existing .codex-build-logs")
    if not output.parent.is_dir():
        parser.error("Output directory must already exist")
    head = git("rev-parse", "HEAD").strip()
    merges = [(row.split()[0], row.split()[1]) for row in
              git("log", "--merges", "--reverse", "--format=%H %cI", "HEAD").splitlines()
              if row.split()[1] < "2026-08-01"]
    blobs = Blobs()
    try:
        head_markers: set[tuple[str, str]] = set()
        workspace_markers: set[tuple[str, str]] = set()
        for path in git("ls-tree", "-r", "--name-only", head, "--", PREFIX).splitlines():
            if selected(path):
                head_markers.update(markers(blobs.read(head, path)[1]))
                local = ROOT / path
                if local.is_file():
                    workspace_markers.update(markers(local.read_text(encoding="utf-8-sig")))
        result: dict = {"head": head, "merge_count": len(merges), "scope": [*sorted(EXACT), *AREAS],
                        "head_unique_markers": len(head_markers), "rows": [], "candidates": []}
        for merge, date in merges:
            parents = git("show", "-s", "--format=%P", merge).split()
            if len(parents) != 2:
                raise ValueError(f"Expected two parents for {merge}")
            base = git("merge-base", *parents).strip()
            changed = [set(git("diff", "--name-only", base, parent, "--", PREFIX).splitlines())
                       for parent in parents]
            paths = sorted(path for path in changed[0] | changed[1] if selected(path))
            row = {"merge": merge, "date": date, "selected_paths": len(paths),
                   "dual_paths": [], "adopted_parent_paths": [], "missing_added": 0,
                   "removed_parent_markers": 0}
            for path in paths:
                bm = markers(blobs.read(base, path)[1])
                result_blob, result_source = blobs.read(merge, path)
                mm = markers(result_source)
                parent_values = [blobs.read(parent, path) for parent in parents]
                # Identical changes carried on both branches do not represent
                # choosing one parent's implementation over the other.
                if path in changed[0] & changed[1] and parent_values[0][0] != parent_values[1][0]:
                    row["dual_paths"].append(path)
                    for index, (blob, _) in enumerate(parent_values):
                        if result_blob == blob:
                            row["adopted_parent_paths"].append({"path": path, "parent_index": index})
                for index, (_, parent_source) in enumerate(parent_values):
                    pm = markers(parent_source)
                    added_missing = (pm - bm) - mm
                    removed = pm - mm
                    row["missing_added"] += len(added_missing)
                    row["removed_parent_markers"] += len(removed)
                    if removed:
                        result["candidates"].append({"merge": merge, "path": path,
                            "parent_index": index, "base": base,
                            "result_equals_parents": [i for i, (blob, _) in enumerate(parent_values)
                                                       if result_blob == blob],
                            "missing_added": records(added_missing), "removed": records(removed),
                            "absent_head": records(removed - head_markers),
                            "absent_workspace": records(removed - workspace_markers)})
            result["rows"].append(row)
            print(f'{merge[:8]} paths={len(paths)} dual={len(row["dual_paths"])} '
                  f'added_missing={row["missing_added"]} removed={row["removed_parent_markers"]}', flush=True)
        output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"OUTPUT={output}")
        print(f"CANDIDATES={len(result['candidates'])}")
    finally:
        blobs.close()


if __name__ == "__main__":
    main()
