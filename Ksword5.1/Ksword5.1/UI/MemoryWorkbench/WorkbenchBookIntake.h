#pragma once

// ============================================================
// WorkbenchBookIntake.h
// 作用：
// - 把旧页面（内存搜索结果表）里用户明确选中的一批地址，按统一规则"加入地址簿"：
//   固定上限、按（目标, 绝对地址）去重、种类固定为 Search（临时，不落盘，用户在地址簿里
//   提升为书签才会保存）。
// - 本模块只依赖 AddressBookStore（Qt 非控件对象）与纯逻辑层的 MemoryAddressBook，
//   不依赖任何控件、也不碰共享单例：传入哪个 store 就写哪个，夹具可以用"仅内存"的 store 直接测。
// - 绝不自动灌入：只有调用方在用户点了"加入地址簿"之后才会调用本函数。
// ============================================================

#include "../../../../shared/evidence/memory_workbench/MemoryAddressBook.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ks::ui
{
    class AddressBookStore;
}

namespace ks::ui::workbench_intake
{
    // kAddressBookCap：地址簿条目总数的上限（含所有种类）。与 AddressBookStore::addMany 注释里
    // "设计上限约一万条"、旧搜索结果表一次最多显示 10000 条的口径一致。
    constexpr std::size_t kAddressBookCap = 10000;

    // IntakeResult：一次加入操作的结果，供调用方拼提示文字。
    struct IntakeResult
    {
        // requested：调用方传入的地址个数（含重复）。
        std::size_t requested = 0;
        // added：实际新增的条目数。
        std::size_t added = 0;
        // duplicates：因"同目标同地址已在簿中（或本批内重复）"而跳过的个数。
        std::size_t duplicates = 0;
        // refusedByCap：因地址簿已满（总数达到上限）而被拒绝的个数。
        std::size_t refusedByCap = 0;
    };

    // BuildProcessTargetKey：进程目标在地址簿里的分组键。
    // 传入：pid 进程号；createTime100ns 进程创建时间（100ns，0 表示未取到）。
    // 传出：与工作台视图（MemoryWorkbenchView）按会话生成的键逐字相同，两处必须一致，
    //       否则"加入"的条目在工作台里按目标过滤时不会出现。
    std::string BuildProcessTargetKey(std::uint32_t pid, std::uint64_t createTime100ns);

    // AddAddressesToBook：把 addresses 逐个加入 store。
    // 传入：store 目标地址簿；targetKey 分组键（见 BuildProcessTargetKey）；addresses 绝对地址列表，
    //       顺序即新增顺序；valueType 这批条目的值解释类型（按搜索时的值类型换算）；
    //       cap 条目总数上限（默认 kAddressBookCap，夹具可传小值）。
    // 传出：见 IntakeResult。规则：
    //   - 与同 targetKey 已有条目（解析为绝对地址相同）重复的、本批内重复的都跳过；
    //   - 簿内现有条目数 + 本次新增 > cap 时，只加到恰好满，余下的计入 refusedByCap；
    //   - 一次批量调用只触发一次 store 的整表重建与一次保存（AddressBookStore::addMany）；
    //   - 种类固定 EntryKind::Search。
    IntakeResult AddAddressesToBook(
        AddressBookStore& store,
        const std::string& targetKey,
        const std::vector<std::uint64_t>& addresses,
        ksword::memwb::ValueType valueType,
        std::size_t cap = kAddressBookCap);
}
