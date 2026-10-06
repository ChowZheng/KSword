#pragma once

// ============================================================
// MemoryAddressBook.h
// 作用：
// - 统一的"地址簿"：把"搜索结果""书签""监视"合并成同一种带 kind 的条目模型，
//   由同一个类持有、同一套规则管理、同一份文件持久化。
// - 纯 C++20 标准库实现：不碰 Qt、不碰 Win32，可以在离线套件里直接跑。
//
// 为什么要重写（旧书签与旧搜索结果表的缺陷，每一条都对应本类的一个设计点）：
// - 旧书签只存一个绝对地址，不绑进程也不绑模块：换了进程、或者同一进程重启后
//   模块基址变了，书签照样"成功"地指向一个完全无关的地址。
//   -> 条目带 targetKey（按目标分组），并且地址优先存成 模块名 + RVA；
//      ResolveAddress 在模块没加载时明确返回"模块未加载"，绝不拿旧绝对地址冒充。
// - 旧书签不持久化。
//   -> Serialize / Deserialize（普通条目保留 v1；指针链使用 v2，见下）。
// - 旧书签备注不可编辑、值只能显示 8 字节十六进制。
//   -> SetNote / SetValueType（ValueType::Hex8 保留旧显示作默认值）。
// - 旧搜索结果表按"行号"去缓存里取地址，一排序行号就和缓存错位，点中的是别的地址。
//   -> 所有操作一律按 id，不存在"第几行"这个概念；id 单调递增、永不复用。
// - 旧定时器无条件常跑。
//   -> ShouldPollValues 纯函数：页面不可见、没有目标、通道很慢时都不轮询。
//
// id 规则：
// - 从 1 开始单调递增，永不复用：删除条目（哪怕是当前最大的那个）之后再添加，
//   也拿到一个全新的 id。持有旧 id 的界面回调因此只会"找不到"，不会"找到别人"。
// - 0 是"无效 id"，Add 失败返回 0。UINT64_MAX 保留为上限哨兵，id 必须小于它，
//   号段用尽后 Add 返回 0 而不是回绕（实际不可能走到，但回绕的后果是复用 id）。
// - id 的唯一性保证只在一次内存生命周期内成立。持久化文件不记录"下一个 id"，
//   反序列化后的下一个 id = max(载入前的下一个 id, 文件中最大 id + 1)。
//   所以一份新建的簿载入文件后，若文件里最大的 id 曾被删除过，该号段可能再次出现；
//   跨进程重启持有旧 id 本来就没有意义，这是有意的取舍。
//
// 地址字段规则（Add 时归一化，Deserialize 时严格校验）：
// - moduleName 非空：地址 = 模块基址 + rva，absoluteAddress 恒为 0。
// - moduleName 为空：地址 = absoluteAddress，rva 恒为 0。
// - Add 会把"不适用"的那个字段清零。这样"拿旧绝对地址冒充"在结构上就不可能：
//   模块条目里根本没有一个旧绝对地址可拿。
//
// Promote 规则：
// - 允许 Search -> Bookmark / Watch，以及 Bookmark <-> Watch。
// - 指针链条目只允许 Bookmark -> Bookmark，不进入 Watch 的周期读取。
// - 禁止把 Bookmark / Watch 降回 Search：搜索结果是临时的，新一轮搜索会清掉它们，
//   降级等于让用户保存的条目被下一次搜索悄悄删掉。
//
// ============================================================
// 持久化格式 v1（一次性新格式，没有任何旧格式兼容代码）
// ============================================================
// - UTF-8 文本，行分隔符只认 LF（'\n'），每一行（含最后一行）都必须以 LF 结尾。
//   文件必须按二进制写入、读取：被文本模式转成 CRLF 的文件会被拒绝（见下），
//   而不是悄悄把每条备注末尾多出一个回车。
// - 第 1 行恰为 "KSWORD-ADDRESS-BOOK 1"。
// - 之后每条一行，8 个字段以制表符分隔，顺序固定：
//     id \t kind \t valueType \t targetKey \t moduleName \t rva \t absolute \t note
//   * id：十进制，1 <= id < UINT64_MAX。
//   * kind：search | bookmark | watch（小写，区分大小写）。
//   * valueType：hex8 u8 u16 u32 u64 i8 i16 i32 i64 f32 f64（小写，区分大小写）。
//   * rva / absolute：小写 "0x" 前缀 + 1~16 位十六进制（数位大小写均可读，写出时为
//     最短小写形式；允许前导零，但总位数超过 16 位一律拒绝）。
//   * targetKey / moduleName / note：反斜杠转义，规则为
//       反斜杠 -> \\   制表符 -> \t   换行 -> \n   回车 -> \r
//     其余字节（含 emoji、中文等多字节 UTF-8）原样写出。出现其它反斜杠序列、
//     或字段末尾孤立的反斜杠，视为损坏。
// - 以下一律拒绝并报告出错行号（从 1 开始，含标题行）：空输入、标题不对、
//   未知版本、行没有以 LF 结尾（典型成因是文件被截断）、行内出现裸回车、
//   字段数不是 8、id 非法、未知 kind / valueType、地址文本非法、转义非法、
//   moduleName 与 rva / absolute 互相矛盾、id 重复。
// - 拒绝时传入的 MemoryAddressBook 保持原样：先解析到临时对象，全部通过才一次性替换。
//
// v2：标题为 "KSWORD-ADDRESS-BOOK 2"，每行在原有 8 字段后追加固定 8 字段：
//   pointer \t processPath \t modulePath \t moduleSize \t fileSize \t fileTime \t width \t offsets
// 普通条目的 8 个追加字段全部为空；指针条目的 marker 为 "pointer"，路径沿用转义，
// 三个大小/时间为正十进制数，width 为 4/8，offsets 为逗号分隔的有符号 0x 十六进制。
// 指针链只允许 Bookmark；moduleName/rva 是根模块与根 RVA，禁止缓存解析后的绝对地址。
// v2 输入最多 kAddressBookV2TextLimit 字节；仅选中普通条目时仍写出 v1。
//
// 约束：命名空间 ksword::memwb，C++20，仅标准库，不含 Windows.h / Qt 头。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ksword::memwb {

