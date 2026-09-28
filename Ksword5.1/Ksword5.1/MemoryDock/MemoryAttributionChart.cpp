#include "MemoryAttributionChart.h"
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <algorithm>
#include <numeric>

namespace {
QString bytesText(quint64 bytes)
{
    return bytes >= (1ULL << 30) ? QStringLiteral("%1 GiB").arg(static_cast<double>(bytes) / (1ULL << 30), 0, 'f', 2)
        : QStringLiteral("%1 MiB").arg(static_cast<double>(bytes) / (1ULL << 20), 0, 'f', 1);
}
QColor segmentColor(const QPalette& palette, int key)
{
    if (key < 0) { return palette.color(QPalette::Mid); }
    const QColor accent = palette.color(QPalette::Highlight);
    const int hue = (std::max(0, accent.hslHue()) + key * 47) % 360;
    return QColor::fromHsl(hue, 145, palette.color(QPalette::Window).lightness() < 128 ? 160 : 112);
}
}
MemoryAttributionChart::MemoryAttributionChart(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(202);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}
void MemoryAttributionChart::setSegments(std::vector<Segment> segments, QString title)
{
    // A queued mouse event may arrive before the deferred repaint. Old hit
    // indices must not be used with the replacement segment vector.
    m_hits.clear();
    m_segments = std::move(segments);
    m_title = std::move(title);
    std::stable_sort(m_segments.begin(), m_segments.end(), [](const auto& a, const auto& b) { return a.bytes > b.bytes; });
    update();
}
void MemoryAttributionChart::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(QRect(8, 0, width() - 16, 28), Qt::AlignVCenter, m_title);
    const auto total = std::accumulate(m_segments.begin(), m_segments.end(), quint64{0}, [](quint64 sum, const auto& row) { return sum + row.bytes; });
    m_hits.clear();
    const qreal chartWidth = std::max(1, width() - 16);
    qreal x = 8;
    if (!total) {
        painter.fillRect(QRectF(8, 32, chartWidth, 24), palette().color(QPalette::AlternateBase));
        return;
    }
    for (std::size_t i = 0; i < m_segments.size(); ++i) {
        const auto& row = m_segments[i];
        if (!row.bytes) { continue; }
        const qreal span = chartWidth * static_cast<qreal>(row.bytes) / static_cast<qreal>(total);
        const QRectF box(x, 32, span, 24);
        painter.fillRect(box, segmentColor(palette(), row.key));
        m_hits.emplace_back(box, i);
        x += span;
    }
    const bool columns = width() >= 680;
    const int limit = columns ? 8 : 4;
    const qreal cellWidth = columns ? chartWidth / 2 : chartWidth;
    const auto largest = std::max<quint64>(1, m_segments.front().bytes);
    for (int i = 0; i < limit && static_cast<std::size_t>(i) < m_segments.size(); ++i) {
        const auto& row = m_segments[static_cast<std::size_t>(i)];
        const qreal left = 8 + (columns ? (i % 2) * cellWidth : 0);
        const qreal top = 66 + (columns ? i / 2 : i) * 32;
        const QRectF box(left, top, cellWidth - 12, 28);
        painter.setPen(palette().color(QPalette::Text));
        const QString amount = QStringLiteral("%1  %2%").arg(bytesText(row.bytes)).arg(100.0 * static_cast<double>(row.bytes) / static_cast<double>(total), 0, 'f', 1);
        const int amountWidth = painter.fontMetrics().horizontalAdvance(amount) + 12;
        painter.drawText(box.adjusted(0, 0, -amountWidth, -9), Qt::AlignLeft | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(row.label, Qt::ElideRight, std::max(0, static_cast<int>(box.width()) - amountWidth)));
        painter.drawText(box.adjusted(0, 0, 0, -9), Qt::AlignRight | Qt::AlignVCenter, amount);
        painter.fillRect(QRectF(left, top + 23, box.width(), 4), palette().color(QPalette::AlternateBase));
        painter.fillRect(QRectF(left, top + 23, box.width() * static_cast<qreal>(row.bytes) / static_cast<qreal>(largest), 4), segmentColor(palette(), row.key));
        m_hits.emplace_back(box, static_cast<std::size_t>(i));
    }
}
void MemoryAttributionChart::mouseMoveEvent(QMouseEvent* event)
{
    for (const auto& hit : m_hits) {
        if (!hit.first.contains(event->position())) { continue; }
        const auto& row = m_segments[hit.second];
        QToolTip::showText(event->globalPosition().toPoint(), row.label + QStringLiteral("  ") + bytesText(row.bytes), this);
        setCursor(Qt::PointingHandCursor);
        return;
    }
    unsetCursor();
    QToolTip::hideText();
}
void MemoryAttributionChart::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) { return; }
    for (const auto& hit : m_hits) {
        if (hit.first.contains(event->position()) && selected) { selected(m_segments[hit.second].key); return; }
    }
}
