// The runner supplies the unmodified bodies extracted from startup.cpp. SCM
// and registry transport are replaced; the action and profile parser are real.
#include "startup_scm_production.inc"

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
    DWORD registryOpenError;
    DWORD registryQueryError;
    DWORD registryType;
    DWORD registryBytes;
    DWORD registryValue;
    DWORD currentStartType;
    unsigned int configQueries;
    unsigned int profileQueries;
    unsigned int mutations;
    unsigned int unsafeMutations;
    unsigned int openHandles;
    unsigned int checks;
    unsigned int flipConfigQuery;
    bool flipScmOpen;
    bool flipServiceOpen;
    bool flipToUnknown;
    bool verificationMismatch;
    bool verificationQueryFailure;
    bool rollbackFailure;
    std::wstring openedService;

    void check(const bool value, const char* message)
    {
        ++checks;
        if (!value) throw std::runtime_error(message);
    }

    void reset(const DWORD profile = 0U, const DWORD startType = SERVICE_DEMAND_START)
    {
        check(openHandles == 0U, "the preceding startup action leaked an SCM handle");
        registryOpenError = registryQueryError = ERROR_SUCCESS;
        registryType = REG_DWORD;
        registryBytes = sizeof(DWORD);
        registryValue = profile;
        currentStartType = startType;
        configQueries = profileQueries = mutations = unsafeMutations = 0U;
        flipConfigQuery = 0U;
        flipScmOpen = flipServiceOpen = flipToUnknown = false;
        verificationMismatch = verificationQueryFailure = rollbackFailure = false;
        openedService.clear();
    }

    void flipProfile()
    {
        if (flipToUnknown) registryQueryError = ERROR_ACCESS_DENIED;
        else registryValue = 1U;
    }

    ks::startup::StartupEntry entry(const char* name = "KswordARK", const bool driver = true)
    {
        ks::startup::StartupEntry result;
        result.actionKind = ks::startup::StartupActionKind::ScmStartType;
        result.actionLocator.serviceNameText = name;
        result.actionLocator.serviceIsDriver = driver;
        result.actionLocator.serviceStartType = currentStartType;
        return result;
    }

    void checkBlocked(const ks::startup::ActionResult& result, const DWORD error,
        const unsigned int expectedMutations = 0U)
    {
        check(!result.success, "protected startup action must fail closed");
        check(result.errorCode == error, "protected startup action must preserve profile error");
        check(mutations == expectedMutations, "protected action invoked an unexpected SCM mutation");
        check(unsafeMutations == 0U, "startup action mutated a protected service profile");
        check(openHandles == 0U, "protected startup action leaked an SCM handle");
        check(result.messageText == (expectedMutations == 0U
            ? FromWide(ksword::ark::serviceProfileManagementMessage(ksword::ark::queryServiceProfile()))
            : FromWide(L"服务启动类型验证失败，且无法恢复操作前配置。")),
            "startup action must accurately distinguish preserved and already changed configuration");
    }

    void protectedProfileTests()
    {
        for (unsigned int mode = 0U; mode < 7U; ++mode)
        {
            for (const bool enable : { false, true })
            {
                reset(1U, enable ? SERVICE_DISABLED : SERVICE_DEMAND_START);
                DWORD expectedError = ERROR_BUSY;
                if (mode == 1U) registryOpenError = expectedError = ERROR_ACCESS_DENIED;
                if (mode == 2U) registryQueryError = expectedError = ERROR_ACCESS_DENIED;
                if (mode == 3U) { registryType = REG_SZ; expectedError = ERROR_INVALID_DATA; }
                if (mode == 4U) { registryBytes = 3U; expectedError = ERROR_INVALID_DATA; }
                if (mode == 5U) { registryValue = 2U; expectedError = ERROR_INVALID_DATA; }
                if (mode == 6U) { registryQueryError = ERROR_MORE_DATA; expectedError = ERROR_MORE_DATA; }
                const auto result = SetScmEntryEnabled(entry("kSwOrDaRk"), enable);
                checkBlocked(result, expectedError);
                check(result.status == ks::startup::StartupActionStatus::WriteFailed && !result.changed,
                    "entry-time profile rejection must report an unchanged failure");
                check(configQueries == 0U && openedService.empty(),
                    "entry-time profile rejection must not open or query SCM");
            }
        }
    }

    void startupRaceTests()
    {
        for (const bool unknown : { false, true })
        {
            for (unsigned int boundary = 0U; boundary < 4U; ++boundary)
            {
                reset();
                flipToUnknown = unknown;
                if (boundary == 0U) flipScmOpen = true;
                if (boundary == 1U) flipServiceOpen = true;
                if (boundary >= 2U) flipConfigQuery = boundary - 1U;
                const auto result = SetScmEntryEnabled(entry(), false);
                checkBlocked(result, unknown ? ERROR_ACCESS_DENIED : ERROR_BUSY);
                check(!result.changed && !result.rollbackAttempted,
                    "profile change before the first mutation must leave startup type unchanged");
                check(currentStartType == SERVICE_DEMAND_START,
                    "profile race must preserve the original service start type");
            }

            for (const bool mismatch : { false, true })
            {
                reset();
                flipToUnknown = unknown;
                flipConfigQuery = 4U;
                verificationMismatch = mismatch;
                const auto result = SetScmEntryEnabled(entry(), false);
                checkBlocked(result, unknown ? ERROR_ACCESS_DENIED : ERROR_BUSY, 1U);
                check(result.changed && !result.rollbackAttempted && !result.rollbackSucceeded,
                    "post-mutation profile transition must report changed state without a forbidden rollback");
                check(result.status == (mismatch ? ks::startup::StartupActionStatus::RollbackFailed
                    : ks::startup::StartupActionStatus::VerificationFailed),
                    "verification and blocked rollback must have distinct result classifications");
                check(currentStartType == SERVICE_DISABLED,
                    "verification-time profile transition must not restore the legacy start type");
            }

            reset();
            flipToUnknown = unknown;
            flipConfigQuery = 3U;
            verificationQueryFailure = true;
            const auto failedQuery = SetScmEntryEnabled(entry(), false);
            checkBlocked(failedQuery, unknown ? ERROR_ACCESS_DENIED : ERROR_BUSY, 1U);
            check(failedQuery.status == ks::startup::StartupActionStatus::RollbackFailed &&
                !failedQuery.rollbackAttempted, "failed verification query must not bypass the rollback profile guard");
        }

        reset(0U, SERVICE_DISABLED);
        flipConfigQuery = 2U;
        const auto unchangedRace = SetScmEntryEnabled(entry(), false);
        checkBlocked(unchangedRace, ERROR_BUSY);
        check(!unchangedRace.changed, "a no-change result must still reject a newly protected profile");
    }

    void compatibilityTests()
    {
        for (const bool driver : { false, true })
        {
            reset(0U, SERVICE_DISABLED);
            const auto enabled = SetScmEntryEnabled(entry("KswordARK", driver), true);
            check(enabled.success && enabled.changed && mutations == 1U,
                "legacy startup enable must retain the ordinary SCM mutation");
            check(currentStartType == (driver ? SERVICE_SYSTEM_START : SERVICE_AUTO_START),
                "legacy service and driver enable must retain their existing start types");
            reset();
            const auto disabled = SetScmEntryEnabled(entry("KswordARK", driver), false);
            check(disabled.success && disabled.changed && mutations == 1U && currentStartType == SERVICE_DISABLED,
                "legacy startup disable must remain available");
        }

        reset(0U, SERVICE_DISABLED);
        const auto unchanged = SetScmEntryEnabled(entry(), false);
        check(unchanged.success && !unchanged.changed && mutations == 0U,
            "legacy no-change action must remain read-only");
        for (const bool keyMissing : { false, true })
        {
            reset();
            if (keyMissing) registryOpenError = ERROR_FILE_NOT_FOUND;
            else registryQueryError = ERROR_FILE_NOT_FOUND;
            check(SetScmEntryEnabled(entry(), false).success && mutations == 1U,
                "missing profile configuration must preserve legacy behavior");
        }

        reset(1U);
        check(SetScmEntryEnabled(entry("UnrelatedDriver"), false).success && mutations == 1U,
            "PnP main driver mode must not block unrelated driver configuration");
        check(profileQueries == 0U, "unrelated service configuration must not query the main driver profile");

        reset();
        auto stale = entry();
        stale.actionLocator.serviceStartType = SERVICE_AUTO_START;
        const auto conflict = SetScmEntryEnabled(stale, false);
        check(conflict.status == ks::startup::StartupActionStatus::Conflict && mutations == 0U,
            "stale enumeration conflict must remain unchanged");

        for (const bool failRollback : { false, true })
        {
            reset();
            verificationMismatch = true;
            rollbackFailure = failRollback;
            const auto result = SetScmEntryEnabled(entry(), false);
            check(!result.success && result.rollbackAttempted && mutations == 2U,
                "legacy verification failure must still attempt the original rollback");
            check(result.rollbackSucceeded == !failRollback && result.changed == failRollback,
                "legacy rollback result must preserve changed-state reporting");
            check(result.status == (failRollback ? ks::startup::StartupActionStatus::RollbackFailed
                : ks::startup::StartupActionStatus::VerificationFailed),
                "legacy rollback outcome classification must remain unchanged");
        }
    }
}

