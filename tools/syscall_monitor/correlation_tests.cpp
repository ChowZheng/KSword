#include "../../shared/evidence/SyscallCorrelation.h"

#include <cassert>
#include <iostream>

using namespace ks::evidence::syscall;
using C = Correlator<int>;

int main()
{
    C c;
    const Key first{ 10000, 1 };
    const auto unknown = C::kUnknownIdentity;

    // Unknown syscall headers acquire identity only from an exact stack payload.
    assert(c.AddEvent(first, unknown, unknown, 7, 0).empty());
    assert(c.AddStack(first, 42, 99, { 0xfffff80100001000ULL }, 5).empty());
    assert(c.AddStack(first, 42, 99, { 0x7ffb00001234ULL, 0x12345678ULL }, 20).empty());
    assert(c.Expire(249).empty());
    auto output = c.Expire(250);
    assert(output.size() == 1 && output[0].row == 7);
    assert(output[0].state == CorrelationState::Matched);
    assert(output[0].pid == 42 && output[0].tid == 99 && output[0].frames.size() == 3);

    // Late stacks for a published key cannot become an early stack for a new row.
    assert(c.AddStack(first, 77, 88, { 0x2222 }, 251).empty());
    assert(c.PendingStackCount() == 0);
    assert(c.AddEvent(first, unknown, unknown, 8, 252).empty());
    output = c.Expire(502);
    assert(output.size() == 1 && output[0].state == CorrelationState::Ambiguous);
    assert(output[0].pid == unknown && output[0].frames.empty());

    // Same-domain conflicting chunks are not a kernel/user split stack.
    c.Reset();
    c.AddEvent(first, 42, 99, 21, 0);
    c.AddStack(first, 42, 99, { 0x1111 }, 1);
    c.AddStack(first, 42, 99, { 0x2222 }, 2);
    output = c.Expire(250);
    assert(output.size() == 1 && output[0].state == CorrelationState::Ambiguous);
    assert(output[0].frames.empty());

    // Out-of-order stacks and user-before-kernel split delivery preserve identity.
    c.Reset();
    assert(c.AddStack(first, 42, 99, { 0x7ffb00001234ULL }, 0).empty());
    assert(c.AddStack(first, 42, 99, { 0xfffff80100001000ULL }, 1).empty());
    assert(c.AddStack(first, 42, 99, { 0x7ffb00001234ULL }, 2).empty());
    assert(c.AddEvent(first, 42, 99, 9, 3).empty());
    output = c.Expire(253);
    assert(output.size() == 1 && output[0].state == CorrelationState::Matched);
    assert(output[0].frames.size() == 2 && output[0].frames[0] == 0x7ffb00001234ULL);

    // Timestamp equality alone cannot cross CPUs, payload identities, or headers.
    c.Reset();
    c.AddEvent(first, 42, 99, 10, 0);
    c.AddStack(Key{ first.timestamp, 2 }, 42, 99, { 0x4444 }, 1);
    output = c.Expire(250);
    assert(output.size() == 1 && output[0].state == CorrelationState::MissingStack);
    c.Reset();
    c.AddEvent(first, 42, 99, 11, 0);
    c.AddStack(first, 43, 99, { 0x4444 }, 1);
    output = c.Expire(250);
    assert(output.size() == 1 && output[0].state == CorrelationState::Ambiguous);
    assert(output[0].pid == 42 && output[0].frames.empty());
    c.Reset();
    c.AddEvent(first, unknown, unknown, 12, 0);
    c.AddStack(first, 42, 99, { 0x4444 }, 1);
    c.AddStack(first, 43, 99, { 0x5555 }, 2);
    output = c.Expire(250);
    assert(output.size() == 1 && output[0].state == CorrelationState::Ambiguous);
    assert(output[0].pid == unknown && output[0].frames.empty());

    // A second syscall at the same key invalidates even an already matched row.
    c.Reset();
    c.AddEvent(first, unknown, unknown, 13, 0);
    c.AddStack(first, 42, 99, { 0x4444 }, 1);
    c.AddEvent(first, unknown, unknown, 14, 249);
    output = c.Expire(250);
    assert(output.size() == 1 && output[0].row == 13);
    assert(output[0].state == CorrelationState::Ambiguous);
    output = c.Expire(499);
    assert(output.size() == 1 && output[0].row == 14);
    assert(output[0].state == CorrelationState::Ambiguous);

    // An expired early stack is never reused, even if a row arrives shortly after.
    c.Reset();
    c.AddStack(first, 42, 99, { 0x4444 }, 0);
    c.AddEvent(first, unknown, unknown, 15, 251);
    output = c.Expire(501);
    assert(output.size() == 1 && output[0].state == CorrelationState::Ambiguous);
    assert(output[0].frames.empty());

    // Saturation rejects the new row without destroying admitted evidence.
    c.Reset();
    for (std::size_t i = 0; i < C::kEventCapacity; ++i)
    {
        assert(c.AddEvent(Key{ 100000 + i, 1 }, unknown, unknown,
            static_cast<int>(i), 0).empty());
    }
    c.AddStack(Key{ 100000, 1 }, 42, 99, { 0x4444 }, 1);
    output = c.AddEvent(Key{ 999999, 1 }, unknown, unknown, 99, 2);
    assert(output.size() == 1 && output[0].state == CorrelationState::CapacityEvicted);
    assert(output[0].row == 99 && output[0].pid == unknown);
    assert(output[0].frames.empty() && c.PendingEventCount() == C::kEventCapacity);
    c.AddStack(Key{ 999999, 1 }, 42, 99, { 0x4444 }, 2);
    assert(c.PendingStackCount() == 0); // Rejected row's late stack is retired.
    output = c.Expire(3, true);
    assert(output.size() == C::kEventCapacity && c.PendingEventCount() == 0);
    assert(output.front().row == 0 && output.front().state == CorrelationState::Matched);
    assert(output.front().pid == 42 && output.front().frames.size() == 1);
    assert(c.RetiredKeyCount() <= C::kRetiredCapacity);

    // A duplicate rejected at saturation must still invalidate the admitted key.
    c.Reset();
    for (std::size_t i = 0; i < C::kEventCapacity; ++i)
    {
        c.AddEvent(Key{ 100000 + i, 1 }, unknown, unknown, static_cast<int>(i), 0);
    }
    c.AddStack(Key{ 100000, 1 }, 42, 99, { 0x4444 }, 1);
    output = c.AddEvent(Key{ 100000, 1 }, unknown, unknown, 99, 2);
    assert(output.size() == 1 && output[0].state == CorrelationState::CapacityEvicted);
    output = c.Expire(250);
    assert(output.size() == C::kEventCapacity);
    assert(output.front().state == CorrelationState::Ambiguous && output.front().frames.empty());

    // Rejecting a row with an early stack also removes that stack's ticket.
    c.Reset();
    for (std::size_t i = 0; i < C::kEventCapacity; ++i)
    {
        c.AddEvent(Key{ 100000 + i, 1 }, unknown, unknown, static_cast<int>(i), 0);
    }
    c.AddStack(Key{ 999999, 1 }, 42, 99, { 0x4444 }, 1);
    assert(c.PendingStackCount() == 1);
    c.AddEvent(Key{ 999999, 1 }, unknown, unknown, 99, 2);
    assert(c.PendingStackCount() == 0);
    c.Expire(250);

    // Sustained 100k events/s previously produced zero matches despite correct
    // stacks for every entry. Admitted samples must survive the full 250 ms.
    c.Reset();
    std::size_t matched = 0, dropped = 0;
    const auto count = [&](const std::vector<C::Output>& rows) {
        for (const auto& result : rows)
        {
            if (result.state == CorrelationState::Matched)
            {
                assert(result.pid == 42 && result.tid == 99 && result.frames.size() == 2);
                ++matched;
            }
            else
            {
                assert(result.state == CorrelationState::CapacityEvicted);
                assert(result.pid == unknown && result.frames.empty());
                ++dropped;
            }
        }
    };
    for (std::uint64_t i = 0; i < 100000; ++i)
    {
        const Key key{ 1000000 + i, 1 };
        count(c.AddEvent(key, unknown, unknown, static_cast<int>(i), i / 100));
        count(c.AddStack(key, 42, 99, { 0xfffff80100001000ULL, 0x7ffb00001234ULL }, i / 100));
        assert(c.PendingEventCount() <= C::kEventCapacity);
        assert(c.PendingStackCount() <= C::kEarlyStackCapacity);
        assert(c.RetiredKeyCount() <= C::kRetiredCapacity);
    }
    assert(matched > 0 && dropped > 0);
    assert(matched + dropped + c.PendingEventCount() == 100000);
    std::cout << "SUSTAINED_LOAD_MATCHED=" << matched << " DROPPED=" << dropped << '\n';

    c.Reset();
    for (std::size_t i = 0; i <= C::kEarlyStackCapacity; ++i)
    {
        c.AddStack(Key{ 100000 + i, 1 }, 42, 99, { 0x4444 }, 0);
    }
    assert(c.PendingStackCount() == C::kEarlyStackCapacity);
    c.AddEvent(Key{ 100000, 1 }, unknown, unknown, 16, 1);
    output = c.Expire(251);
    assert(output.size() == 1 && output[0].state == CorrelationState::Ambiguous);

    // Malformed identities, frame floods and excess nonduplicate chunks are gaps.
    c.Reset();
    c.AddEvent(first, unknown, unknown, 17, 0);
    c.AddStack(first, unknown, 99, { 0x4444 }, 1);
    output = c.Expire(250);
    assert(output.size() == 1 && output[0].state == CorrelationState::Ambiguous);
    c.Reset();
    c.AddEvent(first, 42, 99, 18, 0);
    c.AddStack(first, 42, 99, std::vector<std::uint64_t>(C::kMaxFramesPerStack + 1, 1), 1);
    output = c.Expire(250);
    assert(output.size() == 1 && output[0].state == CorrelationState::Ambiguous);
    c.Reset();
    c.AddEvent(first, 42, 99, 19, 0);
    c.AddStack(first, 42, 99, { 0x1111 }, 1);
    c.AddStack(first, 42, 99, { 0x2222 }, 2);
    c.AddStack(first, 42, 99, { 0x3333 }, 3);
    output = c.Expire(250);
    assert(output.size() == 1 && output[0].state == CorrelationState::Ambiguous);

    // Pause/stop reset discards pending evidence across capture generations.
    c.Reset();
    c.AddStack(first, 42, 99, { 0x4444 }, 0);
    c.Reset();
    c.AddEvent(first, unknown, unknown, 20, 1);
    output = c.Expire(251);
    assert(output.size() == 1 && output[0].state == CorrelationState::MissingStack);
    assert(c.Expire(2000).empty() && c.RetiredKeyCount() == 0);

    std::cout << "Syscall correlation regressions passed\n";
}
