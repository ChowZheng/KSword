#include "SmoothScrollSupport.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QHash>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextLayout>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
    constexpr char kInstalledProperty[] = "KSWORD_SMOOTH_SCROLL_SUPPORT_INSTALLED";
    constexpr char kEnabledProperty[] = "ksword_smooth_scrolling_enabled";
    constexpr char kOriginalVerticalModeProperty[] =
        "KSWORD_SMOOTH_SCROLL_ORIGINAL_VERTICAL_MODE";
    constexpr char kOriginalHorizontalModeProperty[] =
        "KSWORD_SMOOTH_SCROLL_ORIGINAL_HORIZONTAL_MODE";
    constexpr char kFrozenPaneAuxiliaryProperty[] =
        "KSWORD_TABLE_INTERACTION_FROZEN_PANE_AUXILIARY";
    constexpr int kWheelAnimationDurationMs = 180;
    constexpr int kPixelAnimationDurationMs = 100;

    class GlobalSmoothScrollFilter;
    QPointer<GlobalSmoothScrollFilter> g_installedFilter;

    class GlobalSmoothScrollFilter final : public QObject
    {
    public:
        explicit GlobalSmoothScrollFilter(QObject* parentObject)
            : QObject(parentObject)
        {
        }

        void applyEnabledStateToAllWidgets(const bool enabled)
        {
            const QWidgetList widgetList = QApplication::allWidgets();
            for (QWidget* widget : widgetList)
            {
                QAbstractScrollArea* scrollArea =
                    qobject_cast<QAbstractScrollArea*>(widget);
                if (scrollArea != nullptr &&
                    !scrollArea->property(kFrozenPaneAuxiliaryProperty).toBool())
                {
                    configureScrollArea(scrollArea, enabled);
                }
            }
            // 冻结覆盖视图必须在主表切换完滚动模式后再同步，防止关闭平滑滚动时单位不一致。
            for (QWidget* widget : widgetList)
            {
                QAbstractScrollArea* scrollArea =
                    qobject_cast<QAbstractScrollArea*>(widget);
                if (scrollArea != nullptr &&
                    scrollArea->property(kFrozenPaneAuxiliaryProperty).toBool())
                {
                    configureScrollArea(scrollArea, enabled);
                }
            }
            if (!enabled)
            {
                stopAllAnimations();
            }
        }

    protected:
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            if (eventObject == nullptr)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            if (eventObject->type() == QEvent::Show ||
                eventObject->type() == QEvent::Polish)
            {
                if (QAbstractScrollArea* scrollArea =
                    qobject_cast<QAbstractScrollArea*>(watchedObject))
                {
                    configureScrollArea(scrollArea,
                        enabled() && !isSmoothScrollDisabled(scrollArea));
                }
            }

            if (eventObject->type() == QEvent::Resize)
            {
                if (QAbstractScrollArea* scrollArea = scrollAreaForEventObject(watchedObject))
                {
                    // 缩小视口后，旧动画的终点可能超过新的可见范围。
                    stopAnimation(scrollArea->verticalScrollBar());
                    stopAnimation(scrollArea->horizontalScrollBar());
                }
            }

            if (eventObject->type() != QEvent::Wheel)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            QAbstractScrollArea* scrollArea = scrollAreaForEventObject(watchedObject);
            if (scrollArea == nullptr ||
                scrollArea->property(kFrozenPaneAuxiliaryProperty).toBool() ||
                isSmoothScrollDisabled(scrollArea))
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            auto* wheelEvent = static_cast<QWheelEvent*>(eventObject);
            if (wheelEvent->modifiers().testFlag(Qt::ControlModifier) ||
                wheelEvent->modifiers().testFlag(Qt::AltModifier))
            {
                // 保留 Ctrl+滚轮缩放和业务自定义 Alt+滚轮行为。
                return QObject::eventFilter(watchedObject, eventObject);
            }

            const QPoint pixelDelta = wheelEvent->pixelDelta();
            const QPoint angleDelta = wheelEvent->angleDelta();
            const QScrollBar* directScrollBar = qobject_cast<QScrollBar*>(watchedObject);
            const bool horizontal = directScrollBar != nullptr
                ? directScrollBar->orientation() == Qt::Horizontal
                : (wheelEvent->modifiers().testFlag(Qt::ShiftModifier) ||
                    std::abs(pixelDelta.x()) > std::abs(pixelDelta.y()) ||
                    std::abs(angleDelta.x()) > std::abs(angleDelta.y()));

            const QRect visibleRect = scrollArea->viewport()->visibleRegion().boundingRect();
            // QPlainTextEdit 纵向 value/pageStep 是视觉行，不是像素；通常沿用 Qt
            // 对换行、触控板增量和一页上限的处理，禁止对行号做像素动画。
            if (!horizontal)
            {
                if (QPlainTextEdit* plainEdit = qobject_cast<QPlainTextEdit*>(scrollArea))
                {
                    // 结构报告中的固定高度代码块可能被外层滚动区裁切。Qt 的 pageStep
                    // 仍按代码块完整视口计算，此时额外以真正露出的视觉行数限幅。
                    return scrollClippedPlainText(plainEdit, visibleRect, wheelEvent);
                }
            }
            if (!enabled())
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }
            if (QAbstractItemView* itemView = qobject_cast<QAbstractItemView*>(scrollArea))
            {
                const auto mode = horizontal
                    ? itemView->horizontalScrollMode() : itemView->verticalScrollMode();
                if (mode != QAbstractItemView::ScrollPerPixel)
                {
                    return QObject::eventFilter(watchedObject, eventObject);
                }
            }
            QScrollBar* scrollBar = horizontal
                ? scrollArea->horizontalScrollBar()
                : scrollArea->verticalScrollBar();
            if (scrollBar == nullptr || scrollBar->minimum() == scrollBar->maximum())
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            const int rawPixelDelta = horizontal
                ? (pixelDelta.x() != 0 ? pixelDelta.x() : pixelDelta.y())
                : (pixelDelta.y() != 0 ? pixelDelta.y() : pixelDelta.x());
            const int rawAngleDelta = horizontal
                ? (angleDelta.x() != 0 ? angleDelta.x() : angleDelta.y())
                : (angleDelta.y() != 0 ? angleDelta.y() : angleDelta.x());
            if (rawPixelDelta == 0 && rawAngleDelta == 0)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            const int directionMultiplier = wheelEvent->inverted() ? -1 : 1;
            const int visibleExtent = horizontal ? visibleRect.width() : visibleRect.height();
            if (visibleExtent <= 0 || scrollBar->pageStep() <= 0)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }
            const int pageExtent = std::min(visibleExtent, scrollBar->pageStep());
            const int overlap = std::min(scrollArea->fontMetrics().lineSpacing(), pageExtent / 2);
            const int maximumDistance = std::max(1, pageExtent - overlap);
            double requestedDistance = 0.0;
            int durationMs = kWheelAnimationDurationMs;
            if (rawPixelDelta != 0)
            {
                requestedDistance = -static_cast<double>(rawPixelDelta) * directionMultiplier;
                durationMs = kPixelAnimationDurationMs;
            }
            else
            {
                const double wheelSteps =
                    static_cast<double>(rawAngleDelta) * directionMultiplier / 120.0;
                const double pixelsPerStep = std::clamp(
                    static_cast<double>(scrollBar->singleStep()) * 3.0,
                    48.0,
                    120.0);
                requestedDistance = -wheelSteps * pixelsPerStep;
            }
            const int distance = static_cast<int>(std::lround(std::clamp(
                requestedDistance, -static_cast<double>(maximumDistance),
                static_cast<double>(maximumDistance))));
            if (distance == 0)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            QPropertyAnimation* animation = animationForScrollBar(scrollBar);
            const qint64 currentValue = scrollBar->value();
            const qint64 pendingTarget = animation->state() == QAbstractAnimation::Running
                ? animation->endValue().toInt() : currentValue;
            // 反向滚动立即从当前位置反向；连续事件的待滚距离也不能超过一屏。
            const bool sameDirection = distance > 0
                ? pendingTarget > currentValue : pendingTarget < currentValue;
            const qint64 accumulatedStart = sameDirection
                ? pendingTarget : currentValue;
            const int targetValue = static_cast<int>(std::clamp(
                accumulatedStart + distance,
                std::max<qint64>(scrollBar->minimum(), currentValue - maximumDistance),
                std::min<qint64>(scrollBar->maximum(), currentValue + maximumDistance)));
            if (targetValue == scrollBar->value())
            {
                // 到达边界时让未消费的滚轮事件继续向父滚动区域传播。
                animation->stop();
                return QObject::eventFilter(watchedObject, eventObject);
            }

            animation->stop();
            animation->setDuration(durationMs);
            animation->setStartValue(scrollBar->value());
            animation->setEndValue(targetValue);
            animation->setEasingCurve(QEasingCurve::OutCubic);
            animation->start();
            wheelEvent->accept();
            return true;
        }

    private:
        bool scrollClippedPlainText(QPlainTextEdit* edit, const QRect& visibleRect,
            QWheelEvent* event)
        {
            if (visibleRect.isEmpty() || visibleRect.height() >= edit->viewport()->height())
            {
                return false;
            }
            QScrollBar* bar = edit->verticalScrollBar();
            const int direction = event->inverted() ? -1 : 1;
            const int pixelDelta = event->pixelDelta().y();
            const double requested = pixelDelta != 0
                ? -static_cast<double>(pixelDelta) * direction /
                    std::max(1, edit->fontMetrics().lineSpacing())
                : -static_cast<double>(event->angleDelta().y()) * direction / 120.0 *
                    QApplication::wheelScrollLines() * bar->singleStep();
            if (requested == 0.0 ||
                (requested < 0 && bar->value() == bar->minimum()) ||
                (requested > 0 && bar->value() == bar->maximum()))
            {
                bar->setProperty("ksword_clipped_scroll_remainder", 0.0);
                return false;
            }

            const auto visualLineAt = [edit](const QPoint& point)
            {
                const QTextCursor cursor = edit->cursorForPosition(point);
                const QTextBlock block = cursor.block();
                const QTextLine line = block.layout()->lineForTextPosition(cursor.positionInBlock());
                return block.firstLineNumber() + std::max(0, line.lineNumber());
            };
            const int visibleLines = visualLineAt(visibleRect.bottomLeft()) -
                visualLineAt(visibleRect.topLeft());
            const int maximumDistance = std::max(1, visibleLines - 1);
            double remainder = bar->property("ksword_clipped_scroll_remainder").toDouble();
            if (remainder * requested < 0)
            {
                remainder = 0.0;
            }
            const double bounded = std::clamp(remainder + requested,
                -static_cast<double>(maximumDistance), static_cast<double>(maximumDistance));
            const int distance = static_cast<int>(bounded);
            bar->setProperty("ksword_clipped_scroll_remainder", bounded - distance);
            bar->setValue(static_cast<int>(std::clamp<qint64>(
                static_cast<qint64>(bar->value()) + distance, bar->minimum(), bar->maximum())));
            event->accept();
            return true;
        }

        bool isSmoothScrollDisabled(const QWidget* widget) const
        {
            for (const QWidget* current = widget; current != nullptr;
                current = current->parentWidget())
            {
                if (current->property("ksword_disable_smooth_scroll").toBool())
                {
                    return true;
                }
            }
            return false;
        }

        void stopAnimation(QScrollBar* scrollBar)
        {
            if (QPropertyAnimation* animation = m_animations.value(scrollBar, nullptr))
            {
                animation->stop();
            }
        }

        bool enabled() const
        {
            QApplication* appInstance =
                qobject_cast<QApplication*>(QCoreApplication::instance());
            return appInstance != nullptr &&
                appInstance->property(kEnabledProperty).toBool();
        }

        QAbstractScrollArea* scrollAreaForEventObject(QObject* watchedObject) const
        {
            if (QAbstractScrollArea* directArea =
                qobject_cast<QAbstractScrollArea*>(watchedObject))
            {
                return directArea;
            }
            if (QScrollBar* scrollBar = qobject_cast<QScrollBar*>(watchedObject))
            {
                for (QWidget* parent = scrollBar->parentWidget(); parent != nullptr;
                    parent = parent->parentWidget())
                {
                    if (QAbstractScrollArea* area = qobject_cast<QAbstractScrollArea*>(parent))
                    {
                        return scrollBar == area->verticalScrollBar() ||
                            scrollBar == area->horizontalScrollBar()
                            ? area : nullptr;
                    }
                }
                return nullptr;
            }
            QAbstractScrollArea* parentArea = qobject_cast<QAbstractScrollArea*>(
                watchedObject != nullptr ? watchedObject->parent() : nullptr);
            return parentArea != nullptr && parentArea->viewport() == watchedObject
                ? parentArea
                : nullptr;
        }

        void configureScrollArea(QAbstractScrollArea* scrollArea, const bool enabledState)
        {
            QAbstractItemView* itemView = qobject_cast<QAbstractItemView*>(scrollArea);
            if (itemView == nullptr)
            {
                return;
            }
            if (itemView->property(kFrozenPaneAuxiliaryProperty).toBool())
            {
                // 冻结窗格的偏移由 TableFrozenPaneController 按像素直接写入滚动条，
                // 跟随主表切到按整行滚动会让冻结区与主表错行，因此这里固定按像素。
                itemView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
                itemView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
                return;
            }

            if (enabledState && !isSmoothScrollDisabled(scrollArea))
            {
                if (!itemView->property(kOriginalVerticalModeProperty).isValid())
                {
                    itemView->setProperty(
                        kOriginalVerticalModeProperty,
                        static_cast<int>(itemView->verticalScrollMode()));
                    itemView->setProperty(
                        kOriginalHorizontalModeProperty,
                        static_cast<int>(itemView->horizontalScrollMode()));
                }
                itemView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
                itemView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
                return;
            }

            const QVariant originalVerticalMode =
                itemView->property(kOriginalVerticalModeProperty);
            const QVariant originalHorizontalMode =
                itemView->property(kOriginalHorizontalModeProperty);
            if (originalVerticalMode.isValid())
            {
                itemView->setVerticalScrollMode(
                    static_cast<QAbstractItemView::ScrollMode>(
                        originalVerticalMode.toInt()));
                itemView->setProperty(kOriginalVerticalModeProperty, QVariant());
            }
            if (originalHorizontalMode.isValid())
            {
                itemView->setHorizontalScrollMode(
                    static_cast<QAbstractItemView::ScrollMode>(
                        originalHorizontalMode.toInt()));
                itemView->setProperty(kOriginalHorizontalModeProperty, QVariant());
            }
        }

        QPropertyAnimation* animationForScrollBar(QScrollBar* scrollBar)
        {
            QPropertyAnimation* animation = m_animations.value(scrollBar, nullptr);
            if (animation != nullptr)
            {
                return animation;
            }

            animation = new QPropertyAnimation(scrollBar, "value", this);
            m_animations.insert(scrollBar, animation);
            connect(scrollBar, &QScrollBar::sliderPressed, animation, [animation]()
                {
                    animation->stop();
                });
            connect(scrollBar, &QObject::destroyed, this, [this, scrollBar]()
                {
                    if (QPropertyAnimation* removedAnimation =
                        m_animations.take(scrollBar))
                    {
                        removedAnimation->stop();
                        removedAnimation->deleteLater();
                    }
                });
            return animation;
        }

        void stopAllAnimations()
        {
            for (QPropertyAnimation* animation : std::as_const(m_animations))
            {
                if (animation != nullptr)
                {
                    animation->stop();
                }
            }
        }

        QHash<QScrollBar*, QPropertyAnimation*> m_animations;
    };

    GlobalSmoothScrollFilter* installedFilter()
    {
        return g_installedFilter.data();
    }
}

