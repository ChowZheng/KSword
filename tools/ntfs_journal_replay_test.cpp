// Production replay/insert/finish bodies are extracted by the Python runner.
// Only Win32 I/O is replaced; Qt containers and the FILE record parser are real.
#include "NtfsFastIndex.h"
#include "NtfsIndexRecord.h"
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <utility>
#include <vector>

using Bytes = std::vector<std::byte>;
using DWORD = std::uint32_t;
using WORD = std::uint16_t;
constexpr int FALSE = 0;
constexpr DWORD FSCTL_READ_USN_JOURNAL = 1;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 16;
constexpr DWORD ERROR_JOURNAL_NOT_ACTIVE = 1179, ERROR_JOURNAL_DELETE_IN_PROGRESS = 1178;
struct READ_USN_JOURNAL_DATA_V0 {
    qint64 StartUsn = 0;
    DWORD ReasonMask = 0;
    int ReturnOnlyOnClose = 0;
    quint64 UsnJournalID = 0;
};
struct USN_JOURNAL_DATA_V0 {
    quint64 UsnJournalID = 123;
    qint64 FirstUsn = 0;
    qint64 NextUsn = 200;
    qint64 LowestValidUsn = 0;
};
struct Simulation {
    std::vector<Bytes> pages;
    std::size_t page = 0;
    bool readFailed = false;
    bool queryFailed = false;
    unsigned queryFailedAt = 0;
    unsigned queryCalls = 0;
    unsigned recordReadFailedAt = 0;
    std::vector<USN_JOURNAL_DATA_V0> queries;
    std::shared_ptr<std::atomic_bool> cancelOnFinalQuery;
    USN_JOURNAL_DATA_V0 journal;
    std::map<quint64, Bytes> records;
    QSet<quint64> failedRecords;
    std::vector<quint64> recordRequests;
    std::vector<qint64> journalRequests;
};

static bool DeviceIoControl(Simulation* simulation, DWORD operation, void* input,
    DWORD, void* output, DWORD capacity, DWORD* returned, void*)
{
    assert(operation == FSCTL_READ_USN_JOURNAL);
    const auto& request = *static_cast<READ_USN_JOURNAL_DATA_V0*>(input);
    assert(request.UsnJournalID == 123 && request.ReturnOnlyOnClose == FALSE);
    simulation->journalRequests.push_back(request.StartUsn);
    if (simulation->readFailed || simulation->page >= simulation->pages.size())
        return false;
    const auto& bytes = simulation->pages[simulation->page++];
    assert(bytes.size() <= capacity);
    std::memcpy(output, bytes.data(), bytes.size());
    *returned = static_cast<DWORD>(bytes.size());
    return true;
}

namespace ks::files {
namespace {
constexpr quint64 kRecordMask = 0x0000ffffffffffffULL;
constexpr quint64 kDirtyLimit = 2000000;
Simulation* activeSimulation = nullptr;
struct Volume {
    Simulation* handle = nullptr;
    struct {
        DWORD BytesPerSector = 512;
        struct { qint64 QuadPart = 1234; } VolumeSerialNumber;
    } info;
};
bool openVolume(const QString& requested, Volume& volume, QString& root, QString&)
{
    assert(activeSimulation);
    volume.handle = activeSimulation;
    root = requested;
    return true;
}
bool stopped(const NtfsFastIndex::Cancel& cancel) { return cancel && cancel->load(); }
QString failure(const QString& operation, DWORD code = 0) { return operation + QStringLiteral(" failed by fixture %1").arg(code); }
enum class RecordResult { Present, Missing, Failed };
RecordResult readRecord(Volume& volume, quint64 index, Bytes& bytes, QString& error)
{
    auto& simulation = *volume.handle;
    simulation.recordRequests.push_back(index);
    if (simulation.failedRecords.contains(index)
        || (simulation.recordReadFailedAt && simulation.recordRequests.size() == simulation.recordReadFailedAt)) {
        error = QStringLiteral("record I/O failure fixture");
        return RecordResult::Failed;
    }
    const auto record = simulation.records.find(index);
    if (record == simulation.records.end()) return RecordResult::Missing;
    bytes = record->second;
    return RecordResult::Present;
}
bool queryJournal(Volume& volume, USN_JOURNAL_DATA_V0& journal, DWORD& code)
{
    auto& simulation = *volume.handle;
    ++simulation.queryCalls;
    const bool failed = simulation.queryFailed
        || (simulation.queryFailedAt && simulation.queryCalls == simulation.queryFailedAt);
    code = failed ? 5 : 0;
    journal = simulation.queryCalls <= simulation.queries.size()
        ? simulation.queries[simulation.queryCalls - 1] : simulation.journal;
    if (simulation.cancelOnFinalQuery && simulation.queryCalls == 2)
        simulation.cancelOnFinalQuery->store(true);
    return !failed;
}
#include "production_replay.inc"
}
NtfsFastIndex::Snapshot NtfsFastIndex::build(const QString&, const Cancel&, const Progress&)
{
    return {}; // Only the null-previous dispatch needs a build symbol in this fixture.
}
#include "production_refresh.inc"
}

