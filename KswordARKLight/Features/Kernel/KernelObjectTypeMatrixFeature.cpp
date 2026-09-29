#include "KernelObjectTypeMatrixFeature.h"

namespace Ksword::Features::Kernel {

KernelFeatureDescriptor CreateObjectTypeMatrixDescriptor() {
    // The descriptor is intentionally pure data. Driver calls and UI controls
    // are kept out of this feature file so parallel sessions can wire them
    // through KernelFacade and KernelPage without changing this ownership.
    KernelFeatureDescriptor descriptor;
    descriptor.id = KernelFeatureId::ObjectTypeMatrix;
    descriptor.title = L"对象类型矩阵";
    descriptor.category = L"对象命名空间";
    descriptor.summary = L"合并 NtQueryObject 类型统计与 R0 ObTypeIndexTable 槽位、索引和名称验证。";
    descriptor.backend = KernelFeatureBackend::Hybrid;
    descriptor.requiresAdministrator = false;
    descriptor.mayModifyKernelState = false;
    return descriptor;
}

} // namespace Ksword::Features::Kernel
