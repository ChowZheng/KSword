// memwb_wpI_tests.Anchor.cpp
// 作用：
// 1) 锚点冒烟测试——对本进程真实调用 AcquireAnchorFromDockHandle/AcquireAnchorForPid/
//    QueryAnchorAlive/ReleaseAnchorHandle，交叉核对创建时间与位数与系统直接查询一致；
//    并覆盖空句柄/0 号 PID 的安全返回。
// 2) PID 复用探针（target.md 风险清单："持有句柄则 PID 不被复用"是 Windows 已知行为，
//    本仓库未实测"）：反复拉起并等待本可执行文件自身的一个"立即退出"子进程，持有
//    第一个子进程的句柄不关闭，再多拉起若干个，看新 PID 是否撞上被持有的那个。
//    只记读数、打印到 stdout，不做断言——不能让"系统这次刚好复用了/没复用" 影响
//    夹具的通过/失败。
// 3) D3 回归：受保护进程（audiodg.exe）钉住后身份不应该降级为 weak——本机若找不到
//    这个进程就跳过并在输出里明示（计入 skipped，不计入失败）。
// 4) D4 回归：退出码恰好是 259（STILL_ACTIVE 的数值）的已退出进程，QueryAnchorAlive
//    必须报告"已退出"，不能被当成哨兵值误判为"仍在运行"。

#include "memwb_wpI_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>

#include <iostream>
#include <string>
#include <vector>

namespace memwb_wpI_test
{
    namespace
    {
        // FileTimeToUint64Probe：测试自己独立做一份 FILETIME→uint64 转换，用来交叉
        // 核对 AcquireAnchorFromDockHandle 的结果，不能偷懒直接复用被测代码的实现。
        std::uint64_t FileTimeToUint64Probe(const FILETIME& fileTime) noexcept
        {
            return (static_cast<std::uint64_t>(fileTime.dwHighDateTime) << 32)
                | static_cast<std::uint64_t>(fileTime.dwLowDateTime);
        }

        // SpawnSelfAndWait：拉起本可执行文件自身，带 --wpi-exit-now 参数（main.cpp
        // 看到这个参数会在构造 QCoreApplication 之前立即退出），等它跑完但不关闭句柄。
        // 传出：成功时把子进程 PID 与（仍然打开的）句柄写入 pidOut/handleOut。
        bool SpawnSelfAndWait(const std::wstring& exePath, DWORD& pidOut, HANDLE& handleOut)
        {
            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            PROCESS_INFORMATION processInfo{};

            std::wstring commandLine = L"\"" + exePath + L"\" --wpi-exit-now";
            std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
            mutableCommandLine.push_back(L'\0');

            const BOOL created = ::CreateProcessW(
                exePath.c_str(),
                mutableCommandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startupInfo,
                &processInfo);
            if (created == FALSE)
            {
                return false;
            }

            ::WaitForSingleObject(processInfo.hProcess, 5000);
            ::CloseHandle(processInfo.hThread);
            pidOut = processInfo.dwProcessId;
            handleOut = processInfo.hProcess; // 故意不关闭：调用方决定何时关闭。
            return true;
        }

