#include "../../shared/evidence/SyscallEvidence.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace ks::evidence::syscall;
using Bytes = std::vector<std::uint8_t>;

namespace {
std::size_t checks = 0;

void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { throw std::runtime_error(message); }
}

// Inert instruction encodings used only as parser input. They are never
// allocated as executable memory, resolved, invoked, or injected.
const Bytes compactStub = {
    0x4C, 0x8B, 0xD1, 0xB8, 0x44, 0x00, 0x00, 0x00, 0x0F, 0x05, 0xC3
};
const Bytes guardedStub = {
    0x4C, 0x8B, 0xD1, 0xB8, 0x55, 0x00, 0x00, 0x00,
    0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01,
    0x75, 0x03, 0x0F, 0x05, 0xC3, 0xCD, 0x2E, 0xC3
};
const Bytes saves = {
    0x48, 0x89, 0x4C, 0x24, 0x08,
    0x48, 0x89, 0x54, 0x24, 0x10,
    0x4C, 0x89, 0x44, 0x24, 0x18,
    0x4C, 0x89, 0x4C, 0x24, 0x20
};
const Bytes restores = {
    0x48, 0x8B, 0x4C, 0x24, 0x08,
    0x48, 0x8B, 0x54, 0x24, 0x10,
    0x4C, 0x8B, 0x44, 0x24, 0x18,
    0x4C, 0x8B, 0x4C, 0x24, 0x20
};
const Bytes resolver = {0xB9, 0x11, 0x22, 0x33, 0x44, 0xE8, 0, 0, 0, 0};

void Append(Bytes& destination, const Bytes& source)
{
    destination.insert(destination.end(), source.begin(), source.end());
}

Bytes Wrapper(const Bytes& tail, bool twoResolvers = false)
{
    Bytes bytes = saves;
    Append(bytes, {0x48, 0x83, 0xEC, 0x28});
    Append(bytes, resolver);
    if (twoResolvers) {
        Append(bytes, {0x49, 0x89, 0xC3}); // mov r11,rax
        Append(bytes, resolver);
    }
    Append(bytes, {0x48, 0x83, 0xC4, 0x28});
    Append(bytes, restores);
    Append(bytes, {0x4C, 0x8B, 0xD1});
    Append(bytes, tail);
    return bytes;
}

CodeEvidence Inspect(const Bytes& bytes, std::size_t pc)
{
    return InspectCode(bytes.data(), bytes.size(), pc);
}

