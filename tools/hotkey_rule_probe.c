/* Offline PE image adapter for the production hotkey rules. Input bytes are never executed. */
#include "../KswordARKDriver/src/features/keyboard/keyboard_rule_proof.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_MAX_FILE (64U * 1024U * 1024U)
#define PROBE_MAX_IMAGE (128U * 1024U * 1024U)

typedef struct PROBE_IMAGE {
    unsigned char* Bytes;
    uint32_t Size;
} PROBE_IMAGE;

static uint16_t u16(const unsigned char* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t u32(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int within(size_t limit, size_t offset, size_t length)
{
    return offset <= limit && length <= limit - offset;
}

static int load_image(const char* path, PROBE_IMAGE* output)
{
    FILE* file = NULL;
    unsigned char* raw = NULL;
    unsigned char* mapped = NULL;
    long file_length;
    size_t length, nt, optional, sections, index;
    uint16_t section_count, optional_size;
    uint32_t image_size, header_size;
    int ok = 0;

    memset(output, 0, sizeof(*output));
    file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0 || (file_length = ftell(file)) < 0 ||
        (unsigned long)file_length > PROBE_MAX_FILE || fseek(file, 0, SEEK_SET) != 0)
        goto done;
    length = (size_t)file_length;
    raw = (unsigned char*)malloc(length ? length : 1);
    if (!raw || fread(raw, 1, length, file) != length ||
        !within(length, 0, 0x40) || u16(raw) != 0x5A4D) goto done;
    nt = u32(raw + 0x3C);
    if (!within(length, nt, 24) || u32(raw + nt) != 0x00004550 ||
        u16(raw + nt + 4) != 0x8664) goto done;
    section_count = u16(raw + nt + 6);
    optional_size = u16(raw + nt + 20);
    optional = nt + 24;
    if (section_count == 0 || section_count > 96 || optional_size < 64 ||
        !within(length, optional, optional_size) || u16(raw + optional) != 0x20B) goto done;
    image_size = u32(raw + optional + 56);
    header_size = u32(raw + optional + 60);
    sections = optional + optional_size;
    if (image_size < header_size || image_size > PROBE_MAX_IMAGE ||
        header_size == 0 || !within(length, 0, header_size) ||
        !within(length, sections, (size_t)section_count * 40)) goto done;
    mapped = (unsigned char*)calloc(1, image_size);
    if (!mapped) goto done;
    memcpy(mapped, raw, header_size);
    for (index = 0; index < section_count; ++index)
    {
        const unsigned char* section = raw + sections + index * 40;
        uint32_t destination = u32(section + 12);
        uint32_t raw_size = u32(section + 16);
        uint32_t raw_offset = u32(section + 20);
        if (!within(image_size, destination, raw_size) ||
            (raw_size && !within(length, raw_offset, raw_size))) goto done;
        if (raw_size) memcpy(mapped + destination, raw + raw_offset, raw_size);
    }
    output->Bytes = mapped;
    output->Size = image_size;
    mapped = NULL;
    ok = 1;

done:
    free(mapped);
    free(raw);
    if (file) fclose(file);
    return ok;
}

static int read_image(void* context, uint32_t rva, void* output, size_t size)
{
    const PROBE_IMAGE* image = (const PROBE_IMAGE*)context;
    if (!image || !output || !within(image->Size, rva, size)) return 0;
    memcpy(output, image->Bytes + rva, size);
    return 1;
}

static void* alloc_image(void* context, size_t size)
{
    (void)context;
    return malloc(size ? size : 1);
}

static void free_image(void* context, void* pointer)
{
    (void)context;
    free(pointer);
}

static const char* error_name(uint32_t error)
{
    static const char* names[] = {"ok", "invalid_image", "resource", "unregister",
        "remove", "register", "insert", "layout", "ambiguous", "budget"};
    return error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

#define EMIT_FIELD(name) printf("\"" #name "\":%u,", result->name)

static void emit_result(int matched, const KSW_HOTKEY_RULE_RESULT* result)
{
    uint32_t index, byte;
    printf("{\"Matched\":%s,\"ErrorName\":\"%s\",", matched ? "true" : "false",
        error_name(result->Error));
    EMIT_FIELD(Error);
    EMIT_FIELD(RuleVersion);
    EMIT_FIELD(TableOffset);
    EMIT_FIELD(TableRva);
    EMIT_FIELD(CacheOffset);
    EMIT_FIELD(CacheRva);
    EMIT_FIELD(BucketCount);
    EMIT_FIELD(ObjectSize);
    EMIT_FIELD(OwnerOffset);
    EMIT_FIELD(CallbackOffset);
    EMIT_FIELD(WindowOffset);
    EMIT_FIELD(WindowRepresentation);
    EMIT_FIELD(DestinationOffset);
    EMIT_FIELD(DestinationWidth);
    EMIT_FIELD(ModifiersOffset);
    EMIT_FIELD(FlagsOffset);
    EMIT_FIELD(VkOffset);
    EMIT_FIELD(IdOffset);
    EMIT_FIELD(NextOffset);
    EMIT_FIELD(ChildOffset);
    EMIT_FIELD(ModifiersWidth);
    EMIT_FIELD(FlagsWidth);
    EMIT_FIELD(VkWidth);
    EMIT_FIELD(IdWidth);
    EMIT_FIELD(WindowObjectHandleOffset);
    EMIT_FIELD(RemoveRva);
    EMIT_FIELD(ScanRva);
    EMIT_FIELD(UnregisterRva);
    EMIT_FIELD(RegisterRva);
    EMIT_FIELD(InsertRva);
    EMIT_FIELD(OwnerGetterRva);
    EMIT_FIELD(OwnerGetterIndirect);
    EMIT_FIELD(OwnerGetterDepth);
    EMIT_FIELD(EnterCritRva);
    EMIT_FIELD(LeaveCritRva);
    EMIT_FIELD(ValidateHwndRva);
    EMIT_FIELD(AssignmentRva);
    EMIT_FIELD(SessionGetterSlotRva);
    EMIT_FIELD(SessionGetterProvider);
    EMIT_FIELD(AnalyzedFunctionCount);
    EMIT_FIELD(EvidenceCount);
    printf("\"AnalyzedFunctions\":[");
    for (index = 0; index < result->AnalyzedFunctionCount &&
         index < KSW_HOTKEY_RULE_MAX_FUNCTIONS; ++index)
    {
        if (index) putchar(',');
        printf("%u", result->AnalyzedFunctions[index]);
    }
    printf("],");
    printf("\"Evidence\":[");
    for (index = 0; index < result->EvidenceCount &&
         index < KSW_HOTKEY_RULE_MAX_EVIDENCE; ++index)
    {
        const KSW_HOTKEY_RULE_EVIDENCE* item = &result->Evidence[index];
        if (index) putchar(',');
        printf("{\"Rva\":%u,\"Length\":%u,\"Bytes\":\"", item->Rva, item->Length);
        for (byte = 0; byte < item->Length && byte < sizeof(item->Bytes); ++byte)
            printf("%02X", item->Bytes[byte]);
        printf("\"}");
    }
    printf("]}\n");
}

static int parse_rva(const char* text, uint32_t* value)
{
    char* end;
    unsigned long number = strtoul(text, &end, 0);
    if (!text[0] || *end || number > UINT32_MAX) return 0;
    *value = (uint32_t)number;
    return 1;
}

static void print_value(KSW_FLOW_VALUE value)
{
    printf("(%u,%u,%u,%u,%u,%" PRId64 ")", value.Kind, value.Width,
        value.Tag, value.Aux, value.Source, value.Value);
}

static void dump_analysis(const char* stage, const KSW_RULE_FUNCTION* function,
    const KSW_FLOW_ANALYSIS* analysis, uint32_t insert_rva)
{
    uint32_t index, argument, cell;
    printf("STAGE %s entry=0x%X instructions=%u\n", stage, function->EntryRva,
        function->InsnCount);
    for (index = 0; index < function->InsnCount; ++index)
    {
        const KSW_RULE_INSN* call = &function->Insns[index];
        const KSW_FLOW_STATE* state = &analysis->States[index];
        if (!state->Seen || call->Kind != KSW_RULE_CALL) continue;
        printf("CALL rva=0x%X target=0x%X direct=%u crit=%u nodeAlive=%u nullArgs=0x%X",
            call->Rva, call->Target, KswHkCallDirect(call), state->Crit,
            state->NodeAlive, state->NullArgs);
        for (argument = 1; argument <= 6; ++argument)
        {
            printf(" a%u=", argument);
            print_value(KswHkCallArgument(state, argument));
        }
        putchar('\n');
        if (insert_rva == 0 || call->Target != insert_rva) continue;
        for (cell = 0; cell < KSW_FLOW_NODE_CELLS; ++cell)
        {
            const KSW_FLOW_CELL* item = &state->Node[cell];
            if (!item->Used) continue;
            printf("NODE_CELL offset=0x%X width=%u value=", (unsigned)item->Offset,
                item->Width);
            print_value(item->V);
            putchar('\n');
        }
    }
}

static void dump_watch(const char* stage, const KSW_RULE_FUNCTION* function,
    const KSW_FLOW_ANALYSIS* analysis, const KSW_FLOW_MODEL* model, uint32_t rva)
{
    int location;
    uint32_t reg;
    const KSW_FLOW_STATE* state;
    const KSW_RULE_INSN* instruction;
    if (rva == 0) return;
    location = KswFlowIndex(function, rva);
    if (location < 0 || !analysis->States || !analysis->States[location].Seen)
    {
        printf("WATCH stage=%s rva=0x%X unreachable\n", stage, rva);
        return;
    }
    state = &analysis->States[location];
    instruction = &function->Insns[location];
    printf("WATCH stage=%s rva=0x%X kind=%u guards=0x%X lastGuard=0x%X nullArgs=0x%X nonNullArgs=0x%X lastTestReg=%u lastZeroKnown=%u lastZero=%u lastValue=",
        stage, rva, instruction->Kind, state->Guards, state->LastGuard,
        state->NullArgs, state->NonNullArgs,
        state->LastTestReg, state->LastZeroKnown, state->LastZero);
    print_value(state->LastValue);
    if (instruction->OperandCount > 0)
    {
        printf(" op0=");
        print_value(KswFlowOperand(state, &instruction->Operands[0], model));
    }
    if (instruction->OperandCount > 1)
    {
        printf(" source=");
        print_value(KswFlowOperand(state, &instruction->Operands[1], model));
    }
    for (reg = 1; reg < 18; ++reg)
    {
        if (state->Regs[reg].Kind == KSW_V_UNKNOWN) continue;
        printf(" r%u=", reg);
        print_value(state->Regs[reg]);
    }
    putchar('\n');
}

static int debug_flow(const KSW_RULE_IMAGE* full, uint32_t entry, uint32_t mode,
    uint32_t allocation, uint32_t insertion, uint32_t watch)
{
    KSW_FLOW_MODEL model = {0};
    KSW_FLOW_MODEL initial_model;
    KSW_RULE_FUNCTION function = {0};
    KSW_FLOW_ANALYSIS basis = {0}, focus = {0};
    KSW_FLOW_STATE* initial = NULL;
    int allocation_index, ok = 0;
    uint32_t range;

    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "EnterCrit", &model.EnterSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "ValidateHwnd", &model.ValidateSlot);
    if (!KswRuleFindImportSlot(full, "win32k.sys", "W32GetUserSessionState", &model.SessionSlot))
        (void)KswRuleFindImportSlot(full, "win32ksgd.sys", "SGDGetUserSessionState", &model.SessionSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "Win32AllocPoolZInit", &model.AllocSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "gptiCurrent", &model.OwnerDataSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "HMAssignmentLock", &model.AssignmentSlot);
    if (!KswRuleFindImportSlot(full, "win32kbase.sys", "UserSessionSwitchLeaveCrit", &model.LeaveSlot))
        (void)KswRuleFindImportSlot(full, "win32kbase.sys", "LeaveCrit", &model.LeaveSlot);
    (void)KswRuleFindImportSlot(full, "ntoskrnl.exe", "KeBugCheckEx", &model.BugcheckSlot);
    printf("MODEL enter=0x%X leave=0x%X validate=0x%X session=0x%X alloc=0x%X ownerData=0x%X bugcheck=0x%X\n",
        model.EnterSlot, model.LeaveSlot, model.ValidateSlot, model.SessionSlot,
        model.AllocSlot, model.OwnerDataSlot, model.BugcheckSlot);
    initial_model = model;
    if (!KswRuleLoadFunction(full, entry, &function))
    {
        fprintf(stderr, "debug_load_function_failed:0x%X\n", entry);
        return 1;
    }
    printf("FUNCTION entry=0x%X ranges=%u instructions=%u\n", entry,
        function.RangeCount, function.InsnCount);
    for (range = 0; range < function.RangeCount; ++range)
        printf("RANGE begin=0x%X end=0x%X\n", function.Ranges[range].Begin,
            function.Ranges[range].End);
    for (range = 0; range < function.InsnCount; ++range)
    {
        const KSW_RULE_INSN* insn = &function.Insns[range];
        if ((insn->Kind == KSW_RULE_JMP || insn->Kind == KSW_RULE_JCC) &&
            KswFlowIndex(&function, insn->Target) < 0)
            printf("UNRESOLVED_EDGE rva=0x%X target=0x%X kind=%u\n", insn->Rva,
                insn->Target, insn->Kind);
    }
    initial = (KSW_FLOW_STATE*)full->Alloc(full->Context, sizeof(*initial));
    if (!initial) goto done;
    KswFlowInit(initial);
    if (mode == 1) initial->Regs[KSW_REG_RCX].Kind = KSW_V_WNDOBJ;
    if (!KswFlowRun(full, &function, &model, 0, initial, &basis))
    {
        uint32_t seen = 0, item;
        fprintf(stderr, "debug_initial_flow_failed:error=%u\n", basis.Error);
        if (basis.States)
        {
            for (item = 0; item < function.InsnCount; ++item)
                if (basis.States[item].Seen) ++seen;
            printf("PARTIAL seen=%u/%u\n", seen, function.InsnCount);
        }
        if (basis.States) dump_analysis("partial", &function, &basis, insertion);
        goto done;
    }
    dump_analysis("basis", &function, &basis, insertion);
    dump_watch("basis", &function, &basis, &model, watch);
    if (allocation != 0 && insertion != 0)
    {
        KSW_HK_RULE_CONTEXT context = {0};
        KSW_HOTKEY_RULE_RESULT insertion_result = {0};
        int insert_match;
        allocation_index = KswFlowIndex(&function, allocation);
        if (allocation_index < 0 || !basis.States[allocation_index].Seen)
        {
            fprintf(stderr, "debug_allocation_not_reachable:0x%X\n", allocation);
            goto done;
        }
        model.AllocRva = allocation;
        model.InsertRva = insertion;
        if (!KswFlowRun(full, &function, &model, (uint32_t)allocation_index,
                &basis.States[allocation_index], &focus))
        {
            fprintf(stderr, "debug_focus_flow_failed:error=%u\n", focus.Error);
            if (focus.States) dump_analysis("partial-focus", &function, &focus, insertion);
            goto done;
        }
        dump_analysis("focus", &function, &focus, insertion);
        dump_watch("focus", &function, &focus, &model, watch);
        context.Full = full;
        context.Model = model;
        insert_match = KswHkProofInsert(&context, insertion, &insertion_result);
        printf("INSERT_PROOF matched=%u error=%u tableOffset=0x%X vkOffset=0x%X nextOffset=0x%X buckets=%u\n",
            insert_match, context.Error,
            insertion_result.TableOffset, insertion_result.VkOffset,
            insertion_result.NextOffset, insertion_result.BucketCount);
        if (mode == 1)
        {
            KSW_HOTKEY_RULE_RESULT register_result = {0};
            int register_match;
            KswFlowZero(&context, sizeof(context));
            context.Full = full;
            context.Model = initial_model;
            register_match = KswHkProofRegister(&context, &register_result);
            printf("REGISTER_PROOF matched=%u error=%u objectSize=%u windowOffset=0x%X windowSourceOffset=0x%X evidence=%u\n",
                register_match, context.Error, register_result.ObjectSize,
                register_result.WindowOffset, register_result.WindowObjectHandleOffset,
                register_result.EvidenceCount);
        }
    }
    ok = 1;
done:
    KswFlowFree(&focus);
    KswFlowFree(&basis);
    KswRuleFreeFunction(full, &function);
    if (initial) full->Free(full->Context, initial);
    return ok ? 0 : 1;
}

static int selftest_values(void)
{
    KSW_FLOW_STATE left, right;
    int distinct_rejected, equal_preserved, nullable_repeated_zero;
    KswFlowInit(&left);
    KswFlowInit(&right);
    left.Regs[KSW_REG_RAX] = KswFlowConst(0);
    left.Regs[KSW_REG_RAX].Kind = KSW_V_TABLE_SLOT;
    left.Regs[KSW_REG_RAX].Tag = 1;
    left.Regs[KSW_REG_RAX].Value = 0x100;
    right.Regs[KSW_REG_RAX] = left.Regs[KSW_REG_RAX];
    (void)KswFlowJoin(&left, &right);
    equal_preserved = left.Regs[KSW_REG_RAX].Kind == KSW_V_TABLE_SLOT &&
        left.Regs[KSW_REG_RAX].Value == 0x100;
    right.Regs[KSW_REG_RAX].Value = 0x200;
    (void)KswFlowJoin(&left, &right);
    distinct_rejected = left.Regs[KSW_REG_RAX].Kind == KSW_V_UNKNOWN;
    KswFlowInit(&left);
    KswFlowInit(&right);
    left.Regs[KSW_REG_R14] = KswFlowConst(0);
    left.Regs[KSW_REG_R14].Kind = KSW_V_WNDOBJ;
    left.Regs[KSW_REG_R14].Tag = 1;
    right.Regs[KSW_REG_R14] = KswFlowConst(0);
    right.NullArgs = (1U << 1);
    (void)KswFlowJoin(&left, &right);
    (void)KswFlowJoin(&left, &right);
    nullable_repeated_zero = left.Regs[KSW_REG_R14].Kind == KSW_V_NULL_WNDOBJ;
    printf("{\"LinkEqualPreserved\":%s,\"LinkDistinctRejected\":%s,\"NullableRepeatedZeroPreserved\":%s}\n",
        equal_preserved ? "true" : "false", distinct_rejected ? "true" : "false",
        nullable_repeated_zero ? "true" : "false");
    return 0;
}

static int debug_slot(const KSW_RULE_IMAGE* full, const KSW_RULE_IMAGE* base,
    uint32_t entry, uint32_t object_size, uint32_t next_offset,
    uint32_t flags_offset, uint32_t flags_width, uint32_t child_offset)
{
    KSW_HK_RULE_CONTEXT context = {0};
    KSW_HOTKEY_RULE_RESULT result = {0};
    int matched;
    context.Full = full;
    context.Base = base;
    result.ObjectSize = object_size;
    result.NextOffset = next_offset;
    result.FlagsOffset = flags_offset;
    result.FlagsWidth = flags_width;
    result.ChildOffset = child_offset;
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "EnterCrit", &context.Model.EnterSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "ValidateHwnd", &context.Model.ValidateSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "Win32AllocPoolZInit", &context.Model.AllocSlot);
    if (!KswRuleFindImportSlot(full, "win32k.sys", "W32GetUserSessionState", &context.Model.SessionSlot))
        (void)KswRuleFindImportSlot(full, "win32ksgd.sys", "SGDGetUserSessionState", &context.Model.SessionSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "gptiCurrent", &context.Model.OwnerDataSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "HMAssignmentLock", &context.Model.AssignmentSlot);
    if (!KswRuleFindImportSlot(full, "win32kbase.sys", "UserSessionSwitchLeaveCrit", &context.Model.LeaveSlot))
        (void)KswRuleFindImportSlot(full, "win32kbase.sys", "LeaveCrit", &context.Model.LeaveSlot);
    (void)KswRuleFindImportSlot(full, "ntoskrnl.exe", "KeBugCheckEx", &context.Model.BugcheckSlot);
    matched = KswHkProofSlot(&context, entry, &result);
    printf("{\"SlotMatched\":%s,\"Error\":%u,\"ObjectSize\":%u,"
           "\"NextOffset\":%u,\"FlagsOffset\":%u,\"FlagsWidth\":%u,"
           "\"ChildOffset\":%u,\"RemoveRva\":%u,\"EvidenceCount\":%u}\n",
        matched ? "true" : "false", context.Error, result.ObjectSize,
        result.NextOffset, result.FlagsOffset, result.FlagsWidth,
        result.ChildOffset, result.RemoveRva, result.EvidenceCount);
    return 0;
}

