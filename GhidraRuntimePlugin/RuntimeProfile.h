#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>

namespace ks::plugin_host::ghidra_runtime
{
    struct RuntimeAsset
    {
        QString name;
        QUrl url;
        QString sha256;
        QString rootDirectory;
        QString destinationDirectory;
        qint64 maxArchiveBytes = 0;
    };

    // Fixed upstream assets, never latest-version or catalog-controlled URLs.
    QList<RuntimeAsset> assets();
    QJsonObject manifest();
    QJsonObject assetDescription();
    QByteArray licenseText();
    QByteArray noticeText();
    // Creates only new metadata files in the caller-owned staging directory.
    bool writePackageMetadata(const QString& pluginDirectory, QString* error = nullptr);
    // Validates an extracted package without executing Java/Ghidra or samples.
    bool validateDirectory(const QString& pluginDirectory, QString* error = nullptr);
}
