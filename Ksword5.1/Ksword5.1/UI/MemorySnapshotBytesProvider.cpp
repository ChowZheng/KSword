#include "MemorySnapshotBytesProvider.h"

#include <algorithm>
#include <limits>

namespace ks::ui
{
    void MemorySnapshotBytesProvider::setSnapshot(std::uint64_t base, const QByteArray& bytes,
        const QByteArray& baseline, const QByteArray& previous, int addressBits, bool highlightChanges)
    {
        if (!bytes.isEmpty() && static_cast<std::uint64_t>(bytes.size() - 1)
            > std::numeric_limits<std::uint64_t>::max() - base)
        {
            clear();
            return;
        }
        m_base = base;
        m_bytes = bytes;
        m_baseline = baseline;
        m_previous = previous;
        m_addressBits = addressBits == 32 ? 32 : 64;
        m_highlightChanges = highlightChanges;
    }

    void MemorySnapshotBytesProvider::clear()
    {
        m_base = 0;
        m_bytes.clear();
        m_baseline.clear();
        m_previous.clear();
    }

    bool MemorySnapshotBytesProvider::HasPreviousRead() const
    {
        return !m_bytes.isEmpty() && m_previous.size() == m_bytes.size();
    }

    WorkbenchByteWindow MemorySnapshotBytesProvider::FetchWindow(
        std::uint64_t address, std::uint64_t length) const
    {
        WorkbenchByteWindow window;
        window.address = address;
        if (length == 0)
        {
            window.ok = true;
            return window;
        }
        if (address < m_base || address - m_base >= static_cast<std::uint64_t>(m_bytes.size()))
            return window;

        // Return only captured bytes, with a bounded allocation. No padded zero
        // tail is allowed to masquerade as evidence outside the snapshot.
        const auto offset = static_cast<qsizetype>(address - m_base);
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
            std::min<std::uint64_t>(length, 1024ULL * 1024ULL),
            static_cast<std::uint64_t>(m_bytes.size() - offset)));
        window.bytes.reserve(count);
        window.baselineBytes.reserve(count);
        window.previousBytes.reserve(count);
        window.changeKinds.reserve(count);
        window.validMask.assign(count, 1);
        const bool baselineValid = m_baseline.size() == m_bytes.size();
        const bool previousValid = HasPreviousRead();
        window.baselineValidMask.assign(count, baselineValid ? 1 : 0);
        window.previousValidMask.assign(count, previousValid ? 1 : 0);
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto at = offset + static_cast<qsizetype>(i);
            const auto byte = static_cast<std::uint8_t>(m_bytes.at(at));
            const auto baseline = baselineValid ? static_cast<std::uint8_t>(m_baseline.at(at)) : 0;
            const auto previous = previousValid ? static_cast<std::uint8_t>(m_previous.at(at)) : 0;
            window.bytes.push_back(byte);
            window.baselineBytes.push_back(static_cast<std::uint8_t>(baseline));
            window.previousBytes.push_back(static_cast<std::uint8_t>(previous));
            using Kind = ksword::memwb::ByteChangeKind;
            window.changeKinds.push_back(!m_highlightChanges ? Kind::Unchanged
                : baselineValid && byte != baseline ? Kind::Pending
                : baselineValid && previousValid && baseline != previous ? Kind::ExternalChange
                : Kind::Unchanged);
        }
        window.ok = true;
        return window;
    }
}
