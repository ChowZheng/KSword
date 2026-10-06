// ============================================================
// wpK1_tests.AuditFormat.cpp
// 作用：WorkbenchServicesAudit 里"审计记录 -> 日志文本"相关纯函数的逐分支断言：
//       ExtractIdentityField、SanitizeAuditNote、FormatAuditRecord。
// 重点（任务硬性要求）：
//   - 日志里只有地址范围与长度、计数与标志，绝不出现字节内容；
//   - 补充说明经清洗截断：不能含换行（日志注入）、不能无限长、截断不切 UTF-8 多字节序列；
//   - 目标身份串里的 scope/channel/pid 被拆成可读字段。
// ============================================================

#include "wpK1_common.h"

#include "../../../Ksword5.1/Ksword5.1/MemoryDock/WorkbenchServicesAudit.h"
#include "../../../shared/evidence/memory_workbench/MemoryTargetSession.h"

#include <string>

namespace svc = ksword::memwb_services_detail;

namespace
{
    // Contains：text 是否包含 needle。
    bool Contains(const std::string& text, const std::string& needle)
    {
        return text.find(needle) != std::string::npos;
    }

    // MakeIdentity：用真实的 IdentityKey 生成目标身份串，保证夹具与产品格式同源。
    std::string MakeIdentity(
        const ksword::memwb::Scope scope,
        const std::uint32_t pid,
        const ksword::memwb::Channel channel)
    {
        ksword::memwb::MemoryTargetSession session;
        session.scope = scope;
        session.pid = pid;
        session.channel = channel;
        session.processCreateTime100ns = 1234U;
        session.attachGeneration = 5U;
        return ksword::memwb::IdentityKey(session, 0x7FF600001000ULL, 4096U);
    }

    // IsValidUtf8：粗略校验 UTF-8（只检查起始字节与续字节的配对），用于断言截断不切断字符。
    bool IsValidUtf8(const std::string& text)
    {
        std::size_t index = 0U;
        while (index < text.size())
        {
            const unsigned char lead = static_cast<unsigned char>(text[index]);
            std::size_t extra = 0U;
            if (lead < 0x80U)
            {
                extra = 0U;
            }
            else if ((lead & 0xE0U) == 0xC0U)
            {
                extra = 1U;
            }
            else if ((lead & 0xF0U) == 0xE0U)
            {
                extra = 2U;
            }
            else if ((lead & 0xF8U) == 0xF0U)
            {
                extra = 3U;
            }
            else
            {
                // 孤立的续字节（0x80-0xBF）或非法起始字节。
                return false;
            }
            // 起始字节后面必须跟着足够数量的续字节。
            for (std::size_t offset = 1U; offset <= extra; ++offset)
            {
                if (index + offset >= text.size() ||
                    (static_cast<unsigned char>(text[index + offset]) & 0xC0U) != 0x80U)
                {
                    return false;
                }
            }
            index += extra + 1U;
        }
        return true;
    }

    // TestExtractIdentityField：从真实身份串里取字段。
    void TestExtractIdentityField()
    {
        const std::string identity = MakeIdentity(
            ksword::memwb::Scope::ProcessVirtual, 4321U, ksword::memwb::Channel::Hvm);

        WPK1_CHECK(svc::ExtractIdentityField(identity, "scope") == "0");
        WPK1_CHECK(svc::ExtractIdentityField(identity, "pid") == "4321");
        WPK1_CHECK(svc::ExtractIdentityField(identity, "ct") == "1234");
        WPK1_CHECK(svc::ExtractIdentityField(identity, "gen") == "5");
        WPK1_CHECK(svc::ExtractIdentityField(identity, "ch") == "2");
        // 最后一个字段（到串尾）。
        WPK1_CHECK(svc::ExtractIdentityField(identity, "len") == "4096");

        // 标签必须整词匹配："ch" 不会撞 "ct"，"c" 不是任何字段。
        WPK1_CHECK(svc::ExtractIdentityField(identity, "c").empty());
        WPK1_CHECK(svc::ExtractIdentityField(identity, "h").empty());
        // 缺失标签 / 空标签 / 空身份串。
        WPK1_CHECK(svc::ExtractIdentityField(identity, "nosuch").empty());
        WPK1_CHECK(svc::ExtractIdentityField(identity, "").empty());
        WPK1_CHECK(svc::ExtractIdentityField("", "pid").empty());
        // 第一个字段前面是版本前缀而不是 '|'：整串里仍能取到（"|scope=" 前有 '|'）。
        WPK1_CHECK(svc::ExtractIdentityField("memwb-target/1|scope=2|pid=0", "scope") == "2");
        // 值为空（"|pid=|"）返回空串，不越界。
        WPK1_CHECK(svc::ExtractIdentityField("a|pid=|x=1", "pid").empty());
    }

