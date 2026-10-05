// 统一地址簿（shared/evidence/memory_workbench/MemoryAddressBook.h）的离线测试。
//
// 为什么这个模块值得穷举：它属于"算错了不会报错"的那一类。
//   * id 一旦被复用，持有旧 id 的界面回调就会"找到别人"——点中的是另一个地址；
//   * 模块条目一旦拿旧绝对地址冒充，换进程之后书签会指向一个毫不相干的位置；
//   * 持久化若在失败时污染了内存里的簿，用户的书签会被半个损坏的文件悄悄覆盖。
//
// 断言原则与 NumericTextParseTests.cpp 一致：
//   * 期望值独立手算写死（含序列化出来的完整文本），绝不从被测函数反算；
//   * 边界两侧都测（地址加法恰好到 UINT64_MAX / 恰好溢出、id 上限两侧、16 位与 17 位十六进制）；
//   * 该被拒绝的输入必须被显式拒绝，并且同时断言出错行号、错误码、以及传入的簿保持原样；
//   * 拒绝用例里，坏行之前一律先放两条合法行——"边解析边写入"的实现会在这里露馅。

//
// 序列化 / 反序列化的用例在 MemoryAddressBookTests.Serialize.cpp（单文件不超过 800 行），
// 由本文件末尾的入口并入同一个套件。

#include "MemoryAddressBookTests.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using MemwbAddressBookTestSupport::Draft;
using MemwbAddressBookTestSupport::IdsOf;
using MemwbAddressBookTestSupport::kU64Max;
using ksword::memwb::AddressEntry;
using ksword::memwb::AddressFilter;
using ksword::memwb::EntryKind;
using ksword::memwb::MemoryAddressBook;
using ksword::memwb::ModuleBaseLookup;
using ksword::memwb::ModuleLocation;
using ksword::memwb::ResolveStatus;
using ksword::memwb::ValueType;

using Ids = std::vector<std::uint64_t>;

// ------------------------------------------------------------
// 一、id：单调、永不复用、一切按 id。
// ------------------------------------------------------------
void TestIdsMonotonicAndNeverReused(KswordTests::Suite& suite) {
    MemoryAddressBook book;
    suite.expect(book.NextId() == 1ULL, L"address book: a new book hands out id 1 first");

    // 前三条依次拿到 1、2、3。
    const std::uint64_t first = book.Add(Draft(EntryKind::Search, "a.exe", "", 0, 0x1000, ""));
    const std::uint64_t second = book.Add(Draft(EntryKind::Search, "a.exe", "", 0, 0x2000, ""));
    const std::uint64_t third = book.Add(Draft(EntryKind::Bookmark, "a.exe", "", 0, 0x3000, ""));
    suite.expect(first == 1ULL && second == 2ULL && third == 3ULL,
        L"address book: ids are handed out 1, 2, 3 in order");

    // 删中间的：新条目拿 4，不能去填 2 的空。
    suite.expect(book.Remove(2), L"address book: removing an existing id succeeds");
    suite.expect(!book.Find(2).has_value(), L"address book: a removed id can no longer be found");
    const std::uint64_t fourth = book.Add(Draft(EntryKind::Search, "a.exe", "", 0, 0x4000, ""));
    suite.expect(fourth == 4ULL, L"address book: a removed id is not reused by the next Add");

    // 关键：删掉当前最大的 id 之后，再添加也不能拿回它。
    suite.expect(book.Remove(4), L"address book: removing the highest id succeeds");
    const std::uint64_t fifth = book.Add(Draft(EntryKind::Search, "a.exe", "", 0, 0x5000, ""));
    suite.expect(fifth == 5ULL, L"address book: removing the highest id does not let it be reused");
    suite.expect(book.NextId() == 6ULL, L"address book: NextId is one past the last handed-out id");

    // 草稿里的 id 字段被忽略。
    AddressEntry withId = Draft(EntryKind::Search, "a.exe", "", 0, 0x6000, "");
    withId.id = 77;
    const std::uint64_t sixth = book.Add(withId);
    suite.expect(sixth == 6ULL, L"address book: the id in a draft is ignored");
    suite.expect(!book.Find(77).has_value() && book.Find(6).has_value(),
        L"address book: the entry is stored under the assigned id, not the draft id");

    // 无效 id 的各种删除/查找都安静地失败。
    suite.expect(!book.Remove(2) && !book.Remove(999) && !book.Remove(0),
        L"address book: removing an unknown id reports failure");
    suite.expect(!book.Find(0).has_value() && !book.Find(999).has_value(),
        L"address book: finding an unknown id returns nothing");
    suite.expect(book.Size() == 4U, L"address book: size counts the remaining entries (1, 3, 5, 6)");

    // 清空整个簿也不能让 id 回到 1。
    MemoryAddressBook emptied;
    for (int count = 0; count < 100; ++count) {
        emptied.Add(Draft(EntryKind::Search, "t", "", 0, 0x10, ""));
    }
    for (std::uint64_t id = 1; id <= 100; ++id) {
        emptied.Remove(id);
    }
    suite.expect(emptied.Size() == 0U, L"address book: all entries removed");
    suite.expect(emptied.Add(Draft(EntryKind::Search, "t", "", 0, 0x10, "")) == 101ULL,
        L"address book: an emptied book keeps counting instead of restarting at 1");
}

