#include "../Ksword5.1/Ksword5.1/MemoryDock/WorkbenchPointerChainAccess.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wb = ksword::memwb;
namespace access = ksword::memwb_pointer_access;
namespace pc = ksword::pointer_chain;
namespace {

unsigned checks = 0;
unsigned writes = 0;

void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}

std::uint64_t FileTimeValue(const FILETIME& value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

std::string Utf8(const std::wstring& value) {
    const int count = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    Check(count > 0, "current executable path converts to UTF-8");
    std::string result(static_cast<std::size_t>(count), '\0');
    Check(::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) == count,
        "current executable UTF-8 conversion completes");
    return result;
}

wb::IoReadResult Pointer(std::uint64_t value, std::size_t width = 8) {
    wb::IoReadResult result;
    result.status = wb::IoReadStatus::Ok;
    for (std::size_t i = 0; i < width; ++i)
        result.data.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    return result;
}

struct FakePort : wb::IMemoryIoPort {
    std::map<std::uint64_t, wb::IoReadResult> values;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    std::vector<wb::MemoryTargetSession> readSessions;
    std::function<void(std::size_t, wb::IoReadResult&)> afterRead;

    wb::IoLimits Limits(const wb::MemoryTargetSession&) const override { return {8, 0}; }
    wb::IoReadResult Read(const wb::MemoryTargetSession& session, std::uint64_t address, std::uint64_t length) override {
        reads.emplace_back(address, length);
        readSessions.push_back(session);
        const auto found = values.find(address);
        wb::IoReadResult result;
        if (found != values.end()) result = found->second;
        else result.failure = "scripted pointer is unavailable";
        if (afterRead) afterRead(reads.size(), result);
        return result;
    }
    wb::IoWriteResult Write(const wb::MemoryTargetSession&, std::uint64_t,
                           const std::vector<std::uint8_t>&, bool) override {
        ++writes;
        throw std::runtime_error("native pointer adapter must never invoke Write");
    }
};

struct Rig {
    wb::MemoryTargetSession session;
    access::ModuleCapture capture;
    wb::AddressEntry entry;
    FakePort port;
    std::wstring path;
    std::uint64_t root = 0;

    Rig() {
        session.pid = ::GetCurrentProcessId();
        FILETIME created{}, exited{}, kernel{}, user{};
        Check(::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE,
            "current process creation identity is available");
        session.processCreateTime100ns = FileTimeValue(created);
        session.attachGeneration = 17;
        session.channel = wb::Channel::UserMode;
        BOOL wow64 = FALSE;
        Check(::IsWow64Process(::GetCurrentProcess(), &wow64) != FALSE, "current process width is available");
        session.addressBits = wow64 ? 32U : 64U;
        Check(session.addressBits == 64, "test runner is an x64 executable");
        wchar_t executable[32768]{};
        const auto length = ::GetModuleFileNameW(nullptr, executable, 32768);
        Check(length > 0 && length < 32768, "current executable module path is complete");
        path.assign(executable, length);
        capture = access::CaptureModule(session, Utf8(path));
        Check(capture.ok && capture.issue == access::Issue::None, "native module capture succeeds for current executable");
        const auto expectedBase = reinterpret_cast<std::uint64_t>(::GetModuleHandleW(nullptr));
        MODULEINFO info{};
        Check(::GetModuleInformation(::GetCurrentProcess(), ::GetModuleHandleW(nullptr), &info, sizeof(info)) != FALSE,
            "independent Win32 module metadata query succeeds");
        Check(capture.base == expectedBase && capture.definition.moduleSize == info.SizeOfImage,
            "native capture uses actual executable base and image size");
        WIN32_FILE_ATTRIBUTE_DATA file{};
        Check(::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &file) != FALSE,
            "current executable file metadata is available");
        const auto expectedSize = (static_cast<std::uint64_t>(file.nFileSizeHigh) << 32) | file.nFileSizeLow;
        const auto expectedTime = static_cast<std::int64_t>(FileTimeValue(file.ftLastWriteTime) / 10000) - 11644473600000LL;
        Check(capture.definition.moduleFileSize == static_cast<std::int64_t>(expectedSize)
            && capture.definition.moduleFileTime == expectedTime && capture.definition.pointerSize == 8,
            "capture records exact file size, modification time and pointer width");
        Check(!capture.definition.processPath.empty() && !capture.definition.modulePath.empty(),
            "capture binds complete process and module paths");
        entry.id = 1;
        entry.kind = wb::EntryKind::Bookmark;
        entry.targetKey = capture.definition.processPath;
        const auto slash = capture.definition.modulePath.find_last_of("/\\");
        entry.moduleName = capture.definition.modulePath.substr(slash == std::string::npos ? 0 : slash + 1);
        entry.rva = 0x100; // Real committed image header mapping; bytes remain scripted.
        entry.pointerChain = capture.definition;
        root = capture.base + entry.rva;
        Build(2);
    }
    void Build(std::size_t depth) {
        entry.pointerChain->offsets.assign(depth, 0);
        port.values.clear();
        port.reads.clear();
        port.readSessions.clear();
        port.afterRead = {};
        for (std::size_t i = 0; i < depth; ++i)
            port.values[i == 0 ? root : 0x2000 + (i - 1) * 0x20] = Pointer(0x2000 + i * 0x20);
    }
    access::Resolution Resolve(const std::shared_ptr<std::atomic<bool>>& cancel = {}) {
        return access::Resolve(entry, session, port, cancel);
    }
};

