#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ksword::titan
{
    // VirtualFreeEx decommits every page touched by [address,address+bytes).
    // Inclusive endpoints avoid overflowing when the final page ends at UINTPTR_MAX.
    inline bool decommitCovers(std::uintptr_t address, std::size_t bytes,
        std::uintptr_t binding, std::uintptr_t pageSize) noexcept
    {
        if (bytes == 0 || pageSize == 0 || (pageSize & (pageSize - 1)) != 0 ||
            bytes - 1 > (std::numeric_limits<std::uintptr_t>::max)() - address) return false;
        const auto first = address & ~(pageSize - 1);
        const auto last = (address + bytes - 1) | (pageSize - 1);
        return binding >= first && binding <= last;
    }

    inline bool moduleOwnsBinding(bool hvm, std::uint32_t bindingPid,
        std::uintptr_t allocationBase, std::uint32_t eventPid,
        std::uintptr_t unloadedBase) noexcept
    {
        return hvm && unloadedBase != 0 && allocationBase == unloadedBase && bindingPid == eventPid;
    }
}
