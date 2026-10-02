#pragma once
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace apimon
{
    inline bool ReadExportMemory(const void* source, void* destination, std::size_t size)
    {
        const auto first = reinterpret_cast<std::uintptr_t>(source);
        if (!source || size > static_cast<std::uintptr_t>(-1) - first) return false;
        auto cursor = first;
        while (cursor < first + size)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (!::VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info))
                || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
            const DWORD access = info.Protect & 0xFF;
            if (access != PAGE_READONLY && access != PAGE_READWRITE && access != PAGE_WRITECOPY
                && access != PAGE_EXECUTE_READ && access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY) return false;
            const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + static_cast<std::uintptr_t>(info.RegionSize);
            if (end <= cursor) return false;
            cursor = (std::min)(end, first + size);
        }
        __try { std::memcpy(destination, source, size); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    inline bool EnumerateNamedExports(HMODULE module, std::vector<std::string>* names)
    {
        if (!module || !names) return false;
        names->clear();
        // A loader reference pins the image while the export catalog and hook addresses are used.
        HMODULE pinned = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return false;
        const auto* base = reinterpret_cast<const unsigned char*>(module);
        IMAGE_DOS_HEADER dos{};
        if (!ReadExportMemory(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE
            || dos.e_lfanew <= 0 || dos.e_lfanew > 1024 * 1024) return false;
        IMAGE_NT_HEADERS nt{};
        if (!ReadExportMemory(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE
            || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC
            || nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER)
            || nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return false;
        const auto size = nt.OptionalHeader.SizeOfImage;
        const auto valid = [size](DWORD rva, std::size_t count) { return rva && rva < size && count <= size - rva; };
        const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        IMAGE_EXPORT_DIRECTORY exports{};
        if (directory.Size < sizeof(exports) || !valid(directory.VirtualAddress, directory.Size)
            || !ReadExportMemory(base + directory.VirtualAddress, &exports, sizeof(exports))
            || !exports.NumberOfNames || exports.NumberOfNames > 65536
            || !valid(exports.AddressOfNames, exports.NumberOfNames * sizeof(DWORD))) return false;
        std::vector<std::string> result;
        result.reserve(exports.NumberOfNames);
        std::vector<DWORD> nameRvas(exports.NumberOfNames);
        if (!ReadExportMemory(base + exports.AddressOfNames, nameRvas.data(), nameRvas.size() * sizeof(DWORD))) return false;
        for (DWORD index = 0; index < exports.NumberOfNames; ++index)
        {
            const DWORD rva = nameRvas[index];
            if (!valid(rva, 1)) return false;
            char name[512]{};
            const auto available = (std::min)(sizeof(name), static_cast<std::size_t>(size - rva));
            if (!ReadExportMemory(base + rva, name, available)) return false;
            const auto* terminator = static_cast<const char*>(std::memchr(name, 0, available));
            if (!terminator || terminator == name) return false;
            const auto length = static_cast<std::size_t>(terminator - name);
            result.emplace_back(name, length);
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        names->swap(result);
        return !names->empty();
    }
}
