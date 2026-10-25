#!/usr/bin/env python3
"""只读匹配 win32k 热键表布局；只有完整指令链唯一时才生成驱动 profile。"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from uuid import UUID

import pefile
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from win32k_hotkey_pe import PEImage, PEInspectionError, Function, instruction_evidence, one


class MatchError(PEInspectionError):
    pass


def operands(insn):
    return insn.operands


def reg_name(insn, operand) -> str:
    return insn.reg_name(operand.reg) if operand.type == X86_OP_REG else ""


def reg_base(insn, operand) -> str:
    return insn.reg_name(operand.mem.base) if operand.type == X86_OP_MEM else ""


def register_family(name: str) -> str:
    aliases = {
        "rax": {"rax", "eax", "ax", "al", "ah"},
        "rbx": {"rbx", "ebx", "bx", "bl", "bh"},
        "rcx": {"rcx", "ecx", "cx", "cl", "ch"},
        "rdx": {"rdx", "edx", "dx", "dl", "dh"},
        "rsi": {"rsi", "esi", "si", "sil"},
        "rdi": {"rdi", "edi", "di", "dil"},
        "rbp": {"rbp", "ebp", "bp", "bpl"},
        "rsp": {"rsp", "esp", "sp", "spl"},
    }
    for index in range(8, 16):
        aliases[f"r{index}"] = {f"r{index}", f"r{index}d", f"r{index}w", f"r{index}b"}
    return next((family for family, names in aliases.items() if name in names), name)


def mem_disp(insn, operand, base: str | None = None, size: int | None = None) -> int | None:
    if operand.type != X86_OP_MEM:
        return None
    if base is not None and reg_base(insn, operand) != base:
        return None
    if size is not None and operand.size != size:
        return None
    return int(operand.mem.disp)


def is_zero_reg(insn, register: str) -> bool:
    ops = operands(insn)
    return (insn.mnemonic == "xor" and len(ops) == 2 and
            reg_name(insn, ops[0]) == register and reg_name(insn, ops[1]) == register)


def writes_register(insn, register: str) -> bool:
    ops = operands(insn)
    return bool(ops and ops[0].type == X86_OP_REG and
                register_family(reg_name(insn, ops[0])) == register_family(register) and
                insn.mnemonic not in {"cmp", "test"})


def import_call(image: PEImage, insn, name: str, dll: str | None = None) -> bool:
    target = image.call_target(insn)
    if not target or target[0] != "import":
        return False
    parts = str(target[1]).split("!", 1)
    return len(parts) == 2 and parts[1] == name and (dll is None or parts[0] == dll)


def direct_calls(image: PEImage, function: Function) -> list[tuple[Function, object]]:
    calls = []
    for target, call in image.direct_callees(function):
        if target in image.function_ranges:
            calls.append((image.function(target), call))
    return calls


def prove_enter_crit(image: PEImage, function: Function) -> list[dict]:
    calls = [(index, insn) for index, insn in enumerate(function.instructions)
             if import_call(image, insn, "EnterCrit", "win32kbase.sys")]
    index, call = one(calls, "enter_crit_call_missing_or_ambiguous")
    previous = function.instructions[max(0, index - 6):index]
    ecx = one((i for i in previous if is_zero_reg(i, "ecx")), "enter_crit_ecx_zero_missing")
    edx = one((i for i in previous if is_zero_reg(i, "edx")), "enter_crit_edx_zero_missing")
    for register, zero in (("ecx", ecx), ("edx", edx)):
        zero_index = function.instructions.index(zero)
        if any(writes_register(i, register) or i.mnemonic == "call"
               for i in function.instructions[zero_index + 1:index]):
            raise MatchError(f"enter_crit_{register}_overwritten")
    if not image.import_calls(function, "ValidateHwnd"):
        raise MatchError("validate_hwnd_call_missing")
    if not (image.import_calls(function, "UserSessionSwitchLeaveCrit") or
            image.import_calls(function, "LeaveCrit")):
        raise MatchError("leave_crit_call_missing")
    return [instruction_evidence(edx), instruction_evidence(ecx), instruction_evidence(call)]


def cache_access_after_call(function: Function, call_index: int, mnemonic: str) -> list:
    found = []
    for insn in function.instructions[call_index + 1:call_index + 7]:
        if insn.mnemonic not in {"nop", "cmp", "and", "mov", "test", "jmp", "je", "jne"}:
            break
        ops = operands(insn)
        if insn.mnemonic == mnemonic and len(ops) >= 2 and \
                mem_disp(insn, ops[0], "rax", 8) is not None and \
                ops[1].type == X86_OP_IMM and ops[1].imm == 0:
            found.append(insn)
    return found


def prove_unregister_cache(image: PEImage, function: Function) -> tuple[int, list[dict]]:
    compare = []
    clear = []
    for index, insn in enumerate(function.instructions):
        if not import_call(image, insn, "W32GetUserSessionState", "win32k.sys"):
            continue
        compare.extend(cache_access_after_call(function, index, "cmp"))
        clear.extend(cache_access_after_call(function, index, "and"))
        clear.extend(cache_access_after_call(function, index, "mov"))
    matches = [(a, b) for a in compare for b in clear
               if a.address != b.address and
               mem_disp(a, operands(a)[0], "rax", 8) == mem_disp(b, operands(b)[0], "rax", 8)]
    pair = one(matches, "session_cache_cmp_clear_missing_or_ambiguous")
    offset = mem_disp(pair[0], operands(pair[0])[0], "rax", 8)
    if offset is None or offset <= 0:
        raise MatchError("session_cache_offset_invalid")
    return offset, [instruction_evidence(pair[0]), instruction_evidence(pair[1])]


def preceding_r9d_three(function: Function, call) -> object | None:
    index = function.instructions.index(call)
    prior = function.instructions[max(0, index - 8):index]
    candidates = [insn for insn in prior if insn.mnemonic == "mov" and len(operands(insn)) == 2
                  and reg_name(insn, operands(insn)[0]) == "r9d"
                  and operands(insn)[1].type == X86_OP_IMM and operands(insn)[1].imm == 3]
    if len(candidates) != 1:
        return None
    set_index = function.instructions.index(candidates[0])
    if any(writes_register(insn, "r9d") or insn.mnemonic == "call"
           for insn in function.instructions[set_index + 1:index]):
        return None
    return candidates[0]


def cmp_mem_with_branch(function: Function, offset: int) -> list:
    matches = []
    by_address = {insn.address: insn for insn in function.instructions}
    for index, insn in enumerate(function.instructions[:-1]):
        if insn.mnemonic != "cmp" or not operands(insn):
            continue
        op = operands(insn)[0]
        if mem_disp(insn, op) != offset or op.mem.base == 0:
            continue
        next_insn = function.instructions[index + 1]
        if next_insn.mnemonic == "jmp" and len(operands(next_insn)) == 1 and \
                operands(next_insn)[0].type == X86_OP_IMM:
            next_insn = by_address.get(int(operands(next_insn)[0].imm), next_insn)
        if next_insn.mnemonic.startswith("j") and next_insn.mnemonic != "jmp":
            matches.append((reg_base(insn, op), insn, next_insn))
    return matches


def prove_remove_helper(image: PEImage, function: Function) -> tuple[int, list[dict]]:
    table = []
    for index, insn in enumerate(function.instructions):
        if not import_call(image, insn, "W32GetUserSessionState", "win32k.sys"):
            continue
        for later in function.instructions[index + 1:index + 8]:
            ops = operands(later)
            if later.mnemonic == "lea" and len(ops) == 2 and \
                    mem_disp(later, ops[1], "rax") is not None:
                table.append(later)
    table_lea = one(table, "remove_table_lea_missing_or_ambiguous")
    table_offset = mem_disp(table_lea, operands(table_lea)[1], "rax")
    if table_offset is None or table_offset <= 0:
        raise MatchError("remove_table_offset_invalid")
    loop_128 = [i for i in function.instructions if i.mnemonic == "mov" and len(operands(i)) == 2
                and operands(i)[0].type == X86_OP_REG and operands(i)[1].type == X86_OP_IMM
                and operands(i)[1].imm == 128]
    count = one(loop_128, "remove_bucket_count_not_128")
    counter = register_family(reg_name(count, operands(count)[0]))
    decrements = [i for i in function.instructions if i.mnemonic == "sub" and len(operands(i)) == 2
                  and register_family(reg_name(i, operands(i)[0])) == counter
                  and operands(i)[1].type == X86_OP_IMM and operands(i)[1].imm == 1]
    decrement = one(decrements, "remove_bucket_count_not_decremented")
    decrement_index = function.instructions.index(decrement)
    if decrement_index + 1 >= len(function.instructions) or \
            function.instructions[decrement_index + 1].mnemonic not in {"jne", "jnz"}:
        raise MatchError("remove_bucket_loop_branch_missing")
    stride = one((i for i in function.instructions if i.mnemonic == "add" and len(operands(i)) == 2
                  and reg_name(i, operands(i)[0]) == reg_name(table_lea, operands(table_lea)[0])
                  and operands(i)[1].type == X86_OP_IMM
                  and operands(i)[1].imm == 8), "remove_bucket_stride_not_8")
    next_links = [i for i in function.instructions if i.mnemonic in {"add", "lea"}
                  and any(mem_disp(i, op) == 0x30 for op in operands(i) if op.type == X86_OP_MEM)
                  or (i.mnemonic == "add" and len(operands(i)) == 2
                      and operands(i)[1].type == X86_OP_IMM and operands(i)[1].imm == 0x30)]
    if len(next_links) < 2:
        raise MatchError("remove_next_link_evidence_missing")
    for offset in (0, 0x10, 0x28):
        if not cmp_mem_with_branch(function, offset):
            raise MatchError(f"remove_field_compare_missing:0x{offset:X}")
    bases = [set(item[0] for item in cmp_mem_with_branch(function, offset))
             for offset in (0, 0x10, 0x28)]
    if not set.intersection(*bases):
        raise MatchError("remove_field_comparisons_not_same_object")
    evidence = [table_lea, count, decrement, stride] + next_links[:2] + \
        [cmp_mem_with_branch(function, offset)[0][1] for offset in (0, 0x10, 0x28)]
    return table_offset, [instruction_evidence(i) for i in evidence]


def discover_unregister(image: PEImage) -> tuple[Function, Function, Function, int, int, dict]:
    entry = image.function(image.unique_export("NtUserUnregisterHotKey"))
    crit_evidence = prove_enter_crit(image, entry)
    candidates = []
    for unregister, entry_call in direct_calls(image, entry):
        try:
            cache_offset, cache_evidence = prove_unregister_cache(image, unregister)
        except PEInspectionError:
            continue
        for remove, remove_call in direct_calls(image, unregister):
            r9 = preceding_r9d_three(unregister, remove_call)
            if r9 is None:
                continue
            try:
                table_offset, remove_evidence = prove_remove_helper(image, remove)
            except PEInspectionError:
                continue
            candidates.append((unregister, remove, cache_offset, table_offset,
                               entry_call, remove_call, r9, cache_evidence, remove_evidence))
    (unregister, remove, cache_offset, table_offset,
     entry_call, remove_call, r9, cache_evidence, remove_evidence) = one(
         candidates, "unregister_call_chain_missing_or_ambiguous")
    evidence = {
        "export": entry.report_range(),
        "exportCall": instruction_evidence(entry_call),
        "enterCrit": crit_evidence,
        "unregister": unregister.report_range(),
        "cache": cache_evidence,
        "removeCall": [instruction_evidence(r9), instruction_evidence(remove_call)],
        "removeHelper": remove.report_range(),
        "removeStructure": remove_evidence,
    }
    return entry, unregister, remove, cache_offset, table_offset, evidence


def prove_insert_helper(image: PEImage, function: Function) -> tuple[int, list[dict]]:
    vk_loads = []
    for insn in function.instructions:
        ops = operands(insn)
        if insn.mnemonic == "mov" and len(ops) == 2 and ops[0].type == X86_OP_REG and \
                mem_disp(insn, ops[1], size=1) == 0x24:
            vk_loads.append(insn)
    vk_load = one(vk_loads, "insert_vk_load_missing_or_ambiguous")
    masks = [i for i in function.instructions if i.mnemonic == "and" and len(operands(i)) == 2
             and operands(i)[0].type == X86_OP_REG and operands(i)[1].type == X86_OP_IMM
             and operands(i)[1].imm == 0x7F and
             register_family(reg_name(i, operands(i)[0])) ==
                 register_family(reg_name(vk_load, operands(vk_load)[0]))]
    mask = one(masks, "insert_hash_mask_missing_or_ambiguous")
    sessions = [i for i in function.instructions
                if import_call(image, i, "W32GetUserSessionState", "win32k.sys")]
    if len(sessions) != 2:
        raise MatchError("insert_session_calls_not_two")
    indexed_reads, indexed_writes = [], []
    for insn in function.instructions:
        ops = operands(insn)
        if insn.mnemonic != "mov" or len(ops) != 2:
            continue
        for index, operand in enumerate(ops):
            if operand.type != X86_OP_MEM or operand.mem.index == 0 or \
                    operand.mem.scale != 8 or reg_base(insn, operand) != "rax":
                continue
            if operand.mem.disp <= 0:
                continue
            (indexed_writes if index == 0 else indexed_reads).append(insn)
    pairs = [(read, write) for read in indexed_reads for write in indexed_writes
             if mem_disp(read, operands(read)[1]) == mem_disp(write, operands(write)[0])]
    read, write = one(pairs, "insert_table_read_write_missing_or_ambiguous")
    table_offset = mem_disp(read, operands(read)[1])
    next_stores = [i for i in function.instructions if i.mnemonic == "mov" and len(operands(i)) == 2
                   and mem_disp(i, operands(i)[0], size=8) == 0x30]
    next_store = one(next_stores, "insert_next_link_store_missing_or_ambiguous")
    if not (vk_load.address < mask.address < sessions[0].address < read.address <
            next_store.address < sessions[1].address < write.address):
        raise MatchError("insert_dataflow_order_invalid")
    input_copy = [i for i in function.instructions if i.mnemonic == "mov" and len(operands(i)) == 2
                  and reg_name(i, operands(i)[0]) == reg_base(next_store, operands(next_store)[0])
                  and reg_name(i, operands(i)[1]) == "rcx" and i.address < next_store.address]
    if len(input_copy) != 1:
        raise MatchError("insert_object_alias_missing_or_ambiguous")
    evidence = [vk_load, mask, *sessions, read, next_store, write, input_copy[0]]
    return table_offset, [instruction_evidence(i) for i in evidence]


def prove_allocation(image: PEImage, function: Function) -> tuple[object, str, list[dict]]:
    candidates = []
    instructions = function.instructions
    for index, call in enumerate(instructions):
        if not import_call(image, call, "Win32AllocPoolZInit", "win32kbase.sys"):
            continue
        for lea in instructions[max(0, index - 5):index]:
            ops = operands(lea)
            if lea.mnemonic != "lea" or len(ops) != 2 or \
                    reg_name(lea, ops[0]) != "ecx" or \
                    mem_disp(lea, ops[1]) != 0x48:
                continue
            object_reg = reg_base(lea, ops[1])
            tests = [i for i in instructions[max(0, instructions.index(lea) - 8):instructions.index(lea)]
                     if i.mnemonic == "test" and len(operands(i)) == 2 and
                     reg_name(i, operands(i)[0]) == object_reg and
                     reg_name(i, operands(i)[1]) == object_reg]
            if len(tests) != 1:
                continue
            test_index = instructions.index(tests[0])
            branch = instructions[test_index + 1] if test_index + 1 < len(instructions) else None
            if branch is None or branch.mnemonic not in {"jne", "jnz"} or \
                    len(operands(branch)) != 1 or operands(branch)[0].type != X86_OP_IMM or \
                    operands(branch)[0].imm <= call.address:
                continue
            aliases = [i for i in instructions[index + 1:index + 8] if i.mnemonic == "mov"
                       and len(operands(i)) == 2 and
                       reg_name(i, operands(i)[0]) == object_reg and
                       reg_name(i, operands(i)[1]) == "rax"]
            if len(aliases) == 1:
                candidates.append((call, object_reg,
                                   [tests[0], branch, lea, call, aliases[0]]))
    call, object_reg, evidence = one(candidates, "register_48_byte_allocation_missing_or_ambiguous")
    return call, object_reg, [instruction_evidence(i) for i in evidence]


def path_exists(function: Function, start: int, destination: int,
                excluded: int | None = None) -> bool:
    """只沿已解码函数内的直接控制流前进；间接跳转无法证明必经关系。"""
    instructions = function.instructions
    positions = {insn.address: index for index, insn in enumerate(instructions)}
    if start not in positions or destination not in positions:
        return False
    pending = [start]
    seen: set[int] = set()
    while pending:
        address = pending.pop()
        if address == excluded or address in seen:
            continue
        if address == destination:
            return True
        seen.add(address)
        index = positions[address]
        insn = instructions[index]
        next_address = instructions[index + 1].address if index + 1 < len(instructions) else None
        if insn.mnemonic.startswith("ret") or insn.mnemonic in {"int3", "ud2"}:
            continue
        if insn.mnemonic.startswith("loop"):
            raise MatchError(f"unhandled_loop_in_required_path:0x{address:X}")
        if insn.mnemonic == "jmp":
            if len(operands(insn)) != 1 or operands(insn)[0].type != X86_OP_IMM:
                raise MatchError(f"indirect_jump_in_required_path:0x{address:X}")
            target = int(operands(insn)[0].imm)
            if target in positions:
                pending.append(target)
            continue
        if insn.mnemonic.startswith("j") and insn.mnemonic != "jmp":
            if len(operands(insn)) != 1 or operands(insn)[0].type != X86_OP_IMM:
                raise MatchError(f"indirect_branch_in_required_path:0x{address:X}")
            target = int(operands(insn)[0].imm)
            if target in positions:
                pending.append(target)
        if next_address is not None:
            pending.append(next_address)
    return False


def prove_new_object_fields(function: Function, alloc_call, insert_call, object_reg: str) -> list[dict]:
    fields = {0: 8, 8: 8, 0x10: 8, 0x18: 8, 0x20: 2, 0x22: 2, 0x24: 4, 0x28: 4}
    between = [i for i in function.instructions if alloc_call.address < i.address < insert_call.address]
    evidence = []
    for offset, size in fields.items():
        writes = [i for i in between if i.mnemonic == "mov" and len(operands(i)) == 2
                  and mem_disp(i, operands(i)[0], object_reg, size) == offset]
        evidence.append(one(writes, f"register_field_write_missing_or_ambiguous:0x{offset:X}/{size}"))
    child = []
    for i in between:
        ops = operands(i)
        if i.mnemonic == "lea" and len(ops) == 2 and ops[0].type == X86_OP_REG and \
                mem_disp(i, ops[1], object_reg) == 0x38:
            child.append(i)
    child_lea = one(child, "register_child_list_lea_missing_or_ambiguous")
    child_reg = reg_name(child_lea, operands(child_lea)[0])
    child_writes = []
    for offset in (0, 8):
        writes = [i for i in between if i.mnemonic == "mov" and len(operands(i)) == 2
                  and mem_disp(i, operands(i)[0], child_reg, 8) == offset and
                  reg_name(i, operands(i)[1]) == child_reg and i.address > child_lea.address]
        child_writes.append(one(writes, f"register_child_list_init_missing:0x{offset:X}"))
    if not (alloc_call.address < min(i.address for i in evidence) and
            max(i.address for i in evidence + child_writes) < insert_call.address):
        raise MatchError("register_field_order_invalid")
    if not path_exists(function, alloc_call.address, insert_call.address):
        raise MatchError("register_alloc_to_insert_path_missing")
    for required in evidence + [child_lea] + child_writes:
        if path_exists(function, alloc_call.address, insert_call.address,
                       excluded=required.address):
            raise MatchError(f"register_field_not_on_all_insert_paths:0x{required.address:X}")
    return [instruction_evidence(i) for i in evidence + [child_lea] + child_writes]


def discover_register(image: PEImage, expected_table_offset: int) -> tuple[Function, Function, Function, dict]:
    entry = image.function(image.unique_export("NtUserRegisterHotKey"))
    candidates = []
    for register, entry_call in direct_calls(image, entry):
        try:
            allocation, object_reg, allocation_evidence = prove_allocation(image, register)
        except PEInspectionError:
            continue
        for insert, insert_call in direct_calls(image, register):
            if insert_call.address <= allocation.address:
                continue
            try:
                insert_offset, insert_evidence = prove_insert_helper(image, insert)
                if insert_offset != expected_table_offset:
                    continue
                field_evidence = prove_new_object_fields(register, allocation,
                                                          insert_call, object_reg)
            except PEInspectionError:
                continue
            candidates.append((register, insert, entry_call, insert_call,
                               allocation_evidence, field_evidence, insert_evidence))
    (register, insert, entry_call, insert_call, allocation_evidence,
     field_evidence, insert_evidence) = one(candidates, "register_call_chain_missing_or_ambiguous")
    evidence = {
        "export": entry.report_range(),
        "exportCall": instruction_evidence(entry_call),
        "register": register.report_range(),
        "allocation": allocation_evidence,
        "fieldWrites": field_evidence,
        "insertCall": instruction_evidence(insert_call),
        "insertHelper": insert.report_range(),
        "insertStructure": insert_evidence,
    }
    return entry, register, insert, evidence


def match_pair(full_path: Path, base_path: Path) -> dict:
    report = {"fullPath": str(full_path), "basePath": str(base_path), "status": "unsupported"}
    try:
        full = PEImage(full_path)
        base = PEImage(base_path)
        report["full"] = full.identity.report()
        report["base"] = base.identity.report()
        if not full.identity.rsds.pdb_name.lower().endswith("win32kfull.pdb") or \
                not base.identity.rsds.pdb_name.lower().endswith("win32kbase.pdb"):
            raise MatchError("module_rsds_name_mismatch")
        if (full.identity.file_version_ms, full.identity.file_version_ls) != \
                (base.identity.file_version_ms, base.identity.file_version_ls):
            raise MatchError("paired_file_version_mismatch")
        base_exports = {}
        for name in ("EnterCrit", "LeaveCrit", "ValidateHwnd"):
            rva = base.unique_export(name)
            base.function(rva)
            base_exports[name] = f"0x{rva:X}"
        report["baseRequiredExports"] = base_exports
        _, unregister, remove, cache_offset, remove_table_offset, unregister_evidence = \
            discover_unregister(full)
        _, register, insert, register_evidence = discover_register(full, remove_table_offset)
        report["evidence"] = {
            "unregister": unregister_evidence,
            "register": register_evidence,
            "matchedOffsets": {
                "table": f"0x{remove_table_offset:X}",
                "sessionCache": f"0x{cache_offset:X}",
                "removeNextLink": "0x30",
                "registerObjectSize": "0x48",
            },
        }
        prologue = full.read_bytes(remove.begin, 32)
        report["profile"] = {
            "tableOffset": remove_table_offset,
            "sessionCacheOffset": cache_offset,
            "removeMatchingRva": remove.begin,
            "removeMatchingPrologue": prologue.hex().upper(),
            "unregisterRva": unregister.begin,
            "registerRva": register.begin,
            "insertRva": insert.begin,
        }
        report["status"] = "supported"
    except (PEInspectionError, OSError, ValueError, struct.error, pefile.PEFormatError) as error:
        report["reason"] = str(error)
    return report


def guid_initializer(guid_text: str) -> str:
    guid = UUID(guid_text)
    tail = ", ".join(f"0x{value:02X}" for value in guid.bytes[8:])
    return (f"{{0x{guid.time_low:08X}UL, 0x{guid.time_mid:04X}U, "
            f"0x{guid.time_hi_version:04X}U, {{{tail}}}}}")


def profile_initializer(pair: dict) -> str:
    full = pair["full"]
    base = pair["base"]
    profile = pair["profile"]
    prologue = bytes.fromhex(profile["removeMatchingPrologue"])
    if len(prologue) != 32:
        raise MatchError("prologue_not_32_bytes")
    bytes_text = ", ".join(f"0x{value:02X}" for value in prologue)
    return (
        "    {\n"
        f"        0x{int(full['timeDateStamp'], 16):08X}UL, 0x{int(full['sizeOfImage'], 16):X}UL, "
        f"{full['rsdsAge']}UL,\n"
        f"        {guid_initializer(full['rsdsGuid'])},\n"
        f"        0x{int(base['timeDateStamp'], 16):08X}UL, 0x{int(base['sizeOfImage'], 16):X}UL, "
        f"{base['rsdsAge']}UL,\n"
        f"        {guid_initializer(base['rsdsGuid'])},\n"
        f"        0x{profile['tableOffset']:X}UL, 0x{profile['sessionCacheOffset']:X}UL, "
        f"0x{profile['removeMatchingRva']:X}UL,\n"
        f"        {{{bytes_text}}}\n"
        "    }"
    )


def render_driver_header(pairs: list[dict]) -> str:
    verified = [pair for pair in pairs if pair["status"] == "supported"]
    initializers = ",\n".join(profile_initializer(pair) for pair in verified)
    if not initializers:
        # C 不支持标准零长数组；计数为零且占位记录全零，绝不参与匹配。
        initializers = "    {0}"
    return (
        "#pragma once\n"
        "// 离线PE指令链完整验证后生成；未匹配身份不得启用热键变更。\n"
        "#include <ntddk.h>\n\n"
        "typedef struct _KSW_HOTKEY_MATCH_PROFILE\n"
        "{\n"
        "    ULONG FullTimeDateStamp; // win32kfull.sys PE时间戳。\n"
        "    ULONG FullImageSize; // win32kfull.sys 映像大小。\n"
        "    ULONG FullPdbAge; // full RSDS Age。\n"
        "    GUID FullPdbGuid; // full RSDS GUID。\n"
        "    ULONG BaseTimeDateStamp; // win32kbase.sys PE时间戳。\n"
        "    ULONG BaseImageSize; // win32kbase.sys 映像大小。\n"
        "    ULONG BasePdbAge; // base RSDS Age。\n"
        "    GUID BasePdbGuid; // base RSDS GUID。\n"
        "    ULONG TableOffset; // W32GetUserSessionState返回值起的128桶表偏移。\n"
        "    ULONG SessionCacheOffset; // 同一SessionState的卸载缓存位。\n"
        "    ULONG RemoveMatchingRva; // HKRemoveMatchingHotkeys函数起始RVA。\n"
        "    UCHAR RemoveMatchingPrologue[32]; // 对应函数首32字节，运行时再次核验。\n"
        "} KSW_HOTKEY_MATCH_PROFILE;\n\n"
        "static const KSW_HOTKEY_MATCH_PROFILE g_KswHotkeyMatchProfiles[] =\n"
        "{\n" + initializers + "\n};\n"
        f"static const ULONG g_KswHotkeyMatchProfileCount = {len(verified)}UL;\n"
    )


def write_atomic(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as output:
            output.write(content)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pair", nargs=2, action="append", metavar=("FULL", "BASE"),
                        required=True, help="只读分析一对 win32kfull/win32kbase PE，可重复")
    parser.add_argument("--out", type=Path, required=True, help="JSON报告路径")
    parser.add_argument("--emit-driver-header", type=Path, help="可选：生成fail-closed内核profile头")
    args = parser.parse_args(argv)

    pairs = [match_pair(Path(full), Path(base)) for full, base in args.pair]
    seen: set[tuple[str, str]] = set()
    for pair in pairs:
        if pair["status"] != "supported":
            continue
        identity = (pair["full"]["sha256"], pair["base"]["sha256"])
        if identity in seen:
            pair["status"] = "unsupported"
            pair["reason"] = "duplicate_pair_identity"
            pair.pop("profile", None)
        seen.add(identity)
    supported = sum(pair["status"] == "supported" for pair in pairs)
    report = {
        "schemaVersion": 1,
        "method": "read_only_amd64_pe_exports_pdata_capstone",
        "supportedPairs": supported,
        "unsupportedPairs": len(pairs) - supported,
        "pairs": pairs,
    }
    write_atomic(args.out, json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    if args.emit_driver_header:
        write_atomic(args.emit_driver_header, render_driver_header(pairs))
    print(f"HOTKEY_MATCH_SUPPORTED={supported} UNSUPPORTED={len(pairs) - supported}")
    return 0 if supported == len(pairs) else 2


if __name__ == "__main__":
    sys.exit(main())
