"""独立解码 KSQ2 手机扫码文本；仅使用 Python 标准库，不访问驱动或网络。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import uuid
import zlib

ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"
VALUES = {character: value for value, character in enumerate(ALPHABET)}
MAX_BINARY = 2860
MAX_TEXT = 4296
REGISTERS = ("RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP",
             "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15", "RIP", "EFLAGS")


class DecodeError(ValueError):
    """格式、版本或完整性校验失败，禁止输出一份看似完整的部分报告。"""


def base45_decode(text: str) -> bytes:
    """严格执行 RFC 9285；不 strip，因为空格是有效 Base45 字符。"""
    if len(text) % 3 == 1:
        raise DecodeError("Invalid Base45 group length")
    output = bytearray()
    for offset in range(0, len(text), 3):
        group = text[offset:offset + 3]
        try:
            value = sum(VALUES[character] * 45 ** index for index, character in enumerate(group))
        except KeyError as error:
            raise DecodeError(f"Invalid Base45 character: {error.args[0]!r}") from error
        if len(group) == 3:
            if value > 65535:
                raise DecodeError("Base45 two-byte group overflow")
            output.extend(divmod(value, 256))
        elif value <= 255:
            output.append(value)
        else:
            raise DecodeError("Base45 final-byte group overflow")
    return bytes(output)


def base45_encode(binary: bytes) -> str:
    """供离线负例生成使用，与 C 实现独立的标准封装参考。"""
    output = []
    for offset in range(0, len(binary), 2):
        group = binary[offset:offset + 2]
        value = int.from_bytes(group, "big")
        digits = 3 if len(group) == 2 else 2
        for _ in range(digits):
            value, remainder = divmod(value, 45)
            output.append(ALPHABET[remainder])
    return "".join(output)


class Reader:
    """逐字段读取固定 schema，不复用 C ABI、padding 或生产编码器。"""

    def __init__(self, binary: bytes):
        self.binary = binary
        self.offset = 16

    def take(self, count: int) -> bytes:
        if self.offset + count > len(self.binary):
            raise DecodeError(f"Truncated field at byte {self.offset}")
        value = self.binary[self.offset:self.offset + count]
        self.offset += count
        return value

    def number(self, width: int = 4) -> int:
        return int.from_bytes(self.take(width), "little")

    def fields(self, names: str, width: int = 4) -> dict:
        return {name: self.number(width) for name in names.split()}

    def text(self, capacity: int) -> dict:
        length = self.number(1)
        if length > capacity:
            raise DecodeError(f"Text length {length} exceeds schema capacity {capacity}")
        raw = self.take(length)
        return {"Length": length, "RawHex": raw.hex(),
                "Text": raw.decode("utf-8", errors="backslashreplace")}


def read_diagnostics(reader: Reader) -> dict:
    """保留既有全部字段，以及 Stop Code 名称和四个参数角色。"""
    result = reader.fields("Captured BugCheckCode")
    result.update(reader.fields("Parameter1 Parameter2 Parameter3 Parameter4 FaultAddress", 8))
    result.update(reader.fields("FaultParameter"))
    result["FaultMeaning"] = reader.text(64)
    result.update(reader.fields("LastReason LastDumpType DumpBufferLength"))
    result.update(reader.fields("DumpOffset", 8))
    result.update(reader.fields("Irql Cpu"))
    result.update(reader.fields("PerfCounter ProcessObject ProcessId", 8))
    result.update(reader.fields("ProcessSource"))
    result["ProcessName"] = reader.text(16)
    result.update(reader.fields("CandidateAddress CandidateModuleBase CandidateModuleOffset", 8))
    result.update(reader.fields("CandidateModuleSize CandidateParameter CandidateClass CandidateConfidence"))
    result["CandidateModule"] = reader.text(64)
    result["CandidateSource"] = reader.text(64)
    result.update(reader.fields("CallbackMask ModuleCount"))
    result["StopCodeName"] = reader.text(64)
    result["ParameterRoles"] = [reader.text(32) for _ in range(4)]
    return result


def read_bgp(reader: Reader) -> dict:
    """时间线和签名读取固定全部槽位，不把未使用槽位静默删除。"""
    result = reader.fields("Version Size State PreparationStage PreparationStatus Stage LastStatus "
                           "ClearStatus DrawStatus FeatureMask ScreenWidth ScreenHeight ScreenBpp "
                           "RequiredWidth RequiredHeight")
    result.update(reader.fields("DrawCount", 8))
    result["SignatureFamily"] = [reader.number() for _ in range(8)]
    result.update(reader.fields("TimelineCount"))
    result["Timeline"] = [reader.fields("Stage Status") for _ in range(16)]
    return result


def read_environment(reader: Reader) -> dict:
    result = reader.fields("Flags VersionStatus UbrStatus Major Minor Build Ubr ProductType ProcessorCount")
    result.update(reader.fields("SampleTime PerformanceFrequency", 8))
    result.update(reader.fields("CiStatus CiOptions SecureBootStatus SecureBoot HypervisorPresent"))
    result["HypervisorVendor"] = reader.text(16)
    result["DriverBuild"] = reader.text(48)
    flag_names = {1: "VERSION", 2: "UBR", 4: "CPU", 8: "PERF", 16: "CI",
                  32: "SECUREBOOT", 64: "HYPERVISOR", 128: "DRIVERBUILD"}
    result["ValidFields"] = [name for flag, name in flag_names.items() if result["Flags"] & flag]
    # 官方 CodeIntegrityOptions 位：只有有效位及 NT_SUCCESS 同时成立时才解释布尔值。
    ci_available = bool(result["Flags"] & 16) and not bool(result["CiStatus"] & 0x80000000)
    result["CodeIntegrity"] = {"Available": ci_available}
    for flag, name in ((0x1, "ENABLED"), (0x2, "TESTSIGN"), (0x400, "HVCI_KMCI"),
                       (0x800, "HVCI_AUDIT"), (0x1000, "HVCI_STRICT"), (0x2000, "HVCI_IUM")):
        result["CodeIntegrity"][name] = bool(result["CiOptions"] & flag) if ci_available else None
    return result


def read_image(reader: Reader) -> dict:
    result = reader.fields("Flags Status")
    result.update(reader.fields("Base", 8))
    result.update(reader.fields("ImageSize TimeDateStamp Checksum"))
    guid = reader.take(16)
    result["PdbGuidRawHex"] = guid.hex()
    result["PdbGuid"] = str(uuid.UUID(bytes_le=guid))
    result.update(reader.fields("PdbAge"))
    result["Path"] = reader.text(160)
    result["PdbName"] = reader.text(64)
    result["ValidFields"] = [name for flag, name in
                             ((1, "PE"), (2, "RSDS"), (4, "PATH"), (8, "TRUNCATED"))
                             if result["Flags"] & flag]
    return result


def read_context(reader: Reader) -> dict:
    result = reader.fields("ContextSource ContextStatus RegisterMask StackSource StackStatus StackCount")
    result.update(reader.fields("StackThreadId StackSampleTime", 8))
    result["Registers"] = {name: reader.number(8) for name in REGISTERS}
    result["Stack"] = [reader.number(8) for _ in range(16)]
    result["ContextSourceName"] = {0: "MISSING", 1: "SYSTEM_DUMP_HEADER_CONTEXT",
                                    2: "BUGCHECK_CALLBACK_CONTEXT"}.get(result["ContextSource"], "UNKNOWN")
    result["StackSourceName"] = {0: "MISSING", 1: "FAULT_UNWOUND", 2: "RAW_CANDIDATES",
                                3: "LAST_OPERATION", 4: "SAME_THREAD_PRE_CRASH_OPERATION"}.get(
                                    result["StackSource"], "UNKNOWN")
    result["ValidRegisters"] = [name for index, name in enumerate(REGISTERS)
                                if result["RegisterMask"] & (1 << index)]
    return result


def read_event(reader: Reader) -> dict:
    result = reader.fields("Sequence Time", 8)
    result.update(reader.fields("Kind Code Status Flags"))
    result.update(reader.fields("ProcessId ThreadId", 8))
    result["Text"] = reader.text(64)
    result["KindName"] = {1: "IOCTL_BEGIN", 2: "IOCTL_END", 3: "DBGPRINT",
                          4: "DRIVER_LOG", 5: "DBGPRINT_STATE"}.get(result["Kind"], "UNKNOWN")
    result["FlagNames"] = [name for flag, name in
                           ((1, "TEXT_TRUNCATED"), (2, "CONTEXT_VALID"), (4, "STATUS_UNAVAILABLE"))
                           if result["Flags"] & flag]
    return result


def read_evidence(reader: Reader) -> dict:
    result = reader.fields("Version Flags")
    result.update(reader.fields("CaptureTime", 8))
    result.update(reader.fields("CpuGroup CpuNumber"))
    result.update(reader.fields("ThreadId", 8))
    result.update(reader.fields("ProcessCacheFlags"))
    result.update(reader.fields("ProcessSampleTime", 8))
    result["Environment"] = read_environment(reader)
    for image in ("Kernel", "Driver", "Candidate"):
        result[image] = read_image(reader)
    result["Context"] = read_context(reader)
    result.update(reader.fields("EventCount"))
    result.update(reader.fields("EventsDropped EventsOverwritten EventsDiscarded", 8))
    result["Events"] = [read_event(reader) for _ in range(6)]
    result["FlagNames"] = [name for flag, name in
                           ((1, "VALID"), (2, "RACE"), (4, "CACHE_PARTIAL"),
                            (0x100, "TRACE_PRESENT"), (0x200, "TRACE_RACE"),
                            (0x400, "TRACE_OVERWRITTEN"), (0x800, "TRACE_DROPPED"),
                            (0x1000, "DBG_CAPTURE_ACTIVE"), (0x2000, "DBG_CAPTURE_DISABLED"),
                            (0x4000, "CONTEXT_RACE"))
                           if result["Flags"] & flag]
    result["ProcessCacheFlagNames"] = [name for flag, name in
                                       ((1, "HIT"), (2, "EXITING"), (4, "STABLE"))
                                       if result["ProcessCacheFlags"] & flag]
    return result


def decode(text: str) -> tuple[dict, bytes]:
    """只有整个包通过长度、版本、CRC 和字段边界后才发布报告。"""
    if not text.startswith("KSQ2:") or len(text) > MAX_TEXT:
        raise DecodeError("Missing KSQ2 prefix or QR character capacity exceeded")
    binary = base45_decode(text[5:])
    if not 16 <= len(binary) <= MAX_BINARY:
        raise DecodeError("Invalid binary packet length")
    magic, version, flags, length, crc = struct.unpack_from("<4sHHII", binary)
    if magic != b"KSQ2" or version != 2 or flags & ~3:
        raise DecodeError("Unsupported KSQ2 magic, schema version or section flags")
    if length != len(binary):
        raise DecodeError("Declared packet length does not match complete input")
    computed = zlib.crc32(binary[:12] + binary[16:])
    if crc != computed:
        raise DecodeError(f"CRC32 mismatch: stored={crc:08X}, computed={computed:08X}")
    reader = Reader(binary)
    result = {"SchemaVersion": version, "SectionFlags": flags,
              "BinaryLength": length, "TextLength": len(text), "CRC32": f"0x{crc:08X}",
              "Diagnostics": read_diagnostics(reader)}
    result["Bgp"] = read_bgp(reader) if flags & 1 else None
    result["Evidence"] = read_evidence(reader) if flags & 2 else None
    if reader.offset != length:
        raise DecodeError(f"Unrecognized trailing fields: {length - reader.offset} bytes")
    return result, binary


def readable(value, name: str = "") -> list[str]:
    """以完整字段路径输出易读报告；原始文本字节附带 hex 可逆副本。"""
    if isinstance(value, dict):
        return [line for key, item in value.items()
                for line in readable(item, f"{name}.{key}" if name else key)]
    if isinstance(value, list):
        return [line for index, item in enumerate(value)
                for line in readable(item, f"{name}[{index}]" )]
    if isinstance(value, int):
        return [f"{name}={value} (0x{value:X})"]
    return [f"{name}={json.dumps(value, ensure_ascii=False)}"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--input", type=Path, help="保存完整扫码文本的 ASCII 文件，不能附加换行")
    source.add_argument("--text", help="完整 KSQ2: 扫码文本，原始空格必须保留")
    parser.add_argument("--json", action="store_true", help="输出保留全部原始字段的 JSON")
    parser.add_argument("--output", type=Path, help="写入指定报告文件，父目录必须已经存在")
    arguments = parser.parse_args()
    try:
        text = arguments.input.read_text(encoding="ascii") if arguments.input else arguments.text
        report, _ = decode(text)
        output = json.dumps(report, ensure_ascii=False, indent=2) if arguments.json else "\n".join(readable(report))
        if arguments.output:
            arguments.output.write_text(output + "\n", encoding="utf-8")
        else:
            print(output)
    except (DecodeError, OSError, UnicodeError) as error:
        parser.exit(2, f"KSQ2 decode rejected: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
