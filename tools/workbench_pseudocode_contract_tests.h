#pragma once
#include <functional>

// RunWorkbenchPseudocodeContractTests：使用合成字节检查共享 C 页的安全与导航契约。
// require 接收断言和说明；不连接真实进程，不下载或启动真实 Ghidra。
void RunWorkbenchPseudocodeContractTests(const std::function<void(bool, const char*)>& require);
