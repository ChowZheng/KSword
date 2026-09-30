#include "KswordDebuggerBackend.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace ksword::debugger
{
    namespace
    {
        bool sameObject(HANDLE first, HANDLE second)
        {
            using Compare = LONG(NTAPI*)(HANDLE, HANDLE);
            static const auto compare = reinterpret_cast<Compare>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCompareObjects"));
            return compare != nullptr && compare(first, second) >= 0;
        }
    }
    HANDLE Backend::openProcess(DWORD access, BOOL inherit, DWORD pid)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (pid == 0) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
        // Preserve rights for CE operations outside the SDK table (for example
        // VirtualFreeEx), then fall back to identity-only access when necessary.
        HANDLE handle = OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, inherit, pid);
        if (handle == nullptr) handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, inherit, pid);
        if (handle != nullptr) return handle;
        for (auto iterator = processIds_.begin(); iterator != processIds_.end();)
        {
            if (!sameObject(iterator->first, iterator->second.identity.native())) iterator = processIds_.erase(iterator);
            else ++iterator;
        }
        if (processIds_.size() >= 1024) { SetLastError(ERROR_TOO_MANY_OPEN_FILES); return nullptr; }
        handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE identity = nullptr;
        if (handle == nullptr) return nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &identity, 0, FALSE, DUPLICATE_SAME_ACCESS))
        { const DWORD error = GetLastError(); CloseHandle(handle); SetLastError(error); return nullptr; }
        processIds_[handle] = ProcessProxy{pid, ark::DriverHandle(identity)};
        return handle;
    }

    DWORD Backend::processId(HANDLE process)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const DWORD actual = GetProcessId(process);
        if (actual != 0) return actual;
        const auto iterator = processIds_.find(process);
        if (iterator == processIds_.end() || !sameObject(process, iterator->second.identity.native()))
        { if (iterator != processIds_.end()) processIds_.erase(iterator); SetLastError(ERROR_INVALID_HANDLE); return 0; }
        return iterator->second.pid;
    }

    bool Backend::transfer(bool write, DWORD pid, std::uint64_t address, void* data,
        SIZE_T bytes, SIZE_T& transferred)
    {
        transferred = 0;
        if (pid == 0 || (bytes != 0 && (data == nullptr || address == 0)) ||
            bytes > (std::numeric_limits<std::uint64_t>::max)() - address)
        { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        if (!driver_.isValid()) { SetLastError(ERROR_DEVICE_NOT_CONNECTED); return false; }
        while (transferred < bytes)
        {
            const auto count = static_cast<DWORD>((std::min)(bytes - transferred,
                static_cast<SIZE_T>(useHvm_ ? KSWORD_ARK_HVM_MEMORY_MAX_BYTES :
                    (write ? KSWORD_ARK_MEMORY_WRITE_MAX_BYTES : KSWORD_ARK_MEMORY_READ_MAX_BYTES))));
            auto* part = static_cast<unsigned char*>(data) + transferred;
            if (useHvm_)
            {
                const auto result = client_.hvmMemory(write ? KSWORD_ARK_HVM_MEMORY_OP_WRITE_VIRTUAL : KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL,
                    address + transferred, 0, count, write ? part : nullptr, true, true, pid, &driver_);
                if (!result.io.ok || result.response.status != KSWORD_ARK_HVM_MEMORY_STATUS_OK || result.response.usedDirectWindow == 0)
                { SetLastError(result.io.ok ? ERROR_PARTIAL_COPY : result.io.win32Error); return false; }
                const DWORD done = result.response.bytesTransferred;
                if (done > count) { SetLastError(ERROR_INVALID_DATA); return false; }
                if (!write) std::memcpy(part, result.response.data, done);
                transferred += done;
                if (done != count) { SetLastError(ERROR_PARTIAL_COPY); return false; }
            }
            else if (write)
            {
                const std::vector<std::uint8_t> payload(part, part + count);
                const auto result = client_.writeVirtualMemory(pid, address + transferred, payload,
                    KSWORD_ARK_MEMORY_WRITE_FLAG_UI_CONFIRMED, &driver_);
                if (!result.io.ok) { SetLastError(result.io.win32Error); return false; }
                if (result.bytesWritten > count) { SetLastError(ERROR_INVALID_DATA); return false; }
                transferred += result.bytesWritten;
                if (result.writeStatus != KSWORD_ARK_MEMORY_WRITE_STATUS_OK || result.bytesWritten != count)
                { SetLastError(ERROR_PARTIAL_COPY); return false; }
            }
            else
            {
                const auto result = client_.readVirtualMemory(pid, address + transferred, count, 0, &driver_);
                if (!result.io.ok) { SetLastError(result.io.win32Error); return false; }
                if (result.data.size() > count) { SetLastError(ERROR_INVALID_DATA); return false; }
                std::memcpy(part, result.data.data(), result.data.size());
                transferred += result.data.size();
                if (result.readStatus != KSWORD_ARK_MEMORY_READ_STATUS_OK || result.data.size() != count)
                { SetLastError(ERROR_PARTIAL_COPY); return false; }
            }
        }
        SetLastError(ERROR_SUCCESS);
        return true;
    }

    BOOL Backend::readMemory(HANDLE process, LPCVOID address, LPVOID data, SIZE_T bytes, SIZE_T* transferred)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        SIZE_T done = 0;
        const bool ok = transfer(false, processId(process), reinterpret_cast<std::uintptr_t>(address), data, bytes, done);
        if (transferred != nullptr) *transferred = done;
        return ok ? TRUE : FALSE;
    }

    BOOL Backend::writeMemory(HANDLE process, LPVOID address, LPCVOID data, SIZE_T bytes, SIZE_T* transferred)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        SIZE_T done = 0;
        const bool ok = transfer(true, processId(process), reinterpret_cast<std::uintptr_t>(address), const_cast<void*>(data), bytes, done);
        if (transferred != nullptr) *transferred = done;
        return ok ? TRUE : FALSE;
    }

    SIZE_T Backend::queryMemory(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T bytes)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (info == nullptr || bytes < sizeof(*info)) { SetLastError(ERROR_BAD_LENGTH); return 0; }
        const auto requested = reinterpret_cast<std::uintptr_t>(address);
        const auto result = client_.queryVirtualMemory(processId(process), requested, 0, &driver_);
        if (!result.io.ok || (result.queryStatus != KSWORD_ARK_MEMORY_QUERY_STATUS_OK && result.queryStatus != KSWORD_ARK_MEMORY_QUERY_STATUS_PARTIAL) ||
            (result.fieldFlags & KSWORD_ARK_MEMORY_FIELD_BASIC_PRESENT) == 0 ||
            result.regionSize == 0 || result.baseAddress > requested ||
            result.regionSize > (std::numeric_limits<std::uint64_t>::max)() - result.baseAddress ||
            requested >= result.baseAddress + result.regionSize ||
            result.baseAddress > (std::numeric_limits<std::uintptr_t>::max)() ||
            result.allocationBase > (std::numeric_limits<std::uintptr_t>::max)() ||
            result.regionSize > (std::numeric_limits<SIZE_T>::max)())
        { SetLastError(result.io.ok ? ERROR_INVALID_DATA : result.io.win32Error); return 0; }
        *info = MEMORY_BASIC_INFORMATION{};
        info->BaseAddress = reinterpret_cast<void*>(static_cast<std::uintptr_t>(result.baseAddress));
        info->AllocationBase = reinterpret_cast<void*>(static_cast<std::uintptr_t>(result.allocationBase));
        info->AllocationProtect = result.allocationProtect;
        info->RegionSize = static_cast<SIZE_T>(result.regionSize);
        info->State = result.state; info->Protect = result.protect; info->Type = result.type;
        return sizeof(*info);
    }

    BOOL Backend::protectMemory(HANDLE process, LPVOID address, SIZE_T bytes, DWORD protection, PDWORD previous)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (previous == nullptr) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = KSWORD_ARK_DEBUGGER_PROTECT; request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
        request.processId = processId(process); request.address = reinterpret_cast<std::uintptr_t>(address);
        request.bytes = bytes; request.protection = protection;
        const DWORD error = nativeRequest(request, response);
        if (error == ERROR_SUCCESS) *previous = response.previousProtection;
        SetLastError(error); return error == ERROR_SUCCESS ? TRUE : FALSE;
    }

    LPVOID Backend::allocateMemory(HANDLE process, LPVOID address, SIZE_T bytes, DWORD type, DWORD protection)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = KSWORD_ARK_DEBUGGER_ALLOCATE; request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
        request.processId = processId(process); request.address = reinterpret_cast<std::uintptr_t>(address);
        request.bytes = bytes; request.allocationType = type; request.protection = protection;
        const DWORD error = nativeRequest(request, response);
        SetLastError(error);
        return error == ERROR_SUCCESS ? reinterpret_cast<void*>(static_cast<std::uintptr_t>(response.address)) : nullptr;
    }

    HANDLE Backend::createThread(HANDLE process, LPSECURITY_ATTRIBUTES attributes, SIZE_T stackBytes,
        LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD tid)
    {
        const HANDLE handle = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, processId(process));
        if (handle == nullptr) return nullptr;
        HANDLE result = CreateRemoteThread(handle, attributes, stackBytes, start, parameter, flags, tid);
        const DWORD error = GetLastError(); CloseHandle(handle); SetLastError(error); return result;
    }
}
