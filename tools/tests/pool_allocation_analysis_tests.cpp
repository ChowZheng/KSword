#include "../../shared/evidence/PoolAllocationAnalysis.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <utility>
#include <vector>

using namespace ks::evidence::pool;

namespace
{
unsigned checks = 0;
void Check(bool condition, const char* expression, unsigned line)
{
    ++checks;
    if (!condition)
    {
        std::cerr << "line " << line << ": " << expression << '\n';
        std::exit(1);
    }
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

Event Alloc(std::uint64_t address, std::uint64_t size, std::uint64_t timestamp,
            std::uint32_t tag = 1, std::uint32_t pool = 0,
            std::vector<std::uint64_t> stack = {0x100, 0x200})
{
    Event event;
    event.address = address;
    event.size = size;
    event.timestamp = timestamp;
    event.sequence = timestamp;
    event.tag = tag;
    event.poolType = pool;
    event.pid = 123;
    event.tid = 456;
    event.stack = std::move(stack);
    return event;
}

Event Free(std::uint64_t address, std::uint64_t timestamp, std::uint32_t pool = 0)
{
    Event event;
    event.kind = EventKind::Free;
    event.address = address;
    event.timestamp = timestamp;
    event.sequence = timestamp;
    event.poolType = pool;
    return event;
}

bool Gap(const Result& result, GapReason reason)
{
    return (result.gapReasons & static_cast<std::uint32_t>(reason)) != 0;
}

void Conservation(const Result& result)
{
    for (const Group& group : result.groups)
    {
        CHECK(group.allocatedBytes == group.pairedFreedBytes
            + group.outstandingBytes + group.uncertainBytes);
        CHECK(group.allocatedCount == group.pairedFreedCount
            + group.outstandingCount + group.uncertainCount);
        CHECK(group.stack.size() == group.frameImageIds.size());
    }
}

void NormalLifetimes()
{
    Analyzer analyzer;
    CHECK(analyzer.Feed(Free(999, 1))); // Allocated before the interval.
    CHECK(analyzer.Feed(Alloc(10, 32, 2)));
    CHECK(analyzer.Feed(Alloc(20, 64, 3)));
    auto free = Free(10, 4);
    free.tag = 999;
    free.size = 777; // A free's size/tag are not the observed allocation size.
    CHECK(analyzer.Feed(free));
    const Result& result = analyzer.Finish();
    CHECK(result.finished && result.coverageComplete && !result.cancelled);
    CHECK(result.groups.size() == 1);
    const Group& group = result.groups.front();
    CHECK(group.allocatedBytes == 96 && group.allocatedCount == 2);
    CHECK(group.pairedFreedBytes == 32 && group.pairedFreedCount == 1);
    CHECK(group.outstandingBytes == 64 && group.outstandingCount == 1);
    CHECK(group.uncertainCount == 0);
    CHECK(group.representativeTimestamp == 2 && group.representativePid == 123);
    CHECK(group.representativeTid == 456 && group.firstTimestamp == 2);
    CHECK(group.lastTimestamp == 4);
    CHECK(group.stack == std::vector<std::uint64_t>({0x100, 0x200}));
    CHECK(result.stats.unmatchedFrees == 1);
    CHECK(result.stats.eventsSeen == 4 && result.stats.eventsAccepted == 4);
    CHECK(&analyzer.Finish() == &result);
    CHECK(!analyzer.Feed(Free(20, 5)));
    CHECK(result.groups.front().outstandingBytes == 64);
    Conservation(result);
}

void KeysAndStableIds()
{
    Analyzer analyzer;
    std::vector<Event> events;
    events.push_back(Alloc(1, 1, 1));
    events.push_back(Alloc(2, 2, 2, 2));
    events.push_back(Alloc(3, 3, 3, 1, 1));
    events.push_back(Alloc(4, 4, 4, 1, 0, {0x200, 0x100}));
    auto imageOne = Alloc(5, 5, 5);
    imageOne.frameImageIds = {10, 20};
    events.push_back(imageOne);
    auto imageReload = Alloc(6, 6, 6);
    imageReload.frameImageIds = {11, 20};
    events.push_back(imageReload);
    auto unknownIds = Alloc(7, 7, 7);
    unknownIds.frameImageIds = {0, 0};
    events.push_back(unknownIds); // Explicit zeros == omitted IDs.
    auto anotherPid = Alloc(8, 8, 8);
    anotherPid.pid = 789;
    events.push_back(anotherPid); // PID alone is not a source stack identity.
    for (const auto& event : events)
        CHECK(analyzer.Feed(event));
    const Result& result = analyzer.Finish();
    CHECK(result.groups.size() == 6);
    CHECK(result.groups[0].allocatedBytes == 16);
    CHECK(result.groups[4].frameImageIds == std::vector<std::uint64_t>({10, 20}));
    CHECK(result.groups[5].frameImageIds == std::vector<std::uint64_t>({11, 20}));
    Analyzer replay;
    for (const auto& event : events)
        CHECK(replay.Feed(event));
    const Result& replayed = replay.Finish();
    for (std::size_t i = 0; i < result.groups.size(); ++i)
    {
        CHECK(result.groups[i].groupId == i + 1);
        CHECK(result.groups[i].groupId == replayed.groups[i].groupId);
        CHECK(result.groups[i].stack == replayed.groups[i].stack);
    }
    Conservation(result);
}

void ReuseAndPoolMismatch()
{
    Analyzer analyzer;
    CHECK(analyzer.Feed(Alloc(1, 100, 1)));
    CHECK(analyzer.Feed(Alloc(1, 200, 2))); // Old lifetime's fate is unknown.
    CHECK(analyzer.Feed(Free(1, 3)));
    CHECK(analyzer.Feed(Alloc(2, 300, 4, 2, 1)));
    CHECK(analyzer.Feed(Alloc(2, 400, 5, 3, 2)));
    CHECK(analyzer.Feed(Free(2, 6, 2)));
    CHECK(analyzer.Feed(Alloc(3, 500, 7, 4, 1)));
    CHECK(analyzer.Feed(Free(3, 8, 2))); // Must not match across pool types.
    CHECK(analyzer.Feed(Free(3, 9, 1))); // Old lifetime was quarantined.
    const Result& result = analyzer.Finish();
    CHECK(!result.coverageComplete);
    CHECK(result.stats.addressReuses == 2 && result.stats.poolMismatches == 2);
    CHECK(result.stats.unmatchedFrees == 2);
    CHECK(result.groups[0].uncertainBytes == 100);
    CHECK(result.groups[0].pairedFreedBytes == 200);
    CHECK(result.groups[1].uncertainBytes == 300);
    CHECK(result.groups[2].pairedFreedBytes == 400);
    CHECK(result.groups[3].uncertainBytes == 500);
    CHECK(Gap(result, GapReason::AddressReuse) && Gap(result, GapReason::PoolMismatch));
    Conservation(result);
}

void GapsCannotPairOldLifetimes()
{
    Analyzer analyzer;
    CHECK(analyzer.Feed(Alloc(1, 100, 1)));
    CHECK(analyzer.Feed(Alloc(2, 200, 2)));
    analyzer.NoteGap(GapReason::LostEvents, 9);
    CHECK(analyzer.Feed(Free(1, 3)));
    CHECK(analyzer.Feed(Alloc(1, 400, 4)));
    CHECK(analyzer.Feed(Free(1, 5)));
    const Result& result = analyzer.Finish();
    CHECK(!result.coverageComplete && result.stats.lostEvents == 9);
    CHECK(result.groups[0].uncertainBytes == 300);
    CHECK(result.groups[0].pairedFreedBytes == 400);
    CHECK(result.groups[0].outstandingBytes == 0);
    CHECK(result.stats.unmatchedFrees == 1);
    Conservation(result);

    Analyzer unknownPosition;
    unknownPosition.NoteUnknownGap(GapReason::LostEvents, 1);
    CHECK(unknownPosition.Feed(Alloc(1, 64, 1)));
    CHECK(unknownPosition.Feed(Free(1, 2)));
    CHECK(!unknownPosition.Finish().coverageComplete);
    CHECK(unknownPosition.Finish().groups[0].pairedFreedBytes == 0);
    CHECK(unknownPosition.Finish().groups[0].uncertainBytes == 64);
    CHECK(!unknownPosition.Finish().lifetimePairingAvailable);
}

void UnknownPositionGaps()
{
    for (unsigned position = 0; position < 3; ++position)
    {
        Analyzer analyzer;
        if (position == 0)
            analyzer.NoteUnknownGap(GapReason::LostEvents, 3);
        CHECK(analyzer.Feed(Alloc(1, 100, 1)));
        if (position == 1)
            analyzer.NoteUnknownGap(GapReason::LostEvents, 3);
        CHECK(analyzer.Feed(Free(1, 2)));
        if (position == 2)
            analyzer.NoteUnknownGap(GapReason::LostEvents, 3);
        CHECK(analyzer.Feed(Alloc(2, 200, 3)));
        CHECK(analyzer.Feed(Free(2, 4)));
        CHECK(analyzer.Feed(Alloc(3, 300, 5)));
        const Result& result = analyzer.Finish();
        CHECK(!result.coverageComplete && !result.lifetimePairingAvailable);
        CHECK(result.stats.lostEvents == 3);
        CHECK(result.stats.unmatchedFrees == 2);
        CHECK(result.groups[0].allocatedBytes == 600);
        CHECK(result.groups[0].uncertainBytes == 600);
        CHECK(result.groups[0].pairedFreedBytes == 0);
        CHECK(result.groups[0].pairedFreedCount == 0);
        CHECK(result.groups[0].outstandingBytes == 0);
        CHECK(result.groups[0].outstandingCount == 0);
        Conservation(result);
    }

    // A late unknown-position gap must not subtract from saturated totals.
    constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
    Analyzer overflow;
    CHECK(overflow.Feed(Alloc(1, max, 1)));
    CHECK(overflow.Feed(Free(1, 2)));
    CHECK(overflow.Feed(Alloc(2, max, 3)));
    overflow.NoteUnknownGap(GapReason::LostEvents);
    const Result& result = overflow.Finish();
    CHECK(result.groups[0].allocatedBytes == max);
    CHECK(result.groups[0].uncertainBytes == max);
    CHECK(result.groups[0].uncertainCount == 2);
    CHECK(result.groups[0].pairedFreedBytes == 0 && result.groups[0].outstandingBytes == 0);
    CHECK(Gap(result, GapReason::Overflow));

    for (unsigned position = 0; position < 3; ++position)
    {
        // A lost-buffer/marker says coverage failed, but does not reveal an
        // exact number of lost pool events. Do not fabricate an event count.
        Analyzer unknownCount;
        if (position == 0)
            unknownCount.NoteUnknownGap(GapReason::LostEvents, 0);
        CHECK(unknownCount.Feed(Alloc(1, 64, 1)));
        if (position == 1)
            unknownCount.NoteUnknownGap(GapReason::LostEvents, 0);
        CHECK(unknownCount.Feed(Free(1, 2)));
        if (position == 2)
            unknownCount.NoteUnknownGap(GapReason::LostEvents, 0);
        CHECK(unknownCount.Feed(Alloc(2, 128, 3)));
        const Result& unknownResult = unknownCount.Finish();
        CHECK(!unknownResult.coverageComplete && !unknownResult.lifetimePairingAvailable);
        CHECK(unknownResult.stats.lostEvents == 0);
        CHECK(Gap(unknownResult, GapReason::LostEvents));
        CHECK(unknownResult.groups[0].uncertainBytes == 192);
        CHECK(unknownResult.groups[0].pairedFreedBytes == 0);
        CHECK(unknownResult.groups[0].outstandingBytes == 0);
        CHECK(unknownResult.stats.unmatchedFrees == 1);
        Conservation(unknownResult);
        unknownCount.NoteUnknownGap(GapReason::LostEvents, 100); // Frozen result.
        CHECK(unknownResult.stats.lostEvents == 0);
    }
}

void SessionBoundaries()
{
    Analyzer analyzer;
    Event ordinary = Alloc(1, 100, 1);
    CHECK(analyzer.Feed(ordinary));
    Event sessionOne = Alloc(1, 200, 2);
    sessionOne.sessionId = 1;
    CHECK(analyzer.Feed(sessionOne));
    Event sessionTwo = Alloc(1, 300, 3);
    sessionTwo.sessionId = 2;
    CHECK(analyzer.Feed(sessionTwo));
    Event freeSession = Free(1, 4);
    freeSession.sessionId = 1;
    CHECK(analyzer.Feed(freeSession));
    CHECK(analyzer.Feed(Free(1, 5)));
    freeSession.timestamp = 6;
    freeSession.sequence = 6;
    freeSession.sessionId = 3;
    CHECK(analyzer.Feed(freeSession)); // A different session cannot free session 2.
    const Result& result = analyzer.Finish();
    CHECK(result.coverageComplete && result.groups.size() == 3);
    CHECK(result.stats.addressReuses == 0 && result.stats.poolMismatches == 0);
    CHECK(result.stats.unmatchedFrees == 1);
    CHECK(result.groups[0].sessionId == 0xffffffffU && result.groups[0].pairedFreedBytes == 100);
    CHECK(result.groups[1].sessionId == 1 && result.groups[1].pairedFreedBytes == 200);
    CHECK(result.groups[2].sessionId == 2 && result.groups[2].outstandingBytes == 300);
    Conservation(result);
}

void Ordering()
{
    Analyzer analyzer;
    Event alloc = Alloc(1, 64, 20);
    alloc.sequence = 1;
    CHECK(analyzer.Feed(alloc));
    Event free = Free(1, 20);
    free.sequence = 2; // Same timestamp is valid if sequence increases.
    CHECK(analyzer.Feed(free));
    Event next = Alloc(2, 100, 20);
    next.sequence = 3;
    CHECK(analyzer.Feed(next));
    CHECK(!analyzer.Feed(next)); // Duplicate cannot inflate allocation totals.
    CHECK(!analyzer.Feed(Free(2, 19)));
    CHECK(analyzer.Feed(Free(2, 21)));
    const Result& result = analyzer.Finish();
    CHECK(result.stats.outOfOrderEvents == 2);
    CHECK(result.groups[0].allocatedBytes == 164);
    CHECK(result.groups[0].pairedFreedBytes == 64);
    CHECK(result.groups[0].uncertainBytes == 100);
    CHECK(result.stats.unmatchedFrees == 1);
    CHECK(Gap(result, GapReason::OutOfOrder));
    Conservation(result);
}

void CapacityLimits()
{
    Limits limits;
    limits.maxEvents = 2;
    Analyzer events(limits);
    CHECK(events.Feed(Alloc(1, 64, 1)));
    CHECK(events.Feed(Alloc(2, 32, 2)));
    CHECK(!events.Feed(Free(1, 3)));
    CHECK(!events.Feed(Free(2, 4)));
    const Result& eventResult = events.Finish();
    CHECK(Gap(eventResult, GapReason::EventLimit));
    CHECK(eventResult.stats.eventsSeen == 4 && eventResult.stats.eventsAccepted == 2);
    CHECK(eventResult.stats.eventsRejected == 2 && eventResult.stats.capacityRejected == 2);
    CHECK(eventResult.groups[0].uncertainBytes == 96);
    Conservation(eventResult);

    limits = {};
    limits.maxActiveAllocations = 1;
    Analyzer active(limits);
    CHECK(active.Feed(Alloc(1, 64, 1)));
    CHECK(!active.Feed(Alloc(2, 128, 2)));
    CHECK(active.Feed(Alloc(3, 256, 3)));
    CHECK(active.Feed(Free(3, 4)));
    const Result& activeResult = active.Finish();
    CHECK(activeResult.stats.peakActiveAllocations == 1);
    CHECK(activeResult.groups[0].allocatedBytes == 320);
    CHECK(activeResult.groups[0].pairedFreedBytes == 256);
    CHECK(activeResult.groups[0].uncertainBytes == 64);
    CHECK(!activeResult.coverageComplete && Gap(activeResult, GapReason::ActiveLimit));
    Conservation(activeResult);

    limits = {};
    limits.maxGroups = 1;
    Analyzer groups(limits);
    CHECK(groups.Feed(Alloc(1, 64, 1)));
    CHECK(!groups.Feed(Alloc(2, 128, 2, 2)));
    CHECK(groups.Feed(Alloc(3, 256, 3)));
    const Result& groupResult = groups.Finish();
    CHECK(groupResult.groups.size() == 1);
    CHECK(groupResult.groups[0].uncertainBytes == 64);
    CHECK(groupResult.groups[0].outstandingBytes == 256);
    CHECK(!groupResult.coverageComplete && Gap(groupResult, GapReason::GroupLimit));
    Conservation(groupResult);

    limits = {};
    limits.maxStackFrames = 1;
    Analyzer perStack(limits);
    CHECK(!perStack.Feed(Alloc(1, 64, 1)));
    CHECK(perStack.Finish().groups.empty());
    CHECK(Gap(perStack.Finish(), GapReason::StackLimit));

    limits = {};
    limits.maxStoredStackFrames = 2;
    Analyzer stacks(limits);
    CHECK(stacks.Feed(Alloc(1, 64, 1)));
    CHECK(stacks.Feed(Alloc(2, 128, 2))); // Existing group consumes no more frames.
    CHECK(!stacks.Feed(Alloc(3, 256, 3, 2)));
    const Result& stackResult = stacks.Finish();
    CHECK(stackResult.groups.size() == 1 && stackResult.stats.storedStackFrames == 2);
    CHECK(stackResult.groups[0].stack == std::vector<std::uint64_t>({0x100, 0x200}));
    CHECK(stackResult.groups[0].uncertainBytes == 192);
    CHECK(Gap(stackResult, GapReason::StackLimit));
    Conservation(stackResult);

    limits = {};
    limits.maxEvents = limits.maxActiveAllocations = limits.maxGroups = 0;
    Analyzer zero(limits);
    CHECK(!zero.Feed(Alloc(1, 64, 1)));
    CHECK(zero.Finish().groups.empty());
}

void InvalidMissingAndCancelled()
{
    Analyzer analyzer;
    CHECK(analyzer.Feed(Alloc(1, 32, 1, 1, 0, {})));
    CHECK(analyzer.Feed(Free(1, 2))); // Missing stack does not destroy pairability.
    CHECK(analyzer.Feed(Alloc(2, 64, 3)));
    Event mismatch = Alloc(3, 128, 4);
    mismatch.frameImageIds = {123};
    CHECK(!analyzer.Feed(mismatch));
    CHECK(!analyzer.Feed(Alloc(0, 10, 5)));
    CHECK(!analyzer.Feed(Alloc(4, 0, 6)));
    auto invalidKind = Alloc(5, 10, 7);
    invalidKind.kind = static_cast<EventKind>(123);
    CHECK(!analyzer.Feed(invalidKind));
    const Result& result = analyzer.Finish();
    CHECK(!result.stackCoverageComplete && !result.coverageComplete);
    CHECK(result.stats.missingStacks == 1 && result.stats.invalidEvents == 4);
    CHECK(result.stats.decodeFailures == 4);
    CHECK(Gap(result, GapReason::MissingStack) && Gap(result, GapReason::DecodeFailure));
    CHECK(result.groups[0].pairedFreedBytes == 32);
    CHECK(result.groups[1].uncertainBytes == 64);
    Conservation(result);

    Analyzer cancel;
    CHECK(cancel.Feed(Alloc(1, 64, 1)));
    cancel.Cancel();
    CHECK(!cancel.Feed(Free(1, 2)));
    const Result& cancelled = cancel.Finish();
    CHECK(cancelled.cancelled && cancelled.finished && !cancelled.coverageComplete);
    CHECK(cancelled.groups[0].uncertainBytes == 64 && cancelled.groups[0].outstandingBytes == 0);
    CHECK(cancelled.stats.eventsSeen == 1 && Gap(cancelled, GapReason::Cancelled));
    cancel.NoteGap(GapReason::LostEvents, 100);
    CHECK(cancelled.stats.lostEvents == 0);
    Conservation(cancelled);
}

void Overflow()
{
    constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
    Analyzer analyzer;
    CHECK(analyzer.Feed(Alloc(1, max, 1)));
    CHECK(analyzer.Feed(Alloc(2, 1, 2)));
    CHECK(analyzer.Feed(Free(2, 3)));
    const Result& result = analyzer.Finish();
    CHECK(result.groups[0].allocatedBytes == max);
    CHECK(result.groups[0].outstandingBytes == max); // No subtract from saturated total.
    CHECK(result.groups[0].pairedFreedBytes == 1);
    CHECK(!result.coverageComplete && result.stats.arithmeticOverflows == 1);
    CHECK(Gap(result, GapReason::Overflow));

    Analyzer remaining;
    CHECK(remaining.Feed(Alloc(1, max, 1)));
    CHECK(remaining.Feed(Alloc(2, max, 2)));
    CHECK(remaining.Finish().groups[0].outstandingBytes == max);
    CHECK(remaining.Finish().stats.arithmeticOverflows == 2);

    Analyzer losses;
    losses.NoteGap(GapReason::LostEvents, max);
    losses.NoteGap(GapReason::LostEvents, 1);
    CHECK(losses.Finish().stats.lostEvents == max);
    CHECK(losses.Finish().stats.arithmeticOverflows == 1);
}

void StreamingReference()
{
    Analyzer analyzer;
    struct Reference { std::uint64_t size; std::uint32_t tag; };
    std::map<std::uint64_t, Reference> live;
    std::map<std::uint32_t, std::uint64_t> allocated, freed, outstanding;
    std::uint64_t state = 12345;
    std::uint64_t nextAddress = 1;
    for (std::uint64_t time = 1; time <= 20000; ++time)
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        if (live.empty() || (state >> 63) == 0)
        {
            const std::uint64_t address = nextAddress++;
            const std::uint64_t size = 1 + ((state >> 8) & 0xfffU);
            const std::uint32_t tag = static_cast<std::uint32_t>((state >> 24) & 7U);
            CHECK(analyzer.Feed(Alloc(address, size, time, tag)));
            live.emplace(address, Reference{ size, tag });
            allocated[tag] += size;
        }
        else
        {
            const auto first = live.begin();
            CHECK(analyzer.Feed(Free(first->first, time)));
            freed[first->second.tag] += first->second.size;
            live.erase(first);
        }
    }
    for (const auto& entry : live)
        outstanding[entry.second.tag] += entry.second.size;
    const Result& result = analyzer.Finish();
    CHECK(result.coverageComplete && result.stats.eventsAccepted == 20000);
    CHECK(result.groups.size() == allocated.size());
    CHECK(result.stats.storedStackFrames == result.groups.size() * 2);
    for (const auto& group : result.groups)
    {
        CHECK(group.allocatedBytes == allocated[group.tag]);
        CHECK(group.pairedFreedBytes == freed[group.tag]);
        CHECK(group.outstandingBytes == outstanding[group.tag]);
        CHECK(group.uncertainBytes == 0);
    }
    Conservation(result);
}
}

int main()
{
    NormalLifetimes();
    KeysAndStableIds();
    ReuseAndPoolMismatch();
    SessionBoundaries();
    GapsCannotPairOldLifetimes();
    UnknownPositionGaps();
    Ordering();
    CapacityLimits();
    InvalidMissingAndCancelled();
    Overflow();
    StreamingReference();
    std::cout << "POOL_ALLOCATION_ANALYSIS_TESTS=PASS CHECKS=" << checks << '\n';
}
