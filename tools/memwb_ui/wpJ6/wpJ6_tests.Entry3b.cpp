// ============================================================
// wpJ6_tests.Entry3b.cpp
// 作用：波 4 的 3b"入口切换"里属于视图一侧的专项测试：
//   1) int3 账本"当前目标"是全进程唯一一份，主 Dock 的视图与内嵌进程详情窗口的视图共用它——
//      视图必须在自己的 int3 操作前（右键写入/还原、侧栏面板三个按钮、离开询问、分离安全网）
//      以及被显示/窗口被激活时把自己的会话重新声明为当前目标，否则操作会作用在别的视图的目标上；
//   2) focusAddress()：PTE 页取默认地址用；
//   3) 内嵌模式拒绝钉住请求（模块表预览别的进程时，内嵌窗口不会静默去看错误的进程）；
//   4) WorkbenchBookIntake：搜索结果"加入地址簿"的上限/去重/种类/目标键。
// 入口：RunEntry3bTests（由 wpJ6_main.cpp 调用）。
// ============================================================
#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/AddressBookStore.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/Int3Controller.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/MemoryWorkbenchView.Internal.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchBookIntake.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchShared.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>

namespace wpj6_test
{
    namespace
    {
        // ArmClicker：轮询等模态 QMessageBox 出现后点指定文字的按钮（离屏环境没有人能点真实对话框）。
        struct ArmClicker
        {
            explicit ArmClicker(const QString& wantedText)
                : wanted(wantedText)
            {
                QObject::connect(&timer, &QTimer::timeout, [this]() {
                    ++ticks;
                    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                    if (box != nullptr)
                    {
                        seen = true;
                        for (auto* button : box->buttons())
                        {
                            if (button->text() == wanted)
                            {
                                clicked = true;
                                timer.stop();
                                button->click();
                                return;
                            }
                        }
                        timer.stop();
                        box->reject();
                        return;
                    }
                    if (ticks > 120)
                    {
                        timer.stop();
                    }
                });
                timer.start(50);
            }
            ~ArmClicker() { timer.stop(); }

            QTimer timer;
            QString wanted;
            bool clicked = false;
            bool seen = false;
            int ticks = 0;
        };

        // ToggleInt3：经画布右键菜单触发"写入/还原 int3"。
        void ToggleInt3(ks::ui::MemoryWorkbenchView& view, const std::uint64_t address)
        {
            QMenu* menu = view.hexPaneForTest()->canvas()->buildContextMenu(address, true);
            if (menu != nullptr)
            {
                for (auto* action : menu->actions())
                {
                    if (action->text().contains(QStringLiteral("int3")))
                    {
                        emit action->triggered();
                        break;
                    }
                }
            }
            delete menu;
        }

        // ClearInt3AndByte：清掉共享 int3 账本，并把假内存里给定地址的字节恢复成 0x00。
        void ClearInt3AndBytes(const std::vector<std::uint64_t>& addresses)
        {
            auto& backend = ConfigureSharedOnce();
            auto& int3 = ks::ui::WorkbenchShared::Instance().Int3();
            const auto entries = int3.Entries();
            for (const auto& entry : entries)
            {
                int3.Discard(entry.id);
            }
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            for (const std::uint64_t address : addresses)
            {
                backend.backing->bytes[address - backend.backing->base] = 0x00;
            }
        }

        std::uint8_t BackingByte(const std::uint64_t address)
        {
            auto& backend = ConfigureSharedOnce();
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            return backend.backing->bytes[address - backend.backing->base];
        }

        std::size_t Int3Count()
        {
            return ks::ui::WorkbenchShared::Instance().Int3().Entries().size();
        }

        // Int3CountForPid：账本里属于某个 pid 的待还原条目数。
        std::size_t Int3CountForPid(const std::uint32_t pid)
        {
            std::size_t count = 0;
            for (const auto& entry : ks::ui::WorkbenchShared::Instance().Int3().Entries())
            {
                if (entry.pid == pid)
                {
                    ++count;
                }
            }
            return count;
        }

