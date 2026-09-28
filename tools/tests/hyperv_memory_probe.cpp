#include "../../Ksword5.1/Ksword5.1/MemoryDock/HyperVMemoryEvidence.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <cassert>
#include <iostream>

using namespace ksword::hyperv;
void selfTest()
{
    const QString a = QStringLiteral("11111111-1111-1111-1111-111111111111");
    const QString b = QStringLiteral("22222222-2222-2222-2222-222222222222");
    const QString c = QStringLiteral("33333333-3333-3333-3333-333333333333");
    auto counter = [](Metric metric, QString instance, Bytes bytes) { Counter value; value.metric = metric; value.instance = instance; value.bytes = bytes; return value; };
    Snapshot sample;
    Inventory wmi; wmi.id = a; wmi.name = QStringLiteral("Friendly VM"); wmi.wmi = true;
    Inventory hcs; hcs.id = b; hcs.runtimeId = a; hcs.name = b; hcs.hcs = true; hcs.owner = QStringLiteral("WSL");
    Inventory child; child.id = c; child.type = QStringLiteral("Container"); child.hostingSystemId = b; child.hcsPrivateWs = 9000;
    sample.inventory = {wmi, hcs, child};
    sample.counters = {counter(Metric::VidPhysical, b, 4096), counter(Metric::VidPhysical, QStringLiteral("_Total"), 4096),
        counter(Metric::DynamicPhysical, QStringLiteral("Friendly VM"), 4096), counter(Metric::ChildDeposited, a + QStringLiteral(":Hvpt"), 512),
        counter(Metric::RootGpa, QStringLiteral("root"), 100000)};
    correlate(sample);
    assert(sample.partitions.size() == 2 && sample.observedVidBytes == 4096 && sample.unresolvedVidBytes == 0);
    assert(sample.partitions[0].owner == QStringLiteral("WSL") && sample.partitions[0].wmi && sample.partitions[0].hcs);
    assert(sample.partitions[0].name == QStringLiteral("Friendly VM") && sample.partitions[0].depositedBytes == 512);
    assert(sample.partitions[0].counterIndices.size() == 3); // Root, total, guest WS are not additional partition bytes.
    sample.counters.push_back(counter(Metric::VidPhysical, a, 4096));
    correlate(sample);
    assert(sample.vidConflict && !sample.observedVidBytes); // Alias duplicates cannot double count.
    Snapshot ambiguous;
    wmi.name = QStringLiteral("duplicate"); child.name = wmi.name;
    ambiguous.inventory = {wmi, child};
    ambiguous.counters = {counter(Metric::VidPhysical, wmi.name, 8192)};
    correlate(ambiguous);
    assert(ambiguous.partitions[0].ambiguous && ambiguous.unresolvedVidBytes == 8192);
    Snapshot missing;
    missing.counters = {counter(Metric::VidPhysical, a, {})};
    correlate(missing);
    assert(!missing.observedVidBytes && missing.partitions.empty());
    missing.counters[0].bytes = 0;
    correlate(missing);
    assert(missing.observedVidBytes.has_value() && *missing.observedVidBytes == 0);
    assert(canonicalGuid(QStringLiteral("{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}")) == QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"));
    assert(canonicalGuid(QStringLiteral("not-a-guid")).isEmpty());
    assert(canonicalGuid(QStringLiteral("{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA")).isEmpty());
    Snapshot aliases;
    Inventory first; first.id = a;
    Inventory second; second.id = b;
    Inventory bridge; bridge.id = a; bridge.runtimeId = b; bridge.hcs = true;
    aliases.inventory = {first, second, bridge};
    aliases.counters = {counter(Metric::VidPhysical, a, 8192), counter(Metric::VidPhysical, b, 8192)};
    correlate(aliases);
    assert(aliases.vidConflict && !aliases.observedVidBytes);
    assert(toJson(ambiguous).value(QStringLiteral("observed_vid_bytes")).toString() == QStringLiteral("8192"));
    std::cout << "HYPERV_CORRELATION_TESTS=PASS\n";
}
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.contains(QStringLiteral("--self-test"))) { selfTest(); return 0; }
    const int output = arguments.indexOf(QStringLiteral("--output"));
    if (output < 0 || output + 1 >= arguments.size()) { std::cerr << "Specify --self-test or --output <json-file>\n"; return 2; }
    auto job = std::make_shared<Job>();
    collect(job);
    if (!job->result) { return 3; }
    QFile file(arguments[output + 1]);
    if (!file.open(QIODevice::WriteOnly)) { return 4; }
    const auto json = QJsonDocument(toJson(*job->result)).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()) { return 5; }
    std::cout << "HYPERV_PROBE_WRITTEN partitions=" << job->result->partitions.size() << " counters=" << job->result->counters.size() << "\n";
    return 0;
}
