#include "NtfsSpaceMapWidget.h"

#include "../Internationalization/LanguageManager.h"
#include "../UI/PrimaryPageStyle.h"
#include "../theme.h"

#include <QCoreApplication>
#include <QEvent>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QToolTip>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
    constexpr qsizetype kMaxTiles = 20000;

    QString displayBytes(long double bytes)
    {
        static const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
        int unit = 0;
        while (bytes >= 1024.0L && unit < 6)
        {
            bytes /= 1024.0L;
            ++unit;
        }
        return QStringLiteral("%1 %2")
            .arg(QLocale().toString(static_cast<double>(bytes), 'f', unit ? 2 : 0),
                QString::fromLatin1(units[unit]));
    }

    QColor itemColor(const NtfsSpaceMapWidget::Item& item)
    {
        // Fixed extension identity across Qt hash seeds and refreshes. The lightness
        // follows the current theme; text is calibrated against this actual fill.
        quint32 hash = 2166136261U;
        for (const QChar character : item.extension.toCaseFolded())
        {
            hash ^= character.unicode();
            hash *= 16777619U;
        }
        if (item.directory)
            return KswordTheme::PrimaryBlueColor;
        return QColor::fromHsl(static_cast<int>(hash % 360U), 130,
            KswordTheme::IsDarkModeEnabled() ? 92 : 180);
    }
}

NtfsSpaceMapWidget::NtfsSpaceMapWidget(QWidget* parent)
    : QWidget(parent), m_data(prepare({}))
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(100, 100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ks::ui::StylePrimaryContentPanel(this, false);
}

NtfsSpaceMapWidget::DataPtr NtfsSpaceMapWidget::prepare(QVector<Item> items)
{
    auto* data = new Data;
    std::unique_ptr<Data> guard(data);
    data->items = std::move(items);
    data->items.erase(std::remove_if(data->items.begin(), data->items.end(),
        [](const Item& item) { return item.bytes == 0; }), data->items.end());
    data->prefixWeights.reserve(data->items.size() + 1);
    data->prefixWeights.append(0.0L);
    for (const Item& item : std::as_const(data->items))
        data->prefixWeights.append(data->prefixWeights.constLast() + static_cast<long double>(item.bytes));
    // A result may lose its final reference in any callback, including after
    // replacement or page closure. Do not destroy a million QStrings on the GUI.
    guard.release();
    return DataPtr(data, [](const Data* retired)
    {
        const auto* app = QCoreApplication::instance();
        if (app && !QCoreApplication::closingDown() && QThread::currentThread() == app->thread())
        {
            auto* task = QRunnable::create([retired]() { delete retired; });
            task->setAutoDelete(true);
            QThreadPool::globalInstance()->start(task);
        }
        else delete retired;
    });
}

void NtfsSpaceMapWidget::setItems(QVector<Item> items)
{
    setData(prepare(std::move(items)));
}

void NtfsSpaceMapWidget::setData(DataPtr data)
{
    m_data = data ? std::move(data) : prepare({});
    m_hover = -1;
    rebuildLayout();
    update();
}

void NtfsSpaceMapWidget::setSelectedReference(quint64 reference)
{
    m_selectedReference = reference;
    m_hasSelection = true;
    m_selectedTile = -1;
    for (qsizetype i = 0; i < m_tiles.size(); ++i)
    {
        const Tile& tile = m_tiles[i];
        if (tile.last == tile.first + 1 && m_data->items[tile.first].reference == reference)
        {
            m_selectedTile = static_cast<int>(i);
            break;
        }
    }
    update();
}

void NtfsSpaceMapWidget::setEmptyMessage(const QString& source)
{
    if (m_emptyMessage == source) return;
    m_emptyMessage = source;
    update();
}

