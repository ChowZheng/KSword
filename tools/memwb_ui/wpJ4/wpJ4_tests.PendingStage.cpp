// wpJ4_tests.PendingStage.cpp
// 作用：地址簿"值"列编辑落在基线窗口外时的异步暂存票据——
//   - 非法输入（空字节、地址溢出）返回 0，不创建票据。
//   - 地址已经在窗口内时立即解决（成功）。
//   - OutOfWindow 时票据保持挂起，notifyWindowMayCover 在窗口移动后重试成功。
//   - 2 秒超时后发 pendingStageResolved(false, "timeout")，不再重试。
//   - cancelPendingStage 对存在的票据发一次失败结果，对不存在的票据是空操作。

#include "wpJ4_common.h"

namespace wpj4_test
{
    namespace
    {
        struct ResolvedRecord
        {
            ks::ui::WorkbenchWriteController::PendingStageTicket ticket = 0;
            bool ok = false;
            QString reason;
        };

        // M-P1：空字节/地址溢出都返回 0，且不应该触发任何 pendingStageResolved。
        void TestInvalidInputsReturnZero()
        {
            Harness h;
            h.AttachProcess(300);
            int resolvedCount = 0;
            QObject::connect(
                &h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                [&](ks::ui::WorkbenchWriteController::PendingStageTicket, bool, QString) { ++resolvedCount; });

            WPJ4_CHECK(h.controller.beginPendingStage(0x8000, QByteArray()) == 0);
            WPJ4_CHECK(
                h.controller.beginPendingStage(0xFFFFFFFFFFFFFFFFULL, QByteArray(2, '\x01')) == 0);
            WPJ4_CHECK(resolvedCount == 0);
        }

