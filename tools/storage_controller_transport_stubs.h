#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <SetupAPI.h>

BOOL WINAPI KsccTestDeviceIoControl(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
BOOL WINAPI KsccTestCloseHandle(HANDLE);
HANDLE WINAPI KsccTestCreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
HDEVINFO WINAPI KsccTestSetupDiGetClassDevsW(const GUID*, PCWSTR, HWND, DWORD);
BOOL WINAPI KsccTestSetupDiEnumDeviceInterfaces(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA);
BOOL WINAPI KsccTestSetupDiGetDeviceInterfaceDetailW(HDEVINFO, PSP_DEVICE_INTERFACE_DATA,
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD, PSP_DEVINFO_DATA);
BOOL WINAPI KsccTestSetupDiDestroyDeviceInfoList(HDEVINFO);
BOOL WINAPI KsccTestSetupDiGetDeviceRegistryPropertyW(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);

#define DeviceIoControl KsccTestDeviceIoControl
#define CloseHandle KsccTestCloseHandle
#define CreateFileW KsccTestCreateFileW
#define SetupDiGetClassDevsW KsccTestSetupDiGetClassDevsW
#define SetupDiEnumDeviceInterfaces KsccTestSetupDiEnumDeviceInterfaces
#define SetupDiGetDeviceInterfaceDetailW KsccTestSetupDiGetDeviceInterfaceDetailW
#define SetupDiDestroyDeviceInfoList KsccTestSetupDiDestroyDeviceInfoList
#define SetupDiGetDeviceRegistryPropertyW KsccTestSetupDiGetDeviceRegistryPropertyW