    // TestSanitizeNote：控制字符、引号、空白、截断。
    void TestSanitizeNote()
    {
        // 换行/回车/制表/DEL 都换成空格；首尾空格被去掉。
        WPK1_CHECK(svc::SanitizeAuditNote("line1\nline2") == "line1 line2");
        WPK1_CHECK(svc::SanitizeAuditNote("a\r\nb") == "a  b");
        WPK1_CHECK(svc::SanitizeAuditNote("a\tb") == "a b");
        WPK1_CHECK(svc::SanitizeAuditNote(std::string("a\x7F" "b")) == "a b");
        WPK1_CHECK(svc::SanitizeAuditNote("\n\nhello\n") == "hello");
        // 双引号换成单引号，保证日志里 note="..." 的引号配对。
        WPK1_CHECK(svc::SanitizeAuditNote("say \"hi\"") == "say 'hi'");
        // 全是空白/控制字符 -> 空串；空串 -> 空串。
        WPK1_CHECK(svc::SanitizeAuditNote(" \n\t ").empty());
        WPK1_CHECK(svc::SanitizeAuditNote("").empty());
        // 结果里不含任何控制字符（防日志注入）。
        const std::string cleaned = svc::SanitizeAuditNote(std::string("x\0y\x01z\x1F", 6));
        bool hasControl = false;
        for (const char ch : cleaned)
        {
            if (static_cast<unsigned char>(ch) < 0x20U || static_cast<unsigned char>(ch) == 0x7FU)
            {
                hasControl = true;
            }
        }
        WPK1_CHECK(!hasControl);

        // 恰好等于上限：不截断。
        const std::string exact(svc::kMaxAuditNoteBytes, 'a');
        WPK1_CHECK(svc::SanitizeAuditNote(exact) == exact);
        // 超过上限一字节：截断并追加 "..."，总长不超过上限 + 3。
        const std::string over(svc::kMaxAuditNoteBytes + 1U, 'b');
        const std::string truncated = svc::SanitizeAuditNote(over);
        WPK1_CHECK(truncated.size() == svc::kMaxAuditNoteBytes + 3U);
        WPK1_CHECK(truncated.substr(truncated.size() - 3U) == "...");
        WPK1_CHECK(truncated.substr(0U, svc::kMaxAuditNoteBytes) == std::string(svc::kMaxAuditNoteBytes, 'b'));

        // UTF-8 多字节：全是 3 字节汉字，截断点落在字符中间时回退到字符边界，结果仍是合法 UTF-8。
        std::string han;
        for (int count = 0; count < 100; ++count)
        {
            han += "\xE6\xB1\x89";
        }
        const std::string hanTruncated = svc::SanitizeAuditNote(han);
        WPK1_CHECK(hanTruncated.size() <= svc::kMaxAuditNoteBytes + 3U);
        WPK1_CHECK(hanTruncated.substr(hanTruncated.size() - 3U) == "...");
        WPK1_CHECK(IsValidUtf8(hanTruncated));
        // 240 = 80 个完整汉字，恰好落在边界：回退 0 字节。
        WPK1_CHECK(hanTruncated.size() == svc::kMaxAuditNoteBytes + 3U);

        // 截断点落在 2 字节字符的第二个字节上：前面垫 1 个 ASCII 使边界错位 1 字节。
        std::string shifted = "x";
        for (int count = 0; count < 200; ++count)
        {
            shifted += "\xC3\xA9";
        }
        const std::string shiftedTruncated = svc::SanitizeAuditNote(shifted);
        WPK1_CHECK(IsValidUtf8(shiftedTruncated));
        WPK1_CHECK(shiftedTruncated.substr(shiftedTruncated.size() - 3U) == "...");
    }

