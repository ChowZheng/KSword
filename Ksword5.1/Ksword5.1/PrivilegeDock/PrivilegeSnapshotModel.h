#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace ks::privilege
{
    // Persistent snapshots contain evidence, never executable actions or credentials.
    struct SnapshotDifference
    {
        QString key;
        QString before;
        QString after;
        enum class Kind { Added, Removed, Modified, Enabled, Disabled, Uncertain } kind = Kind::Modified;
    };

    bool validatePermissionSnapshot(const QJsonObject& snapshot, QString* error);
    QVector<SnapshotDifference> comparePermissionSnapshots(
        const QJsonObject& before, const QJsonObject& after, QString* error);
}
