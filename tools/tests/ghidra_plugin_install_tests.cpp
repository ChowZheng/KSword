// The parser/promotion definitions below are copied verbatim from the source
// tree under test. Runtime profile and native ZIP installer are linked normally.
// ZIPs contain inert dummy runtime files; no Java/Ghidra/sample is executed.
#include <QtCore/QtCore>
#include <Windows.h>
#include "GhidraRuntimePlugin/RuntimeProfile.h"
#include "Ksword5.1/Ksword5.1/PluginHost.Ghidra.h"
#include "production_plugin_helpers.inc"
#include <cstdio>
#include <cstdlib>

namespace
{
    unsigned checks = 0;
    QString caseRoot;
    QString inertExecutable;
    using Asset = ks::plugin_host::ghidra_runtime::RuntimeAsset;

    void check(bool value, const char* description)
    {
        ++checks;
        if (!value) { std::fprintf(stderr, "FAIL [%u] %s\n", checks, description); std::exit(1); }
    }
    void put(const QString& path, const QByteArray& bytes)
    {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) { std::fprintf(stderr, "Fixture mkdir failed\n"); std::exit(2); }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
            std::fprintf(stderr, "Fixture write failed\n"); std::exit(2);
        }
    }
    QByteArray get(const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    QByteArray inertPe()
    {
        return get(inertExecutable); // Compiled by this runner; never executed here.
    }
    void fixtureRuntime(const QString& path, bool metadata = true)
    {
        using namespace ks::plugin_host::ghidra_runtime;
        const auto object = manifest();
        const auto ghidra = object.value(QStringLiteral("runtime_root")).toString() + u'/';
        const auto java = object.value(QStringLiteral("java_executable")).toString();
        const auto jdk = java.left(java.size() - QStringLiteral("bin/java.exe").size());
        put(path + u'/' + ghidra + QStringLiteral("Ghidra/Framework/Utility/lib/Utility.jar"), "inert fixture jar");
        put(path + u'/' + ghidra + QStringLiteral("Ghidra/application.properties"), "application.version=12.0.4\n");
        put(path + u'/' + ghidra + QStringLiteral("Ghidra/Features/Decompiler/os/win_x86_64/decompile.exe"), inertPe());
        put(path + u'/' + ghidra + QStringLiteral("Ghidra/Features/Decompiler/LICENSE.txt"), "fixture GPLv3 license marker\n");
        put(path + u'/' + ghidra + QStringLiteral("LICENSE"), "fixture upstream license marker\n");
        put(path + u'/' + ghidra + QStringLiteral("NOTICE"), "fixture upstream notice marker\n");
        put(path + u'/' + ghidra + QStringLiteral("licenses/fixture.txt"), "fixture component license\n");
        put(path + u'/' + ghidra + QStringLiteral("GPL/fixture.txt"), "fixture GPL component source marker\n");
        put(path + u'/' + java, inertPe());
        put(path + u'/' + jdk + QStringLiteral("release"),
            "JAVA_VERSION=\"21.0.12.1\"\nOS_ARCH=\"x86_64\"\nIMPLEMENTOR=\"Eclipse Adoptium\"\n");
        put(path + u'/' + jdk + QStringLiteral("NOTICE"), "fixture JDK notice\n");
        put(path + u'/' + jdk + QStringLiteral("legal/java.base/LICENSE"), "fixture GPLv2 license marker\n");
        put(path + u'/' + jdk + QStringLiteral("legal/java.base/ADDITIONAL_LICENSE_INFO"), "fixture classpath exception marker\n");
        put(path + u'/' + jdk + QStringLiteral("lib/src.zip"), "fixture source archive marker\n");
        if (metadata) {
            QString error;
            check(writePackageMetadata(path, &error), "canonical runtime metadata can be written into its owned stage");
        }
    }
    void metadataAndParser()
    {
        using namespace ks::plugin_host::ghidra_runtime;
        const auto path = caseRoot + QStringLiteral("/canonical");
        fixtureRuntime(path);
        QString error;
        check(validateDirectory(path, &error), "complete inert runtime validates without executing any binary");
        check(!writePackageMetadata(path, &error), "metadata writer cannot silently replace files in an existing package");
        PluginDescriptor descriptor;
        check(loadPluginManifestDirectory(path, QStringLiteral("ghidra"), &descriptor, &error),
            "production manager recognizes the standalone Ghidra backend plugin");
        check(descriptor.pluginType == QStringLiteral("backend") && descriptor.runtime == QStringLiteral("ghidra")
            && descriptor.targets == QStringList{QStringLiteral("decompiler")}
            && descriptor.entrypointPath.isEmpty() && descriptor.defaultCommand.isEmpty()
            && !descriptor.tabPresentation.enabled && !descriptor.visualization.enabled,
            "backend plugin has no ordinary executable command or separate UI tab");
        const QList<QPair<QString, QJsonValue>> faults {
            {QStringLiteral("entrypoint"), QStringLiteral("jdk/jdk-21.0.12.1+1/bin/java.exe")},
            {QStringLiteral("default_command"), QStringLiteral("info")},
            {QStringLiteral("tab"), QJsonObject{}},
            {QStringLiteral("visualization"), QJsonObject{}},
            {QStringLiteral("targets"), QJsonArray{QStringLiteral("process")}},
            {QStringLiteral("runtime"), QStringLiteral("executable")},
            {QStringLiteral("runtime_root"), QStringLiteral("../outside")},
            {QStringLiteral("java_executable"), QStringLiteral("C:/outside/java.exe")}
        };
        for (const auto& fault : faults) {
            auto changed = manifest(); changed.insert(fault.first, fault.second);
            put(path + QStringLiteral("/plugin.json"), QJsonDocument(changed).toJson());
            check(!loadPluginManifestDirectory(path, QStringLiteral("ghidra"), &descriptor, &error)
                && !error.isEmpty(), "invalid backend behavior and escaping runtime paths reject through the real manager");
        }
        auto executableGhidra = manifest();
        executableGhidra.insert(QStringLiteral("plugin_type"), QStringLiteral("command"));
        executableGhidra.insert(QStringLiteral("runtime"), QStringLiteral("executable"));
        executableGhidra.insert(QStringLiteral("entrypoint"), QStringLiteral("jdk/jdk-21.0.12.1+1/bin/java.exe"));
        executableGhidra.insert(QStringLiteral("default_command"), QStringLiteral("info"));
        executableGhidra.insert(QStringLiteral("targets"), QJsonArray{QStringLiteral("process")});
        put(path + QStringLiteral("/plugin.json"), QJsonDocument(executableGhidra).toJson());
        check(!loadPluginManifestDirectory(path, QStringLiteral("ghidra"), &descriptor, &error),
            "reserved Ghidra id cannot be rewritten as an ordinary executable plugin");
        put(path + QStringLiteral("/plugin.json"), QJsonDocument(manifest()).toJson());
        const auto java = path + u'/' + manifest().value(QStringLiteral("java_executable")).toString();
        check(QFile::remove(java) && !validateDirectory(path, &error), "missing bundled Java rejects incomplete installation");
        put(java, inertPe());
        check(QFile::remove(path + QStringLiteral("/NOTICE.md")) && !validateDirectory(path, &error),
            "missing license notice prevents an apparently complete backend package");
        put(path + QStringLiteral("/NOTICE.md"), noticeText());
        check(validateDirectory(path, &error), "repairing real required components restores runtime readiness");

        const auto legacy = caseRoot + QStringLiteral("/legacy");
        put(legacy + QStringLiteral("/fixture.exe"), inertPe());
        put(legacy + QStringLiteral("/plugin.json"), QJsonDocument(QJsonObject{
            {QStringLiteral("ksword_plugin_api"), QStringLiteral("1")}, {QStringLiteral("id"), QStringLiteral("legacy")},
            {QStringLiteral("name"), QStringLiteral("Existing executable")}, {QStringLiteral("version"), QStringLiteral("1.0")},
            {QStringLiteral("description"), QStringLiteral("existing protocol fixture")},
            {QStringLiteral("runtime"), QStringLiteral("executable")}, {QStringLiteral("entrypoint"), QStringLiteral("fixture.exe")},
            {QStringLiteral("default_command"), QStringLiteral("info")},
            {QStringLiteral("targets"), QJsonArray{QStringLiteral("process")}}}).toJson());
        check(loadPluginManifestDirectory(legacy, QStringLiteral("legacy"), &descriptor, &error)
            && descriptor.pluginType == QStringLiteral("command") && !descriptor.entrypointPath.isEmpty(),
            "existing executable plugin parsing remains compatible");
        std::puts("Plugin metadata/parser checks completed");
    }

    void promotion()
    {
        const MarketplacePlugin plugin{.id=QStringLiteral("ghidra"), .installDirectory=QStringLiteral("ghidra")};
        for (const bool wrapped : {false, true}) {
            const auto root = caseRoot + (wrapped ? QStringLiteral("/wrapped") : QStringLiteral("/root"));
            const auto stage = root + QStringLiteral("/.stage");
            const auto content = wrapped ? stage + QStringLiteral("/ghidra") : stage;
            fixtureRuntime(content);
            put(root + QStringLiteral("/ghidra/old-marker.txt"), "existing plugin preserved until commit");
            QString error;
            check(promoteExtractedPlugin(plugin, root, stage, &error), "validated root and wrapped backend packages promote transactionally");
            check(ks::plugin_host::ghidra_runtime::validateDirectory(root + QStringLiteral("/ghidra"), &error)
                && !QFileInfo::exists(root + QStringLiteral("/ghidra/old-marker.txt")),
                "successful backend replacement contains the full verified runtime");
        }
        const auto root = caseRoot + QStringLiteral("/bad-promotion");
        const auto stage = root + QStringLiteral("/.stage");
        fixtureRuntime(stage);
        put(root + QStringLiteral("/ghidra/old-marker.txt"), "unchanged existing plugin");
        QFile::remove(stage + QStringLiteral("/LICENSE.txt"));
        QString error;
        check(!promoteExtractedPlugin(plugin, root, stage, &error)
            && get(root + QStringLiteral("/ghidra/old-marker.txt")) == "unchanged existing plugin",
            "invalid backend package cannot replace an installed plugin");
        std::puts("Plugin promotion checks completed");
    }

    QList<Asset> localAssets(const QString& source, const QString& output, const QString& fault = QStringLiteral("valid"))
    {
        auto result = ks::plugin_host::ghidra_runtime::assets();
        for (int index = 0; index < result.size(); ++index) {
            auto& asset = result[index];
            const auto zip = output + QStringLiteral("/asset-%1.zip").arg(index);
            QProcess generator;
            const auto python = qEnvironmentVariable("KSWORD_PLUGIN_TEST_PYTHON");
            generator.start(python.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("python")) : python, {
                QDir::current().filePath(QStringLiteral("tools/tests/create_ghidra_plugin_fixtures.py")),
                QStringLiteral("--source"), source + u'/' + asset.destinationDirectory + u'/' + asset.rootDirectory,
                QStringLiteral("--output"), zip, QStringLiteral("--mode"),
                index == (fault == QStringLiteral("missing-source") ? 1 : 0) ? fault : QStringLiteral("valid")});
            if (!generator.waitForFinished(10000) || generator.exitCode() != 0) {
                std::fprintf(stderr, "Fixture ZIP generator failed: %s\n", generator.readAllStandardError().constData()); std::exit(2);
            }
            asset.url = QUrl::fromLocalFile(zip);
            asset.sha256 = QString::fromLatin1(QCryptographicHash::hash(get(zip), QCryptographicHash::Sha256).toHex());
            asset.maxArchiveBytes = 1024 * 1024;
        }
        return result;
    }

    struct Outcome { bool completed = false; bool success = false; QString stage; QString error; int progress = 0; QString progressStage; };
    Outcome install(const QString& root, const QList<Asset>& assets, bool cancelOnProgress = false)
    {
        Outcome result;
        QEventLoop loop;
        ks::plugin_host::GhidraRuntimeInstaller installer;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, [&]() {
            std::fprintf(stderr, "Native fixture watchdog stage=%s\n", result.progressStage.toUtf8().constData());
            installer.cancel(); loop.quit();
        });
        watchdog.start(120000);
        installer.startForTests(root, assets, [&](const QString& stage, int percent) {
            ++result.progress;
            result.progressStage = stage;
            check(percent >= 0 && percent <= 100, "native installer progress stays within the public percentage range");
            if (cancelOnProgress) installer.cancel();
        }, [&](bool ok, const QString& stage, const QString& error) {
            result.completed = true; result.success = ok; result.stage = stage; result.error = error; loop.quit();
        });
        if (!result.completed) loop.exec();
        check(result.completed, "native asynchronous installer finishes its owned operation");
        return result;
    }
    bool noStages(const QString& root)
    {
        return QDir(root).entryList({QStringLiteral(".ksword-plugin-stage-ghidra-*")},
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty();
    }
    void nativeInstaller()
    {
        const auto source = caseRoot + QStringLiteral("/zip-source");
        fixtureRuntime(source);
        const auto assets = localAssets(source, caseRoot + QStringLiteral("/valid-zips"));
        const auto root = caseRoot + QStringLiteral("/native 空格 plugins");
        put(root + QStringLiteral("/ghidra/old-marker.txt"), "native original plugin");
        auto success = install(root, assets);
        QString error;
        const bool prepared = success.success && !success.stage.isEmpty()
            && ks::plugin_host::ghidra_runtime::validateDirectory(success.stage, &error);
        if (!prepared) std::fprintf(stderr, "Native fixture preparation error: %s %s\n",
            success.error.toUtf8().constData(), error.toUtf8().constData());
        check(prepared,
            "real download/hash/native extraction produces a fully validated inert backend stage");
        check(get(root + QStringLiteral("/ghidra/old-marker.txt")) == "native original plugin",
            "native preparation never changes an installed runtime before manager promotion");
        const MarketplacePlugin plugin{.id=QStringLiteral("ghidra"), .installDirectory=QStringLiteral("ghidra")};
        check(promoteExtractedPlugin(plugin, root, success.stage, &error)
            && ks::plugin_host::ghidra_runtime::validateDirectory(root + QStringLiteral("/ghidra"), &error),
            "native prepared stage commits through the same production manager transaction");
        put(root + QStringLiteral("/ghidra/preserved-marker.txt"), "keep after rejected update");
        for (const auto& fault : {QStringLiteral("checksum"), QStringLiteral("download-size"), QStringLiteral("traversal"),
            QStringLiteral("device"), QStringLiteral("link"), QStringLiteral("duplicate"),
            QStringLiteral("wrong-wrapper"), QStringLiteral("missing-source")}) {
            auto bad = fault == QStringLiteral("checksum") || fault == QStringLiteral("download-size")
                ? assets : localAssets(source, caseRoot + u'/' + fault, fault);
            if (fault == QStringLiteral("checksum")) bad[0].sha256 = QString(64, QLatin1Char('0'));
            if (fault == QStringLiteral("download-size")) bad[0].maxArchiveBytes = 1;
            const auto result = install(root, bad);
            check(!result.success && result.stage.isEmpty() && !result.error.isEmpty() && noStages(root),
                "invalid hash size archive path or legal/source payload leaves no usable partial plugin");
            check(get(root + QStringLiteral("/ghidra/preserved-marker.txt")) == "keep after rejected update",
                "rejected native update preserves the existing installed plugin");
        }
        const auto cancelled = install(root, assets, true);
        check(!cancelled.success && !cancelled.error.isEmpty() && noStages(root),
            "cancelling from progress cleans the private stage without publishing a package");
        ks::plugin_host::GhidraRuntimeInstaller first;
        ks::plugin_host::GhidraRuntimeInstaller second;
        Outcome firstResult, secondResult;
        first.startForTests(root, assets, {}, [&](bool ok, const QString&, const QString& error) {
            firstResult.completed = true; firstResult.success = ok; firstResult.error = error;
        });
        second.startForTests(root, assets, {}, [&](bool ok, const QString&, const QString& error) {
            secondResult.completed = true; secondResult.success = ok; secondResult.error = error;
        });
        check(secondResult.completed && !secondResult.success && !secondResult.error.isEmpty(),
            "two native manager instances cannot concurrently replace the same installed runtime");
        first.cancel();
        check(firstResult.completed && !firstResult.success && noStages(root),
            "cancelling the lock owner leaves no stage from either installer");
        QPointer<ks::plugin_host::GhidraRuntimeInstaller> retiring = new ks::plugin_host::GhidraRuntimeInstaller;
        bool completedAfterDeletion = false;
        retiring->startForTests(root, assets, [retiring](const QString&, int) { delete retiring.data(); },
            [&](bool, const QString&, const QString&) { completedAfterDeletion = true; });
        QCoreApplication::processEvents();
        check(retiring.isNull() && !completedAfterDeletion && noStages(root),
            "destroying installer from progress safely cancels all callbacks and owned files");
        std::puts("Native plugin installer checks completed");
    }

    int officialInstall(const QString& ghidraZip, const QString& jdkZip, const QString& pluginRoot)
    {
        auto assets = ks::plugin_host::ghidra_runtime::assets();
        assets[0].url = QUrl::fromLocalFile(ghidraZip);
        assets[1].url = QUrl::fromLocalFile(jdkZip);
        ks::plugin_host::GhidraRuntimeInstaller installer;
        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        bool completed = false, installed = false;
        QString failure;
        int lastPercent = -10;
        QObject::connect(&watchdog, &QTimer::timeout, &loop, [&]() { installer.cancel(); loop.quit(); });
        watchdog.start(600000);
        installer.startForTests(pluginRoot, assets, [&](const QString& stage, int percent) {
            if (percent >= lastPercent + 10 || stage.contains(QStringLiteral("解压")) || percent >= 95) {
                std::printf("OFFICIAL_INSTALL_PROGRESS=%d %s\n", percent, stage.toUtf8().constData());
                lastPercent = percent;
            }
        }, [&](bool ok, const QString& stage, const QString& error) {
            completed = true;
            if (!ok) failure = error;
            else {
                QString promotionError;
                const MarketplacePlugin plugin{.id=QStringLiteral("ghidra"), .installDirectory=QStringLiteral("ghidra")};
                installed = promoteExtractedPlugin(plugin, pluginRoot, stage, &promotionError)
                    && ks::plugin_host::ghidra_runtime::validateDirectory(pluginRoot + QStringLiteral("/ghidra"), &promotionError);
                if (!installed) failure = promotionError;
            }
            loop.quit();
        });
        if (!completed) loop.exec();
        std::printf("GHIDRA_OFFICIAL_PLUGIN_INSTALL=%s DIRECTORY=%s ERROR=%s\n",
            completed && installed ? "PASS" : "FAIL", pluginRoot.toUtf8().constData(), failure.toUtf8().constData());
        return completed && installed ? 0 : 1;
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) return 2;
    inertExecutable = QString::fromLocal8Bit(argv[2]);
    if (!QFileInfo(inertExecutable).isFile()) return 2;
    if (argc == 7 && QString::fromLocal8Bit(argv[3]) == QStringLiteral("--official-install"))
        return officialInstall(QString::fromLocal8Bit(argv[4]), QString::fromLocal8Bit(argv[5]), QString::fromLocal8Bit(argv[6]));
    QTemporaryDir temporary(QString::fromLocal8Bit(argv[1]) + QStringLiteral("/cases-XXXXXX"));
    if (!temporary.isValid()) return 2;
    caseRoot = temporary.path();
    metadataAndParser();
    promotion();
    nativeInstaller();
    std::printf("GHIDRA_PLUGIN_PORTABLE_RESULT=PASS CHECKS=%u\n", checks);
}
