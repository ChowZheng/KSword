#include <Windows.h>
#include <stdio.h>
#include <string.h>

#include "ci_hash_selection_production.h"

static ULONG failures;
static ULONG checks;

static VOID Expect(const char* Name, BOOLEAN Passed)
{
    ++checks;
    printf("%s %s\n", Passed ? "PASS" : "FAIL", Name);
    if (!Passed) { ++failures; }
}

static KSW_CI_HASH_CANDIDATE MakeList(ULONG_PTR Address, ULONG Length)
{
    KSW_CI_HASH_CANDIDATE candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.ListGlobal = Address;
    candidate.ChainLength = Length;
    candidate.NameOffset = 16UL;
    candidate.ReferenceRoutine = 0x100000U;
    candidate.ReferenceInstruction = candidate.ReferenceRoutine + Address;
    return candidate;
}

static KSW_RUNTIME_DATA_REFERENCE MakeLock(ULONG_PTR Address, ULONG_PTR Instruction)
{
    KSW_RUNTIME_DATA_REFERENCE reference;
    memset(&reference, 0, sizeof(reference));
    reference.Address = Address;
    reference.RoutineAddress = 0x100000U;
    reference.InstructionAddress = Instruction;
    return reference;
}

static VOID TestLists(VOID)
{
    KSW_CI_HASH_CANDIDATE a = MakeList(0x1000U, 2UL);
    KSW_CI_HASH_CANDIDATE b = MakeList(0x2000U, 2UL);
    KSW_CI_HASH_CANDIDATE c = MakeList(0x3000U, 3UL);
    KSW_CI_HASH_CANDIDATE duplicate = a;
    KSW_CI_HASH_CANDIDATE best;
    ULONG score = 0UL;
    BOOLEAN ambiguous = FALSE;
    ULONG permutation;
    BOOLEAN allPermutationsAmbiguous = TRUE;
    const KSW_CI_HASH_CANDIDATE* permutations[3][3] = {
        { &a, &b, &a }, { &a, &a, &b }, { &b, &a, &a }
    };
    duplicate.ReferenceRoutine += 0x100U;
    duplicate.ReferenceInstruction += 0x100U;
    memset(&best, 0, sizeof(best));

    KswordARKCiHashSelectListCandidate(&a, &best, &score, &ambiguous);
    Expect("first validated list is unique", score == 2UL && best.ListGlobal == a.ListGlobal && !ambiguous);
    KswordARKCiHashSelectListCandidate(&duplicate, &best, &score, &ambiguous);
    Expect("same global and layout across code references is not ambiguous", !ambiguous && score == 2UL);
    KswordARKCiHashSelectListCandidate(&b, &best, &score, &ambiguous);
    Expect("different equal length list is ambiguous", ambiguous);
    KswordARKCiHashSelectListCandidate(&a, &best, &score, &ambiguous);
    Expect("list A B A retains equal length conflict", ambiguous && score == 2UL);
    KswordARKCiHashSelectListCandidate(&c, &best, &score, &ambiguous);
    Expect("strictly longer C clears weaker list conflict", !ambiguous && score == 3UL && best.ListGlobal == c.ListGlobal);
    KswordARKCiHashSelectListCandidate(&b, &best, &score, &ambiguous);
    Expect("shorter list cannot displace unique stronger C", !ambiguous && best.ListGlobal == c.ListGlobal);

    score = 0UL; ambiguous = FALSE; memset(&best, 0, sizeof(best));
    duplicate = a; duplicate.NextOffset = 8UL;
    KswordARKCiHashSelectListCandidate(&a, &best, &score, &ambiguous);
    KswordARKCiHashSelectListCandidate(&duplicate, &best, &score, &ambiguous);
    KswordARKCiHashSelectListCandidate(&a, &best, &score, &ambiguous);
    Expect("same global with conflicting NextOffset remains ambiguous", ambiguous);
    score = 0UL; ambiguous = FALSE; memset(&best, 0, sizeof(best));
    duplicate = a; duplicate.NameOffset = 24UL;
    KswordARKCiHashSelectListCandidate(&a, &best, &score, &ambiguous);
    KswordARKCiHashSelectListCandidate(&duplicate, &best, &score, &ambiguous);
    KswordARKCiHashSelectListCandidate(&a, &best, &score, &ambiguous);
    Expect("same global with conflicting NameOffset remains ambiguous", ambiguous);

    for (permutation = 0UL; permutation < 3UL; ++permutation) {
        ULONG index;
        score = 0UL; ambiguous = FALSE; memset(&best, 0, sizeof(best));
        for (index = 0UL; index < 3UL; ++index) {
            KswordARKCiHashSelectListCandidate(permutations[permutation][index], &best, &score, &ambiguous);
        }
        if (!ambiguous || score != 2UL) { allPermutationsAmbiguous = FALSE; }
    }
    Expect("list duplicate order never changes tied conflict", allPermutationsAmbiguous);
}