// ------------------------------------------------------------
// 二、Add 的拒绝与地址字段归一化。
// ------------------------------------------------------------
void TestAddRejectsAndNormalizes(KswordTests::Suite& suite) {
    MemoryAddressBook book;

    // 越界枚举（调用方 static_cast 出来的垃圾）必须被拒绝，且 nextId 不被消耗。
    const std::uint64_t badKind = book.Add(
        Draft(static_cast<EntryKind>(7), "t", "", 0, 0x10, ""));
    const std::uint64_t negativeKind = book.Add(
        Draft(static_cast<EntryKind>(-1), "t", "", 0, 0x10, ""));
    const std::uint64_t badType = book.Add(
        Draft(EntryKind::Search, "t", "", 0, 0x10, "", static_cast<ValueType>(99)));
    suite.expect(badKind == 0ULL && negativeKind == 0ULL && badType == 0ULL,
        L"address book: Add rejects out-of-range kind or value type with id 0");
    suite.expect(book.Size() == 0U && book.NextId() == 1ULL,
        L"address book: a rejected Add leaves the book and the id counter untouched");

    // 有模块名：绝对地址被清零（结构上就不存在可冒充的旧地址），rva 保留。
    const std::uint64_t moduleId = book.Add(
        Draft(EntryKind::Bookmark, "t", "game.dll", 0x1234, 0xDEADBEEF, ""));
    const auto moduleEntry = book.Find(moduleId);
    suite.expect(moduleEntry.has_value() && moduleEntry->rva == 0x1234ULL,
        L"address book: a module entry keeps its rva");
    suite.expect(moduleEntry.has_value() && moduleEntry->absoluteAddress == 0ULL,
        L"address book: a module entry drops the stale absolute address");

    // 无模块名：rva 被清零，绝对地址保留。
    const std::uint64_t plainId = book.Add(
        Draft(EntryKind::Bookmark, "t", "", 0x55, 0xCAFE0000, ""));
    const auto plainEntry = book.Find(plainId);
    suite.expect(plainEntry.has_value() && plainEntry->absoluteAddress == 0xCAFE0000ULL,
        L"address book: a module-less entry keeps its absolute address");
    suite.expect(plainEntry.has_value() && plainEntry->rva == 0ULL,
        L"address book: a module-less entry drops the meaningless rva");

    // 默认值：valueType 默认 Hex8，沿用旧书签的显示。
    const AddressEntry defaults;
    suite.expect(defaults.valueType == ValueType::Hex8 && defaults.id == 0ULL,
        L"address book: a default entry shows 8-byte hex and has no id");
}

