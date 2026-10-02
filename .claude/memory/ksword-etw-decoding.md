# ETW 解析与进程归属

实现集中在 `Ksword5.1/Ksword5.1/MonitorDock/MonitorDock.cpp`。

## 身份来源

- `EVENT_HEADER.ProcessId/ThreadId` 是事件产生/记录上下文，不能一律当成被操作对象。原始值保留在详情 JSON 的 `meta.header_pid/header_tid`，详细筛选器的 HeaderPID/HeaderTID 仍明确筛选这些原始值。
- 列表、简易 PID 筛选、摘要和进程跳转使用关联身份。载荷 PID 按 `TargetProcessId/NewProcessId/ProcessId/PID` 的优先级读取；线程别名还包含 `TThreadId` 和文件事件的 `TTID`。
- 经典内核 Image/Process/Thread/FileIO/TCPIP/UDPIP 事件若缺少可信载荷 PID，显示“未知”，内部值为 `UINT32_MAX`；不可拿当前线程所属进程反推历史 PID，线程可能已退出或复用。
- 缺少关联线程时，枚举事件、关联 PID 与事件头 PID 不同、内核映像 PID=0 或关联 PID 未知，均不能借用事件头 TID。经典线程/文件事件也不借用事件头线程。
- 安全相关 PID/TID 仅从载荷读取，不从事件头补造。显式 SubjectProcessId 可与目标 ProcessId 不同，两者要保留各自语义。
- 不将超过 32 位范围或负数的值截断成 PID/TID。
- 裸 `PID/TID` 只在已知进程、映像、文件、网络或安全事件类别中用作身份别名；`ParentId` 仅在进程线程类别中当作父 PID。私有设备/业务 schema 可能使用相同缩写表示产品、事务或层级 ID，必须保留原字段而不能补造进程含义。显式 ProcessId/ThreadId 仍可读取，磁盘的 IssuingThreadId 单独识别。

## 映像样例与语义

- Provider `{2CB15D1D-5FC1-11D2-ABE1-00A0C911F518}` 是经典 Image provider，显示为 `Microsoft-Windows-Kernel-Image`。
- Image v3、opcode=3 (`DCStart`) 表示枚举已存在映像，不是刚创建/加载；opcode=4 (`DCEnd`) 表示枚举结束，Load/Unload 单独表示加载/卸载。
- 2026-10-02 用户样例 `ndiscap.sys` 长度 138 bytes：事件头 PID=3212 是 KSword，而 offset 0x10 的载荷 ProcessId=0；UTF-16 文件名在 offset 56。应显示关联 PID=0、TID=未知，不合成 PID=4，也不宣称驱动加载进 KSword。
- 动作匹配避免 `Thread` 内的 `read`、`Disconnect` 内的 `connect`、`Unload` 内的 `load` 造成误判。

## 字段布局与缓存

- 缓存完整 TDH `PropertyCount` 及原始 `TRACE_EVENT_INFO`，按 `TopLevelPropertyCount` 遍历顶层，递归展开 StructStartIndex/NumOfStructMembers。
- 数组逐元素消耗字节；动态计数 0 必须消耗 0 字节。数组值不冒充标量身份，嵌套字段保留结构路径，不当成顶层 ProcessId。
- 使用 `TdhFormatProperty` 返回的 `UserDataConsumed` 移动偏移。显式长度原样交给 TDH；Unicode 长度单位不可凭 API 文档的笼统字节说明自行换算，回归包含显式长度 UTF-16 字段。
- PORT 输出类型使用网络字节序：01 BB 应为 443，不能按本机 UINT16 得到 47873；IPv6 二进制未显式指定长度时按 TDH 规则补 16。
- 结构体布局缺失、未知类型、截断字段或长度/数量引用无效时停止解析，保留原始尾部，详情 `meta.decode_complete=false`；不得猜 32 字节后继续读取后续 PID。
- TraceLogging 的 EventId 通常为 0，缓存键必须包含 `EVENT_HEADER_EXT_TYPE_EVENT_SCHEMA_TL` 内容的 SHA256，同时区分 Channel 和头部位宽。缓存上限 4096，避免自描述事件无限增长。