    // TestFormatCommon：公共字段（事件、范围、通道、pid、目标、块数）。
    void TestFormatCommon()
    {
        ksword::memwb::AuditRecord record;
        record.event = ksword::memwb::AuditEvent::CommitStarted;
        record.targetIdentity = MakeIdentity(
            ksword::memwb::Scope::KernelVirtual, 0U, ksword::memwb::Channel::StandardDriver);
        record.blocksTotal = 3U;

        const std::string line = svc::FormatAuditRecord(record);
        WPK1_CHECK(Contains(line, "event=CommitStarted"));
        WPK1_CHECK(Contains(line, "scope=KernelVirtual"));
        WPK1_CHECK(Contains(line, "channel=StandardDriver"));
        WPK1_CHECK(Contains(line, "pid=0"));
        WPK1_CHECK(Contains(line, "target=" + record.targetIdentity));
        WPK1_CHECK(Contains(line, "blocksTotal=3"));
        // 单行：不含换行。
        WPK1_CHECK(!Contains(line, "\n"));
        WPK1_CHECK(!Contains(line, "\r"));
        // CommitStarted 不带结束/强制同意专属字段。
        WPK1_CHECK(!Contains(line, "outcome="));
        WPK1_CHECK(!Contains(line, "range="));
        WPK1_CHECK(!Contains(line, "note="));

        // 范围与通道名映射：物理 + 磁盘传输；进程 + 用户态 + 非零 pid。
        record.targetIdentity = MakeIdentity(
            ksword::memwb::Scope::Physical, 0U, ksword::memwb::Channel::Ddma);
        const std::string physical = svc::FormatAuditRecord(record);
        WPK1_CHECK(Contains(physical, "scope=Physical"));
        WPK1_CHECK(Contains(physical, "channel=Ddma"));
        record.targetIdentity = MakeIdentity(
            ksword::memwb::Scope::ProcessVirtual, 777U, ksword::memwb::Channel::UserMode);
        const std::string process = svc::FormatAuditRecord(record);
        WPK1_CHECK(Contains(process, "scope=ProcessVirtual"));
        WPK1_CHECK(Contains(process, "channel=UserMode"));
        WPK1_CHECK(Contains(process, "pid=777"));

        // 身份串缺失或无法解析：显式写 unknown / ? / (none)，不省略字段也不崩溃。
        record.targetIdentity.clear();
        const std::string empty = svc::FormatAuditRecord(record);
        WPK1_CHECK(Contains(empty, "scope=unknown"));
        WPK1_CHECK(Contains(empty, "channel=unknown"));
        WPK1_CHECK(Contains(empty, "pid=?"));
        WPK1_CHECK(Contains(empty, "target=(none)"));
        record.targetIdentity = "memwb-target/1|scope=abc|ch=9|pid=1";
        const std::string garbage = svc::FormatAuditRecord(record);
        WPK1_CHECK(Contains(garbage, "scope=unknown"));
        WPK1_CHECK(Contains(garbage, "channel=unknown"));
        // 越界数值也是 unknown（不当成某个合法值）。
        record.targetIdentity = "memwb-target/1|scope=7|ch=4";
        const std::string outOfRange = svc::FormatAuditRecord(record);
        WPK1_CHECK(Contains(outOfRange, "scope=unknown"));
        WPK1_CHECK(Contains(outOfRange, "channel=unknown"));
    }

