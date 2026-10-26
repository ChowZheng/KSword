#include "keyboard_rule_ir.h"

/* 所有解析均使用映像RVA和完整读取，不解引用提供者的原始地址。 */
typedef struct KSW_RULE_PE_INFO {
    // PE解析器只保存已核验的可选头、节表和目录边界。
    uint32_t Optional, OptionalBytes, Sections, SectionCount, DirectoryCount;
} KSW_RULE_PE_INFO;

/* 检查减法形式的区间，避免RVA与size相加回绕。 */
int KswRuleRead(const KSW_RULE_IMAGE* image, uint32_t rva, void* output, size_t size)
{
    if (!image || !image->Read || (!output && size) || rva > image->SizeOfImage ||
        size > (size_t)(image->SizeOfImage - rva)) return 0; /* 未验证区间绝不交给宿主。 */
    return size == 0 || image->Read(image->Context, rva, output, size) != 0; /* 只接受完整读取回执。 */
}

/* 从本地字节取小端值，避免非对齐访问和宿主ABI差异。 */
static uint32_t KswPeU32(const unsigned char* p)
{
    // 用逐字节移位组合32位小端字段，不依赖非对齐载入。
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 读取一个完整32位PE字段。 */
static int KswPeRead32(const KSW_RULE_IMAGE* image, uint32_t rva, uint32_t* value)
{
    unsigned char bytes[4]; /* 单字段缓冲不占用大型内核栈。 */
    // 字段读取必须完整成功，才把值写给调用方。
    if (!KswRuleRead(image, rva, bytes, sizeof(bytes))) return 0;
    *value = KswPeU32(bytes); /* 只发布完整字段。 */
    return 1;
}

/* 校验AMD64 PE32+头；版本、时间戳和RSDS均不作为允许条件。 */
static int KswPeOpen(const KSW_RULE_IMAGE* image, KSW_RULE_PE_INFO* pe)
{
    unsigned char dos[64], header[24], optional[112]; /* 固定小头部不包含整张节表。 */
    uint32_t nt, optionalBytes; /* 所有后续偏移仍受SizeOfImage限制。 */
    // DOS签名与固定头部完整存在是PE解析起点。
    if (!KswRuleRead(image, 0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z') return 0;
    nt = KswPeU32(dos + 60); /* e_lfanew是映像内RVA。 */
    // PE签名及AMD64机器类型必须与规则解码器一致。
    if (!KswRuleRead(image, nt, header, sizeof(header)) || KswPeU32(header) != 0x4550U ||
        header[4] != 0x64 || header[5] != 0x86) return 0;
    optionalBytes = (uint32_t)header[20] | ((uint32_t)header[21] << 8); /* 可选头长度来自文件头。 */
    // 可选头需完整容纳PE32+固定字段且SizeOfImage匹配宿主映像。
    if (nt > UINT32_MAX - 24U || optionalBytes < sizeof(optional) ||
        !KswRuleRead(image, nt + 24U, optional, sizeof(optional)) ||
        optional[0] != 0x0b || optional[1] != 0x02 || KswPeU32(optional + 56) != image->SizeOfImage) return 0;
    // 记录实际NT头位置及可选头长度供后续目录定位。
    pe->Optional = nt + 24U; /* 保存目录字段起点。 */
    pe->OptionalBytes = optionalBytes;
    pe->SectionCount = (uint32_t)header[6] | ((uint32_t)header[7] << 8);
    pe->DirectoryCount = KswPeU32(optional + 108);
    // 节数和目录数受可选头及映像长度限制。
    if (pe->SectionCount == 0 || pe->SectionCount > 96U || pe->Optional > image->SizeOfImage ||
        optionalBytes > image->SizeOfImage - pe->Optional ||
        pe->DirectoryCount > (optionalBytes - 112U) / 8U) return 0; /* 目录声明不能越过可选头。 */
    pe->Sections = pe->Optional + optionalBytes; /* 节表紧跟实际长度的可选头。 */
    return pe->Sections <= image->SizeOfImage && pe->SectionCount <= (image->SizeOfImage - pe->Sections) / 40U;
}

/* 取得完整且位于映像内的数据目录。 */
static int KswPeDirectory(const KSW_RULE_IMAGE* image, const KSW_RULE_PE_INFO* pe,
    uint32_t index, uint32_t* rva, uint32_t* size)
{
    unsigned char bytes[8]; /* 单目录由两个DWORD组成。 */
    // 目录索引要同时位于声明数与可选头实际字节范围内。
    if (index >= pe->DirectoryCount || pe->OptionalBytes < 112U ||
        index >= (pe->OptionalBytes - 112U) / 8U ||
        !KswRuleRead(image, pe->Optional + 112U + index * 8U, bytes, sizeof(bytes))) return 0;
    *rva = KswPeU32(bytes); /* 零目录表示此能力不可用。 */
    *size = KswPeU32(bytes + 4);
    // 目录必须非空且完整落在当前映像内。
    return *rva != 0 && *size != 0 && *rva <= image->SizeOfImage && *size <= image->SizeOfImage - *rva;
}

/* 内部接口：取得包含RVA的可执行节终点，供无pdata叶函数限制读取。 */
int KswRuleCodeEnd(const KSW_RULE_IMAGE* image, uint32_t rva, uint32_t* end)
{
    KSW_RULE_PE_INFO pe; /* 仅保留节表位置，不复制全部节。 */
    uint32_t i; /* 节数已由PE头限制。 */
    // 只有合法PE与输出指针才能遍历代码节。
    if (!end || !KswPeOpen(image, &pe)) return 0;
    for (i = 0; i < pe.SectionCount; ++i) {
        unsigned char section[40]; /* 每次仅读取一个节头。 */
        uint32_t begin, size, raw; /* 虚拟范围取VirtualSize和RawSize的较大者。 */
        if (!KswRuleRead(image, pe.Sections + i * 40U, section, sizeof(section))) return 0;
        // 节起点和长度来自实际节表，原始大小大于虚拟大小时扩展覆盖范围。
        begin = KswPeU32(section + 12); size = KswPeU32(section + 8); raw = KswPeU32(section + 16);
        if (raw > size) size = raw;
        // 只有可执行且范围完整的节可提供代码边界。
        if ((KswPeU32(section + 36) & 0x20000000U) == 0 || begin > image->SizeOfImage || size > image->SizeOfImage - begin) continue;
        if (rva >= begin && rva - begin < size) { *end = begin + size; return 1; }
    }
    return 0; /* 不以映像内地址冒充代码地址。 */
}

/* 要求整个指令或函数片段位于同一个可执行节。 */
int KswRuleIsCode(const KSW_RULE_IMAGE* image, uint32_t rva, uint32_t size)
{
    uint32_t end; /* 减法校验使跨节和回绕范围都被拒绝。 */
    // 函数片段不得跨越所属可执行节终点。
    return size != 0 && KswRuleCodeEnd(image, rva, &end) && size <= end - rva;
}

/* 比较有界ANSI名称；DLL允许ASCII大小写不同，导出和导入符号保持区分。 */
static int KswPeName(const KSW_RULE_IMAGE* image, uint32_t rva, const char* wanted, int insensitive)
{
    unsigned char bytes[256]; /* 名称有固定上限，不扫描任意长度字符串。 */
    size_t count = 0, i; /* 读取目标名称长度加终止零即可确认完全相等。 */
    // 待查名称必须在256字节预算内结束。
    if (!wanted) return 0;
    while (count < sizeof(bytes) - 1U && wanted[count]) ++count;
    if (wanted[count] || !KswRuleRead(image, rva, bytes, count + 1U)) return 0;
    for (i = 0; i <= count; ++i) {
        unsigned char a = bytes[i], b = (unsigned char)wanted[i]; /* 不依赖locale或CRT。 */
        // DLL名称按ASCII不区分大小写，符号名保留大小写语义。
        if (insensitive) { if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32); if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32); }
        if (a != b) return 0;
    }
    return 1;
}

/* 按名称解析唯一、非forwarder且可执行的导出RVA。 */
int KswRuleFindExport(const KSW_RULE_IMAGE* image, const char* name, uint32_t* result)
{
    KSW_RULE_PE_INFO pe; /* 目录读取受实际PE范围限制。 */
    unsigned char header[40], ordinal[2]; /* 标准导出目录和单个ordinal。 */
    uint32_t directory, bytes, functions, names, eat, ent, ordinals, i, found = 0;
    // 导出结果先清零，避免失败时残留旧RVA。
    if (!result) return 0;
    *result = 0;
    // 导出目录及完整标准头不合法则停止解析。
    if (!KswPeOpen(image, &pe) || !KswPeDirectory(image, &pe, 0, &directory, &bytes) || bytes < sizeof(header) ||
        !KswRuleRead(image, directory, header, sizeof(header))) return 0;
    functions = KswPeU32(header + 20); names = KswPeU32(header + 24);
    eat = KswPeU32(header + 28); ent = KswPeU32(header + 32); ordinals = KswPeU32(header + 36);
    // 名称、序号和函数表区间分别限制在映像长度内。
    if (functions > image->SizeOfImage / 4U || names > image->SizeOfImage / 4U ||
        eat > image->SizeOfImage || functions > (image->SizeOfImage - eat) / 4U ||
        ent > image->SizeOfImage || names > (image->SizeOfImage - ent) / 4U ||
        ordinals > image->SizeOfImage || names > (image->SizeOfImage - ordinals) / 2U) return 0;
    // 逐项读取名称、ordinal和EAT目标，要求唯一代码入口。
    for (i = 0; i < names; ++i) {
        uint32_t nameRva, index, target; /* ordinal仍须限制在EAT范围内。 */
        if (!KswPeRead32(image, ent + i * 4U, &nameRva)) return 0;
        if (!KswPeName(image, nameRva, name, 0)) continue;
        if (!KswRuleRead(image, ordinals + i * 2U, ordinal, sizeof(ordinal))) return 0;
        index = (uint32_t)ordinal[0] | ((uint32_t)ordinal[1] << 8);
        // 拒绝越界序号、forwarder目录内目标和非代码地址。
        if (index >= functions || !KswPeRead32(image, eat + index * 4U, &target) ||
            (target >= directory && target - directory < bytes) || !KswRuleIsCode(image, target, 1)) return 0;
        if (found && found != target) return 0; /* 同名不同目标是歧义，不取首项。 */
        found = target;
    }
    // 仅在存在合法唯一目标时发布导出RVA。
    *result = found;
    return found != 0;
}

/* 按DLL与符号名称查找原始IAT槽，不把已绑定函数指针当作名字。 */
int KswRuleFindImportSlot(const KSW_RULE_IMAGE* image, const char* dll, const char* name, uint32_t* result)
{
    KSW_RULE_PE_INFO pe; /* 导入目录与查找表均由实际映像提供。 */
    uint32_t directory, bytes, i, found = 0; /* 只有唯一槽位才发布。 */
    // 导入槽输出先清零，再检查真实导入目录。
    if (!result) return 0;
    *result = 0;
    if (!KswPeOpen(image, &pe) || !KswPeDirectory(image, &pe, 1, &directory, &bytes)) return 0;
    // 每个描述符和其INT都限制在显式预算内。
    for (i = 0; i < bytes / 20U && i < 65536U; ++i) {
        unsigned char descriptor[20]; /* 一次读取完整IMAGE_IMPORT_DESCRIPTOR。 */
        uint32_t lookup, moduleName, iat, slot; /* 仅使用保留符号名的OriginalFirstThunk。 */
        if (!KswRuleRead(image, directory + i * 20U, descriptor, sizeof(descriptor))) return 0;
        lookup = KswPeU32(descriptor); moduleName = KswPeU32(descriptor + 12); iat = KswPeU32(descriptor + 16);
        // 完整零描述符结束扫描，已找到的唯一槽才可发布。
        if (!lookup && !moduleName && !iat) { *result = found; return found != 0; }
        if (!KswPeName(image, moduleName, dll, 1)) continue;
        if (!lookup || !iat) return 0; /* 无名字表的bound import不能猜名称。 */
        // 逐个PE32+thunk寻找匹配名称，不能把ordinal当字符串。
        for (slot = 0; slot < 65536U; ++slot) {
            unsigned char thunk[8]; /* PE32+每个thunk固定八字节。 */
            uint32_t low, high, target; /* ordinal导入不等同于所请求的名称。 */
            // 原始名称槽必须在映像内，读取失败不保留部分结果。
            if (lookup > image->SizeOfImage || slot >= (image->SizeOfImage - lookup) / 8U ||
                !KswRuleRead(image, lookup + slot * 8U, thunk, sizeof(thunk))) return 0;
            low = KswPeU32(thunk); high = KswPeU32(thunk + 4);
            if (!low && !high) break;
            if (high & 0x80000000U) continue;
            // Hint/Name读入和IAT地址都须完整位于实际映像。
            if (high || low > UINT32_MAX - 2U || !KswPeName(image, low + 2U, name, 0)) continue;
            if (iat > image->SizeOfImage || slot >= (image->SizeOfImage - iat) / 8U) return 0;
            target = iat + slot * 8U;
            // 同名导入映射到两个不同IAT槽时拒绝歧义。
            if (found && found != target) return 0;
            found = target;
        }
        if (slot == 65536U) return 0; /* 未见终止项属于损坏或预算耗尽。 */
    }
    return 0; /* 导入描述符缺少终止项时不发布部分解析结果。 */
}

/* 读取单个RUNTIME_FUNCTION，零填充项明确跳过。 */
static int KswPeRuntimeEntry(const KSW_RULE_IMAGE* image, uint32_t rva, uint32_t* begin, uint32_t* end, uint32_t* unwind)
{
    unsigned char entry[12]; /* x64异常目录条目与宿主指针大小无关。 */
    // 每条异常记录固定十二字节，完整读取后再解析边界。
    if (!KswRuleRead(image, rva, entry, sizeof(entry))) return 0;
    *begin = KswPeU32(entry); *end = KswPeU32(entry + 4); *unwind = KswPeU32(entry + 8);
    // 零长度记录仅作为填充跳过，不能定义函数范围。
    if (*begin == *end && *begin <= image->SizeOfImage && *unwind < image->SizeOfImage) {
        *begin = 0; *end = 0; *unwind = 0; /* 合法零长度展开记录不承载代码，不作为函数范围或叶函数边界。 */
        return 1;
    }
    // 非填充记录必须有非空有序代码范围和有效unwind位置。
    return (!*begin && !*end && !*unwind) || (*begin < *end && *end <= image->SizeOfImage && *unwind < image->SizeOfImage);
}

/* 展开CHAININFO并限制循环；根代码范围而非unwind元数据地址定义所属函数。 */
static int KswPeUnwindRoot(const KSW_RULE_IMAGE* image, uint32_t* begin, uint32_t* end, uint32_t unwind)
{
    uint32_t seen[16], count = 0; /* 仅64字节的循环保护，不在内核栈保存函数代码。 */
    for (;;) {
        unsigned char header[4]; /* 仅读取版本、标志与槽数。 */
        uint32_t i, flags, tail; /* unwind槽按DWORD对齐后才是链式条目。 */
        // 展开信息要求有效版本、对齐和最大十六层预算。
        if (!unwind || (unwind & 3U) || count == 16U || !KswRuleRead(image, unwind, header, sizeof(header)) ||
            ((header[0] & 7U) != 1U && (header[0] & 7U) != 2U)) return 0;
        // 重复unwind地址意味着链循环，立即拒绝。
        for (i = 0; i < count; ++i) if (seen[i] == unwind) return 0;
        seen[count++] = unwind;
        flags = (uint32_t)header[0] >> 3;
        tail = 4U + (((uint32_t)header[2] + 1U) & ~1U) * 2U;
        // 标志与链尾必须完整有效，未链式时返回当前根范围。
        if (flags & ~7U || unwind > image->SizeOfImage || tail > image->SizeOfImage - unwind) return 0;
        if (!(flags & 4U)) return 1;
        // 链式项继续追到根条目，冲突标志或零根不接受。
        if ((flags & 3U) || !KswPeRuntimeEntry(image, unwind + tail, begin, end, &unwind) || !*begin) return 0;
    }
}

/* 合并合法重复和重叠片段；范围总量仍由16个不相邻块限制。 */
static int KswPeAddRange(const KSW_RULE_IMAGE* image, KSW_RULE_FUNCTION* fn, uint32_t begin, uint32_t end)
{
    uint32_t i; /* 函数片段必须完整属于代码节。 */
    // 函数片段先确认完整处于代码节。
    if (begin >= end || !KswRuleIsCode(image, begin, end - begin)) return 0;
    // 与已收集片段重叠或相接时更新并集边界。
    for (i = 0; i < fn->RangeCount; ++i) {
        if (begin <= fn->Ranges[i].End && end >= fn->Ranges[i].Begin) {
            if (begin < fn->Ranges[i].Begin) fn->Ranges[i].Begin = begin;
            if (end > fn->Ranges[i].End) fn->Ranges[i].End = end;
            return 1;
        }
    }
    // 最多十六个不相邻代码范围，超限拒绝。
    if (fn->RangeCount == 16U) return 0;
    fn->Ranges[fn->RangeCount].Begin = begin; fn->Ranges[fn->RangeCount].End = end; ++fn->RangeCount;
    return 1;
}

/* 内部接口：返回1表示pdata范围，2表示无pdata叶函数候选，0表示损坏或歧义。 */
int KswRuleFunctionRanges(const KSW_RULE_IMAGE* image, uint32_t entry, KSW_RULE_FUNCTION* fn, uint32_t* leafEnd)
{
    KSW_RULE_PE_INFO pe; /* 异常目录只用于边界，不作版本允许名单。 */
    uint32_t directory = 0, bytes = 0, count = 0, i, rootBegin = 0, rootEnd = 0, next = image->SizeOfImage;
    // 入口必须属于代码节，异常目录用于确定真实边界。
    if (!KswPeOpen(image, &pe) || !KswRuleCodeEnd(image, entry, leafEnd)) return 0;
    // 异常目录非零损坏时不能退化到叶函数猜测。
    if (KswPeDirectory(image, &pe, 3, &directory, &bytes)) {
        if (bytes % 12U || bytes / 12U > 262144U) return 0; /* 元数据损坏或超过预算时不能改走叶函数兜底。 */
        count = bytes / 12U;
    } else if (directory || bytes) return 0; /* 非零但越界的异常目录不是合法的无pdata映像。 */
    // 先找唯一根函数及下一函数起点，零填充不参加判断。
    for (i = 0; i < count; ++i) {
        uint32_t begin, end, unwind, b, e; /* 合法全零项不参加排序或包含关系判断。 */
        if (!KswPeRuntimeEntry(image, directory + i * 12U, &begin, &end, &unwind)) return 0;
        if (!begin && !end && !unwind) continue;
        // 入口外条目只可能收窄叶函数终点，不产生归属关系。
        if (begin > entry && begin < next) next = begin;
        if (entry < begin || entry >= end) continue;
        b = begin; e = end;
        // 链式展开后的两个根范围必须一致，否则存在归属歧义。
        if (!KswPeUnwindRoot(image, &b, &e, unwind)) return 0;
        if (rootBegin && (rootBegin != b || rootEnd != e)) return 0;
        rootBegin = b; rootEnd = e;
    }
    // 没有pdata归属时只返回受下一入口限制的叶函数候选。
    if (!rootBegin) { if (next < *leafEnd) *leafEnd = next; return 2; }
    if (!KswPeAddRange(image, fn, rootBegin, rootEnd)) return 0;
    // 已知根之外继续收集归属同根的链式冷代码块。
    for (i = 0; i < count; ++i) {
        uint32_t begin, end, unwind, b, e; /* 收集与根相同的链式冷块。 */
        unsigned char header; /* 非链式无关函数不需要展开。 */
        if (!KswPeRuntimeEntry(image, directory + i * 12U, &begin, &end, &unwind)) return 0;
        if (!begin && !end && !unwind) continue;
        if (begin == rootBegin && end == rootEnd) continue;
        if (!KswRuleRead(image, unwind, &header, 1)) return 0;
        // 非链式无关函数不参与本函数的片段集合。
        if (((uint32_t)header >> 3 & 4U) == 0) continue;
        b = begin; e = end;
        if (!KswPeUnwindRoot(image, &b, &e, unwind)) return 0;
        // 只有展开到同一根的冷块才加入可分析范围。
        if (b == rootBegin && e == rootEnd && !KswPeAddRange(image, fn, begin, end)) return 0;
    }
    return 1;
}