LSTATUS WINAPI KsccStartupRegOpenKeyExW(HKEY, LPCWSTR path, DWORD, REGSAM access, PHKEY key)
{
    ++profileQueries;
    check(std::wcscmp(path, L"SYSTEM\\CurrentControlSet\\Services\\KswordARK\\Parameters") == 0,
        "profile guard must query the actual main driver Parameters key");
    check((access & KEY_SET_VALUE) == 0U, "profile guard must not write registry configuration");
    if (registryOpenError != ERROR_SUCCESS) return registryOpenError;
    *key = reinterpret_cast<HKEY>(static_cast<std::uintptr_t>(3U));
    return ERROR_SUCCESS;
}
LSTATUS WINAPI KsccStartupRegQueryValueExW(HKEY, LPCWSTR name, LPDWORD, LPDWORD type, LPBYTE value, LPDWORD bytes)
{
    check(std::wcscmp(name, L"StorageControllerPnP") == 0, "profile guard must query the explicit PnP setting");
    if (registryQueryError != ERROR_SUCCESS) return registryQueryError;
    *type = registryType;
    std::memcpy(value, &registryValue, sizeof(registryValue));
    *bytes = registryBytes;
    return ERROR_SUCCESS;
}
LSTATUS WINAPI KsccStartupRegCloseKey(HKEY) { return ERROR_SUCCESS; }
SC_HANDLE WINAPI KsccStartupOpenSCManagerW(LPCWSTR, LPCWSTR, DWORD)
{
    ++openHandles;
    if (flipScmOpen) flipProfile();
    return reinterpret_cast<SC_HANDLE>(static_cast<std::uintptr_t>(1U));
}
SC_HANDLE WINAPI KsccStartupOpenServiceW(SC_HANDLE, LPCWSTR name, DWORD)
{
    ++openHandles;
    openedService = name;
    if (flipServiceOpen) flipProfile();
    return reinterpret_cast<SC_HANDLE>(static_cast<std::uintptr_t>(2U));
}
BOOL WINAPI KsccStartupCloseServiceHandle(SC_HANDLE)
{
    check(openHandles != 0U, "SCM handle was closed twice");
    --openHandles;
    return TRUE;
}
BOOL WINAPI KsccStartupQueryServiceConfigW(SC_HANDLE, LPQUERY_SERVICE_CONFIGW config, DWORD, LPDWORD required)
{
    ++configQueries;
    if (configQueries == flipConfigQuery) flipProfile();
    *required = sizeof(QUERY_SERVICE_CONFIGW);
    if (verificationQueryFailure && configQueries >= 3U)
    {
        *required = 0U;
        ::SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (config == nullptr) { ::SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    *config = {};
    config->dwStartType = verificationMismatch && configQueries == 4U
        ? SERVICE_DEMAND_START : currentStartType;
    return TRUE;
}
BOOL WINAPI KsccStartupChangeServiceConfigW(SC_HANDLE, DWORD, DWORD startType, DWORD,
    LPCWSTR, LPCWSTR, LPDWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR)
{
    ++mutations;
    if (ksword::ark::isKswordArkService(openedService) &&
        !ksword::ark::queryServiceProfile().scmManagementAllowed())
        ++unsafeMutations;
    if (rollbackFailure && mutations == 2U) { ::SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    currentStartType = startType;
    return TRUE;
}

int main()
{
    try
    {
        protectedProfileTests();
        startupRaceTests();
        compatibilityTests();
        check(openHandles == 0U, "startup tests must release every SCM handle");
        std::cout << "Storage controller startup regression: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