static VOID TestLocks(VOID)
{
    KSW_RUNTIME_DATA_REFERENCE a = MakeLock(0x4000U, 0x100010U);
    KSW_RUNTIME_DATA_REFERENCE b = MakeLock(0x5000U, 0x100020U);
    KSW_RUNTIME_DATA_REFERENCE c = MakeLock(0x6000U, 0x100030U);
    KSW_RUNTIME_DATA_REFERENCE duplicate = MakeLock(a.Address, 0x200040U);
    const KSW_RUNTIME_DATA_REFERENCE* best = NULL;
    ULONG score = 0UL;
    BOOLEAN ambiguous = FALSE;
    ULONG permutation;
    BOOLEAN allPermutationsAmbiguous = TRUE;
    const KSW_RUNTIME_DATA_REFERENCE* permutations[3][3] = {
        { &a, &b, &a }, { &a, &a, &b }, { &b, &a, &a }
    };

    KswordARKCiHashSelectLockReference(&a, 8UL, &best, &score, &ambiguous);
    Expect("first scored lock is unique", best == &a && score == 8UL && !ambiguous);
    KswordARKCiHashSelectLockReference(&duplicate, 8UL, &best, &score, &ambiguous);
    Expect("same lock through another code reference is not ambiguous", best == &duplicate && !ambiguous);
    KswordARKCiHashSelectLockReference(&b, 8UL, &best, &score, &ambiguous);
    Expect("different equally scored lock is ambiguous", ambiguous);
    KswordARKCiHashSelectLockReference(&a, 8UL, &best, &score, &ambiguous);
    Expect("lock A B A retains equal score conflict", ambiguous && score == 8UL);
    KswordARKCiHashSelectLockReference(&b, 4UL, &best, &score, &ambiguous);
    Expect("weaker lock cannot clear existing ambiguity", ambiguous && score == 8UL);
    KswordARKCiHashSelectLockReference(&c, 12UL, &best, &score, &ambiguous);
    Expect("strictly stronger C clears weaker lock conflict", !ambiguous && best == &c && score == 12UL);
    KswordARKCiHashSelectLockReference(&a, 8UL, &best, &score, &ambiguous);
    Expect("lower scored lock cannot replace stronger C", !ambiguous && best == &c && score == 12UL);

    best = NULL; score = 0UL; ambiguous = FALSE;
    KswordARKCiHashSelectLockReference(&a, 0UL, &best, &score, &ambiguous);
    Expect("unassociated lock remains unavailable", best == NULL && score == 0UL && !ambiguous);
    for (permutation = 0UL; permutation < 3UL; ++permutation) {
        ULONG index;
        best = NULL; score = 0UL; ambiguous = FALSE;
        for (index = 0UL; index < 3UL; ++index) {
            KswordARKCiHashSelectLockReference(permutations[permutation][index], 8UL, &best, &score, &ambiguous);
        }
        if (!ambiguous || score != 8UL) { allPermutationsAmbiguous = FALSE; }
    }
    Expect("lock duplicate order never changes tied conflict", allPermutationsAmbiguous);
}

int main(void)
{
    TestLists();
    TestLocks();
    printf("checks=%lu failures=%lu\n", checks, failures);
    return failures != 0UL;
}