        std::uint32_t Int3ContextPid()
        {
            return ks::ui::WorkbenchShared::Instance().Int3().CurrentTarget().pid;
        }

        // ForceInt3Context：测试里"让别的视图占住账本当前目标"的独立手段——直接写账本，
        // 不经过被测的视图代码（否则被测代码坏了，这一步也会跟着坏，测试变成自证）。
        void ForceInt3Context(ks::ui::MemoryWorkbenchView& view)
        {
            const auto& session = view.target().session();
            ks::ui::WorkbenchShared::Instance().Int3().SetCurrentContext(
                ksword::memwb::PatchTarget{ session.pid, session.processCreateTime100ns, session.attachGeneration },
                session.scope,
                session.channel);
        }

        // JumpTo：经统一入口把插入点落到某地址（同时让侧栏 int3 面板的插入点跟上）。
        bool JumpTo(ks::ui::MemoryWorkbenchView& view, const std::uint64_t address)
        {
            ks::ui::NavRequest request;
            request.address = address;
            return view.openAt(request) == ks::ui::NavStatus::Ok;
        }

        // ---- 1) 两个视图共用账本：右键写入/还原必须作用在自己的目标上 ----
        void TestContextMenuActsOnOwnTarget()
        {
            const std::uint64_t addressA = 0xB0ULL;
            const std::uint64_t addressB = 0xC0ULL;
            Harness a;
            a.AttachProcess(7101);
            Harness b;
            b.AttachProcess(7102);   // 账本当前目标现在是 b 的
            PumpUntil([]() { return true; }, 10);
            WaitForStageable(a.view->hexPaneForTest(), addressA);
            WaitForStageable(b.view->hexPaneForTest(), addressB);

            ToggleInt3(*a.view, addressA);   // 写入：属于 a 的目标
            ToggleInt3(*b.view, addressB);   // 写入：属于 b 的目标
            WPJ6_CHECK(Int3Count() == 2U);
            WPJ6_CHECK(Int3CountForPid(7101) == 1U);
            WPJ6_CHECK(Int3CountForPid(7102) == 1U);

            // a 再点一次同一地址：必须还原 a 自己的补丁，即使账本当前目标刚被 b 占着。
            ToggleInt3(*a.view, addressA);
            WPJ6_CHECK_NOTE(Int3CountForPid(7101) == 0U, QStringLiteral("a 的右键还原必须生效（账本当前目标曾是 b）"));
            WPJ6_CHECK_NOTE(Int3CountForPid(7102) == 1U, QStringLiteral("b 的补丁不得被 a 的操作动到"));
            WPJ6_CHECK(BackingByte(addressA) != 0xCC);
            WPJ6_CHECK(BackingByte(addressB) == 0xCC);
            ClearInt3AndBytes({ addressA, addressB });
        }

