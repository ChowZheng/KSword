#include "TestSupport.h"
#include "../KswordARKLight/Features/Process/ProcessDetails.h"
#include "../KswordARKLight/Features/Process/ProcessTelemetry.h"

#include <algorithm>

int RunProcessInformationTests() {
    using namespace Ksword::Features::Process;
    using C = ProcessColumnId;
    KswordTests::Suite suite(L"ProcessInformation");
    ProcessSnapshotRow row{};
    row.processId = 123;
    row.creationTime100ns = 456;
    ks::process::ProcessRecord details{};
    details.pid = row.processId;
    details.creationTime100ns = row.creationTime100ns;
    details.staticDetailsReady = true;
    details.userName = "DOMAIN\\Alice";
    details.commandLine = "test.exe --probe";
    details.signatureState = "Unsigned";
    details.packageNameKnown = true;
    details.uacVirtualizationState = ks::process::ProcessFeatureState::NotAllowed;
    details.controlFlowGuardState = ks::process::ProcessFeatureState::Enabled;
    details.dpiAwarenessLevel = ks::process::ProcessDpiAwarenessLevel::PerMonitorAwareV2;
    ApplyProcessDetailRecord(row, details);
    suite.expect(ProcessColumnText(row, C::User) == L"DOMAIN\\Alice", L"token user reaches the actual table formatter");
    suite.expect(ProcessColumnText(row, C::CommandLine) == L"test.exe --probe", L"command line is preserved");
    suite.expect(ProcessColumnText(row, C::PackageName) == L"无程序包", L"unpackaged is a known result");
    suite.expect(ProcessColumnText(row, C::UacVirtualization) == L"不适用" &&
        ProcessColumnText(row, C::ControlFlowGuard) == L"已启用", L"policy enums are human readable");
    suite.expect(ProcessColumnText(row, C::DpiAwareness) == L"每监视器 V2", L"DPI level is human readable");
    suite.expect(ProcessColumnText(row, C::DataExecutionPrevention) == L"查询失败或不支持", L"unknown policy does not mean disabled");
    suite.expect(ProcessColumnText(row, C::IsAdmin) == L"令牌访问受限", L"unknown elevation does not mean non-admin");
    details.isAdminKnown = true;
    ApplyProcessDetailRecord(row, details);
    suite.expect(ProcessColumnText(row, C::IsAdmin) == L"否", L"known non-admin token is displayed explicitly");
    details.creationTime100ns++;
    details.userName = "WRONG";
    ApplyProcessDetailRecord(row, details);
    suite.expect(ProcessColumnText(row, C::User) == L"DOMAIN\\Alice", L"recycled PID cannot overwrite metadata");
    row.basePriority = -2;
    suite.expect(ProcessColumnText(row, C::BasePriority) == L"-2", L"signed base priority remains signed");
    const auto demand = DetailDemandForColumns({ C::EnterpriseContext, C::JobObject, C::HardwareStackProtection, C::PackageName });
    suite.expect((demand & ks::process::ProcessDetailDemand::EnterpriseContext) != 0 &&
        (demand & ks::process::ProcessDetailDemand::JobObject) != 0 &&
        (demand & ks::process::ProcessDetailDemand::MitigationPolicy) != 0 &&
        (demand & ks::process::ProcessDetailDemand::PackageName) != 0, L"every optional security collector is requested");
    suite.expect(DetailDemandForColumns({ C::Name, C::Pid }) == ks::process::ProcessDetailDemand::None,
        L"basic columns incur no optional demand");
    details.creationTime100ns = row.creationTime100ns;
    details.gpuUsageKnown = true;
    details.gpuPercent = 12.5;
    details.gpuEngineText = "GPU 0 - 3D";
    ApplyProcessDetailRecord(row, details);
    suite.expect(ProcessColumnText(row, C::Gpu) == L"12.5%" && ProcessColumnText(row, C::GpuEngine) == L"GPU 0 - 3D",
        L"GPU samples reach the table");
    details.gpuPercent = 0;
    details.gpuEngineText.clear();
    ApplyProcessDetailRecord(row, details);
    suite.expect(ProcessColumnText(row, C::Gpu) == L"0.0%" && ProcessColumnText(row, C::GpuEngine) == L"无活动引擎",
        L"idle GPU is a valid zero sample");
    details.gpuUsageKnown = false;
    ApplyProcessDetailRecord(row, details);
    suite.expect(ProcessColumnText(row, C::Gpu) != L"0.0%", L"failed GPU query is not a measured zero");
    row.privatePageBytes = 8192;
    row.privateWorkingSetBytes = 1024;
    suite.expect(ProcessColumnText(row, C::PrivateWorkingSet) == L"1.0 KiB", L"private working set is not private commit");
    row.r0AuditSummary = L"不可用";
    suite.expect(ProcessColumnText(row, C::R0Status) != L"已审计", L"failed R0 query is not a completed audit");
    ProcessTelemetry telemetry;
    std::vector<ProcessSnapshotRow> samples{row};
    samples[0].ioReadBytes = 100;
    samples[0].ioWriteBytes = 200;
    telemetry.Sample(samples, {C::Disk}, 1000);
    suite.expect(!samples[0].diskRateKnown, L"first I/O sample establishes a baseline");
    samples[0].ioReadBytes += 1024;
    samples[0].ioWriteBytes += 1024;
    telemetry.Sample(samples, {C::Disk}, 3000);
    suite.expect(samples[0].diskRateKnown && samples[0].diskBytesPerSecond == 1024 &&
        ProcessColumnText(samples[0], C::Disk) == L"1.0 KiB/s", L"I/O throughput uses the actual sample interval");
    samples[0].creationTime100ns++;
    telemetry.Sample(samples, {C::Disk}, 4000);
    suite.expect(!samples[0].diskRateKnown, L"recycled PID resets the I/O baseline");
    samples[0].ioReadBytes = 0;
    telemetry.Sample(samples, {C::Disk}, 5000);
    suite.expect(!samples[0].diskRateKnown, L"counter regression resets the I/O baseline");
    ProcessSnapshotRow kernelOnly{};
    kernelOnly.r0KernelOnly = true;
    suite.expect(ProcessColumnText(kernelOnly, C::WorkingSet) == L"仅 R0：未返回该字段",
        L"missing R0-only counters are not fabricated zeros");
    const auto snapshot = EnumerateProcessesByNtQuerySystemInformation();
    const auto self = std::find_if(snapshot.rows.begin(), snapshot.rows.end(), [](const auto& item) {
        return item.processId == ::GetCurrentProcessId();
    });
    suite.expect(snapshot.success && self != snapshot.rows.end(), L"live NtQuery enumeration includes self");
    if (self != snapshot.rows.end()) {
        suite.expect(!self->imagePath.empty(), L"live self image path is collected");
        suite.expect(ProcessColumnText(*self, C::StartTime).find(L'-') != std::wstring::npos,
            L"start time is formatted from the native snapshot without static queries");
    }
    suite.report();
    return suite.failures();
}
