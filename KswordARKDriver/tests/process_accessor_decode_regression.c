// Exercise the exact production decoder without loading or calling a driver.
#include "../src/platform/process_accessor_decode.h"
#include <stdio.h>
#include <string.h>

static unsigned int gChecks = 0U;
static unsigned int gFailures = 0U;

static void
Expect(const char* Name, int Condition)
{
    ++gChecks;
    if (!Condition) {
        ++gFailures;
        printf("FAIL: %s\n", Name);
    }
}

typedef struct _CASE
{
    uint8_t Bytes[8];
    size_t Count;
    int32_t Offset;
    KSWORD_ACCESSOR_LOAD_KIND Kind;
} CASE;

typedef struct _IMAGE
{
    uint8_t Bytes[128];
    uintptr_t Base;
    size_t Count;
    uintptr_t DeniedAddress;
    size_t Reads;
} IMAGE;

static int
ReadImage(void* Context, uintptr_t Address, uint8_t* ByteOut)
{
    IMAGE* image = (IMAGE*)Context;
    ++image->Reads;
    if (Address == image->DeniedAddress || Address < image->Base ||
        Address - image->Base >= image->Count) {
        return 0;
    }
    *ByteOut = image->Bytes[Address - image->Base];
    return 1;
}

static void
PositiveAndTruncationCases(void)
{
    // Offsets and load kinds are fixture oracles, not calculated by the decoder.
    static const CASE cases[] = {
        { { 0x48, 0x8B, 0x41, 0x78, 0xC3 }, 5U, 0x78, KswordAccessorLoadPointer },
        { { 0x48, 0x8B, 0x81, 0x28, 0x06, 0, 0, 0xC3 }, 8U, 0x628, KswordAccessorLoadPointer },
        { { 0x8B, 0x41, 0x44, 0xC3 }, 4U, 0x44, KswordAccessorLoadUlong },
        { { 0x8B, 0x81, 0xEC, 0x04, 0, 0, 0xC3 }, 7U, 0x4EC, KswordAccessorLoadUlong },
        { { 0x0F, 0xB7, 0x41, 0x16, 0xC3 }, 5U, 0x16, KswordAccessorLoadUshort },
        { { 0x0F, 0xB7, 0x81, 0x30, 0x08, 0, 0, 0xC3 }, 8U, 0x830, KswordAccessorLoadUshort },
        { { 0x0F, 0xB6, 0x41, 0x10, 0xC3 }, 5U, 0x10, KswordAccessorLoadUchar },
        { { 0x0F, 0xB6, 0x81, 0x02, 0x09, 0, 0, 0xC3 }, 8U, 0x902, KswordAccessorLoadUchar },
        { { 0x48, 0x8D, 0x41, 0x40, 0xC3 }, 5U, 0x40, KswordAccessorAddress },
        { { 0x48, 0x8D, 0x81, 0xFF, 0x3F, 0, 0, 0xC3 }, 8U, 0x3FFF, KswordAccessorAddress }
    };
    static const uint8_t prefixes[][5] = {
        { 0x90 }, { 0x66, 0x90 }, { 0x0F, 0x1F, 0x00 },
        { 0x0F, 0x1F, 0x44, 0x00, 0x00 }, { 0xF3, 0x0F, 0x1E, 0xFA }
    };
    static const size_t prefixCounts[] = { 1U, 2U, 3U, 5U, 4U };
    size_t index = 0U;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const CASE* fixture = &cases[index];
        KSWORD_ACCESSOR_DISPLACEMENT result;
        size_t length = 0U;
        size_t prefix = 0U;
        IMAGE image;

        Expect("direct load/LEA and return", KswordARKDecodeAccessorDisplacement(
            fixture->Bytes, fixture->Count, &result) && result.Offset == fixture->Offset &&
            result.LoadKind == fixture->Kind);
        // Every incomplete instruction prefix must fail and erase stale output.
        for (length = 0U; length < fixture->Count; ++length) {
            result.Offset = 42;
            Expect("truncated accessor", !KswordARKDecodeAccessorDisplacement(
                fixture->Bytes, length, &result) && result.Offset == -1);
        }
        for (prefix = 0U; prefix < sizeof(prefixCounts) / sizeof(prefixCounts[0]); ++prefix) {
            uint8_t padded[16] = { 0 };
            memcpy(padded, prefixes[prefix], prefixCounts[prefix]);
            memcpy(padded + prefixCounts[prefix], fixture->Bytes, fixture->Count);
            Expect("explicit harmless entry prefix", KswordARKDecodeAccessorDisplacement(
                padded, prefixCounts[prefix] + fixture->Count, &result) &&
                result.Offset == fixture->Offset && result.LoadKind == fixture->Kind);
        }
        memset(&image, 0, sizeof(image));
        image.Base = 0x1000U;
        image.Count = fixture->Count;
        memcpy(image.Bytes, fixture->Bytes, fixture->Count);
        Expect("reader stops at return with no readable following byte",
            KswordARKResolveAccessorDisplacement(ReadImage, &image, image.Base, &result) &&
            result.Offset == fixture->Offset && image.Reads == fixture->Count);
        // Failure of each byte read, including RET, cannot publish any offset.
        for (length = 0U; length < fixture->Count; ++length) {
            image.Reads = 0U;
            image.DeniedAddress = image.Base + length;
            result.Offset = 42;
            Expect("incomplete safe read", !KswordARKResolveAccessorDisplacement(
                ReadImage, &image, image.Base, &result) && result.Offset == -1);
        }
    }
}