        // ---- 1) 侧栏面板的按钮也要作用在自己的目标上 ----
        void TestPanelButtonsActOnOwnTarget()
        {
            const std::uint64_t addressA = 0xB0ULL;
            const std::uint64_t addressB = 0xC0ULL;
            Harness a;
            a.AttachProcess(7111);
            Harness b;
            b.AttachProcess(7112);   // 账本当前目标现在是 b 的
            PumpUntil([]() { return true; }, 10);
            WaitForStageable(a.view->hexPaneForTest(), addressA);
            WaitForStageable(b.view->hexPaneForTest(), addressB);

            // b 先经面板写一个补丁（账本当前目标本来就是 b，作为对照）。
            WPJ6_CHECK(JumpTo(*b.view, addressB));
            b.view->int3PanelForTest()->InstallButtonForTest()->click();
            WPJ6_CHECK(Int3CountForPid(7112) == 1U);

            // a 经面板写两个补丁：必须记在 a 的目标名下，而不是账本当时的当前目标（b）名下。
            const std::uint64_t addressA2 = 0xB8ULL;
            ForceInt3Context(*b.view);   // 确保写入前账本当前目标确实是 b 的
            WPJ6_CHECK(JumpTo(*a.view, addressA));
            a.view->int3PanelForTest()->InstallButtonForTest()->click();
            ForceInt3Context(*b.view);
            WPJ6_CHECK(JumpTo(*a.view, addressA2));
            a.view->int3PanelForTest()->InstallButtonForTest()->click();
            WPJ6_CHECK_NOTE(Int3CountForPid(7111) == 2U, QStringLiteral("a 面板写入的两个补丁应属于 a 的目标"));
            WPJ6_CHECK_NOTE(Int3CountForPid(7112) == 1U, QStringLiteral("b 的补丁不应多出或丢失"));

            // 让 b 抢回当前目标，a 选中自己的 0xB0 那一行点"还原"：只能还原那一个。
            ForceInt3Context(*b.view);
            WPJ6_CHECK(Int3ContextPid() == 7112U);
            const auto& ledger = ks::ui::WorkbenchShared::Instance().Int3().Entries();
            int rowOfA = -1;
            for (std::size_t index = 0; index < ledger.size(); ++index)
            {
                if (ledger[index].pid == 7111U && ledger[index].address == addressA)
                {
                    rowOfA = static_cast<int>(index);
                }
            }
            WPJ6_CHECK(rowOfA >= 0);
            a.view->int3PanelForTest()->TableForTest()->selectRow(rowOfA);
            a.view->int3PanelForTest()->RestoreButtonForTest()->click();
            WPJ6_CHECK_NOTE(Int3CountForPid(7111) == 1U, QStringLiteral("a 的单条还原应只还原选中的那一条"));
            WPJ6_CHECK_NOTE(BackingByte(addressA) != 0xCC, QStringLiteral("a 选中的补丁应已还原"));
            WPJ6_CHECK(BackingByte(addressA2) == 0xCC);

            // 再让 b 抢回当前目标，a 点"全部还原"：只能还原 a 自己剩下的补丁。
            ForceInt3Context(*b.view);
            WPJ6_CHECK(Int3ContextPid() == 7112U);
            a.view->int3PanelForTest()->RestoreAllButtonForTest()->click();
            WPJ6_CHECK_NOTE(Int3CountForPid(7111) == 0U, QStringLiteral("a 的全部还原应还原 a 的补丁"));
            WPJ6_CHECK_NOTE(Int3CountForPid(7112) == 1U, QStringLiteral("a 的全部还原不得动 b 的补丁"));
            ClearInt3AndBytes({ addressA, addressA2, addressB });
        }

        // ---- 1) 视图被显示/窗口被激活时重新声明当前目标 ----
        void TestShowAndActivationReassertContext()
        {
            Harness a;
            a.AttachProcess(7121);
            Harness b;
            b.AttachProcess(7122);
            PumpUntil([]() { return true; }, 10);
            WPJ6_CHECK(Int3ContextPid() == 7122U);

            // 显示后**立刻**读、不泵事件：showEvent 是同步送达的；窗口激活事件要靠事件循环投递，
            // 先泵事件的话激活路径会把 showEvent 缺失的缺口遮住（两条路径必须各自被测到）。
            a.view->show();
            WPJ6_CHECK_NOTE(Int3ContextPid() == 7121U, QStringLiteral("a 被显示后（未泵事件）账本当前目标应是 a 的"));
            PumpFor(30);
            b.view->show();
            WPJ6_CHECK_NOTE(Int3ContextPid() == 7122U, QStringLiteral("b 被显示后（未泵事件）账本当前目标应是 b 的"));
            PumpFor(30);

            // 窗口激活：两个窗口都已显示，b 是上一次声明者；a 变成活动窗口时重新声明。
            b.view->activateWindow();
            PumpFor(60);
            a.view->activateWindow();
            PumpFor(60);
            WPJ6_CHECK_NOTE(Int3ContextPid() == 7121U, QStringLiteral("a 被激活后账本当前目标应是 a 的"));
            b.view->activateWindow();
            PumpFor(60);
            WPJ6_CHECK_NOTE(Int3ContextPid() == 7122U, QStringLiteral("b 被激活后账本当前目标应是 b 的"));
            a.view->hide();
            b.view->hide();
        }