// EntryKind：条目的种类。同一份地址簿里三种混放，靠 kind 区分用途。
// Search 是临时的（新一轮搜索会清掉），Bookmark / Watch 是用户保存的。
enum class EntryKind : int {
    Search = 0,
    Bookmark = 1,
    Watch = 2,
};

// ValueType：该条目的值按什么类型解释与显示。Hex8 即旧书签的"8 字节十六进制"。
enum class ValueType : int {
    Hex8 = 0,
    U8,
    U16,
    U32,
    U64,
    I8,
    I16,
    I32,
    I64,
    F32,
    F64,
};

// 号段上限哨兵：id 必须严格小于它，UINT64_MAX 本身不会被分配出去。
inline constexpr std::uint64_t kAddressBookIdLimit = ~0ULL;
inline constexpr std::size_t kAddressBookV2TextLimit = 16U * 1024U * 1024U;

// EntryKindName / ValueTypeName：枚举对应的持久化记号（见上面格式说明）。
// 这些字符串属于 v1 文件格式的一部分，不得改名。传入非法枚举值返回 "unknown"。
const char* EntryKindName(EntryKind kind) noexcept;
const char* ValueTypeName(ValueType valueType) noexcept;

// ParseEntryKind / ParseValueType：持久化记号 -> 枚举。完全匹配才成功（区分大小写）；
// 失败返回 false 且 out 保持不变。
bool ParseEntryKind(std::string_view text, EntryKind& out) noexcept;
bool ParseValueType(std::string_view text, ValueType& out) noexcept;

