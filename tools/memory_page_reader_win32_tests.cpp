// Exercise the production page reader against Windows, using only allocations
// owned by this test process. No external process, driver, or device is opened.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../shared/evidence/memory_workbench/MemoryPageReader.h"
#include "../KswordARKLightTests/TestSupport.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wb = ksword::memwb;
namespace {
constexpr std::size_t kPage = static_cast<std::size_t>(wb::HexViewport::kPageBytes);
constexpr std::size_t kPages = 5;
unsigned checks = 0;
bool allAllocationsReleased = true;

void Check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}

std::uint8_t Pattern(std::size_t page, std::size_t offset) {
    return static_cast<std::uint8_t>((page * 43U + offset * 17U + 11U) % 251U);
}

class Allocation {
public:
    Allocation() {
        SYSTEM_INFO information{};
        ::GetSystemInfo(&information);
        Check(information.dwPageSize == kPage, "Windows page size matches the page-reader contract");
        base_ = static_cast<std::uint8_t*>(::VirtualAlloc(
            nullptr, kPages * kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (base_ == nullptr) throw std::runtime_error("VirtualAlloc failed");
        for (std::size_t page = 0; page < kPages; ++page)
            for (std::size_t offset = 0; offset < kPage; ++offset)
                base_[page * kPage + offset] = Pattern(page, offset);
    }
    ~Allocation() {
        if (base_ != nullptr && ::VirtualFree(base_, 0, MEM_RELEASE) == FALSE)
            allAllocationsReleased = false;
    }
    Allocation(const Allocation&) = delete;
    Allocation& operator=(const Allocation&) = delete;
    std::uint64_t Address() const {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base_));
    }
    void Protect(std::size_t page, DWORD protection) {
        DWORD previous = 0;
        Check(::VirtualProtect(base_ + page * kPage, kPage, protection, &previous) != FALSE,
            "fixture page protection established");
    }
    void Decommit(std::size_t page) {
        Check(::VirtualFree(base_ + page * kPage, kPage, MEM_DECOMMIT) != FALSE,
            "fixture page remains reserved but uncommitted");
    }
    MEMORY_BASIC_INFORMATION Query(std::size_t page) const {
        MEMORY_BASIC_INFORMATION information{};
        Check(::VirtualQuery(base_ + page * kPage, &information, sizeof(information)) == sizeof(information),
            "fixture protection query succeeds");
        return information;
    }
private:
    std::uint8_t* base_ = nullptr;
};

struct ReadCall {
    std::uint64_t address = 0, length = 0;
    wb::IoReadStatus status = wb::IoReadStatus::Failed;
    std::uint64_t bytes = 0;
    DWORD error = ERROR_SUCCESS;
};

class CurrentProcessPort final : public wb::IMemoryIoPort {
public:
    explicit CurrentProcessPort(const Allocation& allocation) : first_(allocation.Address()) {}
    wb::IoLimits Limits(const wb::MemoryTargetSession&) const override { return {0, 0}; }
    wb::IoReadResult Read(const wb::MemoryTargetSession& session,
                         std::uint64_t address, std::uint64_t length) override {
        Check(session.pid == ::GetCurrentProcessId(), "only the fixture process is selected");
        Check(address >= first_ && address - first_ < kPages * kPage
                && length > 0 && length <= kPages * kPage - (address - first_),
            "every native read stays inside the owned fixture allocation");
        wb::IoReadResult result;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        SIZE_T count = 0;
        const BOOL ok = ::ReadProcessMemory(::GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)),
            bytes.data(), static_cast<SIZE_T>(length), &count);
        const DWORD error = ok == FALSE ? ::GetLastError() : ERROR_SUCCESS;
        Check(count <= length, "RPM reports no more than the requested bytes");
        // Match WorkbenchIoPort::ReadUserMode: zero bytes describe an unreadable
        // request; nonempty data is the actual prefix, even if RPM returned false.
        if (count == 0) {
            result.status = wb::IoReadStatus::Unreadable;
            result.failure = "ReadProcessMemory returned zero bytes, win32=" + std::to_string(error);
        } else {
            bytes.resize(count);
            result.data = std::move(bytes);
            result.status = count == length ? wb::IoReadStatus::Ok : wb::IoReadStatus::Partial;
        }
        calls.push_back({address, length, result.status, count, error});
        return result;
    }
    wb::IoWriteResult Write(const wb::MemoryTargetSession&, std::uint64_t,
                           const std::vector<std::uint8_t>&, bool) override {
        throw std::runtime_error("the page-reader fixture must never write through the port");
    }
    std::vector<ReadCall> calls;
private:
    std::uint64_t first_;
};

wb::MemoryTargetSession Session() {
    wb::MemoryTargetSession session;
    session.pid = ::GetCurrentProcessId();
    session.channel = wb::Channel::UserMode;
    session.addressBits = static_cast<std::uint32_t>(sizeof(void*) * 8U);
    return session;
}

