// wpF_tests_panel.cpp
// 作用：Int3PatchPanel 的离屏验证——折叠头部、工具钮可用性、写入/还原/全部还原的 UI 接线、
//       Diverged 行"仅可丢弃"、孤立分组"清除"、确认计数=0、"全部还原"按钮的启用条件与
//       点击后三段式文案（已还原/被别处改过/失败）、深/浅 × 窄/宽截图。

#include "wpF_fake_port.h"
#include "wpF_tests_common.h"

#include "../../../shared/evidence/memory_workbench/MemoryPatchByteStore.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/Int3Controller.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/Int3PatchPanel.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QObject>
#include <QRect>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <iostream>
#include <memory>
#include <vector>

namespace wpf_test
{
    using ks::ui::Int3Controller;
    using ks::ui::Int3InstallOutcome;
    using ks::ui::Int3PatchPanel;
    using ksword::memwb::Channel;
    using ksword::memwb::IPatchByteStore;
    using ksword::memwb::InstallStatus;
    using ksword::memwb::MemoryPatchByteStore;
    using ksword::memwb::MemoryTargetSession;
    using ksword::memwb::PatchTarget;
    using ksword::memwb::Scope;

    namespace
    {
        // MakeFactory：与 wpF_tests_controller.cpp 里的同名函数逻辑一致，各自一份避免
        // 两个测试文件相互依赖内部细节。
        Int3Controller::ByteStoreFactory MakeFactory(FakeMemoryIoPort& port, MemoryTargetSession& session)
        {
            return [&port, &session](const PatchTarget& target, const Channel channel) -> std::unique_ptr<IPatchByteStore> {
                session.scope = Scope::ProcessVirtual;
                session.pid = target.pid;
                session.processCreateTime100ns = target.processCreateTime100ns;
                session.attachGeneration = target.attachGeneration;
                session.channel = channel;
                return std::make_unique<MemoryPatchByteStore>(port, session);
            };
        }

        // FindRowByFirstColumnText：按第一列文字找行号，找不到返回 -1。
        int FindRowByFirstColumnText(const QTableWidget* table, const QString& text)
        {
            for (int row = 0; row < table->rowCount(); ++row)
            {
                const QTableWidgetItem* item = table->item(row, 0);
                if (item != nullptr && item->text() == text)
                {
                    return row;
                }
            }
            return -1;
        }

        // SendContextMenuAt：向表格 viewport 的某个单元格中心发一次右键菜单事件，并用定时器
        // 在菜单弹出的嵌套事件循环里触发指定文字的动作（仿 memwb_ui_tests.Edit.cpp 的做法）。
        void SendContextMenuAt(QTableWidget* table, const int row, const int column, const QString& actionText)
        {
            const QRect cellRect = table->visualItemRect(table->item(row, column));
            const QPoint cellCenter = cellRect.center();
            QTimer invoker;
            invoker.setInterval(20);
            QObject::connect(&invoker, &QTimer::timeout, &invoker, [&actionText]() {
                TriggerPopupMenuActionByText(actionText);
            });
            invoker.start();
            QContextMenuEvent event(
                QContextMenuEvent::Mouse, cellCenter, table->viewport()->mapToGlobal(cellCenter));
            QApplication::sendEvent(table->viewport(), &event);
            invoker.stop();
        }
    }

    // RunPanelTests：见文件头说明。
    void RunPanelTests(const QString& shotsDir)
    {
        const int checksBefore = g_checks;
        const int failuresBefore = g_failures;

        QDir().mkpath(shotsDir);

        FakeMemoryIoPort port;
        MemoryTargetSession session;
        Int3Controller controller(MakeFactory(port, session));
        const PatchTarget target = MakeTarget(4242, 100);
        controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);

        Int3PatchPanel panel(&controller);
        // 必须先显示：QMenu::exec()/QMessageBox::exec() 这类独立弹出窗口在离屏平台下依赖
        // 发起它们的控件已经处于"已显示"的窗口树中，否则 activePopupWidget() 等状态不会
        // 正确建立，exec() 的嵌套事件循环可能永远等不到定时器回调把它关掉（实测会卡死）。
        panel.resize(300, 400);
        panel.show();
        QApplication::processEvents();
        panel.SetTargetLabelFormatter(
            [](const std::uint32_t pid, std::uint64_t) { return QStringLiteral("demo.exe · PID %1").arg(pid); });
        panel.SetAddressFormatter(
            [](const std::uint64_t address) { return QStringLiteral("demo.dll+0x%1").arg(address, 0, 16); });

