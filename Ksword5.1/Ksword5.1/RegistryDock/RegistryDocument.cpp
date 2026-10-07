#include "RegistryDocument.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringConverter>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{
constexpr qint64 kRegFileLimit = 64LL * 1024 * 1024;
constexpr qint64 kBackupFileLimit = 192LL * 1024 * 1024;
constexpr qsizetype kValueBytesLimit = 16LL * 1024 * 1024;
constexpr qint64 kTotalBytesLimit = 128LL * 1024 * 1024;
constexpr qsizetype kKeyLimit = 100000;
constexpr qsizetype kValueLimit = 250000;
constexpr int kDepthLimit = 256;
constexpr qsizetype kPathLimit = 32767;
constexpr qsizetype kNameLimit = 16383;

bool fail(QString& error, const QString& message)
{
    error = message;
    return false;
}

QString canonicalRoot(const QString& name)
{
    if (name.compare(QStringLiteral("HKEY_CLASSES_ROOT"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("HKCR"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("HKEY_CLASSES_ROOT");
    if (name.compare(QStringLiteral("HKEY_CURRENT_USER"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("HKCU"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("HKEY_CURRENT_USER");
    if (name.compare(QStringLiteral("HKEY_LOCAL_MACHINE"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("HKLM"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("HKEY_LOCAL_MACHINE");
    if (name.compare(QStringLiteral("HKEY_USERS"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("HKU"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("HKEY_USERS");
    if (name.compare(QStringLiteral("HKEY_CURRENT_CONFIG"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("HKCC"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("HKEY_CURRENT_CONFIG");
    return {};
}

bool normalizePath(const QString& source, QString& path, QString& error)
{
    if (source.isEmpty() || source.size() > kPathLimit || !source.isValidUtf16() || source.contains(QChar(0))
        || source.endsWith(QLatin1Char('\\')))
        return fail(error, QStringLiteral("Invalid registry key path: %1").arg(source.left(240)));
    const qsizetype slash = source.indexOf(QLatin1Char('\\'));
    const QString root = canonicalRoot(slash < 0 ? source : source.left(slash));
    if (root.isEmpty())
        return fail(error, QStringLiteral("Unsupported registry root: %1").arg(source.left(240)));
    path = root;
    if (slash >= 0) {
        const QString subkey = source.mid(slash + 1);
        const QStringList parts = subkey.split(QLatin1Char('\\'));
        if (parts.size() > kDepthLimit)
            return fail(error, QStringLiteral("Registry key depth exceeds the backup limit."));
        for (const QString& part : parts) {
            if (part.isEmpty() || part.size() > 255)
                return fail(error, QStringLiteral("Invalid registry key component: %1").arg(source.left(240)));
        }
        path += QLatin1Char('\\') + subkey;
    }
    if (path.size() > kPathLimit)
        return fail(error, QStringLiteral("Registry key path exceeds the length limit."));
    return true;
}

bool insideRoot(const QString& path, const QString& root)
{
    return path.compare(root, Qt::CaseInsensitive) == 0
        || path.startsWith(root + QLatin1Char('\\'), Qt::CaseInsensitive);
}

bool readBounded(const QString& path, qint64 limit, QByteArray& bytes, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("Cannot open registry document: %1").arg(file.errorString()));
    if (file.size() < 0 || file.size() > limit)
        return fail(error, QStringLiteral("Registry document exceeds the file size limit."));
    bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError)
        return fail(error, QStringLiteral("Cannot read registry document: %1").arg(file.errorString()));
    if (bytes.size() > limit || !file.atEnd())
        return fail(error, QStringLiteral("Registry document exceeds the file size limit."));
    return true;
}

bool atomicWrite(const QString& path, const QByteArray& bytes, QString& error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return fail(error, QStringLiteral("Cannot write registry document: %1").arg(file.errorString()));
    if (!file.commit())
        return fail(error, QStringLiteral("Cannot save registry document: %1").arg(file.errorString()));
    error.clear();
    return true;
}

QByteArray utf16LittleEndian(const QString& text, bool terminator)
{
    QByteArray bytes;
    bytes.resize((text.size() + (terminator ? 1 : 0)) * 2);
    for (qsizetype i = 0; i < text.size(); ++i)
        qToLittleEndian<quint16>(text.at(i).unicode(), bytes.data() + i * 2);
    if (terminator)
        qToLittleEndian<quint16>(0, bytes.data() + text.size() * 2);
    return bytes;
}

bool decodeText(const QByteArray& bytes, QString& text, QString& error)
{
    QByteArray payload = bytes;
    if (payload.startsWith(QByteArray::fromHex("fffe"))) {
        payload.remove(0, 2);
        if (payload.size() % 2 != 0)
            return fail(error, QStringLiteral("Malformed UTF-16 registry document."));
        QStringDecoder decoder(QStringDecoder::Utf16LE, QStringConverter::Flag::Stateless);
        text = decoder.decode(payload);
        if (decoder.hasError())
            return fail(error, QStringLiteral("Malformed UTF-16 registry document."));
    } else if (payload.startsWith(QByteArray::fromHex("feff"))) {
        return fail(error, QStringLiteral("UTF-16 big-endian registry documents are unsupported."));
    } else {
        const bool utf8Bom = payload.startsWith(QByteArray::fromHex("efbbbf"));
        if (utf8Bom)
            payload.remove(0, 3);
        QStringDecoder utf8(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        const QString utf8Text = utf8.decode(payload);
        // REGEDIT4 without a BOM uses the Windows ANSI code page. Version 5
        // documents without a BOM accept valid UTF-8, then fall back to ANSI.
        if (utf8Bom || (!payload.startsWith("REGEDIT4") && !utf8.hasError())) {
            if (utf8.hasError())
                return fail(error, QStringLiteral("Malformed UTF-8 registry document."));
            text = utf8Text;
        } else {
#ifdef Q_OS_WIN
            const int count = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS,
                payload.constData(), static_cast<int>(payload.size()), nullptr, 0);
            if (count <= 0)
                return fail(error, QStringLiteral("Cannot decode the ANSI registry document."));
            std::vector<wchar_t> decoded(static_cast<size_t>(count));
            if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, payload.constData(),
                static_cast<int>(payload.size()), decoded.data(), count) != count)
                return fail(error, QStringLiteral("Cannot decode the ANSI registry document."));
            text = QString::fromWCharArray(decoded.data(), count);
#else
            text = QString::fromLocal8Bit(payload);
#endif
        }
    }
    if (text.contains(QChar(0)))
        return fail(error, QStringLiteral("Registry document text contains a null character."));
    return true;
}

bool quoted(const QString& source, qsizetype& position, QString& result, QString& error)
{
    if (position >= source.size() || source.at(position) != QLatin1Char('"'))
        return fail(error, QStringLiteral("Expected a quoted registry string."));
    ++position;
    result.clear();
    while (position < source.size()) {
        const QChar next = source.at(position++);
        if (next == QLatin1Char('"'))
            return true;
        if (next == QLatin1Char('\\')) {
            if (position == source.size())
                return fail(error, QStringLiteral("Incomplete registry string escape."));
            const QChar escaped = source.at(position++);
            if (escaped != QLatin1Char('"') && escaped != QLatin1Char('\\'))
                return fail(error, QStringLiteral("Unsupported registry string escape."));
            result += escaped;
        } else {
            result += next;
        }
    }
    return fail(error, QStringLiteral("Unterminated registry string."));
}

QString escapeQuoted(const QString& source)
{
    QString result = source;
    result.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    result.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + result + QLatin1Char('"');
}

bool valueDataPosition(const QString& line, qsizetype& pos, QString& name, QString& error)
{
    pos = 0;
    if (line.startsWith(QLatin1Char('@')))
        pos = 1;
    else if (!quoted(line, pos, name, error))
        return false;
    while (pos < line.size() && line.at(pos).isSpace())
        ++pos;
    if (pos >= line.size() || line.at(pos++) != QLatin1Char('='))
        return fail(error, QStringLiteral("Registry value is missing its equals sign."));
    return true;
}

bool parseValueData(const QString& input, RegistryDocumentValue& value, QString& error)
{
    const QString data = input.trimmed();
    if (data == QStringLiteral("-")) {
        value.deleteValue = true;
        return true;
    }
    if (data.startsWith(QLatin1Char('"'))) {
        qsizetype pos = 0;
        QString stringData;
        if (!quoted(data, pos, stringData, error))
            return false;
        if (!data.mid(pos).trimmed().isEmpty())
            return fail(error, QStringLiteral("Unexpected data after a registry string."));
        value.type = 1; // REG_SZ
        value.data = utf16LittleEndian(stringData, true);
    } else if (data.startsWith(QStringLiteral("dword:"), Qt::CaseInsensitive)) {
        const QString number = data.mid(6).trimmed();
        static const QRegularExpression dwordSyntax(QStringLiteral("^[0-9a-fA-F]{1,8}$"));
        if (!dwordSyntax.match(number).hasMatch())
            return fail(error, QStringLiteral("Invalid DWORD registry data."));
        value.type = 4; // REG_DWORD
        value.data.resize(4);
        qToLittleEndian<quint32>(number.toUInt(nullptr, 16), value.data.data());
    } else {
        static const QRegularExpression hexSyntax(
            QStringLiteral("^hex(?:\\(([0-9a-fA-F]{1,8})\\))?\\s*:(.*)$"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = hexSyntax.match(data);
        if (!match.hasMatch())
            return fail(error, QStringLiteral("Unsupported registry value data syntax."));
        value.type = match.captured(1).isEmpty() ? 3 : match.captured(1).toUInt(nullptr, 16);
        const QString hexBytes = match.captured(2).trimmed();
        if (!hexBytes.isEmpty()) {
            static const QRegularExpression byteSyntax(QStringLiteral("^[0-9a-fA-F]{2}$"));
            qsizetype start = 0;
            while (start <= hexBytes.size()) {
                const qsizetype comma = hexBytes.indexOf(QLatin1Char(','), start);
                const QString byte = hexBytes.mid(start, comma < 0 ? -1 : comma - start).trimmed();
                if (!byteSyntax.match(byte).hasMatch())
                    return fail(error, QStringLiteral("Invalid hexadecimal registry byte."));
                if (value.data.size() >= kValueBytesLimit)
                    return fail(error, QStringLiteral("Registry value exceeds the data size limit."));
                value.data += static_cast<char>(byte.toUInt(nullptr, 16));
                if (comma < 0)
                    break;
                start = comma + 1;
            }
        }
    }
    if (value.data.size() > kValueBytesLimit)
        return fail(error, QStringLiteral("Registry value exceeds the data size limit."));
    return true;
}

bool checkOrder(const RegistryDocument& doc, QString& error)
{
    if (doc.operationOrder.isEmpty())
        return true;
    if (doc.operationOrder.size() != doc.keys.size() + doc.values.size())
        return fail(error, QStringLiteral("Registry document operation order is incomplete."));
    QSet<qsizetype> seenKeys;
    QSet<qsizetype> seenValues;
    for (const auto& operation : doc.operationOrder) {
        if (operation.kind == RegistryDocumentOperation::Kind::Key) {
            if (operation.index < 0 || operation.index >= doc.keys.size()
                || seenKeys.contains(operation.index))
                return fail(error, QStringLiteral("Registry document has an invalid key operation."));
            seenKeys.insert(operation.index);
        } else if (operation.kind == RegistryDocumentOperation::Kind::Value) {
            if (operation.index < 0 || operation.index >= doc.values.size()
                || seenValues.contains(operation.index))
                return fail(error, QStringLiteral("Registry document has an invalid value operation."));
            seenValues.insert(operation.index);
        } else {
            return fail(error, QStringLiteral("Registry document has an unknown operation kind."));
        }
    }
    return true;
}

bool validSecurityDescriptor(const QByteArray& bytes)
{
    if (bytes.isEmpty())
        return true;
    // Check all self-relative offsets before any host restoration API can inspect
    // untrusted metadata. No pointer-based Windows validation is used on file input.
    if (bytes.size() < 20 || static_cast<unsigned char>(bytes.at(0)) != 1
        || (qFromLittleEndian<quint16>(bytes.constData() + 2) & 0x8000U) == 0)
        return false;
    auto validSid = [&](quint32 offset) {
        if (offset == 0)
            return true;
        if (offset < 20 || offset % 4 != 0 || offset > static_cast<quint32>(bytes.size() - 8))
            return false;
        const auto count = static_cast<unsigned char>(bytes.at(offset + 1));
        return static_cast<unsigned char>(bytes.at(offset)) == 1 && count <= 15
            && static_cast<quint64>(offset) + 8 + count * 4 <= static_cast<quint64>(bytes.size());
    };
    auto validAcl = [&](quint32 offset) {
        if (offset == 0)
            return true;
        if (offset < 20 || offset % 4 != 0 || offset > static_cast<quint32>(bytes.size() - 8))
            return false;
        const auto revision = static_cast<unsigned char>(bytes.at(offset));
        const quint16 size = qFromLittleEndian<quint16>(bytes.constData() + offset + 2);
        const quint16 count = qFromLittleEndian<quint16>(bytes.constData() + offset + 4);
        if ((revision != 2 && revision != 4) || size < 8
            || static_cast<quint64>(offset) + size > static_cast<quint64>(bytes.size()))
            return false;
        quint32 position = offset + 8;
        const quint32 end = offset + size;
        for (quint32 i = 0; i < count; ++i) {
            if (position > end - 4)
                return false;
            const quint16 aceSize = qFromLittleEndian<quint16>(bytes.constData() + position + 2);
            if (aceSize < 4 || aceSize % 4 != 0 || aceSize > end - position)
                return false;
            position += aceSize;
        }
        return true;
    };
    return validSid(qFromLittleEndian<quint32>(bytes.constData() + 4))
        && validSid(qFromLittleEndian<quint32>(bytes.constData() + 8))
        && validAcl(qFromLittleEndian<quint32>(bytes.constData() + 12))
        && validAcl(qFromLittleEndian<quint32>(bytes.constData() + 16));
}

bool validateDocument(const RegistryDocument& doc, bool backup, QString& error)
{
    if (doc.viewBits != 0 && doc.viewBits != 32 && doc.viewBits != 64)
        return fail(error, QStringLiteral("Invalid registry view; select native, 32-bit or 64-bit."));
    if (doc.keys.isEmpty() || doc.keys.size() > kKeyLimit || doc.values.size() > kValueLimit)
        return fail(error, QStringLiteral("Registry document key or value count exceeds the limit."));
    QString root;
    if ((!doc.rootPath.isEmpty() || backup) && !normalizePath(doc.rootPath, root, error))
        return false;
    QSet<QString> keys;
    qint64 totalBytes = 0;
    for (const auto& key : doc.keys) {
        QString path;
        if (!normalizePath(key.path, path, error))
            return false;
        if (backup && (key.deleteTree || !insideRoot(path, root) || keys.contains(path.toCaseFolded())))
            return fail(error, QStringLiteral("Backup contains an invalid or duplicate key."));
        if (key.deleteTree && path.indexOf(QLatin1Char('\\')) < 0)
            return fail(error, QStringLiteral("Deleting an entire registry root is unsupported."));
        if (key.securityDescriptor.size() > 1024 * 1024)
            return fail(error, QStringLiteral("Registry security metadata exceeds the size limit."));
        if (!validSecurityDescriptor(key.securityDescriptor))
            return fail(error, QStringLiteral("Registry backup contains invalid security metadata."));
        totalBytes += key.securityDescriptor.size();
        keys.insert(path.toCaseFolded());
    }
    if (backup && !keys.contains(root.toCaseFolded()))
        return fail(error, QStringLiteral("Backup does not contain its root key."));
    if (backup) {
        for (const auto& key : doc.keys) {
            QString path;
            if (!normalizePath(key.path, path, error))
                return false;
            if (path.compare(root, Qt::CaseInsensitive) == 0)
                continue;
            const qsizetype slash = path.lastIndexOf(QLatin1Char('\\'));
            if (!keys.contains(path.left(slash).toCaseFolded()))
                return fail(error, QStringLiteral("Backup is missing a parent key."));
        }
    }
    QHash<QString, QSet<QString>> valueNames;
    for (const auto& value : doc.values) {
        QString path;
        if (!normalizePath(value.keyPath, path, error))
            return false;
        if (value.name.size() > kNameLimit || !value.name.isValidUtf16() || value.name.contains(QChar(0)))
            return fail(error, QStringLiteral("Invalid registry value name."));
        if (value.data.size() > kValueBytesLimit || (value.deleteValue && !value.data.isEmpty()))
            return fail(error, QStringLiteral("Registry value exceeds the data size limit."));
        if (backup && (value.deleteValue || !insideRoot(path, root)
            || !keys.contains(path.toCaseFolded())))
            return fail(error, QStringLiteral("Backup contains a value without its key."));
        const QString foldedPath = path.toCaseFolded();
        const QString foldedName = value.name.toCaseFolded();
        if (backup && valueNames[foldedPath].contains(foldedName))
            return fail(error, QStringLiteral("Backup contains duplicate value names."));
        valueNames[foldedPath].insert(foldedName);
        totalBytes += value.data.size();
        if (totalBytes > kTotalBytesLimit)
            return fail(error, QStringLiteral("Registry document exceeds the total data budget."));
    }
    if (totalBytes > kTotalBytesLimit)
        return fail(error, QStringLiteral("Registry document exceeds the total data budget."));
    return checkOrder(doc, error);
}

bool integer(const QJsonValue& value, quint32& number)
{
    if (!value.isDouble())
        return false;
    const double d = value.toDouble();
    if (!std::isfinite(d) || d < 0 || d > std::numeric_limits<quint32>::max()
        || std::floor(d) != d)
        return false;
    number = static_cast<quint32>(d);
    return true;
}

bool decodeBase64(const QJsonValue& value, qsizetype limit, QByteArray& bytes, QString& error)
{
    if (!value.isString())
        return fail(error, QStringLiteral("Backup data must use a base64 string."));
    const QString string = value.toString();
    if (string.size() > ((limit + 2) / 3) * 4)
        return fail(error, QStringLiteral("Backup value exceeds the data size limit."));
    const QByteArray encoded = string.toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > limit || decoded.decoded.toBase64() != encoded)
        return fail(error, QStringLiteral("Backup contains invalid base64 data."));
    bytes = decoded.decoded;
    return true;
}
} // namespace

bool RegistryDocumentService::parseRegFile(const QString& path, RegistryDocument& document, QString& error)
{
    document = {};
    error.clear();
    QByteArray bytes;
    QString text;
    if (!readBounded(path, kRegFileLimit, bytes, error) || !decodeText(bytes, text, error))
        return false;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (text.contains(QLatin1Char('\r')))
        return fail(error, QStringLiteral("Registry document has invalid line endings."));
    if (text.count(QLatin1Char('\n')) > 2000000)
        return fail(error, QStringLiteral("Registry document exceeds the physical line count limit."));
    const QStringList physicalLines = text.split(QLatin1Char('\n'));
    if (physicalLines.isEmpty() || (physicalLines.first().trimmed() != QStringLiteral("Windows Registry Editor Version 5.00")
        && physicalLines.first().trimmed() != QStringLiteral("REGEDIT4")))
        return fail(error, QStringLiteral("Registry document has an unsupported header."));
    RegistryDocument parsed;
    QString currentPath;
    bool deletedSection = false;
    qint64 total = 0;
    auto lineError = [&](qsizetype number, const QString& detail) {
        return fail(error, QStringLiteral("Registry document line %1: %2").arg(number + 1).arg(detail));
    };
    for (qsizetype i = 1; i < physicalLines.size(); ++i) {
        const qsizetype firstLine = i;
        QString line = physicalLines.at(i).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char(';')))
            continue;
        // Continuations apply only to hex payloads, never names or strings.
        if (line.endsWith(QLatin1Char('\\'))) {
            qsizetype dataPosition = 0;
            QString unusedName, detail;
            if (!valueDataPosition(line, dataPosition, unusedName, detail)
                || !line.mid(dataPosition).trimmed().startsWith(QStringLiteral("hex"), Qt::CaseInsensitive))
                return lineError(firstLine, QStringLiteral("Only hexadecimal registry data may continue onto another line."));
            while (line.endsWith(QLatin1Char('\\'))) {
                line.chop(1);
                if (++i >= physicalLines.size())
                    return lineError(firstLine, QStringLiteral("Unterminated registry data continuation."));
                const QString continuation = physicalLines.at(i).trimmed();
                if (continuation.isEmpty() || continuation.startsWith(QLatin1Char(';')))
                    return lineError(i, QStringLiteral("Empty registry data continuation."));
                line += continuation;
            }
        }
        if (line.startsWith(QLatin1Char('['))) {
            if (!line.endsWith(QLatin1Char(']')))
                return lineError(firstLine, QStringLiteral("Malformed registry key section."));
            QString section = line.mid(1, line.size() - 2);
            deletedSection = section.startsWith(QLatin1Char('-'));
            if (deletedSection)
                section.remove(0, 1);
            QString detail;
            if (!normalizePath(section, currentPath, detail))
                return lineError(firstLine, detail);
            if (deletedSection && currentPath.indexOf(QLatin1Char('\\')) < 0)
                return lineError(firstLine, QStringLiteral("Deleting an entire registry root is unsupported."));
            parsed.operationOrder.append({ RegistryDocumentOperation::Kind::Key, parsed.keys.size() });
            parsed.keys.append({ currentPath, deletedSection, {} });
            if (parsed.keys.size() > kKeyLimit)
                return lineError(firstLine, QStringLiteral("Registry document exceeds the key count limit."));
            continue;
        }
        if (currentPath.isEmpty() || deletedSection)
            return lineError(firstLine, QStringLiteral("Registry value has no writable key section."));
        RegistryDocumentValue value;
        value.keyPath = currentPath;
        qsizetype pos = 0;
        QString detail;
        if (!valueDataPosition(line, pos, value.name, detail))
            return lineError(firstLine, detail);
        if (!parseValueData(line.mid(pos), value, detail))
            return lineError(firstLine, detail);
        total += value.data.size();
        if (total > kTotalBytesLimit || parsed.values.size() >= kValueLimit)
            return lineError(firstLine, QStringLiteral("Registry document exceeds the value count or data budget."));
        parsed.operationOrder.append({ RegistryDocumentOperation::Kind::Value, parsed.values.size() });
        parsed.values.append(std::move(value));
    }
    if (!validateDocument(parsed, false, error))
        return false;
    if (parsed.keys.size() == 1 && !parsed.keys.first().deleteTree)
        parsed.rootPath = parsed.keys.first().path;
    document = std::move(parsed);
    return true;
}

bool RegistryDocumentService::saveRegFile(const QString& path, const RegistryDocument& document, QString& error)
{
    error.clear();
    if (!validateDocument(document, false, error))
        return false;
    QString text = QStringLiteral("Windows Registry Editor Version 5.00\r\n\r\n");
    QString currentKey;
    bool currentDeleted = false;
    auto writeKey = [&](const RegistryDocumentKey& key) -> bool {
        // A closing bracket cannot be represented by regedit section syntax.
        if (key.path.contains(QLatin1Char(']')) || key.path.contains(QLatin1Char('\r'))
            || key.path.contains(QLatin1Char('\n')))
            return fail(error, QStringLiteral("Registry key cannot be represented in a .reg section."));
        text += QLatin1Char('[') + (key.deleteTree ? QStringLiteral("-") : QString())
            + key.path + QStringLiteral("]\r\n");
        currentKey = key.path;
        currentDeleted = key.deleteTree;
        return true;
    };
    auto writeValue = [&](const RegistryDocumentValue& value) -> bool {
        if (value.name.contains(QLatin1Char('\r')) || value.name.contains(QLatin1Char('\n')))
            return fail(error, QStringLiteral("Registry value name cannot be represented in a .reg file."));
        if (currentDeleted || currentKey.compare(value.keyPath, Qt::CaseInsensitive) != 0) {
            text += QStringLiteral("\r\n");
            if (!writeKey({ value.keyPath, false, {} }))
                return false;
        }
        text += (value.name.isEmpty() ? QStringLiteral("@") : escapeQuoted(value.name)) + QLatin1Char('=');
        if (value.deleteValue) {
            text += QStringLiteral("-\r\n");
            return true;
        }
        const qint64 predictedChars = static_cast<qint64>(text.size()) + 20
            + static_cast<qint64>(value.data.size()) * 3
            + static_cast<qint64>(value.data.size()) / 24 * 5;
        if (predictedChars > (kRegFileLimit - 2) / 2)
            return fail(error, QStringLiteral("Registry export exceeds the .reg file size limit."));
        text += QStringLiteral("hex(%1):").arg(value.type, 0, 16);
        for (qsizetype i = 0; i < value.data.size(); ++i) {
            if (i != 0)
                text += (i % 24 == 0 ? QStringLiteral(",\\\r\n  ") : QStringLiteral(","));
            text += QStringLiteral("%1").arg(static_cast<unsigned char>(value.data.at(i)), 2, 16, QLatin1Char('0'));
        }
        text += QStringLiteral("\r\n");
        if (text.size() > kRegFileLimit / 2)
            return fail(error, QStringLiteral("Registry export exceeds the .reg file size limit."));
        return true;
    };
    if (!document.operationOrder.isEmpty()) {
        for (const auto& operation : document.operationOrder) {
            if (operation.kind == RegistryDocumentOperation::Kind::Key) {
                text += QStringLiteral("\r\n");
                if (!writeKey(document.keys.at(operation.index)))
                    return false;
            } else if (!writeValue(document.values.at(operation.index))) {
                return false;
            }
        }
    } else {
        QSet<qsizetype> writtenValues;
        QHash<QString, QVector<qsizetype>> valuesByKey;
        for (qsizetype i = 0; i < document.values.size(); ++i)
            valuesByKey[document.values.at(i).keyPath.toCaseFolded()].append(i);
        for (const auto& key : document.keys) {
            text += QStringLiteral("\r\n");
            if (!writeKey(key))
                return false;
            for (qsizetype i : valuesByKey.value(key.path.toCaseFolded())) {
                if (key.deleteTree)
                    return fail(error, QStringLiteral("Registry export needs an operation order for deleted keys with values."));
                if (!writeValue(document.values.at(i)))
                    return false;
                writtenValues.insert(i);
            }
        }
        for (qsizetype i = 0; i < document.values.size(); ++i) {
            if (!writtenValues.contains(i) && !writeValue(document.values.at(i)))
                return false;
        }
    }
    if (text.size() > (kRegFileLimit - 2) / 2)
        return fail(error, QStringLiteral("Registry export exceeds the .reg file size limit."));
    return atomicWrite(path, QByteArray::fromHex("fffe") + utf16LittleEndian(text, false), error);
}

bool RegistryDocumentService::saveBackup(const QString& path, const RegistryDocument& document, QString& error)
{
    QByteArray bytes;
    return encodeBackup(document, bytes, error) && atomicWrite(path, bytes, error);
}

bool RegistryDocumentService::encodeBackup(const RegistryDocument& document, QByteArray& bytes, QString& error)
{
    error.clear();
    bytes.clear();
    if (!validateDocument(document, true, error))
        return false;
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("KSword.RegistryBackup"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("rootPath"), document.rootPath);
    root.insert(QStringLiteral("viewBits"), document.viewBits);
    QJsonArray keys;
    for (const auto& key : document.keys) {
        QJsonObject object;
        object.insert(QStringLiteral("path"), key.path);
        if (!key.securityDescriptor.isEmpty())
            object.insert(QStringLiteral("securityDescriptorBase64"), QString::fromLatin1(key.securityDescriptor.toBase64()));
        keys.append(object);
    }
    QJsonArray values;
    for (const auto& value : document.values) {
        QJsonObject object;
        object.insert(QStringLiteral("keyPath"), value.keyPath);
        object.insert(QStringLiteral("name"), value.name);
        object.insert(QStringLiteral("type"), static_cast<double>(value.type));
        object.insert(QStringLiteral("dataBase64"), QString::fromLatin1(value.data.toBase64()));
        values.append(object);
    }
    root.insert(QStringLiteral("keys"), keys);
    root.insert(QStringLiteral("values"), values);
    bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (bytes.size() > kBackupFileLimit)
        return fail(error, QStringLiteral("Registry backup exceeds the file size limit."));
    return true;
}

bool RegistryDocumentService::loadBackup(const QString& path, RegistryDocument& document, QString& error)
{
    document = {};
    error.clear();
    QByteArray bytes;
    if (!readBounded(path, kBackupFileLimit, bytes, error))
        return false;
    return decodeBackup(bytes, document, error);
}

bool RegistryDocumentService::decodeBackup(const QByteArray& bytes, RegistryDocument& document, QString& error)
{
    document = {};
    error.clear();
    if (bytes.size() > kBackupFileLimit)
        return fail(error, QStringLiteral("Registry backup exceeds the file size limit."));
    QJsonParseError jsonError;
    const QJsonDocument json = QJsonDocument::fromJson(bytes, &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !json.isObject())
        return fail(error, QStringLiteral("Invalid registry backup JSON: %1").arg(jsonError.errorString()));
    const QJsonObject root = json.object();
    quint32 version = 0;
    quint32 view = 0;
    if (root.value(QStringLiteral("format")) != QJsonValue(QStringLiteral("KSword.RegistryBackup"))
        || !integer(root.value(QStringLiteral("version")), version) || version != 1
        || !integer(root.value(QStringLiteral("viewBits")), view) || (view != 0 && view != 32 && view != 64)
        || !root.value(QStringLiteral("rootPath")).isString()
        || !root.value(QStringLiteral("keys")).isArray() || !root.value(QStringLiteral("values")).isArray())
        return fail(error, QStringLiteral("Unsupported registry backup format, version or structure."));
    RegistryDocument loaded;
    loaded.rootPath = root.value(QStringLiteral("rootPath")).toString();
    loaded.viewBits = static_cast<int>(view);
    const QJsonArray keys = root.value(QStringLiteral("keys")).toArray();
    const QJsonArray values = root.value(QStringLiteral("values")).toArray();
    if (keys.size() > kKeyLimit || values.size() > kValueLimit)
        return fail(error, QStringLiteral("Registry backup exceeds the key or value count limit."));
    qint64 total = 0;
    for (const QJsonValue& entry : keys) {
        const QJsonObject object = entry.toObject();
        if (!entry.isObject() || !object.value(QStringLiteral("path")).isString()
            || object.contains(QStringLiteral("deleteTree")))
            return fail(error, QStringLiteral("Invalid key entry in registry backup."));
        RegistryDocumentKey key;
        key.path = object.value(QStringLiteral("path")).toString();
        if (object.contains(QStringLiteral("securityDescriptorBase64"))
            && !decodeBase64(object.value(QStringLiteral("securityDescriptorBase64")),
                1024 * 1024, key.securityDescriptor, error))
            return false;
        total += key.securityDescriptor.size();
        if (total > kTotalBytesLimit)
            return fail(error, QStringLiteral("Registry backup exceeds the total data budget."));
        loaded.keys.append(std::move(key));
    }
    for (const QJsonValue& entry : values) {
        const QJsonObject object = entry.toObject();
        RegistryDocumentValue value;
        if (!entry.isObject() || !object.value(QStringLiteral("keyPath")).isString()
            || !object.value(QStringLiteral("name")).isString()
            || !integer(object.value(QStringLiteral("type")), value.type)
            || object.contains(QStringLiteral("deleteValue")))
            return fail(error, QStringLiteral("Invalid value entry in registry backup."));
        value.keyPath = object.value(QStringLiteral("keyPath")).toString();
        value.name = object.value(QStringLiteral("name")).toString();
        if (!decodeBase64(object.value(QStringLiteral("dataBase64")), kValueBytesLimit, value.data, error))
            return false;
        total += value.data.size();
        if (total > kTotalBytesLimit)
            return fail(error, QStringLiteral("Registry backup exceeds the total data budget."));
        loaded.values.append(std::move(value));
    }
    if (!validateDocument(loaded, true, error))
        return false;
    document = std::move(loaded);
    return true;
}

#ifdef Q_OS_WIN
namespace
{
class RegistryKey final
{
public:
    ~RegistryKey() { if (handle) RegCloseKey(handle); }
    HKEY handle = nullptr;
};

HKEY nativeRoot(const QString& root)
{
    if (root == QStringLiteral("HKEY_CLASSES_ROOT")) return HKEY_CLASSES_ROOT;
    if (root == QStringLiteral("HKEY_CURRENT_USER")) return HKEY_CURRENT_USER;
    if (root == QStringLiteral("HKEY_LOCAL_MACHINE")) return HKEY_LOCAL_MACHINE;
    if (root == QStringLiteral("HKEY_USERS")) return HKEY_USERS;
    if (root == QStringLiteral("HKEY_CURRENT_CONFIG")) return HKEY_CURRENT_CONFIG;
    return nullptr;
}

bool windowsError(QString& error, const QString& path, LSTATUS status)
{
    return fail(error, QStringLiteral("Registry capture failed at %1 (Win32 %2).").arg(path).arg(status));
}

LSTATUS openCaptureKey(HKEY parent, LPCWSTR name, REGSAM view, HKEY* result)
{
    LSTATUS status = RegOpenKeyExW(parent, name, REG_OPTION_OPEN_LINK, KEY_READ | view, result);
    if (status == ERROR_ACCESS_DENIED)
        status = RegOpenKeyExW(parent, name, REG_OPTION_OPEN_LINK,
            KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | view, result);
    return status;
}

struct CaptureContext
{
    RegistryDocument document;
    qint64 totalBytes = 0;
    qint64 dataBudget = kTotalBytesLimit;
    REGSAM view = 0;
    QString* error = nullptr;

    bool key(HKEY handle, const QString& path, int depth)
    {
        if (depth > kDepthLimit || document.keys.size() >= kKeyLimit)
            return fail(*error, QStringLiteral("Registry capture exceeds the depth or key count limit."));
        DWORD subkeyCount = 0, valueCount = 0, maxSubkey = 0, maxName = 0, maxData = 0;
        FILETIME startWrite {};
        LSTATUS status = RegQueryInfoKeyW(handle, nullptr, nullptr, nullptr, &subkeyCount,
            &maxSubkey, nullptr, &valueCount, &maxName, &maxData, nullptr, &startWrite);
        if (status != ERROR_SUCCESS)
            return windowsError(*error, path, status);
        if (maxSubkey > 255 || maxName > static_cast<DWORD>(kNameLimit)
            || maxData > static_cast<DWORD>(kValueBytesLimit)
            || subkeyCount > static_cast<DWORD>(kKeyLimit - document.keys.size())
            || valueCount > static_cast<DWORD>(kValueLimit - document.values.size()))
            return fail(*error, QStringLiteral("Registry capture exceeds the key, value or data budget."));
        RegistryDocumentKey capturedKey;
        capturedKey.path = path;
        DWORD securityBytes = 0;
        const SECURITY_INFORMATION securityParts = OWNER_SECURITY_INFORMATION
            | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
        status = RegGetKeySecurity(handle, securityParts, nullptr, &securityBytes);
        if (status == ERROR_INSUFFICIENT_BUFFER && securityBytes <= 1024 * 1024) {
            if (securityBytes > dataBudget - totalBytes)
                return fail(*error, QStringLiteral("Registry capture exceeds the total data budget."));
            QByteArray security(static_cast<qsizetype>(securityBytes), Qt::Uninitialized);
            for (int attempt = 0; attempt < 4; ++attempt) {
                DWORD capacity = static_cast<DWORD>(security.size());
                status = RegGetKeySecurity(handle, securityParts,
                    reinterpret_cast<PSECURITY_DESCRIPTOR>(security.data()), &capacity);
                if (status == ERROR_SUCCESS) {
                    security.resize(capacity);
                    capturedKey.securityDescriptor = std::move(security);
                    break;
                }
                if (status != ERROR_INSUFFICIENT_BUFFER || capacity > 1024 * 1024)
                    break;
                if (capacity > dataBudget - totalBytes)
                    return fail(*error, QStringLiteral("Registry capture exceeds the total data budget."));
                security.resize(capacity);
            }
        }
        totalBytes += capturedKey.securityDescriptor.size();
        if (totalBytes > dataBudget)
            return fail(*error, QStringLiteral("Registry capture exceeds the total data budget."));
        document.operationOrder.append({ RegistryDocumentOperation::Kind::Key, document.keys.size() });
        document.keys.append(std::move(capturedKey));
        DWORD enumeratedValues = 0;
        for (DWORD index = 0;; ++index) {
            std::vector<wchar_t> name(static_cast<size_t>(maxName) + 2);
            QByteArray data(static_cast<qsizetype>(std::min<qint64>(maxData, dataBudget - totalBytes)), Qt::Uninitialized);
            DWORD type = 0;
            DWORD nameChars = 0, dataBytes = 0;
            bool received = false;
            for (int attempt = 0; attempt < 5; ++attempt) {
                nameChars = static_cast<DWORD>(name.size());
                dataBytes = static_cast<DWORD>(data.size());
                status = RegEnumValueW(handle, index, name.data(), &nameChars, nullptr,
                    &type, reinterpret_cast<BYTE*>(data.data()), &dataBytes);
                if (status == ERROR_SUCCESS || status == ERROR_NO_MORE_ITEMS) {
                    received = true;
                    break;
                }
                if (status != ERROR_MORE_DATA)
                    return windowsError(*error, path, status);
                // Data and name may both grow after RegQueryInfoKey. ERROR_MORE_DATA
                // does not provide a reliable new name length, so grow both boundedly.
                if (dataBytes > static_cast<DWORD>(kValueBytesLimit) || dataBytes > dataBudget - totalBytes)
                    return fail(*error, QStringLiteral("Registry capture value exceeds the data size limit."));
                name.resize(std::min<size_t>(static_cast<size_t>(kNameLimit) + 2,
                    std::max(name.size() * 2, static_cast<size_t>(nameChars) + 2)));
                const qsizetype requested = std::max<qsizetype>(dataBytes,
                    std::min<qsizetype>(static_cast<qsizetype>(std::min<qint64>(kValueBytesLimit, dataBudget - totalBytes)),
                        std::max<qsizetype>(data.size() * 2, 256)));
                data.resize(requested);
            }
            if (!received)
                return fail(*error, QStringLiteral("Registry capture could not read a growing value completely."));
            if (status == ERROR_NO_MORE_ITEMS)
                break;
            if (nameChars > static_cast<DWORD>(kNameLimit) || dataBytes > static_cast<DWORD>(data.size()))
                return fail(*error, QStringLiteral("Registry capture returned an invalid value length."));
            if (document.values.size() >= kValueLimit)
                return fail(*error, QStringLiteral("Registry capture exceeds the value count limit."));
            data.resize(dataBytes);
            totalBytes += data.size();
            if (totalBytes > dataBudget)
                return fail(*error, QStringLiteral("Registry capture exceeds the total data budget."));
            RegistryDocumentValue value;
            value.keyPath = path;
            value.name = QString::fromWCharArray(name.data(), static_cast<int>(nameChars));
            value.type = type;
            value.data = std::move(data);
            document.operationOrder.append({ RegistryDocumentOperation::Kind::Value, document.values.size() });
            document.values.append(std::move(value));
            ++enumeratedValues;
        }
        DWORD enumeratedKeys = 0;
        for (DWORD index = 0;; ++index) {
            wchar_t name[257] {};
            DWORD chars = 257;
            status = RegEnumKeyExW(handle, index, name, &chars, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS)
                break;
            if (status != ERROR_SUCCESS)
                return windowsError(*error, path, status);
            const QString childName = QString::fromWCharArray(name, static_cast<int>(chars));
            const QString childPath = path + QLatin1Char('\\') + childName;
            if (childPath.size() > kPathLimit)
                return fail(*error, QStringLiteral("Registry capture key path exceeds the length limit."));
            RegistryKey child;
            status = openCaptureKey(handle, name, view, &child.handle);
            if (status != ERROR_SUCCESS)
                return windowsError(*error, childPath, status);
            if (!key(child.handle, childPath, depth + 1))
                return false;
            ++enumeratedKeys;
        }
        DWORD endSubkeys = 0, endValues = 0;
        FILETIME endWrite {};
        status = RegQueryInfoKeyW(handle, nullptr, nullptr, nullptr, &endSubkeys,
            nullptr, nullptr, &endValues, nullptr, nullptr, nullptr, &endWrite);
        if (status != ERROR_SUCCESS)
            return windowsError(*error, path, status);
        if (enumeratedKeys != subkeyCount || enumeratedValues != valueCount
            || endSubkeys != subkeyCount || endValues != valueCount
            || startWrite.dwHighDateTime != endWrite.dwHighDateTime
            || startWrite.dwLowDateTime != endWrite.dwLowDateTime)
            return fail(*error, QStringLiteral("Registry key changed during capture: %1").arg(path));
        return true;
    }
};
} // namespace
#endif

bool RegistryDocumentService::captureWin32(const QString& rootPath, int viewBits,
    RegistryDocument& document, QString& error, qint64 maximumDataBytes)
{
    document = {};
    error.clear();
    QString normalized;
    if (!normalizePath(rootPath, normalized, error))
        return false;
    if (viewBits != 0 && viewBits != 32 && viewBits != 64)
        return fail(error, QStringLiteral("Invalid registry view; select native, 32-bit or 64-bit."));
    if (maximumDataBytes < 0 || maximumDataBytes > kTotalBytesLimit)
        return fail(error, QStringLiteral("Invalid registry capture data budget."));
#ifdef Q_OS_WIN
    const qsizetype slash = normalized.indexOf(QLatin1Char('\\'));
    const HKEY root = nativeRoot(slash < 0 ? normalized : normalized.left(slash));
    const QString subkey = slash < 0 ? QString() : normalized.mid(slash + 1);
    CaptureContext context;
    context.error = &error;
    context.dataBudget = maximumDataBytes;
    context.view = viewBits == 32 ? KEY_WOW64_32KEY : viewBits == 64 ? KEY_WOW64_64KEY : 0;
    context.document.rootPath = normalized;
    context.document.viewBits = viewBits;
    RegistryKey key;
    if (subkey.isEmpty()) {
        const LSTATUS status = openCaptureKey(root, L"", context.view, &key.handle);
        if (status != ERROR_SUCCESS)
            return windowsError(error, normalized, status);
    } else {
        // OPEN_LINK applies only to the final component of one API call. Open
        // each component separately so an intermediate link cannot redirect the
        // requested backup root outside its apparent subtree.
        const auto components = subkey.split(QLatin1Char('\\'));
        HKEY current = root;
        for (qsizetype i = 0; i < components.size(); ++i) {
            HKEY next = nullptr;
            const bool final = i + 1 == components.size();
            const LSTATUS status = final
                ? openCaptureKey(current, reinterpret_cast<LPCWSTR>(components.at(i).utf16()), context.view, &next)
                : RegOpenKeyExW(current, reinterpret_cast<LPCWSTR>(components.at(i).utf16()), REG_OPTION_OPEN_LINK,
                    KEY_QUERY_VALUE | context.view, &next);
            if (status != ERROR_SUCCESS)
                return windowsError(error, normalized, status);
            if (key.handle)
                RegCloseKey(key.handle);
            key.handle = next;
            current = next;
            if (!final) {
                DWORD type = 0;
                const LSTATUS linkStatus = RegQueryValueExW(current, L"SymbolicLinkValue", nullptr, &type, nullptr, nullptr);
                if (linkStatus == ERROR_SUCCESS && type == REG_LINK)
                    return fail(error, QStringLiteral("Registry capture refuses an intermediate symbolic link."));
                if (linkStatus != ERROR_SUCCESS && linkStatus != ERROR_FILE_NOT_FOUND)
                    return windowsError(error, normalized, linkStatus);
            }
        }
    }
    if (!context.key(key.handle, normalized, 0) || !validateDocument(context.document, true, error))
        return false;
    document = std::move(context.document);
    return true;
#else
    return fail(error, QStringLiteral("Registry capture requires Windows."));
#endif
}
