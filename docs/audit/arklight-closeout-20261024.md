# KswordARKLight 收尾记录（2026-10-24）

## 范围和依据

- 读取 `.claude/memory/MEMORY.md`、Light 调查工作台与 R3 迁移记忆、UI 架构记忆，以及 `.workbuddy/memory/MEMORY.md` 和两份日期记录。
- 用户确认本轮仅收尾 Light；UI CPU 采样工具、动态偏移调查笔记和 `.tmp/` 原有文件保留在工作区，不纳入提交。
- 按用户后续要求停止 Computer Use，不继续进行 GUI 操作或 UI 合规性评估。
- 本轮围绕表格快照接线提交一个完整改动单元，不合并远端、不重写既有历史、不推送。

## 源码核对与修正

1. 保留进程主表新增的采集、比对、清除菜单。采集复用当前展示快照与筛选映射，不重新枚举进程，不深拷贝整份进程模型；回调持有对应快照的共享所有权。
2. 修复跨进程列组比对的列身份问题：共享 `TableCompareRows` 可声明与表头平行的稳定字段 ID。采集入口把字段 ID 映射回当前单元格位置；进程表使用既有 `ProcessColumnId`。固定列结构的旧调用继续使用位置编号。
3. 采集拒绝列数不匹配、重复 ID、负 ID，不替换已保留快照。基准 A 及原采集上限不变。
4. 将行源采集和采集说明移入 `Ui/TableCompareRowSource.cpp`，由生产和测试工程共同编译；两个 `.vcxproj` 与两个 `.vcxproj.filters` 均已登记。菜单与结果窗口仍由原共享入口负责。
5. 七个既有调用补充表名，用于共享比对窗口标题和建议导出文件名。杂项审计使用当前子页标题，避免六个审计页显示同一通用名称。
6. 原注释将进程快照描述为可区分 PID 复用，超出了现有比对保证，已修正。进程表没有配置稳定键列，仍明确使用共享引擎的整行比对回退，不宣称按 PID/创建时间进行字段级身份比对。
7. 新增 12 条生产行源适配回归；`T table snapshot stress` 同时补入必跑套件清单，下限为 151 条，避免整个套件缺席时仅凭退出码误判通过。

## 入口核实

以下仅记录源码中的真实入口和本轮接线范围，不作为视觉验收结论。参照服务页、网络审计页和驱动调试输出页的已有菜单与行源接线。

| 导航入口 | 源码与共享入口 | 本轮内容 |
| --- | --- | --- |
| 进程（模块 40001） | `ProcessFeature` → `ProcessView`；裸 owner-data 表 + `TableCompareRows` | 新增快照菜单，使用稳定字段 ID 和当前筛选映射 |
| 硬件 → USB 设备 / 系统总线 | `HardwareFeature` 的 `WorkspaceHost` 工厂 → `VirtualListView` | 补充比对表名 |
| 系统工具 → 文件占用 / 事件日志 / 右键菜单 | `SysToolsFeature` 的 `WorkspaceHost` 工厂 → `VirtualListView` | 补充比对表名 |
| 杂项安全 → Code Integrity / WDAC、VBS / HVCI / SKCI、Hyper-V / VMBus / HvSocket、AppLocker / AppID、BAM / ahcache、Bugcheck / VMware branding | `MiscFeature::CreateAuditView` → `VirtualListView` | 六个实例分别沿用 `state.title` |
| 窗口捕获保护独立页：当前无导航入口 | `WindowToolsCaptureView` 中保留的 `CreateCaptureProtectionView`，仅有声明与定义 | 保留表名修正；不能计作用户可达页面。窗口模块实际注册的是窗口管理、剪贴板查看、热键占用探测、层级诊断 |

共享行源文件拆出后，已有 `VirtualListView` 快照调用仍经相同入口执行。其列身份未改变；回归同时覆盖空 `sourceColumns` 的旧位置编号路径。没有增加 CLI 命令、修改主程序工程或驱动协议；Light 延续中文 Win32 路线。

## 验证

- 源码：工程/filters 注册、UTF-8、测试清单名称唯一性和差异空白检查通过。新行源文件 90 行，原压力测试文件增加后为 861 行。
- Light：Release/x64 Build 成功，退出码 0，耗时约 35 秒；使用 64 位 MSBuild 与 v143 HostX64 编译器/链接器，并传入三个架构属性。复用现有驱动，不执行驱动构建或签名。
- Light 构建仍有 17 条警告，位于本轮未改动的 `KernelPage.cpp` 与 `ArkDriverKernel.cpp` 相关实例化路径；不能将本次增量构建描述为全仓零警告。
- 测试：Release/x64 `/W4 /WX` 构建成功，退出码 0，0 警告、0 错误。`scripts/Test-KswordSuiteManifest.ps1` 实际执行完整测试，43/43 套件通过，`KSWORD_SUITE_MANIFEST=PASS`，退出码 0；快照套件 151/151 条通过，包含新增的 12 条行源适配断言。
- GUI：用户已要求停止 Computer Use 和合规性评估。此前实际程序启动成功，但窗口捕获两次超时（`FrameArrived timed out` / `window capture timed out`）；没有完成页面视觉验收，不以离屏或伪窗口替代。
- 本地日志：`.codex-build-logs/light-closeout-20261024/`。构建与测试产物保留在原工程输出目录，生成文件不提交。