        // RunPidReuseProbe：见文件顶部第 2) 条说明。
        void RunPidReuseProbe()
        {
            wchar_t exePathBuffer[MAX_PATH] = {};
            const DWORD pathLen = ::GetModuleFileNameW(nullptr, exePathBuffer, MAX_PATH);
            if (pathLen == 0 || pathLen >= MAX_PATH)
            {
                std::cout << "wpI pid-reuse-probe: 无法取得自身可执行文件路径，跳过" << std::endl;
                return;
            }
            const std::wstring exePath(exePathBuffer, pathLen);

            DWORD heldPid = 0;
            HANDLE heldHandle = nullptr;
            if (!SpawnSelfAndWait(exePath, heldPid, heldHandle))
            {
                std::cout << "wpI pid-reuse-probe: 拉起第一个子进程失败，跳过" << std::endl;
                return;
            }

            constexpr int kProbeCount = 20;
            int reusedCount = 0;
            int spawnedCount = 0;
            for (int i = 0; i < kProbeCount; ++i)
            {
                DWORD pid = 0;
                HANDLE handle = nullptr;
                if (!SpawnSelfAndWait(exePath, pid, handle))
                {
                    continue;
                }
                ++spawnedCount;
                if (pid == heldPid)
                {
                    ++reusedCount;
                }
                ::CloseHandle(handle);
            }

            std::cout << "wpI pid-reuse-probe: 持有句柄期间又启动并等待了 " << spawnedCount
                       << " 个短命子进程（目标 " << kProbeCount << " 个），其中 PID 与被持有的 "
                       << heldPid << " 撞上的次数 = " << reusedCount
                       << "（按 Windows 文档行为预期为 0；本探针只记读数，不作为断言）"
                       << std::endl;

            ::CloseHandle(heldHandle);
        }

        // FindProcessIdByName：用 Toolhelp 快照按可执行文件名（不含路径）找第一个
        // 匹配的 pid，大小写不敏感（ASCII 范围，足够覆盖 "audiodg.exe" 这种全
        // ASCII 名字）。找不到返回 0。
        std::uint32_t FindProcessIdByName(const wchar_t* exeNameLower)
        {
            const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
            {
                return 0;
            }
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            std::uint32_t found = 0;
            if (::Process32FirstW(snapshot, &entry) != FALSE)
            {
                do
                {
                    std::wstring name = entry.szExeFile;
                    for (wchar_t& ch : name)
                    {
                        if (ch >= L'A' && ch <= L'Z')
                        {
                            ch = static_cast<wchar_t>(ch - L'A' + L'a');
                        }
                    }
                    if (name == exeNameLower)
                    {
                        found = entry.th32ProcessID;
                        break;
                    }
                } while (::Process32NextW(snapshot, &entry) != FALSE);
            }
            ::CloseHandle(snapshot);
            return found;
        }
    }