        QString lastStatusText;
        bool lastStatusError = false;
        int messageCount = 0;
        QObject::connect(
            &panel,
            &Int3PatchPanel::resultMessage,
            [&lastStatusText, &lastStatusError, &messageCount](const QString& text, const bool isError) {
                lastStatusText = text;
                lastStatusError = isError;
                ++messageCount;
            });

        // 初始折叠；没有插入点时"写入"禁用；没有任何条目/选中行时"还原"同样禁用
        // （2026 年第二轮审核新增变异：updateToolbarEnabled 把"还原"启用条件的 && 错改成
        // ||，什么都没选中时 selected.has_value()==false，但 || 会被 !diverged==true
        // 短路成真，错误地启用"还原"按钮；此前全部面板测试都是先装一条再检查，从未
        // 覆盖过"空账本、无选中"这个最初始的状态）。
        CHECK(panel.IsCollapsed());
        CHECK(!panel.InstallButtonForTest()->isEnabled());
        CHECK(!panel.RestoreButtonForTest()->isEnabled());

        // 设置插入点并写入：首次写入成功自动展开。
        panel.SetInsertionPoint(0x1000, true);
        CHECK(panel.InstallButtonForTest()->isEnabled());
        port.memory[0x1000] = 0x90;
        QTest::mouseClick(panel.InstallButtonForTest(), Qt::LeftButton);
        CHECK(!panel.IsCollapsed());
        CHECK(messageCount == 1 && !lastStatusError);
        CHECK(panel.TableForTest()->rowCount() == 1);
        CHECK(panel.TableForTest()->item(0, 0)->text() == QStringLiteral("demo.dll+0x1000"));
        CHECK(panel.TableForTest()->item(0, 2)->text().contains(QStringLiteral("demo.exe")));
        CHECK(panel.HeaderForTest()->text() == QStringLiteral("int3 补丁 (1)"));

        // 再写一个，之后故意让它 Diverged。
        panel.SetInsertionPoint(0x2000, true);
        port.memory[0x2000] = 0x91;
        QTest::mouseClick(panel.InstallButtonForTest(), Qt::LeftButton);
        CHECK(panel.TableForTest()->rowCount() == 2);

        port.memory[0x2000] = 0xAB; // 别处把补丁字节改掉了
        panel.TableForTest()->selectRow(1);
        const qulonglong divergedRowId = panel.TableForTest()->item(1, 0)->data(Qt::UserRole).toULongLong();
        CHECK(panel.RestoreButtonForTest()->isEnabled());
        QTest::mouseClick(panel.RestoreButtonForTest(), Qt::LeftButton);
        CHECK(lastStatusError);
        // 还原会触发表格重建（Int3Controller::changed -> onLedgerChanged -> rebuildTable），
        // 重建会清空选区；必须重新按 id 选中同一条目，才能真正验证"Diverged 后仅可丢弃，
        // 还原钮必须禁用"这条判断——而不是碰巧因为"没有选中任何行"而禁用。
        bool reselected = false;
        for (int row = 0; row < panel.TableForTest()->rowCount(); ++row)
        {
            const QTableWidgetItem* item = panel.TableForTest()->item(row, 0);
            if (item != nullptr && item->data(Qt::UserRole).toULongLong() == divergedRowId)
            {
                panel.TableForTest()->selectRow(row);
                reselected = true;
                break;
            }
        }
        CHECK(reselected);
        CHECK(!panel.RestoreButtonForTest()->isEnabled()); // Diverged 后"还原"禁用，仅可丢弃

        // 右键该 Diverged 行：菜单只提供"丢弃记录"，触发后行消失（账本里也没有了）。
        {
            const int row = 1;
            const int beforeRows = panel.TableForTest()->rowCount();
            SendContextMenuAt(panel.TableForTest(), row, 0, QStringLiteral("丢弃记录"));
            CHECK(panel.TableForTest()->rowCount() == beforeRows - 1);
            CHECK(controller.Entries().size() == 1);
        }

