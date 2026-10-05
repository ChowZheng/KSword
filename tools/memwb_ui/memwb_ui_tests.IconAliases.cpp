// memwb_ui_tests.IconAliases.cpp
// 作用：Phase 3 WP-0 的离屏验证（第三组）——Ksword5.qrc 新增的 11 个 memwb_* 图标别名：
//   1) 夹具精简 qrc 里的别名都能解析成非空、真的画得出像素的图标（证明 svg 与图标引擎插件可用）；
//   2) 主程序 Ksword5.qrc 里每个别名恰好出现一次、指向设计文档（ux.md 第 8 节）指定的源 svg，
//      且源文件在仓库里真实存在、别名总数恰好是 11（没有多加、没有重复）；
//   3) 夹具精简 qrc 与主程序 qrc 对同一别名指向同一个 svg（两份不会悄悄分叉）。
// 必须在仓库根目录运行（构建脚本 build-memwb-ui-tests.cmd 已 pushd 到仓库根）。

#include "memwb_ui_common.h"

#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QPixmap>

#include <iostream>

namespace memwb_test
{
    namespace
    {
        // AliasSpec：一个别名与它应当指向的源 svg（相对 Resource/Icon 目录，取自 ux.md 第 8 节）。
        struct AliasSpec
        {
            const char* alias;      // 别名（不含 .svg 扩展名）
            const char* source;     // 源文件相对 Resource/Icon 的路径
        };

        // kAliases：11 个别名，顺序与 ux.md 的按钮清单一致。
        const AliasSpec kAliases[] = {
            { "memwb_mode_immediate", "device/flash_line.svg" },
            { "memwb_mode_staged", "file/inbox_line.svg" },
            { "memwb_bookmarks", "education/bookmarks_line.svg" },
            { "memwb_bookmark_add", "education/bookmark_add_line.svg" },
            { "memwb_restore", "system/restore_line.svg" },
            { "memwb_restore_all", "system/history_anticlockwise_line.svg" },
            { "memwb_assemble", "development/terminal_box_line.svg" },
            { "memwb_tab_hex", "editor/hashtag_line.svg" },
            { "memwb_tab_disasm", "development/code_line.svg" },
            { "memwb_tab_text", "editor/text_line.svg" },
            { "memwb_tab_compare", "development/git_compare_line.svg" } };

        // ReadText：读一个文本文件（UTF-8）；失败返回空串。
        QString ReadText(const QString& path)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
            {
                return QString();
            }
            return QString::fromUtf8(file.readAll());
        }

        // AliasLine：某个别名在 qrc 里应出现的完整一行内容（不含缩进与换行）。
        // 传入：别名、qrc 里 file 元素的路径文字；传出：形如 <file alias="x.svg">path</file> 的字符串。
        QString AliasLine(const char* alias, const QString& path)
        {
            return QStringLiteral("<file alias=\"%1.svg\">%2</file>").arg(QString::fromLatin1(alias), path);
        }

        // 夹具精简 qrc 解析出的图标：非空，且渲染 16x16 后至少有一个不透明像素。
        void TestFixtureIconsResolve()
        {
            for (const AliasSpec& spec : kAliases)
            {
                const QString resource = QStringLiteral(":/Icon/%1.svg").arg(QString::fromLatin1(spec.alias));
                const QIcon icon(resource);
                CHECK_NOTE(!icon.isNull(), resource);
                const QImage image = icon.pixmap(QSize(16, 16)).toImage().convertToFormat(QImage::Format_ARGB32);
                int opaque = 0;
                for (int y = 0; y < image.height(); ++y)
                {
                    for (int x = 0; x < image.width(); ++x)
                    {
                        opaque += (qAlpha(image.pixel(x, y)) > 0) ? 1 : 0;
                    }
                }
                CHECK_NOTE(!image.isNull() && opaque > 0, resource);
            }
        }

        // 主程序 qrc 与夹具 qrc：别名恰好一次、指向指定 svg、源文件存在、总数恰好 11、两份一致。
        void TestQrcFiles()
        {
            const QString mainQrcPath = QStringLiteral("Ksword5.1/Ksword5.1/Ksword5.qrc");
            const QString fixtureQrcPath = QStringLiteral("tools/memwb_ui/memwb_ui_icons.qrc");
            const QString mainText = ReadText(mainQrcPath);
            const QString fixtureText = ReadText(fixtureQrcPath);
            CHECK_NOTE(!mainText.isEmpty(), QStringLiteral("cannot read %1 (run from the repository root)").arg(mainQrcPath));
            CHECK_NOTE(!fixtureText.isEmpty(), QStringLiteral("cannot read %1").arg(fixtureQrcPath));

            for (const AliasSpec& spec : kAliases)
            {
                const QString source = QString::fromLatin1(spec.source);
                const QString mainLine = AliasLine(spec.alias, QStringLiteral("Resource/Icon/") + source);
                const QString fixtureLine = AliasLine(
                    spec.alias,
                    QStringLiteral("../../Ksword5.1/Ksword5.1/Resource/Icon/") + source);

                // 主程序 qrc：该别名的完整一行恰好出现一次，且这个别名名字只出现在这一行里（没有第二个同名别名）。
                CHECK_NOTE(mainText.count(mainLine) == 1, mainLine);
                const QString aliasMarker = QStringLiteral("alias=\"%1.svg\"").arg(QString::fromLatin1(spec.alias));
                CHECK_NOTE(mainText.count(aliasMarker) == 1, aliasMarker);

                // 夹具 qrc：同一别名指向同一个 svg。
                CHECK_NOTE(fixtureText.count(fixtureLine) == 1, fixtureLine);

                // 源文件在仓库里真实存在（相对仓库根）。
                const QString sourcePath = QStringLiteral("Ksword5.1/Ksword5.1/Resource/Icon/") + source;
                CHECK_NOTE(QFileInfo::exists(sourcePath), sourcePath);
            }

            // 别名总数：主程序与夹具里以 memwb_ 开头的别名都恰好 11 个（没有多加、没有漏掉）。
            const QString prefixMarker = QStringLiteral("alias=\"memwb_");
            CHECK_NOTE(mainText.count(prefixMarker) == 11, QStringLiteral("main memwb aliases=%1").arg(mainText.count(prefixMarker)));
            CHECK_NOTE(fixtureText.count(prefixMarker) == 11, QStringLiteral("fixture memwb aliases=%1").arg(fixtureText.count(prefixMarker)));
        }
    }

    // 入口：图标别名的全部验证，单独报告这一组的断言数与失败数。
    void RunIconAliasTests()
    {
        const int checksBefore = g_checks;
        const int failuresBefore = g_failures;
        TestFixtureIconsResolve();
        TestQrcFiles();
        std::cout << "icon alias tests: " << (g_checks - checksBefore) << " checks, "
                  << (g_failures - failuresBefore) << " failures" << std::endl;
    }
}
