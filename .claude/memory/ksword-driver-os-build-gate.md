# OS build 上限与 DynData fail-closed

## 当前策略（2026-10-03，Issue #213）

- 用户明确要求注释整个 OS build 上限防护，而不是将 26100 改为 26300。该要求覆盖下述历史策略。
- `KSWORD_ARK_MAXIMUM_SUPPORTED_OS_BUILD` 定义保留为注释；启动支持标志仅检查最低 build 16299，不再限制更高 build。
- `ioctl_dispatch.c` 的 build 拦截及其白名单辅助函数整体注释；DynData runtime fallback 与卸载驱动布局 fallback 的 build 防护也已注释。逐功能能力、偏移、IRQL、地址与布局校验继续生效。
- Launcher 上限启动拦截整体注释，因此旧清单也不会恢复上限拦截；默认值及源/发布清单同步为 `advertisedMaximumBuild=0`、`allowNewerWindows11=true`。`advertisedRange` 仍记录原有验证范围，不代表更高 build 已验证。
- 解除上限不会生成精确 PE/PDB profile，也不代表所有驱动功能在新系统可用。不得因旧记忆自动恢复上限。

## 历史策略（已停用）

- `shared/KswordArkStartupProtocol.h` 的 `KSWORD_ARK_MAXIMUM_SUPPORTED_OS_BUILD` 必须与 Launcher 支持清单的 `osPolicy.advertisedMaximumBuild` 对齐；当前上限为 26100。
- `KswordArkStartupGetOsBuildNumber` 设置一次启动级支持标志。驱动仍可加载以提供诊断，但 `KswordARKDynDataActivateRuntimeOffsets` 在上限外不得运行 pattern/layout fallback。
- `ioctl_dispatch.c` 在上限外拒绝所有 offset-dependent IOCTL（包括 capability query），只保留 preflight 与静态 IOCTL registry；这样未知 build 不会因为“看起来结构相似”而触发不受支持的内核访问。
- Launcher 的 `allowNewerWindows11=false` 必须在 GUI 启动路径实际生效；内部 upload 模式可继续生成支持收集包。R0 gate 是最终安全边界，Launcher gate 只负责避免普通 GUI 误启动。