// ------------------------------------------------------------
// 三、查找与编辑：备注可编辑、值类型可改。
// ------------------------------------------------------------
void TestFindAndEdit(KswordTests::Suite& suite) {
    MemoryAddressBook book;
    const std::uint64_t id = book.Add(
        Draft(EntryKind::Bookmark, "a.exe", "m.dll", 0x40, 0, "first", ValueType::U32));
    const std::uint64_t other = book.Add(Draft(EntryKind::Search, "b.exe", "", 0, 0x9000, "keep"));

    // 改备注：成功后只有 note 变，其它字段原样。
    suite.expect(book.SetNote(id, "second"), L"address book: SetNote succeeds on a known id");
    auto edited = book.Find(id);
    suite.expect(edited.has_value() && edited->note == "second",
        L"address book: the new note is visible through Find");
    suite.expect(edited.has_value() && edited->kind == EntryKind::Bookmark
            && edited->targetKey == "a.exe" && edited->moduleName == "m.dll"
            && edited->rva == 0x40ULL && edited->valueType == ValueType::U32,
        L"address book: editing the note leaves every other field alone");

    // 备注可清空。
    suite.expect(book.SetNote(id, ""), L"address book: SetNote accepts an empty note");
    edited = book.Find(id);
    suite.expect(edited.has_value() && edited->note.empty(), L"address book: an empty note is stored");

    // 改值类型：成功后只有 valueType 变。
    suite.expect(book.SetValueType(id, ValueType::F64),
        L"address book: SetValueType succeeds on a known id");
    edited = book.Find(id);
    suite.expect(edited.has_value() && edited->valueType == ValueType::F64 && edited->rva == 0x40ULL,
        L"address book: the value type changes and the address does not");

    // 失败路径：未知 id、越界类型；条目都不能被改动。
    suite.expect(!book.SetNote(12345, "x") && !book.SetValueType(12345, ValueType::U8),
        L"address book: editing an unknown id reports failure");
    suite.expect(!book.SetValueType(id, static_cast<ValueType>(42)),
        L"address book: an out-of-range value type is refused");
    edited = book.Find(id);
    suite.expect(edited.has_value() && edited->valueType == ValueType::F64,
        L"address book: a refused value type leaves the entry unchanged");

    // 另一个条目完全不受影响。
    const auto untouched = book.Find(other);
    suite.expect(untouched.has_value() && untouched->note == "keep"
            && untouched->valueType == ValueType::Hex8,
        L"address book: editing one entry does not touch another");
}

