#pragma once

// ============================================================
// MemoryPatchByteStore.h
// 作用：
// - 实现 Int3PatchLedger.h 里的 IPatchByteStore，把"int3 补丁零摩擦"这条用户
//   决策（设计文档第 0 节第 17 条、第 6 节第 1 条：int3 走会话通道，不接写
//   事务的二次核对链路）落到 IMemoryIoPort 这一份真实读写接口上。
// - Qt-free、Win32-free，只经 IMemoryIoPort 做单字节 I/O。
//
// 范围限制（故意比 MemoryIoByteStore 窄得多）：
// - 只支持"进程虚拟范围 + 用户态/标准驱动/私有页表窗口"三条通道；内核范围、
//   物理范围、磁盘传输通道一律拒绝，且**不发起任何端口调用**——int3 补丁
//   零摩擦换来的是"少一道核对"，不是"在高风险通道上也零摩擦"，这几类通道
//   被排除是设计文档的明确决定，不是本类自己加的限制。
// - 写入时 approved 固定传 false：端口如果要求显式同意（needsApproval），
//   本类把它当成普通失败返回（Install/Restore 因此拒绝），不会自动继续重试
//   带同意标志的那一次——零摩擦止步于"不弹二次核对框"，不延伸到"自动越权"。
//
// ============================================================
// 冻结接口摘要
// ============================================================
//   class MemoryPatchByteStore : IPatchByteStore
//     构造(IMemoryIoPort&, const MemoryTargetSession&)
//     ReadByte(address, valueOut) -> bool
//     WriteByte(address, value) -> bool
//
// ============================================================
// 规则
// ============================================================
// - 范围/通道检查（ReadByte 与 WriteByte 共用，检查顺序固定）：
//     scope != ProcessVirtual        -> 拒绝（内核/物理范围）；
//     channel == Ddma                -> 拒绝（磁盘传输通道）；
//   两条检查都不通过才会继续发起端口调用；被拒绝时端口调用次数恒为 0。
// - ReadByte：调用 port.Read(session, address, 1)；status==Ok 且 data.size()==1
//   时写入 valueOut 并返回 true；其余情况（Partial/Unreadable/Failed，或端口
//   违反契约返回了别的长度）一律返回 false 且不碰 valueOut。
// - WriteByte：调用 port.Write(session, address, {value}, /*approved=*/false)；
//   ok 为真且 bytesDone==1 时返回 true；其余情况（含 needsApproval、
//   partial、失败）一律返回 false——对调用方（Int3PatchLedger）而言这与
//   "写入失败"没有区别，账本按它既有的 WriteFailed/VerifyFailed 规则处理。
// ============================================================

#include "Int3PatchLedger.h"
#include "MemoryIoPort.h"
#include "MemoryTargetSession.h"

#include <cstdint>

namespace ksword::memwb
{
    // MemoryPatchByteStore：int3 补丁账本的真实读写落点，规则见文件头。
    class MemoryPatchByteStore final : public IPatchByteStore
    {
    public:
        // 构造。
        // 传入：port 真实或假的 I/O 端口；session 当前目标会话（只读引用）。
        // 引用必须比本对象活得久。
        MemoryPatchByteStore(IMemoryIoPort& port, const MemoryTargetSession& session);

        MemoryPatchByteStore(const MemoryPatchByteStore&) = delete;
        MemoryPatchByteStore& operator=(const MemoryPatchByteStore&) = delete;

        // ReadByte：见文件头规则。
        bool ReadByte(std::uint64_t address, std::uint8_t& valueOut) override;

        // WriteByte：见文件头规则。
        bool WriteByte(std::uint64_t address, std::uint8_t value) override;

    private:
        // IsSupportedRoute：范围/通道检查，ReadByte 与 WriteByte 共用。
        bool IsSupportedRoute() const;

        // port_：真实或假的 I/O 端口。
        IMemoryIoPort& port_;
        // session_：当前目标会话（只读）。
        const MemoryTargetSession& session_;
    };
}
