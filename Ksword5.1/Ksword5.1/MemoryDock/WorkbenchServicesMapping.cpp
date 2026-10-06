#include "WorkbenchServicesMapping.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

// ============================================================
// WorkbenchServicesMapping.cpp
// 作用：实现 WorkbenchServicesMapping.h 声明的纯函数。全部不触碰 Win32/Qt/驱动，
//       文件顶部的每组函数与头文件分节一一对应。
// ============================================================

namespace ksword::memwb_services_detail
{
    namespace
    {
        // kModuleSourceMarker：ks::process 模块枚举成功时追加到诊断文本里的标记前缀
        // （process.cpp 里 "Module source: Toolhelp" / "Module source: PSAPI fallback"）。
        constexpr const char* kModuleSourceMarker = "Module source:";

        // FormatHex32：把 32 位无符号数格式化成 8 位大写十六进制（不带 0x 前缀）。
        // 传入：value 待格式化的数。传出：固定 8 位的十六进制文本。
        std::string FormatHex32(const std::uint32_t value)
        {
            char buffer[16] = {};
            std::snprintf(buffer, sizeof(buffer), "%08X", static_cast<unsigned int>(value));
            return std::string(buffer);
        }

        // AppendDetail：失败说明后面追加端口给出的细节串（非空才追加）。
        // 传入：base 已有的说明；detail 端口细节串。传出：拼好的文本。
        std::string AppendDetail(const std::string& base, const std::string& detail)
        {
            if (detail.empty())
            {
                return base;
            }
            return base + ": " + detail;
        }
    }

    // ------------------------------------------------------------
    // 一、模块记录映射
    // ------------------------------------------------------------

    std::string FileNameFromPath(const std::string& path)
    {
        // 同时认两种分隔符：进程模块是 Win32 路径，内核模块是 NT 路径，偶尔夹 '/'。
        const std::size_t separator = path.find_last_of("\\/");
        if (separator == std::string::npos)
        {
            return path;
        }
        return path.substr(separator + 1U);
    }

    bool MapRawModule(const RawModuleInfo& raw, ksword::memwb::ModuleRecord* const recordOut)
    {
        if (recordOut == nullptr)
        {
            return false;
        }
        // 基址为 0 是无效记录（枚举接口用 0 表示没有映射）。
        if (raw.base == 0U)
        {
            return false;
        }

        // 名字缺失时从完整路径取最后一个分量；补完仍然为空（路径为空，或以分隔符结尾、
        // 取不出分量）就没有任何可查询的名字，目录里留这样一条记录只会增加噪声。
        std::string name = raw.name;
        if (name.empty())
        {
            name = FileNameFromPath(raw.fullPath);
        }
        if (name.empty())
        {
            return false;
        }

        ksword::memwb::ModuleRecord record;
        record.name = name;
        record.fullPath = raw.fullPath;
        record.base = raw.base;
        record.size = raw.size;
        *recordOut = std::move(record);
        return true;
    }

    std::vector<ksword::memwb::ModuleRecord> MapRawModules(const std::vector<RawModuleInfo>& rawList)
    {
        std::vector<ksword::memwb::ModuleRecord> records;
        records.reserve(rawList.size());
        for (const RawModuleInfo& raw : rawList)
        {
            ksword::memwb::ModuleRecord record;
            if (MapRawModule(raw, &record))
            {
                records.push_back(std::move(record));
            }
        }
        // 稳定排序：基址相同的记录保持枚举顺序，同一输入永远得到同一结果。
        std::stable_sort(
            records.begin(),
            records.end(),
            [](const ksword::memwb::ModuleRecord& left, const ksword::memwb::ModuleRecord& right)
            {
                return left.base < right.base;
            });
        return records;
    }

    ModuleSnapshotVerdict JudgeProcessModuleSnapshot(
        const std::string& diagnosticText,
        const std::size_t moduleCount)
    {
        ModuleSnapshotVerdict verdict;
        // 诊断里出现成功标记才算枚举成功，与 moduleCount 无关（空进程可以真的没有模块）。
        if (diagnosticText.find(kModuleSourceMarker) != std::string::npos)
        {
            verdict.ok = true;
            return verdict;
        }

        // 没有成功标记：一定是失败。失败说明取诊断全文；诊断为空是协议外的情况，
        // 给一句兜底，免得上层拿到一个空的失败原因。
        verdict.ok = false;
        if (diagnosticText.empty())
        {
            verdict.failure =
                "module enumeration returned no diagnostic (module count=" +
                std::to_string(moduleCount) + ")";
        }
        else
        {
            verdict.failure = diagnosticText;
        }
        return verdict;
    }

    std::string FormatKernelModuleFailure(
        const char* const statusName,
        const std::uint32_t ntStatus,
        const std::uint32_t requiredBytes)
    {
        const std::string name = (statusName != nullptr && statusName[0] != '\0')
            ? std::string(statusName)
            : std::string("Unknown");
        return "kernel module query failed: status=" + name +
            ", ntstatus=0x" + FormatHex32(ntStatus) +
            ", requiredBytes=" + std::to_string(requiredBytes);
    }

