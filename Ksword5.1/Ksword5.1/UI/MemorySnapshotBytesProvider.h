#pragma once

#include "MemoryWorkbench/WorkbenchDisasmView.h"

namespace ks::ui
{
    // Static, read-only adapter for the snapshot editor. It never fetches target
    // memory or accepts edits; the owner supplies its existing cache and history.
    class MemorySnapshotBytesProvider final : public IWorkbenchBytesProvider
    {
    public:
        void setSnapshot(std::uint64_t base, const QByteArray& bytes,
            const QByteArray& baseline, const QByteArray& previous,
            int addressBits, bool highlightChanges);
        void clear();
        WorkbenchByteWindow FetchWindow(std::uint64_t address, std::uint64_t length) const override;
        int AddressBits() const override { return m_addressBits; }
        bool HasPreviousRead() const override;

    private:
        std::uint64_t m_base = 0;
        QByteArray m_bytes;
        QByteArray m_baseline;
        QByteArray m_previous;
        int m_addressBits = 64;
        bool m_highlightChanges = true;
    };
}
