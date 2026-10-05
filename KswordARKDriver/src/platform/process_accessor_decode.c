// 用途：生产访问器解码器；离线回归直接编译本文件，不复制另一套实现。
#include "process_accessor_decode.h"

// 分别约束无副作用入口填充、单入口代码读取、跳转次数和对象字段偏移。
#define KSW_ACCESSOR_PREFIX_LIMIT 16U // 最多接受十六字节明确的 NOP/CET 前缀。
#define KSW_ACCESSOR_CODE_LIMIT 32U // 每个入口最多读取三十二字节。
#define KSW_ACCESSOR_THUNK_LIMIT 2U // 至多跟随两次直接相对入口跳转。
#define KSW_ACCESSOR_FIELD_LIMIT 0x3FFFU // 不接受对象有界窗口以外的字段偏移。

// 用途：在开始解码或拒绝解码时清空旧结果；Result 是调用者的结果输出。
static void
KswordARKAccessorReset(KSWORD_ACCESSOR_DISPLACEMENT* Result)
{
    // 拒绝路径不得保留此前已接受的偏移，避免调用者误用旧结果。
    if (Result != NULL) {
        Result->Offset = -1; // 偏移初始化为不可用，禁止继承旧候选。
        Result->LoadKind = KswordAccessorLoadPointer; // 重置读取类型以保持失败输出确定。
    }
}

// 用途：跳过明确无副作用的入口前缀；Bytes/Count 指定已完整读取的本地代码。
// 返回：正文起点；前缀超预算时返回 KSW_ACCESSOR_SIZE_MAX，截断前缀留给正文校验拒绝。
static KSW_ACCESSOR_SIZE
KswordARKAccessorSkipPrefixes(const KSW_ACCESSOR_BYTE* Bytes, KSW_ACCESSOR_SIZE Count)
{
    KSW_ACCESSOR_SIZE position = 0U; // 当前已验证前缀末尾的位置。
    int endBranchSeen = 0; // CET 入口标记只允许出现一次。

    // 正文前只允许精确编码的架构 NOP 与一次 CET 入口标记。
    while (position < Count) {
        KSW_ACCESSOR_SIZE remaining = Count - position; // 本轮剩余的完整本地字节数。
        KSW_ACCESSOR_SIZE width = 0U; // 本轮识别出的前缀宽度，零表示正文。
        if (Bytes[position] == 0x90U) {
            width = 1U; // 接受单字节架构 NOP。
        }
        else if (remaining >= 2U && Bytes[position] == 0x66U &&
            Bytes[position + 1U] == 0x90U) {
            width = 2U; // 记录两字节前缀或短跳转的完整宽度。
        }
        else if (remaining >= 3U && Bytes[position] == 0x0FU &&
            Bytes[position + 1U] == 0x1FU && Bytes[position + 2U] == 0x00U) {
            width = 3U; // 接受三字节架构 NOP。
        }
        else if (remaining >= 5U && Bytes[position] == 0x0FU &&
            Bytes[position + 1U] == 0x1FU && Bytes[position + 2U] == 0x44U &&
            Bytes[position + 3U] == 0x00U && Bytes[position + 4U] == 0x00U) {
            width = 5U; // 记录五字节 NOP 或近跳转的完整宽度。
        }
        else if (remaining >= 4U && !endBranchSeen &&
            Bytes[position] == 0xF3U && Bytes[position + 1U] == 0x0FU &&
            Bytes[position + 2U] == 0x1EU && Bytes[position + 3U] == 0xFAU) {
            width = 4U; // 接受四字节 CET 入口标记。
            endBranchSeen = 1; // 禁止再次接受 CET 入口标记。
        }
        else {
            break; // 本轮形态已确定，停止继续扫描。
        }
        if (width > KSW_ACCESSOR_PREFIX_LIMIT - position) {
            return KSW_ACCESSOR_SIZE_MAX; // 前缀超过明确预算，通知调用者拒绝。
        }
        position += width; // 只前进到已完整验证的前缀末尾。
    }
    return position; // 返回已验证前缀后的正文位置。
}