static int debug_assignment(const KSW_RULE_IMAGE* full, const KSW_RULE_IMAGE* base)
{
    KSW_HK_RULE_CONTEXT context = {0};
    uint32_t base_rva = 0;
    int matched;
    context.Full = full;
    context.Base = base;
    matched = KswHkProofAssignment(&context, &base_rva);
    printf("{\"AssignmentMatched\":%s,\"Error\":%u,\"BaseRva\":%u}\n",
        matched ? "true" : "false", context.Error, base_rva);
    return 0;
}

static int debug_register(const KSW_RULE_IMAGE* full, const KSW_RULE_IMAGE* base)
{
    KSW_HK_RULE_CONTEXT context = {0};
    KSW_HOTKEY_RULE_RESULT result = {0};
    uint32_t assignment = 0;
    int matched;
    context.Full = full;
    context.Base = base;
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "EnterCrit", &context.Model.EnterSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "ValidateHwnd", &context.Model.ValidateSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "Win32AllocPoolZInit", &context.Model.AllocSlot);
    if (!KswRuleFindImportSlot(full, "win32k.sys", "W32GetUserSessionState", &context.Model.SessionSlot))
        (void)KswRuleFindImportSlot(full, "win32ksgd.sys", "SGDGetUserSessionState", &context.Model.SessionSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "gptiCurrent", &context.Model.OwnerDataSlot);
    if (KswHkProofAssignment(&context, &assignment))
        (void)KswRuleFindImportSlot(full, "win32kbase.sys", "HMAssignmentLock", &context.Model.AssignmentSlot);
    if (!KswRuleFindImportSlot(full, "win32kbase.sys", "UserSessionSwitchLeaveCrit", &context.Model.LeaveSlot))
        (void)KswRuleFindImportSlot(full, "win32kbase.sys", "LeaveCrit", &context.Model.LeaveSlot);
    (void)KswRuleFindImportSlot(full, "ntoskrnl.exe", "KeBugCheckEx", &context.Model.BugcheckSlot);
    matched = KswHkProofRegister(&context, &result);
    printf("{\"RegisterMatched\":%s,\"Error\":%u,\"OwnerGetterRva\":%u,"
           "\"OwnerGetterIndirect\":%u,\"OwnerGetterDepth\":%u,"
           "\"WindowRepresentation\":%u,\"WindowOffset\":%u,"
           "\"WindowObjectHandleOffset\":%u,\"CacheOffset\":%u,"
           "\"TableOffset\":%u,\"BucketCount\":%u,\"ObjectSize\":%u}\n",
        matched ? "true" : "false", context.Error, result.OwnerGetterRva,
        result.OwnerGetterIndirect, result.OwnerGetterDepth,
        result.WindowRepresentation, result.WindowOffset,
        result.WindowObjectHandleOffset, result.CacheOffset,
        result.TableOffset, result.BucketCount, result.ObjectSize);
    return 0;
}

