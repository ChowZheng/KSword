#pragma once

// ============================================================
// wpF_tests_common.h
// 作用：WP-F（int3 补丁）离屏验证夹具的公共设施——断言计数、测试数据构造、
//       驱动"模态 QMessageBox / 弹出式 QMenu"这两类阻塞对话框的测试辅助、主题切换。
// 本文件只属于本夹具目录，不是仓库共享文件。
// ============================================================

#include "../../../shared/evidence/memory_workbench/Int3PatchLedger.h"
#include "../../../shared/evidence/memory_workbench/MemoryTargetSession.h"

#include <QString>

namespace wpf_test
{
    // 断言计数：与主夹具（memwb_ui_common）同样的轻量框架，各自独立一份，互不干扰。
    extern int g_checks;
    extern int g_failures;

    // Report：记录一条断言结果，失败时向 stderr 打印位置与表达式。
    void Report(bool ok, const char* expression, const char* file, int line, const QString& note);

#define CHECK(expression) ::wpf_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, QString())
#define CHECK_NOTE(expression, note) \
    ::wpf_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (note))

    // MakeTarget：构造一个 PatchTarget，attachGeneration 默认 1（本夹具不关心这个字段）。
    ksword::memwb::PatchTarget MakeTarget(
        std::uint32_t pid,
        std::uint64_t processCreateTime100ns,
        std::uint64_t attachGeneration = 1);

    // CountOpenMessageBoxes：统计当前可见的顶层 QMessageBox 数量。Install / Restore /
    // RestoreAll / Discard / ClearOrphaned 这些"零摩擦"操作前后，这个数字必须不变——
    // 这是运行时证据，不是只凭代码审查断言"没有调用 QMessageBox"。
    int CountOpenMessageBoxes();

    // ClickModalButtonByText：找到当前的模态 QMessageBox（QApplication::activeModalWidget）
    // 并点击文字匹配的按钮。配合 QTimer::singleShot 在 exec() 进入的嵌套事件循环里调用。
    void ClickModalButtonByText(const QString& text);

    // PressEscapeOnModal：对当前模态对话框发送 Escape 键，验证"Esc 的结果=取消"。
    // 注意：默认按钮与 Esc 是分开设置的两件事（规格：默认按钮="全部还原后继续"，
    // Esc="取消"），不要把这两个断言混在一起写，也不要假设改了一个另一个会跟着变。
    void PressEscapeOnModal();

    // PressEnterOnModal：对当前模态对话框发送 Enter 键，驱动它的默认按钮。配合
    // QTimer::singleShot 在 exec() 的嵌套事件循环里调用，用来验证"默认按钮=全部还原
    // 后继续"（2026 年审核报告缺陷 2：曾经默认按钮是取消，与规格相反）。
    void PressEnterOnModal();

    // ActiveModalMessageBoxText：返回当前模态 QMessageBox 的文案；没有模态框、或模态框
    // 不是 QMessageBox（理论上不会发生，本夹具只用它弹 QMessageBox）时返回空串。
    // 调用方法：在 QTimer::singleShot 排的回调里调用，读取 exec() 嵌套事件循环里那一刻
    // 真实显示的文案——供断言文案里的具体数字（而不是只验证点了哪个按钮），
    // 2026 年第二轮审核报告"发现 1"：CountForCurrentTarget() 与弹框文案的"正数路径"
    // 此前从未被断言过具体数值。
    QString ActiveModalMessageBoxText();

    // ArmUnexpectedModalWatchdog：按 delayMs 排一个安全网定时器，供"这次调用本不应该
    // 弹出任何确认框"的断言使用。到时若真的出现了模态框（说明判据被改错了，错误地弹出
    // 了本不该弹的框），就按一次 Escape 把它关掉，让调用方的返回值 / CountOpenMessageBoxes
    // 断言能跑到并报出失败位置——而不是卡死在 exec() 的嵌套事件循环里出不来（审核报告
    // F-M8：此前这类回归只能靠"整个夹具跑超时"来暴露，不是一条可读的断言失败）。
    // 调用方法：在调用可能误弹框的操作之前调一次，正常路径下（没有弹框）这个定时器
    // 到期时发现 activeModalWidget()==nullptr，什么都不做。
    void ArmUnexpectedModalWatchdog(int delayMs);

    // TriggerPopupMenuActionByText：找到当前弹出的 QMenu（QApplication::activePopupWidget）
    // 并触发文字匹配的动作；找不到就把菜单关掉，避免测试卡死在 exec() 里。
    void TriggerPopupMenuActionByText(const QString& text);

    // ApplyThemeForShots：切换深浅主题并同步应用完整调色板，供截图使用。
    void ApplyThemeForShots(bool dark);

    // 各组测试入口（定义在对应的 .cpp）。
    void RunControllerTests();
    void RunLeavePromptTests();
    void RunPanelTests(const QString& shotsDir);
}