    const char* KernelModuleQueryStatusName(const std::uint32_t statusValue) noexcept
    {
        switch (statusValue)
        {
        case 0U:
            return "Ok";
        case 1U:
            return "ApiUnavailable";
        case 2U:
            return "LengthQueryFailed";
        case 3U:
            return "SnapshotFailed";
        default:
            return "Unknown";
        }
    }

    // ------------------------------------------------------------
    // 二、指针读取
    // ------------------------------------------------------------

    bool IsPointerReadAllowed(
        const ksword::memwb::Scope scope,
        const ksword::memwb::Channel channel) noexcept
    {
        if (scope == ksword::memwb::Scope::Physical)
        {
            return false;
        }
        if (channel == ksword::memwb::Channel::Ddma)
        {
            return false;
        }
        return true;
    }

    bool DecodePointerLittleEndian(
        const std::vector<std::uint8_t>& data,
        const std::uint32_t width,
        std::uint64_t* const valueOut)
    {
        if (valueOut == nullptr)
        {
            return false;
        }
        *valueOut = 0U;
        // 只接受 4/8 字节指针，且字节数必须恰好等于宽度。
        if ((width != 4U && width != 8U) || data.size() != static_cast<std::size_t>(width))
        {
            return false;
        }

        // 小端：第 0 字节是最低位。
        std::uint64_t value = 0U;
        for (std::uint32_t index = 0U; index < width; ++index)
        {
            value |= static_cast<std::uint64_t>(data[index]) << (8U * index);
        }
        *valueOut = value;
        return true;
    }

    ks::ui::PointerReadResult MapPointerRead(
        const ksword::memwb::IoReadResult& read,
        const std::uint32_t width)
    {
        ks::ui::PointerReadResult result;
        switch (read.status)
        {
        case ksword::memwb::IoReadStatus::Ok:
        {
            std::uint64_t value = 0U;
            if (DecodePointerLittleEndian(read.data, width, &value))
            {
                result.ok = true;
                result.value = value;
                return result;
            }
            // Ok 却字节数不对：端口违反契约，按失败处理，不拿残缺字节拼指针。
            result.failure = AppendDetail(
                "pointer read returned an unexpected byte count (expected " +
                    std::to_string(width) + ", got " + std::to_string(read.data.size()) + ")",
                read.failure);
            return result;
        }
        case ksword::memwb::IoReadStatus::Partial:
            result.failure = AppendDetail(
                "pointer read returned only " + std::to_string(read.data.size()) +
                    " of " + std::to_string(width) + " bytes",
                read.failure);
            return result;
        case ksword::memwb::IoReadStatus::Unreadable:
            result.failure = AppendDetail("pointer target memory is unreadable", read.failure);
            return result;
        case ksword::memwb::IoReadStatus::Failed:
            result.failure = AppendDetail("pointer read channel failed", read.failure);
            return result;
        }
        result.failure = "pointer read returned an unknown status";
        return result;
    }

    // ------------------------------------------------------------
    // 三、候选进程
    // ------------------------------------------------------------

    std::string FindProcessName(
        const std::vector<ksword::memwb::ProcessCandidate>& candidates,
        const std::uint32_t pid)
    {
        if (pid == 0U)
        {
            return std::string();
        }
        for (const ksword::memwb::ProcessCandidate& candidate : candidates)
        {
            if (candidate.pid == pid)
            {
                return candidate.name;
            }
        }
        return std::string();
    }

    // ------------------------------------------------------------
    // 四、页保护属性徽章
    // ------------------------------------------------------------

    ProtectionBadge FormatProtectionBadge(
        const std::uint32_t state,
        const std::uint32_t protect,
        const std::uint32_t type)
    {
        ProtectionBadge badge;

        // 未提交的区域没有"保护属性"可言：直接给状态缩写，不追加类型。
        if (state == kMemFree)
        {
            badge.text = "FREE";
            badge.role = BadgeRole::Idle;
            return badge;
        }
        if (state == kMemReserve)
        {
            badge.text = "RSV";
            badge.role = BadgeRole::Idle;
            return badge;
        }
        if (state != kMemCommit)
        {
            badge.text = "?";
            badge.role = BadgeRole::Idle;
            return badge;
        }

        // 基础保护值在低 8 位；高位是 GUARD/NOCACHE/WRITECOMBINE 等修饰标志。
        const std::uint32_t baseProtect = protect & 0xFFU;
        switch (baseProtect)
        {
        case kPageNoAccess:
            badge.text = "NA";
            badge.role = BadgeRole::Idle;
            break;
        case kPageReadOnly:
            badge.text = "R";
            badge.role = BadgeRole::Info;
            break;
        case kPageReadWrite:
            badge.text = "RW";
            badge.role = BadgeRole::Success;
            break;
        case kPageWriteCopy:
            badge.text = "RWC";
            badge.role = BadgeRole::Success;
            break;
        case kPageExecute:
            badge.text = "X";
            badge.role = BadgeRole::Warning;
            break;
        case kPageExecuteRead:
            badge.text = "RX";
            badge.role = BadgeRole::Warning;
            break;
        case kPageExecuteReadWrite:
            badge.text = "RWX";
            badge.role = BadgeRole::Error;
            break;
        case kPageExecuteWriteCopy:
            badge.text = "RWXC";
            badge.role = BadgeRole::Error;
            break;
        default:
            badge.text = "?";
            badge.role = BadgeRole::Idle;
            break;
        }

        // 守护页：读取会触发异常并清除守护标志，必须在徽章上醒目；已经是红色的不降级。
        if ((protect & kPageGuard) != 0U)
        {
            badge.text += "+G";
            if (badge.role != BadgeRole::Error)
            {
                badge.role = BadgeRole::Warning;
            }
        }

        // 区域类型缩写：映像/映射文件/私有；其余类型值不追加。
        if (type == kMemImage)
        {
            badge.text += " (IMG)";
        }
        else if (type == kMemMapped)
        {
            badge.text += " (MAP)";
        }
        else if (type == kMemPrivate)
        {
            badge.text += " (PRV)";
        }
        return badge;
    }