static int debug_owner(const KSW_RULE_IMAGE* full, uint32_t entry, uint32_t depth)
{
    KSW_HK_RULE_CONTEXT context = {0};
    KSW_FLOW_VALUE input = KswFlowConst(depth), output = KswFlowUnknown();
    uint32_t index;
    int matched;
    context.Full = full;
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "gptiCurrent", &context.Model.OwnerDataSlot);
    (void)KswRuleFindImportSlot(full, "ntoskrnl.exe", "KeBugCheckEx", &context.Model.BugcheckSlot);
    input.Kind = depth ? KSW_V_OWNER : KSW_V_CALL;
    input.Width = 8;
    input.Aux = entry;
    input.Tag = 0;
    matched = KswHkCanonicalOwner(&context, input, &output);
    printf("{\"OwnerMatched\":%s,\"Error\":%u,\"Kind\":%u,"
           "\"CanonicalRva\":%u,\"CanonicalKind\":%u,\"Depth\":%" PRId64 ","
           "\"AliasCount\":%u,\"Aliases\":[",
        matched ? "true" : "false", context.Error, output.Kind,
        output.Aux, output.Tag, output.Value, context.Model.OwnerAliasCount);
    for (index = 0; index < context.Model.OwnerAliasCount && index < 8; ++index)
    {
        const KSW_FLOW_OWNER_ALIAS* alias = &context.Model.OwnerAliases[index];
        if (index) putchar(',');
        printf("{\"EntryRva\":%u,\"CanonicalRva\":%u,\"Kind\":%u,\"Depth\":%u}",
            alias->EntryRva, alias->CanonicalRva, alias->Kind, alias->Depth);
    }
    printf("]}\n");
    return 0;
}

