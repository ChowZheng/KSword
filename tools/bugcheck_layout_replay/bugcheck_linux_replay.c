// Linux 蓝屏离线回放：使用生产布局和真实字模，检查边界并导出可扫码的画面。
#include <ntddk.h>
#include <stdio.h>
#include <string.h>
#include "../../KswordARKDriver/src/features/bugcheck/bugcheck_layout.h"
#include "../../third_party/qrcodegen/qrcodegen.h"
#include "../../KswordARKDriver/src/features/bugcheck/Generated/AsciiFont8x12.h"

// 直接复用生产纯布局源，允许测试保存实际编码报告，与旧回放的编译组织保持一致。
#include "../../KswordARKDriver/src/features/bugcheck/bugcheck_qr_codec.c"
#include "../../KswordARKDriver/src/features/bugcheck/bugcheck_linux.c"

// 像素缓冲为测试专用静态存储，不在函数栈上放大对象；蓝屏生产路径不使用此缓冲。
static UCHAR g_LinuxReplayPixels[1280UL * 960UL * 3UL];
static KSWORD_BUGCHECK_EVIDENCE g_LinuxReplayEvidence; // 扩展夹具不放在用户态或内核栈。
static const KSWORD_BUGCHECK_EVIDENCE* g_LinuxReplayPublished; // 场景可明确模拟没有已发布证据。
static UCHAR g_LinuxReplayCodecBinary[KSWORD_ARK_QR_BINARY_CAPACITY + 1UL]; // 包边界测试缓冲。
static CHAR g_LinuxReplayCodecText[KSWORD_ARK_QR_TEXT_CAPACITY]; // 全字符模式封装边界测试。

// 离线只返回合成快照，不链接证据采集、驱动入口或真实内核模块。
const KSWORD_BUGCHECK_EVIDENCE* KswordARKBugcheckEvidenceSnapshot(VOID)
{
    return g_LinuxReplayPublished;
}
typedef struct _LINUX_REPLAY
{
    ULONG Width; // 本场景宽度。
    ULONG Height; // 本场景高度。
    ULONG TextCount; // 已绘制的非空文本行数。
    ULONG PenguinCount; // 左上角固定列对齐的企鹅行数。
    ULONG QrCount; // 本场景的二维码数量。
    ULONG QrScale; // 实际生产布局选择的每模块像素。
    ULONG QrModules; // 不含静区的实际矩阵边长。
    LONG QrBottom; // 二维码含静区的底边，用于检查文字重叠。
    int Failures; // 绘图回调发现的契约违反数。
} LINUX_REPLAY;

// 写入一个测试像素；输入绝对坐标和灰度颜色，边界检查由外层绘图回调保证。
static void LinuxReplayPixel(ULONG x, ULONG y, UCHAR value, ULONG width)
{
    SIZE_T index = ((SIZE_T)y * width + x) * 3UL; // RGB 三字节像素偏移。
    g_LinuxReplayPixels[index] = value;
    g_LinuxReplayPixels[index + 1UL] = value;
    g_LinuxReplayPixels[index + 2UL] = value;
}

// 复现生产 8x12 字模和9像素字距，同时核验文本位于二维码下方且没有被裁切。
static NTSTATUS LinuxReplayText(PVOID context, LONG x, LONG y, PCSTR text, ULONG color)
{
    LINUX_REPLAY* replay = (LINUX_REPLAY*)context; // 本次场景状态。
    SIZE_T length = strlen(text); // 包含前导空格的整行长度。
    SIZE_T character; // 当前字形索引。
    BOOLEAN penguin = x == 8L && y >= 8L && y < 92L && (y - 8L) % 12L == 0L;
    if (x < 0L || y < 0L || (SIZE_T)x + length * 9UL > replay->Width ||
        (ULONG)y + 12UL > replay->Height || (!penguin && y < replay->QrBottom) ||
        color != KswordArkBugcheckLayoutColorLinuxText) {
        ++replay->Failures;
        return STATUS_INVALID_PARAMETER;
    }
    // Linux 页面只允许固定白色正文；记录行数并按真实字模逐像素生成预览。
    ++replay->TextCount;
    if (penguin) {
        ++replay->PenguinCount; // 只有左上角固定间距位置被接受为企鹅行。
    }
    for (character = 0; character < length; ++character) {
        ULONG row; // 字形中的当前行。
        UCHAR code = (UCHAR)text[character]; // 当前 ASCII 字符。
        if (code < DRIVERGUI_FONT_FIRST || code > DRIVERGUI_FONT_LAST) {
            return STATUS_INVALID_PARAMETER;
        }
        for (row = 0; row < DRIVERGUI_FONT_HEIGHT; ++row) {
            ULONG column; // 字形中的当前列。
            UCHAR bits = g_DriverGuiFont8x12[code - DRIVERGUI_FONT_FIRST][row];
            for (column = 0; column < DRIVERGUI_FONT_WIDTH; ++column) {
                if ((bits & (0x80U >> column)) != 0U) {
                    LinuxReplayPixel((ULONG)x + (ULONG)character * 9UL + column,
                        (ULONG)y + row, 255U, replay->Width);
                }
            }
        }
    }
    return STATUS_SUCCESS;
}

