#pragma once

// ============================================================
// WorkbenchDiagnosticsHost.h
// 作用：
// - 接口缺口 G3 的生产实现：实现 WorkbenchStatusBar.h 声明的
//   IWorkbenchDiagnosticsHost（诊断抽屉的最小接口），内部包一层项目内置
//   CodeEditorWidget（只读），供 MemoryWorkbenchView::buildUi() 构造
//   WorkbenchStatusBar 时注入。
// - 决策记录（装配接口文档 §8.0 G2/G3）：**不改 CodeEditorWidget**（主程序
//   基础设施，改头会扩大重编译面）。CodeEditorWidget 现有的换行机制是内部
//   私有的 m_wrapButton（点击即切换 QPlainTextEdit::lineWrapMode），没有任何
//   公开方法可以从外部编程式触发；本类因此不能"复用"它，只能如实让
//   SetWrapEnabled 报告不支持——记录请求值，但不产生任何可见效果。
//   WorkbenchStatusBar.h 是冻结头文件，没有"按宿主能力隐藏换行勾选框"的
//   setter，所以生产界面上这个勾选框仍会显示、仍能点击，只是切换它看不到
//   任何变化；这是任务书允许范围内能做到的最接近"报告不支持"的实现，具体
//   缺口（CodeEditorWidget 需要补一个 setWordWrapEnabled 才能真正解决）记在
//   报告文件里，不是本类能单方面解决的范围。
// - 诊断文本混合中文提示与通道给出的英文原文，属于"日志/原始内容"一类，按
//   仓库规范（AGENTS.md）经 CodeEditorWidget::setRawText 写入，不经任何翻译
//   通道、不受 LanguageChange 影响。
// ============================================================

#include "WorkbenchStatusBar.h"

#include <QWidget>

class CodeEditorWidget;

namespace ks::ui
{
    // WorkbenchDiagnosticsHost：见文件头。既是一个可直接加入布局的 QWidget
    // （HostWidget() 返回 this），也实现 IWorkbenchDiagnosticsHost 四个方法。
    class WorkbenchDiagnosticsHost final : public QWidget, public IWorkbenchDiagnosticsHost
    {
        Q_OBJECT

    public:
        explicit WorkbenchDiagnosticsHost(QWidget* parent = nullptr);
        ~WorkbenchDiagnosticsHost() override;

        // ---- IWorkbenchDiagnosticsHost 四个方法 ----
        QWidget* HostWidget() override;
        void SetDiagnosticsText(const QString& text) override;
        QString DiagnosticsText() const override;
        void SetWrapEnabled(bool wrap) override;

        // ---- 供夹具白盒断言，不是业务接口 ----
        // editorForTest：取内部的 CodeEditorWidget 指针（例如断言只读/文本内容）。
        CodeEditorWidget* editorForTest() const noexcept;
        // lastWrapRequestForTest：SetWrapEnabled 最近一次被要求的值，不代表真的
        // 生效（见文件头说明）。
        bool lastWrapRequestForTest() const noexcept;

    private:
        // editor_：唯一的子控件，拥有（parent 关系释放）。
        CodeEditorWidget* editor_ = nullptr;
        // lastWrapRequest_：见 SetWrapEnabled/lastWrapRequestForTest 的说明。
        bool lastWrapRequest_ = true;
    };
}
