// 映像身份在正常运行期建立，任何候选内核地址都通过有界安全读取。
#include "bugcheck_identity.h" // 引入固定身份合同。
#include "../../platform/runtime_signature_scan.h" // 复用 APC_LEVEL 以下的安全读取。
#include <ntimage.h> // 使用系统 PE 结构而不依赖私有加载器布局。

typedef struct _KSWORD_BUGCHECK_RSDS // RSDS 前缀布局固定为二十四字节。
{
    ULONG Signature; // 必须为 RSDS。
    UCHAR Guid[16]; // 保留原始 GUID 字节。
    ULONG Age; // 保留 PDB Age。
} KSWORD_BUGCHECK_RSDS;

// 映像范围与地址加法均验证后才调用安全读取，不信任 PE 中的 RVA。
static BOOLEAN KswordEvidenceImageRead(ULONG_PTR Base, ULONG Size,
    ULONG Offset, PVOID Output, SIZE_T Bytes)
{
    if (Offset > Size || Bytes > (SIZE_T)(Size - Offset) ||
        Base > MAXULONG_PTR - Size || Bytes == 0U) { // 拒绝越界、加法溢出和零长度读取。
        return FALSE; // 不访问候选地址。
    }
    return KswordARKRuntimeReadMemory((const VOID*)(Base + Offset), Output, Bytes); // 完整读取才成功。
}

// 回调提供的路径在其生命周期内转换为 UTF-8，绝不保留外部字符串指针。
static VOID KswordEvidenceIdentityPath(PCUNICODE_STRING Path, KSWORD_BUGCHECK_IMAGE_ID* Identity)
{
    ULONG source = 0UL; // UTF-16 当前码元。
    ULONG destination = 0UL; // UTF-8 当前字节偏移。
    ULONG characters; // 输入码元数量。
    if (Path == NULL || Path->Buffer == NULL || Path->Length == 0U ||
        (Path->Length & 1U) != 0U || Path->Length > Path->MaximumLength) { // 只读取回调提供的合法长度。
        return; // 缺失路径不伪造身份字段。
    }
    characters = Path->Length / sizeof(WCHAR); // 输入是运行期调用方拥有的固定字符串。
    while (source < characters) { // 每次至少推进一个码元。
        ULONG value = (ULONG)Path->Buffer[source++]; // 读取当前合法 UTF-16 码元。
        UCHAR bytes[4]; // 一个 Unicode 标量最多四个 UTF-8 字节。
        ULONG count; // 当前编码字节数。
        ULONG index; // 当前 UTF-8 字节下标。
        if (value == 0UL) { // 遇到输入 NUL 停止，保留已有路径。
            break; // 不复制潜在尾部垃圾。
        }
        if (value >= 0xD800UL && value <= 0xDBFFUL) { // 高代理项必须与低代理项配对。
            if (source < characters && Path->Buffer[source] >= 0xDC00U && Path->Buffer[source] <= 0xDFFFU) { // 检查低代理项。
                value = 0x10000UL + ((value - 0xD800UL) << 10U) + ((ULONG)Path->Buffer[source++] - 0xDC00UL); // 组合完整标量。
            } else { // 非法代理项使用替代字符。
                value = 0xFFFDUL; // 输出合法 UTF-8，不引入无效编码。
                Identity->Flags |= KSWORD_BUGCHECK_ID_TRUNCATED; // 明确标识不能完整保留原文。
            }
        } else if (value >= 0xDC00UL && value <= 0xDFFFUL) { // 孤立低代理项不能直接编码。
            value = 0xFFFDUL; // 输出替代字符。
            Identity->Flags |= KSWORD_BUGCHECK_ID_TRUNCATED; // 标记原文信息损失。
        }
        if (value < 0x80UL) { // ASCII 使用一个字节。
            count = 1UL; // 当前编码长度。
            bytes[0] = (UCHAR)value; // 保存 ASCII 字节。
        } else if (value < 0x800UL) { // 两字节 UTF-8。
            count = 2UL; // 当前编码长度。
            bytes[0] = (UCHAR)(0xC0UL | (value >> 6U)); // 写入编码头。
            bytes[1] = (UCHAR)(0x80UL | (value & 0x3FUL)); // 写入续字节。
        } else if (value < 0x10000UL) { // 三字节 UTF-8。
            count = 3UL; // 当前编码长度。
            bytes[0] = (UCHAR)(0xE0UL | (value >> 12U)); // 写入编码头。
            bytes[1] = (UCHAR)(0x80UL | ((value >> 6U) & 0x3FUL)); // 写入第一续字节。
            bytes[2] = (UCHAR)(0x80UL | (value & 0x3FUL)); // 写入第二续字节。
        } else { // 已验证的补充平面标量使用四字节。
            count = 4UL; // 当前编码长度。
            bytes[0] = (UCHAR)(0xF0UL | (value >> 18U)); // 写入编码头。
            bytes[1] = (UCHAR)(0x80UL | ((value >> 12U) & 0x3FUL)); // 写入第一续字节。
            bytes[2] = (UCHAR)(0x80UL | ((value >> 6U) & 0x3FUL)); // 写入第二续字节。
            bytes[3] = (UCHAR)(0x80UL | (value & 0x3FUL)); // 写入第三续字节。
        }
        if (count >= KSWORD_BUGCHECK_EVIDENCE_PATH_BYTES - destination) { // 必须保留结尾 NUL，且不截断多字节标量。
            Identity->Flags |= KSWORD_BUGCHECK_ID_TRUNCATED; // 路径容量不足明确标记。
            break; // 保留完整 UTF-8 前缀。
        }
        for (index = 0UL; index < count; ++index) { // 复制当前完整标量。
            Identity->Path[destination++] = (CHAR)bytes[index]; // 写入常驻副本。
        }
    }
    Identity->Path[destination] = '\0'; // 所有路径始终保持 NUL 结尾。
    if (destination != 0UL) { // 只有确实取得路径才发布有效位。
        Identity->Flags |= KSWORD_BUGCHECK_ID_PATH; // 路径可以与 PE 独立成功。
    }
}

