#include "ArkDriverClient.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <vector>

namespace ksword::ark
{
    namespace
    {
        struct RegistryStringArgument
        {
            const std::wstring& text;
            std::size_t capacity;
        };

        template <typename Result>
        bool validateRegistryRequest(
            Result& result,
            const std::uint32_t failedStatus,
            const std::initializer_list<RegistryStringArgument> strings,
            const std::size_t dataBytes = 0U)
        {
            // 定长协议必须完整表达目标与数据；拒绝截断，防止写入另一个键或值。
            unsigned long error = ERROR_SUCCESS;
            const char* message = nullptr;
            for (const auto& argument : strings)
            {
                if (argument.text.find(L'\0') != std::wstring::npos)
                {
                    error = ERROR_INVALID_NAME;
                    message = "registry path or name contains an embedded NUL";
                    break;
                }
                if (argument.text.size() >= argument.capacity)
                {
                    error = ERROR_FILENAME_EXCED_RANGE;
                    message = "registry path or name exceeds the R0 protocol capacity";
                    break;
                }
            }
            if (error == ERROR_SUCCESS && dataBytes > KSWORD_ARK_REGISTRY_DATA_MAX_BYTES)
            {
                error = ERROR_INSUFFICIENT_BUFFER;
                message = "registry value data exceeds the R0 protocol capacity (4096 bytes)";
            }
            if (error == ERROR_SUCCESS)
            {
                return true;
            }

            result.status = failedStatus;
            result.io.ok = false;
            result.io.win32Error = error;
            result.io.message = message;
            return false;
        }

        void copyRegistryWideToFixed(
            wchar_t* destination,
            const std::size_t destinationChars,
            const std::wstring& source)
        {
            // 作用：把 R3 路径/值名复制到共享协议定长 WCHAR 数组。
            // 前置条件：validateRegistryRequest 已确认可完整存入并保留 NUL 结尾。
            if (destination == nullptr || destinationChars == 0U)
            {
                return;
            }

            std::fill(destination, destination + destinationChars, L'\0');
            const std::size_t copyChars = source.size();
            if (copyChars != 0U)
            {
                std::copy(source.data(), source.data() + copyChars, destination);
            }
            destination[copyChars] = L'\0';
        }

        std::wstring registryFixedWideToString(
            const wchar_t* source,
            const std::size_t sourceChars)
        {
            // 作用：把 R0 固定 WCHAR 数组转换为 std::wstring。
            // 返回：遇到 NUL 或达到上限后得到的字符串。
            if (source == nullptr || sourceChars == 0U)
            {
                return {};
            }

            std::size_t length = 0U;
            while (length < sourceChars && source[length] != L'\0')
            {
                ++length;
            }
            return std::wstring(source, source + length);
        }

        RegistryOperationResult parseRegistryOperationResponse(
            IoResult ioResult,
            const KSWORD_ARK_REGISTRY_OPERATION_RESPONSE& response,
            const char* operationName)
        {
            // 作用：把通用 R0 注册表写操作响应转换为 R3 模型。
            // 返回：RegistryOperationResult，失败时保留 DeviceIoControl 详情。
            RegistryOperationResult result{};
            result.io = std::move(ioResult);
            if (!result.io.ok)
            {
                result.io.message =
                    std::string("DeviceIoControl(") + operationName + ") failed, error=" +
                    std::to_string(result.io.win32Error);
                return result;
            }
            if (result.io.bytesReturned < sizeof(response) ||
                response.version != KSWORD_ARK_REGISTRY_PROTOCOL_VERSION)
            {
                result.status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED;
                result.io.ok = false;
                result.io.win32Error = ERROR_INVALID_DATA;
                result.io.message =
                    std::string("registry operation response has invalid size or version, bytesReturned=") +
                    std::to_string(result.io.bytesReturned);
                return result;
            }

            result.version = static_cast<std::uint32_t>(response.version);
            result.status = static_cast<std::uint32_t>(response.status);
            result.lastStatus = static_cast<long>(response.lastStatus);
            result.io.ntStatus = result.lastStatus;

            std::ostringstream stream;
            stream << "status=" << result.status
                << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(result.lastStatus);
            result.io.message = stream.str();
            return result;
        }
    }

