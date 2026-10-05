// HexInspectorPanel.Menu.cpp
// 作用：数据解释器面板的复制与右键菜单。
//
// 右键菜单每次弹出前新建，显式使用不透明的主题静态色样式（背景、文字、选中态、禁用态、分隔线），
// 不依赖默认样式——默认样式在某些宿主页面会继承透明背景，浅色模式下出现黑底黑字。
// 菜单项的图标：复制类复用主程序 Ksword5.qrc 已有的 :/Icon/codeeditor_copy.svg；
// "编辑"项没有现成图标，用 QPainter 按当前主题色现画一支铅笔（菜单每次新建，所以颜色就是当前主题）。

#include "HexInspectorPanel.h"
#include "HexInspectorWidgets.h"

#include "../../theme.h"

#include <QAction>
#include <QClipboard>
#include <QColor>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>

#include <functional>

namespace ks::ui
{
    namespace
    {
        // MakePencilPixmap：画一支 16x16（逻辑像素）的铅笔。
        // 传入：颜色；传出：带 2 倍分辨率的位图，保证高分屏上不糊。
        QPixmap MakePencilPixmap(const QColor& color)
        {
            const int scale = 2;
            QPixmap pixmap(16 * scale, 16 * scale);
            pixmap.setDevicePixelRatio(static_cast<qreal>(scale));
            pixmap.fill(Qt::transparent);

            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing, true);
            QPen pen(color, 1.2);
            pen.setJoinStyle(Qt::RoundJoin);
            pen.setCapStyle(Qt::RoundCap);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);