static void
RejectedSemanticCases(void)
{
    static const uint8_t cases[][16] = {
        { 0x48, 0x8B, 0x42, 0x40, 0xC3 }, // RDX base instead of RCX.
        { 0x48, 0x8B, 0x49, 0x40, 0xC3 }, // RCX destination instead of RAX.
        { 0x48, 0x8B, 0x44, 0x09, 0x40, 0xC3 }, // SIB/indexed access.
        { 0x48, 0x8B, 0x41, 0x80, 0xC3 }, // Negative disp8.
        { 0x48, 0x8B, 0x41, 0x00, 0xC3 }, // Zero offset is unavailable.
        { 0x48, 0x8B, 0x81, 0x00, 0x40, 0, 0, 0xC3 }, // Outside field budget.
        { 0x48, 0x8B, 0x81, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3 }, // Negative disp32.
        { 0x48, 0x8B, 0x41, 0x40, 0x48, 0x8B, 0x41, 0x48, 0xC3 }, // Second load.
        { 0x48, 0x8B, 0x41, 0x40, 0x24, 0x01, 0xC3 }, // Return value changes.
        { 0xC3, 0x48, 0x8B, 0x41, 0x40, 0xC3 }, // Decoy in following function.
        { 0x50, 0x48, 0x8B, 0x41, 0x40, 0xC3 }, // Stack side effect.
        { 0x8A, 0x41, 0x40, 0xC3 }, // Undefined upper ABI return bits.
        { 0x0F, 0xBE, 0x41, 0x40, 0xC3 }, // Sign extension is not MOVZX.
        { 0x48, 0x8B, 0x41, 0x40, 0xC2, 0, 0 }, // RET imm has different semantics.
        { 0xE8, 0, 0, 0, 0, 0x48, 0x8B, 0x41, 0x40, 0xC3 }, // CALL is not a thunk.
        { 0xFF, 0x25, 0, 0, 0, 0 }, // Indirect jumps are unsupported.
        { 0xF3, 0x0F, 0x1E, 0xFA, 0xF3, 0x0F, 0x1E, 0xFA,
          0x48, 0x8B, 0x41, 0x40, 0xC3 } // Duplicate CET prefix.
    };
    size_t index = 0U;
    KSWORD_ACCESSOR_DISPLACEMENT result;
    uint8_t tooMuchPadding[22];
    uint8_t repReturn[] = { 0x48, 0x8B, 0x41, 0x40, 0xF3, 0xC3 };

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        result.Offset = 42;
        Expect("reject non-accessor semantics", !KswordARKDecodeAccessorDisplacement(
            cases[index], sizeof(cases[index]), &result) && result.Offset == -1);
    }
    memset(tooMuchPadding, 0x90, sizeof(tooMuchPadding));
    memcpy(tooMuchPadding + 17U, repReturn, 5U);
    Expect("bounded padding", !KswordARKDecodeAccessorDisplacement(
        tooMuchPadding, sizeof(tooMuchPadding), &result));
    Expect("architectural REP RET", KswordARKDecodeAccessorDisplacement(
        repReturn, sizeof(repReturn), &result) && result.Offset == 0x40);
    Expect("REP without RET is incomplete", !KswordARKDecodeAccessorDisplacement(
        repReturn, sizeof(repReturn) - 1U, &result));
    Expect("null bytes", !KswordARKDecodeAccessorDisplacement(NULL, 8U, &result));
    Expect("null output", !KswordARKDecodeAccessorDisplacement(repReturn, sizeof(repReturn), NULL));
    Expect("null reader", !KswordARKResolveAccessorDisplacement(NULL, NULL, 0x1000U, &result));
}

