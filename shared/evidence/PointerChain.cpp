#include "PointerChain.h"

#include <exception>
#include <limits>
#include <utility>

namespace ksword::pointer_chain {
namespace {

bool IsSpace(char ch) noexcept
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
           ch == '\f' || ch == '\v';
}

bool AddOffset(std::uint64_t pointer, std::int64_t offset,
               std::uint64_t limit, std::uint64_t& address) noexcept
{
    if (offset < 0) {
        // Avoid negating INT64_MIN in its signed representation.
        const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
        if (pointer < magnitude) { return false; }
        address = pointer - magnitude;
    } else {
        const auto magnitude = static_cast<std::uint64_t>(offset);
        if (pointer > limit || magnitude > limit - pointer) { return false; }
        address = pointer + magnitude;
    }
    return address <= limit;
}

} // namespace

bool ParseOffset(std::string_view text, std::int64_t& value) noexcept
{
    while (!text.empty() && IsSpace(text.front())) { text.remove_prefix(1); }
    while (!text.empty() && IsSpace(text.back())) { text.remove_suffix(1); }
    if (text.empty()) { return false; }

    bool negative = false;
    if (text.front() == '-' || text.front() == '+') {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.empty()) { return false; }

    const auto signedMax = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    const auto limit = negative ? signedMax + 1 : signedMax;
    std::uint64_t parsed = 0;
    for (const char ch : text) {
        unsigned int digit = 0;
        if (ch >= '0' && ch <= '9') { digit = static_cast<unsigned int>(ch - '0'); }
        else if (ch >= 'a' && ch <= 'f') { digit = static_cast<unsigned int>(ch - 'a' + 10); }
        else if (ch >= 'A' && ch <= 'F') { digit = static_cast<unsigned int>(ch - 'A' + 10); }
        else { return false; }
        if (parsed > (limit - digit) / 16) { return false; }
        parsed = parsed * 16 + digit;
    }

    if (negative && parsed == signedMax + 1) {
        value = (std::numeric_limits<std::int64_t>::min)();
    } else {
        const auto signedValue = static_cast<std::int64_t>(parsed);
        value = negative ? -signedValue : signedValue;
    }
    return true;
}

Result Resolve(const Chain& chain, std::uint64_t rootAddress,
               const Reader& reader, const Continue& shouldContinue)
{
    Result result;
    const auto fail = [&result](Status status, std::size_t level, std::string detail) {
        result.status = status;
        result.address = 0;
        result.failedLevel = level;
        result.detail = std::move(detail);
    };
    if (chain.offsets.empty() || chain.offsets.size() > MaxDepth ||
        (chain.pointerSize != 4 && chain.pointerSize != 8) || !reader) {
        fail(Status::InvalidChain, 0, "Expected a reader, 1..16 offsets and a 4/8-byte pointer size.");
        return result;
    }

    const std::uint64_t limit = chain.pointerSize == 4
        ? (std::numeric_limits<std::uint32_t>::max)()
        : (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t address = rootAddress;
    result.steps.reserve(chain.offsets.size());
    for (std::size_t level = 0; level < chain.offsets.size(); ++level) {
        if (address == 0) {
            fail(Status::NullPointer, level, "The pointer read address is null.");
            return result;
        }
        if (address > limit || chain.pointerSize - 1 > limit - address) {
            fail(Status::AddressOverflow, level, "The pointer read crosses the address-width limit.");
            return result;
        }
        const auto repeatedAddress = [&result](std::uint64_t candidate) {
            for (const auto& step : result.steps) {
                if (step.readAddress == candidate) { return true; }
            }
            return false;
        };
        if (repeatedAddress(address)) {
            fail(Status::CycleDetected, level, "The chain repeats a pointer read address.");
            return result;
        }
        if (shouldContinue && !shouldContinue()) {
            fail(Status::Cancelled, level, "Pointer-chain resolution was cancelled before a read.");
            return result;
        }

        ReadResult read;
        try {
            read = reader(address, chain.pointerSize);
        } catch (const std::exception& error) {
            read.detail = error.what();
        } catch (...) {
            read.detail = "The pointer reader raised an exception.";
        }
        if (shouldContinue && !shouldContinue()) {
            fail(Status::Cancelled, level, "Pointer-chain resolution was cancelled after a read.");
            return result;
        }
        if (!read.ok) {
            fail(Status::ReadFailed, level, read.detail.empty() ? "Pointer read failed." : read.detail);
            return result;
        }
        if (read.bytesRead != chain.pointerSize || read.bytes.size() != chain.pointerSize) {
            fail(Status::PartialRead, level, read.detail.empty()
                ? "The pointer reader returned an inconsistent or incomplete byte count." : read.detail);
            return result;
        }

        std::uint64_t pointer = 0;
        for (std::size_t byte = 0; byte < read.bytes.size(); ++byte) {
            pointer |= static_cast<std::uint64_t>(read.bytes[byte]) << (byte * 8);
        }
        if (pointer == 0) {
            fail(Status::NullPointer, level, "The dereferenced pointer is null.");
            return result;
        }
        std::uint64_t resolved = 0;
        if (!AddOffset(pointer, chain.offsets[level], limit, resolved)) {
            fail(Status::AddressOverflow, level, "The pointer offset exceeds the address-width limit.");
            return result;
        }
        if (resolved == 0) {
            fail(Status::NullPointer, level, "The pointer offset resolves to a null address.");
            return result;
        }
        if (resolved == address || repeatedAddress(resolved)) {
            fail(Status::CycleDetected, level, "The pointer offset closes a cycle in the chain.");
            return result;
        }
        result.steps.push_back({address, pointer, resolved, chain.offsets[level]});
        address = resolved;
    }
    result.status = Status::Success;
    result.address = address;
    return result;
}

} // namespace ksword::pointer_chain
