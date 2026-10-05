#pragma once

// ============================================================
// memwb_wpI_common.h
// 作用：WP-I（WorkbenchTarget / 目标接入）离屏验证夹具的公共设施——轻量断言计数、
//       可脚本化的假 IWorkbenchServices（FakeWorkbenchServices）、离开守卫录像机
//       （GuardRecorder）与"等到条件成立"的事件泵（PumpUntil）。
// 说明：
// - 本目录（tools/memwb_ui/wpI/）是 WP-I 专属夹具目录，不与 tools/memwb_ui/ 下
//   既有的 HexCanvas 夹具共用产物目录、不链接其 memwb_ui_common.cpp（那份拖着
//   整条 HexCanvas+Qt Widgets 的依赖链，WP-I 不需要）。
// - 命名空间 memwb_wpI_test，与既有夹具的 memwb_test 区分，避免任何名字混淆。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchServices.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"
#include "../../../shared/evidence/memory_workbench/MemoryProcessMatch.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace memwb_wpI_test
{
    // ---- 断言计数：与 tools/memwb_ui 既有夹具同样的轻量框架，独立计数器。----
    extern int g_checks;
    extern int g_failures;

    // Report：记录一条断言结果，失败时向 stderr 打印位置与表达式。
    void Report(bool ok, const char* expression, const char* file, int line, const QString& note);

#define WPI_CHECK(expression) \
    ::memwb_wpI_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, QString())
