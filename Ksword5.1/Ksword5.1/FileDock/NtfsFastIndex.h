#pragma once

#include <QHash>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>

namespace ks::files {

struct NtfsIndexName {
    quint64 parentReference = 0;
    QString name;
};

struct NtfsIndexEntry {
    quint64 fileReference = 0; // Full NTFS reference, including sequence number.
    quint64 parentReference = 0;
    QString name;             // Deterministic canonical hard link.
    QVector<NtfsIndexName> names; // Non-DOS names; storage is counted once per reference.
    QVector<NtfsIndexName> baseNames; // Names physically present in the base record.
    QVector<quint64> attributeReferences;
    bool hasAttributeList = false;
    bool attributeListResident = true;
    quint32 attributes = 0;
    quint64 logicalBytes = 0; // Unnamed $DATA stream only.
    quint64 allocatedBytes = 0;
    bool sizeKnown = false;
    bool isDirectory() const { return (attributes & 0x10U) != 0; }
};

struct NtfsIndexExtension {
    quint64 baseReference = 0;
    QVector<NtfsIndexName> names;
};

struct NtfsIndexSnapshot {
    QString volumeRoot; // Drive root including its trailing separator.
    QHash<quint64, NtfsIndexEntry> entries;
    QHash<quint64, NtfsIndexExtension> extensions; // Needed to update extension FILE_NAME links correctly.
    quint64 journalId = 0;
    qint64 nextUsn = 0;
    quint64 volumeSerial = 0;
    quint64 rootReference = 0;
    quint64 unknownSizeCount = 0;
    quint64 namesIncompleteCount = 0;
    bool complete = false;
    bool journalAvailable = false;
    bool cancelled = false;
    bool rebuildRequired = false;
    QString error;
    QString status;
};

class NtfsFastIndex final {
public:
    using Cancel = std::shared_ptr<std::atomic_bool>;
    using Progress = std::function<void(quint64 processed, quint64 total)>;
    using Snapshot = std::shared_ptr<const NtfsIndexSnapshot>;

    // Blocking read-only operations. Call on a worker, never on the GUI thread.
    static Snapshot build(const QString& volumeRoot, const Cancel& cancel = {},
                          const Progress& progress = {});
    // Journal identity/gap changes return rebuildRequired=true, keeping the old snapshot usable.
    static Snapshot refresh(const Snapshot& previous, const Cancel& cancel = {},
                            const Progress& progress = {});
    // Resolves cached references only, with cycle/stale-parent detection; no file-system I/O.
    static QString fullPath(const NtfsIndexSnapshot& snapshot, quint64 reference);
};

} // namespace ks::files
