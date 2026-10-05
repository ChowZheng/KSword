// Linux 风格重绘使用固定非分页工作区、完整 KSQ2 诊断和预先建立的后端绘制资源。
#include "bugcheck_linux.h"
#include "bugcheck_bgp.h"
#include "bugcheck_qr_codec.h"
#include "../../../../third_party/qrcodegen/qrcodegen.h"

#define KSWORD_ARK_LINUX_REPORT_CAPACITY KSWORD_ARK_QR_TEXT_CAPACITY // Base45 完整包及末尾 NUL。
#define KSWORD_ARK_LINUX_MARGIN 16UL // 为屏幕四周保留固定边距。
#define KSWORD_ARK_LINUX_TEXT_HEIGHT 70UL // 为二维码下方三行文字及间距保留完整高度。
#define KSWORD_ARK_LINUX_QR_QUIET_MODULES 4UL // QR 标准要求四模块静区。
#define KSWORD_ARK_LINUX_MAX_MODULE_PIXELS 8UL // 与两个渲染后端的预生成模块一致。
#define KSWORD_ARK_LINUX_GLYPH_ADVANCE 9UL // 复用现有 8x12 字模的九像素间距。
#define KSWORD_ARK_LINUX_GLYPH_HEIGHT 12L // ASCII 企鹅按字模高度逐行绘制。
#define KSWORD_ARK_LINUX_PENGUIN_CHARS 19UL // 原始企鹅最大行宽，整块共用同一左侧坐标。
#define KSWORD_ARK_LINUX_PENGUIN_X 8L // 按参考图把原始企鹅放在画布左上角。
#define KSWORD_ARK_LINUX_PENGUIN_Y 8L // 为左上角企鹅保留八像素顶部边距。
#define KSWORD_ARK_LINUX_PENGUIN_GAP 8UL // 企鹅和居中二维码之间至少留八像素。

typedef struct _KSWORD_ARK_LINUX_WRITER
{
    CHAR* Text; // 指向调用方固定文本缓冲。
    ULONG Capacity; // 包含结尾 NUL 的缓冲总容量。
    ULONG Length; // 已完整写入的有效字节数。
    BOOLEAN Overflow; // 任意写入失败后拒绝输出被截断的报告。
} KSWORD_ARK_LINUX_WRITER;

typedef struct _KSWORD_ARK_LINUX_WORKSPACE
{
    volatile LONG Busy; // 非等待 CAS 防止并发崩溃回调共用编码缓冲。
    CHAR Report[KSWORD_ARK_LINUX_REPORT_CAPACITY]; // 完整报告驻留在非分页静态数据区。
    UCHAR Binary[KSWORD_ARK_QR_BINARY_CAPACITY]; // 逐字段原始包同样不放入崩溃回调栈。
    ULONG BinaryLength; // 只有完整编码后发布的包长度。
    ULONG ReportLength; // 包含 KSQ2 前缀但不含 NUL 的完整字符数。
    UCHAR Temp[qrcodegen_BUFFER_LEN_MAX]; // 3918 字节编码临时缓冲不得放在内核栈上。
    UCHAR Code[qrcodegen_BUFFER_LEN_MAX]; // 3918 字节 packed QR 输出不得放在内核栈上。
    KSWORD_ARK_BUGCHECK_DIAGNOSTICS Diagnostics; // 本次绘制固定读取同一诊断快照。
    KSWORD_ARK_BGP_DUMP_STATE Bgp; // 可选 BGP 状态复制到相同常驻工作区。
    KSWORD_BUGCHECK_EVIDENCE Evidence; // 正常期准备和崩溃采集的完整扩展证据。
} KSWORD_ARK_LINUX_WORKSPACE;

