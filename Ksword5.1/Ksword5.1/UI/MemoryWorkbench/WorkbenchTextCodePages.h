#pragma once

#include "../../../../shared/evidence/memory_workbench/MemoryTextDecode.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ks::ui
{
    inline ksword::memwb::TextCodePageResult DecodeWorkbenchCodePage(
        const ksword::memwb::MemoryTextEncoding encoding, const std::uint8_t* bytes, const std::size_t available)
    {
        using ksword::memwb::MemoryTextEncoding;
        ksword::memwb::TextCodePageResult result;
        if (available == 0) return result;
#ifdef _WIN32
        const UINT page = encoding == MemoryTextEncoding::Gbk ? 936U
            : encoding == MemoryTextEncoding::Gb18030 ? 54936U : encoding == MemoryTextEncoding::Big5 ? 950U : GetACP();
        if (bytes[0] < 0x80U)
        {
            result.ok = true; result.scalar = bytes[0]; result.consumed = 1;
            return result;
        }
        std::size_t length = 1;
        if (page == CP_UTF8)
        {
            if ((bytes[0] & 0xE0U) == 0xC0U) length = 2;
            else if ((bytes[0] & 0xF0U) == 0xE0U) length = 3;
            else if ((bytes[0] & 0xF8U) == 0xF0U) length = 4;
        }
        else if (page == 54936U && bytes[0] >= 0x81U && bytes[0] <= 0xFEU)
        {
            length = available > 1 && bytes[1] >= 0x30U && bytes[1] <= 0x39U ? 4U : 2U;
        }
        else if (IsDBCSLeadByteEx(page, bytes[0])) length = 2;
        result.consumed = length; // also reports an incomplete character to the core
        if (length > available) return result;
        wchar_t output[2]{};
        // MB_ERR_INVALID_CHARS is supported by the selected Windows pages,
        // including CP_UTF8 and GB18030; never retry with replacement enabled.
        const int count = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(bytes), static_cast<int>(length), output, 2);
        if (count == 1 && (output[0] < 0xD800 || output[0] > 0xDFFF))
        { result.ok = true; result.scalar = static_cast<std::uint32_t>(output[0]); }
        else if (count == 2 && output[0] >= 0xD800 && output[0] <= 0xDBFF
            && output[1] >= 0xDC00 && output[1] <= 0xDFFF)
        {
            result.ok = true;
            result.scalar = 0x10000U + ((static_cast<std::uint32_t>(output[0]) - 0xD800U) << 10U)
                + static_cast<std::uint32_t>(output[1]) - 0xDC00U;
        }
#else
        // Portable tests inject their own code page adapter. ASCII is still
        // interpretable without a platform conversion service.
        static_cast<void>(encoding);
        result.consumed = 1;
        result.ok = bytes[0] < 0x80U;
        result.scalar = bytes[0];
#endif
        return result;
    }
}
