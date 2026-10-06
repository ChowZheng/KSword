"""只读扫描本地 Git 合并历史，输出待人工核查的功能保留候选。"""

import argparse
from collections import Counter
from functools import lru_cache
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


def git(*arguments):
    """读取 Git 对象或拓扑；不 fetch、checkout、修改索引或写 Git 对象。"""
    return subprocess.check_output(
        ["git", "-c", "core.quotepath=false", *arguments],
        text=True, encoding="utf-8", errors="replace").strip()


class BlobReader:
    """复用只读 cat-file 管道；缓存相同历史对象，避免重复启动进程。"""

    def __init__(self):
        self.process = subprocess.Popen(
            ["git", "cat-file", "--batch"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    @lru_cache(maxsize=256)
    def read(self, identity):
        """输入 blob SHA，返回完整 bytes；不存在的文件返回空 bytes。"""
        if identity is None:
            return b""
        self.process.stdin.write((identity + "\n").encode("ascii"))
        self.process.stdin.flush()
        header = self.process.stdout.readline().split()
        if len(header) != 3 or header[1] != b"blob":
            raise ValueError(f"Expected a blob: {identity}")
        contents = self.process.stdout.read(int(header[2]))
        if self.process.stdout.read(1) != b"\n":
            raise ValueError("Invalid cat-file framing")
        return contents

    def close(self):
        """关闭只读子进程，避免会话留下管道和进程。"""
        self.process.stdin.close()
        self.process.stdout.close()
        self.process.wait()


@lru_cache(maxsize=None)
def tree(revision):
    """返回仓库相对路径到 blob 的映射；不把目录或 submodule 当源码。"""
    result = {}
    for line in git("ls-tree", "-r", revision).splitlines():
        metadata, path = line.split("\t", 1)
        fields = metadata.split()
        if fields[1] == "blob":
            result[path] = fields[2]
    return result


def leaves(value, prefix=()):
    """按 JSON 键路径展开；tuple 避免键名包含句点造成身份碰撞。"""
    result = {}
    for key, item in value.items():
        path = (*prefix, key)
        if isinstance(item, dict):
            result.update(leaves(item, path))
        else:
            result[path] = item
    return result


def project_items(contents):
    """提取工程注册身份；同一 Include 改成 QtMoc 等类型单独分类。"""
    if not contents:
        return set()
    root = ET.fromstring(contents)
    return {(node.tag.split("}")[-1], node.attrib["Include"])
            for node in root.iter() if "Include" in node.attrib}


def text_lines(contents):
    """抽取有效文本行；仅作为候选筛选，不能证明函数行为等价或丢失。"""
    ignored = {"{", "}", "};", "else", "break;", "return;"}
    return {line.strip() for line in contents.decode("utf-8-sig", "replace").splitlines()
            if line.strip() and line.strip() not in ignored
            and not line.strip().startswith(("//", "/*", "*", "#include"))}


def audit_merge(merge, parents, current, reader):
    """比较 base/两个父提交/结果；所有异常均为待人工核查的候选。"""
    try:
        bases = git("merge-base", "--all", *parents).splitlines()
    except subprocess.CalledProcessError as error:
        # 无共同祖先的旧分支合并以空树为基线；其它 Git 错误仍必须失败。
        if error.returncode != 1:
            raise
        bases = []
    record = {"merge": merge, "parents": parents, "merge_bases": bases,
              "metadata": git("show", "-s", "--format=%ad %s", "--date=short", merge),
              "single_side_changes": 0, "single_side_differences": [],
              "dual_side_count": 0, "dual_source_candidates": [],
              "language_missing_new_keys": [], "project_missing_new_items": [],
              "project_kind_conversions": [], "parse_errors": []}
    # 多 merge-base 或 octopus 不作单一基线推断，显式报告这一覆盖边界。
    if len(bases) > 1:
        record["unsupported_reason"] = "multiple_merge_bases"
        return record
    record["baseline_kind"] = "common_ancestor" if bases else "unrelated_histories_empty_tree"
    states = [tree(bases[0]) if bases else {}, *[tree(rev) for rev in [*parents, merge]]]
    for path in sorted(set().union(*states)):
        old, left, right, merged = [state.get(path) for state in states]
        side = 0 if left != old and right == old else 1 if right != old and left == old else None
        if side is not None:
            record["single_side_changes"] += 1
            selected = [left, right][side]
            if merged != selected:
                record["single_side_differences"].append({
                    "path": path, "changed_parent_index": side, "base": old,
                    "parents": [left, right], "result": merged,
                    "head": current.get(path), "head_matches_changed_parent": current.get(path) == selected})
        dual = left != old and right != old and left != right
        if dual:
            record["dual_side_count"] += 1
        # 对工程和语言检查新增身份是否保留，禁止整体重写实际语言包。
        if path.endswith((".vcxproj", ".vcxproj.filters")) and len({old, left, right, merged}) > 1:
            try:
                identities = [project_items(reader.read(blob)) for blob in [old, left, right, merged]]
                added = (identities[1] - identities[0]) | (identities[2] - identities[0])
                for item in sorted(added - identities[3]):
                    converted = any(existing[1] == item[1] for existing in identities[3])
                    target = "project_kind_conversions" if converted else "project_missing_new_items"
                    record[target].append({"project": path, "item": item})
            except (ET.ParseError, ValueError) as error:
                record["parse_errors"].append({"path": path, "error": str(error)})
        if path.endswith(("/zh-CN.json", "/en-US.json")) and len({old, left, right, merged}) > 1:
            try:
                keys = [leaves(json.loads(reader.read(blob).decode("utf-8-sig"))) if blob else {}
                        for blob in [old, left, right, merged]]
                added = (keys[1].keys() - keys[0].keys()) | (keys[2].keys() - keys[0].keys())
                missing = added - keys[3].keys()
                if missing:
                    record["language_missing_new_keys"].append({
                        "path": path, "count": len(missing), "keys": sorted([list(key) for key in missing])})
            except (ValueError, UnicodeError) as error:
                record["parse_errors"].append({"path": path, "error": str(error)})
        # 双侧文件即使不是整文件取某一父侧，也检查父侧独有新增文本被移除的候选。
        source_suffixes = (".cpp", ".c", ".h", ".hpp", ".qml", ".ps1", ".py", ".yml", ".cmd")
        if dual and path.endswith(source_suffixes):
            lines = [text_lines(reader.read(blob)) for blob in [old, left, right, merged]]
            contributions = [(lines[1] - lines[0] - lines[2]), (lines[2] - lines[0] - lines[1])]
            missing = [sorted(contribution - lines[3]) for contribution in contributions]
            if any(missing):
                record["dual_source_candidates"].append({
                    "path": path, "result_equals_parent": 0 if merged == left else 1 if merged == right else None,
                    "missing_unique_added_lines": missing})
    return record


def main():
    """输出 JSON 库存及紧凑进度；默认审查 HEAD，--all 包括其它本地引用。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true", help="Include all local Git refs")
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    head = git("rev-parse", "HEAD")
    head_merges = set(git("rev-list", "--merges", head).splitlines())
    scope = "--all" if options.all else head
    merge_rows = git("rev-list", "--parents", "--merges", "--topo-order", scope).splitlines()
    inventory = {"head": head, "shallow": git("rev-parse", "--is-shallow-repository"),
                 "scope": scope, "head_merge_count": len(head_merges), "merges": []}
    reader = BlobReader()
    try:
        for number, row in enumerate(merge_rows, 1):
            merge, *parents = row.split()
            if len(parents) != 2:
                record = {"merge": merge, "parents": parents, "unsupported_reason": "octopus"}
            else:
                record = audit_merge(merge, parents, tree(head), reader)
            record["reachable_from_head"] = merge in head_merges
            inventory["merges"].append(record)
            if number % 10 == 0 or number == len(merge_rows):
                print(f"MERGES_REVIEWED={number}/{len(merge_rows)}", flush=True)
    finally:
        reader.close()
    # JSON 是审查结果库存，不是语言包；父目录须已存在，不创建新构建目录。
    options.output.write_text(json.dumps(inventory, ensure_ascii=False, indent=2), encoding="utf-8")
    totals = Counter()
    for record in inventory["merges"]:
        for field in ["single_side_differences", "dual_source_candidates", "project_missing_new_items",
                      "language_missing_new_keys", "parse_errors"]:
            totals[field] += len(record.get(field, []))
    print(json.dumps({"all_merge_count": len(merge_rows), "head_merge_count": len(head_merges),
                      "candidate_totals": totals}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