// RSDS 文件名按映像声明长度安全读取，目录部分不占用固定 PDB 名称容量。
static NTSTATUS KswordEvidencePdbName(ULONG_PTR Base, ULONG Size,
    ULONG Offset, ULONG Bytes, KSWORD_BUGCHECK_IMAGE_ID* Identity)
{
    ULONG index; // 输入 PDB 路径字节下标。
    ULONG copied = 0UL; // 当前 basename 长度。
    BOOLEAN truncated = FALSE; // 当前 basename 是否超出容量。
    CHAR chunk[64]; // 以固定小块读取名称，避免每个字符一次 MmCopyMemory。
    for (index = 0UL; index < Bytes && index < 1024UL; ++index) { // 输入最多读取一 KiB。
        CHAR value; // 一个文件名原始字节。
        if ((index & 63UL) == 0UL) { // 当前块消耗后才读取下一块。
            ULONG chunkBytes = min(64UL, Bytes - index); // 不越过调试条目声明边界。
            if (!KswordEvidenceImageRead(Base, Size, Offset + index, chunk, chunkBytes)) { // 每个候选地址仍走安全读取。
                return STATUS_PARTIAL_COPY; // 不访问已卸载或不可读的映像页。
            }
        }
        value = chunk[index & 63UL]; // 固定块内偏移不会越界。
        if (value == '\0') { // 发现完整路径结尾。
            Identity->PdbName[copied] = '\0'; // basename 始终有界。
            if (truncated) { // 名称截断仍保留精确 GUID/Age。
                Identity->Flags |= KSWORD_BUGCHECK_ID_TRUNCATED; // 向报告公开文本边界。
            }
            return STATUS_SUCCESS; // PDB 身份与名称读取成功。
        }
        if (value == '\\' || value == '/') { // 遇到目录分隔符重新开始 basename。
            copied = 0UL; // 避免长编译机路径挤掉文件名。
            truncated = FALSE; // 旧目录的截断不代表 basename 截断。
        } else if (copied + 1UL < KSWORD_BUGCHECK_EVIDENCE_PDB_BYTES) { // 保留 NUL 所需一字节。
            Identity->PdbName[copied++] = value; // 保留 PDB 原始字节。
        } else { // 超长 basename 只保留固定前缀。
            truncated = TRUE; // 不把截断名称当成完整名称。
        }
    }
    Identity->PdbName[copied] = '\0'; // 读取失败仍保持合法固定字符串。
    Identity->Flags |= KSWORD_BUGCHECK_ID_TRUNCATED; // 缺失 NUL 或读取预算耗尽必须标记。
    return STATUS_BUFFER_OVERFLOW; // GUID/Age 可用，名称不完整。
}

