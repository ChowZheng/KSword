#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace ks::evidence::syscall {

enum class PathKind { Unknown, NativeStub, Direct, Indirect };

struct CodeEvidence {
    bool syscallInstruction = false;
    bool nativeStub = false;
    // A complete compatible wrapper is evidence of its shape, not attribution
    // to a particular tool, version, author, or malicious intent.
    bool whispererCompatible = false;
    bool hasSystemCallNumber = false;
    std::uint32_t systemCallNumber = 0;
};

struct FrameEvidence {
    std::uint64_t address = 0;
    // The caller must establish system-native module identity. A matching
    // basename alone is insufficient to set nativeModule.
    bool nativeModule = false;
    bool executable = false;
    bool nonImage = false;
    CodeEvidence code;
    // A failed module lookup must not turn an ordinary native stub into a
    // direct call. True means native/non-native identity was established.
    bool moduleIdentityKnown = false;
};

struct Assessment {
    PathKind path = PathKind::Unknown;
    bool whispererCompatible = false;
};

struct SyscallPayload {
    std::uint64_t kernelServiceAddress = 0;
    std::uint32_t ntStatus = 0;
    bool enter = false;
};

// Classic PerfInfo SysCallEnter (51) carries a pointer-sized kernel service
// address, not an SSN; SysCallExit (52) carries a 32-bit NTSTATUS, including
// STATUS_SUCCESS == 0. Unexpected layouts never produce partial evidence.
inline bool ParseSyscallPayload(std::uint8_t opcode, std::size_t pointerSize,
    const void* data, std::size_t size, SyscallPayload& out) noexcept
{
    out = {};
    if (!data || (pointerSize != 4 && pointerSize != 8)) { return false; }
    if (opcode == 51) {
        if (size != pointerSize) { return false; }
        if (pointerSize == 8) {
            std::memcpy(&out.kernelServiceAddress, data, 8);
        } else {
            std::uint32_t address = 0;
            std::memcpy(&address, data, 4);
            out.kernelServiceAddress = address;
        }
        out.enter = true;
        return true;
    }
    if (opcode == 52 && size == 4) {
        std::memcpy(&out.ntStatus, data, 4);
        return true;
    }
    return false;
}

inline constexpr bool IsUserAddress(std::uint64_t address, std::size_t addressSize = 8) noexcept
{
    if (addressSize == 8) {
        return address >= 0x10000ULL && address < 0x0000800000000000ULL;
    }
    if (addressSize == 4) {
        return address >= 0x10000ULL && address < 0x80000000ULL;
    }
    return false;
}

namespace detail {

inline constexpr std::size_t kCodeRadius = 192;
inline constexpr std::size_t kMaxStackFrames = 192;

class Cursor {
public:
    Cursor(const std::uint8_t* bytes, std::size_t begin, std::size_t end)
        : bytes_(bytes), position_(begin), end_(end) {}

    bool Match(std::initializer_list<std::uint8_t> pattern) noexcept
    {
        if (pattern.size() > end_ - position_) { return false; }
        std::size_t offset = position_;
        for (std::uint8_t byte : pattern) {
            if (bytes_[offset++] != byte) { return false; }
        }
        position_ = offset;
        return true;
    }

    bool Skip(std::size_t count) noexcept
    {
        if (count > end_ - position_) { return false; }
        position_ += count;
        return true;
    }

    bool Read(std::uint8_t& byte) noexcept
    {
        if (position_ == end_) { return false; }
        byte = bytes_[position_++];
        return true;
    }

    bool Read32(std::uint32_t& value) noexcept
    {
        if (4 > end_ - position_) { return false; }
        // These are x86 instruction operands: decode little endian even on a
        // host with a different native byte order.
        value = static_cast<std::uint32_t>(bytes_[position_])
            | (static_cast<std::uint32_t>(bytes_[position_ + 1]) << 8)
            | (static_cast<std::uint32_t>(bytes_[position_ + 2]) << 16)
            | (static_cast<std::uint32_t>(bytes_[position_ + 3]) << 24);
        position_ += 4;
        return true;
    }

