#include "MemoryModuleDirectory.h"

// 模块目录的实现。设计动机、状态机与匹配规则见 MemoryModuleDirectory.h 顶部注释。
//
// 本文件的结构：先是匿名命名空间里的字符串比较小工具（逐字节 ASCII 折叠比较、
// 路径分量边界判断），然后是所有者比较，最后是目录类本身。
// 全程只用标准库，不依赖区域设置，因此同一份输入在任何机器上结果一致。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ksword::memwb
{
    namespace
    {
        // IsPathSeparator：判断一个字节是否是路径分隔符。
        // 传入：character 待判断的字节。传出：'\\' 与 '/' 返回 true。
        // 两种分隔符等价：用户可能照抄 Unix 风格写法，旧代码也先把 '/' 换成 '\\'。
        bool IsPathSeparator(const char character)
        {
            return character == '\\' || character == '/';
        }

        // FoldCharacter：比较前对单个字节做规范化。
        // 传入：character 原始字节。
        // 传出：ASCII 大写字母换成小写，'/' 换成 '\\'，其余字节（含全部非 ASCII 字节）原样返回。
        // 刻意不用 std::tolower：它依赖区域设置，对高位字节的行为因机器而异。
        // 这里只认 A-Z，非 ASCII 字节逐字节比较，行为由测试钉死。
        char FoldCharacter(const char character)
        {
            if (character >= 'A' && character <= 'Z')
            {
                return static_cast<char>(character - 'A' + 'a');
            }
            if (character == '/')
            {
                return '\\';
            }
            return character;
        }

        // FoldedEqual：两个字符串在"ASCII 大小写折叠 + 分隔符等价"下是否相等。
        // 传入：left、right 待比较的两个字符串。传出：长度相等且逐字节折叠后相等才为 true。
        bool FoldedEqual(const std::string_view left, const std::string_view right)
        {
            // 长度不同必不相等；折叠是逐字节一对一的，不会改变长度。
            if (left.size() != right.size())
            {
                return false;
            }
            // 逐字节比较折叠后的值。
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                if (FoldCharacter(left[index]) != FoldCharacter(right[index]))
                {
                    return false;
                }
            }
            return true;
        }

        // ContainsSeparator：名字里是否含路径分隔符。
        // 传入：text 待检查的文本。传出：含 '\\' 或 '/' 即为 true，决定走"路径匹配"还是"文件名匹配"。
        bool ContainsSeparator(const std::string_view text)
        {
            for (const char character : text)
            {
                if (IsPathSeparator(character))
                {
                    return true;
                }
            }
            return false;
        }

        // LastComponent：取路径最后一个分量（最后一个分隔符之后的部分）。
        // 传入：path 路径或文件名。传出：不含分隔符的尾部视图；没有分隔符时就是整串。
        std::string_view LastComponent(const std::string_view path)
        {
            // 从后往前找最后一个分隔符。
            for (std::size_t index = path.size(); index > 0; --index)
            {
                if (IsPathSeparator(path[index - 1U]))
                {
                    return path.substr(index);
                }
            }
            return path;
        }

        // FileNameOf：取一条记录用于"文件名匹配"的名字。
        // 传入：record 模块记录。传出：name 非空取 name，否则取 fullPath 的最后一个分量。
        std::string_view FileNameOf(const ModuleRecord& record)
        {
            if (!record.name.empty())
            {
                return record.name;
            }
            return LastComponent(record.fullPath);
        }

        // TailMatchesAtBoundary：内核路径的"尾部匹配"。
        // 传入：path 记录里的完整路径；wanted 用户输入的路径（含分隔符）。
        // 传出：wanted 折叠后是 path 的后缀，并且后缀起点落在分量边界上才为 true。
        // 边界的含义：后缀从 path 开头起，或 wanted 自己以分隔符开头，或 path 里紧贴
        // 后缀之前的字节是分隔符。这样 system32\CI.dll 能命中 \SystemRoot\system32\CI.dll，
        // 而 ystem32\CI.dll（半截分量）不能。
        bool TailMatchesAtBoundary(const std::string_view path, const std::string_view wanted)
        {
            // 后缀不可能比整串还长；wanted 为空由调用方提前挡掉。
            if (wanted.size() > path.size())
            {
                return false;
            }
            // 后缀在 path 中的起点。
            const std::size_t start = path.size() - wanted.size();
            if (!FoldedEqual(path.substr(start), wanted))
            {
                return false;
            }
            // 起点在开头：整串相等，天然对齐。
            if (start == 0U)
            {
                return true;
            }
            // wanted 自己以分隔符开头，则后缀必然从分隔符起，对齐。
            if (IsPathSeparator(wanted.front()))
            {
                return true;
            }
            // 否则要求紧贴后缀之前的那个字节是分隔符。
            return IsPathSeparator(path[start - 1U]);
        }
    }

    bool IsSameModuleOwner(const ModuleOwner& a, const ModuleOwner& b) noexcept
    {
        // 地址空间种类与 pid 必须一致：pid 不同是不同进程，scope 不同是不同空间。
        if (a.scope != b.scope || a.pid != b.pid)
        {
            return false;
        }
        // 创建时间只有双方都已知（非 0）时才参与比较：pid 会被系统复用，
        // 两个已知且不同的创建时间说明是同 pid 的不同进程实例。
        if (a.createTime != 0U && b.createTime != 0U)
        {
            return a.createTime == b.createTime;
        }
        return true;
    }

    ModuleOwner ModuleOwnerForSession(const MemoryTargetSession& session) noexcept
    {
        ModuleOwner owner;
        switch (session.scope)
        {
        case Scope::ProcessVirtual:
            // 进程范围：pid 与创建时间一起标识一个进程实例。
            owner.scope = Scope::ProcessVirtual;
            owner.pid = session.pid;
            owner.createTime = session.processCreateTime100ns;
            break;
        case Scope::KernelVirtual:
            // 内核范围：与具体进程无关，pid 与创建时间恒为 0。
            owner.scope = Scope::KernelVirtual;
            break;
        case Scope::Physical:
        default:
            // 物理范围没有模块；越界的枚举值也归到这里，不冒充进程或内核。
            owner.scope = Scope::Physical;
            break;
        }
        return owner;
    }

    void MemoryModuleDirectory::BeginLoad(const ModuleOwner& owner, const std::uint64_t ticket)
    {
        // 登记新的所有者与票据：之前未完成的加载因票据被覆盖而作废。
        owner_ = owner;
        ticket_ = ticket;
        // 旧记录立即清空：它们属于旧所有者（或旧一轮），新所有者绝不能读到。
        records_.clear();
        failureDetail_.clear();
        state_ = State::Loading;
    }

    bool MemoryModuleDirectory::Commit(
        const ModuleOwner& owner,
        const std::uint64_t ticket,
        std::vector<ModuleRecord>&& records)
    {
        // 只有 Loading 状态才接受提交：Ready 之后同一票据的重复提交也在这里被挡掉。
        if (state_ != State::Loading)
        {
            return false;
        }
        // 所有者必须与 BeginLoad 登记的逐字段一致，票据也必须一致；
        // 任何一项不符都整体丢弃，目录保持原样。
        if (!(owner == owner_) || ticket != ticket_)
        {
            return false;
        }
        // 通过全部校验才移走记录。
        records_ = std::move(records);
        failureDetail_.clear();
        state_ = State::Ready;
        return true;
    }

    bool MemoryModuleDirectory::Fail(
        const ModuleOwner& owner,
        const std::uint64_t ticket,
        std::string detail)
    {
        // 规则与 Commit 完全一致：陈旧的失败回调同样不能破坏当前这一轮。
        if (state_ != State::Loading)
        {
            return false;
        }
        if (!(owner == owner_) || ticket != ticket_)
        {
            return false;
        }
        // 失败时记录保持为空（BeginLoad 已清空），只记下失败说明。
        records_.clear();
        failureDetail_ = std::move(detail);
        state_ = State::Failed;
        return true;
    }

    void MemoryModuleDirectory::Clear()
    {
        // 全部回到默认值。
        state_ = State::Empty;
        owner_ = ModuleOwner();
        ticket_ = 0;
        records_.clear();
        failureDetail_.clear();
    }

    MemoryModuleDirectory::Lookup MemoryModuleDirectory::Find(
        const ModuleOwner& wanted,
        const std::string_view name) const
    {
        // 默认结果是 NotReady/base 0：任何提前返回的路径都不会带出半截数据。
        Lookup result;

        // 第一关：没有加载过任何东西，谈不上所有者。
        if (state_ == State::Empty)
        {
            return result;
        }

        // 第二关：所有者核对。放在状态检查之前，是因为"这份目录属于别人"比
        // "还在加载"更重要：调用方据此知道应当为自己的目标重新触发加载。
        if (!IsSameModuleOwner(wanted, owner_))
        {
            result.kind = Lookup::Kind::OwnerMismatch;
            return result;
        }

        // 第三关：属于该所有者但还没有可用记录。
        if (state_ != State::Ready)
        {
            return result;
        }

        // 到这里才真正查找。空名字永远找不到，绝不能"匹配全部"。
        result.kind = Lookup::Kind::NotFound;
        if (name.empty())
        {
            return result;
        }

        // 含分隔符按路径匹配，否则按文件名匹配；内核目录的路径匹配用尾部匹配。
        const bool byPath = ContainsSeparator(name);
        const bool kernelTail = (owner_.scope == Scope::KernelVirtual);
        for (const ModuleRecord& record : records_)
        {
            bool hit = false;
            if (!byPath)
            {
                hit = FoldedEqual(FileNameOf(record), name);
            }
            else if (kernelTail)
            {
                hit = TailMatchesAtBoundary(record.fullPath, name);
            }
            else
            {
                hit = FoldedEqual(record.fullPath, name);
            }
            if (hit)
            {
                result.matches.push_back(&record);
            }
        }

        // 按命中个数定结果：零个 NotFound（已是默认），一个 Found，多个 Ambiguous。
        if (result.matches.size() == 1U)
        {
            result.kind = Lookup::Kind::Found;
            result.base = result.matches.front()->base;
        }
        else if (result.matches.size() > 1U)
        {
            result.kind = Lookup::Kind::Ambiguous;
        }
        return result;
    }

    MemoryModuleDirectory::State MemoryModuleDirectory::GetState() const noexcept
    {
        return state_;
    }

    const ModuleOwner& MemoryModuleDirectory::Owner() const noexcept
    {
        return owner_;
    }

    std::uint64_t MemoryModuleDirectory::Ticket() const noexcept
    {
        return ticket_;
    }

    const std::vector<ModuleRecord>& MemoryModuleDirectory::Records() const noexcept
    {
        return records_;
    }

    const std::string& MemoryModuleDirectory::FailureDetail() const noexcept
    {
        return failureDetail_;
    }
}
