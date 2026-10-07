#pragma once

#include "../../../shared/evidence/PointerChain.h"
#include "../../../shared/evidence/memory_workbench/MemoryAddressBook.h"
#include "../../../shared/evidence/memory_workbench/MemoryIoPort.h"
#include "../../../shared/evidence/memory_workbench/SessionAddressResolver.h"
#include <atomic>
#include <memory>

namespace ksword::memwb_pointer_access
{
    enum class Issue
    {
        None, InvalidTarget, UnsupportedChannel, IdentityUnavailable, TargetChanged,
        ModuleUnavailable, ModuleChanged, WrongProgram, InvalidRoot, RootRemapped,
        ReadFailed, Cancelled
    };

    struct ModuleCapture
    {
        bool ok = false;
        memwb::PointerBookmarkDefinition definition;
        std::uint64_t base = 0;
        Issue issue = Issue::None;
    };

    // Queries only process/module/file metadata; never reads or writes target bytes.
    ModuleCapture CaptureModule(const memwb::MemoryTargetSession& session, const std::string& modulePath);

    struct Resolution
    {
        pointer_chain::Result result;
        Issue issue = Issue::None;
        std::string annotation;
    };

    // Uses the workbench's configured port, retaining a process identity lease.
    // Cancellation is cooperative between reads, not an IOCTL interruption.
    Resolution Resolve(const memwb::AddressEntry& entry, const memwb::MemoryTargetSession& session,
        memwb::IMemoryIoPort& port, const std::shared_ptr<std::atomic<bool>>& cancel = {});
}