            // 笔身：从左下笔尖斜向右上，再用一条短线隔出笔帽。
            QPainterPath body;
            body.moveTo(2.5, 13.5);
            body.lineTo(3.4, 10.2);
            body.lineTo(10.8, 2.8);
            body.lineTo(13.2, 5.2);
            body.lineTo(5.8, 12.6);
            body.closeSubpath();
            painter.drawPath(body);
            painter.drawLine(QPointF(9.2, 4.4), QPointF(11.6, 6.8));
            return pixmap;
        }

        // MakePencilIcon：给编辑项生成"普通/禁用"两态图标，颜色此刻现取。
        QIcon MakePencilIcon()
        {
            QIcon icon;
            icon.addPixmap(MakePencilPixmap(KswordTheme::TextPrimaryColor()), QIcon::Normal);
            icon.addPixmap(MakePencilPixmap(KswordTheme::TextDisabledColor()), QIcon::Disabled);
            return icon;
        }

        // ElideForMessage：消息里嵌入的值太长时截断加省略号，避免状态条被撑满（全文在悬停提示里）。
        QString ElideForMessage(const QString& text)
        {
            const int limit = 48;
            if (text.size() <= limit)
            {
                return text;
            }
            return text.left(limit) + QStringLiteral("…");
        }
    }

    // 复制请求：值 / 十六进制 / 整行；不可用行没有内容可复制，给出原因而不是复制空串。
    void HexInspectorPanel::onCopyRequested(int row, int kind)
    {
        if (row < 0 || row >= m_rows->rowCount())
        {
            return;
        }
        const HexInspectorRowData& rowData = m_rows->rowAt(row);
        if (!rowData.available)
        {
            m_status->setMessage(HexInspectorStatusBar::Kind::Error, QStringLiteral("该行当前不可用，没有可复制的内容"));
            return;
        }

        // 按种类取文本与消息里的称呼。
        QString text;
        QString what;
        switch (static_cast<HexInspectorRowView::CopyKind>(kind))
        {
        case HexInspectorRowView::CopyKind::Value:
            text = rowData.valueCopy;
            what = QStringLiteral("值");
            break;
        case HexInspectorRowView::CopyKind::Hex:
            text = rowData.hexCopy;
            what = QStringLiteral("十六进制");
            break;
        case HexInspectorRowView::CopyKind::Row:
            text = rowData.typeName + QLatin1Char('\t') + rowData.valueCopy + QLatin1Char('\t') + rowData.hexCopy;
            what = QStringLiteral("整行");
            break;
        }
        if (text.isEmpty())
        {
            m_status->setMessage(HexInspectorStatusBar::Kind::Error, QStringLiteral("该行没有可复制的内容"));
            return;
        }
        QGuiApplication::clipboard()->setText(text);
        m_status->setMessage(
            HexInspectorStatusBar::Kind::Info,
            QStringLiteral("已复制 %1 的%2：%3").arg(rowData.typeName).arg(what).arg(ElideForMessage(text)));
    }

    // 右键请求：构造菜单并非阻塞弹出（关闭后自动销毁）。点在空白处没有菜单。
    void HexInspectorPanel::onContextMenuRequested(int row, const QPoint& globalPos)
    {
        QMenu* menu = buildRowMenu(row);
        if (menu == nullptr)
        {
            return;
        }
        menu->setAttribute(Qt::WA_DeleteOnClose, true);
        menu->popup(globalPos);
    }

    // 菜单样式：全部用主题静态色；菜单每次新建，此刻取到的颜色就是当前主题。
    QString HexInspectorPanel::menuStyleSheet() const
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

    // 构造某行的右键菜单。
    // 传入：行号；传出：新建菜单（父对象为本面板，调用方负责弹出与释放），行号越界返回空指针。
    QMenu* HexInspectorPanel::buildRowMenu(int row)
    {
        if (row < 0 || row >= m_rows->rowCount())
        {
            return nullptr;
        }
        const HexInspectorRowData& rowData = m_rows->rowAt(row);

        // 不透明样式：显式关闭透明背景、铺满背景、开启悬停提示（QAction 的 toolTip 默认不显示）。
        QMenu* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_TranslucentBackground, false);
        menu->setAutoFillBackground(true);
        menu->setStyleSheet(menuStyleSheet());
        menu->setToolTipsVisible(true);

        // addItem：加一个带图标、悬停提示与启用状态的菜单项，触发时执行 handler。
        const auto addItem = [this, menu](
            const QIcon& icon,
            const QString& text,
            const QString& tip,
            bool enabled,
            const std::function<void()>& handler) {
            QAction* action = menu->addAction(icon, text);
            action->setToolTip(tip);
            action->setEnabled(enabled);
            connect(action, &QAction::triggered, this, [handler]() { handler(); });
        };

        // 三个复制项：不可用行整体置灰，提示里说明原因。
        const QIcon copyIcon(QStringLiteral(":/Icon/codeeditor_copy.svg"));
        const QString unavailableTip = QStringLiteral("该行当前不可用，没有可复制的内容");
        addItem(
            copyIcon,
            QStringLiteral("复制值"),
            rowData.available ? QStringLiteral("复制该行的值；指针只复制十六进制地址，不含括号里的描述") : unavailableTip,
            rowData.available,
            [this, row]() { onCopyRequested(row, static_cast<int>(HexInspectorRowView::CopyKind::Value)); });
        addItem(
            copyIcon,
            QStringLiteral("复制十六进制"),
            rowData.available ? QStringLiteral("复制该值的十六进制位模式；字符串与 GUID 复制全部原始字节") : unavailableTip,
            rowData.available,
            [this, row]() { onCopyRequested(row, static_cast<int>(HexInspectorRowView::CopyKind::Hex)); });
        addItem(
            copyIcon,
            QStringLiteral("复制整行"),
            rowData.available ? QStringLiteral("复制类型、值与十六进制，三者用制表符分隔") : unavailableTip,
            rowData.available,
            [this, row]() { onCopyRequested(row, static_cast<int>(HexInspectorRowView::CopyKind::Row)); });

        menu->addSeparator();

        // 编辑项：不能编辑时置灰，提示里直接写明原因（只读视图、只读类型、字节不足）。
        const QString blocked = editBlockedReason(row);
        addItem(
            MakePencilIcon(),
            QStringLiteral("编辑此值"),
            blocked.isEmpty() ? QStringLiteral("在行内编辑这个值，Enter 暂存，Esc 取消") : blocked,
            blocked.isEmpty(),
            [this, row]() { beginEditRow(row); });
        return menu;
    }
}
