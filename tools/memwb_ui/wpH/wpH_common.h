#pragma once

// ============================================================
// wpH_common.h
// 作用：WP-H（反汇编/文本/对比）离屏验证夹具的公共设施。
// - 本目录（tools/memwb_ui/wpH/）与其它工作包的夹具完全隔离，只链接 WP-H 自己需要的源文件，
//   不复用 tools/memwb_ui/memwb_ui_common.h（它的 Fixture 结构体绑死了 HexCanvas，会把一整套
//   HexCanvas.* 拖进链接，与本包无关），因此这里自备一份同等精简的断言框架与主题切换。
// - FakeBytesProvider 包一个真实的 ksword::memwb::MemoryDiffOverlay 实例，让三个子页的测试都
//   经过真实的 Materialize/BaselineByte/PreviousByte/ChangeKind 逻辑，而不是手造一份"看起来像"
//   的窗口数据。
// - MakeRealZydisDecodeBackend / MakeRealAssembleBackend 分别用真实 Zydis 与真实
//   ks::ui::InstructionAssembler::assemble 构造生产可用的后端回调，供夹具验证"注入的后端" 这条
//   接缝；生产环境应改注入包装 ks::ui::InstructionDecoder::decode 的后端（见
//   WorkbenchDisasmView.h 文件头"二"），本夹具不链接那条路径（它依赖 ArkDriverClient/
//   KvmWatchDialog/UI_All.h 等与本组件无关的重依赖），这是本夹具唯一的已知覆盖缺口。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchDisasmView.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTextView.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchCompareView.h"
#include "../../../shared/evidence/memory_workbench/MemoryDiffOverlay.h"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

namespace wpH_test
{
    // 断言计数：与其它 memwb_ui 夹具同一套惯例（Report 失败时打印位置与表达式）。
    extern int g_checks;
    extern int g_failures;

    void Report(bool ok, const char* expression, const char* file, int line, const QString& note);

#define WPH_CHECK(expression) ::wpH_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, QString())
#define WPH_CHECK_NOTE(expression, note) ::wpH_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (note))

    // ApplyTheme：切换深浅主题并同步调色板，使控件与截图跟随（做法与其它 memwb_ui 夹具一致）。
    void ApplyTheme(bool dark);

    // GrabWidget：整控件抓图并转 ARGB32，供截图断言与人工查看。
    QImage GrabWidget(QWidget* widget);

    // FakeBytesProvider：包一个真实 MemoryDiffOverlay 的只读数据源实现，供三个子页测试共用。
    // 测试通过 overlay() 直接操作底层叠加层（LoadBaseline/RefreshBaseline/Stage/AcceptWrite），
    // 本类只负责把它翻译成 WorkbenchByteWindow；FetchWindow 之外不做任何额外加工或缓存。
    class FakeBytesProvider final : public ks::ui::IWorkbenchBytesProvider
    {
    public:
        explicit FakeBytesProvider(int addressBits = 64);

        ks::ui::WorkbenchByteWindow FetchWindow(std::uint64_t address, std::uint64_t length) const override;
        int AddressBits() const override;
        bool HasPreviousRead() const override;

        // overlay：供测试直接操作底层叠加层（载入基线/暂存/确认写入）。
        ksword::memwb::MemoryDiffOverlay& overlay();

        // setAddressBits：切换宣称的架构位数（供反汇编页"默认取 addressBits"场景的测试）。
        void setAddressBits(int bits);

    private:
        ksword::memwb::MemoryDiffOverlay m_overlay;
        int m_addressBits = 64;
    };

    // MakeRealZydisDecodeBackend：返回直接调用 Zydis 的单指令解码后端（见文件头说明）。
    ks::ui::DecodeOneFn MakeRealZydisDecodeBackend();

    // MakeRealAssembleBackend：返回包装 ks::ui::InstructionAssembler::assemble 的汇编后端
    // （真实生产实现，含语言包文案；MemoryAssembly.cpp 依赖的 LanguageManager 已经为
    // WorkbenchTextView 的 CodeEditorWidget 链接进本夹具，顺便复用）。
    ks::ui::AssembleOneFn MakeRealAssembleBackend();

    // 各组测试入口。
    void RunDisasmTests(const QString& shotsDir);
    void RunTextTests(const QString& shotsDir);
    void RunCompareTests(const QString& shotsDir);

    // 修复波（wave2）新增的回归测试入口，见各自文件头注释。
    void RunDisasmRegressionTests();
    void RunTextRegressionTests();
    void RunCompareRegressionTests();

    // 第二轮独立审核（review2-wpH.md）补测入口，见各自文件头注释；Disasm 部分按 ≤700 行
    // 的单文件上限拆成了 2/3 两份。
    void RunDisasmRegressionTests2();
    void RunDisasmRegressionTests3();
    void RunTextRegressionTests2();
    void RunCompareRegressionTests2();
    // RunSExtraTests：提交前独立审核者（S-wpH）重放变异时发现的三处缺口补测——
    // setEditable(false) 必须立即应用被推迟的刷新（N3 后半句）、UTF-8 块尾截断边界
    // （E4 41，原先被误判为"UB 等价体"）、applyTailGiveUp 的 15 字节阈值边界。
    void RunSExtraTests();
}