        // 孤立：目标消失，表格出现分组标题 + 1 条孤立项。
        controller.OnTargetGone(target.pid, target.processCreateTime100ns);
        CHECK(panel.TableForTest()->rowCount() == 2);
        const int headerRow = FindRowByFirstColumnText(panel.TableForTest(), QStringLiteral("孤立补丁（目标已退出）"));
        CHECK(headerRow >= 0);
        if (headerRow >= 0)
        {
            const int orphanRow = headerRow + 1;
            CHECK(panel.TableForTest()->item(orphanRow, 2)->text() == QStringLiteral("已退出"));
        }

        // 右键分组标题行：触发"清除"，整组消失。
        if (headerRow >= 0)
        {
            SendContextMenuAt(panel.TableForTest(), headerRow, 0, QStringLiteral("清除"));
            CHECK(controller.OrphanedEntries().empty());
            CHECK(panel.TableForTest()->rowCount() == 0);
        }

        // 确认计数=0：整段流程（写入、还原、丢弃、清除）都没有出现任何 QMessageBox。
        CHECK(CountOpenMessageBoxes() == 0);

        // ---- 面板"全部还原"按钮：启用条件随账本是否为空联动，点击后的三段式文案
        //      （已还原/被别处改过/失败）要如实反映 Restored/Diverged 的真实计数
        //      （2026 年审核报告 F-M12/F-M13：此前这条接线完全没有任何 UI 级断言覆盖，
        //      用一套独立的 controller/panel 实例跑，避免与上面已经跑脏的账本状态相互
        //      影响）。----
        {
            FakeMemoryIoPort allPort;
            MemoryTargetSession allSession;
            Int3Controller allController(MakeFactory(allPort, allSession));
            const PatchTarget allTarget = MakeTarget(5050, 1);
            allController.SetCurrentContext(allTarget, Scope::ProcessVirtual, Channel::UserMode);

            Int3PatchPanel allPanel(&allController);
            allPanel.resize(300, 400);
            allPanel.show();
            QApplication::processEvents();

            QString allText;
            bool allError = false;
            int allMessageCount = 0;
            QObject::connect(
                &allPanel,
                &Int3PatchPanel::resultMessage,
                [&allText, &allError, &allMessageCount](const QString& text, const bool isError) {
                    allText = text;
                    allError = isError;
                    ++allMessageCount;
                });

            // 空账本：按钮必须禁用（F-M12 若把启用条件取反，这里会错报成"启用"）。
            CHECK(!allPanel.RestoreAllButtonForTest()->isEnabled());

            allPanel.SetInsertionPoint(0x6000, true);
            allPort.memory[0x6000] = 0x80;
            QTest::mouseClick(allPanel.InstallButtonForTest(), Qt::LeftButton);
            allPanel.SetInsertionPoint(0x6100, true);
            allPort.memory[0x6100] = 0x81;
            QTest::mouseClick(allPanel.InstallButtonForTest(), Qt::LeftButton);
            CHECK(allPanel.TableForTest()->rowCount() == 2);

            // 有条目：按钮必须启用（F-M12 的另一半——条件取反时这里会错报成"禁用"）。
            CHECK(allPanel.RestoreAllButtonForTest()->isEnabled());

            allPort.memory[0x6100] = 0xFE; // 第二条变 Diverged：全部还原应得到"1 还原 + 1 改过"。
            allMessageCount = 0;
            QTest::mouseClick(allPanel.RestoreAllButtonForTest(), Qt::LeftButton);
            CHECK(allMessageCount == 1 && allError); // 有 Diverged，状态条应标红
            // F-M13 把 divergedCount==0&&otherFailureCount==0 改成 ||：divergedCount=1 时
            // ||会被 otherFailureCount==0 短路成真，错误地走进"已全部还原"分支，下面这三条
            // 断言就会全部失败（因为文案里不会再有"字节被别处改过"）。
            CHECK(allText.contains(QStringLiteral("已还原 1 处")));
            CHECK(allText.contains(QStringLiteral("1 处字节被别处改过")));
            CHECK(allText.contains(QStringLiteral("0 处失败")));

            // 还有一条 Diverged 留在账本里：按钮必须仍然启用。
            CHECK(allPanel.RestoreAllButtonForTest()->isEnabled());
            CHECK(allController.Entries().size() == 1);

            // 丢弃剩下这条，账本清空：按钮必须变回禁用。
            CHECK(allController.Discard(allController.Entries().front().id));
            CHECK(!allPanel.RestoreAllButtonForTest()->isEnabled());

            // 再装一条并让它顺利全部还原：应该走"已全部还原"分支，不标红，按钮回到禁用。
            allPanel.SetInsertionPoint(0x6200, true);
            allPort.memory[0x6200] = 0x82;
            QTest::mouseClick(allPanel.InstallButtonForTest(), Qt::LeftButton);
            allMessageCount = 0;
            QTest::mouseClick(allPanel.RestoreAllButtonForTest(), Qt::LeftButton);
            CHECK(allMessageCount == 1 && !allError);
            CHECK(allText.contains(QStringLiteral("已全部还原")));
            CHECK(!allPanel.RestoreAllButtonForTest()->isEnabled());
            CHECK(CountOpenMessageBoxes() == 0);
        }

