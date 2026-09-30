# KSword MSVC / WDK 构建恢复

适用范围：`C:\Users\Felix\CLionProjects\KSword` 的主程序 Release/x64 构建与 `KswordARKDriver` WDK 后置验证。

## 主程序 `LNK1000 IMAGE::BuildImage`

- 项目只使用 64 位 MSBuild 与标准 MSVC HostX64 工具、仓库 Qt 与 QtMsBuild；不要切换 LLVM、替代 TargetName，也不要因为一次链接器内部错误自动升级或降级 MSVC。构建入口统一为仓库 `tools/Invoke-KSwordBuildCheck.ps1`。
- 当同一任务出现 `LNK1000`、`IMAGE::BuildImage` 或 `.iobj`，只执行一次 `tools/Invoke-KSwordBuildCheck.ps1 -Action Rebuild -DisableWholeProgramOptimization`。该脚本在临时 props 中关闭 WPO/LTCG，结束后移除该 props，不会改动工程。
- 通过条件必须同时是 `BUILD_RESULT=SUCCESS`、`EXIT_CODE=0`，以及非零 `Ksword5.1\x64\Release\Ksword5.1.exe`。WPO 禁用构建会使普通增量缓存失效；不要紧接着再跑普通 Build，只用 `-VerifyArtifactOnly` 做读回。
- 只有这条构建局部恢复路径仍复现后，才考虑 VS servicing update 或并列 v143 工具集。

## 主程序 `C1002` 与工具架构降级

- Codex 子进程可能同时缺少 `PROCESSOR_ARCHITECTURE` 与 `PROCESSOR_ARCHITEW6432`。MSVC 的 `Microsoft.Cpp.ToolsetLocation.props` 会因此把工程内的 `PreferredToolArchitecture=x64` 改为 x86；大工程的 LTCG 随后可能报 `C1002`，32 位链接器再自行重试 64 位也会留下失败退出码，不能当作完整构建通过。
- 使用仓库构建脚本：它仅选择 `Bin/amd64/MSBuild.exe`，校验入口 PE，并同时传入 `/p:PreferredToolArchitecture=x64`、`/p:PROCESSOR_ARCHITECTURE=AMD64` 与 `/p:PROCESSOR_ARCHITEW6432=AMD64`。构建前读回 `PreferredToolArchitecture` 与 `VC_ExecutablePath_x64`，未解析成 HostX64 即拒绝。
- `ToolsetLocation.props` 的根节点含 `TreatAsLocalProperty="PreferredToolArchitecture"`，会覆盖该全局属性；只传工具偏好仍不够。2026-09-30 的只读 MSBuild 属性输出证实：两项宿主架构为空时工具偏好仍变成 x86。
- CE 的发布入口、调试适配 DLL 与项目构建仅使用 x64。

## Qt 部署重复覆盖

- 2026-09-30 观察到 `windeployqt --force` 无法删除已有 `Release/Qt6Core.dll`，其 SHA256 与仓库 Qt SDK 完全相同。标准增量部署成功输出 `Qt6Core.dll is up to date.` 并完成其他依赖；主程序部署目标已移除无条件 `--force`，仍由 windeployqt 更新有变化的依赖，并传入 `VCINSTALLDIR`。
- 编译/链接成功但 Qt 部署失败时，整体 Build 仍是失败，不能以新 exe 已生成替代后置部署通过。

## 驱动 x64 WDK 后置验证

- 要把 `.sys` 的编译/链接、`ApiValidator`、`Inf2Cat`、签名和实际加载分别报告。`ApiValidator` 通过不等于发行或可加载。
- 已观察到 WDK 后置阶段会误选 ARM64 `ApiValidator`/`aitstatic`，或在 `KswordARK.sys` 链接更新后无子进程地卡住。先确认输出 `.sys` 的时间戳/哈希已更新，并确认指定 MSBuild PID 没有活跃的 `cl.exe`、`link.exe`、`ApiValidator.exe` 或 `aitstatic.exe` 子进程，才能停止该唯一的卡住进程。
- 用 `/t:ApiValidator` 配合 `/p:ApiValidator_ApiExtractorExePath='C:\Program Files (x86)\Windows Kits\10\bin\<已安装版本>\x64'` 验证刚链接的 x64 `.sys`；`Driver is 'Universal'.` 是这一独立阶段的成功标志。
- 若失败发生在 `.sys` 链接之前，后置验证不能掩盖它，必须修复真实的编译/链接报错。
