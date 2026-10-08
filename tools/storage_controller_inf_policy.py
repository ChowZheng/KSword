"""Check the shipped controller INF's installation and unified-image policy.

This is a source-package gate, not a substitute for InfVerif, signing, or a
physical PnP installation. No packages are installed and no storage is opened.
"""
from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re
import sys


class PolicyError(ValueError):
    pass


def _uncomment(line: str) -> str:
    quoted = False
    for index, char in enumerate(line):
        if char == '"':
            quoted = not quoted
        elif char == ";" and not quoted:
            return line[:index].strip()
    return line.strip()


def _fields(value: str) -> list[str]:
    return next(csv.reader([value], skipinitialspace=True))


def _sections(source: str) -> dict[str, list[tuple[str, str]]]:
    result: dict[str, list[tuple[str, str]]] = {}
    section: list[tuple[str, str]] | None = None
    continuation = ""
    for line in source.splitlines():
        line = _uncomment(line)
        if not line:
            continue
        line = continuation + line
        if line.endswith("\\"):
            continuation = line[:-1]
            continue
        continuation = ""
        if line.startswith("[") and line.endswith("]"):
            section = result.setdefault(line[1:-1].strip().casefold(), [])
        elif section is None:
            raise PolicyError("Entry outside an INF section")
        else:
            key, separator, value = line.partition("=")
            section.append((key.strip(), value.strip() if separator else ""))
    if continuation:
        raise PolicyError("Unfinished INF line continuation")
    return result


def check_policy(source: str) -> int:
    sections = _sections(source)
    strings = {key.casefold(): _fields(value)[0]
               for key, value in sections.get("strings", [])}

    def expand(value: str) -> str:
        # Numeric tokens are INF directory IDs, not entries in [Strings].
        for _ in range(len(strings) + 1):
            changed = re.sub(r"%([^%]+)%", lambda match:
                             match[0] if match[1].isdigit() else
                             strings.get(match[1].casefold(), match[0]), value)
            if changed == value:
                if re.search(r"%[^%\d][^%]*%", changed):
                    raise PolicyError(f"Unresolved INF string: {changed}")
                return changed
            value = changed
        raise PolicyError("Cyclic INF string expansion")

    def entries(name: str) -> list[tuple[str, str]]:
        if name.casefold() not in sections:
            raise PolicyError(f"Missing INF section: {name}")
        return sections[name.casefold()]

    def values(name: str, key: str) -> list[list[str]]:
        return [_fields(expand(value)) for actual, value in entries(name)
                if actual.casefold() == key.casefold()]

    def single(name: str, key: str) -> str:
        rows = values(name, key)
        if len(rows) != 1 or len(rows[0]) != 1:
            raise PolicyError(f"Expected one {name}.{key} value")
        return rows[0][0]

    interactive = set()
    for key, value in entries("ControlFlags"):
        if key.casefold() == "interactiveinstall":
            interactive.update(item.casefold() for item in _fields(expand(value)))

    model_sections = []
    for _, value in entries("Manufacturer"):
        row = _fields(expand(value))
        if not row or not row[0]:
            raise PolicyError("Empty Manufacturer model-section reference")
        model_sections.extend(f"{row[0]}.{suffix}" for suffix in row[1:])
        if len(row) == 1:
            model_sections.append(row[0])
    if not model_sections:
        raise PolicyError("Manufacturer does not reference any Models section")

    model_count = 0
    for name in model_sections:
        for _, value in entries(name):
            model = _fields(expand(value))
            if len(model) < 2 or not model[0] or any(not item for item in model[1:]):
                raise PolicyError(f"Invalid model in {name}")
            model_count += 1
            for identifier in model[1:]:
                if identifier.casefold() not in interactive:
                    raise PolicyError(f"Model ID lacks InteractiveInstall: {identifier}")

            # Resolve the actual DDInstall sections selected by this package,
            # rather than assuming every model names Controller_Install.
            install = model[0]
            entries(install + ".NT")
            services = values(install + ".NT.Services", "AddService")
            if len(services) != 1 or len(services[0]) < 3:
                raise PolicyError(f"Expected one unified function service for {install}")
            service, flags, service_install = services[0][:3]
            if service.casefold() != "kswordark" or int(flags, 0) & 0x2 == 0:
                raise PolicyError(f"Model uses an independent/unassociated service: {service}")
            if int(single(service_install, "ServiceType"), 0) != 1:
                raise PolicyError("Controller service must be a kernel driver")
            binary = single(service_install, "ServiceBinary")
            if not re.fullmatch(r"%13%[\\/]KswordARK\.sys", binary, re.I):
                raise PolicyError(f"Controller service uses an independent image: {binary}")
            copies = values(install + ".NT", "CopyFiles")
            if not copies:
                raise PolicyError("Model does not copy the unified driver")
            copied = []
            for copy in copies:
                for target in copy:
                    if target.startswith("@"):
                        copied.append(target[1:])
                    else:
                        copied.extend(_fields(expand(key))[0] for key, _ in entries(target))
            if not copied or any(item.casefold() != "kswordark.sys" for item in copied):
                raise PolicyError(f"Model copies an independent image: {copied}")

    if model_count == 0:
        raise PolicyError("Empty Models sections")
    sources = [key.casefold() for key, _ in entries("SourceDisksFiles")]
    if sources != ["kswordark.sys"]:
        raise PolicyError(f"Package sources include an independent image: {sources}")
    return model_count


