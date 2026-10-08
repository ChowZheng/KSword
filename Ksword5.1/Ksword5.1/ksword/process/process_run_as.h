#pragma once

#include <cstdint>
#include <string>

namespace ks::process
{
    enum class RunAsIdentity { System, TrustedInstaller, Administrator, StandardUser };

    struct RunAsAvailability
    {
        bool system = false;
        bool trustedInstaller = false;
        bool administrator = true;
        bool standardUser = false;
    };

    struct RunAsResult
    {
        bool success = false;
        std::uint32_t error = 0;
        std::uint32_t processId = 0;
        std::wstring detail;
        // Captured from the returned child handle before it is closed; anchors follow-up queries.
        std::uint64_t creationTime100ns = 0;
    };

    // 只读检查；不启动服务、不请求 UAC、不改变调用方令牌。
    RunAsAvailability QueryRunAsAvailability();

    // 仅运行可执行映像，参数独立传递；工作目录为映像所在目录。
    // 不回退到其它身份。服务等待有界，允许在后台线程调用。
    RunAsResult RunExecutableAs(const std::wstring& imagePath, RunAsIdentity identity,
        const std::wstring& arguments = {});
}
