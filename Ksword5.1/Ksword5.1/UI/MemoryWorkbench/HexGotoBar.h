#pragma once

// ============================================================
// HexGotoBar.h
// 作用：
// - HexView 的跳转条：模式（绝对地址 / 相对偏移 / 行号三段按钮）、输入框、历史下拉、执行、关闭。
//   取代旧 HexEditorWidget 的跳转面板。
// - 输入解析复用 shared/evidence/memory_workbench/MemoryAddressExpr（EvaluateAddressExpr）：
//   默认十六进制（纯数字 1233 就是 0x1233，绝不是十进制），0x 前缀可有可无，支持 ` 与 _ 分隔，
//   支持用 + 相加（1000+20），溢出判失败绝不回绕。没有进程上下文，所以模块名与方括号解引用
//   都明确拒绝（提示"此处不支持…"），而不是悄悄算成别的数。
//
// ------------------------------------------------------------
// 一、三种模式（Resolve 的语义）
// ------------------------------------------------------------
// - 绝对地址：表达式的值就是目标地址，必须落在数据范围 [first, last] 内。
// - 相对偏移：表达式的值是相对数据起点（first）的字节偏移，十六进制默认；目标 = first + 偏移，
//   偏移必须不超过 last - first。
// - 行号：十进制非负整数，从 0 起，对应画布显示的第 N 行（行按绝对地址对齐，首行可能含补空位）；
//   目标是该行第一个属于数据范围的地址。行号必须小于总行数。
//
// ------------------------------------------------------------
// 二、错误提示（不弹模态框）
// ------------------------------------------------------------
// - 错误显示在输入框下方一行（红色），下一次输入、切换模式或成功跳转时清除。
// - 范围外明确写"超出数据范围 [起点, 终点]"，起点终点的单位与当前模式一致：
//   绝对地址模式是地址，偏移模式是偏移（0x0 起），行号模式是行号（0 起）。
// - 解析错误带"第 N 个字符"（N 是 1 起的字符序号，输入含中文时也准确）。
//
// ------------------------------------------------------------
// 三、历史
// ------------------------------------------------------------
// - 一次跳转成功后把输入文本放进历史（最近的在最前、去重、最多 10 条），持久化到 QSettings
//   键 memwb/hexview/gotoHistory（HexViewSettings.h）；读失败退回空。输入或超范围错误的文本不进历史。
// - 输入框里按 Up/Down 在历史里前后翻（Down 翻过最新一条回到你正在输入的草稿）；
//   历史按钮（时钟图标）弹出下拉菜单，点一项把它填进输入框。菜单显式设置不透明背景、文字、选中态、禁用态样式。
// - 打开本条（open）时重新读取历史，使多个 HexView 之间的历史保持同步。
//
// ------------------------------------------------------------
// 四、宿主接口
// ------------------------------------------------------------
// - setSpace(first, last, bytesPerRow)：告知数据范围与当前行宽（行号模式要用）；clearSpace 表示没有数据。
// - 跳转成功发 gotoRequested(address)，宿主据此选中并滚动；Esc/关闭按钮发 closeRequested。
// - 所有公开函数只允许在 UI 线程调用。
//
// 文件分工：HexGotoBar.h（Q_OBJECT，需 moc）/ HexGotoBar.cpp。
// ============================================================

#include "HexViewWidgets.h"

#include <QString>
#include <QStringList>

#include <cstdint>

class QLineEdit;
class QMenu;

namespace ks::ui
{
    // HexGotoBar：跳转条，详见文件头。
    class HexGotoBar : public HexViewBarFrame
    {
        Q_OBJECT

    public:
        // Mode：输入的含义。
        enum class Mode : int
        {
            Absolute = 0,   // 绝对地址
            Offset,         // 相对数据起点的偏移
            Row             // 行号
        };

        // Space：跳转的目标空间，即数据范围与当前行宽。
        struct Space
        {
            bool valid = false;             // 是否有数据
            std::uint64_t first = 0;        // 数据起始地址（含）
            std::uint64_t last = 0;         // 数据结束地址（含）
            int bytesPerRow = 16;           // 当前每行字节数（行号模式用）
        };