def self_test(source: str) -> int:
    """Falsify the real parsed policy, including future model additions."""
    mutations = {
        "missing interactive directive": re.sub(
            r"(?im)^InteractiveInstall=.*$", "", source),
        "omitted model ID": source.replace(
            "InteractiveInstall=PCI\\CC_010601,PCI\\CC_010802,PCI\\CC_0101",
            "InteractiveInstall=PCI\\CC_010601,PCI\\CC_010802"),
        "new unprotected model": source.replace(
            "[ControlFlags]", "%IdeDeviceDesc%=Controller_Install,PCI\\CC_010104\n\n[ControlFlags]"),
        "independent service": source.replace(
            "AddService=KswordARK,", "AddService=KswordARKController,"),
        "independent service image": source.replace(
            "ServiceBinary=%13%\\KswordARK.sys", "ServiceBinary=%13%\\KswordARKController.sys"),
        "independent copied image": source.replace(
            "[Driver_Copy]\nKswordARK.sys", "[Driver_Copy]\nKswordARKController.sys"),
        "independent packaged image": source.replace(
            "[SourceDisksFiles]\nKswordARK.sys=1,,",
            "[SourceDisksFiles]\nKswordARK.sys=1,,\nKswordARKController.sys=1,,"),
        "unassociated function service": source.replace(
            "AddService=KswordARK,0x0000003A", "AddService=KswordARK,0x00000038"),
    }
    for name, mutation in mutations.items():
        if mutation == source:
            raise PolicyError(f"Mutation did not change the actual INF: {name}")
        try:
            check_policy(mutation)
        except PolicyError:
            continue
        raise PolicyError(f"Policy accepted adversarial mutation: {name}")
    # INF is case-insensitive; formatting and continued lists cannot weaken
    # the checks or create a false rejection of the shipped package.
    check_policy(source.lower())
    check_policy(source.replace(
        "InteractiveInstall=PCI\\CC_010601,PCI\\CC_010802,PCI\\CC_0101",
        "InteractiveInstall=PCI\\CC_010601,\\\n PCI\\CC_010802,PCI\\CC_0101 ; required"))
    return len(mutations)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inf", type=Path, default=Path(__file__).resolve().parents[1] /
                        "KswordARKDriver/KswordARKStorageController.inf")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        source = args.inf.read_text(encoding="utf-8-sig")
        models = check_policy(source)
        mutations = self_test(source) if args.self_test else 0
    except (PolicyError, OSError, ValueError, csv.Error) as error:
        print(f"Storage controller INF policy FAIL: {error}", file=sys.stderr)
        return 1
    print(f"Storage controller INF policy PASS: {models} protected models; "
          f"{mutations} adversarial mutations rejected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
