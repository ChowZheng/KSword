#include "storage_controller_transport_stubs.h"
#include "../Ksword5.1/Ksword5.1/ArkDriverClient/ArkStorageControllerClient.h"
#include "../Ksword5.1/Ksword5.1/MiscDock/DiskEditor/StorageControllerInput.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    std::function<void(DWORD, void*, DWORD, DWORD&)> mutateResponse;
    KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST capturedTransfer{};
    std::vector<std::uint8_t> capturedPayload;
    unsigned int checks = 0U;
    unsigned int transportCalls = 0U;
    unsigned int closeCalls = 0U;
    bool transportFails = false;
    bool enumerationFails = false;
    bool legacyInterfaceFirst = false;
    bool servicePropertyFails = false;
    bool servicePropertyMalformed = false;
    unsigned int currentInterface = 0U;
    DWORD detailProbeBytes = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 128U;

    void check(const bool value, const char* message)
    {
        ++checks;
        if (!value) throw std::runtime_error(message);
    }

    std::uint32_t advance(const std::uint32_t generation)
    {
        return generation == std::numeric_limits<std::uint32_t>::max() ? 1U : generation + 1U;
    }

    void protocolTests()
    {
        using ksword::ark::ArkStorageControllerClient;
        ArkStorageControllerClient client;
        check(!client.query().io.ok, "closed handle query must fail");
        check(transportCalls == 0U, "closed handle must never issue IOCTL");
        check(client.open(), "enumerated interface must open");
        check(GetLastError() == ERROR_SUCCESS, "SetupAPI cleanup must not overwrite open success");
        check(client.query().io.ok, "valid query must parse");
        for (const auto& field : {0U, 1U, 2U, 3U, 4U, 5U})
        {
            mutateResponse = [field](DWORD, void* output, DWORD bytes, DWORD& returned)
            {
                auto& response = *static_cast<KSWORD_ARK_QUERY_STORAGE_CONTROLLER_RESPONSE*>(output);
                if (field == 0U) --returned;
                if (field == 1U) returned = bytes + 1U;
                if (field == 2U) ++response.version;
                if (field == 3U) --response.size;
                if (field == 4U) response.status = 0xffffffffUL;
                if (field == 5U) std::fill(std::begin(response.model), std::end(response.model), L'X');
            };
            const auto result = client.query();
            check(!result.io.ok && result.io.win32Error == ERROR_INVALID_DATA,
                "malformed query must fail closed");
        }
        mutateResponse = [](DWORD, void* output, DWORD, DWORD&)
        {
            auto& response = *static_cast<KSWORD_ARK_QUERY_STORAGE_CONTROLLER_RESPONSE*>(output);
            response.status = KSWORD_ARK_STORAGE_CONTROLLER_STATUS_NOT_READY;
        };
        check(client.query().io.ok, "valid not-ready query must retain reset identity");
        mutateResponse = {};

        const auto acquire = client.acquire(7U);
        check(acquire.io.ok && acquire.response.sessionId == 0x1234ULL, "acquire reply must parse");
        check(client.release(8U, 0x1234ULL).io.ok, "release reply must parse");
        check(client.reset(7U, 0ULL).io.ok, "sessionless recovery reset must be permitted");
        mutateResponse = [](DWORD, void*, DWORD, DWORD& returned) { returned = 0U; };
        check(!client.acquire(7U).io.ok, "empty acquire reply must fail");
        check(!client.release(7U, 0x1234ULL).io.ok, "empty release reply must fail");
        check(!client.reset(7U, 0x1234ULL).io.ok, "empty reset reply must fail");
        mutateResponse = {};

        const auto read = client.read(7U, 0x1234ULL, 512ULL, 512U);
        check(read.io.ok && read.bytes.size() == 512U, "complete read must parse");
        check(read.bytes[0] == 0U && read.bytes[511] == 255U, "read must preserve payload");
        for (unsigned int field = 0U; field < 8U; ++field)
        {
            mutateResponse = [field](DWORD, void* output, DWORD bytes, DWORD& returned)
            {
                auto& response = *static_cast<KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE*>(output);
                if (field == 0U) returned = KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE_HEADER_SIZE - 1U;
                if (field == 1U) returned = bytes + 1U;
                if (field == 2U) ++response.bytesTransferred;
                if (field == 3U) --response.bytesTransferred;
                if (field == 4U) ++response.operation;
                if (field == 5U) ++response.offset;
                if (field == 6U) ++response.generation;
                if (field == 7U) ++response.sessionId;
            };
            const auto invalid = client.read(7U, 0x1234ULL, 512ULL, 512U);
            check(!invalid.io.ok && invalid.bytes.empty(), "malformed read must never establish snapshot");
        }
        mutateResponse = [](DWORD, void* output, DWORD, DWORD&)
        {
            auto& response = *static_cast<KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE*>(output);
            response.status = KSWORD_ARK_STORAGE_CONTROLLER_STATUS_IO_FAILED;
            response.bytesTransferred = 0U;
        };
        const auto failedRead = client.read(7U, 0x1234ULL, 512ULL, 512U);
        check(failedRead.io.ok && failedRead.bytes.empty(), "failed read must expose status without fabricated bytes");
        mutateResponse = {};

        std::vector<std::uint8_t> payload(512U, 0xabU);
        std::vector<std::uint8_t> hash(KSWORD_ARK_STORAGE_CONTROLLER_HASH_BYTES, 0x34U);
        const auto write = client.write(7U, 0x1234ULL, 512ULL, payload, hash, 0U);
        check(write.io.ok && write.response.generation == 8U, "write must accept advanced generation");
        check(capturedPayload == payload, "write must preserve exact payload");
        const ULONG requiredFlags = KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_UI_CONFIRMED |
            KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_EXPECT_BEFORE_HASH |
            KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_VERIFY |
            KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_FLUSH;
        check((capturedTransfer.flags & requiredFlags) == requiredFlags,
            "write must require confirmation, hash, flush and verification");
        check(std::memcmp(capturedTransfer.expectedBeforeHash, hash.data(), hash.size()) == 0,
            "write must transport the exact condition hash");
        check(client.rollback(8U, 0x1234ULL, 512ULL, 512U).io.ok, "rollback must accept advanced generation");
        check(client.write(0xffffffffU, 0x1234ULL, 512ULL, payload, hash, 0U).io.ok,
            "mutation generation wrap must skip zero");
        mutateResponse = [](DWORD, void* output, DWORD, DWORD&)
        {
            auto& response = *static_cast<KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE*>(output);
            --response.generation;
        };
        check(!client.write(7U, 0x1234ULL, 512ULL, payload, hash, 0U).io.ok,
            "successful write without generation advance must fail");
        check(!client.rollback(8U, 0x1234ULL, 512ULL, 512U).io.ok,
            "successful rollback without generation advance must fail");
        mutateResponse = {};

        const unsigned int callsBeforeInvalid = transportCalls;
        check(!client.read(7U, 0x1234ULL, 0ULL, 0U).io.ok, "zero read length must fail");
        check(!client.read(7U, 0ULL, 0ULL, 512U).io.ok, "missing session must fail");
        check(!client.read(7U, 0x1234ULL, std::numeric_limits<std::uint64_t>::max(), 512U).io.ok,
            "overflowing range must fail");
        check(!client.read(7U, 0x1234ULL, 0ULL, 512U, 60001U).io.ok, "unbounded timeout must fail");
        check(!client.rollback(7U, 0x1234ULL, 0ULL, 0U).io.ok, "empty rollback must fail");
        check(!client.write(7U, 0x1234ULL, 0ULL, payload, {}, 0U).io.ok,
            "write without snapshot hash must fail");
        check(!client.write(7U, 0x1234ULL, 0ULL, payload, hash,
            KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_FORCE).io.ok, "unsafe force write must fail");
        check(transportCalls == callsBeforeInvalid, "invalid requests must never touch transport");

        check(client.queryAudit().io.ok, "bounded audit reply must parse");
        mutateResponse = [](DWORD, void* output, DWORD, DWORD&)
        {
            auto& response = *static_cast<KSWORD_ARK_QUERY_STORAGE_CONTROLLER_AUDIT_RESPONSE*>(output);
            response.rowCount = KSWORD_ARK_STORAGE_CONTROLLER_AUDIT_ROWS + 1U;
        };
        check(!client.queryAudit().io.ok, "oversized audit row count must fail before indexing");
        mutateResponse = {};
        transportFails = true;
        const auto disconnected = client.read(7U, 0x1234ULL, 0ULL, 512U);
        check(!disconnected.io.ok && disconnected.io.win32Error == ERROR_DEVICE_NOT_CONNECTED &&
            disconnected.bytes.empty(), "transport failure must retain Win32 error without payload");
        transportFails = false;
        client.close();
        check(!client.isOpen(), "close must clear handle");
        enumerationFails = true;
        check(!client.open() && GetLastError() == ERROR_NO_MORE_ITEMS,
            "enumeration error must survive SetupAPI cleanup");
        enumerationFails = false;
        detailProbeBytes = 1024U * 1024U;
        check(!client.open(), "unbounded interface detail allocation must fail");
        detailProbeBytes = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 128U;
        legacyInterfaceFirst = true;
        check(client.open(), "old standalone binding must be skipped for main-driver interface");
        client.close();
        legacyInterfaceFirst = false;
        servicePropertyFails = true;
        check(!client.open(), "service property failure must not open an unidentified interface");
        servicePropertyFails = false;
        servicePropertyMalformed = true;
        check(!client.open(), "nonterminated service property must fail closed");
        servicePropertyMalformed = false;
        check(!client.open(1U), "interface selection must count only main-driver bindings");
    }

    void inputTests()
    {
        using ks::misc::detail::parseControllerHex;
        std::vector<std::uint8_t> bytes;
        check(parseControllerHex(L"00 11\r\n22\tff", 4U, bytes) &&
            bytes == std::vector<std::uint8_t>({0U, 0x11U, 0x22U, 0xffU}), "valid HEX must preserve all bytes");
        for (const auto* input : {L"00112zff", L"001122f", L"001122fff", L"0x001122ff", L"0011\u4e0022ff"})
            check(!parseControllerHex(input, 4U, bytes) && bytes.empty(), "malformed HEX must be rejected entirely");
        for (unsigned int value = 0U; value < 256U; ++value)
        {
            static const wchar_t digits[] = L"0123456789ABCDEF";
            std::wstring text;
            text += digits[value >> 4U];
            text += digits[value & 15U];
            check(parseControllerHex(text, 1U, bytes) && bytes[0] == value, "HEX byte roundtrip must succeed");
        }
        check(!parseControllerHex(L"00", 0U, bytes), "empty declared payload must fail");
        check(!parseControllerHex(std::wstring(2000U, L' '), 1U, bytes), "oversized HEX text must fail");
    }
}

