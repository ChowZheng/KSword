// ============================================================
// WorkbenchBookIntake.cpp
// 作用：实现 WorkbenchBookIntake.h——把一批绝对地址按上限/去重规则批量加入地址簿。
// ============================================================

#include "WorkbenchBookIntake.h"

#include "AddressBookStore.h"
#include "MemoryWorkbenchView.Internal.h"

#include <unordered_set>

namespace ks::ui::workbench_intake
{
    // BuildProcessTargetKey：借工作台视图用的同一个函数生成键，保证两处格式逐字一致。
    std::string BuildProcessTargetKey(const std::uint32_t pid, const std::uint64_t createTime100ns)
    {
        ksword::memwb::MemoryTargetSession session;
        session.scope = ksword::memwb::Scope::ProcessVirtual;
        session.pid = pid;
        session.processCreateTime100ns = createTime100ns;
        return detail::BuildAddressBookTargetKey(session);
    }

    // AddAddressesToBook：见头文件。
    IntakeResult AddAddressesToBook(
        AddressBookStore& store,
        const std::string& targetKey,
        const std::vector<std::uint64_t>& addresses,
        const ksword::memwb::ValueType valueType,
        const std::size_t cap)
    {
        IntakeResult result;
        result.requested = addresses.size();

        // known：同目标已有条目的绝对地址集合，用于 O(1) 去重（逐个比较在一万条规模下是 O(n²)）。
        // 带模块名的条目没有固定的绝对地址，解析不出来，不参与去重。
        std::unordered_set<std::uint64_t> known;
        ksword::memwb::AddressFilter filter;
        filter.targetKey = targetKey;
        const auto noModuleLookup = [](const std::string&, std::uint64_t&) { return false; };
        for (const ksword::memwb::AddressEntry& existing : store.list(filter))
        {
            const auto resolved = ksword::memwb::MemoryAddressBook::ResolveAddress(existing, noModuleLookup);
            if (resolved.ok())
            {
                known.insert(resolved.address);
            }
        }

        // room：地址簿还能再容纳多少条（总数含所有种类、所有目标）。
        const std::size_t current = store.size();
        std::size_t room = (current >= cap) ? 0U : (cap - current);

        std::vector<ksword::memwb::AddressEntry> drafts;
        drafts.reserve(addresses.size() < room ? addresses.size() : room);
        for (const std::uint64_t address : addresses)
        {
            if (known.count(address) != 0U)
            {
                ++result.duplicates;
                continue;
            }
            if (room == 0U)
            {
                ++result.refusedByCap;
                continue;
            }
            auto draft = ksword::memwb::MemoryAddressBook::FromAbsolute(
                targetKey, ksword::memwb::EntryKind::Search, address);
            if (!draft.has_value())
            {
                // FromAbsolute 对绝对地址不会失败（只有 kind 越界才拒绝）；防御性按重复口径计，
                // 不静默吞掉。
                ++result.duplicates;
                continue;
            }
            draft->valueType = valueType;
            drafts.push_back(*draft);
            known.insert(address);
            --room;
        }

        if (!drafts.empty())
        {
            result.added = store.addMany(drafts).size();
        }
        return result;
    }
}
