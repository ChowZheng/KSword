#pragma once

// Require 汇总独立离屏主题回归的断言；失败写入诊断并影响最终退出码。
// condition 为待核验条件，message 为失败说明，无返回值。
void Require(bool condition, const char* message);

// TestDockThemeIcons 用真实 ADS 管理器检查主题图标和后创建的控件。
// 不创建生产主窗口、不访问驱动、不执行终止或 ETW 控制，无返回值。
void TestDockThemeIcons();

// TestPerformanceChartTheme 验证图表角色重算、数据保留及浮窗缓存颜色迁移。
void TestPerformanceChartTheme();