static int debug_scan(const KSW_RULE_IMAGE* full, const KSW_RULE_IMAGE* base,
    uint32_t entry, uint32_t watch)
{
    KSW_HK_RULE_CONTEXT context = {0};
    KSW_HOTKEY_RULE_RESULT layout = {0};
    KSW_RULE_FUNCTION function = {0};
    KSW_FLOW_ANALYSIS basis = {0}, scan = {0};
    KSW_FLOW_STATE* initial = NULL;
    KSW_FLOW_MODEL model = {0};
    uint32_t assignment = 0, anchor = 0, index;
    int register_ok, load_ok = 0, basis_ok = 0, loop_ok = 0, scan_ok = 0;
    context.Full = full;
    context.Base = base;
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "EnterCrit", &context.Model.EnterSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "ValidateHwnd", &context.Model.ValidateSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "Win32AllocPoolZInit", &context.Model.AllocSlot);
    if (!KswRuleFindImportSlot(full, "win32k.sys", "W32GetUserSessionState", &context.Model.SessionSlot))
        (void)KswRuleFindImportSlot(full, "win32ksgd.sys", "SGDGetUserSessionState", &context.Model.SessionSlot);
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "gptiCurrent", &context.Model.OwnerDataSlot);
    if (KswHkProofAssignment(&context, &assignment))
        (void)KswRuleFindImportSlot(full, "win32kbase.sys", "HMAssignmentLock", &context.Model.AssignmentSlot);
    if (!KswRuleFindImportSlot(full, "win32kbase.sys", "UserSessionSwitchLeaveCrit", &context.Model.LeaveSlot))
        (void)KswRuleFindImportSlot(full, "win32kbase.sys", "LeaveCrit", &context.Model.LeaveSlot);
    (void)KswRuleFindImportSlot(full, "ntoskrnl.exe", "KeBugCheckEx", &context.Model.BugcheckSlot);
    register_ok = KswHkProofRegister(&context, &layout);
    printf("SCAN_SETUP register=%d owner=0x%X/%u/%u window=%u/0x%X table=0x%X cache=0x%X\n",
        register_ok, layout.OwnerGetterRva, layout.OwnerGetterIndirect,
        layout.OwnerGetterDepth, layout.WindowRepresentation, layout.WindowOffset,
        layout.TableOffset, layout.CacheOffset);
    if (!register_ok) return 1;
    load_ok = KswHkLoad(&context, entry, &function);
    if (!load_ok) goto done;
    initial = (KSW_FLOW_STATE*)full->Alloc(full->Context, sizeof(*initial));
    if (!initial) goto done;
    KswFlowInit(initial);
    initial->Regs[KSW_REG_RCX].Kind = KSW_V_WNDOBJ;
    model = context.Model;
    model.Mode = 0;
    model.RemoveSignature = 1;
    model.TableOffset = layout.TableOffset;
    model.NextOffset = layout.NextOffset;
    model.OwnerOffset = layout.OwnerOffset;
    model.WindowOffset = layout.WindowOffset;
    model.IdOffset = layout.IdOffset;
    model.OwnerSourceRva = layout.OwnerGetterRva;
    model.OwnerSourceKind = layout.OwnerGetterIndirect;
    model.OwnerSourceDepth = layout.OwnerGetterDepth;
    model.WindowRepresentation = layout.WindowRepresentation;
    basis_ok = KswFlowRun(full, &function, &model, 0, initial, &basis);
    if (!basis_ok) goto done;
    loop_ok = KswHkProofLoop(&context, &function, &basis, initial, &model, &layout, &anchor);
    if (!loop_ok) goto done;
    model.Mode = 2;
    printf("SCAN_MODEL signature=%u ownerOffset=0x%X windowOffset=0x%X idOffset=0x%X representation=%u ownerSource=0x%X/%u/%u\n",
        model.RemoveSignature, model.OwnerOffset, model.WindowOffset, model.IdOffset,
        model.WindowRepresentation, model.OwnerSourceRva, model.OwnerSourceKind,
        model.OwnerSourceDepth);
    scan_ok = KswFlowRun(full, &function, &model, 0, initial, &scan);
    if (!scan_ok) goto done;
    dump_watch("scan", &function, &scan, &model, watch);
    for (index = 0; index < function.InsnCount; ++index)
    {
        const KSW_RULE_INSN* insn = &function.Insns[index];
        const KSW_FLOW_STATE* state = &scan.States[index];
        if (!state->Seen) continue;
        if (insn->Kind == KSW_RULE_CALL && KswHkCallDirect(insn))
        {
            printf("SCAN_CALL rva=0x%X target=0x%X guards=0x%X rcx=",
                insn->Rva, insn->Target, state->Guards);
            print_value(state->Regs[KSW_REG_RCX]);
            putchar('\n');
        }
        if (insn->OperandCount == 2 && insn->Operands[0].Kind == KSW_RULE_OP_REG &&
            (insn->Kind == KSW_RULE_LEA || insn->Kind == KSW_RULE_ADD))
        {
            KSW_FLOW_VALUE link = insn->Kind == KSW_RULE_LEA ?
                KswFlowAddress(state, &insn->Operands[1], &model) :
                state->Regs[insn->Operands[0].Reg];
            if (link.Kind == KSW_V_LINK || (link.Kind == KSW_V_NODE &&
                insn->Kind == KSW_RULE_ADD && insn->Operands[1].Kind == KSW_RULE_OP_IMM &&
                insn->Operands[1].Imm == layout.NextOffset))
            {
                printf("SCAN_NEXT rva=0x%X kind=%u value=", insn->Rva, insn->Kind);
                print_value(link);
                putchar('\n');
            }
        }
        if (insn->OperandCount == 2 && insn->Operands[0].Kind == KSW_RULE_OP_MEM &&
            (insn->Kind == KSW_RULE_CMP || insn->Kind == KSW_RULE_MOV || insn->Kind == KSW_RULE_AND))
        {
            KSW_FLOW_VALUE address = KswFlowAddress(state, &insn->Operands[0], &model);
            if (address.Kind == KSW_V_SESSION &&
                address.Value == (int64_t)layout.CacheOffset)
            {
                printf("SCAN_CACHE rva=0x%X kind=%u compared=0x%X guards=0x%X address=",
                    insn->Rva, insn->Kind, state->ComparedCache, state->Guards);
                print_value(address);
                printf(" source=");
                print_value(KswFlowOperand(state, &insn->Operands[1], &model));
                putchar('\n');
            }
        }
    }
