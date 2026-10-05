// HexCanvas.Menu.cpp
// 作用：HexCanvas 的剪贴板复制与右键菜单。
//
// 复制的是"屏幕上看到的值"（含暂存补丁）。选区里只要有未加载/不可读字节就整体拒绝并发
// copyRejected：绝不拿 ?? 或 00 去充数（不变式：不伪造零字节）。
//
// 右键菜单显式使用不透明的主题静态色样式（背景、文字、选中态、禁用态），
// 不依赖默认样式——默认样式在某些宿主页面会继承透明背景，浅色模式下出现黑底黑字。

#include "HexCanvas.h"
#include "HexCanvasFormat.h"

#include "../../theme.h"

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>

#include <functional>
#include <limits>

namespace ks::ui
{
    // 取选区的"所见值"。
    // 传入：结果缓冲与原因输出；传出：是否成功。失败原因可直接给用户看。
    bool HexCanvas::selectedBytes(QByteArray* bytesOut, QString* reasonOut) const
    {
        const auto fail = [reasonOut](const QString& reason) {
            if (reasonOut != nullptr)
            {
                *reasonOut = reason;
            }
            return false;
        };

        const std::optional<AddressRange> range = selectedRange();
        if (!range.has_value())
        {
            return fail(QStringLiteral("没有选区"));
        }

        // 先判上限再分配：span + 1 溢出（整个 64 位空间）与超过上限都在这里被挡住。
        const std::uint64_t span = range->last - range->first;
        if (span >= kMaxCopyBytes)
        {
            return fail(QStringLiteral("选区过大，一次最多复制 16 MiB"));
        }

        // 逐字节取所见值，同时统计没有值的字节数。
        QByteArray collected;
        collected.reserve(static_cast<qsizetype>(span + 1ULL));
        std::uint64_t missing = 0;
        for (std::uint64_t address = range->first;; ++address)
        {
            const CellCore core = resolveCore(address);
            if (!core.hasValue)
            {
                ++missing;
            }
            collected.append(static_cast<char>(core.value));
            if (address == range->last)
            {
                break;
            }
        }
        if (missing != 0)
        {
            return fail(QStringLiteral("选区内有 %1 个字节尚未加载或不可读，无法复制").arg(missing));
        }
        if (bytesOut != nullptr)
        {
            *bytesOut = collected;
        }
        return true;
    }

    // 按格式取选区文本。
    // 传入：格式与原因输出；传出：文本，失败返回空串。
    QString HexCanvas::selectionText(CopyFormat format, QString* reasonOut) const
    {
        // 仅地址不需要字节内容：取选区起点，位数与地址列一致。
        if (format == CopyFormat::AddressOnly)
        {
            const std::optional<AddressRange> range = selectedRange();
            if (!range.has_value())
            {
                if (reasonOut != nullptr)
                {
                    *reasonOut = QStringLiteral("没有选区");
                }
                return QString();
            }
            return hexcanvas_format::FormatAddress(range->first, addressDigits());
        }

        QByteArray bytes;
        if (!selectedBytes(&bytes, reasonOut))
        {
            return QString();
        }
        switch (format)
        {
        case CopyFormat::HexText:
            return hexcanvas_format::FormatHexText(bytes);
        case CopyFormat::AsciiText:
            return hexcanvas_format::FormatAsciiText(bytes);
        case CopyFormat::CArray:
            return hexcanvas_format::FormatCArray(bytes);
        case CopyFormat::PythonBytes:
            return hexcanvas_format::FormatPythonBytes(bytes);
        case CopyFormat::EscapedString:
            return hexcanvas_format::FormatEscapedString(bytes);
        case CopyFormat::AddressOnly:
            break;
        }
        return QString();
    }

    // 把选区按格式复制到剪贴板。
    // 传出：false 表示被拒绝，已发 copyRejected。
    bool HexCanvas::copySelection(CopyFormat format)
    {
        QString reason;
        const QString text = selectionText(format, &reason);
        if (text.isEmpty())
        {
            if (!reason.isEmpty())
            {
                emit copyRejected(reason);
            }
            return false;
        }
        QGuiApplication::clipboard()->setText(text);
        return true;
    }

    // 复制活动面板格式（Ctrl+C）。
    void HexCanvas::copyCurrentPane()
    {
        copySelection(m_viewport.Pane() == ActivePane::Hex ? CopyFormat::HexText : CopyFormat::AsciiText);
    }

    // 复制另一个面板的格式（Ctrl+Shift+C）。
    void HexCanvas::copyOtherPane()
    {
        copySelection(m_viewport.Pane() == ActivePane::Hex ? CopyFormat::AsciiText : CopyFormat::HexText);
    }

    // 右键菜单的样式表：背景、文字、选中态、禁用态、分隔线全部用主题静态色。
    // 菜单每次弹出前新建，所以此刻取到的颜色就是当前主题。
    QString HexCanvas::menuStyleSheet() const
    {
        return QStringLiteral(
            "QMenu{background-color:%1;color:%2;border:1px solid %3;padding:3px;}"
            "QMenu::item{color:%2;background-color:transparent;padding:5px 20px 5px 28px;}"
            "QMenu::item:selected{background-color:%4;color:%5;}"
            "QMenu::item:disabled{color:%6;background-color:transparent;}"
            "QMenu::separator{height:1px;background-color:%3;margin:3px 6px;}")
            .arg(KswordTheme::SurfaceColorHex())
            .arg(KswordTheme::TextPrimaryColorHex())
            .arg(KswordTheme::BorderColorHex())
            .arg(KswordTheme::ThemeColorName(KswordTheme::PrimaryAccentColor()))
            .arg(KswordTheme::OnAccentHex())
            .arg(KswordTheme::TextDisabledColorHex());
    }

