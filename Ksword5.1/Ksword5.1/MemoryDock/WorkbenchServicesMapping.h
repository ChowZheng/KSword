#pragma once

// ============================================================
// WorkbenchServicesMapping.h
// 作用：
// - 内存工作台 Phase 3 WP-K1（MemoryDock 宿主侧生产实现）里"最容易写错、又最适合
//   离线穷举"的纯函数集合：模块记录映射、进程模块快照判定、指针读取结果映射、
//   页保护属性徽章、反汇编/汇编结果判据、地址簿目录选择。
// - 本文件及其 .cpp 是**纯函数**：不包含 Windows.h、不包含 Qt、不包含 Framework.h，
//   不发起任何 Win32/驱动调用；只用标准库与 Core（ksword::memwb）值类型。正因为
//   "纯"，tools/memwb_ui/wpK1/ 夹具才能用手工构造的输入逐分支断言它们，不需要真的
//   打开进程、读驱动或起一个 Dock。
// - 真正触碰系统的那一跳（MemoryDock.WorkbenchServices.*.cpp）只负责"问一次系统"，
//   问完把原始字段交给这里的函数翻译，不在各自文件里各写一份判据。
//
// ============================================================
// 冻结接口摘要（改名或改语义须先同步通知）
// ============================================================
//   namespace ksword::memwb_services_detail
//   MapRawModule / MapRawModules          原始模块字段 -> ksword::memwb::ModuleRecord
//   JudgeProcessModuleSnapshot            进程模块快照诊断文本 -> 枚举是否成功
//   FormatKernelModuleFailure             内核模块枚举失败说明（英文技术串）
//   DecodePointerLittleEndian / MapPointerRead / IsPointerReadAllowed
//   FindProcessName                       候选进程列表按 pid 取名
//   FormatProtectionBadge                 页状态/保护/类型 -> 状态条"保护"段徽章
//   AcceptDecodedRow / JudgeAssembleResult 反汇编/汇编后端结果判据
//   JoinPath / ChooseWritableDirectory    地址簿文件路径选择
//   KernelModuleQueryStatusName           内核模块枚举状态值 -> 英文状态名
//   IsPatchChannelAllowed / BuildPatchSession  int3 补丁的通道准入与会话构造
// ============================================================