        // ---- ChannelGuidance：标准驱动通道下，写入/还原失败的提示文案必须追加"可切换到
        //      用户态通道重试"的引导语（2026 年自补变异：把 channel == StandardDriver
        //      的判据取反；此前面板测试全程只用过 Channel::UserMode，从未真正走到这条
        //      分支，是一个此前完全没有任何断言覆盖的真实路径）。----
        {
            FakeMemoryIoPort guidancePort;
            MemoryTargetSession guidanceSession;
            Int3Controller guidanceController(MakeFactory(guidancePort, guidanceSession));
            const PatchTarget guidanceTarget = MakeTarget(8080, 1);
            guidanceController.SetCurrentContext(guidanceTarget, Scope::ProcessVirtual, Channel::StandardDriver);

            Int3PatchPanel guidancePanel(&guidanceController);
            guidancePanel.resize(300, 400);
            guidancePanel.show();
            QApplication::processEvents();

            QString guidanceText;
            QObject::connect(
                &guidancePanel,
                &Int3PatchPanel::resultMessage,
                [&guidanceText](const QString& text, bool) { guidanceText = text; });

            // 写入失败（目标不可读）：标准驱动通道必须追加引导语。
            guidancePanel.SetInsertionPoint(0x7000, true);
            guidancePort.failAllReads = true;
            QTest::mouseClick(guidancePanel.InstallButtonForTest(), Qt::LeftButton);
            CHECK(guidanceText.contains(QStringLiteral("可切换到用户态通道重试")));
            guidancePort.failAllReads = false;

            // 写入成功一条，再让还原时的写回失败，还原失败也必须追加同一句引导语。
            guidancePort.memory[0x7100] = 0xAA;
            guidancePanel.SetInsertionPoint(0x7100, true);
            QTest::mouseClick(guidancePanel.InstallButtonForTest(), Qt::LeftButton);
            CHECK(guidancePanel.TableForTest()->rowCount() == 1);
            guidancePanel.TableForTest()->selectRow(0);
            guidancePort.failAllWrites = true;
            guidanceText.clear();
            QTest::mouseClick(guidancePanel.RestoreButtonForTest(), Qt::LeftButton);
            CHECK(guidanceText.contains(QStringLiteral("可切换到用户态通道重试")));
        }