// ------------------------------------------------------------
// 四、Promote：Search 可升级，已保存的条目不能降回 Search。
// ------------------------------------------------------------
void TestPromote(KswordTests::Suite& suite) {
    MemoryAddressBook book;
    const std::uint64_t a = book.Add(Draft(EntryKind::Search, "t", "m.dll", 0x10, 0, "na", ValueType::I16));
    const std::uint64_t b = book.Add(Draft(EntryKind::Search, "t", "", 0, 0x2000, "nb"));
    const std::uint64_t c = book.Add(Draft(EntryKind::Search, "t", "", 0, 0x3000, "nc"));

    // Search -> Bookmark：kind 变，其它字段与位置不变。
    suite.expect(book.Promote(b, EntryKind::Bookmark), L"address book: Search promotes to Bookmark");
    const auto promoted = book.Find(b);
    suite.expect(promoted.has_value() && promoted->kind == EntryKind::Bookmark
            && promoted->absoluteAddress == 0x2000ULL && promoted->note == "nb"
            && promoted->id == b,
        L"address book: promoting only changes the kind");
    suite.expect(IdsOf(book.List()) == Ids({ a, b, c }),
        L"address book: promoting does not move the entry in insertion order");

    // Bookmark <-> Watch 两个方向都允许。
    suite.expect(book.Promote(b, EntryKind::Watch) && book.Find(b)->kind == EntryKind::Watch,
        L"address book: Bookmark promotes to Watch");
    suite.expect(book.Promote(b, EntryKind::Bookmark) && book.Find(b)->kind == EntryKind::Bookmark,
        L"address book: Watch can step back to Bookmark");

    // Search -> Watch 直达。
    suite.expect(book.Promote(c, EntryKind::Watch) && book.Find(c)->kind == EntryKind::Watch,
        L"address book: Search promotes straight to Watch");

    // 同 kind 幂等成功。
    suite.expect(book.Promote(a, EntryKind::Search) && book.Find(a)->kind == EntryKind::Search,
        L"address book: promoting to the same kind is an idempotent success");

    // 禁止降回 Search：两种已保存的 kind 都要测，并断言条目没被改。
    suite.expect(!book.Promote(b, EntryKind::Search) && book.Find(b)->kind == EntryKind::Bookmark,
        L"address book: a Bookmark cannot be demoted to Search");
    suite.expect(!book.Promote(c, EntryKind::Search) && book.Find(c)->kind == EntryKind::Watch,
        L"address book: a Watch cannot be demoted to Search");

    // 其它失败路径。
    suite.expect(!book.Promote(9999, EntryKind::Bookmark),
        L"address book: promoting an unknown id reports failure");
    suite.expect(!book.Promote(a, static_cast<EntryKind>(5)) && book.Find(a)->kind == EntryKind::Search,
        L"address book: promoting to an out-of-range kind is refused");
}

// ------------------------------------------------------------
// 五、List：按 kind / target 过滤，插入顺序，返回副本。
// ------------------------------------------------------------
void TestListFilters(KswordTests::Suite& suite) {
    MemoryAddressBook book;
    const std::uint64_t i1 = book.Add(Draft(EntryKind::Search, "a.exe", "", 0, 0x10, ""));
    const std::uint64_t i2 = book.Add(Draft(EntryKind::Bookmark, "b.exe", "", 0, 0x20, ""));
    const std::uint64_t i3 = book.Add(Draft(EntryKind::Watch, "a.exe", "", 0, 0x30, ""));
    const std::uint64_t i4 = book.Add(Draft(EntryKind::Bookmark, "a.exe", "", 0, 0x40, ""));
    const std::uint64_t i5 = book.Add(Draft(EntryKind::Search, "", "", 0, 0x50, ""));

    // 默认过滤器：全部，按插入顺序。
    suite.expect(IdsOf(book.List()) == Ids({ i1, i2, i3, i4, i5 }),
        L"address book: an empty filter lists everything in insertion order");

    // 单个 kind。
    AddressFilter onlySearch;
    onlySearch.kinds = { EntryKind::Search };
    suite.expect(IdsOf(book.List(onlySearch)) == Ids({ i1, i5 }),
        L"address book: filtering by one kind keeps only that kind");

    // 多个 kind 取并集，顺序仍是插入顺序（不是 kind 顺序）。
    AddressFilter savedKinds;
    savedKinds.kinds = { EntryKind::Watch, EntryKind::Bookmark };
    suite.expect(IdsOf(book.List(savedKinds)) == Ids({ i2, i3, i4 }),
        L"address book: a kind set is a union and keeps insertion order");

    // 按目标。
    AddressFilter onlyA;
    onlyA.targetKey = std::string("a.exe");
    suite.expect(IdsOf(book.List(onlyA)) == Ids({ i1, i3, i4 }),
        L"address book: filtering by target keeps only that target");

    // kind 与 target 同时给出取交集。
    AddressFilter bookmarkOfA;
    bookmarkOfA.kinds = { EntryKind::Bookmark };
    bookmarkOfA.targetKey = std::string("a.exe");
    suite.expect(IdsOf(book.List(bookmarkOfA)) == Ids({ i4 }),
        L"address book: kind and target filters intersect");

    // 空串是合法的目标标识：只匹配目标为空的条目，与"未设置过滤"不同。
    AddressFilter emptyTarget;
    emptyTarget.targetKey = std::string();
    suite.expect(IdsOf(book.List(emptyTarget)) == Ids({ i5 }),
        L"address book: an empty-string target filter matches only empty-target entries");

    // 没有命中的过滤返回空，而不是全部。
    AddressFilter nobody;
    nobody.targetKey = std::string("nope.exe");
    suite.expect(book.List(nobody).empty(), L"address book: a filter with no match lists nothing");

    // 返回的是副本：改副本不影响簿。
    std::vector<AddressEntry> copy = book.List();
    copy[0].note = "tampered";
    copy.pop_back();
    suite.expect(book.Find(i1)->note.empty() && book.Size() == 5U,
        L"address book: List returns copies, so editing them does not change the book");

    // 删除与升级之后，顺序仍然是原插入顺序。
    book.Remove(i2);
    book.Promote(i1, EntryKind::Watch);
    suite.expect(IdsOf(book.List()) == Ids({ i1, i3, i4, i5 }),
        L"address book: listing order survives removal and promotion");
}

