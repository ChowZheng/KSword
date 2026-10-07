#include "../shared/evidence/PointerChain.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace pc = ksword::pointer_chain;

namespace {

std::size_t checks = 0;

void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { throw std::runtime_error(message); }
}

pc::ReadResult Pointer(std::uint64_t value, std::size_t width = 8)
{
    pc::ReadResult read;
    read.ok = true;
    read.bytesRead = width;
    for (std::size_t i = 0; i < width; ++i) {
        read.bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    return read;
}

struct Memory {
    std::map<std::uint64_t, pc::ReadResult> values;
    std::vector<std::pair<std::uint64_t, std::size_t>> calls;

    pc::Reader reader()
    {
        return [this](std::uint64_t address, std::size_t length) {
            calls.emplace_back(address, length);
            const auto found = values.find(address);
            if (found == values.end()) {
                pc::ReadResult failure;
                failure.detail = "fixture address is unreadable";
                return failure;
            }
            return found->second;
        };
    }
};

pc::Chain Chain(std::initializer_list<std::int64_t> offsets, std::uint32_t width = 8)
{
    pc::Chain chain;
    chain.offsets = offsets;
    chain.pointerSize = width;
    return chain;
}

void Failure(const pc::Result& result, pc::Status status, std::size_t level,
             std::size_t successfulSteps)
{
    Check(!result.ok(), "failure reported success");
    Check(result.status == status, "incorrect failure status");
    Check(result.address == 0, "failure exposed a stale address");
    Check(result.failedLevel == level, "incorrect failing level");
    Check(result.steps.size() == successfulSteps, "incorrect successful step count");
    Check(!result.detail.empty(), "failure has no detail");
}

void OffsetParsing()
{
    struct Valid { const char* text; std::int64_t expected; };
    const Valid valid[] = {
        {"0", 0}, {"-0", 0}, {"+0", 0}, {"10", 0x10},
        {"-10", -0x10}, {"+0x20", 0x20}, {"-0XAbCd", -0xABCD},
        {" \t-0x40\r\n\f\v", -0x40}, {"ABCDEF", 0xABCDEF},
        {"7fffffffffffffff", (std::numeric_limits<std::int64_t>::max)()},
        {"+0X7FFFFFFFFFFFFFFF", (std::numeric_limits<std::int64_t>::max)()},
        {"-8000000000000000", (std::numeric_limits<std::int64_t>::min)()},
        {"-0x8000000000000000", (std::numeric_limits<std::int64_t>::min)()},
        {"-7fffffffffffffff", -(std::numeric_limits<std::int64_t>::max)()},
        {"000000000000000000000000001", 1}, {"0d10", 0xD10},
    };
    for (const auto& sample : valid) {
        std::int64_t parsed = 0x1234;
        Check(pc::ParseOffset(sample.text, parsed), "valid signed hex rejected");
        Check(parsed == sample.expected, "signed hex misparsed");
    }
    const char* invalid[] = {
        "", " \t\r\n", "+", "-", "0x", "+0x", "-0x", "++1", "--1",
        "+-1", "-+1", "- 1", "0x-1", "1 0", "1\n0", "1_0", "1`0",
        "10h", "0x0x10", "g", "0x1z", "8000000000000000",
        "+8000000000000000", "ffffffffffffffff", "-8000000000000001",
        "-ffffffffffffffff", "10000000000000000", "-10000000000000000",
    };
    for (const auto text : invalid) {
        std::int64_t parsed = 0x1234;
        Check(!pc::ParseOffset(text, parsed), "invalid signed hex accepted");
        Check(parsed == 0x1234, "failed parser changed output");
    }
    std::int64_t parsed = 123;
    Check(!pc::ParseOffset(std::string_view("1\0f", 3), parsed), "embedded NUL accepted");
    Check(parsed == 123, "embedded NUL changed output");
}

void ValidChains()
{
    Memory memory;
    memory.values[0x1000] = Pointer(0x0123456789ABCDEFULL);
    auto chain = Chain({0});
    chain.rootOffset = 0x500; // The supplied root has already been relocated.
    auto result = pc::Resolve(chain, 0x1000, memory.reader());
    Check(result.ok(), "little-endian 64-bit pointer failed");
    Check(result.address == 0x0123456789ABCDEFULL, "64-bit byte order is incorrect");
    Check(memory.calls.size() == 1 && memory.calls[0].first == 0x1000,
          "rootOffset was applied twice");
    Check(memory.calls[0].second == 8, "incorrect 64-bit read length");
    Check(result.steps[0].pointerValue == result.address, "step pointer differs");

    memory = {};
    memory.values[0x1000] = Pointer(0xFEDCBA98, 4);
    result = pc::Resolve(Chain({-0x98}, 4), 0x1000, memory.reader());
    Check(result.ok() && result.address == 0xFEDCBA00, "32-bit signed offset/byte order failed");
    Check(memory.calls[0].second == 4, "incorrect 32-bit read length");

    memory = {};
    memory.values[0x1000] = Pointer(0x2010);
    memory.values[0x2000] = Pointer(0x3000);
    memory.values[0x3020] = Pointer(0x4000);
    result = pc::Resolve(Chain({-0x10, 0x20, -0x30}), 0x1000, memory.reader());
    Check(result.ok() && result.address == 0x3FD0, "multi-level chain failed");
    Check(result.steps.size() == 3 && memory.calls.size() == 3, "wrong multi-level read count");
    Check(result.steps[1].readAddress == 0x2000 && result.steps[1].pointerValue == 0x3000 &&
          result.steps[1].resolvedAddress == 0x3020 && result.steps[1].offset == 0x20,
          "intermediate step evidence is incorrect");
    Check(memory.calls[2].first == 0x3020, "offset was added after the wrong dereference");
}

void AddressLimits()
{
    const auto u64max = (std::numeric_limits<std::uint64_t>::max)();
    const auto i64min = (std::numeric_limits<std::int64_t>::min)();
    Memory memory;
    memory.values[0x1000] = Pointer(u64max);
    auto result = pc::Resolve(Chain({i64min}), 0x1000, memory.reader());
    Check(result.ok() && result.address == 0x7FFFFFFFFFFFFFFFULL, "INT64_MIN subtraction failed");
    result = pc::Resolve(Chain({0}), 0x1000, memory.reader());
    Check(result.ok() && result.address == u64max, "maximum final 64-bit address rejected");
    Failure(pc::Resolve(Chain({1}), 0x1000, memory.reader()), pc::Status::AddressOverflow, 0, 0);
    memory.values[0x1000] = Pointer(1);
    Failure(pc::Resolve(Chain({-2}), 0x1000, memory.reader()), pc::Status::AddressOverflow, 0, 0);
    Failure(pc::Resolve(Chain({i64min}), 0x1000, memory.reader()), pc::Status::AddressOverflow, 0, 0);
    Failure(pc::Resolve(Chain({-1}), 0x1000, memory.reader()), pc::Status::NullPointer, 0, 0);
    memory.values[0x1000] = Pointer(0x8000000000000000ULL);
    Failure(pc::Resolve(Chain({i64min}), 0x1000, memory.reader()), pc::Status::NullPointer, 0, 0);

    memory = {};
    memory.values[0x1000] = Pointer(0xFFFFFFFF, 4);
    result = pc::Resolve(Chain({0}, 4), 0x1000, memory.reader());
    Check(result.ok() && result.address == 0xFFFFFFFF, "maximum final 32-bit address rejected");
    Failure(pc::Resolve(Chain({1}, 4), 0x1000, memory.reader()), pc::Status::AddressOverflow, 0, 0);
    Failure(pc::Resolve(Chain({i64min}, 4), 0x1000, memory.reader()), pc::Status::AddressOverflow, 0, 0);
    memory = {};
    Failure(pc::Resolve(Chain({0}, 4), 0x100000000ULL, memory.reader()), pc::Status::AddressOverflow, 0, 0);
    Failure(pc::Resolve(Chain({0}, 4), 0xFFFFFFFD, memory.reader()), pc::Status::AddressOverflow, 0, 0);
    Failure(pc::Resolve(Chain({0}), u64max - 6, memory.reader()), pc::Status::AddressOverflow, 0, 0);
    Check(memory.calls.empty(), "out-of-range root was read");

    memory.values[0xFFFFFFFC] = Pointer(0x2000, 4);
    Check(pc::Resolve(Chain({0}, 4), 0xFFFFFFFC, memory.reader()).ok(),
          "valid uppermost 32-bit pointer read rejected");
    memory.values[u64max - 7] = Pointer(0x2000);
    Check(pc::Resolve(Chain({0}), u64max - 7, memory.reader()).ok(),
          "valid uppermost 64-bit pointer read rejected");
    memory = {};
    memory.values[0x1000] = Pointer(u64max);
    Failure(pc::Resolve(Chain({0, 0}), 0x1000, memory.reader()), pc::Status::AddressOverflow, 1, 1);
    Check(memory.calls.size() == 1, "overflowing intermediate read was issued");
}

void ReadFailures()
{
    Memory memory;
    memory.values[0x1000] = Pointer(0x2000);
    memory.values[0x2000] = Pointer(0x3000);
    const auto result = pc::Resolve(Chain({0, 0, 0, 0}), 0x1000, memory.reader());
    Failure(result, pc::Status::ReadFailed, 2, 2);
    Check(result.detail == "fixture address is unreadable", "reader failure detail was lost");
    Check(memory.calls.size() == 3, "reads continued after failure");
    memory.values[0x1000] = Pointer(0);
    Failure(pc::Resolve(Chain({0x100}), 0x1000, memory.reader()), pc::Status::NullPointer, 0, 0);

    for (const auto width : {4U, 8U}) {
        for (const auto byteCount : {0U, width - 1, width, width + 1}) {
            for (const auto dataLength : {0U, width - 1, width, width + 1}) {
                if (byteCount == width && dataLength == width) { continue; }
                memory = {};
                auto read = Pointer(0x2000, width);
                read.bytesRead = byteCount;
                read.bytes.resize(dataLength);
                read.detail = "backend transferred an inconsistent size";
                memory.values[0x1000] = read;
                const auto partial = pc::Resolve(Chain({0, 0}, width), 0x1000, memory.reader());
                Failure(partial, pc::Status::PartialRead, 0, 0);
                Check(partial.detail == read.detail, "partial-read detail was lost");
                Check(memory.calls.size() == 1, "reads continued after partial/oversized data");
            }
        }
    }
    memory = {};
    auto failedWithData = Pointer(0x2000);
    failedWithData.ok = false;
    memory.values[0x1000] = failedWithData;
    Failure(pc::Resolve(Chain({0}), 0x1000, memory.reader()), pc::Status::ReadFailed, 0, 0);

    std::size_t continued = 0;
    const auto throwing = pc::Resolve(Chain({0}), 0x1000,
        [](std::uint64_t, std::size_t) -> pc::ReadResult { throw std::runtime_error("reader exception"); },
        [&continued] { ++continued; return true; });
    Failure(throwing, pc::Status::ReadFailed, 0, 0);
    Check(throwing.detail == "reader exception" && continued == 2,
          "reader exception lost detail or skipped post-read continuation");
}

void CancellationAndBounds()
{
    Memory memory;
    memory.values[0x1000] = Pointer(0x2000);
    memory.values[0x2000] = Pointer(0x3000);
    for (const std::size_t cancellationCheck : {1U, 2U, 3U, 4U}) {
        memory.calls.clear();
        std::size_t continuationCalls = 0;
        const auto result = pc::Resolve(Chain({0, 0}), 0x1000, memory.reader(), [&] {
            return ++continuationCalls < cancellationCheck;
        });
        Failure(result, pc::Status::Cancelled, (cancellationCheck - 1) / 2,
                (cancellationCheck - 1) / 2);
        Check(continuationCalls == cancellationCheck, "continued checking after cancellation");
        Check(memory.calls.size() == cancellationCheck / 2, "incorrect reads before cancellation");
    }

    memory = {};
    std::size_t continuationCalls = 0;
    Failure(pc::Resolve(Chain({0}), 0x1000, memory.reader(), [&] {
        return ++continuationCalls < 2;
    }), pc::Status::Cancelled, 0, 0);
    Check(memory.calls.size() == 1 && continuationCalls == 2,
          "failed read skipped the post-read cancellation check");

    memory = {};
    pc::Chain chain;
    chain.offsets.resize(pc::MaxDepth, 0);
    for (std::size_t level = 0; level < pc::MaxDepth; ++level) {
        memory.values[0x1000 + level * 0x10] = Pointer(0x1000 + (level + 1) * 0x10);
    }
    continuationCalls = 0;
    auto result = pc::Resolve(chain, 0x1000, memory.reader(), [&] { ++continuationCalls; return true; });
    Check(result.ok() && result.address == 0x1100, "maximum-depth chain rejected");
    Check(memory.calls.size() == pc::MaxDepth && result.steps.size() == pc::MaxDepth,
          "maximum-depth chain exceeds read bound");
    Check(continuationCalls == 2 * pc::MaxDepth, "maximum-depth continuation count incorrect");

    chain.offsets.push_back(0);
    memory.calls.clear();
    continuationCalls = 0;
    Failure(pc::Resolve(chain, 0x1000, memory.reader(), [&] { ++continuationCalls; return true; }),
            pc::Status::InvalidChain, 0, 0);
    Check(memory.calls.empty() && continuationCalls == 0, "over-depth chain invoked callbacks");
    chain.offsets.clear();
    Failure(pc::Resolve(chain, 0x1000, memory.reader()), pc::Status::InvalidChain, 0, 0);
    chain.offsets = {0};
    for (const auto width : {0U, 1U, 2U, 3U, 5U, 16U}) {
        chain.pointerSize = width;
        Failure(pc::Resolve(chain, 0x1000, memory.reader()), pc::Status::InvalidChain, 0, 0);
    }
    Failure(pc::Resolve(Chain({0}), 0x1000, {}), pc::Status::InvalidChain, 0, 0);
    Failure(pc::Resolve(Chain({0}), 0, memory.reader()), pc::Status::NullPointer, 0, 0);
    Check(memory.calls.empty(), "invalid chain or null root invoked the reader");
}

void Cycles()
{
    Memory memory;
    memory.values[0x1000] = Pointer(0x1000);
    Failure(pc::Resolve(Chain({0}), 0x1000, memory.reader()), pc::Status::CycleDetected, 0, 0);
    Check(memory.calls.size() == 1, "self-cycle issued repeated reads");
    memory = {};
    memory.values[0x1000] = Pointer(0x2000);
    memory.values[0x2000] = Pointer(0x1000);
    Failure(pc::Resolve(Chain({0, 0, 0}), 0x1000, memory.reader()), pc::Status::CycleDetected, 1, 1);
    Check(memory.calls.size() == 2, "two-node cycle issued repeated reads");
    memory.values[0x2000] = Pointer(0x1010);
    memory.calls.clear();
    Failure(pc::Resolve(Chain({0, -0x10}), 0x1000, memory.reader()), pc::Status::CycleDetected, 1, 1);
    Check(memory.calls.size() == 2, "offset-induced cycle issued repeated reads");
}

} // namespace

int main()
{
    try {
        OffsetParsing();
        ValidChains();
        AddressLimits();
        ReadFailures();
        CancellationAndBounds();
        Cycles();
        std::printf("Pointer-chain core: %zu checks passed.\n", checks);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Pointer-chain check %zu failed: %s\n", checks, error.what());
        return EXIT_FAILURE;
    }
}
