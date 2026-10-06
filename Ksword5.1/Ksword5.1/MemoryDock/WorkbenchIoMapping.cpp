#include "WorkbenchIoMapping.h"

// ============================================================
// WorkbenchIoMapping.cpp
// 作用：
// - 实现 WorkbenchIoMapping.h 声明的三个纯函数。本文件不含任何 Win32 调用、
//   不 include 任何 Windows.h 之外会拉来真实 I/O 的头，只做"按字段值分支、
//   拼一个新结构体"这件事；因此 tools/memwb_ui/ 的夹具可以用 MSVC 直接把
//   这个文件链接进一个离屏测试程序，喂手工构造的 AccessOutcome /
//   VirtualMemoryReadResult 穷举每一个分支，不需要真的打开进程或驱动句柄。
// ============================================================

namespace
{
    // QByteArrayToVector：QByteArray -> std::vector<uint8_t> 的转换，只在本
    // 文件内部使用（与 WorkbenchIoPorts.cpp 里同名的私有小工具各自一份，
    // 两个文件都不对外导出，没有共用的必要）。空数组单独判断，避免依赖
    // constData() 在空数组上的具体行为。
    // 传入：bytes 门面读回的数据；传出：等长的 vector。
    std::vector<std::uint8_t> QByteArrayToVector(const QByteArray& bytes)
    {
        if (bytes.isEmpty())
        {
            return std::vector<std::uint8_t>();
        }
        return std::vector<std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()),
            reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
    }
}

namespace ksword::memwb_ports_detail
{
    // MapFacadeReadOutcome：实现见 WorkbenchIoMapping.h 声明处的详细注释。
    ksword::memwb::IoReadResult MapFacadeReadOutcome(
        const ksword::memory_backend::AccessOutcome& outcome)
    {
        ksword::memwb::IoReadResult result;
        result.scratchAreaDirty = outcome.scratchDirty;
        result.readModifyWriteWindow = outcome.lostUpdateWindow;

        if (outcome.scratchDirty)
        {
            result.status = ksword::memwb::IoReadStatus::Failed;
            result.failure = outcome.failureText.toStdString();
            return result;
        }

        if (outcome.ok && !outcome.partial)
        {
            result.status = ksword::memwb::IoReadStatus::Ok;
            result.data = QByteArrayToVector(outcome.data);
            if (!outcome.failureText.isEmpty())
            {
                // 成功但仍带文本：私有页表窗口回退注记一类的说明，原样搬运。
                result.failure = outcome.failureText.toStdString();
            }
            return result;
        }

        const bool partialByFailureWithData = !outcome.ok && !outcome.data.isEmpty();
        if ((outcome.ok && outcome.partial) || partialByFailureWithData)
        {
            result.status = ksword::memwb::IoReadStatus::Partial;
            result.data = QByteArrayToVector(outcome.data);
            if (!outcome.failureText.isEmpty())
            {
                result.failure = outcome.failureText.toStdString();
            }
            return result;
        }

        result.status = ksword::memwb::IoReadStatus::Unreadable;
        result.failure = outcome.failureText.toStdString();
        return result;
    }

    // MapFacadeWriteOutcome：实现见 WorkbenchIoMapping.h 声明处的详细注释。
    ksword::memwb::IoWriteResult MapFacadeWriteOutcome(
        const ksword::memory_backend::AccessOutcome& outcome)
    {
        ksword::memwb::IoWriteResult result;
        result.ok = outcome.ok;
        result.partial = outcome.partial;
        result.bytesDone = outcome.bytesDone;
        result.needsApproval = outcome.forceRequired;
        result.rolledBack = false;
        result.scratchAreaDirty = outcome.scratchDirty;
        result.readModifyWriteWindow = outcome.lostUpdateWindow;
        result.failure = outcome.failureText.toStdString();
        return result;
    }