// ------------------------------------------------------------
// 六、ResolveAddress：模块未加载、溢出、不拿旧绝对地址冒充。
// ------------------------------------------------------------
void TestResolveAddress(KswordTests::Suite& suite) {
    // 无模块条目：直接取绝对地址，且根本不调用回调。
    int callCount = 0;
    const auto countingLookup = [&callCount](const std::string&, std::uint64_t& base) {
        ++callCount;
        base = 0x1000;
        return true;
    };
    AddressEntry plain;
    plain.absoluteAddress = 0x7FF600001000ULL;
    const auto plainResult = MemoryAddressBook::ResolveAddress(plain, countingLookup);
    suite.expect(plainResult.ok() && plainResult.address == 0x7FF600001000ULL,
        L"resolve: a module-less entry resolves to its absolute address");
    suite.expect(callCount == 0, L"resolve: a module-less entry never calls the module lookup");
    const ModuleBaseLookup noLookup;  // 空回调：调用方没有任何查询手段。
    const auto plainNoLookup = MemoryAddressBook::ResolveAddress(plain, noLookup);
    suite.expect(plainNoLookup.ok() && plainNoLookup.address == 0x7FF600001000ULL,
        L"resolve: a module-less entry needs no lookup callback at all");

    // 模块条目：基址 + rva，且回调恰被调用一次、拿到的是条目里存的模块名。
    std::string seenModule;
    int moduleCalls = 0;
    const auto recordingLookup = [&](const std::string& name, std::uint64_t& base) {
        ++moduleCalls;
        seenModule = name;
        base = 0x7FFB00000000ULL;
        return true;
    };
    AddressEntry inModule;
    inModule.moduleName = "ntdll.dll";
    inModule.rva = 0x1234;
    const auto moduleResult = MemoryAddressBook::ResolveAddress(inModule, recordingLookup);
    suite.expect(moduleResult.ok() && moduleResult.address == 0x7FFB00001234ULL,
        L"resolve: a module entry resolves to base plus rva");
    suite.expect(moduleCalls == 1 && seenModule == "ntdll.dll",
        L"resolve: the lookup is called once with the stored module name");

    // 同一个条目换一个基址（进程重启后 ASLR 变了）：地址跟着走。
    const auto movedLookup = [](const std::string&, std::uint64_t& base) {
        base = 0x7FFC00000000ULL;
        return true;
    };
    const auto movedResult = MemoryAddressBook::ResolveAddress(inModule, movedLookup);
    suite.expect(movedResult.ok() && movedResult.address == 0x7FFC00001234ULL,
        L"resolve: the same entry follows the module to its new base");

    // 模块未加载：回调返回 false（哪怕它顺手写了个基址），状态必须是 ModuleNotLoaded，
    // 地址为 0。并且即使条目里残留了旧绝对地址，也绝不能拿它冒充。
    const auto missingLookup = [](const std::string&, std::uint64_t& base) {
        base = 0x7FFB00000000ULL;
        return false;
    };
    AddressEntry stale = inModule;
    stale.absoluteAddress = 0x1111222233334444ULL;
    const auto missing = MemoryAddressBook::ResolveAddress(stale, missingLookup);
    suite.expect(missing.status == ResolveStatus::ModuleNotLoaded && !missing.ok(),
        L"resolve: a module the lookup cannot find is reported as not loaded");
    suite.expect(missing.address == 0ULL,
        L"resolve: a not-loaded module yields address 0, never the stale absolute address");
    const auto withoutCallback = MemoryAddressBook::ResolveAddress(stale, noLookup);
    suite.expect(withoutCallback.status == ResolveStatus::ModuleNotLoaded
            && withoutCallback.address == 0ULL,
        L"resolve: a module entry without any lookup is reported as not loaded");

    // 溢出边界，两侧都测：base + 0xFFF 恰好 = UINT64_MAX 合法；base + 0x1000 恰好 = 2^64 溢出。
    const auto highLookup = [](const std::string&, std::uint64_t& base) {
        base = 0xFFFFFFFFFFFFF000ULL;
        return true;
    };
    AddressEntry edge;
    edge.moduleName = "edge.dll";
    edge.rva = 0xFFF;
    const auto atMax = MemoryAddressBook::ResolveAddress(edge, highLookup);
    suite.expect(atMax.ok() && atMax.address == kU64Max,
        L"resolve: base plus rva landing exactly on UINT64_MAX is accepted");
    edge.rva = 0x1000;
    const auto pastMax = MemoryAddressBook::ResolveAddress(edge, highLookup);
    suite.expect(pastMax.status == ResolveStatus::Overflow && !pastMax.ok(),
        L"resolve: base plus rva one past UINT64_MAX is an overflow");
    suite.expect(pastMax.address == 0ULL, L"resolve: an overflow does not return the wrapped address");

    // 默认构造的结果是安全初值。
    const ksword::memwb::ResolveResult untouched;
    suite.expect(untouched.status == ResolveStatus::None && untouched.address == 0ULL && !untouched.ok(),
        L"resolve: a default result is neither ok nor carrying an address");
}

