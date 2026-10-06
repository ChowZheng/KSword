#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ksword::pointer_chain {

inline constexpr std::size_t MaxDepth = 16;

struct Chain {
    // Relocation metadata. Resolve receives the already relocated absolute root.
    std::uint64_t rootOffset = 0;
    std::vector<std::int64_t> offsets;
    std::uint32_t pointerSize = 8;
};

struct ReadResult {
    bool ok = false;
    std::size_t bytesRead = 0;
    std::vector<std::uint8_t> bytes;
    std::string detail;
};

using Reader = std::function<ReadResult(std::uint64_t address, std::size_t length)>;
using Continue = std::function<bool()>;

enum class Status {
    Success,
    InvalidChain,
    Cancelled,
    ReadFailed,
    PartialRead,
    NullPointer,
    AddressOverflow,
    CycleDetected,
};

struct Step {
    std::uint64_t readAddress = 0;
    std::uint64_t pointerValue = 0;
    std::uint64_t resolvedAddress = 0;
    std::int64_t offset = 0;
};

struct Result {
    Status status = Status::InvalidChain;
    // Valid only on success; failures never expose a previously resolved address.
    std::uint64_t address = 0;
    // Zero-based failing dereference. Not meaningful on success.
    std::size_t failedLevel = 0;
    // Fully successful steps only, in dereference order.
    std::vector<Step> steps;
    std::string detail;

    bool ok() const noexcept { return status == Status::Success; }
};

// Reads little-endian 4/8-byte pointers, then adds each corresponding signed
// offset. There must be 1..MaxDepth offsets. The reader must return precisely the
// requested byte count and data length. The optional continuation is checked
// immediately before and after each read; it cannot interrupt a blocked reader.
Result Resolve(const Chain& chain, std::uint64_t rootAddress,
               const Reader& reader, const Continue& shouldContinue = {});

// Strict signed hexadecimal: outer ASCII whitespace, optional +/- and optional
// 0x prefix. Failure leaves value unchanged; INT64_MIN is accepted.
bool ParseOffset(std::string_view text, std::int64_t& value) noexcept;

} // namespace ksword::pointer_chain