void TestCode()
{
    const auto native = Inspect(compactStub, 8);
    Check(native.syscallInstruction && native.nativeStub, "compact native syscall");
    Check(native.hasSystemCallNumber && native.systemCallNumber == 0x44, "native SSN extraction");
    Check(!native.whispererCompatible, "ordinary native stub is not a compatible wrapper");
    const auto afterSyscall = Inspect(compactStub, 10);
    Check(afterSyscall.syscallInstruction && afterSyscall.nativeStub, "syscall return PC");
    Check(!Inspect(compactStub, 9).syscallInstruction, "middle of opcode is not a syscall PC");
    Check(Inspect(guardedStub, 18).nativeStub, "guarded Windows native stub");
    Check(Inspect(guardedStub, 20).syscallInstruction, "guarded syscall return PC");
    Check(Inspect(guardedStub, 18).systemCallNumber == 0x55, "guarded native SSN extraction");

    Bytes alternate = compactStub;
    alternate[0] = 0x49; alternate[1] = 0x89; alternate[2] = 0xCA;
    Check(Inspect(alternate, 8).nativeStub, "equivalent mov r10,rcx encoding");
    const Bytes bare = {0x0F, 0x05};
    Check(Inspect(bare, 0).syscallInstruction && !Inspect(bare, 0).nativeStub,
        "bare opcode is direct evidence only");
    Check(Inspect(bare, 2).syscallInstruction, "PC immediately after two-byte snapshot");
    Check(!Inspect(bare, 0).whispererCompatible && !Inspect(bare, 0).hasSystemCallNumber,
        "bare opcode has no family or SSN evidence");
    Check(!InspectCode(nullptr, 100, 0).syscallInstruction, "null code buffer");
    Check(!InspectCode(bare.data(), 0, 0).syscallInstruction, "empty code snapshot");
    Check(!Inspect(bare, (std::numeric_limits<std::size_t>::max)()).syscallInstruction,
        "overflowing PC offset rejected");

    for (std::size_t length = 0; length < compactStub.size(); ++length) {
        Check(!InspectCode(compactStub.data(), length, (std::min)(length, std::size_t{8})).nativeStub,
            "truncated native stub rejected");
    }
    for (std::size_t length = 0; length < guardedStub.size(); ++length) {
        Check(!InspectCode(guardedStub.data(), length, (std::min)(length, std::size_t{18})).nativeStub,
            "truncated guarded stub rejected");
    }
    Bytes wrongGuard = guardedStub;
    wrongGuard[17] = 4;
    Check(!Inspect(wrongGuard, 18).nativeStub, "wrong native branch displacement rejected");
    Bytes missingNumber = {0x4C, 0x8B, 0xD1, 0x0F, 0x05, 0xC3};
    Check(!Inspect(missingNumber, 3).nativeStub, "native classification requires SSN instruction");

    const Bytes single = Wrapper({0x0F, 0x05, 0xC3});
    const auto singleEvidence = Inspect(single, single.size() - 3);
    Check(singleEvidence.whispererCompatible && singleEvidence.syscallInstruction,
        "complete single resolver wrapper");
    Check(!singleEvidence.nativeStub && !singleEvidence.hasSystemCallNumber,
        "resolver hash is not a native SSN");
    Check(CompatibleWrapperAtEntry(single.data(), single.size()), "complete wrapper at resolved entry");
    Check(!CompatibleWrapperAtEntry(nullptr, single.size()), "null resolved entry");
    Check(!CompatibleWrapperAtEntry(single.data(), 0), "empty resolved entry");
    for (std::uint8_t leading : {std::uint8_t{0xC3}, std::uint8_t{0x90}}) {
        Bytes unrelated = {leading};
        Append(unrelated, single);
        Check(!CompatibleWrapperAtEntry(unrelated.data(), unrelated.size()),
            "ret/nop followed by unrelated wrapper must not count as target entry");
    }
    const Bytes doubleResolver = Wrapper({0x41, 0xFF, 0xE3}, true);
    const auto jumpEvidence = Inspect(doubleResolver, doubleResolver.size() - 3);
    Check(jumpEvidence.whispererCompatible && !jumpEvidence.syscallInstruction,
        "complete double resolver indirect wrapper");
    Bytes alternateR11 = doubleResolver;
    alternateR11[34] = 0x4C; alternateR11[35] = 0x8B; alternateR11[36] = 0xD8;
    Check(Inspect(alternateR11, alternateR11.size() - 3).whispererCompatible,
        "alternate mov r11,rax encoding");

    for (const auto& encoding : std::vector<Bytes>{{0xFF, 0xE0}, {0x41, 0xFF, 0xE3},
        {0xFF, 0x25, 0, 0, 0, 0}, {0xFF, 0x64, 0x24, 0x20},
        {0xFF, 0xA4, 0x24, 0, 0, 0, 0}, {0xFF, 0x24, 0x25, 0, 0, 0, 0}}) {
        const auto wrapper = Wrapper(encoding);
        Check(Inspect(wrapper, wrapper.size() - encoding.size()).whispererCompatible,
            "complete register/memory near-jump tail");
    }
    for (const auto& encoding : std::vector<Bytes>{{0xFF}, {0x41, 0xFF},
        {0xFF, 0x25, 0, 0}, {0xFF, 0x64, 0x24}, {0xFF, 0xA4, 0x24, 0, 0},
        {0xFF, 0xD0}, {0xFF, 0xE8}}) {
        const auto wrapper = Wrapper(encoding);
        Check(!Inspect(wrapper, wrapper.size() - encoding.size()).whispererCompatible,
            "truncated jump, call and far jump tails rejected");
    }
    for (std::size_t length = 0; length < single.size(); ++length) {
        Check(!InspectCode(single.data(), length, (std::min)(length, single.size() - 3)).whispererCompatible,
            "every incomplete compatible wrapper prefix rejected");
    }
    for (std::size_t length = 0; length < doubleResolver.size(); ++length) {
        Check(!InspectCode(doubleResolver.data(), length, (std::min)(length, doubleResolver.size() - 3)).whispererCompatible,
            "every incomplete double-resolver prefix rejected");
    }
    Bytes badHash = doubleResolver;
    badHash[38] ^= 1;
    Check(!Inspect(badHash, badHash.size() - 3).whispererCompatible, "different resolver hashes rejected");
    Bytes wrongSavedSlot = single;
    wrongSavedSlot[14] = 0x20;
    Check(!Inspect(wrongSavedSlot, wrongSavedSlot.size() - 3).whispererCompatible,
        "incorrect r8 argument save rejected");
    Bytes wrongRestoredSlot = single;
    wrongRestoredSlot[52] = 0x20;
    Check(!Inspect(wrongRestoredSlot, wrongRestoredSlot.size() - 3).whispererCompatible,
        "incorrect r8 argument restore rejected");
    Bytes imbalanced = single;
    imbalanced[37] = 0x20;
    Check(!Inspect(imbalanced, imbalanced.size() - 3).whispererCompatible, "unbalanced shadow space rejected");
    Bytes truncatedCall = single;
    truncatedCall[29] = 0x90;
    Check(!Inspect(truncatedCall, truncatedCall.size() - 3).whispererCompatible, "resolver must be call rel32");

    const std::string names = "SysWhispers SysWhispers2 WhisperMain SW3_GetSyscallNumber";
    const Bytes text(names.begin(), names.end());
    Check(!Inspect(text, 0).whispererCompatible, "tool strings alone are not evidence");
    Bytes disconnected = compactStub;
    Append(disconnected, Bytes(200, 0x90));
    Append(disconnected, single);
    Check(!Inspect(disconnected, 50).nativeStub && !Inspect(disconnected, 50).whispererCompatible,
        "nearby unrelated complete stubs do not describe observed PC");
    Bytes outside = saves;
    Append(outside, Bytes(193, 0x90));
    Append(outside, Bytes(single.begin() + static_cast<std::ptrdiff_t>(saves.size()), single.end()));
    Check(!Inspect(outside, outside.size() - 3).whispererCompatible, "bounded pattern window");
}

