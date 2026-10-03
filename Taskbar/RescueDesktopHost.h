#pragma once

// 在 QApplication 创建前调用。普通启动返回 -1；SOS 监护模式返回其最终退出码。
// 监护进程使用独立 Win32 消息循环，负责输入拦截、应急返回和崩溃后的桌面恢复。
int RunRescueDesktopHostIfRequested();
