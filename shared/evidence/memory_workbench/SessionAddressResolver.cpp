#include "SessionAddressResolver.h"

// 会话地址解析器的实现。设计动机、Issue 含义与判定规则见 SessionAddressResolver.h
// 顶部注释。
//
// 本文件的结构：先是匿名命名空间里的两个格式化小工具，然后是地址空间函数，
// 接着是解析器类（LookupModule、ReadPointer、访问器），最后是 EvaluateForSession。
// 全程只用标准库，不拼面向用户的句子：detail 里只有地址、路径与内部细节串。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ksword::memwb
{
    namespace
    {
        // FormatHexAddress：把 64 位值格式化成 0x 加大写十六进制，不补前导零。
        // 传入：value 待格式化的值。传出：如 0x7FF600000000；0 得到 0x0。
        // 手写而不用 iostream：保证输出与区域设置无关，也不引入流的开销。
        std::string FormatHexAddress(std::uint64_t value)
        {
            // 十六进制数位表。
            static const char kDigits[] = "0123456789ABCDEF";
            // 从低位到高位累积数位，最后再反转成正常顺序。
            std::string reversed;
            if (value == 0U)
            {
                reversed.push_back('0');
            }
            while (value != 0U)
            {
                reversed.push_back(kDigits[value & 0xFU]);
                value >>= 4U;
            }
            std::string text = "0x";
            text.append(reversed.rbegin(), reversed.rend());
            return text;
        }

        // JoinFirstPaths：把二义命中的前几条完整路径用 '\n' 连成一个串。
        // 传入：matches 目录里的全部命中记录。
        // 传出：最多 kMaxAmbiguousDetailPaths 条，保持目录内顺序；某条没有完整路径时
        //       退而写它的文件名，保证每条都不是空串。
        std::string JoinFirstPaths(const std::vector<const ModuleRecord*>& matches)
        {
            std::string joined;
            // 实际列出的条数：不超过上限，也不超过命中总数。
            const std::size_t listed = std::min(matches.size(), kMaxAmbiguousDetailPaths);
            for (std::size_t index = 0; index < listed; ++index)
            {
                if (index > 0U)
                {
                    joined.push_back('\n');
                }
                const ModuleRecord& record = *matches[index];
                joined += record.fullPath.empty() ? record.name : record.fullPath;
            }
            return joined;
        }
    }

    AddrSpace ScopeAddressSpace(const MemoryTargetSession& session) noexcept
    {
        AddrSpace space;
        switch (session.scope)
        {
        case Scope::ProcessVirtual:
            // 进程用户地址空间：32 位目标只有 4 GiB，64 位是 47 位。
            space.first = 0U;
            space.last = (session.addressBits == 32U) ? kProcessSpaceLast32 : kProcessSpaceLast64;
            break;
        case Scope::KernelVirtual:
            // 内核半区从 kKernelSplit 到地址空间末尾；全仓库只认这一份阈值。
            space.first = kKernelSplit;
            space.last = (std::numeric_limits<std::uint64_t>::max)();
            break;
        case Scope::Physical:
            space.first = 0U;
            space.last = kPhysicalSpaceLast;
            break;
        default:
            // scope 取值越界：给一个空区间，任何地址都不在其中，绝不当成某个合法范围。
            space.first = 1U;
            space.last = 0U;
            break;
        }
        return space;
    }

    bool AddrSpaceContains(const AddrSpace& space, const std::uint64_t address) noexcept
    {
        // 空区间（first > last）恒不包含：两个比较一起成立也推不出空区间里有地址。
        return space.first <= space.last && address >= space.first && address <= space.last;
    }

    SessionAddressResolver::SessionAddressResolver(
        const MemoryTargetSession& session,
        const MemoryModuleDirectory* const processDirectory,
        const MemoryModuleDirectory* const kernelDirectory,
        IPointerReader* const reader)
        : session_(session)
        , processDirectory_(processDirectory)
        , kernelDirectory_(kernelDirectory)
        , reader_(reader)
    {
    }

    void SessionAddressResolver::ResetOutcome() noexcept
    {
        // 每次公开调用先清零，失败路径才能只写自己的那一项。
        issue_ = Issue::None;
        detail_.clear();
        matchCount_ = 0;
    }

    ModuleLookup SessionAddressResolver::FailLookup(const Issue issue, std::string detail)
    {
        // "没有可用的模块上下文"统一返回 NeedsProcess，具体成因写进 Issue。
        issue_ = issue;
        detail_ = std::move(detail);
        return ModuleLookup::NeedsProcess;
    }

    bool SessionAddressResolver::FailRead(const Issue issue, std::string detail)
    {
        issue_ = issue;
        detail_ = std::move(detail);
        return false;
    }

    ModuleLookup SessionAddressResolver::LookupModule(
        const std::string& name,
        std::uint64_t& baseOut)
    {
        // 先清输出与成因：任何提前返回的路径都不会带出上一次的残留。
        baseOut = 0U;
        ResetOutcome();

        // 第一关：会话不自洽（还没选目标、pid 规则违反等）。
        if (Validate(session_) != SessionError::None)
        {
            return FailLookup(Issue::NoTarget, std::string());
        }
        // 第二关：物理范围没有模块这个概念，哪怕内核目录里有同名模块也不去查。
        if (session_.scope == Scope::Physical)
        {
            return FailLookup(Issue::ScopeHasNoModules, std::string());
        }

        // 第三关：按范围选目录。进程范围只用进程目录，内核范围只用内核目录，
        // 不做任何跨目录的自动回退。
        const bool kernelScope = (session_.scope == Scope::KernelVirtual);
        const MemoryModuleDirectory* const directory =
            kernelScope ? kernelDirectory_ : processDirectory_;
        if (directory == nullptr)
        {
            return FailLookup(Issue::ModulesNotLoaded, std::string());
        }

        // 以会话推出的所有者去查：目录属于别的进程实例时得到 OwnerMismatch。
        const MemoryModuleDirectory::Lookup lookup =
            directory->Find(ModuleOwnerForSession(session_), name);
        switch (lookup.kind)
        {
        case MemoryModuleDirectory::Lookup::Kind::Found:
            baseOut = lookup.base;
            return ModuleLookup::Found;

        case MemoryModuleDirectory::Lookup::Kind::Ambiguous:
            // 重名不做偏好猜测（WOW64 进程的 ntdll.dll 必然二义）：列出前几条完整路径。
            issue_ = Issue::Ambiguous;
            detail_ = JoinFirstPaths(lookup.matches);
            matchCount_ = lookup.matches.size();
            return ModuleLookup::Ambiguous;

        case MemoryModuleDirectory::Lookup::Kind::OwnerMismatch:
            return FailLookup(Issue::ModulesOwnerMismatch, std::string());

        case MemoryModuleDirectory::Lookup::Kind::NotReady:
            // 区分"正在加载"与"没加载过/加载失败"；失败时把失败说明带出去。
            if (directory->GetState() == MemoryModuleDirectory::State::Loading)
            {
                return FailLookup(Issue::ModulesLoading, std::string());
            }
            return FailLookup(
                Issue::ModulesNotLoaded,
                directory->GetState() == MemoryModuleDirectory::State::Failed
                    ? directory->FailureDetail()
                    : std::string());

        case MemoryModuleDirectory::Lookup::Kind::NotFound:
        default:
            break;
        }

        // 目录就绪且属于当前目标，但没有这个模块。进程范围下再看一眼内核目录，
        // 只为了给出提示，**不**用它的结果作答：命中也仍然返回 NotFound。
        if (!kernelScope && kernelDirectory_ != nullptr)
        {
            MemoryTargetSession kernelSession;
            kernelSession.scope = Scope::KernelVirtual;
            const MemoryModuleDirectory::Lookup kernelLookup =
                kernelDirectory_->Find(ModuleOwnerForSession(kernelSession), name);
            if (kernelLookup.kind == MemoryModuleDirectory::Lookup::Kind::Found)
            {
                issue_ = Issue::FoundInKernelOnly;
                detail_ = FormatHexAddress(kernelLookup.base);
                return ModuleLookup::NotFound;
            }
        }
        issue_ = Issue::NotFound;
        return ModuleLookup::NotFound;
    }

    bool SessionAddressResolver::ReadPointer(
        const std::uint64_t address,
        const std::uint32_t widthBytes,
        std::uint64_t& valueOut)
    {
        // 先清输出与成因。
        valueOut = 0U;
        ResetOutcome();

        // 门控按优先级依次检查，任何一项命中都不会调用读取器。
        // 会话不自洽：没有目标可读。
        if (Validate(session_) != SessionError::None)
        {
            return FailRead(Issue::NoTarget, std::string());
        }
        // 物理范围一律拒绝：输入时反复求值读物理内存不可接受，也没有"指针"的语义。
        if (session_.scope == Scope::Physical)
        {
            return FailRead(Issue::DerefDeniedScope, std::string());
        }
        // DDMA 通道一律拒绝：每次解引用都会改写磁盘暂存扇区。
        if (session_.channel == Channel::Ddma)
        {
            return FailRead(Issue::DerefDeniedDdma, std::string());
        }
        // 没有注入读取器：无法读取，读取器未被调用。
        if (reader_ == nullptr)
        {
            return FailRead(Issue::DerefReadFailed, "no-reader");
        }
        // 宽度必须等于目标指针宽度，不一致就不读：按错宽度读到的值是另一个数。
        const std::uint32_t expectedWidth = session_.addressBits / 8U;
        if (widthBytes != expectedWidth)
        {
            return FailRead(Issue::DerefReadFailed, "width-mismatch");
        }

        // 通过全部门控：恰好调用一次读取器，不回退到别的通道。
        std::uint64_t pointer = 0U;
        std::string failure;
        if (!reader_->ReadPointer(session_, address, expectedWidth, pointer, failure))
        {
            // detail 以失败地址开头；读取器给了说明就用空格接在后面。
            std::string detail = FormatHexAddress(address);
            if (!failure.empty())
            {
                detail.push_back(' ');
                detail += failure;
            }
            return FailRead(Issue::DerefReadFailed, std::move(detail));
        }
        // 32 位宽度下读取器必须零扩展返回；高位非零是违约，拿去当地址只会读到别处。
        if (expectedWidth == 4U && pointer > 0xFFFFFFFFULL)
        {
            return FailRead(Issue::DerefReadFailed, FormatHexAddress(address) + " pointer-too-wide");
        }
        valueOut = pointer;
        return true;
    }

    Issue SessionAddressResolver::LastIssue() const noexcept
    {
        return issue_;
    }

    const std::string& SessionAddressResolver::LastDetail() const noexcept
    {
        return detail_;
    }

    std::size_t SessionAddressResolver::LastMatchCount() const noexcept
    {
        return matchCount_;
    }

    AddressEval EvaluateForSession(
        const std::string_view text,
        const MemoryTargetSession& session,
        const MemoryModuleDirectory* const processDirectory,
        const MemoryModuleDirectory* const kernelDirectory,
        IPointerReader* const reader)
    {
        AddressEval eval;

        // 会话是否自洽决定宽度取值：不自洽时随便取 8，反正解析器会拒绝一切模块与解引用。
        const bool sessionValid = (Validate(session) == SessionError::None);
        const std::uint32_t widthBytes = sessionValid ? (session.addressBits / 8U) : 8U;

        // 求值只在这里跑一次：读取器被调用的次数就是这次求值里实际执行的解引用次数。
        SessionAddressResolver resolver(session, processDirectory, kernelDirectory, reader);
        eval.expr = EvaluateAddressExpr(text, widthBytes, &resolver);

        // 求值失败：成因取自解析器。纯语法/数值错误解析器根本没被调用，Issue 保持 None。
        if (!eval.expr.ok)
        {
            eval.issue = resolver.LastIssue();
            eval.detail = resolver.LastDetail();
            eval.matchCount = resolver.LastMatchCount();
            return eval;
        }

        // 求值成功但没有目标：数字算出来了，却没有任何地址空间可以放它。
        if (!sessionValid)
        {
            eval.issue = Issue::NoTarget;
            return eval;
        }

        // 落在会话范围的地址空间内：可以导航。
        const AddrSpace space = ScopeAddressSpace(session);
        if (AddrSpaceContains(space, eval.expr.value))
        {
            eval.inScopeSpace = true;
            return eval;
        }

        // 落在空间外：进程范围里输入了内核半区地址，建议切到内核范围，**不静默改范围**；
        // 其余情况（非规范空洞、超出 32 位、内核范围里的用户地址、物理地址超 52 位）
        // 一律拒绝，不给建议。
        if (session.scope == Scope::ProcessVirtual && IsKernelVirtualAddress(eval.expr.value))
        {
            eval.suggestScope = Scope::KernelVirtual;
        }
        return eval;
    }
}
