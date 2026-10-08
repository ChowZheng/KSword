#include "PoolAllocationAnalysis.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace ks::evidence::pool
{
namespace
{
constexpr std::size_t kNoGroup = (std::numeric_limits<std::size_t>::max)();
constexpr std::uint64_t kMax = (std::numeric_limits<std::uint64_t>::max)();

void HashWord(std::uint64_t& hash, std::uint64_t value)
{
    // Defined byte order, independent of host ABI and unordered_map salt.
    for (unsigned i = 0; i != 8; ++i)
    {
        hash ^= value & 0xffU;
        hash *= 1099511628211ULL;
        value >>= 8;
    }
}

std::uint64_t ImageId(const Event& event, std::size_t index)
{
    return event.frameImageIds.empty() ? 0 : event.frameImageIds[index];
}

bool SameGroup(const Group& group, const Event& event)
{
    if (group.tag != event.tag || group.poolType != event.poolType
        || group.sessionId != event.sessionId
        || group.stack != event.stack)
        return false;
    for (std::size_t i = 0; i < event.stack.size(); ++i)
        if (group.frameImageIds[i] != ImageId(event, i))
            return false;
    return true;
}
}

Analyzer::Analyzer(Limits limits) : limits_(limits) {}

void Analyzer::MarkGap(GapReason reason)
{
    result_.coverageComplete = false;
    result_.gapReasons |= static_cast<std::uint32_t>(reason);
}

void Analyzer::Add(std::uint64_t& target, std::uint64_t amount)
{
    if (amount > kMax - target)
    {
        target = kMax;
        MarkGap(GapReason::Overflow);
        // Do not recursively add the overflow diagnostic to itself.
        if (result_.stats.arithmeticOverflows != kMax)
            ++result_.stats.arithmeticOverflows;
        return;
    }
    target += amount;
}

void Analyzer::Quarantine(const Active& active)
{
    Group& group = result_.groups[active.group];
    Add(group.uncertainBytes, active.size);
    Add(group.uncertainCount, 1);
}

void Analyzer::QuarantineAll()
{
    for (const auto& entry : active_)
        Quarantine(entry.second);
    active_.clear();
}

void Analyzer::NoteGap(GapReason reason, std::uint64_t count)
{
    if (stopped_ || count == 0)
        return;
    MarkGap(reason);
    if (reason == GapReason::LostEvents)
        Add(result_.stats.lostEvents, count);
    else if (reason == GapReason::DecodeFailure)
        Add(result_.stats.decodeFailures, count);
    // Existing instances cannot be matched across an unobserved interval:
    // an unseen free + reallocation could otherwise pair the wrong lifetime.
    QuarantineAll();
}

void Analyzer::NoteUnknownGap(GapReason reason, std::uint64_t count)
{
    if (stopped_)
        return;
    if (count != 0)
        NoteGap(reason, count);
    else
    {
        MarkGap(reason);
        QuarantineAll();
    }
    unknownGap_ = true;
    result_.lifetimePairingAvailable = false;
    for (Group& group : result_.groups)
    {
        // Assign from the saturated allocation total. Subtracting previously
        // paired values after overflow would manufacture an exact remainder.
        group.uncertainBytes = group.allocatedBytes;
        group.uncertainCount = group.allocatedCount;
        Add(result_.stats.unmatchedFrees, group.pairedFreedCount);
        group.pairedFreedBytes = 0;
        group.pairedFreedCount = 0;
        group.outstandingBytes = 0;
        group.outstandingCount = 0;
    }
}

bool Analyzer::Reject(GapReason reason)
{
    Add(result_.stats.eventsRejected, 1);
    if (reason == GapReason::EventLimit || reason == GapReason::ActiveLimit
        || reason == GapReason::GroupLimit || reason == GapReason::StackLimit)
        Add(result_.stats.capacityRejected, 1);
    NoteGap(reason);
    return false;
}

std::size_t Analyzer::FindGroup(const Event& event)
{
    std::uint64_t hash = 14695981039346656037ULL;
    HashWord(hash, event.tag);
    HashWord(hash, event.poolType);
    HashWord(hash, event.sessionId);
    HashWord(hash, static_cast<std::uint64_t>(event.stack.size()));
    for (std::size_t i = 0; i < event.stack.size(); ++i)
    {
        HashWord(hash, event.stack[i]);
        HashWord(hash, ImageId(event, i));
    }
    const auto found = groupBuckets_.find(hash);
    if (found != groupBuckets_.end())
        for (const std::size_t index : found->second)
            if (SameGroup(result_.groups[index], event))
                return index;

    if (result_.groups.size() >= limits_.maxGroups)
    {
        Reject(GapReason::GroupLimit);
        return kNoGroup;
    }
    if (event.stack.size() > limits_.maxStoredStackFrames
        - result_.stats.storedStackFrames)
    {
        Reject(GapReason::StackLimit);
        return kNoGroup;
    }

    const std::size_t index = result_.groups.size();
    Group group;
    group.groupId = static_cast<std::uint64_t>(index) + 1;
    group.tag = event.tag;
    group.poolType = event.poolType;
    group.sessionId = event.sessionId;
    group.stack = event.stack;
    group.frameImageIds = event.frameImageIds;
    if (group.frameImageIds.empty())
        group.frameImageIds.resize(event.stack.size(), 0);
    group.representativeTimestamp = event.timestamp;
    group.representativePid = event.pid;
    group.representativeTid = event.tid;
    group.firstTimestamp = event.timestamp;
    group.lastTimestamp = event.timestamp;
    result_.groups.push_back(std::move(group));
    groupBuckets_[hash].push_back(index);
    result_.stats.storedStackFrames += event.stack.size();
    return index;
}

bool Analyzer::Feed(const Event& event)
{
    if (stopped_)
        return false;
    Add(result_.stats.eventsSeen, 1);
    if (result_.stats.eventsSeen > static_cast<std::uint64_t>(limits_.maxEvents))
        return Reject(GapReason::EventLimit);
    if (hasOrder_ && (event.timestamp < lastTimestamp_
        || (event.timestamp == lastTimestamp_ && event.sequence <= lastSequence_)))
    {
        Add(result_.stats.outOfOrderEvents, 1);
        return Reject(GapReason::OutOfOrder);
    }
    lastTimestamp_ = event.timestamp;
    lastSequence_ = event.sequence;
    hasOrder_ = true;
    if (event.address == 0
        || (event.kind != EventKind::Allocate && event.kind != EventKind::Free)
        || (event.kind == EventKind::Allocate && (event.size == 0
            || (!event.frameImageIds.empty()
                && event.frameImageIds.size() != event.stack.size()))))
    {
        Add(result_.stats.invalidEvents, 1);
        return Reject(GapReason::DecodeFailure);
    }

    const ActiveKey key{ event.address, event.sessionId };
    auto existing = active_.find(key);
    if (event.kind == EventKind::Free)
    {
        Add(result_.stats.eventsAccepted, 1);
        if (existing == active_.end())
        {
            // A trace can start after allocation. This is a measured boundary,
            // not in itself proof that ETW dropped an event.
            Add(result_.stats.unmatchedFrees, 1);
            return true;
        }
        Group& group = result_.groups[existing->second.group];
        group.lastTimestamp = event.timestamp;
        if (group.poolType != event.poolType)
        {
            Add(result_.stats.unmatchedFrees, 1);
            Add(result_.stats.poolMismatches, 1);
            MarkGap(GapReason::PoolMismatch);
            Quarantine(existing->second);
        }
        else
        {
            Add(group.pairedFreedBytes, existing->second.size);
            Add(group.pairedFreedCount, 1);
        }
        active_.erase(existing);
        return true;
    }

    if (event.stack.size() > limits_.maxStackFrames)
        return Reject(GapReason::StackLimit);
    if (existing != active_.end())
    {
        // Never fabricate a free for an overwritten address, even if pool and
        // tag agree. It is a different allocation instance with unknown fate.
        Add(result_.stats.addressReuses, 1);
        MarkGap(GapReason::AddressReuse);
        Group& previous = result_.groups[existing->second.group];
        previous.lastTimestamp = event.timestamp;
        if (previous.poolType != event.poolType)
        {
            Add(result_.stats.poolMismatches, 1);
            MarkGap(GapReason::PoolMismatch);
        }
        Quarantine(existing->second);
        active_.erase(existing);
    }
    if (!unknownGap_ && active_.size() >= limits_.maxActiveAllocations)
        return Reject(GapReason::ActiveLimit);
    const std::size_t groupIndex = FindGroup(event);
    if (groupIndex == kNoGroup)
        return false;
    Group& group = result_.groups[groupIndex];
    group.lastTimestamp = event.timestamp;
    Add(group.allocatedBytes, event.size);
    Add(group.allocatedCount, 1);
    if (event.stack.empty())
    {
        Add(result_.stats.missingStacks, 1);
        result_.stackCoverageComplete = false;
        MarkGap(GapReason::MissingStack);
    }
    if (unknownGap_)
    {
        Add(group.uncertainBytes, event.size);
        Add(group.uncertainCount, 1);
    }
    else
        active_.emplace(key, Active{ event.size, groupIndex });
    result_.stats.peakActiveAllocations = (std::max)(
        result_.stats.peakActiveAllocations, active_.size());
    Add(result_.stats.eventsAccepted, 1);
    return true;
}

void Analyzer::Cancel()
{
    if (stopped_)
        return;
    NoteGap(GapReason::Cancelled);
    result_.cancelled = true;
    stopped_ = true;
}

const Result& Analyzer::Finish()
{
    if (result_.finished)
        return result_;
    // Compute remaining bytes from the bounded active ledger once. This avoids
    // subtracting from a saturated running total after aggregate overflow.
    for (const auto& entry : active_)
    {
        Group& group = result_.groups[entry.second.group];
        Add(group.outstandingBytes, entry.second.size);
        Add(group.outstandingCount, 1);
    }
    active_.clear();
    groupBuckets_.clear();
    result_.finished = true;
    stopped_ = true;
    return result_;
}
}
