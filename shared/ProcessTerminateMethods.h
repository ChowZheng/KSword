#pragma once

#include "../Ksword5.1/Ksword5.1/ksword/process/process.h"

#include <array>
#include <cstdint>
#include <string>

namespace ks::process {

struct TerminateMethodEntry {
    const char* methodName;
    const wchar_t* wideName;
    bool (*invokeMethod)(std::uint32_t, std::string*);
};

// Keep the process-list advanced menu and both detail-page selectors in sync.
inline const std::array<TerminateMethodEntry, 14>& TerminateMethodTable() {
    static const std::array<TerminateMethodEntry, 14> methods{{
        { "TerminateProcess(Kernel32)", L"TerminateProcess(Kernel32)", TerminateProcessByWin32 },
        { "NtTerminateProcess/ZwTerminateProcess", L"NtTerminateProcess/ZwTerminateProcess", TerminateProcessByNtNative },
        { "WTSTerminateProcess(WTS API)", L"WTSTerminateProcess(WTS API)", TerminateProcessByWtsApi },
        { "WinStationTerminateProcess(winsta)", L"WinStationTerminateProcess(winsta)", TerminateProcessByWinStationApi },
        { "TerminateJobObject(Job)", L"TerminateJobObject(Job)", TerminateProcessByJobObject },
        { "NtTerminateJobObject/ZwTerminateJobObject", L"NtTerminateJobObject/ZwTerminateJobObject", TerminateProcessByNtJobObject },
        { "RmShutdown(Restart Manager)", L"RmShutdown(Restart Manager)",
            [](std::uint32_t pid, std::string* detail) { return TerminateProcessByRestartManager(pid, false, detail); } },
        { "RmShutdown(Restart Manager, force)", L"RmShutdown(Restart Manager, force)",
            [](std::uint32_t pid, std::string* detail) { return TerminateProcessByRestartManager(pid, true, detail); } },
        { "DuplicateHandle(-1)+TerminateProcess", L"DuplicateHandle(-1)+TerminateProcess", TerminateProcessByDuplicateHandlePseudo },
        { "TerminateThread(全部线程)", L"TerminateThread(全部线程)", TerminateAllThreadsByPid },
        { "NtTerminateThread/ZwTerminateThread(全部线程)", L"NtTerminateThread/ZwTerminateThread(全部线程)", TerminateAllThreadsByPidNtNative },
        { "DebugActiveProcess 调试附加", L"DebugActiveProcess 调试附加", TerminateProcessByDebugAttach },
        { "ntsd -c q -p <pid>", L"ntsd -c q -p <pid>", TerminateProcessByNtsdCommand },
        { "NtUnmapViewOfSection 卸载 ntdll.dll", L"NtUnmapViewOfSection 卸载 ntdll.dll", TerminateProcessByNtUnmapNtdll }
    }};
    return methods;
}

} // namespace ks::process