// 绘制 packed 二维码并保留完整四模块静区；检查整数缩放、居中和画布边界。
static NTSTATUS LinuxReplayQr(PVOID context, LONG x, LONG y, ULONG scale, const UCHAR* qr)
{
    LINUX_REPLAY* replay = (LINUX_REPLAY*)context; // 本场景画布。
    ULONG size = (ULONG)qrcodegen_getSize(qr); // 不含静区的矩阵边长。
    ULONG side = (size + 8UL) * scale; // 含四模块静区的像素边长。
    ULONG row; // 当前静区/矩阵行。
    if (x < 0L || y < 0L || scale == 0UL || scale > 8UL ||
        size < 21UL || size > 177UL || (ULONG)x + side > replay->Width ||
        (ULONG)y + side > replay->Height ||
        (ULONG)x != (replay->Width - side) / 2UL ||
        (y < 92L && x < 187L)) {
        ++replay->Failures;
        return STATUS_INVALID_PARAMETER;
    }
    ++replay->QrCount;
    replay->QrScale = scale;
    replay->QrModules = size;
    replay->QrBottom = y + (LONG)side;
    // 蓝色暗模块沿用画布底色，只需绘制白色静区及白色数据单元。
    for (row = 0UL; row < size + 8UL; ++row) {
        ULONG column; // 当前静区/矩阵列。
        for (column = 0UL; column < size + 8UL; ++column) {
            ULONG dy; // 单元内的像素行。
            if (row >= 4UL && row < size + 4UL && column >= 4UL &&
                column < size + 4UL && qrcodegen_getModule(qr,
                    (int)(column - 4UL), (int)(row - 4UL))) {
                continue;
            }
            for (dy = 0UL; dy < scale; ++dy) {
                ULONG dx; // 单元内的像素列。
                for (dx = 0UL; dx < scale; ++dx) {
                    LinuxReplayPixel((ULONG)x + column * scale + dx,
                        (ULONG)y + row * scale + dy, 255U, replay->Width);
                }
            }
        }
    }
    return STATUS_SUCCESS;
}