    std::size_t Position() const noexcept { return position_; }

private:
    const std::uint8_t* bytes_;
    std::size_t position_;
    std::size_t end_;
};

inline bool MoveR10Rcx(Cursor& cursor) noexcept
{
    return cursor.Match({0x4C, 0x8B, 0xD1}) || cursor.Match({0x49, 0x89, 0xCA});
}

inline bool NativeStub(Cursor& cursor, std::uint32_t& number) noexcept
{
    if (!MoveR10Rcx(cursor) || !cursor.Match({0xB8}) || !cursor.Read32(number)) { return false; }
    // The compact and shared-user-data guarded Windows x64 syscall stubs.
    Cursor compact = cursor;
    if (compact.Match({0x0F, 0x05, 0xC3})) { cursor = compact; return true; }
    Cursor guarded = cursor;
    if (guarded.Match({0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01,
        0x75, 0x03, 0x0F, 0x05, 0xC3, 0xCD, 0x2E, 0xC3})) {
        cursor = guarded;
        return true;
    }
    return false;
}

inline bool SaveArguments(Cursor& cursor) noexcept
{
    return cursor.Match({0x48, 0x89, 0x4C, 0x24, 0x08})
        && cursor.Match({0x48, 0x89, 0x54, 0x24, 0x10})
        && cursor.Match({0x4C, 0x89, 0x44, 0x24, 0x18})
        && cursor.Match({0x4C, 0x89, 0x4C, 0x24, 0x20});
}

inline bool RestoreArguments(Cursor& cursor) noexcept
{
    return cursor.Match({0x48, 0x8B, 0x4C, 0x24, 0x08})
        && cursor.Match({0x48, 0x8B, 0x54, 0x24, 0x10})
        && cursor.Match({0x4C, 0x8B, 0x44, 0x24, 0x18})
        && cursor.Match({0x4C, 0x8B, 0x4C, 0x24, 0x20});
}

inline bool ResolverCall(Cursor& cursor, std::uint32_t& hash) noexcept
{
    return cursor.Match({0xB9}) && cursor.Read32(hash)
        && cursor.Match({0xE8}) && cursor.Skip(4);
}

// Accept only a complete FF /4 near indirect JMP encoding. In particular,
// neither CALL (/2) nor a truncated ModRM/SIB/displacement is a jump tail.
inline bool IndirectJump(Cursor& cursor) noexcept
{
    Cursor probe = cursor;
    std::uint8_t opcode = 0;
    if (!probe.Read(opcode)) { return false; }
    if (opcode >= 0x40 && opcode <= 0x4F && !probe.Read(opcode)) { return false; }
    if (opcode != 0xFF) { return false; }
    std::uint8_t modrm = 0;
    if (!probe.Read(modrm) || ((modrm >> 3) & 7) != 4) { return false; }
    const unsigned mod = modrm >> 6;
    const unsigned rm = modrm & 7;
    if (mod != 3) {
        if (rm == 4) {
            std::uint8_t sib = 0;
            if (!probe.Read(sib)) { return false; }
            if (mod == 0 && (sib & 7) == 5 && !probe.Skip(4)) { return false; }
        } else if (mod == 0 && rm == 5 && !probe.Skip(4)) { return false; }
        if (mod == 1 && !probe.Skip(1)) { return false; }
        if (mod == 2 && !probe.Skip(4)) { return false; }
    }
    cursor = probe;
    return true;
}

inline bool CompatibleWrapper(Cursor& cursor) noexcept
{
    if (!SaveArguments(cursor) || !cursor.Match({0x48, 0x83, 0xEC, 0x28})) { return false; }
    std::uint32_t firstHash = 0;
    if (!ResolverCall(cursor, firstHash)) { return false; }

    // A second resolver commonly follows preservation of the returned address
    // in r11. Matching the repeated hash prevents joining unrelated calls.
    Cursor twoResolvers = cursor;
    if (twoResolvers.Match({0x49, 0x89, 0xC3}) || twoResolvers.Match({0x4C, 0x8B, 0xD8})) {
        std::uint32_t secondHash = 0;
        if (!ResolverCall(twoResolvers, secondHash) || firstHash != secondHash) { return false; }
        cursor = twoResolvers;
    }
    if (!cursor.Match({0x48, 0x83, 0xC4, 0x28}) || !RestoreArguments(cursor)
        || !MoveR10Rcx(cursor)) { return false; }
    return cursor.Match({0x0F, 0x05, 0xC3}) || IndirectJump(cursor);
}

} // namespace detail

// Use this for a separately resolved call target: recognition must begin at
// that entry, rather than searching for an unrelated wrapper nearby.
inline bool CompatibleWrapperAtEntry(const std::uint8_t* bytes, std::size_t length) noexcept
{
    if (!bytes || !length) { return false; }
    detail::Cursor cursor(bytes, 0, (std::min)(length, detail::kCodeRadius));
    return detail::CompatibleWrapper(cursor);
}

