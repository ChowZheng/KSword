"""Execute production memory-backend functions using tiny Qt/driver shims.

The public entry points and hvmTransfer are extracted verbatim at run time;
the result model, protocol constants and DDMA page-slicing code are real.
No Qt installation, loaded driver or DMA-capable disk is required. These
host checks validate accounting and control flow, not kernel/device access.
Run: python tools/hvm_unit_tests/test_memory_backend.py
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import uuid

from test_memory_transfer import extract_function


ROOT = Path(__file__).resolve().parents[2]
PRODUCTION = ROOT / "Ksword5.1/Ksword5.1/MemoryDock/MemoryAccessBackend.cpp"

QT_SHIM = r'''
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
using qsizetype = std::int64_t;
class QString {
public:
    std::string value;
    QString() = default;
    QString(const char* text) : value(text) {}
    static QString fromStdString(const std::string& text) {
        QString result; result.value = text; return result;
    }
    template<class... Args> QString arg(const Args&...) const { return *this; }
    QString toUpper() const { return *this; }
    QString replace(const QString&, const QString&) const { return *this; }
    void clear() { value.clear(); }
    bool isEmpty() const { return value.empty(); }
    QString& operator+=(const QString& other) { value += other.value; return *this; }
};
#define QStringLiteral(text) QString(text)
class QByteArray {
    std::vector<char> value;
public:
    QByteArray() = default;
    QByteArray(qsizetype length, char fill) : value(static_cast<std::size_t>(length), fill) {}
    QByteArray(const char* text, qsizetype length) : value(text, text + length) {}
    qsizetype size() const { return static_cast<qsizetype>(value.size()); }
    bool isEmpty() const { return value.empty(); }
    const char* constData() const { return value.data(); }
    void reserve(qsizetype length) { value.reserve(static_cast<std::size_t>(length)); }
    void append(const char* text, qsizetype length) { value.insert(value.end(), text, text + length); }
    void append(const QByteArray& other) { value.insert(value.end(), other.value.begin(), other.value.end()); }
    QByteArray mid(qsizetype offset, qsizetype length) const { return QByteArray(constData() + offset, length); }
};
'''


PRELUDE = r'''
#include "MemoryAccessBackend.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#define _KERNEL_MODE 1
#include "KswordArkHvmIoctl.h"
#include "KswordArkMemoryIoctl.h"
#include "KswordArkDdmaPlan.h"
namespace ksword::ark {
struct IoResult { bool ok = true; std::string message = "test transport error"; };
struct HvmMemoryResult {
    IoResult io;
    bool unsupported = false;
    KSWORD_ARK_HVM_MEMORY_RESPONSE response{};
};
struct PhysicalMemoryReadResult { IoResult io; unsigned long readStatus = KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_OK; std::vector<std::uint8_t> data; };
struct VirtualMemoryReadResult { IoResult io; unsigned long readStatus = KSWORD_ARK_MEMORY_READ_STATUS_OK; std::vector<std::uint8_t> data; };
struct PhysicalMemoryWriteResult { IoResult io; unsigned long writeStatus = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_OK; std::uint32_t bytesWritten = 0; };
struct VirtualMemoryWriteResult { IoResult io; unsigned long writeStatus = KSWORD_ARK_MEMORY_WRITE_STATUS_OK; std::uint32_t bytesWritten = 0; };
struct VirtualAddressTranslateResult { IoResult io; bool resolved = true; std::uint64_t physicalAddress = 0; };
struct Call { unsigned long operation; std::uint64_t address; unsigned long length; bool requireWindow; bool confirmed; unsigned long processId; };
static std::vector<Call> calls;
static std::vector<HvmMemoryResult> script;
static unsigned translationCalls;
static unsigned translationFailure;
static unsigned standardCalls;
class DriverClient {
public:
    HvmMemoryResult hvmMemory(unsigned long operation, std::uint64_t address,
        std::uint64_t, unsigned long length, const unsigned char*,
        bool requireWindow, bool confirmed, unsigned long processId = 0) const {
        calls.push_back({operation, address, length, requireWindow, confirmed, processId});
        HvmMemoryResult result;
        if (calls.size() <= script.size()) result = script[calls.size() - 1];
        else {
            result.response.status = KSWORD_ARK_HVM_MEMORY_STATUS_OK;
            result.response.bytesTransferred = length;
            result.response.usedDirectWindow = 1;
        }
        std::memset(result.response.data, static_cast<int>(0x30U + calls.size()), sizeof(result.response.data));
        return result;
    }
    PhysicalMemoryReadResult readPhysicalMemory(std::uint64_t, std::uint32_t length, unsigned long) const {
        ++standardCalls; PhysicalMemoryReadResult result; result.data.assign(length, 0x61); return result;
    }
    VirtualMemoryReadResult readVirtualMemory(std::uint32_t, std::uint64_t, std::uint32_t length, unsigned long) const {
        ++standardCalls; VirtualMemoryReadResult result; result.data.assign(length, 0x62); return result;
    }
    PhysicalMemoryWriteResult writePhysicalMemory(std::uint64_t, const std::vector<std::uint8_t>& payload, unsigned long) const {
        ++standardCalls; PhysicalMemoryWriteResult result; result.bytesWritten = static_cast<std::uint32_t>(payload.size()); return result;
    }
    VirtualMemoryWriteResult writeVirtualMemory(std::uint32_t, std::uint64_t, const std::vector<std::uint8_t>& payload, unsigned long) const {
        ++standardCalls; VirtualMemoryWriteResult result; result.bytesWritten = static_cast<std::uint32_t>(payload.size()); return result;
    }
    VirtualAddressTranslateResult translateVirtualAddress(std::uint32_t, std::uint64_t address) const {
        ++translationCalls;
        VirtualAddressTranslateResult result;
        result.resolved = translationCalls != translationFailure;
        result.physicalAddress = address + 0x100000ULL;
        return result;
    }
};
}
namespace ksword::memory_backend {
static constexpr std::uint64_t kKernelSpaceStart = 0xFFFF800000000000ULL;
static constexpr std::uint64_t kStandardPhysicalReadMax = KSWORD_ARK_MEMORY_PHYSICAL_READ_MAX_BYTES;
static constexpr std::uint64_t kStandardPhysicalWriteMax = KSWORD_ARK_MEMORY_PHYSICAL_WRITE_MAX_BYTES;
static constexpr std::uint64_t kStandardVirtualReadMax = KSWORD_ARK_MEMORY_READ_MAX_BYTES;
static constexpr std::uint64_t kStandardVirtualWriteMax = KSWORD_ARK_MEMORY_WRITE_MAX_BYTES;
static unsigned ddmaCalls;
static unsigned ddmaDirtyCall;
static unsigned userCalls;
static AccessOutcome ddmaReadOnePage(const ksword::ark::DriverClient&, const DdmaSession&,
    std::uint64_t, std::uint32_t length) {
    ++ddmaCalls;
    AccessOutcome outcome; outcome.ok = true; outcome.bytesDone = length;
    outcome.scratchDirty = ddmaCalls == ddmaDirtyCall;
    outcome.data = QByteArray(length, 'q'); return outcome;
}
static AccessOutcome ddmaWriteOnePage(const ksword::ark::DriverClient&, const DdmaSession&,
    std::uint64_t, const QByteArray& payload, bool) {
    ++ddmaCalls; AccessOutcome outcome; outcome.ok = true; outcome.bytesDone = static_cast<std::uint64_t>(payload.size()); return outcome;
}
bool isDdmaUsable(const DdmaSession& session, QString* reason) {
    if (!session.configured && reason != nullptr) *reason = QStringLiteral("unconfigured");
    return session.configured;
}
static AccessOutcome userModeReadVirtual(std::uint32_t, std::uint64_t, std::uint64_t length) {
    ++userCalls; AccessOutcome outcome; outcome.ok = true; outcome.data = QByteArray(static_cast<qsizetype>(length), 'u'); outcome.bytesDone = length; return outcome;
}
static AccessOutcome userModeWriteVirtual(std::uint32_t, std::uint64_t, const QByteArray& payload) {
    ++userCalls; AccessOutcome outcome; outcome.ok = true; outcome.bytesDone = static_cast<std::uint64_t>(payload.size()); return outcome;
}
static AccessOutcome userModeRejectPhysical() { AccessOutcome outcome; outcome.failureText = QStringLiteral("physical unavailable"); return outcome; }
'''


TESTS = r'''
} // namespace ksword::memory_backend
using namespace ksword::memory_backend;
using ksword::ark::calls;
using ksword::ark::script;
static unsigned checks;
static unsigned failures;
static void Check(bool condition, const char* label) {
    ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
static void Reset() {
    calls.clear(); script.clear(); ksword::ark::translationCalls = 0;
    ksword::ark::translationFailure = 0; ksword::ark::standardCalls = 0;
    ddmaCalls = ddmaDirtyCall = userCalls = 0;
}
static ksword::ark::HvmMemoryResult Response(unsigned long status, unsigned long done, bool direct = true) {
    ksword::ark::HvmMemoryResult result;
    result.response.status = status; result.response.bytesTransferred = done;
    result.response.usedDirectWindow = direct ? 1 : 0;
    return result;
}
static void TestHvmSuccessAndPartial() {
    DdmaSession session;
    Reset();
    auto result = readVirtual(MemoryAccessBackend::Hvm, session, 77, 0x4000, 2050);
    Check(result.ok && !result.partial && result.bytesDone == 2050 && result.data.size() == 2050,
          "full multi-chunk HVM read accounting");
    Check(calls.size() == 3 && calls[1].address == 0x4400 && calls[2].length == 2,
          "production backend slices by protocol limit");
    Check(calls[0].processId == 77 && !calls[0].requireWindow && calls[0].confirmed,
          "read preserves PID, confirmation and fallback permission");
    Check(result.data.constData()[1023] == '1' && result.data.constData()[1024] == '2',
          "collected read contains actual consecutive response fragments");
    Reset(); script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL, 17));
    result = readVirtual(MemoryAccessBackend::Hvm, session, 77, 0x4000, 2048);
    Check(result.ok && result.partial && result.bytesDone == 17 && result.data.size() == 17 && calls.size() == 1,
          "partial read exposes exact usable prefix and stops");
    Reset(); script.push_back(Response(0, 1024)); script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL, 17));
    result = readPhysical(MemoryAccessBackend::Hvm, session, 0x4000, 2048);
    Check(result.ok && result.partial && result.bytesDone == 1041 && result.data.size() == 1041,
          "later partial read preserves all completed prefix bytes");
    Reset(); script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL, 17));
    result = writeVirtual(MemoryAccessBackend::Hvm, session, 77, 0x4000, QByteArray(2048, 'w'), false);
    Check(!result.ok && result.partial && result.bytesDone == 17 && result.data.isEmpty() && calls.size() == 1,
          "partial write cannot report full success or total requested bytes");
    Check(calls[0].requireWindow && calls[0].confirmed && calls[0].processId == 77,
          "write always requires private window and preserves target PID");
    Reset(); script.push_back(Response(0, 13));
    result = writePhysical(MemoryAccessBackend::Hvm, session, 0x4000, QByteArray(2048, 'w'), false);
    Check(!result.ok && result.partial && result.bytesDone == 13 && calls.size() == 1,
          "OK with short write stops and exposes partial failure");
    Reset(); script.push_back(Response(0, 13));
    result = readPhysical(MemoryAccessBackend::Hvm, session, 0x4000, 2048);
    Check(result.ok && result.partial && result.data.size() == 13 && calls.size() == 1,
          "OK with short read stops with exact partial data");
    Reset(); script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL, 1024));
    result = writePhysical(MemoryAccessBackend::Hvm, session, 0x4000, QByteArray(1024, 'w'), false);
    Check(!result.ok && result.partial && result.bytesDone == 1024,
          "PARTIAL status is not overridden by full byte count");
    Reset();
    result = writePhysical(MemoryAccessBackend::Hvm, session, 0x4000, QByteArray(2050, 'w'), false);
    Check(result.ok && !result.partial && result.bytesDone == 2050 && calls.size() == 3,
          "complete multi-chunk write succeeds");
    Check(calls[0].requireWindow && calls[1].requireWindow && calls[2].requireWindow,
          "every write fragment requires the private window");
}
static void TestInvalidAndFailureResponses() {
    DdmaSession session;
    Reset(); script.push_back(Response(0, 1025));
    auto result = readPhysical(MemoryAccessBackend::Hvm, session, 0x4000, 2048);
    Check(!result.ok && result.bytesDone == 0 && result.data.isEmpty() && calls.size() == 1,
          "oversized protocol completion rejected before buffer append");
    Reset(); script.push_back(Response(0, 1025));
    result = writePhysical(MemoryAccessBackend::Hvm, session, 0x4000, QByteArray(2048, 'w'), false);
    Check(!result.ok && result.bytesDone == 0 && calls.size() == 1,
          "oversized write completion never claims success");
    Reset(); script.push_back(Response(0, 0));
    result = readPhysical(MemoryAccessBackend::Hvm, session, 0x4000, 2048);
    Check(!result.ok && result.bytesDone == 0 && calls.size() == 1 && !result.failureText.isEmpty(),
          "zero progress stops instead of looping");
    Reset(); script.push_back(Response(0, 1024)); auto transportFailure = Response(0, 0);
    transportFailure.io.ok = false; script.push_back(transportFailure);
    result = readPhysical(MemoryAccessBackend::Hvm, session, 0x4000, 2048);
    Check(!result.ok && result.partial && result.bytesDone == 1024 && result.data.size() == 1024 && calls.size() == 2,
          "later transport failure retains exact read prefix");
    Reset(); script.push_back(Response(0, 1024)); script.push_back(Response(0, 1025));
    result = readPhysical(MemoryAccessBackend::Hvm, session, 0x4000, 2048);
    Check(!result.ok && result.partial && result.bytesDone == 1024 && result.data.size() == 1024 && calls.size() == 2,
          "oversized later completion preserves previously completed partial prefix");
    Reset(); script.push_back(Response(0, 1024)); script.push_back(transportFailure);
    result = writePhysical(MemoryAccessBackend::Hvm, session, 0x4000, QByteArray(2048, 'w'), false);
    Check(!result.ok && result.partial && result.bytesDone == 1024 && result.data.isEmpty(),
          "later write transport failure reports actual completed prefix");
    Reset(); script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_ACCESS_FAILED, 0));
    result = readPhysical(MemoryAccessBackend::Hvm, session, 0x4000, 2048);
    Check(!result.ok && result.data.isEmpty() && calls.size() == 1, "access failure stops immediately");
    Reset(); script.push_back(Response(0, 1024, false));
    result = readPhysical(MemoryAccessBackend::Hvm, session, 0x4000, 2048);
    Check(result.ok && result.bytesDone == 2048 && !result.failureText.isEmpty(),
          "fallback warning survives a later direct-window fragment");
    Reset(); script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL, 17, false));
    result = readPhysical(MemoryAccessBackend::Hvm, session, 0x4000, 2048);
    Check(result.ok && result.partial && !result.failureText.isEmpty() && result.data.size() == 17,
          "partial fallback read preserves prefix and user warning");
}
static void TestStrictHvmReads() {
    DdmaSession session;
    for (const bool virtualRead : {false, true}) {
        const auto read = [session, virtualRead](std::uint64_t length) {
            return virtualRead
                ? readVirtual(MemoryAccessBackend::Hvm, session, 77, 0x7FFA, length, true)
                : readPhysical(MemoryAccessBackend::Hvm, session, 0x7FFA, length, true);
        };
        Reset();
        auto result = read(2050);
        Check(result.ok && !result.partial && result.bytesDone == 2050 && result.data.size() == 2050 &&
              result.failureText.isEmpty() && calls.size() == 3,
              "strict direct-window read completes exact multi-chunk data");
        Check(calls[0].requireWindow && calls[1].requireWindow && calls[2].requireWindow &&
              calls[0].confirmed && calls[0].processId == (virtualRead ? 77U : 0U) &&
              calls[0].operation == (virtualRead ? KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL : KSWORD_ARK_HVM_MEMORY_OP_READ_PHYSICAL) &&
              calls[1].address == 0x83FA && calls[2].length == 2,
              "every strict fragment preserves operation, PID, address and required private window");
        Check(result.data.constData()[0] == '1' && result.data.constData()[1024] == '2' &&
              result.data.constData()[2049] == '3',
              "strict read collects consecutive authentic response bytes");

        Reset(); script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_OK, 1024, false));
        result = read(2048);
        Check(!result.ok && !result.partial && result.bytesDone == 0 && result.data.isEmpty() &&
              !result.failureText.isEmpty() && calls.size() == 1 && calls[0].requireWindow,
              "old driver ignoring strict requirement cannot expose successful fallback bytes");
        Check(ksword::ark::standardCalls == 0 && ksword::ark::translationCalls == 0 && ddmaCalls == 0 && userCalls == 0,
              "strict rejection never switches to an alternative memory backend");

        Reset(); script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_OK, 1024));
        script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_OK, 1024, false));
        result = read(3072);
        Check(!result.ok && result.partial && result.bytesDone == 1024 && result.data.size() == 1024 &&
              calls.size() == 2 && calls[1].requireWindow && !result.failureText.isEmpty(),
              "later fallback stops strict read with only earlier direct-window prefix");
        Check(result.data.constData()[0] == '1' && result.data.constData()[1023] == '1' &&
              ksword::ark::standardCalls == 0,
              "strict prefix excludes all later fallback bytes and has no R0 retry");

        Reset(); script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL, 17, false));
        result = read(2048);
        Check(!result.ok && !result.partial && result.bytesDone == 0 && result.data.isEmpty() && calls.size() == 1,
              "strict read rejects partial fallback without counting its alleged progress");

        Reset(); script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_OK, 1024));
        script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL, 17));
        result = read(3072);
        Check(result.ok && result.partial && result.bytesDone == 1041 && result.data.size() == 1041 && calls.size() == 2 &&
              result.data.constData()[1023] == '1' && result.data.constData()[1040] == '2',
              "strict direct partial read retains exact usable bytes without fabricating unreadable suffix");

        Reset(); auto transportFailure = Response(KSWORD_ARK_HVM_MEMORY_STATUS_OK, 1024);
        transportFailure.io.ok = false; script.push_back(transportFailure);
        result = read(2048);
        Check(!result.ok && !result.partial && result.bytesDone == 0 && result.data.isEmpty() && calls.size() == 1 &&
              !result.failureText.isEmpty() && ksword::ark::standardCalls == 0,
              "strict transport failure cannot expose untrusted response bytes or trigger R0 fallback");

        Reset(); script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_OK, 1024));
        script.push_back(transportFailure);
        result = read(3072);
        Check(!result.ok && result.partial && result.bytesDone == 1024 && result.data.size() == 1024 && calls.size() == 2 &&
              calls[1].requireWindow && ksword::ark::standardCalls == 0,
              "later strict transport failure preserves only completed direct-window prefix");

        Reset(); script.push_back(Response(KSWORD_ARK_HVM_MEMORY_STATUS_WINDOW_UNAVAILABLE, 0));
        result = read(2048);
        Check(!result.ok && result.data.isEmpty() && result.bytesDone == 0 && calls.size() == 1 &&
              ksword::ark::standardCalls == 0,
              "unavailable strict window stops without standard-driver retry");
    }
}
static void TestPublicRangeGuards() {
    DdmaSession session;
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    for (auto backend : {MemoryAccessBackend::Hvm, MemoryAccessBackend::Ddma,
                         MemoryAccessBackend::StandardDriver, MemoryAccessBackend::UserMode}) {
        Reset();
        Check(!readPhysical(backend, session, maximum - 8, 32).ok, "physical read overflow guard");
        Check(!readVirtual(backend, session, 77, maximum - 8, 32).ok, "virtual read overflow guard");
        Check(!writePhysical(backend, session, maximum - 8, QByteArray(32, 'w'), false).ok,
              "physical write overflow guard");
        Check(!writeVirtual(backend, session, 77, maximum - 8, QByteArray(32, 'w'), false).ok,
              "virtual write overflow guard");
        Check(!readPhysical(backend, session, 0, 0).ok && !readVirtual(backend, session, 77, 0, 0).ok &&
              !writePhysical(backend, session, 0, QByteArray(), false).ok &&
              !writeVirtual(backend, session, 77, 0, QByteArray(), false).ok, "zero-length public guards");
        Check(!readPhysical(backend, session, 0, maximum).ok &&
              !readVirtual(backend, session, 77, 0, maximum).ok, "buffer-capacity public guards");
        Check(calls.empty() && ddmaCalls == 0 && userCalls == 0 && ksword::ark::standardCalls == 0,
              "invalid ranges rejected before any backend access or allocation");
    }
    Reset();
    auto result = readPhysical(MemoryAccessBackend::Hvm, session, maximum - 31, 32);
    Check(result.ok && result.bytesDone == 32 && calls[0].address == maximum - 31,
          "inclusive range ending at UINT64_MAX remains valid");
}
static void TestDdmaUnavailablePages() {
    DdmaSession session; session.configured = true;
    Reset(); ksword::ark::translationFailure = 2;
    auto result = readVirtual(MemoryAccessBackend::Ddma, session, 77, 0x7FF0, 32);
    Check(!result.ok && result.partial && result.bytesDone == 16 && result.data.size() == 16,
          "second DDMA translation failure returns actual prefix, no fabricated zeros");
    Check(ddmaCalls == 1 && ksword::ark::translationCalls == 2 && result.data.constData()[15] == 'q',
          "DDMA stops before accessing an unavailable next page");
    Reset(); ksword::ark::translationFailure = 1;
    result = readVirtual(MemoryAccessBackend::Ddma, session, 77, 0x7FF0, 32);
    Check(!result.ok && !result.partial && result.bytesDone == 0 && result.data.isEmpty() && ddmaCalls == 0,
          "first unavailable DDMA page returns no data");
    Reset();
    result = readVirtual(MemoryAccessBackend::Ddma, session, 77, 0x7FF0, 32);
    Check(result.ok && !result.partial && result.bytesDone == 32 && result.data.size() == 32 && ddmaCalls == 2,
          "successful DDMA read still uses both page fragments");
    Reset();
    result = readVirtual(MemoryAccessBackend::StandardDriver, session, 77, 0x4000, 32);
    Check(result.ok && result.data.size() == 32 && ksword::ark::standardCalls == 1,
          "ordinary driver read remains usable");
}
static void TestDdmaDirtyScratchStopsPages() {
    DdmaSession session; session.configured = true;
    for (const bool virtualRead : {false, true}) {
        const auto read = [session, virtualRead](std::uint64_t address, std::uint64_t length) {
            return virtualRead
                ? readVirtual(MemoryAccessBackend::Ddma, session, 77, address, length)
                : readPhysical(MemoryAccessBackend::Ddma, session, address, length);
        };
        Reset(); ddmaDirtyCall = 1;
        auto result = read(0x7FFA, 8);
        Check(!result.ok && result.partial && result.scratchDirty &&
              result.bytesDone == 6 && result.data.size() == 6,
              "dirty first DDMA fragment preserves exact six-byte prefix as partial failure");
        Check(ddmaCalls == 1 && ksword::ark::translationCalls == (virtualRead ? 1U : 0U),
              "dirty DDMA fragment stops before next page read or VA translation");
        Check(!result.failureText.isEmpty() && result.data.size() == 6 && result.data.constData()[0] == 'q' &&
              result.data.constData()[5] == 'q',
              "dirty DDMA fragment retains real data and restoration warning");
        Reset(); ddmaDirtyCall = 2;
        result = read(0x7FFA, 8200);
        Check(!result.ok && result.partial && result.scratchDirty &&
              result.bytesDone == 4102 && result.data.size() == 4102,
              "later dirty DDMA fragment retains all completed prefix bytes");
        Check(ddmaCalls == 2 && ksword::ark::translationCalls == (virtualRead ? 2U : 0U),
              "later dirty DDMA fragment prevents third page access");
        Reset(); ddmaDirtyCall = 1;
        result = read(0x7FFA, 6);
        Check(result.ok && !result.partial && result.scratchDirty &&
              result.bytesDone == 6 && result.data.size() == 6 && !result.failureText.isEmpty(),
              "dirty complete single DDMA fragment remains usable with warning");
        Check(ddmaCalls == 1 && ksword::ark::translationCalls == (virtualRead ? 1U : 0U),
              "dirty complete DDMA read issues no redundant page request");
        Reset(); ddmaDirtyCall = 2;
        result = read(0x7FFA, 8);
        Check(result.ok && !result.partial && result.scratchDirty &&
              result.bytesDone == 8 && result.data.size() == 8 && !result.failureText.isEmpty(),
              "dirty final DDMA fragment preserves complete multi-page data with warning");
        Check(ddmaCalls == 2 && ksword::ark::translationCalls == (virtualRead ? 2U : 0U),
              "dirty final DDMA fragment completes only the required page requests");
        Reset();
        result = read(0x7FFA, 8);
        Check(result.ok && !result.partial && !result.scratchDirty && result.bytesDone == 8 &&
              result.data.size() == 8 && result.failureText.isEmpty() && ddmaCalls == 2 &&
              ksword::ark::translationCalls == (virtualRead ? 2U : 0U),
              "clean DDMA read still completes all requested page fragments");
    }
}
int main() {
    TestHvmSuccessAndPartial(); TestInvalidAndFailureResponses(); TestStrictHvmReads();
    TestPublicRangeGuards(); TestDdmaUnavailablePages(); TestDdmaDirtyScratchStopsPages();
    std::cout << "Production memory backend: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=PRODUCTION)
    parser.add_argument("--compiler", default=os.environ.get("CXX"))
    parser.add_argument("--work-dir", type=Path, default=ROOT / "work/hvm-memory-tests")
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        parser.error("G++ or Clang++ is required")
    source = args.source.read_text(encoding="utf-8-sig")
    functions = [extract_function(source, name) for name in (
        "isValidMemoryRange", "formatHex", "mergeChunkOutcome", "describeHvmMemoryStatus",
        "hvmTransfer", "isKernelVirtualAddress", "readPhysical", "writePhysical",
        "readVirtual", "writeVirtual",
    )]
    args.work_dir.mkdir(parents=True, exist_ok=True)
    work_dir = args.work_dir.resolve()
    scratch = work_dir / f"backend-{uuid.uuid4().hex}"
    scratch.mkdir()
    try:
        (scratch / "qt_shim.h").write_text(QT_SHIM, encoding="utf-8")
        for name in ("QByteArray", "QString"):
            (scratch / name).write_text('#include "qt_shim.h"\n', encoding="utf-8")
        cpp_path = scratch / "production_backend.cpp"
        binary = scratch / ("memory-backend.exe" if os.name == "nt" else "memory-backend")
        cpp_path.write_text(PRELUDE + "\n".join(functions) + TESTS, encoding="utf-8")
        subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-I", str(scratch), "-I", str(ROOT / "shared/driver"),
            "-I", str(PRODUCTION.parent), str(cpp_path), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    finally:
        if scratch.resolve().parent != work_dir:
            raise RuntimeError("Refusing to clean outside the test work directory")
        shutil.rmtree(scratch)


if __name__ == "__main__":
    main()
