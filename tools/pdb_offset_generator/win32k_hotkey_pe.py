"""只读 AMD64 PE 解析：身份、导出/导入、异常表函数边界及指令。"""

from __future__ import annotations

from dataclasses import dataclass
from hashlib import sha256
from pathlib import Path
from typing import Iterable
from uuid import UUID
import struct

import capstone
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP
import pefile


class PEInspectionError(ValueError):
    pass


@dataclass(frozen=True)
class RSDSIdentity:
    guid: str
    age: int
    pdb_name: str


@dataclass(frozen=True)
class PEIdentity:
    path: str
    sha256: str
    time_date_stamp: int
    image_size: int
    file_version_ms: int
    file_version_ls: int
    rsds: RSDSIdentity

    def report(self) -> dict:
        return {
            "path": self.path,
            "sha256": self.sha256,
            "timeDateStamp": f"0x{self.time_date_stamp:08X}",
            "sizeOfImage": f"0x{self.image_size:X}",
            "fileVersion": f"{self.file_version_ms >> 16}.{self.file_version_ms & 0xFFFF}."
                           f"{self.file_version_ls >> 16}.{self.file_version_ls & 0xFFFF}",
            "rsdsGuid": self.rsds.guid,
            "rsdsAge": self.rsds.age,
            "pdbName": self.rsds.pdb_name,
        }


@dataclass
class Function:
    begin: int
    end: int
    instructions: list

    def report_range(self) -> dict:
        return {"beginRva": f"0x{self.begin:X}", "endRva": f"0x{self.end:X}"}


def instruction_evidence(insn) -> dict:
    return {
        "rva": f"0x{insn.address:X}",
        "bytes": insn.bytes.hex(),
        "instruction": f"{insn.mnemonic} {insn.op_str}".strip(),
    }


