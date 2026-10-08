#include "RuntimeProfile.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <QtEndian>

#include "RuntimeLicense.inc"

namespace ks::plugin_host::ghidra_runtime
{
    namespace
    {
        bool fail(QString* error, const QString& code)
        {
            if (error) *error = code;
            return false;
        }

        bool insideRoot(const QString& root, const QString& relative, bool directory)
        {
            if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains(u':') ||
                relative.contains(u'\\') || relative.split(u'/').contains(QStringLiteral(".."))) return false;
            const QFileInfo info(QDir(root).filePath(relative));
            if (info.isSymLink() || (directory ? !info.isDir() : !info.isFile()) || (!directory && info.size() <= 0)) return false;
            const QString path = QDir::fromNativeSeparators(info.canonicalFilePath());
            const QString prefix = QDir::fromNativeSeparators(QFileInfo(root).canonicalFilePath()) + u'/';
#ifdef Q_OS_WIN
            return path.startsWith(prefix, Qt::CaseInsensitive);
#else
            return path.startsWith(prefix);
#endif
        }

        bool writeNew(const QString& directory, const QString& name, const QByteArray& bytes)
        {
            QFile file(QDir(directory).filePath(name));
            return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size() && file.flush();
        }

        bool amd64Executable(const QString& path)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() < 64) return false;
            const auto header = file.read(64);
            if (header.size() != 64 || qFromLittleEndian<quint16>(header.constData()) != 0x5a4d) return false;
            const auto offset = qFromLittleEndian<quint32>(header.constData() + 0x3c);
            if (offset > 1024 * 1024 || offset > quint64(file.size()) - 6 || !file.seek(offset)) return false;
            const auto pe = file.read(6);
            return pe.size() == 6 && qFromLittleEndian<quint32>(pe.constData()) == 0x4550 &&
                qFromLittleEndian<quint16>(pe.constData() + 4) == 0x8664;
        }

        QByteArray readBounded(const QString& path)
        {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) && file.size() <= 64 * 1024 ? file.readAll() : QByteArray();
        }
    }

    QList<RuntimeAsset> assets()
    {
        return {
            {QStringLiteral("Ghidra 12.0.4"),
                QUrl(QStringLiteral("https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.0.4_build/ghidra_12.0.4_PUBLIC_20260303.zip")),
                QStringLiteral("c3b458661d69e26e203d739c0c82d143cc8a4a29d9e571f099c2cf4bda62a120"),
                QStringLiteral("ghidra_12.0.4_PUBLIC"), QStringLiteral("runtime"), 1024ll * 1024 * 1024},
            {QStringLiteral("Eclipse Temurin 21.0.12.1+1 x64"),
                QUrl(QStringLiteral("https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip")),
                QStringLiteral("f9d6e191ab098c0d416e7d588a24420a8621cd2f4720dab2459b8b7b2d2d8b4e"),
                QStringLiteral("jdk-21.0.12.1+1"), QStringLiteral("jdk"), 512ll * 1024 * 1024}
        };
    }

    QJsonObject manifest()
    {
        return {
            {QStringLiteral("ksword_plugin_api"), QStringLiteral("1")},
            {QStringLiteral("id"), QStringLiteral("ghidra")},
            {QStringLiteral("name"), QStringLiteral("Ghidra C decompiler")},
            {QStringLiteral("version"), QStringLiteral("12.0.4")},
            {QStringLiteral("description"), QStringLiteral("Independent Ghidra headless decompiler runtime with its pinned Temurin JDK. Supplies C pseudocode to the shared byte editor without a separate UI tab.")},
            {QStringLiteral("plugin_type"), QStringLiteral("backend")},
            {QStringLiteral("runtime"), QStringLiteral("ghidra")},
            {QStringLiteral("targets"), QJsonArray{QStringLiteral("decompiler")}},
            {QStringLiteral("runtime_root"), QStringLiteral("runtime/ghidra_12.0.4_PUBLIC")},
            {QStringLiteral("java_executable"), QStringLiteral("jdk/jdk-21.0.12.1+1/bin/java.exe")},
            {QStringLiteral("license"), QStringLiteral("LICENSE.txt")},
            {QStringLiteral("notice"), QStringLiteral("NOTICE.md")}
        };
    }

    QJsonObject assetDescription()
    {
        QJsonArray list;
        for (const auto& asset : assets()) list.append(QJsonObject{
            {QStringLiteral("name"), asset.name}, {QStringLiteral("url"), asset.url.toString(QUrl::FullyEncoded)},
            {QStringLiteral("sha256"), asset.sha256}, {QStringLiteral("root_directory"), asset.rootDirectory},
            {QStringLiteral("destination_directory"), asset.destinationDirectory},
            {QStringLiteral("max_archive_bytes"), asset.maxArchiveBytes}});
        return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("id"), QStringLiteral("ghidra")},
            {QStringLiteral("platform"), QStringLiteral("windows-x64")}, {QStringLiteral("assets"), list}};
    }

    QByteArray licenseText() { return payloadLicenseBytes(); }
    QByteArray noticeText() { return runtimeNoticeBytes(); }

    bool writePackageMetadata(const QString& pluginDirectory, QString* error)
    {
        if (!QFileInfo(pluginDirectory).isDir()) return fail(error, QStringLiteral("runtime_staging_missing"));
        if (!writeNew(pluginDirectory, QStringLiteral("plugin.json"), QJsonDocument(manifest()).toJson()) ||
            !writeNew(pluginDirectory, QStringLiteral("runtime-assets.json"), QJsonDocument(assetDescription()).toJson()) ||
            !writeNew(pluginDirectory, QStringLiteral("LICENSE.txt"), licenseText()) ||
            !writeNew(pluginDirectory, QStringLiteral("NOTICE.md"), noticeText()) ||
            !writeNew(pluginDirectory, QStringLiteral("KSword-LICENSE.txt"), wrapperLicenseBytes()) ||
            !writeNew(pluginDirectory, QStringLiteral("UPSTREAM-GHIDRA-NOTICE.txt"), runtimeUpstreamNoticeBytes()))
            return fail(error, QStringLiteral("runtime_metadata_write_failed"));
        if (error) error->clear();
        return true;
    }

    bool validateDirectory(const QString& pluginDirectory, QString* error)
    {
        if (!QFileInfo(pluginDirectory).isDir() || QFileInfo(pluginDirectory).isSymLink())
            return fail(error, QStringLiteral("runtime_plugin_missing"));
        if (!insideRoot(pluginDirectory, QStringLiteral("plugin.json"), false))
            return fail(error, QStringLiteral("runtime_manifest_missing"));
        QFile file(QDir(pluginDirectory).filePath(QStringLiteral("plugin.json")));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024)
            return fail(error, QStringLiteral("runtime_manifest_invalid"));
        const auto document = QJsonDocument::fromJson(file.readAll());
        const auto actual = document.object();
        const auto expected = manifest();
        for (const auto& key : {QStringLiteral("ksword_plugin_api"), QStringLiteral("id"), QStringLiteral("version"),
            QStringLiteral("plugin_type"), QStringLiteral("runtime"), QStringLiteral("targets"),
            QStringLiteral("runtime_root"), QStringLiteral("java_executable"), QStringLiteral("license"), QStringLiteral("notice")}) {
            if (!document.isObject() || actual.value(key) != expected.value(key))
                return fail(error, QStringLiteral("runtime_manifest_invalid"));
        }
        for (const auto& key : {QStringLiteral("entrypoint"), QStringLiteral("default_command"), QStringLiteral("commands"),
            QStringLiteral("tab"), QStringLiteral("visualization")}) {
            if (actual.contains(key)) return fail(error, QStringLiteral("runtime_manifest_executable"));
        }
        const QString ghidra = QStringLiteral("runtime/ghidra_12.0.4_PUBLIC/");
        const QString jdk = QStringLiteral("jdk/jdk-21.0.12.1+1/");
        const QStringList required {
            QStringLiteral("LICENSE.txt"), QStringLiteral("NOTICE.md"), QStringLiteral("runtime-assets.json"),
            QStringLiteral("KSword-LICENSE.txt"), QStringLiteral("UPSTREAM-GHIDRA-NOTICE.txt"),
            ghidra + QStringLiteral("Ghidra/Framework/Utility/lib/Utility.jar"),
            ghidra + QStringLiteral("Ghidra/application.properties"),
            ghidra + QStringLiteral("Ghidra/Features/Decompiler/os/win_x86_64/decompile.exe"),
            ghidra + QStringLiteral("Ghidra/Features/Decompiler/LICENSE.txt"),
            ghidra + QStringLiteral("LICENSE"),
            jdk + QStringLiteral("bin/java.exe"), jdk + QStringLiteral("release"), jdk + QStringLiteral("NOTICE"),
            jdk + QStringLiteral("legal/java.base/LICENSE"), jdk + QStringLiteral("legal/java.base/ADDITIONAL_LICENSE_INFO"),
            jdk + QStringLiteral("lib/src.zip")
        };
        for (const auto& relative : required)
            if (!insideRoot(pluginDirectory, relative, false)) return fail(error, QStringLiteral("runtime_payload_missing:") + relative);
        for (const auto& relative : {ghidra + QStringLiteral("licenses"), ghidra + QStringLiteral("GPL"), jdk + QStringLiteral("legal")}) {
            if (!insideRoot(pluginDirectory, relative, true) || QDir(QDir(pluginDirectory).filePath(relative)).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty())
                return fail(error, QStringLiteral("runtime_licenses_missing"));
        }
        const auto recordedAssets = QJsonDocument::fromJson(readBounded(QDir(pluginDirectory).filePath(QStringLiteral("runtime-assets.json"))));
        if (!recordedAssets.isObject() || recordedAssets.object() != assetDescription())
            return fail(error, QStringLiteral("runtime_assets_invalid"));
        const auto properties = readBounded(QDir(pluginDirectory).filePath(ghidra + QStringLiteral("Ghidra/application.properties")));
        const auto release = readBounded(QDir(pluginDirectory).filePath(jdk + QStringLiteral("release")));
        if (!properties.split('\n').contains("application.version=12.0.4") &&
            !properties.split('\n').contains("application.version=12.0.4\r"))
            return fail(error, QStringLiteral("runtime_version_mismatch"));
        if (!release.contains("JAVA_VERSION=\"21.0.12.1\"") || !release.contains("OS_ARCH=\"x86_64\"") ||
            !release.contains("IMPLEMENTOR=\"Eclipse Adoptium\""))
            return fail(error, QStringLiteral("runtime_java_version_mismatch"));
        if (!amd64Executable(QDir(pluginDirectory).filePath(jdk + QStringLiteral("bin/java.exe"))) ||
            !amd64Executable(QDir(pluginDirectory).filePath(ghidra + QStringLiteral("Ghidra/Features/Decompiler/os/win_x86_64/decompile.exe"))))
            return fail(error, QStringLiteral("runtime_executable_architecture"));
        if (error) error->clear();
        return true;
    }
}
