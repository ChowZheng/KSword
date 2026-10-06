#pragma once

#include "MemoryAddressBook.h"
#include "MemoryTargetSession.h"
#include "../PointerChain.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace ksword::memwb {

struct PointerChainBinding {
    AddressEntry entry;
    MemoryTargetSession session;
    pointer_chain::Result resolved;
};

struct GuardResult {
    enum class Failure { None, InvalidBinding, ChangedDefinition, ChangedSession, ChangedPath, Capacity };
    bool ok = false;
    Failure failure = Failure::InvalidBinding;
};

// Per-view, UI-thread-owned write provenance. Definitions and successful paths
// are immutable witnesses. State changes during callbacks fail closed.
class PointerChainBindings {
public:
    static constexpr std::size_t MaxSpans = 256;
    using Lookup = std::function<std::optional<AddressEntry>(std::uint64_t id)>;
    using Resolver = std::function<pointer_chain::Result(const AddressEntry&, const MemoryTargetSession&)>;

    // Invalid replacement leaves the previous active binding intact.
    bool Bind(PointerChainBinding binding);
    void ClearActive() noexcept;
    void Clear() noexcept;
    std::size_t SpanCount() const noexcept;

    // An active chain is always validated. Normal writes additionally record
    // the attempted span before the underlying write; failures may therefore
    // retain a conservative witness. Historical writes never record and check
    // every overlapping retained witness. Different chains covering the same
    // bytes must ALL still match, even if only one actually wrote those bytes.
    // Navigation clears only the active binding; target changes must Clear().
    // Notes/value types do not change a definition. Empty writes do nothing.
    GuardResult Validate(const MemoryTargetSession& session, std::uint64_t address,
                         std::uint64_t length, bool historyReplay,
                         const Lookup& lookup, const Resolver& resolver);

private:
    struct Span {
        std::uint64_t first = 0;
        std::uint64_t last = 0; // Inclusive; range arithmetic is checked first.
        PointerChainBinding witness;
    };
    static bool ValidBinding(const PointerChainBinding& binding);
    static bool SameDefinition(const AddressEntry& left, const AddressEntry& right);
    static bool SamePath(const pointer_chain::Result& left, const pointer_chain::Result& right);
    static bool SameWitness(const PointerChainBinding& left, const PointerChainBinding& right);
    static GuardResult ValidateWitness(const PointerChainBinding& witness,
                                      const MemoryTargetSession& session,
                                      const Lookup& lookup, const Resolver& resolver);
    GuardResult RecordSpan(std::uint64_t first, std::uint64_t last);
    std::optional<PointerChainBinding> active_;
    std::vector<Span> spans_;
    std::uint64_t revision_ = 0;
};

} // namespace ksword::memwb
