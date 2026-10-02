#include "pch.h"
#include "HookEngine.h"

#include <TlHelp32.h>

namespace apimon
{
    namespace
    {
        constexpr std::size_t kAbsoluteJumpSize = 14; // kAbsoluteJumpSize：FF 25 [rip+0] + 8 字节目标地址，不破坏通用寄存器。
        thread_local std::uint32_t g_inlineHookInternalBypassDepth = 0; // g_inlineHookInternalBypassDepth：HookEngine 内部操作重入屏蔽深度。

        // Allocate the handle list before suspension: a suspended thread may own the heap lock.
        class ScopedOtherThreadsSuspender
        {
        public:
            ScopedOtherThreadsSuspender()
            {
                const DWORD pid = ::GetCurrentProcessId();
                const DWORD tid = ::GetCurrentThreadId();
                HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (snapshot == INVALID_HANDLE_VALUE) return;
                THREADENTRY32 entry{};
                entry.dwSize = sizeof(entry);
                bool complete = ::Thread32First(snapshot, &entry) != FALSE;
                if (complete)
                {
                    do
                    {
                        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == tid) continue;
                        HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | SYNCHRONIZE,
                            FALSE, entry.th32ThreadID);
                        if (thread != nullptr) m_threads.push_back(thread);
                        else if (::GetLastError() != ERROR_INVALID_PARAMETER) complete = false;
                    } while (::Thread32Next(snapshot, &entry));
                }
                ::CloseHandle(snapshot);
                if (!complete) return;
                for (HANDLE thread : m_threads)
                {
                    if (::SuspendThread(thread) == static_cast<DWORD>(-1)) return;
                    ++m_suspendedCount;
                }
                m_ready = true;
            }

            bool CanPatch(const void* address, const std::size_t length) const
            {
                if (!m_ready) return false;
                const auto first = reinterpret_cast<std::uintptr_t>(address);
                for (HANDLE thread : m_threads)
                {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL;
                    if (::GetThreadContext(thread, &context) == FALSE) return false;
                    if (context.Rip >= first && context.Rip < first + length) return false;
                }
                return true;
            }

            void Resume()
            {
                while (m_suspendedCount > 0) ::ResumeThread(m_threads[--m_suspendedCount]);
            }

