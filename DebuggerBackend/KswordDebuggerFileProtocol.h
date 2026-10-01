#pragma once
#include <Windows.h>
#include <array>
#include <string>

namespace ksword::debugger
{
    // Writers replace complete packets atomically. A reader must share delete
    // access and close before dispatch, UI work or waiting for another packet.
    inline bool readControlPacket(const std::wstring& path, std::string& packet)
    {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        std::array<char, 512> bytes{};
        DWORD count = 0;
        const BOOL read = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr);
        CloseHandle(file);
        if (!read || count == bytes.size()) return false;
        packet.assign(bytes.data(), count);
        return true;
    }
}