// IsValidEntryKind / IsValidValueType：判断枚举值是否落在已定义的取值内。
// 调用方可能 static_cast 出越界值，Add / SetValueType / Promote 据此拒绝。
bool IsValidEntryKind(EntryKind kind) noexcept;
bool IsValidValueType(ValueType valueType) noexcept;

struct PointerBookmarkDefinition {
    std::string processPath;
    std::string modulePath;
    std::uint64_t moduleSize = 0;
    std::int64_t moduleFileSize = 0;
    std::int64_t moduleFileTime = 0;
    std::uint32_t pointerSize = 8;
    std::vector<std::int64_t> offsets;

    friend bool operator==(const PointerBookmarkDefinition&, const PointerBookmarkDefinition&) = default;
};

// AddressEntry：地址簿中的一个条目。
struct AddressEntry {
    std::uint64_t id = 0;                 // 条目 id；Add 时由簿分配，草稿里填什么都会被覆盖。
    EntryKind kind = EntryKind::Search;   // 条目种类。
    std::string targetKey;                // 调用方给的目标标识（如映像名），用于按目标分组。
    std::string moduleName;               // 所在模块名；为空表示无模块（按绝对地址）。
    std::uint64_t rva = 0;                // 相对模块基址的偏移；仅 moduleName 非空时有效。
    std::uint64_t absoluteAddress = 0;    // 绝对地址；仅 moduleName 为空时有效。
    std::string note;                     // 用户备注，可为空，可编辑。
    ValueType valueType = ValueType::Hex8; // 值的解释类型，默认保持旧书签的 8 字节十六进制。
    std::optional<PointerBookmarkDefinition> pointerChain; // 仅书签；根地址仍由 moduleName/rva 表达。
};

// AddressFilter：List / Serialize 的过滤条件。两个条件同时给出时取交集。
struct AddressFilter {
    std::vector<EntryKind> kinds;            // 允许的 kind 集合；为空表示不按 kind 过滤。
    std::optional<std::string> targetKey;    // 只要这个目标的条目；未设置表示不按目标过滤。
                                             // 空串是合法的目标标识，与"未设置"不同。
};

// ModuleLocation：FromAbsolute 的输入——某个绝对地址所在模块的名字与基址。
struct ModuleLocation {
    std::string moduleName;        // 模块名，必须非空。
    std::uint64_t moduleBase = 0;  // 该模块在目标里的加载基址。
};

// ResolveStatus：ResolveAddress 的结论。每种失败独立成一个状态，调用方据此告诉用户该怎么办。
enum class ResolveStatus : int {
    None = 0,          // 默认值：尚未解析（也是失败时的安全初值）。
    Ok,                // 解析成功，address 有效。
    ModuleNotLoaded,   // 条目绑定的模块当前没有加载（或没提供查询回调）。
    Overflow,          // 模块基址 + rva 超出 64 位地址空间。
    RequiresPointerResolution, // 只能由显式指针解析读取；普通查询/轮询不得使用根 RVA。
};

// ResolveResult：ResolveAddress 的输出。失败时 address 恒为 0。
struct ResolveResult {
    ResolveStatus status = ResolveStatus::None;  // 解析结论。
    std::uint64_t address = 0;                   // 解析出的绝对地址；仅 status==Ok 时有意义。

    // ok：status 是否为 Ok。
    bool ok() const noexcept {
        return status == ResolveStatus::Ok;
    }
};

// ModuleBaseLookup：按模块名查询当前加载基址的回调。
// 找到返回 true 并写 moduleBaseOut；找不到返回 false（此时 moduleBaseOut 会被忽略）。
// 模块名大小写是否敏感由回调自己决定（Windows 上通常不敏感）。
using ModuleBaseLookup =
    std::function<bool(const std::string& moduleName, std::uint64_t& moduleBaseOut)>;