done:
    printf("SCAN_STATUS load=%d basis=%d loop=%d scan=%d error=%u basisError=%u scanError=%u anchor=0x%X loopReg=%u indexReg=%u\n",
        load_ok, basis_ok, loop_ok, scan_ok, context.Error, basis.Error, scan.Error,
        loop_ok && anchor < function.InsnCount ? function.Insns[anchor].Rva : 0,
        model.LoopReg, model.LoopIndexReg);
    KswFlowFree(&basis);
    KswFlowFree(&scan);
    KswRuleFreeFunction(full, &function);
    if (initial) full->Free(full->Context, initial);
    return 0;
}

static int debug_loop(const KSW_RULE_IMAGE* full, uint32_t entry,
    uint32_t table_offset, uint32_t next_offset, uint32_t bucket_count,
    int override_session, uint32_t session_slot)
{
    KSW_HK_RULE_CONTEXT context = {0};
    KSW_HOTKEY_RULE_RESULT layout = {0};
    KSW_FLOW_MODEL model = {0};
    KSW_RULE_FUNCTION function = {0};
    KSW_FLOW_ANALYSIS basis = {0};
    KSW_FLOW_STATE* initial = NULL;
    uint32_t anchor = 0, anchor_rva = 0;
    int loaded = 0, flowed = 0, matched = 0;
    context.Full = full;
    if (!KswRuleFindImportSlot(full, "win32k.sys", "W32GetUserSessionState", &model.SessionSlot))
        (void)KswRuleFindImportSlot(full, "win32ksgd.sys", "SGDGetUserSessionState", &model.SessionSlot);
    if (override_session) model.SessionSlot = session_slot;
    model.TableOffset = table_offset;
    model.NextOffset = next_offset;
    layout.TableOffset = table_offset;
    layout.NextOffset = next_offset;
    layout.BucketCount = bucket_count;
    context.Model = model;
    loaded = KswHkLoad(&context, entry, &function);
    if (!loaded) goto done;
    initial = (KSW_FLOW_STATE*)full->Alloc(full->Context, sizeof(*initial));
    if (!initial) {context.Error = KSW_HK_RULE_RESOURCE; goto done;}
    KswFlowInit(initial);
    initial->Regs[KSW_REG_RDX].Kind = KSW_V_WNDOBJ;
    initial->Regs[KSW_REG_R9] = KswFlowConst(3);
    flowed = KswFlowRun(full, &function, &model, 0, initial, &basis);
    if (!flowed) goto done;
    matched = KswHkProofLoop(&context, &function, &basis, initial,
        &model, &layout, &anchor);
    if (matched && anchor < function.InsnCount) anchor_rva = function.Insns[anchor].Rva;
done:
    printf("{\"LoopMatched\":%s,\"Loaded\":%s,\"Flowed\":%s,"
           "\"Error\":%u,\"FlowError\":%u,\"SessionSlot\":%u,"
           "\"AnchorRva\":%u,\"LoopReg\":%u,\"LoopIndexReg\":%u}\n",
        matched ? "true" : "false", loaded ? "true" : "false",
        flowed ? "true" : "false", context.Error, basis.Error,
        context.Model.SessionSlot, anchor_rva, model.LoopReg, model.LoopIndexReg);
    KswFlowFree(&basis);
    KswRuleFreeFunction(full, &function);
    if (initial) full->Free(full->Context, initial);
    return 0;
}

