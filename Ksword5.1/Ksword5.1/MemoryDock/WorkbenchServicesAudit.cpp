#include "WorkbenchServicesAudit.h"

#include <cstdio>

// ============================================================
// WorkbenchServicesAudit.cpp
// 作用：实现 WorkbenchServicesAudit.h 的审计分级、字段格式化与文本清洗。
//       全部是纯函数，不触碰 Framework.h/Qt/Win32。
// ============================================================

namespace ksword::memwb_services_detail
{
    namespace
    {
        // ParseUnsigned：把纯十进制数字文本解析成无符号数。
        // 传入：text 待解析文本；valueOut 输出。
        // 传出：true 表示 text 非空、全是数字且不溢出 64 位；否则 false。
        bool ParseUnsigned(const std::string& text, std::uint64_t* const valueOut)
        {
            if (text.empty() || text.size() > 20U)
            {
                return false;
            }
            std::uint64_t value = 0U;
            for (const char ch : text)
            {
                if (ch < '0' || ch > '9')
                {
                    return false;
                }
                const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
                // 乘 10 加 digit 之前先判溢出。
                if (value > (UINT64_MAX - digit) / 10U)
                {
                    return false;
                }
                value = value * 10U + digit;
            }
            *valueOut = value;
            return true;
        }

        // FormatHex64：格式化成 "0x" 加 16 位大写十六进制。
        std::string FormatHex64(const std::uint64_t value)
        {
            char buffer[32] = {};
            std::snprintf(
                buffer,
                sizeof(buffer),
                "0x%016llX",
                static_cast<unsigned long long>(value));
            return std::string(buffer);
        }

        // ScopeDisplayName：身份串里的 scope 数值文本 -> 范围名；解析不出返回 "unknown"。
        // 数值与 ksword::memwb::Scope 的钉死取值一致（见 MemoryTargetSession.h）。
        std::string ScopeDisplayName(const std::string& scopeText)
        {
            std::uint64_t value = 0U;
            if (!ParseUnsigned(scopeText, &value))
            {
                return "unknown";
            }
            switch (value)
            {
            case 0U:
                return "ProcessVirtual";
            case 1U:
                return "KernelVirtual";
            case 2U:
                return "Physical";
            default:
                return "unknown";
            }
        }

        // ChannelDisplayName：身份串里的 ch 数值文本 -> 通道名；解析不出返回 "unknown"。
        std::string ChannelDisplayName(const std::string& channelText)
        {
            std::uint64_t value = 0U;
            if (!ParseUnsigned(channelText, &value))
            {
                return "unknown";
            }
            switch (value)
            {
            case 0U:
                return "UserMode";
            case 1U:
                return "StandardDriver";
            case 2U:
                return "Hvm";
            case 3U:
                return "Ddma";
            default:
                return "unknown";
            }
        }
    }

    const char* AuditEventName(const ksword::memwb::AuditEvent event) noexcept
    {
        switch (event)
        {
        case ksword::memwb::AuditEvent::CommitStarted:
            return "CommitStarted";
        case ksword::memwb::AuditEvent::UiConfirmAccepted:
            return "UiConfirmAccepted";
        case ksword::memwb::AuditEvent::UiConfirmDenied:
            return "UiConfirmDenied";
        case ksword::memwb::AuditEvent::UiConfirmSuppressed:
            return "UiConfirmSuppressed";
        case ksword::memwb::AuditEvent::ApprovalAnswered:
            return "ApprovalAnswered";
        case ksword::memwb::AuditEvent::CommitFinished:
            return "CommitFinished";
        }
        return "Unknown";
    }

