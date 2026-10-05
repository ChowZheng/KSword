#pragma once

// ============================================================
// HexViewWidgets.h
// 作用：
// - HexView 复合控件一族（工具栏、查找条、跳转条、状态条）共用的五个"纯自绘小控件"：
//     HexViewBarFrame      条带底板：画 SurfaceAlt 底色与一条边线，作为各条带的基类/容器；
//     HexViewGlyphButton   图标按钮：十二种用 QPainter 画的矢量图标，可带当前值徽标与下拉菜单；
//     HexViewSegmented     分段按钮：互斥的几个文字段（查找模式、跳转模式），带逐段悬停提示，
//                          单个段可被禁用（不可点击、键盘跳过、画灰、提示改为原因）；
//     HexViewMessageLabel  单行消息：提示/信息/警告/错误四种颜色，过长省略、全文在悬停提示里；
//     HexViewStatusBar     底部状态条：插入点 | 选区 | 范围 三段，或一条替换它们的瞬时消息。
// - 为什么全部自绘：主程序 Ksword5.qrc 里没有"行宽/分组/解释器面板"这类图标，本阶段又不允许改 qrc；
//   自绘图形每次 paintEvent 现取 KswordTheme 的静态颜色，切换主题后下一次绘制就是新主题，
//   不使用样式表，因此不会踩 palette(...) 与静态色的"两族 token"陷阱，也没有主题残留。
// - 只有 HexViewSegmented 有信号（含 Q_OBJECT，需要 moc）；其余四个不含 Q_OBJECT。
//
// 输入焦点约定：
// - 图标按钮与分段按钮的焦点策略是 Qt::TabFocus：能用 Tab 键到达，但鼠标点击不抢焦点，
//   所以点工具栏按钮后键盘焦点仍留在画布/输入框里，Ctrl+C、方向键不会因点了一下按钮而失效。
//
// ------------------------------------------------------------
// 冻结接口摘要（Phase 3 WP-0 新增；后续会话条等工作包依赖，改签名或语义须同步依赖方）
// ------------------------------------------------------------
// HexViewSegmented（只列新增与语义有变化的成员，其余成员签名与语义不变）：
// - void setSegmentEnabled(int index, bool enabled, const QString& tooltipWhenDisabled = QString())
//     启用/禁用某一段，默认全部启用；越界无操作；不发信号、不替调用方换走当前段。
//     禁用段：鼠标点击被吞掉、键盘左右键跳过（不回绕）、文字画禁用灰、无悬停高亮、光标箭头、
//     悬停提示换成 tooltipWhenDisabled（空串则仍显示常规提示）；再启用清掉原因，常规提示不丢。
// - bool isSegmentEnabled(int index) const
//     某段是否可用；越界返回 false。
// - QString segmentToolTip(int index) const
//     语义变化：返回"此刻实际显示的提示"（禁用且有原因时是原因，否则是 setSegmentToolTip 设的常规提示）。
// - void setCurrentIndex(int index)
//     签名与语义不变：代码通路不受禁用限制，禁用的段也能被选中（用于"记住上次选择，此刻不可用也不替用户换"）。
// ============================================================

#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <QWidget>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;

namespace ks::ui
{
    // HexViewBarFrame：条带底板。
    // 用法：作为工具栏/查找条/跳转条的基类或容器，自己在 paintEvent 里画底色与边线，子控件放在上面。
    class HexViewBarFrame : public QWidget
    {
    public:
        // Edge：边线画在哪一侧。工具栏在顶部，边线画在下沿；状态条类的在底部，边线画在上沿。
        enum class Edge : int
        {
            None = 0,   // 不画边线
            Top,        // 画在上沿
            Bottom      // 画在下沿
        };

        // 构造：edge 决定边线位置，parent 为父控件。
        explicit HexViewBarFrame(Edge edge, QWidget* parent = nullptr);

        // edge：当前边线位置。
        Edge edge() const;

    protected:
        // paintEvent：画 SurfaceAlt 底色与边线（颜色现取）。
        void paintEvent(QPaintEvent* event) override;

    private:
        // m_edge：边线位置，构造后不变。
        Edge m_edge;
    };

    // HexViewGlyphButton：图标按钮，继承 QToolButton 以复用"点击弹出菜单"的行为。
    // 用法：构造时给定图形；需要下拉菜单就 setMenu + setPopupMode(InstantPopup)；
    //       需要显示当前值（行宽 16）就 setBadgeText；调用方自行 setToolTip / setCheckable。
    class HexViewGlyphButton : public QToolButton
    {
    public:
        // Glyph：按钮上画的图形。
        enum class Glyph : int
        {
            RowWidth = 0,   // 两行小方格 + 下方双向箭头：每行字节数
            GroupSize,      // 两组方格 + 下方括线：十六进制列分组
            Find,           // 放大镜：查找
            Goto,           // 箭头指向竖线：跳转到地址
            Export,         // 托盘 + 向下箭头：导出
            Inspector,      // 带右侧窗格的窗口：数据解释器面板
            CaseSensitive,  // "Aa"：区分大小写
            Previous,       // 向上的尖角：上一个
            Next,           // 向下的尖角：下一个
            Close,          // 叉：关闭
            Go,             // 向右箭头：执行跳转
            History         // 时钟：历史记录
        };

