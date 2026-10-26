# DynData v4 读取、提取与分模块回执

- 2026-10-11：协议/配置包含可选 `PspTerminateProcess`（1401），旧 GUI 的手写 ID switch 漏项，导致整份 NTOS profile 无效，EX/V4 都不下发。包格式合规不等于实际消费者能接受；回归必须执行生产 `loadV4ItemsFromJson`。
- 支持的特殊 ID 统一登记于 `shared/driver/KswordArkDynDataIoctl.h` 的 `KSW_DYN_V4_SUPPORTED_SPECIAL_ITEMS`；GUI 使用该头的 `KswordArkDynDataV4ItemIdSupported`，Python 发布器读取同一目录和数值宏。1004/1006 仍保留协议编号，但不进入接受目录。不要恢复独立的 GUI 数字名单。
- 原汇总会把 FLTMGR 成功的 7 项计入 NTOS 的已应用/状态，掩盖 NTOS 读取失败。现有兼容汇总只代表 NTOS；`profileModules` 分别记录 EX/V4 是否尝试、状态、本次接受及最终缓存接受，界面和复制报告共用。缓存接受不能覆盖本轮格式失败。
- 新增 21 个结构字段映射和 2 个句柄位域 shift 映射。`_SECTION.u1.ControlArea`、`_ALPC_PORT.u1.State` 必须按成员的确切 CodeView type ID 追踪匿名 union，不能按重复的 `<anonymous-tag>` 类型名匹配。
- `ObDecodeShift`/`ObAttributesShift` 是位位置，不是成员字节偏移；只从 `_HANDLE_TABLE_ENTRY` 的真实 LF_BITFIELD 元数据产生，并校验 64 位存储及对象指针位域终点。零偏移的 ALPC 成员合法，不能用真假值过滤。
- 当前内核 GUID `63950D24-7362-0017-E1FC-214544590A77`：profile 从 225 项增至 248 项，原有值及 PE/PDB 身份不变；EX 投影 209 项。其余 profile 没有本次补提取，完整 Release 矩阵仍为 2856 项、580004 items。
- `tools/test_dyndata_profile_loader.py` 从生产源码抽取实际 Qt 解析和 EX 投影，离线遍历 Release 包并验证未知/重复 ID、非法 uint32 和错误组计数；生成的源码登记在独立 vcxproj/filters 中，不调用驱动。`--source-ref` 可复现旧消费者拒绝合法包。
- 本次 Python 回归 8 项、Qt 全包 2856 profiles 加 4 个拒绝用例通过；主程序用仓库 HostX64 构建脚本。最初因运行中的 exe 锁定报 LNK1104，用户正常退出后构建成功。
- 没有停用 Runtime pattern，没有更改 R0 ABI 或驱动活动状态提交规则。EX 精确身份/条目校验失败时保留原活动状态；PDB 接受会覆盖候选中的对应字段来源。离线校验和构建不替代用户的实际下发与业务验证。

## 2026-10-24：发行资源覆盖与 Launcher 完整性判定核查

- 已从公开 `5.2.1.0Pre` 应用包选择性提取 profiles 并校验文件 CRC；其中 v4 qz 与本地 Release 逐字节相同，SHA256 为 `2783851fd312f73c318e83840a0ffd372e51cd3567d57ee5a8688055997be56b`。Launcher 清单也逐字节相同，SHA256 为 `a8c3d0dca382d7240b7a09a983431211fb2ba43484becf5fa11360793531cb1f`。此结论只覆盖所提取文件，不是整个发行包完整性测试。
- 源 JSON 与 Release qz 解压后的 JSON 对象相同：2856 profiles、580004 items，分为 NTOS 1906、NTKRLA57 814、FLTMGR 136。含 70 个 NTOS 28000 系列身份（28000.1–28000.3181）；版本标签存在不代表每个更新的精确 PE/PDB 身份都已覆盖。
- 当前生产 `loadV4ItemsFromJson` 的独立 Qt 回归再次通过全部 2856 profiles 与 4 个非法输入用例。不能据“缺偏移”提示直接断言当前包无法解析；此测试不替代驱动实际下发。
- `Launcher/tools/generate_support_manifest.py::profile_is_complete` 根据旧 `missingFields`/`missingGlobals` 判完整。全部 2720 个 NTOS/NTKRLA57 档案都缺 `_PIDDB_CACHE_ENTRY` 的 DriverName/TimeDateStamp/LoadStatus 三字段，公共 PDB 不提供该私有类型；这三项不在可选字段列表，导致清单所有 2856 项均为 `complete=false`。`Launcher/Compatibility.cpp::ScanCompatibility` 将命中但不完整与无身份一起计入 missing，故已有身份也可弹缺偏移提示。仅在内存中排除这三项后，1490 个 NTOS、813 个 NTKRLA57 满足其余旧完整性条件；其余缺项不能一并宣称误报。
- 包只有上述三个模块类别；CI、WIN32K 等其它类别和独立 deep offset 库须分别核查，不能以 NTOS 覆盖推导全模块覆盖。核查机器当时磁盘 NTOS RSDS `91A0697C-BDF1-F6C1-B8D2-82E109E671E6/1` 在包中无身份，仍有真实覆盖缺口。
- 自动发行脚本从最新手工应用包继承 profiles，CI 不发布占位偏移包；普通拉取/构建不会补齐精确身份。此次仅核查与独立读取测试，未修改生产逻辑、生成偏移、下载 PDB 或加载驱动。
- 用户随后提供主程序实际 DynData 报告，明确绕过 Launcher。该现场 `profiles=2856 / 扫描=2856 / 无效命中=0`，NTOS 的 `0x8664 / 0xAEEE138F / 0x01045000` 与本机磁盘内核完全相同，包按这四个 PE 身份字段命中为 0，RSDS GUID/Age 命中也为 0。磁盘内核版本为 `10.0.19041.7802`，包内 175 个 NTOS 19041 系列档案最高为 `19041.7725`。报告 231 个字段中 51 个来自 Runtime pattern、180 个 Unavailable，EX/V4 均未下发。此现场根因是精确身份未覆盖，不能套用前述 Launcher 完整性问题；未命中时 `valid` 保持默认 false，报告“格式有效=否”不代表匹配档案被格式校验拒绝。
