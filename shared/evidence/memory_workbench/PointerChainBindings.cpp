#include "PointerChainBindings.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace ksword::memwb {
namespace {
GuardResult Success() { return {true, GuardResult::Failure::None}; }
GuardResult Failed(GuardResult::Failure failure) { return {false, failure}; }
bool Overlap(std::uint64_t aFirst, std::uint64_t aLast, std::uint64_t bFirst, std::uint64_t bLast) {
    return aFirst <= bLast && bFirst <= aLast;
}
bool Touch(std::uint64_t aFirst, std::uint64_t aLast, std::uint64_t bFirst, std::uint64_t bLast) {
    return Overlap(aFirst, aLast, bFirst, bLast)
        || (aLast < bFirst && bFirst - aLast == 1)
        || (bLast < aFirst && aFirst - bLast == 1);
}
} // namespace

bool PointerChainBindings::ValidBinding(const PointerChainBinding& binding) {
    const auto& entry = binding.entry;
    const auto& session = binding.session;
    const auto& result = binding.resolved;
    if (!entry.pointerChain || entry.id == 0 || entry.id >= kAddressBookIdLimit
        || !IsValidValueType(entry.valueType)
        || !MemoryAddressBook::ValidPointerChain(entry)
        || session.scope != Scope::ProcessVirtual || session.processCreateTime100ns == 0
        || ksword::memwb::Validate(session) != SessionError::None
        || entry.pointerChain->pointerSize != session.addressBits / 8
        || !result.ok() || result.address == 0
        || result.steps.size() != entry.pointerChain->offsets.size()) return false;

    const auto limit = session.addressBits == 32
        ? static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())
        : 0x00007FFFFFFFFFFFULL;
    const auto width = entry.pointerChain->pointerSize;
    if (result.address > limit || result.steps.front().readAddress < entry.rva) return false;
    for (std::size_t i = 0; i < result.steps.size(); ++i) {
        const auto& step = result.steps[i];
        if (step.readAddress == 0 || step.readAddress > limit || width - 1 > limit - step.readAddress
            || step.pointerValue == 0 || step.pointerValue > limit
            || step.resolvedAddress == 0 || step.resolvedAddress > limit
            || step.offset != entry.pointerChain->offsets[i]
            || (i != 0 && step.readAddress != result.steps[i - 1].resolvedAddress)) return false;
        std::uint64_t expected = 0;
        if (step.offset < 0) {
            const auto magnitude = static_cast<std::uint64_t>(-(step.offset + 1)) + 1;
            if (step.pointerValue < magnitude) return false;
            expected = step.pointerValue - magnitude;
        } else {
            const auto magnitude = static_cast<std::uint64_t>(step.offset);
            if (magnitude > limit - step.pointerValue) return false;
            expected = step.pointerValue + magnitude;
        }
        if (expected != step.resolvedAddress) return false;
        for (std::size_t previous = 0; previous <= i; ++previous)
            if (step.resolvedAddress == result.steps[previous].readAddress) return false;
    }
    return result.address == result.steps.back().resolvedAddress;
}

bool PointerChainBindings::SameDefinition(const AddressEntry& left, const AddressEntry& right) {
    return left.id == right.id && left.moduleName == right.moduleName && left.rva == right.rva
        && left.targetKey == right.targetKey && left.pointerChain == right.pointerChain
        && right.pointerChain && MemoryAddressBook::ValidPointerChain(right);
}

bool PointerChainBindings::SamePath(const pointer_chain::Result& left, const pointer_chain::Result& right) {
    if (!left.ok() || !right.ok() || left.address != right.address || left.steps.size() != right.steps.size())
        return false;
    for (std::size_t i = 0; i < left.steps.size(); ++i) {
        const auto& a = left.steps[i];
        const auto& b = right.steps[i];
        if (a.readAddress != b.readAddress || a.pointerValue != b.pointerValue
            || a.resolvedAddress != b.resolvedAddress || a.offset != b.offset) return false;
    }
    return true;
}

bool PointerChainBindings::SameWitness(const PointerChainBinding& left, const PointerChainBinding& right) {
    return SameTarget(left.session, right.session) && SameDefinition(left.entry, right.entry)
        && SamePath(left.resolved, right.resolved);
}

bool PointerChainBindings::Bind(PointerChainBinding binding) {
    if (!ValidBinding(binding)) return false;
    active_ = std::move(binding);
    ++revision_;
    return true;
}

