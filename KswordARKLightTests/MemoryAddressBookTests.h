#pragma once

// MemoryAddressBookTests.cpp 与 MemoryAddressBookTests.Serialize.cpp 共用的测试脚手架。
// 测试文件拆成两个，是因为单个文件不得超过 800 行；脚手架放在这里，避免两份拷贝。
// 这里的函数全是测试辅助，不依赖被测的序列化实现。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryAddressBook.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace MemwbAddressBookTestSupport {

using ksword::memwb::AddressEntry;
using ksword::memwb::EntryKind;
using ksword::memwb::ValueType;

// 64 位无符号数的最大值，边界测试用。
constexpr std::uint64_t kU64Max = 0xFFFFFFFFFFFFFFFFULL;

// Draft：构造一个草稿条目。id 恒为 0，由 Add 去分配。
inline AddressEntry Draft(
    const EntryKind kind,
    const char* target,
    const char* module,
    const std::uint64_t rva,
    const std::uint64_t absolute,
    const char* note,
    const ValueType valueType = ValueType::Hex8) {
    AddressEntry entry;
    entry.kind = kind;
    entry.targetKey = target;
    entry.moduleName = module;
    entry.rva = rva;
    entry.absoluteAddress = absolute;
    entry.note = note;
    entry.valueType = valueType;
    return entry;
}

// SameEntry：逐字段比较两个条目。
inline bool SameEntry(const AddressEntry& a, const AddressEntry& b) {
    return a.id == b.id && a.kind == b.kind && a.targetKey == b.targetKey
        && a.moduleName == b.moduleName && a.rva == b.rva
        && a.absoluteAddress == b.absoluteAddress && a.note == b.note
        && a.valueType == b.valueType && a.pointerChain == b.pointerChain;
}

inline AddressEntry PointerDraft() {
    auto entry = Draft(EntryKind::Bookmark, "game.exe", "game.dll", 0x100, 0, "pointer note", ValueType::U32);
    ksword::memwb::PointerBookmarkDefinition definition;
    definition.processPath = "C:\\Game\\game.exe";
    definition.modulePath = "C:\\Game\\game.dll";
    definition.moduleSize = 0x4000;
    definition.moduleFileSize = 0x8000;
    definition.moduleFileTime = 123456789;
    definition.offsets = {0, -0x10, 0x20};
    entry.pointerChain = std::move(definition);
    return entry;
}

// SameList：逐项、按顺序比较两组条目。
inline bool SameList(const std::vector<AddressEntry>& a, const std::vector<AddressEntry>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (!SameEntry(a[index], b[index])) {
            return false;
        }
    }
    return true;
}

// IdsOf：取出一组条目的 id 序列，便于一次断言顺序。
inline std::vector<std::uint64_t> IdsOf(const std::vector<AddressEntry>& entries) {
    std::vector<std::uint64_t> ids;
    for (const AddressEntry& entry : entries) {
        ids.push_back(entry.id);
    }
    return ids;
}

} // namespace MemwbAddressBookTestSupport

// RunMemwbAddressBookSerializeTests：序列化 / 反序列化相关用例，定义在
// MemoryAddressBookTests.Serialize.cpp，由 RunMemwbAddressBookTests 并入同一个套件。
void RunMemwbAddressBookSerializeTests(KswordTests::Suite& suite);