FrameEvidence Frame(std::uint64_t address, bool native, bool executable, bool nonImage, CodeEvidence code)
{
    return {address, native, executable, nonImage, code, true};
}

void TestAssessment()
{
    const auto native = Frame(0x7FFB12340008ULL, true, true, false, Inspect(compactStub, 8));
    const auto ordinary = Frame(0x7FF612340000ULL, false, true, false, {});
    const auto privateCode = Frame(0x12340000ULL, false, true, true, {});
    const auto direct = Frame(0x12340000ULL, false, true, true, Inspect(Bytes{0x0F, 0x05}, 0));
    const auto wrapperBytes = Wrapper({0x41, 0xFF, 0xE3}, true);
    const auto compatible = Frame(0x7FF612340000ULL, false, true, false,
        Inspect(wrapperBytes, wrapperBytes.size() - 3));
    Check(Assess({}).path == PathKind::Unknown, "empty stack is unknown");
    Check(Assess({native}).path == PathKind::NativeStub, "native code evidence with trusted native module");
    Check(Assess({native, ordinary}).path == PathKind::NativeStub, "ordinary image caller stays native");
    Check(Assess({native, privateCode}).path == PathKind::Indirect, "native syscall with executable non-image caller");
    const auto indirect = Assess({native, compatible});
    Check(indirect.path == PathKind::Indirect && indirect.whispererCompatible,
        "native syscall with complete compatible image caller");
    Check(Assess({direct}).path == PathKind::Direct, "non-native executable syscall PC");
    auto unknownModule = direct;
    unknownModule.moduleIdentityKnown = false;
    unknownModule.nonImage = false;
    Check(Assess({unknownModule}).path == PathKind::Unknown,
        "unresolved module identity must not become direct syscall evidence");
    auto knownPrivate = direct;
    knownPrivate.moduleIdentityKnown = false;
    Check(Assess({knownPrivate}).path == PathKind::Direct,
        "verified non-image allocation establishes non-native origin");
    auto unknownNativeIdentity = native;
    unknownNativeIdentity.moduleIdentityKnown = false;
    Check(Assess({unknownNativeIdentity}).path == PathKind::Unknown,
        "native stub requires established native module identity");
    auto nonExecutableNative = native;
    nonExecutableNative.executable = false;
    Check(Assess({nonExecutableNative}).path == PathKind::Unknown,
        "native shape in non-executable memory is not a native syscall path");
    auto nonExecutable = direct;
    nonExecutable.executable = false;
    Check(Assess({nonExecutable}).path == PathKind::Unknown, "non-executable syscall bytes are not direct");
    auto unreadableNative = native;
    unreadableNative.code = {};
    Check(Assess({unreadableNative, privateCode}).path == PathKind::Unknown,
        "native module alone does not classify syscall path");
    auto nativeBare = direct;
    nativeBare.nativeModule = true;
    Check(Assess({nativeBare}).path == PathKind::Unknown, "native module plus bare opcode is not native stub");
    auto kernel = direct;
    kernel.address = 0xFFFFF80012340000ULL;
    Check(Assess({kernel}).path == PathKind::Unknown, "kernel address is not direct user syscall");
    auto nonCanonical = direct;
    nonCanonical.address = 0x800000000000ULL;
    Check(Assess({nonCanonical}).path == PathKind::Unknown, "non-canonical address is not direct");
    auto nonExecutableCaller = privateCode;
    nonExecutableCaller.executable = false;
    Check(Assess({native, nonExecutableCaller}).path == PathKind::NativeStub,
        "non-image data caller does not classify indirect");
    auto kernelCaller = privateCode;
    kernelCaller.address = kernel.address;
    Check(Assess({native, kernelCaller}).path == PathKind::NativeStub, "kernel caller is not indirect user evidence");
    Check(Assess({ordinary, direct}).path == PathKind::Unknown, "lower frame opcode does not prove top-frame direct path");
    const auto compatibleOnly = Assess({compatible});
    Check(compatibleOnly.path == PathKind::Unknown && compatibleOnly.whispererCompatible,
        "complete jump wrapper without native target retains shape but unknown path");
    Check(Assess({ordinary, ordinary, compatible}).whispererCompatible, "compatible evidence propagated independently");
}

