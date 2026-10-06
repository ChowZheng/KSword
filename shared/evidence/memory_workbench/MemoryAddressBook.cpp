// MemoryAddressBook.cpp
// 地址簿的条目管理、过滤、地址解析与枚举记号。序列化部分在 MemoryAddressBook.Serialize.cpp。

#include "MemoryAddressBook.h"

#include <algorithm>
#include <utility>

namespace ksword::memwb {

namespace {

// matchesFilter：判断一个条目是否满足过滤条件。
// 传入：条目与过滤器；传出：满足返回 true。
bool MatchesFilter(const AddressEntry& entry, const AddressFilter& filter) {
    // 先看 kind 集合：非空时条目的 kind 必须在集合里。
    if (!filter.kinds.empty()) {
        // kindAllowed：条目的 kind 是否在过滤器允许的集合里。
        const bool kindAllowed =
            std::find(filter.kinds.begin(), filter.kinds.end(), entry.kind) != filter.kinds.end();
        if (!kindAllowed) {
            return false;
        }
    }

    // 再看目标：设置了 targetKey（哪怕是空串）就必须精确相等。
    if (filter.targetKey.has_value()) {
        if (entry.targetKey != *filter.targetKey) {
            return false;
        }
    }
    return true;
}

} // namespace

// ------------------------------------------------------------
// 枚举记号
// ------------------------------------------------------------

const char* EntryKindName(const EntryKind kind) noexcept {
    switch (kind) {
    case EntryKind::Search:
        return "search";
    case EntryKind::Bookmark:
        return "bookmark";
    case EntryKind::Watch:
        return "watch";
    default:
        return "unknown";
    }
}

const char* ValueTypeName(const ValueType valueType) noexcept {
    switch (valueType) {
    case ValueType::Hex8:
        return "hex8";
    case ValueType::U8:
        return "u8";
    case ValueType::U16:
        return "u16";
    case ValueType::U32:
        return "u32";
    case ValueType::U64:
        return "u64";
    case ValueType::I8:
        return "i8";
    case ValueType::I16:
        return "i16";
    case ValueType::I32:
        return "i32";
    case ValueType::I64:
        return "i64";
    case ValueType::F32:
        return "f32";
    case ValueType::F64:
        return "f64";
    default:
        return "unknown";
    }
}

bool IsValidEntryKind(const EntryKind kind) noexcept {
    switch (kind) {
    case EntryKind::Search:
    case EntryKind::Bookmark:
    case EntryKind::Watch:
        return true;
    default:
        return false;
    }
}

bool IsValidValueType(const ValueType valueType) noexcept {
    switch (valueType) {
    case ValueType::Hex8:
    case ValueType::U8:
    case ValueType::U16:
    case ValueType::U32:
    case ValueType::U64:
    case ValueType::I8:
    case ValueType::I16:
    case ValueType::I32:
    case ValueType::I64:
    case ValueType::F32:
    case ValueType::F64:
        return true;
    default:
        return false;
    }
}

bool ParseEntryKind(const std::string_view text, EntryKind& out) noexcept {
    // 逐个已定义的 kind 与记号比对；完全相等才算成功，失败不碰 out。
    constexpr EntryKind kAllKinds[] = {
        EntryKind::Search,
        EntryKind::Bookmark,
        EntryKind::Watch,
    };
    for (const EntryKind candidate : kAllKinds) {
        if (text == EntryKindName(candidate)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

bool ParseValueType(const std::string_view text, ValueType& out) noexcept {
    // 与 ParseEntryKind 同理，把 11 个已定义类型逐个比对。
    constexpr ValueType kAllTypes[] = {
        ValueType::Hex8,
        ValueType::U8,
        ValueType::U16,
        ValueType::U32,
        ValueType::U64,
        ValueType::I8,
        ValueType::I16,
        ValueType::I32,
        ValueType::I64,
        ValueType::F32,
        ValueType::F64,
    };
    for (const ValueType candidate : kAllTypes) {
        if (text == ValueTypeName(candidate)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

const char* DeserializeErrorText(const DeserializeError code) noexcept {
    switch (code) {
    case DeserializeError::None:
        return "";
    case DeserializeError::EmptyInput:
        return "empty input: header line is missing";
    case DeserializeError::BadHeader:
        return "header line is not KSWORD-ADDRESS-BOOK <version>";
    case DeserializeError::UnsupportedVersion:
        return "unsupported address book version";
    case DeserializeError::UnterminatedLine:
        return "line is not terminated by LF (file truncated?)";
    case DeserializeError::RawCarriageReturn:
        return "raw carriage return in line (file converted to CRLF?)";
    case DeserializeError::WrongFieldCount:
        return "entry line does not have exactly 8 tab-separated fields";
    case DeserializeError::BadId:
        return "entry id is not a decimal number in range";
    case DeserializeError::UnknownKind:
        return "unknown entry kind";
    case DeserializeError::UnknownValueType:
        return "unknown value type";
    case DeserializeError::BadAddress:
        return "address field is not 0x followed by 1-16 hex digits";
    case DeserializeError::BadEscape:
        return "invalid backslash escape in text field";
    case DeserializeError::InconsistentAddress:
        return "module name and rva/absolute address contradict each other";
    case DeserializeError::DuplicateId:
        return "duplicate entry id";
    default:
        return "unknown error";
    }
}

// ------------------------------------------------------------
// 条目管理
// ------------------------------------------------------------

void MemoryAddressBook::NormalizeAddressFields(AddressEntry& entry) noexcept {
    // 有模块名：地址以 模块基址+rva 为准，旧绝对地址清零，防止被拿来冒充。
    if (!entry.moduleName.empty()) {
        entry.absoluteAddress = 0;
        return;
    }
    // 无模块名：地址就是绝对地址，rva 没有意义，清零。
    entry.rva = 0;
}

std::uint64_t MemoryAddressBook::Add(const AddressEntry& draft) {
    // 枚举取值越界（调用方 static_cast 出来的垃圾）与号段用尽都直接拒绝，
    // 此时不碰任何内部状态，nextId_ 也不推进。
    if (!IsValidEntryKind(draft.kind) || !IsValidValueType(draft.valueType)) {
        return 0;
    }
    if (nextId_ >= kAddressBookIdLimit) {
        return 0;
    }

    // 复制草稿、盖上新 id、归一化不适用的地址字段。
    // stored：真正入库的条目副本，与调用方的草稿互不影响。
    AddressEntry stored = draft;
    // newId：本次分配的 id，先存下来，因为 stored 随后会被移动走。
    const std::uint64_t newId = nextId_;
    stored.id = newId;
    NormalizeAddressFields(stored);

    // 入库并记录插入顺序，最后推进 nextId_——删除不会让它回退，id 因此永不复用。
    entries_.emplace(newId, std::move(stored));
    order_.push_back(newId);
    nextId_ = newId + 1;
    return newId;
}

bool MemoryAddressBook::Remove(const std::uint64_t id) {
    // 先从按 id 的存储里摘掉；不存在就什么都不动。
    // erasedCount：被摘掉的条目数，0 表示 id 不存在，1 表示删除成功。
    const std::size_t erasedCount = entries_.erase(id);
    if (erasedCount == 0) {
        return false;
    }

    // 再从插入顺序里去掉。nextId_ 保持不变，这是"永不复用"的关键。
    // position：该 id 在插入顺序里的位置。
    const auto position = std::find(order_.begin(), order_.end(), id);
    if (position != order_.end()) {
        order_.erase(position);
    }
    return true;
}

std::optional<AddressEntry> MemoryAddressBook::Find(const std::uint64_t id) const {
    // found：按 id 查到的存储位置，end() 表示不存在。
    const auto found = entries_.find(id);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool MemoryAddressBook::SetNote(const std::uint64_t id, std::string note) {
    // found：要改备注的条目的存储位置。
    const auto found = entries_.find(id);
    if (found == entries_.end()) {
        return false;
    }
    found->second.note = std::move(note);
    return true;
}

bool MemoryAddressBook::SetValueType(const std::uint64_t id, const ValueType valueType) {
    // 类型越界先拒绝，不能把垃圾枚举值写进条目再被序列化成 "unknown"。
    if (!IsValidValueType(valueType)) {
        return false;
    }
    // found：要改类型的条目的存储位置。
    const auto found = entries_.find(id);
    if (found == entries_.end()) {
        return false;
    }
    found->second.valueType = valueType;
    return true;
}

bool MemoryAddressBook::Promote(const std::uint64_t id, const EntryKind newKind) {
    if (!IsValidEntryKind(newKind)) {
        return false;
    }
    // found：要升级的条目的存储位置。
    const auto found = entries_.find(id);
    if (found == entries_.end()) {
        return false;
    }

    // 禁止降级回 Search：Search 是临时的，降级会让用户保存的条目被下一轮搜索清掉。
    // 同 kind（包括 Search -> Search）视为幂等成功，不走这条拒绝。
    if (newKind == EntryKind::Search && found->second.kind != EntryKind::Search) {
        return false;
    }
    found->second.kind = newKind;
    return true;
}

std::vector<AddressEntry> MemoryAddressBook::List(const AddressFilter& filter) const {
    // 沿插入顺序逐个取出条目副本，满足过滤条件的才收下。
    // result：满足条件的条目副本，按插入顺序排列。
    std::vector<AddressEntry> result;
    result.reserve(order_.size());
    for (const std::uint64_t id : order_) {
        // found：当前 id 对应的存储位置。
        const auto found = entries_.find(id);
        if (found == entries_.end()) {
            // order_ 与 entries_ 理应同步；万一不同步就跳过，不制造空条目。
            continue;
        }
        if (MatchesFilter(found->second, filter)) {
            result.push_back(found->second);
        }
    }
    return result;
}

std::size_t MemoryAddressBook::Size() const noexcept {
    return entries_.size();
}

std::uint64_t MemoryAddressBook::NextId() const noexcept {
    return nextId_;
}

// ------------------------------------------------------------
// 地址解析与构造
// ------------------------------------------------------------

ResolveResult MemoryAddressBook::ResolveAddress(
    const AddressEntry& entry,
    const ModuleBaseLookup& lookup) {
    // result：返回值，默认 status==None、address==0，每条失败路径都原样带出这个安全初值。
    ResolveResult result;

    // 无模块条目：绝对地址就是答案，根本不需要（也不会调用）查询回调。
    if (entry.moduleName.empty()) {
        result.status = ResolveStatus::Ok;
        result.address = entry.absoluteAddress;
        return result;
    }

    // 有模块名但没有查询手段：等同于"模块未加载"，不能退回绝对地址。
    if (!lookup) {
        result.status = ResolveStatus::ModuleNotLoaded;
        return result;
    }

    // 回调返回 false 就忽略它可能写出的基址——找不到就是找不到。
    // moduleBase：回调写出的模块基址，只在 moduleLoaded 为真时有意义。
    std::uint64_t moduleBase = 0;
    // moduleLoaded：回调是否找到了该模块。
    const bool moduleLoaded = lookup(entry.moduleName, moduleBase);
    if (!moduleLoaded) {
        result.status = ResolveStatus::ModuleNotLoaded;
        return result;
    }

    // 先判溢出再相加：回绕出来的值同样是个"合法"地址，会被照单读下去。
    if (moduleBase > ~0ULL - entry.rva) {
        result.status = ResolveStatus::Overflow;
        return result;
    }
    result.status = ResolveStatus::Ok;
    result.address = moduleBase + entry.rva;
    return result;
}

std::optional<AddressEntry> MemoryAddressBook::FromAbsolute(
    const std::string& targetKey,
    const EntryKind kind,
    const std::uint64_t address,
    const std::optional<ModuleLocation>& moduleInfo) {
    if (!IsValidEntryKind(kind)) {
        return std::nullopt;
    }

    // 草稿的公共部分：目标、kind；valueType 保持默认的 Hex8，id 为 0 等 Add 分配。
    // draft：要返回的条目草稿。
    AddressEntry draft;
    draft.kind = kind;
    draft.targetKey = targetKey;

    // 没给模块信息：按绝对地址存，模块与 rva 留空。
    if (!moduleInfo.has_value()) {
        draft.absoluteAddress = address;
        return draft;
    }

    // 给了模块信息：模块名不能为空（空名在条目里表示"无模块"，会自相矛盾）；
    // 地址不能低于基址（否则 address - base 回绕成一个巨大的假 rva）。
    if (moduleInfo->moduleName.empty()) {
        return std::nullopt;
    }
    if (address < moduleInfo->moduleBase) {
        return std::nullopt;
    }
    draft.moduleName = moduleInfo->moduleName;
    draft.rva = address - moduleInfo->moduleBase;
    return draft;
}

} // namespace ksword::memwb