BOOL WINAPI KsccTestDeviceIoControl(HANDLE, DWORD ioctl, LPVOID input, DWORD inputBytes,
    LPVOID output, DWORD outputBytes, LPDWORD returned, LPOVERLAPPED)
{
    ++transportCalls;
    if (transportFails)
    {
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        *returned = 0U;
        return FALSE;
    }
    std::memset(output, 0, outputBytes);
    *returned = outputBytes;
    if (ioctl == IOCTL_KSWORD_ARK_QUERY_STORAGE_CONTROLLER)
    {
        auto& response = *static_cast<KSWORD_ARK_QUERY_STORAGE_CONTROLLER_RESPONSE*>(output);
        response.version = KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION;
        response.size = outputBytes;
        response.generation = 7U;
        response.ownership = KSWORD_ARK_STORAGE_OWNERSHIP_EXCLUSIVE;
        response.coherency = KSWORD_ARK_STORAGE_COHERENCY_EXCLUSIVE;
    }
    else if (ioctl == IOCTL_KSWORD_ARK_CONTROL_STORAGE_CONTROLLER)
    {
        const auto& request = *static_cast<KSWORD_ARK_CONTROL_STORAGE_CONTROLLER_REQUEST*>(input);
        auto& response = *static_cast<KSWORD_ARK_CONTROL_STORAGE_CONTROLLER_RESPONSE*>(output);
        response.version = KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION;
        response.size = outputBytes;
        response.oldGeneration = request.expectedGeneration;
        response.newGeneration = advance(request.expectedGeneration);
        response.ownership = KSWORD_ARK_STORAGE_OWNERSHIP_EXCLUSIVE;
        response.coherency = KSWORD_ARK_STORAGE_COHERENCY_EXCLUSIVE;
        response.sessionId = request.command == KSWORD_ARK_STORAGE_CONTROLLER_CONTROL_ACQUIRE ? 0x1234ULL : 0ULL;
    }
    else if (ioctl == IOCTL_KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER)
    {
        const auto& request = *static_cast<KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST*>(input);
        std::memcpy(&capturedTransfer, input, KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST_HEADER_SIZE);
        capturedPayload.clear();
        if (request.operation == KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_WRITE)
        {
            check(inputBytes == static_cast<DWORD>(KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST_HEADER_SIZE) + request.length,
                "write envelope must have exact payload size");
            capturedPayload.assign(request.data, request.data + request.length);
        }
        auto& response = *static_cast<KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE*>(output);
        response.version = KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION;
        response.size = outputBytes;
        response.operation = request.operation;
        response.bytesTransferred = request.length;
        response.generation = request.operation == KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_READ
            ? request.expectedGeneration : advance(request.expectedGeneration);
        response.sessionId = request.sessionId;
        response.offset = request.offset;
        if (request.operation == KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_READ)
            for (ULONG index = 0U; index < request.length; ++index) response.data[index] = static_cast<std::uint8_t>(index);
    }
    else if (ioctl == IOCTL_KSWORD_ARK_QUERY_STORAGE_CONTROLLER_AUDIT)
    {
        auto& response = *static_cast<KSWORD_ARK_QUERY_STORAGE_CONTROLLER_AUDIT_RESPONSE*>(output);
        response.version = KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION;
        response.size = outputBytes;
    }
    else return FALSE;
    if (mutateResponse) mutateResponse(ioctl, output, outputBytes, *returned);
    return TRUE;
}

