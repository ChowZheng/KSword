"""独立 OpenCV 扫码、Base45/schema/CRC 解码及完整极值/非法输入回归。"""

from pathlib import Path
import cv2
import struct
import zlib
from decode_ksq2 import DecodeError, REGISTERS, base45_decode, base45_encode, decode


def reject(text: str) -> None:
    """任何非法输入都必须整体拒绝，不能输出部分正常字段。"""
    try:
        decode(text)
    except DecodeError:
        return
    raise AssertionError("Invalid KSQ2 input was accepted")


def repack(binary: bytes) -> str:
    """负例重新计算CRC，以便真正验证schema而非只触发CRC错误。"""
    packet = bytearray(binary)
    struct.pack_into("<I", packet, 12, zlib.crc32(packet[:12] + packet[16:]))
    return "KSQ2:" + base45_encode(packet)


def negative_tests(text: str, binary: bytes) -> None:
    """覆盖非法长度、CRC、版本、标志、Base45溢出及尾随字节。"""
    assert zlib.crc32(b"123456789") == 0xCBF43926
    # RFC9285公开示例和奇偶组完整往返独立检查字节顺序。
    assert base45_encode(b"AB") == "BB8"
    assert base45_decode("BB8") == b"AB"
    for value in (b"", b"A", b"AB", b"ABC", bytes(range(256))):
        assert base45_decode(base45_encode(value)) == value
    for invalid in ("KSQ2:A", "KSQ2:ZZZ", "KSQ2:ZZ", "KSQ2:aaa", "KSQ2:\n",
                    "KSQ1:" + text[5:], text[:-1], text + "A"):
        reject(invalid)
    crc_bad = bytearray(binary)
    crc_bad[-1] ^= 1
    reject("KSQ2:" + base45_encode(crc_bad))
    length_bad = bytearray(binary)
    struct.pack_into("<I", length_bad, 8, len(binary) - 1)
    reject(repack(length_bad))
    version_bad = bytearray(binary)
    struct.pack_into("<H", version_bad, 4, 3)
    reject(repack(version_bad))
    flags_bad = bytearray(binary)
    struct.pack_into("<H", flags_bad, 6, 0x8000)
    reject(repack(flags_bad))
    text_bad = bytearray(binary)
    text_bad[68] = 65  # FaultMeaning 的固定schema上限64，位置由字段宽度独立推导。
    reject(repack(text_bad))
    trailing = bytearray(binary + b"\0")
    struct.pack_into("<I", trailing, 8, len(trailing))
    reject(repack(trailing))
    short = bytearray(binary[:68])
    struct.pack_into("<I", short, 8, len(short))
    reject(repack(short))
    reject("KSQ2:" + base45_encode(bytes(2861)))
    print("PASS KSQ2 invalid lengths, schema, CRC and Base45 rejection")


