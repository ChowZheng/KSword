---
name: ksword-file-metadata-editor
description: FileDock 文件元数据编辑的 Win32 写入边界、身份复核、异步回读与 i18n 约束
metadata:
  type: project
---

# FileDock 文件元数据编辑

主程序文件属性窗口位于 `Ksword5.1/Ksword5.1/FileDock/FileDock.cpp` 的
`FileDetailDialog`。元数据编辑作为左侧导航中的懒加载页接入，不应在文件属性窗口首屏
同步打开句柄或访问可能阻塞的网络路径。

## Win32 基本信息写入

- 使用 `CreateFileW` 打开 `FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES` 句柄，保留
  `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`，目录需要
  `FILE_FLAG_BACKUP_SEMANTICS`。
- 始终带 `FILE_FLAG_OPEN_REPARSE_POINT`，叶节点为符号链接/Junction 时修改链接自身，
  不静默跟随到目标。
- 通过 `GetFileInformationByHandleEx(FileBasicInfo)` 读取、
  `SetFileInformationByHandle(FileBasicInfo)` 写入，再用同一句柄回读实际结果。
- `FILE_BASIC_INFO` 中未选择的四个时间字段保持 `0`；`FileAttributes=0` 表示不修改属性。
  不要把完整旧结构原样回写，否则会无意重写未选择字段或结构性属性。
- UI 使用本地时区和毫秒精度；后台请求转换成自 1601-01-01 UTC 起的 100ns 计数。
  FAT/exFAT 等文件系统可能按自身时间粒度舍入，写入成功但回读不一致时展示实际值，
  不伪报为精确匹配。

## 属性与身份安全边界

- 只开放 `READONLY`、`HIDDEN`、`SYSTEM`、`ARCHIVE`、`TEMPORARY`、
  `NOT_CONTENT_INDEXED` 六个位。
- `DIRECTORY`、`REPARSE_POINT`、`COMPRESSED`、`ENCRYPTED`、`SPARSE_FILE`、
  `INTEGRITY_STREAM` 等必须保留；需要改变时走各自专用 API/FSCTL，不能当普通布尔位编辑。
- 初次读取时记录卷序列号和 64 位文件索引。用户确认后重新打开目标，在写入前用同一句柄
  复核身份；路径已替换时以 `ERROR_FILE_INVALID` 失败，不能把旧页面里的值写到新对象。
- 写入前在同一句柄重新读取最新 `FILE_BASIC_INFO`，只把六个开放位合并到最新属性值，
  避免属性窗停留期间的外部变化被覆盖。

## UI、异步与国际化

- 读取和写入都放入 `QThreadPool`，回填使用 `QPointer<FileDetailDialog>` 和操作代数；
  对话框关闭或新操作取代旧操作后丢弃迟到结果。
- 写入成功后刷新常规属性树并重新发起 R0 文件信息查询；R0 查询也要带代数，防止写入前
  的旧结果覆盖新状态。
- 组合文本必须先对模板调用 `ks::i18n::sourceText` 再 `.arg(...)`。新增可见文本定点同步
  `languages/zh-CN.json` 与 `languages/en-US.json`，并运行 `tools/i18n_language_pack.py audit`。

## 统一暂存与 R3 编辑范围

- 文件属性窗口支持 `QStringList` 多目标。多选只打开一个批量窗口，常规页显示汇总，哈希页和
  元数据页支持批量处理；PE、签名、重解析点、占用、FileObject、Storage、Minifilter、依赖 DLL、
  字符串和十六进制等单文件分析导航在批量模式禁用并给出原因。
- 所有编辑页只生成 `ks::file::metadata::TargetPatch`，页面内的“暂存”按钮不触碰文件。窗口底部
  “保存全部修改”是唯一写入入口，带“创建备份再修改”选项，默认勾选。
