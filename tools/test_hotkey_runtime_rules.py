#!/usr/bin/env python3
"""Exercise the production C hotkey matcher on inert PE images and byte mutations.

The PE files are copied into RVA images by hotkey_rule_probe.c and never loaded
as executable modules. Version, RSDS and SHA are recorded only as specimen data.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
DRIVER = REPO / "KswordARKDriver" / "src" / "features" / "keyboard"
BUILD = REPO / ".codex-build-logs" / "hotkey-rule-host"
CORPUS = REPO / ".codex-build-logs" / "hotkey-cross-version-20261025"
LOCAL_FULL = REPO / ".codex-build-logs" / "hotkey-analysis-20261025" / "win32kfull.sys"
LOCAL_BASE = Path(r"C:\Windows\System32\win32kbase.sys")
LOCAL_REPORT = REPO / ".codex-build-logs" / "hotkey-analysis-20261025" / "matcher-report.json"
SOURCES = [REPO / "tools" / "hotkey_rule_probe.c"] + [
    DRIVER / f"keyboard_rule_{name}.c" for name in (
        "match", "register", "insert", "remove", "slot", "loop", "assignment", "owner", "flow", "values", "pe", "decode"
    )
] + [REPO / "third_party" / "zydis" / "Zydis.c"]
HEADERS = [DRIVER / f"keyboard_rule_{name}.h" for name in
           ("ir", "match", "flow", "proof")] + [REPO / "third_party" / "zydis" / "Zydis.h"]


def compiler() -> str:
    found = shutil.which("gcc")
    if found:
        return found
    known = Path.home() / "AppData" / "Local" / "Programs" / "CLion" / "bin" / "mingw" / "bin" / "gcc.exe"
    if known.is_file():
        return str(known)
    raise RuntimeError("gcc unavailable; no compiler installation attempted")


def build_probe() -> Path:
    BUILD.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    for source in [*SOURCES, *HEADERS]:
        digest.update(str(source.relative_to(REPO)).encode("utf-8"))
        digest.update(source.read_bytes())
    binary = BUILD / f"hotkey_rule_probe-{digest.hexdigest()[:16]}.exe"
    if binary.is_file() and binary.stat().st_size > 0:
        return binary
    command = [compiler(), "-std=c11", "-O0", "-DZYDIS_STATIC_BUILD",
               "-DZYCORE_STATIC_BUILD", "-DZYAN_NO_LIBC", "-I",
               str(REPO / "third_party" / "zydis"), *(str(path) for path in SOURCES),
               "-o", str(binary)]
    completed = subprocess.run(command, cwd=REPO, capture_output=True, text=True,
                               timeout=180, check=False)
    if completed.returncode:
        raise RuntimeError(f"host compile failed ({completed.returncode}):\n{completed.stderr}")
    return binary


def probe(binary: Path, full: Path, base: Path) -> dict:
    completed = subprocess.run([str(binary), str(full), str(base)], cwd=REPO,
                               capture_output=True, text=True, timeout=120, check=False)
    if completed.returncode:
        raise RuntimeError(f"probe exit {completed.returncode}: {completed.stderr.strip()}")
    result = json.loads(completed.stdout)
    if result["Matched"]:
        assert result["Error"] == 0 and result["ErrorName"] == "ok"
    else:
        assert result["Error"] != 0 and result["EvidenceCount"] == 0
    return result


def assignment_probe(binary: Path, full: Path, base: Path) -> dict:
    completed = subprocess.run([str(binary), str(full), str(base), "--debug-assignment"],
                               cwd=REPO, capture_output=True, text=True,
                               timeout=120, check=True)
    return json.loads(completed.stdout)


def owner_probe(binary: Path, full: Path, base: Path, entry: int) -> dict:
    completed = subprocess.run([str(binary), str(full), str(base), "--debug-owner", hex(entry)],
                               cwd=REPO, capture_output=True, text=True,
                               timeout=120, check=True)
    return json.loads(completed.stdout)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pe_layout(data: bytes) -> tuple[int, int, int, int]:
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError("dos header missing")
    nt = struct.unpack_from("<I", data, 0x3C)[0]
    if nt + 24 > len(data) or data[nt:nt + 4] != b"PE\0\0":
        raise ValueError("nt header missing")
    count = struct.unpack_from("<H", data, nt + 6)[0]
    optional_size = struct.unpack_from("<H", data, nt + 20)[0]
    section_table = nt + 24 + optional_size
    if count > 96 or section_table + 40 * count > len(data):
        raise ValueError("section table missing")
    return nt, nt + 24, section_table, count


def rva_offset(data: bytes, rva: int) -> int:
    _, optional, sections, count = pe_layout(data)
    header_size = struct.unpack_from("<I", data, optional + 60)[0]
    if rva < header_size:
        return rva
    for index in range(count):
        section = sections + 40 * index
        virtual_size, virtual_address, raw_size, raw_address = struct.unpack_from(
            "<IIII", data, section + 8)
        delta = rva - virtual_address
        if 0 <= delta < min(max(virtual_size, raw_size), raw_size):
            offset = raw_address + delta
            if offset < len(data):
                return offset
    raise ValueError(f"RVA not backed by file bytes: 0x{rva:X}")


def changed_file(source: Path, directory: Path, name: str,
                 edits: list[tuple[int, bytes, bytes]]) -> Path:
    data = bytearray(source.read_bytes())
    for offset, expected, replacement in edits:
        if len(expected) != len(replacement) or data[offset:offset + len(expected)] != expected:
            raise AssertionError(f"mutation precondition failed at file offset 0x{offset:X}")
        data[offset:offset + len(expected)] = replacement
    destination = directory / name
    destination.write_bytes(data)
    assert sha256(destination) != sha256(source)
    return destination


class HotkeyRuntimeRuleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.binary = build_probe()
        cls.local = (probe(cls.binary, LOCAL_FULL, LOCAL_BASE)
                     if LOCAL_FULL.is_file() and LOCAL_BASE.is_file() else None)

    def require_local_match(self) -> None:
        if self.local is None:
            self.skipTest("local full/base specimen unavailable")
        if not self.local["Matched"]:
            self.skipTest(f"local positive still unresolved: {self.local['ErrorName']}")

    def assert_rejected(self, full: Path, base: Path = LOCAL_BASE) -> None:
        value = probe(self.binary, full, base)
        self.assertFalse(value["Matched"], value)
        self.assertNotEqual(value["Error"], 0)
        self.assertEqual(value["EvidenceCount"], 0)

    def test_local_actual_pair(self) -> None:
        if self.local is None:
            self.skipTest("local full/base specimen unavailable")
        self.assertTrue(self.local["Matched"], self.local["ErrorName"])
        self.assertEqual(self.local["TableOffset"], 0x3290)
        self.assertEqual(self.local["CacheOffset"], 0x36A8)
        self.assertEqual(self.local["BucketCount"], 128)
        self.assertEqual(self.local["ObjectSize"], 0x48)
        self.assertGreater(self.local["EvidenceCount"], 0)

    def test_different_link_sources_do_not_merge(self) -> None:
        completed = subprocess.run([str(self.binary), "--selftest-values"], cwd=REPO,
                                   capture_output=True, text=True, timeout=30, check=True)
        result = json.loads(completed.stdout)
        self.assertTrue(result["LinkEqualPreserved"])
        self.assertTrue(result["LinkDistinctRejected"], result)

    def test_nullable_window_survives_multiple_zero_paths(self) -> None:
        completed = subprocess.run([str(self.binary), "--selftest-values"], cwd=REPO,
                                   capture_output=True, text=True, timeout=30, check=True)
        result = json.loads(completed.stdout)
        self.assertTrue(result["NullableRepeatedZeroPreserved"], result)

    def test_single_slot_helpers_against_production_c(self) -> None:
        specimens = (
            ("22621.900", 0x1819C0, 0x40, 0x28, 0x1A, 0x30),
            ("22621.6133", 0x3FE0C, 0x48, 0x30, 0x22, 0x38),
            ("22621.7079", 0x25A74, 0x48, 0x30, 0x22, 0x38),
            ("26100.1", 0x298174, 0x48, 0x30, 0x22, 0x38),
            ("26100.5074", 0x28A758, 0x48, 0x30, 0x22, 0x38),
        )
        if not (CORPUS / "manifest.json").is_file():
            self.skipTest("cross-version PE corpus unavailable")
        for version, entry, object_size, next_offset, flags_offset, child_offset in specimens:
            folder = CORPUS / f"10.0.{version}"
            full, base = folder / "win32kfull.sys", folder / "win32kbase.sys"
            if not full.is_file() or not base.is_file():
                continue
            with self.subTest(version=version):
                completed = subprocess.run([
                    str(self.binary), str(full), str(base), "--debug-slot",
                    hex(entry), hex(object_size), hex(next_offset), hex(flags_offset),
                    "2", hex(child_offset)
                ], cwd=REPO, capture_output=True, text=True, timeout=120, check=True)
                result = json.loads(completed.stdout)
                self.assertTrue(result["SlotMatched"], result)
                self.assertEqual(result["RemoveRva"], entry)
        if self.local is not None:
            with self.subTest(version="local-26100.9022"):
                completed = subprocess.run([
                    str(self.binary), str(LOCAL_FULL), str(LOCAL_BASE), "--debug-slot",
                    "0x285BA0", "0x48", "0x30", "0x22", "2", "0x38"
                ], cwd=REPO, capture_output=True, text=True, timeout=120, check=True)
                result = json.loads(completed.stdout)
                self.assertTrue(result["SlotMatched"], result)
                self.assertEqual(result["RemoveRva"], 0x285BA0)

    def test_base_assignment_all_six_samples(self) -> None:
        if not (CORPUS / "manifest.json").is_file():
            self.skipTest("cross-version PE corpus unavailable")
        for version in ("22621.900", "22621.6133", "22621.7079",
                        "26100.1", "26100.5074"):
            folder = CORPUS / f"10.0.{version}"
            full, base = folder / "win32kfull.sys", folder / "win32kbase.sys"
            if not full.is_file() or not base.is_file():
                continue
            with self.subTest(version=version):
                result = assignment_probe(self.binary, full, base)
                self.assertTrue(result["AssignmentMatched"], result)
                self.assertGreater(result["BaseRva"], 0)
        if self.local is not None:
            with self.subTest(version="local-26100.9022"):
                result = assignment_probe(self.binary, LOCAL_FULL, LOCAL_BASE)
                self.assertTrue(result["AssignmentMatched"], result)
                self.assertGreater(result["BaseRva"], 0)

    def test_base_assignment_store_must_be_real_and_mandatory(self) -> None:
        folder = CORPUS / "10.0.22621.6133"
        full, base = folder / "win32kfull.sys", folder / "win32kbase.sys"
        if not full.is_file() or not base.is_file():
            self.skipTest("22621.6133 PE pair unavailable")
        self.assertTrue(assignment_probe(self.binary, full, base)["AssignmentMatched"])
        data = base.read_bytes()
        store = rva_offset(data, 0x4FF64)
        prior = rva_offset(data, 0x4FF61)
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            directory = Path(temporary)
            wrong_source = changed_file(base, directory, "assignment-wrong-source.sys", [
                (store, b"\x48\x89\x38", b"\x48\x89\x18"),
            ])
            partial_write = changed_file(base, directory, "assignment-partial-write.sys", [
                (store, b"\x48\x89\x38", b"\x90\x89\x38"),
            ])
            bypass = changed_file(base, directory, "assignment-bypass-store.sys", [
                (prior, b"\x48\x8B\x18", b"\x74\x04\x90"),
            ])
            for mutant in (wrong_source, partial_write, bypass):
                with self.subTest(mutant=mutant.name):
                    result = assignment_probe(self.binary, full, mutant)
                    self.assertFalse(result["AssignmentMatched"], result)
                    self.assertEqual(result["BaseRva"], 0)

    def test_base_assignment_metadata_is_not_a_support_list(self) -> None:
        folder = CORPUS / "10.0.22621.6133"
        full, base = folder / "win32kfull.sys", folder / "win32kbase.sys"
        if not full.is_file() or not base.is_file():
            self.skipTest("22621.6133 PE pair unavailable")
        baseline = assignment_probe(self.binary, full, base)
        self.assertTrue(baseline["AssignmentMatched"], baseline)
        data = base.read_bytes()
        nt, _, _, _ = pe_layout(data)
        rsds = data.find(b"RSDS")
        self.assertGreater(rsds, 0)
        timestamp = data[nt + 8:nt + 12]
        guid_byte = data[rsds + 4:rsds + 5]
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            changed = changed_file(base, Path(temporary), "assignment-metadata.sys", [
                (nt + 8, timestamp,
                 struct.pack("<I", (struct.unpack("<I", timestamp)[0] + 1) & 0xFFFFFFFF)),
                (rsds + 4, guid_byte, bytes([guid_byte[0] ^ 1])),
            ])
            self.assertEqual(assignment_probe(self.binary, full, changed), baseline)

    def test_owner_transparent_wrappers_from_actual_code(self) -> None:
        folder = CORPUS / "10.0.26100.5074"
        full, base = folder / "win32kfull.sys", folder / "win32kbase.sys"
        if not full.is_file() or not base.is_file():
            self.skipTest("26100.5074 PE pair unavailable")
        result = owner_probe(self.binary, full, base, 0x36850)
        self.assertTrue(result["OwnerMatched"], result)
        self.assertEqual((result["CanonicalKind"], result["Depth"]), (1, 1))
        self.assertEqual(result["AliasCount"], 2)
        self.assertEqual([alias["EntryRva"] for alias in result["Aliases"]],
                         [0x270D34, 0x36850])
        direct = owner_probe(self.binary, full, base, 0x270D34)
        self.assertTrue(direct["OwnerMatched"], direct)
        self.assertEqual((direct["CanonicalRva"], direct["Depth"]),
                         (result["CanonicalRva"], 0))
        for version, entry in (("26100.1", 0x553E0),):
            folder = CORPUS / f"10.0.{version}"
            if (folder.joinpath("win32kfull.sys").is_file() and
                    folder.joinpath("win32kbase.sys").is_file()):
                with self.subTest(version=version):
                    value = owner_probe(self.binary, folder / "win32kfull.sys",
                                        folder / "win32kbase.sys", entry)
                    self.assertTrue(value["OwnerMatched"], value)
                    self.assertEqual((value["CanonicalKind"], value["Depth"]), (1, 1))
        if self.local is not None:
            with self.subTest(version="local-26100.9022"):
                value = owner_probe(self.binary, LOCAL_FULL, LOCAL_BASE, 0x60240)
                self.assertTrue(value["OwnerMatched"], value)
                self.assertEqual((value["CanonicalKind"], value["Depth"]), (1, 1))

    def test_owner_wrapper_rejects_side_effects_and_extra_calls(self) -> None:
        folder = CORPUS / "10.0.26100.5074"
        full, base = folder / "win32kfull.sys", folder / "win32kbase.sys"
        if not full.is_file() or not base.is_file():
            self.skipTest("26100.5074 PE pair unavailable")
        self.assertTrue(owner_probe(self.binary, full, base, 0x36850)["OwnerMatched"])
        data = full.read_bytes()
        write_site = rva_offset(data, 0x36859)
        call_site = rva_offset(data, 0x270D3F)
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            directory = Path(temporary)
            side_effect = changed_file(full, directory, "owner-side-effect.sys", [
                (write_site, b"\x48\x85\xC0", b"\x48\x89\x00"),
            ])
            extra_call = changed_file(full, directory, "owner-extra-call.sys", [
                (call_site, b"\x0F\x1F\x44\x00\x00", b"\xE8\xF0\xFF\xFF\xFF"),
            ])
            for mutant in (side_effect, extra_call):
                with self.subTest(mutant=mutant.name):
                    value = owner_probe(self.binary, mutant, base, 0x36850)
                    self.assertFalse(value["OwnerMatched"], value)
                    self.assertEqual(value["AliasCount"], 0)

    def test_bucket_loop_all_six_samples(self) -> None:
        if not (CORPUS / "manifest.json").is_file():
            self.skipTest("cross-version PE corpus unavailable")
        specimens = (
            ("22621.900", 0x90690, 0x31A8, 0x28, True),
            ("22621.6133", 0x3EBD0, 0x31C0, 0x30, True),
            ("22621.7079", 0x257A0, 0x31C0, 0x30, True),
            ("26100.1", 0x1C9D70, 0x30C0, 0x30, False),
            ("26100.5074", 0x18163C, 0x3298, 0x30, False),
        )
        for version, entry, table, next_offset, indexed in specimens:
            folder = CORPUS / f"10.0.{version}"
            full, base = folder / "win32kfull.sys", folder / "win32kbase.sys"
            if not full.is_file() or not base.is_file():
                continue
            with self.subTest(version=version):
                completed = subprocess.run([
                    str(self.binary), str(full), str(base), "--debug-loop",
                    hex(entry), hex(table), hex(next_offset), "128"
                ], cwd=REPO, capture_output=True, text=True, timeout=120, check=True)
                result = json.loads(completed.stdout)
                self.assertTrue(result["LoopMatched"], result)
                self.assertTrue(result["Loaded"] and result["Flowed"])
                self.assertGreater(result["SessionSlot"], 0)
                self.assertGreater(result["AnchorRva"], 0)
                self.assertEqual(result["LoopIndexReg"] != 0, indexed)
        if self.local is not None:
            with self.subTest(version="local-26100.9022"):
                completed = subprocess.run([
                    str(self.binary), str(LOCAL_FULL), str(LOCAL_BASE), "--debug-loop",
                    "0x1C8108", "0x3290", "0x30", "128"
                ], cwd=REPO, capture_output=True, text=True, timeout=120, check=True)
                result = json.loads(completed.stdout)
                self.assertTrue(result["LoopMatched"], result)
                self.assertGreater(result["LoopReg"], 0)

    def test_bucket_loop_rejects_wrong_count_table_and_provider(self) -> None:
        folder = CORPUS / "10.0.22621.6133"
        full, base = folder / "win32kfull.sys", folder / "win32kbase.sys"
        if not full.is_file() or not base.is_file():
            self.skipTest("22621.6133 PE pair unavailable")
        prefix = [str(self.binary), str(full), str(base), "--debug-loop", "0x3EBD0"]
        baseline = json.loads(subprocess.run(prefix + ["0x31C0", "0x30", "128"],
                                             cwd=REPO, capture_output=True, text=True,
                                             timeout=120, check=True).stdout)
        self.assertTrue(baseline["LoopMatched"], baseline)
        alternatives = (
            ("wrong_bucket_count", ["0x31C0", "0x30", "64"]),
            ("wrong_table", ["0x31C8", "0x30", "128"]),
            ("no_session_getter", ["0x31C0", "0x30", "128", "0"]),
            ("wrong_session_getter", ["0x31C0", "0x30", "128", "0x1234"]),
        )
        for name, arguments in alternatives:
            with self.subTest(case=name):
                result = json.loads(subprocess.run(prefix + arguments, cwd=REPO,
                                                   capture_output=True, text=True,
                                                   timeout=120, check=True).stdout)
                self.assertFalse(result["LoopMatched"], result)

    def test_verified_cross_version_pairs(self) -> None:
        if not (CORPUS / "manifest.json").is_file():
            self.skipTest("cross-version PE corpus unavailable")
        manifest = json.loads((CORPUS / "manifest.json").read_text(encoding="utf-8"))
        pairs = [pair for pair in manifest["pairs"] if pair["status"] == "verified"]
        self.assertGreaterEqual(len(pairs), 2)
        for pair in pairs:
            folder = CORPUS / pair["version"]
            full, base = folder / "win32kfull.sys", folder / "win32kbase.sys"
            with self.subTest(version=pair["version"]):
                self.assertEqual(sha256(full), pair["files"]["win32kfull.sys"]["sha256"])
                self.assertEqual(sha256(base), pair["files"]["win32kbase.sys"]["sha256"])
                result = probe(self.binary, full, base)
                self.assertTrue(result["Matched"], result["ErrorName"])
                self.assertGreater(result["EvidenceCount"], 0)

    def test_identity_metadata_is_not_a_support_list(self) -> None:
        self.require_local_match()
        data = LOCAL_FULL.read_bytes()
        nt, _, _, _ = pe_layout(data)
        rsds = data.find(b"RSDS")
        self.assertGreater(rsds, 0)
        self.assertEqual(data.find(b"RSDS", rsds + 4), -1)
        fixed_signature = bytes.fromhex("BD04EFFE")
        self.assertEqual(data.count(fixed_signature), 1)
        version_offset = data.index(fixed_signature) + 12
        timestamp = data[nt + 8:nt + 12]
        guid_byte = data[rsds + 4:rsds + 5]
        version = data[version_offset:version_offset + 4]
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            changed = changed_file(LOCAL_FULL, Path(temporary), "metadata-win32kfull.sys", [
                (nt + 8, timestamp, struct.pack("<I", (struct.unpack("<I", timestamp)[0] + 1) & 0xFFFFFFFF)),
                (rsds + 4, guid_byte, bytes([guid_byte[0] ^ 1])),
                (version_offset, version,
                 struct.pack("<I", (struct.unpack("<I", version)[0] + 1) & 0xFFFFFFFF)),
            ])
            self.assertEqual(probe(self.binary, changed, LOCAL_BASE), self.local)

    def test_stack_epilogue_cannot_replace_a_real_chain_step(self) -> None:
        self.require_local_match()
        if not LOCAL_REPORT.is_file():
            self.skipTest("mutation coordinate report unavailable")
        report = json.loads(LOCAL_REPORT.read_text(encoding="utf-8"))["pairs"][0]
        steps = report["evidence"]["unregister"]["removeStructure"]
        chain = next(item for item in steps if item["instruction"] == "add rbx, 0x30")
        stack = next(item for item in steps if item["instruction"] == "add rsp, 0x30")
        data = LOCAL_FULL.read_bytes()
        chain_offset = rva_offset(data, int(chain["rva"], 16))
        stack_offset = rva_offset(data, int(stack["rva"], 16))
        self.assertEqual(data[stack_offset:stack_offset + 4], bytes.fromhex(stack["bytes"]))
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            changed = changed_file(LOCAL_FULL, Path(temporary), "no-chain-win32kfull.sys", [
                (chain_offset, bytes.fromhex(chain["bytes"]), b"\x48\x83\xC3\x28"),
            ])
            self.assert_rejected(changed)

    def test_entercrit_argument_provenance(self) -> None:
        self.require_local_match()
        report = json.loads(LOCAL_REPORT.read_text(encoding="utf-8"))["pairs"][0]
        zero = next(item for item in report["evidence"]["unregister"]["enterCrit"]
                    if item["instruction"] == "xor ecx, ecx")
        data = LOCAL_FULL.read_bytes()
        offset = rva_offset(data, int(zero["rva"], 16))
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            changed = changed_file(LOCAL_FULL, Path(temporary), "nonzero-crit-win32kfull.sys", [
                (offset, bytes.fromhex(zero["bytes"]), b"\x0B\xC9"),
            ])
            self.assert_rejected(changed)

    def test_duplicate_export_name_is_ambiguous(self) -> None:
        self.require_local_match()
        data = LOCAL_FULL.read_bytes()
        old_name = b"ClearForegroundQueue"
        new_name = b"NtUserRegisterHotKey"
        self.assertEqual(len(old_name), len(new_name))
        self.assertEqual(data.count(old_name), 1)
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            changed = changed_file(LOCAL_FULL, Path(temporary), "duplicate-export-win32kfull.sys", [
                (data.index(old_name), old_name, new_name),
            ])
            self.assert_rejected(changed)

    def test_session_getter_must_be_proven_by_import(self) -> None:
        self.require_local_match()
        data = LOCAL_FULL.read_bytes()
        original = b"W32GetUserSessionState"
        replacement = b"X32GetUserSessionState"
        self.assertEqual(data.count(original), 1)
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            changed = changed_file(LOCAL_FULL, Path(temporary), "missing-getter-win32kfull.sys", [
                (data.index(original), original, replacement),
            ])
            self.assert_rejected(changed)

    def test_constructor_vk_field_is_not_guessed_from_neighbor(self) -> None:
        self.require_local_match()
        if not LOCAL_REPORT.is_file():
            self.skipTest("mutation coordinate report unavailable")
        report = json.loads(LOCAL_REPORT.read_text(encoding="utf-8"))["pairs"][0]
        write = next(item for item in report["evidence"]["register"]["fieldWrites"]
                     if item["instruction"] == "mov dword ptr [rbx + 0x24], r12d")
        data = LOCAL_FULL.read_bytes()
        offset = rva_offset(data, int(write["rva"], 16))
        original = bytes.fromhex(write["bytes"])
        self.assertEqual(original[-1], 0x24)
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            changed = changed_file(LOCAL_FULL, Path(temporary), "moved-vk-win32kfull.sys", [
                (offset, original, original[:-1] + b"\x25"),
            ])
            self.assert_rejected(changed)

    def test_truncated_window_handle_cannot_regain_pointer_identity(self) -> None:
        self.require_local_match()
        data = LOCAL_FULL.read_bytes()
        offset = rva_offset(data, 0x1F632A)
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            changed = changed_file(LOCAL_FULL, Path(temporary), "truncated-hwnd-win32kfull.sys", [
                (offset, b"\x48\x8B\xCD", b"\x90\x8B\xCD"),
            ])
            self.assert_rejected(changed)

    def test_mandatory_constructor_field_on_only_one_cfg_path(self) -> None:
        self.require_local_match()
        data = LOCAL_FULL.read_bytes()
        branch_rva, after_stores = 0x27CBB6, 0x27CBF0
        offset = rva_offset(data, branch_rva)
        displacement = after_stores - (branch_rva + 2)
        self.assertTrue(0 <= displacement <= 127)
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            changed = changed_file(LOCAL_FULL, Path(temporary), "bypass-fields-win32kfull.sys", [
                (offset, b"\x74\x03", bytes([0x74, displacement])),
            ])
            self.assert_rejected(changed)

    def test_constructor_node_alias_cannot_be_retargeted(self) -> None:
        self.require_local_match()
        data = LOCAL_FULL.read_bytes()
        offset = rva_offset(data, 0x27CBDD)
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            changed = changed_file(LOCAL_FULL, Path(temporary), "wrong-node-win32kfull.sys", [
                (offset, b"\x48\x8B\xCB", b"\x48\x8B\xC8"),
            ])
            self.assert_rejected(changed)

    def test_truncated_and_non_amd64_inputs_are_not_mapped(self) -> None:
        if self.local is None:
            self.skipTest("local full/base specimen unavailable")
        data = LOCAL_FULL.read_bytes()
        nt, _, _, _ = pe_layout(data)
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            directory = Path(temporary)
            truncated = directory / "truncated.sys"
            truncated.write_bytes(data[:4096])
            wrong_machine = changed_file(LOCAL_FULL, directory, "x86.sys", [
                (nt + 4, b"\x64\x86", b"\x4C\x01"),
            ])
            for bad in (truncated, wrong_machine):
                with self.subTest(file=bad.name):
                    completed = subprocess.run([str(self.binary), str(bad), str(LOCAL_BASE)],
                                               cwd=REPO, capture_output=True, text=True,
                                               timeout=30, check=False)
                    self.assertEqual(completed.returncode, 2)
                    self.assertIn("invalid_or_unreadable_amd64_pe", completed.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