BOOL WINAPI KsccTestCloseHandle(HANDLE) { ++closeCalls; return TRUE; }
HANDLE WINAPI KsccTestCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)
{
    check(std::wcscmp(path, L"test interface") == 0, "client must open the enumerated controller interface");
    check(access == (GENERIC_READ | GENERIC_WRITE) && share == 0U, "controller handle must be exclusive");
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x1234U));
}
HDEVINFO WINAPI KsccTestSetupDiGetClassDevsW(const GUID* guid, PCWSTR, HWND, DWORD)
{
    static const GUID expected = KSWORD_ARK_STORAGE_CONTROLLER_INTERFACE_GUID_INIT;
    check(std::memcmp(guid, &expected, sizeof(expected)) == 0, "client must enumerate controller interface GUID");
    return reinterpret_cast<HDEVINFO>(static_cast<std::uintptr_t>(1U));
}
BOOL WINAPI KsccTestSetupDiEnumDeviceInterfaces(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD index, PSP_DEVICE_INTERFACE_DATA)
{
    if (enumerationFails || index >= (legacyInterfaceFirst ? 2U : 1U))
    { SetLastError(ERROR_NO_MORE_ITEMS); return FALSE; }
    currentInterface = index;
    return TRUE;
}
BOOL WINAPI KsccTestSetupDiGetDeviceInterfaceDetailW(HDEVINFO, PSP_DEVICE_INTERFACE_DATA,
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail, DWORD, PDWORD required, PSP_DEVINFO_DATA)
{
    if (!detail)
    {
        if (required) *required = detailProbeBytes;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::wcscpy(detail->DevicePath, L"test interface");
    return TRUE;
}
BOOL WINAPI KsccTestSetupDiDestroyDeviceInfoList(HDEVINFO)
{ SetLastError(ERROR_BAD_COMMAND); return TRUE; }
BOOL WINAPI KsccTestSetupDiGetDeviceRegistryPropertyW(HDEVINFO, PSP_DEVINFO_DATA,
    DWORD property, PDWORD type, PBYTE buffer, DWORD bufferBytes, PDWORD returned)
{
    check(property == SPDRP_SERVICE, "service owner must be verified");
    if (servicePropertyFails) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    const wchar_t* service = legacyInterfaceFirst && currentInterface == 0U
        ? L"KswordARKController" : L"KswordARK";
    const DWORD bytes = static_cast<DWORD>((std::wcslen(service) + 1U) * sizeof(wchar_t));
    if (bufferBytes < bytes) return FALSE;
    std::memcpy(buffer, service, bytes);
    *type = REG_SZ;
    *returned = servicePropertyMalformed ? bytes - sizeof(wchar_t) : bytes;
    return TRUE;
}

int main()
{
    try
    {
        protocolTests();
        inputTests();
        check(closeCalls > 0U, "client destruction and close must release handle leases");
        std::cout << "Storage controller client/input regression: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