void TestAddresses()
{
    Check(!IsUserAddress(0), "null is not user code");
    Check(!IsUserAddress(0xFFFF), "low allocation boundary rejected");
    Check(IsUserAddress(0x10000), "lowest valid user allocation");
    Check(IsUserAddress(0x7FFFFFFFFFFFULL), "highest low-canonical user address");
    Check(!IsUserAddress(0x800000000000ULL), "x64 non-canonical boundary");
    Check(!IsUserAddress(0xFFFF800000000000ULL), "kernel canonical address");
    Check(IsUserAddress(0x7FFFFFFFULL, 4), "32-bit user boundary");
    Check(!IsUserAddress(0x80000000ULL, 4), "32-bit kernel boundary");
    Check(!IsUserAddress(0x100000000ULL, 4), "32-bit address must not truncate");
    Check(!IsUserAddress(0x10000, 0) && !IsUserAddress(0x10000, 16), "invalid address width");
}

Bytes StackPayload(std::size_t pointerSize, std::size_t count)
{
    Bytes payload(16 + pointerSize * count);
    const std::uint64_t timestamp = 0x1122334455667788ULL;
    const std::uint32_t pid = 0x12345678, tid = 0xABCDEF01;
    std::memcpy(payload.data(), &timestamp, 8);
    std::memcpy(payload.data() + 8, &pid, 4);
    std::memcpy(payload.data() + 12, &tid, 4);
    for (std::size_t i = 0; i < count; ++i) {
        if (pointerSize == 8) {
            const std::uint64_t address = 0xFFFFF80012340000ULL + i;
            std::memcpy(payload.data() + 16 + i * 8, &address, 8);
        } else {
            const std::uint32_t address = 0xF1234000U + static_cast<std::uint32_t>(i);
            std::memcpy(payload.data() + 16 + i * 4, &address, 4);
        }
    }
    return payload;
}

