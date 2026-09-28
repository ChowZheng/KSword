#pragma once
#include <QWidget>
#include <QString>
#include <QRectF>
#include <functional>
#include <vector>

class MemoryAttributionChart final : public QWidget {
public:
    struct Segment { QString label; quint64 bytes = 0; int key = -1; };
    explicit MemoryAttributionChart(QWidget* parent = nullptr);
    void setSegments(std::vector<Segment> segments, QString title);
    std::function<void(int)> selected;
protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
private:
    std::vector<Segment> m_segments;
    std::vector<std::pair<QRectF, std::size_t>> m_hits;
    QString m_title;
};
