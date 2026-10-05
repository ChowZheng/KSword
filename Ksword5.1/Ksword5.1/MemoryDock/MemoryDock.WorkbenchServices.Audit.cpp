#include "../Framework.h"

#include "MemoryDock.WorkbenchServices.Internal.h"
#include "WorkbenchServicesAudit.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

// ============================================================
// MemoryDock.WorkbenchServices.Audit.cpp
// 作用：
// - 生产审计接收器 WorkbenchAuditLogSink（ksword::memwb::IAuditSink 的实现）：把写事务产出的
//   每一条 AuditRecord 写进项目日志系统（Framework.h 的 info/warn/err 流 + eol）。
// - "跳过 UI 确认不跳过审计"：每个事件（CommitStarted / UiConfirmAccepted / UiConfirmDenied /
//   UiConfirmSuppressed / ApprovalAnswered / CommitFinished）各记一条，没有任何事件被省略。
// - 日志级别、文本格式、控制字符清洗、链路登记表都在纯函数文件 WorkbenchServicesAudit.*，
//   这里只负责"取到链路事件、选对流、写一行"。
//
// 日志语法（Ksword5.1/AGENTS.md 第 2 节）：
// - 每条日志都先有一个 kLogEvent 并传入流；同一次提交链路（开始 -> 确认 -> 强制同意 -> 结束）
//   的全部审计复用**同一个** kLogEvent，不到处临时创建：登记表按目标身份串为键保存链路事件，
//   CommitStarted 打开链路，CommitFinished 关闭链路；重入的提交（Busy）复用同一条链路。
// - 日志文本是内部诊断串（英文字段名 + 数值），**不含任何字节内容**：AuditRecord 本身没有
//   字节字段，只有地址范围、长度、计数与标志。
//
// 线程：Record 可能被多个视图在 UI 线程调用，也不排除工作线程；登记表由互斥锁保护，日志框架
// 本身线程安全（每个线程各自的待写缓冲）。日志在锁外写出，持锁时间只覆盖登记表的增删查。
// ============================================================

namespace svc = ksword::memwb_services_detail;

namespace
{
    // kMaxAuditChains：同时保留的提交链路上限。正常情况下同时只有一条（提交在 UI 线程同步
    // 执行），上限只是防止注入接口异常留下"只有开始没有结束"的落单链路无限增长。
    constexpr std::size_t kMaxAuditChains = 64U;

    // AuditLogChain：一次提交链路在日志系统里的身份——一个 kLogEvent。
    // kLogEvent 含 const 成员（GUID 不可修改），可拷贝构造但不可赋值，所以登记表按指针保存。
    struct AuditLogChain
    {
        // event：本条链路的日志事件；默认构造即生成新的 GUID。
        kLogEvent event;
    };

    // WorkbenchAuditLogSink：生产审计接收器，详见文件头。
    class WorkbenchAuditLogSink final : public ksword::memwb::IAuditSink
    {
    public:
        // Record：记录一条审计。
        // 传入：record 写事务给出的审计记录。
        // 行为：算出日志文本与级别 -> 取（或新开）本次提交链路的日志事件 -> 写一行日志 ->
        //       CommitFinished 时关闭链路。接收器自身的任何异常都不得中断写事务管线：
        //       捕获后补一条错误日志说明"这条审计没能格式化"。
        void Record(const ksword::memwb::AuditRecord& record) override
        {
            try
            {
                RecordImpl(record);
            }
            catch (...)
            {
                // 到这里说明格式化或登记表操作抛了异常（通常是内存不足）。审计不能静默丢失，
                // 但也不能让异常穿过写事务：用一个新事件记一条最小化的错误日志。
                try
                {
                    kLogEvent failureEvent;
                    err << failureEvent
                        << "[MemoryWorkbench][Audit] audit record could not be formatted, event="
                        << svc::AuditEventName(record.event)
                        << eol;
                }
                catch (...)
                {
                    // 连错误日志都写不出来：没有更多可做的了，放弃。
                }
            }
        }

    private:
        // RecordImpl：Record 的正常路径，可能抛异常，由 Record 兜底。
        void RecordImpl(const ksword::memwb::AuditRecord& record)
        {
            // 纯函数部分在锁外完成：级别与文本。
            const svc::AuditLogLevel level = svc::ClassifyAuditRecord(record);
            const std::string text = "[MemoryWorkbench][Audit] " + svc::FormatAuditRecord(record);

            // 取得本次提交链路的日志事件：拷贝一份出来，之后在锁外写日志。
            // - CommitStarted：打开（或在重入时加深）链路；
            // - 其它事件：找已有链路，找不到（例如链路被上限淘汰）就当场补开一条，保证
            //   这条审计仍然有事件可挂，不丢；
            // - CommitFinished：写完这条之后关闭一层链路（拷贝已经取出，关闭不影响写入）。
            std::optional<kLogEvent> chainEvent;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                AuditLogChain* chain = nullptr;
                if (record.event == ksword::memwb::AuditEvent::CommitStarted)
                {
                    chain = &chains_.Open(record.targetIdentity, AuditLogChain{});
                }
                else
                {
                    chain = chains_.Find(record.targetIdentity);
                    if (chain == nullptr)
                    {
                        chain = &chains_.Open(record.targetIdentity, AuditLogChain{});
                    }
                }
                chainEvent.emplace(chain->event);
                if (record.event == ksword::memwb::AuditEvent::CommitFinished)
                {
                    chains_.Close(record.targetIdentity);
                }
            }

            // 按级别选流；eol 提交这一行并自动附带文件/行号/函数。
            switch (level)
            {
            case svc::AuditLogLevel::Info:
                info << *chainEvent << text << eol;
                break;
            case svc::AuditLogLevel::Warn:
                warn << *chainEvent << text << eol;
                break;
            case svc::AuditLogLevel::Error:
                err << *chainEvent << text << eol;
                break;
            }
        }

        // mutex_：保护 chains_。
        std::mutex mutex_;
        // chains_：提交链路登记表，键是目标身份串。
        svc::AuditChainRegistry<AuditLogChain> chains_{kMaxAuditChains};
    };
}

namespace ks::ui::workbench_dock::detail
{
    std::unique_ptr<ksword::memwb::IAuditSink> CreateAuditLogSink()
    {
        return std::make_unique<WorkbenchAuditLogSink>();
    }
}
