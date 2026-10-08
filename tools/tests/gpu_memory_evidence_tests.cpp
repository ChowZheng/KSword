#include "../../shared/evidence/GpuMemoryEvidence.h"
#include <cassert>
#include <iostream>
int main(int argc, char**)
{
    using namespace ksword::gpu_memory;
    assert(processId(L"pid_123_luid_0x1_phys_0") == 123);
    assert(processId(L"pid_4294967295_luid_0") == 4294967295U);
    assert(processId(L"pid_4294967296_luid_0") == 0);
    assert(processId(L"pid_123x_luid_0") == 0);
    assert(processId(L"pid__luid_0") == 0);
    assert(processId(L"pid_123") == 0);
    assert(processId(L"luid_0_phys_0") == 0);
    Snapshot cancelled;
    std::atomic_bool stop{true};
    collect(cancelled, stop);
    assert(cancelled.cancelled && cancelled.counters.empty());
    if (argc > 1) {
        Snapshot live;
        stop.store(false);
        collect(live, stop);
        std::cout << "GPU_PROBE_COUNTERS=" << live.counters.size() << " QUERY_STATUS=" << live.queryStatus << '\n';
        for (const auto& row : live.counters) {
            assert(row.valueKnown || row.bytes == 0);
            assert(!row.processIdentityKnown || row.processCreateTime != 0);
        }
    }
    std::cout << "GPU_MEMORY_EVIDENCE_TESTS=PASS\n";
}