        // M-P2：地址已经在当前窗口内——beginPendingStage 应该在返回之前（同一次
        // 调用栈内）就已经解决，立即模式下顺带走一次 Commit。
        void TestImmediatelyWithinWindowResolves()
        {
            Harness h;
            h.AttachProcess(301);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0x8100, {0x00, 0x00});
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00, 0x00}), MemwbIoTests::MakeOk({0x07, 0x08})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(2)};

            std::vector<ResolvedRecord> resolved;
            QObject::connect(
                &h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                [&](ks::ui::WorkbenchWriteController::PendingStageTicket ticket, bool ok, QString reason)
                {
                    resolved.push_back({ticket, ok, reason});
                });

            const auto ticket = h.controller.beginPendingStage(0x8100, QByteArray("\x07\x08", 2));
            WPJ4_CHECK(ticket != 0);
            WPJ4_CHECK(resolved.size() == 1);
            if (!resolved.empty())
            {
                WPJ4_CHECK(resolved[0].ticket == ticket);
                WPJ4_CHECK(resolved[0].ok == true);
            }
            WPJ4_CHECK(h.rawPort->writeCalls.size() == 1); // 立即模式顺带走了一次 Commit。
        }

        // M-P3：地址在窗口外——票据保持挂起（不解决），直到 LoadBaseline 把窗口
        // 移到覆盖该地址之后调用 notifyWindowMayCover 才成功。
        void TestOutOfWindowThenNotifyResolves()
        {
            Harness h;
            h.AttachProcess(302);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            // 当前窗口不覆盖 0x9000。
            h.LoadBaseline(0x8200, {0x00});

            std::vector<ResolvedRecord> resolved;
            QObject::connect(
                &h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                [&](ks::ui::WorkbenchWriteController::PendingStageTicket ticket, bool ok, QString reason)
                {
                    resolved.push_back({ticket, ok, reason});
                });

            const auto ticket = h.controller.beginPendingStage(0x9000, QByteArray(1, '\x05'));
            WPJ4_CHECK(ticket != 0);
            WPJ4_CHECK_NOTE(resolved.empty(), QStringLiteral("OutOfWindow 应保持挂起，不立即解决"));

            // 装配层的职责：把窗口移动到覆盖该地址（这里直接模拟 LoadBaseline）。
            h.LoadBaseline(0x9000, {0x00});
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x05})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};
            h.controller.notifyWindowMayCover();

            WPJ4_CHECK(resolved.size() == 1);
            if (!resolved.empty())
            {
                WPJ4_CHECK(resolved[0].ticket == ticket);
                WPJ4_CHECK(resolved[0].ok == true);
            }
            // 再调用一次 notifyWindowMayCover 不应该对已解决的票据重复发信号。
            h.controller.notifyWindowMayCover();
            WPJ4_CHECK(resolved.size() == 1);
        }

        // M-P4：2 秒超时后发 (false, "timeout")，不再重试（超时之后即使窗口移动
        // 覆盖了地址，也不会再解决一次）。
        void TestTimeoutResolvesAsFailure()
        {
            Harness h;
            h.AttachProcess(303);
            h.LoadBaseline(0x8300, {0x00}); // 窗口不覆盖下面的地址，保持挂起。

            std::vector<ResolvedRecord> resolved;
            QObject::connect(
                &h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                [&](ks::ui::WorkbenchWriteController::PendingStageTicket ticket, bool ok, QString reason)
                {
                    resolved.push_back({ticket, ok, reason});
                });

            const auto ticket = h.controller.beginPendingStage(0xA000, QByteArray(1, '\x01'));
            WPJ4_CHECK(ticket != 0);

            const bool fired = PumpUntil([&]() { return !resolved.empty(); }, 4000);
            WPJ4_CHECK_NOTE(fired, QStringLiteral("2 秒超时计时器应该在 4 秒内触发一次解决"));
            WPJ4_CHECK(resolved.size() == 1);
            if (!resolved.empty())
            {
                WPJ4_CHECK(resolved[0].ticket == ticket);
                WPJ4_CHECK(resolved[0].ok == false);
                WPJ4_CHECK(resolved[0].reason == QStringLiteral("timeout"));
            }

            // 超时之后窗口才覆盖到该地址：不应该再被重试/再次解决。
            h.LoadBaseline(0xA000, {0x00});
            h.controller.notifyWindowMayCover();
            WPJ4_CHECK(resolved.size() == 1);
        }

        // M-P5：cancelPendingStage 对存在的票据发一次 (false, "cancelled")；对
        // 不存在/已解决的票据是空操作（不重复发信号）。
        void TestCancelPendingStage()
        {
            Harness h;
            h.AttachProcess(304);
            h.LoadBaseline(0x8400, {0x00});

            std::vector<ResolvedRecord> resolved;
            QObject::connect(
                &h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                [&](ks::ui::WorkbenchWriteController::PendingStageTicket ticket, bool ok, QString reason)
                {
                    resolved.push_back({ticket, ok, reason});
                });

            const auto ticket = h.controller.beginPendingStage(0xB000, QByteArray(1, '\x02'));
            WPJ4_CHECK(ticket != 0);
            WPJ4_CHECK(resolved.empty());

            h.controller.cancelPendingStage(ticket);
            WPJ4_CHECK(resolved.size() == 1);
            if (!resolved.empty())
            {
                WPJ4_CHECK(resolved[0].ok == false);
                WPJ4_CHECK(resolved[0].reason == QStringLiteral("cancelled"));
            }

            // 再取消同一张票据：空操作，不重复发信号。
            h.controller.cancelPendingStage(ticket);
            WPJ4_CHECK(resolved.size() == 1);

            // 取消一个从未存在的票据号：空操作。
            h.controller.cancelPendingStage(999999);
            WPJ4_CHECK(resolved.size() == 1);
        }

        // M-P6：FinalizePendingStage 对不存在的票据必须是空操作（不发信号、不
        // 崩溃）——三个公开入口（beginPendingStage/cancelPendingStage/
        // notifyWindowMayCover）在调用它之前都已经各自确认过票据存在，这条白盒
        // 测试经由测试专用的 FinalizePendingStageForTest 包装（头文件解冻后
        // FinalizePendingStage 本身已经改为私有成员方法，不能再从外部直接调用）
        // 绕过它们、直接核对这层防御本身没有失效（见变异 M13：删掉这层防御后，
        // 当前三个公开入口确实都测不出来，这条测试是补上的那个缺口）。
        void TestFinalizePendingStageIgnoresUnknownTicket()
        {
            Harness h;
            h.AttachProcess(305);
            int resolvedCount = 0;
            QObject::connect(
                &h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                [&](ks::ui::WorkbenchWriteController::PendingStageTicket, bool, QString) { ++resolvedCount; });

            h.controller.FinalizePendingStageForTest(424242, false, QStringLiteral("bogus"));
            WPJ4_CHECK_NOTE(
                resolvedCount == 0,
                QStringLiteral("对不存在的票据调用 FinalizePendingStage 不应该发出任何信号"));
        }

        // M-P7（review-wpJ4.md R4 测试缺口补测）：UnreadBytes 必须保持挂起而不是
        // 立即判失败——窗口已经覆盖地址、但其中某个字节当时没有真实读到
        // （validMask 该位为 0）属于"窗口内但数据还没来"，和 OutOfWindow 一样
        // 应该允许重试，而不是和 Empty/AddressOverflow/TooLarge 一样立即拒绝。
        void TestUnreadBytesStaysPendingNotImmediateFail()
        {
            Harness h;
            h.AttachProcess(307);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            // 手动载入一段基线，validMask 第二个字节标 0（没有真实读到）。
            h.overlay.LoadBaseline(
                ksword::memwb::IdentityKey(h.target.session(), 0x8800, 2),
                0x8800, {0x00, 0x00}, {1, 0});

            std::vector<ResolvedRecord> resolved;
            QObject::connect(
                &h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                [&](ks::ui::WorkbenchWriteController::PendingStageTicket ticket, bool ok, QString reason)
                {
                    resolved.push_back({ticket, ok, reason});
                });

            const auto ticket = h.controller.beginPendingStage(0x8800, QByteArray(2, '\x05'));
            WPJ4_CHECK(ticket != 0);
            WPJ4_CHECK_NOTE(resolved.empty(), QStringLiteral("UnreadBytes 应保持挂起，不应立即判失败"));

            // 补齐真实读到的基线之后，notifyWindowMayCover 应该能让它成功。
            h.overlay.LoadBaseline(
                ksword::memwb::IdentityKey(h.target.session(), 0x8800, 2),
                0x8800, {0x00, 0x00}, {1, 1});
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00, 0x00}), MemwbIoTests::MakeOk({0x05, 0x05})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(2)};
            h.controller.notifyWindowMayCover();
            WPJ4_CHECK(resolved.size() == 1);
            if (!resolved.empty())
            {
                WPJ4_CHECK(resolved[0].ok == true);
            }
        }

        // M-P8（review-wpJ4.md R9 测试缺口补测）：一次 notifyWindowMayCover() 调用
        // 应该能同时解决两个以上待决票据，不应该因为"遍历时 erase"导致迭代器
        // 失效漏掉其中一张（生产代码里的快照拷贝现在是对的，但之前没有任何
        // 测试在一次调用里放两张以上的票据，这层防御本身测不出来）。
        void TestNotifyWindowMayCoverHandlesMultipleTickets()
        {
            Harness h;
            h.AttachProcess(308);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0x8500, {0x00}); // 当前窗口不覆盖下面两个地址。

            std::vector<ResolvedRecord> resolved;
            QObject::connect(
                &h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                [&](ks::ui::WorkbenchWriteController::PendingStageTicket ticket, bool ok, QString reason)
                {
                    resolved.push_back({ticket, ok, reason});
                });

            // 放 6 张票据（而不是 2 张）以增大"遍历时 erase 导致迭代器失效"这类
            // 未定义行为实际表现出来的概率——地址彼此不相邻，确保各自独立成块。
            std::vector<ks::ui::WorkbenchWriteController::PendingStageTicket> tickets;
            for (int i = 0; i < 6; ++i)
            {
                const auto ticket = h.controller.beginPendingStage(
                    0x8600 + static_cast<std::uint64_t>(i) * 0x100, QByteArray(1, static_cast<char>(0x10 + i)));
                WPJ4_CHECK(ticket != 0);
                tickets.push_back(ticket);
            }
            WPJ4_CHECK(resolved.empty());

            // 一次窗口移动同时覆盖全部 6 个地址。
            h.LoadBaseline(0x8600, std::vector<std::uint8_t>(0x700, 0x00));
            for (int i = 0; i < 6; ++i)
            {
                h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
                h.rawPort->script.push_back(MemwbIoTests::MakeOk({static_cast<std::uint8_t>(0x10 + i)}));
                h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            }

            h.controller.notifyWindowMayCover();
            WPJ4_CHECK_NOTE(
                resolved.size() == 6, QStringLiteral("6 张票据都应该在同一次调用里被解决，一张都不能漏"));
        }

        // M-P9（review2-wpJ4.md C6 测试设计缺陷补测）：M-P6 只传一个不存在的
        // 票据号，"把 FinalizePendingStageForTest 悄悄改成彻底空操作"与"正确
        // 转发到 FinalizePendingStage"在那条断言下完全不可区分——两者都不发
        // 信号。本测试改用一个真实存在、仍挂起的票据，断言它确实被正确转发并
        // 解决，这样空操作版本才会被抓到。
        void TestFinalizePendingStageForTestForwardsExistingTicket()
        {
            Harness h;
            h.AttachProcess(306);
            h.LoadBaseline(0x8700, {0x00}); // 当前窗口不覆盖下面的地址，票据保持挂起。

            std::vector<ResolvedRecord> resolved;
            QObject::connect(
                &h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                [&](ks::ui::WorkbenchWriteController::PendingStageTicket ticket, bool ok, QString reason)
                {
                    resolved.push_back({ticket, ok, reason});
                });

            const auto ticket = h.controller.beginPendingStage(0x8900, QByteArray(1, '\x06'));
            WPJ4_CHECK(ticket != 0);
            WPJ4_CHECK(resolved.empty());

            h.controller.FinalizePendingStageForTest(ticket, true, QStringLiteral("manual-test"));
            WPJ4_CHECK_NOTE(
                resolved.size() == 1,
                QStringLiteral("对已存在的票据调用 FinalizePendingStageForTest 必须真的转发、发出信号"));
            if (!resolved.empty())
            {
                WPJ4_CHECK(resolved[0].ticket == ticket);
                WPJ4_CHECK(resolved[0].ok == true);
                WPJ4_CHECK(resolved[0].reason == QStringLiteral("manual-test"));
            }
            // 票据已被解决、已从表里摘除：再取消同一张票据应为空操作。
            h.controller.cancelPendingStage(ticket);
            WPJ4_CHECK(resolved.size() == 1);
        }

        // M-P10（review2-wpJ4.md C11 可疑点锁定测试）：FinalizePendingStage 必须
        // 先 erase 再 emit——槛函数体内重新操作同一票据不应该再找到它。用一个
        // 一次性标记防止"若顺序被换掉"时的嵌套 emit 无限递归，只需要看第二层
        // 是否发生即可判断顺序。
        void TestFinalizePendingStageErasesBeforeEmitting()
        {
            Harness h;
            h.AttachProcess(309);
            h.LoadBaseline(0x8A00, {0x00}); // 窗口不覆盖，票据保持挂起。

            int resolvedCount = 0;
            bool alreadyRetried = false;
            ks::ui::WorkbenchWriteController::PendingStageTicket ticket = 0;
            QObject::connect(
                &h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                [&](ks::ui::WorkbenchWriteController::PendingStageTicket resolvedTicket, bool, QString)
                {
                    ++resolvedCount;
                    if (!alreadyRetried)
                    {
                        alreadyRetried = true;
                        // 若 erase 已经先发生（当前/正确顺序），票据已经不在表
                        // 里，这里是空操作；若顺序被换成"先 emit 再 erase"，
                        // 票据此刻还"存在"，会再触发一次 pendingStageResolved。
                        h.controller.cancelPendingStage(resolvedTicket);
                    }
                });

            ticket = h.controller.beginPendingStage(0x8B00, QByteArray(1, '\x07'));
            WPJ4_CHECK(ticket != 0);
            h.controller.cancelPendingStage(ticket);
            WPJ4_CHECK_NOTE(
                resolvedCount == 1,
                QStringLiteral(
                    "FinalizePendingStage 必须先 erase 再 emit：槛函数体内重新取消同一票据不应该触发第二次信号"));
        }
    }

    // 发布前审阅回归：异步地址簿编辑不得跨PID、通道、范围、重读或DDMA代次落地。
    void TestPendingStageRejectsSourceChanges()
    {
        for (int scenario = 0; scenario < 5; ++scenario)
        {
            Harness h;
            h.AttachProcess(311);
            if (scenario == 4)
            {
                h.servicesRaw->SetDdmaGeneration(1);
                WPJ4_CHECK(h.target.requestChannel(ksword::memwb::Channel::Ddma));
            }
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0x8000, {0x00});

            std::vector<ResolvedRecord> resolved;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                [&](auto ticket, bool ok, QString reason) { resolved.push_back({ticket, ok, reason}); });
            const auto ticket = h.controller.beginPendingStage(0x9000, QByteArray(1, '\x73'));
            WPJ4_CHECK(ticket != 0);
            WPJ4_CHECK(resolved.empty());
            const auto revision = h.target.revisions().Source();
            if (scenario == 0)
            {
                h.AttachProcess(312, 2);
            }
            else if (scenario == 1)
            {
                WPJ4_CHECK(h.target.requestChannel(ksword::memwb::Channel::Hvm));
            }
            else if (scenario == 2)
            {
                WPJ4_CHECK(h.target.requestScope(ksword::memwb::Scope::KernelVirtual));
            }
            else if (scenario == 3)
            {
                h.target.requestReload();
            }
            else
            {
                h.servicesRaw->SetDdmaGeneration(2);
                h.target.capture();
            }
            WPJ4_CHECK(h.target.revisions().Source() != revision);
            h.LoadBaseline(0x9000, {0x00});
            h.controller.notifyWindowMayCover();
            WPJ4_CHECK(resolved.size() == 1);
            if (!resolved.empty())
            {
                WPJ4_CHECK(resolved.front().ticket == ticket);
                WPJ4_CHECK(!resolved.front().ok);
            }
            WPJ4_CHECK_NOTE(!h.overlay.HasPendingPatches(),
                QStringLiteral("陈旧编辑不得进入新目标的暂存层"));
            WPJ4_CHECK_NOTE(h.rawPort->writeCalls.empty(),
                QStringLiteral("陈旧编辑不得对新目标调用端口写入"));
        }
    }

    // 同步取消其它票据后，整表快照不能把已经取消的编辑重新暂存。
    void TestPendingStageSnapshotSkipsCancelledTickets()
    {
        Harness h;
        h.AttachProcess(313);
        WPJ4_CHECK(h.controller.requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply) ==
            ksword::memwb::ModeSwitchStatus::Switched);
        h.LoadBaseline(0x8000, {0x00});
        const auto first = h.controller.beginPendingStage(0x9200, QByteArray(1, '\x11'));
        const auto second = h.controller.beginPendingStage(0x9201, QByteArray(1, '\x22'));
        WPJ4_CHECK(first != 0 && second != 0);
        int successCount = 0;
        int cancelledCount = 0;
        QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
            [&](quint64 ticket, bool ok, QString)
            {
                if (ticket == first && ok)
                {
                    ++successCount;
                    h.controller.cancelPendingStage(second);
                }
                if (ticket == second && !ok)
                {
                    ++cancelledCount;
                }
            });
        h.LoadBaseline(0x9200, {0x00, 0x00});
        h.controller.notifyWindowMayCover();
        const auto blocks = h.overlay.DiffBlocks();
        WPJ4_CHECK(successCount == 1 && cancelledCount == 1);
        WPJ4_CHECK(blocks.size() == 1);
        if (blocks.size() == 1)
        {
            WPJ4_CHECK(blocks[0].address == 0x9200 && blocks[0].after == std::vector<std::uint8_t>{0x11});
        }
        WPJ4_CHECK(h.rawPort->writeCalls.empty());
    }

    // capture 的 DDMA 通知或结果槽同步销毁控制器后，调用端必须立即退出。
    void TestPendingStageStopsAfterSynchronousDestruction()
    {
        for (int scenario = 0; scenario < 3; ++scenario)
        {
            Harness h;
            h.AttachProcess(314);
            if (scenario == 0)
            {
                h.servicesRaw->SetDdmaGeneration(1);
                WPJ4_CHECK(h.target.requestChannel(ksword::memwb::Channel::Ddma));
            }
            auto* controller = new ks::ui::WorkbenchWriteController();
            controller->setTarget(&h.target);
            controller->setOverlay(&h.overlay);
            QPointer<ks::ui::WorkbenchWriteController> guarded(controller);
            bool deletedInSlot = false;
            h.LoadBaseline(0x8000, {0x00});
            if (scenario == 2)
            {
                h.LoadBaseline(0x9300, {0x00});
            }
            else
            {
                WPJ4_CHECK(controller->beginPendingStage(0x9300, QByteArray(1, '\x33')) != 0);
                WPJ4_CHECK(controller->beginPendingStage(0x9301, QByteArray(1, '\x44')) != 0);
            }
            if (scenario == 0)
            {
                QObject::connect(&h.target, &ks::ui::WorkbenchTarget::sessionChanged,
                    [&](quint32) { deletedInSlot = true; delete controller; });
                h.servicesRaw->SetDdmaGeneration(2);
                controller->notifyWindowMayCover();
                WPJ4_CHECK(!h.overlay.HasPendingPatches());
            }
            else
            {
                QObject::connect(controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                    [&](quint64, bool, QString) { deletedInSlot = true; delete controller; });
                if (scenario == 2)
                {
                    controller->beginPendingStage(0x9300, QByteArray(1, '\x33'));
                }
                else
                {
                    h.LoadBaseline(0x9300, {0x00, 0x00});
                    controller->notifyWindowMayCover();
                }
                const auto blocks = h.overlay.DiffBlocks();
                WPJ4_CHECK(blocks.size() == 1);
                if (blocks.size() == 1)
                {
                    WPJ4_CHECK(blocks[0].address == 0x9300 &&
                        blocks[0].after == std::vector<std::uint8_t>{0x33});
                }
            }
            WPJ4_CHECK(deletedInSlot && guarded.isNull());
        }
    }

    // 不同目标对象即使代次数字相同，也不能接管旧票据。
    void TestPendingStageRejectsTargetObjectReplacement()
    {
        Harness h;
        h.AttachProcess(315);
        h.LoadBaseline(0x8000, {0x00});
        const auto ticket = h.controller.beginPendingStage(0x9400, QByteArray(1, '\x55'));
        WPJ4_CHECK(ticket != 0);
        ks::ui::WorkbenchTarget other(std::make_unique<memwb_wpI_test::FakeWorkbenchServices>());
        memwb_wpI_test::AttachFakeProcess(other, 315, 1);
        WPJ4_CHECK(other.revisions().Source() == h.target.revisions().Source());
        h.controller.setTarget(&other);
        int rejected = 0;
        QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
            [&](quint64 id, bool ok, QString) { if (id == ticket && !ok) ++rejected; });
        h.LoadBaseline(0x9400, {0x00});
        h.controller.notifyWindowMayCover();
        WPJ4_CHECK(rejected == 1);
        WPJ4_CHECK(!h.overlay.HasPendingPatches());
    }

    void RunPendingStageTests()
    {
        TestInvalidInputsReturnZero();
        TestImmediatelyWithinWindowResolves();
        TestOutOfWindowThenNotifyResolves();
        TestTimeoutResolvesAsFailure();
        TestCancelPendingStage();
        TestFinalizePendingStageIgnoresUnknownTicket();
        TestUnreadBytesStaysPendingNotImmediateFail();
        TestNotifyWindowMayCoverHandlesMultipleTickets();
        TestFinalizePendingStageForTestForwardsExistingTicket();
        TestFinalizePendingStageErasesBeforeEmitting();
        TestPendingStageRejectsSourceChanges();
        TestPendingStageSnapshotSkipsCancelledTickets();
        TestPendingStageStopsAfterSynchronousDestruction();
        TestPendingStageRejectsTargetObjectReplacement();
    }
}
