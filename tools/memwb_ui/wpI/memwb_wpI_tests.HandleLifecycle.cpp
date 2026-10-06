// memwb_wpI_tests.HandleLifecycle.cpp
// 作用：验证 WorkbenchTarget 与锚点相关的句柄生命周期、位数接线、钉住存活探测
//       （审核报告 §5 T7，对应变异 M06 M07 M08 M17 M18 M19 M26 M41 M42）。
// 这里全部使用**真实拉起的子进程**（不是编号凑出来的假 pid）：D6 修复后
// requestPin 会先验证目标是否真的存在，假 pid 在这类测试里会被直接拒绝，
// 必须有一个真正的 Win32 进程对象才能走到锚点/存活/句柄计数这些语义。

#include "memwb_wpI_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace memwb_wpI_test
{
    namespace
    {
        // SpawnChild：拉起一条命令行，成功时把 PROCESS_INFORMATION 写入 piOut
        // （调用方负责最终关闭 hProcess/hThread）。
        // 传入：commandLine 完整命令行（可写缓冲区由本函数自己拼）。
        // 传出：成功返回 true；失败（找不到可执行文件等）返回 false。
        bool SpawnChild(const wchar_t* commandLine, PROCESS_INFORMATION& piOut)
        {
            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            std::vector<wchar_t> mutableCommandLine(commandLine, commandLine + wcslen(commandLine) + 1);
            return ::CreateProcessW(
                nullptr, mutableCommandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startupInfo, &piOut) != FALSE;
        }
    }

    void RunHandleLifecycleTests()
    {
        // ---- T7（锁定 M17 M19）：32 位目标的位数要如实接线到 AnchorInfo 与会话 ----
        // SysWOW64 下的 cmd.exe 是 32 位进程；只在 64 位 Windows 上才存在这个目录，
        // 32 位系统或裁剪版系统上可能没有，找不到就跳过，不计入失败。
        {
            PROCESS_INFORMATION processInfo{};
            if (SpawnChild(L"C:\\Windows\\SysWOW64\\cmd.exe /c ping -n 10 127.0.0.1", processInfo))
            {
                const ks::ui::AnchorInfo info = ks::ui::AcquireAnchorForPid(processInfo.dwProcessId);
                WPI_CHECK_NOTE(info.addressBits == 32, QStringLiteral("M17：32 位目标的锚点位数接线错误"));
                ks::ui::ReleaseAnchorHandle(info.handle);

                auto owned = std::make_unique<FakeWorkbenchServices>();
                ks::ui::WorkbenchTarget target(std::move(owned));
                ks::ui::WorkbenchTarget::DockAttach attach;
                attach.handle = processInfo.hProcess;
                attach.pid = processInfo.dwProcessId;
                attach.attachGeneration = 1;
                target.onDockAttached(attach);
                WPI_CHECK_NOTE(
                    target.session().addressBits == 32,
                    QStringLiteral("M19：onDockAttached 的位数接线错误"));
                target.onDockDetached();

                ::TerminateProcess(processInfo.hProcess, 0);
                ::CloseHandle(processInfo.hProcess);
                ::CloseHandle(processInfo.hThread);
            }
            else
            {
                // SysWOW64\cmd.exe 在本机不可用（32 位系统或裁剪版系统）：跳过这条，
                // 不计入失败——32 位目标本身不是本包的主要场景，找不到才跳过。
                std::cout << "wpI handle-lifecycle: SysWOW64\\cmd.exe 不可用，跳过 32 位目标测试"
                          << std::endl;
            }
        }

        // ---- T7（锁定 M18 M26 M41 M42）：钉住带创建时间、存活探测、解除钉住释放句柄 ----
        {
            PROCESS_INFORMATION processInfo{};
            if (SpawnChild(L"cmd.exe /c ping -n 30 127.0.0.1", processInfo))
            {
                auto owned = std::make_unique<FakeWorkbenchServices>();
                ks::ui::WorkbenchTarget target(std::move(owned));
                int lastLivenessState = -1;
                QObject::connect(&target, &ks::ui::WorkbenchTarget::livenessChanged,
                    [&](int state) { lastLivenessState = state; });

                DWORD handlesBeforePin = 0;
                DWORD handlesAfterPin = 0;
                DWORD handlesAfterUnpin = 0;
                ::GetProcessHandleCount(::GetCurrentProcess(), &handlesBeforePin);

                WPI_CHECK(target.requestPin(processInfo.dwProcessId));
                WPI_CHECK_NOTE(
                    target.session().processCreateTime100ns != 0,
                    QStringLiteral("M18：钉住真实存在的进程后创建时间不应该是 0"));
                ::GetProcessHandleCount(::GetCurrentProcess(), &handlesAfterPin);
                WPI_CHECK_NOTE(
                    handlesAfterPin > handlesBeforePin,
                    QStringLiteral("钉住应该持有一个新句柄，句柄计数理应增加"));

                target.checkLiveness();
                WPI_CHECK_NOTE(
                    lastLivenessState == static_cast<int>(ks::ui::LivenessState::Alive),
                    QStringLiteral("M41 M42：钉住的真实子进程存活时应探测为 Alive"));

                ::TerminateProcess(processInfo.hProcess, 1);
                ::WaitForSingleObject(processInfo.hProcess, 5000);
                target.checkLiveness();
                WPI_CHECK_NOTE(
                    lastLivenessState == static_cast<int>(ks::ui::LivenessState::Exited),
                    QStringLiteral("子进程已终止，存活探测应变为 Exited"));

                WPI_CHECK(target.requestFollowDock()); // 回到跟随：必须释放钉住句柄（M26）。
                ::GetProcessHandleCount(::GetCurrentProcess(), &handlesAfterUnpin);
                WPI_CHECK_NOTE(
                    handlesAfterUnpin < handlesAfterPin,
                    QStringLiteral("M26：解除钉住后应该释放钉住句柄，句柄计数理应回落"));

                ::CloseHandle(processInfo.hProcess);
                ::CloseHandle(processInfo.hThread);
            }
            else
            {
                WPI_CHECK_NOTE(false, QStringLiteral("T7 需要能够拉起一个子进程（cmd.exe）"));
            }
        }

        // ---- T7（锁定 M06）：Dock 反复重附加同一个进程，句柄不应该泄漏 ----
        // 判据必须留足够大的余量：每次 onDockAttached 的 attachGeneration 都不同，
        // 按契约这算一次 Process 位变更，会同步触发一次异步模块枚举派去
        // QThreadPool；线程池自身为了跟上吞吐量可能临时多开几个工作线程（线程本身
        // 也是一个句柄），这部分"抖动"和真正的锚点句柄泄漏（每次 +1、规模等于
        // 迭代次数）完全不是一个量级——用大边界（迭代次数的一个小分数）而不是
        // "+2" 这种几乎必然被线程池噪声触发的死板边界。
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* fake = owned.get();
            const std::uint32_t selfPid = ::GetCurrentProcessId();
            fake->SetProcessModulesResult(selfPid, ks::ui::ModuleEnumResult{true, {}, {}});
            ks::ui::WorkbenchTarget target(std::move(owned));
            ks::ui::WorkbenchTarget::DockAttach attach;
            attach.handle = reinterpret_cast<void*>(::GetCurrentProcess());
            attach.pid = selfPid;

            constexpr int kIterations = 300;

            // 先预热：让线程池、Qt 内部缓存该创建的句柄都创建一遍，并在过程中多次
            // drain 事件队列，让它稳定下来，不计入下面的测量。
            for (int i = 0; i < 50; ++i)
            {
                attach.attachGeneration = 2 + static_cast<std::uint64_t>(i);
                target.onDockAttached(attach);
                if (i % 10 == 0)
                {
                    PumpUntil([] { return false; }, 20);
                }
            }
            PumpUntil([] { return false; }, 500);

            DWORD handlesBefore = 0;
            DWORD handlesAfter = 0;
            ::GetProcessHandleCount(::GetCurrentProcess(), &handlesBefore);
            for (int i = 0; i < kIterations; ++i)
            {
                attach.attachGeneration = 10000 + static_cast<std::uint64_t>(i);
                target.onDockAttached(attach);
                if (i % 10 == 0)
                {
                    PumpUntil([] { return false; }, 20); // 边跑边 drain，避免队列无限堆积。
                }
            }
            PumpUntil([] { return false; }, 500);
            ::GetProcessHandleCount(::GetCurrentProcess(), &handlesAfter);
            // 真正的泄漏（M06 的变异：换目标前不释放旧锚点）每次 +1，300 次迭代
            // 会让句柄数净增约 300；这里用迭代次数的四分之一（75）做上限，远大于
            // 线程池/Qt 内部状态的正常抖动，又远小于真实泄漏的规模，两者不会混淆。
            WPI_CHECK_NOTE(
                handlesAfter <= handlesBefore + kIterations / 4,
                QStringLiteral("M06：反复重附加同一个进程不应该泄漏句柄（前 %1 → 后 %2，迭代 %3 次）")
                    .arg(handlesBefore).arg(handlesAfter).arg(kIterations));
        }

        // ---- T7（锁定 M07）：分离后旧句柄值不应该在析构时被二次关闭 ----
        {
            HANDLE probe = nullptr;
            {
                auto owned = std::make_unique<FakeWorkbenchServices>();
                ks::ui::WorkbenchTarget target(std::move(owned));
                ks::ui::WorkbenchTarget::DockAttach attach;
                attach.handle = reinterpret_cast<void*>(::GetCurrentProcess());
                attach.pid = ::GetCurrentProcessId();
                attach.attachGeneration = 1;
                target.onDockAttached(attach);
                target.onDockDetached(); // 分离：dockAnchorHandle_ 的句柄值此刻已经被关闭、归还给系统。
                // 极可能复用刚释放的句柄值——如果 onDockDetached 之后析构时又把这个
                // 值当成自己的句柄关掉一次（M07 的双重关闭），就会把下面这个事件
                // 对象的句柄关掉。
                probe = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            } // target 在这里析构。
            WPI_CHECK_NOTE(
                probe != nullptr && ::SetEvent(probe) != FALSE,
                QStringLiteral("M07：分离后的句柄值被析构函数二次关闭，殃及了被复用的 probe 句柄"));
            if (probe != nullptr)
            {
                ::CloseHandle(probe);
            }
        }

        // ---- T7（锁定 M08）："句柄仍有效"的真证明：用真实子进程当 Dock 句柄，
        // 在 aboutToDetach 的槅里再探测一次存活状态 ----
        {
            PROCESS_INFORMATION processInfo{};
            if (SpawnChild(L"cmd.exe /c ping -n 30 127.0.0.1", processInfo))
            {
                auto owned = std::make_unique<FakeWorkbenchServices>();
                ks::ui::WorkbenchTarget target(std::move(owned));
                int liveEmits = 0;
                QObject::connect(&target, &ks::ui::WorkbenchTarget::livenessChanged,
                    [&](int) { ++liveEmits; });

                ks::ui::WorkbenchTarget::DockAttach attach;
                attach.handle = processInfo.hProcess;
                attach.pid = processInfo.dwProcessId;
                attach.attachGeneration = 1;
                target.onDockAttached(attach);
                target.checkLiveness(); // 首次 Alive，liveEmits==1。
                WPI_CHECK(liveEmits == 1);

                bool stillAliveInsideSlot = false;
                QObject::connect(&target, &ks::ui::WorkbenchTarget::aboutToDetach, [&]()
                    {
                        // 真正的证明：在 aboutToDetach 槅内部再查一次存活状态——如果
                        // 这里 Dock 的句柄已经失效，checkLiveness 会把状态探测成
                        // Exited（liveEmits 变化），stillAliveInsideSlot 就会是 false。
                        target.checkLiveness();
                        stillAliveInsideSlot = (liveEmits == 1);
                    });
                target.onDockAboutToDetach();
                WPI_CHECK_NOTE(
                    stillAliveInsideSlot,
                    QStringLiteral("M08：aboutToDetach 发出时 Dock 句柄必须仍然有效"));

                target.onDockDetached();
                ::TerminateProcess(processInfo.hProcess, 0);
                ::CloseHandle(processInfo.hProcess);
                ::CloseHandle(processInfo.hThread);
            }
            else
            {
                WPI_CHECK_NOTE(false, QStringLiteral("T7(M08) 需要能够拉起一个子进程（cmd.exe）"));
            }
        }
    }
}
