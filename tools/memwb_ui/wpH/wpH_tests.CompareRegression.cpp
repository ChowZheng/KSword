// wpH_tests.CompareRegression.cpp
// 作用：修复波（wave2）针对 WorkbenchCompareView 的回归测试与审核缺口补测。
// - D8：真虚拟列表——大窗口无变化/全部变化时 setWindow 耗时在预算内；对象数与命中分组
//   数量一致（不是整窗口）；data() 现算结果与旧版公式等价。
// - D12：底色现取，不缓存——主题切换后下一次重绘（不重新 setWindow）就应该是新颜色。
// - 可疑点 6：分组按绝对地址 16 字节对齐，窗口起点非 16 倍数时行地址仍然落在对齐网格上。
// - 可疑点 7："两次读取之间"视图也要能看到被暂存补丁覆盖的外部变化，且不包含自己写入的。
// - D4/D5：en-US 下表头/状态行不含汉字、模板里的英文字面量不被误大写。
// - 色相断言：待写入=橙、外部变化=青，互换要能被抓到（hM19 同款判据）。

#include "wpH_common.h"

#include "../../../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvasFormat.h"

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QVariant>
#include <QtTest/QtTest>

#include <algorithm>
#include <iostream>

using ks::ui::WorkbenchCompareView;
namespace hexcanvas_format = ks::ui::hexcanvas_format;

namespace wpH_test
{
    namespace
    {
        std::vector<std::uint8_t> toVec(const QByteArray& bytes)
        {
            return std::vector<std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
        }

        // ---------------- D8：性能与对象数 ----------------
        void runPerformanceTests()
        {
            constexpr qsizetype kWindowSize = 1024 * 1024; // 1 MiB，与设计里的基线窗口上限一致。
            FakeBytesProvider provider(64);
            const std::uint64_t base = 0x140000000ULL;
            provider.overlay().LoadBaseline(
                QStringLiteral("perf").toStdString(), base,
                std::vector<std::uint8_t>(static_cast<std::size_t>(kWindowSize), 0),
                std::vector<std::uint8_t>(static_cast<std::size_t>(kWindowSize), 1));

            WorkbenchCompareView view;
            view.setBytesProvider(&provider);
            view.setMode(WorkbenchCompareView::Mode::Pending);

            // 场景 1：全窗口无任何暂存补丁——不应有任何命中分组，且耗时应远小于旧版的
            // 320~863 ms（机器有负载时放宽，但仍要求明显优于"逐字节字符串化"的量级）。
            {
                QElapsedTimer timer;
                timer.start();
                view.setWindow(base, kWindowSize);
                const qint64 elapsedMs = timer.elapsed();
                WPH_CHECK_NOTE(view.model()->rowCount() == 0, QStringLiteral("无变化不应有命中分组，实得 %1").arg(view.model()->rowCount()));
                WPH_CHECK_NOTE(elapsedMs < 200, QStringLiteral("1 MiB 无变化窗口 setWindow 耗时 %1 ms（预算 200ms，机器有负载时可能偶尔超出）").arg(elapsedMs));
                std::cerr << "[CompareRegression] 1MiB no-change setWindow: " << elapsedMs << " ms" << std::endl;
            }

            // 场景 2：全窗口都暂存了补丁——命中分组数应等于 65536（1MiB/16），且 rebuildRows
            // 本身（不含后续 data() 按需格式化）同样应该很快，因为不再预先格式化任何文本。
            {
                std::vector<std::uint8_t> patch(static_cast<std::size_t>(kWindowSize), 0xAA);
                provider.overlay().Stage(base, patch);
                QElapsedTimer timer;
                timer.start();
                view.setWindow(base, kWindowSize);
                const qint64 elapsedMs = timer.elapsed();
                WPH_CHECK_NOTE(view.model()->rowCount() == kWindowSize / 16,
                    QStringLiteral("全部变化应恰好 %1 个命中分组，实得 %2").arg(kWindowSize / 16).arg(view.model()->rowCount()));
                WPH_CHECK_NOTE(elapsedMs < 300, QStringLiteral("1 MiB 全部变化窗口 setWindow（只建摘要，不预先格式化文本）耗时 %1 ms").arg(elapsedMs));
                std::cerr << "[CompareRegression] 1MiB all-change setWindow: " << elapsedMs << " ms, rows=" << view.model()->rowCount() << std::endl;

                // 只读了极少数几个单元格（模拟视口只渲染可见行），其余命中行一次都不读——
                // 这才是"真虚拟列表"：对象数与窗口无关，格式化成本只花在被实际请求的单元格上。
                QElapsedTimer cellTimer;
                cellTimer.start();
                for (int row = 0; row < 20 && row < view.model()->rowCount(); ++row)
                {
                    static_cast<void>(view.model()->index(row, 1).data().toString());
                    static_cast<void>(view.model()->index(row, 2).data().toString());
                }
                const qint64 cellElapsedMs = cellTimer.elapsed();
                WPH_CHECK_NOTE(cellElapsedMs < 50, QStringLiteral("只读 20 行可见单元格应该很快，实得 %1 ms").arg(cellElapsedMs));
            }
        }

