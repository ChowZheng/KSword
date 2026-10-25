#include "NtfsIndexRecord.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

using Bytes = std::vector<std::byte>;
using namespace ks::files::ntfs;

static void Put(Bytes& b, std::size_t p, std::uint64_t v, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i)
        b.at(p + i) = static_cast<std::byte>((v >> (i * 8)) & 255U);
}

static Bytes Name(std::uint64_t parent, const std::u16string& name, unsigned ns = 1)
{
    Bytes value(66 + name.size() * 2);
    Put(value, 0, parent, 8);
    Put(value, 64, name.size(), 1);
    Put(value, 65, ns, 1);
    for (std::size_t i = 0; i < name.size(); ++i)
        Put(value, 66 + i * 2, name[i], 2);
    return value;
}

static Bytes Resident(unsigned type, const Bytes& value, bool named = false)
{
    const std::size_t offset = named ? 32 : 24;
    Bytes b((offset + value.size() + 7) & ~std::size_t{7});
    Put(b, 0, type, 4);
    Put(b, 4, b.size(), 4);
    if (named)
    {
        Put(b, 9, 1, 1);
        Put(b, 10, 24, 2);
        Put(b, 24, u'X', 2);
    }
    Put(b, 16, value.size(), 4);
    Put(b, 20, offset, 2);
    std::copy(value.begin(), value.end(), b.begin() + offset);
    return b;
}

static Bytes Nonresident(unsigned flags = 0, std::uint64_t lowest = 0)
{
    const std::size_t mapping = flags & 0x8001U ? 72 : 64;
    Bytes b(mapping + 8);
    Put(b, 0, 0x80, 4);
    Put(b, 4, b.size(), 4);
    Put(b, 8, 1, 1);
    Put(b, 12, flags, 2);
    Put(b, 16, lowest, 8);
    Put(b, 24, lowest + 3, 8);
    Put(b, 32, mapping, 2);
    Put(b, 40, 16384, 8);
    Put(b, 48, 12500, 8);
    Put(b, 56, 12000, 8);
    if (mapping == 72)
        Put(b, 64, 8192, 8);
    Put(b, mapping, 0x11, 1);
    Put(b, mapping + 1, 4, 1);
    Put(b, mapping + 2, 20, 1);
    return b;
}

static Bytes Record(const std::vector<Bytes>& attributes, unsigned flags = 1)
{
    Bytes b(1024);
    Put(b, 0, 0x454c4946, 4);
    Put(b, 4, 48, 2);
    Put(b, 6, 3, 2);
    Put(b, 16, 7, 2);
    Put(b, 20, 56, 2);
    Put(b, 22, flags, 2);
    Put(b, 28, 1024, 4);
    std::size_t position = 56;
    for (const auto& attribute : attributes)
    {
        assert(position + attribute.size() + 4 < b.size());
        std::copy(attribute.begin(), attribute.end(), b.begin() + position);
        position += attribute.size();
    }
    Put(b, position, 0xffffffffU, 4);
    Put(b, 24, position + 4, 4);
    // Persist update sequence fixups last, exactly as an on-disk record does.
    Put(b, 48, 0xa55a, 2);
    for (unsigned sector = 0; sector < 2; ++sector)
    {
        const auto tail = (sector + 1U) * 512U - 2;
        b[50 + sector * 2] = b[tail];
        b[51 + sector * 2] = b[tail + 1];
        Put(b, tail, 0xa55a, 2);
    }
    return b;
}

