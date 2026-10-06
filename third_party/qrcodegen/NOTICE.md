# Nayuki QR Code generator

`qrcodegen.c` 与 `qrcodegen.h` 来自 Project Nayuki 的 MIT 许可纯 C 二维码编码器：

- 官方项目：https://www.nayuki.io/page/qr-code-generator-library
- 上游仓库：https://github.com/nayuki/QR-Code-generator
- 固定提交：`3c6d0b3cefb4e049dc337e82237c9644399716a8`
- 原文件：`c/qrcodegen.c`、`c/qrcodegen.h`；本目录保留上游原文和源内许可证。
- 原始 SHA256：C 源码 `6a2b9cc65176f2345dde260c74b6d352627e8a0a6385d086ae0e9c5d0913c70c`，
  头文件 `e82df4bff37d18b5863b9e7486fe6bda1b6cda8c3b9ecebfec473907265cb589`。
- 许可证全文：`LICENSE.txt`。

上游 C 实现不进行堆分配，使用调用方提供的缓冲区和整数运算。最大 QR 矩阵缓冲区为
3918 字节；版本 40、低纠错的字节模式最多容纳 2953 字节。KSword 在非分页静态工作区
保留报告、编码临时缓冲与输出矩阵，崩溃路径不申请内存、不进行文件或网络访问。
编码固定使用低纠错等级、掩码 0，并按完整输入选最小版本，省去八掩码评分；
模块使用 1 至 8 的整数像素倍率，四周完整保留四模块白色静区。

驱动工程仅对 `qrcodegen.c` 定义 `NDEBUG`，避免 Debug 驱动调用用户态 CRT 的断言处理。
`kernel_compat.h` 仅作为该文件的强制包含头，为 `abs` 与 `labs` 提供有界整数内联实现；
本机 WDK 内核导入库包含其余 `memcpy`、`memmove`、`memset`、`strlen`、`strchr` 依赖。
WDK 内核 CRT 未提供 `stdint.h`；直接读 MSVC 用户 CRT 的对应头会与 WDK 的
`crtdefs.h` 发生宏定义冲突。因此工程仅对二维码源及三个蓝屏二维码使用方优先加入
`kernel_include/`，使用本目录独立的整数类型适配头。该头提供标准整数类型及常用边界，
保留 WDK 已定义的 `SIZE_MAX` 等宏，不引入 `vcruntime.h`，不修改上游源码；
用户态离线回放不使用该目录，继续使用其原生标准库头。
`abs` 仅用于 -4 至 4 的图形坐标；`labs` 用于绝对值不超过二维码模块数乘 20 的掩码
评分表达式，因此不会出现最小有符号整数取负。

二维码采用 `KSQ2:`、版本化逐字段二进制格式、CRC32 和 RFC9285 Base45，包含完整诊断、
回调位图、缓存模块数、BGP 的全部标量/8 个签名族/16 个时间线槽位，以及系统环境、
映像 PE/RSDS 身份和明确来源的上下文/近期事件。文本按长度和原始字节保存，固定数组
没有 NUL 时保留全部容量；不复制 C padding、不压缩或静默截断证据。
Base45 使用字母数字模式，版本 40/低纠错最多 4296 字符，完整原始包上限为 2860 字节。
二维码不依赖在线链接；只代表绘制时已采集的快照，其后的转储进度和绘制状态仍会
继续写入 SecondaryDumpData。格式和离线解码器见 `tools/bugcheck_layout_replay/README.md`。
