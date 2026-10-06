// memwb_ui_common.cpp
// 作用：夹具公共设施的实现，见 memwb_ui_common.h。

#include "memwb_ui_common.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

#include <iostream>

namespace memwb_test
{
    int g_checks = 0;
    int g_failures = 0;

    // 记录一条断言。
    void Report(bool ok, const char* expression, const char* file, int line, const QString& note)
    {
        ++g_checks;
        if (ok)
        {
            return;
        }
        ++g_failures;
        std::cerr << "FAIL: " << expression << "  (" << file << ":" << line << ")";
        if (!note.isEmpty())
        {
            std::cerr << "  " << note.toStdString();
        }
        std::cerr << std::endl;
    }

    // 切换主题：设置 KswordTheme 的深浅标志，并按主题静态色构造完整调色板。
    // 完整调色板（含 PlaceholderText/AlternateBase/disabled 角色）避免默认浅色调色板造成错误的深色截图。
    void ApplyTheme(bool dark)
    {
        KswordTheme::SetDarkModeEnabled(dark);
        qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

        QPalette palette;
        palette.setColor(QPalette::Window, KswordTheme::WindowColor());
        palette.setColor(QPalette::WindowText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Base, KswordTheme::SurfaceColor());
        palette.setColor(QPalette::AlternateBase, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::Text, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Button, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::ButtonText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::PlaceholderText, KswordTheme::TextSecondaryColor());
        palette.setColor(QPalette::Highlight, KswordTheme::PrimaryAccentColor());
        palette.setColor(QPalette::HighlightedText, KswordTheme::OnAccentColor());
        palette.setColor(QPalette::Mid, KswordTheme::BorderColor());
        palette.setColor(QPalette::Midlight, KswordTheme::BorderStrongColor());
        palette.setColor(QPalette::Dark, KswordTheme::PaletteDarkColor());
        palette.setColor(QPalette::ToolTipBase, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::ToolTipText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Disabled, QPalette::Text, KswordTheme::TextDisabledColor());
        palette.setColor(QPalette::Disabled, QPalette::WindowText, KswordTheme::TextDisabledColor());
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, KswordTheme::TextDisabledColor());
        qApp->setPalette(palette);
    }

    // 生成确定性测试字节。
    QByteArray MakePattern(int size, int seed)
    {
        QByteArray bytes(size, '\0');
        for (int index = 0; index < size; ++index)
        {
            // 乘一个奇数再加偏移：覆盖全部字节值，且相邻字节明显不同；避开 0x00 方便测编辑差异。
            const int value = (index * 37 + 11 + seed * 53) & 0xFF;
            bytes[index] = static_cast<char>(value == 0 ? 0x5A : value);
        }
        return bytes;
    }

    // 构造夹具并显示。
    std::unique_ptr<Fixture> MakeStaticFixture(
        std::uint64_t base,
        const QByteArray& data,
        bool editable,
        const QSize& size,
        bool loadBaseline)
    {
        auto fixture = std::make_unique<Fixture>();
        fixture->base = base;
        fixture->data = data;
        fixture->canvas = std::make_unique<ks::ui::HexCanvas>();

        // 先决定窗口大小与显示，再装数据：这样首屏的页请求按真实视口规划。
        fixture->canvas->resize(size);
        fixture->canvas->show();
        fixture->canvas->setEditable(editable);
        fixture->canvas->setOverlay(&fixture->overlay);
        fixture->canvas->setStaticData(base, data);

        // 叠加层基线：与静态数据逐字节一致，掩码全 1。
        if (loadBaseline)
        {
            const std::vector<std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(data.constData()),
                reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size());
            const std::vector<std::uint8_t> mask(bytes.size(), 1);
            fixture->overlay.LoadBaseline(QStringLiteral("memwb-test").toStdString(), base, bytes, mask);
        }
        return fixture;
    }

    // 记录页请求。
    void RecordingProvider::RequestPages(const std::vector<ks::ui::HexFetchRange>& ranges, std::uint64_t sourceRevision)
    {
        Request request;
        request.ranges = ranges;
        request.revision = sourceRevision;
        requests.push_back(request);
        if (onRequest)
        {
            onRequest(requests.back());
        }
    }

    // 累计页数。
    std::uint64_t RecordingProvider::totalPages() const
    {
        std::uint64_t total = 0;
        for (const Request& request : requests)
        {
            for (const ks::ui::HexFetchRange& range : request.ranges)
            {
                total += range.pageCount;
            }
        }
        return total;
    }

    // 单元格中心。
    QPoint CellCenter(ks::ui::HexCanvas& canvas, std::uint64_t address, ks::ui::HexCanvas::ActivePane pane)
    {
        return canvas.cellRect(address, pane).center();
    }

    // 点击单元格。
    void Click(
        ks::ui::HexCanvas& canvas,
        std::uint64_t address,
        ks::ui::HexCanvas::ActivePane pane,
        Qt::KeyboardModifiers modifiers)
    {
        QTest::mouseClick(canvas.viewport(), Qt::LeftButton, modifiers, CellCenter(canvas, address, pane));
    }

    // 按住左键移动鼠标：构造带 LeftButton 状态的 MouseMove 并直接投递给视口。
    void DragMove(QWidget* viewportWidget, const QPoint& pos)
    {
        QMouseEvent move(
            QEvent::MouseMove,
            QPointF(pos),
            QPointF(viewportWidget->mapToGlobal(pos)),
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(viewportWidget, &move);
    }

    // 发送一次按键。
    void Key(ks::ui::HexCanvas& canvas, Qt::Key key, Qt::KeyboardModifiers modifiers)
    {
        QTest::keyClick(&canvas, key, modifiers);
    }

    // 发送文字按键：每个字符一次 keyClick，带 text 字段。
    void Type(ks::ui::HexCanvas& canvas, const QString& text)
    {
        for (const QChar ch : text)
        {
            QTest::keyClick(&canvas, ch.toLatin1());
        }
    }

    // 抓图。
    QImage GrabImage(ks::ui::HexCanvas& canvas)
    {
        return canvas.viewport()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
    }

    // 颜色差之和。
    int ColorDistance(const QColor& left, const QColor& right)
    {
        return qAbs(left.red() - right.red()) + qAbs(left.green() - right.green()) + qAbs(left.blue() - right.blue());
    }

    // 颜色是否足够接近。
    bool ColorsClose(const QColor& left, const QColor& right, int tolerance)
    {
        return ColorDistance(left, right) <= tolerance;
    }
}
