#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winsvc.h>

LSTATUS WINAPI KsccStartupRegOpenKeyExW(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
LSTATUS WINAPI KsccStartupRegQueryValueExW(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
LSTATUS WINAPI KsccStartupRegCloseKey(HKEY);
SC_HANDLE WINAPI KsccStartupOpenSCManagerW(LPCWSTR, LPCWSTR, DWORD);
SC_HANDLE WINAPI KsccStartupOpenServiceW(SC_HANDLE, LPCWSTR, DWORD);
BOOL WINAPI KsccStartupCloseServiceHandle(SC_HANDLE);
BOOL WINAPI KsccStartupQueryServiceConfigW(SC_HANDLE, LPQUERY_SERVICE_CONFIGW, DWORD, LPDWORD);
BOOL WINAPI KsccStartupChangeServiceConfigW(SC_HANDLE, DWORD, DWORD, DWORD, LPCWSTR,
    LPCWSTR, LPDWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);

#define RegOpenKeyExW KsccStartupRegOpenKeyExW
#define RegQueryValueExW KsccStartupRegQueryValueExW
#define RegCloseKey KsccStartupRegCloseKey
#define OpenSCManagerW KsccStartupOpenSCManagerW
#define OpenServiceW KsccStartupOpenServiceW
#define CloseServiceHandle KsccStartupCloseServiceHandle
#define QueryServiceConfigW KsccStartupQueryServiceConfigW
#define ChangeServiceConfigW KsccStartupChangeServiceConfigW
