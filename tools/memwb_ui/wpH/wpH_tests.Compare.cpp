// wpH_tests.Compare.cpp
// 作用：WorkbenchCompareView 的验证：
// - "待写入修改"分段：暂存补丁产生的行、颜色、变化数；
// - "两次读取之间"分段：RefreshBaseline 产生的"上次读取"差异行；没有上次读取时的旧文案；
// - 对象数：只有命中当前分段的 16 字节分组才会成行（不是整窗口逐行铺开）；
// - 截图。

#include "wpH_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvasFormat.h"

#include <QCoreApplication>
#include <QHeaderView>
#include <QLabel>
#include <QTableView>
#include <QtTest/QtTest>

#include <iostream>

using ks::ui::WorkbenchCompareView;
namespace hexcanvas_format = ks::ui::hexcanvas_format;

namespace wpH_test
{
    namespace
    {
        // runPendingModeTests："待写入修改"分段：暂存两处补丁，只有这两组 16 字节分组成行。
        void runPendingModeTests()
        {
            FakeBytesProvider provider;
            const QByteArray bytes(64, '\x00');
            const std::uint64_t base = 0x4000;
            provider.overlay().LoadBaseline(QStringLiteral("pending").toStdString(), base,
                std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                std::vector<std::uint8_t>(static_cast<std::size_t>(bytes.size()), 1));
            // 第 0 组（0x4000-0x400F）暂存 1 字节；第 2 组（0x4020-0x402F）暂存 2 字节；
            // 第 1、3 组不改动，不应出现在结果里。
            provider.overlay().Stage(base + 0x03, {0xAA});
            provider.overlay().Stage(base + 0x21, {0xBB, 0xCC});

            WorkbenchCompareView view;
            view.setBytesProvider(&provider);
            view.setMode(WorkbenchCompareView::Mode::Pending);
            view.setWindow(base, static_cast<std::uint64_t>(bytes.size()));

            WPH_CHECK_NOTE(view.model()->rowCount() == 2, QStringLiteral("rowCount=%1").arg(view.model()->rowCount()));
            if (view.model()->rowCount() == 2)
            {
                const QModelIndex row0Addr = view.model()->index(0, 0);
                const QModelIndex row0Count = view.model()->index(0, 5);
                const QModelIndex row1Addr = view.model()->index(1, 0);
                const QModelIndex row1Count = view.model()->index(1, 5);
                WPH_CHECK_NOTE(row0Addr.data().toString() == hexcanvas_format::FormatAddress(base, 16),
                    row0Addr.data().toString());
                WPH_CHECK(row0Count.data().toInt() == 1);
                WPH_CHECK(row1Addr.data().toString() == hexcanvas_format::FormatAddress(base + 0x20, 16));
                WPH_CHECK(row1Count.data().toInt() == 2);
                // 待写入修改的底色应该是有效颜色（橙），"变化数"列应命中着色条件。
                const QVariant background = view.model()->data(view.model()->index(0, 2), Qt::BackgroundRole);
                WPH_CHECK(background.canConvert<QColor>() && qvariant_cast<QColor>(background).isValid());
                // 旧值必须取基线（未叠加补丁）、新值必须取叠加后的现值——不能两边取同一个源，
                // 否则"待写入修改"看不出真正要写入的内容。
                const QString oldHex = view.model()->index(0, 1).data().toString();
                const QString newHex = view.model()->index(0, 2).data().toString();
                WPH_CHECK_NOTE(oldHex.startsWith(QStringLiteral("00 00 00 00")), oldHex);
                WPH_CHECK_NOTE(newHex.startsWith(QStringLiteral("00 00 00 AA")), newHex);
            }
        }

