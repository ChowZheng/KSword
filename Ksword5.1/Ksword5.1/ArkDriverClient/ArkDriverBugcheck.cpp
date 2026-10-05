#include "ArkDriverClient.h"

#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

namespace ksword::ark
{
    IoResult DriverClient::setBugcheckBitmap(
        const std::uint32_t width,
        const std::uint32_t height,
        const std::uint32_t stride,
        const std::uint32_t brandColorRgb,
        const std::vector<std::uint8_t>& bgraPixels) const
    {
        IoResult result{};
        const std::uint64_t expectedStride = static_cast<std::uint64_t>(width) * 4ULL;
        const std::uint64_t expectedBytes = expectedStride * static_cast<std::uint64_t>(height);

        if (width == 0 || height == 0 ||
            width > KSWORD_ARK_BUGCHECK_BITMAP_MAX_WIDTH ||
            height > KSWORD_ARK_BUGCHECK_BITMAP_MAX_HEIGHT ||
            stride != expectedStride ||
            expectedBytes == 0 ||
            expectedBytes > KSWORD_ARK_BUGCHECK_BITMAP_MAX_BYTES ||
            bgraPixels.size() != static_cast<std::size_t>(expectedBytes))
        {
            result.win32Error = ERROR_INVALID_PARAMETER;
            return result;
        }

        const std::size_t payloadBytes = sizeof(KSWORD_ARK_BUGCHECK_BITMAP_HEADER) + bgraPixels.size();
        if (payloadBytes > std::numeric_limits<unsigned long>::max())
        {
            result.win32Error = ERROR_ARITHMETIC_OVERFLOW;
            return result;
        }

        KSWORD_ARK_BUGCHECK_BITMAP_HEADER header{};
        header.version = KSWORD_ARK_BUGCHECK_BITMAP_PROTOCOL_VERSION;
        header.size = sizeof(header);
        header.magic = KSWORD_ARK_BUGCHECK_BITMAP_MAGIC;
        header.width = width;
        header.height = height;
        header.stride = stride;
        header.format = KSWORD_ARK_BUGCHECK_BITMAP_FORMAT_BGRA32;
        header.brandColorRgb = brandColorRgb & 0x00FFFFFFUL;
        header.dataLength = static_cast<unsigned long>(bgraPixels.size());

        std::vector<std::uint8_t> payload(payloadBytes);
        std::memcpy(payload.data(), &header, sizeof(header));
        std::memcpy(payload.data() + sizeof(header), bgraPixels.data(), bgraPixels.size());

        return deviceIoControl(
            IOCTL_KSWORD_ARK_SET_BUGCHECK_BITMAP,
            payload.data(),
            static_cast<unsigned long>(payload.size()),
            nullptr,
            0);
    }