    // TestFormatApprovalAndFinished：强制同意与结束事件的专属字段。
    void TestFormatApprovalAndFinished()
    {
        ksword::memwb::AuditRecord approval;
        approval.event = ksword::memwb::AuditEvent::ApprovalAnswered;
        approval.targetIdentity = MakeIdentity(
            ksword::memwb::Scope::KernelVirtual, 0U, ksword::memwb::Channel::StandardDriver);
        approval.blocksTotal = 4U;
        approval.blockIndex = 2U;
        approval.address = 0xFFFFF80012345678ULL;
        approval.length = 64U;
        approval.approvalAnswer = ksword::memwb::ApprovalAnswer::RestOfBatch;
        const std::string approvalLine = svc::FormatAuditRecord(approval);
        WPK1_CHECK(Contains(approvalLine, "event=ApprovalAnswered"));
        WPK1_CHECK(Contains(approvalLine, "blockIndex=2"));
        // 地址范围：16 位大写十六进制起点 + 十进制长度；没有任何字节内容字段。
        WPK1_CHECK(Contains(approvalLine, "range=0xFFFFF80012345678+64"));
        WPK1_CHECK(Contains(approvalLine, "answer=RestOfBatch"));
        WPK1_CHECK(!Contains(approvalLine, "outcome="));

        ksword::memwb::AuditRecord finished;
        finished.event = ksword::memwb::AuditEvent::CommitFinished;
        finished.targetIdentity = approval.targetIdentity;
        finished.blocksTotal = 4U;
        finished.outcome = ksword::memwb::CommitOutcome::VerifyMismatch;
        finished.blocksWritten = 3U;
        finished.bytesWritten = 192U;
        finished.scratchAreaDirty = true;
        finished.readModifyWriteWindow = false;
        const std::string finishedLine = svc::FormatAuditRecord(finished);
        WPK1_CHECK(Contains(finishedLine, "event=CommitFinished"));
        WPK1_CHECK(Contains(finishedLine, "outcome=VerifyMismatch"));
        WPK1_CHECK(Contains(finishedLine, "blocksWritten=3"));
        WPK1_CHECK(Contains(finishedLine, "bytesWritten=192"));
        WPK1_CHECK(Contains(finishedLine, "scratchDirty=1"));
        WPK1_CHECK(Contains(finishedLine, "rmwWindow=0"));
        // 结束事件没有强制同意专属字段。
        WPK1_CHECK(!Contains(finishedLine, "answer="));
        WPK1_CHECK(!Contains(finishedLine, "blockIndex="));

        // 两个标志取反。
        finished.scratchAreaDirty = false;
        finished.readModifyWriteWindow = true;
        const std::string flipped = svc::FormatAuditRecord(finished);
        WPK1_CHECK(Contains(flipped, "scratchDirty=0"));
        WPK1_CHECK(Contains(flipped, "rmwWindow=1"));

        // 64 位计数不被截断。
        finished.bytesWritten = 0xFFFFFFFFFFFFFFFFULL;
        WPK1_CHECK(Contains(svc::FormatAuditRecord(finished), "bytesWritten=18446744073709551615"));
    }

    // TestFormatNote：补充说明的引号、清洗与截断都体现在最终日志行里。
    void TestFormatNote()
    {
        ksword::memwb::AuditRecord record;
        record.event = ksword::memwb::AuditEvent::CommitFinished;
        record.targetIdentity = "memwb-target/1|scope=0|pid=1|ch=0";

        // 没有说明：不出现 note=。
        WPK1_CHECK(!Contains(svc::FormatAuditRecord(record), "note="));

        // 说明带换行与引号：行内仍是单行，引号被替换，note 整体被双引号包住。
        record.text = "write failed:\nstatus=\"0xC0000022\"\r\nnext";
        const std::string line = svc::FormatAuditRecord(record);
        WPK1_CHECK(Contains(line, "note=\"write failed: status='0xC0000022'  next\""));
        WPK1_CHECK(!Contains(line, "\n"));
        WPK1_CHECK(!Contains(line, "\r"));

        // 超长说明被截断：整行长度有上界（公共字段 + 240 + 引号等），不会随说明长度增长。
        record.text = std::string(100000U, 'z');
        const std::string longLine = svc::FormatAuditRecord(record);
        WPK1_CHECK(longLine.size() < 1000U);
        WPK1_CHECK(Contains(longLine, "...\""));

        // 说明全是空白：视为没有说明。
        record.text = " \n\t ";
        WPK1_CHECK(!Contains(svc::FormatAuditRecord(record), "note="));
    }

    // TestNoByteContent：AuditRecord 的字段里没有字节内容；用一个"看起来像数据"的说明验证只有
    // 说明文本会被带出，且受清洗与截断约束（字节内容不可能经由其它字段进入日志）。
    void TestNoByteContent()
    {
        ksword::memwb::AuditRecord record;
        record.event = ksword::memwb::AuditEvent::CommitFinished;
        record.targetIdentity = "memwb-target/1|scope=0|pid=1|ch=0";
        record.address = 0x1000U;
        record.length = 4U;
        record.blocksWritten = 1U;
        record.bytesWritten = 4U;
        const std::string line = svc::FormatAuditRecord(record);
        // 数值字段只以十进制/十六进制地址形式出现，没有 "bytes=" / "data=" / "before=" / "after=" 一类字段。
        WPK1_CHECK(!Contains(line, "bytes="));
        WPK1_CHECK(!Contains(line, "data="));
        WPK1_CHECK(!Contains(line, "before="));
        WPK1_CHECK(!Contains(line, "after="));
        WPK1_CHECK(!Contains(line, "value="));
    }
}

namespace wpK1_test
{
    void RunAuditFormatTests()
    {
        TestExtractIdentityField();
        TestSanitizeNote();
        TestFormatCommon();
        TestFormatApprovalAndFinished();
        TestFormatNote();
        TestNoByteContent();
    }
}
