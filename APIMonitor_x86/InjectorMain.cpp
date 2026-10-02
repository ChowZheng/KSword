#include "../shared/ApiMonitorInjection.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 7 || wcscmp(argv[1], L"--pid") || wcscmp(argv[3], L"--creation") || wcscmp(argv[5], L"--dll")) return ERROR_INVALID_PARAMETER;
    wchar_t* end = nullptr; errno = 0;
    const auto pid = wcstoull(argv[2], &end, 10);
    if (errno || !end || *end || !pid || pid > MAXDWORD) return ERROR_INVALID_PARAMETER;
    errno = 0; const auto creation = wcstoull(argv[4], &end, 10);
    if (errno || !end || *end || !creation) return ERROR_INVALID_PARAMETER;
    std::wstring error;
    if (ks::winapi_monitor::injectAgentNative(static_cast<DWORD>(pid), argv[6], &error, creation)) return 0;
    const DWORD code = GetLastError(); fwprintf(stderr, L"%s\n", error.c_str());
    return static_cast<int>(code ? code : ERROR_DLL_INIT_FAILED);
}
