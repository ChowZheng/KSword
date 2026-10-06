#pragma once

// ============================================================
// MemoryTargetSession.h
// 作用：
// - 描述"十六进制/内存查看器当前在看什么"：目标范围、进程身份、读取通道、
//   各类代次。取代现状里散落在十余处的目标状态副本。
// - 提供一份、且只有一份的内核地址判据（kKernelSplit / IsKernelVirtualAddress）。
// - 提供 SessionRevisions：把"换目标/重读"与"编辑/撤销"拆成两个独立计数器，
//   让异步回调落地前可以同时核对两者，丢弃陈旧结果。
//
// 为什么要有这个模块：
// - 现状里"目标是什么"被分别存在十余个控件里，换目标时漏改其中一处，界面照样
//   显示、驱动照样去读，只是读的是上一个目标。这类错误不会报错，必须把目标收敛
//   成一个值类型，并且让"两次读取是不是同一个目标"变成一次纯比较。
// - 内核地址阈值现状有两处且互不一致：一处用 0xFFFF000000000000，一处用
//   0xFFFF800000000000。后者才是 x64 规范地址的内核半区起点。之后只允许这里这一份。
// - 快照代次原先实为内容代次：重读与编辑共用一个计数器，导致"重读后旧编辑回调
//   被误当成新鲜"。拆成两个计数器之后，陈旧判据必须两者皆核对才算新鲜。
//
// 规则：
// - 通道是四选一的显式枚举，**没有"自动"项**，也不提供任何"自动选择/回退到另一
//   通道"的函数。通道选错必须由调用方显式报告，不能由本模块悄悄换成别的通道。
// - Validate 只检查"这个会话自身是否自洽"，不检查目标进程是否存在、驱动是否
//   已加载——那些属于运行期状态，不在纯逻辑层。
// - IdentityKey 与 SameTarget 都不调用 Validate：它们只比较，不裁决。调用方应当
//   先 Validate 再使用结果。
// - 计数器溢出按无符号回绕定义（2^64-1 之后下一个值是 0），回绕后与回绕前的快照
//   仍然不相等，因此仍判陈旧。唯一的盲区是恰好经过 2^64 次递增，物理上不可达。
// - 仅使用标准库，不包含 Windows.h，不包含任何 Qt 头。
// ============================================================

#include <cstdint>
#include <string>

namespace ksword::memwb
{
    // Scope：地址空间的种类，决定地址的含义与 pid 的取值规则。
    // 数值固定，因为它会进入 IdentityKey 字符串，改动数值等于改动键格式。
    enum class Scope : std::uint32_t
    {
        // ProcessVirtual：某个进程的用户态虚拟地址空间，必须带非零 pid。
        ProcessVirtual = 0,
        // KernelVirtual：系统内核虚拟地址空间，与具体进程无关，pid 必须为 0。
        KernelVirtual = 1,
        // Physical：物理地址空间，与具体进程无关，pid 必须为 0。
        Physical = 2,
    };

    // Channel：读写目标内存所走的通道。顺序与数值固定，没有"自动"项。
    // 数值固定的原因同 Scope：它会进入 IdentityKey。
    enum class Channel : std::uint32_t
    {
        // UserMode：用户态 API 通道（R3）。
        UserMode = 0,
        // StandardDriver：标准驱动通道（R0，经 KswordARK 设备）。
        StandardDriver = 1,
        // Hvm：虚拟化监视器通道（R-1）。
        Hvm = 2,
        // Ddma：磁盘 DMA 通道。
        Ddma = 3,
    };

    // SessionError：Validate 的返回值。None 之外的每一项对应一种自洽性违规。
    // 前四项与设计文档约定一致；BadScope/BadChannel 是对枚举越界值的防御：
    // 持久化设置损坏或类型强转出错时，越界值不能被当作某个合法值放行。
    enum class SessionError : std::uint32_t
    {
        // None：会话自洽。
        None = 0,
        // NeedsPid：进程虚拟地址空间要求 pid 非零，但 pid 为 0。
        NeedsPid = 1,
        // PidMustBeZero：内核/物理地址空间要求 pid 为 0，但 pid 非零。
        PidMustBeZero = 2,
        // BadAddressBits：addressBits 既不是 32 也不是 64。
        BadAddressBits = 3,
        // BadScope：scope 不是 Scope 的三个合法值之一。
        BadScope = 4,
        // BadChannel：channel 不是 Channel 的四个合法值之一。
        BadChannel = 5,
    };

    // MemoryTargetSession：查看器当前目标的完整描述。Dock 持有一份权威，
    // 各宿主只持只读拷贝，并用代次字段判断自己的拷贝是否已过期。
    // 默认构造出来的会话**故意是无效的**：ProcessVirtual 且 pid 为 0，Validate
    // 返回 NeedsPid。这样"还没选目标"的状态无法被误用来读任何东西。
    struct MemoryTargetSession
    {
        // scope：地址空间种类。
        Scope scope = Scope::ProcessVirtual;
        // pid：目标进程号。ProcessVirtual 时必须非零；其余 Scope 必须为 0。
        std::uint32_t pid = 0;
        // processCreateTime100ns：目标进程创建时间（100ns 单位）。与 pid 一起标识
        // 一个进程实例，防止 pid 被系统复用后把新进程当成旧进程。
        std::uint64_t processCreateTime100ns = 0;
        // attachGeneration：每次附加到目标（含重新附加同一目标）时递增的代次。
        std::uint64_t attachGeneration = 0;
        // channel：当前读写通道。默认 UserMode，是权限最低的一档。
        Channel channel = Channel::UserMode;
        // ddmaGeneration：DDMA 通道的暂存扇区代次；换了暂存扇区，旧读取不再可信。
        std::uint64_t ddmaGeneration = 0;
        // addressBits：目标地址宽度，只能是 32 或 64。
        std::uint32_t addressBits = 64;
    };

