#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// No Qt, Windows headers, live process inspection or PFN snapshot assumptions.
namespace ks::evidence::pool
{
enum class EventKind { Allocate, Free };

struct Event
{
    EventKind kind = EventKind::Allocate;
    std::uint64_t address = 0;
    std::uint64_t size = 0; // Allocation size; ignored on Free.
    std::uint32_t tag = 0;
    std::uint32_t poolType = 0;
    std::uint32_t sessionId = 0xffffffffU; // Ordinary pool has no session ID.
    std::uint64_t timestamp = 0;
    std::uint32_t pid = 0;
    std::uint32_t tid = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint64_t> stack;
    // Same length as stack, stable historical image IDs, 0 = unresolved.
    // An empty vector is equivalent to all-zero IDs.
    std::vector<std::uint64_t> frameImageIds;
};

enum class GapReason : std::uint32_t
{
    LostEvents = 1U << 0,
    DecodeFailure = 1U << 1,
    EventLimit = 1U << 2,
    ActiveLimit = 1U << 3,
    GroupLimit = 1U << 4,
    StackLimit = 1U << 5,
    OutOfOrder = 1U << 6,
    AddressReuse = 1U << 7,
    PoolMismatch = 1U << 8,
    Overflow = 1U << 9,
    Cancelled = 1U << 10,
    MissingStack = 1U << 11
};

struct Limits
{
    std::size_t maxEvents = 5000000;
    std::size_t maxActiveAllocations = 250000;
    std::size_t maxGroups = 10000;
    std::size_t maxStackFrames = 192;
    std::size_t maxStoredStackFrames = 1000000;
};

struct Group
{
    // Stable ordinal for the same ordered input and limits. Never a stack hash.
    std::uint64_t groupId = 0;
    std::uint32_t tag = 0;
    std::uint32_t poolType = 0;
    std::uint32_t sessionId = 0xffffffffU;
    std::vector<std::uint64_t> stack; // Full original frames; never truncated.
    std::vector<std::uint64_t> frameImageIds;
    std::uint64_t representativeTimestamp = 0;
    std::uint32_t representativePid = 0;
    std::uint32_t representativeTid = 0;
    std::uint64_t firstTimestamp = 0;
    std::uint64_t lastTimestamp = 0;
    std::uint64_t allocatedBytes = 0;
    std::uint64_t allocatedCount = 0;
    std::uint64_t pairedFreedBytes = 0;
    std::uint64_t pairedFreedCount = 0;
    // Allocations observed in this interval with no observed matching free.
    // Neither these values nor uncertain values constitute proof of a leak.
    std::uint64_t outstandingBytes = 0;
    std::uint64_t outstandingCount = 0;
    // Instances whose fate cannot be confirmed (gap, address reuse, mismatch).
    std::uint64_t uncertainBytes = 0;
    std::uint64_t uncertainCount = 0;
};

struct Statistics
{
    std::uint64_t eventsSeen = 0;
    std::uint64_t eventsAccepted = 0;
    std::uint64_t eventsRejected = 0;
    std::uint64_t unmatchedFrees = 0;
    std::uint64_t addressReuses = 0;
    std::uint64_t poolMismatches = 0;
    std::uint64_t outOfOrderEvents = 0;
    std::uint64_t invalidEvents = 0;
    std::uint64_t missingStacks = 0;
    std::uint64_t capacityRejected = 0;
    std::uint64_t lostEvents = 0;
    std::uint64_t decodeFailures = 0;
    std::uint64_t arithmeticOverflows = 0;
    std::size_t peakActiveAllocations = 0;
    std::size_t storedStackFrames = 0;
};

struct Result
{
    std::vector<Group> groups;
    Statistics stats;
    std::uint32_t gapReasons = 0;
    // Sticky: later matched events never erase a previous coverage gap.
    bool coverageComplete = true;
    bool stackCoverageComplete = true;
    bool lifetimePairingAvailable = true;
    bool cancelled = false;
    bool finished = false;
};

class Analyzer
{
public:
    explicit Analyzer(Limits limits = {});
    // Caller owns synchronization and the timestamp clock domain. Feed in
    // strictly increasing (timestamp, sequence) order. Out-of-order records
    // are rejected rather than retroactively changing allocation lifetimes.
    bool Feed(const Event& event);
    // Call at the actual gap position. Unknown intervals quarantine current
    // live instances; later allocations may be paired, but coverageComplete
    // remains false. Numeric totals saturate; Overflow makes them inexact.
    void NoteGap(GapReason reason, std::uint64_t count = 1);
    // The gap could occur anywhere in the interval (e.g. ETL EventsLost).
    // Reclassifies all observed allocations, including previously paired ones,
    // as uncertain and disables lifetime pairing for the rest of this replay.
    // count = 0 means a known gap with no exact event count, not absence of loss.
    void NoteUnknownGap(GapReason reason, std::uint64_t count = 1);
    void Cancel();
    // Freezes this analyzer; repeated Finish returns the same immutable result.
    const Result& Finish();

private:
    struct ActiveKey
    {
        std::uint64_t address = 0;
        std::uint32_t sessionId = 0xffffffffU;
        bool operator==(const ActiveKey& other) const noexcept
        {
            return address == other.address && sessionId == other.sessionId;
        }
    };
    struct ActiveHash
    {
        std::size_t operator()(const ActiveKey& key) const noexcept
        {
            return static_cast<std::size_t>(key.address
                ^ (static_cast<std::uint64_t>(key.sessionId) << 32)
                ^ key.sessionId);
        }
    };
    struct Active
    {
        std::uint64_t size = 0;
        std::size_t group = 0;
    };
    Limits limits_;
    Result result_;
    std::unordered_map<ActiveKey, Active, ActiveHash> active_;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> groupBuckets_;
    std::uint64_t lastTimestamp_ = 0;
    std::uint64_t lastSequence_ = 0;
    bool hasOrder_ = false;
    bool stopped_ = false;
    bool unknownGap_ = false;
    void Add(std::uint64_t& target, std::uint64_t amount);
    void MarkGap(GapReason reason);
    void Quarantine(const Active& active);
    void QuarantineAll();
    std::size_t FindGroup(const Event& event);
    bool Reject(GapReason reason);
};
}
