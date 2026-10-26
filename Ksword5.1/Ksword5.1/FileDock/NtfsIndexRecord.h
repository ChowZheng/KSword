#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ks::files::ntfs
{
    enum class RecordFormat
    {
        RawDisk,
        FileSystemControl
    };

    struct ParsedName
    {
        std::uint64_t parentReference = 0;
        std::u16string name;
    };

    struct ParsedRecord
    {
        std::uint64_t reference = 0;
        std::vector<ParsedName> names;
        std::vector<std::uint64_t> attributeReferences;
        std::uint32_t attributes = 0;
        std::uint64_t logicalBytes = 0;
        std::uint64_t allocatedBytes = 0;
        bool sizeKnown = false;
        bool directory = false;
        bool inUse = false;
        bool baseRecord = false;
        bool hasAttributeList = false;
        bool attributeListResident = true;
    };

    // Parses one FILE record. RawDisk strictly verifies/restores 512-byte USA
    // strides; bytesPerSector is volume I/O geometry, never the fixup stride.
    // FileSystemControl requires NTFS's already-fixed-up logical API output:
    // preserve its tail bytes, which may have changed since the USA was built.
    // False means malformed/torn data; output is reset on failure. Valid unused
    // and extension records return true with inUse/baseRecord identifying them.
    // Only unnamed $DATA contributes sizes. Resident data occupies no separate
    // clusters; attribute-list files have unknown sizes until extents are resolved.
    bool ParseRecord(const std::byte* bytes, std::size_t length,
        std::uint32_t bytesPerSector, std::uint64_t recordIndex,
        ParsedRecord& output, RecordFormat format = RecordFormat::RawDisk);

    struct DataExtent
    {
        std::uint64_t startVcn = 0;
        std::uint64_t startLcn = 0;
        std::uint64_t clusterCount = 0;
        bool sparse = false;
    };

    struct MftStreamPart
    {
        std::uint64_t lowestVcn = 0;
        std::uint64_t highestVcn = 0;
        std::uint64_t logicalBytes = 0;
        std::vector<DataExtent> extents;
    };

    // Extracts $MFT's unnamed stream mapping pairs and extension-record refs.
    // Nonresident attribute lists require an external stream reader. When
    // attributeListParts is supplied their mapping is returned separately;
    // otherwise they retain the unsupported/false result. References include
    // sequence numbers and must be revalidated.
    bool ParseMftStreamParts(const std::byte* bytes, std::size_t length,
        std::uint32_t bytesPerSector, std::vector<MftStreamPart>& parts,
        std::vector<std::uint64_t>& extensionReferences,
        RecordFormat format = RecordFormat::RawDisk,
        std::vector<MftStreamPart>* attributeListParts = nullptr);

    // Parses a complete externally read $ATTRIBUTE_LIST value and returns only
    // unnamed $DATA references. Every entry is validated before publication;
    // malformed/truncated input returns false and clears the output.
    bool ParseMftAttributeList(const std::byte* bytes, std::size_t length,
        std::vector<std::uint64_t>& extensionReferences);
}
