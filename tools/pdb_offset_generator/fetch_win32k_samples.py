#!/usr/bin/env python3
"""从用户指定样本站点只读下载精确 AMD64 win32k PE 配对并保存验证清单。"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tempfile
from urllib.parse import urljoin
from uuid import UUID

import pefile
import requests


AMD64 = 0x8664
FILES = ("win32kfull.sys", "win32kbase.sys")
VERSION_RE = re.compile(r"^10\.0\.\d+\.\d+$")


def canonical_version(raw: str) -> str:
    value = str(raw).split(" ", 1)[0]
    return value if VERSION_RE.fullmatch(value) else ""


def read_metadata(session: requests.Session, site: str, filename: str,
                  cached: Path | None = None) -> dict:
    if cached:
        source = cached.read_bytes()
    else:
        response = session.get(urljoin(site, "/api/metadata"),
            params={"file": filename, "arch": "x64"}, timeout=45)
        response.raise_for_status()
        source = response.content
    if source.startswith(b"\x1f\x8b"):
        source = gzip.decompress(source)
    result = json.loads(source)
    if not isinstance(result, dict):
        raise ValueError(f"metadata_not_object:{filename}")
    return result


def select_record(metadata: dict, version: str, filename: str) -> tuple[str, dict, dict]:
    candidates = []
    for digest, record in metadata.items():
        info = record.get("fileInfo", {}) if isinstance(record, dict) else {}
        if not isinstance(info, dict) or info.get("machineType") != AMD64:
            continue
        if canonical_version(info.get("version", "")) != version:
            continue
        if not re.fullmatch(r"[A-Fa-f0-9]{64}", digest):
            continue
        candidates.append((digest.lower(), info, record))
    if len(candidates) != 1:
        raise ValueError(f"metadata_{filename}_version_missing_or_ambiguous:{version}:count={len(candidates)}")
    return candidates[0]


def resolve_identity(session: requests.Session, site: str, filename: str,
                     version: str, digest: str, info: dict) -> dict:
    record = {
        "file": filename,
        "arch": "x64",
        "hash": digest,
        "timestamp": int(info["timestamp"]),
        "virtualSize": int(info["virtualSize"]),
        "version": version,
    }
    response = session.post(urljoin(site, "/api/resolve"), json=record, timeout=60)
    if response.status_code != 200:
        try:
            detail = response.json()
        except ValueError:
            detail = {"error": response.text[:250]}
        raise ValueError(f"resolve_{filename}_http_{response.status_code}:"
                         f"{detail.get('code', detail.get('error', 'unknown'))}")
    payload = response.json()
    pdb = payload.get("pdb") if isinstance(payload, dict) else None
    if not isinstance(pdb, dict):
        raise ValueError(f"resolve_{filename}_pdb_missing")
    guid = re.sub(r"[{}-]", "", str(pdb.get("guid", ""))).upper()
    age = pdb.get("age")
    if not re.fullmatch(r"[A-F0-9]{32}", guid) or not isinstance(age, int) or age < 0:
        raise ValueError(f"resolve_{filename}_rsds_invalid")
    return {"guid": guid, "age": age, "pdbName": str(pdb.get("name", ""))}


def inspect_pe(data: bytes, filename: str, version: str,
               digest: str, info: dict, resolved: dict) -> dict:
    if hashlib.sha256(data).hexdigest().lower() != digest.lower():
        raise ValueError(f"{filename}_sha256_mismatch")
    if len(data) != int(info["size"]):
        raise ValueError(f"{filename}_size_mismatch")
    image = pefile.PE(data=data, fast_load=False)
    try:
        if image.FILE_HEADER.Machine != AMD64 or image.OPTIONAL_HEADER.Magic != 0x20B:
            raise ValueError(f"{filename}_not_amd64_pe32_plus")
        if image.FILE_HEADER.TimeDateStamp != int(info["timestamp"]) or \
                image.OPTIONAL_HEADER.SizeOfImage != int(info["virtualSize"]):
            raise ValueError(f"{filename}_pe_identity_mismatch")
        fixed = getattr(image, "VS_FIXEDFILEINFO", [])
        if len(fixed) != 1:
            raise ValueError(f"{filename}_fixed_version_missing_or_ambiguous")
        v = fixed[0]
        actual_version = (f"{v.FileVersionMS >> 16}.{v.FileVersionMS & 0xFFFF}."
                          f"{v.FileVersionLS >> 16}.{v.FileVersionLS & 0xFFFF}")
        if actual_version != version:
            raise ValueError(f"{filename}_fixed_version_mismatch:{actual_version}")
        rsds = []
        for entry in getattr(image, "DIRECTORY_ENTRY_DEBUG", []):
            if entry.struct.Type != 2 or entry.struct.SizeOfData < 24:
                continue
            raw = image.get_data(entry.struct.AddressOfRawData, entry.struct.SizeOfData)
            if raw[:4] == b"RSDS" and len(raw) >= 24:
                rsds.append({
                    "guid": str(UUID(bytes_le=raw[4:20])).replace("-", "").upper(),
                    "age": struct.unpack_from("<I", raw, 20)[0],
                    "pdbName": raw[24:].split(b"\0", 1)[0].decode("utf-8", "replace"),
                })
        if len(rsds) != 1 or rsds[0]["guid"] != resolved["guid"] or \
                rsds[0]["age"] != resolved["age"] or \
                not rsds[0]["pdbName"].lower().endswith(filename.replace(".sys", ".pdb")):
            raise ValueError(f"{filename}_rsds_mismatch")
        return {
            "sha256": digest.lower(),
            "size": len(data),
            "machineType": f"0x{AMD64:04X}",
            "timestamp": int(info["timestamp"]),
            "virtualSize": int(info["virtualSize"]),
            "version": actual_version,
            "rsds": rsds[0],
        }
    finally:
        image.close()


def download_verified(session: requests.Session, site: str, filename: str,
                      version: str, digest: str, info: dict, resolved: dict,
                      target: Path, budget: dict, max_bytes: int) -> dict:
    if target.is_file():
        data = target.read_bytes()
        verified = inspect_pe(data, filename, version, digest, info, resolved)
        verified["path"] = str(target.resolve())
        verified["reusedExisting"] = True
        return verified
    if int(info["size"]) > max_bytes - budget["downloadedBytes"]:
        raise ValueError(f"download_budget_exceeded:{filename}:{version}")
    parameters = {
        "file": filename,
        "timestamp": str(info["timestamp"]),
        "virtualSize": str(info["virtualSize"]),
        "guid": resolved["guid"],
        "age": str(resolved["age"]),
        "hash": digest,
    }
    handle, temporary_name = tempfile.mkstemp(prefix=f".{filename}.", suffix=".part", dir=target.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(handle, "wb") as output, session.get(
            urljoin(site, "/api/binary"), params=parameters, stream=True,
            timeout=(20, 120)) as response:
            if response.status_code != 200:
                try:
                    detail = response.json()
                except ValueError:
                    detail = {"error": response.text[:250]}
                raise ValueError(f"binary_{filename}_http_{response.status_code}:"
                                 f"{detail.get('code', detail.get('error', 'unknown'))}")
            for chunk in response.iter_content(1024 * 1024):
                if not chunk:
                    continue
                budget["downloadedBytes"] += len(chunk)
                if budget["downloadedBytes"] > max_bytes:
                    raise ValueError(f"download_budget_exceeded:{filename}:{version}")
                output.write(chunk)
        verified = inspect_pe(temporary.read_bytes(), filename, version, digest, info, resolved)
        temporary.replace(target)
        verified["path"] = str(target.resolve())
        verified["reusedExisting"] = False
        return verified
    finally:
        temporary.unlink(missing_ok=True)


def write_manifest(path: Path, manifest: dict) -> None:
    content = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    handle, temp_name = tempfile.mkstemp(prefix=".manifest.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as output:
            output.write(content)
        os.replace(temp_name, path)
    finally:
        Path(temp_name).unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", required=True, help="由用户提供的样本站点根URL")
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("--version", action="append", required=True,
                        help="需要下载的精确文件版本，可重复")
    parser.add_argument("--max-bytes", type=int, default=100_000_000,
                        help="本次网络下载字节上限，默认100MB")
    parser.add_argument("--full-metadata", type=Path,
                        help="可选：已保存的win32kfull元数据JSON/gzip")
    args = parser.parse_args()
    site = args.site.rstrip("/") + "/"
    if not site.startswith(("https://", "http://")):
        parser.error("--site must be HTTP(S)")
    if args.max_bytes <= 0:
        parser.error("--max-bytes must be positive")
    if any(not VERSION_RE.fullmatch(version) for version in args.version):
        parser.error("--version must be exact 10.0.build.revision")
    outdir = args.outdir.resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    session = requests.Session()
    metadata = {
        "win32kfull.sys": read_metadata(session, site, "win32kfull.sys", args.full_metadata),
        "win32kbase.sys": read_metadata(session, site, "win32kbase.sys"),
    }
    manifest_path = outdir / "manifest.json"
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("site") != site or manifest.get("schemaVersion") != 1:
            parser.error("existing manifest belongs to another site or schema")
        manifest["maxDownloadBytes"] = args.max_bytes
    else:
        manifest = {"schemaVersion": 1, "site": site, "maxDownloadBytes": args.max_bytes,
                    "downloadedBytes": 0, "pairs": []}
    for version in args.version:
        pair: dict = {"version": version, "status": "unsupported", "files": {}}
        existing_index = next((index for index, item in enumerate(manifest["pairs"])
                               if item.get("version") == version), None)
        if existing_index is None:
            manifest["pairs"].append(pair)
        else:
            manifest["pairs"][existing_index] = pair
        folder = outdir / version
        folder.mkdir(exist_ok=True)
        try:
            selected = {name: select_record(metadata[name], version, name) for name in FILES}
            full_labels = set(selected["win32kfull.sys"][2].get("windowsVersions", {}))
            base_labels = set(selected["win32kbase.sys"][2].get("windowsVersions", {}))
            if full_labels and base_labels and not (full_labels & base_labels):
                raise ValueError("paired_windows_release_labels_disagree")
            pair["windowsVersions"] = sorted(full_labels & base_labels)
            resolved = {name: resolve_identity(session, site, name, version,
                        selected[name][0], selected[name][1]) for name in FILES}
            needed = sum(int(selected[name][1]["size"]) for name in FILES
                         if not (folder / name).is_file())
            if needed > args.max_bytes - manifest["downloadedBytes"]:
                raise ValueError("paired_download_budget_exceeded")
            for name in FILES:
                digest, info, _ = selected[name]
                verified = download_verified(session, site, name, version,
                    digest, info, resolved[name], folder / name,
                    manifest, args.max_bytes)
                pair["files"][name] = verified
            pair["status"] = "verified"
            print(f"VERIFIED {version} full={pair['files']['win32kfull.sys']['sha256'][:12]} "
                  f"base={pair['files']['win32kbase.sys']['sha256'][:12]}")
        except (OSError, ValueError, requests.RequestException, pefile.PEFormatError) as error:
            pair["reason"] = str(error)
            print(f"UNSUPPORTED {version} {error}")
        write_manifest(manifest_path, manifest)
    return 0 if all(item["status"] == "verified" for item in manifest["pairs"]) else 2


if __name__ == "__main__":
    raise SystemExit(main())