// DeserializeError：Deserialize 失败的具体原因，供界面翻译成提示文字、供测试精确断言。
enum class DeserializeError : int {
    None = 0,              // 成功。
    EmptyInput,            // 输入为空，连标题行都没有。
    BadHeader,             // 标题行不是 "KSWORD-ADDRESS-BOOK <版本>"。
    UnsupportedVersion,    // 标题魔数正确但版本不是 1/2。
    UnterminatedLine,      // 某行没有以 LF 结尾（文件被截断的典型表现）。
    RawCarriageReturn,     // 行内出现裸回车（文件被文本模式转成了 CRLF）。
    WrongFieldCount,       // 字段数与版本不符（v1 为 8；v2 为 16）。
    BadId,                 // id 不是十进制数、为 0、为上限哨兵或溢出。
    UnknownKind,           // kind 记号未知。
    UnknownValueType,      // valueType 记号未知。
    BadAddress,            // rva / absolute 不是合法的 0x 十六进制文本。
    BadEscape,             // 字符串字段里有非法的反斜杠序列。
    InconsistentAddress,   // moduleName 与 rva / absolute 互相矛盾。
    DuplicateId,           // 同一个 id 出现了两次。
    BadPointerChain,       // 指针链标记、元数据、偏移或条目种类不合法。
    InputTooLarge,         // v2 文本超出容量上限。
};

// DeserializeErrorText：错误码对应的简短英文诊断文本，成功码返回空串。
const char* DeserializeErrorText(DeserializeError code) noexcept;

// DeserializeResult：Deserialize 的输出。成功时 errorLine==0、errorCode==None、errorText 为空。
struct DeserializeResult {
    bool ok = false;                                  // 是否成功。
    std::size_t errorLine = 0;                        // 出错行号，从 1 开始，含标题行；成功为 0。
    DeserializeError errorCode = DeserializeError::None; // 出错原因。
    std::string errorText;                            // errorCode 对应的诊断文本。
};

// ShouldPollValues：值刷新定时器该不该这一拍去读目标。
// 调用方式：定时器触发时传入当前页面可见性、是否已有目标、当前通道是否为慢通道。
// 三个条件缺一不可：页面不可见（没人看）、没有目标（没东西可读）、通道很慢
// （例如磁盘 DMA，轮询会把它拖垮）时都返回 false。
constexpr bool ShouldPollValues(
    const bool pageVisible,
    const bool hasTarget,
    const bool channelIsSlow) noexcept {
    return pageVisible && hasTarget && !channelIsSlow;
}

// MemoryAddressBook：统一地址簿。
// 线程模型：非线程安全，由调用方（通常是 UI 线程）串行使用。
class MemoryAddressBook {
public:
    // Add：添加一个条目。
    // 传入：草稿，其 id 字段被忽略；不适用的地址字段会被清零（见文件头）。
    // 传出：分配到的新 id（>=1）。kind / valueType 越界、指针定义非法，或号段用尽时
    //       返回 0，簿保持不变。
    std::uint64_t Add(const AddressEntry& draft);

    // Remove：按 id 删除。返回是否真的删掉了东西；id 不存在返回 false。
    bool Remove(std::uint64_t id);

    // Find：按 id 取条目的副本。id 不存在返回 std::nullopt。
    // 返回副本而不是指针：条目会被增删，悬垂指针比多拷一份贵得多。
    std::optional<AddressEntry> Find(std::uint64_t id) const;

    // SetNote：改备注，可为空串。id 不存在返回 false。
    bool SetNote(std::uint64_t id, std::string note);

    // SetValueType：改值的解释类型。id 不存在或类型越界返回 false，条目保持不变。
    bool SetValueType(std::uint64_t id, ValueType valueType);

    // 更新现有 Bookmark 的指针链及模块根位置；保留 id、目标、备注、值类型。
    // 不接受 Search/Watch 或无效定义，失败时条目完全不变。
    bool SetPointerChain(std::uint64_t id, const PointerBookmarkDefinition& definition,
                         std::string moduleName, std::uint64_t rootRva);