int main(int argc, char** argv)
{
    PROBE_IMAGE full = {0}, base = {0};
    KSW_RULE_IMAGE full_input, base_input;
    KSW_HOTKEY_RULE_RESULT result = {0};
    int matched, debug = (argc == 6 || argc == 8 || argc == 9) &&
        strcmp(argv[3], "--debug-flow") == 0;
    int slot = argc == 10 && strcmp(argv[3], "--debug-slot") == 0;
    int assignment = argc == 4 && strcmp(argv[3], "--debug-assignment") == 0;
    int register_mode = argc == 4 && strcmp(argv[3], "--debug-register") == 0;
    int owner_mode = (argc == 5 || argc == 6) && strcmp(argv[3], "--debug-owner") == 0;
    int scan_mode = (argc == 5 || argc == 6) && strcmp(argv[3], "--debug-scan") == 0;
    int loop = (argc == 8 || argc == 9) && strcmp(argv[3], "--debug-loop") == 0;
    uint32_t debug_rva = 0, debug_mode = 0, allocation = 0, insertion = 0, watch = 0;
    uint32_t slot_arguments[6] = {0};
    uint32_t loop_arguments[5] = {0};
    uint32_t argument;
    if (argc == 2 && strcmp(argv[1], "--selftest-values") == 0)
        return selftest_values();
    if (argc != 3 && !debug && !slot && !assignment && !register_mode && !owner_mode && !scan_mode && !loop)
    {
        fprintf(stderr, "usage: hotkey_rule_probe FULL_PE BASE_PE [--debug-flow RVA MODE [ALLOC_RVA INSERT_RVA [WATCH_RVA]] | --debug-slot ENTRY OBJECT_SIZE NEXT_OFFSET FLAGS_OFFSET FLAGS_WIDTH CHILD_OFFSET | --debug-assignment | --debug-register | --debug-owner ENTRY [DEPTH] | --debug-scan ENTRY [WATCH_RVA] | --debug-loop ENTRY TABLE_OFFSET NEXT_OFFSET BUCKET_COUNT [SESSION_SLOT_OVERRIDE]]\n");
        return 2;
    }
    if (debug && (strcmp(argv[3], "--debug-flow") != 0 ||
        !parse_rva(argv[4], &debug_rva) || !parse_rva(argv[5], &debug_mode) ||
        debug_mode > 1 || (argc >= 8 && (!parse_rva(argv[6], &allocation) ||
        !parse_rva(argv[7], &insertion))) ||
        (argc == 9 && !parse_rva(argv[8], &watch)))) return 2;
    if (slot)
        for (argument = 0; argument < 6; ++argument)
            if (!parse_rva(argv[4 + argument], &slot_arguments[argument])) return 2;
    if (loop)
        for (argument = 0; argument < 4; ++argument)
            if (!parse_rva(argv[4 + argument], &loop_arguments[argument])) return 2;
    if (loop && argc == 9 && !parse_rva(argv[8], &loop_arguments[4])) return 2;
    if (scan_mode && !parse_rva(argv[4], &debug_rva)) return 2;
    if (scan_mode && argc == 6 && !parse_rva(argv[5], &watch)) return 2;
    if (owner_mode && (!parse_rva(argv[4], &debug_rva) ||
        (argc == 6 && !parse_rva(argv[5], &watch)))) return 2;
    if (!load_image(argv[1], &full) || !load_image(argv[2], &base))
    {
        fprintf(stderr, "invalid_or_unreadable_amd64_pe\n");
        free(full.Bytes);
        free(base.Bytes);
        return 2;
    }
    full_input.Context = &full;
    full_input.SizeOfImage = full.Size;
    full_input.Read = read_image;
    full_input.Alloc = alloc_image;
    full_input.Free = free_image;
    base_input.Context = &base;
    base_input.SizeOfImage = base.Size;
    base_input.Read = read_image;
    base_input.Alloc = alloc_image;
    base_input.Free = free_image;
    if (debug)
    {
        int status = debug_flow(&full_input, debug_rva, debug_mode, allocation, insertion, watch);
        free(full.Bytes);
        free(base.Bytes);
        return status;
    }
    if (assignment)
    {
        int status = debug_assignment(&full_input, &base_input);
        free(full.Bytes);
        free(base.Bytes);
        return status;
    }
    if (register_mode)
    {
        int status = debug_register(&full_input, &base_input);
        free(full.Bytes);
        free(base.Bytes);
        return status;
    }
    if (owner_mode)
    {
        int status = debug_owner(&full_input, debug_rva, watch);
        free(full.Bytes);
        free(base.Bytes);
        return status;
    }
    if (scan_mode)
    {
        int status = debug_scan(&full_input, &base_input, debug_rva, watch);
        free(full.Bytes);
        free(base.Bytes);
        return status;
    }
    if (loop)
    {
        int status = debug_loop(&full_input, loop_arguments[0], loop_arguments[1],
            loop_arguments[2], loop_arguments[3], argc == 9, loop_arguments[4]);
        free(full.Bytes);
        free(base.Bytes);
        return status;
    }
    if (slot)
    {
        int status = debug_slot(&full_input, &base_input, slot_arguments[0],
            slot_arguments[1], slot_arguments[2], slot_arguments[3],
            slot_arguments[4], slot_arguments[5]);
        free(full.Bytes);
        free(base.Bytes);
        return status;
    }
    matched = KswHotkeyMatchRules(&full_input, &base_input, &result);
    emit_result(matched, &result);
    free(full.Bytes);
    free(base.Bytes);
    return 0;
}
