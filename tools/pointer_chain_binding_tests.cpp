#include "../shared/evidence/memory_workbench/PointerChainBindings.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace wb = ksword::memwb;
namespace pc = ksword::pointer_chain;
using Failure = wb::GuardResult::Failure;
namespace {
unsigned checks = 0;
void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}

struct Rig {
    wb::PointerChainBindings bindings;
    wb::MemoryTargetSession session;
    std::map<std::uint64_t, wb::AddressEntry> entries;
    std::map<std::uint64_t, std::uint64_t> bases;
    std::map<std::uint64_t, std::uint64_t> pointers;
    unsigned lookupCalls = 0, resolveCalls = 0, readCalls = 0;

    Rig() {
        session.pid = 77; session.processCreateTime100ns = 1234; session.attachGeneration = 5;
        session.addressBits = 64; session.channel = wb::Channel::Hvm;
        AddEntry(1, 0x1000, 0x2000, 0x3000);
    }
    void AddEntry(std::uint64_t id, std::uint64_t moduleBase, std::uint64_t intermediary,
                  std::uint64_t endpoint) {
        wb::AddressEntry entry;
        entry.id = id; entry.kind = wb::EntryKind::Bookmark; entry.targetKey = "C:/game/game.exe";
        entry.moduleName = "client" + std::to_string(id) + ".dll"; entry.rva = 0x100;
        wb::PointerBookmarkDefinition definition;
        definition.processPath = "C:/game/game.exe";
        definition.modulePath = "C:/game/" + entry.moduleName;
        definition.moduleSize = 0x10000; definition.moduleFileSize = 0x8000; definition.moduleFileTime = 123;
        definition.pointerSize = 8; definition.offsets = {0x20, -8};
        entry.pointerChain = definition;
        entries[id] = entry; bases[id] = moduleBase;
        pointers[moduleBase + entry.rva] = intermediary;
        pointers[intermediary + 0x20] = endpoint + 8;
    }
    wb::PointerChainBindings::Lookup Lookup() {
        return [this](std::uint64_t id) -> std::optional<wb::AddressEntry> {
            ++lookupCalls;
            const auto found = entries.find(id);
            return found == entries.end() ? std::nullopt : std::optional(found->second);
        };
    }
    wb::PointerChainBindings::Resolver Resolver() {
        return [this](const wb::AddressEntry& entry, const wb::MemoryTargetSession&) {
            ++resolveCalls;
            if (!entry.pointerChain) return pc::Result{};
            const auto& definition = *entry.pointerChain;
            pc::Chain chain{entry.rva, definition.offsets, definition.pointerSize};
            return pc::Resolve(chain, bases.at(entry.id) + entry.rva,
                [this](std::uint64_t address, std::size_t width) {
                    ++readCalls;
                    pc::ReadResult read;
                    const auto found = pointers.find(address);
                    if (found == pointers.end()) return read;
                    read.ok = true; read.bytesRead = width;
                    for (std::size_t i = 0; i < width; ++i)
                        read.bytes.push_back(static_cast<std::uint8_t>(found->second >> (8 * i)));
                    return read;
                });
        };
    }
    wb::PointerChainBinding Binding(std::uint64_t id = 1) {
        return {entries.at(id), session, Resolver()(entries.at(id), session)};
    }
    wb::GuardResult Validate(std::uint64_t address = 0x3000, std::uint64_t length = 2, bool history = false) {
        return bindings.Validate(session, address, length, history, Lookup(), Resolver());
    }
};

