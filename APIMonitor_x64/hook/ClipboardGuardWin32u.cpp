#include "pch.h"
#include "ClipboardGuardWin32u.h"
#include "ClipboardGuardCommon.h"
#include "HookEngine.h"
#include "../core/MonitorCoverage.h"
#include "../MonitorAgent.h"

namespace apimon
{
    namespace
    {
        // Win32uBlockContext 作用：嵌进入口桩里的只读上下文，告诉 handler
        // "是哪个 API、属于读还是写"，handler 据此拼事件详情，不读取任何真实入参。
        struct Win32uBlockContext
        {
            const wchar_t* apiName = nullptr;
            ClipboardOperationKind kind = ClipboardOperationKind::Read;
        };

        // Win32uHookState 作用：单个 win32u 导出当前的安装状态。
        struct Win32uHookState
        {
            InlineHookRecord hookRecord{};
            void* entryStubAddress = nullptr; // 本模块生成的入口桩，安装成功后非空。
            void* originalAddress = nullptr;  // InstallInlineHook 要求的落点；本 stub 从不调用它。
            bool installed = false;
            Win32uBlockContext context{};
        };

        auto& g_win32uMutex = *new std::mutex;
        Win32uHookState g_win32uGetClipboardDataHook;
        Win32uHookState g_win32uSetClipboardDataHook;
        Win32uHookState g_win32uEmptyClipboardHook;

        // Win32uClipboardTarget 作用：把"导出名 + 归属方向 + 状态落点"串成一张表，
        // SyncClipboardWin32uHooks 只需要遍历这张表，不必为每个目标各写一份逻辑。
        struct Win32uClipboardTarget
        {
            const char* exportNameAnsi;
            const wchar_t* exportNameWide;
            ClipboardOperationKind kind;
            Win32uHookState* state;
        };

        // 只覆盖三个"确实搬运/清空数据"的方向，与 tier1 Win32 侧的
        // GetClipboardData/SetClipboardData/EmptyClipboard 一一对应；
        // Open/CloseClipboard 不搬运数据，tier1 已经解释过为什么不拦，这里同理不做。
        Win32uClipboardTarget g_win32uTargets[] = {
            { "NtUserGetClipboardData", L"NtUserGetClipboardData", ClipboardOperationKind::Read, &g_win32uGetClipboardDataHook },
            { "NtUserSetClipboardData", L"NtUserSetClipboardData", ClipboardOperationKind::Write, &g_win32uSetClipboardDataHook },
            { "NtUserEmptyClipboard", L"NtUserEmptyClipboard", ClipboardOperationKind::Write, &g_win32uEmptyClipboardHook },
        };

        // Win32uBlockEnter 作用：入口桩调用的唯一 C++ 落点。
        // - 输入：contextPointer 指向对应目标的 Win32uBlockContext；
        // - 处理：上报一条"该方向被 tier2 拦截"的事件，动作固定是 Block——
        //   这个函数只在策略判定为 BLOCK 时才会被装上，能走到这里就是拦截生效；
        // - 返回：写进 RAX 的 NTSTATUS，win32u 这层导出统一按 NTSTATUS 语义处理，
        //   STATUS_ACCESS_DENIED 是最贴近"没权限做这件事"的标准状态码。
        std::uint64_t Win32uBlockEnter(const Win32uBlockContext* const contextPointer)
        {
            if (contextPointer != nullptr)
            {
                ReportClipboardEvent(
                    contextPointer->kind,
                    L"win32u",
                    contextPointer->apiName,
                    0,
                    nullptr,
                    ClipboardPolicyAction::Block);
            }
            constexpr std::uint64_t kStatusAccessDenied = 0xC0000022ULL;
            return kStatusAccessDenied;
        }

        void EmitStubByte(unsigned char* const codePointer, std::size_t& offsetValue, const unsigned char byteValue)
        {
            codePointer[offsetValue++] = byteValue;
        }

        void EmitStubU64(unsigned char* const codePointer, std::size_t& offsetValue, const std::uint64_t value)
        {
            std::memcpy(codePointer + offsetValue, &value, sizeof(value));
            offsetValue += sizeof(value);
        }

