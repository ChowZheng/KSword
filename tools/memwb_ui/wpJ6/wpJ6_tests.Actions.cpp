// ============================================================
// wpJ6_tests.Actions.cpp
// 作用：Redo 主键与候补键触发同一次 redo()；析构安全（构造/销毁 200 轮）。
// ============================================================

#include "wpJ6_common.h"

#include <QKeySequence>
#include <QShortcut>

namespace wpj6_test
{
    namespace
    {
        // FindShortcutsByKey：在 host 的直接子对象里找出按键序列匹配的全部
        // QShortcut（wireActions 把 Redo 的主键与候补键各建一个 QShortcut，
        // 都挂在 this 上）。
        std::vector<QShortcut*> FindShortcutsByKey(QObject* host, const QKeySequence& key)
        {
            std::vector<QShortcut*> result;
            const auto children = host->findChildren<QShortcut*>();
            for (auto* shortcut : children)
            {
                if (shortcut->key() == key)
                {
                    result.push_back(shortcut);
                }
            }
            return result;
        }
    }

    // TestRedoMainAndAlternateKeyTriggerSameRedo：Ctrl+Y 与 Ctrl+Shift+Z 都应
    // 触发真正的 redo()，不是两个互不相关的槛。判据：先用立即写入模式真实提交
    // 一次编辑（journal 记一步），undo() 一次产生"可重做"状态，分别用主键/
    // 候补键触发 redo，核对两次都真的把 canRedo() 从 true 变回 false（撤销历史
    // 的游标真的前进了一步，不是空操作）。
    void TestRedoMainAndAlternateKeyTriggerSameRedo()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* pane = harness.view->hexPaneForTest();
        auto* controller = harness.view->writeControllerForTest();
        WPJ6_CHECK(pane != nullptr && controller != nullptr);
        if (pane == nullptr || controller == nullptr)
        {
            return;
        }
        const bool settled = WaitForStageable(pane, 0x20ULL);
        WPJ6_CHECK(settled);

        // 立即写入模式（默认）：stageBytes 经 editStaged 同步触发一次完整提交，
        // 成功才会被 journal 记一步，canUndo() 随之变 true。
        WPJ6_CHECK(controller->mode() == ksword::memwb::WriteMode::Immediate);
        QString reason;
        const bool staged = pane->canvas()->stageBytes(0x20ULL, QByteArray(1, '\x01'), &reason);
        WPJ6_CHECK_NOTE(staged, reason);
        WPJ6_CHECK_NOTE(controller->canUndo(), QStringLiteral("提交应已记入撤销历史"));

        const auto mainKeys = FindShortcutsByKey(harness.view.get(), QKeySequence(QStringLiteral("Ctrl+Y")));
        const auto altKeys = FindShortcutsByKey(harness.view.get(), QKeySequence(QStringLiteral("Ctrl+Shift+Z")));
        WPJ6_CHECK_NOTE(!mainKeys.empty(), QStringLiteral("未找到 Redo 主键 Ctrl+Y"));
        WPJ6_CHECK_NOTE(!altKeys.empty(), QStringLiteral("未找到 Redo 候补键 Ctrl+Shift+Z"));

        // 主键：先 undo() 产生可重做状态，触发主键快捷键，核对 canRedo() 真的
        // 被消费（变回 false）且 canUndo() 恢复为真。
        controller->undo();
        WPJ6_CHECK(controller->canRedo());
        for (auto* shortcut : mainKeys)
        {
            emit shortcut->activated();
        }
        const bool mainConsumedRedo = !controller->canRedo() && controller->canUndo();
        WPJ6_CHECK_NOTE(mainConsumedRedo, QStringLiteral("主键 Ctrl+Y 未能触发真正的 redo()"));

        // 候补键：同样的流程，核对行为完全一致。
        controller->undo();
        WPJ6_CHECK(controller->canRedo());
        for (auto* shortcut : altKeys)
        {
            emit shortcut->activated();
        }
        const bool altConsumedRedo = !controller->canRedo() && controller->canUndo();
        WPJ6_CHECK_NOTE(altConsumedRedo, QStringLiteral("候补键 Ctrl+Shift+Z 未能触发真正的 redo()"));
    }

    // TestDestructorSafety200Rounds：反复构造/销毁 Harness（含其内部完整的
    // MemoryWorkbenchView 装配），覆盖"任意销毁顺序"的隐含要求——每一轮都是
    // 一次全新的构造+析构，不依赖上一轮遗留的任何状态。
    void TestDestructorSafety200Rounds()
    {
        bool allOk = true;
        for (int round = 0; round < 200 && allOk; ++round)
        {
            Harness harness;
            if (round % 3 == 0)
            {
                harness.AttachProcess(1000U + static_cast<std::uint32_t>(round), static_cast<std::uint64_t>(round) + 1U);
            }
            if (round % 5 == 0)
            {
                harness.view->setEmbeddedProcessMode(true);
            }
            PumpFor(1);
            // Harness 在这里离开作用域，触发完整析构；WPJ6_CHECK 本身只在最后
            // 统一断言一次，避免 200 条几乎相同的日志行淹没真正的失败信息。
        }
        WPJ6_CHECK_NOTE(allOk, QStringLiteral("200 轮构造/销毁未发生崩溃（能跑到这里本身就是判据）"));
    }

    void RunActionsTests()
    {
        TestRedoMainAndAlternateKeyTriggerSameRedo();
        TestDestructorSafety200Rounds();
    }
}
