#include "PrivilegeSnapshotModel.h"
#include "../Internationalization/LanguageManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QDateTime>
#include <QMap>
#include <QSet>

namespace ks::privilege
{
    namespace
    {
        constexpr int MaximumEntries = 200000;
        struct Evidence { QString value; QString state; };
        QString message(const QString& source) { return ks::i18n::sourceText(source); }

        bool collect(const QJsonObject& section, const QString& prefix,
            QMap<QString, Evidence>* entries, QString* error)
        {
            if (!section.value(QStringLiteral("entries")).isArray()
                || !section.value(QStringLiteral("errors")).isArray())
            {
                if (error) *error = message(QStringLiteral("快照缺少条目或采集错误信息。"));
                return false;
            }
            const QJsonArray rows = section.value(QStringLiteral("entries")).toArray();
            if (rows.size() > MaximumEntries)
            {
                if (error) *error = message(QStringLiteral("快照条目数量超过限制。"));
                return false;
            }
            for (const QJsonValue& row : rows)
            {
                const QJsonObject object = row.toObject();
                const QString key = object.value(QStringLiteral("key")).toString();
                const QJsonValue value = object.value(QStringLiteral("value"));
                const QString state = object.value(QStringLiteral("state")).toString(QStringLiteral("read"));
                if (!row.isObject() || key.isEmpty() || key.size() > 4096
                    || value.isUndefined() || value.isNull()
                    || (!value.isString() && !value.isBool() && !value.isDouble())
                    || (object.contains(QStringLiteral("state")) && !object.value(QStringLiteral("state")).isString())
                    || (state != QLatin1String("read") && state != QLatin1String("absent") && state != QLatin1String("unreadable"))
                    || (value.isString() && value.toString().size() > 65536)
                    || entries->contains(prefix + key))
                {
                    if (error) *error = message(QStringLiteral("快照含无效或重复的条目。"));
                    return false;
                }
                // Keep a type-tagged representation: string "1" and number 1 differ.
                entries->insert(prefix + key, {QString::fromUtf8(
                    QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)), state});
            }
            if (section.value(QStringLiteral("errors")).toArray().size() > 10000)
            {
                if (error) *error = message(QStringLiteral("快照采集错误信息格式无效。"));
                return false;
            }
            for (const QJsonValue& diagnostic : section.value(QStringLiteral("errors")).toArray())
            {
                if (!diagnostic.isString() || diagnostic.toString().size() > 16384)
                {
                    if (error) *error = message(QStringLiteral("快照采集错误信息格式无效。"));
                    return false;
                }
            }
            return true;
        }

        bool partial(const QJsonObject& section)
        {
            return !section.value(QStringLiteral("errors")).toArray().isEmpty();
        }
    }

    bool validatePermissionSnapshot(const QJsonObject& snapshot, QString* error)
    {
        if (error) error->clear();
        if (snapshot.value(QStringLiteral("schema")).toString() != QLatin1String("ksword.permission.snapshot")
            || snapshot.value(QStringLiteral("version")).toInt() != 1
            || snapshot.value(QStringLiteral("computer")).toString().isEmpty()
            || snapshot.value(QStringLiteral("computer")).toString().size() > 255
            || !snapshot.value(QStringLiteral("tokenIncluded")).isBool()
            || snapshot.value(QStringLiteral("capturedUtc")).toString().size() > 64
            || !QDateTime::fromString(snapshot.value(QStringLiteral("capturedUtc")).toString(), Qt::ISODateWithMs).isValid()
            || !snapshot.value(QStringLiteral("policy")).isObject()
            || !snapshot.value(QStringLiteral("token")).isObject())
        {
            if (error) *error = message(QStringLiteral("快照格式或版本不受支持。"));
            return false;
        }
        QMap<QString, Evidence> entries;
        if (!collect(snapshot.value(QStringLiteral("policy")).toObject(), QStringLiteral("policy/"), &entries, error)
            || !collect(snapshot.value(QStringLiteral("token")).toObject(), QStringLiteral("token/"), &entries, error)) return false;
        if (entries.size() > MaximumEntries)
        {
            if (error) *error = message(QStringLiteral("快照条目数量超过限制。"));
            return false;
        }
        return true;
    }

    QVector<SnapshotDifference> comparePermissionSnapshots(
        const QJsonObject& before, const QJsonObject& after, QString* error)
    {
        QVector<SnapshotDifference> differences;
        if (!validatePermissionSnapshot(before, error) || !validatePermissionSnapshot(after, error))
            return differences;
        if (before.value(QStringLiteral("computer")) != after.value(QStringLiteral("computer")))
        {
            if (error) *error = message(QStringLiteral("两份快照来自不同计算机，无法比较本地权限变化。"));
            return differences;
        }
        if (before.value(QStringLiteral("tokenIncluded")) != after.value(QStringLiteral("tokenIncluded")))
        {
            if (error) *error = message(QStringLiteral("两份快照的令牌采集选项不同，请使用相同选项重新采集。"));
            return differences;
        }
        for (const QString& sectionName : {QStringLiteral("policy"), QStringLiteral("token")})
        {
            const QJsonObject previous = before.value(sectionName).toObject();
            const QJsonObject current = after.value(sectionName).toObject();
            QMap<QString, Evidence> a, b;
            collect(previous, sectionName + QLatin1Char('/'), &a, error);
            collect(current, sectionName + QLatin1Char('/'), &b, error);
            QSet<QString> keys;
            for (auto it = a.cbegin(); it != a.cend(); ++it) keys.insert(it.key());
            for (auto it = b.cbegin(); it != b.cend(); ++it) keys.insert(it.key());
            QStringList ordered = keys.values();
            ordered.sort();
            for (const QString& key : ordered)
            {
                const bool hasBefore = a.contains(key), hasAfter = b.contains(key);
                const Evidence av = a.value(key), bv = b.value(key);
                if (hasBefore && hasAfter && ((av.state == QLatin1String("unreadable") && bv.state == QLatin1String("unreadable"))
                    || (av.value == bv.value && av.state == bv.state))) continue;
                SnapshotDifference row{key, av.value, bv.value};
                if (av.state == QLatin1String("unreadable") || bv.state == QLatin1String("unreadable"))
                    row.kind = SnapshotDifference::Kind::Uncertain;
                else if (!hasBefore)
                    row.kind = partial(previous) ? SnapshotDifference::Kind::Uncertain : SnapshotDifference::Kind::Added;
                else if (!hasAfter)
                    row.kind = partial(current) ? SnapshotDifference::Kind::Uncertain : SnapshotDifference::Kind::Removed;
                else if (av.state == QLatin1String("absent") && bv.state == QLatin1String("read"))
                    row.kind = SnapshotDifference::Kind::Added;
                else if (av.state == QLatin1String("read") && bv.state == QLatin1String("absent"))
                    row.kind = SnapshotDifference::Kind::Removed;
                else if (key.endsWith(QLatin1String("/enabled")))
                {
                    const auto enabled = [](const QString& value) { return value == QLatin1String("[true]") || value == QLatin1String("[\"1\"]"); };
                    const auto disabled = [](const QString& value) { return value == QLatin1String("[false]") || value == QLatin1String("[\"0\"]"); };
                    if (disabled(av.value) && enabled(bv.value)) row.kind = SnapshotDifference::Kind::Enabled;
                    else if (enabled(av.value) && disabled(bv.value)) row.kind = SnapshotDifference::Kind::Disabled;
                }
                differences.push_back(row);
            }
        }
        return differences;
    }
}
