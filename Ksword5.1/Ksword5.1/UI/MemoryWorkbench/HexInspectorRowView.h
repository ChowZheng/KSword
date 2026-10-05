#pragma once

// ============================================================
// HexInspectorRowView.h
// 作用：
// - 数据解释器面板的"行列表"：固定 17 行（类型名 | 值 | 十六进制），整个视口自绘，
//   不使用 QTableWidget / QTreeWidget——这样每次 paintEvent 都能现取主题静态颜色，
//   不依赖 palette(...) 样式串，也没有逐单元格的 item 对象开销。
// - 只负责"显示 + 手势转发"：不做任何字节解释，也不碰画布与叠加层。行内容由面板
//   经 setRows 整体灌入；用户的双击/回车/右键/复制/行内编辑提交都以信号交还给面板。
//
// 交互：
// - 单击选中当前行；上下/Home/End/PageUp/PageDown 移动当前行；
// - 双击或 Enter/F2：发 rowActivated（面板决定能否编辑）；
// - 悬停某行时，在行尾显示"复制"图标，点击发 copyRequested(row, Value)；
// - 右键或菜单键：发 contextMenuRequested，菜单由面板构造（本控件不弹菜单）；
// - Ctrl+C：发 copyRequested(当前行, Value)；
// - 行内编辑：beginEdit 在值列+十六进制列的位置盖一个 QLineEdit；
//   Enter 发 editCommitted（由面板校验，失败则 setEditError(true) 保持编辑器打开）；
//   Esc 或点到别处（失去焦点）发 editCancelled。窗口失活/弹出菜单造成的失焦不取消。
//
// 布局：
// - 列宽由字体度量决定：类型列固定为最宽类型名；末尾 22 像素是"复制图标列"；
//   剩余宽度里，十六进制列取其理想宽度，其余全给值列（时间字符串更长）；
//   放不下时按理想宽度比例分配，文字省略（值列右省略、十六进制列中间省略），全文在悬停提示里。
// - 没有横向滚动条；行数据高度超过视口时出现竖向滚动条（像素滚动，单步一行）。
//
// 主题：全部颜色在 paintEvent 里现取 KswordTheme 静态访问器，不缓存。行内编辑器的样式表在
// 每次 beginEdit 时用当时的静态颜色生成（编辑器是临时对象，主题就绪后才构造）。
// ============================================================

#include <QAbstractScrollArea>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <vector>

class QContextMenuEvent;
class QEvent;
class QFocusEvent;
class QKeyEvent;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;

namespace ks::ui
{
    // HexInspectorRowData：一行要显示的全部内容，由面板算好后灌入。
    struct HexInspectorRowData
    {
        QString typeKey;        // 解释器行的英文键（i8/u32/f64/ptr...），同时是 EncodeValue 的类型名
        QString typeName;       // 类型列显示的名称
        QString valueText;      // 值列文字；字节不足的行为"不可用"
        QString hexText;        // 十六进制列文字
        QString valueCopy;      // "复制值"得到的文本；不可用行为空
        QString hexCopy;        // "复制十六进制"得到的完整文本；不可用行为空
        QString toolTip;        // 整行悬停提示（含全文与操作说明）
        bool available = false; // 字节是否足够解出这一行
        bool valid = true;      // 字节足够但内容是否合法（时间超范围、孤立代理项为 false）
        bool editable = false;  // 该类型是否支持编辑（整数/浮点/指针）
        bool editEnabled = false; // 类型支持编辑且画布当前允许编辑；决定悬停时是否描出"可编辑"细边
    };

    // HexInspectorRowView：自绘行列表，详见文件头。
    class HexInspectorRowView : public QAbstractScrollArea
    {
        Q_OBJECT

    public:
        // CopyKind：复制请求的内容种类。
        enum class CopyKind : int
        {
            Value = 0,  // 值列
            Hex,        // 十六进制列
            Row         // 整行（类型、值、十六进制以制表符分隔）
        };

        // 构造：parent 为父控件。默认空行表、等宽字体、可获得焦点。
        explicit HexInspectorRowView(QWidget* parent = nullptr);

        // setRows：整体替换行内容。行数变化会重算滚动范围；当前行编号被夹取到有效范围。
        void setRows(std::vector<HexInspectorRowData> rows);

        // rowCount：当前行数。
        int rowCount() const;

        // rowAt：取一行的只读引用；index 越界时返回一行空数据（不会崩溃）。
        const HexInspectorRowData& rowAt(int index) const;

        // currentRow：当前行（键盘焦点行），没有行时为 -1。
        int currentRow() const;

        // setCurrentRow：设置当前行并滚动到可见；越界时夹取；发 currentRowChanged。
        void setCurrentRow(int row);

        // rowHeight / headerHeight：行高与列标题高度（像素）。
        int rowHeight() const;
        int headerHeight() const;

        // rowRect：整行矩形（视口坐标）；越界返回空矩形。
        QRect rowRect(int row) const;

        // valueCellRect / hexCellRect / copyGlyphRect：值列、十六进制列、复制图标的矩形（视口坐标）。
        QRect valueCellRect(int row) const;
        QRect hexCellRect(int row) const;
        QRect copyGlyphRect(int row) const;

        // rowAtPosition：视口坐标落在哪一行；标题区或行外返回 -1。
        int rowAtPosition(const QPoint& viewportPos) const;