    RegistryReadResult DriverClient::readRegistryValue(
        const std::wstring& kernelKeyPath,
        const std::wstring& valueName,
        const unsigned long maxDataBytes) const
    {
        // 作用：读取 R0 注册表值，路径必须是 \REGISTRY\... 内核路径。
        // 返回：RegistryReadResult；结构化失败也会携带 R0 status/lastStatus。
        RegistryReadResult readResult{};
        if (!validateRegistryRequest(readResult, KSWORD_ARK_REGISTRY_READ_STATUS_FAILED,
            {{kernelKeyPath, KSWORD_ARK_REGISTRY_PATH_CHARS},
             {valueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS}}))
        {
            return readResult;
        }
        KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST request{};
        KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        request.maxDataBytes = maxDataBytes;
        copyRegistryWideToFixed(
            request.keyPath,
            KSWORD_ARK_REGISTRY_PATH_CHARS,
            kernelKeyPath);
        if (!valueName.empty())
        {
            request.flags |= KSWORD_ARK_REGISTRY_READ_FLAG_VALUE_NAME_PRESENT;
            copyRegistryWideToFixed(
                request.valueName,
                KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS,
                valueName);
        }

        readResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_READ_REGISTRY_VALUE,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)));
        if (!readResult.io.ok)
        {
            readResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_READ_REGISTRY_VALUE) failed, error=" +
                std::to_string(readResult.io.win32Error);
            return readResult;
        }
        if (readResult.io.bytesReturned < sizeof(response) ||
            response.version != KSWORD_ARK_REGISTRY_PROTOCOL_VERSION)
        {
            readResult.status = KSWORD_ARK_REGISTRY_READ_STATUS_FAILED;
            readResult.io.ok = false;
            readResult.io.win32Error = ERROR_INVALID_DATA;
            readResult.io.message =
                "registry read response has invalid size or version, bytesReturned=" +
                std::to_string(readResult.io.bytesReturned);
            return readResult;
        }

        readResult.version = static_cast<std::uint32_t>(response.version);
        readResult.status = static_cast<std::uint32_t>(response.status);
        readResult.valueType = static_cast<std::uint32_t>(response.valueType);
        readResult.dataBytes = static_cast<std::uint32_t>(response.dataBytes);
        readResult.requiredBytes = static_cast<std::uint32_t>(response.requiredBytes);
        readResult.lastStatus = static_cast<long>(response.lastStatus);
        readResult.io.ntStatus = readResult.lastStatus;

        const std::size_t copyBytes = std::min<std::size_t>(
            static_cast<std::size_t>(readResult.dataBytes),
            KSWORD_ARK_REGISTRY_DATA_MAX_BYTES);
        readResult.data.assign(response.data, response.data + copyBytes);
        readResult.dataBytes = static_cast<std::uint32_t>(copyBytes);
        if (readResult.status == KSWORD_ARK_REGISTRY_READ_STATUS_SUCCESS &&
            (response.dataBytes != copyBytes || readResult.requiredBytes != copyBytes))
        {
            // 即使旧驱动误报 SUCCESS，前缀也不能被当成完整值提交或备份。
            readResult.status = KSWORD_ARK_REGISTRY_READ_STATUS_BUFFER_TOO_SMALL;
        }

        std::ostringstream stream;
        stream << "status=" << readResult.status
            << ", type=" << readResult.valueType
            << ", data=" << readResult.dataBytes
            << "/" << readResult.requiredBytes
            << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(readResult.lastStatus);
        readResult.io.message = stream.str();
        return readResult;
    }

    RegistryEnumResult DriverClient::enumerateRegistryKey(
        const std::wstring& kernelKeyPath,
        const unsigned long flags) const
    {
        // 作用：通过 R0 枚举注册表键下的子键和值。
        // 返回：RegistryEnumResult；部分返回时 status 为 PARTIAL。
        RegistryEnumResult enumResult{};
        if (!validateRegistryRequest(enumResult, KSWORD_ARK_REGISTRY_ENUM_STATUS_FAILED,
            {{kernelKeyPath, KSWORD_ARK_REGISTRY_PATH_CHARS}}))
        {
            return enumResult;
        }
        KSWORD_ARK_ENUM_REGISTRY_KEY_REQUEST request{};
        auto response = std::make_unique<KSWORD_ARK_ENUM_REGISTRY_KEY_RESPONSE>();
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        request.flags = flags;
        request.maxSubKeys = KSWORD_ARK_REGISTRY_ENUM_MAX_SUBKEYS;
        request.maxValues = KSWORD_ARK_REGISTRY_ENUM_MAX_VALUES;
        request.maxValueDataBytes = KSWORD_ARK_REGISTRY_ENUM_VALUE_DATA_MAX_BYTES;
        copyRegistryWideToFixed(
            request.keyPath,
            KSWORD_ARK_REGISTRY_PATH_CHARS,
            kernelKeyPath);

        enumResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUM_REGISTRY_KEY,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            response.get(),
            static_cast<unsigned long>(sizeof(*response)));
        if (!enumResult.io.ok)
        {
            enumResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_ENUM_REGISTRY_KEY) failed, error=" +
                std::to_string(enumResult.io.win32Error);
            return enumResult;
        }
        if (enumResult.io.bytesReturned < sizeof(*response) ||
            response->version != KSWORD_ARK_REGISTRY_PROTOCOL_VERSION)
        {
            enumResult.status = KSWORD_ARK_REGISTRY_ENUM_STATUS_FAILED;
            enumResult.io.ok = false;
            enumResult.io.win32Error = ERROR_INVALID_DATA;
            enumResult.io.message =
                "registry enum response has invalid size or version, bytesReturned=" +
                std::to_string(enumResult.io.bytesReturned);
            return enumResult;
        }

        enumResult.version = static_cast<std::uint32_t>(response->version);
        enumResult.status = static_cast<std::uint32_t>(response->status);
        enumResult.subKeyCount = static_cast<std::uint32_t>(response->subKeyCount);
        enumResult.returnedSubKeyCount = static_cast<std::uint32_t>(response->returnedSubKeyCount);
        enumResult.valueCount = static_cast<std::uint32_t>(response->valueCount);
        enumResult.returnedValueCount = static_cast<std::uint32_t>(response->returnedValueCount);
        enumResult.lastStatus = static_cast<long>(response->lastStatus);
        enumResult.io.ntStatus = enumResult.lastStatus;

        if (enumResult.status == KSWORD_ARK_REGISTRY_ENUM_STATUS_SUCCESS &&
            (enumResult.lastStatus < 0 ||
             enumResult.returnedSubKeyCount != enumResult.subKeyCount ||
             enumResult.returnedValueCount != enumResult.valueCount ||
             enumResult.returnedSubKeyCount > KSWORD_ARK_REGISTRY_ENUM_MAX_SUBKEYS ||
             enumResult.returnedValueCount > KSWORD_ARK_REGISTRY_ENUM_MAX_VALUES))
        {
            enumResult.status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
        }

        const std::size_t subKeyCount = std::min<std::size_t>(
            enumResult.returnedSubKeyCount,
            KSWORD_ARK_REGISTRY_ENUM_MAX_SUBKEYS);
        enumResult.subKeys.reserve(subKeyCount);
        for (std::size_t index = 0U; index < subKeyCount; ++index)
        {
            RegistrySubKeyEntry entry{};
            entry.name = registryFixedWideToString(
                response->subKeys[index].name,
                KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS);
            if (entry.name.empty() ||
                entry.name.size() == KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS ||
                (enumResult.status == KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL &&
                 entry.name.size() == KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS - 1U))
            {
                // v1 不提供单项名称长度；PARTIAL 的边界名称可能是别的键的前缀。
                enumResult.status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
                continue;
            }
            enumResult.subKeys.push_back(std::move(entry));
        }

        const std::size_t valueCount = std::min<std::size_t>(
            enumResult.returnedValueCount,
            KSWORD_ARK_REGISTRY_ENUM_MAX_VALUES);
        enumResult.values.reserve(valueCount);
        for (std::size_t index = 0U; index < valueCount; ++index)
        {
            const auto& source = response->values[index];
            RegistryValueEntry entry{};
            entry.name = registryFixedWideToString(
                source.name,
                KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS);
            if ((source.flags & KSWORD_ARK_REGISTRY_ENUM_VALUE_FLAG_NAME_PRESENT) == 0UL)
            {
                if (!entry.name.empty())
                {
                    // 不一致的命名标志不能把一个普通值误认成默认值。
                    enumResult.status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
                    continue;
                }
                entry.name.clear();
            }
            if (entry.name.size() == KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS ||
                (enumResult.status == KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL &&
                 entry.name.size() == KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS - 1U))
            {
                enumResult.status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
                continue;
            }
            entry.valueType = static_cast<std::uint32_t>(source.valueType);
            entry.dataBytes = static_cast<std::uint32_t>(source.dataBytes);
            entry.requiredBytes = static_cast<std::uint32_t>(source.requiredBytes);
            const std::size_t dataBytes = std::min<std::size_t>(
                static_cast<std::size_t>(entry.dataBytes),
                KSWORD_ARK_REGISTRY_ENUM_VALUE_DATA_MAX_BYTES);
            entry.data.assign(source.data, source.data + dataBytes);
            entry.dataBytes = static_cast<std::uint32_t>(dataBytes);
            if (source.dataBytes != dataBytes || entry.requiredBytes != dataBytes)
            {
                if (enumResult.status == KSWORD_ARK_REGISTRY_ENUM_STATUS_SUCCESS)
                {
                    enumResult.status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
                }
            }
            enumResult.values.push_back(std::move(entry));
        }

        enumResult.returnedSubKeyCount = static_cast<std::uint32_t>(enumResult.subKeys.size());
        enumResult.returnedValueCount = static_cast<std::uint32_t>(enumResult.values.size());

        std::ostringstream stream;
        stream << "status=" << enumResult.status
            << ", subkeys=" << enumResult.returnedSubKeyCount << "/" << enumResult.subKeyCount
            << ", values=" << enumResult.returnedValueCount << "/" << enumResult.valueCount
            << ", lastStatus=0x" << std::hex << static_cast<unsigned long>(enumResult.lastStatus);
        enumResult.io.message = stream.str();
        return enumResult;
    }

    RegistryOperationResult DriverClient::setRegistryValue(
        const std::wstring& kernelKeyPath,
        const std::wstring& valueName,
        const std::uint32_t valueType,
        const std::vector<std::uint8_t>& data) const
    {
        // 作用：通过 R0 写入或创建注册表值。
        // 返回：RegistryOperationResult，status 表示 R0 聚合状态。
        RegistryOperationResult result{};
        if (!validateRegistryRequest(result, KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED,
            {{kernelKeyPath, KSWORD_ARK_REGISTRY_PATH_CHARS},
             {valueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS}}, data.size()))
        {
            return result;
        }
        KSWORD_ARK_SET_REGISTRY_VALUE_REQUEST request{};
        KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        request.valueType = valueType;
        request.dataBytes = static_cast<unsigned long>(data.size());
        copyRegistryWideToFixed(request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS, kernelKeyPath);
        if (!valueName.empty())
        {
            request.flags |= KSWORD_ARK_REGISTRY_SET_FLAG_VALUE_NAME_PRESENT;
            copyRegistryWideToFixed(request.valueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS, valueName);
        }
        if (request.dataBytes != 0UL)
        {
            std::copy(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(request.dataBytes), request.data);
        }

        IoResult ioResult = deviceIoControl(IOCTL_KSWORD_ARK_SET_REGISTRY_VALUE, &request, sizeof(request), &response, sizeof(response));
        return parseRegistryOperationResponse(std::move(ioResult), response, "IOCTL_KSWORD_ARK_SET_REGISTRY_VALUE");
    }

    RegistryOperationResult DriverClient::deleteRegistryValue(
        const std::wstring& kernelKeyPath,
        const std::wstring& valueName) const
    {
        RegistryOperationResult result{};
        if (!validateRegistryRequest(result, KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED,
            {{kernelKeyPath, KSWORD_ARK_REGISTRY_PATH_CHARS},
             {valueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS}}))
        {
            return result;
        }
        KSWORD_ARK_REGISTRY_VALUE_NAME_REQUEST request{};
        KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        copyRegistryWideToFixed(request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS, kernelKeyPath);
        if (!valueName.empty())
        {
            request.flags |= KSWORD_ARK_REGISTRY_DELETE_VALUE_FLAG_NAME_PRESENT;
            copyRegistryWideToFixed(request.valueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS, valueName);
        }

        IoResult ioResult = deviceIoControl(IOCTL_KSWORD_ARK_DELETE_REGISTRY_VALUE, &request, sizeof(request), &response, sizeof(response));
        return parseRegistryOperationResponse(std::move(ioResult), response, "IOCTL_KSWORD_ARK_DELETE_REGISTRY_VALUE");
    }

    RegistryOperationResult DriverClient::createRegistryKey(const std::wstring& kernelKeyPath) const
    {
        RegistryOperationResult result{};
        if (!validateRegistryRequest(result, KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED,
            {{kernelKeyPath, KSWORD_ARK_REGISTRY_PATH_CHARS}}))
        {
            return result;
        }
        KSWORD_ARK_REGISTRY_KEY_PATH_REQUEST request{};
        KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        copyRegistryWideToFixed(request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS, kernelKeyPath);

        IoResult ioResult = deviceIoControl(IOCTL_KSWORD_ARK_CREATE_REGISTRY_KEY, &request, sizeof(request), &response, sizeof(response));
        return parseRegistryOperationResponse(std::move(ioResult), response, "IOCTL_KSWORD_ARK_CREATE_REGISTRY_KEY");
    }

    RegistryOperationResult DriverClient::deleteRegistryKey(const std::wstring& kernelKeyPath) const
    {
        RegistryOperationResult result{};
        if (!validateRegistryRequest(result, KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED,
            {{kernelKeyPath, KSWORD_ARK_REGISTRY_PATH_CHARS}}))
        {
            return result;
        }
        KSWORD_ARK_REGISTRY_KEY_PATH_REQUEST request{};
        KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        copyRegistryWideToFixed(request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS, kernelKeyPath);

        IoResult ioResult = deviceIoControl(IOCTL_KSWORD_ARK_DELETE_REGISTRY_KEY, &request, sizeof(request), &response, sizeof(response));
        return parseRegistryOperationResponse(std::move(ioResult), response, "IOCTL_KSWORD_ARK_DELETE_REGISTRY_KEY");
    }

    RegistryOperationResult DriverClient::renameRegistryValue(
        const std::wstring& kernelKeyPath,
        const std::wstring& oldValueName,
        const std::wstring& newValueName) const
    {
        RegistryOperationResult result{};
        if (!validateRegistryRequest(result, KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED,
            {{kernelKeyPath, KSWORD_ARK_REGISTRY_PATH_CHARS},
             {oldValueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS},
             {newValueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS}}))
        {
            return result;
        }
        KSWORD_ARK_RENAME_REGISTRY_VALUE_REQUEST request{};
        KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        copyRegistryWideToFixed(request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS, kernelKeyPath);
        copyRegistryWideToFixed(request.oldValueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS, oldValueName);
        copyRegistryWideToFixed(request.newValueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS, newValueName);

        IoResult ioResult = deviceIoControl(IOCTL_KSWORD_ARK_RENAME_REGISTRY_VALUE, &request, sizeof(request), &response, sizeof(response));
        return parseRegistryOperationResponse(std::move(ioResult), response, "IOCTL_KSWORD_ARK_RENAME_REGISTRY_VALUE");
    }

    RegistryOperationResult DriverClient::renameRegistryKey(
        const std::wstring& kernelKeyPath,
        const std::wstring& newKeyName) const
    {
        RegistryOperationResult result{};
        if (!validateRegistryRequest(result, KSWORD_ARK_REGISTRY_OPERATION_STATUS_FAILED,
            {{kernelKeyPath, KSWORD_ARK_REGISTRY_PATH_CHARS},
             {newKeyName, KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS}}))
        {
            return result;
        }
        KSWORD_ARK_RENAME_REGISTRY_KEY_REQUEST request{};
        KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
        request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
        copyRegistryWideToFixed(request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS, kernelKeyPath);
        copyRegistryWideToFixed(request.newKeyName, KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS, newKeyName);

        IoResult ioResult = deviceIoControl(IOCTL_KSWORD_ARK_RENAME_REGISTRY_KEY, &request, sizeof(request), &response, sizeof(response));
        return parseRegistryOperationResponse(std::move(ioResult), response, "IOCTL_KSWORD_ARK_RENAME_REGISTRY_KEY");
    }
}
