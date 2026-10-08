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
static int test_group_reclaim(void)
{
    KSW_NMMU_RESULT mapping = {0};
    unsigned i, source, target;
    KSW_SVM_U64 rootEntry, pdptEntry, epoch;
    memset(&shadow, 0, sizeof(shadow)); memset(tableWords, 0, sizeof(tableWords));
    for (i = 0; i < 5; ++i) { pages[i].Words = tableWords[i]; pages[i].Physical = 0x100000 + 4096ULL * i; }
    CHECK(KswSvmNestedShadowInitialize(&shadow, pages, 5, 45) == KSW_NSHADOW_OK);
    mapping.Status = KSW_NNPT_OK; mapping.Inner.Complete = mapping.Outer.Complete = 1;
    mapping.Inner.Permissions = mapping.Outer.Permissions = 7; mapping.Inner.Count = 1;
    for (i = 0; i < 2; ++i) {
        mapping.Epoch = shadow.Epoch; mapping.Gpa = mapping.Inner.InputAddress = 0x1000 + 0x200000ULL * i;
        mapping.Inner.Address = mapping.Outer.InputAddress = mapping.Outer.Address = 0x3000 + 4096ULL * i;
        mapping.Leaf = mapping.Inner.EntryValue[0] = (0x3000 + 4096ULL * i) | 0x67;
        mapping.Inner.EntryAddress[0] = 0x7000 + 4096ULL * i;
        CHECK(KswSvmNestedShadowInstall(&shadow, &mapping) == KSW_NSHADOW_OK);
    }
    CHECK(shadow.Used == 5 && shadow.SourceCount == 2);
    rootEntry = tableWords[0][0]; pdptEntry = tableWords[1][0]; epoch = shadow.Epoch;
    tableWords[2][0] |= 0x20;
    mapping.Gpa = mapping.Inner.InputAddress = 0x401000;
    CHECK(KswSvmNestedShadowInstall(&shadow, &mapping) == KSW_NSHADOW_FULL);
    CHECK(shadow.Epoch == epoch && tableWords[2][2] == 0);
    CHECK(KswSvmNestedShadowReclaim(&shadow, mapping.Gpa) == KSW_NSHADOW_OK);
    CHECK(shadow.Epoch == epoch + 1 && shadow.FlushPending && shadow.Used == 5);
    CHECK(tableWords[0][0] == rootEntry && tableWords[1][0] == pdptEntry);
    CHECK(tableWords[3][1] == 0x3067 && tableWords[2][1] == 0);
    CHECK(tableWords[2][2] == (pages[4].Physical | 7) && tableWords[4][1] == 0);
    CHECK(shadow.SourceCount == 1 && shadow.SourceAddress[0] == 0x7000);
    CHECK(KswSvmNestedShadowInstall(&shadow, &mapping) == KSW_NSHADOW_STALE);
    mapping.Epoch = shadow.Epoch;
    CHECK(KswSvmNestedShadowInstall(&shadow, &mapping) == KSW_NSHADOW_OK);
    for (i = 0; i < KSW_NSHADOW_SOURCE_WORDS * 2U + 17U; ++i) {
        for (target = 0; target < 3 && tableWords[2][target]; ++target) {}
        CHECK(target < 3);
        mapping.Gpa = mapping.Inner.InputAddress = 0x1000 + 0x200000ULL * target;
        CHECK(KswSvmNestedShadowReclaim(&shadow, mapping.Gpa) == KSW_NSHADOW_OK);
        mapping.Epoch = shadow.Epoch;
        mapping.Inner.EntryAddress[0] = 0x10000 + 8ULL * i;
        CHECK(KswSvmNestedShadowInstall(&shadow, &mapping) == KSW_NSHADOW_OK);
        CHECK(shadow.Used == 5 && shadow.SourceCount <= 2 && !shadow.SourceUntracked);
        for (source = 0; source < shadow.SourceCount; ++source) { CHECK(shadow.SourceId[source] < KSW_NSHADOW_SOURCE_WORDS); }
    }
    CHECK(tableWords[0][0] == rootEntry && tableWords[1][0] == pdptEntry);
    epoch = shadow.Epoch; shadow.SourceUntracked = 1;
    CHECK(KswSvmNestedShadowReclaim(&shadow, 0x601000) == KSW_NSHADOW_FULL && shadow.Epoch == epoch);
    shadow.SourceUntracked = 0;
    CHECK(KswSvmNestedShadowReclaim(&shadow, 0x40001000) == KSW_NSHADOW_FULL && shadow.Epoch == epoch);
    CHECK(KswSvmNestedShadowReset(&shadow) == KSW_NSHADOW_OK && shadow.Used == 1 && !shadow.SourceCount);
    for (i = 0; i < KSW_NSHADOW_SOURCE_WORDS / 64U; ++i) { CHECK(!shadow.SourceIdsUsed[i]); }
    CHECK(KswSvmNestedShadowBudget(16384, 1282, 32, 256, 64, 256) == 26);
    CHECK(KswSvmNestedShadowBudget(16384, 1282, 16, 256, 32, 256) == 32);
    CHECK(KswSvmNestedShadowBudget(16384, 1282, 64, 256, 128, 256) == 0);
    CHECK(KswSvmNestedShadowBudget(16384, 16385, 1, 256, 2, 256) == 0);
    CHECK(KswSvmNestedShadowBudget(16384, 0, ~0U, 256, 2, 256) == 0);
    return 0;
}
int main(void)
{
    if (test_accel() || test_provenance() || test_group_reclaim()) { return 1; }
    printf("SVM_ACCEL_CHECKS=%u RESULT=PASS (no hardware)\n", checks); return 0;
}
