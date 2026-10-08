#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

// Fixed-width, same-user local IPC. This protocol intentionally has no attach,
// run, pause, detach, breakpoint, register or memory-write operation.
namespace ksword::x64dbg_navigation
{
    constexpr std::uint32_t kMagic = 0x4e44534b;
    constexpr std::uint32_t kVersion = 2;
    constexpr std::uint32_t kReceiptMagic = 0x4144534b;
    enum class Operation : std::uint32_t { Query = 1, Navigate = 2 };
    enum class View : std::uint32_t { Disassembly = 0, Dump = 1 };
    enum Flag : std::uint32_t { Debugging = 1, Running = 2, AttachReady = 4 };
#pragma pack(push, 8)
    struct Request
    {
        std::uint32_t magic = kMagic, version = kVersion, bytes = sizeof(Request);
        Operation operation = Operation::Query;
        std::uint64_t requestId = 0;
        std::uint32_t targetPid = 0;
        View view = View::Disassembly;
        std::uint64_t targetCreateTime = 0, address = 0;
        std::uint64_t reserved = 0;
    };
    struct Response
    {
        std::uint32_t magic = kMagic, version = kVersion, bytes = sizeof(Response), error = ERROR_SUCCESS;
        std::uint64_t requestId = 0;
        std::uint32_t debuggerPid = 0, targetPid = 0;
        std::uint64_t targetCreateTime = 0, actualAddress = 0;
        std::uint32_t flags = 0, addressBits = sizeof(void*) * 8;
        std::uint64_t reserved = 0;
    };
    struct Receipt
    {
        std::uint32_t magic = kReceiptMagic, version = kVersion, bytes = sizeof(Receipt), reserved = 0;
        std::uint64_t requestId = 0;
    };
#pragma pack(pop)
    static_assert(sizeof(Request) == 56 && sizeof(Response) == 64, "Navigation wire ABI");
    static_assert(sizeof(Receipt) == 24, "Navigation receipt wire ABI");

    inline bool valid(const Request& request) noexcept
    {
        return request.magic == kMagic && request.version == kVersion && request.bytes == sizeof(request)
            && request.requestId != 0 && request.reserved == 0
            && (request.operation == Operation::Query || request.operation == Operation::Navigate)
            && (request.view == View::Disassembly || request.view == View::Dump)
            && (request.operation == Operation::Query || (request.targetPid != 0 && request.targetCreateTime != 0));
    }
    inline bool valid(const Receipt& receipt, std::uint64_t requestId) noexcept
    {
        return receipt.magic == kReceiptMagic && receipt.version == kVersion && receipt.bytes == sizeof(receipt)
            && receipt.reserved == 0 && requestId != 0 && receipt.requestId == requestId;
    }
    inline std::uint64_t creationTime(HANDLE process) noexcept
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        // GetCurrentProcess() is the legitimate pseudo-handle value -1, which
        // is also INVALID_HANDLE_VALUE for file APIs. Process APIs accept it.
        if (process == nullptr || !GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
        return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }
    inline std::wstring pipeName(DWORD pid, std::uint64_t created)
    {
        return L"\\\\.\\pipe\\KSword.X64Dbg.Navigation." + std::to_wstring(pid) + L"." + std::to_wstring(created);
    }
}
