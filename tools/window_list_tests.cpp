#include "../Ksword5.1/Ksword5.1/OtherDock/WindowListInteraction.h"
#include <QApplication>
#include <QPointer>
#include <QTest>
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <memory>
#include <vector>

static void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static bool sameBounds(const RECT& left, const RECT& right, qreal dpiScale)
{
    // Qt converts native resize events through logical coordinates, rounding
    // by at most one logical pixel at fractional display scale factors.
    const int tolerance = static_cast<int>(std::ceil(dpiScale));
    return std::abs(left.left - right.left) <= tolerance
        && std::abs(left.top - right.top) <= tolerance
        && std::abs(left.right - right.right) <= tolerance
        && std::abs(left.bottom - right.bottom) <= tolerance;
}

static BOOL CALLBACK collectMonitorRects(HMONITOR, HDC, LPRECT bounds, LPARAM context)
{
    auto* rectangles = reinterpret_cast<QVector<QRect>*>(context);
    rectangles->push_back(QRect(bounds->left, bounds->top, bounds->right - bounds->left, bounds->bottom - bounds->top));
    return TRUE;
}

static void runTests()
{
    // The single HWND has one backing scale even when selected windows belong
    // to differently scaled monitors. All coordinates must round-trip in pixels.
    const QPoint virtualOrigin(-320, -120);
    const QList<QRect> mixedDpiTargets{
        QRect(-280, -100, 180, 80), QRect(30, -70, 140, 120), QRect(-60, -10, 100, 90)};
    for (const qreal backingScale : {1.0, 1.25, 1.5, 2.0})
    {
        QImage canvas(QSize(640, 240), QImage::Format_ARGB32_Premultiplied);
        canvas.setDevicePixelRatio(backingScale);
        canvas.fill(Qt::transparent);
        {
            QPainter painter(&canvas);
            for (const auto& physical : mixedDpiTargets)
            {
                const QRectF mapped = ks::window::physicalRectToOverlay(physical, virtualOrigin, backingScale);
                require(std::abs(mapped.left() * backingScale + virtualOrigin.x() - physical.left()) < 0.001
                    && std::abs(mapped.top() * backingScale + virtualOrigin.y() - physical.top()) < 0.001
                    && std::abs(mapped.width() * backingScale - physical.width()) < 0.001
                    && std::abs(mapped.height() * backingScale - physical.height()) < 0.001,
                    "Mixed DPI mapping preserves negative origins, cross-screen positions and dimensions");
                painter.fillRect(mapped, QColor(30, 120, 220));
            }
        }
        for (const auto& physical : mixedDpiTargets)
            require(canvas.pixelColor(physical.center() - virtualOrigin) == QColor(30, 120, 220),
                "100/125/150/200 percent backing stores paint targets at the same physical pixels");
        require(canvas.pixelColor(5, 5).alpha() == 0, "Scaling does not move a target into unrelated pixels");
    }

    QTreeWidget tree;
    tree.setColumnCount(2);
    tree.setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree.setSelectionBehavior(QAbstractItemView::SelectRows);
    auto* group = new QTreeWidgetItem(&tree, {"Process"});
    group->setData(0, Qt::UserRole + 1, true);
    QList<QTreeWidgetItem*> leaves;
    for (quint64 hwnd = 1; hwnd <= 4; ++hwnd)
    {
        auto* leaf = new QTreeWidgetItem(group, {QString::number(hwnd)});
        leaf->setData(0, Qt::UserRole, hwnd);
        leaves.push_back(leaf);
    }
    group->setExpanded(true);
    tree.resize(400, 250);
    tree.setAttribute(Qt::WA_ShowWithoutActivating);
    tree.show();
    QTest::qWait(30);
    const auto clickRow = [&](int row, Qt::KeyboardModifiers modifiers) {
        QTest::mouseClick(tree.viewport(), Qt::LeftButton, modifiers,
            tree.visualItemRect(leaves[row]).center());
    };
    clickRow(0, Qt::NoModifier);
    clickRow(2, Qt::ControlModifier);
    require(ks::window::selectedWindowHandles(&tree).size() == 2, "Ctrl multi-select");
    clickRow(0, Qt::NoModifier);
    clickRow(3, Qt::ShiftModifier);
    require(ks::window::selectedWindowHandles(&tree).size() == 4, "Shift range selection");
    ks::window::selectContextWindow(&tree, leaves[1]);
    require(ks::window::selectedWindowHandles(&tree).size() == 4, "Right-click preserves selection");
    tree.clearSelection();
    leaves[0]->setSelected(true);
    ks::window::selectContextWindow(&tree, leaves[3]);
    require(ks::window::selectedWindowHandles(&tree) == QList<quint64>{4}, "Right-click unselected row replaces selection");
    leaves[1]->setSelected(true);
    leaves[1]->setData(0, Qt::UserRole + 2, 12);
    leaves[1]->setData(0, Qt::UserRole + 3, 21);
    leaves[1]->setData(0, Qt::UserRole + 4, quint64(1234));
    const auto savedSelection = ks::window::captureWindowSelection(&tree);
    tree.clearSelection();
    leaves[1]->setData(0, Qt::UserRole + 4, quint64(5678));
    tree.sortItems(0, Qt::DescendingOrder);
    ks::window::restoreWindowSelection(&tree, savedSelection);
    require(ks::window::selectedWindowHandles(&tree) == QList<quint64>{4}, "Refresh skips a reused process identity");
    require(tree.currentItem() == leaves[3], "Refresh restores current row after reordering");
    QTreeWidget rebuiltTree;
    rebuiltTree.setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto* recreated = new QTreeWidgetItem(&rebuiltTree, {"Recreated"});
    recreated->setData(0, Qt::UserRole, quint64(4));
    ks::window::restoreWindowSelection(&rebuiltTree, savedSelection);
    require(recreated->isSelected() && rebuiltTree.currentItem() == recreated,
        "Selection restores onto newly created tree items");
    tree.sortItems(0, Qt::AscendingOrder);
    tree.clearSelection();
    group->setSelected(true);
    require(ks::window::selectedWindowHandles(&tree).isEmpty(), "Groups are not window targets");
    tree.selectAll();
    require(ks::window::selectedWindowHandles(&tree).size() == 4, "Select all excludes groups");
    auto* duplicate = new QTreeWidgetItem(group, {"Duplicate"});
    duplicate->setData(0, Qt::UserRole, quint64(1));
    duplicate->setSelected(true);
    require(ks::window::selectedWindowHandles(&tree).size() == 4, "Duplicate HWND excluded");
    duplicate->setHidden(true);
    leaves[0]->setHidden(true);
    require(ks::window::selectedWindowHandles(&tree).size() == 3, "Hidden rows excluded");

    // Every native window belongs to this test on a non-input desktop.
    QWidget owner;
    struct TestWindow : QWidget { using QWidget::destroy; } target;
    target.setAttribute(Qt::WA_ShowWithoutActivating);
    target.resize(220, 130);
    target.show();
    const HWND nativeTarget = reinterpret_cast<HWND>(target.winId());
    ::SetWindowPos(nativeTarget, nullptr, 127, 93, 220, 130,
        SWP_NOACTIVATE | SWP_NOZORDER);
    DWORD pid = 0;
    const DWORD tid = ::GetWindowThreadProcessId(nativeTarget, &pid);
    const quint64 hwnd = static_cast<quint64>(reinterpret_cast<quintptr>(nativeTarget));
    require(ks::window::windowIdentityMatches(hwnd, pid, tid), "Live HWND identity");
    require(!ks::window::windowIdentityMatches(hwnd, pid + 1, tid), "PID mismatch rejected");
    require(!ks::window::windowIdentityMatches(hwnd, pid, tid + 1), "TID mismatch rejected");
    FILETIME creation{}, exit{}, kernel{}, user{};
    require(::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user) != FALSE, "Test process creation time");
    const quint64 created = (static_cast<quint64>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    require(ks::window::windowIdentityMatches(hwnd, pid, tid, created), "Process creation time matches");
    require(!ks::window::windowIdentityMatches(hwnd, pid, tid, created + 1), "Reused PID identity rejected");

    QWidget otherTarget;
    otherTarget.setAttribute(Qt::WA_ShowWithoutActivating);
    otherTarget.show();
    const HWND otherHwnd = reinterpret_cast<HWND>(otherTarget.winId());
    require(ks::window::setWindowTopMost(nativeTarget, true), "Prepare mixed topmost state");
    require(ks::window::setWindowEnabled(nativeTarget, false), "Prepare mixed enabled state");
    require(ks::window::requestWindowVisible(nativeTarget, false), "Prepare mixed visible state");
    QTest::qWait(30);
    for (const bool requested : {true, false})
    {
        for (const HWND selected : {nativeTarget, otherHwnd})
        {
            require(ks::window::setWindowTopMost(selected, requested), "Batch sets a uniform topmost state");
            require(ks::window::setWindowEnabled(selected, requested), "Batch sets a uniform enabled state");
            require(ks::window::requestWindowVisible(selected, requested), "Batch visibility request accepted");
        }
        QTest::qWait(30);
        for (const HWND selected : {nativeTarget, otherHwnd})
            require((::IsWindowVisible(selected) != FALSE) == requested, "Batch sets a uniform visibility state");
    }
    require(ks::window::requestWindowVisible(nativeTarget, true), "Restore test target visibility");
    QTest::qWait(30);
    const HWND foregroundBefore = ::GetForegroundWindow();
    QRect markRect;
    require(ks::window::queryWindowMarkRect(nativeTarget, markRect), "Query live marker bounds");
    const QRect physicalMarkRect = markRect;
    const auto originalDpiContext = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    require(ks::window::queryWindowMarkRect(nativeTarget, markRect) && markRect == physicalMarkRect,
        "Native bounds are physical even when the calling thread starts DPI unaware");
    require(::AreDpiAwarenessContextsEqual(::GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_UNAWARE) != FALSE,
        "Physical query restores caller DPI awareness");
    ::SetThreadDpiAwarenessContext(originalDpiContext);
    const QRect virtualBounds = ks::window::physicalVirtualDesktop();
    markRect = QRect(virtualBounds.topLeft() + QPoint(100, 130), QSize(400, 250));
    const ks::window::WindowPositionMark first{markRect,
        {"First marked window", "Process: TargetOne   PID/TID: 42 / 51", "Class: Example   HWND: 0x1234", "Position: 100, 130   Size: 400 x 250"}};
    const ks::window::WindowPositionMark second{markRect.translated(30, 10),
        {"Second overlapping window", "Process: TargetTwo   PID/TID: 43 / 52", "Class: Example   HWND: 0x5678", "Position: 130, 140   Size: 400 x 250"}};
    const QString hint = "Marked windows - press left or right mouse button to close";
    QPalette palette = owner.palette();
    palette.setColor(QPalette::Highlight, QColor(30, 120, 220));
    palette.setColor(QPalette::Base, QColor(245, 248, 253));
    palette.setColor(QPalette::Text, QColor(20, 25, 30));
    owner.setPalette(palette);
    QPointer<ks::window::WindowPositionOverlay> single =
        new ks::window::WindowPositionOverlay({first}, hint, &owner);
    QTest::qWait(40);
    const QImage singlePaint = single->grab().toImage();
    RECT overlayRect{};
    const HWND nativeOverlay = reinterpret_cast<HWND>(single->winId());
    ::GetWindowRect(nativeOverlay, &overlayRect);
    const RECT expected{virtualBounds.x(), virtualBounds.y(),
        virtualBounds.x() + virtualBounds.width(), virtualBounds.y() + virtualBounds.height()};
    require(sameBounds(expected, overlayRect, single->devicePixelRatioF()), "One overlay covers the virtual desktop");
    const LONG_PTR exStyle = ::GetWindowLongPtrW(nativeOverlay, GWL_EXSTYLE);
    require((exStyle & WS_EX_TRANSPARENT) == 0, "Overlay receives dismissal clicks");
    require((exStyle & WS_EX_NOACTIVATE) != 0, "Overlay does not activate");
    require(::GetAwarenessFromDpiAwarenessContext(::GetWindowDpiAwarenessContext(nativeOverlay))
        == DPI_AWARENESS_PER_MONITOR_AWARE, "Overlay HWND is per-monitor DPI aware");
    require(::GetForegroundWindow() == foregroundBefore, "Foreground preserved");
    require(singlePaint.pixelColor(singlePaint.width() - 12, singlePaint.height() - 12).alpha() == 1,
        "Blank pixels remain transparent and hit-testable");
    const QPoint fillPoint = markRect.bottomLeft() + QPoint(30, -25) - QPoint(overlayRect.left, overlayRect.top);
    const QColor fill = singlePaint.pixelColor(fillPoint);
    if (!(fill.alpha() > 1 && fill.alpha() < 80 && fill.blue() > fill.red()))
    {
        std::cerr << "Fill: " << fill.red() << ',' << fill.green() << ',' << fill.blue() << ',' << fill.alpha()
            << " Point: " << fillPoint.x() << ',' << fillPoint.y()
            << " Overlay origin: " << overlayRect.left << ',' << overlayRect.top
            << " DPR: " << single->devicePixelRatioF() << '\n';
        singlePaint.save(QStringLiteral(".codex-build-logs/window-list-tests/window-position-debug.png"));
    }
    require(fill.alpha() > 1 && fill.alpha() < 80 && fill.blue() > fill.red(), "Pale theme fill is visible");
    const qreal scale = single->devicePixelRatioF();
    const QRect cardSample(qRound(markRect.x() - overlayRect.left + 8 * scale),
        qRound(markRect.y() - overlayRect.top + 8 * scale), qRound(265 * scale), qRound(55 * scale));
    const QImage cardBefore = singlePaint.copy(cardSample);
    require(cardBefore.pixelColor(0, 0).alpha() == 255, "Information has an opaque background");

    QPointer<ks::window::WindowPositionOverlay> overlay =
        new ks::window::WindowPositionOverlay({first, second}, hint, &owner);
    QTest::qWait(40);
    require(single.isNull(), "New marking replaces the previous overlay");
    int overlayCount = 0;
    for (auto* widget : QApplication::topLevelWidgets())
        if (dynamic_cast<ks::window::WindowPositionOverlay*>(widget)) ++overlayCount;
    require(overlayCount == 1, "Multiple targets share one native overlay window");
    const QImage multiPaint = overlay->grab().toImage();
    require(cardBefore == multiPaint.copy(cardSample), "Overlapping target shading never covers basic information");
    QImage preview(multiPaint.size(), QImage::Format_ARGB32_Premultiplied);
    preview.fill(QColor(215, 219, 226));
    {
        QPainter painter(&preview);
        painter.fillRect(markRect.translated(-overlayRect.left, -overlayRect.top), QColor(253, 253, 253));
        painter.fillRect(second.physicalRect.translated(-overlayRect.left, -overlayRect.top), QColor(235, 238, 244));
        QImage overlayPixels = multiPaint;
        overlayPixels.setDevicePixelRatio(1);
        painter.drawImage(0, 0, overlayPixels);
    }
    require(preview.save(QStringLiteral(".codex-build-logs/window-list-tests/window-position-overlay.png")), "Save overlay visual regression image");
    require(preview.copy(0, 0, std::min(1280, preview.width()), std::min(720, preview.height())).save(
        QStringLiteral(".codex-build-logs/window-list-tests/window-position-overlay-detail.png")), "Save marker detail visual regression image");
    QTest::qWait(1200);
    require(overlay && overlay->isVisible(), "Marker persists until a dismissal click");
    QTest::mousePress(overlay, Qt::LeftButton, Qt::NoModifier, overlay->rect().bottomRight() - QPoint(12, 12));
    QTest::qWait(30);
    require(overlay.isNull(), "Left button press destroys the full-screen overlay");
    overlay = new ks::window::WindowPositionOverlay({first, second}, hint, &owner);
    QTest::qWait(30);
    QTest::mousePress(overlay, Qt::RightButton, Qt::NoModifier, QPoint(15, 15));
    QTest::qWait(30);
    require(overlay.isNull(), "Right button press destroys the full-screen overlay");
    require(::GetForegroundWindow() == foregroundBefore, "Dismissal does not activate a target");

    QVector<QRect> monitors;
    ::EnumDisplayMonitors(nullptr, nullptr, collectMonitorRects, reinterpret_cast<LPARAM>(&monitors));
    QVector<ks::window::WindowPositionMark> monitorMarks;
    std::vector<std::unique_ptr<QWidget>> monitorWindows;
    for (const auto& monitor : monitors)
    {
        auto monitorWindow = std::make_unique<QWidget>();
        monitorWindow->setAttribute(Qt::WA_ShowWithoutActivating);
        monitorWindow->show();
        const HWND native = reinterpret_cast<HWND>(monitorWindow->winId());
        {
            const ks::window::PhysicalCoordinateScope coordinates;
            ::SetWindowPos(native, nullptr, monitor.left() + 80, monitor.top() + 100,
                std::min(400, monitor.width() - 100), std::min(300, monitor.height() - 120), SWP_NOACTIVATE | SWP_NOZORDER);
        }
        QTest::qWait(40);
        QRect bounds;
        require(ks::window::queryWindowMarkRect(native, bounds), "Read target on each physical monitor");
        monitorMarks.push_back({bounds, {"Target on another monitor", QString::number(::GetDpiForWindow(native))}});
        monitorWindows.push_back(std::move(monitorWindow));
    }
    overlay = new ks::window::WindowPositionOverlay(monitorMarks, hint, &owner);
    QTest::qWait(50);
    {
        const ks::window::PhysicalCoordinateScope coordinates;
        ::GetWindowRect(reinterpret_cast<HWND>(overlay->winId()), &overlayRect);
    }
    const QImage monitorPaint = overlay->grab().toImage();
    for (const auto& mark : monitorMarks)
    {
        const QPoint inside = mark.physicalRect.bottomLeft() + QPoint(12, -12)
            - QPoint(overlayRect.left, overlayRect.top);
        const QColor color = monitorPaint.pixelColor(inside);
        require(color.alpha() > 1 && color.alpha() < 80 && color.blue() > color.red(),
            "One overlay paints a target on every monitor at its actual physical location");
    }
    std::cout << "MONITOR_DPI_TESTS=" << monitorMarks.size()
        << " OVERLAY_DPR=" << overlay->devicePixelRatioF() << '\n';
    overlay->close();
    QTest::qWait(30);

    ::ShowWindow(nativeTarget, SW_MINIMIZE);
    require(ks::window::queryWindowMarkRect(nativeTarget, markRect) && markRect.width() > 0,
        "Minimized window uses its restoration bounds");
    require(std::abs(markRect.left() - physicalMarkRect.left()) <= 2
        && std::abs(markRect.top() - physicalMarkRect.top()) <= 2
        && std::abs(markRect.width() - physicalMarkRect.width()) <= 2
        && std::abs(markRect.height() - physicalMarkRect.height()) <= 2,
        "Minimized restoration bounds remain in screen coordinates");
    require(::IsIconic(nativeTarget) != FALSE, "Marking does not restore a minimized window");
    target.destroy();
    require(!ks::window::queryWindowMarkRect(nativeTarget, markRect), "Destroyed target cannot be marked");
    require(!ks::window::windowIdentityMatches(hwnd, pid, tid), "Destroyed HWND rejected");
    require(!ks::window::setWindowEnabled(nativeTarget, false), "Disabling an invalid HWND is not success");

}

int main(int argc, char** argv)
{
    const HDESK original = ::GetThreadDesktop(::GetCurrentThreadId());
    const std::wstring name = L"KswordWindowListTests-" + std::to_wstring(::GetCurrentProcessId());
    HDESK desktop = ::CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !::SetThreadDesktop(desktop)) return 2;
    int result = 0;
    {
        QApplication app(argc, argv);
        try { runTests(); std::cout << "WINDOW_LIST_TESTS=SUCCESS\n"; }
        catch (const std::exception& error)
        {
            std::cerr << "WINDOW_LIST_TESTS=FAILURE: " << error.what() << '\n';
            result = 1;
        }
    }
    ::SetThreadDesktop(original);
    ::CloseDesktop(desktop);
    return result;
}