using namespace ks::files;

static void Put(Bytes& bytes, std::size_t offset, std::uint64_t value, std::size_t width)
{
    for (std::size_t i = 0; i < width; ++i)
        bytes.at(offset + i) = static_cast<std::byte>((value >> (i * 8)) & 255);
}

static quint64 Ref(quint64 index, quint64 sequence = 7) { return (sequence << 48) | index; }

static std::uint64_t Get(const Bytes& bytes, std::size_t offset, std::size_t width)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
        value |= static_cast<std::uint64_t>(bytes.at(offset + i)) << (i * 8);
    return value;
}

static Bytes Resident(unsigned type, const Bytes& value)
{
    Bytes bytes((24 + value.size() + 7) & ~std::size_t{7});
    Put(bytes, 0, type, 4);
    Put(bytes, 4, bytes.size(), 4);
    Put(bytes, 16, value.size(), 4);
    Put(bytes, 20, 24, 2);
    std::copy(value.begin(), value.end(), bytes.begin() + 24);
    return bytes;
}

static Bytes File(quint64 reference, quint64 parent, const std::u16string& name,
    quint64 base = 0, const std::vector<quint64>& attributeReferences = {})
{
    Bytes filename(66 + name.size() * 2);
    Put(filename, 0, parent, 8);
    Put(filename, 64, name.size(), 1);
    Put(filename, 65, 1, 1);
    for (std::size_t i = 0; i < name.size(); ++i) Put(filename, 66 + i * 2, name[i], 2);
    std::vector<Bytes> attributes{ Resident(0x30, filename), Resident(0x80, Bytes(17)) };
    if (!attributeReferences.empty()) {
        Bytes list(attributeReferences.size() * 32);
        for (std::size_t i = 0; i < attributeReferences.size(); ++i) {
            Put(list, i * 32, 0x30, 4);
            Put(list, i * 32 + 4, 32, 2);
            Put(list, i * 32 + 16, attributeReferences[i], 8);
        }
        attributes.push_back(Resident(0x20, list));
    }
    Bytes record(1024);
    Put(record, 0, 0x454c4946U, 4);
    Put(record, 4, 48, 2);
    Put(record, 6, 3, 2);
    Put(record, 16, reference >> 48, 2);
    Put(record, 20, 56, 2);
    Put(record, 22, 1, 2);
    Put(record, 28, 1024, 4);
    Put(record, 32, base, 8);
    std::size_t offset = 56;
    for (const auto& attribute : attributes) {
        assert(offset + attribute.size() + 4 < record.size());
        std::copy(attribute.begin(), attribute.end(), record.begin() + offset);
        offset += attribute.size();
    }
    Put(record, offset, 0xffffffffU, 4);
    Put(record, 24, offset + 4, 4);
    Put(record, 48, 0xa55a, 2);
    for (unsigned sector = 0; sector < 2; ++sector) {
        const auto tail = (sector + 1U) * 512U - 2;
        record[50 + sector * 2] = record[tail];
        record[51 + sector * 2] = record[tail + 1];
        Put(record, tail, 0xa55a, 2);
    }
    return record;
}

static Bytes Journal(qint64 next, const std::vector<std::pair<quint64, qint64>>& changes = {})
{
    Bytes bytes(8 + changes.size() * 64);
    Put(bytes, 0, static_cast<quint64>(next), 8);
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const auto offset = 8 + i * 64;
        Put(bytes, offset, 64, 4);
        Put(bytes, offset + 4, 2, 2);
        Put(bytes, offset + 8, changes[i].first, 8);
        Put(bytes, offset + 24, static_cast<quint64>(changes[i].second), 8);
        Put(bytes, offset + 56, 2, 2);
        Put(bytes, offset + 58, 60, 2);
        Put(bytes, offset + 60, u'x', 2);
    }
    return bytes;
}

