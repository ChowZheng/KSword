#include "DriverLease.h"
#include "../../shared/usermode/KswordArkServiceMode.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Ksword::Core {
namespace {

constexpr wchar_t kLeaseMutexName[] = L"Local\\KswordARKLight.DriverLease.Mutex.v1";
constexpr wchar_t kLeaseMappingName[] = L"Local\\KswordARKLight.DriverLease.Mapping.v1";
constexpr std::uint32_t kLeaseMagic = 0x4C534B41U; // AKSL
constexpr std::uint32_t kLeaseVersion = 1U;
constexpr std::size_t kMaximumLeases = 32U;

struct LeaseEntry final {
    DWORD processId = 0;
    std::uint64_t creationTime100ns = 0;
};

struct SharedLeaseState final {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t driverOwnedByLight = 0;
    std::uint32_t reserved = 0;
    std::array<LeaseEntry, kMaximumLeases> leases{};
};

class MutexGuard final {
public:
    explicit MutexGuard(HANDLE mutex) : mutex_(mutex) {
        if (mutex_) {
            const DWORD wait = ::WaitForSingleObject(mutex_, 5000);
            locked_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        }
    }
    ~MutexGuard() {
        if (locked_) {
            ::ReleaseMutex(mutex_);
        }
    }
    bool locked() const noexcept { return locked_; }
private:
    HANDLE mutex_ = nullptr;
    bool locked_ = false;
};

std::uint64_t FileTimeValue(const FILETIME& value) noexcept {
    ULARGE_INTEGER integer{};
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

std::uint64_t ProcessCreationTime(HANDLE process) noexcept {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    return process && ::GetProcessTimes(process, &creation, &exit, &kernel, &user)
        ? FileTimeValue(creation)
        : 0U;
}

bool IsEntryAlive(const LeaseEntry& entry) noexcept {
    if (entry.processId == 0 || entry.creationTime100ns == 0) {
        return false;
    }
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.processId);
    if (!process) {
        return false;
    }
    const DWORD wait = ::WaitForSingleObject(process, 0);
    const std::uint64_t creation = ProcessCreationTime(process);
    ::CloseHandle(process);
    return wait == WAIT_TIMEOUT && creation == entry.creationTime100ns;
}

void InitializeIfNeeded(SharedLeaseState& state) noexcept {
    if (state.magic == kLeaseMagic && state.version == kLeaseVersion) {
        return;
    }
    std::memset(&state, 0, sizeof(state));
    state.magic = kLeaseMagic;
    state.version = kLeaseVersion;
}

std::size_t CompactLiveLeases(SharedLeaseState& state) noexcept {
    std::array<LeaseEntry, kMaximumLeases> live{};
    std::size_t count = 0;
    for (const LeaseEntry& entry : state.leases) {
        if (IsEntryAlive(entry) && count < live.size()) {
            live[count++] = entry;
        }
    }
    state.leases = live;
    return count;
}

SharedLeaseState* State(void* shared) noexcept {
    return static_cast<SharedLeaseState*>(shared);
}

} // namespace

DriverLease::~DriverLease() {
    if (!released_) {
        // The owner never completed the release. The shared registry entry must
        // not outlive this process, and a Light-owned driver that this process
        // was the last live lease of must not survive it. The result cannot be
        // reported to a user from a destructor, so it goes to the debugger.
        try {
            const DriverLeaseRelease release = releaseRequestsStop();
            if (!release.ok()) {
                ::OutputDebugStringW(L"[KswordARKLight] Driver lease release failed during teardown: ");
                ::OutputDebugStringW(release.error.c_str());
                ::OutputDebugStringW(L"\r\n");
            }
        } catch (...) {
            ::OutputDebugStringW(L"[KswordARKLight] Driver lease release raised an exception during teardown.\r\n");
        }
    }
    closeHandles();
}

bool DriverLease::acquire() {
    if (registered_) {
        return true;
    }
    processId_ = ::GetCurrentProcessId();
    processCreationTime100ns_ = ProcessCreationTime(::GetCurrentProcess());
    if (processCreationTime100ns_ == 0) {
        return false;
    }

    // The lease object can be reused after a completed release, for example
    // when an exit was cancelled and the session continued. The existing
    // handles stay open so the named shared state is never dropped, and the
    // registry entry of this process is registered again in it.
    if (!mutex_) {
        mutex_ = ::CreateMutexW(nullptr, FALSE, kLeaseMutexName);
        if (!mutex_) {
            closeHandles();
            return false;
        }
    }
    if (!mapping_) {
        mapping_ = ::CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(sizeof(SharedLeaseState)),
            kLeaseMappingName);
        if (!mapping_) {
            closeHandles();
            return false;
        }
    }
    if (!shared_) {
        shared_ = ::MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedLeaseState));
        if (!shared_) {
            closeHandles();
            return false;
        }
    }

    bool acquired = false;
    {
        MutexGuard guard(mutex_);
        if (guard.locked()) {
            SharedLeaseState& state = *State(shared_);
            InitializeIfNeeded(state);
            const std::size_t count = CompactLiveLeases(state);
            for (std::size_t index = 0; index < count; ++index) {
                if (state.leases[index].processId == processId_ &&
                    state.leases[index].creationTime100ns == processCreationTime100ns_) {
                    acquired = true;
                    break;
                }
            }
            if (!acquired && count < state.leases.size()) {
                state.leases[count] = { processId_, processCreationTime100ns_ };
                acquired = true;
            }
        }
    }
    if (!acquired) {
        closeHandles();
        return false;
    }
    // A successful registration starts a fresh ownership cycle. Any stop
    // obligation of an earlier cycle is dropped here, because that cycle's
    // registry entry is gone and the new entry is re-evaluated against the
    // shared state when this cycle is released.
    stopRequired_ = false;
    stopCompleted_ = false;
    released_ = false;
    registered_ = true;
    return true;
}

