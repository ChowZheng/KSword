# 蓝屏布局离线回放

在仓库根目录执行 `tools/bugcheck_layout_replay/run.ps1`。工具复用生产解码器、
两种页面布局、二维码编码器和实际 8×12 字模，在既有 `output/` 目录编译；
不加载驱动、不注册回调，也不触发真实蓝屏。

Linux 模式覆盖 640×480、1024×768、1280×720、空诊断、无 BGP/扩展快照、最长数值/文本及
固定数组没有 NUL 且包含任意原始字节的边界。断言检查左上角企鹅列对齐、居中二维码、
四模块静区、三行正文、画布边界和两区不重叠。原四区诊断页继续运行原有回归。

生成的 `output/bugcheck_linux_*.ppm` 是实际生产布局回放；配对 `.ppm.txt` 保存
真正进入二维码编码器的 `KSQ2:` ASCII 文本，`.ppm.bin` 保存完整原始包。
在已有 Python/OpenCV 环境执行（禁止产生额外缓存目录）：

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
py -3.13 tools/bugcheck_layout_replay/validate_qr.py
```

该工具用独立 OpenCV 扫码，逐字节比对 Base45 文本和原始包，并独立校验版本、长度、
CRC32、全部诊断字段、8 个签名族、16个 BGP 时间线槽位以及整个扩展证据。
覆盖三个 PE/RSDS 映像身份、环境与独立 UBR 状态、18个寄存器、16个栈项、六个事件、
采样来源/状态/时间和覆盖/丢弃计数；非法长度、CRC、版本、标志和 Base45 必须整体拒绝。
低分辨率矩阵使用近邻整数放大辅助离线解码，成功后生成
对应 PNG 预览。扫码测试验证生成矩阵和报告完整性，不证明实机显示或手机摄像头
在小分辨率蓝屏上的识别率；实际 BGP/SVGA 显示、转储进度覆盖和驱动加载另行验收。

扫码内容使用 `KSQ2:` 加 RFC9285 Base45，可由普通二维码扫描器复制。Base45 不是压缩；
使用 QR 字母数字模式可在一个 LOW/version40 码中容纳最多4296字符，本实现完整包上限
2860字节（封装4295字符）。企鹅及四模块静区仍保留，640模式可自动降到1像素模块。
手机扫描后把完整文本保存为已有目录里的 ASCII 文件，保留所有空格，不附加换行：

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
py -3.13 tools/bugcheck_layout_replay/decode_ksq2.py --input output/scan.txt
py -3.13 tools/bugcheck_layout_replay/decode_ksq2.py --input output/scan.txt --json --output output/scan.json
```

解码器只依赖 Python 标准库，不访问网络、驱动或崩溃指针。所有原始数值以无符号位模式
保留，文本同时提供原始长度、`RawHex` 和可读 UTF-8 表示；没有 NUL 的数组保留全部容量。
`RegisterMask` 决定有效寄存器，`ContextSource` 区分系统转储头与回调上下文，系统头本身
不保证就是故障现场；`StackSource` 为最近操作或同线程崩溃前操作时，必须结合
`StackThreadId`、`StackSampleTime` 和状态解释，不能称为已经展开的故障调用栈。
固定数组总是保留全部槽位，异常计数也独立保存，不能用计数驱动越界读取。
`CiOptions` 保留原始位图；仅当CI字段有效且查询状态为成功时，额外解释启用、测试签名及
HVCI内核/审计/严格/IUM选项，否则解释值为null。位定义来自
[Microsoft NtQuerySystemInformation](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation)。

KSQ2 schema2 的二进制头为16字节：`KSQ2` magic、LE uint16 schema、LE uint16 section flags
（1=BGP，2=Evidence）、LE uint32 总包长、LE uint32 CRC32/ISO-HDLC。
CRC覆盖头部0..11字节及16..末尾字节，排除CRC自身。正文依次是基础诊断/回调位图/
模块计数/Stop Code名称/四个参数角色、可选完整BGP、可选完整Evidence。字段按
`bugcheck_qr_codec.c` 和 `decode_ksq2.py` 中共同声明的成员顺序逐一编码，所有数值按
固定4/8字节小端，文本按一个长度字节加真实原始字节，GUID保留原始16字节。
这个格式不复制C结构或padding。未来改变字段必须更新schema及独立解码器；
超出2860字节或内置语义名称容量时返回明确失败，不输出被截断的二维码。