static NtfsIndexSnapshot Baseline()
{
    NtfsIndexSnapshot snapshot;
    snapshot.journalId = 123;
    snapshot.nextUsn = 100;
    snapshot.journalAvailable = true;
    return snapshot;
}

static void Seed(NtfsIndexSnapshot& snapshot, quint64 reference, const Bytes& bytes)
{
    ntfs::ParsedRecord parsed;
    assert(ntfs::ParseRecord(bytes.data(), bytes.size(), 512,
        reference & 0x0000ffffffffffffULL, parsed));
    insertRecord(snapshot, parsed, bytes.data());
}

static bool Replay(Simulation& simulation, NtfsIndexSnapshot& snapshot,
    qint64 upper = 200, const NtfsFastIndex::Cancel& cancel = {})
{
    Volume volume{};
    volume.handle = &simulation;
    return replay(volume, snapshot, upper, cancel, {});
}

int main()
{
    unsigned scenarios = 0;
    const auto file = Ref(40), extension = Ref(41), root = Ref(5);
    {
        auto snapshot = Baseline();
        Seed(snapshot, file, File(file, root, u"old.txt"));
        Simulation io;
        io.pages = {Journal(200, {{file, 110}})};
        io.records[40] = File(file, Ref(6), u"renamed.txt");
        assert(Replay(io, snapshot)); finish(snapshot);
        assert(snapshot.nextUsn == 200 && snapshot.entries[file].name == QStringLiteral("renamed.txt"));
        assert(snapshot.entries[file].parentReference == Ref(6));
        ++scenarios;
    }
    {
        auto snapshot = Baseline(); Seed(snapshot, file, File(file, root, u"deleted.txt"));
        Simulation io; io.pages = {Journal(200, {{file, 110}})};
        assert(Replay(io, snapshot)); finish(snapshot);
        assert(snapshot.entries.isEmpty() && snapshot.nextUsn == 200);
        ++scenarios;
    }
    {
        auto snapshot = Baseline(); Seed(snapshot, file, File(file, root, u"reused.txt"));
        Simulation io; io.pages = {Journal(200, {{file, 110}, {Ref(40, 8), 120}})};
        io.records[40] = File(Ref(40, 8), root, u"new-incarnation.txt");
        assert(Replay(io, snapshot)); finish(snapshot);
        assert(!snapshot.entries.contains(file) && snapshot.entries.contains(Ref(40, 8)));
        assert(io.recordRequests.size() == 1);
        ++scenarios;
    }
    {
        auto snapshot = Baseline(); Seed(snapshot, file, File(file, root, u"unchanged.txt"));
        Simulation io; io.pages = {Journal(150), Journal(200)};
        assert(Replay(io, snapshot));
        assert(snapshot.nextUsn == 200 && io.recordRequests.empty());
        assert((io.journalRequests == std::vector<qint64>{100, 150}));
        ++scenarios;
    }
    for (int mode = 0; mode < 7; ++mode) {
        auto snapshot = Baseline();
        Simulation io; io.pages = {Journal(200)};
        if (mode == 0) io.journal.UsnJournalID = 124;
        if (mode == 1) io.journal.FirstUsn = 201;
        if (mode == 2) io.journal.LowestValidUsn = 201;
        if (mode == 3) io.queryFailed = true;
        if (mode == 4) io.readFailed = true;
        if (mode == 5) io.pages = {Journal(100)};
        if (mode == 6) io.pages = {Bytes(4)};
        assert(!Replay(io, snapshot) && snapshot.nextUsn == 100 && !snapshot.error.isEmpty());
        if (mode < 6 && mode != 3) assert(snapshot.rebuildRequired);
        if (mode == 3) assert(!snapshot.rebuildRequired && snapshot.error.contains(QStringLiteral("5")));
        ++scenarios;
    }
    {
        auto snapshot = Baseline(); Simulation io;
        auto cancel = std::make_shared<std::atomic_bool>(true);
        assert(!Replay(io, snapshot, 200, cancel));
        assert(snapshot.cancelled && snapshot.nextUsn == 100 && io.journalRequests.empty());
        ++scenarios;
    }
    {
        auto snapshot = Baseline(); Simulation io;
        io.pages = {Journal(300, {{file, 120}, {extension, 220}})};
        io.records[40] = File(file, root, u"captured.txt");
        io.journal.NextUsn = 300;
        assert(Replay(io, snapshot));
        assert(snapshot.nextUsn == 200 && io.recordRequests == std::vector<quint64>{40});
        ++scenarios;
    }
    {
        auto snapshot = Baseline(); Simulation io;
        io.pages = {Journal(150, {{file, 120}}), Journal(200, {{file, 170}})};
        io.records[40] = File(file, root, u"coalesced.txt");
        assert(Replay(io, snapshot) && io.recordRequests.size() == 1);
        ++scenarios;
    }
    {
        auto snapshot = Baseline(); Simulation io;
        io.pages = {Journal(200, {{file, 110}})};
        io.records[40] = File(file, root, u"base.txt", 0, {file, extension});
        io.records[41] = File(extension, Ref(6), u"alias.txt", file);
        assert(Replay(io, snapshot)); finish(snapshot);
        assert(snapshot.entries[file].names.size() == 2 && snapshot.extensions.contains(extension));
        assert(snapshot.unknownSizeCount == 1 && snapshot.entries[file].hasAttributeList);
        ++scenarios;
    }
    // Both records are already scheduled by USN. Validation must not depend on
    // QSet iteration order or skip a full-reference/base check for that reason.
    for (int mode = 0; mode < 3; ++mode) {
        auto snapshot = Baseline(); Simulation io;
        io.pages = {Journal(200, {{file, 110}, {extension, 120}})};
        io.records[40] = File(file, root, u"base.txt", 0, {file, extension});
        if (mode == 0) io.records[41] = File(Ref(41, 8), root, u"wrong-sequence", file);
        if (mode == 1) io.records[41] = File(extension, root, u"wrong-base", Ref(42));
        assert(!Replay(io, snapshot) && snapshot.rebuildRequired && snapshot.nextUsn == 100);
        ++scenarios;
    }
    {
        auto snapshot = Baseline();
        Seed(snapshot, file, File(file, root, u"base.txt", 0, {extension}));
        Seed(snapshot, extension, File(extension, root, u"obsolete-alias.txt", file));
        finish(snapshot); snapshot.complete = false;
        assert(snapshot.entries[file].names.size() == 2);
        Simulation io; io.pages = {Journal(200, {{file, 110}})};
        io.records[40] = File(file, root, u"base.txt", 0, {extension});
        io.records[41] = File(extension, root, u"current-alias.txt", file);
        assert(Replay(io, snapshot)); finish(snapshot);
        assert(snapshot.entries[file].names.size() == 2);
        for (const auto& name : snapshot.entries[file].names)
            assert(name.name != QStringLiteral("obsolete-alias.txt"));
        ++scenarios;
    }
    {
        auto snapshot = Baseline(); Simulation io;
        io.pages = {Journal(200, {{file, 110}, {Ref(42), 120}})};
        io.records[40] = File(file, root, u"first-base", 0, {extension});
        io.records[42] = File(Ref(42), root, u"second-base", 0, {extension});
        io.records[41] = File(extension, root, u"claimed-twice", file);
        assert(!Replay(io, snapshot) && snapshot.rebuildRequired && snapshot.nextUsn == 100);
        ++scenarios;
    }
    {
        auto snapshot = Baseline(); Simulation io;
        io.pages = {Journal(200, {{file, 110}})};
        io.records[40] = File(file, root, u"stale-self", 0, {Ref(40, 8)});
        assert(!Replay(io, snapshot) && snapshot.rebuildRequired && snapshot.nextUsn == 100);
        ++scenarios;
    }
    {
        auto snapshot = Baseline(); Simulation io;
        auto bytes = File(file, root, u"external-list");
        const auto end = static_cast<std::size_t>(Get(bytes, 24, 4)) - 4;
        Bytes external(72);
        Put(external, 0, 0x20, 4); Put(external, 4, 72, 4); Put(external, 8, 1, 1);
        Put(external, 32, 64, 2); Put(external, 40, 4096, 8);
        Put(external, 48, 128, 8); Put(external, 56, 128, 8);
        Put(external, 64, 0x11, 1); Put(external, 65, 1, 1); Put(external, 66, 20, 1);
        assert(end + external.size() + 4 < 510);
        std::copy(external.begin(), external.end(), bytes.begin() + end);
        Put(bytes, end + external.size(), 0xffffffffU, 4);
        Put(bytes, 24, end + external.size() + 4, 4);
        io.pages = {Journal(200, {{file, 110}})}; io.records[40] = bytes;
        assert(!Replay(io, snapshot) && snapshot.rebuildRequired && snapshot.nextUsn == 100);
        ++scenarios;
    }
    {
        auto snapshot = Baseline();
        Seed(snapshot, file, File(file, root, u"base", 0, {extension}));
        Seed(snapshot, extension, File(extension, root, u"obsolete-alias", file));
        finish(snapshot); snapshot.complete = false;
        Simulation io; io.pages = {Journal(200, {{extension, 110}})};
        io.records[40] = File(file, root, u"base", 0, {extension});
        io.records[41] = File(extension, root, u"updated-alias", file);
        assert(Replay(io, snapshot)); finish(snapshot);
        assert(std::find(io.recordRequests.begin(), io.recordRequests.end(), 40) != io.recordRequests.end());
        assert(snapshot.entries[file].names.size() == 2);
        for (const auto& name : snapshot.entries[file].names)
            assert(name.name != QStringLiteral("obsolete-alias"));
        ++scenarios;
    }
    {
        auto snapshot = Baseline(); Simulation io;
        io.pages = {Journal(200, {{file, 110}})};
        io.failedRecords.insert(40);
        assert(!Replay(io, snapshot) && snapshot.nextUsn == 100 && !snapshot.error.isEmpty());
        ++scenarios;
    }
    for (int mode = 0; mode < 3; ++mode) {
        auto snapshot = Baseline(); Simulation io;
        auto page = Journal(200, {{file, 110}});
        if (mode == 0) Put(page, 8, 63, 4);
        if (mode == 1) Put(page, 12, 3, 2);
        if (mode == 2) Put(page, 8 + 58, 65535, 2);
        io.pages = {page};
        assert(!Replay(io, snapshot) && snapshot.nextUsn == 100);
        ++scenarios;
    }
    // Exercise the actual publication boundary too: replay may already erase or
    // replace entries before a later read/query failure or finish cancellation.
    for (int mode = 0; mode < 5; ++mode) {
        auto previous = std::make_shared<NtfsIndexSnapshot>(Baseline());
        previous->volumeRoot = QStringLiteral("C:\\");
        previous->volumeSerial = 1234;
        Seed(*previous, file, File(file, root, u"original-a"));
        Seed(*previous, Ref(42), File(Ref(42), root, u"original-b"));
        finish(*previous);
        Simulation io;
        io.pages = {Journal(200, {{file, 110}, {Ref(42), 120}})};
        io.records[40] = File(file, root, u"modified-a");
        io.records[42] = File(Ref(42), root, u"modified-b");
        auto cancel = std::make_shared<std::atomic_bool>(false);
        if (mode == 0) {
            auto reset = io.journal; reset.UsnJournalID = 124;
            io.queries = {io.journal, reset};
        }
        if (mode == 1) io.queryFailedAt = 2;
        if (mode == 2) io.recordReadFailedAt = 2;
        if (mode == 3) io.cancelOnFinalQuery = cancel;
        activeSimulation = &io;
        const auto result = NtfsFastIndex::refresh(previous, cancel);
        activeSimulation = nullptr;
        assert(result && previous->entries[file].name == QStringLiteral("original-a"));
        assert(previous->entries[Ref(42)].name == QStringLiteral("original-b") && previous->nextUsn == 100);
        if (mode == 4) {
            assert(result->complete && result->nextUsn == 200);
            assert(result->entries[file].name == QStringLiteral("modified-a"));
            assert(result->entries[Ref(42)].name == QStringLiteral("modified-b"));
        } else {
            assert(!result->complete && result->nextUsn == 100);
            assert(result->entries.size() == previous->entries.size());
            assert(result->entries[file].name == QStringLiteral("original-a"));
            assert(result->entries[Ref(42)].name == QStringLiteral("original-b"));
            assert(io.recordRequests.size() == 2); // A later boundary failed after real mutation.
            if (mode == 3) assert(result->cancelled && cancel->load());
            else assert(!result->error.isEmpty());
        }
        ++scenarios;
    }
    std::cout << "Production NTFS journal replay tests passed (" << scenarios << " scenarios)\n";
}