// 使用调用方提供的诊断快照绘制一个场景；成功时导出 PPM 供独立解码器检验。
static int LinuxReplayScenario(ULONG width, ULONG height,
    const KSWORD_ARK_BUGCHECK_DIAGNOSTICS* diagnostics,
    const KSWORD_ARK_BGP_DUMP_STATE* bgp, const KSWORD_BUGCHECK_EVIDENCE* evidence, PCSTR path)
{
    LINUX_REPLAY replay; // 本场景计数与边界状态。
    KSWORD_ARK_BUGCHECK_LAYOUT_CANVAS canvas; // 生产布局的可替换画布。
    NTSTATUS status; // 布局返回状态。
    SIZE_T pixel; // 用于蓝底初始化的RGB偏移。
    FILE* output = NULL; // 可选预览文件，只在用户态离线工具中访问。
    RtlZeroMemory(&replay, sizeof(replay));
    RtlZeroMemory(&canvas, sizeof(canvas));
    replay.Width = width;
    replay.Height = height;
    for (pixel = 0UL; pixel < (SIZE_T)width * height * 3UL; pixel += 3UL) {
        g_LinuxReplayPixels[pixel] = 0U;
        g_LinuxReplayPixels[pixel + 1UL] = 0U;
        g_LinuxReplayPixels[pixel + 2UL] = 170U;
    }
    // 只提供 Linux 页面实际使用的两个回调，防止误画原面板/Logo/判词。
    canvas.Context = &replay;
    canvas.Width = width;
    canvas.Height = height;
    canvas.RenderMode = KSWORD_ARK_BUGCHECK_RENDER_MODE_LINUX_QR;
    canvas.BgpSnapshot = bgp;
    canvas.DrawText = LinuxReplayText;
    canvas.DrawQr = LinuxReplayQr;
    g_LinuxReplayPublished = evidence; // 明确控制本场景的证据存在位。
    status = KswordARKBugcheckLayoutDraw(&canvas, diagnostics, 15UL, 512UL);
    if (!NT_SUCCESS(status) || replay.Failures != 0 || replay.QrCount != 1UL ||
        replay.TextCount != 10UL || replay.PenguinCount != 7UL) {
        printf("FAIL Linux %lux%lu status=0x%08lX qr=%lu text=%lu bounds=%d\n",
            width, height, (ULONG)status, replay.QrCount, replay.TextCount, replay.Failures);
        return 1;
    }
    // 文件是实际生产布局和字模的离线结果，便于检查完整扫码文本与视觉版式。
    if (path != NULL) {
        if (fopen_s(&output, path, "wb") != 0 || output == NULL) {
            return 1;
        }
        fprintf(output, "P6\n%lu %lu\n255\n", width, height);
        if (fwrite(g_LinuxReplayPixels, 3UL, (SIZE_T)width * height, output) !=
            (SIZE_T)width * height) {
            fclose(output);
            return 1;
        }
        fclose(output);
        {
            CHAR reportPath[256]; // 与预览配对保存真正进入编码器的完整报告。
            sprintf_s(reportPath, sizeof(reportPath), "%s.txt", path);
            if (fopen_s(&output, reportPath, "wb") != 0 || output == NULL) {
                return 1;
            }
            if (fwrite(g_KswordArkLinux.Report, 1UL, g_KswordArkLinux.ReportLength, output) !=
                g_KswordArkLinux.ReportLength) {
                fclose(output);
                return 1;
            }
            fclose(output);
            sprintf_s(reportPath, sizeof(reportPath), "%s.bin", path);
            if (fopen_s(&output, reportPath, "wb") != 0 || output == NULL) {
                return 1;
            }
            if (fwrite(g_KswordArkLinux.Binary, 1UL, g_KswordArkLinux.BinaryLength, output) !=
                g_KswordArkLinux.BinaryLength) {
                fclose(output);
                return 1;
            }
            fclose(output);
        }
    }
    printf("PASS Linux %lux%lu binary=%lu base45=%lu version=%lu modulePixels=%lu\n",
        width, height, g_KswordArkLinux.BinaryLength, g_KswordArkLinux.ReportLength,
        (replay.QrModules - 17UL) / 4UL, replay.QrScale);
    return 0;
}