        // 构造：glyph 决定图形，parent 为父控件。
        explicit HexViewGlyphButton(Glyph glyph, QWidget* parent = nullptr);

        // glyph：当前图形。
        Glyph glyph() const;

        // setBadgeText：设置图形右侧的小字（例如当前行宽 "16"）；空串表示不显示。
        void setBadgeText(const QString& text);

        // badgeText：当前徽标文字。
        QString badgeText() const;

        // sizeHint：按图形、徽标与下拉箭头计算的紧凑尺寸。
        QSize sizeHint() const override;

    protected:
        // paintEvent：画底板（选中/悬停/按下）、图形、徽标与下拉箭头。
        void paintEvent(QPaintEvent* event) override;

    private:
        // m_glyph：图形种类，构造后不变。
        Glyph m_glyph;
        // m_badge：徽标文字。
        QString m_badge;
    };

    // HexViewSegmented：互斥的分段按钮。
    // 用法：构造时给出各段文字；用 setSegmentToolTip 给每一段配悬停提示；
    //       订阅 currentIndexChanged 得知用户（或代码）切换了哪一段。
    //       某一段此刻不可用（例如"该通道在当前范围下不可用"）时用 setSegmentEnabled 禁用它并写明原因。
    class HexViewSegmented : public QWidget
    {
        Q_OBJECT

    public:
        // 构造：labels 是各段文字（至少一段），parent 为父控件。当前段初始为第 0 段，所有段初始都可用。
        explicit HexViewSegmented(const QStringList& labels, QWidget* parent = nullptr);

        // setSegmentToolTip：设置某一段的（常规）悬停提示；下标越界什么也不做。
        // 该段被禁用时显示的是 setSegmentEnabled 给的原因，这里设的提示在重新启用后恢复显示。
        void setSegmentToolTip(int index, const QString& tip);

        // segmentToolTip：某一段此刻实际显示的悬停提示；越界返回空串。
        // 启用的段返回常规提示；禁用且给了原因的段返回原因；禁用但原因为空的段仍返回常规提示。
        QString segmentToolTip(int index) const;

        // setSegmentEnabled：启用或禁用某一段（所有段初始都启用）；下标越界什么也不做。
        // 传入：段下标；是否可用；tooltipWhenDisabled 仅在 enabled 为假时有意义——禁用期间悬停显示它
        //       （通常写"为什么不可用"）；传空串则仍显示常规提示。重新启用会清掉它。
        // 禁用段的行为（只管用户通路）：
        //   - 鼠标点击无效（点击被吞掉，不切换也不外传）；
        //   - 键盘左右键切换时被跳过：从当前段出发朝按键方向找下一个可用段，不回绕，找不到就保持不动；
        //   - 文字画成禁用灰，没有悬停高亮，鼠标移上去光标变回箭头；
        //   - 悬停提示换成 tooltipWhenDisabled。
        // 不管的两件事：
        //   - 禁用不会替调用方换走当前段。若当前段被禁用，它仍保持选中（画成灰底选中），是否改选由调用方决定
        //     （"记住上次的通道，此刻不可用时不自动换"就靠这一点）；
        //   - 代码调用 setCurrentIndex 仍可选中禁用段，也照常发信号。
        // 全部段都被禁用时：点击与按键都无效、当前段不变，控件本身仍是启用状态（整体 setEnabled(false) 另当别论）。
        void setSegmentEnabled(int index, bool enabled, const QString& tooltipWhenDisabled = QString());

        // isSegmentEnabled：某一段是否可用；越界返回 false。
        bool isSegmentEnabled(int index) const;

        // currentIndex：当前段下标。
        int currentIndex() const;

        // setCurrentIndex：切换当前段；下标越界被忽略；值真的变了才发 currentIndexChanged。
        // 这是代码通路，不受 setSegmentEnabled 的限制：禁用的段也能被代码选中。
        void setCurrentIndex(int index);

        // count：段数。
        int count() const;

        // labelAt：某段文字；越界返回空串。
        QString labelAt(int index) const;

        // segmentRect：某段在控件内的矩形（供测试点击与悬停定位）；越界返回空矩形。
        QRect segmentRect(int index) const;

        // sizeHint：各段文字宽度加内边距之和。
        QSize sizeHint() const override;

    signals:
        // currentIndexChanged：当前段变化，参数是新下标。
        void currentIndexChanged(int index);

    protected:
        // paintEvent：画外框、各段底板与文字。
        void paintEvent(QPaintEvent* event) override;

        // mousePressEvent：点击某段即切换到该段。
        void mousePressEvent(QMouseEvent* event) override;