class PEImage:
    def __init__(self, path: Path):
        self.path = path.resolve()
        if not self.path.is_file():
            raise PEInspectionError(f"input_missing:{self.path}")
        self.data = self.path.read_bytes()
        self.pe = pefile.PE(data=self.data, fast_load=False)
        if self.pe.FILE_HEADER.Machine != 0x8664 or self.pe.OPTIONAL_HEADER.Magic != 0x20B:
            raise PEInspectionError("not_amd64_pe32_plus")
        rsds = self._read_rsds()
        versions = getattr(self.pe, "VS_FIXEDFILEINFO", [])
        if len(versions) != 1:
            raise PEInspectionError("fixed_file_version_missing_or_ambiguous")
        self.identity = PEIdentity(
            path=str(self.path),
            sha256=sha256(self.data).hexdigest().upper(),
            time_date_stamp=int(self.pe.FILE_HEADER.TimeDateStamp),
            image_size=int(self.pe.OPTIONAL_HEADER.SizeOfImage),
            file_version_ms=int(versions[0].FileVersionMS),
            file_version_ls=int(versions[0].FileVersionLS),
            rsds=rsds,
        )
        self.exports = self._read_exports()
        self.imports = self._read_imports()
        self.function_ranges = self._read_function_ranges()
        self._function_cache: dict[int, Function] = {}
        self.decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.decoder.detail = True

    def _read_rsds(self) -> RSDSIdentity:
        matches: list[RSDSIdentity] = []
        for entry in getattr(self.pe, "DIRECTORY_ENTRY_DEBUG", []):
            if int(entry.struct.Type) != 2 or entry.struct.SizeOfData < 24:
                continue
            payload = self.pe.get_data(int(entry.struct.AddressOfRawData),
                                       int(entry.struct.SizeOfData))
            if len(payload) < 24 or payload[:4] != b"RSDS":
                continue
            guid = str(UUID(bytes_le=payload[4:20]))
            age = struct.unpack_from("<I", payload, 20)[0]
            pdb_name = payload[24:].split(b"\0", 1)[0].decode("utf-8", "replace")
            matches.append(RSDSIdentity(guid=guid, age=age, pdb_name=pdb_name))
        if len(matches) != 1:
            raise PEInspectionError("rsds_missing_or_ambiguous")
        return matches[0]

    def _read_exports(self) -> dict[str, list[int]]:
        exports: dict[str, list[int]] = {}
        directory = getattr(self.pe, "DIRECTORY_ENTRY_EXPORT", None)
        if directory is None:
            raise PEInspectionError("exports_missing")
        for symbol in directory.symbols:
            if not symbol.name or symbol.forwarder:
                continue
            name = symbol.name.decode("ascii", "replace")
            exports.setdefault(name, []).append(int(symbol.address))
        return exports

    def _read_imports(self) -> dict[int, tuple[str, str]]:
        imports: dict[int, tuple[str, str]] = {}
        image_base = int(self.pe.OPTIONAL_HEADER.ImageBase)
        for descriptor in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = descriptor.dll.decode("ascii", "replace").lower()
            for symbol in descriptor.imports:
                if symbol.name:
                    name = symbol.name.decode("ascii", "replace")
                else:
                    name = f"ordinal_{symbol.ordinal}"
                imports[int(symbol.address) - image_base] = (dll, name)
        return imports

    def _read_function_ranges(self) -> dict[int, int]:
        ranges: dict[int, int] = {}
        for entry in getattr(self.pe, "DIRECTORY_ENTRY_EXCEPTION", []):
            begin = int(entry.struct.BeginAddress)
            end = int(entry.struct.EndAddress)
            if begin >= end or begin in ranges:
                raise PEInspectionError("pdata_invalid_or_duplicate")
            ranges[begin] = end
        if not ranges:
            raise PEInspectionError("pdata_missing")
        ordered = sorted(ranges.items())
        if any(left_end > right_begin for (_, left_end), (right_begin, _) in
               zip(ordered, ordered[1:])):
            raise PEInspectionError("pdata_function_ranges_overlap")
        return ranges

    def unique_export(self, name: str) -> int:
        values = self.exports.get(name, [])
        if len(values) != 1:
            raise PEInspectionError(f"export_missing_or_ambiguous:{name}")
        return values[0]

    def function(self, begin: int) -> Function:
        if begin not in self.function_ranges:
            raise PEInspectionError(f"function_boundary_missing:0x{begin:X}")
        cached = self._function_cache.get(begin)
        if cached is not None:
            return cached
        end = self.function_ranges[begin]
        sections = [section for section in self.pe.sections
                    if (section.Characteristics & 0x20000000) != 0 and
                    section.VirtualAddress <= begin and
                    end <= section.VirtualAddress + max(section.Misc_VirtualSize,
                                                       section.SizeOfRawData)]
        if len(sections) != 1:
            raise PEInspectionError(f"function_not_in_unique_executable_section:0x{begin:X}")
        data = self.pe.get_data(begin, end - begin)
        if len(data) != end - begin:
            raise PEInspectionError(f"function_bytes_missing:0x{begin:X}")
        instructions = list(self.decoder.disasm(data, begin))
        if not instructions or instructions[0].address != begin or \
                instructions[-1].address + instructions[-1].size != end:
            raise PEInspectionError(f"function_decode_incomplete:0x{begin:X}")
        result = Function(begin, end, instructions)
        self._function_cache[begin] = result
        return result

    def call_target(self, insn) -> tuple[str, int | str] | None:
        if insn.mnemonic != "call" or len(insn.operands) != 1:
            return None
        operand = insn.operands[0]
        if operand.type == X86_OP_IMM:
            return ("direct", int(operand.imm))
        if operand.type == X86_OP_MEM and operand.mem.base == X86_REG_RIP:
            slot = int(insn.address + insn.size + operand.mem.disp)
            imported = self.imports.get(slot)
            return ("import", f"{imported[0]}!{imported[1]}" if imported else f"unknown_slot:0x{slot:X}")
        return None

    def direct_callees(self, function: Function) -> list[tuple[int, object]]:
        result: list[tuple[int, object]] = []
        for insn in function.instructions:
            target = self.call_target(insn)
            if target and target[0] == "direct":
                result.append((int(target[1]), insn))
        return result

    def import_calls(self, function: Function, name: str) -> list:
        calls = []
        for insn in function.instructions:
            target = self.call_target(insn)
            if target and target[0] == "import" and str(target[1]).split("!", 1)[-1] == name:
                calls.append(insn)
        return calls

    def read_bytes(self, rva: int, size: int) -> bytes:
        value = self.pe.get_data(rva, size)
        if len(value) != size:
            raise PEInspectionError(f"bytes_missing:0x{rva:X}")
        return value


def one(items: Iterable, reason: str):
    values = list(items)
    if len(values) != 1:
        raise PEInspectionError(f"{reason}:count={len(values)}")
    return values[0]