## 语义翻译边界

- Provider 分类优先保持预设描述符的一致性；资源类型按已知 Provider 匹配，不能用事件名中的 `reg/file/set/read` 等子串归类或翻译。实际清单中的 DNS Registration、TCPIP Ndkpi_Register、Profile、Reset 都曾触发误判。
- 标准操作码按数值翻译，不依赖本机 TDH 返回的是英文 Start/Stop 还是中文“开始/停止”。只翻译明确操作名；TcpConnectTcbTimeout、TcpCreateEndpointAfFailure、TcpFastopenStateChange 等复杂诊断事件保留原文。
- 经典事件具体名称经常在 OpcodeName，而 EventName/TaskName 只有 Image/Thread/TcpIp 等父类名。未知子事件保留具体操作码名；SendIPV4、RecvIPV6、SetInfo、QueryInfo 等明确别名单独识别。
- Thread、DiskIo、PageFault、PerfInfo 等事件类会在同一 GUID 下承载多个子类，结合 opcode 区分 CSwitch/Dispatcher、磁盘初始化/驱动、HardFault/VirtualAlloc、Profile/DPC/Interrupt/SystemCall；手动 GUID 订阅可用 TDH ProviderNameOffset 补全名称。
- `Microsoft-Windows-Kernel-Process` 的清单还包含线程和映像事件。时间轴优先使用解码资源类别和明确事件名，不让 Provider 名覆盖它们；内部映像泳道标识仍为“镜像”。Windows Kernel Trace 的未分类记录不能默认归成进程。
- Result/Status 可能是 BOOL、枚举、计数或错误码，不能一律把数值 0 翻译成“成功”。摘要保留 TDH 按实际输出类型格式化的值，不把所有整数强制变成错误码十六进制。
- FileIo Create 包含 NtCreateFile 的创建与打开请求，动作显示“创建/打开”；NameCreate/NameDelete 是名称记录，不宣称磁盘文件创建/删除成功。OpenPath 是路径字段；FileObject 是对象标识，不能填入文件路径。
- Microsoft-Windows-TCPIP 也记录 UDP/IP 事件，不能只凭 Provider 名推断 TCP。断开连接、QueryInformation 等不能推断出站/入站；只接受明确方向、已知发送/接收事件及经典网络操作码。内存地址不能填入 IP 字段。
- 字段说明使用中性含义：FileName、ImageName 可以是名称或路径；Path 不一定是对象路径；Disposition 无明确上下文时保留“处置值”；Hive 是注册表配置单元。未知字段保留原名和值，不能保证所有私有 Provider 都已有中文语义映射。

## 回归与构建

- `python tools/etw/test_event_decode.py` 提取并编译生产解析器，使用 x64 MSVC/Qt/TDH。17 组回归涵盖原始解析问题及 Provider 分类、动作/操作码、状态、文件/网络/字段语义、时间轴一致性。
- 可加 `--catalog-output .codex-build-logs/etw-semantics.json` 保存本机实际声明与当前翻译。2026-10-02 本机覆盖 11 个预设 manifest Provider 的 1,808 个定义，以及 12 个经典事件类 GUID 在版本 0–5、操作码 0–255 中可由 TDH 查询到的 395 个布局。此清单核对不等于所有 Windows 版本/私有 Provider 的人工翻译或完整实机载荷验收。
- 主程序构建继续使用 `tools/Invoke-KSwordBuildCheck.ps1`，遵守 HostX64 与仓库构建恢复规则。
- 新增用户可见文本时同步双语包并运行 i18n audit；并行修改语言包时仅暂存自己新增的词条，不能连带提交其他人的修改。

参考：Microsoft Learn [Image_Load](https://learn.microsoft.com/en-us/windows/win32/etw/image-load)、[EVENT_HEADER](https://learn.microsoft.com/en-us/windows/win32/api/evntcons/ns-evntcons-event_header)、[TDH 官方解析示例](https://learn.microsoft.com/en-us/windows/win32/etw/using-tdhformatproperty-to-consume-event-data)。
