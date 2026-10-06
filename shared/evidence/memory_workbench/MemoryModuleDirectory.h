#pragma once

// ============================================================
// MemoryModuleDirectory.h
// 作用：
// - 内存工作台私有的"模块目录"：保存某一个所有者（进程实例或内核）的模块快照，
//   并按模块名/完整路径查基址。纯 C++20 标准库实现，不依赖 Qt、不依赖 Win32。
//
// 为什么必须带"所有者身份"：
// - 旧代码的模块缓存只有一份，却被"预览进程"和"已附加进程"共用：用户在下拉框里
//   预览进程 B 时缓存被换成 B 的模块，随后在已附加的进程 A 上解析 client.dll+X，
//   用的却是 B 的基址。读取照样成功，只是读到了别的进程——这类错误不会报错。
//   新目录把"这份快照属于谁"写进数据里，查询时必须出示自己要查的所有者，
//   不符就返回 OwnerMismatch，绝不用别人的基址作答。
// - 加载是异步的：枚举在后台线程跑，期间用户可能已经换了目标。BeginLoad 登记
//   "我在为谁加载、票据多少"，Commit/Fail 必须出示同一个所有者与票据，否则整体丢弃，
//   陈旧结果永远写不进目录。
//
// 状态机：
//   Empty   --BeginLoad-->  Loading  --Commit-->  Ready
//                              |  \---Fail----->  Failed
//   任意状态 --BeginLoad--> Loading（重新加载；旧记录立即清空，不会被新所有者读到）
//   任意状态 --Clear-----> Empty
//   Commit/Fail 只在 Loading 状态且所有者、票据都与 BeginLoad 登记的一致时生效；
//   否则返回 false，目录状态与记录一概不变（陈旧回调不能破坏正在进行的那一轮）。
//
// 匹配规则（Find）：
// - 名字含路径分隔符（'\\' 或 '/'，两者等价）按"路径"匹配，否则按"文件名"匹配。
// - 大小写不敏感，**仅限 ASCII**：只有 A-Z 与 a-z 互相等价，非 ASCII 字节原样逐字节
//   比较（UTF-8 的 É 与 é 不相等）。不依赖区域设置，结果在任何机器上一致。
// - 路径匹配：进程目录要求整条路径相等；内核目录用"尾部匹配"，因为内核模块路径是
//   NT 形式（\SystemRoot\system32\CI.dll、\??\C:\...），用户习惯写简称。尾部匹配
//   必须落在路径分量边界上：写 system32\CI.dll 能命中 \SystemRoot\system32\CI.dll，
//   写 ystem32\CI.dll 不能（旧代码是裸 endsWith，会误命中半截分量）。
// - 文件名匹配用记录的 name；name 为空时取 fullPath 最后一个分量。
// - 空名字永远 NotFound，绝不"匹配全部"。
//
// 所有者比较（IsSameModuleOwner，查询时用）：scope 必须相等，pid 必须相等；
// 双方创建时间都已知（非 0）时也必须相等，任一方未知（0，身份未锚定）则只比 pid。
// Commit/Fail 用更严的逐字段相等（operator==）：登记什么就必须原样出示什么。
//
// 线程：本类不加锁，只能在一个线程上使用（工作台的 UI 线程）。后台线程只产出
// 记录向量，回到 UI 线程后再 Commit。Lookup::matches 里的指针指向目录内部的记录，
// 在下一次 BeginLoad/Commit/Fail/Clear 之前有效；需要长期持有请复制记录。
//
// 冻结接口摘要（后续各工作包只依赖这里列出的内容）：
//   struct ModuleOwner{scope,pid,createTime}         模块快照所有者；operator== 逐字段严格相等
//   bool IsSameModuleOwner(a,b)                       查询用的宽松比较（见上）
//   ModuleOwner ModuleOwnerForSession(session)        由会话推出所有者：进程=(Process,pid,创建时间)，
//                                                     内核=(Kernel,0,0)，物理=(Physical,0,0)
//   struct ModuleRecord{name,fullPath,base,size}      一条模块记录
//   enum class State{Empty,Loading,Ready,Failed}      目录状态
//   void BeginLoad(owner, ticket)                     开始为 owner 加载，清空旧记录，状态=Loading
//   bool Commit(owner, ticket, records)               提交记录；所有者/票据/状态不符返回 false 并丢弃
//   bool Fail(owner, ticket, detail)                  提交失败；规则同 Commit，状态=Failed
//   void Clear()                                      回到 Empty
//   Lookup Find(wanted, name) const                   查询，Lookup::Kind 五种：Found/NotFound/Ambiguous/
//                                                     NotReady（Empty/Loading/Failed）/OwnerMismatch
//   GetState()/Owner()/Ticket()/Records()/FailureDetail()  只读访问
//
// 测试：KswordARKLightTests/MemoryModuleDirectoryTests.cpp
// （入口 RunMemwbModuleDirTests，套件名 "MEMWB module dir"）。
// ============================================================