void Rejected(const access::Resolution& result, access::Issue issue, const char* label) {
    Check(!result.result.ok() && result.result.address == 0 && result.issue == issue, label);
}

void TestCaptureAndMetadata(Rig& rig) {
    for (int mutation = 0; mutation < 6; ++mutation) {
        auto session = rig.session;
        auto path = rig.capture.definition.modulePath;
        if (mutation == 0) session.pid ^= 0x80000000U;
        if (mutation == 1) ++session.processCreateTime100ns;
        if (mutation == 2) session.processCreateTime100ns = 0;
        if (mutation == 3) session.addressBits = 32;
        if (mutation == 4) session.channel = wb::Channel::Ddma;
        if (mutation == 5) path += ".missing";
        const auto result = access::CaptureModule(session, path);
        Check(!result.ok, "capture rejects wrong PID, creation, width, weak identity, DDMA and missing module");
    }
    const auto savedEntry = rig.entry;
    const auto savedSession = rig.session;
    for (int mutation = 0; mutation < 13; ++mutation) {
        rig.entry = savedEntry;
        rig.session = savedSession;
        rig.port.reads.clear();
        auto issue = access::Issue::InvalidRoot;
        if (mutation == 0) { rig.entry.pointerChain.reset(); }
        if (mutation == 1) { rig.session.pid ^= 0x80000000U; issue = access::Issue::TargetChanged; }
        if (mutation == 2) { ++rig.session.processCreateTime100ns; issue = access::Issue::TargetChanged; }
        if (mutation == 3) { rig.session.processCreateTime100ns = 0; issue = access::Issue::IdentityUnavailable; }
        if (mutation == 4) { rig.session.addressBits = 32; issue = access::Issue::IdentityUnavailable; }
        if (mutation == 5) { rig.session.channel = wb::Channel::Ddma; issue = access::Issue::UnsupportedChannel; }
        if (mutation == 6) { rig.entry.pointerChain->modulePath += ".missing"; issue = access::Issue::ModuleUnavailable; }
        if (mutation == 7) { rig.entry.pointerChain->processPath += ".different"; issue = access::Issue::WrongProgram; }
        if (mutation == 8) { ++rig.entry.pointerChain->moduleSize; issue = access::Issue::ModuleChanged; }
        if (mutation == 9) { ++rig.entry.pointerChain->moduleFileSize; issue = access::Issue::ModuleChanged; }
        if (mutation == 10) { ++rig.entry.pointerChain->moduleFileTime; issue = access::Issue::ModuleChanged; }
        if (mutation == 11) { rig.entry.pointerChain->pointerSize = 4; issue = access::Issue::ModuleChanged; }
        if (mutation == 12) { rig.entry.rva = rig.entry.pointerChain->moduleSize - 7; }
        Rejected(rig.Resolve(), issue, "resolve rejects stale, missing, malformed and mismatched metadata");
        Check(rig.port.reads.empty(), "metadata rejection invokes no byte-read callback");
    }
    rig.entry = savedEntry;
    rig.session = savedSession;
}

void TestBoundedChains(Rig& rig) {
    for (std::size_t depth = 1; depth <= pc::MaxDepth; ++depth) {
        rig.Build(depth);
        const auto result = rig.Resolve();
        Check(result.result.ok() && result.issue == access::Issue::None
            && result.result.address == 0x2000 + (depth - 1) * 0x20,
            "real metadata and fake bytes resolve every depth from 1 through 16");
        Check(result.result.steps.size() == depth && rig.port.reads.size() == depth,
            "native adapter preserves exact bounded step/read count");
        Check(rig.port.reads.front() == std::pair<std::uint64_t, std::uint64_t>{rig.root, 8},
            "first fake byte read uses the actual relocated module root");
    }
    for (const auto depth : {0U, 17U}) {
        rig.Build(depth);
        Rejected(rig.Resolve(), access::Issue::InvalidRoot, "invalid depth is rejected before reading");
        Check(rig.port.reads.empty(), "invalid depth has no read callbacks");
    }
    rig.Build(2);
    rig.entry.pointerChain->offsets = {0x20, -8};
    rig.port.values[rig.root] = Pointer(0x2000);
    rig.port.values[0x2020] = Pointer(0x5008);
    const auto negative = rig.Resolve();
    Check(negative.result.ok() && negative.result.address == 0x5000
        && negative.result.steps[1].offset == -8, "signed offsets are applied after each little-endian dereference");
}

