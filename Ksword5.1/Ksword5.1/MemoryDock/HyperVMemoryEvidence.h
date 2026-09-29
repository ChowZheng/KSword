#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace ksword::hyperv {
using Bytes = std::optional<std::uint64_t>;
enum class Metric {
    VidPhysical, VidRemote, DynamicPhysical, GuestVisible, HypervisorTotal,
    ChildDeposited, RootDeposited, ChildGpa, RootGpa, ChildTlb, RootTlb,
    BalancerAvailable, BalancerAvailableForBalancing
};
struct Counter {
    Metric metric = Metric::VidPhysical;
    QString path, instance, sampledAt, explanation;
    std::uint64_t raw = 0;
    Bytes bytes;
    std::uint32_t status = 0;
};
struct Source {
    QString name;
    std::uint32_t status = 0, rows = 0;
    bool complete = false;
};
// An inventory object is separate from a counter instance. Only stable IDs or
// an unambiguous exact display name may connect these independently sampled views.
struct Inventory {
    QString id, runtimeId, name, owner, type, state, hostingSystemId;
    bool hcs = false, wmi = false;
    std::uint32_t workerPid = 0;
    Bytes wmiCapacity, hcsNodeBytes, hcsPrivateWs, hcsCommit;
    QJsonObject memoryEvidence;
};
struct Process {
    QString name;
    std::uint32_t pid = 0, status = 0;
    Bytes workingSet, privateCommit;
};
struct Partition : Inventory {
    QString key, counterInstance;
    QStringList aliases;
    Bytes vidBytes, dynamicBytes, guestVisibleBytes, depositedBytes;
    bool inventoryMatched = false, ambiguous = false, conflictingVid = false;
    std::vector<std::size_t> counterIndices;
};
struct Snapshot {
    QString started, finished, hypervisorVendor;
    std::uint64_t elapsedMs = 0, installed = 0, total = 0, available = 0;
    std::uint64_t totalAfter = 0, availableAfter = 0;
    unsigned vbsState = 0;
    bool vbsKnown = false, hypervisorPresent = false, cancelled = false, timedOut = false, resourceFailure = false;
    std::vector<unsigned> securityServices;
    std::vector<Counter> counters;
    std::vector<Inventory> inventory;
    std::vector<Process> processes;
    std::vector<Source> sources;
    std::vector<Partition> partitions;
    Bytes observedVidBytes, vidTotalBytes, unresolvedVidBytes;
    bool vidConflict = false;
};
struct Context {
    QString snapshotTime, pfnTime;
    Bytes snapshotRemainder, pfnDriverLocked, pfnUnknown, pfnUnscanned;
};
struct Job {
    std::atomic_bool cancel{false}, done{false};
    std::atomic<unsigned> phase{0};
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::mutex mutex;
    std::shared_ptr<Snapshot> result;
    bool stopped() const { return cancel.load() || std::chrono::steady_clock::now() >= deadline; }
};
QString canonicalGuid(QString text);
const char* metricName(Metric metric);
void correlate(Snapshot& snapshot);
QJsonObject toJson(const Snapshot& snapshot, const Context& context = {});
void collect(const std::shared_ptr<Job>& job);
// Internal read-only providers, also reused by the non-GUI diagnostic probe.
void collectWmi(Snapshot& snapshot, const std::shared_ptr<Job>& job);
void collectHcs(Snapshot& snapshot, const std::shared_ptr<Job>& job);
}
