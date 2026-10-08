// wpH_tests.SExtra.cpp -- 提交前独立审核（S-wpH）补测，已并入默认运行。
// 每个用例对应审核重放时一个 SURVIVED 的变异（N3 补刷新、UTF-8 块尾截断 E4 41、
// applyTailGiveUp 的 15 字节阈值）；断言的是应有行为，对未变异代码必须 PASS，
// 撤回对应修复/改写边界后必须 FAIL（审核者已实测）。
#include "wpH_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvasFormat.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLineEdit>
#include <QTableView>
#include <QtTest/QtTest>

#include <iostream>

namespace wpH_test
{
    namespace
    {
        std::vector<std::uint8_t> toVecS(const QByteArray& bytes)
        {
            return std::vector<std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
        }

        void loadBytesS(FakeBytesProvider& provider, const std::uint64_t base, const QByteArray& bytes)
        {
            provider.overlay().LoadBaseline(
                QStringLiteral("rigS").toStdString(), base, toVecS(bytes), std::vector<std::uint8_t>(static_cast<std::size_t>(bytes.size()), 1));
        }

        struct RigS
        {
            FakeBytesProvider provider;
            ks::ui::WorkbenchDisasmView view;
            std::uint64_t base;

            explicit RigS(const QByteArray& bytes, const int bits = 64, const std::uint64_t baseAddress = 0x140001000ULL)
                : provider(bits), base(baseAddress)
            {
                loadBytesS(provider, base, bytes);
                view.setBytesProvider(&provider);
                view.setDecodeBackend(MakeRealZydisDecodeBackend());
                view.setAssembleBackend(MakeRealAssembleBackend());
                view.resize(760, 420);
                view.show();
                static_cast<void>(QTest::qWaitForWindowExposed(&view));
                view.jumpTo(base);
                QCoreApplication::processEvents();
            }
        };

        QLineEdit* openEditorS(RigS& rig, const int row)
        {
            rig.view.canvas()->setFocus();
            rig.view.canvas()->setSelectedRow(row);
            QTest::keyClick(rig.view.canvas(), Qt::Key_F2);
            return qobject_cast<QLineEdit*>(QApplication::focusWidget());
        }

        // S18：编辑期间到达的刷新被推迟；此时 setEditable(false) 关闭编辑框，被推迟的刷新必须
        // 在同一次调用里补上（否则表格停在旧内容，直到下一次无关的刷新）。同步断言，不依赖事件循环。
        void runSetEditableAppliesPendingRefreshTests()
        {
            RigS rig(QByteArray::fromHex("554889E590E900000000"));
            auto* editor = openEditorS(rig, 1);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            loadBytesS(rig.provider, rig.base, QByteArray::fromHex("90909090909090909090")); // 编辑期间数据变了
            rig.view.refreshView();                                                          // 被推迟
            WPH_CHECK_NOTE(rig.view.model()->rowAt(1) && rig.view.model()->rowAt(1)->mnemonic == QStringLiteral("mov"),
                QStringLiteral("前置条件：编辑中表格不应被重建"));
            rig.view.setEditable(false);
            WPH_CHECK_NOTE(rig.view.model()->rowAt(1) && rig.view.model()->rowAt(1)->mnemonic == QStringLiteral("nop"),
                QStringLiteral("setEditable(false) 关闭编辑框后，被推迟的刷新必须立即应用，实得 %1")
                    .arg(rig.view.model()->rowAt(1) ? rig.view.model()->rowAt(1)->mnemonic : QString()));
        }

        // S23：UTF-8 引导字节 E4 声明 3 字节序列，块里只剩 2 个字节、且第 2 个字节不是续体。
        // 当前实现先判"被块尾截断"（i+extra >= size），画不可读占位符；把 >= 改成 > 会改成
        // 画 U+FFFD。输入确定、不依赖 UB（第 1 个续体就非法，不会读到块外）。
        void runUtf8TruncationBoundaryTests()
        {
            const std::vector<std::uint8_t> bytes{0xE4, 0x41};
            const QString s = ks::ui::DecodeTextChunkForTest(
                bytes, std::vector<std::uint8_t>(2, 1), ks::ui::WorkbenchTextView::Encoding::Utf8);
            WPH_CHECK_NOTE(s.size() == 2 && s.at(0) == QChar(ks::ui::hexcanvas_format::kUnreadableAsciiGlyph) && s.at(1) == QChar(QLatin1Char('A')),
                QStringLiteral("E4 41：引导字节声明的序列比块还长，应按截断画不可读占位符，实得 U+%1")
                    .arg(s.isEmpty() ? 0U : static_cast<unsigned>(s.at(0).unicode()), 0, 16));
        }

        // S14：rC14 的"等价体"论据——QChar::isPrint() 本身就把 Cf 排除了，Other_Format 分支是死代码。
        void runCfIsAlreadyNotPrintableTests()
        {
            WPH_CHECK(!QChar(0x200B).isPrint());
            WPH_CHECK(!QChar(0xFEFF).isPrint());
            WPH_CHECK(!QChar(0x00AD).isPrint());
        }

        // S24：applyTailGiveUp 阈值边界。06 在 x64 下非法 => 第 0 行是失败的 db。
        // 恰好 15 字节有效（remaining==15）：文档规则"距末尾 15 字节以内"，当前实现 <= 15 触发放弃，
        // 全部逐字节 db；16 字节（remaining==16）则正常解码后面的 nop。
        void runTailGiveUpThresholdTests()
        {
            {
                RigS rig(QByteArray::fromHex("06") + QByteArray(14, '\x90')); // 共 15 字节
                int dbRows = 0;
                int total = 0;
                for (int i = 0; i < rig.view.model()->rowCount(); ++i)
                {
                    const auto row = rig.view.model()->rowAt(i);
                    if (row)
                    {
                        ++total;
                        dbRows += row->mnemonic == QStringLiteral("db") ? 1 : 0;
                    }
                }
                WPH_CHECK_NOTE(total == 15 && dbRows == 15,
                    QStringLiteral("remaining==15 时应整体放弃（15 行 db），实得 total=%1 db=%2").arg(total).arg(dbRows));
            }
            {
                RigS rig(QByteArray::fromHex("06") + QByteArray(15, '\x90')); // 共 16 字节
                const auto row1 = rig.view.model()->rowAt(1);
                WPH_CHECK_NOTE(row1.has_value() && row1->mnemonic == QStringLiteral("nop") && row1->decoded,
                    QStringLiteral("remaining==16 时不应放弃，第 1 行应正常解码为 nop"));
            }
        }
    }

    void RunSExtraTests()
    {
        const int before = g_checks;
        const int beforeFail = g_failures;
        runUtf8TruncationBoundaryTests();
        runCfIsAlreadyNotPrintableTests();
        runTailGiveUpThresholdTests();
        runSetEditableAppliesPendingRefreshTests();
        std::cerr << "[SExtra] checks=" << (g_checks - before) << " failures=" << (g_failures - beforeFail) << std::endl;
    }
}