void DriverLease::observeStartTransition(const bool runningBefore, const bool runningAfter) {
    if (!ksword::ark::queryServiceProfile().scmManagementAllowed()) {
        observeExplicitStop();
        return;
    }
    if (!registered_ || !DriverLeasePolicy::OwnsStartTransition(runningBefore, runningAfter)) {
        return;
    }
    MutexGuard guard(mutex_);
    if (guard.locked() && shared_) {
        SharedLeaseState& state = *State(shared_);
        InitializeIfNeeded(state);
        state.driverOwnedByLight = 1U;
    }
}

void DriverLease::observeExplicitStop() {
    if (!registered_) {
        return;
    }
    MutexGuard guard(mutex_);
    if (guard.locked() && shared_) {
        SharedLeaseState& state = *State(shared_);
        InitializeIfNeeded(state);
        state.driverOwnedByLight = 0U;
    }
}

DriverLeaseRelease DriverLease::releaseRequestsStop() {
    DriverLeaseRelease result;
    result.outcome = DriverLeaseReleaseOutcome::Failed;

    // A previous attempt may have removed the registry entry but still owe a
    // driver stop. That obligation survives here so a retry finishes it.
    const bool stopPending = stopRequired_ && !stopCompleted_;
    if (!registered_ && !stopPending) {
        // Nothing was ever registered, or an earlier attempt already released
        // the lease. There is no registry entry and no driver stop to make.
        released_ = true;
        result.outcome = DriverLeaseReleaseOutcome::NotNeeded;
        return result;
    }

    if (registered_) {
        bool shouldStop = false;
        {
            MutexGuard guard(mutex_);
            if (!guard.locked()) {
                // The registry was not touched, so the release stays pending
                // and the next attempt can retry it.
                result.error = L"释放驱动租约失败：等待租约互斥体超时（5 秒）";
                return result;
            }
            SharedLeaseState& state = *State(shared_);
            InitializeIfNeeded(state);
            for (LeaseEntry& entry : state.leases) {
                if (entry.processId == processId_ && entry.creationTime100ns == processCreationTime100ns_) {
                    entry = {};
                }
            }
            const std::size_t remaining = CompactLiveLeases(state);
            shouldStop = DriverLeasePolicy::ShouldStopOnLastRelease(state.driverOwnedByLight != 0U, remaining);
        }
        // The registry really no longer lists this process. The ownership flag
        // is cleared only after a confirmed stop, so a failed stop stays
        // visible to the next attempt.
        registered_ = false;
        stopRequired_ = stopRequired_ || shouldStop;
    }

    bool stopRequested = false;
    if (stopRequired_ && !stopCompleted_) {
        if (!ksword::ark::queryServiceProfile().scmManagementAllowed()) {
            // This profile does not allow SCM control, so there is no stop to
            // perform here and nothing failed.
            stopCompleted_ = true;
        } else {
            DriverRuntimeStatus status = QueryDriverStatus();
            if (status.serviceRunning || status.controlDeviceOpen) {
                status = StopDriverService();
                stopRequested = true;
            }
            result.status = status;
            if (status.serviceRunning || status.controlDeviceOpen) {
                // The driver is still loaded. The stop obligation stays open so
                // the caller can retry it.
                result.error = L"驱动仍在运行：停止驱动服务失败";
                return result;
            }
            stopCompleted_ = true;
            {
                MutexGuard guard(mutex_);
                if (guard.locked() && shared_) {
                    SharedLeaseState& state = *State(shared_);
                    InitializeIfNeeded(state);
                    // Another live lease may have joined while the stop was in
                    // flight. Its own release owns the stop then, so the shared
                    // ownership flag stays set and is not stolen here.
                    if (CompactLiveLeases(state) == 0) {
                        state.driverOwnedByLight = 0U;
                    }
                }
            }
        }
    }

    released_ = true;
    result.outcome = stopRequested ? DriverLeaseReleaseOutcome::Stopped
                                   : DriverLeaseReleaseOutcome::NotNeeded;
    return result;
}

void DriverLease::closeHandles() noexcept {
    if (shared_) {
        ::UnmapViewOfFile(shared_);
        shared_ = nullptr;
    }
    if (mapping_) {
        ::CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    if (mutex_) {
        ::CloseHandle(mutex_);
        mutex_ = nullptr;
    }
}

} // namespace Ksword::Core