        // ---- 1) 分离安全网：账本当前目标被别的视图改走之后，仍要还原自己的补丁 ----
        void TestDetachSafetyNetRestoresOwnPatches()
        {
            const std::uint64_t address = 0xB0ULL;
            Harness a;     // 相当于内嵌进程详情窗口里的视图
            a.AttachProcess(7131);
            PumpUntil([]() { return true; }, 10);
            WaitForStageable(a.view->hexPaneForTest(), address);
            ToggleInt3(*a.view, address);
            WPJ6_CHECK(Int3CountForPid(7131) == 1U);

            Harness b;     // 主 Dock 的视图附加了另一个进程：账本当前目标被改走
            b.AttachProcess(7132);
            PumpUntil([]() { return true; }, 10);
            WPJ6_CHECK(Int3ContextPid() == 7132U);

            // 内嵌窗口关闭 → Dock 析构 → detachProcess 的第一步：目标即将分离。
            a.view->target().onDockAboutToDetach();
            WPJ6_CHECK_NOTE(Int3CountForPid(7131) == 0U, QStringLiteral("分离安全网必须还原 a 自己的补丁"));
            WPJ6_CHECK(BackingByte(address) != 0xCC);
            ClearInt3AndBytes({ address });
        }

        // ---- 1) 离开询问：账本当前目标被别的视图改走之后，仍要问到自己的补丁 ----
        void TestLeaveSequenceSeesOwnPatches()
        {
            const std::uint64_t address = 0xB0ULL;
            Harness a;
            a.AttachProcess(7141);
            PumpUntil([]() { return true; }, 10);
            WaitForStageable(a.view->hexPaneForTest(), address);
            ToggleInt3(*a.view, address);
            Harness b;
            b.AttachProcess(7142);
            PumpUntil([]() { return true; }, 10);
            WPJ6_CHECK(Int3ContextPid() == 7142U);

            ArmClicker clicker(QStringLiteral("全部还原后继续"));
            const bool allowed = a.view->confirmQuit();
            WPJ6_CHECK_NOTE(clicker.clicked, QStringLiteral("退出时必须问到 a 自己未还原的 int3 补丁"));
            WPJ6_CHECK(allowed);
            WPJ6_CHECK(Int3CountForPid(7141) == 0U);
            ClearInt3AndBytes({ address });
        }

        // ---- 2) focusAddress ----
        void TestFocusAddress()
        {
            Harness h;
            WPJ6_CHECK_NOTE(!h.view->focusAddress().has_value(), QStringLiteral("未附加目标：没有插入点可报"));
            h.AttachProcess(7151);
            PumpUntil([]() { return true; }, 10);
            // 刚附加、用户还没有定位：插入点是地址空间起点 0，不能把它当成"用户正看着地址 0"报出去。
            WPJ6_CHECK_NOTE(
                !h.view->focusAddress().has_value(),
                QStringLiteral("刚附加时插入点为 0，应报告没有焦点地址，实际 %1").arg(h.view->focusAddress().value_or(0)));
            WaitForStageable(h.view->hexPaneForTest(), 0x40ULL);
            WPJ6_CHECK(JumpTo(*h.view, 0x40ULL));
            WPJ6_CHECK(h.view->focusAddress().has_value());
            WPJ6_CHECK(h.view->focusAddress().value_or(0) == 0x40ULL);
            WPJ6_CHECK(JumpTo(*h.view, 0x80ULL));
            WPJ6_CHECK_NOTE(h.view->focusAddress().value_or(0) == 0x80ULL, QStringLiteral("跳转后插入点应跟着走"));
        }

