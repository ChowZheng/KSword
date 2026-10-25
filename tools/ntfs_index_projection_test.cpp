// Compiled with exact production project()/fullPath() excerpts by the runner.
#include "ntfs_projection.inc"
#include <cstdio>

int main()
{
    int checks = 0, failures = 0;
    auto check = [&](bool value) { ++checks; if (!value) ++failures; };
    auto data = std::make_shared<ks::files::NtfsIndexSnapshot>();
    data->rootReference = 0x1000000000005ULL;
    data->volumeRoot = QStringLiteral("T:\\");
    auto add = [&](quint64 id, quint64 parent, const QString& name, bool directory,
                   quint64 logical = 0, quint64 allocated = 0, bool known = true) {
        Entry entry;
        entry.fileReference = id; entry.parentReference = parent; entry.name = name;
        entry.attributes = directory ? 0x10U : 0U;
        entry.logicalBytes = logical; entry.allocatedBytes = allocated; entry.sizeKnown = known;
        entry.names.append({parent, name});
        entry.baseNames = entry.names;
        data->entries.insert(id, entry);
    };
    const quint64 root = data->rootReference;
    add(root, root, QStringLiteral("."), true);
    add(10, root, QStringLiteral("a"), true);
    add(11, 10, QStringLiteral("b"), true);
    add(12, 11, QStringLiteral("leaf.bin"), false, 23, 4096);
    add(13, 10, QStringLiteral("unknown.bin"), false, 999999, 999999, false);
    add(14, root, QStringLiteral("resident.txt"), false, 42, 0);
    // Two names share one record. Storage goes to its canonical parent once.
    data->entries[12].names.append({root, QStringLiteral("other.bin")});
    check(Index::fullPath(*data, 12) == QStringLiteral("T:\\a\\b\\leaf.bin"));
    check(Index::fullPath(*data, root) == QStringLiteral("T:\\"));
    check(Index::fullPath(*data, 0x2000000000005ULL).isEmpty()); // stale sequence
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    auto result = project(data, cancel);
    check(bool(result));
    check(result->directories[root].totals.files == 3);
    check(result->directories[root].totals.logical == 65);
    check(result->directories[root].totals.allocated == 4096);
    check(result->directories[root].totals.unknown == 1);
    check(result->directories[10].totals.files == 2);
    check(result->directories[10].totals.logical == 23);
    check(result->children[root].count(12) == 0);
    check(result->children[11].count(12) == 1);
    check(result->directories[10].directories.size() == 1);
    check(result->directories[11].row == 0);

    // Extension names merge only into the exact base sequence and are rebuilt
    // from the base/extension sources, so deleting an alias cannot leave it cached.
    ks::files::NtfsIndexExtension links;
    links.baseReference = 12;
    links.names = {{11, QStringLiteral("leaf.bin")}, {root, QStringLiteral("linked.bin")}};
    data->entries[12].hasAttributeList = true;
    data->entries[12].attributeReferences = {400};
    data->extensions.insert(400, links);
    ks::files::NtfsIndexExtension stale;
    stale.baseReference = (quint64(1) << 48) | 12;
    stale.names = {{root, QStringLiteral("stale-name.bin")}};
    data->extensions.insert(401, stale);
    ks::files::finish(*data);
    check(data->entries[12].names.size() == 2);
    check(data->entries[12].names.size() == 2
        && data->entries[12].names[1].name == QStringLiteral("linked.bin"));
    check(data->unknownSizeCount == 1);
    check(data->complete);
    data->extensions.remove(400);
    ks::files::finish(*data);
    check(data->entries[12].names.size() == 1);
    check(data->entries[12].name == QStringLiteral("leaf.bin"));

    // Missing parents and directory cycles cannot make a recursive tree or loop.
    add(20, 21, QStringLiteral("cycle1"), true);
    add(21, 20, QStringLiteral("cycle2"), true);
    add(22, 20, QStringLiteral("cycle-file"), false, 7, 8);
    add(23, 98765, QStringLiteral("orphan"), false, 2, 4);
    check(Index::fullPath(*data, 20).isEmpty());
    check(Index::fullPath(*data, 21).isEmpty());
    check(Index::fullPath(*data, 23).isEmpty());
    result = project(data, cancel);
    check(result->directories[20].parent == root);
    check(result->directories[21].parent == root);
    check(result->directories[root].totals.files == 5);
    check(result->directories[root].totals.logical == 74);
    check(result->directories[root].totals.allocated == 4108);

    // Overflow is a saturated lower bound, never a wrapped tiny allocation.
    const quint64 maximum = std::numeric_limits<quint64>::max();
    check(addBytes(maximum - 2, 8) == maximum);
    add(30, root, QStringLiteral("large"), false, maximum, maximum);
    result = project(data, cancel);
    check(result->directories[root].totals.logical == maximum);
    check(result->directories[root].totals.allocated == maximum);
    cancel->store(true);
    check(!project(data, cancel));

    std::printf("NTFS_PROJECTION_CHECKS=%d FAILURES=%d GUI=0 NATIVE_IO=0\n", checks, failures);
    return failures ? 1 : 0;
}