void ExpectPage(const wb::PageRecord& page, std::size_t index,
                std::uint64_t first, std::size_t prefixBytes, const std::string& label) {
    Check(page.pageStart == first + index * kPage, label + ": page address");
    Check(page.bytes.size() == kPage && page.valid.size() == kPage, label + ": complete page storage");
    const auto expectedState = prefixBytes == kPage ? wb::PageState::Valid
        : prefixBytes == 0 ? wb::PageState::Unreadable : wb::PageState::PartiallyValid;
    Check(page.state == expectedState,
        label + ": correct page state " + std::to_string(index));
    bool exact = true;
    for (std::size_t offset = 0; offset < kPage; ++offset)
        exact = exact && page.valid[offset] == (offset < prefixBytes ? 1U : 0U)
            && page.bytes[offset] == (offset < prefixBytes ? Pattern(index, offset) : 0U);
    Check(exact, label + ": every byte and validity bit matches page " + std::to_string(index));
}

void ExpectReadResult(const wb::PageReadResult& result, const Allocation& allocation,
                      const CurrentProcessPort& port, std::size_t hole, const std::string& label,
                      std::size_t holePrefixBytes = 0) {
    Check(!result.channelFailed && !result.cancelled && !result.scratchAreaDirty,
        label + ": no channel failure, cancellation, or dirty scratch state");
    Check(result.pages.size() == kPages, label + ": all pages retained");
    Check(result.portCalls == port.calls.size(), label + ": real native calls accounted for");
    for (std::size_t index = 0; index < kPages; ++index)
        ExpectPage(result.pages[index], index, allocation.Address(),
            index != hole ? kPage : holePrefixBytes, label);
}

void TestHealthyBulk() {
    Allocation allocation;
    CurrentProcessPort port(allocation);
    const auto result = wb::ReadPages(port, Session(), allocation.Address(), kPages, nullptr);
    ExpectReadResult(result, allocation, port, kPages, "healthy bulk");
    Check(result.portCalls == 1 && port.calls.front().length == kPages * kPage,
        "fully readable pages stay one native bulk call");
}

enum class Hole { NoAccess, Reserved, Guard };

void TestHole(Hole kind, std::size_t hole, const std::string& label) {
    Allocation allocation;
    if (kind == Hole::Reserved) allocation.Decommit(hole);
    else allocation.Protect(hole, kind == Hole::Guard ? PAGE_READWRITE | PAGE_GUARD : PAGE_NOACCESS);
    const auto protection = allocation.Query(hole);
    Check(kind == Hole::Reserved ? protection.State == MEM_RESERVE
            : protection.State == MEM_COMMIT && (protection.Protect &
                (kind == Hole::Guard ? PAGE_GUARD : PAGE_NOACCESS)) != 0,
        label + ": the intended hole exists before reading");
    CurrentProcessPort port(allocation);
    const auto result = wb::ReadPages(port, Session(), allocation.Address(), kPages, nullptr);
    // Guard protection is one-shot. Its post-read protection alone cannot prove
    // data was returned: a failed RPM may clear it while still returning zero.
    // Derive the expected guard-page prefix only from actual RPM byte counts.
    const bool guardStillPresent = kind == Hole::Guard
        && (allocation.Query(hole).Protect & PAGE_GUARD) != 0;
    std::size_t guardPrefixBytes = 0;
    if (kind == Hole::Guard) {
        const auto guardStart = allocation.Address() + hole * kPage;
        for (const auto& call : port.calls) {
            if (call.address <= guardStart && call.bytes > guardStart - call.address)
                guardPrefixBytes = (std::max)(guardPrefixBytes, static_cast<std::size_t>(
                    (std::min<std::uint64_t>)(kPage, call.bytes - (guardStart - call.address))));
        }
    }
    ExpectReadResult(result, allocation, port, hole, label, guardPrefixBytes);
    Check(port.calls.front().length == kPages * kPage, label + ": starts with bulk RPM");
    if (kind != Hole::Guard || port.calls.front().status != wb::IoReadStatus::Ok)
        Check(result.portCalls > 1, label + ": bounded fallback reads retain readable neighbors");
    Check(result.portCalls <= kPages + 1, label + ": at most one bulk plus one read per page");
    std::cout << label << ": initial RPM bytes=" << port.calls.front().bytes
              << ", win32=" << port.calls.front().error << ", native calls=" << result.portCalls;
    if (kind == Hole::Guard)
        std::cout << ", guard after reading=" << (guardStillPresent ? "present" : "cleared")
                  << ", guard bytes actually returned=" << guardPrefixBytes;
    std::cout << '\n';
}
} // namespace

int main() {
    try {
        const int scriptedFailures = RunMemwbPageReaderTests();
        TestHealthyBulk();
        TestHole(Hole::NoAccess, 2, "middle no-access page");
        TestHole(Hole::NoAccess, kPages - 1, "last no-access page");
        TestHole(Hole::Reserved, 2, "middle reserved page");
        TestHole(Hole::Reserved, kPages - 1, "last reserved page");
        TestHole(Hole::Guard, 2, "middle guard page");
        TestHole(Hole::Guard, kPages - 1, "last guard page");
        Check(allAllocationsReleased, "all fixture reservations were released");
        Check(scriptedFailures == 0, "production scripted page-reader regression suite passes");
        std::cout << "MemoryPageReader Win32 tests: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "MemoryPageReader Win32 tests failed after " << checks
                  << " checks: " << exception.what() << '\n';
        return 1;
    }
}