void NtfsSpaceMapWidget::rebuildLayout()
{
    m_tiles.clear();
    m_selectedTile = -1;
    if (m_data->items.isEmpty() || width() < 2 || height() < 2) return;

    // Weighted binary partitions avoid skinny slice-and-dice bands and keep
    // all bytes represented. Once a region is subpixel or the tile budget is
    // exhausted, retain it as an explicit aggregate instead of dropping files.
    QVector<Tile> pending;
    pending.append({ QRectF(rect()).adjusted(4, 4, -4, -4), 0, m_data->items.size() });
    while (!pending.isEmpty())
    {
        const Tile region = pending.takeLast();
        if (region.last <= region.first || region.rectangle.isEmpty()) continue;
        const long double weight = m_data->prefixWeights[region.last] - m_data->prefixWeights[region.first];
        if (weight <= 0.0L) continue;
        if (region.last == region.first + 1 || region.rectangle.width() < 2.0
            || region.rectangle.height() < 2.0
            || region.rectangle.width() * region.rectangle.height() < 4.0
            || m_tiles.size() + pending.size() + 1 >= kMaxTiles)
        {
            if (region.last == region.first + 1 && m_hasSelection
                && m_data->items[region.first].reference == m_selectedReference)
                m_selectedTile = static_cast<int>(m_tiles.size());
            m_tiles.append(region);
            continue;
        }
        const long double middle = m_data->prefixWeights[region.first] + weight / 2.0L;
        auto iterator = std::lower_bound(m_data->prefixWeights.cbegin() + region.first + 1,
            m_data->prefixWeights.cbegin() + region.last, middle);
        qsizetype split = iterator - m_data->prefixWeights.cbegin();
        if (split > region.first + 1
            && std::abs(m_data->prefixWeights[split - 1] - middle) < std::abs(m_data->prefixWeights[split] - middle))
            --split;
        split = std::clamp(split, region.first + 1, region.last - 1);
        const qreal proportion = static_cast<qreal>(
            (m_data->prefixWeights[split] - m_data->prefixWeights[region.first]) / weight);
        QRectF first = region.rectangle;
        QRectF second = region.rectangle;
        if (first.width() >= first.height())
        {
            first.setWidth(first.width() * proportion);
            second.setLeft(first.right());
        }
        else
        {
            first.setHeight(first.height() * proportion);
            second.setTop(first.bottom());
        }
        pending.append({ second, split, region.last });
        pending.append({ first, region.first, split });
    }
}

void NtfsSpaceMapWidget::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setClipRect(rect().adjusted(4, 4, -4, -4));
    if (m_tiles.isEmpty())
    {
        painter.setPen(KswordTheme::TextSecondaryColor());
        painter.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap,
            ks::i18n::sourceText(m_emptyMessage.isEmpty()
                ? QStringLiteral("没有可显示的已知大小文件") : m_emptyMessage));
        return;
    }
    for (qsizetype i = 0; i < m_tiles.size(); ++i)
    {
        const Tile& tile = m_tiles[i];
        if (!tile.rectangle.intersects(event->rect())) continue;
        const bool aggregate = tile.last > tile.first + 1;
        const QColor fill = aggregate ? KswordTheme::SurfaceMutedColor() : itemColor(m_data->items[tile.first]);
        const QRectF box = tile.rectangle;
        painter.fillRect(box, fill);
        if (box.width() > 3.0 && box.height() > 3.0)
        {
            painter.setPen(QPen(KswordTheme::EnsureTextContrast(fill, fill, 1.4), 1.0));
            painter.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
        }
        if (static_cast<int>(i) == m_selectedTile || static_cast<int>(i) == m_hover)
        {
            painter.setPen(QPen(KswordTheme::EnsureTextContrast(
                KswordTheme::TextPrimaryColor(), fill, 3.0), static_cast<int>(i) == m_selectedTile ? 3.0 : 2.0));
            painter.drawRect(box.adjusted(1.5, 1.5, -1.5, -1.5));
        }
        const QFontMetrics metrics(font());
        if (box.width() < 50.0 || box.height() < metrics.height() + 6.0) continue;
        const QString name = aggregate
            ? ks::i18n::sourceText(QStringLiteral("%1 个较小项")).arg(tile.last - tile.first)
            : m_data->items[tile.first].name;
        painter.setPen(KswordTheme::EnsureTextContrast(KswordTheme::TextPrimaryColor(), fill));
        painter.drawText(box.adjusted(5, 3, -5, -3), Qt::AlignLeft | Qt::AlignTop,
            metrics.elidedText(name, Qt::ElideMiddle, static_cast<int>(box.width()) - 10));
        if (box.height() >= metrics.height() * 2 + 10)
            painter.drawText(box.adjusted(5, metrics.height() + 5, -5, -3), Qt::AlignLeft | Qt::AlignTop,
                displayBytes(m_data->prefixWeights[tile.last] - m_data->prefixWeights[tile.first]));
    }
}

void NtfsSpaceMapWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_hover = -1;
    rebuildLayout();
}

int NtfsSpaceMapWidget::hitTest(const QPointF& point) const
{
    for (qsizetype i = 0; i < m_tiles.size(); ++i)
        if (m_tiles[i].rectangle.contains(point)) return static_cast<int>(i);
    return -1;
}

void NtfsSpaceMapWidget::chooseTile(int tile)
{
    if (tile < 0 || tile >= m_tiles.size()) return;
    m_selectedTile = tile;
    const Tile& region = m_tiles[tile];
    m_hasSelection = region.last == region.first + 1;
    if (m_hasSelection)
    {
        m_selectedReference = m_data->items[region.first].reference;
        if (selected) selected(m_selectedReference);
    }
    update();
}

void NtfsSpaceMapWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        chooseTile(hitTest(event->position()));
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void NtfsSpaceMapWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        const int tile = hitTest(event->position());
        chooseTile(tile);
        if (tile >= 0 && m_tiles[tile].last == m_tiles[tile].first + 1 && activated)
            activated(m_data->items[m_tiles[tile].first].reference);
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void NtfsSpaceMapWidget::mouseMoveEvent(QMouseEvent* event)
{
    const int hovered = hitTest(event->position());
    if (hovered != m_hover)
    {
        m_hover = hovered;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void NtfsSpaceMapWidget::leaveEvent(QEvent* event)
{
    m_hover = -1;
    update();
    QWidget::leaveEvent(event);
}

QString NtfsSpaceMapWidget::tileToolTip(int tile) const
{
    if (tile < 0 || tile >= m_tiles.size()) return {};
    const Tile& region = m_tiles[tile];
    const QString amount = displayBytes(m_data->prefixWeights[region.last] - m_data->prefixWeights[region.first]);
    if (region.last > region.first + 1)
        return ks::i18n::sourceText(QStringLiteral("%1 个较小项，共 %2；请使用列表查看"))
            .arg(region.last - region.first).arg(amount);
    const Item& item = m_data->items[region.first];
    return QStringLiteral("%1\n%2").arg(item.path, amount);
}

bool NtfsSpaceMapWidget::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip)
    {
        auto* help = static_cast<QHelpEvent*>(event);
        const QString text = tileToolTip(hitTest(help->pos()));
        if (text.isEmpty()) QToolTip::hideText();
        else QToolTip::showText(help->globalPos(), text, this);
        return true;
    }
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        update();
    return QWidget::event(event);
}

void NtfsSpaceMapWidget::keyPressEvent(QKeyEvent* event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && m_selectedTile >= 0)
    {
        const Tile& tile = m_tiles[m_selectedTile];
        if (tile.last == tile.first + 1 && activated) activated(m_data->items[tile.first].reference);
        event->accept();
        return;
    }
    int dx = 0;
    int dy = 0;
    if (event->key() == Qt::Key_Left) dx = -1;
    if (event->key() == Qt::Key_Right) dx = 1;
    if (event->key() == Qt::Key_Up) dy = -1;
    if (event->key() == Qt::Key_Down) dy = 1;
    if ((dx || dy) && !m_tiles.isEmpty())
    {
        int candidate = 0;
        if (m_selectedTile >= 0)
        {
            candidate = m_selectedTile;
            const QPointF origin = m_tiles[m_selectedTile].rectangle.center();
            qreal best = std::numeric_limits<qreal>::max();
            for (qsizetype i = 0; i < m_tiles.size(); ++i)
            {
                const QPointF delta = m_tiles[i].rectangle.center() - origin;
                const qreal forward = delta.x() * dx + delta.y() * dy;
                if (forward <= 0.0) continue;
                const qreal sideways = std::abs(delta.x() * dy + delta.y() * dx);
                const qreal score = forward + 3.0 * sideways;
                if (score < best) { best = score; candidate = static_cast<int>(i); }
            }
        }
        chooseTile(candidate);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