VOID KswordARKBugcheckIdentityRead(ULONG_PTR Base, ULONG Size,
    PCUNICODE_STRING Path, KSWORD_BUGCHECK_IMAGE_ID* Identity)
{
    IMAGE_DOS_HEADER dos; // 安全读取的 DOS 头副本。
    IMAGE_NT_HEADERS64 nt; // 安全读取的 x64 NT 头副本。
    IMAGE_DATA_DIRECTORY directory; // 调试目录的映像内范围。
    ULONG index; // 有界调试目录项下标。
    if (Identity == NULL) { // 拒绝无效输出指针。
        return; // 不写入未知缓冲。
    }
    RtlZeroMemory(Identity, sizeof(*Identity)); // 失败路径不泄漏旧身份。
    Identity->Base = Base; // 即使解析失败仍保存请求的数值范围。
    Identity->ImageSize = Size; // 保留加载回调给出的映像大小。
    Identity->Status = STATUS_INVALID_IMAGE_FORMAT; // 未通过 PE 校验不能声称有效身份。
    if (KeGetCurrentIrql() > APC_LEVEL) { // 高 IRQL 绝不读取候选地址。
        Identity->Status = STATUS_INVALID_DEVICE_STATE; // 明确记录采集环境不允许。
        return; // 不解析 PE、RSDS 或路径。
    }
    KswordEvidenceIdentityPath(Path, Identity); // 正常回调路径在其有效生命周期内复制。
    if (Base < (ULONG_PTR)MmSystemRangeStart || Size < sizeof(nt) ||
        !KswordEvidenceImageRead(Base, Size, 0UL, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0L ||
        !KswordEvidenceImageRead(Base, Size, (ULONG)dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(nt.OptionalHeader) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage == 0UL || nt.OptionalHeader.SizeOfImage > Size) { // 校验整个头及加载范围。
        return; // 不采用损坏映像的身份数据。
    }
    Identity->ImageSize = nt.OptionalHeader.SizeOfImage; // 使用验证过的 PE 映像大小。
    Identity->TimeDateStamp = nt.FileHeader.TimeDateStamp; // 保存用于符号匹配的编译时间戳。
    Identity->Checksum = nt.OptionalHeader.CheckSum; // 保存原始映像校验和。
    Identity->Flags |= KSWORD_BUGCHECK_ID_PE; // PE 元数据已完成读取。
    Identity->Status = STATUS_NOT_FOUND; // 没有 RSDS 时仍区分 PE 成功与 PDB 缺失。
    if (nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG) { // 映像可以合法缺少调试目录。
        return; // 不编造 PDB 身份。
    }
    directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]; // 调试目录来自已验证头。
    if (directory.VirtualAddress == 0UL || directory.Size == 0UL) { // 没有调试信息。
        return; // 保留 PE 身份及明确缺失状态。
    }
    if (directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) != 0UL ||
        directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY) > 64UL ||
        directory.VirtualAddress > Identity->ImageSize ||
        directory.Size > Identity->ImageSize - directory.VirtualAddress) { // 整张目录必须有界且条目完整。
        Identity->Status = STATUS_INVALID_IMAGE_FORMAT; // 不遍历损坏目录。
        return; // PE 标量仍保留有效位。
    }
    for (index = 0UL; index < directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++index) { // 最多读取六十四项。
        IMAGE_DEBUG_DIRECTORY entry; // 当前调试项固定副本。
        KSWORD_BUGCHECK_RSDS rsds; // 当前 RSDS 固定前缀。
        ULONG offset = directory.VirtualAddress + index * (ULONG)sizeof(entry); // 已验证目录保证加法不溢出。
        if (!KswordEvidenceImageRead(Base, Identity->ImageSize, offset, &entry, sizeof(entry))) { // 页面丢失或卸载安全失败。
            Identity->Status = STATUS_PARTIAL_COPY; // 保留准确的读取失败状态。
            return; // 不回退到直接解引用。
        }
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData < sizeof(rsds)) { // 只读取足够长的 CodeView。
            continue; // 其他合法调试项无须解析。
        }
        if (entry.AddressOfRawData > Identity->ImageSize ||
            entry.SizeOfData > Identity->ImageSize - entry.AddressOfRawData ||
            !KswordEvidenceImageRead(Base, Identity->ImageSize, entry.AddressOfRawData, &rsds, sizeof(rsds))) { // 内存映像必须使用 RVA 而非文件偏移。
            Identity->Status = STATUS_PARTIAL_COPY; // 不使用越界调试数据。
            continue; // 允许后续完整 CodeView 条目提供身份。
        }
        if (rsds.Signature != 0x53445352UL) { // 仅支持 GUID/Age 形式 RSDS。
            continue; // 不把旧 NBxx 格式误当 RSDS。
        }
        RtlCopyMemory(Identity->PdbGuid, rsds.Guid, sizeof(rsds.Guid)); // 复制精确原始 GUID。
        Identity->PdbAge = rsds.Age; // 保留完整 Age，包括合法零值。
        Identity->Flags |= KSWORD_BUGCHECK_ID_RSDS; // GUID/Age 与文件名有效性分离。
        Identity->Status = KswordEvidencePdbName(Base, Identity->ImageSize,
            entry.AddressOfRawData + (ULONG)sizeof(rsds),
            entry.SizeOfData - (ULONG)sizeof(rsds), Identity); // basename 缺失不抹掉精确 RSDS 标识。
        return; // 使用首个通过边界校验的 RSDS。
    }
}