    const char* CommitOutcomeName(const ksword::memwb::CommitOutcome outcome) noexcept
    {
        switch (outcome)
        {
        case ksword::memwb::CommitOutcome::NoChange:
            return "NoChange";
        case ksword::memwb::CommitOutcome::Committed:
            return "Committed";
        case ksword::memwb::CommitOutcome::UserCancelled:
            return "UserCancelled";
        case ksword::memwb::CommitOutcome::Stale:
            return "Stale";
        case ksword::memwb::CommitOutcome::TargetChanged:
            return "TargetChanged";
        case ksword::memwb::CommitOutcome::ApprovalDenied:
            return "ApprovalDenied";
        case ksword::memwb::CommitOutcome::WriteFailed:
            return "WriteFailed";
        case ksword::memwb::CommitOutcome::VerifyMismatch:
            return "VerifyMismatch";
        case ksword::memwb::CommitOutcome::InvalidSession:
            return "InvalidSession";
        case ksword::memwb::CommitOutcome::Busy:
            return "Busy";
        }
        return "Unknown";
    }

    const char* ApprovalAnswerName(const ksword::memwb::ApprovalAnswer answer) noexcept
    {
        switch (answer)
        {
        case ksword::memwb::ApprovalAnswer::Deny:
            return "Deny";
        case ksword::memwb::ApprovalAnswer::ThisBlockOnly:
            return "ThisBlockOnly";
        case ksword::memwb::ApprovalAnswer::RestOfBatch:
            return "RestOfBatch";
        }
        return "Unknown";
    }

    AuditLogLevel ClassifyAuditRecord(const ksword::memwb::AuditRecord& record)
    {
        switch (record.event)
        {
        case ksword::memwb::AuditEvent::CommitStarted:
        case ksword::memwb::AuditEvent::UiConfirmAccepted:
        case ksword::memwb::AuditEvent::UiConfirmSuppressed:
            return AuditLogLevel::Info;
        case ksword::memwb::AuditEvent::UiConfirmDenied:
            return AuditLogLevel::Warn;
        case ksword::memwb::AuditEvent::ApprovalAnswered:
            // 回答 Deny 是用户拒绝了协议级强制同意（写入被中止）；同意则只是常规留痕。
            return record.approvalAnswer == ksword::memwb::ApprovalAnswer::Deny
                ? AuditLogLevel::Warn
                : AuditLogLevel::Info;
        case ksword::memwb::AuditEvent::CommitFinished:
            break;
        }

        // 到这里要么是 CommitFinished，要么是未识别的事件值（枚举越界）：后者没有可
        // 依据的结果语义，取保守级别 Warn，不能让它借默认的 NoChange 结果被报成 Info。
        if (record.event != ksword::memwb::AuditEvent::CommitFinished)
        {
            return AuditLogLevel::Warn;
        }
        AuditLogLevel level = AuditLogLevel::Warn;
        switch (record.outcome)
        {
        case ksword::memwb::CommitOutcome::NoChange:
        case ksword::memwb::CommitOutcome::Committed:
            level = AuditLogLevel::Info;
            break;
        case ksword::memwb::CommitOutcome::UserCancelled:
        case ksword::memwb::CommitOutcome::Stale:
        case ksword::memwb::CommitOutcome::ApprovalDenied:
        case ksword::memwb::CommitOutcome::Busy:
            level = AuditLogLevel::Warn;
            break;
        case ksword::memwb::CommitOutcome::TargetChanged:
        case ksword::memwb::CommitOutcome::WriteFailed:
        case ksword::memwb::CommitOutcome::VerifyMismatch:
        case ksword::memwb::CommitOutcome::InvalidSession:
            level = AuditLogLevel::Error;
            break;
        }

        // 风险标志提升级别：暂存区未还原（磁盘留了脏扇区）无论成败都最醒目；
        // 读-改-写窗口只把 Info 提升到 Warn（失败级别本来就不低于它）。
        if (record.scratchAreaDirty)
        {
            level = AuditLogLevel::Error;
        }
        else if (record.readModifyWriteWindow && level == AuditLogLevel::Info)
        {
            level = AuditLogLevel::Warn;
        }
        return level;
    }

