#include "WorkbenchPointerChainAccess.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <vector>

namespace ksword::memwb_pointer_access
{
    namespace
    {
        std::uint64_t Time(const FILETIME& value)
        {
            return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) | value.dwLowDateTime;
        }

        std::wstring Wide(const std::string& value)
        {
            const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (count <= 0) return {};
            std::wstring result(static_cast<std::size_t>(count), L'\0');
            ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
            return result;
        }

        std::string Utf8(const std::wstring& value)
        {
            const int count = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (count <= 0) return {};
            std::string result(static_cast<std::size_t>(count), '\0');
            ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
            return result;
        }

        std::wstring Normalize(std::wstring value)
        {
            std::replace(value.begin(), value.end(), L'/', L'\\');
            if (value.compare(0, 8, L"\\\\?\\UNC\\") == 0) value = L"\\\\" + value.substr(8);
            else if (value.compare(0, 4, L"\\\\?\\") == 0) value.erase(0, 4);
            return value;
        }

        bool PathMatches(const std::wstring& left, const std::wstring& right)
        {
            const auto a = Normalize(left);
            const auto b = Normalize(right);
            return !a.empty() && !b.empty() && ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        }

        bool Alive(const HANDLE handle, const memwb::MemoryTargetSession& session)
        {
            FILETIME creation{}, exit{}, kernel{}, user{};
            DWORD code = 0;
            return handle && ::GetProcessId(handle) == session.pid
                && ::GetProcessTimes(handle, &creation, &exit, &kernel, &user)
                && Time(creation) == session.processCreateTime100ns && Time(exit) == 0
                && ::GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
        }