int main()
{
    ParsedRecord parsed;
    const auto data = Resident(0x80, Bytes(17));
    auto b = Record({Resident(0x30, Name(0x9000000000005ULL, u"hello.txt")), data});
    assert(ParseRecord(b.data(), b.size(), 512, 55, parsed));
    assert(parsed.reference == ((std::uint64_t{7} << 48) | 55));
    assert(parsed.inUse && parsed.baseRecord && !parsed.directory && parsed.sizeKnown);
    assert(parsed.logicalBytes == 17 && parsed.allocatedBytes == 0);
    assert(parsed.names.size() == 1 && parsed.names[0].name == u"hello.txt");
    assert(parsed.names[0].parentReference == 0x9000000000005ULL);

    b = Record({Resident(0x30, Name(5, u"a")), Resident(0x30, Name(6, u"b", 0)),
        Resident(0x30, Name(5, u"A~1", 2)), Resident(0x30, Name(5, u"a", 3)), data});
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed) && parsed.names.size() == 2);
    b = Record({data, Resident(0x80, Bytes(25), true)});
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed));
    assert(parsed.logicalBytes == 17 && parsed.sizeKnown);

    b = Record({Nonresident()});
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed));
    assert(parsed.sizeKnown && parsed.logicalBytes == 12500 && parsed.allocatedBytes == 16384);
    std::vector<MftStreamPart> parts;
    std::vector<std::uint64_t> refs;
    assert(ParseMftStreamParts(b.data(), b.size(), 512, parts, refs));
    assert(parts.size() == 1 && parts[0].extents.size() == 1);
    assert(parts[0].extents[0].startVcn == 0 && parts[0].extents[0].startLcn == 20);
    assert(parts[0].extents[0].clusterCount == 4 && !parts[0].extents[0].sparse);

    for (const auto flags : {0x8000U, 1U, 0x8001U})
    {
        b = Record({Nonresident(flags)});
        assert(ParseRecord(b.data(), b.size(), 512, 1, parsed));
        assert(parsed.sizeKnown && parsed.allocatedBytes == 8192);
        assert(!ParseMftStreamParts(b.data(), b.size(), 512, parts, refs));
    }
    b = Record({Nonresident(0, 4)});
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed) && !parsed.sizeKnown);
    auto emptyNonresident = Nonresident();
    Put(emptyNonresident, 24, UINT64_MAX, 8); // Empty nonresident stream sentinel.
    Put(emptyNonresident, 40, 0, 8);
    Put(emptyNonresident, 48, 0, 8);
    Put(emptyNonresident, 56, 0, 8);
    Put(emptyNonresident, 64, 0, 1);
    b = Record({emptyNonresident});
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed));
    assert(parsed.sizeKnown && parsed.logicalBytes == 0 && parsed.allocatedBytes == 0);
    assert(!ParseMftStreamParts(b.data(), b.size(), 512, parts, refs));
    Put(emptyNonresident, 48, 1, 8);
    b = Record({emptyNonresident});
    assert(!ParseRecord(b.data(), b.size(), 512, 1, parsed));
    b = Record({data, Resident(0x20, {})});
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed));
    assert(parsed.hasAttributeList && !parsed.sizeKnown);

    Bytes list(64);
    for (std::size_t offset : {0U, 32U})
    {
        Put(list, offset, 0x80, 4);
        Put(list, offset + 4, 32, 2);
        Put(list, offset + 16, 0x3000000000018ULL + offset, 8);
    }
    b = Record({Nonresident(), Resident(0x20, list)});
    assert(ParseMftStreamParts(b.data(), b.size(), 512, parts, refs) && refs.size() == 2);
    assert(refs[0] == 0x3000000000018ULL && refs[1] == 0x3000000000038ULL);
    Put(list, 0, 0x30, 4); // Extension FILE_NAME references are equally important.
    b = Record({data, Resident(0x20, list)});
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed));
    assert(parsed.attributeListResident && parsed.attributeReferences.size() == 2);
    assert(parsed.attributeReferences[0] == 0x3000000000018ULL);
    auto externalList = Nonresident();
    Put(externalList, 0, 0x20, 4);
    b = Record({data, externalList});
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed));
    assert(parsed.hasAttributeList && !parsed.attributeListResident && !parsed.sizeKnown);
    assert(parsed.attributeReferences.empty());
    assert(!ParseMftStreamParts(b.data(), b.size(), 512, parts, refs));
    Put(list, 4, 25, 2); // Every resident list entry is validated before publication.
    b = Record({data, Resident(0x20, list)});
    assert(!ParseRecord(b.data(), b.size(), 512, 1, parsed));

    // A negative mapping-pair LCN delta is relative to the previous run.
    auto nonresident = Nonresident();
    nonresident.resize(80);
    Put(nonresident, 4, 80, 4);
    Put(nonresident, 65, 2, 1);
    Put(nonresident, 67, 0x11, 1);
    Put(nonresident, 68, 2, 1);
    Put(nonresident, 69, 0xfb, 1); // 20 - 5 = 15
    b = Record({nonresident});
    assert(ParseMftStreamParts(b.data(), b.size(), 512, parts, refs));
    assert(parts[0].extents.size() == 2 && parts[0].extents[1].startLcn == 15);
    Put(nonresident, 69, 0xe0, 1); // 20 - 32 is invalid.
    b = Record({nonresident});
    assert(!ParseMftStreamParts(b.data(), b.size(), 512, parts, refs) && parts.empty());

    b = Record({}, 3);
    assert(ParseRecord(b.data(), b.size(), 512, 5, parsed));
    assert(parsed.directory && parsed.sizeKnown && (parsed.attributes & 16));
    b = Record({}, 0);
    Put(b, 20, 0, 2); // Unused record attributes need not remain valid.
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed) && !parsed.inUse);
    b = Record({data});
    Put(b, 32, 4, 8);
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed) && !parsed.baseRecord);

    b = Record({data});
    auto broken = b;
    broken[1023] = std::byte{0};
    assert(!ParseRecord(broken.data(), broken.size(), 512, 1, parsed));
    assert(parsed.names.empty() && !parsed.inUse && !parsed.sizeKnown);
    assert(!ParseRecord(b.data(), b.size(), 0, 1, parsed));
    assert(!ParseRecord(b.data(), b.size(), 513, 1, parsed));
    assert(!ParseRecord(b.data(), b.size(), 512, std::uint64_t{1} << 48, parsed));
    for (std::size_t length = 0; length < b.size(); ++length)
        assert(!ParseRecord(b.data(), length, 512, 1, parsed));

    broken = b;
    Put(broken, 60, 0xfffffff8U, 4); // Attribute length exceeds used bytes.
    assert(!ParseRecord(broken.data(), broken.size(), 512, 1, parsed));
    broken = b;
    Put(broken, 24, 56 + data.size(), 4); // Missing attribute terminator.
    assert(!ParseRecord(broken.data(), broken.size(), 512, 1, parsed));

    // Attribute contents cross a sector trailer: restoration must precede parsing.
    b = Record({Resident(0x80, Bytes(500))});
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed) && parsed.logicalBytes == 500);
    std::u16string longName(240, u'文');
    longName[183] = u'件';
    b = Record({Resident(0x30, Name(5, longName)), data});
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed));
    assert(parsed.names.size() == 1 && parsed.names[0].name == longName);
    auto restored = b;
    for (unsigned sector = 0; sector < 2; ++sector)
    {
        const auto tail = (sector + 1U) * 512U - 2;
        restored[tail] = restored[50 + sector * 2];
        restored[tail + 1] = restored[51 + sector * 2];
    }
    assert(!ParseRecord(restored.data(), restored.size(), 512, 1, parsed));
    assert(ParseRecord(restored.data(), restored.size(), 512, 1, parsed, RecordFormat::FileSystemControl));
    assert(parsed.names.size() == 1 && parsed.names[0].name == longName);
    auto mixed = restored;
    Put(mixed, 1022, 0xa55a, 2);
    assert(!ParseRecord(mixed.data(), mixed.size(), 512, 1, parsed, RecordFormat::FileSystemControl));
    restored[1022] = std::byte{1};
    assert(!ParseRecord(restored.data(), restored.size(), 512, 1, parsed, RecordFormat::FileSystemControl));
    b = Record({Nonresident()});
    for (unsigned sector = 0; sector < 2; ++sector)
    {
        const auto tail = (sector + 1U) * 512U - 2;
        b[tail] = b[50 + sector * 2];
        b[tail + 1] = b[51 + sector * 2];
    }
    assert(!ParseMftStreamParts(b.data(), b.size(), 512, parts, refs));
    assert(ParseMftStreamParts(b.data(), b.size(), 512, parts, refs, RecordFormat::FileSystemControl));
    assert(parts.size() == 1 && parts[0].extents[0].startLcn == 20);
    Bytes standard(48);
    Put(standard, 32, 0x2020, 4);
    b = Record({Resident(0x10, standard), Resident(0x30, Name(5, u"flags")), data});
    assert(ParseRecord(b.data(), b.size(), 512, 1, parsed) && parsed.attributes == 0x2020);
    std::mt19937 generator(0x1234);
    for (unsigned i = 0; i < 5000; ++i)
    {
        auto fuzzed = b;
        for (unsigned j = 0; j < 4; ++j)
            fuzzed[generator() % fuzzed.size()] = static_cast<std::byte>(generator() & 255U);
        ParseRecord(fuzzed.data(), fuzzed.size(), 512, 1, parsed);
        ParseMftStreamParts(fuzzed.data(), fuzzed.size(), 512, parts, refs);
    }
    std::cout << "NTFS index record tests passed (5000 mutation inputs)\n";
}
