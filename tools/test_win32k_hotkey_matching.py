#!/usr/bin/env python3
"""用本机精确PE和受控字节变异核对热键匹配器的 fail-closed 行为。"""

from __future__ import annotations

from pathlib import Path
import json
import os
import struct
import sys
import tempfile
import unittest

import pefile

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools" / "pdb_offset_generator"))

from match_win32k_hotkeys import main, match_pair, render_driver_header  # noqa: E402
from win32k_hotkey_pe import PEImage  # noqa: E402

FULL = Path(os.environ.get("KSWORD_HOTKEY_FULL_PE",
    str(REPO / ".codex-build-logs" / "hotkey-analysis-20261025" / "win32kfull.sys")))
BASE = Path(os.environ.get("KSWORD_HOTKEY_BASE_PE", r"C:\Windows\System32\win32kbase.sys"))


def altered_copy(source: Path, directory: Path, name: str, changes: list[tuple[int, bytes]]) -> Path:
    data = bytearray(source.read_bytes())
    for offset, value in changes:
        if offset < 0 or offset + len(value) > len(data):
            raise ValueError("mutation_out_of_range")
        data[offset:offset + len(value)] = value
    output = directory / name
    output.write_bytes(data)
    return output


class HotkeyMatchingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not FULL.is_file() or not BASE.is_file():
            raise FileNotFoundError("Provide the exact win32kfull/base PE pair via "
                                    "KSWORD_HOTKEY_FULL_PE and KSWORD_HOTKEY_BASE_PE")
        cls.full = PEImage(FULL)
        cls.base = PEImage(BASE)
        cls.positive = match_pair(FULL, BASE)
        if cls.positive["status"] != "supported":
            raise AssertionError(f"fixture did not match: {cls.positive.get('reason')}")

    def test_actual_pair_requires_exact_identity_and_evidence(self) -> None:
        pair = self.positive
        self.assertEqual(pair["full"]["sha256"],
            "A8C75A11C28E54B62BC86E5566872241A94D6C485E0A93DE1B39D89D57D40445")
        self.assertEqual(pair["base"]["sha256"],
            "ADA11DB7B97AC5D3ACA370A988C8655E24BED2B4DAD2D1C38E034AF00B9BF517")
        self.assertEqual(pair["full"]["rsdsGuid"], "80db0813-4711-330d-d706-8d826ff8e1b0")
        self.assertEqual(pair["base"]["rsdsGuid"], "45d0067d-cac4-0108-52e2-b643f02d42bf")
        self.assertEqual(pair["profile"]["tableOffset"], 0x3290)
        self.assertEqual(pair["profile"]["sessionCacheOffset"], 0x36A8)
        self.assertEqual(pair["profile"]["removeMatchingRva"], 0x1C8108)
        self.assertEqual(len(bytes.fromhex(pair["profile"]["removeMatchingPrologue"])), 32)
        evidence = pair["evidence"]
        self.assertIn("xor ecx, ecx", [x["instruction"] for x in evidence["unregister"]["enterCrit"]])
        self.assertIn("xor edx, edx", [x["instruction"] for x in evidence["unregister"]["enterCrit"]])
        self.assertTrue(any("0x48" in x["instruction"] for x in evidence["register"]["allocation"]))

    def test_missing_unregister_call_rejects(self) -> None:
        call = self.positive["evidence"]["unregister"]["exportCall"]
        rva = int(call["rva"], 16)
        call_size = len(bytes.fromhex(call["bytes"]))
        self.assertEqual(call_size, 5)
        with tempfile.TemporaryDirectory() as temporary:
            changed = altered_copy(FULL, Path(temporary), "win32kfull.sys",
                [(self.full.pe.get_offset_from_rva(rva), b"\x90" * call_size)])
            result = match_pair(changed, BASE)
            self.assertEqual(result["status"], "unsupported")
            self.assertIn("unregister_call_chain", result["reason"])

    def test_duplicate_unregister_candidate_rejects(self) -> None:
        entry = self.full.function(self.full.unique_export("NtUserUnregisterHotKey"))
        primary = int(self.positive["evidence"]["unregister"]["exportCall"]["rva"], 16)
        target = int(self.positive["evidence"]["unregister"]["unregister"]["beginRva"], 16)
        other_calls = [call for callee, call in self.full.direct_callees(entry)
                       if call.address != primary and call.size == 5]
        self.assertTrue(other_calls)
        chosen = other_calls[0]
        displacement = target - (chosen.address + 5)
        encoded = b"\xE8" + struct.pack("<i", displacement)
        with tempfile.TemporaryDirectory() as temporary:
            changed = altered_copy(FULL, Path(temporary), "win32kfull.sys",
                [(self.full.pe.get_offset_from_rva(chosen.address), encoded)])
            result = match_pair(changed, BASE)
            self.assertEqual(result["status"], "unsupported")
            self.assertIn("ambiguous", result["reason"])

    def test_modified_vk_field_offset_rejects(self) -> None:
        writes = self.positive["evidence"]["register"]["fieldWrites"]
        vk = writes[6]  # owner/callback/window/dest/modifiers/flags/vk/id
        rva = int(vk["rva"], 16)
        function = self.full.function(int(self.positive["evidence"]["register"]["register"]["beginRva"], 16))
        instruction = next(insn for insn in function.instructions if insn.address == rva)
        self.assertEqual(instruction.disp_size, 1)
        self.assertEqual(instruction.bytes[instruction.disp_offset], 0x24)
        with tempfile.TemporaryDirectory() as temporary:
            changed = altered_copy(FULL, Path(temporary), "win32kfull.sys",
                [(self.full.pe.get_offset_from_rva(rva) + instruction.disp_offset, b"\x25")])
            result = match_pair(changed, BASE)
            self.assertEqual(result["status"], "unsupported")
            self.assertIn("register_call_chain", result["reason"])

    def test_base_rsds_identity_mismatch_rejects(self) -> None:
        debug = [entry for entry in self.base.pe.DIRECTORY_ENTRY_DEBUG
                 if entry.struct.Type == 2 and self.base.data[entry.struct.PointerToRawData:
                    entry.struct.PointerToRawData + 4] == b"RSDS"]
        self.assertEqual(len(debug), 1)
        offset = int(debug[0].struct.PointerToRawData) + 24
        self.assertEqual(self.base.data[offset:offset + len(b"win32kbase.pdb")], b"win32kbase.pdb")
        with tempfile.TemporaryDirectory() as temporary:
            changed = altered_copy(BASE, Path(temporary), "win32kbase.sys",
                [(offset, b"win32kfull.pdb")])
            result = match_pair(FULL, changed)
            self.assertEqual(result["status"], "unsupported")
            self.assertEqual(result["reason"], "module_rsds_name_mismatch")

    def test_paired_file_version_mismatch_rejects(self) -> None:
        signature = struct.pack("<I", 0xFEEF04BD)
        self.assertEqual(self.base.data.count(signature), 1)
        version_offset = self.base.data.index(signature) + 12
        old_version = struct.unpack_from("<I", self.base.data, version_offset)[0]
        with tempfile.TemporaryDirectory() as temporary:
            changed = altered_copy(BASE, Path(temporary), "win32kbase.sys",
                [(version_offset, struct.pack("<I", old_version + 1))])
            result = match_pair(FULL, changed)
            self.assertEqual(result["status"], "unsupported")
            self.assertEqual(result["reason"], "paired_file_version_mismatch")

    def test_entercrit_two_zero_arguments_required(self) -> None:
        evidence = self.positive["evidence"]["unregister"]["enterCrit"]
        ecx = next(item for item in evidence if item["instruction"] == "xor ecx, ecx")
        rva = int(ecx["rva"], 16)
        size = len(bytes.fromhex(ecx["bytes"]))
        with tempfile.TemporaryDirectory() as temporary:
            changed = altered_copy(FULL, Path(temporary), "win32kfull.sys",
                [(self.full.pe.get_offset_from_rva(rva), b"\x90" * size)])
            result = match_pair(changed, BASE)
            self.assertEqual(result["status"], "unsupported")
            self.assertIn("enter_crit_ecx_zero_missing", result["reason"])

    def test_cli_report_and_header_never_admit_unsupported_pair(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            call = self.positive["evidence"]["unregister"]["exportCall"]
            call_rva = int(call["rva"], 16)
            bad = altered_copy(FULL, directory, "bad-win32kfull.sys",
                [(self.full.pe.get_offset_from_rva(call_rva), b"\x90" * 5)])
            report = directory / "report.json"
            header = directory / "profiles.h"
            exit_code = main(["--pair", str(FULL), str(BASE), "--pair", str(bad), str(BASE),
                              "--out", str(report), "--emit-driver-header", str(header)])
            self.assertEqual(exit_code, 2)
            text = header.read_text(encoding="utf-8")
            self.assertIn("g_KswHotkeyMatchProfileCount = 1UL", text)
            self.assertEqual(text.count("0x1C8108UL"), 1)
            self.assertNotIn("Windows.h", text)
            self.assertIn("#include <ntddk.h>", text)
            self.assertIn('"unsupported"', report.read_text(encoding="utf-8"))

    def test_repeated_identical_pair_cannot_duplicate_driver_profile(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            report_path = directory / "repeated.json"
            header_path = directory / "repeated.h"
            exit_code = main(["--pair", str(FULL), str(BASE),
                              "--pair", str(FULL), str(BASE),
                              "--out", str(report_path),
                              "--emit-driver-header", str(header_path)])
            report = json.loads(report_path.read_text(encoding="utf-8"))
            header = header_path.read_text(encoding="utf-8")
            self.assertEqual(exit_code, 2)
            self.assertEqual(report["supportedPairs"], 1)
            self.assertEqual(report["unsupportedPairs"], 1)
            self.assertEqual(report["pairs"][1]["reason"], "duplicate_pair_identity")
            self.assertIn("g_KswHotkeyMatchProfileCount = 1UL", header)
            self.assertEqual(header.count("0x1C8108UL"), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