void TestBindingValidation() {
    for (int mutation = 0; mutation < 12; ++mutation) {
        Rig rig; auto binding = rig.Binding();
        if (mutation == 0) binding.session.processCreateTime100ns = 0;
        if (mutation == 1) binding.session.pid = 0;
        if (mutation == 2) binding.session.scope = wb::Scope::KernelVirtual;
        if (mutation == 3) binding.session.addressBits = 32;
        if (mutation == 4) binding.entry.id = 0;
        if (mutation == 5) binding.entry.pointerChain.reset();
        if (mutation == 6) binding.entry.pointerChain->offsets.clear();
        if (mutation == 7) binding.resolved.status = pc::Status::PartialRead;
        if (mutation == 8) binding.resolved.steps.clear();
        if (mutation == 9) binding.resolved.steps[0].pointerValue += 1;
        if (mutation == 10) binding.resolved.address += 1;
        if (mutation == 11) binding.entry.kind = wb::EntryKind::Watch;
        Check(!rig.bindings.Bind(std::move(binding)), "invalid session, metadata or successful path cannot bind");
    }
    Rig rig;
    Check(rig.bindings.Bind(rig.Binding()), "strong process binding with actual resolver steps is accepted");
    auto invalid = rig.Binding(); invalid.session.processCreateTime100ns = 0;
    Check(!rig.bindings.Bind(invalid) && rig.Validate().ok, "invalid replacement preserves valid active witness");
    rig.entries[1].pointerChain->pointerSize = 4; rig.session.addressBits = 32;
    Check(rig.bindings.Bind(rig.Binding()) && rig.Validate().ok, "32-bit session and pointer width bind together");
}

void TestDefinitionAndSessionChanges() {
    for (int mutation = 0; mutation < 9; ++mutation) {
        Rig rig; rig.bindings.Bind(rig.Binding());
        if (mutation == 0) rig.entries.erase(1);
        if (mutation == 1) ++rig.entries[1].rva;
        if (mutation == 2) rig.entries[1].moduleName = "other.dll";
        if (mutation == 3) rig.entries[1].targetKey = "other.exe";
        if (mutation == 4) ++rig.entries[1].pointerChain->moduleFileTime;
        if (mutation == 5) rig.entries[1].pointerChain->offsets[0] += 1;
        if (mutation == 6) rig.entries[1].pointerChain->modulePath = "C:/other.dll";
        if (mutation == 7) rig.entries[1].pointerChain.reset();
        if (mutation == 8) rig.entries[1].kind = wb::EntryKind::Watch;
        const auto result = rig.Validate();
        Check(!result.ok && result.failure == Failure::ChangedDefinition && rig.bindings.SpanCount() == 0,
              "deleted or changed address definitions reject before recording attempted writes");
    }
    Rig harmless; harmless.bindings.Bind(harmless.Binding());
    harmless.entries[1].note = "edited note"; harmless.entries[1].valueType = wb::ValueType::U32;
    Check(harmless.Validate().ok, "note and value type edits leave chain provenance intact");
    for (int mutation = 0; mutation < 7; ++mutation) {
        Rig rig; rig.bindings.Bind(rig.Binding());
        if (mutation == 0) ++rig.session.pid;
        if (mutation == 1) ++rig.session.processCreateTime100ns;
        if (mutation == 2) ++rig.session.attachGeneration;
        if (mutation == 3) rig.session.channel = wb::Channel::UserMode;
        if (mutation == 4) ++rig.session.ddmaGeneration;
        if (mutation == 5) rig.session.addressBits = 32;
        if (mutation == 6) rig.session.scope = wb::Scope::Physical;
        const auto result = rig.Validate();
        Check(!result.ok && result.failure == Failure::ChangedSession,
              "every SameTarget session field is required for a bound write");
    }
}

void TestFullPathAndHistoricalNavigation() {
    Rig rig; rig.bindings.Bind(rig.Binding());
    Check(rig.Validate().ok && rig.bindings.SpanCount() == 1, "normal bound write records its attempted span");
    rig.bindings.ClearActive();
    rig.pointers[0x1100] = 0x4000; rig.pointers[0x4020] = 0x3008;
    const auto moved = rig.Resolver()(rig.entries[1], rig.session);
    Check(moved.ok() && moved.address == 0x3000, "fixture changes intermediary without moving terminal address");
    const auto calls = rig.resolveCalls;
    Check(rig.Validate().ok && rig.resolveCalls == calls && rig.bindings.SpanCount() == 1,
          "manual raw edit after navigation is allowed without validating or recording old chain");
    const auto undo = rig.Validate(0x3000, 2, true);
    Check(!undo.ok && undo.failure == Failure::ChangedPath && rig.bindings.SpanCount() == 1,
          "post-navigation undo still rejects changed original chain even at same endpoint");
    Check(rig.Validate(0x3002, 1, true).ok, "adjacent non-overlapping history range has no unrelated witness");
    rig.bindings.Clear();
    Check(rig.bindings.SpanCount() == 0 && rig.Validate(0x3000, 2, true).ok,
          "target Clear removes both active and historical provenance");

    Rig relocated; relocated.bindings.Bind(relocated.Binding());
    relocated.bases[1] = 0x5000; relocated.pointers[0x5100] = 0x2000;
    Check(relocated.Validate().failure == Failure::ChangedPath, "changed module/root read address rejects old path");
}

