#include "../Ksword5.1/Ksword5.1/UI/MemoryEditHistory.Core.h"

#include <cstdlib>
#include <iostream>
#include <random>

using History = ks::ui::detail::MemoryEditHistory;
namespace
{
    unsigned checks = 0;
    void require(bool condition, const char* description)
    {
        ++checks;
        if (condition) return;
        std::cerr << "FAIL: " << description << '\n';
        std::exit(1);
    }
}

int main()
{
    History history;
    const History::Bytes original{1,2,3,4,5,6,7,8};
    History::Bytes current = original;
    current[0] = 9; current[1] = 10; current[6] = 11;
    require(history.record(original, current) == History::RecordResult::Recorded, "batch edit records");
    require(history.undoCount() == 1 && history.redoCount() == 0, "one refresh is one transaction");
    const auto firstUndo = history.undo(current);
    require(firstUndo && *firstUndo == original, "undo sparse and contiguous ranges atomically");
    require(history.canRedo() && !history.canUndo(), "cursor after undo");
    require(history.record(original, original) == History::RecordResult::Unchanged && history.canRedo(),
        "no-op preserves redo");
    require(history.redo(*firstUndo) == current, "redo exact bytes");
    auto mismatch = current;
    mismatch[6] = 99;
    require(!history.undo(mismatch) && history.canUndo(), "mismatch rejects atomically without moving cursor");
    require(!history.undo(History::Bytes{1,2}), "length mismatch rejects");
    require(history.undo(current) == original, "failed playback preserves valid transaction");
    auto replacement = original;
    replacement[4] = 42;
    require(history.record(original.data(), replacement.data(), original.size()) == History::RecordResult::Recorded,
        "pointer API avoids full snapshot copy");
    require(!history.canRedo() && history.undoCount() == 1, "new edit branches and removes redo");
    require(history.undo(replacement.data(), replacement.size()) == original, "pointer undo");
    require(history.redo(original.data(), original.size()) == replacement, "pointer redo");
    const auto retainedCost = history.memoryUsage();
    require(retainedCost > 0 && retainedCost <= 32U * 1024U * 1024U, "accounted memory bounded");
    history.reset();
    require(!history.canUndo() && !history.canRedo() && !history.memoryUsage(), "reset clears all history");

    History countBounded(4096, 2);
    History::Bytes countCurrent(16, 0);
    for (unsigned i = 1; i <= 3; ++i)
    {
        auto next = countCurrent;
        next[0] = static_cast<std::uint8_t>(i);
        require(countBounded.record(countCurrent, next) == History::RecordResult::Recorded, "record bounded count");
        countCurrent = std::move(next);
    }
    require(countBounded.undoCount() == 2, "oldest command evicted at count limit");
    countCurrent = *countBounded.undo(countCurrent);
    countCurrent = *countBounded.undo(countCurrent);
    require(countCurrent[0] == 1 && !countBounded.canUndo(), "eviction retains nearest valid history");

    History byteBounded(retainedCost, 256);
    History::Bytes byteCurrent(16, 0);
    for (unsigned i = 1; i <= 4; ++i)
    {
        auto next = byteCurrent;
        next[0] = static_cast<std::uint8_t>(i);
        require(byteBounded.record(byteCurrent, next) == History::RecordResult::Recorded, "record byte bounded");
        require(byteBounded.memoryUsage() <= retainedCost && byteBounded.undoCount() == 1,
            "byte bound evicts earlier changes");
        byteCurrent = std::move(next);
    }
    const History::Bytes largeBefore(1024, 0), largeAfter(1024, 1);
    require(byteBounded.record(largeBefore, largeAfter) == History::RecordResult::CapacityExceeded,
        "oversized transaction rejected before retaining payload");
    require(!byteBounded.canUndo() && !byteBounded.canRedo() && !byteBounded.memoryUsage(),
        "oversized edit invalidates stale timeline");
    require(largeAfter.front() == 1 && largeAfter.back() == 1, "edited buffer never discarded");
    History disabled(4096, 0);
    require(disabled.record(original, replacement) == History::RecordResult::CapacityExceeded, "zero command bound");
    require(history.record(original, replacement) == History::RecordResult::Recorded, "record before size reset");
    require(history.record(original, History::Bytes{1}) == History::RecordResult::SizeMismatch
        && !history.canUndo(), "size change resets timeline");
    require(history.record(nullptr, original.data(), original.size()) == History::RecordResult::InvalidInput,
        "invalid pointer rejects");
    require(history.record(nullptr, nullptr, 0) == History::RecordResult::Unchanged, "empty input is no-op");

    // Random batches test the actual timeline against complete reference snapshots.
    History randomHistory(2U * 1024U * 1024U, 256);
    std::mt19937 random(42);
    std::vector<History::Bytes> snapshots{History::Bytes(4096, 0)};
    for (unsigned transaction = 0; transaction < 128; ++transaction)
    {
        auto next = snapshots.back();
        for (unsigned byte = 0; byte < 8; ++byte)
        {
            const auto offset = random() % next.size();
            next[offset] ^= static_cast<std::uint8_t>((random() % 255) + 1);
        }
        require(randomHistory.record(snapshots.back(), next) == History::RecordResult::Recorded,
            "random transaction records");
        snapshots.push_back(std::move(next));
    }
    auto randomCurrent = snapshots.back();
    for (std::size_t i = snapshots.size() - 1; i; --i)
    {
        const auto restored = randomHistory.undo(randomCurrent);
        require(restored && *restored == snapshots[i - 1], "random undo equals reference snapshot");
        randomCurrent = *restored;
    }
    for (std::size_t i = 1; i < snapshots.size(); ++i)
    {
        const auto restored = randomHistory.redo(randomCurrent);
        require(restored && *restored == snapshots[i], "random redo equals reference snapshot");
        randomCurrent = *restored;
    }
    std::cout << "PASS: " << checks << " memory history checks\n";
}
