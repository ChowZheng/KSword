#pragma once
#include "ApiMonitorInjectionProtocol.h"
#include <memory>

namespace ks::winapi_monitor
{
    // Hosted by the x64 main program, independent of Qt and the event queue.
    class InjectionBroker {
    public:
        InjectionBroker();
        ~InjectionBroker();
        InjectionBroker(const InjectionBroker&) = delete;
        InjectionBroker& operator=(const InjectionBroker&) = delete;
        bool start(DWORD rootPid, const std::wstring& agentPath, const std::wstring& session,
            const std::wstring& rootStopPath, std::wstring* error);
        void stop();
        InjectionEndpoint endpoint() const;
    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
}
