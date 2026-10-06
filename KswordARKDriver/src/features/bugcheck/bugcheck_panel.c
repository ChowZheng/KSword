/*++

Module Name:

    bugcheck_panel.c

Abstract:

    PASSIVE_LEVEL preparation and crash-time rendering for the physical BGP
    diagnostic panel. The callback path uses only fixed nonpaged buffers and
    rectangles created before the bugcheck occurs.

--*/

#include "bugcheck_internal.h"
#include "bugcheck_bgp.h"
#include "bugcheck_layout.h"
#include "bugcheck_panel.h"
#include "../../platform/pool_compat.h"
#include "../../../../third_party/qrcodegen/qrcodegen.h" // 只读固定二维码矩阵，崩溃路径不分配。

#include "Generated/AsciiFont8x12.h"
#include "Generated/MainLogoBitmap.h"

#define KSWORD_ARK_PANEL_POOL_TAG 'lPgK'
#define KSWORD_ARK_PANEL_BACKGROUND_ARGB 0xFF050F21UL
#define KSWORD_ARK_PANEL_LINUX_BACKGROUND_ARGB 0xFF0000AAUL // Linux 模式的纯蓝不透明画布。
#define KSWORD_ARK_PANEL_QR_MODULE_MAX 8UL // 在正常运行期准备 1 至 8 像素的模块。
#define KSWORD_ARK_PANEL_QR_RUN_COUNT 8UL // 二进制长度 1 至 128 的白色横向段。
#define KSWORD_ARK_PANEL_QR_QUIET_ZONE 4L // QR Model 2 标准四模块静区。
#define KSWORD_ARK_PANEL_GLYPH_ADVANCE 9L
#define KSWORD_ARK_PANEL_GLYPH_BORDER 1UL
#define KSWORD_ARK_PANEL_GLYPH_BITMAP_WIDTH \
    (DRIVERGUI_FONT_WIDTH + (KSWORD_ARK_PANEL_GLYPH_BORDER * 2UL))
#define KSWORD_ARK_PANEL_GLYPH_BITMAP_HEIGHT \
    (DRIVERGUI_FONT_HEIGHT + (KSWORD_ARK_PANEL_GLYPH_BORDER * 2UL))
#define KSWORD_ARK_PANEL_COLOR_COUNT \
    ((ULONG)KswordArkBugcheckLayoutColorCount)
#define KSWORD_ARK_PANEL_BPP24_INDEX 0UL
#define KSWORD_ARK_PANEL_BPP32_INDEX 1UL
#define KSWORD_ARK_PANEL_BPP_VARIANT_COUNT 2UL
#define KSWORD_ARK_PANEL_VERDICT_SET_COUNT 2UL

#pragma pack(push, 1)
typedef struct _KSWORD_ARK_PANEL_BITMAP_FILE_HEADER
{
    USHORT Type;
    ULONG Size;
    USHORT Reserved1;
    USHORT Reserved2;
    ULONG PixelOffset;
} KSWORD_ARK_PANEL_BITMAP_FILE_HEADER, *PKSWORD_ARK_PANEL_BITMAP_FILE_HEADER;

typedef struct _KSWORD_ARK_PANEL_BITMAP_INFO_HEADER
{
    ULONG Size;
    LONG Width;
    LONG Height;
    USHORT Planes;
    USHORT BitsPerPixel;
    ULONG Compression;
    ULONG ImageSize;
    LONG XPelsPerMeter;
    LONG YPelsPerMeter;
    ULONG ColorsUsed;
    ULONG ColorsImportant;
} KSWORD_ARK_PANEL_BITMAP_INFO_HEADER, *PKSWORD_ARK_PANEL_BITMAP_INFO_HEADER;
#pragma pack(pop)

typedef struct _KSWORD_ARK_PANEL_VARIANT
{
    ULONG BitsPerPixel;
    PVOID LogoRectangle;
    PVOID GlyphRectangles[KSWORD_ARK_PANEL_COLOR_COUNT][DRIVERGUI_FONT_COUNT];
    PVOID FrameHorizontalRectangles[KswordArkBugcheckLayoutFrameCount];
    PVOID FrameVerticalRectangles[KswordArkBugcheckLayoutFrameCount];
    // Keep the source BMPs resident for the lifetime of the parsed glyphs.
    // BgpGxParseBitmap is private and its ownership contract is not documented.
    // A persistent nonpaged backing buffer prevents a small-rectangle parser
    // from retaining a reused preparation stack buffer.
    PUCHAR GlyphBitmaps[KSWORD_ARK_PANEL_COLOR_COUNT][DRIVERGUI_FONT_COUNT];
    PUCHAR FrameHorizontalBitmaps[KswordArkBugcheckLayoutFrameCount];
    PUCHAR FrameVerticalBitmaps[KswordArkBugcheckLayoutFrameCount];
    PVOID QrRunRectangles[KSWORD_ARK_PANEL_QR_MODULE_MAX][KSWORD_ARK_PANEL_QR_RUN_COUNT]; // 模块与横向段均提前解析。
    PUCHAR QrRunBitmaps[KSWORD_ARK_PANEL_QR_MODULE_MAX][KSWORD_ARK_PANEL_QR_RUN_COUNT]; // 保持私有解析器输入的非分页生命周期。
} KSWORD_ARK_PANEL_VARIANT, *PKSWORD_ARK_PANEL_VARIANT;

typedef struct _KSWORD_ARK_PANEL_VERDICT_ITEM
{
    PVOID Rectangle;
    PUCHAR BackingBitmap;
    ULONG Width;
    ULONG Height;
} KSWORD_ARK_PANEL_VERDICT_ITEM, *PKSWORD_ARK_PANEL_VERDICT_ITEM;

typedef struct _KSWORD_ARK_PANEL_VERDICT_SET
{
    BOOLEAN Complete;
    KSWORD_ARK_PANEL_VERDICT_ITEM
        Items[KSWORD_ARK_PANEL_BPP_VARIANT_COUNT]
             [KSWORD_ARK_BUGCHECK_VERDICT_LANGUAGE_COUNT]
             [KSWORD_ARK_BUGCHECK_VERDICT_CLASS_COUNT];
} KSWORD_ARK_PANEL_VERDICT_SET, *PKSWORD_ARK_PANEL_VERDICT_SET;

typedef struct _KSWORD_ARK_PANEL_STATE
{
    volatile LONG Ready;
    volatile LONG ActiveVariant;
    volatile LONG ActiveVerdictSet;
    volatile LONG PreferredLanguage;
    KSWORD_ARK_PANEL_VARIANT Variants[KSWORD_ARK_PANEL_BPP_VARIANT_COUNT];
    KSWORD_ARK_PANEL_VERDICT_SET
        VerdictSets[KSWORD_ARK_PANEL_VERDICT_SET_COUNT];
} KSWORD_ARK_PANEL_STATE, *PKSWORD_ARK_PANEL_STATE;

static KSWORD_ARK_PANEL_STATE g_KswordArkPanel;

C_ASSERT(
    KSWORD_ARK_BUGCHECK_VERDICT_CLASS_UNKNOWN ==
    KSWORD_ARK_BUGCHECK_MODULE_UNKNOWN);
C_ASSERT(
    KSWORD_ARK_BUGCHECK_VERDICT_CLASS_OURS ==
    KSWORD_ARK_BUGCHECK_MODULE_OURS);
C_ASSERT(
    KSWORD_ARK_BUGCHECK_VERDICT_CLASS_MICROSOFT ==
    KSWORD_ARK_BUGCHECK_MODULE_MICROSOFT);
C_ASSERT(
    KSWORD_ARK_BUGCHECK_VERDICT_CLASS_THIRD_PARTY ==
    KSWORD_ARK_BUGCHECK_MODULE_THIRD_PARTY);

