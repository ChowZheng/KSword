#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winsvc.h>

LSTATUS WINAPI KsccTestRegOpenKeyExW(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
LSTATUS WINAPI KsccTestRegQueryValueExW(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
LSTATUS WINAPI KsccTestRegCloseKey(HKEY);
SC_HANDLE WINAPI KsccTestOpenSCManagerW(LPCWSTR, LPCWSTR, DWORD);
SC_HANDLE WINAPI KsccTestOpenServiceW(SC_HANDLE, LPCWSTR, DWORD);
BOOL WINAPI KsccTestCloseServiceHandle(SC_HANDLE);
BOOL WINAPI KsccTestQueryServiceStatusEx(SC_HANDLE, SC_STATUS_TYPE, LPBYTE, DWORD, LPDWORD);
BOOL WINAPI KsccTestStartServiceW(SC_HANDLE, DWORD, LPCWSTR*);
BOOL WINAPI KsccTestControlService(SC_HANDLE, DWORD, LPSERVICE_STATUS);
BOOL WINAPI KsccTestDeleteService(SC_HANDLE);
BOOL WINAPI KsccTestChangeServiceConfigW(SC_HANDLE, DWORD, DWORD, DWORD, LPCWSTR, LPCWSTR, LPDWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
BOOL WINAPI KsccTestChangeServiceConfig2W(SC_HANDLE, DWORD, LPVOID);
SC_HANDLE WINAPI KsccTestCreateServiceW(SC_HANDLE, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD, DWORD, LPCWSTR, LPCWSTR, LPDWORD, LPCWSTR, LPCWSTR, LPCWSTR);

#define RegOpenKeyExW KsccTestRegOpenKeyExW
#define RegQueryValueExW KsccTestRegQueryValueExW
#define RegCloseKey KsccTestRegCloseKey
#define OpenSCManagerW KsccTestOpenSCManagerW
#define OpenServiceW KsccTestOpenServiceW
#define CloseServiceHandle KsccTestCloseServiceHandle
#define QueryServiceStatusEx KsccTestQueryServiceStatusEx
#define StartServiceW KsccTestStartServiceW
#define ControlService KsccTestControlService
#define DeleteService KsccTestDeleteService
#define ChangeServiceConfigW KsccTestChangeServiceConfigW
#define ChangeServiceConfig2W KsccTestChangeServiceConfig2W
#define CreateServiceW KsccTestCreateServiceW
