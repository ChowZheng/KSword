#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <iterator>
#include <utility>

namespace ks::winapi_monitor
{
    inline USHORT currentMachine()
    {
#ifdef _WIN64
        return IMAGE_FILE_MACHINE_AMD64;
#else
        return IMAGE_FILE_MACHINE_I386;
#endif
    }
    inline bool platformFailure(std::wstring* error, const wchar_t* operation, DWORD code)
    {
        if (error) *error = std::wstring(operation) + L"; error=" + std::to_wstring(code);
        ::SetLastError(code); return false;
    }
    inline bool queryProcessMachine(HANDLE process, USHORT* machine, std::wstring* error)
    {
        if (!process || !machine) return platformFailure(error, L"invalid process architecture query", ERROR_INVALID_PARAMETER);
        using Query = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
        const auto query = reinterpret_cast<Query>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
        if (query) {
            USHORT processMachine = 0, nativeMachine = 0;
            if (!query(process, &processMachine, &nativeMachine)) return platformFailure(error, L"IsWow64Process2", ::GetLastError());
            *machine = processMachine == IMAGE_FILE_MACHINE_UNKNOWN ? nativeMachine : processMachine;
        } else {
            BOOL wow64 = FALSE;
            if (!::IsWow64Process(process, &wow64)) return platformFailure(error, L"IsWow64Process", ::GetLastError());
            SYSTEM_INFO native{}; ::GetNativeSystemInfo(&native);
            *machine = wow64 || native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? IMAGE_FILE_MACHINE_I386
                : native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_UNKNOWN;
        }
        if (*machine != IMAGE_FILE_MACHINE_I386 && *machine != IMAGE_FILE_MACHINE_AMD64)
            return platformFailure(error, L"unsupported target architecture", ERROR_NOT_SUPPORTED);
        return true;
    }
    inline bool queryProcessMachine(DWORD pid, USHORT* machine, std::wstring* error)
    {
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return platformFailure(error, L"OpenProcess architecture query", ::GetLastError());
        const bool result = queryProcessMachine(process, machine, error);
        const DWORD saved = ::GetLastError(); ::CloseHandle(process); ::SetLastError(saved); return result;
    }
    inline bool queryImageMachine(const std::wstring& path, USHORT* machine, std::wstring* error, bool requireDll = true)
    {
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return platformFailure(error, L"open Agent image", ::GetLastError());
        LARGE_INTEGER size{}; IMAGE_DOS_HEADER dos{}; DWORD read = 0;
        bool valid = ::GetFileSizeEx(file, &size) && ::ReadFile(file, &dos, sizeof(dos), &read, nullptr)
            && read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= sizeof(dos)
            && static_cast<std::uint64_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(WORD)
                <= static_cast<std::uint64_t>(size.QuadPart);
        DWORD signature = 0; IMAGE_FILE_HEADER header{}; WORD magic = 0;
        if (valid) {
            LARGE_INTEGER offset{}; offset.QuadPart = dos.e_lfanew;
            valid = ::SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)
                && ::ReadFile(file, &signature, sizeof(signature), &read, nullptr) && read == sizeof(signature)
                && signature == IMAGE_NT_SIGNATURE
                && ::ReadFile(file, &header, sizeof(header), &read, nullptr) && read == sizeof(header)
                && ::ReadFile(file, &magic, sizeof(magic), &read, nullptr) && read == sizeof(magic)
                && header.SizeOfOptionalHeader >= sizeof(WORD)
                && (!requireDll || (header.Characteristics & IMAGE_FILE_DLL))
                && ((header.Machine == IMAGE_FILE_MACHINE_I386 && magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
                    || (header.Machine == IMAGE_FILE_MACHINE_AMD64 && magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC));
        }
        ::CloseHandle(file);
        if (!valid) return platformFailure(error, L"invalid or unsupported Agent PE", ERROR_BAD_EXE_FORMAT);
        if (machine) *machine = header.Machine; return true;
    }
    inline std::wstring platformDirectory(const std::wstring& path)
    {
        const auto separator = path.find_last_of(L"\\/");
        return separator == std::wstring::npos ? L"." : path.substr(0, separator);
    }
    inline const wchar_t* agentFileName(USHORT machine)
    { return machine == IMAGE_FILE_MACHINE_I386 ? L"APIMonitor_x86.dll" : L"APIMonitor_x64.dll"; }
    inline bool resolveAgentPath(DWORD pid, const std::wstring& requested, std::wstring* resolved, std::wstring* error)
    {
        if (!resolved || requested.empty()) return platformFailure(error, L"empty Agent path", ERROR_INVALID_PARAMETER);
        USHORT target = 0;
        if (!queryProcessMachine(pid, &target, error)) return false;
        std::vector<wchar_t> absolute(32768);
        const DWORD count = ::GetFullPathNameW(requested.c_str(), static_cast<DWORD>(absolute.size()), absolute.data(), nullptr);
        if (!count || count >= absolute.size()) return platformFailure(error, L"invalid Agent path", ERROR_BAD_PATHNAME);
        std::wstring selected(absolute.data());
        const auto name = selected.substr(selected.find_last_of(L"\\/") + 1);
        if (_wcsicmp(name.c_str(), L"APIMonitor_x64.dll") == 0 || _wcsicmp(name.c_str(), L"APIMonitor_x86.dll") == 0)
            selected = platformDirectory(selected) + L"\\" + agentFileName(target);
        USHORT image = 0;
        if (!queryImageMachine(selected, &image, error)) return false;
        if (image != target) return platformFailure(error, L"Agent DLL architecture does not match target process", ERROR_BAD_EXE_FORMAT);
        *resolved = std::move(selected); return true;
    }
}
