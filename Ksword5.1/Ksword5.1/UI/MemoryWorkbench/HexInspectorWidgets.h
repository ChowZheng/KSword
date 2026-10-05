#pragma once

// ============================================================
// HexInspectorWidgets.h
// 作用：
// - 数据解释器面板（HexInspectorPanel）用到的三个"纯自绘小控件"：
//     HexInspectorGlyphButton  图标化的切换按钮（小端/大端、指针 4/8 字节），
//     HexInspectorTopBar       顶栏：左侧画插入点地址与选区提示，右侧放按钮，
//     HexInspectorStatusBar    底部状态条：提示 / 信息 / 错误三种消息，最多两行。
// - 三个控件都没有任何样式表：每次 paintEvent 现取 KswordTheme 的静态颜色，
//   因此切换主题后下一次绘制就是新主题，不需要任何通知，也不会残留旧色。
// - 三个控件都不含 Q_OBJECT（没有自己的信号槽），不需要 moc。
//
// 为什么自绘而不用 QToolButton + 图标：
// - 主程序 Ksword5.qrc 里没有"字节序"类的图标，本阶段又不允许改 qrc；
//   用 QPainter 按主题色即时画出的图形既能表达含义，又天然跟随主题与深浅色。
// ============================================================

#include <QAbstractButton>
#include <QFont>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

class QPaintEvent;

namespace ks::ui
{
    // HexInspectorFixedFont：解释器面板统一使用的等宽字体。
    // 做法与 HexCanvas 一致：Consolas 优先，其次系统等宽字体，最后 Microsoft YaHei UI 兜底中文。
    // 传出：字体对象（每次新建，调用方在构造期取一次即可）。
    QFont HexInspectorFixedFont();

    // HexInspectorRichToolTip：把一段纯文本变成"原样显示"的悬停提示。
    // 为什么需要：Qt 会把"看起来像 HTML"的提示文字当富文本渲染，而解释器的提示里含有来自目标内存的字符串
    // （例如字节恰好是 "<b>x</b>"），必须转义后再包进 white-space:pre 的块里，才能原样显示且保留换行。
    // 传入：纯文本；传出：可直接交给 setToolTip / QToolTip::showText 的文字。
    QString HexInspectorRichToolTip(const QString& plainText);

    // HexInspectorGlyphButton：一个可勾选的小图标按钮，图形由 Glyph 决定。
    // 用法：构造时给定图形，调用方自行 setCheckable / setToolTip / 放进布局。
    class HexInspectorGlyphButton : public QAbstractButton
    {
    public:
        // Glyph：按钮上画的图形种类。
        enum class Glyph : int
        {
            LittleEndian = 0,   // 三根由矮到高的竖条 + 向右箭头：低位字节在低地址
            BigEndian,          // 三根由高到矮的竖条 + 向右箭头：高位字节在低地址
            Pointer32,          // "32" 徽标：4 字节指针
            Pointer64           // "64" 徽标：8 字节指针
        };

        // 构造：glyph 决定图形，parent 为父控件。
        explicit HexInspectorGlyphButton(Glyph glyph, QWidget* parent = nullptr);

        // glyph：当前图形种类。
        Glyph glyph() const;

        // sizeHint：固定的紧凑尺寸（宽 28、高 24 像素量级）。
        QSize sizeHint() const override;

    protected:
        // paintEvent：画底板（选中/悬停/按下）与图形。
        void paintEvent(QPaintEvent* event) override;

    private:
        // m_glyph：图形种类，构造后不变。
        Glyph m_glyph;
    };

    // HexInspectorTopBar：面板顶栏。
    // 用法：把按钮作为子控件放进它的布局；调用 setTextLimitWidget 指定"文字画到哪个控件的左边缘为止"。
    class HexInspectorTopBar : public QWidget
    {
    public:
        // 构造：parent 为父控件。
        explicit HexInspectorTopBar(QWidget* parent = nullptr);

        // setAddressText：设置左侧的插入点地址文字（等宽字体）；与旧值相同则不重绘。
        void setAddressText(const QString& text);

        // addressText：当前地址文字。
        QString addressText() const;

        // setHintText：设置地址右侧的灰色提示（例如"已选 36 字节"）；空串表示不显示。
        void setHintText(const QString& text);

        // hintText：当前提示文字。
        QString hintText() const;

        // setTextLimitWidget：文字最右只画到该控件左边缘之前；传空表示画到控件右边缘。
        void setTextLimitWidget(QWidget* widget);

        // sizeHint：与按钮高度匹配的建议尺寸。
        QSize sizeHint() const override;

    protected:
        // paintEvent：画底色、下边框、地址与提示。
        void paintEvent(QPaintEvent* event) override;

    private:
        // m_addressText：地址文字。
        QString m_addressText;
        // m_hintText：提示文字。
        QString m_hintText;
        // m_limitWidget：限定文字右边界的控件（非拥有，用 QPointer 防悬空）。
        QPointer<QWidget> m_limitWidget;
    };

    // HexInspectorStatusBar：面板底部的消息条，固定两行高度，消息超长时在第二行末尾截断，悬停显示全文。
    class HexInspectorStatusBar : public QWidget
    {
    public:
        // Kind：消息种类，决定文字颜色与左侧色条。
        enum class Kind : int
        {
            Hint = 0,   // 常驻提示（没有消息时显示）
            Info,       // 一次操作的结果信息
            Error       // 错误：编辑被拒绝等
        };

        // 构造：parent 为父控件。
        explicit HexInspectorStatusBar(QWidget* parent = nullptr);

        // setIdleHint：设置没有消息时显示的常驻提示。
        void setIdleHint(const QString& hint);

        // setMessage：显示一条消息；同时把全文设为悬停提示。空文本等同 clearMessage。
        void setMessage(Kind kind, const QString& text);

        // clearMessage：清除消息，回到常驻提示。
        void clearMessage();

        // messageText：当前显示的文字（没有消息时是常驻提示）。
        QString messageText() const;

        // kind：当前消息种类。
        Kind kind() const;

        // sizeHint：两行文字高度。
        QSize sizeHint() const override;

    protected:
        // paintEvent：画底色、上边框、色条与文字。
        void paintEvent(QPaintEvent* event) override;

    private:
        // refreshToolTip：把当前消息全文同步给悬停提示。
        void refreshToolTip();

        // m_idleHint：常驻提示文字。
        QString m_idleHint;
        // m_message：当前消息文字，空表示没有消息。
        QString m_message;
        // m_kind：当前消息种类。
        Kind m_kind = Kind::Hint;
    };
}