static NTSTATUS
KswordARKBugcheckPanelInitializeBitmap(
    _Out_writes_bytes_(BitmapCapacity) UCHAR* Bitmap,
    _In_ ULONG BitmapCapacity,
    _In_ ULONG Width,
    _In_ ULONG Height,
    _In_ ULONG BitsPerPixel,
    _Out_ PULONG BitmapLength,
    _Out_ PULONG BitmapStride,
    _Out_ PUCHAR* PixelBytes
    )
{
    PKSWORD_ARK_PANEL_BITMAP_FILE_HEADER fileHeader;
    PKSWORD_ARK_PANEL_BITMAP_INFO_HEADER infoHeader;
    ULONG64 stride;
    ULONG64 imageBytes;
    ULONG64 totalBytes;

    if (Bitmap == NULL ||
        BitmapLength == NULL ||
        BitmapStride == NULL ||
        PixelBytes == NULL ||
        Width == 0 ||
        Height == 0 ||
        (BitsPerPixel != 24UL && BitsPerPixel != 32UL)) {
        return STATUS_INVALID_PARAMETER;
    }

    stride = (((ULONG64)Width * BitsPerPixel + 31ULL) / 32ULL) * 4ULL;
    imageBytes = stride * Height;
    totalBytes =
        sizeof(KSWORD_ARK_PANEL_BITMAP_FILE_HEADER) +
        sizeof(KSWORD_ARK_PANEL_BITMAP_INFO_HEADER) +
        imageBytes;
    if (stride > MAXULONG ||
        imageBytes > MAXULONG ||
        totalBytes > BitmapCapacity ||
        totalBytes > MAXULONG) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(Bitmap, (SIZE_T)totalBytes);
    fileHeader = (PKSWORD_ARK_PANEL_BITMAP_FILE_HEADER)Bitmap;
    infoHeader = (PKSWORD_ARK_PANEL_BITMAP_INFO_HEADER)(Bitmap + sizeof(*fileHeader));
    fileHeader->Type = 0x4D42U;
    fileHeader->Size = (ULONG)totalBytes;
    fileHeader->PixelOffset = sizeof(*fileHeader) + sizeof(*infoHeader);
    infoHeader->Size = sizeof(*infoHeader);
    infoHeader->Width = (LONG)Width;
    infoHeader->Height = (LONG)Height;
    infoHeader->Planes = 1U;
    infoHeader->BitsPerPixel = (USHORT)BitsPerPixel;
    infoHeader->ImageSize = (ULONG)imageBytes;

    *BitmapLength = (ULONG)totalBytes;
    *BitmapStride = (ULONG)stride;
    *PixelBytes = Bitmap + fileHeader->PixelOffset;
    return STATUS_SUCCESS;
}

typedef NTSTATUS
(NTAPI *PKSWORD_ARK_ZW_QUERY_DEFAULT_UI_LANGUAGE)(
    _Out_ PUSHORT DefaultUILanguageId
    );

static ULONG
KswordARKBugcheckPanelQueryPreferredLanguage(
    VOID
    )
{
    UNICODE_STRING routineName;
    PKSWORD_ARK_ZW_QUERY_DEFAULT_UI_LANGUAGE queryLanguage;
    USHORT languageId;

    languageId = 0;
    RtlInitUnicodeString(&routineName, L"ZwQueryDefaultUILanguage");
    queryLanguage = (PKSWORD_ARK_ZW_QUERY_DEFAULT_UI_LANGUAGE)
        MmGetSystemRoutineAddress(&routineName);
    if (queryLanguage != NULL &&
        NT_SUCCESS(queryLanguage(&languageId)) &&
        (languageId & 0x03FFU) == 0x0004U) {
        return KSWORD_ARK_BUGCHECK_VERDICT_LANGUAGE_CHINESE;
    }
    return KSWORD_ARK_BUGCHECK_VERDICT_LANGUAGE_ENGLISH;
}

static VOID
KswordARKBugcheckPanelReleaseVerdictSet(
    _Inout_ PKSWORD_ARK_PANEL_VERDICT_SET VerdictSet
    )
{
    ULONG variantIndex;

    if (VerdictSet == NULL) {
        return;
    }
    VerdictSet->Complete = FALSE;
    for (variantIndex = 0;
         variantIndex < KSWORD_ARK_PANEL_BPP_VARIANT_COUNT;
         ++variantIndex) {
        ULONG language;

        for (language = 0;
             language < KSWORD_ARK_BUGCHECK_VERDICT_LANGUAGE_COUNT;
             ++language) {
            ULONG classification;

            for (classification = 0;
                 classification < KSWORD_ARK_BUGCHECK_VERDICT_CLASS_COUNT;
                 ++classification) {
                PKSWORD_ARK_PANEL_VERDICT_ITEM item;

                item = &VerdictSet->Items[variantIndex]
                    [language][classification];
                KswordARKBugcheckBgpDestroyRectangle(item->Rectangle);
                item->Rectangle = NULL;
                if (item->BackingBitmap != NULL) {
                    ExFreePoolWithTag(
                        item->BackingBitmap,
                        KSWORD_ARK_PANEL_POOL_TAG);
                    item->BackingBitmap = NULL;
                }
                item->Width = 0;
                item->Height = 0;
            }
        }
    }
}