    std::string ExtractIdentityField(const std::string& identity, const std::string& label)
    {
        if (label.empty())
        {
            return std::string();
        }
        // 字段在身份串里都以 "|标签=" 开头，要求前导 '|' 避免 "ch" 误撞 "…ch…" 之类的子串。
        const std::string needle = "|" + label + "=";
        const std::size_t start = identity.find(needle);
        if (start == std::string::npos)
        {
            return std::string();
        }
        const std::size_t valueStart = start + needle.size();
        const std::size_t valueEnd = identity.find('|', valueStart);
        if (valueEnd == std::string::npos)
        {
            return identity.substr(valueStart);
        }
        return identity.substr(valueStart, valueEnd - valueStart);
    }

    std::string SanitizeAuditNote(const std::string& text)
    {
        // 第一步：控制字符换空格；双引号换成单引号，保证日志里 note="..." 的引号配对
        // 不会被说明文本本身破坏。
        std::string cleaned;
        cleaned.reserve(text.size());
        for (const char ch : text)
        {
            const unsigned char byte = static_cast<unsigned char>(ch);
            if (byte < 0x20U || byte == 0x7FU)
            {
                cleaned.push_back(' ');
            }
            else if (ch == '"')
            {
                cleaned.push_back('\'');
            }
            else
            {
                cleaned.push_back(ch);
            }
        }

        // 第二步：去掉首尾空格（换行变成的空格不应该造成首尾空白）。
        const std::size_t first = cleaned.find_first_not_of(' ');
        if (first == std::string::npos)
        {
            return std::string();
        }
        const std::size_t last = cleaned.find_last_not_of(' ');
        cleaned = cleaned.substr(first, last - first + 1U);

        // 第三步：超长截断，不切在 UTF-8 多字节序列中间（回退到最近的起始字节）。
        if (cleaned.size() > kMaxAuditNoteBytes)
        {
            std::size_t cut = kMaxAuditNoteBytes;
            while (cut > 0U && (static_cast<unsigned char>(cleaned[cut]) & 0xC0U) == 0x80U)
            {
                --cut;
            }
            cleaned.resize(cut);
            cleaned += "...";
        }
        return cleaned;
    }

    std::string FormatAuditRecord(const ksword::memwb::AuditRecord& record)
    {
        const std::string pidText = ExtractIdentityField(record.targetIdentity, "pid");

        // 公共字段：事件、范围、通道、pid、完整目标身份、总块数。
        std::string line = "event=";
        line += AuditEventName(record.event);
        line += " scope=" + ScopeDisplayName(ExtractIdentityField(record.targetIdentity, "scope"));
        line += " channel=" + ChannelDisplayName(ExtractIdentityField(record.targetIdentity, "ch"));
        line += " pid=" + (pidText.empty() ? std::string("?") : pidText);
        line += " target=" + (record.targetIdentity.empty() ? std::string("(none)") : record.targetIdentity);
        line += " blocksTotal=" + std::to_string(record.blocksTotal);

        // 事件专属字段：只写地址范围与长度、计数与标志，不写任何字节内容。
        if (record.event == ksword::memwb::AuditEvent::ApprovalAnswered)
        {
            line += " blockIndex=" + std::to_string(record.blockIndex);
            line += " range=" + FormatHex64(record.address) + "+" + std::to_string(record.length);
            line += " answer=";
            line += ApprovalAnswerName(record.approvalAnswer);
        }
        else if (record.event == ksword::memwb::AuditEvent::CommitFinished)
        {
            line += " outcome=";
            line += CommitOutcomeName(record.outcome);
            line += " blocksWritten=" + std::to_string(record.blocksWritten);
            line += " bytesWritten=" + std::to_string(record.bytesWritten);
            line += std::string(" scratchDirty=") + (record.scratchAreaDirty ? "1" : "0");
            line += std::string(" rmwWindow=") + (record.readModifyWriteWindow ? "1" : "0");
        }

        // 补充说明：清洗并截断后追加；为空不追加。
        const std::string note = SanitizeAuditNote(record.text);
        if (!note.empty())
        {
            line += " note=\"" + note + "\"";
        }
        return line;
    }
}