#define WPI_CHECK_NOTE(expression, note) \
    ::memwb_wpI_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (note))

    // PumpUntil：反复处理一次 Qt 事件循环直到 predicate 为真或超时。
    // 用途：WorkbenchTarget 的模块枚举经 QThreadPool 异步完成，测试需要等待
    // QMetaObject::invokeMethod 排队的回调真正落到 UI 线程。
    // 传入：predicate 判定是否已达成；timeoutMs 最长等待毫秒数。
    // 传出：达成返回 true；超时仍未达成返回 false（调用方应据此判失败，不要
    //       把超时静默当成功）。
    bool PumpUntil(const std::function<bool()>& predicate, int timeoutMs);

    // GuardRecorder：可控的离开守卫——记录每次被问到的理由，返回值由测试预设。
    class GuardRecorder
    {
    public:
        // asFunction：转成 WorkbenchTarget::setLeaveGuard 需要的 std::function。
        std::function<bool(ks::ui::LeaveReason)> asFunction();

        // approve：设置下一次（及之后，直到再次调用本函数）被问到时返回的值。
        void SetApproval(bool approve);

        // callCount：累计被问到的次数。
        int CallCount() const;

        // reasons：按顺序记录的每次调用的理由（LeaveReason 转成 int）。
        const std::vector<int>& Reasons() const;

        // Reset：清空计数与记录，不改变当前的 approve 设置。
        void Reset();

    private:
        bool approve_ = true;
        int callCount_ = 0;
        std::vector<int> reasons_;
    };

    // FakeWorkbenchServices：IWorkbenchServices 的完全可脚本化假实现。
    // 线程安全：enumerateProcessModules/enumerateKernelModules 会被 WorkbenchTarget
    // 从 QThreadPool 的工作线程调用，本类用一把 mutex 保护全部可配置状态与计数器。
    class FakeWorkbenchServices final : public ks::ui::IWorkbenchServices
    {
    public:
        // ---- 配置：进程模块枚举 ----
        // 传入：pid 目标进程号；result 该 pid 被枚举时应返回的结果。
        void SetProcessModulesResult(std::uint32_t pid, ks::ui::ModuleEnumResult result);
        // 传入：pid 目标进程号；delayMs 该 pid 被枚举时在工作线程上先睡眠的毫秒数
        //       （用于构造"先发起的枚举反而后完成"的陈旧票据场景）。
        void SetProcessEnumDelayMs(std::uint32_t pid, int delayMs);
        int ProcessEnumCallCount() const;
        // LastExpectCreateTime（T6 M24 新增）：最近一次 enumerateProcessModules
        // 被调用时收到的 expectCreateTime 实参；旧版假实现直接把这个参数丢在
        // 函数签名里不记录，M24（期望创建时间恒传 0）这个变异因此抓不到——补上
        // 记录才能断言"WorkbenchTarget 确实把会话里已锚定的创建时间一路传了
        // 下去，不是随手传了个 0"。
        std::uint64_t LastExpectCreateTime() const;

        // ---- 配置：内核模块枚举 ----
        void SetKernelModulesResult(ks::ui::ModuleEnumResult result);
        void SetKernelEnumDelayMs(int delayMs);
        int KernelEnumCallCount() const;

        // ---- 配置：候选进程 ----
        void SetProcessCandidates(std::vector<ksword::memwb::ProcessCandidate> candidates);

        // ---- 配置：指针读取 ----
        void SetPointerReadResult(std::uint64_t address, ks::ui::PointerReadResult result);
        int ReadPointerCallCount() const;

        // ---- 配置：DDMA 代次 ----
        void SetDdmaGeneration(std::uint64_t generation);
        int DdmaGenerationCallCount() const;

        // ---- IWorkbenchServices ----
        ks::ui::ModuleEnumResult enumerateProcessModules(std::uint32_t pid, std::uint64_t expectCreateTime) override;
        ks::ui::ModuleEnumResult enumerateKernelModules() override;
        std::vector<ksword::memwb::ProcessCandidate> processCandidates() override;
        ks::ui::PointerReadResult readPointer(
            const ksword::memwb::MemoryTargetSession& session,
            std::uint64_t address,
            std::uint32_t width) override;
        std::uint64_t ddmaGeneration() override;

    private:
        mutable std::mutex mutex_;
        std::map<std::uint32_t, ks::ui::ModuleEnumResult> processResults_;
        std::map<std::uint32_t, int> processDelaysMs_;
        int processEnumCallCount_ = 0;
        // lastExpectCreateTime_：见 LastExpectCreateTime() 的用途说明。
        std::uint64_t lastExpectCreateTime_ = 0;

        ks::ui::ModuleEnumResult kernelResult_;
        int kernelDelayMs_ = 0;
        int kernelEnumCallCount_ = 0;

        std::vector<ksword::memwb::ProcessCandidate> candidates_;

        std::map<std::uint64_t, ks::ui::PointerReadResult> pointerResults_;
        int readPointerCallCount_ = 0;

        std::uint64_t ddmaGeneration_ = 0;
        int ddmaGenerationCallCount_ = 0;
    };

    // AttachFakeProcess：模拟 Dock 附加了一个"假"进程——传入任意 pid（**不要求
    // 真实存在**），但附带一个真实有效的句柄（本进程的伪句柄 GetCurrentProcess()）
    // 用于锚定。专供"只关心模块目录/求值逻辑对某个 pid 的反应，不关心锚点/存活/
    // 离开守卫语义"的测试场景使用——这类场景历史上直接调用 requestPin(假pid)；
    // D6 修复给 requestPin 加了"目标是否真的存在"的前置校验（AcquireAnchorForPid
    // 对不存在的 pid 会返回 targetGone），假 pid 会被这条校验直接拒绝，不再能用
    // requestPin 搭场景。onDockAttached 走 FollowAttach，不经过这条校验（它接收
    // 的是 Dock 已经持有的句柄，不需要再问系统"这个 pid 存不存在"），所以改用它。
    // 需要真实测试锚点/存活/pin-守卫语义的场景必须使用真实拉起的子进程
    // （见 memwb_wpI_tests.HandleLifecycle.cpp），不能用这个函数。
    // 传入：target 目标对象；fakePid 要让会话看到的 pid（可以不对应任何真实
    //       进程）；attachGeneration Dock 的附加代次（必须单调，重复值会被
    //       tracker 当成"没有新信息"）。
    void AttachFakeProcess(
        ks::ui::WorkbenchTarget& target, std::uint32_t fakePid, std::uint64_t attachGeneration);

    // 各组测试入口（定义在对应的 memwb_wpI_tests.*.cpp，由 main.cpp 依次调用）。
    void RunHooksTests();
    void RunGuardTests();
    void RunDdmaTests();
    void RunRevisionTests();
    void RunModulesTests();
    void RunEvaluateTests();
    void RunAnchorTests();
    void RunHandleLifecycleTests();
    void RunStressTests();
    void RunAccessorTests();
    // RunExtraTests：第二轮审核报告 review2-wpI.md 的补测原文（76 checks，
    // 小节 X1-X20），定义在 memwb_wpI_tests.Extra.cpp。
    void RunExtraTests();
    // RunFix2Tests：本轮修复新增的 N1-N5 回归 + revisions() 访问器 + 按 N4
    // 决策改写的 X18，定义在 memwb_wpI_tests.Fix2.cpp。
    void RunFix2Tests();
}