            ~ScopedOtherThreadsSuspender()
            {
                Resume();
                for (HANDLE thread : m_threads) ::CloseHandle(thread);
            }
        private:
            ScopedInlineHookInternalBypass m_hookBypass;
            std::vector<HANDLE> m_threads;
            std::size_t m_suspendedCount = 0;
            bool m_ready = false;
        };

        std::size_t ModRmLength(
            const unsigned char* codePointer,
            const std::size_t maxLength,
            bool* usesRipRelativeOut)
        {
            if (codePointer == nullptr || maxLength < 1)
            {
                return 0;
            }
            if (usesRipRelativeOut != nullptr)
            {
                *usesRipRelativeOut = false;
            }

            std::size_t totalLength = 1;
            const unsigned char modrmValue = codePointer[0];
            const unsigned char modValue = static_cast<unsigned char>((modrmValue >> 6) & 0x3);
            const unsigned char rmValue = static_cast<unsigned char>(modrmValue & 0x7);

            if (modValue != 3 && rmValue == 4)
            {
                if (maxLength < totalLength + 1)
                {
                    return 0;
                }
                const unsigned char sibValue = codePointer[totalLength++];
                const unsigned char baseValue = static_cast<unsigned char>(sibValue & 0x7);
                if (modValue == 0 && baseValue == 5)
                {
                    totalLength += 4;
                }
            }

            if (modValue == 0 && rmValue == 5)
            {
                if (usesRipRelativeOut != nullptr)
                {
                    *usesRipRelativeOut = true;
                }
                totalLength += 4;
            }
            else if (modValue == 1)
            {
                totalLength += 1;
            }
            else if (modValue == 2)
            {
                totalLength += 4;
            }

            return totalLength <= maxLength ? totalLength : 0;
        }

        std::size_t DecodeInstructionLength(const unsigned char* codePointer, const std::size_t maxLength)
        {
            if (codePointer == nullptr || maxLength == 0)
            {
                return 0;
            }

            std::size_t offsetValue = 0;
            bool operandOverride = false;
            bool rexW = false;

            while (offsetValue < maxLength)
            {
                const unsigned char prefixValue = codePointer[offsetValue];
                if (prefixValue == 0x66)
                {
                    operandOverride = true;
                    ++offsetValue;
                    continue;
                }
                if ((prefixValue >= 0x40 && prefixValue <= 0x4F))
                {
                    rexW = (prefixValue & 0x08) != 0;
                    ++offsetValue;
                    continue;
                }
                if (prefixValue == 0xF0 || prefixValue == 0xF2 || prefixValue == 0xF3
                    || prefixValue == 0x2E || prefixValue == 0x36 || prefixValue == 0x3E
                    || prefixValue == 0x26 || prefixValue == 0x64 || prefixValue == 0x65)
                {
                    ++offsetValue;
                    continue;
                }
                break;
            }

            if (offsetValue >= maxLength)
            {
                return 0;
            }

            const unsigned char opcodeValue = codePointer[offsetValue++];
            if ((opcodeValue >= 0x50 && opcodeValue <= 0x5F)
                || opcodeValue == 0x90)
            {
                return offsetValue;
            }
            if (opcodeValue == 0x6A)
            {
                return offsetValue + 1 <= maxLength ? offsetValue + 1 : 0;
            }
            if (opcodeValue == 0x68)
            {
                const std::size_t immediateLength = operandOverride ? 2 : 4;
                return offsetValue + immediateLength <= maxLength ? offsetValue + immediateLength : 0;
            }
            if (opcodeValue == 0xE8 || opcodeValue == 0xE9 || opcodeValue == 0xEB)
            {
                // 相对控制流在复制到 trampoline 后会改写语义，这里直接视为不可安全 Hook。
                return 0;
            }
            if (opcodeValue >= 0xB8 && opcodeValue <= 0xBF)
            {
                const std::size_t immLength = rexW ? 8 : (operandOverride ? 2 : 4);
                return offsetValue + immLength <= maxLength ? offsetValue + immLength : 0;
            }
            if (opcodeValue == 0x0F)
            {
                if (offsetValue >= maxLength)
                {
                    return 0;
                }

                const unsigned char secondOpcode = codePointer[offsetValue++];
                if (secondOpcode >= 0x80 && secondOpcode <= 0x8F)
                {
                    return 0;
                }
                if (secondOpcode == 0x1F)
                {
                    bool usesRipRelative = false;
                    const std::size_t modrmLength = ModRmLength(
                        codePointer + offsetValue,
                        maxLength - offsetValue,
                        &usesRipRelative);
                    if (usesRipRelative)
                    {
                        return 0;
                    }
                    return modrmLength == 0 ? 0 : offsetValue + modrmLength;
                }
                return 0;
            }

            const auto appendModRmInstruction = [&](const std::size_t immediateLength) -> std::size_t {
                bool usesRipRelative = false;
                const std::size_t modrmLength = ModRmLength(
                    codePointer + offsetValue,
                    maxLength - offsetValue,
                    &usesRipRelative);
                if (modrmLength == 0)
                {
                    return 0;
                }
                if (usesRipRelative)
                {
                    return 0;
                }
                const std::size_t totalLength = offsetValue + modrmLength + immediateLength;
                return totalLength <= maxLength ? totalLength : 0;
            };

            switch (opcodeValue)
            {
            case 0x01:
            case 0x03:
            case 0x09:
            case 0x0B:
            case 0x21:
            case 0x23:
            case 0x29:
            case 0x2B:
            case 0x31:
            case 0x33:
            case 0x39:
            case 0x3B:
            case 0x63:
            case 0x84:
            case 0x85:
            case 0x88:
            case 0x89:
            case 0x8A:
            case 0x8B:
            case 0x8D:
                return appendModRmInstruction(0);
            case 0x81:
            case 0xC7:
                return appendModRmInstruction(operandOverride && !rexW ? 2 : 4);
            case 0x80:
            case 0x83:
            case 0xC6:
                return appendModRmInstruction(1);
            case 0xFF:
                // 0xFF 同时覆盖 call/jmp/push 等多种语义，这里统一保守拒绝。
                return 0;
            case 0xF6:
            case 0xF7:
            {
                // Nt* syscall stub 在现代 x64 ntdll 中常见形态包含：
                //   F6 04 25 08 03 FE 7F 01    test byte ptr [KUSER_SHARED_DATA+0x308], 1
                // 旧解码器不认识 F6，导致 Nt* 导出桩无法安装 inline hook。
                // 这里只复制不会改变相对控制流的 F6/F7 ModRM 指令；TEST /0,/1 带立即数，其它一元操作不带立即数。
                if (offsetValue >= maxLength)
                {
                    return 0;
                }

                const unsigned char modrmValue = codePointer[offsetValue];
                const unsigned char regValue = static_cast<unsigned char>((modrmValue >> 3) & 0x7);
                const std::size_t immediateLength = (regValue == 0 || regValue == 1)
                    ? (opcodeValue == 0xF6 ? 1 : (operandOverride && !rexW ? 2 : 4))
                    : 0;
                return appendModRmInstruction(immediateLength);
            }
            default:
                break;
            }

            return 0;
        }

        // BuildAbsoluteJump 作用：
        // - 输入：targetBuffer 指向至少 kAbsoluteJumpSize 字节可写内存，destinationAddress 为跳转目标；
        // - 处理：写入 x64 RIP 间接绝对跳转桩，避免 mov rax/jmp rax 破坏 trampoline 已恢复的 RAX；
        // - 返回：无返回值，调用者随后负责刷新指令缓存。
        void BuildAbsoluteJump(unsigned char* targetBuffer, const void* destinationAddress)
        {
            targetBuffer[0] = 0xFF;
            targetBuffer[1] = 0x25;
            targetBuffer[2] = 0x00;
            targetBuffer[3] = 0x00;
            targetBuffer[4] = 0x00;
            targetBuffer[5] = 0x00;
            std::memcpy(targetBuffer + 6, &destinationAddress, sizeof(destinationAddress));
        }

        void* ResolveJumpStub(void* addressValue)
        {
            unsigned char* currentPointer = static_cast<unsigned char*>(addressValue);
            for (int depth = 0; depth < 8 && currentPointer != nullptr; ++depth)
            {
                if (currentPointer[0] == 0xE9)
                {
                    const std::int32_t relativeOffset = *reinterpret_cast<std::int32_t*>(currentPointer + 1);
                    currentPointer = currentPointer + 5 + relativeOffset;
                    continue;
                }
                if (currentPointer[0] == 0xEB)
                {
                    const std::int8_t relativeOffset = *reinterpret_cast<std::int8_t*>(currentPointer + 1);
                    currentPointer = currentPointer + 2 + relativeOffset;
                    continue;
                }
                if (currentPointer[0] == 0xFF && currentPointer[1] == 0x25)
                {
                    const std::int32_t relativeOffset = *reinterpret_cast<std::int32_t*>(currentPointer + 2);
                    void** indirectPointer = reinterpret_cast<void**>(currentPointer + 6 + relativeOffset);
                    currentPointer = static_cast<unsigned char*>(*indirectPointer);
                    continue;
                }
                if (currentPointer[0] == 0x48 && currentPointer[1] == 0xFF && currentPointer[2] == 0x25)
                {
                    const std::int32_t relativeOffset = *reinterpret_cast<std::int32_t*>(currentPointer + 3);
                    void** indirectPointer = reinterpret_cast<void**>(currentPointer + 7 + relativeOffset);
                    currentPointer = static_cast<unsigned char*>(*indirectPointer);
                    continue;
                }
                break;
            }
            return currentPointer;
        }

        std::size_t CalculatePatchSize(const unsigned char* codePointer)
        {
            std::size_t patchSize = 0;
            while (patchSize < kAbsoluteJumpSize && patchSize < 24)
            {
                const std::size_t instructionLength = DecodeInstructionLength(codePointer + patchSize, 24 - patchSize);
                if (instructionLength == 0)
                {
                    return 0;
                }
                patchSize += instructionLength;
            }
            return patchSize >= kAbsoluteJumpSize ? patchSize : 0;
        }
    }

    bool IsInlineHookInternalBypassActive()
    {
        // IsInlineHookInternalBypassActive 作用：
        // - 输入：无；
        // - 处理：读取当前线程 HookEngine 内部操作深度；
        // - 返回：深度大于 0 返回 true。
        return g_inlineHookInternalBypassDepth != 0;
    }

    ScopedInlineHookInternalBypass::ScopedInlineHookInternalBypass()
    {
        // 构造函数：
        // - 输入：无；
        // - 处理：当前线程进入 HookEngine 内部区间，HookedXXX wrapper 将直接旁路；
        // - 返回：无返回值。
        ++g_inlineHookInternalBypassDepth;
        m_entered = true;
    }

    ScopedInlineHookInternalBypass::~ScopedInlineHookInternalBypass()
    {
        // 析构函数：
        // - 输入：无；
        // - 处理：当前线程退出 HookEngine 内部区间，防止安装/卸载结束后继续旁路用户调用；
        // - 返回：无返回值。
        if (m_entered && g_inlineHookInternalBypassDepth != 0)
        {
            --g_inlineHookInternalBypassDepth;
        }
    }

    InlineHookInstallResult InstallInlineHook(
        const wchar_t* moduleName,
        const char* procName,
        void* detourAddress,
        InlineHookRecord* hookOut,
        void** originalOut,
        std::wstring* errorTextOut)
    {
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }

        ScopedInlineHookInternalBypass hookBypassScope;
        if (moduleName == nullptr || procName == nullptr || detourAddress == nullptr || hookOut == nullptr || originalOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"InstallInlineHook received invalid argument.";
            }
            return InlineHookInstallResult::PermanentFailure;
        }
        if (hookOut->permanentlyDisabled)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"Hook has been permanently disabled after a previous unsafe install attempt.";
            }
            return InlineHookInstallResult::PermanentFailure;
        }

        HMODULE moduleHandle = ::GetModuleHandleW(moduleName);
        if (moduleHandle == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = std::wstring(L"Module not loaded: ") + moduleName;
            }
            return InlineHookInstallResult::RetryableFailure;
        }

        void* exportAddress = reinterpret_cast<void*>(::GetProcAddress(moduleHandle, procName));
        if (exportAddress == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"GetProcAddress failed.";
            }
            hookOut->permanentlyDisabled = true;
            return InlineHookInstallResult::PermanentFailure;
        }

        unsigned char* targetPointer = static_cast<unsigned char*>(ResolveJumpStub(exportAddress));
        const std::size_t patchSize = CalculatePatchSize(targetPointer);
        if (patchSize == 0 || patchSize > hookOut->originalBytes.size())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"Unsupported or relocation-unsafe prologue for inline hook.";
            }
            hookOut->permanentlyDisabled = true;
            return InlineHookInstallResult::PermanentFailure;
        }

        unsigned char* trampolinePointer = static_cast<unsigned char*>(::VirtualAlloc(
            nullptr,
            patchSize + kAbsoluteJumpSize,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
        if (trampolinePointer == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"VirtualAlloc for trampoline failed.";
            }
            hookOut->permanentlyDisabled = true;
            return InlineHookInstallResult::PermanentFailure;
        }

        // Keep the underlying image resident while an old trampoline may still execute.
        HMODULE pinnedModule = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(targetPointer), &pinnedModule))
        {
            ::VirtualFree(trampolinePointer, 0, MEM_RELEASE);
            if (errorTextOut) *errorTextOut = L"Could not pin the hook target module.";
            return InlineHookInstallResult::RetryableFailure;
        }
        std::memcpy(trampolinePointer, targetPointer, patchSize);
        BuildAbsoluteJump(trampolinePointer + patchSize, targetPointer + patchSize);

        DWORD oldProtect = 0;
        if (::VirtualProtect(targetPointer, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect) == FALSE)
        {
            ::VirtualFree(trampolinePointer, 0, MEM_RELEASE);
            if (errorTextOut != nullptr)
            {
                *errorTextOut = L"VirtualProtect for target patch failed.";
            }
            hookOut->permanentlyDisabled = true;
            return InlineHookInstallResult::PermanentFailure;
        }

        ScopedOtherThreadsSuspender suspendOtherThreadsScope;
        if (!suspendOtherThreadsScope.CanPatch(targetPointer, patchSize))
        {
            suspendOtherThreadsScope.Resume();
            DWORD ignored = 0;
            ::VirtualProtect(targetPointer, patchSize, oldProtect, &ignored);
            ::VirtualFree(trampolinePointer, 0, MEM_RELEASE);
            if (errorTextOut) *errorTextOut = L"A thread is executing the patch region, or cannot be suspended safely.";
            return InlineHookInstallResult::RetryableFailure;
        }
        hookOut->targetAddress = targetPointer;
        hookOut->detourAddress = detourAddress;
        hookOut->trampolineAddress = trampolinePointer;
        hookOut->patchSize = patchSize;
        hookOut->permanentlyDisabled = false;
        *originalOut = trampolinePointer;
        std::memcpy(hookOut->originalBytes.data(), targetPointer, patchSize);

        unsigned char patchBuffer[32] = {};
        BuildAbsoluteJump(patchBuffer, detourAddress);
        std::memset(patchBuffer + kAbsoluteJumpSize, 0x90, patchSize - kAbsoluteJumpSize);
        std::memcpy(targetPointer, patchBuffer, patchSize);
        ::FlushInstructionCache(::GetCurrentProcess(), targetPointer, patchSize);

        DWORD unusedProtect = 0;
        ::VirtualProtect(targetPointer, patchSize, oldProtect, &unusedProtect);
        hookOut->installed = true;
        return InlineHookInstallResult::Installed;
    }

    bool UninstallInlineHook(InlineHookRecord* hookValue)
    {
        ScopedInlineHookInternalBypass hookBypassScope;
        if (hookValue == nullptr || !hookValue->installed) return true;
        if (hookValue->targetAddress == nullptr) return false;

        DWORD oldProtect = 0;
        if (!::VirtualProtect(hookValue->targetAddress, hookValue->patchSize, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;
        ScopedOtherThreadsSuspender suspended;
        DWORD ignored = 0;
        if (!suspended.CanPatch(hookValue->targetAddress, hookValue->patchSize))
        {
            ::VirtualProtect(hookValue->targetAddress, hookValue->patchSize, oldProtect, &ignored);
            return false;
        }
        std::memcpy(hookValue->targetAddress, hookValue->originalBytes.data(), hookValue->patchSize);
        ::FlushInstructionCache(::GetCurrentProcess(), hookValue->targetAddress, hookValue->patchSize);
        ::VirtualProtect(hookValue->targetAddress, hookValue->patchSize, oldProtect, &ignored);
        hookValue->installed = false;
        // Calls can be inside the detour, trampoline, or the original function (with a return
        // address into the trampoline). Retire executable code until process exit; never clear
        // its original pointer. The target and Agent images are pinned for that lifetime.
        return true;
    }
}