static void
ThunkCases(void)
{
    static const uint8_t body[] = { 0x48, 0x8B, 0x81, 0x28, 0x06, 0, 0, 0xC3 };
    KSWORD_ACCESSOR_DISPLACEMENT result;
    IMAGE image;

    memset(&image, 0xCC, sizeof(image));
    image.Base = 0x1000U;
    image.Count = sizeof(image.Bytes);
    image.DeniedAddress = 0U;
    image.Reads = 0U;
    // Two different direct thunk encodings are followed before reaching the body.
    image.Bytes[0] = 0xEB; image.Bytes[1] = 0x0E;
    image.Bytes[16] = 0xE9; image.Bytes[17] = 0x0B;
    image.Bytes[18] = 0; image.Bytes[19] = 0; image.Bytes[20] = 0;
    memcpy(image.Bytes + 32U, body, sizeof(body));
    Expect("bounded EB/E9 thunk chain", KswordARKResolveAccessorDisplacement(
        ReadImage, &image, image.Base, &result) && result.Offset == 0x628);
    Expect("thunk reads only entry instructions and body", image.Reads == 15U);
    // A third hop exhausts the budget and never reaches the valid later body.
    image.Bytes[32] = 0xEB; image.Bytes[33] = 0x0E;
    memcpy(image.Bytes + 48U, body, sizeof(body));
    Expect("three-hop chain rejected", !KswordARKResolveAccessorDisplacement(
        ReadImage, &image, image.Base, &result) && result.Offset == -1);
    image.Bytes[0] = 0xEB; image.Bytes[1] = 0xFE;
    Expect("self-loop rejected", !KswordARKResolveAccessorDisplacement(
        ReadImage, &image, image.Base, &result));
    image.Bytes[0] = 0xEB; image.Bytes[1] = 0x0E;
    image.Bytes[16] = 0xEB; image.Bytes[17] = 0xEE;
    Expect("two-entry loop rejected", !KswordARKResolveAccessorDisplacement(
        ReadImage, &image, image.Base, &result));
    // A valid negative rel32 jump back to a prior body is accepted without a loop.
    memcpy(image.Bytes, body, sizeof(body));
    image.Bytes[32] = 0xE9; image.Bytes[33] = 0xDB;
    image.Bytes[34] = 0xFF; image.Bytes[35] = 0xFF; image.Bytes[36] = 0xFF;
    Expect("negative rel32 thunk", KswordARKResolveAccessorDisplacement(
        ReadImage, &image, image.Base + 32U, &result) && result.Offset == 0x628);
    image.Bytes[0] = 0xE9; image.Bytes[1] = 0xFF;
    image.Bytes[2] = 0xFF; image.Bytes[3] = 0xFF; image.Bytes[4] = 0x7F;
    Expect("jump leaves permitted executable image", !KswordARKResolveAccessorDisplacement(
        ReadImage, &image, image.Base, &result));
    image.Base = UINTPTR_MAX - 4U;
    image.Bytes[0] = 0xE9; image.Bytes[1] = 0; image.Bytes[2] = 0;
    image.Bytes[3] = 0; image.Bytes[4] = 0;
    Expect("jump address overflow", !KswordARKResolveAccessorDisplacement(
        ReadImage, &image, image.Base, &result));
    image.Base = 1U;
    image.Bytes[0] = 0xEB; image.Bytes[1] = 0x80;
    Expect("jump address underflow", !KswordARKResolveAccessorDisplacement(
        ReadImage, &image, image.Base, &result));
}

int
main(void)
{
    PositiveAndTruncationCases();
    RejectedSemanticCases();
    ThunkCases();
    printf("PROCESS_ACCESSOR_REGRESSION checks=%u failures=%u\n", gChecks, gFailures);
    return gFailures == 0U ? 0 : 1;
}