        // toolTipAt：视口坐标处的悬停提示文字（标题、复制图标、整行三种），没有则返回空串。
        QString toolTipAt(const QPoint& viewportPos) const;

        // beginEdit：在指定行的值列位置打开行内编辑器。
        // 传入：行号与初始文本。传出：false 表示行号越界。已有编辑器会先被结束（不发取消信号）。
        bool beginEdit(int row, const QString& initialText);

        // endEdit：结束并销毁编辑器，不发任何信号；没有编辑器时什么也不做。
        void endEdit();

        // isEditing / editingRow：是否正在编辑与被编辑的行号（没有时为 -1）。
        bool isEditing() const;
        int editingRow() const;

        // editor：当前编辑器（没有时为空），供测试与面板读取文本。
        QLineEdit* editor() const;

        // setEditError：把编辑器边框切成错误色（true）或恢复强调色（false）。没有编辑器时什么也不做。
        void setEditError(bool hasError);

        // sizeHint：按理想列宽与全部行高给出建议尺寸。
        QSize sizeHint() const override;

        // minimumSizeHint：能看清类型名与一小段值的最小尺寸。
        QSize minimumSizeHint() const override;

    signals:
        // rowActivated：双击或 Enter/F2 激活某行。
        void rowActivated(int row);

        // copyRequested：请求复制某行的某种内容；kind 取 CopyKind 的整数值。
        void copyRequested(int row, int kind);

        // contextMenuRequested：请求在 globalPos 弹出某行的菜单；row 为 -1 表示点在空白处。
        void contextMenuRequested(int row, const QPoint& globalPos);

        // editCommitted：用户在编辑器里按了 Enter，text 是当前输入。
        void editCommitted(int row, const QString& text);

        // editCancelled：用户取消了编辑（Esc 或点到别处）。
        void editCancelled(int row);

        // currentRowChanged：当前行变化。
        void currentRowChanged(int row);

    protected:
        void paintEvent(QPaintEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;
        void changeEvent(QEvent* event) override;
        void scrollContentsBy(int dx, int dy) override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseDoubleClickEvent(QMouseEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void contextMenuEvent(QContextMenuEvent* event) override;
        void focusInEvent(QFocusEvent* event) override;
        void focusOutEvent(QFocusEvent* event) override;
        bool viewportEvent(QEvent* event) override;
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        // Columns：各列在视口里的横向位置（像素）。
        struct Columns
        {
            int nameX = 0;      // 类型列左边界
            int nameW = 0;      // 类型列宽度
            int valueX = 0;     // 值列左边界
            int valueW = 0;     // 值列宽度
            int hexX = 0;       // 十六进制列左边界
            int hexW = 0;       // 十六进制列宽度
            int actionX = 0;    // 复制图标列左边界
            int actionW = 0;    // 复制图标列宽度
        };

        // PaintPalette：一次绘制用到的全部颜色，定义在 HexInspectorRowView.Paint.cpp。
        struct PaintPalette;

        // ---------- HexInspectorRowView.cpp：状态、布局、输入、编辑器 ----------
        void rebuildMetrics();
        void updateScrollBars();
        Columns computeColumns() const;
        QRect editorRect(int row) const;
        void repositionEditor();
        void endEditInternal(bool restoreFocus);
        QString editorStyleSheet(bool hasError) const;
        void moveCurrentRow(int delta);
        void ensureRowVisible(int row);
        bool overCopyGlyph(const QPoint& viewportPos, int row) const;
        void setHoverRow(int row);

        // ---------- HexInspectorRowView.Paint.cpp：绘制 ----------
        PaintPalette makePalette() const;
        void paintHeader(QPainter& painter, const PaintPalette& palette, const Columns& columns) const;
        void paintRow(QPainter& painter, const PaintPalette& palette, const Columns& columns, int row) const;
        void paintCopyGlyph(QPainter& painter, const QRect& rect, const QColor& color) const;

        // 暂定常量。
        static constexpr int kCellPadding = 8;      // 单元格左右内边距（像素）
        static constexpr int kActionWidth = 24;     // 复制图标列宽度（像素）
        static constexpr int kValueIdealChars = 20; // 值列理想字符数（u64 最长 20 位十进制）
        static constexpr int kHexIdealChars = 18;   // 十六进制列理想字符数（"0x" + 16 位）

        // ---- 数据 ----
        std::vector<HexInspectorRowData> m_rows;    // 全部行
        int m_currentRow = -1;                      // 当前行，-1 表示没有
        int m_hoverRow = -1;                        // 鼠标悬停行，-1 表示没有

        // ---- 度量（rebuildMetrics 维护） ----
        int m_charWidth = 8;                        // 等宽字符宽度
        int m_rowHeight = 24;                       // 行高
        int m_headerHeight = 24;                    // 列标题高度
        int m_nameWidth = 70;                       // 类型列宽度（含内边距）

        // ---- 编辑 ----
        QLineEdit* m_editor = nullptr;              // 行内编辑器（子控件，由本类管理生命周期）
        int m_editRow = -1;                         // 正在编辑的行，-1 表示没有
        bool m_editHasError = false;                // 编辑器边框当前是否处于错误态（避免每次按键都重设样式表）
    };
}