    // ------------------------------------------------------------
    // 五、反汇编/汇编后端结果判据
    // ------------------------------------------------------------

    bool AcceptDecodedRow(
        const bool decodedFlag,
        const bool fromZydis,
        const std::size_t rowLength,
        const std::size_t available) noexcept
    {
        if (!decodedFlag || !fromZydis)
        {
            return false;
        }
        // 长度必须有界：非零（否则视图会原地不动死循环）、不超过可用字节、不超过 15。
        if (rowLength == 0U || rowLength > available || rowLength > kMaxInstructionBytes)
        {
            return false;
        }
        return true;
    }

    AssembleRejection JudgeAssembleResult(const bool success, const std::size_t byteCount) noexcept
    {
        if (!success)
        {
            return AssembleRejection::BackendFailed;
        }
        if (byteCount == 0U)
        {
            return AssembleRejection::EmptyOutput;
        }
        if (byteCount > kMaxAssembledBytes)
        {
            return AssembleRejection::OutputTooLarge;
        }
        return AssembleRejection::None;
    }

    // ------------------------------------------------------------
    // 六、地址簿文件路径
    // ------------------------------------------------------------

    std::string JoinPath(const std::string& dir, const std::string& fileName)
    {
        if (dir.empty() || fileName.empty())
        {
            return std::string();
        }
        const char last = dir.back();
        if (last == '/' || last == '\\')
        {
            return dir + fileName;
        }
        return dir + "/" + fileName;
    }

    std::string ChooseWritableDirectory(
        const std::vector<std::string>& candidates,
        const std::function<bool(const std::string&)>& isWritable)
    {
        if (!isWritable)
        {
            return std::string();
        }
        for (const std::string& candidate : candidates)
        {
            // 空串候选（例如 QStandardPaths 在某些环境返回空）直接跳过，不交给回调。
            if (candidate.empty())
            {
                continue;
            }
            if (isWritable(candidate))
            {
                return candidate;
            }
        }
        return std::string();
    }

    // ------------------------------------------------------------
    // 七、int3 补丁的字节存储会话
    // ------------------------------------------------------------

    bool IsPatchChannelAllowed(const ksword::memwb::Channel channel) noexcept
    {
        // 白名单写法：只放行三条明确允许的通道，新增或越界的枚举值默认拒绝。
        return channel == ksword::memwb::Channel::UserMode ||
            channel == ksword::memwb::Channel::StandardDriver ||
            channel == ksword::memwb::Channel::Hvm;
    }

    ksword::memwb::MemoryTargetSession BuildPatchSession(
        const ksword::memwb::PatchTarget& target,
        const ksword::memwb::Channel channel)
    {
        ksword::memwb::MemoryTargetSession session;
        // int3 补丁只存在于进程虚拟范围，范围不由调用方指定。
        session.scope = ksword::memwb::Scope::ProcessVirtual;
        session.pid = target.pid;
        session.processCreateTime100ns = target.processCreateTime100ns;
        session.attachGeneration = target.attachGeneration;
        session.channel = channel;
        // 只允许非 Ddma 通道，DDMA 代次对本会话没有意义。
        session.ddmaGeneration = 0U;
        session.addressBits = 64U;
        return session;
    }

    // ------------------------------------------------------------
    // 八、旧入口跳转的目标决议
    // ------------------------------------------------------------

    ModuleJumpPin DecideModuleJumpPin(
        const std::uint32_t previewPid,
        const std::uint64_t previewCreateTime100ns,
        const std::uint32_t attachedPid) noexcept
    {
        ModuleJumpPin decision;
        // 缓存为空：没有"预览进程"可钉，保持不钉住。
        if (previewPid == 0U)
        {
            return decision;
        }
        // 预览的就是附加的进程：不钉住，工作台跟随 Dock（若它此前钉在别处会被带回跟随）。
        if (previewPid == attachedPid)
        {
            return decision;
        }
        decision.pid = previewPid;
        decision.createTime100ns = previewCreateTime100ns;
        return decision;
    }
}
