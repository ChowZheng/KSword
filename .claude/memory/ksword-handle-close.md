# 句柄页 R3/R0 关闭（2026-10-05）

- `句柄/HandleDock.Filter.cpp` 在 Handle Table 明细行右键提供“R3关闭句柄”和“R0关闭句柄”。摘要、占位及继续加载节点不提供关闭。菜单打开前复制完整行，确认框前再次复制；工作线程只持有值，不能捕获 `m_allRows` 指针或 Dock。成功/失败均刷新，过期身份失败不能盲目重试。
- R3 复用 `ksword/file/file_handle_tools.cpp::CloseRemoteHandleByObjectIdentity`：持有验证过创建时间的进程句柄，临时挂起目标，重新取系统句柄快照比较对象，执行 `DuplicateHandle(DUPLICATE_CLOSE_SOURCE)`，作用域退出恢复进程。R0 只走 `ArkDriverClient::closeHandle`，旧驱动不支持时明确失败，不能偷偷回落 R3。
- 唯一共享协议是 `shared/driver/KswordArkHandleIoctl.h`：`CLOSE_HANDLE` function `0x8D4`，METHOD_BUFFERED，读写访问，独立 v1 关闭包。请求必须具备完整 size/version、精确 UI_CONFIRMED flags、PID、非零真实创建时间与对象地址；只接受小于 `0x80000000` 的非零用户句柄。地址只比较，不解引用。
- `handle_ioctl.c` 快照 buffered 输入，再检查设备写权限、PID、自身请求者、执行进程及既有中央 suspend 策略。`handle_close.c` 先解析成对的 PsSuspendProcess/PsResumeProcess，引用并核验进程，获取退出 rundown，临时挂起，在目标上下文用 UserMode 引用/关闭句柄，最后 detach、释放对象、恢复、释放 rundown 和进程。保留 protect-from-close，不修改私有 HandleTable 位。
- 响应分别报告 closeStatus/resumeStatus，不能把“已关闭但恢复失败”说成完整成功。暂停目标线程缩小句柄复用窗口，不能承诺阻止其他进程或内核同时改动该表；公共关闭 API 不提供原子 compare-and-close。对象引用只保证对象存活，不锁住句柄槽。
- 枚举 entry 尾部追加 `processCreationTime100ns`，直接路径与 system-snapshot 回退都从同一被引用 owner 读取，R0-only 行不依赖 R3 OpenProcess。R3 客户端通过 entrySize 判断是否能读尾字段；旧 entry 仍可解析，缺少 R0 时间时保留原 R3 identity。CLI 也接受 LEGACY_SIZE 前缀，命令/参数未变。
- `tools/Invoke-HandleCloseTests.cmd` 用 x64 MSVC `/W4 /WX` 跑生产 DriverClient 的模拟 IOCTL 回执、生产 `handle_close.c` 业务的 Windows API fixture（仅替换两条 include），以及专属子进程持文件句柄的真实 R3 关闭。覆盖 PID/对象失配、挂起/引用/关闭/恢复失败、SEH、IRQL、退出锁/引用/attach 清理、旧协议前缀、关闭后句柄消失与子进程恢复。
- 主程序 Release 构建使用 `Invoke-KSwordBuildCheck.ps1`，驱动完整 Release/x64 Build 已通过（x64 ApiValidator Universal、CAT 生成，无告警）。未加载新驱动或做真实 R0/UI 点击验收；API fixture 不能代替内核实测，产物跳过自动测试签名。
- 功能 CI 对 CLOSE_HANDLE 明确记为 no-cli-path；预算 20→21 的原因保存在计划 policy，不能将任意机器现有句柄作为 destructive fixture。
