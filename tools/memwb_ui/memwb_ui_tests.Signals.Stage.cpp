// memwb_ui_tests.Signals.Stage.cpp
// 作用：第二轮接口补全的"画布侧"离屏验证（第二组）——公开的 stageBytes：
//   成功路径、每种拒绝原因（只读/未加载/不可读/跨页/空/范围/64 位溢出/叠加层窗口与基线/总量上限）、
//   已有暂存补丁的字节页被淘汰后仍可改、键盘编辑与粘贴与填充共用同一套检查、超大选区填充早退。
// 全部经公开接口或 QTest 模拟的真实输入进入画布，结果从叠加层与信号读回。

#include "memwb_ui_signals.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QSignalSpy>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using Bytes = std::vector<std::uint8_t>;

        // 公开 stageBytes 的成功路径：返回 true、清空原因、editStaged 一次、叠加层与显示都更新、不动插入点。
        void TestStageBytesSuccess()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), true, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            QSignalSpy rejected(&canvas, &HexCanvas::editRejected);
            canvas.setCaretAddress(0x1100);

            QString reason = QStringLiteral("stale");
            const bool ok = canvas.stageBytes(0x1010, QByteArray("\x11\x22\x33", 3), &reason);
            CHECK(ok);
            CHECK(reason.isEmpty());
            CHECK(staged.count() == 1);
            CHECK(staged.count() == 1 && staged.at(0).at(0).toULongLong() == 0x1010 && staged.at(0).at(1).toULongLong() == 3);
            CHECK(rejected.isEmpty());
            CHECK(Effective(fixture->overlay, 0x1010, 3) == Bytes({ 0x11, 0x22, 0x33 }));
            CHECK(canvas.cellStateAt(0x1011).change == HexCanvas::ChangeKind::Pending);
            CHECK(canvas.cellStateAt(0x1011).hexText == QStringLiteral("22"));
            CHECK(canvas.caretAddress() == 0x1100);

            // 原因输出可以是空指针；覆盖已暂存的字节不增加总量，仍成功。
            CHECK(canvas.stageBytes(0x1011, QByteArray("\x44", 1)));
            CHECK(staged.count() == 2);
            CHECK(Effective(fixture->overlay, 0x1010, 3) == Bytes({ 0x11, 0x44, 0x33 }));

            // 与现值相同的写入是叠加层的空操作，仍算成功。
            CHECK(canvas.stageBytes(0x1011, QByteArray("\x44", 1), &reason));
            CHECK(reason.isEmpty());
            CHECK(rejected.isEmpty());
        }

        // 公开 stageBytes 的拒绝路径：每种原因都有文案、同时发 editRejected 与填 reasonOut、叠加层不变。
        void TestStageBytesRejections()
        {
            ApplyTheme(false);

            // 只读：未允许编辑 / 允许编辑但没有叠加层。
            {
                auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), false, QSize(900, 420));
                HexCanvas& canvas = *fixture->canvas;
                QSignalSpy staged(&canvas, &HexCanvas::editStaged);
                QSignalSpy rejected(&canvas, &HexCanvas::editRejected);
                QString reason;
                CHECK(!canvas.stageBytes(0x1010, QByteArray("\x01", 1), &reason));
                CHECK(reason.contains(QStringLiteral("只读")));
                CHECK(rejected.count() == 1);
                CHECK(rejected.count() == 1 && rejected.at(0).at(0).toString() == reason);
                CHECK(staged.isEmpty());
                CHECK(!fixture->overlay.HasPendingPatches());

                canvas.setEditable(true);
                canvas.setOverlay(nullptr);
                reason.clear();
                CHECK(!canvas.stageBytes(0x1010, QByteArray("\x01", 1), &reason));
                CHECK(reason.contains(QStringLiteral("只读")));
                CHECK(rejected.count() == 2);

                // 空指针原因输出不崩溃，信号照发。
                CHECK(!canvas.stageBytes(0x1010, QByteArray("\x01", 1), nullptr));
                CHECK(rejected.count() == 3);
            }

            // 页缓存层：未加载 / 不可读 / 跨页 / 范围与溢出。
            {
                auto holder = MakeAsyncCanvas(0x10000, 0x13FFF, true, true);
                HexCanvas& canvas = *holder->canvas;
                QSignalSpy staged(&canvas, &HexCanvas::editStaged);
                QSignalSpy rejected(&canvas, &HexCanvas::editRejected);
                CHECK(DeliverPage(canvas, 0x10000) == HexCanvas::PageResult::Accepted);
                ks::ui::HexFetchRange unreadable;
                unreadable.firstPageStart = 0x12000;
                unreadable.pageCount = 1;
                CHECK(canvas.deliverUnreadable(unreadable, canvas.sourceRevision()) == HexCanvas::PageResult::Accepted);
                const auto snapshot = holder->overlay.DiffBlocks();

                // 已加载页：成功。
                QString reason;
                CHECK(canvas.stageBytes(0x10010, QByteArray("\x11\x22", 2), &reason));

                // 单字节未加载 / 多字节未加载（措辞不同，都含"尚未加载"）。
                CHECK(!canvas.stageBytes(0x11010, QByteArray("\x01", 1), &reason));
                CHECK(reason == QStringLiteral("该字节尚未加载，不能编辑"));
                CHECK(!canvas.stageBytes(0x11010, QByteArray("\x01\x02", 2), &reason));
                CHECK(reason == QStringLiteral("写入范围内有字节尚未加载，不能编辑"));

                // 不可读：单字节 / 多字节。
                CHECK(!canvas.stageBytes(0x12000, QByteArray("\x01", 1), &reason));
                CHECK(reason == QStringLiteral("该字节不可读，不能编辑"));
                CHECK(!canvas.stageBytes(0x12000, QByteArray("\x01\x02", 2), &reason));
                CHECK(reason == QStringLiteral("写入范围内有字节不可读，不能编辑"));

                // 跨页：前半在已加载页、后半在未加载页，整体拒绝且不留任何半截暂存。
                const auto afterFirst = holder->overlay.DiffBlocks();
                CHECK(!canvas.stageBytes(0x10FFF, QByteArray("\x01\x02", 2), &reason));
                CHECK(reason.contains(QStringLiteral("尚未加载")));
                CHECK(holder->overlay.DiffBlocks() == afterFirst);
                CHECK(Effective(holder->overlay, 0x10FFF, 1) != Bytes({ 0x01 }));

                // 空字节、地址空间之外、范围越过空间末尾。
                CHECK(!canvas.stageBytes(0x10010, QByteArray(), &reason));
                CHECK(reason == QStringLiteral("没有可写入的字节"));
                CHECK(!canvas.stageBytes(0x14000, QByteArray("\x01", 1), &reason));
                CHECK(reason == QStringLiteral("该地址不在可编辑的地址空间内"));
                CHECK(!canvas.stageBytes(0x13FFF, QByteArray("\x01\x02", 2), &reason));
                CHECK(reason == QStringLiteral("写入范围超出了地址空间"));
                CHECK(!canvas.stageBytes(0x0FFFF, QByteArray("\x01\x02", 2), &reason));
                CHECK(reason == QStringLiteral("写入范围超出了地址空间"));

                // 每次失败都发了 editRejected（上面共 9 次拒绝），成功只有一次 editStaged，叠加层只含那一次成功的暂存。
                CHECK(rejected.count() == 9);
                CHECK(staged.count() == 1);
                CHECK(holder->overlay.DiffBlocks() != snapshot);
                CHECK(holder->overlay.DiffBlocks().size() == 1);

                // 已有暂存补丁的字节即使页被淘汰（refresh 清缓存且提供者不回填）仍有值可显示，可以继续改；没补丁的不行。
                canvas.refresh();
                CHECK(canvas.cellStateAt(0x10010).hasValue);
                CHECK(!canvas.cellStateAt(0x10012).hasValue);
                CHECK(canvas.stageBytes(0x10010, QByteArray("\x77", 1), &reason));
                CHECK(!canvas.stageBytes(0x10012, QByteArray("\x77", 1), &reason));
                CHECK(reason == QStringLiteral("该字节尚未加载，不能编辑"));
            }

            // 64 位溢出：终点超过 2^64 先于范围检查被拒。
            {
                HexCanvas giant;
                RecordingProvider provider;
                ksword::memwb::MemoryDiffOverlay overlay;
                giant.resize(600, 300);
                giant.show();
                giant.setAddressSpace(0, 0xFFFFFFFFFFFFFFFFULL);
                giant.setPageProvider(&provider);
                giant.setOverlay(&overlay);
                giant.setEditable(true);
                QString reason;
                CHECK(!giant.stageBytes(0xFFFFFFFFFFFFFFF0ULL, QByteArray(0x20, 'a'), &reason));
                CHECK(reason == QStringLiteral("写入范围超出 64 位地址空间"));
            }

            // 叠加层层面的拒绝：基线之外（数据窗口）、基线里没读到的字节、暂存总量上限。
            {
                auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), true, QSize(900, 420), false);
                HexCanvas& canvas = *fixture->canvas;
                QSignalSpy rejected(&canvas, &HexCanvas::editRejected);
                QString reason;

                // 没有基线：按"超出数据窗口"处理。
                CHECK(!canvas.stageBytes(0x1010, QByteArray("\x01", 1), &reason));
                CHECK(reason.contains(QStringLiteral("数据窗口")));

                Bytes bytes(0x100, 0x11);
                Bytes mask(0x100, 1);
                mask[0x20] = 0;
                fixture->overlay.LoadBaseline("memwb-signals-window", 0x1000, bytes, mask);
                CHECK(!canvas.stageBytes(0x1500, QByteArray("\x01", 1), &reason));
                CHECK(reason.contains(QStringLiteral("数据窗口")));
                CHECK(!canvas.stageBytes(0x1020, QByteArray("\x01", 1), &reason));
                CHECK(reason.contains(QStringLiteral("尚未读取")));
                CHECK(rejected.count() == 3);
                CHECK(!fixture->overlay.HasPendingPatches());

                ksword::memwb::MemoryDiffOverlay tiny(4);
                canvas.setOverlay(&tiny);
                tiny.LoadBaseline("memwb-signals-tiny", 0x1000, Bytes(0x1000, 0x22), Bytes(0x1000, 1));
                CHECK(canvas.stageBytes(0x1000, QByteArray("\x01\x02\x03\x04", 4), &reason));
                CHECK(!canvas.stageBytes(0x1010, QByteArray("\x01", 1), &reason));
                CHECK(reason == QStringLiteral("暂存的修改总量超过上限"));
                canvas.setOverlay(nullptr);
            }
        }

        // 键盘编辑、粘贴、填充与 stageBytes 共用同一套检查：只有共用的预检才会产生这些原文。
        void TestEntryPointsShareChecks()
        {
            ApplyTheme(false);
            auto holder = MakeAsyncCanvas(0x10000, 0x13FFF, true, true);
            HexCanvas& canvas = *holder->canvas;
            QSignalSpy rejected(&canvas, &HexCanvas::editRejected);
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            CHECK(DeliverPage(canvas, 0x10000) == HexCanvas::PageResult::Accepted);
            const auto snapshot = holder->overlay.DiffBlocks();

            // 键盘：单字节落在未加载页，第一个半字节就被拒。
            canvas.setCaretAddress(0x11005);
            Type(canvas, QStringLiteral("4"));
            CHECK(rejected.count() == 1);
            CHECK(rejected.count() == 1 && rejected.at(0).at(0).toString() == QStringLiteral("该字节尚未加载，不能编辑"));

            // 填充：选区跨进未加载页，整体拒绝，用的是多字节措辞，且没有为注定被拒的请求暂存任何字节。
            canvas.setCaretAddress(0x10FF0);
            canvas.setCaretAddress(0x11010, true);
            canvas.fillSelection(0xAA);
            CHECK(rejected.count() == 2);
            CHECK(rejected.count() == 2 && rejected.at(1).at(0).toString() == QStringLiteral("写入范围内有字节尚未加载，不能编辑"));

            // 粘贴：同样的检查，同样的措辞。
            QGuiApplication::clipboard()->setText(QStringLiteral("11 22 33"));
            canvas.setCaretAddress(0x11000);
            Key(canvas, Qt::Key_V, Qt::ControlModifier);
            CHECK(rejected.count() == 3);
            CHECK(rejected.count() == 3 && rejected.at(2).at(0).toString() == QStringLiteral("写入范围内有字节尚未加载，不能编辑"));
            CHECK(staged.isEmpty());
            CHECK(holder->overlay.DiffBlocks() == snapshot);

            // 已加载页里三条路径都成功，且都只经 stageBytes 发出恰好一个 editStaged。
            canvas.setCaretAddress(0x10100);
            Type(canvas, QStringLiteral("4A"));
            CHECK(staged.count() == 1);
            canvas.setCaretAddress(0x10200);
            canvas.setCaretAddress(0x1020F, true);
            canvas.fillSelection(0xFF);
            CHECK(staged.count() == 2);
            canvas.setCaretAddress(0x10300);
            Key(canvas, Qt::Key_V, Qt::ControlModifier);
            CHECK(staged.count() == 3);
            CHECK(Effective(holder->overlay, 0x10100, 1) == Bytes({ 0x4A }));
            CHECK(Effective(holder->overlay, 0x10200, 16) == Bytes(16, 0xFF));
            CHECK(Effective(holder->overlay, 0x10300, 3) == Bytes({ 0x11, 0x22, 0x33 }));
            CHECK(rejected.count() == 3);
        }

        // 超大选区的填充：先预检再分配——2 MiB 全选里绝大部分页没加载，直接拒绝，且很快（早退，不遍历 2 MiB）。
        void TestFillBeyondLoadedPages()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(2 * 1024 * 1024), true, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            QSignalSpy rejected(&canvas, &HexCanvas::editRejected);
            canvas.selectAll();
            QElapsedTimer timer;
            timer.start();
            canvas.fillSelection(0xAA);
            const qint64 elapsedMs = timer.elapsed();
            CHECK(rejected.count() == 1);
            CHECK(rejected.count() == 1 && rejected.at(0).at(0).toString().contains(QStringLiteral("尚未加载")));
            CHECK(!fixture->overlay.HasPendingPatches());
            CHECK_NOTE(elapsedMs < 1500, QString::number(elapsedMs));

            // 暂存上限（64 MiB）正好大小的选区：一页都没加载，预检在第一个字节就早退。
            // 如果实现改成"先把整段 64 MiB 扫完再报错"，逐字节查询要几秒；早退只要一两毫秒，所以 300 ms 的界线两头都留足余量。
            auto holder = MakeAsyncCanvas(0x100000, 0x100000 + 64ULL * 1024ULL * 1024ULL - 1ULL, true, false);
            QSignalSpy bigRejected(holder->canvas.get(), &HexCanvas::editRejected);
            holder->canvas->selectAll();
            timer.start();
            holder->canvas->fillSelection(0x00);
            const qint64 bigMs = timer.elapsed();
            CHECK(bigRejected.count() == 1);
            CHECK(bigRejected.count() == 1 && bigRejected.at(0).at(0).toString().contains(QStringLiteral("尚未加载")));
            CHECK(!holder->overlay.HasPendingPatches());
            CHECK_NOTE(bigMs < 300, QString::number(bigMs));
        }
    }

    // 第二组入口：stageBytes。
    void RunSignalStageTests()
    {
        TestStageBytesSuccess();
        TestStageBytesRejections();
        TestEntryPointsShareChecks();
        TestFillBeyondLoadedPages();
    }
}
