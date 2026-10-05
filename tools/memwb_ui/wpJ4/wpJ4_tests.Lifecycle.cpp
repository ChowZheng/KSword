// wpJ4_tests.Lifecycle.cpp
// 作用：装配顺序/惰性构造/缓存值相关的关键判断——
//   - overlay/target/confirmation/audit/端口工厂任一缺失时 ensureTransaction()
//     失败（不弹框、不崩溃），Idle/Staged 状态下 IByteStore::Write 调用次数恒为 0。
//   - 审计接收器未设置时 ensureTransaction() 必须失败（"跳过 UI 确认不跳过审计"
//     的不变式，审阅后新增的硬性要求）。
//   - setUiConfirmSuppressed 在 transaction_ 构造前后都要正确缓存/应用。
//   - mode()/isCommitting() 在事务未构造时的安全默认值。
//   - D1/D2：transaction_ 构造成功之后，setOverlay/setTarget/setConfirmationSink/
//     setAuditSink 换成不同的值必须被拒绝（晚绑定防御），换成相同的值是空操作。
// 注：原 M-L8（析构函数清理"以 this 指针为键的关联表"的 ExtraCountForTest 断言）
// 随审核报告 D 节的裁决一起删除——头文件解冻后代次镶像/票据表都是直接私有成员，
// 跟随对象本身的构造/析构自动管理，不存在"关联表"这层东西需要单独验证。

#include "wpJ4_common.h"

#include <QDebug>

namespace wpj4_test
{
    namespace
    {
        // ------------------------------------------------------------
        // qWarning 捕获辅助（供 M-L10/M-L11 使用）：
        // confirmation_/audit_ 两个成员在 transaction_ 构造完成之后不会再被任何
        // 生产代码直接读取（只在 ensureTransaction 的一次性构造语句里用过）——
        // 这意味着 setConfirmationSink/setAuditSink 的晚绑定拒绝对"之后的提交
        // 结果"没有任何可观察差异（不像 setOverlay/setTarget，那两个成员在每次
        // 提交时都会被直接解引用，拒绝与否能从提交是否崩溃/用哪个确认接口上
        // 看出来）。这条防线唯一的可观察副作用是它的 qWarning 诊断日志，所以
        // 这里用 Qt 的全局消息处理器捕获它，而不是去读无法从外部观察的私有
        // 成员。g_captured 为空指针时表示没有测试在捕获，恢复为"转发给上一个
        // 处理器"（通常是 Qt 默认的控制台输出），避免吞掉其它测试的诊断信息。
        std::vector<QString>* g_capturedWarnings = nullptr;
        QtMessageHandler g_previousMessageHandler = nullptr;

        void CaptureWarningsHandler(
            QtMsgType type, const QMessageLogContext& context, const QString& message)
        {
            if (type == QtWarningMsg && g_capturedWarnings != nullptr)
            {
                g_capturedWarnings->push_back(message);
                return; // 吞掉，不打到控制台，避免污染测试输出。
            }
            if (g_previousMessageHandler != nullptr)
            {
                g_previousMessageHandler(type, context, message);
            }
        }
        // M-L1：overlay_ 缺失时 ensureTransaction 失败（commitPendingNow 返回默认
        // NoChange 报告，不崩溃）。
        void TestMissingOverlayFailsEnsure()
        {
            Harness h;
            h.AttachProcess(100);
            h.controller.setOverlay(nullptr); // 刚好覆盖掉构造时已经设置好的 overlay。
            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK(attempt.status == ks::ui::CommitEntryStatus::Started);
            WPJ4_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::NoChange);
            WPJ4_CHECK(h.rawPort == nullptr); // 端口工厂理应从未被调用过。
        }

