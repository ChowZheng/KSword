#include "ThemeControlGlyphs.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace ks::ui
{
    QString ThemedControlGlyphPath(
        const QString& resourcePath,
        const QColor& foregroundColor,
        const QString& cacheDirectory)
    {
        // sourceFile 用途：只读单色资源形状，不修改 qrc 或原 SVG。
        QFile sourceFile(resourcePath);
        if (!foregroundColor.isValid() || !sourceFile.open(QIODevice::ReadOnly))
        {
            return resourcePath;
        }
        const QByteArray originalSvg = sourceFile.readAll();
        if (originalSvg.isEmpty())
        {
            return resourcePath;
        }

        // paintAttribute 用途：只替换明确的实色填充/描边，保留 fill="none" 与透明轮廓。
        // 本接口只接收项目内 ks_control 单色 SVG，不对多色文件/进程图标做全图覆盖。
        static const QRegularExpression paintAttribute(
            QStringLiteral(R"((\b(?:fill|stroke)\s*=\s*)(["'])#[0-9a-fA-F]{3,8}\2)"));
        const QString colorText = foregroundColor.name(QColor::HexRgb).toUpper();
        QString tintedSvg = QString::fromUtf8(originalSvg);
        tintedSvg.replace(paintAttribute, QStringLiteral("\\1\\2%1\\2").arg(colorText));
        if (tintedSvg == QString::fromUtf8(originalSvg)
            && !originalSvg.contains(colorText.toUtf8()))
        {
            return resourcePath;
        }

        // cacheRoot 用途：生产写应用缓存；测试显式给目录，避免污染用户 AppData。
        const QString applicationCacheDirectory =
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (cacheDirectory.isEmpty() && applicationCacheDirectory.isEmpty())
        {
            return resourcePath;
        }
        const QString cacheRoot = cacheDirectory.isEmpty()
            ? QDir(applicationCacheDirectory)
                .filePath(QStringLiteral("theme-control-glyphs"))
            : cacheDirectory;
        if (cacheRoot.isEmpty() || !QDir().mkpath(cacheRoot))
        {
            return resourcePath;
        }

        // cacheIdentity 同时包含原 SVG 内容和颜色，资源形状更新不会命中旧文件。
        const QByteArray cacheIdentity = resourcePath.toUtf8() + originalSvg + colorText.toUtf8();
        const QString cacheName = QString::fromLatin1(QCryptographicHash::hash(
            cacheIdentity, QCryptographicHash::Sha256).toHex()) + QStringLiteral(".svg");
        const QString cachePath = QDir(cacheRoot).filePath(cacheName);
        const QByteArray outputSvg = tintedSvg.toUtf8();
        QFile existingFile(cachePath);
        if (existingFile.open(QIODevice::ReadOnly) && existingFile.readAll() == outputSvg)
        {
            return QDir::fromNativeSeparators(cachePath);
        }
        // Windows 原子替换要求旧读取句柄已释放；缓存损坏时不能持有它再提交新文件。
        existingFile.close();

        // outputFile 用途：原子提交完整 SVG，防止 QSS 读取到写入一半的图形。
        QSaveFile outputFile(cachePath);
        if (!outputFile.open(QIODevice::WriteOnly)
            || outputFile.write(outputSvg) != outputSvg.size()
            || !outputFile.commit())
        {
            return resourcePath;
        }
        return QDir::fromNativeSeparators(cachePath);
    }
}