- `ksword/file/file_metadata_transaction.*` 只使用 R3 Win32/NT API，统一执行卷序列号 + 文件索引
  身份复核、备份、后台写入、写后回读、逐操作结果和失败回滚。高风险操作（原始重解析点、EA、
  Object ID、PE 资源、嵌入式签名清除）未勾选备份时拒绝执行。
- 已覆盖基础属性/四个时间戳、重命名、8.3 短名、目录大小写敏感、Shell PropertyStore、ADS 与
  Zone.Identifier、EA 原始字节、安全 SDDL/Owner/Group/DACL/SACL/继承保护/有效权限、压缩/稀疏/
  EFS/Integrity Stream/Object ID/硬链接、原始重解析缓冲、PE VERSIONINFO/Manifest/其它资源原始
  更新，以及 R3 `WinVerifyTrust` 证书链、签名者、颁发者、SHA-256 指纹、有效期、时间戳和 Catalog
  状态展示。
- 默认数据流、文件长度、有效数据长度、分配大小、USN/MFT 日志和自动重新签名仍保持只读。已签名
  文件保存前提供“清除嵌入式签名并继续 / 保留签名数据并继续 / 取消”三选一，Catalog 签名只显示
  失效，不能从目标文件本身删除。

## 文件详情签名摘要

- 签名页使用滚动表单，按信任状态、颁发者、签名者、签名时间排列，证书详情和 R0 原始证据默认折叠。
- Windows 信任判断仅以 `WinVerifyTrust == 0` 为成功，区分未签名、不受信任、摘要不匹配、过期、吊销与吊销状态未知；Catalog 文件存在不等于其签名已经通过验证。
- `SignatureInspection::signingTime` 仅在有效时间戳 countersigner 存在时读取主签名者的 `sftVerifyAsOf`；无时间戳时显示缺失，不以当前验证时间或证书有效期替代。
- R3 摘要先异步回填，R0 证据单独随后回填。证书名称与路径使用纯文本 QLabel，防止证书字段被当作富文本解释。

## 文件字符串全文件搜索

- 字符串页在后台按当前查询条件扫描整个文件，支持普通文本、正则、大小写、最小长度和 ASCII/UTF-16LE 编码；不再截断于前 128 MiB 或前 2000 条候选。
- 单个连续字符串按 65536 字符分段，查询时保留重叠，预览截取命中附近 1024 字符；内存最多保存 20000 行，但仍扫描并统计后续命中。正则在字符串片段内匹配，不跨独立字符串或任意长片段。
- 每次查找都有独立取消标记和 UI 代次；非法正则也取消旧扫描并提升代次，禁止旧结果覆盖新错误状态。关闭页面会取消扫描。
- 回归已覆盖 2 MiB/128 MiB 后的命中、超过 2000 候选、跨读取块及长字符串分段、UTF-16LE 奇数偏移、大小写/正则、取消和显示行数上限。

## 文件详情外部十六进制导航

- 只在 FileDetailDialog 页面增加工具栏，保持共用 `UI/HexEditorWidget.*` 不变。文件偏移支持十进制与 `0x` 十六进制，按需在后台 `QFile::seek/read`，覆盖整个文件而非前 2 MiB。
- 默认加载 64 KiB，可选择 256 KiB/1 MiB/2 MiB；通过 `setByteArray(bytes, fileOffset)` 保留磁盘基址，再使用 `jumpToAbsoluteAddress` 定位。当前范围查找仍使用已有编辑器接口，不能把它描述为全文件字节搜索。
- 字符串双击将偏移放入 TabWidget 的 `ks_file_detail_hex_offset`，懒加载 hex 页消费一次；已创建页面则在再次切入时消费。使用页面代次丢弃过期读取结果，非法输入也提升代次。
- 已验证 2 MiB/4 GiB 之后的读取、文件末尾、越界、十进制/十六进制输入、上下范围、空文件及字符串跨页定位。界面层回归用共用编辑器 API 替身，实际主程序 Release 编译链接通过。
