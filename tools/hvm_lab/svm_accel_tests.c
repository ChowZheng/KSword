/* Offline fixtures for the production acceleration/provenance policy; no SVM instructions. */
#include <stdio.h>
#include <string.h>
#include "../../KswordARKDriver/src/features/hvm/hvm_svm_accel.h"
#include "../../KswordARKDriver/src/features/hvm/hvm_svm_nested_shadow.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL %u: %s\n", __LINE__, #x); return 1; } } while (0)
static KSW_SVM_VMCB image;
static KSW_SVM_ACCEL accel;
static KSW_NSHADOW shadow;
__declspec(align(4096)) static KSW_SVM_U64 tableWords[8][512];
static KSW_SVM_U64 values[2];
static KSW_NSHADOW_PAGE pages[8];
static unsigned reads, dirty;
static int read_source(void* context, KSW_SVM_U64 address, KSW_SVM_U64* value)
{
    (void)context; ++reads;
    if (address != 0x7000 && address != 0x8000) { return 0; }
    *value = values[address == 0x8000]; return 1;
}
static int stable_source(void* context, KSW_SVM_U64 address)
{
    (void)context; return !(dirty && address == 0x7000);
}
static int test_accel(void)
{
    unsigned inner, svme;
    for (inner = 0; inner < 2; ++inner) {
        for (svme = 0; svme < 2; ++svme) {
            memset(&image, 0, sizeof(image)); memset(&accel, 0, sizeof(accel));
            accel.Enabled = 1; accel.Features = 32U | (1U << 15) | (1U << 16);
            KswSvmWrite32(&image, KSW_VMCB_MISC2, 0x7ff);
            KswSvmWrite64(&image, KSW_VMCB_EFER, 0x1d01); KswSvmWrite64(&image, KSW_VMCB_NP, 1);
            ((unsigned char*)&image)[KSW_VMCB_CS + 3] = 2;
            KswSvmWrite64(&image, KSW_VMCB_INTCTL, KSW_SVM_VGIF_ENABLE | KSW_SVM_VGIF);
            KswSvmAccelApply(&accel, &image, inner, svme ? KSW_SVM_EFER_SVME : 0, 1);
            CHECK((unsigned)KswSvmRead64(&image, KSW_VMCB_MISC2) == (!inner && svme ? 0x7f3U : 0x7ffU));
            CHECK((KswSvmRead64(&image, KSW_VMCB_MISC2) & 0x30U) == 0x30U);
            CHECK(accel.VgifEntries == 0);
            CHECK(accel.VlsActive == (unsigned)(!inner && svme));
            CHECK(KswSvmAccelRestore(&accel, &image));
            CHECK((unsigned)KswSvmRead64(&image, KSW_VMCB_MISC2) == 0x7ffU && !KswSvmRead64(&image, 0xb8));
            CHECK(!KswSvmAccelRestore(&accel, &image));
        }
    }
    CHECK(KswSvmAccelCleanPrepare(&accel, &image, 0x1000) == 0);
    KswSvmAccelComplete(&accel, KSW_SVM_EXIT_CPUID);
    CHECK(KswSvmAccelCleanPrepare(&accel, &image, 0x1000) == 0x17);
    KswSvmAccelComplete(&accel, KSW_SVM_EXIT_CPUID); ++accel.MapsGeneration;
    CHECK(!(KswSvmAccelCleanPrepare(&accel, &image, 0x1000) & 2U));
    KswSvmAccelComplete(&accel, KSW_SVM_EXIT_INVALID);
    CHECK(KswSvmAccelCleanPrepare(&accel, &image, 0x1000) == 0);
    KswSvmAccelComplete(&accel, KSW_SVM_EXIT_CPUID);
    CHECK(KswSvmAccelCleanPrepare(&accel, &image, 0x2000) == 0);
    return 0;
}
static int test_provenance(void)
{
    KSW_NMMU_RESULT mapping = {0};
    unsigned i;
    memset(&shadow, 0, sizeof(shadow));
    for (i = 0; i < 8; ++i) { pages[i].Words = tableWords[i]; pages[i].Physical = 0x100000 + 4096ULL * i; }
    CHECK(!KswSvmNestedShadowInitialize(&shadow, pages, 8, 45));
    values[0] = 0x3067; values[1] = 0x4067;
    mapping.Status = KSW_NNPT_OK; mapping.Epoch = shadow.Epoch;
    mapping.Inner.Complete = mapping.Outer.Complete = 1;
    mapping.Inner.Permissions = mapping.Outer.Permissions = 7; mapping.Inner.Count = 1;
    for (i = 0; i < 2; ++i) {
        mapping.Gpa = mapping.Inner.InputAddress = 0x1000 + 0x200000ULL * i;
        mapping.Inner.Address = mapping.Outer.InputAddress = mapping.Outer.Address = 0x3000 + 4096ULL * i;
        mapping.Leaf = values[i]; mapping.Inner.EntryAddress[0] = 0x7000 + 4096ULL * i; mapping.Inner.EntryValue[0] = values[i];
        CHECK(!KswSvmNestedShadowInstall(&shadow, &mapping));
    }
    reads = dirty = 0;
    CHECK(KswSvmNestedShadowSourcesVerify(&shadow, read_source, NULL, stable_source, NULL) && reads == 2);
    CHECK(KswSvmNestedShadowSourcesVerify(&shadow, read_source, NULL, stable_source, NULL) && reads == 2);
    dirty = 1; values[0] ^= 2;
    CHECK(KswSvmNestedShadowSourcesVerify(&shadow, read_source, NULL, stable_source, NULL));
    CHECK(shadow.Epoch == 2 && shadow.DependencyRetirements == 1);
    CHECK(!tableWords[3][1] && tableWords[4][1] == 0x4067);
    CHECK(shadow.FlushPending && !shadow.SourceProven[0] && shadow.SourceProven[1]);
    return 0;
}
int main(void)
{
    if (test_accel() || test_provenance()) { return 1; }
    printf("SVM_ACCEL_CHECKS=%u RESULT=PASS (no hardware)\n", checks); return 0;
}