void TestConservativeOverlappingWitnesses() {
    Rig rig; rig.AddEntry(2, 0x5000, 0x6000, 0x3000);
    rig.bindings.Bind(rig.Binding(1)); Check(rig.Validate(0x3000, 4).ok, "first chain records overlapping span");
    rig.bindings.Bind(rig.Binding(2)); Check(rig.Validate(0x3002, 4).ok, "second chain records its overlapping span");
    rig.bindings.ClearActive();
    Check(rig.bindings.SpanCount() == 2, "different witnesses never merge just because spans overlap");
    const auto calls = rig.resolveCalls;
    Check(rig.Validate(0x3002, 2, true).ok && rig.resolveCalls == calls + 2,
          "history replay validates every overlapping chain witness");
    rig.pointers[0x1100] = 0x4000; rig.pointers[0x4020] = 0x3008;
    Check(rig.Validate(0x3004, 2, true).ok, "history overlap with only second chain ignores distant first witness");
    Check(rig.Validate(0x3002, 2, true).failure == Failure::ChangedPath,
          "history overlap requiring both chains rejects if either path changed");
    rig.entries.erase(2);
    Check(rig.Validate(0x3004, 2, true).failure == Failure::ChangedDefinition,
          "deleted historical chain still prevents replay to its attempted write span");
}

void TestMergingCapacityAndOverflow() {
    Rig rig; rig.bindings.Bind(rig.Binding());
    Check(rig.Validate(0x3000, 2).ok && rig.Validate(0x3004, 2).ok && rig.bindings.SpanCount() == 2,
          "disjoint equivalent witnesses initially retain two spans");
    Check(rig.Validate(0x3002, 2).ok && rig.bindings.SpanCount() == 1,
          "bridging adjacent equivalent spans merges transitively");
    Check(rig.Validate(0x3001, 3).ok && rig.bindings.SpanCount() == 1,
          "overlapping equivalent attempted writes reuse existing span");
    rig.bindings.Clear(); rig.bindings.Bind(rig.Binding());
    for (std::size_t i = 0; i < wb::PointerChainBindings::MaxSpans; ++i)
        Check(rig.Validate(0x10000 + 2 * i, 1).ok, "bounded disjoint witness capacity accepts up to limit");
    Check(rig.bindings.SpanCount() == 256 && rig.Validate(0x20000, 1).failure == Failure::Capacity
          && rig.bindings.SpanCount() == 256, "new write is rejected before exceeding span capacity");
    Check(rig.Validate(0x10000, 512).ok && rig.bindings.SpanCount() == 1,
          "merging remains allowed at full capacity and releases slots");
    const auto before = rig.bindings.SpanCount(); const auto calls = rig.resolveCalls;
    Check(rig.Validate((std::numeric_limits<std::uint64_t>::max)() - 1, 3).failure == Failure::InvalidBinding
          && rig.bindings.SpanCount() == before && rig.resolveCalls == calls,
          "overflow rejects before callback or history mutation");
    Check(rig.Validate((std::numeric_limits<std::uint64_t>::max)(), 0).ok
          && rig.resolveCalls == calls && rig.bindings.SpanCount() == before, "empty range reads and records nothing");
}