        // ---- 未证实回滚的恢复条目也必须保留安装通道：通过真实面板按钮安装和还原，
        //      安装验证读失败、回滚写失败后切换通道，仍用最初通道还原原字节。----
        {
            FakeMemoryIoPort recoveryPort;
            MemoryTargetSession recoverySession;
            std::vector<Channel> requestedChannels;
            Int3Controller::ByteStoreFactory factory =
                [&](const PatchTarget& targetAtCall, const Channel channelAtCall) -> std::unique_ptr<IPatchByteStore> {
                    requestedChannels.push_back(channelAtCall);
                    recoverySession.scope = Scope::ProcessVirtual;
                    recoverySession.pid = targetAtCall.pid;
                    recoverySession.processCreateTime100ns = targetAtCall.processCreateTime100ns;
                    recoverySession.attachGeneration = targetAtCall.attachGeneration;
                    recoverySession.channel = channelAtCall;
                    // 用错切换后的标准驱动通道会要求批准，不能碰巧还原成功。
                    recoveryPort.requireApproval = channelAtCall == Channel::StandardDriver;
                    return std::make_unique<MemoryPatchByteStore>(recoveryPort, recoverySession);
                };
            Int3Controller recoveryController(factory);
            const PatchTarget recoveryTarget = MakeTarget(8085, 123);
            recoveryController.SetCurrentContext(recoveryTarget, Scope::ProcessVirtual, Channel::UserMode);

            Int3PatchPanel recoveryPanel(&recoveryController);
            recoveryPanel.SetCollapsed(false);
            recoveryPanel.resize(300, 400);
            recoveryPanel.show();
            QApplication::processEvents();
            QString recoveryText;
            bool recoveryError = false;
            int recoveryMessages = 0;
            QObject::connect(&recoveryPanel, &Int3PatchPanel::resultMessage,
                [&](const QString& text, const bool isError) {
                    recoveryText = text;
                    recoveryError = isError;
                    ++recoveryMessages;
                });

            constexpr std::uint64_t recoveryAddress = 0x7150;
            constexpr std::uint8_t originalByte = 0x53;
            recoveryPort.memory[recoveryAddress] = originalByte;
            recoveryPort.failReadOnCall = 2;
            recoveryPort.failWriteOnCall = 2;
            recoveryPanel.SetInsertionPoint(recoveryAddress, true);
            QTest::mouseClick(recoveryPanel.InstallButtonForTest(), Qt::LeftButton);
            CHECK(recoveryMessages == 1 && recoveryError);
            CHECK(recoveryText.contains(QStringLiteral("写入后回读不符，写回原字节也失败")));
            CHECK(recoveryPort.readCalls == 2 && recoveryPort.writeCalls == 2);
            CHECK(recoveryPort.memory[recoveryAddress] == ksword::memwb::kInt3PatchByte);
            CHECK(recoveryController.Entries().size() == 1);
            CHECK(recoveryPanel.TableForTest()->rowCount() == 1);
            const auto recoveryEntry = recoveryController.Entries().empty()
                ? ksword::memwb::PatchEntry{} : recoveryController.Entries().front();
            CHECK(recoveryEntry.id != 0 && recoveryEntry.originalByte == originalByte
                && recoveryEntry.address == recoveryAddress && recoveryEntry.pid == recoveryTarget.pid
                && recoveryEntry.processCreateTime100ns == recoveryTarget.processCreateTime100ns);
            CHECK(recoveryController.InstalledChannel(recoveryEntry.id) == Channel::UserMode);
            CHECK(requestedChannels.size() == 1 && requestedChannels.front() == Channel::UserMode);
            CHECK(CountOpenMessageBoxes() == 0);

            recoveryController.SetCurrentContext(recoveryTarget, Scope::ProcessVirtual, Channel::StandardDriver);
            recoveryPanel.TableForTest()->selectRow(0);
            CHECK(recoveryPanel.RestoreButtonForTest()->isEnabled());
            recoveryMessages = 0;
            QTest::mouseClick(recoveryPanel.RestoreButtonForTest(), Qt::LeftButton);
            CHECK(recoveryMessages == 1 && !recoveryError && recoveryText == QStringLiteral("已还原"));
            CHECK(requestedChannels.size() == 2 && requestedChannels.back() == Channel::UserMode);
            CHECK(recoveryPort.memory[recoveryAddress] == originalByte);
            CHECK(recoveryPort.readCalls == 4 && recoveryPort.writeCalls == 3
                && recoveryPort.approvedTrueCount == 0 && recoveryPort.approvedFalseCount == 3);
            CHECK(recoveryController.Entries().empty() && recoveryPanel.TableForTest()->rowCount() == 0);
            CHECK(!recoveryController.InstalledChannel(recoveryEntry.id).has_value());
            CHECK(CountOpenMessageBoxes() == 0);
        }

