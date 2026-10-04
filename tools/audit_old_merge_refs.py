"""只读复核旧引用：多共同祖先分别比较，不写 Git 或生产源码。"""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


def git(*arguments: str) -> bytes:
    return subprocess.check_output(["git", *arguments])


def audit(inventory: Path) -> dict:
    data = json.loads(inventory.read_text(encoding="utf-8-sig"))
    trees: dict[str, dict[str, str]] = {}

    def tree(revision: str) -> dict[str, str]:
        if revision not in trees:
            entries = {}
            for entry in git("ls-tree", "-r", "-z", "--full-tree", revision).split(b"\0"):
                if not entry:
                    continue
                metadata, path = entry.split(b"\t", 1)
                entries[path.decode("utf-8")] = metadata.split()[2].decode("ascii")
            trees[revision] = entries
        return trees[revision]

    results = []
    for record in data["merges"]:
        if record["reachable_from_head"]:
            continue
        merge = record["merge"]
        parents = record["parents"]
        # 所有共同祖先都从 Git 重新读取，绝不任意取一个作结论。
        process = subprocess.run(
            ["git", "merge-base", "--all", *parents],
            check=False, capture_output=True, text=True,
        )
        if process.returncode not in (0, 1):
            raise RuntimeError(f"merge-base failed: {merge}")
        bases = process.stdout.splitlines()
        if sorted(bases) != sorted(record["merge_bases"]):
            raise RuntimeError(f"inventory merge-base drift: {merge}")
        parent_trees = [tree(parent) for parent in parents]
        merged = tree(merge)
        parent_paths = set().union(*(set(value) for value in parent_trees))
        per_base = []
        for base in bases:
            original = tree(base)
            paths = parent_paths | set(original) | set(merged)
            unilateral_count = 0
            unilateral_differences = []
            dual_winners = []
            dual_custom = []
            for path in sorted(paths):
                initial = original.get(path)
                one, two = (value.get(path) for value in parent_trees)
                result = merged.get(path)
                if (one != initial) != (two != initial):
                    unilateral_count += 1
                    expected = one if one != initial else two
                    if result != expected:
                        unilateral_differences.append(path)
                elif one != initial and two != initial and one != two:
                    if result in (one, two):
                        dual_winners.append(path)
                    else:
                        dual_custom.append(path)
            per_base.append({
                "base": base,
                "single_side_changes": unilateral_count,
                "single_side_differences": unilateral_differences,
                "dual_winners": dual_winners,
                "dual_custom": dual_custom,
            })
        # 只有每个 base 都成立的回退才列为一致候选；其余仍保留供人工复核。
        consistent = sorted(set.intersection(*(
            set(value["single_side_differences"]) for value in per_base
        ))) if per_base else None
        results.append({
            "merge": merge,
            "metadata": record["metadata"],
            "parents": parents,
            "merge_bases": bases,
            "per_base": per_base,
            "consistent_single_side_differences": consistent,
            "parent_paths_absent_from_merge": sorted(parent_paths - set(merged)),
            "different_parent_blobs": sorted(
                path for path in parent_paths
                if parent_trees[0].get(path) != parent_trees[1].get(path)
            ),
        })
    return {"head": data["head"], "scope": "38 off-HEAD merges, all bases", "merges": results}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path,
                        default=Path(".codex-build-logs/history-merge-audit.json"))
    parser.add_argument("--output", type=Path,
                        default=Path(".codex-build-logs/old-merge-base-audit.json"))
    arguments = parser.parse_args()
    result = audit(arguments.inventory)
    # 仅写本次 JSON 证据；不保存源码行或配置值，避免暴露历史敏感配置。
    arguments.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                                encoding="utf-8")
    print(f"merges={len(result['merges'])}; "
          f"multiple_bases={sum(len(item['merge_bases']) > 1 for item in result['merges'])}; "
          f"no_base={sum(not item['merge_bases'] for item in result['merges'])}")


if __name__ == "__main__":
    main()