        // BuildWin32uBlockStub 作用：
        // - 处理：生成一个不读入参、不调用原函数的最小 x64 桩——
        //   sub rsp,0x28（对齐栈+shadow space）→ mov rcx,contextPointer →
        //   mov rax,&Win32uBlockEnter → call rax → add rsp,0x28 → ret；
        //   字节序列与 HookTargets.cpp 里 Fake-Success 桩的手法完全一致
        //   （那份是私有实现，这里按同样、已验证过的模式为 tier2 单独写一份）；
        // - 返回：可执行内存地址，失败返回 nullptr。
        void* BuildWin32uBlockStub(const Win32uBlockContext* const contextPointer)
        {
            constexpr std::size_t kStubBytes = 64;
            unsigned char* const codePointer = static_cast<unsigned char*>(::VirtualAlloc(
                nullptr, kStubBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (codePointer == nullptr)
            {
                return nullptr;
            }

            std::size_t offsetValue = 0;
            EmitStubByte(codePointer, offsetValue, 0x48); EmitStubByte(codePointer, offsetValue, 0x83);
            EmitStubByte(codePointer, offsetValue, 0xEC); EmitStubByte(codePointer, offsetValue, 0x28); // sub rsp, 0x28
            EmitStubByte(codePointer, offsetValue, 0x48); EmitStubByte(codePointer, offsetValue, 0xB9); // mov rcx, imm64
            EmitStubU64(codePointer, offsetValue, reinterpret_cast<std::uint64_t>(contextPointer));
            EmitStubByte(codePointer, offsetValue, 0x48); EmitStubByte(codePointer, offsetValue, 0xB8); // mov rax, imm64
            EmitStubU64(codePointer, offsetValue, reinterpret_cast<std::uint64_t>(&Win32uBlockEnter));
            EmitStubByte(codePointer, offsetValue, 0xFF); EmitStubByte(codePointer, offsetValue, 0xD0); // call rax
            EmitStubByte(codePointer, offsetValue, 0x48); EmitStubByte(codePointer, offsetValue, 0x83);
            EmitStubByte(codePointer, offsetValue, 0xC4); EmitStubByte(codePointer, offsetValue, 0x28); // add rsp, 0x28
            EmitStubByte(codePointer, offsetValue, 0xC3);                                               // ret

            ::FlushInstructionCache(::GetCurrentProcess(), codePointer, offsetValue);
            return codePointer;
        }

        void FreeWin32uBlockStub(void* const stubAddress)
        {
            if (stubAddress != nullptr)
            {
                ::VirtualFree(stubAddress, 0, MEM_RELEASE);
            }
        }

        // InstallOneWin32uTarget/UninstallOneWin32uTarget 作用：
        // 单个目标的安装/卸载，供 SyncClipboardWin32uHooks 按策略调用。
        void InstallOneWin32uTarget(const Win32uClipboardTarget& target, const HMODULE win32uModule)
        {
            if (target.state->installed || target.state->hookRecord.permanentlyDisabled)
            {
                return;
            }
            // 找不到导出说明这个 Windows 版本没有这个函数名（老系统/未来改名），
            // 直接跳过——tier1 仍然完整工作，这只是少一层深度防御。
            if (::GetProcAddress(win32uModule, target.exportNameAnsi) == nullptr)
            {
                return;
            }

            target.state->context.apiName = target.exportNameWide;
            target.state->context.kind = target.kind;
            void* const stubAddress = BuildWin32uBlockStub(&target.state->context);
            if (stubAddress == nullptr)
            {
                return;
            }

            std::wstring ignoredErrorText;
            const InlineHookInstallResult installResult = InstallInlineHook(
                L"win32u.dll",
                target.exportNameAnsi,
                stubAddress,
                &target.state->hookRecord,
                &target.state->originalAddress,
                &ignoredErrorText);
            if (installResult == InlineHookInstallResult::Installed)
            {
                target.state->entryStubAddress = stubAddress;
                target.state->installed = true;
            }
            else
            {
                FreeWin32uBlockStub(stubAddress);
            }
        }

        bool UninstallOneWin32uTarget(const Win32uClipboardTarget& target)
        {
            if (!target.state->installed) return true;
            if (!UninstallInlineHook(&target.state->hookRecord)) return false;
            // The tiny entry stub can still be on another thread's instruction/return path.
            target.state->entryStubAddress = nullptr;
            target.state->installed = false;
            return true;
        }

    }

    void SyncClipboardWin32uHooks()
    {
        std::lock_guard lock(g_win32uMutex);
        const HMODULE win32uModule = ::GetModuleHandleW(L"win32u.dll");
        if (win32uModule == nullptr)
        {
            // 没有独立 win32u.dll 的系统（win32u 是 Win10 才拆出来的）没有 tier2 可装，
            // 直接返回，tier1 不受影响。
            return;
        }

        for (const Win32uClipboardTarget& target : g_win32uTargets)
        {
            const bool shouldBeBlocked = ResolveClipboardAction(target.kind) == ClipboardPolicyAction::Block;
            if (shouldBeBlocked)
            {
                InstallOneWin32uTarget(target, win32uModule);
            }
            else
            {
                UninstallOneWin32uTarget(target);
            }
        }
    }

    bool UninstallAllClipboardWin32uHooks()
    {
        std::lock_guard lock(g_win32uMutex);
        bool removed = true;
        for (const Win32uClipboardTarget& target : g_win32uTargets)
            removed = UninstallOneWin32uTarget(target) && removed;
        return removed;
    }
    void AppendWin32uClipboardCoverage(std::vector<ks::winapi_monitor::ApiMonitorEventPacket>& rows, bool removing)
    {
        using namespace ks::winapi_monitor;
        std::lock_guard lock(g_win32uMutex);
        const auto module = ::GetModuleHandleW(L"win32u.dll");
        for (const auto& target : g_win32uTargets)
        {
            const auto& record = target.state->hookRecord;
            const bool blocked = ResolveClipboardAction(target.kind) == ClipboardPolicyAction::Block;
            const auto state = record.installed ? (record.sharedEntry ? CoverageState::SharedEntry : CoverageState::Installed)
                : removing && record.targetAddress ? CoverageState::Removed
                : !ActiveConfig().enableClipboard ? CoverageState::CategoryDisabled
                : !blocked ? CoverageState::RuleExcluded
                : !module ? CoverageState::WaitingModule
                : !::GetProcAddress(module, target.exportNameAnsi) ? CoverageState::ExportMissing
                : record.permanentlyDisabled ? CoverageState::Unsupported : CoverageState::RetryableFailure;
            ApiMonitorEventPacket row{};
            row.apiId = RuntimeApiId(L"win32u.dll", target.exportNameWide);
            row.hookKind = static_cast<std::uint32_t>(HookKind::Fake);
            row.coverageState = static_cast<std::uint32_t>(state);
            row.hookAddress = reinterpret_cast<std::uintptr_t>(record.targetAddress);
            wcscpy_s(row.moduleName, L"win32u.dll"); wcscpy_s(row.apiName, target.exportNameWide);
            wcsncpy_s(row.detailText, record.lastFailure.empty()
                ? L"clipboard tier2 policy handler; installed only for Block policy" : record.lastFailure.c_str(), _TRUNCATE);
            rows.push_back(row);
        }
    }
}