        // M-L2：audit_ 缺失时 ensureTransaction 失败——"跳过 UI 确认不跳过审计"
        // 的不变式：没有审计接收器，提交必须整体失败，不能退回空审计悄悄放行。
        void TestMissingAuditFailsEnsure()
        {
            Harness h;
            h.AttachProcess(101);
            h.controller.setAuditSink(nullptr);
            h.LoadBaseline(0x1000, {0x11, 0x22, 0x33, 0x44});
            WPJ4_CHECK(h.Stage(0x1000, {0xAA, 0xBB, 0xCC, 0xDD}) == ksword::memwb::StageStatus::Ok);
            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::NoChange);
            WPJ4_CHECK(h.rawPort == nullptr);
        }

        // M-L3：confirmation_ 缺失时同理失败。注意：必须先真的 Stage 一个补丁再
        // 调用 commitPendingNow——否则 Commit() 的 (a) 步因为没有差异块直接返回
        // NoChange，根本不会走到需要 confirmation_ 的那一步，测不出"缺
        // confirmation_ 就不能构造事务"这条规则（曾经漏了这一步，变异 M15
        // 幸存，补上 Stage 之后才真正命中）。
        void TestMissingConfirmationFailsEnsure()
        {
            Harness h;
            h.AttachProcess(102);
            h.controller.setConfirmationSink(nullptr);
            h.LoadBaseline(0x1100, {0x00});
            WPJ4_CHECK(h.Stage(0x1100, {0x01}) == ksword::memwb::StageStatus::Ok);
            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::NoChange);
            WPJ4_CHECK(h.rawPort == nullptr); // ensureTransaction 应该在碰端口之前就失败。
        }

        // M-L4：ioPortFactory_ 未设置时 byteStore() 拿不到端口，ensureTransaction
        // 失败。
        void TestMissingIoPortFactoryFailsEnsure()
        {
            Harness h;
            h.AttachProcess(103);
            h.controller.setIoPortFactory(nullptr);
            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::NoChange);
        }

        // M-L5：Idle/Staged 状态下 IByteStore::Write 调用次数恒为 0——没有任何
        // Commit 发生时，端口的 Write 从未被调用过（即使已经成功 Stage 了补丁）。
        void TestIdleStagedNeverWrites()
        {
            Harness h;
            h.AttachProcess(104);
            h.EnsureWired();
            WPJ4_CHECK(h.rawPort != nullptr);
            h.LoadBaseline(0x2000, {0, 0, 0, 0});
            WPJ4_CHECK(h.Stage(0x2000, {1, 2, 3, 4}) == ksword::memwb::StageStatus::Ok);
            // 只是 Stage，没有调用 onEditCompleted/commitPendingNow：端口不该被碰。
            WPJ4_CHECK(h.rawPort->writeCalls.empty());
            WPJ4_CHECK(h.rawPort->calls.empty());
        }

        // M-L6：setUiConfirmSuppressed 在事务构造前缓存，构造后立即应用；缓存值
        // 不因为 ensureTransaction 先失败过一次就丢失。
        void TestUiConfirmSuppressedCached()
        {
            Harness h;
            // 先故意缺 target_，让 ensureTransaction 必然失败一次。
            h.controller.setTarget(nullptr);
            h.controller.setUiConfirmSuppressed(true);
            WPJ4_CHECK(h.controller.uiConfirmSuppressed() == true);
            const ks::ui::CommitAttempt failedAttempt = h.controller.commitPendingNow();
            WPJ4_CHECK(failedAttempt.report.outcome == ksword::memwb::CommitOutcome::NoChange);
            WPJ4_CHECK_NOTE(
                h.controller.uiConfirmSuppressed() == true,
                QStringLiteral("ensureTransaction 失败路径不得丢掉缓存的 suppressed 值"));

            // 补上 target_，这次 ensureTransaction 应该成功，缓存值立即生效。
            h.controller.setTarget(&h.target);
            h.AttachProcess(105);
            h.EnsureWired();
            WPJ4_CHECK(h.controller.uiConfirmSuppressed() == true);
        }

        // M-L7：mode()/isCommitting() 在事务尚未构造时的安全默认值。
        void TestDefaultsBeforeConstruction()
        {
            Harness h;
            WPJ4_CHECK(h.controller.mode() == ksword::memwb::WriteMode::Immediate);
            WPJ4_CHECK(h.controller.isCommitting() == false);
            WPJ4_CHECK(h.controller.canUndo() == false);
            WPJ4_CHECK(h.controller.canRedo() == false);
        }

        // M-L8（D1/D2 回归，原探针 overlay-null 场景）：transaction_ 已经构造
        // 成功之后，setOverlay(nullptr) 必须被拒绝（忽略调用），overlay_ 仍然是
        // 原来那份；否则接下来的 commitPendingNow() 会在内部对 overlay_->
        // DiffBlocks() 空指针解引用，崩溃（0xC0000005，见 review-wpJ4.md D1/D2
        // 探针实测读数）。本测试把探针场景搬进夹具，用"没有崩溃且第二次编辑
        // 正常提交成功"做可观察的回归判据。
        void TestLateRebindOverlayAfterTransactionIsRejected()
        {
            Harness h;
            h.AttachProcess(107);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0x1200, {0x00});
            WPJ4_CHECK(h.Stage(0x1200, {0x01}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x01})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};
            h.controller.onEditCompleted(); // 强制 ensureTransaction() 成功构造 transaction_。
            WPJ4_CHECK(h.rawPort->writeCalls.size() == 1);

            h.controller.setOverlay(nullptr); // D1/D2 修复：必须被拒绝，不是真的换成空。
            // 注意：这里不再补一次 setOverlay(&h.overlay) 把它"换回来"——那样会
            // 掩盖掉"拒绝晚绑定"这条防线本身是否存在（即使没有这条防线，紧跟着
            // 把指针换回原值也会让下面的提交照样成功，测不出差异）。本测试要
            // 验证的正是"不补这一步，overlay_ 也必须还是原来那份"。

            // 再来一次编辑+提交：如果 overlay_ 真的被换掉了，这一步会直接崩溃
            // （进程退出码 0xC0000005）；夹具进程能跑到这里并且断言通过，本身
            // 就是"没有崩溃"的证据。
            WPJ4_CHECK(h.Stage(0x1200, {0x02}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x01}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x02}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK_NOTE(
                attempt.status == ks::ui::CommitEntryStatus::Started
                    && attempt.report.outcome == ksword::memwb::CommitOutcome::Committed,
                QStringLiteral("setOverlay(nullptr) 必须被拒绝，overlay_ 应仍是原来那份且仍可提交"));
        }

        // M-L8b（D1/D2 回归，原探针 target-null 场景）：同上，换成 setTarget。
        void TestLateRebindTargetAfterTransactionIsRejected()
        {
            Harness h;
            h.AttachProcess(108);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0x1300, {0x00});
            WPJ4_CHECK(h.Stage(0x1300, {0x01}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x01})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};
            h.controller.onEditCompleted();
            WPJ4_CHECK(h.rawPort->writeCalls.size() == 1);

            h.controller.setTarget(nullptr); // D1/D2 修复：必须被拒绝。
            // 同上，不补一次换回原值的调用，否则测不出"拒绝晚绑定"这条防线。

            // 再来一次编辑+提交：commitPendingNow()/onEditCompleted() 内部都会调用
            // target_->session()——如果 target_ 真的被换掉了，这里会崩溃。
            WPJ4_CHECK(h.Stage(0x1300, {0x03}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x01}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x03}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK_NOTE(
                attempt.status == ks::ui::CommitEntryStatus::Started
                    && attempt.report.outcome == ksword::memwb::CommitOutcome::Committed,
                QStringLiteral("setTarget(nullptr) 必须被拒绝，target_ 应仍是原来那份且仍可提交"));
        }

        // M-L8c（D1/D2 补充）：transaction_ 已构造后，setOverlay/setTarget 换成
        // "同一个值"（包括同一个非空指针）必须是空操作，不应该产生任何副作用
        // 或警告（不是本条测试的断言对象，这里只验证不会崩溃、后续提交仍正常）。
        void TestLateRebindWithSameValueIsNoop()
        {
            Harness h;
            h.AttachProcess(109);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0x1400, {0x00});
            WPJ4_CHECK(h.Stage(0x1400, {0x01}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x01})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};
            h.controller.onEditCompleted();

            h.controller.setOverlay(&h.overlay); // 同一个指针：空操作。
            h.controller.setTarget(&h.target);    // 同一个指针：空操作。

            WPJ4_CHECK(h.Stage(0x1400, {0x02}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x01}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x02}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::Committed);
        }

        // M-L9：内核端口工厂未设置（返回 nullptr）时不是错误——只是"没有内核分步
        // 事务端口"，kernelPort_ 保持为空，不会尝试调用空指针。
        void TestKernelPortFactoryOptional()
        {
            Harness h;
            h.AttachProcess(106);
            h.kernelPortEnabled = false; // 工厂会返回 nullptr。
            h.EnsureWired();
            WPJ4_CHECK(h.rawKernelPort == nullptr);
            WPJ4_CHECK(h.rawPort != nullptr); // 普通端口工厂仍然正常被调用。
        }

        // M-L10（review2-wpJ4.md A3 测试缺口补测）：transaction_ 已经构造成功
        // 之后，setConfirmationSink 换成一个不同的非空值必须被拒绝——生产代码
        // 本来就和 setOverlay/setTarget 对称写了这条防线（D1/D2）。**重要区别**：
        // confirmation_ 成员在 transaction_ 构造完成之后不会再被任何生产代码
        // 直接读取（只在 ensureTransaction 的一次性构造语句里用过），所以"换成
        // 不同值后提交结果有没有变化"这条路径天生测不出任何差异——审核者
        // review2-wpJ4.md §C-1 建议的"提交结果"断言经本轮核实同样测不出来
        // （已记入 fix2-wpJ4.md 的异议）。这条防线唯一真正可观察的副作用是
        // qWarning 诊断日志，改用消息处理器捕获它。
        void TestLateRebindConfirmationSinkAfterTransactionIsRejected()
        {
            Harness h;
            h.AttachProcess(110);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0x1500, {0x00});
            WPJ4_CHECK(h.Stage(0x1500, {0x01}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x01})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};
            h.controller.onEditCompleted(); // 强制 ensureTransaction() 成功构造 transaction_。
            WPJ4_CHECK(h.rawPort->writeCalls.size() == 1);

            auto otherPrompter = std::make_unique<FakeConfirmPrompter>();
            ksword::memwb::MemoryWritePolicy otherPolicy;
            auto otherConfirmations = std::make_unique<ks::ui::WorkbenchConfirmations>(
                nullptr, &otherPolicy, std::move(otherPrompter));

            std::vector<QString> captured;
            g_capturedWarnings = &captured;
            g_previousMessageHandler = qInstallMessageHandler(CaptureWarningsHandler);
            h.controller.setConfirmationSink(otherConfirmations.get()); // D1/D2：必须被拒绝。
            qInstallMessageHandler(g_previousMessageHandler);
            g_capturedWarnings = nullptr;

            WPJ4_CHECK_NOTE(
                !captured.empty(),
                QStringLiteral("晚绑定换成不同的确认接口必须触发 qWarning 诊断（这是该防线唯一的可观察副作用）"));
            if (!captured.empty())
            {
                WPJ4_CHECK(captured.front().contains(QStringLiteral("setConfirmationSink")));
            }
        }

        // M-L11（review2-wpJ4.md A4 测试缺口补测）：同上，换成 setAuditSink，
        // 同一类"提交结果测不出差异"的理由（audit_ 同样只在构造语句里用过一次）。
        void TestLateRebindAuditSinkAfterTransactionIsRejected()
        {
            Harness h;
            h.AttachProcess(111);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0x1600, {0x00});
            WPJ4_CHECK(h.Stage(0x1600, {0x01}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x01})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};
            h.controller.onEditCompleted();
            WPJ4_CHECK(h.rawPort->writeCalls.size() == 1);

            FakeAuditSink otherAudit;
            std::vector<QString> captured;
            g_capturedWarnings = &captured;
            g_previousMessageHandler = qInstallMessageHandler(CaptureWarningsHandler);
            h.controller.setAuditSink(&otherAudit); // D1/D2：必须被拒绝。
            qInstallMessageHandler(g_previousMessageHandler);
            g_capturedWarnings = nullptr;

            WPJ4_CHECK_NOTE(
                !captured.empty(),
                QStringLiteral("晚绑定换成不同的审计接收器必须触发 qWarning 诊断（这是该防线唯一的可观察副作用）"));
            if (!captured.empty())
            {
                WPJ4_CHECK(captured.front().contains(QStringLiteral("setAuditSink")));
            }
        }

        // ---------------- S-wpJ4 补测（审核者追加，不在仓库夹具里） ----------------

        // S5：晚绑定被拒之后，"换回原值"必须是空操作、不再警告——这样"只警告不拒绝"
        // （警告照打但仍把成员赋成新值）的实现才会在第二次调用时多出一条警告。
        // 杀 x01-A3b。
        void TestSuppLateRebindSinkRejectedNotJustWarned()
        {
            Harness h;
            h.AttachProcess(644);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();

            auto otherPrompter = std::make_unique<FakeConfirmPrompter>();
            ksword::memwb::MemoryWritePolicy otherPolicy;
            auto otherConfirmations = std::make_unique<ks::ui::WorkbenchConfirmations>(
                nullptr, &otherPolicy, std::move(otherPrompter));

            std::vector<QString> captured;
            g_capturedWarnings = &captured;
            g_previousMessageHandler = qInstallMessageHandler(CaptureWarningsHandler);
            h.controller.setConfirmationSink(otherConfirmations.get());  // 被拒：恰好一条警告。
            h.controller.setConfirmationSink(h.confirmations.get());     // 换回原值：必须是空操作。
            qInstallMessageHandler(g_previousMessageHandler);
            g_capturedWarnings = nullptr;

            WPJ4_CHECK_NOTE(
                captured.size() == 1,
                QStringLiteral("被拒之后换回原值必须是空操作（总共只应有 1 条警告）"));
        }

        // S8（review2-wpJ4.md §C-3 的探针，仓库夹具没有收入）：transaction_ 构造之前把
        // 目标换成另一个对象，旧目标上的连接必须已被显式断开。杀 x27-R5。
        void TestSuppSetTargetDisconnectsOldConnectionBeforeTransactionExists()
        {
            Harness h;
            h.AttachProcess(646);
            auto services2 = std::make_unique<memwb_wpI_test::FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target2(std::move(services2));
            memwb_wpI_test::AttachFakeProcess(target2, 647, 1);
            h.controller.setTarget(&target2);
            const bool stillConnected = QObject::disconnect(&h.target, nullptr, &h.controller, nullptr);
            WPJ4_CHECK_NOTE(!stillConnected, QStringLiteral("旧目标上的连接应已被显式断开"));
        }

        // S9：内核端口工厂启用时 ensureTransaction 必须真的调用它，并把端口交给
        // MemoryIoByteStore（内核虚拟地址 + StandardDriver 走分步事务）。杀 x30。
        void TestSuppKernelPortFactoryInvokedAndRoutedWhenEnabled()
        {
            Harness h;
            h.SwitchToKernel(ksword::memwb::Channel::StandardDriver);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            WPJ4_CHECK_NOTE(
                h.rawKernelPort != nullptr,
                QStringLiteral("内核端口工厂启用时必须被调用一次"));
            const std::uint64_t kernelAddress = 0xFFFF800000001000ULL;
            h.LoadBaseline(kernelAddress, {0x00});
            WPJ4_CHECK(h.Stage(kernelAddress, {0x01}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x00})};
            h.controller.commitPendingNow();
            if (h.rawKernelPort != nullptr)
            {
                WPJ4_CHECK_NOTE(
                    h.rawKernelPort->prepareCalls.size() >= 1,
                    QStringLiteral("内核虚拟地址 + StandardDriver 的写入必须经由内核端口 Prepare"));
            }
        }

        // S10：端口工厂返回空指针时 ensureTransaction 必须失败而不是解引用空指针。
        // 杀 x31。
        void TestSuppIoPortFactoryReturningNullFailsEnsure()
        {
            Harness h;
            h.AttachProcess(648);
            h.controller.setUiConfirmSuppressed(true);
            h.controller.setIoPortFactory(
                []() -> std::unique_ptr<ksword::memwb::IMemoryIoPort> { return nullptr; });
            h.LoadBaseline(0x1700, {0x00});
            WPJ4_CHECK(h.Stage(0x1700, {0x01}) == ksword::memwb::StageStatus::Ok);
            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::NoChange);
        }

        // S11：没有目标时 onEditCompleted 必须是空操作（不崩溃、不发提交信号）。杀 x37。
        void TestSuppOnEditCompletedWithoutTargetIsNoop()
        {
            Harness h;
            h.controller.setTarget(nullptr);
            int finishedCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFinished,
                [&](ksword::memwb::CommitReport) { ++finishedCount; });
            h.controller.onEditCompleted();
            WPJ4_CHECK(finishedCount == 0);
        }
    }

    void RunLifecycleTests()
    {
        TestSuppKernelPortFactoryInvokedAndRoutedWhenEnabled();
        TestSuppIoPortFactoryReturningNullFailsEnsure();
        TestSuppOnEditCompletedWithoutTargetIsNoop();
        TestSuppSetTargetDisconnectsOldConnectionBeforeTransactionExists();
        TestSuppLateRebindSinkRejectedNotJustWarned();
        TestMissingOverlayFailsEnsure();
        TestMissingAuditFailsEnsure();
        TestMissingConfirmationFailsEnsure();
        TestMissingIoPortFactoryFailsEnsure();
        TestIdleStagedNeverWrites();
        TestUiConfirmSuppressedCached();
        TestDefaultsBeforeConstruction();
        TestLateRebindOverlayAfterTransactionIsRejected();
        TestLateRebindTargetAfterTransactionIsRejected();
        TestLateRebindWithSameValueIsNoop();
        TestKernelPortFactoryOptional();
        TestLateRebindConfirmationSinkAfterTransactionIsRejected();
        TestLateRebindAuditSinkAfterTransactionIsRejected();
    }
}