// 合成正常期环境、三个身份、系统头寄存器及最近操作栈；每个来源和状态都可独立核对。
static void LinuxReplayEvidenceNormal(void)
{
    KSWORD_BUGCHECK_IMAGE_ID* images[3]; // 固定三个映像身份的夹具游标。
    ULONG index; // 固定槽位编号。
    ULONG byte; // GUID 内字节编号。
    RtlZeroMemory(&g_LinuxReplayEvidence, sizeof(g_LinuxReplayEvidence));
    g_LinuxReplayEvidence.Version = KSWORD_BUGCHECK_EVIDENCE_VERSION;
    g_LinuxReplayEvidence.Flags = KSWORD_BUGCHECK_EVIDENCE_VALID | KSWORD_BUGCHECK_EVIDENCE_TRACE_PRESENT;
    g_LinuxReplayEvidence.CaptureTime = 123456789ULL;
    g_LinuxReplayEvidence.CpuGroup = 1UL;
    g_LinuxReplayEvidence.CpuNumber = 7UL;
    g_LinuxReplayEvidence.ThreadId = 0x4321ULL;
    g_LinuxReplayEvidence.ProcessCacheFlags = KSWORD_BUGCHECK_PROCESS_HIT | KSWORD_BUGCHECK_PROCESS_STABLE;
    g_LinuxReplayEvidence.ProcessSampleTime = 123450000ULL;
    g_LinuxReplayEvidence.Environment.Flags = 255UL;
    g_LinuxReplayEvidence.Environment.Major = 10UL;
    g_LinuxReplayEvidence.Environment.Build = 26100UL;
    g_LinuxReplayEvidence.Environment.Ubr = 2605UL;
    g_LinuxReplayEvidence.Environment.ProcessorCount = 16UL;
    g_LinuxReplayEvidence.Environment.SampleTime = 120000000ULL;
    g_LinuxReplayEvidence.Environment.PerformanceFrequency = 10000000ULL;
    g_LinuxReplayEvidence.Environment.CiOptions = 0x401UL;
    g_LinuxReplayEvidence.Environment.SecureBoot = 1UL;
    g_LinuxReplayEvidence.Environment.HypervisorPresent = 1UL;
    strcpy_s(g_LinuxReplayEvidence.Environment.HypervisorVendor,
        sizeof(g_LinuxReplayEvidence.Environment.HypervisorVendor), "Microsoft Hv");
    strcpy_s(g_LinuxReplayEvidence.Environment.DriverBuild,
        sizeof(g_LinuxReplayEvidence.Environment.DriverBuild), "KSword offline fixture 2026-10-05");
    images[0] = &g_LinuxReplayEvidence.Kernel;
    images[1] = &g_LinuxReplayEvidence.Driver;
    images[2] = &g_LinuxReplayEvidence.Candidate;
    for (index = 0UL; index < 3UL; ++index) {
        images[index]->Flags = KSWORD_BUGCHECK_ID_PE | KSWORD_BUGCHECK_ID_RSDS | KSWORD_BUGCHECK_ID_PATH;
        images[index]->Base = 0xFFFFF80000000000ULL + (ULONG64)index * 0x100000ULL;
        images[index]->ImageSize = 0x10000UL + index;
        images[index]->TimeDateStamp = 0x12345678UL + index;
        images[index]->Checksum = 0xABCDEFUL + index;
        images[index]->PdbAge = 2UL + index;
        for (byte = 0UL; byte < 16UL; ++byte) {
            images[index]->PdbGuid[byte] = (UCHAR)(index * 16UL + byte);
        }
        sprintf_s(images[index]->Path, sizeof(images[index]->Path), "\\SystemRoot\\System32\\fixture%lu.sys", index);
        sprintf_s(images[index]->PdbName, sizeof(images[index]->PdbName), "fixture%lu.pdb", index);
    }
    g_LinuxReplayEvidence.Context.ContextSource = 1UL;
    g_LinuxReplayEvidence.Context.RegisterMask = (1UL << KSWORD_BUGCHECK_EVIDENCE_REGISTERS) - 1UL;
    g_LinuxReplayEvidence.Context.StackSource = 4UL;
    g_LinuxReplayEvidence.Context.StackCount = KSWORD_BUGCHECK_EVIDENCE_STACK;
    g_LinuxReplayEvidence.Context.StackThreadId = 0x4321ULL;
    g_LinuxReplayEvidence.Context.StackSampleTime = 123450001ULL;
    for (index = 0UL; index < KSWORD_BUGCHECK_EVIDENCE_REGISTERS; ++index) {
        g_LinuxReplayEvidence.Context.Registers[index] = 0x1122334455660000ULL + index;
    }
    for (index = 0UL; index < KSWORD_BUGCHECK_EVIDENCE_STACK; ++index) {
        g_LinuxReplayEvidence.Context.Stack[index] = 0xFFFFF80400001000ULL + index * 16UL;
    }
    g_LinuxReplayEvidence.EventCount = KSWORD_BUGCHECK_EVIDENCE_EVENTS;
    g_LinuxReplayEvidence.EventsDropped = 9ULL;
    g_LinuxReplayEvidence.EventsOverwritten = 7ULL;
    g_LinuxReplayEvidence.EventsDiscarded = 2ULL;
    for (index = 0UL; index < KSWORD_BUGCHECK_EVIDENCE_EVENTS; ++index) {
        KSWORD_BUGCHECK_EVENT* event = &g_LinuxReplayEvidence.Events[index]; // 当前六槽位之一。
        event->Sequence = 100ULL + index;
        event->Time = 123440000ULL + index;
        event->Kind = index % 5UL + 1UL;
        event->Code = 0x800UL + index;
        event->Status = index == 2UL ? STATUS_NOT_FOUND : STATUS_SUCCESS;
        event->Flags = KSWORD_BUGCHECK_TRACE_EVENT_CONTEXT_VALID;
        event->ProcessId = 644ULL;
        event->ThreadId = 0x4321ULL;
        sprintf_s(event->Text, sizeof(event->Text), "offline event %lu", index);
    }
}