static KSWORD_ARK_LINUX_WORKSPACE g_KswordArkLinux; // 默认 .data 不放入 PAGE 节。
static const CHAR g_KswordArkLinuxHex[] = "0123456789ABCDEF"; // 统一数值和转义表示。
static const PCSTR g_KswordArkLinuxPenguin[] = {
    "     .--.        _", // 原请求中的企鹅及右侧感叹号第一行。
    "    |o_o |      | |", // 保留全部前导和中间空格。
    "    |:_/ |      | |", // 保留用户给出的嘴部图案。
    "   //   \\ \\     |_|", // C 字符串中的双反斜线表示单个原始反斜线。
    "  (|     | )     _", // 保留企鹅身体和感叹号底部。
    " /'\\_   _/`\\    (_)", // 保留原文脚部的引号、反引号和括号。
    " \\___)=(___/" // 保留原文最后一行。
};

// 只写入完整字节并始终保留 NUL；失败后整个报告不进入二维码。
static VOID
KswordArkLinuxAppendByte(
    _Inout_ KSWORD_ARK_LINUX_WRITER* Writer,
    _In_ CHAR Value
    )
{
    if (Writer->Overflow) { // 不继续推进已经溢出的报告。
        return; // 由上层统一返回失败。
    }
    if (Writer->Length >= Writer->Capacity - 1UL) { // 始终为 NUL 保留一字节。
        Writer->Overflow = TRUE; // 明确记录截断风险。
        return; // 不把残缺字段伪装成完整报告。
    }
    Writer->Text[Writer->Length++] = Value; // 将完整的一个字节写入固定缓冲。
    Writer->Text[Writer->Length] = '\0'; // 维护编码器要求的字符串边界。
}

// 本接口只接收本模块及驱动内置的常量字符串，不解引用诊断中的对象指针。
static VOID
KswordArkLinuxAppendLiteral(
    _Inout_ KSWORD_ARK_LINUX_WRITER* Writer,
    _In_z_ PCSTR Text
    )
{
    while (*Text != '\0' && !Writer->Overflow) { // 常量字符串按 NUL 边界有界写入。
        KswordArkLinuxAppendByte(Writer, *Text++); // 写入当前常量字节并推进游标。
    }
}

// 常量和固定本地字符串居中绘制；调用前先证明全部文字适合屏幕。
static NTSTATUS
KswordArkLinuxCenteredText(
    _In_ const KSWORD_ARK_BUGCHECK_LAYOUT_CANVAS* Canvas,
    _In_ LONG Y,
    _In_z_ PCSTR Text
    )
{
    ULONG length = 0UL; // 文本长度受到现有面板行容量限制。
    ULONG pixels; // 当前完整文本所占像素宽度。
    while (length < KSWORD_ARK_BUGCHECK_PANEL_LINE_CHARS && Text[length] != '\0') { // 小字符串有界读。
        ++length; // 统计实际 ASCII 字符数。
    }
    pixels = length * KSWORD_ARK_LINUX_GLYPH_ADVANCE; // 最大 192 字符不会溢出 ULONG。
    if (length == KSWORD_ARK_BUGCHECK_PANEL_LINE_CHARS || pixels > Canvas->Width) { // 不截断长错误码。
        return STATUS_BUFFER_TOO_SMALL; // 将尺寸不足作为绘制失败。
    }
    return Canvas->DrawText(Canvas->Context, (LONG)((Canvas->Width - pixels) / 2UL),
        Y, Text, (ULONG)KswordArkBugcheckLayoutColorLinuxText); // 使用纯白 Linux 字模。
}