    // MapStandardDriverVirtualRead：实现见 WorkbenchIoMapping.h 声明处的详细
    // 注释；判据已对照 KswordARKDriver/src/features/process/process_memory.c
    // 第 890-915 行（不带 ZERO_FILL_UNREADABLE 的分支）核实；长度三支是 R-1
    // 的修复点。
    ksword::memwb::IoReadResult MapStandardDriverVirtualRead(
        const ksword::ark::VirtualMemoryReadResult& driverResult)
    {
        ksword::memwb::IoReadResult result;

        if (!driverResult.io.ok)
        {
            result.status = ksword::memwb::IoReadStatus::Failed;
            result.failure = "标准驱动通道通信失败：" + driverResult.io.message;
            return result;
        }

        switch (driverResult.readStatus)
        {
        case KSWORD_ARK_MEMORY_READ_STATUS_OK:
            if (driverResult.data.size() == static_cast<std::size_t>(driverResult.requestedBytes))
            {
                result.status = ksword::memwb::IoReadStatus::Ok;
                result.data.assign(driverResult.data.cbegin(), driverResult.data.cend());
                return result;
            }
            if (!driverResult.data.empty()
                && driverResult.data.size() < static_cast<std::size_t>(driverResult.requestedBytes))
            {
                // R-1：readStatus==OK 但响应被截断，真实字节数少于请求字节
                // 数——绝不能当作"整段都读到了"直接报 Ok，否则上层会把前缀
                // 之后、从未真实读到的内容也当成有效数据。按真实长度报
                // Partial，data 即真实前缀。
                result.status = ksword::memwb::IoReadStatus::Partial;
                result.data.assign(driverResult.data.cbegin(), driverResult.data.cend());
                result.failure = "标准驱动通道报告 OK，但响应被截断，实际字节数少于请求字节数。";
                return result;
            }
            if (driverResult.data.empty())
            {
                // R-1：readStatus==OK 但一个字节都没带回来——没有真实数据可
                // 用，不能报 Partial（Partial 要求非空前缀），也不能报 Ok
                // （违反 IoReadResult 的契约：Ok 时 data.size()==length）。
                // 这是协议层面的异常响应，按 Failed 处理，不是"目标不可读"。
                result.status = ksword::memwb::IoReadStatus::Failed;
                result.failure = "标准驱动通道报告 OK，但未带回任何字节。";
                return result;
            }
            // 防御性兜底：data 比 requestedBytes 还长（协议不应出现），仍按
            // Ok 处理但裁到 requestedBytes，保持 IoReadResult 的契约。
            result.status = ksword::memwb::IoReadStatus::Ok;
            result.data.assign(
                driverResult.data.cbegin(),
                driverResult.data.cbegin() + static_cast<std::ptrdiff_t>(driverResult.requestedBytes));
            return result;

        case KSWORD_ARK_MEMORY_READ_STATUS_PARTIAL_COPY:
            if (driverResult.data.empty())
            {
                // 协议上不应出现（PARTIAL_COPY 意味着 bytesRead>0），防御性
                // 兜底：没有真实前缀就不能报 Partial。
                result.status = ksword::memwb::IoReadStatus::Unreadable;
                result.failure = "标准驱动通道报告部分读取，但未带回任何字节。";
                return result;
            }
            // 不看 data.size() 与 requestedBytes 的长度关系：即便出现
            // "PARTIAL_COPY 且 data.size()>=requestedBytes"这种协议上矛盾的
            // 组合，也只按"有没有真实前缀"判断，保持 Partial，不在这里额外
            // 升级成 Ok（那需要调用方自己核对长度，不是这一层的职责）。
            result.status = ksword::memwb::IoReadStatus::Partial;
            result.data.assign(driverResult.data.cbegin(), driverResult.data.cend());
            return result;

        case KSWORD_ARK_MEMORY_READ_STATUS_COPY_FAILED:
            result.status = ksword::memwb::IoReadStatus::Unreadable;
            result.failure = "目标虚拟地址不可读（内存复制 0 字节）。";
            return result;

        default:
            // 协议不匹配 / 进程查找失败 / 范围被拒 / 响应缓冲不足等，都是通道
            // 自身出了问题，不是"目标不可读"，按 Failed 处理。
            result.status = ksword::memwb::IoReadStatus::Failed;
            result.failure =
                "标准驱动通道返回未预期的读取状态(" +
                std::to_string(driverResult.readStatus) + ")。";
            return result;
        }
    }
}
