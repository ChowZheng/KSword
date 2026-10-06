#pragma once

// ============================================================
// SessionAddressResolver.h
// 作用：
// - 把"当前会话"接到地址表达式求值器上：按会话的范围选择模块目录、对解引用
//   做门控、把失败成因细分成界面能据以出文案的 Issue。纯 C++20 标准库实现，
//   不依赖 Qt、不依赖 Win32。
// - 提供各范围的地址空间边界 ScopeAddressSpace，以及一个面向界面的一站式入口
//   EvaluateForSession（回车提交时调用一次）。
//
// 它取代旧代码里三处毛病（来源见 target.md 第 1.2/1.3 节）：
// 1) 模块缓存无归属：旧代码用"预览进程"的模块缓存去解析"已附加进程"的表达式。
//    本类只读调用方传入的目录，并以会话推出的所有者去查（ModuleOwnerForSession）；
//    目录属于别的进程时得到 ModulesOwnerMismatch，绝不拿别人的基址作答。
// 2) 自动回退：旧代码在进程里找不到模块时静默去内核模块里找。本类**不回退**：
//    进程目录未命中而内核目录已加载且命中时，返回 NotFound，并把成因记为
//    FoundInKernelOnly（detail 带内核基址），由界面给出"切换到内核范围并跳转"。
// 3) 内核判据不一致：范围判断只用 MemoryTargetSession.h 里那一份 kKernelSplit。
//
// Issue 的含义（界面按它出文案；NeedsProcess 在这里统一表示"没有可用的模块上下文"）：
//   None                  没有解析器层面的问题（成功，或纯语法/数值错误）
//   NoTarget              会话不自洽（没选目标）：Validate 不为 None
//   ScopeHasNoModules     物理范围没有模块
//   ModulesNotLoaded      目录没加载过，或最近一次加载失败（Failed 时 detail 是失败说明）
//   ModulesLoading        目录正在为当前所有者加载
//   ModulesOwnerMismatch  目录属于另一个进程实例
//   NotFound              目录就绪，但没有这个模块
//   FoundInKernelOnly     进程范围里没有，内核目录里有唯一命中；detail=内核基址（0x 大写十六进制）
//   Ambiguous             重名；detail=前 5 条完整路径，以 '\n' 分隔；总数见 matchCount
//   DerefDeniedScope      物理范围拒绝解引用
//   DerefDeniedDdma       磁盘传输（DDMA）通道拒绝解引用：每次解引用都会改写暂存扇区
//   DerefReadFailed       读取指针失败；detail=失败地址（0x 十六进制），读取器给了说明时
//                         追加 ' ' + 说明；另有两个内部细节串 no-reader（没注入读取器）与
//                         width-mismatch（调用宽度不等于 addressBits/8），读取器未被调用
//   Issue 只在对应的求值失败时出现；detail 里没有任何面向用户的句子。
//
// 解引用门控（ReadPointer，检查顺序即优先级，先命中先返回，且都不会调用读取器）：
//   会话不自洽 → NoTarget；物理范围 → DerefDeniedScope；DDMA 通道 → DerefDeniedDdma；
//   没有读取器 → DerefReadFailed；宽度不等于 addressBits/8 → DerefReadFailed。
//   通过后读取器恰好被调用一次；不回退到别的通道，通道对不对由读取器如实报失败。
//   32 位宽度下读取器返回高位非零的值视为违约，同样判 DerefReadFailed。
//
// 范围地址空间（ScopeAddressSpace，闭区间）：
//   进程：[0, 0x7FFFFFFFFFFF]；addressBits==32 的目标 [0, 0xFFFFFFFF]
//   内核：[kKernelSplit, UINT64_MAX]
//   物理：[0, 0xFFFFFFFFFFFFF]（52 位）
//   不自洽的 scope 取值得到空区间（first > last），任何地址都不在其中。
//
// EvaluateForSession 的判定（求值只跑一次）：
//   1. 用会话建解析器，宽度取 addressBits/8（会话不自洽时取 8，且解析器会拒绝一切模块与解引用），
//      调用一次 EvaluateAddressExpr。
//   2. 求值失败：issue/detail/matchCount 取自解析器；inScopeSpace=false，suggestScope 为空。
//   3. 求值成功但会话不自洽：issue=NoTarget，inScopeSpace=false。
//   4. 成功且落在会话范围的地址空间内：inScopeSpace=true。
//   5. 成功但落在空间外：inScopeSpace=false；若 scope=进程 且地址在内核半区，
//      suggestScope=Kernel（"需要切换范围"，**不静默改范围**）；其余（非规范空洞、
//      超出 32 位、内核范围里的用户地址、物理地址超 52 位）一律拒绝，suggestScope 为空。
//   Accepted() 是"可以直接导航"的唯一判据：expr.ok && inScopeSpace。
//
// 冻结接口摘要（后续各工作包只依赖这里列出的内容）：
//   enum class Issue{None,NoTarget,ScopeHasNoModules,ModulesNotLoaded,ModulesLoading,
//                    ModulesOwnerMismatch,NotFound,FoundInKernelOnly,Ambiguous,
//                    DerefDeniedScope,DerefDeniedDdma,DerefReadFailed}
//   class IPointerReader::ReadPointer(session, address, widthBytes, valueOut, failureOut)->bool
//                                                    注入的指针读取端口（UI 线程调用，不得回退通道）
//   struct AddrSpace{first,last}                     闭区间
//   AddrSpace ScopeAddressSpace(session)             各范围的地址空间（见上）
//   bool AddrSpaceContains(space, address)           地址是否在区间内（空区间恒 false）
//   class SessionAddressResolver : IAddressExprResolver
//       ctor(session, processDirectory, kernelDirectory, reader)   目录与读取器按指针传入，可为空
//       LookupModule / ReadPointer                   见上
//       LastIssue()/LastDetail()/LastMatchCount()    最近一次失败的成因（每次调用先清零）
//   struct AddressEval{expr,issue,detail,matchCount,inScopeSpace,suggestScope; Accepted()}
//   AddressEval EvaluateForSession(text, session, processDirectory, kernelDirectory, reader)
//
// 测试：KswordARKLightTests/SessionAddressResolverTests.cpp
// （入口 RunMemwbSessionResolverTests，套件名 "MEMWB session resolver"）+
// ModuleResolverTestSupport.h（假读取器与标准目录夹具）。
// ============================================================