// ------------------------------------------------------------
// 七、FromAbsolute：给定模块信息时自动转成 模块+rva。
// ------------------------------------------------------------
void TestFromAbsolute(KswordTests::Suite& suite) {
    // 带模块信息：rva = 地址 - 基址（手算：0x7FFB00001234 - 0x7FFB00000000 = 0x1234）。
    const ModuleLocation ntdll{ "ntdll.dll", 0x7FFB00000000ULL };
    const auto inModule = MemoryAddressBook::FromAbsolute(
        "game.exe", EntryKind::Bookmark, 0x7FFB00001234ULL, ntdll);
    suite.expect(inModule.has_value() && inModule->moduleName == "ntdll.dll"
            && inModule->rva == 0x1234ULL,
        L"from absolute: an address inside a module becomes module plus rva");
    suite.expect(inModule.has_value() && inModule->absoluteAddress == 0ULL,
        L"from absolute: the module form does not also keep the absolute address");
    suite.expect(inModule.has_value() && inModule->targetKey == "game.exe"
            && inModule->kind == EntryKind::Bookmark && inModule->id == 0ULL
            && inModule->note.empty() && inModule->valueType == ValueType::Hex8,
        L"from absolute: target, kind and defaults are filled and the id is left for Add");

    // 地址恰等于基址：rva 为 0 是合法的（模块头）。
    const auto atBase = MemoryAddressBook::FromAbsolute(
        "game.exe", EntryKind::Search, 0x7FFB00000000ULL, ntdll);
    suite.expect(atBase.has_value() && atBase->rva == 0ULL && atBase->moduleName == "ntdll.dll",
        L"from absolute: an address exactly at the module base has rva 0");

    // 地址比基址低 1：模块信息与地址矛盾，必须拒绝而不是回绕出一个巨大的假 rva。
    const auto belowBase = MemoryAddressBook::FromAbsolute(
        "game.exe", EntryKind::Search, 0x7FFAFFFFFFFFULL, ntdll);
    suite.expect(!belowBase.has_value(),
        L"from absolute: an address below the module base is rejected, not wrapped");

    // 模块名为空与"无模块"自相矛盾，拒绝。
    const ModuleLocation nameless{ "", 0x1000 };
    suite.expect(!MemoryAddressBook::FromAbsolute("t", EntryKind::Search, 0x2000, nameless).has_value(),
        L"from absolute: module info with an empty name is rejected");

    // 不带模块信息：按绝对地址存。
    const auto bare = MemoryAddressBook::FromAbsolute("kernel", EntryKind::Watch, 0xFFFFF80000001000ULL);
    suite.expect(bare.has_value() && bare->moduleName.empty() && bare->rva == 0ULL
            && bare->absoluteAddress == 0xFFFFF80000001000ULL && bare->kind == EntryKind::Watch,
        L"from absolute: without module info the address is stored as absolute");

    // 越界 kind 被拒绝。
    suite.expect(!MemoryAddressBook::FromAbsolute("t", static_cast<EntryKind>(9), 0x10).has_value(),
        L"from absolute: an out-of-range kind is rejected");

    // 端到端：转成模块条目后，换一个基址解析，地址随模块移动。
    MemoryAddressBook book;
    const std::uint64_t id = book.Add(*inModule);
    const auto relocate = [](const std::string&, std::uint64_t& base) {
        base = 0x7FFD00000000ULL;
        return true;
    };
    const auto relocated = MemoryAddressBook::ResolveAddress(*book.Find(id), relocate);
    suite.expect(relocated.ok() && relocated.address == 0x7FFD00001234ULL,
        L"from absolute: the stored entry follows the module after a rebase");
}