        // runExternalModeTests："两次读取之间"分段：RefreshBaseline 制造一次真实的外部变化。
        void runExternalModeTests()
        {
            FakeBytesProvider provider;
            const QByteArray first(32, '\x11');
            const std::uint64_t base = 0x5000;
            provider.overlay().LoadBaseline(QStringLiteral("ext").toStdString(), base,
                std::vector<std::uint8_t>(first.begin(), first.end()),
                std::vector<std::uint8_t>(static_cast<std::size_t>(first.size()), 1));

            WorkbenchCompareView view;
            view.setBytesProvider(&provider);
            view.setMode(WorkbenchCompareView::Mode::ExternalChange);
            view.setWindow(base, static_cast<std::uint64_t>(first.size()));
            WPH_CHECK_NOTE(view.model()->rowCount() == 0, QStringLiteral("尚无上次读取前应为空，实得 %1").arg(view.model()->rowCount()));

            // 第二次读取：只改第 20 字节（属于第 1 组 0x5010-0x501F），应恰好产生 1 行、1 处变化。
            QByteArray second = first;
            second[20] = '\x22';
            provider.overlay().RefreshBaseline(QStringLiteral("ext").toStdString(), base,
                std::vector<std::uint8_t>(second.begin(), second.end()),
                std::vector<std::uint8_t>(static_cast<std::size_t>(second.size()), 1));
            view.refreshView();
            WPH_CHECK_NOTE(view.model()->rowCount() == 1, QStringLiteral("应恰好 1 行，实得 %1").arg(view.model()->rowCount()));
            if (view.model()->rowCount() == 1)
            {
                WPH_CHECK(view.model()->index(0, 0).data().toString() == hexcanvas_format::FormatAddress(base + 0x10, 16));
                WPH_CHECK(view.model()->index(0, 5).data().toInt() == 1);
                // 旧值必须取上次读取（全 11），新值必须取这次的基线（第 5 个字节变成 22）——
                // 两次读取之间对比的是目标自己变了没有，与任何暂存补丁无关。
                const QString oldHex = view.model()->index(0, 1).data().toString();
                const QString newHex = view.model()->index(0, 2).data().toString();
                WPH_CHECK_NOTE(oldHex == QStringLiteral("11 11 11 11 11 11 11 11 11 11 11 11 11 11 11 11"), oldHex);
                WPH_CHECK_NOTE(newHex == QStringLiteral("11 11 11 11 22 11 11 11 11 11 11 11 11 11 11 11"), newHex);
            }
        }

        // runNoPreviousReadTests：没有上次读取时必须显示旧文案并隐藏行，不是空表格硬凹。
        void runNoPreviousReadTests()
        {
            FakeBytesProvider provider;
            const QByteArray bytes(16, '\x00');
            provider.overlay().LoadBaseline(QStringLiteral("solo").toStdString(), 0x6000,
                std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                std::vector<std::uint8_t>(static_cast<std::size_t>(bytes.size()), 1));
            WPH_CHECK(!provider.HasPreviousRead());

            WorkbenchCompareView view;
            view.setBytesProvider(&provider);
            view.setMode(WorkbenchCompareView::Mode::ExternalChange);
            view.setWindow(0x6000, static_cast<std::uint64_t>(bytes.size()));
            WPH_CHECK(view.model()->rowCount() == 0);
            auto* status = view.findChild<QLabel*>(QStringLiteral("ksMemwbCompareStatus"));
            WPH_CHECK_NOTE(status != nullptr && status->text() == QStringLiteral("没有上次相同目标和范围的读取可供对比。"),
                status != nullptr ? status->text() : QStringLiteral("<no status label>"));
        }

        void takeShots(const QString& shotsDir)
        {
            if (shotsDir.isEmpty())
            {
                return;
            }
            FakeBytesProvider provider;
            const QByteArray bytes(48, '\x00');
            provider.overlay().LoadBaseline(QStringLiteral("shot").toStdString(), 0x7000,
                std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                std::vector<std::uint8_t>(static_cast<std::size_t>(bytes.size()), 1));
            provider.overlay().Stage(0x7001, {0x5A, 0x5B});
            WorkbenchCompareView view;
            view.setBytesProvider(&provider);
            view.setWindow(0x7000, static_cast<std::uint64_t>(bytes.size()));
            view.resize(760, 300);
            view.show();
            static_cast<void>(QTest::qWaitForWindowExposed(&view));
            GrabWidget(&view).save(shotsDir + QStringLiteral("/wpH_compare_pending.png"));
        }
    }

    void RunCompareTests(const QString& shotsDir)
    {
        const int before = g_checks;
        const int beforeFail = g_failures;
        runPendingModeTests();
        runExternalModeTests();
        runNoPreviousReadTests();
        takeShots(shotsDir);
        std::cerr << "[Compare] checks=" << (g_checks - before) << " failures=" << (g_failures - beforeFail) << std::endl;
    }
}
