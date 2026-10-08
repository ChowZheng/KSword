#pragma once

#include "../../GhidraRuntimePlugin/RuntimeProfile.h"
#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

class QCryptographicHash;
class QNetworkAccessManager;
class QNetworkReply;
class QLockFile;
class QProcess;
class QSaveFile;
class QTimer;

namespace ks::plugin_host
{
    // Streams the two pinned vendor archives into a private sibling stage.
    // Never launches Ghidra, Java, or any installed plugin. The caller performs
    // the existing manifest-validated directory promotion after success.
    class GhidraRuntimeInstaller final : public QObject
    {
    public:
        using Progress = std::function<void(const QString&, int)>;
        using Completion = std::function<void(bool, const QString&, const QString&)>;
        using Translator = std::function<QString(const QString&)>;
        explicit GhidraRuntimeInstaller(QObject* parent = nullptr, Translator translator = {});
        ~GhidraRuntimeInstaller() override;
        void start(const QString& pluginRoot, Progress progress, Completion completion);
        void cancel();
#ifdef KSWORD_PLUGIN_INSTALL_TESTING
        // Exists only in isolated test binaries. Production cannot replace the
        // fixed vendor URLs/digests with local or catalog-supplied artifacts.
        void startForTests(const QString& pluginRoot,
            const QList<ghidra_runtime::RuntimeAsset>& assets, Progress progress, Completion completion);
#endif
    private:
        void begin(const QString& pluginRoot, const QList<ghidra_runtime::RuntimeAsset>& assets,
            Progress progress, Completion completion);
        void downloadNext();
        void openDownload(const QUrl& url);
        void consumeReply();
        void extractArchive();
        void finish(bool success, const QString& error);
        void stopOperations();
        void cleanupStage();
        bool report(const QString& stage, int percent);
        bool trustedRedirect(const QUrl& url) const;
        QString text(const QString& source) const;
        QNetworkAccessManager* m_network = nullptr;
        QPointer<QNetworkReply> m_reply;
        QPointer<QProcess> m_extractor;
        QTimer* m_deadline = nullptr;
        std::unique_ptr<QSaveFile> m_archive;
        std::unique_ptr<QCryptographicHash> m_digest;
        std::unique_ptr<QLockFile> m_installLock;
        QList<ghidra_runtime::RuntimeAsset> m_assets;
        QString m_pluginRoot;
        QString m_stage;
        QString m_archivePath;
        QString m_downloadError;
        Progress m_progress;
        Completion m_completion;
        Translator m_translator;
        qint64 m_received = 0;
        int m_assetIndex = 0;
        int m_redirectCount = 0;
        quint64 m_operationGeneration = 0;
        bool m_active = false;
        bool m_allowLocalTestAssets = false;
    };
}