    void RunAnchorTests()
    {
        // ---- 冒烟：对真实本进程取锚点，交叉核对创建时间与位数 ----
        {
            FILETIME creationTime{};
            FILETIME exitTime{};
            FILETIME kernelTime{};
            FILETIME userTime{};
            const BOOL timesOk = ::GetProcessTimes(
                ::GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime);
            WPI_CHECK(timesOk != FALSE);
            const std::uint64_t expectedCreateTime = FileTimeToUint64Probe(creationTime);

            BOOL isWow64 = FALSE;
            const BOOL wowOk = ::IsWow64Process(::GetCurrentProcess(), &isWow64);
            WPI_CHECK(wowOk != FALSE);
            const std::uint32_t expectedBits = (isWow64 != FALSE) ? 32U : 64U;

            const ks::ui::AnchorInfo fromDock =
                ks::ui::AcquireAnchorFromDockHandle(reinterpret_cast<void*>(::GetCurrentProcess()));
            WPI_CHECK(!fromDock.identityWeak);
            WPI_CHECK(fromDock.createTime100ns == expectedCreateTime);
            WPI_CHECK(fromDock.addressBits == expectedBits);
            WPI_CHECK(fromDock.handle != nullptr);

            const std::optional<bool> aliveViaDock = ks::ui::QueryAnchorAlive(fromDock.handle);
            WPI_CHECK(aliveViaDock.has_value());
            WPI_CHECK(aliveViaDock.has_value() && *aliveViaDock);
            ks::ui::ReleaseAnchorHandle(fromDock.handle);

            const ks::ui::AnchorInfo byPid =
                ks::ui::AcquireAnchorForPid(static_cast<std::uint32_t>(::GetCurrentProcessId()));
            WPI_CHECK(!byPid.identityWeak);
            WPI_CHECK(byPid.createTime100ns == expectedCreateTime);
            WPI_CHECK(byPid.addressBits == expectedBits);
            const std::optional<bool> aliveViaPid = ks::ui::QueryAnchorAlive(byPid.handle);
            WPI_CHECK(aliveViaPid.has_value() && *aliveViaPid);
            ks::ui::ReleaseAnchorHandle(byPid.handle);
        }

        // ---- 边界：空句柄 / 0 号 PID / 空查询都应安全返回"未锚定"，不崩溃 ----
        {
            const ks::ui::AnchorInfo nullHandle = ks::ui::AcquireAnchorFromDockHandle(nullptr);
            WPI_CHECK(nullHandle.identityWeak);
            WPI_CHECK(nullHandle.handle == nullptr);

            const ks::ui::AnchorInfo zeroPid = ks::ui::AcquireAnchorForPid(0);
            WPI_CHECK(zeroPid.identityWeak);
            WPI_CHECK(zeroPid.handle == nullptr);

            WPI_CHECK(!ks::ui::QueryAnchorAlive(nullptr).has_value());
            ks::ui::ReleaseAnchorHandle(nullptr); // 不应崩溃。
        }

        // ---- D3 回归：受保护进程钉住后不应该降级为 weak ----
        // audiodg.exe（Windows 音频设备图形隔离进程）是系统上常见的受保护进程；
        // 本机找不到就跳过（计入 skipped，不计入失败）——这不是本包能控制的
        // 环境条件，强行断言只会在没有该进程的机器上制造假失败。
        {
            const std::uint32_t audiodgPid = FindProcessIdByName(L"audiodg.exe");
            if (audiodgPid == 0)
            {
                std::cout << "wpI D3-regression: 本机未找到 audiodg.exe，跳过受保护进程验证"
                           << " (skipped)" << std::endl;
            }
            else
            {
                const ks::ui::AnchorInfo protectedAnchor = ks::ui::AcquireAnchorForPid(audiodgPid);
                WPI_CHECK_NOTE(
                    !protectedAnchor.identityWeak,
                    QStringLiteral("D3：PROCESS_QUERY_LIMITED_INFORMATION 应该足以锚定受保护进程"));
                WPI_CHECK(protectedAnchor.createTime100ns != 0);
                ks::ui::ReleaseAnchorHandle(protectedAnchor.handle);
            }
        }

        // ---- D4 回归：退出码恰好是 259（STILL_ACTIVE）的已退出进程不应被误判为存活 ----
        {
            PROCESS_INFORMATION exitCodeProcess{};
            std::wstring exitCmd = L"cmd.exe /c exit 259";
            std::vector<wchar_t> exitCmdBuf(exitCmd.begin(), exitCmd.end());
            exitCmdBuf.push_back(L'\0');
            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            const BOOL spawned = ::CreateProcessW(
                nullptr, exitCmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startupInfo, &exitCodeProcess);
            WPI_CHECK_NOTE(spawned != FALSE, QStringLiteral("D4 回归需要能够拉起 cmd.exe"));
            if (spawned != FALSE)
            {
                const ks::ui::AnchorInfo anchor = ks::ui::AcquireAnchorForPid(exitCodeProcess.dwProcessId);
                ::WaitForSingleObject(exitCodeProcess.hProcess, 5000);
                const std::optional<bool> alive = ks::ui::QueryAnchorAlive(anchor.handle);
                WPI_CHECK_NOTE(
                    alive.has_value() && !*alive,
                    QStringLiteral("D4：退出码恰好是 259 的已退出进程必须被判定为已退出，不是仍在运行"));
                ks::ui::ReleaseAnchorHandle(anchor.handle);
                ::CloseHandle(exitCodeProcess.hProcess);
                ::CloseHandle(exitCodeProcess.hThread);
            }
        }

        RunPidReuseProbe();
    }
}