// ------------------------------------------------------------
// 八、轮询策略：8 种组合手写穷举。
// ------------------------------------------------------------
void TestPollPolicy(KswordTests::Suite& suite) {
    // 只有"可见 + 有目标 + 非慢通道"这一种组合允许轮询。
    struct Case {
        bool visible;
        bool hasTarget;
        bool slow;
        bool expected;
    };
    const std::array<Case, 8> cases = { {
        { false, false, false, false },
        { false, false, true, false },
        { false, true, false, false },
        { false, true, true, false },
        { true, false, false, false },
        { true, false, true, false },
        { true, true, false, true },
        { true, true, true, false },
    } };
    for (const Case& item : cases) {
        const bool actual = ksword::memwb::ShouldPollValues(item.visible, item.hasTarget, item.slow);
        suite.expect(actual == item.expected,
            L"poll policy: only visible + has target + not slow may poll");
    }
}

} // namespace

// RunMemwbAddressBookTests：套件入口。先跑本文件的条目管理 / 解析 / 轮询用例，
// 再并入序列化用例（同一个套件、同一份失败计数）。
int RunMemwbAddressBookTests() {
    KswordTests::Suite suite(L"MEMWB address book");
    TestIdsMonotonicAndNeverReused(suite);
    TestAddRejectsAndNormalizes(suite);
    TestFindAndEdit(suite);
    TestPromote(suite);
    TestListFilters(suite);
    TestResolveAddress(suite);
    TestFromAbsolute(suite);
    TestPollPolicy(suite);
    RunMemwbAddressBookSerializeTests(suite);
    suite.report();
    return suite.failures();
}
