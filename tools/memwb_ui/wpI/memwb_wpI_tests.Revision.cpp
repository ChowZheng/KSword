// memwb_wpI_tests.Revision.cpp
// 作用：验证 WorkbenchTarget 的两个代次计数器（来源代次/内容代次）相关行为——
//       isStale() 对两者任一变化都判陈旧（M09：旧夹具从没写过一条 isStale 用例，
//       这条断言本身就是在锁定 D2 之外、isStale 最基本的契约）；noteContentChanged
//       只 bump 内容代次（M10）；requestReload 恒 bump 来源代次、恒成功、不经过
//       离开守卫（M11）；以及 D2 本身——DDMA 换代后，即便调用方从未调用过
//       session()，只调用 isStale() 也必须能感知到陈旧。
// 对应审核报告 §5 T4，断言原样并入（最后一条在修复前会失败，用来锁定 D2）。

#include "memwb_wpI_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"

#include <memory>

namespace memwb_wpI_test
{
    void RunRevisionTests()
    {
        // ---- T4 前半：isStale 对来源代次/内容代次任一变化都判陈旧（M09 M10 M11）----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* fake = owned.get();
            fake->SetDdmaGeneration(100);
            ks::ui::WorkbenchTarget target(std::move(owned));

            // 刚 capture 完：不应该是陈旧的（两个代次都还没变过）。
            ks::ui::TargetCapture cap = target.capture();
            WPI_CHECK(!target.isStale(cap.rev));

            // noteContentChanged 只 bump 内容代次：isStale 应该因此判陈旧（M10：
            // 如果变异让它不再 bump，这里会从陈旧变回新鲜）。
            target.noteContentChanged();
            WPI_CHECK(target.isStale(cap.rev));

            // 重新 capture 一次作为新基线，再验证 requestReload 恒 bump 来源代次
            // （M11：如果变异只回 Reload 位但不真的 bump，这里也会从陈旧变回新鲜），
            // 且不经过离开守卫（没有注册守卫，也没有调用 setLeaveGuard，若
            // requestReload 内部误问了守卫，本函数会在无守卫时走"放行"分支，
            // 这里主要锁的是"bump"这件事本身，守卫豁免由 target.md 契约保证）。
            cap = target.capture();
            quint32 mask = 0;
            int sessionChangedCount = 0;
            QObject::connect(&target, &ks::ui::WorkbenchTarget::sessionChanged,
                [&](quint32 m) { ++sessionChangedCount; mask = m; });
            target.requestReload();
            WPI_CHECK(sessionChangedCount == 1);
            WPI_CHECK(mask == static_cast<quint32>(ksword::memwb::TargetChange::Reload));
            WPI_CHECK(target.isStale(cap.rev));
        }

        // ---- T4 后半（锁定 D2）：DDMA 换代后，哪怕从未调用过 session()，只调用
        // isStale() 也必须判陈旧 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* fake = owned.get();
            fake->SetDdmaGeneration(100);
            ks::ui::WorkbenchTarget target(std::move(owned));

            GuardRecorder guard;
            guard.SetApproval(true);
            target.setLeaveGuard(guard.asFunction());
            WPI_CHECK(target.requestChannel(ksword::memwb::Channel::Ddma));

            const ks::ui::TargetCapture cap = target.capture(); // 此刻代次=100，已经通过 capture 落过一次。

            // 模拟另一个 Dock 实例把暂存扇区换代到 101，期间调用方只调用
            // isStale()，从未再调用过 session()/capture()。
            fake->SetDdmaGeneration(101);

            // D2 修复前：isStale 是 const，只比对 tracker 内部已有的代次，不会
            // 主动拉取——会错误地判"未陈旧"。修复后：isStale 内部先拉一次最新
            // DDMA 代次喂给 tracker，能正确判陈旧。
            WPI_CHECK_NOTE(
                target.isStale(cap.rev),
                QStringLiteral("DDMA 代次变化后，只调用 isStale()（不调用 session()）也必须判陈旧"));
        }
    }
}
