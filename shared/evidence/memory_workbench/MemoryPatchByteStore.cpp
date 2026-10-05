// MemoryPatchByteStore.h 的实现。范围/通道检查必须排在任何端口调用之前，
// 这样被拒绝的请求才能保证"零读写"，供测试用调用计数直接断言。

#include "MemoryPatchByteStore.h"

namespace ksword::memwb
{
    MemoryPatchByteStore::MemoryPatchByteStore(IMemoryIoPort& port, const MemoryTargetSession& session)
        : port_(port)
        , session_(session)
    {
    }

    bool MemoryPatchByteStore::IsSupportedRoute() const
    {
        // 只认"进程虚拟范围"；内核/物理范围一律拒绝。
        if (session_.scope != Scope::ProcessVirtual)
        {
            return false;
        }
        // 磁盘传输通道一律拒绝；UserMode/StandardDriver/Hvm 三条都放行。
        if (session_.channel == Channel::Ddma)
        {
            return false;
        }
        return true;
    }

    bool MemoryPatchByteStore::ReadByte(const std::uint64_t address, std::uint8_t& valueOut)
    {
        if (!IsSupportedRoute())
        {
            // 不发起任何端口调用：被排除的通道上"读不到"和"没问"必须有区别，
            // 调用方（Int3PatchLedger）据此得到 ReadFailed，不会误当成目标本身不可读。
            return false;
        }

        const IoReadResult outcome = port_.Read(session_, address, 1ULL);
        if (outcome.status != IoReadStatus::Ok || outcome.data.size() != 1U)
        {
            return false;
        }
        valueOut = outcome.data[0];
        return true;
    }

    bool MemoryPatchByteStore::WriteByte(const std::uint64_t address, const std::uint8_t value)
    {
        if (!IsSupportedRoute())
        {
            return false;
        }

        const std::vector<std::uint8_t> payload{ value };
        // approved 固定传 false：零摩擦止步于"不弹二次核对框"，端口如果要求
        // 显式同意，这里不会自动带着同意标志重试，直接按失败返回。
        const IoWriteResult outcome = port_.Write(session_, address, payload, /*approved=*/false);
        return outcome.ok && outcome.bytesDone == 1U;
    }
}
