#include "ArkStorageControllerClient.h"

#include <SetupAPI.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <sstream>

#pragma comment(lib, "SetupAPI.lib")

namespace ksword::ark
{
    namespace
    {
        std::string formatWin32Failure(const char* operation, const unsigned long error)
        {
            std::ostringstream stream;
            stream << operation << " failed, error=" << error;
            return stream.str();
        }

        void rejectResponse(StorageControllerIoResult& io)
        {
            io.ok = false;
            io.win32Error = ERROR_INVALID_DATA;
            io.message = "invalid KswordARK storage controller response";
        }

        template<typename Response>
        bool validateResponse(StorageControllerIoResult& io, const Response& response,
            const std::size_t requiredBytes = sizeof(Response))
        {
            if (!io.ok) return false;
            if (io.bytesReturned != requiredBytes || response.size != requiredBytes ||
                response.version != KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION ||
                response.status > KSWORD_ARK_STORAGE_CONTROLLER_STATUS_RECOVERY_UNAVAILABLE)
            {
                rejectResponse(io);
                return false;
            }
            return true;
        }

        bool validRange(const std::uint32_t generation, const std::uint64_t session,
            const std::uint64_t offset, const std::size_t length,
            const std::uint32_t timeout)
        {
            return generation != 0U && session != 0ULL && length != 0U &&
                length <= KSWORD_ARK_STORAGE_CONTROLLER_MAX_TRANSFER_BYTES &&
                offset <= std::numeric_limits<std::uint64_t>::max() - length &&
                timeout <= 60000U;
        }

        void rejectRequest(StorageControllerIoResult& io)
        {
            io.win32Error = ERROR_INVALID_PARAMETER;
            io.message = "invalid KswordARK storage controller request";
        }

        void validateTransfer(StorageControllerTransferResult& result,
            const unsigned long operation, const std::uint32_t generation,
            const std::uint64_t session, const std::uint64_t offset,
            const std::uint32_t length, const std::size_t responseBytes)
        {
            if (!validateResponse(result.io, result.response, responseBytes)) return;
            const auto& response = result.response;
            const std::uint32_t completedGeneration = operation ==
                KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_READ ? generation
                : generation == std::numeric_limits<std::uint32_t>::max() ? 1U : generation + 1U;
            if (response.operation != operation || response.offset != offset ||
                response.bytesTransferred > length ||
                (response.status == KSWORD_ARK_STORAGE_CONTROLLER_STATUS_OK &&
                 (response.bytesTransferred != length || response.generation != completedGeneration ||
                  response.sessionId != session)))
            {
                rejectResponse(result.io);
            }
        }
    }

    ArkStorageControllerClient::~ArkStorageControllerClient()
    {
        close();
    }