// 调用方已取得显示所有权；本路径只使用静态内存和后端准备好的模块矩形。
NTSTATUS
KswordARKBugcheckLinuxDraw(
    _In_ const KSWORD_ARK_BUGCHECK_LAYOUT_CANVAS* Canvas,
    _In_ const KSWORD_ARK_BUGCHECK_DIAGNOSTICS* Diagnostics,
    _In_ ULONG CallbackMask,
    _In_ ULONG ModuleCount
    )
{
    NTSTATUS status; // 保存每个完整操作的状态。
    ULONG maxQrPixels; // 扣除文字和边距后剩余的二维码正方形边长。
    ULONG modulePixels; // 每个二维码模块的整数像素尺寸。
    ULONG qrModules; // packed 矩阵的实际模块边长。
    ULONG qrPixels; // 加入静区后的最终二维码像素尺寸。
    ULONG index; // 数值错误码和企鹅行索引。
    LONG qrX; // 中央二维码的左侧坐标。
    LONG qrY; // 整组内容垂直居中后的二维码顶部。
    LONG textY; // 二维码下方文本的第一行。
    ULONG qrSideMargin; // 左侧企鹅宽度对称限制二维码，保持水平居中且互不重叠。
    CHAR moduleText[KSWORD_ARK_BUGCHECK_MODULE_NAME_CHARS + 1UL]; // 固定模块名副本始终保留 NUL。
    CHAR codeText[KSWORD_ARK_BUGCHECK_PANEL_LINE_CHARS]; // 只存短错误码标题，不承载大报告。
    KSWORD_ARK_LINUX_WRITER codeWriter; // 标题复用同一个无分配写入器。
    const KSWORD_ARK_BGP_DUMP_STATE* bgp = NULL; // 未提供 BGP 快照时不编造字段。
    const KSWORD_BUGCHECK_EVIDENCE* evidence = NULL; // 可选扩展证据保持明确存在标志。
    const KSWORD_BUGCHECK_EVIDENCE* published; // 只接收模块保证常驻的已发布快照。
    if (Canvas == NULL || Diagnostics == NULL || Canvas->DrawQr == NULL ||
        Canvas->DrawText == NULL || Canvas->Width < KSWORD_ARK_BUGCHECK_LAYOUT_REQUIRED_WIDTH ||
        Canvas->Height < KSWORD_ARK_BUGCHECK_LAYOUT_REQUIRED_HEIGHT ||
        Canvas->Width > (ULONG)MAXLONG || Canvas->Height > (ULONG)MAXLONG) { // 所有坐标必须适合 LONG 和最低画布。
        return STATUS_INVALID_PARAMETER; // 清屏前上层应已有相同尺寸门禁。
    }
    if (InterlockedCompareExchange(&g_KswordArkLinux.Busy, 1L, 0L) != 0L) { // 不等待其他处理器或回调。
        return STATUS_DEVICE_BUSY; // 防止共用编码工作区被并发覆盖。
    }
    RtlCopyMemory(&g_KswordArkLinux.Diagnostics, Diagnostics,
        sizeof(g_KswordArkLinux.Diagnostics)); // 本次报告与标题读取同一固定快照。
    if (Canvas->BgpSnapshot != NULL) { // 输入已经由后端采集，不访问任意指针。
        RtlCopyMemory(&g_KswordArkLinux.Bgp, Canvas->BgpSnapshot,
            sizeof(g_KswordArkLinux.Bgp)); // 小型 BGP 快照同样留在静态非分页区。
        bgp = &g_KswordArkLinux.Bgp; // 后续只读取常驻副本。
    }
    published = KswordARKBugcheckEvidenceSnapshot(); // 不访问 BugCheck 参数或其他未验证指针。
    if (published != NULL) { // 有扩展快照时保留全部字段及原始状态。
        RtlCopyMemory(&g_KswordArkLinux.Evidence, published,
            sizeof(g_KswordArkLinux.Evidence)); // 大副本留在固定非分页工作区。
        evidence = &g_KswordArkLinux.Evidence; // 编码过程只读取自己的常驻副本。
    }
    status = KswordARKBugcheckQrEncode(&g_KswordArkLinux.Diagnostics, CallbackMask,
        ModuleCount, bgp, evidence, g_KswordArkLinux.Binary,
        (ULONG)sizeof(g_KswordArkLinux.Binary), g_KswordArkLinux.Report,
        (ULONG)sizeof(g_KswordArkLinux.Report), &g_KswordArkLinux.BinaryLength,
        &g_KswordArkLinux.ReportLength); // 所有诊断和扩展证据必须完整进入同一个包。
    if (!NT_SUCCESS(status)) { // 长度失败不能输出只含部分字段的二维码。
        goto Exit; // 在统一出口释放静态工作区。
    }
    if (!qrcodegen_encodeText(g_KswordArkLinux.Report, g_KswordArkLinux.Temp,
        g_KswordArkLinux.Code, qrcodegen_Ecc_LOW, 1, 40, qrcodegen_Mask_0, false)) { // 固定低纠错和掩码，减少崩溃时评分工作。
        status = STATUS_BUFFER_TOO_SMALL; // 保留完整内容，不以截断缩小二维码。
        goto Exit; // 编码失败时不绘制残缺矩阵。
    }
    qrModules = (ULONG)qrcodegen_getSize(g_KswordArkLinux.Code); // 编码成功后取可靠矩阵尺寸。
    maxQrPixels = Canvas->Height - KSWORD_ARK_LINUX_TEXT_HEIGHT -
        (KSWORD_ARK_LINUX_MARGIN * 2UL); // 为完整底部文字和上下边距保留空间。
    qrSideMargin = (ULONG)KSWORD_ARK_LINUX_PENGUIN_X +
        KSWORD_ARK_LINUX_PENGUIN_CHARS * KSWORD_ARK_LINUX_GLYPH_ADVANCE +
        KSWORD_ARK_LINUX_PENGUIN_GAP; // 十九列企鹅占 171 像素，加原点及间距共 187 像素。
    if (maxQrPixels > Canvas->Width - qrSideMargin * 2UL) { // 水平居中二维码不得覆盖左上企鹅。
        maxQrPixels = Canvas->Width - qrSideMargin * 2UL; // 640 像素宽时仍容纳 266 像素二维码。
    }
    modulePixels = maxQrPixels / (qrModules + KSWORD_ARK_LINUX_QR_QUIET_MODULES * 2UL); // 优先最大整数尺寸。
    if (modulePixels == 0UL) { // 极端尺寸不足时拒绝绘制。
        status = STATUS_BUFFER_TOO_SMALL; // 绝不丢掉标准静区。
        goto Exit; // 保持统一工作区释放路径。
    }
    if (modulePixels > KSWORD_ARK_LINUX_MAX_MODULE_PIXELS) { // 限制在后端预准备模块范围内。
        modulePixels = KSWORD_ARK_LINUX_MAX_MODULE_PIXELS; // 大屏上最多使用八像素模块。
    }
    qrPixels = (qrModules + KSWORD_ARK_LINUX_QR_QUIET_MODULES * 2UL) * modulePixels; // 包含四周静区。
    qrX = (LONG)((Canvas->Width - qrPixels) / 2UL); // 二维码按屏幕水平居中。
    qrY = (LONG)((Canvas->Height - qrPixels - KSWORD_ARK_LINUX_TEXT_HEIGHT) / 2UL); // 整体内容垂直居中。
    textY = qrY + (LONG)qrPixels + 16L; // 文字与二维码之间保留一行以上空白。
    if (g_KswordArkLinux.Diagnostics.CandidateConfidence != KSWORD_ARK_BUGCHECK_CONFIDENCE_NONE &&
        g_KswordArkLinux.Diagnostics.CandidateConfidence <= KSWORD_ARK_BUGCHECK_CONFIDENCE_HIGH &&
        g_KswordArkLinux.Diagnostics.CandidateModuleBase != 0 &&
        g_KswordArkLinux.Diagnostics.CandidateModuleSize != 0 &&
        g_KswordArkLinux.Diagnostics.CandidateAddress >= g_KswordArkLinux.Diagnostics.CandidateModuleBase &&
        g_KswordArkLinux.Diagnostics.CandidateAddress - g_KswordArkLinux.Diagnostics.CandidateModuleBase <
            g_KswordArkLinux.Diagnostics.CandidateModuleSize &&
        g_KswordArkLinux.Diagnostics.CandidateModule[0] != '\0' &&
        g_KswordArkLinux.Diagnostics.CandidateModule[0] != '(') { // 只显示已安全归因到缓存映像范围的模块。
        for (index = 0UL; index < KSWORD_ARK_BUGCHECK_MODULE_NAME_CHARS &&
            g_KswordArkLinux.Diagnostics.CandidateModule[index] != '\0'; ++index) { // 名称读取受缓存数组边界约束。
            UCHAR value = (UCHAR)g_KswordArkLinux.Diagnostics.CandidateModule[index]; // 按无符号字节验证字体范围。
            moduleText[index] = value >= 32U && value <= 126U ? (CHAR)value : '?'; // 字模只显示安全 ASCII，二维码仍保留原值。
        }
        moduleText[index] = '\0'; // 即使源名称填满容量也不会读出边界。
    } else { // 无安全归因证据时不把缓存说明冒充崩溃模块。
        RtlCopyMemory(moduleText, "UNKNOWN MODULE", sizeof("UNKNOWN MODULE")); // 显示简洁的未知模块占位符。
    }
    codeWriter.Text = codeText; // 为短错误码标题绑定栈上固定行缓冲。
    codeWriter.Capacity = (ULONG)sizeof(codeText); // 保留完整标题边界。
    codeWriter.Length = 0UL; // 从空标题开始。
    codeWriter.Overflow = FALSE; // 初始标题无截断。
    codeText[0] = '\0'; // 初始化短标题字符串。
    KswordArkLinuxAppendLiteral(&codeWriter, "0x"); // 数值 Stop Code 始终可见。
    for (index = 0UL; index < 8UL; ++index) { // 蓝屏代码使用固定八位十六进制。
        KswordArkLinuxAppendByte(&codeWriter,
            g_KswordArkLinuxHex[(g_KswordArkLinux.Diagnostics.BugCheckCode >>
            ((7UL - index) * 4UL)) & 15UL]); // 完整保留全部 32 位错误码。
    }
    KswordArkLinuxAppendByte(&codeWriter, ' '); // 分隔数值和原始 Stop Code 名称。
    KswordArkLinuxAppendLiteral(&codeWriter,
        KswordARKBugcheckName(g_KswordArkLinux.Diagnostics.BugCheckCode)); // 只读取驱动常量名称。
    if (codeWriter.Overflow || codeWriter.Length * KSWORD_ARK_LINUX_GLYPH_ADVANCE > Canvas->Width) { // 标题必须完整可见。
        codeText[10] = '\0'; // 超长名称只隐藏名称，八位数值蓝屏代码始终保留。
    }
    status = Canvas->DrawQr(Canvas->Context, qrX, qrY, modulePixels,
        g_KswordArkLinux.Code); // 后端绘制 packed 矩阵和四模块白色静区。
    if (!NT_SUCCESS(status)) { // 私有矩形后端失败必须向上层传播。
        goto Exit; // 不把部分绘制报告为成功。
    }
    status = KswordArkLinuxCenteredText(Canvas, textY, moduleText); // 第一行显示用户最新要求的崩溃模块。
    if (!NT_SUCCESS(status)) { // 保留后端文本绘制失败信息。
        goto Exit; // 统一释放工作区。
    }
    status = KswordArkLinuxCenteredText(Canvas, textY + 18L, codeText); // 第二行显示数值蓝屏代码和名称。
    if (!NT_SUCCESS(status)) { // 错误码不可缺失。
        goto Exit; // 不继续生成不完整页面。
    }
    status = KswordArkLinuxCenteredText(Canvas, textY + 36L, "KSword ARK"); // 第三行使用用户要求的产品名。
    if (!NT_SUCCESS(status)) { // 品牌文本也必须完整绘制。
        goto Exit; // 统一释放工作区。
    }
    for (index = 0UL; index < RTL_NUMBER_OF(g_KswordArkLinuxPenguin); ++index) { // 原始企鹅固定放在参考图左上角。
        status = Canvas->DrawText(Canvas->Context, KSWORD_ARK_LINUX_PENGUIN_X,
            KSWORD_ARK_LINUX_PENGUIN_Y + (LONG)index * KSWORD_ARK_LINUX_GLYPH_HEIGHT,
            g_KswordArkLinuxPenguin[index],
            (ULONG)KswordArkBugcheckLayoutColorLinuxText); // 同一左边界完整保留原始 ASCII 列对齐。
        if (!NT_SUCCESS(status)) { // 任意一行失败即报告真实错误。
            goto Exit; // 统一释放工作区。
        }
    }
Exit:
    InterlockedExchange(&g_KswordArkLinux.Busy, 0L); // 无论成功失败都释放常驻工作区。
    return status; // 只在所有二维码和文字绘制成功后返回成功。
}