    // 构造右键菜单。
    // 传入：菜单针对的地址、该处是否有字节。传出：新建菜单（父对象为本控件，调用方 exec 后释放）。
    // 内置项构造完毕后发 contextMenuAboutToShow，宿主在槽里追加书签/补丁等项。
    QMenu* HexCanvas::buildContextMenu(std::uint64_t address, bool hasByte)
    {
        QMenu* menu = new QMenu(this);

        // 不透明样式：显式关闭透明背景，铺满背景，并开启悬停提示（QAction 的 toolTip 默认不显示）。
        menu->setAttribute(Qt::WA_TranslucentBackground, false);
        menu->setAutoFillBackground(true);
        menu->setStyleSheet(menuStyleSheet());
        menu->setToolTipsVisible(true);

        const bool hasSelection = selectedRange().has_value();
        const bool canEdit = m_editable && m_overlay != nullptr && hasSelection;

        // addItem：加一个带图标、悬停提示与启用状态的菜单项，触发时执行 handler。
        const auto addItem = [this, menu](
            const QString& iconPath,
            const QString& text,
            const QString& tip,
            bool enabled,
            const std::function<void()>& handler) {
            QAction* action = menu->addAction(QIcon(iconPath), text);
            action->setToolTip(tip);
            action->setEnabled(enabled);
            connect(action, &QAction::triggered, this, [handler]() { handler(); });
        };

        const QString copyIcon = QStringLiteral(":/Icon/codeeditor_copy.svg");
        addItem(
            copyIcon,
            QStringLiteral("复制十六进制"),
            QStringLiteral("把选区复制为大写、空格分隔的十六进制文本"),
            hasSelection,
            [this]() { copySelection(CopyFormat::HexText); });
        addItem(
            copyIcon,
            QStringLiteral("复制 ASCII 文本"),
            QStringLiteral("把选区复制为 ASCII 文本，不可见字节用点号表示"),
            hasSelection,
            [this]() { copySelection(CopyFormat::AsciiText); });
        addItem(
            copyIcon,
            QStringLiteral("复制为 C 数组"),
            QStringLiteral("把选区复制为 C 数组初始化列表，例如 { 0x41, 0x42 }"),
            hasSelection,
            [this]() { copySelection(CopyFormat::CArray); });
        addItem(
            copyIcon,
            QStringLiteral("复制为 Python bytes"),
            QStringLiteral("把选区复制为 Python bytes 字面量，每个字节都用 \\xNN 转义"),
            hasSelection,
            [this]() { copySelection(CopyFormat::PythonBytes); });
        addItem(
            copyIcon,
            QStringLiteral("复制为转义字符串"),
            QStringLiteral("把选区复制为 C 风格转义字符串，可见字符原样保留"),
            hasSelection,
            [this]() { copySelection(CopyFormat::EscapedString); });
        addItem(
            copyIcon,
            QStringLiteral("复制地址"),
            QStringLiteral("只复制选区起点的地址"),
            hasSelection,
            [this]() { copySelection(CopyFormat::AddressOnly); });

        menu->addSeparator();

        // 粘贴：需要可编辑且剪贴板里有文本。
        const bool clipboardHasText = !QGuiApplication::clipboard()->text().isEmpty();
        addItem(
            QStringLiteral(":/Icon/codeeditor_paste.svg"),
            QStringLiteral("粘贴"),
            QStringLiteral("把剪贴板内容粘贴到选区起点；十六进制面板按十六进制文本解析，ASCII 面板按文本原样写入"),
            canEdit && clipboardHasText,
            [this]() { pasteFromClipboard(); });

        // 三种填充：直接作用于选区，不弹对话框。
        // 图标：斜线填充图案（design/background_line.svg，Ksword5.qrc 里已有别名 settings_background_reset.svg），
        // 表达"用同一个值铺满"。原先借用的循环箭头（codeeditor_replace）语义是"刷新/替换"，与填充无关。
        // 更贴切的是油漆桶 editor/paint_line.svg，但它在主 qrc 里还没有别名；本组文件不得改 qrc，
        // 主会话若加了别名（例如 hexcanvas_fill.svg），只需改这一处常量。
        const QString fillIcon = QStringLiteral(":/Icon/settings_background_reset.svg");
        addItem(
            fillIcon,
            QStringLiteral("填充 00"),
            QStringLiteral("把选区的全部字节暂存为 00"),
            canEdit,
            [this]() { fillSelection(0x00); });
        addItem(
            fillIcon,
            QStringLiteral("填充 FF"),
            QStringLiteral("把选区的全部字节暂存为 FF"),
            canEdit,
            [this]() { fillSelection(0xFF); });
        addItem(
            fillIcon,
            QStringLiteral("NOP 填充（0x90）"),
            QStringLiteral("把选区的全部字节暂存为 0x90（x86 的 NOP 指令）"),
            canEdit,
            [this]() { fillSelection(0x90); });

        // 内置项完毕，让宿主追加自己的项。
        emit contextMenuAboutToShow(menu, address, hasByte);
        return menu;
    }
}