    IoResult DriverClient::setBugcheckVerdictResources(
        const std::vector<BugcheckVerdictBitmap>& resources) const
    {
        IoResult result{};
        std::uint32_t seenMask = 0;
        std::uint64_t dataBytes = 0;

        if (resources.size() != KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_COUNT)
        {
            result.win32Error = ERROR_INVALID_PARAMETER;
            return result;
        }

        for (const BugcheckVerdictBitmap& resource : resources)
        {
            const std::uint64_t expectedStride =
                static_cast<std::uint64_t>(resource.width) * 4ULL;
            const std::uint64_t expectedBytes =
                expectedStride * static_cast<std::uint64_t>(resource.height);
            if (resource.language >= KSWORD_ARK_BUGCHECK_VERDICT_LANGUAGE_COUNT ||
                resource.classification >= KSWORD_ARK_BUGCHECK_VERDICT_CLASS_COUNT ||
                resource.width == 0 || resource.height == 0 ||
                resource.width > KSWORD_ARK_BUGCHECK_VERDICT_MAX_WIDTH ||
                resource.height > KSWORD_ARK_BUGCHECK_VERDICT_MAX_HEIGHT ||
                resource.stride != expectedStride ||
                expectedBytes == 0 ||
                expectedBytes != resource.bgraPixels.size())
            {
                result.win32Error = ERROR_INVALID_PARAMETER;
                return result;
            }

            const std::uint32_t bitIndex =
                resource.language * KSWORD_ARK_BUGCHECK_VERDICT_CLASS_COUNT +
                resource.classification;
            const std::uint32_t bit = 1UL << bitIndex;
            if ((seenMask & bit) != 0)
            {
                result.win32Error = ERROR_INVALID_PARAMETER;
                return result;
            }
            seenMask |= bit;
            dataBytes += expectedBytes;
        }

        const std::uint64_t entriesBytes =
            sizeof(KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_ENTRY) *
            KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_COUNT;
        const std::uint64_t packetBytes64 =
            sizeof(KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_HEADER) +
            entriesBytes + dataBytes;
        if (dataBytes > KSWORD_ARK_BUGCHECK_VERDICT_MAX_DATA_BYTES ||
            packetBytes64 > std::numeric_limits<unsigned long>::max())
        {
            result.win32Error = ERROR_ARITHMETIC_OVERFLOW;
            return result;
        }

        std::vector<std::uint8_t> packet(
            static_cast<std::size_t>(packetBytes64));
        auto* header = reinterpret_cast<
            KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_HEADER*>(packet.data());
        auto* entries = reinterpret_cast<
            KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_ENTRY*>(
                packet.data() + sizeof(*header));
        header->version = KSWORD_ARK_BUGCHECK_VERDICT_PROTOCOL_VERSION;
        header->size = sizeof(*header);
        header->magic = KSWORD_ARK_BUGCHECK_VERDICT_MAGIC;
        header->resourceCount =
            KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_COUNT;
        header->entriesOffset = sizeof(*header);
        header->totalSize = static_cast<unsigned long>(packet.size());

        std::size_t dataOffset = sizeof(*header) +
            static_cast<std::size_t>(entriesBytes);
        for (std::size_t index = 0; index < resources.size(); ++index)
        {
            const BugcheckVerdictBitmap& resource = resources[index];
            KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_ENTRY& entry = entries[index];
            entry.language = resource.language;
            entry.classification = resource.classification;
            entry.width = resource.width;
            entry.height = resource.height;
            entry.stride = resource.stride;
            entry.format = KSWORD_ARK_BUGCHECK_VERDICT_FORMAT_BGRA32;
            entry.dataOffset = static_cast<unsigned long>(dataOffset);
            entry.dataLength = static_cast<unsigned long>(
                resource.bgraPixels.size());
            std::memcpy(
                packet.data() + dataOffset,
                resource.bgraPixels.data(),
                resource.bgraPixels.size());
            dataOffset += resource.bgraPixels.size();
        }

        return deviceIoControl(
            IOCTL_KSWORD_ARK_SET_BUGCHECK_VERDICT_RESOURCES,
            packet.data(),
            static_cast<unsigned long>(packet.size()),
            nullptr,
            0);
    }

    BugcheckDiagnosticsResult DriverClient::configureBugcheckDiagnostics(
        const unsigned long action,
        const unsigned long renderMode) const
    {
        // 在 R3 拒绝未知动作/模式，避免把无效配置提交给 R0；查询不携带设置语义。
        if ((action != KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_QUERY &&
             action != KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_INSTALL &&
             action != KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_SET_RENDER_MODE) ||
            (renderMode != KSWORD_ARK_BUGCHECK_RENDER_MODE_DIAGNOSTIC &&
             renderMode != KSWORD_ARK_BUGCHECK_RENDER_MODE_LINUX_QR))
        {
            BugcheckDiagnosticsResult invalidResult{}; // 只返回参数错误，不访问设备。
            invalidResult.io.win32Error = ERROR_INVALID_PARAMETER;
            return invalidResult;
        }

        const auto sendRequest = [this, renderMode](const unsigned long requestAction)
        {
            BugcheckDiagnosticsResult current{};
            KSWORD_ARK_BUGCHECK_DIAGNOSTICS_REQUEST request{};

            // INSTALL 只排队，SET_RENDER_MODE 只改模式；QUERY 的模式字段维持零。
            request.size = sizeof(request);
            request.version = KSWORD_ARK_BUGCHECK_DIAGNOSTICS_PROTOCOL_VERSION;
            request.action = requestAction;
            request.renderMode = requestAction == KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_QUERY
                ? KSWORD_ARK_BUGCHECK_RENDER_MODE_DIAGNOSTIC
                : renderMode;
            // 模式保存允许驱动不在线；静默探测失败直接返回，避免弹出启用驱动提示。
            DriverHandle modeHandle; // SET 使用的短期设备句柄，不跨请求持有。
            if (requestAction == KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_SET_RENDER_MODE)
            {
                modeHandle = openSilently();
                if (!modeHandle.isValid())
                {
                    current.io.win32Error = ::GetLastError();
                    return current;
                }
            }
            current.io = deviceIoControl(
                IOCTL_KSWORD_ARK_CONFIGURE_BUGCHECK_DIAGNOSTICS,
                &request,
                static_cast<unsigned long>(sizeof(request)),
                &current.response,
                static_cast<unsigned long>(sizeof(current.response)),
                modeHandle.isValid() ? &modeHandle : nullptr);
            current.unsupported = !current.io.ok &&
                (current.io.win32Error == ERROR_INVALID_FUNCTION ||
                 current.io.win32Error == ERROR_NOT_SUPPORTED);
            if (current.io.ok &&
                (current.io.bytesReturned < sizeof(current.response) ||
                 current.response.version !=
                     KSWORD_ARK_BUGCHECK_DIAGNOSTICS_PROTOCOL_VERSION ||
                 current.response.size != sizeof(current.response) ||
                 (current.response.renderMode != KSWORD_ARK_BUGCHECK_RENDER_MODE_DIAGNOSTIC &&
                  current.response.renderMode != KSWORD_ARK_BUGCHECK_RENDER_MODE_LINUX_QR)))
            {
                current.unsupported = current.io.bytesReturned >= sizeof(current.response) &&
                    current.response.version != KSWORD_ARK_BUGCHECK_DIAGNOSTICS_PROTOCOL_VERSION;
                current.io.ok = false;
                current.io.win32Error = ERROR_INVALID_DATA;
            }
            current.io.ntStatus = current.response.lastStatus;
            return current;
        };

        BugcheckDiagnosticsResult result = sendRequest(action);
        if (action != KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_INSTALL ||
            !result.io.ok ||
            result.response.status != KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_BUSY)
        {
            return result;
        }

        // 新协议的安装 IOCTL 不再占用一个设备句柄等待 R0。后台调用只用短 QUERY
        // 轮询终态；驱动卸载时下一次查询会快速失败，线程不会阻止 SCM 停止服务。
        const auto pollDeadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(35);
        do
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            result = sendRequest(KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_QUERY);
            if (!result.io.ok ||
                result.response.status != KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_BUSY)
            {
                break;
            }
        } while (std::chrono::steady_clock::now() < pollDeadline);