        // 构造：parent 为父控件。读取一次历史。
        explicit HexGotoBar(QWidget* parent = nullptr);

        // Resolve：把一段输入按模式解析成目标地址（纯函数，不碰界面与历史，供夹具直接测试）。
        // 传入：模式、输入文本、目标空间；
        // 传出：addressOut 成功时为目标地址；errorOut 失败时是给用户看的原因；返回 true 表示成功。
        static bool Resolve(
            Mode mode,
            const QString& text,
            const Space& space,
            std::uint64_t* addressOut,
            QString* errorOut);

        // ---------------- 宿主接口 ----------------

        // setSpace：设置数据范围与行宽；first > last 或 bytesPerRow 不被接受时等同 clearSpace。
        void setSpace(std::uint64_t first, std::uint64_t last, int bytesPerRow);

        // clearSpace：没有数据，之后的跳转都报"没有可跳转的数据"。
        void clearSpace();

        // space：当前目标空间。
        Space space() const;

        // open：显示本条、重读历史、聚焦输入框并全选。
        void open();

        // ---------------- 状态与输入 ----------------

        // mode / setMode：当前模式；切换会更新占位提示并清除错误，不清输入。
        Mode mode() const;
        void setMode(Mode mode);

        // inputText / setInputText：输入框文字。
        QString inputText() const;
        void setInputText(const QString& text);

        // submit：解析当前输入并跳转。
        // 传出：true 表示已发 gotoRequested；false 表示被拒绝，原因显示在输入框下方（errorText）。
        bool submit();

        // errorText：输入框下方的错误文字，没有错误为空串。
        QString errorText() const;

        // history：当前历史（最近的在最前）。
        QStringList history() const;

        // reloadHistory：从设置里重读历史。
        void reloadHistory();

        // historyMenu：历史下拉菜单（内部对象，随本条销毁）；rebuildHistoryMenu 按当前历史重建菜单项。
        QMenu* historyMenu() const;
        void rebuildHistoryMenu();

        // 内部控件访问器：供宿主微调与离屏测试读取，不改变行为。
        QLineEdit* lineEdit() const;
        HexViewSegmented* modeSegment() const;
        HexViewGlyphButton* goButton() const;
        HexViewGlyphButton* historyButton() const;
        HexViewGlyphButton* closeButton() const;
        HexViewMessageLabel* errorLabel() const;

    signals:
        // gotoRequested：解析成功且落在范围内，address 是目标地址。
        void gotoRequested(quint64 address);

        // closeRequested：用户点了关闭按钮。
        void closeRequested();

    protected:
        // eventFilter：拦截输入框的 Up/Down，在历史里前后翻。
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        // buildUi：搭建界面并连接信号。
        void buildUi();

        // showError / clearError：设置或清除输入框下方的错误行。
        void showError(const QString& text);
        void clearError();

        // onModeChanged：模式切换后的更新（占位提示、清错误）。
        void onModeChanged();

        // browseHistory：在历史里翻 delta 格（+1 更旧，-1 更新）；翻过最新一条恢复草稿。
        void browseHistory(int delta);

        // menuStyleSheet：历史菜单的样式表（静态主题色，菜单每次弹出前重新生成）。
        QString menuStyleSheet() const;

        // ---- 子控件 ----
        HexViewSegmented* m_modeSegment = nullptr;      // 模式三段按钮
        QLineEdit* m_edit = nullptr;                    // 输入框
        HexViewGlyphButton* m_historyButton = nullptr;  // 历史按钮（带下拉菜单）
        HexViewGlyphButton* m_goButton = nullptr;       // 执行按钮
        HexViewGlyphButton* m_closeButton = nullptr;    // 关闭按钮
        HexViewMessageLabel* m_error = nullptr;         // 输入框下方的错误行
        QMenu* m_historyMenu = nullptr;                 // 历史下拉菜单

        // ---- 状态 ----
        Space m_space;                                  // 目标空间
        QStringList m_history;                          // 历史（最近的在最前）
        int m_historyCursor = -1;                       // Up/Down 翻历史的当前位置，-1 表示没有在翻
        QString m_draft;                                // 开始翻历史时用户正在输入的草稿
    };
}