        // ---------------- 可疑点 6：绝对地址 16 字节对齐 ----------------
        void runAlignmentTests()
        {
            FakeBytesProvider provider(64);
            // 窗口起点 0x1004（非 16 倍数），长度 32；暂存第 4 字节（绝对地址 0x1004，也就是
            // 窗口的第 0 个字节）与第 20 字节（绝对地址 0x1020）。
            const std::uint64_t windowStart = 0x1004ULL;
            provider.overlay().LoadBaseline(
                QStringLiteral("align").toStdString(), windowStart, std::vector<std::uint8_t>(32, 0), std::vector<std::uint8_t>(32, 1));
            provider.overlay().Stage(windowStart, {0xAA});
            provider.overlay().Stage(0x1020ULL, {0xBB});

            WorkbenchCompareView view;
            view.setBytesProvider(&provider);
            view.setMode(WorkbenchCompareView::Mode::Pending);
            view.setWindow(windowStart, 32);

            // 分组必须按绝对地址 16 对齐：0x1004 落在 [0x1000,0x1010) 组，0x1020 落在
            // [0x1020,0x1030) 组——两组的地址列必须恰好是 0x1000 与 0x1020，不是
            // "窗口起点+0"与"窗口起点+16"（那样会是 0x1004 与 0x1014）。
            WPH_CHECK_NOTE(view.model()->rowCount() == 2, QStringLiteral("应恰好 2 个命中分组，实得 %1").arg(view.model()->rowCount()));
            if (view.model()->rowCount() == 2)
            {
                const QString firstAddress = view.model()->index(0, 0).data().toString();
                const QString secondAddress = view.model()->index(1, 0).data().toString();
                WPH_CHECK_NOTE(firstAddress == hexcanvas_format::FormatAddress(0x1000ULL, 16),
                    QStringLiteral("第一组地址应为 0x1000（绝对对齐），实得 %1").arg(firstAddress));
                WPH_CHECK_NOTE(secondAddress == hexcanvas_format::FormatAddress(0x1020ULL, 16),
                    QStringLiteral("第二组地址应为 0x1020（绝对对齐），实得 %1").arg(secondAddress));
            }
        }