void TestReadFailures(Rig& rig) {
    for (int mutation = 0; mutation < 8; ++mutation) {
        rig.Build(2);
        auto status = pc::Status::ReadFailed;
        auto& read = rig.port.values[rig.root];
        if (mutation == 0) { read.data.resize(7); status = pc::Status::PartialRead; }
        if (mutation == 1) { read.data.resize(9); status = pc::Status::PartialRead; }
        if (mutation == 2) { read.status = wb::IoReadStatus::Partial; read.data.resize(4); }
        if (mutation == 3) { read.status = wb::IoReadStatus::Unreadable; read.data.clear(); }
        if (mutation == 4) { read.status = wb::IoReadStatus::Failed; }
        if (mutation == 5) { read.scratchAreaDirty = true; }
        if (mutation == 6) { read = Pointer(0); status = pc::Status::NullPointer; }
        if (mutation == 7) { read = Pointer(rig.root); status = pc::Status::CycleDetected; }
        read.failure = "scripted transfer detail";
        const auto result = rig.Resolve();
        Rejected(result, access::Issue::ReadFailed, "short, oversized, partial, dirty, null and cyclic reads fail closed");
        Check(result.result.status == status && result.result.failedLevel == 0 && rig.port.reads.size() == 1,
            "failed first step exposes exact status and performs no further read");
        Check(result.annotation == read.failure, "native adapter preserves backend annotation on failure");
    }
    rig.Build(3);
    rig.port.values.erase(0x2000);
    const auto missing = rig.Resolve();
    Rejected(missing, access::Issue::ReadFailed, "missing middle pointer cannot produce a final address");
    Check(missing.result.failedLevel == 1 && missing.result.steps.size() == 1 && rig.port.reads.size() == 2,
        "middle failure preserves successful prefix and stops bounded reads");
}

void TestCallbackContextChanges(Rig& rig) {
    const auto original = rig.session;
    for (int mutation = 0; mutation < 7; ++mutation) {
        rig.session = original;
        rig.Build(3);
        rig.port.afterRead = [&rig, mutation](std::size_t reads, wb::IoReadResult&) {
            if (reads != 1) return;
            if (mutation == 0) ++rig.session.pid;
            if (mutation == 1) ++rig.session.processCreateTime100ns;
            if (mutation == 2) ++rig.session.attachGeneration;
            if (mutation == 3) rig.session.channel = wb::Channel::Ddma;
            if (mutation == 4) rig.session.scope = wb::Scope::Physical;
            if (mutation == 5) rig.session.addressBits = 32;
            if (mutation == 6) ++rig.session.ddmaGeneration;
        };
        const auto result = rig.Resolve();
        Rejected(result, access::Issue::TargetChanged, "callback changes to any session field invalidate the result");
        Check(rig.port.reads.size() == 1 && result.result.failedLevel == 0,
            "changed channel, width or identity cannot reach a second byte read");
        Check(rig.port.readSessions.size() == 1 && wb::SameTarget(rig.port.readSessions.front(), original),
            "the byte-read port receives the original immutable session snapshot");
    }
    rig.session = original;
    rig.Build(3);
    const auto entry = rig.entry;
    rig.port.afterRead = [&rig](std::size_t reads, wb::IoReadResult&) {
        if (reads == 1) rig.entry.pointerChain.reset();
    };
    const auto result = rig.Resolve();
    Check(result.result.ok() && result.result.address == 0x2040 && rig.port.reads.size() == 3,
        "caller optional invalidation cannot dangle the resolver's copied definition");
    rig.entry = entry;
}