// 用途：按小端读取四字节本地整数；调用方须先证明 Bytes 的四字节完整可读。
static KSW_ACCESSOR_U32
KswordARKAccessorReadU32(const KSW_ACCESSOR_BYTE* Bytes)
{
    // 按字节组合数值，既不假设四字节对齐，也不对候选地址做指针强转读取。
    return (KSW_ACCESSOR_U32)Bytes[0] | ((KSW_ACCESSOR_U32)Bytes[1] << 8U) |
        ((KSW_ACCESSOR_U32)Bytes[2] << 16U) | ((KSW_ACCESSOR_U32)Bytes[3] << 24U); // 只组合调用方已验证完整的四个本地字节。
}

// 用途与参数见头文件；只接受一个明确访问器操作及其立即返回。
int
KswordARKDecodeAccessorDisplacement(
    const KSW_ACCESSOR_BYTE* Bytes,
    KSW_ACCESSOR_SIZE ByteCount,
    KSWORD_ACCESSOR_DISPLACEMENT* DisplacementOut)
{
    KSW_ACCESSOR_SIZE position = 0U; // 本地缓冲区内下一条待校验字节的位置。
    KSW_ACCESSOR_SIZE displacementWidth = 0U; // ModRM 明确指定的一字节或四字节位移宽度。
    KSW_ACCESSOR_U32 displacement = 0U; // 尚未通过正值与窗口校验的候选位移。
    KSW_ACCESSOR_BYTE modrm = 0U; // 当前指令的寻址与目标寄存器编码。
    KSWORD_ACCESSOR_LOAD_KIND kind = KswordAccessorLoadPointer; // 指令确定的读取操作。

    KswordARKAccessorReset(DisplacementOut); // 先清空旧偏移，所有失败路径保持不可用。
    if (Bytes == NULL || DisplacementOut == NULL || ByteCount == 0U) {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    position = KswordARKAccessorSkipPrefixes(Bytes, ByteCount); // 只跳过精确的无副作用入口前缀。
    if (position == KSW_ACCESSOR_SIZE_MAX || position >= ByteCount) {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    // REX.W 的 MOV/LEA 明确向 RAX 返回一个指针宽度字段或对象内字段地址。
    if (ByteCount - position >= 3U && Bytes[position] == 0x48U &&
        (Bytes[position + 1U] == 0x8BU || Bytes[position + 1U] == 0x8DU)) {
        kind = Bytes[position + 1U] == 0x8DU ?
            KswordAccessorAddress : KswordAccessorLoadPointer; // 区分字段地址计算与指针字段加载。
        position += 2U; // 越过已经完整匹配的双字节操作码。
    }
    // MOV EAX 将一个 ULONG 字段零扩展到完整返回寄存器。
    else if (Bytes[position] == 0x8BU) {
        kind = KswordAccessorLoadUlong; // 记录 EAX 中的零扩展 ULONG 读取。
        position += 1U; // 越过已经匹配的单字节操作码。
    }
    // MOVZX EAX 将 UCHAR/USHORT 零扩展；不接受只写 AL 导致高位未定义的形态。
    else if (ByteCount - position >= 3U && Bytes[position] == 0x0FU &&
        (Bytes[position + 1U] == 0xB6U || Bytes[position + 1U] == 0xB7U)) {
        kind = Bytes[position + 1U] == 0xB6U ?
            KswordAccessorLoadUchar : KswordAccessorLoadUshort; // 按 MOVZX 操作码确定零扩展宽度。
        position += 2U; // 越过已经完整匹配的双字节操作码。
    }
    else {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    if (position >= ByteCount) {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    // 仅接受 RCX 基址、RAX/EAX 目标且无 SIB 的精确 ModRM，避免错认其它对象或寄存器。
    modrm = Bytes[position++]; // 读取寻址字节并移动到候选位移起点。
    if (modrm == 0x41U) {
        displacementWidth = 1U; // RCX 加 disp8 的精确寻址形态。
    }
    else if (modrm == 0x81U) {
        displacementWidth = 4U; // RCX 加 disp32 的精确寻址形态。
    }
    else {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    if (ByteCount - position < displacementWidth) {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    // disp8 是有符号位移；负偏移和零偏移按当前对象窗口约定保持不可用。
    if (displacementWidth == 1U) {
        displacement = Bytes[position]; // 先读取本地 disp8 编码，再排除负值。
        if (displacement >= 0x80U) {
            return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
        }
    }
    else {
        displacement = KswordARKAccessorReadU32(Bytes + position); // 按小端取得完整 disp32 编码。
    }
    position += displacementWidth; // 移动到完整加载指令的下一字节。
    if (displacement == 0U || displacement > KSW_ACCESSOR_FIELD_LIMIT ||
        position >= ByteCount) {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    // 加载后不得夹带分支、第二次加载、位掩码或其它副作用，必须立即完整返回。
    if (Bytes[position] != 0xC3U &&
        !(ByteCount - position >= 2U && Bytes[position] == 0xF3U &&
          Bytes[position + 1U] == 0xC3U)) {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    DisplacementOut->Offset = (KSW_ACCESSOR_OFFSET)displacement; // 仅发布已满足指令与正值窗口校验的偏移。
    DisplacementOut->LoadKind = kind; // 将确证的读取宽度与偏移同时发布。
    return 1; // 完整访问器或跳转形态已经通过全部所需字节校验。
}

// 用途：只解码 EB/E9 直接入口跳转；Bytes/Count 是完整本地代码，Address 是入口地址。
// TargetOut 接收经过溢出检查的目标；代码区段归属由安全读取提供者继续确认。
static int
KswordARKAccessorDecodeThunk(
    const KSW_ACCESSOR_BYTE* Bytes, KSW_ACCESSOR_SIZE Count, KSW_ACCESSOR_ADDRESS Address, KSW_ACCESSOR_ADDRESS* TargetOut)
{
    KSW_ACCESSOR_SIZE position = KswordARKAccessorSkipPrefixes(Bytes, Count); // 无副作用前缀后的入口正文位置。
    KSW_ACCESSOR_SIZE width = 0U; // 完整跳转指令宽度。
    KSW_ACCESSOR_U32 encoded = 0U; // 相对跳转位移的原始编码。
    KSW_ACCESSOR_ADDRESS magnitude = 0U; // 避免有符号溢出的跳转距离绝对值。
    KSW_ACCESSOR_ADDRESS nextAddress = 0U; // 相对跳转指令的下一条指令地址。
    int negative = 0; // 跳转方向，非零表示向较低地址跳转。

    if (position == KSW_ACCESSOR_SIZE_MAX || position >= Count) {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    // 只跟随完整 EB/E9 相对入口跳转，它们不会更改承载对象指针的 RCX。
    if (Bytes[position] == 0xEBU && Count - position >= 2U) {
        width = 2U; // 记录两字节前缀或短跳转的完整宽度。
        encoded = Bytes[position + 1U]; // 取得短跳转的完整 disp8 编码。
        negative = encoded >= 0x80U; // 按 disp8 符号位确定跳转方向。
        magnitude = negative ? 0x100U - encoded : encoded; // 以无符号绝对值计算短跳转距离。
    }
    else if (Bytes[position] == 0xE9U && Count - position >= 5U) {
        width = 5U; // 记录五字节 NOP 或近跳转的完整宽度。
        encoded = KswordARKAccessorReadU32(Bytes + position + 1U); // 取得近跳转的完整 disp32 编码。
        negative = encoded >= 0x80000000U; // 按 disp32 符号位确定跳转方向。
        magnitude = negative ? (KSW_ACCESSOR_ADDRESS)(0U - encoded) : (KSW_ACCESSOR_ADDRESS)encoded; // 避免对最小负数取负造成有符号溢出。
    }
    else {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    // 请求下一跳字节前先排除所有地址加法溢出与减法下溢。
    if (Address > KSW_ACCESSOR_ADDRESS_MAX - position - width) {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    nextAddress = Address + position + width; // 在已完成溢出检查后计算下一指令地址。
    if ((negative && nextAddress < magnitude) ||
        (!negative && nextAddress > KSW_ACCESSOR_ADDRESS_MAX - magnitude)) {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    *TargetOut = negative ? nextAddress - magnitude : nextAddress + magnitude; // 只发布不会回绕的直接入口目标。
    return 1; // 完整访问器或跳转形态已经通过全部所需字节校验。
}

// 用途与参数见头文件；安全逐字节读取正文，并以次数和循环检测约束直接入口跳转。
int
KswordARKResolveAccessorDisplacement(
    KSWORD_ACCESSOR_READ_CODE_FN ReadCode,
    void* Context,
    KSW_ACCESSOR_ADDRESS RoutineAddress,
    KSWORD_ACCESSOR_DISPLACEMENT* DisplacementOut)
{
    KSW_ACCESSOR_ADDRESS visited[KSW_ACCESSOR_THUNK_LIMIT + 1U] = { 0U }; // 已访问的入口地址，用于拒绝循环。
    KSW_ACCESSOR_ADDRESS address = RoutineAddress; // 本轮允许读取的入口地址。
    KSW_ACCESSOR_SIZE hop = 0U; // 已跟随的直接入口跳转次数。

    KswordARKAccessorReset(DisplacementOut); // 先清空旧偏移，所有失败路径保持不可用。
    if (ReadCode == NULL || DisplacementOut == NULL || RoutineAddress == 0U) {
        return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
    }
    for (hop = 0U; hop <= KSW_ACCESSOR_THUNK_LIMIT; ++hop) {
        KSW_ACCESSOR_BYTE bytes[KSW_ACCESSOR_CODE_LIMIT] = { 0U }; // 已完整读到本地的代码字节。
        KSW_ACCESSOR_ADDRESS target = 0U; // 本轮直接跳转解码产生的下一入口地址。
        KSW_ACCESSOR_SIZE index = 0U; // 本轮已完整读取的字节数减一。
        KSW_ACCESSOR_SIZE previous = 0U; // 检查已访问入口时使用的下标。
        int thunkFound = 0; // 本轮是否完整识别到可跟随的直接入口跳转。

        // 即使代码地址属于已验证区段，重复访问入口仍表示循环，必须拒绝。
        for (previous = 0U; previous < hop; ++previous) {
            if (visited[previous] == address) { // 拒绝任何已经访问过的入口。
                return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
            }
        }
        visited[hop] = address; // 登记本轮入口，供后续跳转检测循环。
        // 小步读取仅覆盖完整访问器或入口跳转所需字节，返回之后不跨页补读。
        for (index = 0U; index < KSW_ACCESSOR_CODE_LIMIT; ++index) {
            if (address > KSW_ACCESSOR_ADDRESS_MAX - index ||
                !ReadCode(Context, address + index, &bytes[index])) {
                return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
            }
            if (KswordARKDecodeAccessorDisplacement(
                    bytes, index + 1U, DisplacementOut)) {
                return 1; // 完整访问器或跳转形态已经通过全部所需字节校验。
            }
            if (KswordARKAccessorDecodeThunk(bytes, index + 1U, address, &target)) { // 只在完整直接跳转形态成立时进入下一跳。
                thunkFound = 1; // 记录本轮确证的直接入口跳转。
                break; // 本轮形态已确定，停止继续扫描。
            }
        }
        if (!thunkFound || hop == KSW_ACCESSOR_THUNK_LIMIT || target == 0U) {
            return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
        }
        address = target; // 下一轮仍交给同映像安全读取回调验证目标。
    }
    return 0; // 校验或读取未完整通过，拒绝发布任何偏移。
}