#include "MemoryAddressExpr.h"
#include "MemoryModuleDirectory.h"
#include "MemoryTargetSession.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ksword::memwb
{
    // kProcessSpaceLast64：64 位进程用户地址空间的最后一个地址（47 位）。
    inline constexpr std::uint64_t kProcessSpaceLast64 = 0x00007FFFFFFFFFFFULL;
    // kProcessSpaceLast32：32 位目标的最后一个地址。
    inline constexpr std::uint64_t kProcessSpaceLast32 = 0x00000000FFFFFFFFULL;
    // kPhysicalSpaceLast：物理地址空间的最后一个地址（52 位物理地址宽度上限）。
    inline constexpr std::uint64_t kPhysicalSpaceLast = 0x000FFFFFFFFFFFFFULL;
    // kMaxAmbiguousDetailPaths：Ambiguous 的 detail 最多列出的路径条数。
    inline constexpr std::size_t kMaxAmbiguousDetailPaths = 5;

    // Issue：求值失败的细分成因，见文件顶部的含义表。
    enum class Issue
    {
        None = 0,
        NoTarget,
        ScopeHasNoModules,
        ModulesNotLoaded,
        ModulesLoading,
        ModulesOwnerMismatch,
        NotFound,
        FoundInKernelOnly,
        Ambiguous,
        DerefDeniedScope,
        DerefDeniedDdma,
        DerefReadFailed,
    };

    // IPointerReader：解析器读指针用的外部端口，由调用方注入。
    // 与 IAddressExprResolver::ReadPointer 的区别是带上完整会话：真实实现要按会话的
    // 范围/通道/进程去读；本模块的门控已经保证了不会在物理范围或 DDMA 通道上调用它。
    class IPointerReader
    {
    public:
        // 虚析构：允许经基类指针销毁实现。
        virtual ~IPointerReader() = default;

        // ReadPointer：在会话描述的目标地址空间里读一个指针。
        // 传入：session 当前会话；address 读取地址；widthBytes 指针宽度（4 或 8，恒等于
        //       session.addressBits/8）。
        // 传出：valueOut 零扩展后的指针值，失败时请置 0；failureOut 失败时的简短说明
        //       （错误码/细节串，可为空，不要写面向用户的句子）。返回 false 表示读取失败。
        virtual bool ReadPointer(
            const MemoryTargetSession& session,
            std::uint64_t address,
            std::uint32_t widthBytes,
            std::uint64_t& valueOut,
            std::string& failureOut) = 0;
    };

    // AddrSpace：一个地址空间的闭区间 [first, last]。first > last 表示空区间。
    struct AddrSpace
    {
        // first：区间内最小的地址。
        std::uint64_t first = 0;
        // last：区间内最大的地址。
        std::uint64_t last = 0;
    };

    // ScopeAddressSpace：会话所在范围的地址空间。
    // 传入：session 会话（不校验自洽性，只看 scope 与 addressBits）。
    // 传出：进程 [0,0x7FFFFFFFFFFF]（addressBits==32 时 [0,0xFFFFFFFF]）；
    //       内核 [kKernelSplit,UINT64_MAX]；物理 [0,0xFFFFFFFFFFFFF]；
    //       scope 越界时返回空区间 {1, 0}。
    AddrSpace ScopeAddressSpace(const MemoryTargetSession& session) noexcept;

    // AddrSpaceContains：地址是否落在区间内（闭区间）。
    // 传入：space 区间；address 待判断地址。传出：first<=address<=last 为 true；空区间恒为 false。
    bool AddrSpaceContains(const AddrSpace& space, std::uint64_t address) noexcept;

    // SessionAddressResolver：把会话接到 EvaluateAddressExpr 上的解析器。
    // 对象只在一次求值期间存活，持有会话的一份拷贝；目录与读取器只借用指针，调用方保证
    // 求值期间它们有效。每次 LookupModule/ReadPointer 调用先把 Last* 清零，
    // 求值在第一次失败处停止，所以失败后 Last* 就是那次失败的成因。
    class SessionAddressResolver final : public IAddressExprResolver
    {
    public:
        // 构造。
        // 传入：session 会话（拷贝）；processDirectory 进程模块目录；kernelDirectory 内核
        //       模块目录；reader 指针读取器。后三者可为 nullptr：空目录等同于"没加载"，
        //       空读取器使解引用判 DerefReadFailed。
        SessionAddressResolver(
            const MemoryTargetSession& session,
            const MemoryModuleDirectory* processDirectory,
            const MemoryModuleDirectory* kernelDirectory,
            IPointerReader* reader);

        // LookupModule：按名字查模块基址（实现 IAddressExprResolver）。
        // 映射：会话不自洽/物理范围/目录没就绪/所有者不符 → NeedsProcess；
        //       Ready 但缺 → NotFound；多于一个 → Ambiguous；唯一命中 → Found。
        // 传出：baseOut 仅 Found 时有意义，其余恒为 0；成因写进 LastIssue()。
        ModuleLookup LookupModule(const std::string& name, std::uint64_t& baseOut) override;

        // ReadPointer：解引用门控 + 读取（实现 IAddressExprResolver）。
        // 传入：address 读取地址；widthBytes 必须等于 addressBits/8。
        // 传出：valueOut 成功时是指针值，失败恒为 0；返回 false 的成因写进 LastIssue()。
        bool ReadPointer(
            std::uint64_t address,
            std::uint32_t widthBytes,
            std::uint64_t& valueOut) override;

        // LastIssue：最近一次 LookupModule/ReadPointer 调用的成因；成功为 None。
        Issue LastIssue() const noexcept;

        // LastDetail：最近一次调用的细节串（含义见文件顶部的 Issue 表）。
        const std::string& LastDetail() const noexcept;

        // LastMatchCount：最近一次调用若是 Ambiguous，命中的总条数；否则为 0。
        std::size_t LastMatchCount() const noexcept;

    private:
        // ResetOutcome：把 Last* 三项清零。每次公开调用开头执行。
        void ResetOutcome() noexcept;

        // FailLookup：记录成因并返回 NeedsProcess（"没有可用模块上下文"的统一返回）。
        ModuleLookup FailLookup(Issue issue, std::string detail);

        // FailRead：记录成因并返回 false。
        bool FailRead(Issue issue, std::string detail);

        // session_：会话拷贝。
        MemoryTargetSession session_;
        // processDirectory_：进程模块目录（借用）。
        const MemoryModuleDirectory* processDirectory_ = nullptr;
        // kernelDirectory_：内核模块目录（借用）。
        const MemoryModuleDirectory* kernelDirectory_ = nullptr;
        // reader_：指针读取器（借用）。
        IPointerReader* reader_ = nullptr;
        // issue_：最近一次调用的成因。
        Issue issue_ = Issue::None;
        // detail_：最近一次调用的细节串。
        std::string detail_;
        // matchCount_：最近一次 Ambiguous 的命中总数。
        std::size_t matchCount_ = 0;
    };

    // AddressEval：EvaluateForSession 的完整结果。
    struct AddressEval
    {
        // expr：求值结果。成功时 value 是地址；注意成功不等于可以导航，要看 Accepted()。
        ExprResult expr;
        // issue：解析器层面的细分成因；None 表示没有（成功，或纯语法/数值错误）。
        Issue issue = Issue::None;
        // detail：细节串，含义见 SessionAddressResolver.h 顶部的 Issue 表；无则为空。
        std::string detail;
        // matchCount：issue==Ambiguous 时的命中总条数，否则为 0。
        std::size_t matchCount = 0;
        // inScopeSpace：求值成功且地址落在会话范围的地址空间内。
        bool inScopeSpace = false;
        // suggestScope：求值成功但需要先切换范围才能到达该地址时的建议范围；
        // 目前只有"进程范围里输入了内核半区地址"会给出 Kernel。有值时一定有 expr.ok。
        std::optional<Scope> suggestScope;

        // Accepted：是否可以直接导航到 expr.value。
        // 传出：expr.ok && inScopeSpace。调用方不得只看 expr.ok。
        bool Accepted() const noexcept
        {
            return expr.ok && inScopeSpace;
        }
    };

    // EvaluateForSession：面向界面的一站式求值（回车提交时调用，绝不逐键调用）。
    // 传入：text 表达式文本（UTF-8）；session 当前会话；processDirectory/kernelDirectory
    //       两个模块目录（可为空）；reader 指针读取器（可为空）。
    // 传出：AddressEval，判定规则见文件顶部。EvaluateAddressExpr 在这里只被调用一次，
    //       因此读取器被调用的次数恰好等于一次求值里实际执行的解引用次数。
    AddressEval EvaluateForSession(
        std::string_view text,
        const MemoryTargetSession& session,
        const MemoryModuleDirectory* processDirectory,
        const MemoryModuleDirectory* kernelDirectory,
        IPointerReader* reader);
}
