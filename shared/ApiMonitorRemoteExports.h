#pragma once
#include "ApiMonitorPlatform.h"
#include <TlHelp32.h>
#include <algorithm>
#include <cstring>

namespace ks::winapi_monitor
{
    inline HANDLE remoteModuleSnapshot(DWORD pid)
    {
        HANDLE snapshot = INVALID_HANDLE_VALUE;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snapshot != INVALID_HANDLE_VALUE || ::GetLastError() != ERROR_BAD_LENGTH) break;
        }
        return snapshot;
    }
    inline bool readRemoteBytes(HANDLE process, std::uintptr_t address, void* output, SIZE_T size)
    {
        SIZE_T read = 0;
        return address && size <= UINTPTR_MAX - address
            && ::ReadProcessMemory(process, reinterpret_cast<const void*>(address), output, size, &read) && read == size;
    }
    // Parse the target's loaded PE, never a local DLL's RVA from a different architecture.
    inline std::uintptr_t findRemoteExport(HANDLE process, DWORD pid, USHORT machine,
        std::wstring moduleName, const std::string& name, unsigned depth = 0)
    {
        if (depth >= 8 || name.empty()) return 0;
        if (moduleName.find(L'.') == std::wstring::npos) moduleName += L".dll";
        HANDLE snapshot = remoteModuleSnapshot(pid);
        if (snapshot == INVALID_HANDLE_VALUE) return 0;
        MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry); std::uintptr_t base = 0; DWORD imageSize = 0;
        for (BOOL found = ::Module32FirstW(snapshot, &entry); found; found = ::Module32NextW(snapshot, &entry)) {
            if (_wcsicmp(entry.szModule, moduleName.c_str()) != 0) continue;
            IMAGE_DOS_HEADER dos{}; IMAGE_FILE_HEADER file{}; DWORD signature = 0;
            const auto address = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
            if (!readRemoteBytes(process, address, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE
                || dos.e_lfanew < sizeof(dos) || static_cast<unsigned>(dos.e_lfanew) > entry.modBaseSize
                || entry.modBaseSize - dos.e_lfanew < sizeof(DWORD) + sizeof(file)
                || !readRemoteBytes(process, address + dos.e_lfanew, &signature, sizeof(signature))
                || signature != IMAGE_NT_SIGNATURE
                || !readRemoteBytes(process, address + dos.e_lfanew + sizeof(DWORD), &file, sizeof(file))
                || file.Machine != machine) continue;
            base = address; imageSize = entry.modBaseSize; break;
        }
        ::CloseHandle(snapshot);
        if (!base || imageSize < sizeof(IMAGE_DOS_HEADER) || imageSize > UINTPTR_MAX - base
            || (machine == IMAGE_FILE_MACHINE_I386 && base + imageSize > UINT64_C(0x100000000))) return 0;
        const auto valid = [imageSize](DWORD rva, SIZE_T size) { return rva && rva < imageSize && size <= imageSize - rva; };
        IMAGE_DOS_HEADER dos{}; WORD magic = 0;
        if (!readRemoteBytes(process, base, &dos, sizeof(dos))) return 0;
        const DWORD optionalRva = dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        if (!valid(optionalRva, sizeof(magic)) || !readRemoteBytes(process, base + optionalRva, &magic, sizeof(magic))) return 0;
        IMAGE_DATA_DIRECTORY directory{}; DWORD declaredSize = 0;
        if (machine == IMAGE_FILE_MACHINE_I386 && magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            IMAGE_OPTIONAL_HEADER32 header{};
            if (!valid(optionalRva, sizeof(header)) || !readRemoteBytes(process, base + optionalRva, &header, sizeof(header))
                || header.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return 0;
            directory = header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]; declaredSize = header.SizeOfImage;
        } else if (machine == IMAGE_FILE_MACHINE_AMD64 && magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            IMAGE_OPTIONAL_HEADER64 header{};
            if (!valid(optionalRva, sizeof(header)) || !readRemoteBytes(process, base + optionalRva, &header, sizeof(header))
                || header.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return 0;
            directory = header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]; declaredSize = header.SizeOfImage;
        } else return 0;
        if (!declaredSize || declaredSize > imageSize) return 0;
        imageSize = declaredSize;
        IMAGE_EXPORT_DIRECTORY exports{};
        if (directory.Size < sizeof(exports) || !valid(directory.VirtualAddress, directory.Size)
            || !readRemoteBytes(process, base + directory.VirtualAddress, &exports, sizeof(exports))
            || !exports.NumberOfFunctions || exports.NumberOfFunctions > 65536 || exports.NumberOfNames > 65536
            || !valid(exports.AddressOfFunctions, exports.NumberOfFunctions * sizeof(DWORD))) return 0;
        std::vector<DWORD> functions(exports.NumberOfFunctions);
        if (!readRemoteBytes(process, base + exports.AddressOfFunctions, functions.data(), functions.size() * sizeof(DWORD))) return 0;
        const auto stringAt = [&](DWORD rva, std::string& output) {
            output.clear();
            for (unsigned i = 0; i < 512; ++i) {
                char ch = 0;
                if (i > UINT32_MAX - rva || !valid(rva + i, 1) || !readRemoteBytes(process, base + rva + i, &ch, 1)) return false;
                if (!ch) return !output.empty(); output.push_back(ch);
            }
            return false;
        };
        DWORD index = UINT32_MAX;
        if (name[0] == '#') {
            std::uint64_t ordinal = 0;
            if (name.size() == 1) return 0;
            for (size_t i = 1; i < name.size(); ++i) {
                if (name[i] < '0' || name[i] > '9' || ordinal > 65535) return 0;
                ordinal = ordinal * 10 + name[i] - '0';
            }
            if (ordinal < exports.Base || ordinal - exports.Base >= functions.size()) return 0;
            index = static_cast<DWORD>(ordinal - exports.Base);
        } else {
            if (!valid(exports.AddressOfNames, exports.NumberOfNames * sizeof(DWORD))
                || !valid(exports.AddressOfNameOrdinals, exports.NumberOfNames * sizeof(WORD))) return 0;
            std::vector<DWORD> names(exports.NumberOfNames); std::vector<WORD> ordinals(exports.NumberOfNames);
            if (!readRemoteBytes(process, base + exports.AddressOfNames, names.data(), names.size() * sizeof(DWORD))
                || !readRemoteBytes(process, base + exports.AddressOfNameOrdinals, ordinals.data(), ordinals.size() * sizeof(WORD))) return 0;
            for (size_t i = 0; i < names.size(); ++i) {
                std::string candidate;
                if (!stringAt(names[i], candidate)) return 0;
                if (candidate == name) { index = ordinals[i]; break; }
            }
        }
        if (index >= functions.size() || !valid(functions[index], 1)) return 0;
        const DWORD rva = functions[index];
        if (rva >= directory.VirtualAddress && rva - directory.VirtualAddress < directory.Size) {
            std::string forward;
            if (!stringAt(rva, forward)) return 0;
            const auto dot = forward.find_last_of('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 == forward.size()) return 0;
            const std::wstring module(forward.begin(), forward.begin() + dot);
            return findRemoteExport(process, pid, machine, module, forward.substr(dot + 1), depth + 1);
        }
        MEMORY_BASIC_INFORMATION memory{};
        if (!::VirtualQueryEx(process, reinterpret_cast<void*>(base + rva), &memory, sizeof(memory))
            || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        const DWORD protection = memory.Protect & 0xFF;
        if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE
            && protection != PAGE_EXECUTE_WRITECOPY) return 0;
        return base + rva;
    }
}