void PointerChainBindings::ClearActive() noexcept { active_.reset(); ++revision_; }
void PointerChainBindings::Clear() noexcept { active_.reset(); spans_.clear(); ++revision_; }
std::size_t PointerChainBindings::SpanCount() const noexcept { return spans_.size(); }

GuardResult PointerChainBindings::ValidateWitness(const PointerChainBinding& witness,
    const MemoryTargetSession& session, const Lookup& lookup, const Resolver& resolver) {
    if (!SameTarget(witness.session, session)) return Failed(GuardResult::Failure::ChangedSession);
    if (!lookup || !resolver) return Failed(GuardResult::Failure::InvalidBinding);
    std::optional<AddressEntry> current;
    try { current = lookup(witness.entry.id); }
    catch (...) { return Failed(GuardResult::Failure::ChangedDefinition); }
    if (!current || !SameDefinition(witness.entry, *current)) return Failed(GuardResult::Failure::ChangedDefinition);
    if (!SameTarget(witness.session, session)) return Failed(GuardResult::Failure::ChangedSession);
    pointer_chain::Result actual;
    try { actual = resolver(*current, session); }
    catch (...) { return Failed(GuardResult::Failure::ChangedPath); }
    if (!SameTarget(witness.session, session)) return Failed(GuardResult::Failure::ChangedSession);
    if (!SamePath(witness.resolved, actual)) return Failed(GuardResult::Failure::ChangedPath);
    // Resolution can outlive or reenter external book/session updates. The
    // binding-local revision does not track those externally owned objects.
    try { current = lookup(witness.entry.id); }
    catch (...) { return Failed(GuardResult::Failure::ChangedDefinition); }
    if (!current || !SameDefinition(witness.entry, *current)) return Failed(GuardResult::Failure::ChangedDefinition);
    if (!SameTarget(witness.session, session)) return Failed(GuardResult::Failure::ChangedSession);
    return Success();
}

GuardResult PointerChainBindings::RecordSpan(const std::uint64_t first, const std::uint64_t last) {
    Span merged{first, last, *active_};
    std::vector<bool> selected(spans_.size(), false);
    std::size_t count = 0;
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (std::size_t i = 0; i < spans_.size(); ++i) {
            const auto& span = spans_[i];
            if (!selected[i] && SameWitness(merged.witness, span.witness)
                && Touch(merged.first, merged.last, span.first, span.last)) {
                selected[i] = true;
                ++count;
                merged.first = (std::min)(merged.first, span.first);
                merged.last = (std::max)(merged.last, span.last);
                expanded = true;
            }
        }
    }
    if (spans_.size() - count >= MaxSpans) return Failed(GuardResult::Failure::Capacity);
    std::vector<Span> updated;
    updated.reserve(spans_.size() - count + 1);
    for (std::size_t i = 0; i < spans_.size(); ++i)
        if (!selected[i]) updated.push_back(spans_[i]);
    updated.push_back(std::move(merged));
    spans_ = std::move(updated);
    ++revision_;
    return Success();
}

GuardResult PointerChainBindings::Validate(const MemoryTargetSession& session,
    const std::uint64_t address, const std::uint64_t length, const bool historyReplay,
    const Lookup& lookup, const Resolver& resolver) {
    if (length == 0) return Success();
    if (length - 1 > (std::numeric_limits<std::uint64_t>::max)() - address)
        return Failed(GuardResult::Failure::InvalidBinding);
    const auto last = address + length - 1;
    const auto revision = revision_;
    if (active_) {
        const auto witness = *active_;
        const auto result = ValidateWitness(witness, session, lookup, resolver);
        if (revision != revision_) return Failed(GuardResult::Failure::InvalidBinding);
        if (!result.ok) return result;
    }
    if (historyReplay) {
        for (std::size_t i = 0; i < spans_.size(); ++i) {
            const auto& span = spans_[i];
            if (!Overlap(address, last, span.first, span.last)) continue;
            const auto witness = span.witness;
            const auto result = ValidateWitness(witness, session, lookup, resolver);
            if (revision != revision_) return Failed(GuardResult::Failure::InvalidBinding);
            if (!result.ok) return result;
        }
        return Success();
    }
    return active_ ? RecordSpan(address, last) : Success();
}

} // namespace ksword::memwb
