#!/usr/bin/env python3
"""Exercise the production R0 registry client with a recording IOCTL transport.

The shared Windows ABI, client method bodies, result structs, and declarations
come from production files. No registry, driver, or GUI is opened. Rejected
targets must never reach the transport; transmitted targets and bytes must be
exact, and partial responses must never be presented as complete values.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid


ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / "Ksword5.1/Ksword5.1/ArkDriverClient"

HARNESS = r"""
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "driver/KswordArkRegistryIoctl.h"
static_assert(sizeof(unsigned long) == 4 && sizeof(wchar_t) == 2, "Windows protocol ABI required");
namespace ksword::ark {
/*TYPES*/
class DriverHandle;
class DriverClient {
public:
/*DECLARATIONS*/
};
static unsigned calls, cases;
static unsigned long lastCode;
static std::vector<unsigned char> transmitted;
static std::function<void(unsigned long, void*, unsigned long)> fillResponse;
static bool transportOk = true;
static unsigned shortResponse;
IoResult DriverClient::deviceIoControl(unsigned long code, void* input, unsigned long inputBytes,
                                       void* output, unsigned long outputBytes, DriverHandle*) const {
    ++calls; lastCode = code;
    transmitted.assign(static_cast<const unsigned char*>(input), static_cast<const unsigned char*>(input) + inputBytes);
    std::memset(output, 0, outputBytes);
    if (fillResponse) fillResponse(code, output, outputBytes);
    else {
        auto* response = static_cast<KSWORD_ARK_REGISTRY_OPERATION_RESPONSE*>(output);
        response->version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        response->status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_SUCCESS;
    }
    IoResult result{}; result.ok = transportOk; result.win32Error = transportOk ? 0 : ERROR_ACCESS_DENIED;
    result.bytesReturned = outputBytes - shortResponse; return result;
}
}
/*PRODUCTION*/
using namespace ksword::ark;
static void reset() {
    calls = 0; transmitted.clear(); fillResponse = nullptr; transportOk = true; shortResponse = 0;
}
template<typename Result> static void rejected(const Result& result, unsigned long error) {
    assert(!result.io.ok && result.io.win32Error == error && calls == 0);
    assert(!result.io.message.empty()); ++cases;
}
static void readResponse(unsigned long, void* output, unsigned long bytes) {
    assert(bytes == sizeof(KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE));
    auto* response = static_cast<KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE*>(output);
    response->version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_REGISTRY_READ_STATUS_SUCCESS;
    response->valueType = REG_BINARY; response->requiredBytes = 4; response->dataBytes = 4;
    for (unsigned index = 0; index < KSWORD_ARK_REGISTRY_DATA_MAX_BYTES; ++index)
        response->data[index] = static_cast<unsigned char>(index % 251);
}
static void enumResponse(unsigned long, void* output, unsigned long bytes) {
    assert(bytes == sizeof(KSWORD_ARK_ENUM_REGISTRY_KEY_RESPONSE));
    auto* response = static_cast<KSWORD_ARK_ENUM_REGISTRY_KEY_RESPONSE*>(output);
    response->version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_SUCCESS;
    response->subKeyCount = 1; response->returnedSubKeyCount = 1; response->subKeys[0].name[0] = L'K';
    response->valueCount = 1; response->returnedValueCount = 1;
    auto& value = response->values[0]; value.valueType = REG_BINARY;
    value.dataBytes = 4; value.requiredBytes = 4; // A valid default value has no name flag.
    for (unsigned index = 0; index < KSWORD_ARK_REGISTRY_ENUM_VALUE_DATA_MAX_BYTES; ++index)
        value.data[index] = static_cast<unsigned char>(index % 251);
}
int main() {
    DriverClient client;
    const std::wstring key = L"\\REGISTRY\\MACHINE\\SOFTWARE\\KSwordTest";
    const std::wstring longKey(KSWORD_ARK_REGISTRY_PATH_CHARS, L'K');
    const std::wstring longValue(KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS, L'V');
    const std::wstring nullName(L"a\0b", 3);
    const std::wstring nullKey = key + std::wstring(1, L'\0') + L"Other";
    const std::vector<std::uint8_t> data = {0, 1, 2, 255};
    for (const auto& path : {longKey, nullKey}) {
        const unsigned long error = path == longKey ? ERROR_FILENAME_EXCED_RANGE : ERROR_INVALID_NAME;
        reset(); rejected(client.readRegistryValue(path, L"v"), error);
        reset(); rejected(client.enumerateRegistryKey(path), error);
        reset(); rejected(client.setRegistryValue(path, L"v", REG_BINARY, data), error);
        reset(); rejected(client.deleteRegistryValue(path, L"v"), error);
        reset(); rejected(client.createRegistryKey(path), error);
        reset(); rejected(client.deleteRegistryKey(path), error);
        reset(); rejected(client.renameRegistryValue(path, L"v", L"w"), error);
        reset(); rejected(client.renameRegistryKey(path, L"w"), error);
    }
    for (const auto& name : {longValue, nullName}) {
        const unsigned long error = name == longValue ? ERROR_FILENAME_EXCED_RANGE : ERROR_INVALID_NAME;
        reset(); rejected(client.readRegistryValue(key, name), error);
        reset(); rejected(client.setRegistryValue(key, name, REG_BINARY, data), error);
        reset(); rejected(client.deleteRegistryValue(key, name), error);
        reset(); rejected(client.renameRegistryValue(key, name, L"w"), error);
        reset(); rejected(client.renameRegistryValue(key, L"v", name), error);
        reset(); rejected(client.renameRegistryKey(key, name), error);
    }
    for (const unsigned length : {4097U, 65536U}) {
        reset(); rejected(client.setRegistryValue(key, L"v", REG_BINARY,
            std::vector<std::uint8_t>(length, 0xA5)), ERROR_INSUFFICIENT_BUFFER);
    }
    for (const unsigned length : {0U, 4U, 4095U, 4096U}) {
        reset(); std::vector<std::uint8_t> payload(length);
        for (unsigned index = 0; index < length; ++index) payload[index] = static_cast<unsigned char>(index % 251);
        const auto result = client.setRegistryValue(key, L"v", REG_BINARY, payload);
        assert(result.io.ok && result.status == KSWORD_ARK_REGISTRY_OPERATION_STATUS_SUCCESS && calls == 1);
        assert(lastCode == IOCTL_KSWORD_ARK_SET_REGISTRY_VALUE && transmitted.size() == sizeof(KSWORD_ARK_SET_REGISTRY_VALUE_REQUEST));
        const auto* request = reinterpret_cast<const KSWORD_ARK_SET_REGISTRY_VALUE_REQUEST*>(transmitted.data());
        assert(std::wstring(request->keyPath) == key && std::wstring(request->valueName) == L"v");
        assert(request->dataBytes == length && request->valueType == REG_BINARY);
        assert(std::equal(payload.begin(), payload.end(), request->data)); ++cases;
    }
    for (const auto& name : {std::wstring(), std::wstring(255, L'V')}) {
        reset(); const auto result = client.setRegistryValue(std::wstring(511, L'K'), name, REG_BINARY, data);
        assert(result.io.ok && calls == 1);
        const auto* request = reinterpret_cast<const KSWORD_ARK_SET_REGISTRY_VALUE_REQUEST*>(transmitted.data());
        assert(std::wstring(request->keyPath).size() == 511 && std::wstring(request->valueName) == name);
        assert((request->flags & KSWORD_ARK_REGISTRY_SET_FLAG_VALUE_NAME_PRESENT) == (name.empty() ? 0UL : 1UL)); ++cases;
    }
    for (const unsigned long maximum : {0UL, 4UL, 4096UL, 65536UL}) {
        reset(); fillResponse = readResponse; const auto result = client.readRegistryValue(key, L"", maximum);
        assert(result.io.ok && result.status == KSWORD_ARK_REGISTRY_READ_STATUS_SUCCESS);
        assert(result.requiredBytes == 4 && result.dataBytes == 4 && result.data.size() == 4);
        const auto* request = reinterpret_cast<const KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST*>(transmitted.data());
        assert(request->maxDataBytes == maximum && request->flags == 0 && std::wstring(request->keyPath) == key); ++cases;
    }
    for (const unsigned long required : {5UL, 4096UL, 8192UL}) {
        reset(); fillResponse = [required](unsigned long code, void* output, unsigned long bytes) {
            readResponse(code, output, bytes); static_cast<KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE*>(output)->requiredBytes = required;
        };
        const auto result = client.readRegistryValue(key, L"v");
        assert(result.io.ok && result.status == KSWORD_ARK_REGISTRY_READ_STATUS_BUFFER_TOO_SMALL);
        assert(result.requiredBytes == required && result.dataBytes == result.data.size() && result.data.size() == 4); ++cases;
    }
    reset(); fillResponse = [](unsigned long code, void* output, unsigned long bytes) {
        readResponse(code, output, bytes); auto* response = static_cast<KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE*>(output);
        response->dataBytes = 6000; response->requiredBytes = 6000;
    };
    { const auto result = client.readRegistryValue(key, L"v");
      assert(result.io.ok && result.status == KSWORD_ARK_REGISTRY_READ_STATUS_BUFFER_TOO_SMALL);
      assert(result.dataBytes == 4096 && result.data.size() == 4096 && result.requiredBytes == 6000); ++cases; }
    reset(); fillResponse = enumResponse;
    { const auto result = client.enumerateRegistryKey(key);
      assert(result.io.ok && result.status == KSWORD_ARK_REGISTRY_ENUM_STATUS_SUCCESS);
      assert(result.subKeys.size() == 1 && result.subKeys[0].name == L"K");
      assert(result.values.size() == 1 && result.values[0].name.empty() && result.values[0].data.size() == 4); ++cases; }
    for (const unsigned variant : {0U, 1U, 2U, 3U, 4U, 5U, 6U}) {
        reset(); fillResponse = [variant](unsigned long code, void* output, unsigned long bytes) {
            enumResponse(code, output, bytes); auto* response = static_cast<KSWORD_ARK_ENUM_REGISTRY_KEY_RESPONSE*>(output);
            if (variant == 0) response->subKeyCount = 2;
            if (variant == 1) response->valueCount = 2;
            if (variant == 2) response->lastStatus = static_cast<long>(0xC0000022UL);
            if (variant == 3) response->values[0].requiredBytes = 8192;
            if (variant == 4) { response->values[0].dataBytes = 8192; response->values[0].requiredBytes = 8192; }
            if (variant == 5) { response->returnedSubKeyCount = 300; response->subKeyCount = 300; }
            if (variant == 6) { response->returnedValueCount = 300; response->valueCount = 300; }
        };
        const auto result = client.enumerateRegistryKey(key);
        assert(result.io.ok && result.status == KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL);
        assert(result.returnedSubKeyCount == result.subKeys.size() && result.returnedValueCount == result.values.size());
        for (const auto& value : result.values) { assert(value.dataBytes == value.data.size() && value.data.size() <= 1024); }
        ++cases;
    }
    for (const unsigned variant : {0U, 1U, 2U, 3U}) {
        reset(); fillResponse = [variant](unsigned long code, void* output, unsigned long bytes) {
            enumResponse(code, output, bytes); auto* response = static_cast<KSWORD_ARK_ENUM_REGISTRY_KEY_RESPONSE*>(output);
            if (variant == 0) { response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
                std::fill_n(response->subKeys[0].name, 255, L'K'); }
            if (variant == 1) { response->status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
                std::fill_n(response->values[0].name, 255, L'V'); response->values[0].flags = 1; }
            if (variant == 2) { std::fill_n(response->values[0].name, 256, L'V'); response->values[0].flags = 1; }
            if (variant == 3) { response->values[0].name[0] = L'V'; } // Name and flag disagree; never target the default value.
        };
        const auto result = client.enumerateRegistryKey(key);
        assert(result.io.ok && result.status == KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL);
        assert(variant == 0 ? result.subKeys.empty() : result.values.empty()); ++cases;
    }
    for (const bool wrongVersion : {false, true}) {
        reset(); if (!wrongVersion) shortResponse = 1;
        fillResponse = [wrongVersion](unsigned long code, void* output, unsigned long bytes) {
            readResponse(code, output, bytes); if (wrongVersion) static_cast<KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE*>(output)->version = 99;
        };
        { const auto result = client.readRegistryValue(key, L"v"); assert(!result.io.ok && result.io.win32Error == ERROR_INVALID_DATA && result.data.empty()); ++cases; }
        reset(); if (!wrongVersion) shortResponse = 1;
        fillResponse = [wrongVersion](unsigned long code, void* output, unsigned long bytes) {
            enumResponse(code, output, bytes); if (wrongVersion) static_cast<KSWORD_ARK_ENUM_REGISTRY_KEY_RESPONSE*>(output)->version = 99;
        };
        { const auto result = client.enumerateRegistryKey(key); assert(!result.io.ok && result.io.win32Error == ERROR_INVALID_DATA && result.values.empty()); ++cases; }
        reset(); if (!wrongVersion) shortResponse = 1;
        fillResponse = [wrongVersion](unsigned long, void* output, unsigned long) {
            auto* response = static_cast<KSWORD_ARK_REGISTRY_OPERATION_RESPONSE*>(output);
            response->version = wrongVersion ? 99 : KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
            response->status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_SUCCESS;
        };
        { const auto result = client.setRegistryValue(key, L"v", REG_BINARY, data);
          assert(!result.io.ok && result.io.win32Error == ERROR_INVALID_DATA && result.status == KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED); ++cases; }
    }
    reset(); transportOk = false;
    { const auto result = client.setRegistryValue(key, L"v", REG_BINARY, data);
      assert(!result.io.ok && result.io.win32Error == ERROR_ACCESS_DENIED && calls == 1); ++cases; }
    std::printf("registry-driver-client regression: %u cases passed; rejected requests sent no IOCTL\n", cases);
}
"""


def production() -> str:
    types_text = (CLIENT / "ArkDriverTypes.h").read_text(encoding="utf-8-sig")
    names = ["IoResult", "RegistryReadResult", "RegistrySubKeyEntry", "RegistryValueEntry", "RegistryEnumResult", "RegistryOperationResult"]
    types = []
    for name in names:
        match = re.search(rf"(?ms)^    struct {name}\s*\n    \{{.*?^    \}};", types_text)
        if not match:
            raise SystemExit(f"Production result struct missing: {name}")
        types.append(match.group(0))
    header = (CLIENT / "ArkDriverClient.h").read_text(encoding="utf-8-sig")
    declarations = re.findall(r"(?m)^        (?:Registry\w+Result \w+Registry\w*\([^\n]*\) const;)", header)
    ioctl = re.search(r"(?ms)^        IoResult deviceIoControl\(.*?\) const;", header)
    if len(declarations) != 8 or not ioctl:
        raise SystemExit("Production client declarations changed; update the harness explicitly.")
    source = (CLIENT / "ArkDriverRegistry.cpp").read_text(encoding="utf-8-sig")
    if source.count('#include "ArkDriverClient.h"') != 1:
        raise SystemExit("Production client include changed; update the harness explicitly.")
    source = source.replace('#include "ArkDriverClient.h"', "", 1)
    return HARNESS.replace("/*TYPES*/", "\n".join(types)).replace("/*DECLARATIONS*/", "\n".join(declarations) + "\n" + ioctl.group(0)).replace("/*PRODUCTION*/", source)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit(f"C++ compiler unavailable: {args.cxx}")
    environment = os.environ.copy()
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
    temporary_root = ROOT / "work/registry-client-tests"
    temporary_root.mkdir(parents=True, exist_ok=True)
    temporary = temporary_root / f"case-{uuid.uuid4().hex}"
    temporary.mkdir()
    try:
        source = temporary / "regression.cpp"
        binary = temporary / "regression.exe"
        source.write_text(production(), encoding="utf-8")
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "shared"), str(source), "-o", str(binary)], check=True, env=environment)
        subprocess.run([str(binary)], check=True, env=environment)
    finally:
        if temporary.resolve().is_relative_to(temporary_root.resolve()):
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
