// ============================================================
// wpK1_header_contract.cpp
// 作用：MemoryDock.WorkbenchServices.h 冻结接口的"契约探针"，只做语法检查（cl /Zs），
//       由 build-wpK1-check.cmd 编译：
//       把七个自由函数逐个绑定到 MemoryWorkbenchView 对应注入点期望的 std::function 类型上，
//       任何一个签名被改动（参数、返回类型、命名空间）都会在这里编译失败——后续接线步骤依赖的
//       正是这些形状。
// 说明：
// - 没有任何运行期行为，不被链接进任何产物；
// - 注入点类型取自 UI/MemoryWorkbench/MemoryWorkbenchView.h 的 setXxxProvider 声明；
// - QueryProtection 的注入点只带地址，pid 由接线步骤绑定，所以这里按 (pid, address) 绑定。
// ============================================================

#include "Framework.h"
#include "MemoryDock/MemoryDock.WorkbenchServices.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace
{
    // ContractProbe：把七个函数绑定到注入点类型。函数本身从不被调用。
    [[maybe_unused]] void ContractProbe()
    {
        // 装配入口：void()。
        void (*configure)() = &ks::ui::workbench_dock::ConfigureShared;
        // MemoryWorkbenchView::setGateInputsProvider：function<GateInputs()>。
        std::function<ksword::memwb::GateInputs()> gate = &ks::ui::workbench_dock::QueryGateInputs;
        // 状态条"保护"段：接线步骤把 pid 绑进去之后是 function<optional<WorkbenchProtectionInfo>(uint64)>，
        // 这里按未绑定的 (pid, address) 形状核对。
        std::function<std::optional<ks::ui::WorkbenchProtectionInfo>(std::uint32_t, std::uint64_t)> protection =
            &ks::ui::workbench_dock::QueryProtection;
        // MemoryWorkbenchView::setAttachedProcessInfoProvider：function<optional<AttachedProcessDisplayInfo>(uint32)>。
        std::function<std::optional<ks::ui::AttachedProcessDisplayInfo>(std::uint32_t)> attached =
            &ks::ui::workbench_dock::QueryAttachedProcessInfo;
        // MemoryWorkbenchView::setGlobalSkipDangerousConfirmProvider：function<bool()>。
        std::function<bool()> skipConfirm = &ks::ui::workbench_dock::GlobalSkipDangerousConfirm;
        // MemoryWorkbenchView::setDisasmBackends(DecodeOneFn, AssembleOneFn)。
        ks::ui::DecodeOneFn decode = ks::ui::workbench_dock::MakeDecodeBackend();
        ks::ui::AssembleOneFn assemble = ks::ui::workbench_dock::MakeAssembleBackend();

        (void)configure;
        (void)gate;
        (void)protection;
        (void)attached;
        (void)skipConfirm;
        (void)decode;
        (void)assemble;
    }
}
