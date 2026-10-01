# KSword MSVC / WDK 构建恢复

## Qt 运行库部署需要 VS 开发环境

- 直接从普通 PowerShell 调用 MSBuild 可以完成主程序链接，但构建后
  `windeployqt --compiler-runtime` 可能因 `VCINSTALLDIR` 未设置而返回 1，最终报 MSB3073。
  先通过当前 VS 的 `Common7/Tools/VsDevCmd.bat -arch=x64 -host_arch=x64` 初始化环境，
  再运行原有 `/t:Build`；确认 Qt/VC 运行库部署与整个 Build 都成功。不要仅凭 exe 已更新
  宣称完整构建通过，也不要跳过部署来掩盖环境错误。
- 主程序 `DeployQtRuntime` 使用增量 windeployqt，不再默认 `--force`。已经相同的 Qt DLL
  可能被辅助程序占用；强制删除会报 Cannot remove existing file。增量部署仍复制缺失或更新
  的依赖，不关闭部署目标。2026-10-01 现场已确认 Release/Qt6Core.dll 与 Qt 6.9.3 源 DLL
  SHA256 相同，无 `--force` 部署正常通过。

适用范围：`C:\Users\Felix\CLionProjects\KSword` 的主程序 Release/x64 构建与 `KswordARKDriver` WDK 后置验证。

## 主程序 `LNK1000 IMAGE::BuildImage`

- 项目只使用标准 MSVC、仓库 Qt 与 QtMsBuild；不要切换 LLVM、`amd64\MSBuild.exe`、替代 TargetName，也不要因为一次链接器内部错误自动升级或降级 MSVC。
- 当同一任务出现 `LNK1000`、`IMAGE::BuildImage` 或 `.iobj`，只执行一次 `Invoke-KSwordBuildCheck.ps1 -Action Rebuild -DisableWholeProgramOptimization`。该脚本在临时 props 中关闭 WPO/LTCG，结束后移除该 props，不会改动工程。
- 通过条件必须同时是 `BUILD_RESULT=SUCCESS`、`EXIT_CODE=0`，以及非零 `Ksword5.1\x64\Release\Ksword5.1.exe`。WPO 禁用构建会使普通增量缓存失效；不要紧接着再跑普通 Build，只用 `-VerifyArtifactOnly` 做读回。
- 只有这条构建局部恢复路径仍复现后，才考虑 VS servicing update 或并列 v143 工具集。

## 驱动 x64 WDK 后置验证

- 要把 `.sys` 的编译/链接、`ApiValidator`、`Inf2Cat`、签名和实际加载分别报告。`ApiValidator` 通过不等于发行或可加载。
- 已观察到 WDK 后置阶段会误选 ARM64 `ApiValidator`/`aitstatic`，或在 `KswordARK.sys` 链接更新后无子进程地卡住。先确认输出 `.sys` 的时间戳/哈希已更新，并确认指定 MSBuild PID 没有活跃的 `cl.exe`、`link.exe`、`ApiValidator.exe` 或 `aitstatic.exe` 子进程，才能停止该唯一的卡住进程。
- 用 `/t:ApiValidator` 配合 `/p:ApiValidator_ApiExtractorExePath='C:\Program Files (x86)\Windows Kits\10\bin\<已安装版本>\x64'` 验证刚链接的 x64 `.sys`；`Driver is 'Universal'.` 是这一独立阶段的成功标志。
- 若失败发生在 `.sys` 链接之前，后置验证不能掩盖它，必须修复真实的编译/链接报错。
