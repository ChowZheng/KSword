#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace ksword::pfn {
constexpr std::uint64_t pageBytes = 4096;
enum class Use : std::uint8_t {
    Private, MappedFile, Shareable, PageTable, PagedPool, NonpagedPool,
    SystemPte, SessionPrivate, Metafile, Awe, DriverLocked, KernelStack,
    Image, Compression, Unassigned, Unknown, Count
};
constexpr std::size_t useCount = static_cast<std::size_t>(Use::Count);
struct Range { std::uint64_t first = 0, count = 0; };
struct Identity { std::uint64_t frame = ~0ULL, backing = 0; };

// Normalize overlapping/unsorted RAM extents before querying. Holes are never
// queried or converted into hardware/hypervisor consumption.
inline bool normalizeRanges(std::vector<Range>& ranges)
{
    for (const auto& range : ranges) {
        if (!range.count || range.first >= (1ULL << 40) || range.count > (1ULL << 40) - range.first) { return false; }
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Range> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().first + merged.back().count) {
            auto& last = merged.back();
            last.count = std::max(last.first + last.count, range.first + range.count) - last.first;
        } else { merged.push_back(range); }
    }
    ranges.swap(merged);
    return true;
}

inline unsigned state(const Identity& page) { return static_cast<unsigned>((page.frame >> 4) & 7); }
inline unsigned nativeUse(const Identity& page) { return static_cast<unsigned>(page.frame & 15); }
inline std::uint64_t processKey(const Identity& page) { return (page.frame >> 9) & 0xFFFFFFFFFFFFULL; }
inline bool available(unsigned list) { return list <= 2; }
inline Use classify(const Identity& page, bool compressionOwner = false)
{
    const unsigned use = nativeUse(page);
    if (page.frame == ~0ULL) { return Use::Unknown; }
    if (state(page) < 2) { return Use::Unassigned; }
    if (use > 11) { return Use::Unknown; }
    if (use == 0 && compressionOwner) { return Use::Compression; }
    if (use == 1 && (page.backing & 1) != 0) { return Use::Image; }
    return static_cast<Use>(use);
}

struct Accounting {
    std::array<std::array<std::uint64_t, 8>, useCount> byUseAndState{};
    std::uint64_t expected = 0, visited = 0, valid = 0, unreadable = 0, availablePages = 0;
    std::uint64_t pinned = 0, nonTradeable = 0;
    void add(const Identity& page, Use use) {
        ++visited;
        if (page.frame == ~0ULL) { ++unreadable; return; }
        ++valid;
        const auto list = state(page);
        ++byUseAndState[static_cast<std::size_t>(use)][list];
        availablePages += available(list) ? 1 : 0;
        pinned += (page.frame >> 8) & 1;
        nonTradeable += (page.frame >> 60) & 1;
    }
    void failed(std::uint64_t count) { visited += count; unreadable += count; }
    std::uint64_t inUse(Use use) const {
        std::uint64_t sum = 0;
        // Bad pages are not usable in-use RAM; retain them in the state matrix.
        for (unsigned list : {3U, 4U, 6U, 7U}) { sum += byUseAndState[static_cast<std::size_t>(use)][list]; }
        return sum;
    }
    std::uint64_t bad() const {
        std::uint64_t sum = 0;
        for (const auto& row : byUseAndState) { sum += row[5]; }
        return sum;
    }
    std::uint64_t notScanned() const { return expected > visited ? expected - visited : 0; }
    bool reconciles() const {
        std::uint64_t counted = availablePages + bad() + unreadable + notScanned();
        for (std::size_t use = 0; use < useCount; ++use) { counted += inUse(static_cast<Use>(use)); }
        return counted == expected && visited == valid + unreadable;
    }
};
}