static NTSTATUS
KswordARKBugcheckPanelValidateVerdictPacket(
    _In_reads_bytes_(PacketLength) const VOID* Packet,
    _In_ ULONG PacketLength,
    _Out_ const KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_ENTRY** Entries
    )
{
    const KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_HEADER* header;
    const KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_ENTRY* entries;
    ULONG64 entriesBytes;
    ULONG64 minimumDataOffset;
    ULONG64 totalDataBytes;
    ULONG seenMask;
    ULONG index;

    if (Packet == NULL || Entries == NULL ||
        PacketLength < sizeof(*header)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    *Entries = NULL;
    header = (const KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_HEADER*)Packet;
    entriesBytes =
        (ULONG64)sizeof(KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_ENTRY) *
        KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_COUNT;
    minimumDataOffset = (ULONG64)sizeof(*header) + entriesBytes;
    if (header->version != KSWORD_ARK_BUGCHECK_VERDICT_PROTOCOL_VERSION ||
        header->size != sizeof(*header) ||
        header->magic != KSWORD_ARK_BUGCHECK_VERDICT_MAGIC ||
        header->resourceCount !=
            KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_COUNT ||
        header->entriesOffset != sizeof(*header) ||
        header->totalSize != PacketLength ||
        header->flags != 0 || header->reserved != 0 ||
        minimumDataOffset > PacketLength) {
        return STATUS_INVALID_PARAMETER;
    }

    entries = (const KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_ENTRY*)(
        (const UCHAR*)Packet + header->entriesOffset);
    totalDataBytes = 0;
    seenMask = 0;
    for (index = 0;
         index < KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_COUNT;
         ++index) {
        const KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_ENTRY* entry;
        ULONG64 expectedStride;
        ULONG64 expectedBytes;
        ULONG64 dataEnd;
        ULONG bitIndex;
        ULONG bit;

        entry = &entries[index];
        expectedStride = (ULONG64)entry->width * 4ULL;
        expectedBytes = expectedStride * entry->height;
        dataEnd = (ULONG64)entry->dataOffset + entry->dataLength;
        if (entry->language >=
                KSWORD_ARK_BUGCHECK_VERDICT_LANGUAGE_COUNT ||
            entry->classification >=
                KSWORD_ARK_BUGCHECK_VERDICT_CLASS_COUNT ||
            entry->width == 0 || entry->height == 0 ||
            entry->width > KSWORD_ARK_BUGCHECK_VERDICT_MAX_WIDTH ||
            entry->height > KSWORD_ARK_BUGCHECK_VERDICT_MAX_HEIGHT ||
            entry->format != KSWORD_ARK_BUGCHECK_VERDICT_FORMAT_BGRA32 ||
            expectedStride != entry->stride ||
            expectedBytes == 0 || expectedBytes != entry->dataLength ||
            entry->dataOffset < minimumDataOffset ||
            dataEnd > PacketLength) {
            return STATUS_INVALID_PARAMETER;
        }

        bitIndex = entry->language *
            KSWORD_ARK_BUGCHECK_VERDICT_CLASS_COUNT +
            entry->classification;
        bit = 1UL << bitIndex;
        if ((seenMask & bit) != 0) {
            return STATUS_INVALID_PARAMETER;
        }
        seenMask |= bit;
        totalDataBytes += entry->dataLength;
        if (totalDataBytes >
            KSWORD_ARK_BUGCHECK_VERDICT_MAX_DATA_BYTES) {
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (seenMask !=
        ((1UL << KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_COUNT) - 1UL)) {
        return STATUS_INVALID_PARAMETER;
    }
    *Entries = entries;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKBugcheckPanelPrepareVerdictItem(
    _In_ ULONG BitsPerPixel,
    _In_ const KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_ENTRY* Entry,
    _In_reads_bytes_(Entry->dataLength) const UCHAR* SourcePixels,
    _Out_ PKSWORD_ARK_PANEL_VERDICT_ITEM Item
    )
{
    PUCHAR bitmap;
    PUCHAR pixels;
    ULONG64 bitmapCapacity64;
    ULONG bitmapCapacity;
    ULONG bitmapLength;
    ULONG bitmapStride;
    ULONG bytesPerPixel;
    ULONG y;
    NTSTATUS status;

    if (Entry == NULL || SourcePixels == NULL || Item == NULL ||
        (BitsPerPixel != 24UL && BitsPerPixel != 32UL)) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Item, sizeof(*Item));
    bitmapCapacity64 =
        sizeof(KSWORD_ARK_PANEL_BITMAP_FILE_HEADER) +
        sizeof(KSWORD_ARK_PANEL_BITMAP_INFO_HEADER) +
        ((((ULONG64)Entry->width * BitsPerPixel + 31ULL) / 32ULL) *
         4ULL * Entry->height);
    if (bitmapCapacity64 > MAXULONG) {
        return STATUS_INTEGER_OVERFLOW;
    }
    bitmapCapacity = (ULONG)bitmapCapacity64;
    bitmap = (PUCHAR)KswordARKAllocateNonPagedPool(
        bitmapCapacity,
        KSWORD_ARK_PANEL_POOL_TAG);
    if (bitmap == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pixels = NULL;
    status = KswordARKBugcheckPanelInitializeBitmap(
        bitmap,
        bitmapCapacity,
        Entry->width,
        Entry->height,
        BitsPerPixel,
        &bitmapLength,
        &bitmapStride,
        &pixels);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(bitmap, KSWORD_ARK_PANEL_POOL_TAG);
        return status;
    }

    bytesPerPixel = BitsPerPixel / 8UL;
    for (y = 0; y < Entry->height; ++y) {
        const UCHAR* sourceRow;
        PUCHAR destinationRow;
        ULONG x;

        sourceRow = SourcePixels + ((SIZE_T)y * Entry->stride);
        destinationRow = pixels +
            ((SIZE_T)(Entry->height - 1UL - y) * bitmapStride);
        for (x = 0; x < Entry->width; ++x) {
            const UCHAR* sourcePixel;
            PUCHAR destinationPixel;
            ULONG alpha;

            sourcePixel = sourceRow + ((SIZE_T)x * 4UL);
            destinationPixel = destinationRow +
                ((SIZE_T)x * bytesPerPixel);
            alpha = sourcePixel[3];
            destinationPixel[0] = (UCHAR)(
                (sourcePixel[0] * alpha +
                 KSWORD_ARK_BUGCHECK_LAYOUT_BACKGROUND_BLUE *
                    (255UL - alpha)) / 255UL);
            destinationPixel[1] = (UCHAR)(
                (sourcePixel[1] * alpha +
                 KSWORD_ARK_BUGCHECK_LAYOUT_BACKGROUND_GREEN *
                    (255UL - alpha)) / 255UL);
            destinationPixel[2] = (UCHAR)(
                (sourcePixel[2] * alpha +
                 KSWORD_ARK_BUGCHECK_LAYOUT_BACKGROUND_RED *
                    (255UL - alpha)) / 255UL);
            if (bytesPerPixel == 4UL) {
                destinationPixel[3] = 0xFFU;
            }
        }
    }

    status = KswordARKBugcheckBgpParseBitmap(
        bitmap,
        bitmapLength,
        &Item->Rectangle);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(bitmap, KSWORD_ARK_PANEL_POOL_TAG);
        return status;
    }
    Item->BackingBitmap = bitmap;
    Item->Width = Entry->width;
    Item->Height = Entry->height;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKBugcheckPanelInstallVerdictResources(
    _In_reads_bytes_(PacketLength) const VOID* Packet,
    _In_ ULONG PacketLength
    )
{
    const KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_ENTRY* entries;
    PKSWORD_ARK_PANEL_VERDICT_SET verdictSet;
    LONG activeSet;
    ULONG stagingSet;
    ULONG index;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (InterlockedCompareExchange(&g_KswordArkPanel.Ready, 0, 0) == 0) {
        return STATUS_DEVICE_NOT_READY;
    }
    status = KswordARKBugcheckPanelValidateVerdictPacket(
        Packet,
        PacketLength,
        &entries);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = KswordARKBugcheckBgpBeginResourceUpdate();
    if (!NT_SUCCESS(status)) {
        return status;
    }

    activeSet = InterlockedCompareExchange(
        &g_KswordArkPanel.ActiveVerdictSet,
        0,
        0);
    stagingSet = activeSet == 0 ? 1UL : 0UL;
    verdictSet = &g_KswordArkPanel.VerdictSets[stagingSet];
    KswordARKBugcheckPanelReleaseVerdictSet(verdictSet);
    status = STATUS_SUCCESS;
    for (index = 0;
         index < KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_COUNT &&
             NT_SUCCESS(status);
         ++index) {
        const KSWORD_ARK_BUGCHECK_VERDICT_RESOURCE_ENTRY* entry;
        ULONG variantIndex;

        entry = &entries[index];
        for (variantIndex = 0;
             variantIndex < KSWORD_ARK_PANEL_BPP_VARIANT_COUNT &&
                 NT_SUCCESS(status);
             ++variantIndex) {
            ULONG bitsPerPixel;

            bitsPerPixel = variantIndex == KSWORD_ARK_PANEL_BPP24_INDEX
                ? 24UL
                : 32UL;
            status = KswordARKBugcheckPanelPrepareVerdictItem(
                bitsPerPixel,
                entry,
                (const UCHAR*)Packet + entry->dataOffset,
                &verdictSet->Items[variantIndex]
                    [entry->language][entry->classification]);
        }
    }

    if (NT_SUCCESS(status)) {
        verdictSet->Complete = TRUE;
        KeMemoryBarrier();
        InterlockedExchange(
            &g_KswordArkPanel.ActiveVerdictSet,
            (LONG)stagingSet);
    } else {
        KswordARKBugcheckPanelReleaseVerdictSet(verdictSet);
    }
    KswordARKBugcheckBgpEndResourceUpdate();
    return status;
}

static NTSTATUS
KswordARKBugcheckPanelPrepareLogoRectangle(
    _In_ ULONG BitsPerPixel,
    _In_ ULONG Width,
    _In_ ULONG Height,
    _Out_ PVOID* Rectangle
    )
{
    UCHAR* bitmap;
    PUCHAR pixels;
    ULONG bitmapLength;
    ULONG bitmapStride;
    ULONG bytesPerPixel;
    ULONG64 bitmapCapacity64;
    ULONG bitmapCapacity;
    ULONG destinationY;
    NTSTATUS status;

    if (Rectangle == NULL ||
        Width == 0 ||
        Height == 0 ||
        (BitsPerPixel != 24UL && BitsPerPixel != 32UL)) {
        return STATUS_INVALID_PARAMETER;
    }
    *Rectangle = NULL;

    bytesPerPixel = BitsPerPixel / 8UL;
    bitmapCapacity64 =
        sizeof(KSWORD_ARK_PANEL_BITMAP_FILE_HEADER) +
        sizeof(KSWORD_ARK_PANEL_BITMAP_INFO_HEADER) +
        ((((ULONG64)Width *
           BitsPerPixel + 31ULL) / 32ULL) * 4ULL) *
            Height;
    if (bitmapCapacity64 > MAXULONG) {
        return STATUS_INTEGER_OVERFLOW;
    }

    bitmapCapacity = (ULONG)bitmapCapacity64;
    bitmap = (UCHAR*)KswordARKAllocateNonPagedPool(
        bitmapCapacity,
        KSWORD_ARK_PANEL_POOL_TAG);
    if (bitmap == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pixels = NULL;
    status = KswordARKBugcheckPanelInitializeBitmap(
        bitmap,
        bitmapCapacity,
        Width,
        Height,
        BitsPerPixel,
        &bitmapLength,
        &bitmapStride,
        &pixels);
    if (NT_SUCCESS(status)) {
        for (destinationY = 0;
             destinationY < Height;
             ++destinationY) {
            ULONG destinationX;
            ULONG sourceY;
            PUCHAR destinationRow;
            const UCHAR* sourceRow;

            sourceY = (destinationY * DRIVERGUI_MAINLOGO_HEIGHT) / Height;
            destinationRow =
                pixels +
                ((SIZE_T)(Height - 1UL - destinationY) *
                 bitmapStride);
            sourceRow =
                g_DriverGuiMainLogoBgra +
                 ((SIZE_T)sourceY * DRIVERGUI_MAINLOGO_STRIDE);
            for (destinationX = 0;
                 destinationX < Width;
                 ++destinationX) {
                ULONG alpha;
                ULONG sourceBlue;
                ULONG sourceGreen;
                ULONG sourceRed;
                ULONG sourceX;
                PUCHAR destinationPixel;
                const UCHAR* sourcePixel;

                sourceX = (destinationX * DRIVERGUI_MAINLOGO_WIDTH) / Width;
                destinationPixel =
                    destinationRow + ((SIZE_T)destinationX * bytesPerPixel);
                sourcePixel = sourceRow + ((SIZE_T)sourceX * 4UL);
                sourceBlue = sourcePixel[0];
                sourceGreen = sourcePixel[1];
                sourceRed = sourcePixel[2];
                alpha = sourcePixel[3];

                // Keep the KSwordDEV artwork legible on the dark crash canvas.
                if (alpha != 0 && sourceRed < 72UL &&
                    sourceGreen < 72UL && sourceBlue < 72UL) {
                    sourceRed = KSWORD_ARK_BUGCHECK_LAYOUT_TEXT_RED;
                    sourceGreen = KSWORD_ARK_BUGCHECK_LAYOUT_TEXT_GREEN;
                    sourceBlue = KSWORD_ARK_BUGCHECK_LAYOUT_TEXT_BLUE;
                }
                destinationPixel[0] = (UCHAR)(
                    (sourceBlue * alpha +
                     KSWORD_ARK_BUGCHECK_LAYOUT_BACKGROUND_BLUE *
                         (255UL - alpha)) / 255UL);
                destinationPixel[1] = (UCHAR)(
                    (sourceGreen * alpha +
                     KSWORD_ARK_BUGCHECK_LAYOUT_BACKGROUND_GREEN *
                         (255UL - alpha)) / 255UL);
                destinationPixel[2] = (UCHAR)(
                    (sourceRed * alpha +
                     KSWORD_ARK_BUGCHECK_LAYOUT_BACKGROUND_RED *
                         (255UL - alpha)) / 255UL);
                if (bytesPerPixel == 4UL) {
                    destinationPixel[3] = 0xFFU;
                }
            }
        }

        status = KswordARKBugcheckBgpParseBitmap(
            bitmap,
            bitmapLength,
            Rectangle);
    }

    ExFreePoolWithTag(bitmap, KSWORD_ARK_PANEL_POOL_TAG);
    return status;
}

static NTSTATUS
KswordARKBugcheckPanelPrepareLogos(
    _In_ ULONG VariantIndex,
    _In_ ULONG BitsPerPixel
    )
{
    NTSTATUS status;

    if (VariantIndex >= KSWORD_ARK_PANEL_BPP_VARIANT_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKBugcheckPanelPrepareLogoRectangle(
        BitsPerPixel,
        KSWORD_ARK_BUGCHECK_LAYOUT_LOGO_WIDTH,
        KSWORD_ARK_BUGCHECK_LAYOUT_LOGO_HEIGHT,
        &g_KswordArkPanel.Variants[VariantIndex].LogoRectangle);
    if (!NT_SUCCESS(status)) {
        KswordARKBugcheckBgpDestroyRectangle(
            g_KswordArkPanel.Variants[VariantIndex].LogoRectangle);
        g_KswordArkPanel.Variants[VariantIndex].LogoRectangle = NULL;
    }
    return status;
}

static VOID
KswordARKBugcheckPanelWriteGlyphPixel(
    _Out_writes_bytes_(BytesPerPixel) PUCHAR Pixel,
    _In_ ULONG BytesPerPixel,
    _In_ ULONG ColorIndex,
    _In_ BOOLEAN Foreground
    )
{
    if (!Foreground) {
        // BGP's parsed 32-bit rectangles are rendered as opaque pixels.
        // Match every padded glyph cell to the dark crash canvas.
        Pixel[0] = (UCHAR)(ColorIndex == KswordArkBugcheckLayoutColorLinuxText
            ? 170U : KSWORD_ARK_BUGCHECK_LAYOUT_BACKGROUND_BLUE); // Linux 字形背景与纯蓝画布一致。
        Pixel[1] = (UCHAR)(ColorIndex == KswordArkBugcheckLayoutColorLinuxText
            ? 0U : KSWORD_ARK_BUGCHECK_LAYOUT_BACKGROUND_GREEN); // 不依赖私有 BGP 的 alpha 混合。
        Pixel[2] = (UCHAR)(ColorIndex == KswordArkBugcheckLayoutColorLinuxText
            ? 0U : KSWORD_ARK_BUGCHECK_LAYOUT_BACKGROUND_RED); // 留白和边距均保持蓝底。
        if (BytesPerPixel == 4UL) {
            // BGP requires opaque pixels for reliable 32-bit glyph rendering.
            Pixel[3] = 0xFFU;
        }
        return;
    }

    if (ColorIndex == KswordArkBugcheckLayoutColorLinuxText) { // 新模式的正文与企鹅均使用纯白。
        Pixel[0] = 255U; // 蓝色通道。
        Pixel[1] = 255U; // 绿色通道。
        Pixel[2] = 255U; // 红色通道。
    } else if (ColorIndex == KswordArkBugcheckLayoutColorAccent) {
        Pixel[0] = KSWORD_ARK_BUGCHECK_LAYOUT_ACCENT_BLUE;
        Pixel[1] = KSWORD_ARK_BUGCHECK_LAYOUT_ACCENT_GREEN;
        Pixel[2] = KSWORD_ARK_BUGCHECK_LAYOUT_ACCENT_RED;
    } else if (ColorIndex == KswordArkBugcheckLayoutColorWarning) {
        Pixel[0] = KSWORD_ARK_BUGCHECK_LAYOUT_WARNING_BLUE;
        Pixel[1] = KSWORD_ARK_BUGCHECK_LAYOUT_WARNING_GREEN;
        Pixel[2] = KSWORD_ARK_BUGCHECK_LAYOUT_WARNING_RED;
    } else if (ColorIndex == KswordArkBugcheckLayoutColorMuted) {
        Pixel[0] = KSWORD_ARK_BUGCHECK_LAYOUT_MUTED_BLUE;
        Pixel[1] = KSWORD_ARK_BUGCHECK_LAYOUT_MUTED_GREEN;
        Pixel[2] = KSWORD_ARK_BUGCHECK_LAYOUT_MUTED_RED;
    } else {
        Pixel[0] = KSWORD_ARK_BUGCHECK_LAYOUT_TEXT_BLUE;
        Pixel[1] = KSWORD_ARK_BUGCHECK_LAYOUT_TEXT_GREEN;
        Pixel[2] = KSWORD_ARK_BUGCHECK_LAYOUT_TEXT_RED;
    }
    if (BytesPerPixel == 4UL) {
        Pixel[3] = 0xFFU;
    }
}

static NTSTATUS
KswordARKBugcheckPanelPrepareGlyph(
    _In_ ULONG VariantIndex,
    _In_ ULONG BitsPerPixel,
    _In_ ULONG ColorIndex,
    _In_ ULONG GlyphIndex
    )
{
    PUCHAR bitmap;
    PUCHAR pixels;
    ULONG bitmapLength;
    ULONG bitmapStride;
    ULONG bytesPerPixel;
    ULONG bitmapRowIndex;
    ULONG bitmapColumnIndex;
    ULONG rowIndex;
    PVOID* rectangle;
    NTSTATUS status;

    if (VariantIndex >= KSWORD_ARK_PANEL_BPP_VARIANT_COUNT ||
        (BitsPerPixel != 24UL && BitsPerPixel != 32UL)) {
        return STATUS_INVALID_PARAMETER;
    }

    bitmap = (PUCHAR)KswordARKAllocateNonPagedPool(
        1024UL,
        KSWORD_ARK_PANEL_POOL_TAG);
    if (bitmap == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pixels = NULL;
    status = KswordARKBugcheckPanelInitializeBitmap(
        bitmap,
        1024UL,
        KSWORD_ARK_PANEL_GLYPH_BITMAP_WIDTH,
        KSWORD_ARK_PANEL_GLYPH_BITMAP_HEIGHT,
        BitsPerPixel,
        &bitmapLength,
        &bitmapStride,
        &pixels);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(bitmap, KSWORD_ARK_PANEL_POOL_TAG);
        return status;
    }

    bytesPerPixel = BitsPerPixel / 8UL;
    // Paint the complete padded cell before overlaying foreground bits.
    for (bitmapRowIndex = 0;
         bitmapRowIndex < KSWORD_ARK_PANEL_GLYPH_BITMAP_HEIGHT;
         ++bitmapRowIndex) {
        PUCHAR destinationRow;

        destinationRow =
            pixels +
            ((SIZE_T)(KSWORD_ARK_PANEL_GLYPH_BITMAP_HEIGHT -
                      1UL - bitmapRowIndex) * bitmapStride);
        for (bitmapColumnIndex = 0;
             bitmapColumnIndex < KSWORD_ARK_PANEL_GLYPH_BITMAP_WIDTH;
             ++bitmapColumnIndex) {
            KswordARKBugcheckPanelWriteGlyphPixel(
                destinationRow +
                    ((SIZE_T)bitmapColumnIndex * bytesPerPixel),
                bytesPerPixel,
                ColorIndex,
                FALSE);
        }
    }
    for (rowIndex = 0;
         rowIndex < DRIVERGUI_FONT_HEIGHT;
         ++rowIndex) {
        ULONG columnIndex;
        UCHAR rowBits;
        PUCHAR destinationRow;

        rowBits = g_DriverGuiFont8x12[GlyphIndex][rowIndex];
        destinationRow =
            pixels +
            ((SIZE_T)(KSWORD_ARK_PANEL_GLYPH_BITMAP_HEIGHT -
                      1UL - KSWORD_ARK_PANEL_GLYPH_BORDER - rowIndex) *
             bitmapStride);
        for (columnIndex = 0;
             columnIndex < DRIVERGUI_FONT_WIDTH;
             ++columnIndex) {
            BOOLEAN foreground;

            foreground =
                (rowBits & (UCHAR)(1U << (7UL - columnIndex))) != 0;
            KswordARKBugcheckPanelWriteGlyphPixel(
                destinationRow +
                    ((SIZE_T)(KSWORD_ARK_PANEL_GLYPH_BORDER + columnIndex) *
                     bytesPerPixel),
                bytesPerPixel,
                ColorIndex,
                foreground);
        }
    }

    rectangle = &g_KswordArkPanel.Variants[VariantIndex]
        .GlyphRectangles[ColorIndex][GlyphIndex];
    status = KswordARKBugcheckBgpParseBitmap(
        bitmap,
        bitmapLength,
        rectangle);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(bitmap, KSWORD_ARK_PANEL_POOL_TAG);
        return status;
    }

    g_KswordArkPanel.Variants[VariantIndex]
        .GlyphBitmaps[ColorIndex][GlyphIndex] = bitmap;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKBugcheckPanelPrepareGlyphs(
    _In_ ULONG VariantIndex,
    _In_ ULONG BitsPerPixel
    )
{
    ULONG colorIndex;

    for (colorIndex = 0;
         colorIndex < KSWORD_ARK_PANEL_COLOR_COUNT;
         ++colorIndex) {
        ULONG glyphIndex;

        for (glyphIndex = 0;
             glyphIndex < DRIVERGUI_FONT_COUNT;
             ++glyphIndex) {
            NTSTATUS status;

            status = KswordARKBugcheckControlCheckAbort(); // 增加的 Linux 字形也必须遵守准备取消和时间预算。
            if (!NT_SUCCESS(status)) { // 不在准备超时后继续解析字形。
                return status; // 已准备资源仍由统一关闭路径回收。
            }
            status = KswordARKBugcheckPanelPrepareGlyph(
                VariantIndex,
                BitsPerPixel,
                colorIndex,
                glyphIndex);
            if (!NT_SUCCESS(status)) {
                return status;
            }
        }
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKBugcheckPanelPrepareSolidRectangle(
    _In_ ULONG BitsPerPixel,
    _In_ ULONG Width,
    _In_ ULONG Height,
    _In_ BOOLEAN White, // 白色二维码段和原诊断边框共用安全准备流程。
    _Out_ PVOID* Rectangle,
    _Out_ PUCHAR* BackingBitmap
    )
{
    PUCHAR bitmap;
    PUCHAR pixels;
    ULONG64 bitmapCapacity64;
    ULONG bitmapCapacity;
    ULONG bitmapLength;
    ULONG bitmapStride;
    ULONG bytesPerPixel;
    ULONG x;
    ULONG y;
    NTSTATUS status;

    if (Rectangle == NULL || BackingBitmap == NULL ||
        Width == 0 || Height == 0 ||
        (BitsPerPixel != 24UL && BitsPerPixel != 32UL)) {
        return STATUS_INVALID_PARAMETER;
    }
    *Rectangle = NULL;
    *BackingBitmap = NULL;

    bitmapCapacity64 =
        sizeof(KSWORD_ARK_PANEL_BITMAP_FILE_HEADER) +
        sizeof(KSWORD_ARK_PANEL_BITMAP_INFO_HEADER) +
        ((((ULONG64)Width * BitsPerPixel + 31ULL) / 32ULL) *
         4ULL * Height);
    if (bitmapCapacity64 > MAXULONG) {
        return STATUS_INTEGER_OVERFLOW;
    }
    bitmapCapacity = (ULONG)bitmapCapacity64;
    bitmap = (PUCHAR)KswordARKAllocateNonPagedPool(
        bitmapCapacity,
        KSWORD_ARK_PANEL_POOL_TAG);
    if (bitmap == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pixels = NULL;
    status = KswordARKBugcheckPanelInitializeBitmap(
        bitmap,
        bitmapCapacity,
        Width,
        Height,
        BitsPerPixel,
        &bitmapLength,
        &bitmapStride,
        &pixels);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(bitmap, KSWORD_ARK_PANEL_POOL_TAG);
        return status;
    }

    bytesPerPixel = BitsPerPixel / 8UL;
    for (y = 0; y < Height; ++y) {
        PUCHAR row;

        row = pixels + ((SIZE_T)y * bitmapStride);
        for (x = 0; x < Width; ++x) {
            PUCHAR pixel;

            pixel = row + ((SIZE_T)x * bytesPerPixel);
            pixel[0] = (UCHAR)(White ? 255U : KSWORD_ARK_BUGCHECK_LAYOUT_BORDER_BLUE); // 二维码的亮模块必须为纯白。
            pixel[1] = (UCHAR)(White ? 255U : KSWORD_ARK_BUGCHECK_LAYOUT_BORDER_GREEN); // 保持原模式边框颜色。
            pixel[2] = (UCHAR)(White ? 255U : KSWORD_ARK_BUGCHECK_LAYOUT_BORDER_RED); // 两种资源仍由正常运行期解析。
            if (bytesPerPixel == 4UL) {
                pixel[3] = 0xFFU;
            }
        }
    }

    status = KswordARKBugcheckBgpParseBitmap(
        bitmap,
        bitmapLength,
        Rectangle);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(bitmap, KSWORD_ARK_PANEL_POOL_TAG);
        return status;
    }

    // Retain the source because the private parser's ownership is undocumented.
    *BackingBitmap = bitmap;
    return STATUS_SUCCESS;
}

// QR 的正方形模块和二进制长度横向段均在 PASSIVE_LEVEL 准备，避免崩溃时解析或分配。
static NTSTATUS
KswordARKBugcheckPanelPrepareQrRuns(
    _In_ ULONG VariantIndex,
    _In_ ULONG BitsPerPixel
    )
{
    ULONG moduleIndex; // 0 至 7 对应 1 至 8 像素模块。

    for (moduleIndex = 0; moduleIndex < KSWORD_ARK_PANEL_QR_MODULE_MAX; ++moduleIndex) { // 固定八种可用缩放。
        ULONG runIndex; // 每行白色模块按二进制长度分段。
        ULONG modulePixels; // 本组预生成矩形高度。

        modulePixels = moduleIndex + 1UL; // 长度一即要求的正方形模块。
        for (runIndex = 0; runIndex < KSWORD_ARK_PANEL_QR_RUN_COUNT; ++runIndex) { // 最长段覆盖 128 模块。
            NTSTATUS status; // 保留预算中止或解析器的原始失败状态。

            status = KswordARKBugcheckControlCheckAbort(); // 每个资源都遵守现有卸载和 30 秒准备预算。
            if (!NT_SUCCESS(status)) { // 不继续创建取消请求后的非分页资源。
                return status; // 调用方统一释放已经完成的资源。
            }
            status = KswordARKBugcheckPanelPrepareSolidRectangle(
                BitsPerPixel,
                modulePixels * (1UL << runIndex),
                modulePixels,
                TRUE,
                &g_KswordArkPanel.Variants[VariantIndex].QrRunRectangles[moduleIndex][runIndex],
                &g_KswordArkPanel.Variants[VariantIndex].QrRunBitmaps[moduleIndex][runIndex]); // 每个私有矩形保留其 backing BMP。
            if (!NT_SUCCESS(status)) { // 局部准备失败绝不发布为已就绪。
                return status; // 初始化失败路径清理之前的所有矩形。
            }
        }
    }
    return STATUS_SUCCESS; // 八组缩放均完整可用。
}

static NTSTATUS
KswordARKBugcheckPanelPrepareFrames(
    _In_ ULONG VariantIndex,
    _In_ ULONG BitsPerPixel
    )
{
    KSWORD_ARK_BUGCHECK_LAYOUT_FRAME frame;

    if (VariantIndex >= KSWORD_ARK_PANEL_BPP_VARIANT_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    for (frame = KswordArkBugcheckLayoutFrameCompactColumn;
         frame < KswordArkBugcheckLayoutFrameCount;
         frame = (KSWORD_ARK_BUGCHECK_LAYOUT_FRAME)(frame + 1)) {
        ULONG width;
        ULONG height;
        NTSTATUS status;

        if (!KswordARKBugcheckLayoutGetFrameMetrics(
                frame,
                &width,
                &height)) {
            return STATUS_INVALID_PARAMETER;
        }

        status = KswordARKBugcheckPanelPrepareSolidRectangle(
            BitsPerPixel,
            width,
            1UL,
            FALSE, // 原模式仍使用诊断页边框颜色。
            &g_KswordArkPanel.Variants[VariantIndex]
                .FrameHorizontalRectangles[frame],
            &g_KswordArkPanel.Variants[VariantIndex]
                .FrameHorizontalBitmaps[frame]);
        if (NT_SUCCESS(status)) {
            status = KswordARKBugcheckPanelPrepareSolidRectangle(
                BitsPerPixel,
                1UL,
                height,
                FALSE, // 原模式竖边框无需改变。
                &g_KswordArkPanel.Variants[VariantIndex]
                    .FrameVerticalRectangles[frame],
                &g_KswordArkPanel.Variants[VariantIndex]
                    .FrameVerticalBitmaps[frame]);
        }
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKBugcheckPanelInitialize(
    VOID
    )
{
    KSWORD_ARK_BGP_SCREEN_INFO screen;
    ULONG variantIndex;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    RtlZeroMemory(&g_KswordArkPanel, sizeof(g_KswordArkPanel));
    InterlockedExchange(&g_KswordArkPanel.ActiveVerdictSet, -1);
    InterlockedExchange(
        &g_KswordArkPanel.PreferredLanguage,
        (LONG)KswordARKBugcheckPanelQueryPreferredLanguage());
    KswordARKBugcheckBgpRecordPreparation(
        KswordArkBgpPreparationValidatePanelScreen,
        STATUS_PENDING);
    status = KswordARKBugcheckBgpGetScreenInfo(&screen);
    if (!NT_SUCCESS(status)) {
        KswordARKBugcheckBgpRecordPreparation(
            KswordArkBgpPreparationValidatePanelScreen,
            status);
        KswordARKBugcheckBgpRejectPreparation(status);
        return status;
    }
    // Accept the fully hidden pre-ownership mode so both supported rectangle
    // variants can still be prepared at PASSIVE_LEVEL.
    if (screen.BitsPerPixel != KSWORD_ARK_BGP_UNOWNED_BPP &&
        (screen.Width < KSWORD_ARK_BUGCHECK_LAYOUT_REQUIRED_WIDTH ||
         screen.Height < KSWORD_ARK_BUGCHECK_LAYOUT_REQUIRED_HEIGHT ||
         (screen.BitsPerPixel != 24UL &&
          screen.BitsPerPixel != 32UL))) {
        KswordARKBugcheckBgpRecordPreparation(
            KswordArkBgpPreparationValidatePanelScreen,
            STATUS_NOT_SUPPORTED);
        KswordARKBugcheckBgpRejectPreparation(STATUS_NOT_SUPPORTED);
        return STATUS_NOT_SUPPORTED;
    }

    KswordARKBugcheckBgpRecordPreparation(
        KswordArkBgpPreparationValidatePanelScreen,
        STATUS_SUCCESS);
    status = STATUS_SUCCESS;
    for (variantIndex = 0;
         variantIndex < KSWORD_ARK_PANEL_BPP_VARIANT_COUNT &&
             NT_SUCCESS(status);
         ++variantIndex) {
        ULONG bitsPerPixel;

        bitsPerPixel = variantIndex == KSWORD_ARK_PANEL_BPP24_INDEX
            ? 24UL
            : 32UL;
        g_KswordArkPanel.Variants[variantIndex].BitsPerPixel = bitsPerPixel;
        KswordARKBugcheckBgpRecordPreparation(
            KswordArkBgpPreparationPrepareLogo,
            STATUS_PENDING);
        status = KswordARKBugcheckPanelPrepareLogos(
            variantIndex,
            bitsPerPixel);
        KswordARKBugcheckBgpRecordPreparation(
            KswordArkBgpPreparationPrepareLogo,
            status);
        if (NT_SUCCESS(status)) {
            KswordARKBugcheckBgpRecordPreparation(
                KswordArkBgpPreparationPrepareGlyphs,
                STATUS_PENDING);
            status = KswordARKBugcheckPanelPrepareGlyphs(
                variantIndex,
                bitsPerPixel);
            if (NT_SUCCESS(status)) {
                // Frames are parsed with glyph resources before the crash.
                status = KswordARKBugcheckPanelPrepareFrames(
                    variantIndex,
                    bitsPerPixel);
            }
            if (NT_SUCCESS(status)) { // QR 资源必须与当前 24/32 BPP 变体一同完成。
                status = KswordARKBugcheckPanelPrepareQrRuns(
                    variantIndex,
                    bitsPerPixel); // 新模式不引入崩溃期的位图构建或解析。
            }
            KswordARKBugcheckBgpRecordPreparation(
                KswordArkBgpPreparationPrepareGlyphs,
                status);
        }
    }
    if (NT_SUCCESS(status)) {
        KswordARKBugcheckBgpRecordPreparation(
            KswordArkBgpPreparationArm,
            STATUS_PENDING);
        status = KswordARKBugcheckBgpArm(
            KSWORD_ARK_BUGCHECK_LAYOUT_REQUIRED_WIDTH,
            KSWORD_ARK_BUGCHECK_LAYOUT_REQUIRED_HEIGHT);
        KswordARKBugcheckBgpRecordPreparation(
            KswordArkBgpPreparationArm,
            status);
    }
    if (!NT_SUCCESS(status)) {
        KswordARKBugcheckPanelShutdown();
        KswordARKBugcheckBgpRejectPreparation(status);
        return status;
    }

    InterlockedExchange(&g_KswordArkPanel.Ready, 1);
    KswordARKBugcheckBgpRecordPreparation(
        KswordArkBgpPreparationComplete,
        STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

VOID
KswordARKBugcheckPanelShutdown(
    VOID
    )
{
    ULONG variantIndex;
    ULONG colorIndex;
    ULONG verdictSetIndex;

    InterlockedExchange(&g_KswordArkPanel.Ready, 0);
    InterlockedExchange(&g_KswordArkPanel.ActiveVariant, 0);
    InterlockedExchange(&g_KswordArkPanel.ActiveVerdictSet, -1);
    for (verdictSetIndex = 0;
         verdictSetIndex < KSWORD_ARK_PANEL_VERDICT_SET_COUNT;
         ++verdictSetIndex) {
        KswordARKBugcheckPanelReleaseVerdictSet(
            &g_KswordArkPanel.VerdictSets[verdictSetIndex]);
    }
    for (variantIndex = 0;
         variantIndex < KSWORD_ARK_PANEL_BPP_VARIANT_COUNT;
         ++variantIndex) {
        KswordARKBugcheckBgpDestroyRectangle(
            g_KswordArkPanel.Variants[variantIndex].LogoRectangle);
        g_KswordArkPanel.Variants[variantIndex].LogoRectangle = NULL;
        {
            ULONG moduleIndex; // 遍历所有提前准备的模块缩放。

            for (moduleIndex = 0; moduleIndex < KSWORD_ARK_PANEL_QR_MODULE_MAX; ++moduleIndex) { // 包括失败准备留下的部分资源。
                ULONG runIndex; // 释放各二进制长度段。

                for (runIndex = 0; runIndex < KSWORD_ARK_PANEL_QR_RUN_COUNT; ++runIndex) { // 固定且有界的资源表。
                    KswordARKBugcheckBgpDestroyRectangle(
                        g_KswordArkPanel.Variants[variantIndex].QrRunRectangles[moduleIndex][runIndex]); // 先释放可能引用 BMP 的私有矩形。
                    g_KswordArkPanel.Variants[variantIndex].QrRunRectangles[moduleIndex][runIndex] = NULL; // 防止重复释放。
                    if (g_KswordArkPanel.Variants[variantIndex].QrRunBitmaps[moduleIndex][runIndex] != NULL) { // backing 可能尚未创建。
                        ExFreePoolWithTag(
                            g_KswordArkPanel.Variants[variantIndex].QrRunBitmaps[moduleIndex][runIndex],
                            KSWORD_ARK_PANEL_POOL_TAG); // 与正常运行期分配标签保持一致。
                        g_KswordArkPanel.Variants[variantIndex].QrRunBitmaps[moduleIndex][runIndex] = NULL; // 生命周期结束后清空 resident 指针。
                    }
                }
            }
        }
        for (colorIndex = 0;
             colorIndex < KSWORD_ARK_PANEL_COLOR_COUNT;
             ++colorIndex) {
            ULONG glyphIndex;

            for (glyphIndex = 0;
                 glyphIndex < DRIVERGUI_FONT_COUNT;
                 ++glyphIndex) {
                KswordARKBugcheckBgpDestroyRectangle(
                    g_KswordArkPanel.Variants[variantIndex]
                        .GlyphRectangles[colorIndex][glyphIndex]);
                g_KswordArkPanel.Variants[variantIndex]
                    .GlyphRectangles[colorIndex][glyphIndex] = NULL;
                if (g_KswordArkPanel.Variants[variantIndex]
                        .GlyphBitmaps[colorIndex][glyphIndex] != NULL) {
                    ExFreePoolWithTag(
                        g_KswordArkPanel.Variants[variantIndex]
                            .GlyphBitmaps[colorIndex][glyphIndex],
                        KSWORD_ARK_PANEL_POOL_TAG);
                    g_KswordArkPanel.Variants[variantIndex]
                        .GlyphBitmaps[colorIndex][glyphIndex] = NULL;
                }
            }
        }
        {
            KSWORD_ARK_BUGCHECK_LAYOUT_FRAME frame;

            for (frame = KswordArkBugcheckLayoutFrameCompactColumn;
                 frame < KswordArkBugcheckLayoutFrameCount;
                 frame = (KSWORD_ARK_BUGCHECK_LAYOUT_FRAME)(frame + 1)) {
                KswordARKBugcheckBgpDestroyRectangle(
                    g_KswordArkPanel.Variants[variantIndex]
                        .FrameHorizontalRectangles[frame]);
                g_KswordArkPanel.Variants[variantIndex]
                    .FrameHorizontalRectangles[frame] = NULL;
                KswordARKBugcheckBgpDestroyRectangle(
                    g_KswordArkPanel.Variants[variantIndex]
                        .FrameVerticalRectangles[frame]);
                g_KswordArkPanel.Variants[variantIndex]
                    .FrameVerticalRectangles[frame] = NULL;
                if (g_KswordArkPanel.Variants[variantIndex]
                        .FrameHorizontalBitmaps[frame] != NULL) {
                    ExFreePoolWithTag(
                        g_KswordArkPanel.Variants[variantIndex]
                            .FrameHorizontalBitmaps[frame],
                        KSWORD_ARK_PANEL_POOL_TAG);
                    g_KswordArkPanel.Variants[variantIndex]
                        .FrameHorizontalBitmaps[frame] = NULL;
                }
                if (g_KswordArkPanel.Variants[variantIndex]
                        .FrameVerticalBitmaps[frame] != NULL) {
                    ExFreePoolWithTag(
                        g_KswordArkPanel.Variants[variantIndex]
                            .FrameVerticalBitmaps[frame],
                        KSWORD_ARK_PANEL_POOL_TAG);
                    g_KswordArkPanel.Variants[variantIndex]
                        .FrameVerticalBitmaps[frame] = NULL;
                }
            }
        }
    }
}

static NTSTATUS
KswordARKBugcheckPanelDrawText(
    _In_opt_ PVOID Context,
    _In_ LONG X,
    _In_ LONG Y,
    _In_z_ PCSTR Text,
    _In_ ULONG ColorIndex
    )
{
    LONG activeVariant;
    LONG cursorX;

    UNREFERENCED_PARAMETER(Context);
    if (Text == NULL || ColorIndex >= KSWORD_ARK_PANEL_COLOR_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    activeVariant = InterlockedCompareExchange(
        &g_KswordArkPanel.ActiveVariant,
        0,
        0);
    if (activeVariant < 0 ||
        activeVariant >= (LONG)KSWORD_ARK_PANEL_BPP_VARIANT_COUNT) {
        return STATUS_DEVICE_NOT_READY;
    }

    cursorX = X;
    while (*Text != '\0') {
        UCHAR character;
        ULONG glyphIndex;
        NTSTATUS status;

        character = (UCHAR)*Text;
        if (character < DRIVERGUI_FONT_FIRST ||
            character > DRIVERGUI_FONT_LAST) {
            character = (UCHAR)'?';
        }
        glyphIndex = character - DRIVERGUI_FONT_FIRST;
        if (character != (UCHAR)' ') {
            status = KswordARKBugcheckBgpDrawRectangle(
                g_KswordArkPanel.Variants[activeVariant]
                    .GlyphRectangles[ColorIndex][glyphIndex],
                cursorX - (LONG)KSWORD_ARK_PANEL_GLYPH_BORDER,
                Y - (LONG)KSWORD_ARK_PANEL_GLYPH_BORDER);
            if (!NT_SUCCESS(status)) {
                return status;
            }
        }
        cursorX += KSWORD_ARK_PANEL_GLYPH_ADVANCE;
        ++Text;
    }

    return STATUS_SUCCESS;
}

// 蓝底中的暗模块无需绘制；亮模块及四模块静区使用 resident 白色横向段。
static NTSTATUS
KswordARKBugcheckPanelDrawQr(
    _In_opt_ PVOID Context,
    _In_ LONG X,
    _In_ LONG Y,
    _In_ ULONG ModulePixels,
    _In_ const UCHAR* QrCode
    )
{
    const KSWORD_ARK_BGP_DUMP_STATE* screen; // 已持有显示锁后采集的真实屏幕几何。
    LONG activeVariant; // 24 或 32 BPP 的 resident 矩形组。
    LONG qrSize; // Nayuki 矩阵的实际边长。
    LONG totalModules; // 含四模块静区的总边长。
    LONG row; // 总图像中的模块行号。
    ULONG64 pixelExtent; // 先用宽整数验证乘法和屏幕边界。

    screen = (const KSWORD_ARK_BGP_DUMP_STATE*)Context; // Canvas 保留本次绘制的固定快照。
    if (screen == NULL || QrCode == NULL || X < 0 || Y < 0 ||
        ModulePixels == 0 || ModulePixels > KSWORD_ARK_PANEL_QR_MODULE_MAX ||
        QrCode[0] < 21U || QrCode[0] > 177U || ((QrCode[0] - 17U) & 3U) != 0U) { // 先验证尺寸再调用库中的断言接口。
        return STATUS_INVALID_PARAMETER; // 畸形矩阵不得进入私有绘制器。
    }
    qrSize = qrcodegen_getSize(QrCode); // 只读取已经编码的固定矩阵。
    totalModules = qrSize + KSWORD_ARK_PANEL_QR_QUIET_ZONE * 2L; // 四边静区全部包含在调用坐标内。
    pixelExtent = (ULONG64)(ULONG)totalModules * ModulePixels; // 缩放后边长最多 1480 像素。
    if ((ULONG64)(ULONG)X + pixelExtent > screen->ScreenWidth ||
        (ULONG64)(ULONG)Y + pixelExtent > screen->ScreenHeight ||
        (ULONG64)(ULONG)X + pixelExtent > (ULONG64)MAXLONG ||
        (ULONG64)(ULONG)Y + pixelExtent > (ULONG64)MAXLONG) { // 整张二维码完整落在画布中，且私有 LONG 坐标不会溢出。
        return STATUS_BUFFER_TOO_SMALL; // 不裁剪可扫描的数据。
    }
    activeVariant = InterlockedCompareExchange(&g_KswordArkPanel.ActiveVariant, 0, 0); // 读取当前真实 BPP 资源组。
    if (activeVariant < 0 || activeVariant >= (LONG)KSWORD_ARK_PANEL_BPP_VARIANT_COUNT) { // 防御未发布或关闭中的资源状态。
        return STATUS_DEVICE_NOT_READY; // 不访问无效矩形表。
    }

    for (row = 0; row < totalModules; ++row) { // 最大版本也只遍历 185 行。
        LONG column; // 每行从左到右合并相邻白色模块。

        column = 0; // 静区起点位于左边界。
        while (column < totalModules) { // 总访问次数受 QR 版本 40 上界限制。
            LONG runStart; // 当前白色段的起始模块。
            ULONG remaining; // 二进制段切分后剩余的模块数。

            if (qrcodegen_getModule(QrCode,
                    (int)(column - KSWORD_ARK_PANEL_QR_QUIET_ZONE),
                    (int)(row - KSWORD_ARK_PANEL_QR_QUIET_ZONE))) { // 越界坐标由库定义为亮模块。
                ++column; // 纯蓝暗模块已经由全屏清色提供。
                continue; // 不覆盖蓝色数据模块。
            }
            runStart = column; // 保存本行亮模块序列的起点。
            do {
                ++column; // 向右查找亮模块序列终点。
            } while (column < totalModules && !qrcodegen_getModule(QrCode,
                (int)(column - KSWORD_ARK_PANEL_QR_QUIET_ZONE),
                (int)(row - KSWORD_ARK_PANEL_QR_QUIET_ZONE))); // 完整扫描所有静区与矩阵模块。
            remaining = (ULONG)(column - runStart); // 长度至少为一，最多为 185。
            while (remaining != 0UL) { // 贪心二进制切分只使用准备好的矩形。
                ULONG runIndex; // 最大可容纳的 resident 段。
                ULONG runModules; // 本次画出的模块数。
                NTSTATUS status; // 部分绘制失败必须向上传播。

                runIndex = KSWORD_ARK_PANEL_QR_RUN_COUNT - 1UL; // 从 128 模块段开始选择。
                while ((1UL << runIndex) > remaining) { // 剩余长度始终非零，不会出现下溢。
                    --runIndex; // 缩小到能完整覆盖的二进制长度。
                }
                runModules = 1UL << runIndex; // 确定本次矩形宽度。
                status = KswordARKBugcheckBgpDrawRectangle(
                    g_KswordArkPanel.Variants[activeVariant].QrRunRectangles[ModulePixels - 1UL][runIndex],
                    X + runStart * (LONG)ModulePixels,
                    Y + row * (LONG)ModulePixels); // 不创建位图、不分配内存、不执行文件 I/O。
                if (!NT_SUCCESS(status)) { // 扫描图案有缺口时不能返回绘制成功。
                    return status; // 由 BGP FinishDraw 记录失败并释放显示锁。
                }
                runStart += (LONG)runModules; // 当前白色段的绘制起点右移。
                remaining -= runModules; // 已完成模块不会重复绘制。
            }
        }
    }
    return STATUS_SUCCESS; // 只有矩阵和静区全部绘制完毕才报告成功。
}

static NTSTATUS
KswordARKBugcheckPanelDrawFrame(
    _In_opt_ PVOID Context,
    _In_ LONG X,
    _In_ LONG Y,
    _In_ KSWORD_ARK_BUGCHECK_LAYOUT_FRAME Frame
    )
{
    LONG activeVariant;
    ULONG width;
    ULONG height;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Context);
    if (!KswordARKBugcheckLayoutGetFrameMetrics(
            Frame,
            &width,
            &height)) {
        return STATUS_INVALID_PARAMETER;
    }

    activeVariant = InterlockedCompareExchange(
        &g_KswordArkPanel.ActiveVariant,
        0,
        0);
    if (activeVariant < 0 ||
        activeVariant >= (LONG)KSWORD_ARK_PANEL_BPP_VARIANT_COUNT) {
        return STATUS_DEVICE_NOT_READY;
    }

    // Each frame uses four rectangles parsed before the bugcheck occurs.
    status = KswordARKBugcheckBgpDrawRectangle(
        g_KswordArkPanel.Variants[activeVariant]
            .FrameHorizontalRectangles[Frame],
        X,
        Y);
    if (NT_SUCCESS(status)) {
        status = KswordARKBugcheckBgpDrawRectangle(
            g_KswordArkPanel.Variants[activeVariant]
                .FrameHorizontalRectangles[Frame],
            X,
            Y + (LONG)height - 1L);
    }
    if (NT_SUCCESS(status)) {
        status = KswordARKBugcheckBgpDrawRectangle(
            g_KswordArkPanel.Variants[activeVariant]
                .FrameVerticalRectangles[Frame],
            X,
            Y);
    }
    if (NT_SUCCESS(status)) {
        status = KswordARKBugcheckBgpDrawRectangle(
            g_KswordArkPanel.Variants[activeVariant]
                .FrameVerticalRectangles[Frame],
            X + (LONG)width - 1L,
            Y);
    }
    return status;
}

static NTSTATUS
KswordARKBugcheckPanelDrawVerdict(
    _In_opt_ PVOID Context,
    _In_ LONG X,
    _In_ LONG Y,
    _In_ ULONG Classification
    )
{
    LONG activeSet;
    LONG activeVariant;
    LONG preferredLanguage;
    PKSWORD_ARK_PANEL_VERDICT_SET verdictSet;
    PKSWORD_ARK_PANEL_VERDICT_ITEM item;

    UNREFERENCED_PARAMETER(Context);
    activeSet = InterlockedCompareExchange(
        &g_KswordArkPanel.ActiveVerdictSet,
        0,
        0);
    activeVariant = InterlockedCompareExchange(
        &g_KswordArkPanel.ActiveVariant,
        0,
        0);
    preferredLanguage = InterlockedCompareExchange(
        &g_KswordArkPanel.PreferredLanguage,
        0,
        0);
    if (activeSet < 0 ||
        activeSet >= (LONG)KSWORD_ARK_PANEL_VERDICT_SET_COUNT ||
        activeVariant < 0 ||
        activeVariant >= (LONG)KSWORD_ARK_PANEL_BPP_VARIANT_COUNT) {
        return STATUS_NOT_FOUND;
    }
    if (preferredLanguage < 0 ||
        preferredLanguage >=
            (LONG)KSWORD_ARK_BUGCHECK_VERDICT_LANGUAGE_COUNT) {
        preferredLanguage =
            KSWORD_ARK_BUGCHECK_VERDICT_LANGUAGE_ENGLISH;
    }
    if (Classification >= KSWORD_ARK_BUGCHECK_VERDICT_CLASS_COUNT) {
        Classification = KSWORD_ARK_BUGCHECK_VERDICT_CLASS_UNKNOWN;
    }

    verdictSet = &g_KswordArkPanel.VerdictSets[activeSet];
    if (!verdictSet->Complete) {
        return STATUS_NOT_FOUND;
    }
    item = &verdictSet->Items[activeVariant]
        [preferredLanguage][Classification];
    if (item->Rectangle == NULL) {
        return STATUS_NOT_FOUND;
    }
    return KswordARKBugcheckBgpDrawRectangle(item->Rectangle, X, Y);
}

NTSTATUS
KswordARKBugcheckPanelDraw(
    _In_ const KSWORD_ARK_BUGCHECK_DIAGNOSTICS* Diagnostics,
    _In_ ULONG CallbackMask,
    _In_ ULONG ModuleCount
    )
{
    KSWORD_ARK_BGP_DUMP_STATE bgpState;
    KSWORD_ARK_BUGCHECK_LAYOUT_CANVAS canvas;
    LONG activeVariant;
    LONG originX;
    ULONG renderMode; // 一次回调固定选择模式，避免画布和布局不一致。
    NTSTATUS status;

    if (Diagnostics == NULL ||
        InterlockedCompareExchange(&g_KswordArkPanel.Ready, 0, 0) == 0) {
        return STATUS_DEVICE_NOT_READY;
    }

    status = KswordARKBugcheckBgpBeginDraw();
    if (!NT_SUCCESS(status)) {
        return status;
    }

    activeVariant = KswordARKBugcheckBgpGetCurrentBpp() == 24UL
        ? (LONG)KSWORD_ARK_PANEL_BPP24_INDEX
        : (LONG)KSWORD_ARK_PANEL_BPP32_INDEX;
    InterlockedExchange(&g_KswordArkPanel.ActiveVariant, activeVariant);
    KswordARKBugcheckBgpSnapshot(&bgpState);
    renderMode = KswordARKBugcheckControlGetRenderMode(); // 模式在正常运行期通过共享协议原子发布。
    originX = KswordARKBugcheckLayoutOriginX(
        bgpState.ScreenWidth,
        bgpState.ScreenHeight);

    // Clear only after BeginDraw has acquired ownership and validated geometry.
    status = KswordARKBugcheckBgpClearScreen(
        renderMode == KSWORD_ARK_BUGCHECK_RENDER_MODE_LINUX_QR
            ? KSWORD_ARK_PANEL_LINUX_BACKGROUND_ARGB
            : KSWORD_ARK_PANEL_BACKGROUND_ARGB); // Linux 模式使用示例中的纯蓝底。
    if (NT_SUCCESS(status) && renderMode != KSWORD_ARK_BUGCHECK_RENDER_MODE_LINUX_QR) { // Linux 模式由共用布局绘制企鹅，不叠加原 Logo。
        status = KswordARKBugcheckBgpDrawRectangle(
            g_KswordArkPanel.Variants[activeVariant].LogoRectangle,
            originX + KSWORD_ARK_BUGCHECK_LAYOUT_LOGO_X,
            KSWORD_ARK_BUGCHECK_LAYOUT_LOGO_Y);
    }
    if (NT_SUCCESS(status)) {
        KswordARKBugcheckBgpSnapshot(&bgpState); // 纳入已完成清屏的最新阶段，报告不预测后续二维码绘制结果。
        RtlZeroMemory(&canvas, sizeof(canvas));
        canvas.Context = &bgpState; // 二维码回调用实际屏幕几何验证完整边界。
        canvas.Width = bgpState.ScreenWidth;
        canvas.Height = bgpState.ScreenHeight;
        canvas.DrawText = KswordARKBugcheckPanelDrawText;
        canvas.DrawFrame = KswordARKBugcheckPanelDrawFrame;
        canvas.DrawVerdict = KswordARKBugcheckPanelDrawVerdict;
        canvas.RenderMode = renderMode; // 两个后端使用相同的共享模式枚举。
        canvas.BgpSnapshot = &bgpState; // 二维码报告包含已采集的 BGP 准备和崩溃阶段状态。
        canvas.DrawQr = KswordARKBugcheckPanelDrawQr; // 只使用正常运行期预生成的 resident 矩形。
        status = KswordARKBugcheckLayoutDraw(
            &canvas,
            Diagnostics,
            CallbackMask,
            ModuleCount);
    }

    KswordARKBugcheckBgpFinishDraw(status);
    return status;
}