void TestCancellationAndLimits(Rig& rig) {
    rig.Build(3);
    auto cancel = std::make_shared<std::atomic<bool>>(true);
    auto result = rig.Resolve(cancel);
    Rejected(result, access::Issue::Cancelled, "already cancelled resolution is refused");
    Check(result.result.status == pc::Status::Cancelled && rig.port.reads.empty(), "before-read cancellation reads no bytes");
    for (const std::size_t count : {1U, 2U}) {
        rig.Build(3);
        cancel->store(false);
        rig.port.afterRead = [cancel, count](std::size_t reads, wb::IoReadResult&) {
            if (reads == count) cancel->store(true);
        };
        result = rig.Resolve(cancel);
        Rejected(result, access::Issue::Cancelled, "cancellation during a fake read refuses further steps");
        Check(result.result.failedLevel == count - 1 && result.result.steps.size() == count - 1
            && rig.port.reads.size() == count, "post-read cancellation cannot expose a stale successful step");
    }
    rig.Build(2);
    rig.port.afterRead = [](std::size_t reads, wb::IoReadResult&) { if (reads == 1) ::Sleep(1600); };
    result = rig.Resolve();
    Rejected(result, access::Issue::Cancelled, "cooperative deadline refuses a delayed read result");
    Check(result.result.status == pc::Status::Cancelled && result.result.failedLevel == 0
        && rig.port.reads.size() == 1, "deadline stops before any second pointer read");
    for (int mutation = 0; mutation < 4; ++mutation) {
        rig.Build(1);
        if (mutation == 0) { rig.port.values[rig.root] = Pointer(1); rig.entry.pointerChain->offsets = {-2}; }
        if (mutation == 1) { rig.port.values[rig.root] = Pointer((std::numeric_limits<std::uint64_t>::max)()); rig.entry.pointerChain->offsets = {1}; }
        if (mutation == 2) rig.port.values[rig.root] = Pointer(wb::kProcessSpaceLast64 + 1);
        if (mutation == 3) { rig.port.values[rig.root] = Pointer(wb::kProcessSpaceLast64); rig.entry.pointerChain->offsets = {1}; }
        result = rig.Resolve();
        Rejected(result, access::Issue::ReadFailed, "underflow, overflow and out-of-user-space endpoints are rejected");
        Check(result.result.status == pc::Status::AddressOverflow && rig.port.reads.size() == 1,
            "arithmetic/range failures do not perform a second read");
    }
    rig.Build(1);
    rig.port.values[rig.root] = Pointer(wb::kProcessSpaceLast64);
    result = rig.Resolve();
    Check(result.result.ok() && result.result.address == wb::kProcessSpaceLast64,
        "last user-space address is a valid final endpoint");
    rig.Build(1);
    rig.port.values[rig.root] = Pointer(wb::kProcessSpaceLast64 + 1);
    rig.entry.pointerChain->offsets = {-1};
    result = rig.Resolve();
    Rejected(result, access::Issue::ReadFailed, "negative offset cannot launder an out-of-space pointer word");
    Check(result.result.status == pc::Status::AddressOverflow,
        "decoded pointer-word rejection agrees with binding user-space validation");
    for (const std::int64_t offset : {0, -1}) {
        rig.Build(2);
        rig.port.values[0x2000] = Pointer(wb::kProcessSpaceLast64 + 1);
        rig.entry.pointerChain->offsets[1] = offset;
        result = rig.Resolve();
        Rejected(result, access::Issue::ReadFailed, "out-of-space second pointer step is rejected");
        Check(result.result.status == pc::Status::AddressOverflow && result.result.failedLevel == 1
            && result.result.steps.size() == 1 && rig.port.reads.size() == 2,
            "post-resolution range validation reports the actual failing level and only the successful prefix");
    }
    rig.Build(2);
    rig.port.values[rig.root] = Pointer(wb::kProcessSpaceLast64 - 6);
    result = rig.Resolve();
    Rejected(result, access::Issue::ReadFailed, "pointer read extent cannot cross the user-space boundary");
    Check(result.result.failedLevel == 1 && rig.port.reads.size() == 1,
        "out-of-range intermediate read is stopped before the fake port");
    rig.Build(2);
    rig.port.values[rig.root] = Pointer(wb::kProcessSpaceLast64 - 7);
    rig.port.values[wb::kProcessSpaceLast64 - 7] = Pointer(0x5000);
    result = rig.Resolve();
    Check(result.result.ok() && rig.port.reads.size() == 2, "last complete user-space pointer read is accepted");
}

} // namespace

int main() {
    try {
        Rig rig;
        TestCaptureAndMetadata(rig);
        TestBoundedChains(rig);
        TestReadFailures(rig);
        TestCallbackContextChanges(rig);
        TestCancellationAndLimits(rig);
        Check(writes == 0, "the native adapter test never performs any target write");
        std::cout << "Pointer-chain workbench Win32 metadata: " << checks << " checks passed; no target writes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Pointer-chain workbench check " << checks << " failed: " << error.what() << '\n';
        return 1;
    }
}
