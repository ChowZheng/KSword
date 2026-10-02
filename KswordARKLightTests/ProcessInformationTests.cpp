#include "TestSupport.h"
#include "../KswordARKLight/Features/Process/ProcessDetails.h"

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