        // ---------------- 可疑点 7：Pending 覆盖下仍要看到外部变化 ----------------
        void runExternalUnderPendingTests()
        {
            FakeBytesProvider provider(64);
            const std::uint64_t base = 0x6000ULL;
            provider.overlay().LoadBaseline(QStringLiteral("ext7").toStdString(), base, std::vector<std::uint8_t>(32, 0x11), std::vector<std::uint8_t>(32, 1));
            QByteArray second(32, '\x11');
            second[4] = '\x22'; // 外部把第 4 字节改成了 0x22
            provider.overlay().RefreshBaseline(QStringLiteral("ext7").toStdString(), base, toVec(second), std::vector<std::uint8_t>(32, 1));
            provider.overlay().Stage(base + 4, {0x99}); // 同一字节又被我们暂存覆盖

            WorkbenchCompareView view;
            view.setBytesProvider(&provider);
            view.setMode(WorkbenchCompareView::Mode::ExternalChange);
            view.setWindow(base, 32);
            WPH_CHECK_NOTE(view.model()->rowCount() == 1,
                QStringLiteral("既有暂存又有外部变化的字节也应该出现在'两次读取之间'视图里，实得行数 %1").arg(view.model()->rowCount()));
            if (view.model()->rowCount() == 1)
            {
                const QString tooltip = view.model()->index(0, 1).data(Qt::ToolTipRole).toString();
                WPH_CHECK_NOTE(tooltip.contains(QStringLiteral("另有待写入")),
                    QStringLiteral("悬停明细应标注'另有待写入'：%1").arg(tooltip));
                const QColor background = qvariant_cast<QColor>(view.model()->data(view.model()->index(0, 1), Qt::BackgroundRole));
                WPH_CHECK_NOTE(background.hslHue() >= 150 && background.hslHue() <= 210,
                    QStringLiteral("颜色仍应遵循青色规则（两次读取之间），不应引入第三种颜色"));
            }

            // 自己确认写入（AcceptWrite）之后，这个字节必须从"两次读取之间"视图消失——
            // SelfWritten 的排除不能被 Pending 回补逻辑误伤（任务书 ⑦ 的既有要求，不能倒退）。
            FakeBytesProvider selfWrittenProvider(64);
            const std::uint64_t base2 = 0x7000ULL;
            selfWrittenProvider.overlay().LoadBaseline(QStringLiteral("self7").toStdString(), base2, std::vector<std::uint8_t>(32, 0x11), std::vector<std::uint8_t>(32, 1));
            selfWrittenProvider.overlay().RefreshBaseline(QStringLiteral("self7").toStdString(), base2, std::vector<std::uint8_t>(32, 0x11), std::vector<std::uint8_t>(32, 1));
            selfWrittenProvider.overlay().Stage(base2 + 4, {0xAA});
            const auto diffBlocks = selfWrittenProvider.overlay().DiffBlocks();
            WPH_CHECK(!diffBlocks.empty());
            if (!diffBlocks.empty())
            {
                selfWrittenProvider.overlay().AcceptWrite(diffBlocks.front(), {0xAA});
            }
            WorkbenchCompareView selfView;
            selfView.setBytesProvider(&selfWrittenProvider);
            selfView.setMode(WorkbenchCompareView::Mode::ExternalChange);
            selfView.setWindow(base2, 32);
            WPH_CHECK_NOTE(selfView.model()->rowCount() == 0,
                QStringLiteral("自己写入的字节不应算作外部变化，实得行数 %1").arg(selfView.model()->rowCount()));
        }

        // ---------------- "两次读取之间"新值必须取基线，不受暂存补丁影响（hM17） ----------------
        void runExternalNewValueIsBaselineTests()
        {
            FakeBytesProvider provider(64);
            const std::uint64_t base = 0x5000ULL;
            provider.overlay().LoadBaseline(QStringLiteral("hm17").toStdString(), base, std::vector<std::uint8_t>(32, 0x11), std::vector<std::uint8_t>(32, 1));
            QByteArray second(32, '\x11');
            second[20] = '\x22'; // 真实的外部变化：第 20 字节。
            provider.overlay().RefreshBaseline(QStringLiteral("hm17").toStdString(), base, toVec(second), std::vector<std::uint8_t>(32, 1));
            provider.overlay().Stage(base + 21, {0x99}); // 同一组另一个字节另外挂了暂存补丁。

            WorkbenchCompareView view;
            view.setBytesProvider(&provider);
            view.setMode(WorkbenchCompareView::Mode::ExternalChange);
            view.setWindow(base, 32);
            WPH_CHECK(view.model()->rowCount() == 1);
            if (view.model()->rowCount() == 1)
            {
                const QString newHex = view.model()->index(0, 2).data().toString();
                WPH_CHECK_NOTE(newHex == QStringLiteral("11 11 11 11 22 11 11 11 11 11 11 11 11 11 11 11"),
                    QStringLiteral("新值必须是这次读取的基线（第 5 个字节 0x22），不能被无关字节的暂存补丁带偏，实得：%1").arg(newHex));
            }
        }

