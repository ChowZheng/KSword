#pragma once
#include "../DebuggerBackend/KswordDebuggerApi.h"
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

// Text transport shared by the Tab launcher and the Titan adapter. Its legacy
// prefix stays readable by older Tabs; options always describe confirmed state.
namespace ksword::titan::control
{
    inline KSWORD_DEBUGGER_OPTIONS defaults()
    { return {KSWORD_DEBUGGER_OPTIONS_VERSION, sizeof(KSWORD_DEBUGGER_OPTIONS),
        KSWORD_DEBUGGER_MODE_NORMAL, 0, 1, 1, 1, 1, 32, {0, 0, 0}}; }
    inline bool valid(const KSWORD_DEBUGGER_OPTIONS& value)
    {
        return value.version == KSWORD_DEBUGGER_OPTIONS_VERSION && value.size == sizeof(value) &&
            value.mode <= KSWORD_DEBUGGER_MODE_STEALTH && value.shadowMemoryWrites <= 1 &&
            value.allowFallback <= 1 && value.logFallback == 1 && value.nativeContextFallback <= 1 &&
            value.nativeSuspendFallback <= 1 && value.maxShadowPages >= 1 && value.maxShadowPages <= 32 &&
            value.reserved[0] == 0 && value.reserved[1] == 0 && value.reserved[2] == 0;
    }
    inline bool same(const KSWORD_DEBUGGER_OPTIONS& a, const KSWORD_DEBUGGER_OPTIONS& b)
    {
        return a.mode == b.mode && a.shadowMemoryWrites == b.shadowMemoryWrites &&
            a.allowFallback == b.allowFallback && a.logFallback == b.logFallback &&
            a.nativeContextFallback == b.nativeContextFallback && a.nativeSuspendFallback == b.nativeSuspendFallback &&
            a.maxShadowPages == b.maxShadowPages;
    }
    inline bool changeAllowed(bool selected, const KSWORD_DEBUGGER_OPTIONS& requested,
        bool currentSelected, const KSWORD_DEBUGGER_OPTIONS& current, bool running, bool bindings)
    {
        return (selected == currentSelected && same(requested, current)) || (!running && !bindings);
    }
    inline bool number(std::istream& stream, std::uint64_t& value)
    {
        std::string token; if (!(stream >> token) || token.empty()) return false;
        value = 0;
        for (const unsigned char c : token)
        {
            if (c < '0' || c > '9' || value > ((std::numeric_limits<std::uint64_t>::max)() - (c - '0')) / 10) return false;
            value = value * 10 + c - '0';
        }
        return true;
    }
    inline bool dword(std::istream& stream, std::uint32_t& value)
    { std::uint64_t wide = 0; if (!number(stream, wide) || wide > UINT32_MAX) return false; value = static_cast<std::uint32_t>(wide); return true; }
    inline bool end(std::istream& stream) { stream >> std::ws; return stream.eof(); }
    inline bool sessionValid(const std::string& session)
    {
        if (session.empty() || session.size() > 128) return false;
        for (const unsigned char c : session) if (c < 0x21 || c > 0x7e) return false;
        return true;
    }
    inline bool readOptions(std::istream& stream, KSWORD_DEBUGGER_OPTIONS& options)
    {
        options = defaults();
        return dword(stream, options.mode) && dword(stream, options.shadowMemoryWrites) &&
            dword(stream, options.allowFallback) && dword(stream, options.nativeContextFallback) &&
            dword(stream, options.nativeSuspendFallback) && dword(stream, options.maxShadowPages) && valid(options);
    }
    inline void writeOptions(std::ostream& stream, const KSWORD_DEBUGGER_OPTIONS& options)
    {
        stream << options.mode << ' ' << options.shadowMemoryWrites << ' ' << options.allowFallback << ' '
            << options.nativeContextFallback << ' ' << options.nativeSuspendFallback << ' ' << options.maxShadowPages;
    }
    struct Request
    {
        std::string session;
        std::uint64_t revision = 0;
        std::uint32_t selected = 0;
        bool extended = false;
        KSWORD_DEBUGGER_OPTIONS options = defaults();
    };
    inline bool parseRequest(const std::string& packet, Request& result)
    {
        std::istringstream stream(packet); Request candidate;
        if (!(stream >> candidate.session) || !sessionValid(candidate.session) || !number(stream, candidate.revision) ||
            !dword(stream, candidate.selected) || candidate.selected > 1) return false;
        if (!end(stream))
        {
            std::string version;
            if (!(stream >> version) || version != "v2" || !readOptions(stream, candidate.options) || !end(stream)) return false;
            candidate.extended = true;
        }
        result = candidate; return true;
    }
    inline std::string request(const std::string& session, std::uint64_t revision,
        std::uint32_t selected, const KSWORD_DEBUGGER_OPTIONS& options)
    {
        std::ostringstream stream; stream << session << ' ' << revision << ' ' << selected << " v2 ";
        writeOptions(stream, options); stream << '\n'; return stream.str();
    }
    struct Ack
    {
        std::string session;
        std::uint64_t revision = 0;
        std::uint32_t error = 0, selected = 0, driver = 0, resident = 0, protocol = 0;
        bool extended = false;
        KSWORD_DEBUGGER_POLICY_STATUS policy{defaults(), 0, 0, 0, 0, 0, 0};
    };
    inline bool parseAck(const std::string& packet, Ack& result)
    {
        std::istringstream stream(packet); Ack candidate;
        if (!(stream >> candidate.session) || !sessionValid(candidate.session) || !number(stream, candidate.revision) ||
            !dword(stream, candidate.error) || !dword(stream, candidate.selected) || !dword(stream, candidate.driver) ||
            !dword(stream, candidate.resident) || !dword(stream, candidate.protocol) || candidate.selected > 1 ||
            candidate.driver > 1 || candidate.resident > 1 || candidate.protocol > 1) return false;
        if (!end(stream))
        {
            std::string version; auto& policy = candidate.policy;
            if (!(stream >> version) || version != "v2" || !readOptions(stream, policy.options) ||
                !dword(stream, policy.activePath) || !dword(stream, policy.activeBreakpoints) ||
                !dword(stream, policy.shadowWritePages) || !dword(stream, policy.canChangeOptions) ||
                !dword(stream, policy.fallbackCount) || !dword(stream, policy.lastFallbackError) ||
                policy.activePath > KSWORD_DEBUGGER_PATH_SHADOW || policy.canChangeOptions > 1 || !end(stream)) return false;
            candidate.extended = true;
        }
        result = candidate; return true;
    }
    inline std::string ack(const std::string& session, std::uint64_t revision, std::uint32_t error,
        const KSWORD_DEBUGGER_BACKEND_STATUS& state, const KSWORD_DEBUGGER_POLICY_STATUS& policy)
    {
        std::ostringstream stream;
        stream << session << ' ' << revision << ' ' << error << ' ' << state.useHvm << ' ' << state.driverReady << ' '
            << state.residentActive << ' ' << state.eptBreakpointProtocol << " v2 "; writeOptions(stream, policy.options);
        stream << ' ' << policy.activePath << ' ' << policy.activeBreakpoints << ' ' << policy.shadowWritePages << ' '
            << policy.canChangeOptions << ' ' << policy.fallbackCount << ' ' << policy.lastFallbackError << '\n';
        return stream.str();
    }
    inline bool preferences(const std::string& packet, KSWORD_DEBUGGER_OPTIONS& value)
    {
        std::istringstream stream(packet); std::string version; KSWORD_DEBUGGER_OPTIONS candidate{};
        if (!(stream >> version) || version != "ksword-x96dbg-options/1" || !readOptions(stream, candidate) || !end(stream)) return false;
        value = candidate; return true;
    }
    inline std::string preferences(const KSWORD_DEBUGGER_OPTIONS& value)
    { std::ostringstream stream; stream << "ksword-x96dbg-options/1 "; writeOptions(stream, value); stream << '\n'; return stream.str(); }
}
