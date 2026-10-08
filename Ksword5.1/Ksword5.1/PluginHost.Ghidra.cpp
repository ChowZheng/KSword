#include "PluginHost.Ghidra.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace ks::plugin_host
{
    namespace
    {
        QString literal(QString value)
        {
            return QLatin1Char('\'') + value.replace(QLatin1Char('\''), QStringLiteral("''")) + QLatin1Char('\'');
        }
        // Preflight every ZIP entry before extracting any payload. The official
        // wrapper must be exact; links, traversal, Windows aliases and bombs are
        // rejected even though both archives have already passed their pin.
        QString extractionScript(const QString& archive, const QString& destination,
            const QString& wrapper, const qint64 budget)
        {
            return QStringLiteral(R"PS(
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.AppContext]::SetSwitch('Switch.System.IO.UseLegacyPathHandling',$false)
[System.AppContext]::SetSwitch('Switch.System.IO.BlockLongPaths',$false)
$destination=%1
$wrapper=%2
function Native-Path([string]$path){
  $path=$path.Replace('/','\')
  if($path.StartsWith('\\?\')){return $path}
  if($path.StartsWith('\\')){return '\\?\UNC\'+$path.Substring(2)}
  return '\\?\'+$path
}
$zip=[System.IO.Compression.ZipFile]::OpenRead((Native-Path %3))
try {
  if($zip.Entries.Count -gt 100000){throw 'ZIP entry limit exceeded'}
  [long]$total=0
  $names=New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
  foreach($entry in $zip.Entries){
    $name=$entry.FullName.Replace('\','/')
    if(!$name -or $name.StartsWith('/') -or $name.Contains(':') -or $name.Contains([char]0)){throw 'Unsafe ZIP entry'}
    $parts=$name.Split('/',[System.StringSplitOptions]::RemoveEmptyEntries)
    if(!$parts.Length -or $parts[0] -cne $wrapper){throw 'Unexpected ZIP wrapper'}
    foreach($part in $parts){
      if($part.Length -gt 255 -or $part -eq '.' -or $part -eq '..' -or $part.EndsWith('.') -or $part.EndsWith(' ') -or $part.IndexOfAny([char[]]'<>|?*') -ge 0){throw 'Unsafe ZIP component'}
      if($part -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\..*)?$'){throw 'Windows device alias in ZIP'}
    }
    if(!$names.Add($name.TrimEnd('/'))){throw 'Duplicate ZIP path'}
    if((($entry.ExternalAttributes -shr 16) -band 61440) -eq 40960 -or ($entry.ExternalAttributes -band 1024)){throw 'ZIP links/reparse entries are forbidden'}
    if($entry.Length -lt 0 -or $entry.Length -gt 1073741824){throw 'ZIP file limit exceeded'}
    $total += $entry.Length
    if($total -gt %4){throw 'ZIP expansion limit exceeded'}
  }
  # destination is already the canonical Qt-owned stage. Every relative
  # component was preflighted above; avoid PS5's cached MAX_PATH normalizer.
  $prefix=$destination.Replace('/','\').TrimEnd('\')+'\'
  foreach($entry in $zip.Entries){
    $target=$prefix+$entry.FullName.Replace('/','\')
    if($target.Length -gt 32760 -or !$target.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'ZIP escaped staging directory'}
    $native=Native-Path $target
    if($entry.FullName.EndsWith('/')){[IO.Directory]::CreateDirectory($native)|Out-Null;continue}
    $parent=$native.Substring(0,$native.LastIndexOf('\'))
    [IO.Directory]::CreateDirectory($parent)|Out-Null
    $kswordEntryStream=$entry.Open()
    try {
      $kswordOutputStream=[IO.FileStream]::new($native,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
      try {$kswordEntryStream.CopyTo($kswordOutputStream)} finally {$kswordOutputStream.Dispose()}
    } finally {$kswordEntryStream.Dispose()}
  }
} finally {$zip.Dispose()}
)PS").arg(literal(destination), literal(wrapper), literal(archive), QString::number(budget));
        }
    }

    GhidraRuntimeInstaller::GhidraRuntimeInstaller(QObject* parent, Translator translator)
        : QObject(parent), m_translator(std::move(translator))
    {
        m_network = new QNetworkAccessManager(this);
        m_deadline = new QTimer(this);
        m_deadline->setSingleShot(true);
        connect(m_deadline, &QTimer::timeout, this, [this]() {
            if (m_active) finish(false, text(QStringLiteral("Ghidra 插件安装超时。")));
        });
    }
    GhidraRuntimeInstaller::~GhidraRuntimeInstaller()
    {
        m_active = false;
        m_completion = {};
        stopOperations();
        cleanupStage();
    }
    QString GhidraRuntimeInstaller::text(const QString& source) const
    {
        return m_translator ? m_translator(source) : source;
    }
    void GhidraRuntimeInstaller::start(const QString& pluginRoot, Progress progress, Completion completion)
    {
        if (!m_active) m_allowLocalTestAssets = false;
        begin(pluginRoot, ghidra_runtime::assets(), std::move(progress), std::move(completion));
    }
#ifdef KSWORD_PLUGIN_INSTALL_TESTING
    void GhidraRuntimeInstaller::startForTests(const QString& pluginRoot,
        const QList<ghidra_runtime::RuntimeAsset>& assets, Progress progress, Completion completion)
    {
        if (!m_active) m_allowLocalTestAssets = true;
        begin(pluginRoot, assets, std::move(progress), std::move(completion));
    }
#endif
    void GhidraRuntimeInstaller::begin(const QString& pluginRoot,
        const QList<ghidra_runtime::RuntimeAsset>& assets, Progress progress, Completion completion)
    {
        if (m_active)
        {
            if (completion) completion(false, {}, text(QStringLiteral("Ghidra 插件安装已在进行。")));
            return;
        }
        m_progress = std::move(progress);
        m_completion = std::move(completion);
        m_assets = assets;
        m_assetIndex = 0;
        ++m_operationGeneration;
        m_active = true;
        if (assets.size() != 2 || !QDir().mkpath(pluginRoot))
        {
            finish(false, text(QStringLiteral("无法创建 Ghidra 插件安装目录。")));
            return;
        }
        m_pluginRoot = QFileInfo(pluginRoot).canonicalFilePath();
        if (m_pluginRoot.isEmpty()) { finish(false, text(QStringLiteral("插件目录无法规范化。"))); return; }
        m_installLock = std::make_unique<QLockFile>(QDir(m_pluginRoot).filePath(QStringLiteral(".ksword-ghidra-install.lock")));
        m_installLock->setStaleLockTime(0);
        if (!m_installLock->tryLock(0))
        {
            finish(false, text(QStringLiteral("另一个窗口正在安装 Ghidra 插件。")));
            return;
        }
        m_stage = QDir(m_pluginRoot).filePath(QStringLiteral(".ksword-plugin-stage-ghidra-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!QDir().mkpath(QDir(m_stage).filePath(QStringLiteral(".archives"))))
        {
            finish(false, text(QStringLiteral("无法创建 Ghidra 安装暂存目录。")));
            return;
        }
        m_deadline->start(30 * 60 * 1000);
        downloadNext();
    }
    bool GhidraRuntimeInstaller::report(const QString& stage, const int percent)
    {
        const QPointer<GhidraRuntimeInstaller> self(this);
        const auto generation = m_operationGeneration;
        const auto progress = m_progress;
        if (m_active && progress) progress(stage, std::clamp(percent, 0, 100));
        return self && m_active && generation == m_operationGeneration;
    }
    bool GhidraRuntimeInstaller::trustedRedirect(const QUrl& url) const
    {
        if (m_allowLocalTestAssets && url.isLocalFile()) return true;
        if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
            !url.userInfo().isEmpty() || (url.port(-1) != -1 && url.port() != 443)) return false;
        const auto host = url.host().toLower();
        return host == QStringLiteral("github.com") || host == QStringLiteral("release-assets.githubusercontent.com") ||
            host == QStringLiteral("objects.githubusercontent.com");
    }
    void GhidraRuntimeInstaller::downloadNext()
    {
        if (!m_active) return;
        if (m_assetIndex >= m_assets.size())
        {
            QString error;
            if (!report(text(QStringLiteral("正在验证 Ghidra 与 Java 运行环境")), 95)) return;
            if (!ghidra_runtime::writePackageMetadata(m_stage, &error) ||
                !ghidra_runtime::validateDirectory(m_stage, &error))
            {
                finish(false, error);
                return;
            }
            finish(true, {});
            return;
        }
        const auto& asset = m_assets[m_assetIndex];
        if (!trustedRedirect(asset.url) || asset.sha256.size() != 64 || asset.maxArchiveBytes <= 0)
        {
            finish(false, text(QStringLiteral("Ghidra 官方下载配置不合法。")));
            return;
        }
        m_archivePath = QDir(m_stage).filePath(QStringLiteral(".archives/download-%1.zip").arg(m_assetIndex));
        m_archive = std::make_unique<QSaveFile>(m_archivePath);
        if (!m_archive->open(QIODevice::WriteOnly))
        {
            finish(false, text(QStringLiteral("无法创建运行环境下载文件：%1")).arg(m_archive->errorString()));
            return;
        }
        m_digest = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
        m_received = 0;
        m_redirectCount = 0;
        m_downloadError.clear();
        if (!report(text(QStringLiteral("正在下载 %1")).arg(asset.name), m_assetIndex * 45)) return;
        openDownload(asset.url);
    }
    void GhidraRuntimeInstaller::openDownload(const QUrl& url)
    {
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(60000);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("KSword-GhidraRuntime/1"));
        m_reply = m_network->get(request);
        auto* reply = m_reply.data();
        reply->setReadBufferSize(256 * 1024);
        connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
            if (m_active && reply == m_reply) consumeReply();
        });
        connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 received, qint64 total) {
            if (!m_active || reply != m_reply) return;
            if (total > m_assets[m_assetIndex].maxArchiveBytes)
            {
                m_downloadError = text(QStringLiteral("官方运行环境包超过下载大小限制。"));
                reply->abort();
                return;
            }
            const int part = total > 0 ? static_cast<int>(std::min<qint64>(35, received * 35 / total)) : 0;
            report(text(QStringLiteral("正在下载 %1（%2 MiB）")).arg(m_assets[m_assetIndex].name)
                .arg(received / (1024.0 * 1024.0), 0, 'f', 1), m_assetIndex * 45 + part);
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            if (!m_active || reply != m_reply) return;
            const auto redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
            if (!redirect.isEmpty())
            {
                const auto target = reply->url().resolved(redirect);
                reply->deleteLater();
                m_reply.clear();
                if (!trustedRedirect(target) || ++m_redirectCount > 5)
                {
                    finish(false, text(QStringLiteral("官方运行环境下载跳转不被允许。")));
                    return;
                }
                openDownload(target);
                return;
            }
            const QPointer<GhidraRuntimeInstaller> self(this);
            consumeReply();
            if (!self || !m_active || reply != m_reply) return;
            const bool ok = m_downloadError.isEmpty() && reply->error() == QNetworkReply::NoError;
            const auto error = !m_downloadError.isEmpty() ? m_downloadError : reply->errorString();
            reply->deleteLater();
            m_reply.clear();
            if (!ok || m_received == 0)
            {
                finish(false, text(QStringLiteral("Ghidra 运行环境下载失败：%1")).arg(error));
                return;
            }
            const auto digest = QString::fromLatin1(m_digest->result().toHex());
            if (digest.compare(m_assets[m_assetIndex].sha256, Qt::CaseInsensitive) != 0)
            {
                finish(false, text(QStringLiteral("运行环境 SHA-256 校验失败，已拒绝安装。")));
                return;
            }
            if (!m_archive->commit())
            {
                finish(false, text(QStringLiteral("无法保存已验证的运行环境包：%1")).arg(m_archive->errorString()));
                return;
            }
            m_archive.reset();
            m_digest.reset();
            extractArchive();
        });
    }
    void GhidraRuntimeInstaller::consumeReply()
    {
        if (!m_reply || !m_archive || !m_downloadError.isEmpty()) return;
        // Redirect response bodies are not part of the pinned ZIP digest.
        if (!m_reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl().isEmpty())
        {
            m_reply->readAll();
            return;
        }
        while (m_reply->bytesAvailable() > 0)
        {
            const auto bytes = m_reply->read(256 * 1024);
            if (bytes.isEmpty()) break;
            if (bytes.size() > m_assets[m_assetIndex].maxArchiveBytes - m_received)
            {
                m_downloadError = text(QStringLiteral("运行环境下载超过大小限制。"));
                m_reply->abort();
                return;
            }
            if (m_archive->write(bytes) != bytes.size())
            {
                m_downloadError = text(QStringLiteral("无法写入运行环境暂存文件：%1")).arg(m_archive->errorString());
                m_reply->abort();
                return;
            }
            m_digest->addData(bytes);
            m_received += bytes.size();
        }
    }
    void GhidraRuntimeInstaller::extractArchive()
    {
        auto powerShell = QStandardPaths::findExecutable(QStringLiteral("pwsh.exe"));
        if (powerShell.isEmpty()) powerShell = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
        if (powerShell.isEmpty()) { finish(false, text(QStringLiteral("未找到 PowerShell，无法解压 Ghidra 运行环境。"))); return; }
        const auto destination = QDir(m_stage).filePath(m_assets[m_assetIndex].destinationDirectory);
        if (!QDir().mkpath(destination)) { finish(false, text(QStringLiteral("无法创建运行环境解压目录。"))); return; }
        if (!report(text(QStringLiteral("正在安全解压 %1")).arg(m_assets[m_assetIndex].name), m_assetIndex * 45 + 38)) return;
        auto* process = new QProcess(this);
        m_extractor = process;
        process->setProgram(powerShell);
        process->setProcessChannelMode(QProcess::SeparateChannels);
        process->setArguments({QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
            QStringLiteral("-Command"), extractionScript(m_archivePath, destination, m_assets[m_assetIndex].rootDirectory,
                std::min<qint64>(4LL * 1024 * 1024 * 1024, m_assets[m_assetIndex].maxArchiveBytes * 4))});
#ifdef Q_OS_WIN
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
        connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
            if (m_active && process == m_extractor && error == QProcess::FailedToStart)
                finish(false, text(QStringLiteral("无法启动运行环境解压器：%1")).arg(process->errorString()));
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
                if (!m_active || process != m_extractor) return;
                const auto details = QString::fromLocal8Bit(process->readAllStandardError().right(64 * 1024)).trimmed();
                m_extractor.clear();
                process->deleteLater();
                QFile::remove(m_archivePath);
                if (status != QProcess::NormalExit || code != 0)
                {
                    finish(false, text(QStringLiteral("运行环境解压失败：%1")).arg(details));
                    return;
                }
                ++m_assetIndex;
                downloadNext();
            });
        process->start();
    }
    void GhidraRuntimeInstaller::stopOperations()
    {
        m_deadline->stop();
        if (m_reply)
        {
            disconnect(m_reply, nullptr, this, nullptr);
            m_reply->abort();
            m_reply->deleteLater();
            m_reply.clear();
        }
        if (m_extractor)
        {
            disconnect(m_extractor, nullptr, this, nullptr);
            m_extractor->kill();
            m_extractor->waitForFinished(5000);
            m_extractor->deleteLater();
            m_extractor.clear();
        }
        m_archive.reset();
        m_digest.reset();
    }
    void GhidraRuntimeInstaller::cleanupStage()
    {
        if (m_stage.isEmpty() || m_pluginRoot.isEmpty()) return;
        const auto root = QFileInfo(m_pluginRoot).canonicalFilePath();
        const auto stage = QFileInfo(m_stage).canonicalFilePath();
        const auto prefix = QDir::cleanPath(root) + QLatin1Char('/');
        if (!root.isEmpty() && !stage.isEmpty() && stage.startsWith(prefix, Qt::CaseInsensitive) &&
            QFileInfo(m_stage).fileName().startsWith(QStringLiteral(".ksword-plugin-stage-ghidra-")) &&
            !QFileInfo(m_stage).isSymLink()) QDir(stage).removeRecursively();
        m_stage.clear();
    }
    void GhidraRuntimeInstaller::finish(const bool success, const QString& error)
    {
        if (!m_active) return;
        m_active = false;
        stopOperations();
        auto completion = std::move(m_completion);
        m_progress = {};
        QString preparedStage;
        if (success)
        {
            preparedStage = m_stage;
            m_stage.clear(); // caller owns the verified stage and its promotion
        }
        else { cleanupStage(); m_installLock.reset(); }
        if (completion) completion(success, preparedStage, error);
    }
    void GhidraRuntimeInstaller::cancel()
    {
        if (m_active) finish(false, text(QStringLiteral("Ghidra 插件安装已取消。")));
    }
}