        // 本次请求可能等待了另一模式的并发安装；只补一次 SET，不重试安装或扫描。
        if (result.io.ok &&
            result.response.status == KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_OK &&
            result.response.renderMode != renderMode)
        {
            result = sendRequest(KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_SET_RENDER_MODE);
            // SET 响应必须明确回读目标模式；忙碌、失败或模式不符均不能报告安装成功。
            if (!result.io.ok ||
                result.response.status != KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_OK ||
                result.response.renderMode != renderMode)
            {
                result.io.ok = false;
                result.io.win32Error = ERROR_INVALID_DATA;
                result.io.message = "bugcheck_render_mode_not_applied";
            }
        }

        // 仅限制 R3 轮询线程；实际 R0 工作项拥有自己的 30 秒预算和卸载取消协议。
        return result;
    }

    BugcheckGuardResult DriverClient::configureBugcheckGuard(
        const unsigned long action,
        const unsigned long delaySeconds,
        const bool uiConfirmed,
        const bool tryIgnoreError,
        DriverHandle* const existingHandle) const
    {
        BugcheckGuardResult result{};
        KSWORD_ARK_BUGCHECK_GUARD_REQUEST request{};

        request.size = sizeof(request);
        request.version = KSWORD_ARK_BUGCHECK_GUARD_PROTOCOL_VERSION;
        request.action = action;
        request.delaySeconds = delaySeconds;
        if (uiConfirmed) {
            request.flags = KSWORD_ARK_BUGCHECK_GUARD_FLAG_UI_CONFIRMED;
            request.confirmationToken =
                KSWORD_ARK_BUGCHECK_GUARD_CONFIRMATION_TOKEN;
        }
        if (tryIgnoreError) {
            request.flags |= KSWORD_ARK_BUGCHECK_GUARD_FLAG_TRY_IGNORE_ERROR;
        }
        result.io = deviceIoControl(
            IOCTL_KSWORD_ARK_CONFIGURE_BUGCHECK_GUARD,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &result.response,
            static_cast<unsigned long>(sizeof(result.response)),
            existingHandle);
        result.unsupported = !result.io.ok &&
            (result.io.win32Error == ERROR_INVALID_FUNCTION ||
             result.io.win32Error == ERROR_NOT_SUPPORTED);
        if (result.io.ok &&
            (result.io.bytesReturned < sizeof(result.response) ||
             result.response.version != KSWORD_ARK_BUGCHECK_GUARD_PROTOCOL_VERSION ||
             result.response.size != sizeof(result.response))) {
            result.unsupported = result.io.bytesReturned >= sizeof(result.response) &&
                result.response.version != KSWORD_ARK_BUGCHECK_GUARD_PROTOCOL_VERSION;
            result.io.ok = false;
            result.io.win32Error = ERROR_INVALID_DATA;
        }
        result.io.ntStatus = result.response.lastStatus;
        return result;
    }
}
