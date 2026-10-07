// wpJ4_tests.Undo.cpp
// 作用：撤销/重做关键判断——
//   - 正常撤销：向目标补写旧值，journal 游标后退一步，undoRedoAvailabilityChanged
//     在可用性真的变化时才发。
//   - Undo()/Redo() 本身不向 journal 记新的一步（连续撤销不会把历史越撤越长）。
//   - 目标已变（写前复核与 journal 记录的 expectedCurrent 不符）返回
//     TargetChanged，游标不动，可以重试。
//   - 换目标（身份类变更）清空撤销历史。

#include "wpJ4_common.h"

namespace wpj4_test
{
    namespace
    {
        // 辅助：提交一次简单的单字节编辑，使用立即模式；调用方必须已经
        // LoadBaseline 覆盖 address。
        void CommitOneEdit(
            Harness& h, std::uint64_t address, std::uint8_t beforeValue, std::uint8_t afterValue)
        {
            WPJ4_CHECK(h.Stage(address, {afterValue}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({beforeValue}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({afterValue}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.onEditCompleted();
        }

        // M-U1：连续两次编辑 -> 两步历史；撤销一次应该补写第二次编辑之前的值，
        // 游标后退，Undo 本身不记新的一步（历史步数不会因为撤销而增加）。
        void TestUndoReplaysPreviousValue()
        {
            Harness h;
            h.AttachProcess(400, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            // 一次性载入一段覆盖两个地址的基线窗口（LoadBaseline 会整段替换窗口，
            // 分两次调用会让第二次把第一个地址挤出窗口）。
            h.LoadBaseline(0xC000, std::vector<std::uint8_t>(0x200, 0));

            CommitOneEdit(h, 0xC000, 0x00, 0x10);
            CommitOneEdit(h, 0xC100, 0x00, 0x20);

            WPJ4_CHECK(h.controller.canUndo() == true);
            WPJ4_CHECK(h.controller.canRedo() == false);

            int availabilityChanged = 0;
            QObject::connect(
                &h.controller, &ks::ui::WorkbenchWriteController::undoRedoAvailabilityChanged,
                [&]() { ++availabilityChanged; });

            // 撤销最近一步（地址 0xC100）：写前复核读到"after"=0x20，回读核对
            // "restore"=0x00。
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x20}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));

            h.controller.undo();
            WPJ4_CHECK_NOTE(
                availabilityChanged >= 1,
                QStringLiteral("撤销成功后 canRedo() 从 false 变 true，应该发一次可用性信号"));
            WPJ4_CHECK(h.controller.canUndo() == true); // 第一步仍可撤销。
            WPJ4_CHECK(h.controller.canRedo() == true); // 刚撤销的这一步可以重做。
            WPJ4_CHECK(h.rawPort->writeCalls.size() == 3); // 两次编辑 + 一次撤销补写。

            // 再撤销一次（地址 0xC000）之后，没有更多历史：第三次 undo() 应该是
            // NothingToReplay，canUndo() 变为 false，不应该再碰端口。
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x10}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.undo();
            WPJ4_CHECK(h.controller.canUndo() == false);
            WPJ4_CHECK(h.controller.canRedo() == true);

            const std::size_t writeCallsBeforeNoopUndo = h.rawPort->writeCalls.size();
            int noOpReports = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFinished,
                [&](ksword::memwb::CommitReport) { ++noOpReports; });
            h.controller.undo(); // 没有更多可撤销步骤：应是空操作。
            WPJ4_CHECK_NOTE(
                h.rawPort->writeCalls.size() == writeCallsBeforeNoopUndo,
                QStringLiteral("NothingToReplay 不应该触碰端口"));
            WPJ4_CHECK_NOTE(noOpReports == 0,
                QStringLiteral("没有回放历史时不能虚构 CommitReport"));
        }