    // Shared validation for explicit pointer bindings/adapters; ordinary entries
    // without a pointer definition are valid for this particular policy.
    static bool ValidPointerChain(const AddressEntry& entry) noexcept;

    // Promote：改条目 kind（如 Search -> Bookmark），位置与其它字段都不变。
    // 目标 kind 与当前相同视为成功（幂等）。Bookmark / Watch -> Search 被拒绝，
    // id 不存在、kind 越界同样返回 false，条目保持不变。
    bool Promote(std::uint64_t id, EntryKind newKind);

    // List：按插入顺序返回满足过滤条件的条目副本；默认过滤器返回全部。
    std::vector<AddressEntry> List(const AddressFilter& filter = AddressFilter{}) const;

    // Size：当前条目数。
    std::size_t Size() const noexcept;

    // NextId：下一个将被分配的 id（诊断与测试用）。
    std::uint64_t NextId() const noexcept;

    // ResolveAddress：把条目解析成当前目标里的绝对地址。
    // 传入：条目；模块基址查询回调（无模块条目用不到，可传空）。
    // 传出：见 ResolveStatus。有模块名的条目只走"基址 + rva"：回调找不到模块就是
    //       ModuleNotLoaded，加法溢出就是 Overflow，两种情况都不会回退到绝对地址。
    //       指针书签恒返回 RequiresPointerResolution，查询回调不会被调用。
    static ResolveResult ResolveAddress(
        const AddressEntry& entry,
        const ModuleBaseLookup& lookup);

    // FromAbsolute：由绝对地址构造一个条目草稿（id 为 0，需再交给 Add）。
    // 传入：目标标识、kind、绝对地址；可选的 moduleInfo 表示"这个地址位于该模块里"，
    //       此时自动转成 模块名 + rva 存储（rva = address - moduleBase）。
    // 传出：草稿；以下情况返回 std::nullopt：kind 越界；moduleInfo 的模块名为空；
    //       address 小于 moduleBase（模块信息与地址矛盾，rva 会回绕成巨大的假值）。
    // 调用方负责保证地址确实落在该模块范围内，本函数不知道模块大小。
    static std::optional<AddressEntry> FromAbsolute(
        const std::string& targetKey,
        EntryKind kind,
        std::uint64_t address,
        const std::optional<ModuleLocation>& moduleInfo = std::nullopt);

    // Serialize：存在选中的指针书签时写 v2，否则仍写 v1；顺序与 id 保持原样。
    // 传入：可选过滤器（例如只保存 Bookmark / Watch，不保存临时的 Search 结果）。
    // 传出：以 LF 结尾的文本，至少含标题行。
    std::string Serialize(const AddressFilter& filter = AddressFilter{}) const;

    // Deserialize：解析 v1/v2 文本，成功时整体替换 out 的内容。
    // 传入：文本；输出簿。
    // 传出：见 DeserializeResult。失败时 out 完全不变（包括下一个 id）；成功后
    //       下一个 id = max(out 载入前的下一个 id, 文件中最大 id + 1)。
    static DeserializeResult Deserialize(std::string_view text, MemoryAddressBook& out);

private:
    // 归一化：清零条目里不适用的地址字段。
    static void NormalizeAddressFields(AddressEntry& entry) noexcept;

    // 按 id 索引的条目存储，取 id 为键保证按 id 查找是对数级。
    std::map<std::uint64_t, AddressEntry> entries_;
    // 插入顺序：List / Serialize 沿用它，与 id 大小无关（文件里的 id 可以乱序）。
    std::vector<std::uint64_t> order_;
    // 下一个要分配的 id，起点 1，单调不减。
    std::uint64_t nextId_ = 1;
};

} // namespace ksword::memwb