def validate(path: Path) -> None:
    """接受一幅生产布局回放图，解码成功后核对原报告并保存PNG预览。"""
    pixels = cv2.imread(str(path))  # PPM的蓝色模块与白色静区由生产矩阵驱动。
    if pixels is None:
        raise RuntimeError(f"Missing replay image: {path}")
    expected = Path(str(path) + ".txt").read_text(encoding="ascii")
    expected_binary = Path(str(path) + ".bin").read_bytes()
    gray = cv2.cvtColor(pixels, cv2.COLOR_BGR2GRAY)  # 与普通手机扫码器一样分离亮暗模块。
    _, binary = cv2.threshold(gray, 100, 255, cv2.THRESH_BINARY)
    decoded = ""  # 小分辨率时按整数近邻放大，避免插值修改模块边界。
    detectors = [cv2.QRCodeDetector()]
    if hasattr(cv2, "QRCodeDetectorAruco"):
        detectors.insert(0, cv2.QRCodeDetectorAruco())
    for scale in (1, 2, 4):
        frame = binary if scale == 1 else cv2.resize(
            binary, None, fx=scale, fy=scale, interpolation=cv2.INTER_NEAREST
        )
        for detector in detectors:
            decoded = detector.detectAndDecode(frame)[0]
            if decoded:
                break
        if decoded:
            break
    if decoded != expected:
        raise AssertionError(f"QR round-trip mismatch: {path}, decoded={len(decoded)}")
    # 独立核对协议字段集合及关键零值，不能只证明一个部分报告可扫码。
    report, packet = decode(decoded)
    assert packet == expected_binary, "Independent Base45 bytes differ from native serialized packet"
    fields = report["Diagnostics"]
    required = {
        "Captured", "BugCheckCode", "Parameter1", "Parameter2", "Parameter3", "Parameter4",
        "FaultAddress", "FaultParameter", "FaultMeaning", "LastReason", "LastDumpType",
        "DumpBufferLength", "DumpOffset", "Irql", "Cpu", "PerfCounter", "ProcessObject",
        "ProcessId", "ProcessSource", "ProcessName", "CandidateAddress", "CandidateModuleBase",
        "CandidateModuleOffset", "CandidateModuleSize", "CandidateParameter", "CandidateClass",
        "CandidateConfidence", "CandidateModule", "CandidateSource", "CallbackMask", "ModuleCount",
        "StopCodeName", "ParameterRoles",
    }
    assert required <= fields.keys(), f"Missing diagnostics: {required - fields.keys()}"
    assert set(fields) == required, f"Unexpected diagnostics schema: {set(fields) ^ required}"
    assert len(fields["ParameterRoles"]) == 4
    bgp = report["Bgp"]
    evidence = report["Evidence"]
    if "empty" in path.stem:
        assert bgp is None and evidence is None and report["SectionFlags"] == 0
        assert fields["Captured"] == fields["BugCheckCode"] == 0
        assert fields["ProcessName"]["Length"] == 0
    else:
        assert len(bgp["Timeline"]) == 16 and len(bgp["SignatureFamily"]) == 8
        assert len(bgp) == 19
        assert len(evidence["Events"]) == 6
        assert len(evidence["Context"]["Registers"]) == 18
        assert len(evidence["Context"]["Stack"]) == 16
        assert "StackThreadId" in evidence["Context"] and "StackSampleTime" in evidence["Context"]
        assert "UbrStatus" in evidence["Environment"]
        assert {"EventsDropped", "EventsOverwritten", "EventsDiscarded"} <= evidence.keys()
        for image in ("Kernel", "Driver", "Candidate"):
            assert len(bytes.fromhex(evidence[image]["PdbGuidRawHex"])) == 16
            assert {"Flags", "Status", "Base", "ImageSize", "TimeDateStamp", "Checksum", "PdbAge",
                    "Path", "PdbName"} <= evidence[image].keys()
    if "640x480" in path.stem or ("1024x768" in path.stem and "full" not in path.stem):
        assert fields["Parameter2"] == fields["Parameter3"] == 0
        assert fields["Cpu"] == 7 and fields["ProcessId"] == 644
        assert fields["CandidateModule"]["Text"] == "example.sys"
        assert fields["StopCodeName"]["Text"] == "DRIVER_IRQL_NOT_LESS_OR_EQUAL"
        assert fields["ParameterRoles"][3]["Text"] == "INSTRUCTION"
        assert evidence["Version"] == 1 and evidence["Environment"]["Build"] == 26100
        assert evidence["Environment"]["Ubr"] == 2605 and evidence["Environment"]["UbrStatus"] == 0
        assert evidence["Environment"]["CodeIntegrity"]["ENABLED"] is True
        assert evidence["Environment"]["CodeIntegrity"]["HVCI_KMCI"] is True
        assert evidence["Environment"]["CodeIntegrity"]["TESTSIGN"] is False
        assert evidence["Context"]["StackSource"] == 4
        assert evidence["Context"]["StackThreadId"] == 0x4321
        assert evidence["Context"]["StackSampleTime"] == 123450001
        assert evidence["EventsDropped"] == 9 and evidence["EventsOverwritten"] == 7
        assert evidence["EventsDiscarded"] == 2
        for index, name in enumerate(REGISTERS):
            assert evidence["Context"]["Registers"][name] == 0x1122334455660000 + index
        for index, event in enumerate(evidence["Events"]):
            assert event["Sequence"] == 100 + index and event["Text"]["Text"] == f"offline event {index}"
            assert event["Kind"] == index % 5 + 1 and event["Code"] == 0x800 + index
    if "escaped" in path.stem or "full" in path.stem:
        for name, capacity in (("CandidateModule", 64), ("CandidateSource", 64),
                               ("ProcessName", 16), ("FaultMeaning", 64)):
            assert fields[name]["RawHex"] == "80" * capacity
        for image in ("Kernel", "Driver", "Candidate"):
            assert evidence[image]["Path"]["RawHex"] == "ff" * 160
            assert evidence[image]["PdbName"]["RawHex"] == "ff" * 64
            assert evidence[image]["PdbGuidRawHex"] == "ff" * 16
            assert evidence[image]["Base"] == 0xFFFFFFFFFFFFFFFF
        assert evidence["Environment"]["HypervisorVendor"]["RawHex"] == "ff" * 16
        assert evidence["Environment"]["DriverBuild"]["RawHex"] == "ff" * 48
        assert evidence["Environment"]["UbrStatus"] == 0xFFFFFFFF
        assert evidence["Environment"]["CodeIntegrity"]["Available"] is False
        assert evidence["Environment"]["CodeIntegrity"]["HVCI_KMCI"] is None
        assert "CONTEXT_RACE" in evidence["FlagNames"]
        assert evidence["Context"]["StackCount"] == evidence["EventCount"] == 0xFFFFFFFF
        assert all(value == 0xFFFFFFFFFFFFFFFF for value in evidence["Context"]["Registers"].values())
        assert all(value == 0xFFFFFFFFFFFFFFFF for value in evidence["Context"]["Stack"])
        for event in evidence["Events"]:
            assert event["Text"]["RawHex"] == "ff" * 64
            assert event["Sequence"] == event["Time"] == 0xFFFFFFFFFFFFFFFF
            assert event["Status"] == event["Flags"] == 0xFFFFFFFF
    if "640x480" in path.stem:
        negative_tests(decoded, packet)
    cv2.imwrite(str(path.with_suffix(".png")), pixels)
    print(f"PASS {path.name}: binary={len(packet)} bytes, Base45={len(decoded)} chars, complete QR round-trip")


if __name__ == "__main__":
    # 只访问仓库已有output目录中的回放结果，不连接驱动、数据库或网络端点。
    root = Path(__file__).resolve().parents[2] / "output"
    for scene in ("empty", "640x480", "1024x768", "maximum", "escaped", "full_1024x768"):
        validate(root / f"bugcheck_linux_{scene}.ppm")