void TestPayload()
{
    std::uint64_t timestamp = 0;
    std::uint32_t pid = 0, tid = 0;
    std::vector<std::uint64_t> frames;
    for (std::size_t width : {4U, 8U}) {
        const Bytes payload = StackPayload(width, 3);
        Check(ParseStackPayload(payload.data(), payload.size(), width, timestamp, pid, tid, frames),
            "complete StackWalk payload");
        Check(timestamp == 0x1122334455667788ULL && pid == 0x12345678 && tid == 0xABCDEF01,
            "payload timestamp and identities use exact prefix layout");
        Check(frames.size() == 3 && frames[2] == (width == 8 ? 0xFFFFF80012340002ULL : 0xF1234002ULL),
            "frame bit width preserved without filtering/truncation");
        Bytes unaligned = {0};
        Append(unaligned, payload);
        Check(ParseStackPayload(unaligned.data() + 1, payload.size(), width, timestamp, pid, tid, frames),
            "unaligned payload parsed with memcpy");
        for (std::size_t prefix = 0; prefix < 16; ++prefix) {
            Check(!ParseStackPayload(payload.data(), prefix, width, timestamp, pid, tid, frames),
                "truncated timestamp/PID/TID prefix rejected");
            Check(timestamp == 0 && pid == 0 && tid == 0 && frames.empty(), "failed parse clears all output");
        }
        for (std::size_t partial = 1; partial < width; ++partial) {
            Check(!ParseStackPayload(payload.data(), payload.size() - partial, width, timestamp, pid, tid, frames),
                "truncated final frame rejected rather than silently rounded");
        }
        const Bytes maximum = StackPayload(width, 192);
        Check(ParseStackPayload(maximum.data(), maximum.size(), width, timestamp, pid, tid, frames)
            && frames.size() == 192, "maximum frame count accepted");
        const Bytes tooLarge = StackPayload(width, 193);
        Check(!ParseStackPayload(tooLarge.data(), tooLarge.size(), width, timestamp, pid, tid, frames),
            "oversized frame list rejected");
    }
    const auto prefixOnly = StackPayload(8, 0);
    Check(!ParseStackPayload(prefixOnly.data(), prefixOnly.size(), 8, timestamp, pid, tid, frames),
        "empty frame list rejected");
    Check(!ParseStackPayload(nullptr, 24, 8, timestamp, pid, tid, frames), "null payload rejected");
    const auto payload = StackPayload(8, 1);
    for (std::size_t invalidWidth : {0U, 1U, 2U, 16U}) {
        Check(!ParseStackPayload(payload.data(), payload.size(), invalidWidth, timestamp, pid, tid, frames),
            "unsupported pointer width rejected");
    }
}

void TestSyscallPayload()
{
    SyscallPayload output;
    const std::uint64_t address64 = 0xFFFFF80012345678ULL;
    const std::uint32_t address32 = 0xF1234567U;
    const std::uint32_t success = 0;
    const std::uint32_t denied = 0xC0000022U;
    Check(ParseSyscallPayload(51, 8, &address64, 8, output) && output.enter
        && output.kernelServiceAddress == address64 && output.ntStatus == 0,
        "enter payload preserves high kernel service address");
    Check(ParseSyscallPayload(51, 4, &address32, 4, output) && output.enter
        && output.kernelServiceAddress == address32 && output.ntStatus == 0,
        "32-bit enter address zero extended without user-address filtering");
    Check(ParseSyscallPayload(52, 8, &success, 4, output) && !output.enter
        && output.ntStatus == 0 && output.kernelServiceAddress == 0,
        "STATUS_SUCCESS is exit status rather than service number");
    Check(ParseSyscallPayload(52, 4, &denied, 4, output) && !output.enter
        && output.ntStatus == denied && output.kernelServiceAddress == 0,
        "negative NTSTATUS preserves all status bits");
    Bytes unaligned(9);
    std::memcpy(unaligned.data() + 1, &address64, 8);
    Check(ParseSyscallPayload(51, 8, unaligned.data() + 1, 8, output)
        && output.kernelServiceAddress == address64, "unaligned enter parsed with memcpy");
    for (const auto& input : std::vector<std::vector<std::size_t>>{
        {51, 8, 0}, {51, 8, 7}, {51, 8, 9}, {51, 4, 3}, {51, 4, 5},
        {52, 8, 0}, {52, 8, 3}, {52, 8, 5}, {52, 4, 8},
        {51, 0, 8}, {52, 16, 4}, {0, 8, 8}, {53, 8, 8}, {255, 4, 4}}) {
        output = {address64, denied, true};
        Check(!ParseSyscallPayload(static_cast<std::uint8_t>(input[0]), input[1],
            &address64, input[2], output), "malformed/unknown syscall payload rejected");
        Check(!output.enter && output.kernelServiceAddress == 0 && output.ntStatus == 0,
            "syscall parse failure resets previous evidence");
    }
    output = {address64, denied, true};
    Check(!ParseSyscallPayload(51, 8, nullptr, 8, output) && !output.enter
        && output.kernelServiceAddress == 0 && output.ntStatus == 0,
        "null syscall input resets output");
}
} // namespace

int main()
{
    try {
        TestCode();
        TestAssessment();
        TestAddresses();
        TestPayload();
        TestSyscallPayload();
        std::cout << "SYSCALL_EVIDENCE_TESTS=PASS CHECKS=" << checks << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "SYSCALL_EVIDENCE_TESTS=FAIL CHECK=" << checks << " ERROR=" << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