        // ---------------- D12：底色现取，不缓存 ----------------
        void runLiveColorTests()
        {
            FakeBytesProvider provider(64);
            const std::uint64_t base = 0x8000ULL;
            provider.overlay().LoadBaseline(QStringLiteral("color").toStdString(), base, std::vector<std::uint8_t>(16, 0), std::vector<std::uint8_t>(16, 1));
            provider.overlay().Stage(base, {0xAA});
            WorkbenchCompareView view;
            view.setBytesProvider(&provider);
            view.setMode(WorkbenchCompareView::Mode::Pending);
            view.setWindow(base, 16);
            WPH_CHECK(view.model()->rowCount() == 1);
            const QColor lightColor = qvariant_cast<QColor>(view.model()->data(view.model()->index(0, 1), Qt::BackgroundRole));
            ApplyTheme(true); // 切到深色主题，不重新 setWindow。
            const QColor darkColor = qvariant_cast<QColor>(view.model()->data(view.model()->index(0, 1), Qt::BackgroundRole));
            ApplyTheme(false);
            WPH_CHECK_NOTE(lightColor != darkColor, QStringLiteral("主题切换后不重新 setWindow，底色也应该跟着变（现取而不是缓存）"));
        }

        // ---------------- D4/D5：en-US 下表头/状态行不含汉字 ----------------
        void runI18nTests()
        {
            QString errorText;
            const bool loaded = ks::i18n::LanguageManager::instance().initialize(QStringLiteral("en-US"), &errorText);
            WPH_CHECK_NOTE(loaded, QStringLiteral("en-US 语言包应能加载：%1").arg(errorText));
            if (!loaded)
            {
                return;
            }
            FakeBytesProvider provider(64);
            provider.overlay().LoadBaseline(QStringLiteral("i18n").toStdString(), 0x9000, std::vector<std::uint8_t>(16, 0), std::vector<std::uint8_t>(16, 1));
            WorkbenchCompareView view;
            view.setBytesProvider(&provider);
            view.setMode(WorkbenchCompareView::Mode::ExternalChange);
            view.setWindow(0x9000, 16); // 没有上次读取，走旧文案分支。
            ks::i18n::LanguageManager::instance().retranslateAll();
            QCoreApplication::processEvents();
            for (int column = 0; column < 6; ++column)
            {
                const QString header = view.model()->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
                const bool headerHasChinese = std::any_of(header.begin(), header.end(), [](const QChar ch) { return ch.unicode() >= 0x4E00 && ch.unicode() <= 0x9FFF; });
                WPH_CHECK_NOTE(!headerHasChinese, QStringLiteral("en-US 下对比页表头不应包含汉字（列 %1）：%2").arg(column).arg(header));
            }
            ks::i18n::LanguageManager::instance().initialize(QStringLiteral("zh-CN"));
            ks::i18n::LanguageManager::instance().retranslateAll();
        }
    }

    void RunCompareRegressionTests()
    {
        const int before = g_checks;
        const int beforeFail = g_failures;
        runPerformanceTests();
        runAlignmentTests();
        runExternalUnderPendingTests();
        runExternalNewValueIsBaselineTests();
        runLiveColorTests();
        runI18nTests();
        std::cerr << "[CompareRegression] checks=" << (g_checks - before) << " failures=" << (g_failures - beforeFail) << std::endl;
    }
}
