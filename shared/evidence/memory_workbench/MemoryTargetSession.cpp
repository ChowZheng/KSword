// ============================================================
// MemoryTargetSession.cpp
// 作用：
// - 实现 MemoryTargetSession.h 声明的会话校验、身份键、同目标判断与双代次计数器。
// - 全部是纯逻辑，不依赖 Qt 与 Win32。
// ============================================================

#include "MemoryTargetSession.h"

namespace ksword::memwb
{
    namespace
    {
        // kKeyVersionPrefix：身份键的版本前缀。键格式变动时必须同步升级这里的版本号，
        // 这样旧格式的缓存键不会和新格式的键意外相等。
        constexpr const char* kKeyVersionPrefix = "memwb-target/1";

        // kHexDigits：十六进制数位表，用于把基址写成固定宽度的小写十六进制。
        constexpr const char kHexDigits[] = "0123456789abcdef";

        // kHexWidth：uint64 写成十六进制的固定位数（16 位），不足补前导零。
        constexpr int kHexWidth = 16;

        // IsKnownScope：判断 scope 是否是三个合法取值之一。
        // 传入：scope 待判断的范围值。传出：合法为 true。
        bool IsKnownScope(const Scope scope) noexcept
        {
            // 逐个列出合法值，而不是比较数值上限，这样以后在枚举里插入新值时
            // 这里会被迫一起修改，而不是悄悄把新值放行。
            const bool isProcess = (scope == Scope::ProcessVirtual);
            const bool isKernel = (scope == Scope::KernelVirtual);
            const bool isPhysical = (scope == Scope::Physical);
            return isProcess || isKernel || isPhysical;
        }

        // IsKnownChannel：判断 channel 是否是四个合法取值之一。
        // 传入：channel 待判断的通道值。传出：合法为 true。
        bool IsKnownChannel(const Channel channel) noexcept
        {
            // 同 IsKnownScope：逐个列出，不依赖数值区间。
            const bool isUser = (channel == Channel::UserMode);
            const bool isDriver = (channel == Channel::StandardDriver);
            const bool isHvm = (channel == Channel::Hvm);
            const bool isDdma = (channel == Channel::Ddma);
            return isUser || isDriver || isHvm || isDdma;
        }

        // AppendField：向身份键末尾追加一项 "|标签=十进制值"。
        // 传入：key 被追加的字符串；label 字段标签（ASCII）；value 字段取值。
        // 传出：直接修改 key。所有字段都带标签与分隔符，因此任意两项数位不会粘连，
        // 键到字段的解码是唯一的（单射）。
        void AppendField(std::string& key, const char* const label, const std::uint64_t value)
        {
            // 分隔符、标签与等号。
            key.push_back('|');
            key.append(label);
            key.push_back('=');
            // 取值按十进制写出，to_string 对整数不受 locale 影响。
            key.append(std::to_string(value));
        }

        // AppendHexField：向身份键末尾追加一项 "|标签=0x十六位小写十六进制"。
        // 传入：key 被追加的字符串；label 字段标签；value 字段取值。
        // 传出：直接修改 key。基址用十六进制是为了让人读日志时能直接对上地址。
        void AppendHexField(std::string& key, const char* const label, std::uint64_t value)
        {
            // 分隔符、标签、等号与十六进制前缀。
            key.push_back('|');
            key.append(label);
            key.append("=0x");
            // 从最低位开始逐个取四位，填进缓冲区的末尾，得到固定宽度的十六进制。
            char digits[kHexWidth] = {};
            for (int index = kHexWidth - 1; index >= 0; --index)
            {
                digits[index] = kHexDigits[value & 0xFULL];
                value >>= 4;
            }
            key.append(digits, static_cast<std::size_t>(kHexWidth));
        }
    }

    // Validate：检查会话自身是否自洽，规则见头文件。
    SessionError Validate(const MemoryTargetSession& session) noexcept
    {
        // 第一步：先确认 scope 与 channel 都是合法枚举值。越界值后面的 pid 规则
        // 无从谈起，必须最先拒绝，并且 scope 排在 channel 之前。
        if (!IsKnownScope(session.scope))
        {
            return SessionError::BadScope;
        }
        if (!IsKnownChannel(session.channel))
        {
            return SessionError::BadChannel;
        }

        // 第二步：pid 规则。进程虚拟地址空间必须带 pid；内核与物理空间与进程
        // 无关，pid 必须为 0，否则说明调用方把"进程目标"的状态残留带了过来。
        if (session.scope == Scope::ProcessVirtual)
        {
            if (session.pid == 0)
            {
                return SessionError::NeedsPid;
            }
        }
        else
        {
            if (session.pid != 0)
            {
                return SessionError::PidMustBeZero;
            }
        }

        // 第三步：地址宽度只能是 32 或 64，其它值一律拒绝，不做就近取整。
        const bool widthOk = (session.addressBits == 32U) || (session.addressBits == 64U);
        if (!widthOk)
        {
            return SessionError::BadAddressBits;
        }

        // 三步都通过，会话自洽。
        return SessionError::None;
    }

