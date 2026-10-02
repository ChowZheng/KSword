#include "MemoryEditHistory.Core.h"

#include <algorithm>
#include <utility>

namespace ks::ui::detail
{
    MemoryEditHistory::MemoryEditHistory(std::size_t maximumBytes, std::size_t maximumCommands)
        : m_maximumBytes(maximumBytes), m_maximumCommands(maximumCommands)
    {
    }

    void MemoryEditHistory::reset()
    {
        m_commands.clear();
        m_cursor = 0;
        m_memoryUsage = 0;
    }

    MemoryEditHistory::RecordResult MemoryEditHistory::record(const Bytes& before, const Bytes& after)
    {
        if (before.size() != after.size())
        {
            reset();
            return RecordResult::SizeMismatch;
        }
        return record(before.data(), after.data(), before.size());
    }

    MemoryEditHistory::RecordResult MemoryEditHistory::record(
        const std::uint8_t* before, const std::uint8_t* after, std::size_t length)
    {
        if (length && (!before || !after)) return RecordResult::InvalidInput;
        Command command;
        command.length = length;
        command.cost = sizeof(Command);
        std::size_t payloadCapacity = 0;
        // Build a single transaction containing only contiguous differing runs.
        // Check the bound before allocating each payload, including sparse edits.
        for (std::size_t offset = 0; offset < length;)
        {
            if (before[offset] == after[offset]) { ++offset; continue; }
            const auto start = offset;
            while (offset < length && before[offset] != after[offset]) ++offset;
            const auto runLength = offset - start;
            if (command.cost > m_maximumBytes || sizeof(Range) > m_maximumBytes - command.cost
                || runLength > (m_maximumBytes - command.cost - sizeof(Range)) / 2)
            {
                // The caller already owns the edited buffer. Drop stale history
                // across an unrecordable edit, without modifying that buffer.
                reset();
                return RecordResult::CapacityExceeded;
            }
            Range range;
            range.offset = start;
            range.before.assign(before + start, before + offset);
            range.after.assign(after + start, after + offset);
            payloadCapacity += range.before.capacity() + range.after.capacity();
            command.ranges.push_back(std::move(range));
            command.cost = sizeof(Command)
                + command.ranges.capacity() * sizeof(Range) + payloadCapacity;
            if (command.cost > m_maximumBytes)
            {
                reset();
                return RecordResult::CapacityExceeded;
            }
        }
        if (command.ranges.empty()) return RecordResult::Unchanged;
        if (!m_maximumCommands)
        {
            reset();
            return RecordResult::CapacityExceeded;
        }
        while (m_commands.size() > m_cursor)
        {
            m_memoryUsage -= m_commands.back().cost;
            m_commands.pop_back();
        }
        while (!m_commands.empty()
            && (m_commands.size() >= m_maximumCommands
                || command.cost > m_maximumBytes - m_memoryUsage))
        {
            m_memoryUsage -= m_commands.front().cost;
            m_commands.pop_front();
            --m_cursor;
        }
        m_memoryUsage += command.cost;
        m_commands.push_back(std::move(command));
        m_cursor = m_commands.size();
        return RecordResult::Recorded;
    }

    std::optional<MemoryEditHistory::Bytes> MemoryEditHistory::undo(const Bytes& current)
    {
        return undo(current.data(), current.size());
    }

    std::optional<MemoryEditHistory::Bytes> MemoryEditHistory::redo(const Bytes& current)
    {
        return redo(current.data(), current.size());
    }

    std::optional<MemoryEditHistory::Bytes> MemoryEditHistory::undo(const std::uint8_t* current, std::size_t length)
    {
        return apply(current, length, false);
    }

    std::optional<MemoryEditHistory::Bytes> MemoryEditHistory::redo(const std::uint8_t* current, std::size_t length)
    {
        return apply(current, length, true);
    }

    std::optional<MemoryEditHistory::Bytes> MemoryEditHistory::apply(
        const std::uint8_t* current, std::size_t length, bool forward)
    {
        if ((forward ? !canRedo() : !canUndo()) || (length && !current)) return std::nullopt;
        const auto& command = m_commands[forward ? m_cursor : m_cursor - 1];
        if (command.length != length) return std::nullopt;
        // Verify every source range before changing either output or the cursor.
        for (const auto& range : command.ranges)
        {
            const auto& expected = forward ? range.before : range.after;
            if (!std::equal(expected.begin(), expected.end(), current + range.offset)) return std::nullopt;
        }
        Bytes restored(current, current + length);
        for (const auto& range : command.ranges)
        {
            const auto& replacement = forward ? range.after : range.before;
            std::copy(replacement.begin(), replacement.end(), restored.begin() + range.offset);
        }
        if (forward) ++m_cursor;
        else --m_cursor;
        return restored;
    }

    bool MemoryEditHistory::canUndo() const { return m_cursor != 0; }
    bool MemoryEditHistory::canRedo() const { return m_cursor < m_commands.size(); }
    std::size_t MemoryEditHistory::undoCount() const { return m_cursor; }
    std::size_t MemoryEditHistory::redoCount() const { return m_commands.size() - m_cursor; }
    std::size_t MemoryEditHistory::memoryUsage() const { return m_memoryUsage; }
}
