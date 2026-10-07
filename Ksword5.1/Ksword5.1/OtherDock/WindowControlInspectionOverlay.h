#pragma once
#include "WindowControlInspection.h"
class QWidget;

namespace ks::control_inspection
{
    QWidget* CreateOverlay(QWidget* owner);
    void FitOverlay(QWidget* overlay);
    bool ClipOverlay(QWidget* overlay, HWND root, HWND excluded);
    void UpdateOverlay(QWidget* overlay, const QVector<Node>& nodes,
        const Node& hovered, const Node& selected, bool all, bool tips, QPoint physicalCursor);
}
