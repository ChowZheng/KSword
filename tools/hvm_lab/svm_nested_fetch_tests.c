/* Offline source cases. Execution is deliberately deferred; these do not execute SVM instructions. */
#include <stdio.h>
#include <string.h>
#include "../../KswordARKDriver/src/features/hvm/hvm_svm_nested_fetch.h"
#define CHECK(x) do { if (!(x)) { printf("fetch:%u: %s\n", (unsigned)__LINE__, #x); return 1; } } while (0)
static KSW_SVM_U64 ram[16][512];
static unsigned failRead, reads, codeReads, failOrdinal, mutateCode;
static int read_word(void* context, KSW_SVM_U64 address, KSW_SVM_U64* value)
{
    (void)context;
    ++reads;
    if (failRead || (failOrdinal && reads == failOrdinal) || address >= sizeof(ram) || (address & 7)) { return 0; }
    *value = ram[address >> 12][(address & 4095) >> 3];
    if (address >= 0x8000 && address < 0xa000) {
        ++codeReads;
        if (mutateCode) { ram[7][0] ^= 0x1000; mutateCode = 0; }
    }
    return 1;
}
int main(void)
{
    const unsigned char raw[] = {0x0f, 0x01, 0xd8};
    const unsigned char prefixed[] = {0xf3, 0x67, 0x0f, 0x01, 0xd8};
    unsigned char repeated[] = {0x67, 0x67, 0x0f, 0x01, 0xda}, bytes[15];
    KSW_SVM_VMCB vmcb;
    KSW_NSVM_OPERAND_IO io = {0};
    KSW_SVM_U64 operand = 0;
    unsigned bits = 0, length = 0, index;
    KSW_SVM_SEGMENT* cs;
    CHECK(KswSvmNestedDecodeSvmOperand(raw, 3, 0x80, 1, 0, 0x123456789abcdef0ULL, &operand, &bits) == KSW_NNPT_OK);
    CHECK(bits == 64 && operand == 0x123456789abcdef0ULL);
    CHECK(KswSvmNestedDecodeSvmOperand(prefixed, 5, 0x80, 1, 0, 0x123456789abcdef0ULL, &operand, &bits) == KSW_NNPT_OK);
    CHECK(bits == 32 && operand == 0x9abcdef0ULL);
    CHECK(KswSvmNestedDecodeSvmOperand(repeated, 5, 0x82, 1, 0, 0x123456789abcdef0ULL, &operand, &bits) == KSW_NNPT_OK);
    CHECK(bits == 32 && operand == 0x9abcdef0ULL);
    CHECK(KswSvmNestedDecodeSvmOperand(raw, 3, 0x80, 0, 1, ~0ULL, &operand, &bits) == KSW_NNPT_OK && bits == 32 && operand == 0xffffffffULL);
    CHECK(KswSvmNestedDecodeSvmOperand(prefixed, 5, 0x80, 0, 1, ~0ULL, &operand, &bits) == KSW_NNPT_OK && bits == 16 && operand == 0xffffULL);
    CHECK(KswSvmNestedDecodeSvmOperand(raw, 3, 0x80, 0, 0, ~0ULL, &operand, &bits) == KSW_NNPT_OK && bits == 16);
    CHECK(KswSvmNestedDecodeSvmOperand(prefixed, 5, 0x80, 0, 0, ~0ULL, &operand, &bits) == KSW_NNPT_OK && bits == 32);
    CHECK(KswSvmNestedDecodeSvmOperand(raw, 3, 0x82, 1, 0, 0, &operand, &bits) == KSW_NNPT_RETRY);
    repeated[0] = 0xf0;
    CHECK(KswSvmNestedDecodeSvmOperand(repeated, 5, 0x82, 1, 0, 0, &operand, &bits) == KSW_NNPT_RETRY);
    repeated[0] = 0x48;
    CHECK(KswSvmNestedDecodeSvmOperand(repeated, 5, 0x82, 1, 0, 0, &operand, &bits) == KSW_NNPT_OK);
    CHECK(KswSvmNestedDecodeSvmOperand(repeated, 5, 0x82, 0, 1, 0, &operand, &bits) == KSW_NNPT_RETRY);
    memset(&vmcb, 0, sizeof(vmcb));
    io.Root = 0x1000; io.Pat = 6; io.PhysicalBits = 48; io.Page1Gb = 1; io.Nx = 1; io.Read = read_word;
    ram[1][0] = 0x2007; ram[2][0] = 0x3007; ram[3][0] = 0x87;
    ram[4][256] = 0x5007; ram[5][0] = 0x6007; ram[6][0] = 0x7007;
    ram[7][0] = 0x8007; ram[7][1] = 0x9007;
    KswSvmWrite64(&vmcb, KSW_VMCB_EFER, 0xd00);
    KswSvmWrite64(&vmcb, KSW_VMCB_CR0, 0x80000001);
    KswSvmWrite64(&vmcb, KSW_VMCB_CR4, 0x20);
    KswSvmWrite64(&vmcb, KSW_VMCB_CR3, 0x4000);
    cs = (KSW_SVM_SEGMENT*)((unsigned char*)&vmcb + KSW_VMCB_CS);
    /* VMCB attributes use a compact format, with L at bit nine. */
    cs->attributes = 0x29b;
    KswSvmWrite64(&vmcb, KSW_VMCB_RIP, 0xffff800000000ffeULL);
    KswSvmWrite64(&vmcb, KSW_VMCB_NRIP, 0xffff800000001003ULL);
    for (index = 0; index < sizeof(prefixed); ++index) { ((unsigned char*)ram)[0x8ffe + index] = prefixed[index]; }
    CHECK(KswSvmNestedFetchInstruction(&io, &vmcb, bytes, &length) == KSW_NNPT_OK);
    CHECK(length == sizeof(prefixed) && !memcmp(bytes, prefixed, sizeof(prefixed)));
    {
        unsigned offset, size, j, baselineReads = 0;
        static const unsigned offsets[] = {0, 1, 7, 0xff8, 0xffe, 0xfff};
        for (offset = 0; offset < sizeof(offsets) / sizeof(offsets[0]); ++offset) {
            for (size = 1; size <= 15; ++size) {
                KSW_SVM_U64 start = 0xffff800000000000ULL + offsets[offset];
                unsigned expectedWords = ((offsets[offset] & 7U) + size + 7U) / 8U;
                for (j = 0; j < size; ++j) { ((unsigned char*)ram)[0x8000 + offsets[offset] + j] = (unsigned char)(j + 19U); }
                KswSvmWrite64(&vmcb, KSW_VMCB_RIP, start);
                KswSvmWrite64(&vmcb, KSW_VMCB_NRIP, start + size);
                reads = codeReads = 0;
                CHECK(KswSvmNestedFetchInstruction(&io, &vmcb, bytes, &length) == KSW_NNPT_OK);
                CHECK(length == size && codeReads == expectedWords);
                for (j = 0; j < size; ++j) { CHECK(bytes[j] == (unsigned char)(j + 19U)); }
                if (offset == 0 && size == 3) { baselineReads = reads; }
            }
        }
        KswSvmWrite64(&vmcb, KSW_VMCB_RIP, 0xffff800000000000ULL);
        KswSvmWrite64(&vmcb, KSW_VMCB_NRIP, 0xffff800000000003ULL);
        CHECK(baselineReads > 0 && baselineReads < 100);
        for (j = 1; j <= baselineReads; ++j) {
            reads = 0; failOrdinal = j;
            CHECK(KswSvmNestedFetchInstruction(&io, &vmcb, bytes, &length) == KSW_NNPT_UNREADABLE);
            CHECK(length == 0 && bytes[0] == 0);
        }
        failOrdinal = 0; mutateCode = 1;
        CHECK(KswSvmNestedFetchInstruction(&io, &vmcb, bytes, &length) == KSW_NNPT_RETRY && !length);
        ram[7][0] ^= 0x1000;
        printf("FETCH_3BYTE_PHYSICAL_READS=%u (previous per-byte path=189)\n", baselineReads);
    }
    KswSvmWrite64(&vmcb, KSW_VMCB_RIP, 0xffff800000000ffeULL);
    KswSvmWrite64(&vmcb, KSW_VMCB_NRIP, 0xffff800000001003ULL);
    ram[7][1] = 0;
    CHECK(KswSvmNestedFetchInstruction(&io, &vmcb, bytes, &length) == KSW_NNPT_RETRY && !length && !bytes[0]);
    ram[7][1] = 0x9007;
    ram[4][256] |= 1ULL << 63;
    CHECK(KswSvmNestedFetchInstruction(&io, &vmcb, bytes, &length) == KSW_NNPT_RETRY && !length);
    ram[4][256] &= ~(1ULL << 63);
    KswSvmWrite64(&vmcb, KSW_VMCB_CR4, 0x1020);
    CHECK(KswSvmNestedFetchInstruction(&io, &vmcb, bytes, &length) == KSW_NNPT_UNSUPPORTED);
    KswSvmWrite64(&vmcb, KSW_VMCB_CR4, 0x20); failRead = 1;
    CHECK(KswSvmNestedFetchInstruction(&io, &vmcb, bytes, &length) == KSW_NNPT_UNREADABLE && !length);
    puts("nested fetch source cases passed"); return 0;
}