#include "MemoryTargetSession.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ksword::memwb
{
    // ModuleOwner：一份模块快照的所有者。
    // 内核目录的所有者是 (KernelVirtual, 0, 0)；进程目录是 (ProcessVirtual, pid, 创建时间)。
    struct ModuleOwner
    {
        // scope：所有者所在的地址空间种类。物理空间没有模块，不会出现在目录里。
        Scope scope = Scope::ProcessVirtual;
        // pid：进程号；内核所有者必须为 0。
        std::uint32_t pid = 0;
        // createTime：进程创建时间（100ns 单位）；0 表示未知（身份未锚定），内核恒为 0。
        std::uint64_t createTime = 0;

        // 三个字段逐一相等才相等（严格比较，Commit/Fail 用它）。
        friend bool operator==(const ModuleOwner&, const ModuleOwner&) = default;
    };

    // IsSameModuleOwner：查询时的宽松比较，判断 a 与 b 是不是同一个所有者。
    // 传入：两个所有者。传出：scope 与 pid 都相等，且（任一方创建时间为 0，或两者相等）才为 true。
    // 注意：创建时间未知不等于"可以匹配任何进程"，pid 仍必须相等。
    bool IsSameModuleOwner(const ModuleOwner& a, const ModuleOwner& b) noexcept;

    // ModuleOwnerForSession：由会话推出模块目录的所有者。
    // 传入：session 目标会话（不校验自洽性）。
    // 传出：进程范围返回 (ProcessVirtual, pid, processCreateTime100ns)；
    //       内核范围返回 (KernelVirtual, 0, 0)；其余（物理、越界值）返回 (Physical, 0, 0)。
    // 目的：加载方（BeginLoad）与查询方（解析器）用同一个函数推所有者，口径不会分叉。
    ModuleOwner ModuleOwnerForSession(const MemoryTargetSession& session) noexcept;

    // ModuleRecord：一个已加载模块的记录。
    struct ModuleRecord
    {
        // name：模块文件名（不含目录），如 ntdll.dll。为空时按 fullPath 最后一个分量取。
        std::string name;
        // fullPath：完整路径。进程模块是 Win32 路径（C:\Windows\System32\ntdll.dll），
        // 内核模块是 NT 路径（\SystemRoot\system32\CI.dll）。
        std::string fullPath;
        // base：模块加载基址。
        std::uint64_t base = 0;
        // size：模块映像大小（字节）。
        std::uint64_t size = 0;
    };

    // MemoryModuleDirectory：带所有者身份的模块快照与查询。
    class MemoryModuleDirectory
    {
    public:
        // State：目录状态，见文件顶部的状态机。
        enum class State
        {
            Empty = 0,    // 没有加载过任何东西
            Loading,      // 正在为某个所有者加载
            Ready,        // 记录可用
            Failed,       // 最近一次加载失败
        };

        // Lookup：一次查询的结果。
        struct Lookup
        {
            // Kind：查询结果的种类。
            enum class Kind
            {
                Found = 0,       // 唯一命中，base 有效
                NotFound,        // 目录就绪且属于该所有者，但没有这个模块
                Ambiguous,       // 命中多个，matches 列出全部
                NotReady,        // 目录是 Empty/Loading/Failed，没有可用记录
                OwnerMismatch,   // 目录属于另一个所有者，绝不拿它的基址作答
            };

            // kind：结果种类。默认 NotReady：未赋值的结果不会被当成"找到"。
            Kind kind = Kind::NotReady;
            // base：仅 kind==Found 时有效；其余恒为 0。
            std::uint64_t base = 0;
            // matches：命中的记录。Found 时恰好一条，Ambiguous 时全部（按目录内顺序），
            // 其余为空。指针在目录下一次改动之前有效。
            std::vector<const ModuleRecord*> matches;
        };

        // BeginLoad：开始为 owner 加载。
        // 传入：owner 加载对象；ticket 调用方分配的票据（通常单调递增）。
        // 效果：登记所有者与票据，清空旧记录与旧失败详情，状态变为 Loading。
        // 之前未完成的加载因票据被覆盖而作废：它们之后的 Commit/Fail 都会返回 false。
        void BeginLoad(const ModuleOwner& owner, std::uint64_t ticket);

        // Commit：提交加载结果。
        // 传入：owner、ticket 必须与 BeginLoad 登记的完全一致（owner 用 operator==）；
        //       records 枚举得到的全部记录（右值，成功时被移走）。
        // 传出：true 表示已接受，状态变为 Ready；false 表示丢弃——状态不是 Loading、
        //       所有者不符或票据不符，此时目录保持原样，records 也保持原样（未被移走）。
        bool Commit(const ModuleOwner& owner, std::uint64_t ticket, std::vector<ModuleRecord>&& records);

        // Fail：登记加载失败。
        // 传入：owner、ticket 同 Commit；detail 失败说明（只含错误码/细节串，不含面向用户的句子）。
        // 传出：true 表示已接受，状态变为 Failed，旧记录保持为空；false 表示丢弃，目录不变。
        bool Fail(const ModuleOwner& owner, std::uint64_t ticket, std::string detail);

        // Clear：回到 Empty，清空所有者、票据、记录与失败详情。
        void Clear();

        // Find：按名字查模块基址。
        // 传入：wanted 调用方要查的所有者；name 模块名或路径（匹配规则见文件顶部）。
        // 传出：Lookup。检查顺序固定：Empty→NotReady；所有者不符→OwnerMismatch；
        //       Loading/Failed→NotReady；Ready 才真正查找。
        Lookup Find(const ModuleOwner& wanted, std::string_view name) const;

        // GetState：当前状态。
        State GetState() const noexcept;

        // Owner：当前登记的所有者；Empty 时是默认值。
        const ModuleOwner& Owner() const noexcept;

        // Ticket：当前登记的票据；Empty 时为 0。
        std::uint64_t Ticket() const noexcept;

        // Records：当前记录；非 Ready 时为空。
        const std::vector<ModuleRecord>& Records() const noexcept;

        // FailureDetail：最近一次 Fail 的说明；非 Failed 时为空。
        const std::string& FailureDetail() const noexcept;

    private:
        // state_：目录状态。
        State state_ = State::Empty;
        // owner_：当前登记的所有者。
        ModuleOwner owner_;
        // ticket_：当前登记的票据。
        std::uint64_t ticket_ = 0;
        // records_：就绪时的记录。
        std::vector<ModuleRecord> records_;
        // failureDetail_：失败说明。
        std::string failureDetail_;
    };
}
