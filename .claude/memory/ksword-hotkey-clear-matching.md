# 快捷键清除与本机Win32k匹配

- 2026-10-25本机版：窗口→热键/钩子的程序快捷键、注册热键支持多选清除；杂项末尾追加快捷键管理导航代理，实际只保留原列表/扫描实例。进程详情单项删除共用 `ProcessDock/HotkeyClearController`，编辑路径保留。
- 行身份必须保留typed原始记录、PID创建时间、模型token和刷新代次；不得从格式化列或可视行号构造变更。确认框返回后需QPointer及PID/time/ticket/快照/选区复核，避免重入换进程。
- 窗口激活热键、快捷方式文件配置、系统注册热键是不同来源。PE Accelerator和菜单文字只读。`.lnk` Save后通知Shell并重开核对文件ID/目标路径/Hotkey；verified只指文件配置，不保证Explorer内存注册已撤销。未确认回执不可报成功或确定未修改。
- `DetailActionResult::outcomeUncertain`仅用于未确认副作用，跳过共用权限失败提示后由热键回调显示警告；不改变其它动作默认行为，不添加诊断事件日志。
- 本机实际Win32k证据：full SHA A8C75A11C28E54B62BC86E5566872241A94D6C485E0A93DE1B39D89D57D40445，base SHA ADA11DB7B97AC5D3ACA370A988C8655E24BED2B4DAD2D1C38E034AF00B9BF517。Full热键代码以WIN32K!W32GetUserSessionState为基址；Base!UserGetSiloGlobals返回不同间接层，不能混用。
- 本机EnterCrit真实消费ECX/EDX，Full的NtUserUnregisterHotKey先将两参置零；不得声明为无参。实际私有删除helper RVA1C8108、表偏移3290、缓存位36A8；节点modifiers+20(word)、flags+22(word)、VK+24(dword)、ID+28(dword)，旧推测字段曾混淆，必须以本轮实际指令/JSON为准，均限定当前精确配对。
- `tools/pdb_offset_generator/match_win32k_hotkeys.py`用PE导出/pdata和Capstone对注册/插入/删除指令链交叉验证，输出PE/RSDS/SHA及证据，`--pair`可重复；缺失、歧义、不兼容或重复配对不授权。`keyboard_profiles.generated.h`当前仅本机一项；驱动运行时再匹配full/base双身份、布局和函数头，不能把单版本结论推广到22H2以上所有版本。
- 键盘协议v3保留v2结构，但新客户端不会用旧v2回执提交清除，旧v2变更请求也被新驱动拒绝。主程序与驱动必须配套。只读枚举改InCallerContext并准备USER；PID/session/thread筛选、读取失败/环/预算的partial状态不应丢失。
- 验证：匹配9项、生产纯值门禁40例、语言包29598字符串、IOCTL计划、主程序Release和驱动编译/API均通过。驱动后置测试签名的信任与内核签名验证未通过；未加载驱动、运行GUI或执行真实清除。完整证据和边界见 `docs/本机快捷键清除与自动匹配-20261025.md`。