        // M-U2：撤销本身不向 journal 记新的一步——连续撤销/重做若干轮之后，
        // 可撤销总步数应该保持不变（这里用"撤销两步、重做两步、再撤销两步都能
        // 成功"间接验证历史没有被撤销动作污染出新的步骤）。
        void TestUndoRedoDoNotGrowHistory()
        {
            Harness h;
            h.AttachProcess(401, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xC200, {0x00});
            CommitOneEdit(h, 0xC200, 0x00, 0x30);

            // 撤销、重做各三次：如果 Undo/Redo 会向 journal 记新的一步，第二轮
            // undo 就会撤销出"撤销本身"这一步，行为会明显偏离（例如 canRedo 在
            // 奇怪的时机变化）。这里只断言"每一轮撤销/重做都成功，且最终字节
            // 回到 afterValue"这个可观察结果。
            for (int round = 0; round < 3; ++round)
            {
                h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x30}));
                h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
                h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
                h.controller.undo();
                WPJ4_CHECK(h.controller.canUndo() == false);
                WPJ4_CHECK(h.controller.canRedo() == true);

                h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
                h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x30}));
                h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
                h.controller.redo();
                WPJ4_CHECK(h.controller.canUndo() == true);
                WPJ4_CHECK(h.controller.canRedo() == false);
            }
        }

        // M-U3（**第二轮审核 C2 测试缺口补测后扩展**）：目标已变（写前复核读到的
        // "当前字节"与 journal 记录的 expectedCurrent 不符）——整步拒绝，游标
        // 不动，端口没有发生任何写入；失败报告必须通知宿主并触发重读，不能让
        // 用户按 Ctrl+Z 后静默无反馈。未写入时内容代次保持不变。
        void TestUndoRejectedOnTargetChanged()
        {
            Harness h;
            h.AttachProcess(402, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xC300, {0x00});
            CommitOneEdit(h, 0xC300, 0x00, 0x40);
            WPJ4_CHECK(h.controller.canUndo() == true);

            int finishedCount = 0;
            int failedCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFinished,
                [&](ksword::memwb::CommitReport) { ++finishedCount; });
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFailed,
                [&](ksword::memwb::CommitReport) { ++failedCount; });
            const std::uint64_t contentBefore = h.target.capture().rev.content;

            // 撤销时的写前复核读到的不是 0x40（期望的当前字节），而是别的值，
            // 模拟"目标已被其它程序改动"。
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x99}));
            const std::size_t writeCallsBefore = h.rawPort->writeCalls.size();

            h.controller.undo();
            WPJ4_CHECK_NOTE(
                h.controller.canUndo() == true,
                QStringLiteral("TargetChanged 必须整步拒绝，游标不能动"));
            WPJ4_CHECK_NOTE(
                h.rawPort->writeCalls.size() == writeCallsBefore,
                QStringLiteral("写前复核失败，不应该发生任何写入"));
            WPJ4_CHECK_NOTE(
                finishedCount == 1 && failedCount == 1,
                QStringLiteral("TargetChanged 必须报告失败，不能静默丢弃临时事务的结果"));
            WPJ4_CHECK_NOTE(
                h.target.capture().rev.content == contentBefore,
                QStringLiteral("TargetChanged 不应该推进内容代次"));
        }

        // M-U4：换目标（身份类变更：pid 不同）清空撤销历史，并发一次
        // undoRedoAvailabilityChanged（因为 canUndo() 从 true 变 false）。
        void TestBindTargetClearsHistoryOnIdentityChange()
        {
            Harness h;
            h.AttachProcess(403, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xC400, {0x00});
            CommitOneEdit(h, 0xC400, 0x00, 0x50);
            WPJ4_CHECK(h.controller.canUndo() == true);

            int availabilityChanged = 0;
            QObject::connect(
                &h.controller, &ks::ui::WorkbenchWriteController::undoRedoAvailabilityChanged,
                [&]() { ++availabilityChanged; });

            // 换一个不同的 pid（且 attachGeneration 也不同）：SameTarget 必然为假。
            h.AttachProcess(404, 2);
            WPJ4_CHECK_NOTE(
                h.controller.canUndo() == false,
                QStringLiteral("换目标应当清空撤销历史"));
            WPJ4_CHECK_NOTE(
                availabilityChanged >= 1,
                QStringLiteral("历史被清空且原本非空时应发一次可用性变化信号"));
        }

        // M-U5：同一目标重新附加（pid 相同、attachGeneration 不同但 SameTarget
        // 的七个字段里 attachGeneration 确实会变化——这里改用"仅重读"场景核对
        // 不清空历史）：调用 requestReload（不改变身份）之后撤销历史应保持。
        void TestReloadDoesNotClearHistory()
        {
            Harness h;
            h.AttachProcess(405, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xC500, {0x00});
            CommitOneEdit(h, 0xC500, 0x00, 0x60);
            WPJ4_CHECK(h.controller.canUndo() == true);

            h.target.requestReload(); // 只是重读，不改变身份字段。
            WPJ4_CHECK_NOTE(
                h.controller.canUndo() == true,
                QStringLiteral("仅重读（来源代次 +1，身份不变）不应清空撤销历史"));
        }

        // M-U6（D4 回归，原审核探针 reentrant-undo 场景）：确认框弹出期间
        // （PromptUiConfirm 的回调还没返回），模拟用户按下 Ctrl+Z。修复前：
        // undo() 会新建一个完全独立的临时 scratch 事务，对同一个 confirmation_
        // 再弹一次确认框（模态框叠模态框），且外层正在确认的那次编辑可能从此
        // 既不失败也不提交，静默卡在半途。修复后：undo() 在 commitDepth_ > 0
        // 时直接返回 Busy，不新建临时事务、不弹第二个确认框，外层正在确认的编辑
        // 继续走它自己的流程（本例里因为 undo() 被直接拒绝、不会推进任何代次，
        // 外层编辑不会被判 Stale，能正常提交成功）。
        void TestUndoRejectedWhileOuterCommitIsConfirming()
        {
            Harness h;
            h.AttachProcess(406, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xC600, std::vector<std::uint8_t>(0x200, 0x00));
            // 先正常提交一步，制造一段可撤销历史。
            CommitOneEdit(h, 0xC600, 0x00, 0x70);
            WPJ4_CHECK(h.controller.canUndo() == true);

            // 第二次编辑改成不抑制确认，制造"确认框开着"的窗口。
            h.controller.setUiConfirmSuppressed(false);
            WPJ4_CHECK(h.Stage(0xC700, {0x80}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00})); // 写前复核 @0xC700。
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x80})); // 回读核对 @0xC700。
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));

            int busyCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitRejectedBusy,
                [&](QString) { ++busyCount; });

            ks::ui::CommitEntryStatus nestedUndoStatus = ks::ui::CommitEntryStatus::Started;
            bool nestedUndoCalled = false;
            h.prompter->duringUiConfirm = [&]()
            {
                if (nestedUndoCalled)
                {
                    return;
                }
                nestedUndoCalled = true;
                nestedUndoStatus = h.controller.undo(); // 嵌套：确认框开着时模拟 Ctrl+Z。
            };

            const std::size_t writeCallsBefore = h.rawPort->writeCalls.size();
            h.controller.onEditCompleted();

            WPJ4_CHECK(nestedUndoCalled);
            WPJ4_CHECK_NOTE(
                nestedUndoStatus == ks::ui::CommitEntryStatus::Busy,
                QStringLiteral("D4 修复：确认框开着时 undo() 必须返回 Busy，不能新建第二个确认框"));
            WPJ4_CHECK(busyCount >= 1);
            // 嵌套 undo 被拒绝、完全没有触碰端口：外层这次编辑没有被判 Stale，
            // 正常提交成功——写入次数只增加外层这一次（地址 0xC700）。
            WPJ4_CHECK_NOTE(
                h.rawPort->writeCalls.size() == writeCallsBefore + 1,
                QStringLiteral("嵌套 undo 被拒绝不应该产生任何写入，外层编辑应正常提交"));
            // 历史没有被嵌套 undo 污染：仍然是"两步可撤销"（第一步 0xC600，第二步
            // 刚提交的 0xC700），第一步的撤销历史没有被偷偷消耗掉。
            WPJ4_CHECK(h.controller.canUndo() == true);
        }

        // M-U7（D4 修复，redo() 对称场景）：确认框弹出期间触发 redo()，同样必须
        // 直接返回 Busy，不新建临时事务。
        void TestRedoRejectedWhileOuterCommitIsConfirming()
        {
            Harness h;
            h.AttachProcess(407, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xC800, std::vector<std::uint8_t>(0x200, 0x00));
            CommitOneEdit(h, 0xC800, 0x00, 0x90);
            WPJ4_CHECK(h.controller.canUndo() == true);
            // 先正常撤销一步，制造"可以重做"的游标位置。
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x90}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.undo();
            WPJ4_CHECK(h.controller.canRedo() == true);

            h.controller.setUiConfirmSuppressed(false);
            WPJ4_CHECK(h.Stage(0xC900, {0xA0}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0xA0}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));

            int busyCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitRejectedBusy,
                [&](QString) { ++busyCount; });
            ks::ui::CommitEntryStatus nestedRedoStatus = ks::ui::CommitEntryStatus::Started;
            bool nestedRedoCalled = false;
            h.prompter->duringUiConfirm = [&]()
            {
                if (nestedRedoCalled)
                {
                    return;
                }
                nestedRedoCalled = true;
                nestedRedoStatus = h.controller.redo();
            };

            h.controller.onEditCompleted();

            WPJ4_CHECK(nestedRedoCalled);
            WPJ4_CHECK_NOTE(
                nestedRedoStatus == ks::ui::CommitEntryStatus::Busy,
                QStringLiteral("D4 修复：确认框开着时 redo() 必须返回 Busy"));
            WPJ4_CHECK(busyCount >= 1);
            // 注意：这里不断言 canRedo()——外层那次新编辑（0xC900）本身提交成功
            // 之后，正常的 journal 语义就会切断重做历史（“新记录会切断重做步数”，
            // 与是否发生过嵌套 redo() 无关，任何编辑器的撤销/重做都是这个规则）。
            // 本测试要验证的是"嵌套 redo() 被拒绝、没有另外新建一次提交"，不是
            // "重做历史不受外层编辑影响"。
            WPJ4_CHECK(h.controller.canUndo() == true);
        }

        // M-U8（review-wpJ4.md R8 测试缺口补测）：redo() 成功后必须推进 target_
        // 真实的内容代次（不仅仅是本类自己的镶像 localRevisions_），否则其它
        // 消费 target_ 真实代次的组件（例如将来的 PageProvider）感知不到重做
        // 发生。
        void TestRedoAdvancesRealTargetContentRevision()
        {
            Harness h;
            h.AttachProcess(409, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xCB00, {0x00});
            CommitOneEdit(h, 0xCB00, 0x00, 0xB0);
            WPJ4_CHECK(h.controller.canUndo() == true);

            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0xB0}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.undo();
            WPJ4_CHECK(h.controller.canRedo() == true);

            const std::uint64_t contentBeforeRedo = h.target.capture().rev.content;
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0xB0}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.redo();
            const std::uint64_t contentAfterRedo = h.target.capture().rev.content;
            WPJ4_CHECK_NOTE(
                contentAfterRedo != contentBeforeRedo,
                QStringLiteral("redo() 成功后必须推进 target_ 真实的内容代次"));
        }

        // M-U9（review-wpJ4.md R13 测试缺口补测）：record() 必须真的用
        // tickProvider_ 取得的时间戳，不能恒为 0——否则两次时间间隔很大的编辑
        // 会被 journal 误判成"同一插入点连续键入"合并成一步。
        // 判据设计：两种情形（合并成一步 / 两条独立历史）在"撤销一次"之后的
        // expectedCurrent（写前复核期望值）都是 0x22（两种情形里最后一次 after
        // 都是 0x22，这一步分不出差异）；但 restore（真正要写回的值）不同——
        // 合并成一步时 restore 是"最早一次 before"=0x00，两条独立历史时第二步
        // 自己的 restore 是 0x11。下面的回读核对脚本只给 0x00（故意只匹配"被
        // 错误合并"那一种情形）：
        //   - 正确实现（tick 间隔 5000 超出合并窗口，两条独立历史）：这次撤销
        //     的真实 restore 应该是 0x11，与脚本给的 0x00 不符，Commit 的 (e)
        //     步判 VerifyMismatch，整步拒绝、游标不动，canUndo() 仍为 true。
        //   - 变异（tick 恒为 0，被错误合并成一步）：这次撤销的真实 restore
        //     恰好是 0x00，与脚本吻合，提交成功，这唯一一步被消耗掉，
        //     canUndo() 变为 false。
        // 两者在 canUndo() 的最终取值上必然不同，用它做判据。
        void TestRecordUsesTickProviderForMergeWindow()
        {
            Harness h;
            h.AttachProcess(410, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xCA00, {0x00});

            h.tickValue = 0;
            CommitOneEdit(h, 0xCA00, 0x00, 0x11);
            h.tickValue = 5000; // 与上一步间隔 5000，超出 1500 的合并窗口，理应另起一步。
            CommitOneEdit(h, 0xCA00, 0x11, 0x22);
            WPJ4_CHECK(h.controller.canUndo() == true);

            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x22})); // 写前复核：两种情形都是 0x22。
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00})); // 回读核对：故意只匹配"被合并"那一种。
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.undo();
            WPJ4_CHECK_NOTE(
                h.controller.canUndo() == true,
                QStringLiteral(
                    "tick 间隔超出合并窗口时两次编辑必须是两条独立历史：这次撤销的真实"
                    "restore 应该是第二步自己的 0x11（与脚本给的 0x00 不符，整步拒绝、"
                    "游标不动），而不是被错误合并后的最早 before 0x00"));
        }

        // M-U11（review2-wpJ4.md C3 测试缺口补测，比 C2 更严重——此前 redo()
        // 遇到 TargetChanged 完全没有任何测试覆盖，不只是"条件放宽测不出"）：
        // 目标已变时 redo() 必须整步拒绝，游标不动，并向宿主报告失败与重读需求。
        void TestRedoRejectedOnTargetChanged()
        {
            Harness h;
            h.AttachProcess(412, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xCD00, {0x00});
            CommitOneEdit(h, 0xCD00, 0x00, 0xD0);
            WPJ4_CHECK(h.controller.canUndo() == true);

            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0xD0}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.undo();
            WPJ4_CHECK(h.controller.canRedo() == true);

            int finishedCount = 0;
            int failedCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFinished,
                [&](ksword::memwb::CommitReport) { ++finishedCount; });
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFailed,
                [&](ksword::memwb::CommitReport) { ++failedCount; });
            const std::uint64_t contentBefore = h.target.capture().rev.content;

            // 重做时的写前复核读到的不是 0x00（期望的当前字节），模拟"目标已被
            // 其它程序改动"。
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x77}));
            const std::size_t writeCallsBefore = h.rawPort->writeCalls.size();

            h.controller.redo();
            WPJ4_CHECK_NOTE(
                h.controller.canRedo() == true,
                QStringLiteral("TargetChanged 必须整步拒绝，游标不能动"));
            WPJ4_CHECK_NOTE(
                h.rawPort->writeCalls.size() == writeCallsBefore,
                QStringLiteral("写前复核失败，不应该发生任何写入"));
            WPJ4_CHECK_NOTE(
                finishedCount == 1 && failedCount == 1,
                QStringLiteral("TargetChanged 必须报告失败，不能静默丢弃临时事务的结果"));
            WPJ4_CHECK_NOTE(
                h.target.capture().rev.content == contentBefore,
                QStringLiteral("TargetChanged 不应该推进内容代次"));
        }

        // M-U12（review2-wpJ4.md C8 测试缺口补测）：一次真正成功执行、没有被
        // D4 的 Busy 守卫拦下的撤销，在不抑制确认时必须真的弹一次确认框——
        // replayOnce 构造的临时事务要继承 suppressedProvider_ 取到的当前值；
        // M-U6/M-U7 的两例撤销/重做都被 Busy 挡住，从未真正走到
        // scratchTransaction.Commit()，这条路径此前完全没有被验证过。
        void TestUndoPromptsConfirmationWhenNotSuppressed()
        {
            Harness h;
            h.AttachProcess(413, 1);
            h.controller.setUiConfirmSuppressed(true); // 先用抑制确认完成一次正常编辑。
            h.EnsureWired();
            h.LoadBaseline(0xCE00, {0x00});
            CommitOneEdit(h, 0xCE00, 0x00, 0xE0);
            WPJ4_CHECK(h.controller.canUndo() == true);

            // 切到不抑制确认：接下来这次撤销必须真的弹一次框。
            h.controller.setUiConfirmSuppressed(false);
            WPJ4_CHECK(h.prompter->uiConfirmCallCount == 0);

            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0xE0}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.undo();
            WPJ4_CHECK_NOTE(
                h.prompter->uiConfirmCallCount == 1,
                QStringLiteral("撤销的临时事务必须继承当前 uiConfirmSuppressed=false，真的弹一次确认框"));
            WPJ4_CHECK(h.controller.canUndo() == false); // 撤销成功，游标移动。
        }

        // M-U10（COMMON 裁决第 3 项测试）：撤销/重做的临时事务必须继承控制器
        // 当前的 uiConfirmSuppressed——抑制确认时，撤销也不应该额外弹一次
        // 确认框（ux 表："撤销是同一条确认/审计/回读链"）。
        void TestUndoInheritsSuppressedConfirmState()
        {
            Harness h;
            h.AttachProcess(411, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xCC00, {0x00});
            CommitOneEdit(h, 0xCC00, 0x00, 0xC0);
            WPJ4_CHECK(h.controller.canUndo() == true);
            WPJ4_CHECK(h.prompter->uiConfirmCallCount == 0);

            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0xC0}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.undo();
            WPJ4_CHECK_NOTE(
                h.prompter->uiConfirmCallCount == 0,
                QStringLiteral("撤销抑制确认时的临时事务不应该弹出确认框"));
        }

        // ---------------- S-wpJ4 补测（审核者追加，不在仓库夹具里） ----------------

        // S1：成功的 undo()/redo() 必须做完整 W4 收尾（commitFinished 一次、局部重读
        // 一次且范围等于被回放的那个块）。杀 r5-W4-undo / x26-W4-redo。
        void TestSuppUndoRedoRunW4Wrapup()
        {
            Harness h;
            h.AttachProcess(620, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xE000, {0x00});
            CommitOneEdit(h, 0xE000, 0x00, 0x11);
            h.rereadCalls.clear();

            int finishedCount = 0;
            ksword::memwb::CommitReport lastReport;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFinished,
                [&](ksword::memwb::CommitReport report)
                {
                    ++finishedCount;
                    lastReport = report;
                });

            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x11}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.undo();
            WPJ4_CHECK_NOTE(
                finishedCount == 1 && lastReport.outcome == ksword::memwb::CommitOutcome::Committed,
                QStringLiteral("成功的 undo() 必须发一次 commitFinished(Committed)（W4 收尾）"));
            WPJ4_CHECK_NOTE(
                h.rereadCalls.size() == 1,
                QStringLiteral("成功的 undo() 必须对回放的块做一次局部重读"));
            if (h.rereadCalls.size() == 1)
            {
                WPJ4_CHECK(h.rereadCalls[0].first == 0xE000);
                WPJ4_CHECK(h.rereadCalls[0].second == 1);
            }

            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x11}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.redo();
            WPJ4_CHECK_NOTE(
                finishedCount == 2 && lastReport.outcome == ksword::memwb::CommitOutcome::Committed,
                QStringLiteral("成功的 redo() 必须发一次 commitFinished(Committed)（W4 收尾）"));
            WPJ4_CHECK_NOTE(
                h.rereadCalls.size() == 2,
                QStringLiteral("成功的 redo() 必须对回放的块做一次局部重读"));
            if (h.rereadCalls.size() == 2)
            {
                WPJ4_CHECK(h.rereadCalls[1].first == 0xE000);
                WPJ4_CHECK(h.rereadCalls[1].second == 1);
            }
        }

        // 回放失败仍须报告真实落地字节、暂存区告警及重读需求，但不移动日志游标。
        // 两种方向各覆盖部分写入和回读不符，随后恢复目标再重试，确认未污染历史。
        void TestFailedUndoRedoRunW4Wrapup()
        {
            for (const bool isRedo : {false, true})
            {
                for (const bool partialWrite : {false, true})
                {
                    Harness h;
                    h.AttachProcess(646, 1);
                    h.controller.setUiConfirmSuppressed(true);
                    h.EnsureWired();
                    h.LoadBaseline(0xEA00, {0x00, 0x00});
                    WPJ4_CHECK(h.Stage(0xEA00, {0x51, 0x52}) == ksword::memwb::StageStatus::Ok);
                    h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00, 0x00}));
                    h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x51, 0x52}));
                    h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(2));
                    h.controller.onEditCompleted();

                    // 重做用例先成功撤销，建立唯一一条可重做历史。
                    if (isRedo)
                    {
                        h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x51, 0x52}));
                        h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00, 0x00}));
                        h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(2));
                        h.controller.undo();
                    }
                    const std::vector<std::uint8_t> expected = isRedo
                        ? std::vector<std::uint8_t>{0x00, 0x00}
                        : std::vector<std::uint8_t>{0x51, 0x52};
                    const std::vector<std::uint8_t> restored = isRedo
                        ? std::vector<std::uint8_t>{0x51, 0x52}
                        : std::vector<std::uint8_t>{0x00, 0x00};
                    h.rawPort->script.push_back(MemwbIoTests::MakeOk(expected));
                    ksword::memwb::IoWriteResult write = MemwbIoTests::MakeWriteOk(2);
                    if (partialWrite)
                    {
                        write.ok = false;
                        write.partial = true;
                        write.bytesDone = 1;
                        write.scratchAreaDirty = true;
                        write.failure = "scripted partial replay write";
                    }
                    else
                    {
                        // 全段已写入，但回读与期望不符；脏标记来自这次真实回读。
                        auto verify = MemwbIoTests::MakeOk({0xFF, 0xFF});
                        verify.scratchAreaDirty = true;
                        h.rawPort->script.push_back(verify);
                    }
                    h.rawPort->writeScript.push_back(write);
                    int finished = 0;
                    int failed = 0;
                    int dirty = 0;
                    ksword::memwb::CommitReport lastReport;
                    QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFinished,
                        [&](ksword::memwb::CommitReport report) { ++finished; lastReport = report; });
                    QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFailed,
                        [&](ksword::memwb::CommitReport) { ++failed; });
                    QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::scratchAreaDirtyReported,
                        [&]() { ++dirty; });
                    const auto before = h.target.capture().rev;
                    isRedo ? h.controller.redo() : h.controller.undo();
                    WPJ4_CHECK(finished == 1 && failed == 1 && dirty == 1);
                    WPJ4_CHECK(lastReport.outcome == (partialWrite
                        ? ksword::memwb::CommitOutcome::WriteFailed
                        : ksword::memwb::CommitOutcome::VerifyMismatch));
                    WPJ4_CHECK(lastReport.bytesWritten == (partialWrite ? 1U : 2U));
                    WPJ4_CHECK(lastReport.blocksWritten == 0);
                    WPJ4_CHECK(lastReport.needsReread && lastReport.scratchAreaDirty);
                    WPJ4_CHECK(h.target.capture().rev.source != before.source);
                    WPJ4_CHECK(h.target.capture().rev.content == before.content + 1);
                    WPJ4_CHECK(h.controller.canUndo() == !isRedo);
                    WPJ4_CHECK(h.controller.canRedo() == isRedo);

                    // 假设目标已恢复到回放前的值，再次成功回放；只能消耗原有一步。
                    h.rawPort->script.push_back(MemwbIoTests::MakeOk(expected));
                    h.rawPort->script.push_back(MemwbIoTests::MakeOk(restored));
                    h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(2));
                    isRedo ? h.controller.redo() : h.controller.undo();
                    WPJ4_CHECK(h.controller.canUndo() == isRedo);
                    WPJ4_CHECK(h.controller.canRedo() == !isRedo);
                }
            }
        }

        // S2：一次提交落地多个块时，每个落地块各记一步（可逐步撤销到底）。杀 x15。
        void TestSuppMultiBlockCommitRecordsEveryLandedBlock()
        {
            Harness h;
            h.AttachProcess(642, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xE600, std::vector<std::uint8_t>(0x110, 0x00));
            WPJ4_CHECK(h.Stage(0xE600, {0x21}) == ksword::memwb::StageStatus::Ok);
            WPJ4_CHECK(h.Stage(0xE700, {0x22}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {
                MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x00}),
                MemwbIoTests::MakeOk({0x21}), MemwbIoTests::MakeOk({0x22})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1), MemwbIoTests::MakeWriteOk(1)};
            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::Committed);
            WPJ4_CHECK(attempt.report.blocksWritten == 2);

            // 最新一步（0xE700）先撤销。
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x22}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.undo();
            WPJ4_CHECK(h.controller.canUndo() == true);
            // 再撤销 0xE600。
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x21}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.undo();
            WPJ4_CHECK_NOTE(
                h.controller.canUndo() == false,
                QStringLiteral("两个落地块应各记一步：两次撤销之后历史应当耗尽"));
        }

        // S4：撤销/重做的临时提交期间 isCommitting() 必须为真（transaction_ 自己是
        // 空闲的，只能靠 commitDepth_）。杀 x20。
        void TestSuppIsCommittingTrueDuringUndoReplay()
        {
            Harness h;
            h.AttachProcess(643, 1);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xE800, {0x00});
            CommitOneEdit(h, 0xE800, 0x00, 0x31);
            h.controller.setUiConfirmSuppressed(false);

            bool probed = false;
            bool sawCommitting = false;
            h.prompter->duringUiConfirm = [&]()
            {
                probed = true;
                sawCommitting = h.controller.isCommitting();
            };
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x31}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.undo();
            WPJ4_CHECK(probed);
            WPJ4_CHECK_NOTE(
                sawCommitting,
                QStringLiteral("撤销的临时事务确认框期间 isCommitting() 必须为真"));
            WPJ4_CHECK(h.controller.isCommitting() == false);
        }

        // S7：undo()/redo() 在回放前都必须刷新一次会话（DDMA 代次）。杀 x24/x25。
        void TestSuppUndoRedoRefreshDdmaGeneration()
        {
            Harness h;
            h.AttachProcess(645, 1);
            WPJ4_CHECK(h.target.requestChannel(ksword::memwb::Channel::Ddma));
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xE900, {0x00});
            CommitOneEdit(h, 0xE900, 0x00, 0x41);
            WPJ4_CHECK(h.controller.canUndo() == true);

            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x41}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            const int beforeUndo = h.servicesRaw->DdmaGenerationCallCount();
            h.controller.undo();
            WPJ4_CHECK_NOTE(
                h.servicesRaw->DdmaGenerationCallCount() > beforeUndo,
                QStringLiteral("undo() 必须在回放前刷新一次 DDMA 代次"));

            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x41}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            const int beforeRedo = h.servicesRaw->DdmaGenerationCallCount();
            h.controller.redo();
            WPJ4_CHECK_NOTE(
                h.servicesRaw->DdmaGenerationCallCount() > beforeRedo,
                QStringLiteral("redo() 必须在回放前刷新一次 DDMA 代次"));
        }
    }

    void RunUndoTests()
    {
        TestSuppUndoRedoRunW4Wrapup();
        TestFailedUndoRedoRunW4Wrapup();
        TestSuppMultiBlockCommitRecordsEveryLandedBlock();
        TestSuppIsCommittingTrueDuringUndoReplay();
        TestSuppUndoRedoRefreshDdmaGeneration();
        TestUndoReplaysPreviousValue();
        TestUndoRedoDoNotGrowHistory();
        TestUndoRejectedOnTargetChanged();
        TestBindTargetClearsHistoryOnIdentityChange();
        TestReloadDoesNotClearHistory();
        TestUndoRejectedWhileOuterCommitIsConfirming();
        TestRedoRejectedWhileOuterCommitIsConfirming();
        TestRedoAdvancesRealTargetContentRevision();
        TestRecordUsesTickProviderForMergeWindow();
        TestRedoRejectedOnTargetChanged();
        TestUndoPromptsConfirmationWhenNotSuppressed();
        TestUndoInheritsSuppressedConfirmState();
    }
}
