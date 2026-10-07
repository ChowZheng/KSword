#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ksword::memoryaudit {
enum class RegionKind : std::uint8_t { Unknown, Private, Image, Mapped };
enum class ResidentKind : std::uint8_t {
    Private, PrivateMappedCopy, Image, MappedFile, PagefileSection, MappedBackingUnknown, Unknown
};
enum class BackingProof : std::uint8_t { Unresolved, DataFile, Pagefile, Image, Physical };

// VirtualQueryEx preserves MEM_IMAGE/MEM_MAPPED after COW. The resident
// working-set Shared bit, rather than the region type or ShareCount, tells us
// whether this particular resident page is private.
constexpr ResidentKind classifyResident(RegionKind region, bool shared, bool completePath,
    BackingProof proof = BackingProof::Unresolved)
{
    if (region == RegionKind::Private) { return ResidentKind::Private; }
    if (region == RegionKind::Unknown) { return ResidentKind::Unknown; }
    if (!shared) { return ResidentKind::PrivateMappedCopy; }
    if (region == RegionKind::Image) { return ResidentKind::Image; }
    if (proof == BackingProof::Pagefile) {
        return completePath ? ResidentKind::MappedBackingUnknown : ResidentKind::PagefileSection;
    }
    if (proof == BackingProof::DataFile) { return ResidentKind::MappedFile; }
    if (proof == BackingProof::Physical) { return ResidentKind::MappedBackingUnknown; }
    return completePath ? ResidentKind::MappedFile : ResidentKind::MappedBackingUnknown;
}

struct PathOutcome { bool valid; std::uint32_t error; };
constexpr PathOutcome mappedPathOutcome(std::size_t length, std::size_t capacity, std::uint32_t error)
{
    // ERROR_INSUFFICIENT_BUFFER is 122. A truncated successful path is not an
    // identity; zero remains a failed query, even if an API supplied no error.
    if (!length) { return {false, error}; }
    if (!capacity || length >= capacity) { return {false, 122}; }
    return {true, 0};
}

enum class CommitSource : std::uint8_t { Unavailable, PerformanceInfo, NativeMemoryUsage };
struct SystemCommit {
    bool valid = false;
    CommitSource source = CommitSource::Unavailable;
    std::uint64_t bytes = 0, limit = 0, peak = 0;
    bool publicAttempted = false, nativeAttempted = false;
    std::uint32_t publicStatus = 0;
    std::int32_t nativeStatus = 0;

    void recordPublic(bool succeeded, std::uint32_t error, std::uint64_t totalPages,
        std::uint64_t limitPages, std::uint64_t peakPages, std::uint64_t pageSize)
    {
        publicAttempted = true;
        publicStatus = succeeded ? 0 : error;
        if (!succeeded) { return; }
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        if (!pageSize || totalPages > maximum / pageSize || limitPages > maximum / pageSize
            || peakPages > maximum / pageSize) {
            publicStatus = 534; // ERROR_ARITHMETIC_OVERFLOW
            return;
        }
        bytes = totalPages * pageSize;
        limit = limitPages * pageSize;
        peak = peakPages * pageSize;
        source = CommitSource::PerformanceInfo;
        valid = true; // Zero commitment is a successful observation, not failure.
    }

    void recordNative(std::int32_t status, std::size_t returnedBytes, std::size_t expectedBytes,
        std::uint64_t totalBytes, std::uint64_t limitBytes, std::uint64_t peakBytes)
    {
        nativeAttempted = true;
        nativeStatus = status;
        if (status < 0) { return; }
        // This undocumented fallback must establish that the complete fixed
        // payload was returned. Success with a short/unspecified length is not
        // enough to replace an unavailable system counter with zero.
        if (!expectedBytes || returnedBytes != expectedBytes) {
            nativeStatus = static_cast<std::int32_t>(0xC000003EU); // STATUS_DATA_ERROR
            return;
        }
        if (source == CommitSource::PerformanceInfo) { return; }
        bytes = totalBytes;
        limit = limitBytes;
        peak = peakBytes;
        source = CommitSource::NativeMemoryUsage;
        valid = true;
    }
};

struct ReservedEstimate { bool valid = false; std::uint64_t bytes = 0; };
constexpr ReservedEstimate reservedEstimate(bool installedValid, bool usableValid,
    std::uint64_t installed, std::uint64_t usable)
{
    return installedValid && usableValid && installed >= usable
        ? ReservedEstimate{true, installed - usable} : ReservedEstimate{};
}
}