#include "../UI/MemoryWorkbench/WorkbenchServices.h"
#include "../../../shared/evidence/memory_workbench/Int3PatchLedger.h"
#include "../../../shared/evidence/memory_workbench/MemoryIoPort.h"
#include "../../../shared/evidence/memory_workbench/MemoryModuleDirectory.h"
#include "../../../shared/evidence/memory_workbench/MemoryProcessMatch.h"
#include "../../../shared/evidence/memory_workbench/MemoryTargetSession.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ksword::memwb_services_detail
{
    // ------------------------------------------------------------
    // 一、模块记录映射
    // ------------------------------------------------------------

    // RawModuleInfo：从系统枚举结果里取出来的"原始模块字段"，与具体来源
    // （ks::process::ProcessModuleRecord / KernelThreadAuditTab::ModuleRecord）无关。
    struct RawModuleInfo
    {
        // name：模块文件名（UTF-8，不含目录）；为空时由 fullPath 的最后一个分量补。
        std::string name;
        // fullPath：完整路径（UTF-8）。进程模块是 Win32 路径，内核模块是 NT 路径。
        std::string fullPath;
        // base：模块加载基址；为 0 视为无效记录。
        std::uint64_t base = 0;
        // size：模块映像大小（字节）。
        std::uint64_t size = 0;
    };

    // FileNameFromPath：取路径的最后一个分量（同时认 '\\' 与 '/'）。
    // 传入：path 完整路径（可为空）。传出：最后一个分量；没有分隔符时原样返回；
    //       以分隔符结尾时返回空串。
    std::string FileNameFromPath(const std::string& path);

    // MapRawModule：把一条原始模块字段映射成 ModuleRecord。
    // 传入：raw 原始字段；recordOut 输出（不可为空）。
    // 传出：true 表示映射成功；base 为 0、或 name 为空且无法从 fullPath 补出文件名
    //       （路径为空或以分隔符结尾，即取不出任何可查询的名字）时返回 false 且
    //       recordOut 保持不变。
    bool MapRawModule(const RawModuleInfo& raw, ksword::memwb::ModuleRecord* recordOut);

    // MapRawModules：批量映射，丢弃无效项，并按基址升序稳定排序（与旧模块缓存
    // 提交处的排序规则一致，保证同一输入永远得到同一顺序）。
    std::vector<ksword::memwb::ModuleRecord> MapRawModules(const std::vector<RawModuleInfo>& rawList);

    // ModuleSnapshotVerdict：进程模块快照"枚举本身是否成功"的判定结果。
    struct ModuleSnapshotVerdict
    {
        // ok：true 表示枚举本身成功（即便模块为空）。
        bool ok = false;
        // failure：ok 为 false 时的失败说明（英文技术串）；ok 为 true 时恒为空。
        std::string failure;
    };

    // JudgeProcessModuleSnapshot：依据 ks::process 枚举返回的诊断文本判定成功与否。
    // 背景：ks::process::EnumerateProcessModulesAndThreads* 没有单独的"成功"标志，
    //       空模块列表既可能是"进程真的没有模块"，也可能是"身份核验失败/打不开进程"。
    //       唯一可靠的成功标记是诊断文本里的 "Module source:" 记录（Toolhelp 或 PSAPI
    //       任一路径枚举成功时才会追加）；没有它就一定是失败，失败文本取诊断全文。
    // 传入：diagnosticText 诊断全文；moduleCount 模块数（仅用于空诊断时的兜底说明）。
    ModuleSnapshotVerdict JudgeProcessModuleSnapshot(
        const std::string& diagnosticText,
        std::size_t moduleCount);

    // FormatKernelModuleFailure：内核模块枚举失败说明（英文技术串）。
    // 传入：statusName 枚举状态名（如 "SnapshotFailed"）；ntStatus 最后一次原生
    //       NTSTATUS；requiredBytes 预查询所需缓冲区字节数。
    std::string FormatKernelModuleFailure(
        const char* statusName,
        std::uint32_t ntStatus,
        std::uint32_t requiredBytes);

    // KernelModuleQueryStatusName：内核模块枚举状态值（KernelThreadAuditTab::
    // ModuleQueryStatus 的底层整数）-> 英文状态名，供 FormatKernelModuleFailure 使用。
    // 传入：statusValue 状态整数（0 Ok、1 ApiUnavailable、2 LengthQueryFailed、
    //       3 SnapshotFailed）；传出：静态字符串，越界值返回 "Unknown"。
    // 宿主侧 .cpp 用 static_assert 核对这四个数值与枚举一致，枚举被改动会编译失败。
    const char* KernelModuleQueryStatusName(std::uint32_t statusValue) noexcept;

    // ------------------------------------------------------------
    // 二、指针读取
    // ------------------------------------------------------------

    // IsPointerReadAllowed：解引用是否允许经本通道/范围发起。
    // 规则：物理范围与磁盘传输（DDMA）通道一律不允许——每次解引用都会改写磁盘暂存
    //       扇区，且输入时反复求值不可接受（目标与解析器设计的明确约束）；服务实现
    //       在发起读之前再核对一遍，作为解析器闸门之外的第二道保险。
    bool IsPointerReadAllowed(ksword::memwb::Scope scope, ksword::memwb::Channel channel) noexcept;

    // DecodePointerLittleEndian：把小端字节序列解成零扩展的指针值。
    // 传入：data 读到的字节；width 指针宽度（只接受 4 或 8）；valueOut 输出。
    // 传出：true 表示 width 合法且 data.size()==width；否则 false 且 *valueOut=0。
    bool DecodePointerLittleEndian(
        const std::vector<std::uint8_t>& data,
        std::uint32_t width,
        std::uint64_t* valueOut);

    // MapPointerRead：把端口读取结果映射成 PointerReadResult。
    // 规则：只有 Ok 且字节数恰好等于 width 才算成功；Partial/Unreadable/Failed 各自带
    //       不同的英文失败说明（并附上端口给出的细节串），绝不拿部分字节拼指针。
    ks::ui::PointerReadResult MapPointerRead(
        const ksword::memwb::IoReadResult& read,
        std::uint32_t width);

    // ------------------------------------------------------------
    // 三、候选进程
    // ------------------------------------------------------------

    // FindProcessName：在候选列表里按 pid 取进程名（取第一个同 pid 项；pid 为 0 或
    // 没有命中返回空串）。
    std::string FindProcessName(
        const std::vector<ksword::memwb::ProcessCandidate>& candidates,
        std::uint32_t pid);

    // ------------------------------------------------------------
    // 四、页保护属性徽章（状态条"保护"段）
    // ------------------------------------------------------------

    // Win32 页状态/保护/类型常量的本地副本：本文件不包含 Windows.h，这些数值是
    // 稳定的 Win32 ABI；MemoryDock.WorkbenchServices.Gate.cpp 里有 static_assert
    // 逐个核对它们与 winnt.h 的宏一致，宏被改动会立刻编译失败而不是静默错配。
    inline constexpr std::uint32_t kMemCommit = 0x1000U;
    inline constexpr std::uint32_t kMemReserve = 0x2000U;
    inline constexpr std::uint32_t kMemFree = 0x10000U;
    inline constexpr std::uint32_t kMemPrivate = 0x20000U;
    inline constexpr std::uint32_t kMemMapped = 0x40000U;
    inline constexpr std::uint32_t kMemImage = 0x1000000U;
    inline constexpr std::uint32_t kPageNoAccess = 0x01U;
    inline constexpr std::uint32_t kPageReadOnly = 0x02U;
    inline constexpr std::uint32_t kPageReadWrite = 0x04U;
    inline constexpr std::uint32_t kPageWriteCopy = 0x08U;
    inline constexpr std::uint32_t kPageExecute = 0x10U;
    inline constexpr std::uint32_t kPageExecuteRead = 0x20U;
    inline constexpr std::uint32_t kPageExecuteReadWrite = 0x40U;
    inline constexpr std::uint32_t kPageExecuteWriteCopy = 0x80U;
    inline constexpr std::uint32_t kPageGuard = 0x100U;

    // BadgeRole：徽章配色语义，与 ks::ui::StatusRole 一一对应（后者在 Qt 头里，
    // 本文件不能包含，宿主侧 .cpp 负责转换）。
    enum class BadgeRole
    {
        Idle,
        Info,
        Success,
        Warning,
        Error
    };

    // ProtectionBadge：徽章文字与配色。
    struct ProtectionBadge
    {
        // text：纯 ASCII 技术缩写，不需要翻译（例如 "RWX (PRV)"）。
        std::string text;
        // role：配色语义。
        BadgeRole role = BadgeRole::Idle;
    };

    // FormatProtectionBadge：按 MEMORY_BASIC_INFORMATION 的三个字段生成徽章。
    // 规则（ux.md §5 第③段：RWX 红 / X 橙 / RW 绿）：
    //   FREE 区域 -> "FREE"（Idle）；RESERVE 区域 -> "RSV"（Idle）；
    //   已提交页：NA / R / RW / RWC / X / RX / RWX / RWXC；
    //     含执行且含写（RWX、RWXC）-> Error（红）；
    //     含执行不含写（X、RX）-> Warning（橙）；
    //     含写不含执行（RW、RWC）-> Success（绿）；R -> Info；NA 与未知 -> Idle；
    //   PAGE_GUARD 追加 "+G" 并把非 Error 的配色提升到 Warning（读取守护页会触发异常）；
    //   区域类型追加 " (IMG)" / " (MAP)" / " (PRV)"（其余类型值不追加）。
    // 传入：state/protect/type 即 MEMORY_BASIC_INFORMATION 的 State/Protect/Type。
    ProtectionBadge FormatProtectionBadge(
        std::uint32_t state,
        std::uint32_t protect,
        std::uint32_t type);

    // ------------------------------------------------------------
    // 五、反汇编/汇编后端结果判据
    // ------------------------------------------------------------

    // kMaxInstructionBytes：x86/x64 单条指令的最大字节数；解码后端一次最多取这么多。
    inline constexpr std::size_t kMaxInstructionBytes = 15U;
    // kMaxAssembledBytes：汇编结果的字节数上限（与汇编核心的 64 KiB 输出上限一致，
    // 这里再核一次，保证"有界"不依赖核心内部实现不被改动）。
    inline constexpr std::size_t kMaxAssembledBytes = 65536U;

    // AcceptDecodedRow：解码后端返回的这一行能不能当作"真实解码出的指令"交给视图。
    // 规则：必须同时满足——后端标记为已解码（decodedFlag）、来自 Zydis（fromZydis，
    // 不接受"有界降级解码器"的结果：它不区分指令边界，正是旧缺陷 E-02 的来源）、
    // 长度非零、不超过 available、不超过 kMaxInstructionBytes。任一不满足就返回
    // false，调用方据此返回 nullopt，让视图把这一个字节标成 db 并从下一字节重同步。
    bool AcceptDecodedRow(
        bool decodedFlag,
        bool fromZydis,
        std::size_t rowLength,
        std::size_t available) noexcept;

    // AssembleRejection：汇编结果被拒绝的原因。
    enum class AssembleRejection
    {
        // None：接受。
        None,
        // BackendFailed：后端自己报告失败（原因与行号由后端给出，原样透传）。
        BackendFailed,
        // EmptyOutput：后端报告成功却没有输出机器码（协议上不该出现，按失败处理）。
        EmptyOutput,
        // OutputTooLarge：输出超过 kMaxAssembledBytes。
        OutputTooLarge
    };

    // JudgeAssembleResult：汇编后端结果判据。失败（含 EmptyOutput/OutputTooLarge）时
    // 调用方必须清空机器码，绝不返回部分机器码。
    // 传入：success 后端报告是否成功；byteCount 后端给出的机器码字节数。
    AssembleRejection JudgeAssembleResult(bool success, std::size_t byteCount) noexcept;

    // ------------------------------------------------------------
    // 六、地址簿文件路径
    // ------------------------------------------------------------

    // JoinPath：拼接目录与文件名，只在目录尾部没有分隔符时补一个 '/'。
    // 传入：dir 目录；fileName 文件名。传出：dir 或 fileName 为空时返回空串。
    std::string JoinPath(const std::string& dir, const std::string& fileName);

    // ChooseWritableDirectory：从候选目录里按顺序选第一个"可写"的。
    // 传入：candidates 候选目录（按优先级排序，空串被跳过）；isWritable 判定回调
    //       （宿主侧负责真正创建目录并做写入探测）。
    // 传出：第一个 isWritable 返回 true 的候选；全部不可写返回空串。
    std::string ChooseWritableDirectory(
        const std::vector<std::string>& candidates,
        const std::function<bool(const std::string&)>& isWritable);

    // ------------------------------------------------------------
    // 七、int3 补丁的字节存储会话
    // ------------------------------------------------------------

    // IsPatchChannelAllowed：int3 补丁允许使用的通道。
    // 规则：只允许 UserMode / StandardDriver / Hvm（进程范围上的三条通道）；磁盘传输
    //       （Ddma）与越界通道值一律拒绝——int3 零摩擦换来的是"少一道核对"，不是"在
    //       磁盘暂存扇区上也零摩擦"（设计文档 §0 第 17 条）。
    bool IsPatchChannelAllowed(ksword::memwb::Channel channel) noexcept;

    // BuildPatchSession：为一次 int3 字节读写构造会话。
    // 传入：target 补丁目标身份（pid + 创建时间 + 附加代次）；channel 必须使用的通道
    //       （安装时取当前通道，还原时取安装通道，由 Int3Controller 区分）。
    // 传出：范围恒为进程虚拟范围（int3 只允许进程范围）、pid/创建时间/附加代次取自
    //       target、ddmaGeneration 为 0（Ddma 通道已被拒绝，不需要代次）、addressBits
    //       取 64（单字节读写不依赖地址宽度，端口也不读这个字段）。
    ksword::memwb::MemoryTargetSession BuildPatchSession(
        const ksword::memwb::PatchTarget& target,
        ksword::memwb::Channel channel);
}