        std::shared_ptr<void> Open(const memwb::MemoryTargetSession& session)
        {
            HANDLE handle = ::OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, session.pid);
            if (!handle) handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, session.pid);
            return std::shared_ptr<void>(handle, [](void* h) { if (h) ::CloseHandle(h); });
        }

        ModuleCapture Capture(const memwb::MemoryTargetSession& session, const std::string& path, const HANDLE handle)
        {
            ModuleCapture capture;
            if (memwb::Validate(session) != memwb::SessionError::None || session.scope != memwb::Scope::ProcessVirtual)
            { capture.issue = Issue::InvalidTarget; return capture; }
            if (session.channel == memwb::Channel::Ddma)
            { capture.issue = Issue::UnsupportedChannel; return capture; }
            if (session.processCreateTime100ns == 0)
            { capture.issue = Issue::IdentityUnavailable; return capture; }
            if (!Alive(handle, session)) { capture.issue = Issue::TargetChanged; return capture; }
            wchar_t processPath[32768]{};
            DWORD length = 32768;
            BOOL wow64 = FALSE;
            if (!::QueryFullProcessImageNameW(handle, 0, processPath, &length)
                || !::IsWow64Process(handle, &wow64) || session.addressBits != (wow64 ? 32U : 64U))
            { capture.issue = Issue::IdentityUnavailable; return capture; }
            const auto modulePath = Wide(path);
            HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, session.pid);
            if (snapshotHandle == INVALID_HANDLE_VALUE) { capture.issue = Issue::ModuleUnavailable; return capture; }
            const std::shared_ptr<void> snapshot(snapshotHandle, [](void* h) { ::CloseHandle(h); });
            MODULEENTRY32W module{};
            module.dwSize = sizeof(module);
            int matches = 0, count = 0;
            BOOL more = ::Module32FirstW(snapshotHandle, &module);
            while (more && count++ < 8192)
            {
                if (PathMatches(module.szExePath, modulePath))
                {
                    ++matches;
                    capture.base = reinterpret_cast<std::uint64_t>(module.modBaseAddr);
                    capture.definition.moduleSize = module.modBaseSize;
                    capture.definition.modulePath = Utf8(Normalize(module.szExePath));
                }
                more = ::Module32NextW(snapshotHandle, &module);
            }
            if (more || ::GetLastError() != ERROR_NO_MORE_FILES || matches != 1 || !Alive(handle, session))
            { capture.issue = Issue::ModuleUnavailable; return capture; }
            WIN32_FILE_ATTRIBUTE_DATA file{};
            if (!::GetFileAttributesExW(modulePath.c_str(), GetFileExInfoStandard, &file)
                || (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            { capture.issue = Issue::ModuleUnavailable; return capture; }
            capture.definition.moduleFileSize = static_cast<std::int64_t>(
                (static_cast<std::uint64_t>(file.nFileSizeHigh) << 32U) | file.nFileSizeLow);
            capture.definition.moduleFileTime = static_cast<std::int64_t>(Time(file.ftLastWriteTime) / 10000U) - 11644473600000LL;
            capture.definition.processPath = Utf8(Normalize(std::wstring(processPath, length)));
            capture.definition.pointerSize = session.addressBits / 8U;
            capture.ok = true;
            return capture;
        }
    }

    ModuleCapture CaptureModule(const memwb::MemoryTargetSession& session, const std::string& modulePath)
    {
        const auto handle = Open(session);
        return Capture(session, modulePath, static_cast<HANDLE>(handle.get()));
    }

    Resolution Resolve(const memwb::AddressEntry& entry, const memwb::MemoryTargetSession& session,
        memwb::IMemoryIoPort& port, const std::shared_ptr<std::atomic<bool>>& cancel)
    {
        Resolution resolution;
        if (!entry.pointerChain || !memwb::MemoryAddressBook::ValidPointerChain(entry))
        { resolution.issue = Issue::InvalidRoot; return resolution; }
        const auto snapshot = session;
        const auto definition = *entry.pointerChain;
        const auto handle = Open(snapshot);
        const auto captured = Capture(snapshot, definition.modulePath, static_cast<HANDLE>(handle.get()));
        if (!captured.ok) { resolution.issue = captured.issue; return resolution; }
        const auto& now = captured.definition;
        if (!PathMatches(Wide(now.processPath), Wide(definition.processPath)))
        { resolution.issue = Issue::WrongProgram; return resolution; }
        if (now.moduleSize != definition.moduleSize || now.moduleFileSize != definition.moduleFileSize
            || now.moduleFileTime != definition.moduleFileTime || now.pointerSize != definition.pointerSize)
        { resolution.issue = Issue::ModuleChanged; return resolution; }
        if (entry.rva >= now.moduleSize || definition.pointerSize > now.moduleSize - entry.rva
            || captured.base > (std::numeric_limits<std::uint64_t>::max)() - entry.rva)
        { resolution.issue = Issue::InvalidRoot; return resolution; }
        const auto root = captured.base + entry.rva;
        const auto mapped = [&]() {
            for (const auto address : { root, root + definition.pointerSize - 1 })
            {
                MEMORY_BASIC_INFORMATION info{};
                if (::VirtualQueryEx(handle.get(), reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)), &info, sizeof(info)) != sizeof(info)
                    || reinterpret_cast<std::uint64_t>(info.AllocationBase) != captured.base
                    || info.Type != MEM_IMAGE || info.State != MEM_COMMIT) return false;
            }
            return true;
        };
        if (!mapped()) { resolution.issue = Issue::RootRemapped; return resolution; }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        const auto more = [&]() {
            if (!memwb::SameTarget(session, snapshot) || !Alive(static_cast<HANDLE>(handle.get()), snapshot))
            { resolution.issue = Issue::TargetChanged; return false; }
            return (!cancel || !cancel->load()) && std::chrono::steady_clock::now() < deadline;
        };
        pointer_chain::Chain chain;
        chain.rootOffset = entry.rva;
        chain.offsets = definition.offsets;
        chain.pointerSize = definition.pointerSize;
        resolution.result = pointer_chain::Resolve(chain, root, [&](std::uint64_t address, std::size_t count) {
            pointer_chain::ReadResult result;
            if (!more() || !memwb::AddrSpaceContains(memwb::ScopeAddressSpace(snapshot), address)
                || count - 1 > memwb::ScopeAddressSpace(snapshot).last - address) return result;
            const auto read = port.Read(snapshot, address, count);
            result.ok = read.status == memwb::IoReadStatus::Ok && !read.scratchAreaDirty;
            result.bytesRead = read.data.size();
            result.bytes = read.data;
            if (!read.failure.empty()) resolution.annotation = read.failure;
            return result;
        }, more);
        if (resolution.result.ok() && !mapped())
        {
            resolution.result.status = pointer_chain::Status::ReadFailed;
            resolution.result.address = 0;
            resolution.issue = Issue::RootRemapped;
        }
        if (resolution.result.ok())
        {
            const auto after = Capture(snapshot, definition.modulePath, static_cast<HANDLE>(handle.get()));
            if (!after.ok || after.base != captured.base || after.definition != captured.definition)
            {
                resolution.result.status = pointer_chain::Status::ReadFailed;
                resolution.result.address = 0;
                resolution.issue = after.ok ? Issue::ModuleChanged : after.issue;
            }
            else if (!more())
            {
                resolution.result.status = pointer_chain::Status::Cancelled;
                resolution.result.address = 0;
            }
        }
        const auto space = memwb::ScopeAddressSpace(snapshot);
        const auto outside = std::find_if(resolution.result.steps.begin(), resolution.result.steps.end(), [&](const auto& step) {
            return !memwb::AddrSpaceContains(space, step.pointerValue)
                || !memwb::AddrSpaceContains(space, step.resolvedAddress);
        });
        if (resolution.result.ok() && (!memwb::AddrSpaceContains(space, resolution.result.address)
            || outside != resolution.result.steps.end()))
        {
            resolution.result.status = pointer_chain::Status::AddressOverflow;
            resolution.result.address = 0;
            resolution.result.failedLevel = outside != resolution.result.steps.end()
                ? static_cast<std::size_t>(outside - resolution.result.steps.begin()) : resolution.result.steps.size() - 1;
            resolution.result.steps.resize(resolution.result.failedLevel);
        }
        if (!resolution.result.ok() && resolution.issue == Issue::None)
            resolution.issue = resolution.result.status == pointer_chain::Status::Cancelled ? Issue::Cancelled : Issue::ReadFailed;
        return resolution;
    }
}
