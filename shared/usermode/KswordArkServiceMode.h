#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cwchar>
#include <string>

namespace ksword::ark
{
    enum class ServiceProfile { Legacy, StorageControllerPnp, Unknown };

    struct ServiceProfileResult
    {
        ServiceProfile profile = ServiceProfile::Unknown;
        DWORD error = ERROR_SUCCESS;
        bool scmManagementAllowed() const noexcept { return profile == ServiceProfile::Legacy; }
    };

    inline bool isKswordArkService(const std::wstring& name) noexcept
    {
        return _wcsicmp(name.c_str(), L"KswordARK") == 0;
    }

    // This is a read-only profile query. Missing Parameters/value is the legacy
    // default; unreadable or malformed configuration must never permit unload,
    // service deletion or replacement of the DriverStore image path.
    inline ServiceProfileResult queryServiceProfile()
    {
        HKEY key = nullptr;
        const LSTATUS openError = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Services\\KswordARK\\Parameters",
            0U, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
        if (openError == ERROR_FILE_NOT_FOUND || openError == ERROR_PATH_NOT_FOUND)
            return { ServiceProfile::Legacy, ERROR_SUCCESS };
        if (openError != ERROR_SUCCESS)
            return { ServiceProfile::Unknown, static_cast<DWORD>(openError) };
        DWORD value = 0U;
        DWORD type = 0U;
        DWORD bytes = sizeof(value);
        const LSTATUS queryError = ::RegQueryValueExW(key, L"StorageControllerPnP",
            nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes);
        ::RegCloseKey(key);
        if (queryError == ERROR_FILE_NOT_FOUND)
            return { ServiceProfile::Legacy, ERROR_SUCCESS };
        if (queryError != ERROR_SUCCESS)
            return { ServiceProfile::Unknown, static_cast<DWORD>(queryError) };
        if (type != REG_DWORD || bytes != sizeof(value) || value > 1U)
            return { ServiceProfile::Unknown, ERROR_INVALID_DATA };
        return { value == 1U ? ServiceProfile::StorageControllerPnp : ServiceProfile::Legacy,
            ERROR_SUCCESS };
    }

    inline DWORD serviceProfileManagementError(const ServiceProfileResult& profile) noexcept
    {
        return profile.profile == ServiceProfile::StorageControllerPnp ? ERROR_BUSY
            : profile.error == ERROR_SUCCESS ? ERROR_INVALID_DATA : profile.error;
    }

    inline constexpr const wchar_t* kStorageControllerPnpManagementMessage =
        L"KswordARK is configured for PnP storage controllers. Restore the Windows driver binding or remove each controller in Device Manager. After all controllers are unbound and the driver image has exited, manually set StorageControllerPnP to 0 to return to ordinary R0. SCM start, stop and service configuration changes are disabled in the PnP profile.";

    inline const wchar_t* serviceProfileManagementMessage(const ServiceProfileResult& profile) noexcept
    {
        return profile.profile == ServiceProfile::StorageControllerPnp
            ? kStorageControllerPnpManagementMessage
            : L"Cannot read the KswordARK service profile; service configuration was preserved.";
    }
}