    // kKernelSplit：x64 规范地址中内核半区的起点。**全仓库只允许这一份阈值。**
    // 低于它的地址不是内核虚拟地址（含用户半区与不规范空洞），不低于它的才是。
    inline constexpr std::uint64_t kKernelSplit = 0xFFFF800000000000ULL;

    // IsKernelVirtualAddress：判断一个 64 位地址是否落在内核虚拟地址半区。
    // 调用方法：传入任意 64 位地址；返回 address >= kKernelSplit。
    // 传入：address 待判断的地址。传出：true 表示内核半区。
    // 注意：它不是规范性检查，不规范空洞内的地址一律返回 false；32 位目标的地址
    // 都小于 2^32，因此恒返回 false，调用方不需要为 32 位另写一套阈值。
    constexpr bool IsKernelVirtualAddress(const std::uint64_t address) noexcept
    {
        return address >= kKernelSplit;
    }

    // Validate：检查会话自身是否自洽。
    // 调用方法：Validate(session)；返回 SessionError::None 才可继续使用该会话。
    // 传入：session 待检查的会话。传出：第一个被发现的违规，或 None。
    // 检查顺序固定为 scope 合法、channel 合法、pid 规则、addressBits，
    // 同时存在多处违规时返回排在前面的那一项。
    SessionError Validate(const MemoryTargetSession& session) noexcept;

    // IdentityKey：生成"目标 + 基址 + 长度"的稳定身份字符串。
    // 调用方法：两次读取的 IdentityKey 相等才允许保留上一次读取的结果。
    // 传入：session 目标会话；baseAddress 读取基址；lengthBytes 读取长度（字节）。
    // 传出：形如 memwb-target/1|scope=0|pid=1234|...|base=0x...|len=4096 的字符串。
    // 保证：七个会话字段加基址、长度共九项任一不同，字符串必然不同；
    // 字符串只含 ASCII，格式即契约，调整格式必须同时改版本号 "memwb-target/1"。
    std::string IdentityKey(
        const MemoryTargetSession& session,
        std::uint64_t baseAddress,
        std::uint64_t lengthBytes);

    // SameTarget：判断两个会话是否指向同一个目标。
    // 调用方法：SameTarget(a, b)。传入：两个会话。传出：七个字段全部相等才为 true。
    // 注意：它不校验会话是否自洽，也不比较基址与长度。
    bool SameTarget(const MemoryTargetSession& a, const MemoryTargetSession& b) noexcept;

    // RevisionSnapshot：某一时刻两个计数器的取值，由 SessionRevisions::Capture 产生。
    struct RevisionSnapshot
    {
        // source：捕获时的来源代次。
        std::uint64_t source = 0;
        // content：捕获时的内容代次。
        std::uint64_t content = 0;

        // 两个快照逐字段相等才相等。
        friend bool operator==(const RevisionSnapshot&, const RevisionSnapshot&) = default;
    };

    // SessionRevisions：两个相互独立的计数器。
    // - sourceRevision 只在"换目标/重读"时递增；
    // - contentRevision 只在"编辑/撤销"时递增。
    // 异步回调发起前 Capture，落地前 IsStale：任一计数器变过都算陈旧。
    class SessionRevisions
    {
    public:
        // 构造：两个计数器的初值，默认都是 0。
        // 传入：initialSource/initialContent 用于从已保存的状态恢复。
        explicit SessionRevisions(
            std::uint64_t initialSource = 0,
            std::uint64_t initialContent = 0) noexcept;

        // BumpSource：来源代次加一（换目标或重读时调用），溢出回绕到 0。
        // 不会触碰内容代次。
        void BumpSource() noexcept;

        // BumpContent：内容代次加一（编辑或撤销时调用），溢出回绕到 0。
        // 不会触碰来源代次。
        void BumpContent() noexcept;

        // Source：读取当前来源代次。
        std::uint64_t Source() const noexcept;

        // Content：读取当前内容代次。
        std::uint64_t Content() const noexcept;

        // Capture：捕获当前两个计数器的快照，交给异步任务带走。
        RevisionSnapshot Capture() const noexcept;

        // IsStale：快照是否已陈旧。
        // 传入：captured 之前 Capture 得到的快照。
        // 传出：来源代次或内容代次任一与快照不等即为 true；两者都相等才是 false。
        bool IsStale(const RevisionSnapshot& captured) const noexcept;

    private:
        // sourceRevision_：来源代次，只在换目标/重读时递增。
        std::uint64_t sourceRevision_ = 0;
        // contentRevision_：内容代次，只在编辑/撤销时递增。
        std::uint64_t contentRevision_ = 0;
    };
}
