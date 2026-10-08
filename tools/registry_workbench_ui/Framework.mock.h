#pragma once
#include <Windows.h>
#include <QtWidgets>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Only the Framework/log/progress boundary is replaced in the staged fixture.
struct kLogEvent {};
struct RegistryUiLog { template<class T> RegistryUiLog& operator<<(const T&) { return *this; } };
inline RegistryUiLog info, warn, dbg, err, fatal;
#define eol nullptr
struct RegistryUiProgress {
    template<class... T> int addReusable(T&&...) { return 1; }
    template<class... T> int add(T&&...) { return 1; }
    template<class... T> void set(T&&...) {}
};
inline RegistryUiProgress kPro;

LSTATUS WINAPI RegistryUiRegOpenKeyExW(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
LSTATUS WINAPI RegistryUiRegCloseKey(HKEY);
LSTATUS WINAPI RegistryUiRegEnumKeyExW(HKEY,DWORD,LPWSTR,LPDWORD,LPDWORD,LPWSTR,LPDWORD,PFILETIME);
LSTATUS WINAPI RegistryUiRegQueryValueExW(HKEY,LPCWSTR,LPDWORD,LPDWORD,LPBYTE,LPDWORD);
LSTATUS WINAPI RegistryUiRegSetValueExW(HKEY,LPCWSTR,DWORD,DWORD,const BYTE*,DWORD);
FARPROC WINAPI RegistryUiGetProcAddress(HMODULE,LPCSTR);
#define RegOpenKeyExW RegistryUiRegOpenKeyExW
#define RegCloseKey RegistryUiRegCloseKey
#define RegEnumKeyExW RegistryUiRegEnumKeyExW
#define RegQueryValueExW RegistryUiRegQueryValueExW
#define RegSetValueExW RegistryUiRegSetValueExW
#define GetProcAddress RegistryUiGetProcAddress