void TestCallbackFailuresAndActiveReplay() {
    Rig rig; rig.bindings.Bind(rig.Binding());
    Check(rig.bindings.Validate(rig.session, 0x3000, 1, false, {}, rig.Resolver()).failure == Failure::InvalidBinding,
          "active chain without lookup cannot authorize write");
    Check(rig.bindings.Validate(rig.session, 0x3000, 1, false, rig.Lookup(), {}).failure == Failure::InvalidBinding,
          "active chain without resolver cannot authorize write");
    const auto throwingLookup = [](std::uint64_t) -> std::optional<wb::AddressEntry> { throw std::runtime_error("lookup"); };
    const auto throwingResolver = [](const wb::AddressEntry&, const wb::MemoryTargetSession&) -> pc::Result {
        throw std::runtime_error("resolve");
    };
    Check(rig.bindings.Validate(rig.session, 0x3000, 1, false, throwingLookup, rig.Resolver()).failure == Failure::ChangedDefinition,
          "lookup exceptions fail closed");
    Check(rig.bindings.Validate(rig.session, 0x3000, 1, false, rig.Lookup(), throwingResolver).failure == Failure::ChangedPath,
          "resolver exceptions fail closed");
    rig.pointers.erase(0x2020);
    Check(rig.Validate(0x9000, 1, true).failure == Failure::ChangedPath,
          "active provenance is required during historical replay even before a span is recorded");
    rig.bindings.ClearActive();
    Check(rig.bindings.Validate(rig.session, 0x9000, 1, true, {}, {}).ok,
          "unbound historical range needs no callbacks");
    Rig changing; changing.bindings.Bind(changing.Binding());
    auto clearDuringResolve = [&changing](const wb::AddressEntry& entry, const wb::MemoryTargetSession& session) {
        auto result = changing.Resolver()(entry, session);
        changing.bindings.ClearActive();
        return result;
    };
    Check(changing.bindings.Validate(changing.session, 0x3000, 1, false, changing.Lookup(), clearDuringResolve)
          .failure == Failure::InvalidBinding && changing.bindings.SpanCount() == 0,
          "navigation during callback invalidates active authorization without recording a stale witness");
    changing.bindings.Bind(changing.Binding()); changing.Validate(); changing.bindings.ClearActive();
    auto clearHistory = [&changing](const wb::AddressEntry& entry, const wb::MemoryTargetSession& session) {
        auto result = changing.Resolver()(entry, session);
        changing.bindings.Clear();
        return result;
    };
    Check(changing.bindings.Validate(changing.session, 0x3000, 1, true, changing.Lookup(), clearHistory)
          .failure == Failure::InvalidBinding && changing.bindings.SpanCount() == 0,
          "history mutation during callback rejects safely without retaining dangling witness references");
    for (int mutation = 0; mutation < 4; ++mutation) {
        Rig external; external.bindings.Bind(external.Binding());
        auto mutateExternal = [&external, mutation](const wb::AddressEntry& entry, const wb::MemoryTargetSession& session) {
            auto result = external.Resolver()(entry, session);
            if (mutation == 0) ++external.session.pid;
            if (mutation == 1) external.entries.erase(1);
            if (mutation == 2) ++external.entries[1].rva;
            if (mutation == 3) external.entries[1].note = "harmless edit during resolution";
            return result;
        };
        const auto result = external.bindings.Validate(external.session, 0x3000, 1, false, external.Lookup(), mutateExternal);
        Check(mutation == 3 ? result.ok && external.bindings.SpanCount() == 1
              : !result.ok && external.bindings.SpanCount() == 0
                  && result.failure == (mutation == 0 ? Failure::ChangedSession : Failure::ChangedDefinition),
              "external session and definition mutations during resolution recheck before authorization");
    }
    Rig lookupMutation; lookupMutation.bindings.Bind(lookupMutation.Binding());
    const auto changingLookup = [&lookupMutation](std::uint64_t id) {
        auto entry = lookupMutation.entries.at(id);
        ++lookupMutation.session.pid;
        return std::optional(entry);
    };
    const auto before = lookupMutation.resolveCalls;
    Check(lookupMutation.bindings.Validate(lookupMutation.session, 0x3000, 1, false, changingLookup,
          lookupMutation.Resolver()).failure == Failure::ChangedSession && lookupMutation.resolveCalls == before,
          "lookup changing the external session rejects before resolution");
}
} // namespace

int main() {
    try {
        TestBindingValidation(); TestDefinitionAndSessionChanges(); TestFullPathAndHistoricalNavigation();
        TestConservativeOverlappingWitnesses(); TestMergingCapacityAndOverflow(); TestCallbackFailuresAndActiveReplay();
        std::cout << "Pointer-chain provenance: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Pointer-chain provenance failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
