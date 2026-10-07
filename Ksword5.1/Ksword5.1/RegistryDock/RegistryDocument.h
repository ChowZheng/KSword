#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

struct RegistryDocumentValue
{
    QString keyPath;
    QString name;
    quint32 type = 0;
    QByteArray data;
    bool deleteValue = false;
};

struct RegistryDocumentKey
{
    QString path;
    bool deleteTree = false;
    // Optional self-relative OWNER/GROUP/DACL metadata. No SACL is requested.
    // An empty array means security metadata was not available; restoration is opt-in.
    QByteArray securityDescriptor;
};

struct RegistryDocumentOperation
{
    enum class Kind { Key, Value };
    Kind kind = Kind::Key;
    qsizetype index = 0;
};

struct RegistryDocument
{
    QString rootPath;
    // 0 = native view, 32 = KEY_WOW64_32KEY, 64 = KEY_WOW64_64KEY.
    // .reg has no view metadata; its parser always returns 0 for the host to bind.
    int viewBits = 0;
    QVector<RegistryDocumentKey> keys;
    QVector<RegistryDocumentValue> values;
    // Every .reg operation is represented exactly once, in source order.
    // A backup captured here also has an order. Empty order on host-created documents
    // means keys and their values are grouped in keys-vector order on .reg export.
    QVector<RegistryDocumentOperation> operationOrder;
};

class RegistryDocumentService final
{
public:
    static bool parseRegFile(const QString& path, RegistryDocument& document, QString& error);
    static bool saveRegFile(const QString& path, const RegistryDocument& document, QString& error);
    static bool saveBackup(const QString& path, const RegistryDocument& document, QString& error);
    static bool loadBackup(const QString& path, RegistryDocument& document, QString& error);
    // Same strict codec, without temporary files, for change-journal segments.
    static bool encodeBackup(const RegistryDocument& document, QByteArray& bytes, QString& error);
    static bool decodeBackup(const QByteArray& bytes, RegistryDocument& document, QString& error);
    // Read-only recursive capture. On any failure the output is cleared. Key/value
    // data is never truncated; access, change detection and budget failures are errors.
    // Captures raw registry links, instead of traversing their targets.
    static bool captureWin32(const QString& rootPath, int viewBits,
        RegistryDocument& document, QString& error,
        qint64 maximumDataBytes = 128LL * 1024 * 1024);
};
