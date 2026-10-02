// Conservative legacy x86 decoder. No REX interpretation or RIP-relative operands.
InstructionDescription DecodeInstruction(const unsigned char* code, std::size_t size)
{
    InstructionDescription d;
    if (!code || !size) return d;
    size = (std::min)(size, std::size_t(15));
    if (size >= 4 && std::memcmp(code, kLandingPad, 4) == 0)
    { d.length = 4; d.flow = ControlFlow::LandingPad; return d; }
    std::size_t offset = 0;
    bool operand16 = false;
    while (offset < size) {
        const auto prefix = code[offset];
        if (prefix == 0x66) { operand16 = true; ++offset; }
        else if (prefix == 0x2E || prefix == 0x36 || prefix == 0x3E || prefix == 0x26 || prefix == 0x64 || prefix == 0x65) ++offset;
        else break;
    }
    if (offset >= size) return {};
    const auto op = code[offset++];
    auto finish = [&](std::size_t immediate) -> InstructionDescription {
        if (offset + immediate > size) return {};
        d.length = offset + immediate; return d;
    };
    auto relative = [&](ControlFlow flow, unsigned width, unsigned condition = 0) -> InstructionDescription {
        if (operand16 || offset != (op == 0x0F ? 2u : 1u) || offset + width > size) return {};
        d.flow = flow; d.condition = condition; d.displacementOffset = offset; d.displacementSize = width;
        if (width == 1) d.relativeDisplacement = static_cast<std::int8_t>(code[offset]);
        else { std::int32_t value; std::memcpy(&value, code + offset, 4); d.relativeDisplacement = value; }
        return finish(width);
    };
    if (op == 0x67 || op == 0xF0 || op == 0xF2 || op == 0xF3 || op == 0xC4 || op == 0xC5) return {};
    if ((op >= 0x40 && op <= 0x5F) || op == 0x90 || op == 0x99 || op == 0x9C || op == 0x9D) return finish(0);
    if (op == 0xC3 || op == 0xC2 || op == 0xCC) { d.flow = ControlFlow::Terminal; return finish(op == 0xC2 ? 2 : 0); }
    if (op == 0x6A) return finish(1);
    if (op == 0x68 || (op >= 0xB8 && op <= 0xBF)) return finish(operand16 ? 2 : 4);
    if (op >= 0xB0 && op <= 0xB7) return finish(1);
    if (op >= 0xA0 && op <= 0xA3) return finish(4); // absolute moffs32, even with operand-size override
    if (op == 0xE8 || op == 0xE9) return relative(op == 0xE8 ? ControlFlow::Call : ControlFlow::Jump, 4);
    if (op == 0xEB) return relative(ControlFlow::Jump, 1);
    if (op >= 0x70 && op <= 0x7F) return relative(ControlFlow::Conditional, 1, op & 15);
    unsigned char second = 0;
    if (op == 0x0F) {
        if (offset >= size) return {};
        second = code[offset++];
        if (second >= 0x80 && second <= 0x8F) return relative(ControlFlow::Conditional, 4, second & 15);
        if (second != 0x1F && second != 0xB6 && second != 0xB7 && second != 0xBE && second != 0xBF
            && !(second >= 0x40 && second <= 0x4F)) return {};
    }
    std::size_t immediate = 0;
    if (op != 0x0F) {
        switch (op) {
        case 0x01: case 0x03: case 0x09: case 0x0B: case 0x21: case 0x23: case 0x29: case 0x2B:
        case 0x31: case 0x33: case 0x39: case 0x3B: case 0x84: case 0x85:
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D: case 0xFF: break;
        case 0x69: case 0x81: case 0xC7: immediate = operand16 ? 2 : 4; break;
        case 0x6B: case 0x80: case 0x83: case 0xC6: immediate = 1; break;
        case 0xF6: case 0xF7:
            if (offset >= size || ((code[offset] >> 3) & 7) == 1) return {};
            if (((code[offset] >> 3) & 7) == 0) immediate = op == 0xF6 ? 1 : operand16 ? 2 : 4;
            break;
        default: return {};
        }
    }
    if (offset >= size) return {};
    const auto modrm = code[offset++];
    const unsigned mod = modrm >> 6, rm = modrm & 7, group = (modrm >> 3) & 7;
    if ((op == 0xC6 || op == 0xC7 || (op == 0x0F && second == 0x1F)) && group != 0) return {};
    if (op == 0x8D && mod == 3) return {};
    // Far CALL/JMP and reserved FF encodings have different semantics.
    if (op == 0xFF && (group == 3 || group == 5 || group == 7)) return {};
    if (op == 0xFF && group == 4) d.flow = ControlFlow::Terminal;
    unsigned displacement = 0;
    if (mod != 3 && rm == 4) {
        if (offset >= size) return {};
        const unsigned base = code[offset++] & 7;
        if (mod == 0 && base == 5) displacement = 4;
    }
    if (mod == 0 && rm == 5) displacement = 4;
    if (mod == 1) displacement = 1;
    if (mod == 2) displacement = 4;
    d.displacementOffset = offset; d.displacementSize = displacement;
    offset += displacement;
    return finish(immediate);
}