        // ---- 3) 内嵌模式拒绝钉住请求 ----
        void TestEmbeddedRefusesPinRequest()
        {
            Harness h;
            h.AttachProcess(7161);
            PumpUntil([]() { return true; }, 10);
            h.view->setEmbeddedProcessMode(true);
            ks::ui::NavRequest request;
            request.pid = 7162;           // 模块表预览了另一个进程，要求钉住它
            request.address = 0x40ULL;
            const auto status = h.view->openAt(request);
            WPJ6_CHECK_NOTE(status == ks::ui::NavStatus::Unavailable, QStringLiteral("内嵌模式必须拒绝钉住请求"));
            WPJ6_CHECK_NOTE(h.view->target().session().pid == 7161U, QStringLiteral("拒绝后会话仍是 Dock 附加的进程"));
            // 同一进程（pid 留空）的跳转不受影响。
            request.pid = 0;
            WaitForStageable(h.view->hexPaneForTest(), 0x40ULL);
            WPJ6_CHECK(h.view->openAt(request) == ks::ui::NavStatus::Ok);
        }

        // ---- 4) 搜索结果加入地址簿 ----
        ksword::memwb::AddressFilter FilterForKey(const std::string& key)
        {
            ksword::memwb::AddressFilter filter;
            filter.targetKey = key;
            return filter;
        }

