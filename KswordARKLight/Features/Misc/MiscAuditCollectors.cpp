// 只读审计数据采集：杂项审计页的六个标签页在这里各自拼一次行快照，
// 视图只负责显示，不直接碰注册表/SCM/驱动 IOCTL。

#include "MiscAuditPage.h"

#include "../../../shared/usermode/backend/security/AppLocker.h"
#include "../../../shared/usermode/backend/security/BamAhcache.h"
#include "../../../shared/usermode/backend/security/BugcheckEvidence.h"
#include "../../../shared/usermode/backend/security/CodeIntegrity.h"
#include "../../../shared/usermode/backend/security/HyperV.h"
#include "../../../shared/usermode/backend/security/Vbs.h"
#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <sstream>
#include <string>
#include <vector>

namespace Ksword::Features::Misc {
namespace {

using namespace ks::r3::security;

// AddDriverCapabilityRow queries existing ArkDriverClient capability status as
// the only R0-facing check in this module. Inputs are a row list and scope label;
// processing calls the existing wrapper and never opens DeviceIoControl directly;
// no value is returned.
void AddDriverCapabilityRow(std::vector<MiscAuditRow>& rows, const std::wstring& scope) {
    const ksword::ark::DriverClient client;
    const ksword::ark::DriverCapabilitiesQueryResult capability = client.queryDriverCapabilities();
    if (capability.io.ok) {
        std::wostringstream detail;
        detail << L"Protocol=" << capability.driverProtocolVersion
               << L"; StatusFlags=0x" << std::hex << std::uppercase << capability.statusFlags
               << L"; DynData=0x" << capability.dynDataStatusFlags
               << L"; Returned=" << std::dec << capability.returnedFeatureCount << L"/" << capability.totalFeatureCount;
        AppendRow(rows, scope, L"KswordARK R0 capability", L"Online", L"ArkDriverClient::queryDriverCapabilities", L"Info", detail.str());
    } else {
        std::wstring detail = L"R0 能力查询失败：Win32=" + std::to_wstring(capability.io.win32Error) + L"; " +
            std::wstring(capability.io.message.begin(), capability.io.message.end());
        AppendRow(rows, scope, L"KswordARK R0 capability", L"Unavailable", L"ArkDriverClient::queryDriverCapabilities", L"Unknown", detail);
    }
}

// AddSecurityAuditRows 调用新增 ArkDriverClient 安全审计 wrapper。
// 输入：目标 rows 和页面 scope。
// 处理：按当前页面追加 Security/DriverTrust/HyperV/AppControl 的只读摘要。
// 返回：无返回值；所有失败原因保留在详情列，不裸 DeviceIoControl。
void AddSecurityAuditRows(std::vector<MiscAuditRow>& rows, const std::wstring& scope) {
    const ksword::ark::DriverClient client;
    const auto security = client.querySecurityStatus();
    {
        std::wostringstream detail;
        detail << L"Win32=" << security.io.win32Error
               << L"; QueryStatus=0x" << std::hex << std::uppercase << static_cast<unsigned long>(security.response.queryStatus)
               << L"; CIOptions=0x" << security.response.codeIntegrityOptions
               << L"; SecureBoot=" << std::dec << security.response.secureBootEnabled
               << L"; VBS=" << security.response.vbsPresent
               << L"; HVCI=" << security.response.hvciKmciEnabled
               << L"; TestSigning=" << security.response.testSigningEnabled;
        AppendRow(rows, scope, L"R0 SecurityStatus", security.io.ok ? L"OK" : (security.unsupported ? L"Unsupported" : L"Unavailable"), L"ArkDriverClient::querySecurityStatus", security.io.ok ? L"Info" : L"Unknown", detail.str());
    }

    const auto trust = client.queryDriverTrustView();
    {
        std::wostringstream detail;
        detail << L"Win32=" << trust.io.win32Error
               << L"; total=" << trust.totalCount
               << L"; returned=" << trust.returnedCount
               << L"; truncated=" << trust.truncated
               << L"; moduleStatus=0x" << std::hex << std::uppercase << static_cast<unsigned long>(trust.moduleQueryStatus);
        AppendRow(rows, scope, L"R0 DriverTrustView", trust.io.ok ? L"OK" : (trust.unsupported ? L"Unsupported" : L"Unavailable"), L"ArkDriverClient::queryDriverTrustView", trust.truncated ? L"Partial" : L"Info", detail.str());
    }

    const auto hyperv = client.queryHyperVSummary();
    {
        std::wostringstream detail;
        detail << L"Win32=" << hyperv.io.win32Error
               << L"; Hypervisor=" << hyperv.response.hypervisorPresent
               << L"; VMBus=" << hyperv.response.vmbusStatus
               << L"; vSwitch=" << hyperv.response.vSwitchStatus
               << L"; vPCI=" << hyperv.response.vPciStatus
               << L"; vendor=" << hyperv.response.hypervisorVendor;
        AppendRow(rows, scope, L"R0 HyperVSummary", hyperv.io.ok ? L"OK" : (hyperv.unsupported ? L"Unsupported" : L"Unavailable"), L"ArkDriverClient::queryHyperVSummary", hyperv.io.ok ? L"Info" : L"Unknown", detail.str());
    }

    const auto appControl = client.queryAppControlStatus();
    {
        std::wostringstream detail;
        detail << L"Win32=" << appControl.io.win32Error
               << L"; AppID=" << appControl.response.appidStatus
               << L"; AppLockerFilter=" << appControl.response.appLockerFilterStatus
               << L"; mssecflt=" << appControl.response.mssecfltStatus
               << L"; BAM=" << appControl.response.bamStatus
               << L"; owner=" << appControl.response.appLockerOwnerModule;
        AppendRow(rows, scope, L"R0 AppControlStatus", appControl.io.ok ? L"OK" : (appControl.unsupported ? L"Unsupported" : L"Unavailable"), L"ArkDriverClient::queryAppControlStatus", appControl.io.ok ? L"Info" : L"Unknown", detail.str());
    }
}

// CollectCodeIntegrityRows gathers CI/WDAC posture evidence through documented
// R3 queries and registry reads. There is no input; output is a row vector for
// the Code Integrity / WDAC tab.
std::vector<MiscAuditRow> CollectCodeIntegrityRows() {
    std::vector<MiscAuditRow> rows;
    AddDriverCapabilityRow(rows, L"Code Integrity / WDAC");
    AddSecurityAuditRows(rows, L"Code Integrity / WDAC");
    ks::r3::security::AppendCodeIntegrityR3(rows);

    return rows;
}

// CollectVbsRows gathers VBS/HVCI/SKCI evidence from DeviceGuard CIM, systeminfo
// and service/module presence. There is no input; output is a row vector for the
// VBS/HVCI/SKCI tab.
std::vector<MiscAuditRow> CollectVbsRows() {
    std::vector<MiscAuditRow> rows;
    AddDriverCapabilityRow(rows, L"VBS / HVCI / SKCI");
    AddSecurityAuditRows(rows, L"VBS / HVCI / SKCI");
    ks::r3::security::AppendVbsR3(rows);

    return rows;
}

// CollectHyperVRows gathers Hyper-V, VMBus, vSwitch, vPCI and HvSocket posture
// with read-only service/module/CIM evidence. There is no input; output is a row
// vector for the Hyper-V tab.
std::vector<MiscAuditRow> CollectHyperVRows() {
    std::vector<MiscAuditRow> rows;
    AddDriverCapabilityRow(rows, L"Hyper-V / VMBus / HvSocket");
    AddSecurityAuditRows(rows, L"Hyper-V / VMBus / HvSocket");
    ks::r3::security::AppendHyperVR3(rows);

    return rows;
}

// CollectAppLockerRows gathers AppID/AppLocker/appid.sys/applockerfltr/mssecflt
// evidence without policy mutation or rule export. There is no input; output is
// a row vector for the AppLocker tab.
std::vector<MiscAuditRow> CollectAppLockerRows() {
    std::vector<MiscAuditRow> rows;
    AddDriverCapabilityRow(rows, L"AppLocker / AppID");
    AddSecurityAuditRows(rows, L"AppLocker / AppID");
    ks::r3::security::AppendAppLockerR3(rows);

    return rows;
}

// CollectAuxiliaryRows gathers BAM and ahcache availability in privacy-preserving
// summary mode. There is no input; output is a row vector for the BAM/ahcache tab.
std::vector<MiscAuditRow> CollectAuxiliaryRows() {
    std::vector<MiscAuditRow> rows;
    AddDriverCapabilityRow(rows, L"BAM / ahcache");
    AddSecurityAuditRows(rows, L"BAM / ahcache");
    ks::r3::security::AppendBamAhcacheR3(rows);

    return rows;
}

// CollectBugcheckRows reports only feature applicability. The R0 feature is
// VMware-only and its valid upload path is intentionally quiet when inactive,
// so this page never claims that a successful transport activated a panel.
std::vector<MiscAuditRow> CollectBugcheckRows() {
    std::vector<MiscAuditRow> rows;
    AddDriverCapabilityRow(rows, L"Bugcheck / VMware branding");
    ks::r3::security::AppendBugcheckEvidenceR3(rows);
    AppendRow(rows, L"Bugcheck / VMware branding", L"R0 feature scope", L"VMware-only", L"KswordARK bugcheck runtime", L"Info",
        L"驱动仅在检测到受支持的 VMware 显示环境时启用该诊断面板；非 VMware 环境会安全忽略合法上传包。");
    AppendRow(rows, L"Bugcheck / VMware branding", L"内置测试位图", L"Explicit action required", L"ArkDriverClient::setBugcheckBitmap", L"Low",
        L"点击“上传内置位图”后才会在后台发送 16×16 BGRA 测试图；不会自动上传，也不会改变 Bugcheck 策略。传输成功不代表 VMware 面板已经激活。");

    return rows;
}

} // namespace

std::vector<ks::r3::security::MiscAuditRow> CollectRowsForPage(const MiscAuditPageId pageId) {
    switch (pageId) {
    case MiscAuditPageId::CodeIntegrity:
        return CollectCodeIntegrityRows();
    case MiscAuditPageId::VbsHvciSkci:
        return CollectVbsRows();
    case MiscAuditPageId::HyperV:
        return CollectHyperVRows();
    case MiscAuditPageId::AppLocker:
        return CollectAppLockerRows();
    case MiscAuditPageId::BamAhcache:
        return CollectAuxiliaryRows();
    case MiscAuditPageId::Bugcheck:
        return CollectBugcheckRows();
    default:
        return {};
    }
}

} // namespace Ksword::Features::Misc