    bool ArkStorageControllerClient::open(const std::uint32_t interfaceIndex)
    {
        static const GUID interfaceGuid =
            KSWORD_ARK_STORAGE_CONTROLLER_INTERFACE_GUID_INIT;
        close();
        const HDEVINFO deviceInfo = SetupDiGetClassDevsW(
            &interfaceGuid,
            nullptr,
            nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (deviceInfo == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        struct DeviceInfoLease
        {
            HDEVINFO value;
            ~DeviceInfoLease()
            {
                const DWORD error = GetLastError();
                SetupDiDestroyDeviceInfoList(value);
                SetLastError(error);
            }
        } deviceInfoLease{ deviceInfo };
        std::uint32_t matchingIndex = 0U;
        for (DWORD index = 0U; ; ++index)
        {
            SP_DEVICE_INTERFACE_DATA interfaceData{};
            interfaceData.cbSize = sizeof(interfaceData);
            if (SetupDiEnumDeviceInterfaces(deviceInfo, nullptr, &interfaceGuid,
                index, &interfaceData) == FALSE) return false;
            DWORD requiredBytes = 0U;
            const BOOL probe = SetupDiGetDeviceInterfaceDetailW(deviceInfo,
                &interfaceData, nullptr, 0U, &requiredBytes, nullptr);
            if (probe != FALSE || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
                requiredBytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) ||
                requiredBytes > 64U * 1024U)
            {
                SetLastError(ERROR_INVALID_DATA);
                return false;
            }
            std::vector<std::uint8_t> detailBuffer(requiredBytes, 0U);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
            detail->cbSize = sizeof(*detail);
            SP_DEVINFO_DATA deviceData{};
            deviceData.cbSize = sizeof(deviceData);
            if (!SetupDiGetDeviceInterfaceDetailW(deviceInfo, &interfaceData,
                detail, requiredBytes, nullptr, &deviceData)) return false;

            // Old standalone driver packages used this interface GUID too.
            // Only interfaces bound to the unified KswordARK service are eligible.
            wchar_t serviceName[64]{};
            DWORD type = 0U;
            DWORD serviceBytes = 0U;
            if (!SetupDiGetDeviceRegistryPropertyW(deviceInfo, &deviceData, SPDRP_SERVICE,
                &type, reinterpret_cast<BYTE*>(serviceName), sizeof(serviceName), &serviceBytes) ||
                type != REG_SZ || serviceBytes < sizeof(wchar_t) ||
                serviceBytes > sizeof(serviceName) || serviceBytes % sizeof(wchar_t) != 0U ||
                std::find(serviceName, serviceName + serviceBytes / sizeof(wchar_t), L'\0') ==
                    serviceName + serviceBytes / sizeof(wchar_t) ||
                _wcsicmp(serviceName, L"KswordARK") != 0)
                continue;
            if (matchingIndex++ != interfaceIndex) continue;
            const std::size_t pathChars = (requiredBytes -
                FIELD_OFFSET(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath)) / sizeof(wchar_t);
            if (std::find(detail->DevicePath, detail->DevicePath + pathChars, L'\0') ==
                detail->DevicePath + pathChars)
            {
                SetLastError(ERROR_INVALID_DATA);
                return false;
            }
            m_handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                0U, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (isOpen()) SetLastError(ERROR_SUCCESS);
            return isOpen();
        }
    }

    void ArkStorageControllerClient::close()
    {
        if (isOpen())
        {
            CloseHandle(m_handle);
        }
        m_handle = INVALID_HANDLE_VALUE;
    }

    bool ArkStorageControllerClient::isOpen() const noexcept
    {
        return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr;
    }

    StorageControllerIoResult ArkStorageControllerClient::invoke(
        const unsigned long ioctl,
        void* const input,
        const unsigned long inputBytes,
        void* const output,
        const unsigned long outputBytes) const
    {
        StorageControllerIoResult result;
        if (!isOpen())
        {
            result.win32Error = ERROR_INVALID_HANDLE;
            result.message = "KswordARK storage controller interface is not open";
            return result;
        }
        DWORD bytesReturned = 0U;
        result.ok = DeviceIoControl(
            m_handle,
            ioctl,
            input,
            inputBytes,
            output,
            outputBytes,
            &bytesReturned,
            nullptr) != FALSE;
        result.bytesReturned = bytesReturned;
        result.win32Error = result.ok ? ERROR_SUCCESS : GetLastError();
        if (!result.ok)
        {
            result.message = formatWin32Failure("DeviceIoControl", result.win32Error);
        }
        else if (bytesReturned > outputBytes)
        {
            rejectResponse(result);
        }
        return result;
    }

    StorageControllerQueryResult ArkStorageControllerClient::query() const
    {
        StorageControllerQueryResult result;
        KSWORD_ARK_QUERY_STORAGE_CONTROLLER_REQUEST request{};
        request.version = KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION;
        request.size = sizeof(request);
        result.io = invoke(
            IOCTL_KSWORD_ARK_QUERY_STORAGE_CONTROLLER,
            &request,
            sizeof(request),
            &result.response,
            sizeof(result.response));
        if (validateResponse(result.io, result.response))
        {
            const auto& response = result.response;
            if (response.controllerType > KSWORD_ARK_STORAGE_CONTROLLER_TYPE_IDE ||
                response.ownership > KSWORD_ARK_STORAGE_OWNERSHIP_EXCLUSIVE ||
                response.coherency > KSWORD_ARK_STORAGE_COHERENCY_EXCLUSIVE ||
                std::find(std::begin(response.model), std::end(response.model), L'\0') == std::end(response.model) ||
                std::find(std::begin(response.serial), std::end(response.serial), L'\0') == std::end(response.serial) ||
                std::find(std::begin(response.detail), std::end(response.detail), L'\0') == std::end(response.detail))
            {
                rejectResponse(result.io);
            }
        }
        return result;
    }

    StorageControllerControlResult ArkStorageControllerClient::acquire(
        const std::uint32_t expectedGeneration,
        const std::uint32_t timeoutMilliseconds) const
    {
        StorageControllerControlResult result;
        if (expectedGeneration == 0U || timeoutMilliseconds > 60000U)
        {
            rejectRequest(result.io);
            return result;
        }
        KSWORD_ARK_CONTROL_STORAGE_CONTROLLER_REQUEST request{};
        request.version = KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION;
        request.size = sizeof(request);
        request.command = KSWORD_ARK_STORAGE_CONTROLLER_CONTROL_ACQUIRE;
        request.flags =
            KSWORD_ARK_STORAGE_CONTROLLER_CONTROL_FLAG_EXCLUSIVE |
            KSWORD_ARK_STORAGE_CONTROLLER_CONTROL_FLAG_UI_CONFIRMED;
        request.expectedGeneration = expectedGeneration;
        request.confirmationToken = KSWORD_ARK_STORAGE_CONTROLLER_CONFIRMATION_TOKEN;
        request.timeoutMilliseconds = timeoutMilliseconds;
        result.io = invoke(
            IOCTL_KSWORD_ARK_CONTROL_STORAGE_CONTROLLER,
            &request,
            sizeof(request),
            &result.response,
            sizeof(result.response));
        if (validateResponse(result.io, result.response) &&
            result.response.status == KSWORD_ARK_STORAGE_CONTROLLER_STATUS_OK &&
            (result.response.sessionId == 0ULL || result.response.newGeneration == 0U ||
             result.response.ownership != KSWORD_ARK_STORAGE_OWNERSHIP_EXCLUSIVE ||
             result.response.coherency != KSWORD_ARK_STORAGE_COHERENCY_EXCLUSIVE))
        {
            rejectResponse(result.io);
        }
        return result;
    }

    StorageControllerControlResult ArkStorageControllerClient::release(
        const std::uint32_t expectedGeneration,
        const std::uint64_t sessionId) const
    {
        StorageControllerControlResult result;
        if (expectedGeneration == 0U || sessionId == 0ULL)
        {
            rejectRequest(result.io);
            return result;
        }
        KSWORD_ARK_CONTROL_STORAGE_CONTROLLER_REQUEST request{};
        request.version = KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION;
        request.size = sizeof(request);
        request.command = KSWORD_ARK_STORAGE_CONTROLLER_CONTROL_RELEASE;
        request.expectedGeneration = expectedGeneration;
        request.expectedSessionId = sessionId;
        result.io = invoke(
            IOCTL_KSWORD_ARK_CONTROL_STORAGE_CONTROLLER,
            &request,
            sizeof(request),
            &result.response,
            sizeof(result.response));
        if (validateResponse(result.io, result.response) &&
            result.response.status == KSWORD_ARK_STORAGE_CONTROLLER_STATUS_OK &&
            result.response.sessionId != 0ULL)
        {
            rejectResponse(result.io);
        }
        return result;
    }

    StorageControllerControlResult ArkStorageControllerClient::reset(
        const std::uint32_t expectedGeneration,
        const std::uint64_t sessionId,
        const std::uint32_t timeoutMilliseconds) const
    {
        StorageControllerControlResult result;
        if (expectedGeneration == 0U || timeoutMilliseconds > 60000U)
        {
            rejectRequest(result.io);
            return result;
        }
        KSWORD_ARK_CONTROL_STORAGE_CONTROLLER_REQUEST request{};
        request.version = KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION;
        request.size = sizeof(request);
        request.command = KSWORD_ARK_STORAGE_CONTROLLER_CONTROL_RESET;
        request.flags =
            KSWORD_ARK_STORAGE_CONTROLLER_CONTROL_FLAG_EXCLUSIVE |
            KSWORD_ARK_STORAGE_CONTROLLER_CONTROL_FLAG_UI_CONFIRMED;
        request.expectedGeneration = expectedGeneration;
        request.confirmationToken = KSWORD_ARK_STORAGE_CONTROLLER_CONFIRMATION_TOKEN;
        request.timeoutMilliseconds = timeoutMilliseconds;
        request.expectedSessionId = sessionId;
        result.io = invoke(
            IOCTL_KSWORD_ARK_CONTROL_STORAGE_CONTROLLER,
            &request,
            sizeof(request),
            &result.response,
            sizeof(result.response));
        validateResponse(result.io, result.response);
        return result;
    }

    StorageControllerTransferResult ArkStorageControllerClient::read(
        const std::uint32_t expectedGeneration,
        const std::uint64_t sessionId,
        const std::uint64_t offset,
        const std::uint32_t length,
        const std::uint32_t timeoutMilliseconds) const
    {
        StorageControllerTransferResult result;
        if (!validRange(expectedGeneration, sessionId, offset, length, timeoutMilliseconds))
        {
            result.io.win32Error = ERROR_INVALID_PARAMETER;
            result.io.message = "read length is outside the protocol boundary";
            return result;
        }
        const std::size_t headerBytes =
            KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE_HEADER_SIZE;
        if (static_cast<std::size_t>(length) >
                std::numeric_limits<std::size_t>::max() - headerBytes ||
            headerBytes + static_cast<std::size_t>(length) >
                std::numeric_limits<unsigned long>::max())
        {
            result.io.win32Error = ERROR_ARITHMETIC_OVERFLOW;
            result.io.message = "read response size overflows the transport";
            return result;
        }
        KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST request{};
        request.version = KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION;
        request.size = KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST_HEADER_SIZE;
        request.operation = KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_READ;
        request.expectedGeneration = expectedGeneration;
        request.length = length;
        request.timeoutMilliseconds = timeoutMilliseconds;
        request.sessionId = sessionId;
        request.offset = offset;
        const std::size_t responseBytes =
            headerBytes + static_cast<std::size_t>(length);
        std::vector<std::uint8_t> responseBuffer(responseBytes, 0U);
        result.io = invoke(
            IOCTL_KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER,
            &request,
            KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST_HEADER_SIZE,
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (result.io.ok && result.io.bytesReturned == responseBytes)
        {
            const auto* response =
                reinterpret_cast<const KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE*>(
                    responseBuffer.data());
            std::memcpy(
                &result.response,
                response,
                KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE_HEADER_SIZE);
            validateTransfer(result, request.operation, expectedGeneration, sessionId,
                offset, length, responseBytes);
            if (result.io.ok && result.response.status == KSWORD_ARK_STORAGE_CONTROLLER_STATUS_OK)
                result.bytes.assign(response->data, response->data + length);
        }
        else if (result.io.ok) rejectResponse(result.io);
        return result;
    }

    StorageControllerTransferResult ArkStorageControllerClient::write(
        const std::uint32_t expectedGeneration,
        const std::uint64_t sessionId,
        const std::uint64_t offset,
        const std::vector<std::uint8_t>& bytes,
        const std::vector<std::uint8_t>& expectedBeforeHash,
        const std::uint32_t flags,
        const std::uint32_t timeoutMilliseconds) const
    {
        StorageControllerTransferResult result;
        if (!validRange(expectedGeneration, sessionId, offset, bytes.size(), timeoutMilliseconds) ||
            expectedBeforeHash.size() != KSWORD_ARK_STORAGE_CONTROLLER_HASH_BYTES ||
            (flags & ~(KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_FUA |
                       KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_FLUSH |
                       KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_VERIFY)) != 0U)
        {
            result.io.win32Error = ERROR_INVALID_PARAMETER;
            result.io.message = "write payload is outside the protocol boundary";
            return result;
        }
        const std::size_t requestBytes =
            KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST_HEADER_SIZE +
            bytes.size();
        std::vector<std::uint8_t> requestBuffer(requestBytes, 0U);
        auto* request =
            reinterpret_cast<KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST*>(
                requestBuffer.data());
        request->version = KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION;
        request->size = static_cast<unsigned long>(requestBytes);
        request->operation = KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_WRITE;
        request->flags =
            flags |
            KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_UI_CONFIRMED |
            KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_VERIFY |
            KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_FLUSH;
        request->expectedGeneration = expectedGeneration;
        request->length = static_cast<unsigned long>(bytes.size());
        request->confirmationToken = KSWORD_ARK_STORAGE_CONTROLLER_CONFIRMATION_TOKEN;
        request->timeoutMilliseconds = timeoutMilliseconds;
        request->sessionId = sessionId;
        request->offset = offset;
        if (expectedBeforeHash.size() ==
            KSWORD_ARK_STORAGE_CONTROLLER_HASH_BYTES)
        {
            std::memcpy(
                request->expectedBeforeHash,
                expectedBeforeHash.data(),
                expectedBeforeHash.size());
            request->flags |=
                KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_EXPECT_BEFORE_HASH;
        }
        std::memcpy(request->data, bytes.data(), bytes.size());
        result.io = invoke(
            IOCTL_KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER,
            requestBuffer.data(),
            static_cast<unsigned long>(requestBuffer.size()),
            &result.response,
            KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE_HEADER_SIZE);
        validateTransfer(result, request->operation, expectedGeneration, sessionId,
            offset, static_cast<std::uint32_t>(bytes.size()),
            KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE_HEADER_SIZE);
        return result;
    }

    StorageControllerTransferResult ArkStorageControllerClient::rollback(
        const std::uint32_t expectedGeneration,
        const std::uint64_t sessionId,
        const std::uint64_t offset,
        const std::uint32_t length,
        const std::uint32_t timeoutMilliseconds) const
    {
        StorageControllerTransferResult result;
        if (!validRange(expectedGeneration, sessionId, offset, length, timeoutMilliseconds))
        {
            rejectRequest(result.io);
            return result;
        }
        KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST request{};
        request.version = KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION;
        request.size = KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST_HEADER_SIZE;
        request.operation = KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_ROLLBACK;
        request.flags =
            KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_UI_CONFIRMED |
            KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_FLUSH |
            KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_VERIFY;
        request.expectedGeneration = expectedGeneration;
        request.length = length;
        request.confirmationToken = KSWORD_ARK_STORAGE_CONTROLLER_CONFIRMATION_TOKEN;
        request.timeoutMilliseconds = timeoutMilliseconds;
        request.sessionId = sessionId;
        request.offset = offset;
        result.io = invoke(
            IOCTL_KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER,
            &request,
            KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST_HEADER_SIZE,
            &result.response,
            KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE_HEADER_SIZE);
        validateTransfer(result, request.operation, expectedGeneration, sessionId,
            offset, length, KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE_HEADER_SIZE);
        return result;
    }

    StorageControllerAuditResult ArkStorageControllerClient::queryAudit(
        const std::uint64_t afterSequence) const
    {
        StorageControllerAuditResult result;
        KSWORD_ARK_QUERY_STORAGE_CONTROLLER_AUDIT_REQUEST request{};
        request.version = KSWORD_ARK_STORAGE_CONTROLLER_PROTOCOL_VERSION;
        request.size = sizeof(request);
        request.maximumRows = KSWORD_ARK_STORAGE_CONTROLLER_AUDIT_ROWS;
        request.afterSequence = afterSequence;
        result.io = invoke(
            IOCTL_KSWORD_ARK_QUERY_STORAGE_CONTROLLER_AUDIT,
            &request,
            sizeof(request),
            &result.response,
            sizeof(result.response));
        if (validateResponse(result.io, result.response) &&
            (result.response.rowCount > KSWORD_ARK_STORAGE_CONTROLLER_AUDIT_ROWS ||
             result.response.rowCount > result.response.availableCount ||
             result.response.truncated > 1U))
        {
            rejectResponse(result.io);
        }
        return result;
    }
}
