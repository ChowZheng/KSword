#include "../ControlProtocol.h"
#include <cstdio>

bool controlProtocolTests()
{
    using namespace ksword::titan::control;
    int failures = 0;
    const auto check = [&failures](bool okay, const char* name) { if (!okay) { std::printf("FAIL: %s\n", name); ++failures; } };
    const auto normal = defaults(); auto stealth = normal; stealth.mode = 1; stealth.shadowMemoryWrites = 1; stealth.allowFallback = 0;
    check(valid(normal) && normal.mode == 0 && normal.shadowMemoryWrites == 0 && normal.allowFallback == 1 && normal.logFallback == 1 && normal.maxShadowPages == 32, "documented normal defaults");
    Request parsed;
    check(parseRequest("session 1 0\n", parsed) && !parsed.extended && same(parsed.options, normal), "legacy request defaults");
    check(parseRequest(request("session", 9, 1, stealth), parsed) && parsed.extended && parsed.revision == 9 && parsed.selected == 1 && same(parsed.options, stealth), "v2 request round trip");
    for (const auto packet : {"session -1 0", "session 18446744073709551616 0", "session 1 2", "session 1 0 v3 0 0 1 1 1 32", "session 1 0 v2 2 0 1 1 1 32", "session 1 0 v2 0 2 1 1 1 32", "session 1 0 v2 0 0 2 1 1 32", "session 1 0 v2 0 0 1 1 1 0", "session 1 0 v2 0 0 1 1 1 33", "session 1 0 v2 0 0 1 1 1 32 ignored"})
        check(!parseRequest(packet, parsed), "malformed packet rejected");
    auto invalid = normal; invalid.logFallback = 0; check(!valid(invalid), "critical fallback logging cannot be disabled");
    invalid = normal; invalid.reserved[2] = 1; check(!valid(invalid), "reserved field rejected");
    invalid = normal; invalid.version = 2; check(!valid(invalid), "unknown ABI version rejected");
    Ack confirmed; KSWORD_DEBUGGER_BACKEND_STATUS state{}; state.useHvm = 0;
    KSWORD_DEBUGGER_POLICY_STATUS actual{normal, 0, 0, 2, 0, 3, 50};
    check(parseAck(ack("session", 9, 170, state, actual), confirmed) && confirmed.extended && confirmed.error == 170 &&
        confirmed.selected == 0 && same(confirmed.policy.options, normal) && confirmed.policy.activeBreakpoints == 2 &&
        confirmed.policy.fallbackCount == 3 && confirmed.policy.lastFallbackError == 50, "failed request ACK retains actual policy");
    check(parseAck("session 1 0 0 0 0 0", confirmed) && !confirmed.extended, "legacy ACK accepted without invented options support");
    check(!parseAck("session 1 0 0 0 0 0 v2 0 0 1 1 1 32 3 0 0 1 0 0", confirmed), "unknown actual path rejected");
    KSWORD_DEBUGGER_OPTIONS restored{};
    check(preferences(preferences(stealth), restored) && same(restored, stealth), "preferences round trip excludes HVM activation");
    check(!preferences("ksword-x96dbg-options/2 0 0 1 1 1 32", restored), "unknown preference version rejected");
    check(changeAllowed(false, stealth, false, normal, false, false), "idle unbound mode change allowed");
    check(!changeAllowed(false, stealth, false, normal, true, false), "running target mode change refused");
    check(!changeAllowed(false, stealth, false, normal, false, true), "active binding mode change refused");
    check(!changeAllowed(true, normal, false, normal, false, true), "HVM change refused with existing native binding");
    check(changeAllowed(true, normal, true, normal, true, true), "identical policy remains idempotent with bindings");
    std::printf("%s: headless control v1/v2, invalid options, actual-state failure ACK, preferences and policy guards\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0;
}