        // keyPressEvent：左右方向键在段间切换（获得 Tab 焦点时）。
        void keyPressEvent(QKeyEvent* event) override;

        // event：处理逐段悬停提示（ToolTip）与悬停段跟踪（HoverMove/HoverLeave）。
        bool event(QEvent* event) override;

    private:
        // indexAt：控件坐标命中哪一段，没有命中返回 -1。
        int indexAt(const QPoint& pos) const;

        // nextEnabledIndex：从 from 出发沿 step 方向（+1 向右，-1 向左）找下一个可用段，不回绕。
        // 传入：起点（不含）、步进方向；传出：找到的段下标，没有可用段返回 -1。
        int nextEnabledIndex(int from, int step) const;

        // refreshHoverCursor：按悬停段是否可用设置光标：可用段（或没有悬停段）手形，禁用段箭头。
        void refreshHoverCursor();

        // m_labels：各段文字。
        QStringList m_labels;
        // m_tips：各段常规悬停提示，与 m_labels 等长。
        QStringList m_tips;
        // m_enabled：各段是否可用，与 m_labels 等长，初始全为真。
        QList<bool> m_enabled;
        // m_disabledTips：各段禁用时显示的提示（原因），与 m_labels 等长；段可用时恒为空串。
        QStringList m_disabledTips;
        // m_current：当前段下标。
        int m_current = 0;
        // m_hover：鼠标悬停的段下标，-1 表示没有。
        int m_hover = -1;
    };

    // HexViewMessageLabel：单行消息标签。
    // 用法：setMessage(种类, 文字) 显示；clearMessage 清空；文字超出宽度时右省略，全文在悬停提示里。
    class HexViewMessageLabel : public QWidget
    {
    public:
        // Kind：消息种类，决定文字颜色。
        enum class Kind : int
        {
            Hint = 0,   // 次要提示（灰）
            Info,       // 一次操作的结果（正文色）
            Warning,    // 警告（橙）
            Error       // 错误（红）
        };

        // 构造：parent 为父控件。
        explicit HexViewMessageLabel(QWidget* parent = nullptr);

        // setMessage：显示一条消息，同时把全文设为悬停提示；空文本等同 clearMessage。
        void setMessage(Kind kind, const QString& text);

        // clearMessage：清空消息。
        void clearMessage();

        // text：当前消息文字（空表示没有消息）。
        QString text() const;

        // kind：当前消息种类。
        Kind kind() const;

        // sizeHint / minimumSizeHint：一行文字高度；最小宽度很小，允许被布局压缩。
        QSize sizeHint() const override;
        QSize minimumSizeHint() const override;

    protected:
        // paintEvent：按种类取颜色画省略后的文字。
        void paintEvent(QPaintEvent* event) override;

    private:
        // m_text：消息文字。
        QString m_text;
        // m_kind：消息种类。
        Kind m_kind = Kind::Hint;
    };

    // HexViewStatusBar：底部状态条。
    // 用法：平时用 setCaretText/setSelectionText/setRangeText 画三段；
    //       有瞬时消息（跳转失败、导出结果、编辑被拒绝）时 setMessage 让它替换三段显示，
    //       clearMessage 或下一次选区变化后恢复。
    class HexViewStatusBar : public HexViewBarFrame
    {
    public:
        // Kind：瞬时消息种类，决定文字颜色与左侧色条。
        enum class Kind : int
        {
            Info = 0,   // 结果信息
            Warning,    // 警告
            Error       // 错误
        };

        // 构造：parent 为父控件。边线画在上沿。
        explicit HexViewStatusBar(QWidget* parent = nullptr);

        // 三段文字的设置与读取；与旧值相同不重绘。
        void setCaretText(const QString& text);
        void setSelectionText(const QString& text);
        void setRangeText(const QString& text);
        QString caretText() const;
        QString selectionText() const;
        QString rangeText() const;

        // setMessage：显示瞬时消息（替换三段）；空文本等同 clearMessage。全文同时设为悬停提示。
        void setMessage(Kind kind, const QString& text);

        // clearMessage：清除瞬时消息，恢复三段显示。
        void clearMessage();

        // hasMessage / messageText / messageKind：瞬时消息的状态。
        bool hasMessage() const;
        QString messageText() const;
        Kind messageKind() const;

        // sizeHint：一行文字高度加内边距。
        QSize sizeHint() const override;

    protected:
        // paintEvent：画底板、三段文字或瞬时消息。
        void paintEvent(QPaintEvent* event) override;

    private:
        // refreshToolTip：悬停提示 = 消息全文，或三段合并文字。
        void refreshToolTip();

        // m_caretText：插入点段文字。
        QString m_caretText;
        // m_selectionText：选区段文字。
        QString m_selectionText;
        // m_rangeText：范围段文字。
        QString m_rangeText;
        // m_message：瞬时消息文字，空表示没有。
        QString m_message;
        // m_messageKind：瞬时消息种类。
        Kind m_messageKind = Kind::Info;
    };
}
