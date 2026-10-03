#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ks::evidence::syscall
{
    struct Key
    {
        std::uint64_t timestamp = 0;
        std::uint16_t cpu = 0;

        bool operator==(const Key& other) const noexcept
        {
            return timestamp == other.timestamp && cpu == other.cpu;
        }
    };

    enum class CorrelationState
    {
        MissingStack,
        Matched,
        Ambiguous,
        CapacityEvicted
    };

    // Caller owns synchronization and one instance per capture generation.
    // All timestamps in Key must use the same raw QPC clock. nowMs must be
    // monotonic. No nearest-time or live-thread-identity fallback is admitted.
    template<class Row>
    class Correlator
    {
    public:
        Correlator() = default;
        Correlator(const Correlator&) = delete;
        Correlator& operator=(const Correlator&) = delete;
        Correlator(Correlator&&) = delete;
        Correlator& operator=(Correlator&&) = delete;

        static constexpr std::uint64_t kWindowMs = 250;
        static constexpr std::uint64_t kRetiredWindowMs = 1000;
        static constexpr std::size_t kEventCapacity = 2048;
        static constexpr std::size_t kEarlyStackCapacity = 512;
        static constexpr std::size_t kRetiredCapacity = 2048;
        static constexpr std::size_t kMaxFramesPerStack = 192;
        static constexpr std::size_t kMaxStackSegments = 2;
        static constexpr std::uint32_t kUnknownIdentity =
            (std::numeric_limits<std::uint32_t>::max)();

        struct Output
        {
            Row row;
            std::uint32_t pid = kUnknownIdentity;
            std::uint32_t tid = kUnknownIdentity;
            std::vector<std::uint64_t> frames;
            CorrelationState state = CorrelationState::MissingStack;
        };

        std::vector<Output> AddEvent(
            Key key, std::uint32_t pid, std::uint32_t tid, Row row,
            std::uint64_t nowMs)
        {
            auto output = Expire(nowMs);
            while (events_.size() >= kEventCapacity)
            {
                PublishFront(nowMs, true, output);
            }
            auto& bucket = buckets_[key];
            if (bucket.eventCount != 0 || retired_.find(key) != retired_.end())
            {
                bucket.ambiguous = true;
            }
            if (bucket.early)
            {
                earlyStacks_.erase(bucket.earlyPosition);
                bucket.early = false;
            }
            ++bucket.eventCount;
            events_.push_back(Event{ key, pid, tid, std::move(row), nowMs });
            return output;
        }

        std::vector<Output> AddStack(
            Key key, std::uint32_t pid, std::uint32_t tid,
            std::vector<std::uint64_t> frames, std::uint64_t nowMs)
        {
            auto output = Expire(nowMs);
            if (retired_.find(key) != retired_.end())
            {
                return output;
            }
            auto found = buckets_.find(key);
            if (found == buckets_.end())
            {
                while (earlyStacks_.size() >= kEarlyStackCapacity)
                {
                    RetireEarlyFront(nowMs);
                }
                found = buckets_.emplace(key, Bucket{}).first;
                earlyStacks_.push_back(Ticket{ key, nowMs });
                found->second.early = true;
                found->second.earlyPosition = std::prev(earlyStacks_.end());
            }
            Bucket& bucket = found->second;
            if (pid == kUnknownIdentity || tid == kUnknownIdentity || tid == 0
                || frames.empty() || frames.size() > kMaxFramesPerStack)
            {
                bucket.ambiguous = true;
                return output;
            }
            if (bucket.hasStack && (bucket.stackPid != pid || bucket.stackTid != tid))
            {
                bucket.ambiguous = true;
                return output;
            }
            bucket.hasStack = true;
            bucket.stackPid = pid;
            bucket.stackTid = tid;
            for (const auto& segment : bucket.segments)
            {
                if (segment == frames)
                {
                    return output; // Duplicate delivery must not duplicate frames.
                }
            }
            const auto domain = Native64Domain(frames);
            if (domain == StackDomain::Invalid)
            {
                bucket.ambiguous = true;
                return output;
            }
            if (!bucket.segments.empty()
                && (domain == StackDomain::Mixed || bucket.domain == StackDomain::Mixed
                    || domain == bucket.domain))
            {
                // Two different user (or kernel) stacks at an identical key
                // cannot be distinguished from a timestamp collision. Only
                // complementary native x64 kernel/user chunks may be joined.
                bucket.ambiguous = true;
                return output;
            }
            if (bucket.segments.size() >= kMaxStackSegments)
            {
                bucket.ambiguous = true;
                return output;
            }
            // Kernel and user stack chunks can arrive in either order. Preserve
            // each chunk's frame order; the consumer determines address domains.
            bucket.domain = bucket.segments.empty() ? domain : StackDomain::Mixed;
            bucket.segments.push_back(std::move(frames));
            return output;
        }

        std::vector<Output> Expire(std::uint64_t nowMs, bool force = false)
        {
            ExpireRetired(nowMs);
            std::vector<Output> output;
            // Even a matched event waits for the full window, so a second event
            // with the same key cannot silently steal the first event's stack.
            while (!events_.empty()
                && (force || Elapsed(nowMs, events_.front().receivedMs, kWindowMs)))
            {
                PublishFront(nowMs, false, output);
            }
            while (!earlyStacks_.empty()
                && (force || Elapsed(nowMs, earlyStacks_.front().receivedMs, kWindowMs)))
            {
                RetireEarlyFront(nowMs);
            }
            return output;
        }

        void Reset()
        {
            events_.clear();
            earlyStacks_.clear();
            buckets_.clear();
            retired_.clear();
            retiredOrder_.clear();
        }

        std::size_t PendingEventCount() const noexcept { return events_.size(); }
        std::size_t PendingStackCount() const noexcept { return earlyStacks_.size(); }
        std::size_t RetiredKeyCount() const noexcept { return retired_.size(); }

    private:
        enum class StackDomain { Invalid, User, Kernel, Mixed };

        static StackDomain Native64Domain(const std::vector<std::uint64_t>& frames) noexcept
        {
            bool user = false;
            bool kernel = false;
            for (const auto frame : frames)
            {
                if (frame > 0 && frame <= 0x00007fffffffffffULL)
                {
                    user = true;
                }
                else if (frame >= 0xffff800000000000ULL)
                {
                    kernel = true;
                }
                else
                {
                    return StackDomain::Invalid;
                }
            }
            return user && kernel ? StackDomain::Mixed
                : user ? StackDomain::User : kernel ? StackDomain::Kernel : StackDomain::Invalid;
        }

        struct KeyHash
        {
            std::size_t operator()(const Key& key) const noexcept
            {
                std::uint64_t mixed = key.timestamp ^ (std::uint64_t(key.cpu) << 48);
                mixed ^= mixed >> 33;
                mixed *= 0xff51afd7ed558ccdULL;
                mixed ^= mixed >> 33;
                return static_cast<std::size_t>(mixed);
            }
        };

        struct Ticket
        {
            Key key;
            std::uint64_t receivedMs;
        };

        struct Event
        {
            Key key;
            std::uint32_t pid;
            std::uint32_t tid;
            Row row;
            std::uint64_t receivedMs;
        };

        struct Bucket
        {
            std::size_t eventCount = 0;
            bool ambiguous = false;
            bool hasStack = false;
            bool early = false;
            std::uint32_t stackPid = kUnknownIdentity;
            std::uint32_t stackTid = kUnknownIdentity;
            std::vector<std::vector<std::uint64_t>> segments;
            StackDomain domain = StackDomain::Invalid;
            typename std::list<Ticket>::iterator earlyPosition;
        };

        static bool Elapsed(std::uint64_t now, std::uint64_t before,
            std::uint64_t interval) noexcept
        {
            return now >= before && now - before >= interval;
        }

        static bool IdentityMatches(std::uint32_t header, std::uint32_t payload) noexcept
        {
            return header == kUnknownIdentity || header == payload;
        }

        void PublishFront(std::uint64_t nowMs, bool capacity,
            std::vector<Output>& output)
        {
            Event event = std::move(events_.front());
            events_.pop_front();
            auto found = buckets_.find(event.key);
            Bucket& bucket = found->second;
            Output result{ std::move(event.row), event.pid, event.tid, {},
                CorrelationState::MissingStack };
            if (capacity)
            {
                result.state = CorrelationState::CapacityEvicted;
                // Any sibling must also remain unknown after forced publication.
                bucket.ambiguous = true;
            }
            else if (bucket.ambiguous || (bucket.hasStack
                && (!IdentityMatches(event.pid, bucket.stackPid)
                    || !IdentityMatches(event.tid, bucket.stackTid))))
            {
                result.state = CorrelationState::Ambiguous;
            }
            else if (bucket.hasStack)
            {
                result.state = CorrelationState::Matched;
                result.pid = bucket.stackPid;
                result.tid = bucket.stackTid;
                for (const auto& segment : bucket.segments)
                {
                    result.frames.insert(result.frames.end(), segment.begin(), segment.end());
                }
            }
            output.push_back(std::move(result));
            if (--bucket.eventCount == 0)
            {
                buckets_.erase(found);
                Retire(event.key, nowMs);
            }
        }

        void RetireEarlyFront(std::uint64_t nowMs)
        {
            const Key key = earlyStacks_.front().key;
            earlyStacks_.pop_front();
            buckets_.erase(key);
            Retire(key, nowMs);
        }

        void Retire(Key key, std::uint64_t nowMs)
        {
            // Existing tickets retain their initial deadline; no unbounded list
            // growth or attacker-controlled extension of a key's lifetime.
            if (retired_.find(key) != retired_.end())
            {
                return;
            }
            while (retiredOrder_.size() >= kRetiredCapacity)
            {
                retired_.erase(retiredOrder_.front().key);
                retiredOrder_.pop_front();
            }
            retired_.emplace(key, nowMs);
            retiredOrder_.push_back(Ticket{ key, nowMs });
        }

        void ExpireRetired(std::uint64_t nowMs)
        {
            while (!retiredOrder_.empty()
                && Elapsed(nowMs, retiredOrder_.front().receivedMs, kRetiredWindowMs))
            {
                retired_.erase(retiredOrder_.front().key);
                retiredOrder_.pop_front();
            }
        }

        std::list<Event> events_;
        std::list<Ticket> earlyStacks_;
        std::unordered_map<Key, Bucket, KeyHash> buckets_;
        std::unordered_map<Key, std::uint64_t, KeyHash> retired_;
        std::list<Ticket> retiredOrder_;
    };
}
