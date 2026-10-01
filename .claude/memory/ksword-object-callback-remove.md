---
name: ksword-object-callback-remove
description: Object Callback 枚举与安全注销链路的句柄语义、重验证和 fail-closed 边界
metadata:
  type: project
---

# Object Callback 安全注销

- `_OBJECT_TYPE.CallbackList` 中的 `_CALLBACK_ENTRY_ITEM.EntryItemList` 节点不是
  `ObRegisterCallbacks` 返回的 `RegistrationHandle`，回调函数地址也不是句柄。枚举协议中，
  仅由匹配内核 PDB profile 的 `_CALLBACK_ENTRY_ITEM.CallbackEntry` 字段解析出的值可放入
  `registrationAddress`；链节点必须单独放在 `rawStorageValue`。
- 启发式 Object Callback 扫描按产品要求开放高风险候选移除：不得设置 `HANDLE` 或
  `VERIFIED_REMOVE`，但找到非零 registration block 时可设置 `REMOVABLE_CANDIDATE`，通过
  EX 协议携带该候选值；UI 必须继续显示 candidate 而不是 verified。
- Object Callback 注销只走 `REMOVE_EXTERNAL_CALLBACK_EX`。请求必须来自完整 V3 枚举行，
  携带 callback、真实 RegistrationHandle、raw node、object/operation mask、trust flags、
  identity hash 和完整快照 generation，并要求 revalidation。
- R0 在调用 `ObUnRegisterCallbacks` 前重新构建与默认 R3 相同来源的内核注册行；排除无关
  WFP/Minifilter 查询。类别、source、callback、registration、raw node、operation/object mask
  唯一匹配时使用重新枚举得到的句柄或启发式候选；请求仍需携带非零 identityHash，但不要求
  含易变名称/模块归属的全行 hash 相等。移除不再要求全局快照代次一致，避免
  无关行增删阻止注销；模块归属只作诊断，不作移除门槛。调用后再次枚举，按类别、回调和注册
  地址检查仍注册的目标，不依赖名称/模块/可信位等显示元数据，仍存在或复核失败不能报告成功。
  若当前句柄等于 runtime->ObRegistrationHandle，走已有 ObjectCallbackUnregister 生命周期
  入口清空句柄和保护状态，并清除自身注册位，防止卸载时重复使用已失效句柄。
- 旧版纯 callback-address 移除入口必须拒绝 Object Callback 和 Registry Cookie。Registry 已有
  EX 候选注销：同一条链必须用活跃自身注册的 Cookie+Context+Function 校准 +0x18/+0x20/+0x28
  布局；真实 Cookie 放 registrationAddress，原始节点放 rawStorageValue。Cookie 是值，不能
  施加内核地址/对齐校验。调用 `CmUnRegisterCallback` 后保留 NTSTATUS，并按原始节点确认缺失；
  注销自身时同步清除 runtime Cookie 和注册位。ETW 注册句柄仍未可靠恢复。
- 回调遍历默认隐藏基本“类别”列。注册子类型为 UNKNOWN/未识别且基本类别已知时，注册类型列显示“基本类型（未分类）”（例如 Minifilter），不能只显示“未分类”；该展示不提升可信等级或开放移除能力。底部手动地址面板仍保留原有布局。
- 强制移除（experimental unlink）只有共享协议与 R0 拒绝分支，没有摘链/改数组实现；GUI 不再提供该菜单、确认弹窗或操作模式说明。仅预留 unlink 的行显示不可移除 `×`，普通公开 API 候选（含启发式 Object 候选）仍保留 `!`，不得把这两类混同或把候选标成 verified。

- 扩展公开注销后端位于 `callback_remove_extended.c`，使用现有 EX IOCTL；新增类别编号在
  `shared/driver/KswordArkCallbackIoctl.h`，不新增 IOCTL 或改 ABI 布局。BugCheck/Reason 使用公共
  Record；FS change 使用当前行的 DriverObject + routine；Logon legacy 使用 routine，Ex 使用
  routine+Context（Context 可为零）；Shutdown 使用 DeviceObject；NMI 使用自句柄；命名
  CallbackObject 使用 ExUnregisterCallback。Image Verification 必须调用
  SeUnregisterImageVerificationCallback 包装器维护计数，不能直接调用 ExUnregisterCallback。
  所有新路径发布 candidate + revalidation，保留 `!`，不等于实机已验证。
- 电源设置/PnP 后端分别有 `PoUnregisterPowerSettingCallback` / `IoUnregisterPlugPlayNotificationEx`，
  但必须由枚举证明真实 HANDLE 后才能发布候选。special 捕获用记录前缀验证电源 PSet tag、
  PnP 公共通知前缀/DriverObject/Unregistered 状态，才发布 HANDLE；不满足仍只读。
  Coalescing Link+0x30 回溯真实记录，用 SelfPtr 和 ExCallback 验证；Priority 数组先清除
  EX_FAST_REF 低四位，读取真实 routine+DriverObject（不是 slot）；Debug Print 读取 Link 前
  一个指针槽函数并用 DbgSetDebugPrintCallback(FALSE) 注销。前两者注销导出按名称动态解析。
  EMP 正确导出名是 EmProviderRegister/EmProviderDeregister，但当前 callback record 不是
  provider handle；ETW provider 也不是 ETW_REG_ENTRY，仍不可移除。传统 FsRtl filter 没有
  独立注销 API。研究证据见 [扩展注销研究](ksword-callback-remove-research.md)。
- NMI/CallbackObject 不再持私有 KSPIN_LOCK 后调用 MmCopyMemory：统一读取器会拒绝
  DISPATCH_LEVEL。改为 PASSIVE_LEVEL 两次安全读取比对链路/函数/上下文/自句柄；变化返回
  RETRY，不发布不一致快照。依然存在读取后到 API 调用前的并发变化窗口，不能声称原子注销。
- 后置复核记录目标类别的定位/节点/模块查询失败，未匹配且读取失败返回具体状态，不能把
  “没读到”报成“已消失”。其它类别错误不应阻断目标注销。
  Registry 链和 PDB Object 目标链若已验证为空，记录正常空容器证据，避免旧版
  empty/unsupported 展示行把注销最后一项的成功复核误报为不支持。
- BugCheck 自身行和锚点以公共 Record.State == BufferInserted 为准，避免外部注销后自身
  Registered 标记仍为真而展示已失效的注册。VOID 注销 API 必须后置复核；API 失败保留原始
  NTSTATUS 与 API 名称。驱动失败弹窗与详情直接保留驱动具体原因。
- 离线验证：`python tools/callback_remove_tests/run.py --cc cl`（已初始化 MSVC 环境），对生产
  注销函数和快照 matcher 使用模拟 API，覆盖参数传递、失败状态、VOID 假成功、后置枚举失败、
  未知句柄拒绝、前置身份核对和后置显示信息变化；这不替代真实系统的驱动加载/注销验收。
- 本次主程序/驱动 Release 构建及语言/主题审计通过；驱动编译零警告、x64 ApiValidator
  和 Inf2Cat 通过。最终驱动为未签名产物；主程序测试签名信任验证返回 0x80096019。
  没有进行实机加载或外部回调注销验收，不能把构建/模拟 API 回归等同于实机支持矩阵。
