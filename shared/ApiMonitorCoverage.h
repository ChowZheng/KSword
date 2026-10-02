#pragma once
#include "WinApiMonitorProtocol.h"
#include <vector>

namespace ks::winapi_monitor
{
    // One receiver per pipe. Only a complete, ordered snapshot can replace visible state.
    class CoverageSnapshotReceiver
    {
    public:
        CoverageSnapshotReceiver(std::uint32_t pid, std::uint64_t session) : m_pid(pid), m_session(session) {}
        bool consume(const ApiMonitorEventPacket& packet)
        {
            if (packet.pid != m_pid || packet.sessionIdentity != m_session || packet.version != kProtocolVersion
                || packet.size != sizeof(packet) || packet.snapshotRevision <= m_committedRevision) return false;
            const auto kind = static_cast<EventKind>(packet.eventKind);
            if (kind == EventKind::CoverageBegin)
            {
                if (packet.snapshotRevision < m_pendingRevision) return false;
                m_stale = true;
                m_pending.clear();
                m_receiving = packet.snapshotCount <= 65536 && packet.snapshotRevision >= m_pendingRevision;
                if (m_receiving)
                {
                    m_pendingRevision = packet.snapshotRevision;
                    m_expectedCount = packet.snapshotCount;
                    m_pending.reserve(m_expectedCount);
                }
                return false;
            }
            if (!m_receiving || packet.snapshotRevision != m_pendingRevision) return false;
            if (packet.snapshotCount != m_expectedCount) { interrupt(); return false; }
            if (kind == EventKind::CoverageItem)
            {
                if (packet.snapshotIndex != m_pending.size() || m_pending.size() >= m_expectedCount)
                { interrupt(); return false; }
                m_pending.push_back(packet);
                return false;
            }
            if (kind == EventKind::CoverageEnd && m_pending.size() == m_expectedCount
                && packet.snapshotIndex == m_expectedCount)
            {
                m_complete.swap(m_pending);
                m_committedRevision = m_pendingRevision;
                m_receiving = false;
                m_stale = false;
                return true;
            }
            interrupt();
            return false;
        }
        void interrupt() { m_receiving = false; m_pending.clear(); m_stale = true; }
        bool stale() const { return m_stale; }
        std::uint64_t revision() const { return m_committedRevision; }
        const std::vector<ApiMonitorEventPacket>& items() const { return m_complete; }
    private:
        std::uint32_t m_pid;
        std::uint64_t m_session;
        std::uint64_t m_pendingRevision = 0, m_committedRevision = 0;
        std::uint32_t m_expectedCount = 0;
        bool m_receiving = false, m_stale = true;
        std::vector<ApiMonitorEventPacket> m_pending, m_complete;
    };
}