void ks::ui::InstallGlobalSmoothScrollSupport(QApplication* appInstance)
{
    if (appInstance == nullptr || appInstance->property(kInstalledProperty).toBool())
    {
        return;
    }

    auto* filter = new GlobalSmoothScrollFilter(appInstance);
    filter->setObjectName(QStringLiteral("KSWORD_GLOBAL_SMOOTH_SCROLL_FILTER"));
    g_installedFilter = filter;
    appInstance->installEventFilter(filter);
    appInstance->setProperty(kInstalledProperty, true);
    filter->applyEnabledStateToAllWidgets(
        appInstance->property(kEnabledProperty).toBool());
}

void ks::ui::SetGlobalSmoothScrollingEnabled(const bool enabled)
{
    QApplication* appInstance =
        qobject_cast<QApplication*>(QCoreApplication::instance());
    if (appInstance == nullptr)
    {
        return;
    }
    appInstance->setProperty(kEnabledProperty, enabled);
    if (GlobalSmoothScrollFilter* filter = installedFilter())
    {
        filter->applyEnabledStateToAllWidgets(enabled);
    }
}

bool ks::ui::IsGlobalSmoothScrollingEnabled()
{
    QApplication* appInstance =
        qobject_cast<QApplication*>(QCoreApplication::instance());
    return appInstance != nullptr &&
        appInstance->property(kEnabledProperty).toBool();
}
