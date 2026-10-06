// ============================================================
// wpK1_tests.AuditSink.cpp
// 作用：生产审计接收器（MemoryDock.WorkbenchServices.Audit.cpp 里的 WorkbenchAuditLogSink）的
//       运行期测试——链接真实的项目日志实现（ksword/log/log.cpp），把审计记录喂进接收器，再从
//       全局日志仓库 KswordARKEventEntry 里读回去断言。
// 覆盖 AGENTS.md 日志语法的两条硬要求：
//   1) 每条日志都带 kLogEvent（GUID 非空）；
//   2) 同一次提交链路（开始 -> 确认 -> 强制同意 -> 结束）的全部审计复用**同一个** GUID，
//      不同链路 GUID 不同；链路结束后新的提交得到新 GUID。
// 另外覆盖：级别映射、文本字段、重入（Busy）复用链路、落单事件不丢、淘汰后审计仍然落盘、
//       多线程并发下链路不串、说明文本里的换行不会把一条审计拆成多行。
// 说明：日志框架会把每一行打印到 std::cout，这里在喂记录期间把 std::cout 的缓冲区临时换成
//       丢弃缓冲区，避免淘汰/并发测试刷屏，汇总行恒为最后一行。
// ============================================================

#include "wpK1_common.h"

#include "../../../Ksword5.1/Ksword5.1/Framework.h"
#include "../../../Ksword5.1/Ksword5.1/MemoryDock/MemoryDock.WorkbenchServices.Internal.h"
#include "../../../shared/evidence/memory_workbench/MemoryTargetSession.h"

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // DiscardBuffer：丢弃所有输出的流缓冲区，用来静音日志框架的控制台打印。
    class DiscardBuffer final : public std::streambuf
    {
    protected:
        // overflow：吞掉每个字符并报告成功。
        int_type overflow(const int_type character) override
        {
            return traits_type::not_eof(character);
        }
    };

    // ConsoleMuter：构造时把 std::cout 重定向到丢弃缓冲区，析构时恢复。
    class ConsoleMuter
    {
    public:
        ConsoleMuter()
            : previous_(std::cout.rdbuf(&discard_))
        {
        }

        ~ConsoleMuter()
        {
            std::cout.rdbuf(previous_);
        }

        ConsoleMuter(const ConsoleMuter&) = delete;
        ConsoleMuter& operator=(const ConsoleMuter&) = delete;

    private:
        // discard_：丢弃缓冲区。声明在 previous_ 之前，保证初始化时已构造完成。
        DiscardBuffer discard_;
        // previous_：被替换掉的原缓冲区。
        std::streambuf* previous_;
    };

    // MakeIdentity：用真实 IdentityKey 生成目标身份串。
    std::string MakeIdentity(const std::uint32_t pid)
    {
        ksword::memwb::MemoryTargetSession session;
        session.scope = ksword::memwb::Scope::ProcessVirtual;
        session.pid = pid;
        session.channel = ksword::memwb::Channel::UserMode;
        return ksword::memwb::IdentityKey(session, 0x1000U, 64U);
    }

    // MakeRecord：构造一条审计记录。
    ksword::memwb::AuditRecord MakeRecord(
        const ksword::memwb::AuditEvent event,
        const std::string& identity)
    {
        ksword::memwb::AuditRecord record;
        record.event = event;
        record.targetIdentity = identity;
        record.blocksTotal = 2U;
        return record;
    }

    // Snapshot：当前全部日志行的拷贝。
    std::vector<kEvent> Snapshot()
    {
        return KswordARKEventEntry.Snapshot();
    }

    // GuidText：GUID 的文本形式，便于放进 set/map。
    std::string GuidText(const GUID& guid)
    {
        return GuidToString(guid);
    }

    // IsZeroGuid：是否全零（日志系统生成 GUID 失败时的兜底值）。
    bool IsZeroGuid(const GUID& guid)
    {
        return IsSameGuid(guid, GUID{});
    }

    // Contains：text 是否包含 needle。
    bool Contains(const std::string& text, const std::string& needle)
    {
        return text.find(needle) != std::string::npos;
    }

    // TestChainSharesOneEvent：一条完整链路的全部行共用同一个 GUID，两条链路互不相同，
    // 并核对级别与文本字段。
    void TestChainSharesOneEvent()
    {
        const std::unique_ptr<ksword::memwb::IAuditSink> sink = ks::ui::workbench_dock::detail::CreateAuditLogSink();
        WPK1_CHECK(sink != nullptr);
        if (sink == nullptr)
        {
            return;
        }

        const std::string identityA = MakeIdentity(1111U);
        const std::string identityB = MakeIdentity(2222U);
        const std::size_t before = Snapshot().size();
        {
            ConsoleMuter muter;
            sink->Record(MakeRecord(ksword::memwb::AuditEvent::CommitStarted, identityA));
            sink->Record(MakeRecord(ksword::memwb::AuditEvent::UiConfirmSuppressed, identityA));

            ksword::memwb::AuditRecord approval = MakeRecord(ksword::memwb::AuditEvent::ApprovalAnswered, identityA);
            approval.blockIndex = 1U;
            approval.address = 0x7FF600001000ULL;
            approval.length = 64U;
            approval.approvalAnswer = ksword::memwb::ApprovalAnswer::Deny;
            sink->Record(approval);

            ksword::memwb::AuditRecord finished = MakeRecord(ksword::memwb::AuditEvent::CommitFinished, identityA);
            finished.outcome = ksword::memwb::CommitOutcome::VerifyMismatch;
            finished.blocksWritten = 1U;
            finished.bytesWritten = 64U;
            finished.scratchAreaDirty = true;
            sink->Record(finished);

            // 另一条链路：Started -> Finished(Committed)。
            sink->Record(MakeRecord(ksword::memwb::AuditEvent::CommitStarted, identityB));
            ksword::memwb::AuditRecord finishedB = MakeRecord(ksword::memwb::AuditEvent::CommitFinished, identityB);
            finishedB.outcome = ksword::memwb::CommitOutcome::Committed;
            sink->Record(finishedB);
        }

        const std::vector<kEvent> rows = Snapshot();
        WPK1_CHECK(rows.size() == before + 6U);
        if (rows.size() != before + 6U)
        {
            return;
        }
        const kEvent* const chain = rows.data() + before;

        // 每一行都有非零 GUID（带 kLogEvent）。
        for (std::size_t index = 0U; index < 6U; ++index)
        {
            WPK1_CHECK(!IsZeroGuid(chain[index].guid));
        }

        // 同一条链路四行共用同一个 GUID；另一条链路两行共用另一个 GUID。
        WPK1_CHECK(IsSameGuid(chain[0].guid, chain[1].guid));
        WPK1_CHECK(IsSameGuid(chain[0].guid, chain[2].guid));
        WPK1_CHECK(IsSameGuid(chain[0].guid, chain[3].guid));
        WPK1_CHECK(IsSameGuid(chain[4].guid, chain[5].guid));
        WPK1_CHECK(!IsSameGuid(chain[0].guid, chain[4].guid));

        // Track(GUID) 能把同一条链路的审计一次取全（日志管理器的链路追踪能力）。
        WPK1_CHECK(KswordARKEventEntry.Track(chain[0].guid).size() == 4U);
        WPK1_CHECK(KswordARKEventEntry.Track(chain[4].guid).size() == 2U);

        // 级别映射：开始/抑制 Info；强制同意 Deny -> Warn；脏扇区的结束 -> Error；
        // 正常提交的结束 -> Info。
        WPK1_CHECK(chain[0].level == kLogLevel::Info);
        WPK1_CHECK(chain[1].level == kLogLevel::Info);
        WPK1_CHECK(chain[2].level == kLogLevel::Warn);
        WPK1_CHECK(chain[3].level == kLogLevel::Error);
        WPK1_CHECK(chain[4].level == kLogLevel::Info);
        WPK1_CHECK(chain[5].level == kLogLevel::Info);

        // 文本字段。
        WPK1_CHECK(Contains(chain[0].content, "[MemoryWorkbench][Audit] event=CommitStarted"));
        WPK1_CHECK(Contains(chain[1].content, "event=UiConfirmSuppressed"));
        WPK1_CHECK(Contains(chain[2].content, "event=ApprovalAnswered"));
        WPK1_CHECK(Contains(chain[2].content, "range=0x00007FF600001000+64"));
        WPK1_CHECK(Contains(chain[2].content, "answer=Deny"));
        WPK1_CHECK(Contains(chain[3].content, "outcome=VerifyMismatch"));
        WPK1_CHECK(Contains(chain[3].content, "scratchDirty=1"));
        WPK1_CHECK(Contains(chain[0].content, "target=" + identityA));
        WPK1_CHECK(Contains(chain[4].content, "target=" + identityB));
        WPK1_CHECK(Contains(chain[0].content, "pid=1111"));
        WPK1_CHECK(Contains(chain[4].content, "pid=2222"));

        // 链路已关闭：同一个目标再来一次提交，得到新的 GUID。
        {
            ConsoleMuter muter;
            sink->Record(MakeRecord(ksword::memwb::AuditEvent::CommitStarted, identityA));
            sink->Record(MakeRecord(ksword::memwb::AuditEvent::CommitFinished, identityA));
        }
        const std::vector<kEvent> after = Snapshot();
        WPK1_CHECK(after.size() == before + 8U);
        if (after.size() == before + 8U)
        {
            WPK1_CHECK(IsSameGuid(after[before + 6U].guid, after[before + 7U].guid));
            WPK1_CHECK(!IsSameGuid(after[before + 6U].guid, chain[0].guid));
        }
    }

    // TestReentrantCommitSharesChain：Commit 进行中又来一次 Commit（第二次以 Busy 结束）——
    // 两次调用的全部审计挂在同一条链路上，内层的结束不拆掉外层链路。
    void TestReentrantCommitSharesChain()
    {
        const std::unique_ptr<ksword::memwb::IAuditSink> sink = ks::ui::workbench_dock::detail::CreateAuditLogSink();
        const std::string identity = MakeIdentity(3333U);
        const std::size_t before = Snapshot().size();
        {
            ConsoleMuter muter;
            // 外层 Started；内层（重入）Started + Finished(Busy)；外层继续 + Finished。
            sink->Record(MakeRecord(ksword::memwb::AuditEvent::CommitStarted, identity));
            sink->Record(MakeRecord(ksword::memwb::AuditEvent::CommitStarted, identity));
            ksword::memwb::AuditRecord busy = MakeRecord(ksword::memwb::AuditEvent::CommitFinished, identity);
            busy.outcome = ksword::memwb::CommitOutcome::Busy;
            sink->Record(busy);
            sink->Record(MakeRecord(ksword::memwb::AuditEvent::UiConfirmAccepted, identity));
            ksword::memwb::AuditRecord done = MakeRecord(ksword::memwb::AuditEvent::CommitFinished, identity);
            done.outcome = ksword::memwb::CommitOutcome::Committed;
            sink->Record(done);
        }
        const std::vector<kEvent> rows = Snapshot();
        WPK1_CHECK(rows.size() == before + 5U);
        if (rows.size() != before + 5U)
        {
            return;
        }
        for (std::size_t index = 1U; index < 5U; ++index)
        {
            WPK1_CHECK(IsSameGuid(rows[before].guid, rows[before + index].guid));
        }
        // Busy 的结束是 Warn，最终的 Committed 是 Info。
        WPK1_CHECK(rows[before + 2U].level == kLogLevel::Warn);
        WPK1_CHECK(rows[before + 4U].level == kLogLevel::Info);

        // 两层都关闭之后，新的提交得到新 GUID。
        {
            ConsoleMuter muter;
            sink->Record(MakeRecord(ksword::memwb::AuditEvent::CommitStarted, identity));
        }
        const std::vector<kEvent> again = Snapshot();
        WPK1_CHECK(again.size() == before + 6U);
        if (again.size() == before + 6U)
        {
            WPK1_CHECK(!IsSameGuid(again[before + 5U].guid, rows[before].guid));
        }
    }

    // TestOrphanEventsAreNotLost：没有 Started 就到达的事件（例如链路已被淘汰）也必须落盘，
    // 且挂在一条新链路上，Finished 之后关闭。
    void TestOrphanEventsAreNotLost()
    {
        const std::unique_ptr<ksword::memwb::IAuditSink> sink = ks::ui::workbench_dock::detail::CreateAuditLogSink();
        const std::string identity = MakeIdentity(4444U);
        const std::size_t before = Snapshot().size();
        {
            ConsoleMuter muter;
            sink->Record(MakeRecord(ksword::memwb::AuditEvent::ApprovalAnswered, identity));
            ksword::memwb::AuditRecord done = MakeRecord(ksword::memwb::AuditEvent::CommitFinished, identity);
            done.outcome = ksword::memwb::CommitOutcome::WriteFailed;
            sink->Record(done);
            sink->Record(MakeRecord(ksword::memwb::AuditEvent::CommitStarted, identity));
        }
        const std::vector<kEvent> rows = Snapshot();
        WPK1_CHECK(rows.size() == before + 3U);
        if (rows.size() != before + 3U)
        {
            return;
        }
        WPK1_CHECK(!IsZeroGuid(rows[before].guid));
        // 落单事件与随后的 Finished 同链路；Finished 关闭它之后的 Started 是新链路。
        WPK1_CHECK(IsSameGuid(rows[before].guid, rows[before + 1U].guid));
        WPK1_CHECK(!IsSameGuid(rows[before + 1U].guid, rows[before + 2U].guid));
        WPK1_CHECK(rows[before + 1U].level == kLogLevel::Error);
    }

    // TestEvictionNeverDropsAudit：超过链路上限（落单链路累积）时淘汰最早的链路，但每一条审计
    // 记录仍然产生一行日志——淘汰只影响"挂在哪个事件上"，不影响"有没有记录"。
    void TestEvictionNeverDropsAudit()
    {
        const std::unique_ptr<ksword::memwb::IAuditSink> sink = ks::ui::workbench_dock::detail::CreateAuditLogSink();
        const std::size_t before = Snapshot().size();
        constexpr std::uint32_t kOpenChains = 100U;
        {
            ConsoleMuter muter;
            // 只开始、永不结束的链路 100 条（超过内部上限 64）。
            for (std::uint32_t index = 0U; index < kOpenChains; ++index)
            {
                sink->Record(MakeRecord(ksword::memwb::AuditEvent::CommitStarted, MakeIdentity(10000U + index)));
            }
            // 最早的那条已被淘汰：它的结束仍然要有一行日志。
            ksword::memwb::AuditRecord done = MakeRecord(
                ksword::memwb::AuditEvent::CommitFinished, MakeIdentity(10000U));
            done.outcome = ksword::memwb::CommitOutcome::Committed;
            sink->Record(done);
        }
        const std::vector<kEvent> rows = Snapshot();
        WPK1_CHECK(rows.size() == before + kOpenChains + 1U);
        if (rows.size() == before + kOpenChains + 1U)
        {
            WPK1_CHECK(Contains(rows.back().content, "event=CommitFinished"));
            WPK1_CHECK(Contains(rows.back().content, "pid=10000"));
            WPK1_CHECK(!IsZeroGuid(rows.back().guid));
        }
    }

    // TestNoteIsSingleLine：补充说明里的换行与引号不会把一条审计拆成多行，也不会破坏引号。
    void TestNoteIsSingleLine()
    {
        const std::unique_ptr<ksword::memwb::IAuditSink> sink = ks::ui::workbench_dock::detail::CreateAuditLogSink();
        const std::string identity = MakeIdentity(5555U);
        const std::size_t before = Snapshot().size();
        {
            ConsoleMuter muter;
            ksword::memwb::AuditRecord done = MakeRecord(ksword::memwb::AuditEvent::CommitFinished, identity);
            done.outcome = ksword::memwb::CommitOutcome::WriteFailed;
            done.text = "line one\nline \"two\"\r\nline three";
            sink->Record(done);
        }
        const std::vector<kEvent> rows = Snapshot();
        WPK1_CHECK(rows.size() == before + 1U);
        if (rows.size() == before + 1U)
        {
            const std::string& content = rows[before].content;
            WPK1_CHECK(!Contains(content, "\n"));
            WPK1_CHECK(!Contains(content, "\r"));
            WPK1_CHECK(Contains(content, "note=\"line one line 'two'  line three\""));
        }
    }

    // TestConcurrentChainsDoNotMix：多线程各自并发地写自己的链路，每条链路的行必须共用唯一一个
    // GUID，不同链路的 GUID 互不相同，且没有任何行丢失。
    void TestConcurrentChainsDoNotMix()
    {
        const std::unique_ptr<ksword::memwb::IAuditSink> sink = ks::ui::workbench_dock::detail::CreateAuditLogSink();
        constexpr int kThreads = 4;
        constexpr int kChainsPerThread = 100;
        const std::size_t before = Snapshot().size();
        {
            ConsoleMuter muter;
            std::vector<std::thread> threads;
            for (int threadIndex = 0; threadIndex < kThreads; ++threadIndex)
            {
                threads.emplace_back([&sink, threadIndex]() {
                    for (int chain = 0; chain < kChainsPerThread; ++chain)
                    {
                        // 每条链路一个唯一目标身份：pid = 20000 + 线程号 * 1000 + 序号。
                        const std::string identity = MakeIdentity(
                            static_cast<std::uint32_t>(20000 + threadIndex * 1000 + chain));
                        sink->Record(MakeRecord(ksword::memwb::AuditEvent::CommitStarted, identity));
                        sink->Record(MakeRecord(ksword::memwb::AuditEvent::UiConfirmAccepted, identity));
                        ksword::memwb::AuditRecord done =
                            MakeRecord(ksword::memwb::AuditEvent::CommitFinished, identity);
                        done.outcome = ksword::memwb::CommitOutcome::Committed;
                        sink->Record(done);
                    }
                });
            }
            for (std::thread& thread : threads)
            {
                thread.join();
            }
        }

        const std::vector<kEvent> rows = Snapshot();
        const std::size_t expectedRows = static_cast<std::size_t>(kThreads) * kChainsPerThread * 3U;
        WPK1_CHECK(rows.size() == before + expectedRows);
        if (rows.size() != before + expectedRows)
        {
            return;
        }

        // 按行内 pid 字段分组，统计每组的行数与不同 GUID 数。
        std::map<std::string, std::set<std::string>> guidsByPid;
        std::map<std::string, int> rowsByPid;
        for (std::size_t index = before; index < rows.size(); ++index)
        {
            const std::string& content = rows[index].content;
            const std::size_t pidPos = content.find(" pid=");
            const std::size_t pidEnd = content.find(' ', pidPos + 5U);
            const std::string pid = content.substr(pidPos + 5U, pidEnd - (pidPos + 5U));
            guidsByPid[pid].insert(GuidText(rows[index].guid));
            ++rowsByPid[pid];
        }
        WPK1_CHECK(guidsByPid.size() == static_cast<std::size_t>(kThreads * kChainsPerThread));
        bool everyChainHasOneGuid = true;
        bool everyChainHasThreeRows = true;
        std::set<std::string> allGuids;
        for (const auto& entry : guidsByPid)
        {
            everyChainHasOneGuid = everyChainHasOneGuid && entry.second.size() == 1U;
            allGuids.insert(entry.second.begin(), entry.second.end());
        }
        for (const auto& entry : rowsByPid)
        {
            everyChainHasThreeRows = everyChainHasThreeRows && entry.second == 3;
        }
        WPK1_CHECK(everyChainHasOneGuid);
        WPK1_CHECK(everyChainHasThreeRows);
        // 400 条链路 400 个互不相同的 GUID。
        WPK1_CHECK(allGuids.size() == static_cast<std::size_t>(kThreads * kChainsPerThread));
    }
}

namespace wpK1_test
{
    void RunAuditSinkTests()
    {
        TestChainSharesOneEvent();
        TestReentrantCommitSharesChain();
        TestOrphanEventsAreNotLost();
        TestEvictionNeverDropsAudit();
        TestNoteIsSingleLine();
        TestConcurrentChainsDoNotMix();
    }
}
