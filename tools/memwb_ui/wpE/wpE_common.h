#pragma once

// ============================================================
// wpE_common.h
// 作用：WP-E（地址簿）离屏验证夹具的公共设施——断言计数、主题切换、临时文件路径、
// 测试条目构造等辅助函数。仿照 tools/memwb_ui/memwb_ui_common.h 的形状单独写一份，
// 不链接也不修改那份文件：那份文件为了 RecordingProvider/MakeStaticFixture 拖进了
// 整个 HexCanvas 工具链，WP-E 的地址簿面板完全不需要 HexCanvas，重链接只会白白增加
// 构建面与出错概率（任务要求"只链接你需要的源文件"）。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/AddressBookModel.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/AddressBookPanel.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/AddressBookStore.h"

#include <QString>

#include <cstdint>
#include <memory>
#include <string>

namespace wpe_test
{
    // 断言计数：与 memwb_ui_common.h 同样的轻量框架，独立一份计数器，互不干扰。
    extern int g_checks;
    extern int g_failures;

    // Report：记录一条断言结果，失败时向 stderr 打印位置与表达式。
    void Report(bool ok, const char* expression, const char* file, int line, const QString& note);

#define WPE_CHECK(expression) ::wpe_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, QString())
#define WPE_CHECK_NOTE(expression, note) ::wpe_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (note))

    // ApplyTheme：切换深浅主题并同步应用调色板（做法与 memwb_ui_common.cpp 的
    // ApplyTheme 完全一致，为避免链接整份文件而单独复制一遍，不是两份代码会漂移的那种
    // 共用逻辑——它只是设置 QPalette 的八九行搬运）。
    void ApplyTheme(bool dark);

    // ScratchFilePath：返回本次进程独占的一个临时地址簿文件路径（位于
    // .codex-tmp/memwb-wpE/scratch/ 下，文件名按 tag 区分，不同测试互不冲突）。
    // 调用方自行负责在用完后删除（大多数测试用 QTemporaryDir 更彻底，这个函数只用于
    // 需要"确定路径、测试间可复用"的少数场景，例如校验原子写不留 .tmp 残留）。
    QString ScratchFilePath(const QString& tag);

    // MakeModuleEntry / MakeAbsoluteEntry：构造一个条目草稿（id 由 Store::add 分配）。
    ksword::memwb::AddressEntry MakeModuleEntry(
        ksword::memwb::EntryKind kind,
        const std::string& targetKey,
        const std::string& moduleName,
        std::uint64_t rva);
    ksword::memwb::AddressEntry MakeAbsoluteEntry(
        ksword::memwb::EntryKind kind,
        const std::string& targetKey,
        std::uint64_t absoluteAddress);

    // WaitMs：运行一小段事件循环（Qt 定时器依赖事件循环才会触发），用于等待
    // AddressBookStore 的防抖定时器到期。
    void WaitMs(int milliseconds);

    // 各组测试入口（定义在各自的 .cpp）。
    void RunStoreTests();
    void RunModelTests();
    void RunPanelTests();
    // RunPanelSurvivorTests / RunPanelDefectTests：修复波新增，分别定义在
    // wpE_tests.Panel.Survivors.cpp（审核报告 §5.2 的幸存变异补测：排序后键盘/菜单落点、
    // A/B 选中态、kind 索引往返、右键菜单剩余分支）与 wpE_tests.Panel.Defects.cpp（§5.3
    // 的缺陷回归测试 + 可疑点 #4 的回归）——单独开两个文件，避免把 wpE_tests.Panel.cpp
    // 撑过单文件行数上限。
    void RunPanelSurvivorTests();
    void RunPanelDefectTests();
    void RunShotTests(const QString& shotsDir);

    // ---- 第二轮修复（wave2）新增：并入审核者 §5 的补测 + 主会话自己补的新变异 ----
    // RunStoreWave2Tests / RunModelWave2Tests / RunPanelWave2Tests：第二轮审核报告
    // review2-wpE.md 的 scratch/suggested_tests2.cpp（31 个，已实测对未变异代码 26 PASS、
    // 5 FAIL=D1/D2/D3/D5/D11 的缺陷回归）按 Store/Model/Panel 拆分到三个新文件，直接并入
    // 默认运行，不再靠环境变量开关。
    void RunStoreWave2Tests();
    void RunModelWave2Tests();
    // RunPanelWave2Tests / RunPanelWave2MoreTests：Panel 相关补测数量较多，按既有"一个
    // .cpp 管一类话题"的拆分习惯分成两份：前者是主题跟随（D11）/右键菜单使能（C7）/
    // en-US 下无汉字的扫描扩面（任务书点名把 C12 的断言扩大到分段文字/类型图标提示/值列
    // 占位/横幅）/排序键；后者是载入失败横幅（D2）/键盘与分段/值编辑/复制值/增量信号。
    void RunPanelWave2Tests();
    void RunPanelWave2MoreTests();
    // RunWave2NewMutationTests：本轮主会话自己补的新变异回归（D1 不靠析构的重试、D4 语言
    // 切换刷新分段、D6 两部分、D7 两部分、D8、D9、D10、D12），覆盖审核报告列出的变异之外、
    // 本轮修复新增代码里仍然缺牙齿的地方。
    void RunWave2NewMutationTests();
    // RunR3Tests：第三轮独立验证者（review3-wpE）重放第二轮 35 个幸存变异时发现的缺口补测
    // （横幅/载入失败文案的 i18n、D1 写失败后的退避重试、复制值菜单使能、reset 后当前格恢复等），
    // 每个测试在文件里注明杀死哪个变异。
    void RunR3Tests();
}
