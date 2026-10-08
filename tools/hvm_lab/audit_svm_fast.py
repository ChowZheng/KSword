"""Reject generated code that could corrupt unsaved guest extended or TLS state."""
import argparse
import re
import subprocess
from pathlib import Path

# This closed list intentionally fails a new compiler instruction until it is reviewed.
INTEGER = set('mov movzx movsx movsxd lea cmp test and or xor not neg add sub adc sbb inc dec shr shl sar sal push pop ret nop xchg cmove cmovne cmovb cmova cmovbe cmovae cmovz cmovnz sete setne setb seta bt btr bts btc'.split())
BRANCH = set('jmp je jne jz jnz ja jae jb jbe jg jge jl jle js jns jo jno jc jnc'.split())


def inspect(text):
    instructions = []
    for line in text.splitlines():
        match = re.match(r'\s*([0-9a-fA-F]{16}):\s+([a-zA-Z][a-zA-Z0-9]*)\s*(.*)', line)
        if match:
            instructions.append((int(match[1], 16), match[2].lower(), match[3]))
    if not instructions or 'KswSvmFastTry' not in text:
        raise ValueError('missing scalar leaf disassembly')
    addresses = {address for address, _, _ in instructions}
    for address, mnemonic, operand in instructions:
        if mnemonic not in INTEGER | BRANCH:
            raise ValueError(f'forbidden instruction at {address:x}: {mnemonic} {operand}')
        if re.search(r'\b(?:[xyz]mm\d+|mm\d+|st\b|fs:|gs:)', operand, re.I):
            raise ValueError(f'extended/TLS register at {address:x}: {operand}')
        if mnemonic in BRANCH:
            target = re.fullmatch(r'([0-9a-fA-F]{16})(?:h)?', operand.strip())
            if not target or int(target[1], 16) not in addresses:
                raise ValueError(f'nonlocal/indirect branch at {address:x}: {operand}')
    return len(instructions)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--object', required=True, type=Path)
    parser.add_argument('--dumpbin', required=True, type=Path)
    args = parser.parse_args()
    result = subprocess.run([str(args.dumpbin), '/DISASM:NOBYTES', str(args.object)], capture_output=True, check=True)
    # Instruction mnemonics, addresses and symbols are ASCII even with localized headers.
    text = result.stdout.decode('ascii', errors='replace')
    args.object.with_suffix('.integer-disasm.txt').write_text(text, encoding='utf-8')
    count = inspect(text)
    print(f'SVM_FAST_INTEGER_GATE=PASS instructions={count} calls=0 vector=0 x87=0 TLS=0')


if __name__ == '__main__':
    main()
