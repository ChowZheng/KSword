#pragma once

#include "DriverLeasePolicy.h"
#include "DriverService.h"
#include "Win32Lean.h"

#include <cstdint>
#include <string>

namespace Ksword::Core {

// DriverLeaseReleaseOutcome is the three-state outcome of a release attempt.
// Stopped and NotNeeded are completed releases; Failed means the release did
// not complete and the caller may retry it.
enum class DriverLeaseReleaseOutcome {
    Stopped,    // The lease was released and the driver owned by Light was stopped.
    NotNeeded,  // The lease was released; no driver stop was required.
    Failed      // The lease was not released; the caller may retry.
};

// DriverLeaseRelease reports the outcome of DriverLease::releaseRequestsStop.
// error carries the failure reason when the outcome is Failed; status carries
// the driver state observed by the release.
struct DriverLeaseRelease {
    DriverLeaseReleaseOutcome outcome = DriverLeaseReleaseOutcome::Failed;
    std::wstring error;
    DriverRuntimeStatus status;

    bool ok() const noexcept { return outcome != DriverLeaseReleaseOutcome::Failed; }
};

// DriverLease coordinates KswordARK ownership across concurrently running
// KswordARKLight processes. acquire registers the current process identity;
// observeStartTransition records only a stopped-to-running transition;
// releaseRequestsStop releases this process's lease and stops the driver only
// when this process held the last live lease of a driver started by Light.
class DriverLease final {
public:
    DriverLease() = default;
    ~DriverLease();

    DriverLease(const DriverLease&) = delete;
    DriverLease& operator=(const DriverLease&) = delete;

    bool acquire();
    void observeStartTransition(bool runningBefore, bool runningAfter);
    void observeExplicitStop();
    // releaseRequestsStop releases this process's lease and stops the driver
    // only when the released lease was the last live one of a driver started by
    // Light. The outcome separates a completed stop from a release that needed
    // no stop and from a release that did not complete; a Failed outcome stays
    // retryable, and the failure reason is reported in the returned value.
    DriverLeaseRelease releaseRequestsStop();
    bool registered() const noexcept { return registered_; }

private:
    void closeHandles() noexcept;

    HANDLE mutex_ = nullptr;
    HANDLE mapping_ = nullptr;
    void* shared_ = nullptr;
    DWORD processId_ = 0;
    std::uint64_t processCreationTime100ns_ = 0;
    bool registered_ = false;
    // released_ means the release really completed: the shared registry no
    // longer lists this process and no driver stop is owed. It is set only
    // after that work finished, so a failed attempt stays retryable.
    bool released_ = false;
    // stopRequired_ records that the released lease owned the last live lease
    // of a driver started by Light; stopCompleted_ records that the driver was
    // confirmed stopped afterwards.
    bool stopRequired_ = false;
    bool stopCompleted_ = false;
};

} // namespace Ksword::Core