// 容量失败、CRC参考值和 Base45 单码极限独立于屏幕绘图检查。
static int LinuxReplayCodecBounds(const KSWORD_ARK_BUGCHECK_DIAGNOSTICS* diagnostics)
{
    ULONG binaryLength; // 失败时必须被清零。
    ULONG textLength; // 失败时不能发布旧长度。
    NTSTATUS status; // 当前边界返回值。
    KSWORD_QR_WRITER writer; // 仅测试严格文本 schema 长度，不引入额外生产入口。
    CHAR unterminated[64]; // 模拟内置标签超出固定 schema 容量。
    int failures = 0; // 边界测试失败数。
    if (KswordARKBugcheckQrCrc32((const UCHAR*)"123456789", 9UL) != 0xCBF43926UL) {
        ++failures;
    }
    status = KswordARKBugcheckQrEncode(diagnostics, 0UL, 0UL, NULL, NULL,
        g_LinuxReplayCodecBinary, 16UL, g_LinuxReplayCodecText,
        (ULONG)sizeof(g_LinuxReplayCodecText), &binaryLength, &textLength);
    if (status != STATUS_BUFFER_TOO_SMALL || binaryLength != 0UL || textLength != 0UL ||
        g_LinuxReplayCodecText[0] != '\0') {
        ++failures;
    }
    status = KswordARKBugcheckQrEncode(diagnostics, 0UL, 0UL, NULL, NULL,
        g_LinuxReplayCodecBinary, (ULONG)sizeof(g_LinuxReplayCodecBinary),
        g_LinuxReplayCodecText, 1UL, &binaryLength, &textLength);
    if (status != STATUS_BUFFER_TOO_SMALL || binaryLength != 0UL || textLength != 0UL ||
        g_LinuxReplayCodecText[0] != '\0') {
        ++failures;
    }
    memset(unterminated, 'A', sizeof(unterminated));
    writer.Data = g_LinuxReplayCodecBinary;
    writer.Capacity = (ULONG)sizeof(g_LinuxReplayCodecBinary);
    writer.Length = 0UL;
    writer.Status = STATUS_SUCCESS;
    KswordQrString(&writer, unterminated, (ULONG)sizeof(unterminated), TRUE);
    if (writer.Status != STATUS_BUFFER_TOO_SMALL || writer.Length != 0UL) {
        ++failures;
    }
    memset(g_LinuxReplayCodecBinary, 0xFF, sizeof(g_LinuxReplayCodecBinary));
    status = KswordQrBase45(g_LinuxReplayCodecBinary, 2860UL, g_LinuxReplayCodecText,
        (ULONG)sizeof(g_LinuxReplayCodecText), &textLength);
    if (!NT_SUCCESS(status) || textLength != 4295UL) {
        ++failures;
    }
    status = KswordQrBase45(g_LinuxReplayCodecBinary, 2861UL, g_LinuxReplayCodecText,
        (ULONG)sizeof(g_LinuxReplayCodecText), &textLength);
    if (status != STATUS_BUFFER_TOO_SMALL) {
        ++failures;
    }
    printf("KSQ2 codec bounds: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures;
}

// 测试入口覆盖640回落、宽屏、无BGP快照和最大长度快照，绝不访问真实驱动。
int ReplayCheckLinux(void)
{
    KSWORD_ARK_BUGCHECK_DIAGNOSTICS diagnostics; // 本轮可控的崩溃诊断夹具。
    KSWORD_ARK_BGP_DUMP_STATE bgp; // 含完整16条时间线的BGP快照夹具。
    ULONG index; // 时间线/签名填充下标。
    int failures = 0; // 各场景失败数。
    RtlZeroMemory(&diagnostics, sizeof(diagnostics));
    RtlZeroMemory(&bgp, sizeof(bgp));
    failures += LinuxReplayCodecBounds(&diagnostics);
    failures += LinuxReplayScenario(640UL, 480UL, &diagnostics, NULL, NULL,
        "output/bugcheck_linux_empty.ppm");
    LinuxReplayEvidenceNormal();
    diagnostics.Captured = 1L;
    diagnostics.BugCheckCode = 0xD1UL;
    diagnostics.Parameter1 = 0xFFFFF80412345678ULL;
    diagnostics.Parameter4 = 0xFFFFF80487654321ULL;
    diagnostics.ProcessId = 644UL;
    diagnostics.Cpu = 7UL;
    diagnostics.CandidateConfidence = KSWORD_ARK_BUGCHECK_CONFIDENCE_HIGH;
    diagnostics.CandidateModuleBase = 0xFFFFF80487650000ULL;
    diagnostics.CandidateModuleSize = 0x10000UL;
    diagnostics.CandidateAddress = diagnostics.Parameter4;
    strcpy_s(diagnostics.ProcessName, sizeof(diagnostics.ProcessName), "worker.exe");
    strcpy_s(diagnostics.CandidateModule, sizeof(diagnostics.CandidateModule), "example.sys");
    bgp.Version = 2UL;
    bgp.Size = sizeof(bgp);
    bgp.ScreenWidth = 640UL;
    bgp.ScreenHeight = 480UL;
    bgp.ScreenBpp = 32UL;
    failures += LinuxReplayScenario(640UL, 480UL, &diagnostics, &bgp, &g_LinuxReplayEvidence,
        "output/bugcheck_linux_640x480.ppm");
    failures += LinuxReplayScenario(1024UL, 768UL, &diagnostics, &bgp, &g_LinuxReplayEvidence,
        "output/bugcheck_linux_1024x768.ppm");
    failures += LinuxReplayScenario(1280UL, 720UL, &diagnostics, NULL, &g_LinuxReplayEvidence, NULL);
    // 最长合法文本保留结尾NUL；数值以全宽非零值覆盖最坏报告大小，不应截断任何字段。
    memset(&diagnostics, 0xFF, sizeof(diagnostics));
    memset(diagnostics.ProcessName, 'P', sizeof(diagnostics.ProcessName) - 1UL);
    diagnostics.ProcessName[sizeof(diagnostics.ProcessName) - 1UL] = '\0';
    memset(diagnostics.CandidateModule, 'M', sizeof(diagnostics.CandidateModule) - 1UL);
    diagnostics.CandidateModule[sizeof(diagnostics.CandidateModule) - 1UL] = '\0';
    memset(diagnostics.CandidateSource, 'S', sizeof(diagnostics.CandidateSource) - 1UL);
    diagnostics.CandidateSource[sizeof(diagnostics.CandidateSource) - 1UL] = '\0';
    memset(diagnostics.FaultMeaning, 'F', sizeof(diagnostics.FaultMeaning) - 1UL);
    diagnostics.FaultMeaning[sizeof(diagnostics.FaultMeaning) - 1UL] = '\0';
    memset(&bgp, 0xFF, sizeof(bgp));
    bgp.TimelineCount = KSWORD_ARK_BGP_TIMELINE_COUNT;
    for (index = 0UL; index < bgp.TimelineCount; ++index) {
        bgp.Timeline[index].Stage = index;
    }
    failures += LinuxReplayScenario(640UL, 480UL, &diagnostics, &bgp, &g_LinuxReplayEvidence,
        "output/bugcheck_linux_maximum.ppm");
    // 缺失NUL且每个字节都要转义的固定数组仍必须完整编码，不能只测普通ASCII名字。
    memset(diagnostics.ProcessName, 0x80, sizeof(diagnostics.ProcessName));
    memset(diagnostics.CandidateModule, 0x80, sizeof(diagnostics.CandidateModule));
    memset(diagnostics.CandidateSource, 0x80, sizeof(diagnostics.CandidateSource));
    memset(diagnostics.FaultMeaning, 0x80, sizeof(diagnostics.FaultMeaning));
    memset(&g_LinuxReplayEvidence, 0xFF, sizeof(g_LinuxReplayEvidence));
    // 全部固定文本无NUL，全部标量全位置一；计数异常仍不丢失任何固定槽位。
    failures += LinuxReplayScenario(640UL, 480UL, &diagnostics, &bgp, &g_LinuxReplayEvidence,
        "output/bugcheck_linux_escaped.ppm");
    failures += LinuxReplayScenario(1024UL, 768UL, &diagnostics, &bgp, &g_LinuxReplayEvidence,
        "output/bugcheck_linux_full_1024x768.ppm");
    printf("Linux replay: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures;
}