        void TestIntakeRulesAndTargetKey()
        {
            namespace intake = ks::ui::workbench_intake;

            // 目标键必须与视图生成的逐字相同（否则加入的条目在工作台按目标过滤时不会出现）。
            ksword::memwb::MemoryTargetSession session;
            session.scope = ksword::memwb::Scope::ProcessVirtual;
            session.pid = 4242;
            session.processCreateTime100ns = 133000000000000000ULL;
            WPJ6_CHECK(intake::BuildProcessTargetKey(4242, 133000000000000000ULL)
                == ks::ui::detail::BuildAddressBookTargetKey(session));
            WPJ6_CHECK(intake::BuildProcessTargetKey(4242, 133000000000000000ULL) == "pid:4242@133000000000000000");
            WPJ6_CHECK(intake::BuildProcessTargetKey(4242, 1) != intake::BuildProcessTargetKey(4243, 1));
            WPJ6_CHECK(intake::kAddressBookCap == 10000U);

            ks::ui::AddressBookStore store{ QString() };
            const std::string key = intake::BuildProcessTargetKey(4242, 7);

            // 首次加入：全部新增，种类=搜索结果，绝对地址，值类型按传入。
            auto result = intake::AddAddressesToBook(
                store, key, { 0x1000, 0x2000, 0x3000 }, ksword::memwb::ValueType::I32);
            WPJ6_CHECK(result.requested == 3U && result.added == 3U);
            WPJ6_CHECK(result.duplicates == 0U && result.refusedByCap == 0U);
            const auto listed = store.list(FilterForKey(key));
            WPJ6_CHECK(listed.size() == 3U);
            for (const auto& entry : listed)
            {
                WPJ6_CHECK(entry.kind == ksword::memwb::EntryKind::Search);
                WPJ6_CHECK(entry.valueType == ksword::memwb::ValueType::I32);
                WPJ6_CHECK(entry.moduleName.empty());
                WPJ6_CHECK(entry.targetKey == key);
            }
            WPJ6_CHECK(listed[0].absoluteAddress == 0x1000U && listed[2].absoluteAddress == 0x3000U);

            // 再次加入：已有的与批内重复的都算重复，只新增真正新的。
            result = intake::AddAddressesToBook(
                store, key, { 0x2000, 0x4000, 0x4000, 0x1000 }, ksword::memwb::ValueType::I32);
            WPJ6_CHECK_NOTE(result.added == 1U, QStringLiteral("只有 0x4000 是新的，实际新增 %1").arg(result.added));
            WPJ6_CHECK(result.duplicates == 3U);
            WPJ6_CHECK(store.size() == 4U);

            // 同一地址换一个目标不算重复。
            result = intake::AddAddressesToBook(
                store, intake::BuildProcessTargetKey(4243, 7), { 0x1000 }, ksword::memwb::ValueType::Hex8);
            WPJ6_CHECK(result.added == 1U && result.duplicates == 0U);
            WPJ6_CHECK(store.size() == 5U);

            // 上限：总数已 5，上限 7 → 只能再加 2 条，其余拒绝（顺序即新增顺序）。
            result = intake::AddAddressesToBook(
                store, key, { 0x5000, 0x6000, 0x7000, 0x8000 }, ksword::memwb::ValueType::I32, 7);
            WPJ6_CHECK(result.added == 2U);
            WPJ6_CHECK(result.refusedByCap == 2U);
            WPJ6_CHECK(store.size() == 7U);
            WPJ6_CHECK(store.list(FilterForKey(key)).back().absoluteAddress == 0x6000U);

            // 已满：再加全部被拒，不新增；重复地址仍按重复计，不算被拒。
            result = intake::AddAddressesToBook(
                store, key, { 0x9000, 0x1000 }, ksword::memwb::ValueType::I32, 7);
            WPJ6_CHECK(result.added == 0U && result.refusedByCap == 1U && result.duplicates == 1U);
            WPJ6_CHECK(store.size() == 7U);

            // 带模块名的条目没有固定绝对地址：不参与去重，也不能让整批失败。
            ks::ui::AddressBookStore moduleStore{ QString() };
            const std::string moduleKey = intake::BuildProcessTargetKey(5000, 9);
            const auto moduleDraft = ksword::memwb::MemoryAddressBook::FromAbsolute(
                moduleKey, ksword::memwb::EntryKind::Bookmark, 0x1100,
                ksword::memwb::ModuleLocation{ "demo.dll", 0x1000 });
            WPJ6_CHECK(moduleDraft.has_value());
            WPJ6_CHECK(moduleStore.add(*moduleDraft) != 0U);
            result = intake::AddAddressesToBook(
                moduleStore, moduleKey, { 0x1100 }, ksword::memwb::ValueType::U8);
            WPJ6_CHECK_NOTE(result.added == 1U, QStringLiteral("模块+RVA 条目解析不出固定地址，不应挡住新增"));
        }

        void TestIntakeDefaultCapIsTenThousand()
        {
            namespace intake = ks::ui::workbench_intake;
            ks::ui::AddressBookStore store{ QString() };
            const std::string key = intake::BuildProcessTargetKey(6000, 1);
            std::vector<std::uint64_t> addresses;
            addresses.reserve(10001U);
            for (std::uint64_t i = 0; i < 10001U; ++i)
            {
                addresses.push_back(0x10000ULL + i * 4U);
            }
            const auto result = intake::AddAddressesToBook(store, key, addresses, ksword::memwb::ValueType::U32);
            WPJ6_CHECK_NOTE(result.added == 10000U, QStringLiteral("默认上限应是 10000，实际新增 %1").arg(result.added));
            WPJ6_CHECK(result.refusedByCap == 1U);
            WPJ6_CHECK(store.size() == 10000U);
        }
    }

    void RunEntry3bTests()
    {
        TestContextMenuActsOnOwnTarget();
        TestPanelButtonsActOnOwnTarget();
        TestShowAndActivationReassertContext();
        TestDetachSafetyNetRestoresOwnPatches();
        TestLeaveSequenceSeesOwnPatches();
        TestFocusAddress();
        TestEmbeddedRefusesPinRequest();
        TestIntakeRulesAndTargetKey();
        TestIntakeDefaultCapIsTenThousand();
    }
}