        // ---- 自动展开只应该在"真正写入成功"时触发（2026 年自补变异：把 onInstallClicked
        //      判断自动展开的 && 错改成 ||，面板仍处于初始折叠状态时，任何一次失败的写入
        //      都会被误判成"该展开了"；此前全部面板测试的第一次 Install 调用都是成功的，
        //      从未覆盖过"折叠状态下写入失败"这条路径）。----
        {
            FakeMemoryIoPort failPort;
            MemoryTargetSession failSession;
            Int3Controller failController(MakeFactory(failPort, failSession));
            const PatchTarget failTarget = MakeTarget(8090, 1);
            failController.SetCurrentContext(failTarget, Scope::ProcessVirtual, Channel::UserMode);

            Int3PatchPanel failPanel(&failController);
            failPanel.resize(300, 400);
            failPanel.show();
            QApplication::processEvents();

            CHECK(failPanel.IsCollapsed());
            failPort.memory[0x7200] = ksword::memwb::kInt3PatchByte; // 让第一次写入必然失败（AlreadyContainsPatchByte）
            failPanel.SetInsertionPoint(0x7200, true);
            QTest::mouseClick(failPanel.InstallButtonForTest(), Qt::LeftButton);
            CHECK(failPanel.TableForTest()->rowCount() == 0); // 确认确实没有写入成功
            CHECK(failPanel.IsCollapsed()); // 失败不应该触发自动展开
        }

        // ---- 截图：深/浅 × 窄/宽，表格里同时含正常、Diverged、孤立三种行，信息丰富 ----
        // 一条孤立项：先切到另一个目标装一条，再调 OnTargetGone——必须真的先有属于该目标的
        // 条目，OnTargetGone 对不存在的目标什么也不会做（这是正确行为，截图数据要配合它）。
        const PatchTarget otherTarget = MakeTarget(9999, 1);
        controller.SetCurrentContext(otherTarget, Scope::ProcessVirtual, Channel::UserMode);
        port.memory[0x5000] = 0x99;
        controller.Install(otherTarget, 0x5000, 1);
        controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
        controller.OnTargetGone(otherTarget.pid, otherTarget.processCreateTime100ns);

        // 一条正常、一条 Diverged：Diverged 必须经由面板自身的还原按钮触发（而不是直接调
        // controller->Restore），因为"当前字节已不是 CC"这个警示标记只存在于面板侧
        // （m_divergedIds），账本本身不持久记录"曾经 Diverged 过"。
        port.memory[0x3000] = 0x10;
        controller.Install(target, 0x3000, 1);
        port.memory[0x4000] = 0x11;
        const Int3InstallOutcome divergedSample = controller.Install(target, 0x4000, 2);
        CHECK(divergedSample.status == InstallStatus::Installed);
        port.memory[0x4000] = 0x12; // 别处改过，制造 Diverged
        for (int row = 0; row < panel.TableForTest()->rowCount(); ++row)
        {
            const QTableWidgetItem* item = panel.TableForTest()->item(row, 0);
            if (item != nullptr && item->data(Qt::UserRole).toULongLong() == divergedSample.id)
            {
                panel.TableForTest()->selectRow(row);
                break;
            }
        }
        QTest::mouseClick(panel.RestoreButtonForTest(), Qt::LeftButton);

        panel.SetCollapsed(false);
        for (const bool dark : { false, true })
        {
            ApplyThemeForShots(dark);
            for (const int width : { 260, 420 })
            {
                panel.resize(width, 320);
                panel.show();
                QApplication::processEvents();
                const QImage image = panel.grab().toImage().convertToFormat(QImage::Format_ARGB32);
                CHECK(!image.isNull());
                const QString path = QStringLiteral("%1/int3_panel_%2_%3.png")
                                          .arg(shotsDir)
                                          .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                                          .arg(width);
                CHECK(image.save(path));
                panel.hide();
            }
        }
        ApplyThemeForShots(false);

        std::cout << "wpF panel tests: " << (g_checks - checksBefore) << " checks, "
                  << (g_failures - failuresBefore) << " failures" << std::endl;
    }
}