// bytes must be a caller-owned, readable snapshot. pcOffset names an observed
// PC inside it (or the byte just past its end for a syscall return address).
// Pattern searches are bounded to 192 bytes on either side of the PC and must
// contain that PC. Nearby unrelated stubs do not become evidence for the PC.
inline CodeEvidence InspectCode(const std::uint8_t* bytes, std::size_t length,
    std::size_t pcOffset) noexcept
{
    CodeEvidence result;
    if (!bytes || !length || pcOffset > length) { return result; }
    if (pcOffset < length && length - pcOffset >= 2
        && bytes[pcOffset] == 0x0F && bytes[pcOffset + 1] == 0x05) {
        result.syscallInstruction = true;
    }
    if (pcOffset >= 2 && bytes[pcOffset - 2] == 0x0F && bytes[pcOffset - 1] == 0x05) {
        result.syscallInstruction = true;
    }
    const std::size_t begin = pcOffset > detail::kCodeRadius ? pcOffset - detail::kCodeRadius : 0;
    const std::size_t end = pcOffset + (std::min)(detail::kCodeRadius, length - pcOffset);
    for (std::size_t start = begin; start < end && start <= pcOffset; ++start) {
        detail::Cursor native(bytes, start, end);
        std::uint32_t number = 0;
        if (detail::NativeStub(native, number) && pcOffset < native.Position()) {
            result.nativeStub = true;
            result.hasSystemCallNumber = true;
            result.systemCallNumber = number;
        }
        detail::Cursor compatible(bytes, start, end);
        if (detail::CompatibleWrapper(compatible) && pcOffset < compatible.Position()) {
            result.whispererCompatible = true;
        }
    }
    return result;
}

// Frames are supplied in top-of-user-stack order. This remains a heuristic:
// memory/stack snapshots may be incomplete, and a compatible wrapper alone
// does not prove execution or malicious activity.
inline Assessment Assess(const std::vector<FrameEvidence>& userFrames) noexcept
{
    Assessment result;
    for (const auto& frame : userFrames) {
        result.whispererCompatible = result.whispererCompatible || frame.code.whispererCompatible;
    }
    if (userFrames.empty() || !IsUserAddress(userFrames.front().address)) { return result; }
    const auto& first = userFrames.front();
    if (first.moduleIdentityKnown && first.nativeModule && first.executable && first.code.nativeStub) {
        result.path = PathKind::NativeStub;
        if (userFrames.size() > 1) {
            const auto& caller = userFrames[1];
            if (IsUserAddress(caller.address) && caller.executable
                && (caller.nonImage || caller.code.whispererCompatible)) {
                result.path = PathKind::Indirect;
            }
        }
    } else if ((first.nonImage || first.moduleIdentityKnown) && !first.nativeModule
        && first.executable && first.code.syscallInstruction) {
        result.path = PathKind::Direct;
    }
    return result;
}

// ETW StackWalk payload: uint64 timestamp, uint32 PID, uint32 TID, then
// pointer-sized addresses. Output is cleared on every failure; no partial
// frame list or inferred process/thread identity escapes a malformed payload.
inline bool ParseStackPayload(const void* data, std::size_t size, std::size_t pointerSize,
    std::uint64_t& timestamp, std::uint32_t& pid, std::uint32_t& tid,
    std::vector<std::uint64_t>& frames)
{
    timestamp = 0;
    pid = 0;
    tid = 0;
    frames.clear();
    if (!data || (pointerSize != 4 && pointerSize != 8) || size < 16) { return false; }
    const std::size_t addressBytes = size - 16;
    if (!addressBytes || addressBytes % pointerSize != 0) { return false; }
    const std::size_t frameCount = addressBytes / pointerSize;
    if (frameCount > detail::kMaxStackFrames) { return false; }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    frames.reserve(frameCount);
    std::memcpy(&timestamp, bytes, 8);
    std::memcpy(&pid, bytes + 8, 4);
    std::memcpy(&tid, bytes + 12, 4);
    for (std::size_t i = 0; i < frameCount; ++i) {
        if (pointerSize == 8) {
            std::uint64_t address = 0;
            std::memcpy(&address, bytes + 16 + i * 8, 8);
            frames.push_back(address);
        } else {
            std::uint32_t address = 0;
            std::memcpy(&address, bytes + 16 + i * 4, 4);
            frames.push_back(address);
        }
    }
    return true;
}

} // namespace ks::evidence::syscall
