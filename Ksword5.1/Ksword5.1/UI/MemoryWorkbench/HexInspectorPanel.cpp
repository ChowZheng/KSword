// HexInspectorPanel.cpp
// 作用：数据解释器面板的构造、画布绑定（订阅画布的信号）、字节序/指针宽度设置与刷新。
// 面板没有任何定时器：画布数据变化由 HexCanvas::contentChanged 通知，可编辑状态由 editableChanged 通知。
// 取字节与行内容见 HexInspectorPanel.Rows.cpp，行内编辑见 .Edit.cpp，复制与菜单见 .Menu.cpp。

#include "HexInspectorPanel.h"
#include "HexInspectorWidgets.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>

namespace ks::ui
{
    namespace
    {
        // kByteOrderKey / kPointerWidthKey：QSettings 里的键名。
        const QString& ByteOrderKey()
        {
            static const QString key = QStringLiteral("memwb/inspector/byteOrder");
            return key;
        }

        const QString& PointerWidthKey()
        {
            static const QString key = QStringLiteral("memwb/inspector/pointerWidth");
            return key;
        }
    }

    // 构造：建界面、读设置、同步按钮状态、首次刷新。
    HexInspectorPanel::HexInspectorPanel(QWidget* parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("memwb_inspector_panel"));
        buildUi();
        loadSettings();
        applyToggleState();
        refresh(true);
    }

    // 析构：子控件随父对象自动释放。
    HexInspectorPanel::~HexInspectorPanel() = default;

    // 关联画布。
    void HexInspectorPanel::setCanvas(HexCanvas* canvas)
    {
        if (m_canvas.data() == canvas)
        {
            return;
        }

        // 先断开旧画布的全部连接，再接新画布；正在进行的行内编辑属于旧画布，直接放弃。
        if (m_canvas != nullptr)
        {
            disconnect(m_canvas.data(), nullptr, this, nullptr);
        }
        m_rows->endEdit();
        m_editRow = -1;
        m_canvas = canvas;
        if (canvas != nullptr)
        {
            // 选区与插入点变化：同步重读窗口。
            connect(canvas, &HexCanvas::caretMoved, this, &HexInspectorPanel::onCaretOrSelectionChanged);
            connect(canvas, &HexCanvas::selectionChanged, this, &HexInspectorPanel::onCaretOrSelectionChanged);

            // 画布显示的值可能变了（页回填、换代次/空间/叠加层、暂存、丢弃）：排队合并过的 contentChanged 一发就重读，
            // 取代原先 150 ms 的轮询。可编辑状态变化（editableChanged，同步信号）另有专门的槽：
            // 重读时快照里的 canEdit 变了会重建各行的"可编辑"状态与提示，并清掉已经过时的错误消息。
            connect(canvas, &HexCanvas::contentChanged, this, &HexInspectorPanel::onCanvasDataChanged);
            connect(canvas, &HexCanvas::editableChanged, this, &HexInspectorPanel::onCanvasEditableChanged);

            // 画布被销毁时回到"没有数据"：此时 QPointer 已经为空，refresh 会正确显示不可用。
            connect(canvas, &QObject::destroyed, this, [this]() { refresh(true); });
        }
        m_status->clearMessage();
        refresh(true);
    }

    // 当前画布。
    HexCanvas* HexInspectorPanel::canvas() const
    {
        return m_canvas.data();
    }

    // 设置指针描述器并重建。
    void HexInspectorPanel::setPointerNamer(ksword::memwb::IPointerNamer* namer)
    {
        m_namer = namer;
        refresh(true);
    }

    // 字节序。
    ksword::memwb::ByteOrder HexInspectorPanel::byteOrder() const
    {
        return m_order;
    }

    // 设置字节序：同步按钮、保存设置、取消进行中的编辑（旧字节序下的输入含义已变）、重建。
    void HexInspectorPanel::setByteOrder(ksword::memwb::ByteOrder order)
    {
        if (order == m_order)
        {
            applyToggleState();
            return;
        }
        m_order = order;
        applyToggleState();
        saveSettings();
        m_rows->endEdit();
        m_editRow = -1;
        emit byteOrderChanged(order == ksword::memwb::ByteOrder::Big ? 1 : 0);
        refresh(true);
    }

    // 指针宽度。
    std::uint32_t HexInspectorPanel::pointerWidthBytes() const
    {
        return m_pointerWidth;
    }

    // 设置指针宽度，非法值忽略。
    void HexInspectorPanel::setPointerWidthBytes(std::uint32_t widthBytes)
    {
        if (widthBytes != 4U && widthBytes != 8U)
        {
            return;
        }
        if (widthBytes == m_pointerWidth)
        {
            applyToggleState();
            return;
        }
        m_pointerWidth = widthBytes;
        applyToggleState();
        saveSettings();
        m_rows->endEdit();
        m_editRow = -1;
        emit pointerWidthChanged(widthBytes);
        refresh(true);
    }

    // 改存到指定 INI，并立即按文件内容重读。
    void HexInspectorPanel::setSettingsFile(const QString& iniPath)
    {
        m_settingsFile = iniPath;
        loadSettings();
        applyToggleState();
        m_rows->endEdit();
        m_editRow = -1;
        refresh(true);
    }

    // 内部控件访问器。
    HexInspectorRowView* HexInspectorPanel::rowView() const
    {
        return m_rows;
    }

    HexInspectorTopBar* HexInspectorPanel::topBar() const
    {
        return m_topBar;
    }

    HexInspectorStatusBar* HexInspectorPanel::statusBar() const
    {
        return m_status;
    }

    // 字节序按钮。
    HexInspectorGlyphButton* HexInspectorPanel::byteOrderButton(ksword::memwb::ByteOrder order) const
    {
        return order == ksword::memwb::ByteOrder::Big ? m_bigButton : m_littleButton;
    }

    // 指针宽度按钮。
    HexInspectorGlyphButton* HexInspectorPanel::pointerWidthButton(std::uint32_t widthBytes) const
    {
        return widthBytes == 4U ? m_ptr32Button : m_ptr64Button;
    }

    // 建议尺寸：宽度取行列表的理想宽度，高度取各部件之和。
    QSize HexInspectorPanel::sizeHint() const
    {
        const QSize rows = m_rows->sizeHint();
        const int height = m_topBar->sizeHint().height() + rows.height() + m_status->sizeHint().height();
        return QSize(std::max(rows.width(), 380), height);
    }

    // 最小尺寸：窄到 260 像素左右仍能看清类型名、一小段值与四个按钮。
    QSize HexInspectorPanel::minimumSizeHint() const
    {
        const QSize rows = m_rows->minimumSizeHint();
        const int height = m_topBar->sizeHint().height() + rows.height() + m_status->sizeHint().height();
        return QSize(std::max(rows.width(), 260), height);
    }

    // 重读并重建（槽，供宿主调用）。
    void HexInspectorPanel::refreshFromCanvas()
    {
        refresh(true);
    }

    // 画布插入点或选区变化。
    void HexInspectorPanel::onCaretOrSelectionChanged()
    {
        refresh(false);
    }

    // 画布显示内容变化（contentChanged）：重读窗口，
    // 只有快照真的变了才重建行，所以一个页的回填如果与插入点附近无关，开销就是一次 16 字节的读取比较。
    void HexInspectorPanel::onCanvasDataChanged()
    {
        refresh(false);
    }

    // 画布可编辑状态变化（editableChanged）：清掉过时的错误消息并重读。
    // 状态条里"当前为只读视图，不能编辑"这类错误直接与可编辑状态相关，状态翻转后已不成立；
    // 重读时快照里的 canEdit 变了，会重建各行的"可编辑"状态、悬停提示与状态条的空闲提示。
    void HexInspectorPanel::onCanvasEditableChanged()
    {
        if (m_status->kind() == HexInspectorStatusBar::Kind::Error)
        {
            m_status->clearMessage();
        }
        refresh(false);
    }

    // 建界面：顶栏（地址/提示 + 四个图标按钮）、行列表、状态条自上而下排列。
    void HexInspectorPanel::buildUi()
    {
        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        // 顶栏：地址文字由顶栏自己画，右侧按钮用布局摆放。
        m_topBar = new HexInspectorTopBar(this);
        m_topBar->setFont(HexInspectorFixedFont());
        QHBoxLayout* barLayout = new QHBoxLayout(m_topBar);
        barLayout->setContentsMargins(8, 3, 8, 3);
        barLayout->setSpacing(2);
        barLayout->addStretch(1);

        // 字节序两个按钮：互斥，悬停提示说明含义与默认。
        m_littleButton = new HexInspectorGlyphButton(HexInspectorGlyphButton::Glyph::LittleEndian, m_topBar);
        m_bigButton = new HexInspectorGlyphButton(HexInspectorGlyphButton::Glyph::BigEndian, m_topBar);
        m_littleButton->setToolTip(QStringLiteral("小端序（低位字节在低地址，x86 默认）：整数、浮点、指针与时间按此解释与写入"));
        m_bigButton->setToolTip(QStringLiteral("大端序（高位字节在低地址）：整数、浮点、指针与时间按此解释与写入"));
        m_orderGroup = new QButtonGroup(this);
        m_orderGroup->setExclusive(true);
        m_orderGroup->addButton(m_littleButton);
        m_orderGroup->addButton(m_bigButton);
        barLayout->addWidget(m_littleButton);
        barLayout->addWidget(m_bigButton);
        barLayout->addSpacing(8);

        // 指针宽度两个按钮：互斥。
        m_ptr32Button = new HexInspectorGlyphButton(HexInspectorGlyphButton::Glyph::Pointer32, m_topBar);
        m_ptr64Button = new HexInspectorGlyphButton(HexInspectorGlyphButton::Glyph::Pointer64, m_topBar);
        m_ptr32Button->setToolTip(QStringLiteral("指针宽度 4 字节（32 位）"));
        m_ptr64Button->setToolTip(QStringLiteral("指针宽度 8 字节（64 位）"));
        m_pointerGroup = new QButtonGroup(this);
        m_pointerGroup->setExclusive(true);
        m_pointerGroup->addButton(m_ptr32Button);
        m_pointerGroup->addButton(m_ptr64Button);
        barLayout->addWidget(m_ptr32Button);
        barLayout->addWidget(m_ptr64Button);
        m_topBar->setTextLimitWidget(m_littleButton);

        // 按钮点击 -> 设置；用 clicked 而不是 toggled，这样程序同步勾选状态时不会递归回到设置。
        connect(m_littleButton, &QAbstractButton::clicked, this, [this]() {
            setByteOrder(ksword::memwb::ByteOrder::Little);
        });
        connect(m_bigButton, &QAbstractButton::clicked, this, [this]() {
            setByteOrder(ksword::memwb::ByteOrder::Big);
        });
        connect(m_ptr32Button, &QAbstractButton::clicked, this, [this]() {
            setPointerWidthBytes(4U);
        });
        connect(m_ptr64Button, &QAbstractButton::clicked, this, [this]() {
            setPointerWidthBytes(8U);
        });

        // 行列表与状态条。
        m_rows = new HexInspectorRowView(this);
        m_status = new HexInspectorStatusBar(this);
        connect(m_rows, &HexInspectorRowView::rowActivated, this, &HexInspectorPanel::onRowActivated);
        connect(m_rows, &HexInspectorRowView::copyRequested, this, &HexInspectorPanel::onCopyRequested);
        connect(m_rows, &HexInspectorRowView::contextMenuRequested, this, &HexInspectorPanel::onContextMenuRequested);
        connect(m_rows, &HexInspectorRowView::editCommitted, this, &HexInspectorPanel::onEditCommitted);
        connect(m_rows, &HexInspectorRowView::editCancelled, this, &HexInspectorPanel::onEditCancelled);

        layout->addWidget(m_topBar);
        layout->addWidget(m_rows, 1);
        layout->addWidget(m_status);
    }

    // 让四个按钮的勾选状态与当前字节序/指针宽度一致。
    void HexInspectorPanel::applyToggleState()
    {
        m_littleButton->setChecked(m_order == ksword::memwb::ByteOrder::Little);
        m_bigButton->setChecked(m_order == ksword::memwb::ByteOrder::Big);
        m_ptr32Button->setChecked(m_pointerWidth == 4U);
        m_ptr64Button->setChecked(m_pointerWidth == 8U);
    }

    // 打开设置：默认 QSettings 或指定 INI。
    std::unique_ptr<QSettings> HexInspectorPanel::openSettings() const
    {
        if (m_settingsFile.isEmpty())
        {
            return std::make_unique<QSettings>();
        }
        return std::make_unique<QSettings>(m_settingsFile, QSettings::IniFormat);
    }

    // 读设置：先落到默认值，只有读取成功且值合法才采用，任何失败都退回默认（小端、8 字节）。
    void HexInspectorPanel::loadSettings()
    {
        m_order = ksword::memwb::ByteOrder::Little;
        m_pointerWidth = 8U;

        const std::unique_ptr<QSettings> settings = openSettings();
        if (settings->status() != QSettings::NoError)
        {
            return;
        }

        // 字节序：只认 1（大端），其余（缺失、0、乱写）都是默认小端。
        bool parsed = false;
        const int orderValue = settings->value(ByteOrderKey()).toInt(&parsed);
        if (parsed && orderValue == 1)
        {
            m_order = ksword::memwb::ByteOrder::Big;
        }

        // 指针宽度：只认 4，其余都是默认 8。
        parsed = false;
        const int widthValue = settings->value(PointerWidthKey()).toInt(&parsed);
        if (parsed && widthValue == 4)
        {
            m_pointerWidth = 4U;
        }
    }

    // 写设置：写失败（只读位置、没有组织名）静默忽略，下次启动退回默认即可，不影响本次使用。
    void HexInspectorPanel::saveSettings()
    {
        const std::unique_ptr<QSettings> settings = openSettings();
        settings->setValue(ByteOrderKey(), m_order == ksword::memwb::ByteOrder::Big ? 1 : 0);
        settings->setValue(PointerWidthKey(), static_cast<int>(m_pointerWidth));
        settings->sync();
    }

    // 刷新：重读窗口，变化了才重建行；插入点移动时放弃正在进行的编辑并清掉旧消息。
    void HexInspectorPanel::refresh(bool force)
    {
        const WindowSnapshot snapshot = collectWindow();
        const bool changed = force || !m_hasBuilt || !(snapshot == m_window);
        const bool moved = m_hasBuilt && (snapshot.address != m_window.address || snapshot.hasData != m_window.hasData);
        m_window = snapshot;

        if (moved)
        {
            // 编辑属于旧插入点；消息（上一次编辑的结果、复制结果）也只对旧位置有意义。
            if (m_rows->isEditing())
            {
                m_rows->endEdit();
            }
            m_editRow = -1;
            m_status->clearMessage();
        }
        if (!changed)
        {
            return;
        }

        m_hasBuilt = true;
        updateTopBar();
        m_rows->setRows(buildRows());
        if (!m_window.hasData)
        {
            m_status->setIdleHint(QStringLiteral("没有数据：先在左侧视图里载入内存或文件"));
        }
        else if (m_window.canEdit)
        {
            m_status->setIdleHint(QStringLiteral("双击整数、浮点或指针行可直接编辑；右键复制"));
        }
        else
        {
            m_status->setIdleHint(QStringLiteral("当前为只读视图，不能编辑；右键复制"));
        }
    }

    // 更新顶栏：插入点地址与选区提示。
    void HexInspectorPanel::updateTopBar()
    {
        if (!m_window.hasData)
        {
            m_topBar->setAddressText(QStringLiteral("—"));
            m_topBar->setHintText(QStringLiteral("没有数据"));
            return;
        }

        // 地址位数：32 位范围内补到 8 位，否则 16 位，与画布地址列的习惯一致。
        const int digits = (m_window.address > 0xFFFFFFFFULL) ? 16 : 8;
        const QString hexDigits = QString::number(m_window.address, 16).toUpper().rightJustified(digits, QLatin1Char('0'));
        m_topBar->setAddressText(QStringLiteral("0x") + hexDigits);
        if (m_window.selectionBytes > 1ULL)
        {
            m_topBar->setHintText(QStringLiteral("已选 %1 字节").arg(m_window.selectionBytes));
        }
        else
        {
            m_topBar->setHintText(QString());
        }
    }
}
