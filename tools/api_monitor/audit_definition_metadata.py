"""Check parameter directions against local SDK/native declarations; never rewrite the catalog."""
import argparse
import json
from pathlib import Path
import re


def parameter_length(param):
    """Extract only a simple SAL parameter reference; expressions remain C++ handler responsibilities."""
    match = re.search(r"\b_((?:In_reads|Out_writes|Inout_updates|Outptr_result_(?:byte)?buffer)[A-Za-z_]*)_\s*\(([^()]*)\)", param)
    if not match:
        return None
    source = re.fullmatch(r"\s*(\*?)\s*([A-Za-z_][A-Za-z_0-9]*)\s*", match.group(2).split(",")[0])
    if not source:
        return None
    return {"parameter": source.group(2), "indirect": bool(source.group(1)),
            "unit": "bytes" if "bytes" in match.group(1) or "bytebuffer" in match.group(1) else "elements"}


def declaration_directions(directory, names, details=False):
    aliases = {"LPFN_" + name.upper(): name for name in names if name in {
        "AcceptEx", "ConnectEx", "WSARecvMsg", "WSASendMsg", "TransmitFile", "TransmitPackets", "GetAcceptExSockaddrs"
    }}
    matcher = re.compile(r"\b(" + "|".join(sorted(names, key=len, reverse=True)) + r")\s*\("
                         + (r"|\b(" + "|".join(aliases) + r")\s*\)\s*\(" if aliases else ""))
    found = {}
    for header in sorted(directory.rglob("*.h")):
        source = header.read_text(encoding="utf-8-sig", errors="replace")
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
        for match in matcher.finditer(source):
            cursor, start, depth, params = match.end(), match.end(), 1, []
            while cursor < len(source) and depth:
                char = source[cursor]
                if char == "(":
                    depth += 1
                elif char == ")":
                    depth -= 1
                elif char == "," and depth == 1:
                    params.append(source[start:cursor])
                    start = cursor + 1
                cursor += 1
            if depth or not source[cursor:].lstrip().startswith(";"):
                continue
            params.append(source[start:cursor - 1])
            if len(params) == 1 and params[0].strip() in {"", "void", "VOID"}:
                params = []
            directions = []
            for param in params:
                sal = re.search(r"\b_(Inout|Outptr|Out|In|Reserved)(?:_|[A-Za-z]+_)", param)
                directions.append(None if not sal else {
                    "Inout": "inout", "Outptr": "out", "Out": "out", "In": "in", "Reserved": "in"
                }[sal.group(1)])
            name = match.group(1) or aliases[match.group(2)]
            record = {"directions": directions, "parameters": params}
            if name not in found or sum(d is not None for d in directions) > sum(d is not None for d in found[name]["directions"]):
                found[name] = record
    return found if details else {name: record["directions"] for name, record in found.items()}


def audit(catalog, declarations):
    checked, differences = 0, []
    for api in catalog["apis"]:
        directions = declarations.get(api["export"])
        # SSPI's W declarations contain mutually exclusive SECURITY_KERNEL string parameters.
        # Their user-mode A declarations have the same parameter directions and arity.
        if api["export"] in {"AcquireCredentialsHandleW", "InitializeSecurityContextW"} and (
                directions is None or len(directions) != len(api["parameters"])):
            directions = declarations.get(api["export"][:-1] + "A")
        if directions is None or len(directions) != len(api["parameters"]):
            continue
        for param, direction in zip(api["parameters"], directions):
            if direction:
                checked += 1
                if param["direction"] != direction:
                    differences.append(f"{api['export']}.{param['name']}: JSON={param['direction']} declaration={direction}")
    return checked, differences


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--headers", type=Path, action="append", required=True)
    parser.add_argument("--definitions", type=Path,
                        default=Path(__file__).resolve().parents[2] / "APIMonitor_x64/api_monitor_definitions.json")
    args = parser.parse_args()
    catalog = json.loads(args.definitions.read_bytes())
    declarations = {}
    for directory in args.headers:
        if not directory.is_dir():
            parser.error(f"header directory unavailable: {directory}")
        declarations.update(declaration_directions(directory, {a["export"] for a in catalog["apis"]}))
    checked, differences = audit(catalog, declarations)
    for difference in differences:
        print(difference)
    print(f"METADATA_PARAMETERS_CHECKED={checked} DIFFERENCES={len(differences)}")
    raise SystemExit(bool(differences))


if __name__ == "__main__":
    main()
