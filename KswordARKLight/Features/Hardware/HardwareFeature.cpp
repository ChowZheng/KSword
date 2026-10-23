#include "HardwareFeature.h"

#include "HardwareHwidDispatchView.h"
#include "HardwareView.h"
#include "../HardwareStats/BusDeviceView.h"
#include "../HardwareStats/DiskActivityView.h"
#include "../HardwareStats/PerformanceView.h"
#include "../HardwareStats/UsbTopologyView.h"
#include "../Kernel/KernelFeature.h"
#include "../../Ui/WorkspaceHost.h"

#include <utility>
#include <vector>

namespace Ksword::Features::Hardware {
namespace {

// 顶层页顺序对齐 Ksword5.1(Qt) HardwareDock 的 addTab 顺序（HardwareDock.cpp:5154-5169）：
// 性能监控 -> 处理器 -> 内存 -> 磁盘活动 -> 设备管理 -> 硬件标识设置 ->
// i8042prt 审计 -> USB 设备 -> 系统总线 -> 底层硬件检查。
// Qt 有而 Light 没有实现的页：硬件概览、电源、显卡、其他设备、设备驱动链。
// Light 独有的「物理磁盘扇区」「系统计时状态」放在末尾补充分组。
constexpr int kPerformanceTab = 61100;
constexpr int kCpuSnapshotTab = 61101;
constexpr int kMemoryLayoutTab = 61102;
constexpr int kDiskActivityTab = 61103;
constexpr int kRawDiskTab = 61104;
constexpr int kDeviceManagerTab = 61105;
constexpr int kHwidDispatchTab = 61106;
constexpr int kI8042Tab = 61107;
constexpr int kUsbTopologyTab = 61108;
constexpr int kBusDeviceTab = 61109;
constexpr int kCpuIntegrityTab = 61111;
constexpr int kSystemTimeTab = 61114;

// 内嵌内核页用独立的自定义控件 ID，避免与上面的 tab 标识混用。
constexpr int kCpuSnapshotPageId = 61120;
constexpr int kMemoryLayoutPageId = 61121;
constexpr int kRawDiskPageId = 61122;
constexpr int kI8042PageId = 61123;
constexpr int kCpuIntegrityPageId = 61124;
constexpr int kSystemTimePageId = 61125;
constexpr int kEmbeddedKernelPrimaryTabId = 51001;
constexpr int kEmbeddedKernelSecondaryTabId = 51002;

HWND CreateEmbeddedKernelPage(HWND host, const RECT& bounds, const int controlId, const Kernel::KernelFeatureId featureId) {
    HWND page = Kernel::CreateKernelSingleFeaturePage(host, controlId, bounds, featureId);
    if (page) {
        if (HWND primary = ::GetDlgItem(page, kEmbeddedKernelPrimaryTabId)) {
            ::ShowWindow(primary, SW_HIDE);
        }
        if (HWND secondary = ::GetDlgItem(page, kEmbeddedKernelSecondaryTabId)) {
            ::ShowWindow(secondary, SW_HIDE);
        }
    }
    return page;
}

} // namespace

HWND CreateHardwareFeaturePage(HWND parent, const RECT& bounds) {
    std::vector<Ksword::Ui::WorkspaceTabDescriptor> tabs;

    // 1 性能监控（Qt hardware.tab.utilization）
    tabs.push_back({ kPerformanceTab, L"性能监控", L"按需启动性能采样。",
        [](HWND host, const RECT& pageBounds) { return HardwareStats::CreatePerformanceView(host, pageBounds); } });
    // 2 处理器（Qt hardware.tab.cpu：逐逻辑处理器利用率）
    tabs.push_back({ kCpuSnapshotTab, L"处理器", L"按需加载 CPU 硬件快照。",
        [](HWND host, const RECT& pageBounds) {
            return CreateEmbeddedKernelPage(host, pageBounds, kCpuSnapshotPageId, Kernel::KernelFeatureId::CpuHardwareSnapshot);
        } });
    // 3 内存（Qt hardware.tab.memory）
    tabs.push_back({ kMemoryLayoutTab, L"内存", L"按需读取 MmGetPhysicalMemoryRanges 物理内存范围。",
        [](HWND host, const RECT& pageBounds) {
            return CreateEmbeddedKernelPage(host, pageBounds, kMemoryLayoutPageId, Kernel::KernelFeatureId::PhysicalMemoryLayout);
        } });
    // 4 磁盘活动（Qt hardware.tab.disk_activity）
    tabs.push_back({ kDiskActivityTab, L"磁盘活动", L"按需启动磁盘活动采样。",
        [](HWND host, const RECT& pageBounds) { return HardwareStats::CreateDiskActivityView(host, pageBounds); } });
    // 5 物理磁盘扇区（Light 独有，Qt 无对应页）
    tabs.push_back({ kRawDiskTab, L"物理磁盘扇区", L"按扇区对齐读取物理磁盘原始字节：过滤框填盘号、起点填偏移。",
        [](HWND host, const RECT& pageBounds) {
            return CreateEmbeddedKernelPage(host, pageBounds, kRawDiskPageId, Kernel::KernelFeatureId::RawDiskSectors);
        } });
    // 6 设备管理（Qt hardware.tab.device_manager）
    tabs.push_back({ kDeviceManagerTab, L"设备管理", L"设备页首次打开时枚举当前设备树。",
        [](HWND host, const RECT& pageBounds) { return CreateHardwareDeviceManagerView(host, pageBounds); } });
    // 7 硬件标识设置（Qt hardware.tab.hwid）
    tabs.push_back({ kHwidDispatchTab, L"硬件标识设置", L"按需加载 HWID Dispatch 审计与控制页。",
        [](HWND host, const RECT& pageBounds) { return CreateHardwareHwidDispatchView(host, pageBounds); } });
    // 8 i8042prt 审计（Qt hardware.tab.i8042_audit）
    tabs.push_back({ kI8042Tab, L"i8042prt 审计", L"只读枚举 i8042 键鼠过滤回调、承载模块与判定结果。",
        [](HWND host, const RECT& pageBounds) {
            return CreateEmbeddedKernelPage(host, pageBounds, kI8042PageId, Kernel::KernelFeatureId::I8042Audit);
        } });
    // 9 USB 设备（Qt hardware.tab.usb）
    tabs.push_back({ kUsbTopologyTab, L"USB 设备", L"按需枚举 USB 拓扑。",
        [](HWND host, const RECT& pageBounds) { return HardwareStats::CreateUsbTopologyView(host, pageBounds); } });
    // 10 系统总线（Qt hardware.tab.system_bus）
    tabs.push_back({ kBusDeviceTab, L"系统总线", L"按需枚举系统总线设备。",
        [](HWND host, const RECT& pageBounds) { return HardwareStats::CreateBusDeviceView(host, pageBounds); } });
    // 11 底层硬件检查（Qt hardware.tab.r0_evidence）
    tabs.push_back({ kCpuIntegrityTab, L"底层硬件检查", L"只读读取 CPU、IDT、描述符表与完整性证据。",
        [](HWND host, const RECT& pageBounds) {
            return CreateEmbeddedKernelPage(host, pageBounds, kCpuIntegrityPageId, Kernel::KernelFeatureId::KernelCpuIntegrity);
        } });
    // 12 系统计时状态（Light 独有，Qt 无对应页）
    tabs.push_back({ kSystemTimeTab, L"系统计时状态", L"只读查询性能计数器后端、倍率接管状态与计时槽地址。",
        [](HWND host, const RECT& pageBounds) {
            return CreateEmbeddedKernelPage(host, pageBounds, kSystemTimePageId, Kernel::KernelFeatureId::SystemTimeState);
        } });

    Ksword::Ui::WorkspaceOptions options{};
    options.tabControlId = 61110;
    options.initialTabId = kPerformanceTab;
    return Ksword::Ui::CreateWorkspaceHost(parent, bounds, std::move(tabs), std::move(options));
}

} // namespace Ksword::Features::Hardware