    // IdentityKey：生成"目标 + 基址 + 长度"的稳定身份字符串，格式见头文件。
    std::string IdentityKey(
        const MemoryTargetSession& session,
        const std::uint64_t baseAddress,
        const std::uint64_t lengthBytes)
    {
        // key：正在拼装的身份键，先放版本前缀。
        std::string key(kKeyVersionPrefix);

        // 会话的七个字段，按固定顺序追加；枚举取其数值（数值已在头文件里钉死）。
        AppendField(key, "scope", static_cast<std::uint64_t>(session.scope));
        AppendField(key, "pid", session.pid);
        AppendField(key, "ct", session.processCreateTime100ns);
        AppendField(key, "gen", session.attachGeneration);
        AppendField(key, "ch", static_cast<std::uint64_t>(session.channel));
        AppendField(key, "ddma", session.ddmaGeneration);
        AppendField(key, "bits", session.addressBits);

        // 读取范围：基址用十六进制，长度用十进制。
        AppendHexField(key, "base", baseAddress);
        AppendField(key, "len", lengthBytes);
        return key;
    }

    // SameTarget：七个字段全部相等才是同一个目标。
    bool SameTarget(const MemoryTargetSession& a, const MemoryTargetSession& b) noexcept
    {
        // 逐字段比较，任何一个不等就立刻判不同。这里刻意不用 memcmp：
        // 结构体里有对齐填充，填充字节的内容是未定义的。
        if (a.scope != b.scope)
        {
            return false;
        }
        if (a.pid != b.pid)
        {
            return false;
        }
        if (a.processCreateTime100ns != b.processCreateTime100ns)
        {
            return false;
        }
        if (a.attachGeneration != b.attachGeneration)
        {
            return false;
        }
        if (a.channel != b.channel)
        {
            return false;
        }
        if (a.ddmaGeneration != b.ddmaGeneration)
        {
            return false;
        }
        if (a.addressBits != b.addressBits)
        {
            return false;
        }
        return true;
    }

    // SessionRevisions 构造：把两个计数器置为给定初值。
    SessionRevisions::SessionRevisions(
        const std::uint64_t initialSource,
        const std::uint64_t initialContent) noexcept
        : sourceRevision_(initialSource)
        , contentRevision_(initialContent)
    {
    }

    // BumpSource：来源代次加一。无符号加法的溢出是有定义的回绕，最大值之后是 0。
    void SessionRevisions::BumpSource() noexcept
    {
        ++sourceRevision_;
    }

    // BumpContent：内容代次加一，回绕规则同 BumpSource。
    void SessionRevisions::BumpContent() noexcept
    {
        ++contentRevision_;
    }

    // Source：返回当前来源代次。
    std::uint64_t SessionRevisions::Source() const noexcept
    {
        return sourceRevision_;
    }

    // Content：返回当前内容代次。
    std::uint64_t SessionRevisions::Content() const noexcept
    {
        return contentRevision_;
    }

    // Capture：把两个计数器当前的值打包成快照。
    RevisionSnapshot SessionRevisions::Capture() const noexcept
    {
        RevisionSnapshot snapshot;
        snapshot.source = sourceRevision_;
        snapshot.content = contentRevision_;
        return snapshot;
    }

    // IsStale：任一计数器与快照不等即陈旧。两者都核对，只核对一个会放过另一类陈旧回调。
    bool SessionRevisions::IsStale(const RevisionSnapshot& captured) const noexcept
    {
        // 来源代次变过：换过目标或重读过，旧结果对应的是另一份数据。
        const bool sourceChanged = (captured.source != sourceRevision_);
        // 内容代次变过：编辑或撤销过，旧结果没有包含这些改动。
        const bool contentChanged = (captured.content != contentRevision_);
        return sourceChanged || contentChanged;
    }
}
