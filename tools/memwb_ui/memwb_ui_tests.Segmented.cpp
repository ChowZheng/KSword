// memwb_ui_tests.Segmented.cpp
// 作用：Phase 3 WP-0 的离屏验证（第一组）——HexViewSegmented::setSegmentEnabled 禁用段：
//   1) 默认全部可用，越界下标安全；
//   2) 点击禁用段无效（点击被吞掉、不发信号），启用段照常切换；
//   3) 键盘左右键跳过禁用段（含连续多个、当前段自己被禁用、两端没有可用段不回绕）；
//   4) 全部禁用时点击与按键都无效，重新启用一段后恢复；
//   5) 重新启用后一切恢复，常规提示不丢；
//   6) 悬停提示：禁用段显示原因（真实 ToolTip 事件），原因为空退回常规提示，启用时清掉原因；
//   7) 代码通路 setCurrentIndex 仍可选中禁用段，禁用当前段不替调用方换走；
//   8) 像素实测：禁用段的文字是禁用灰、选中的禁用段底色权重减半、禁用段没有悬停高亮；
//   9) 悬停光标：禁用段上是箭头，其余是手形。
// 全部经 QTest 模拟的真实输入、真实的 ToolTip/Hover 事件与离屏渲染进入控件，结果从公开访问器、信号与像素读回。

#include "memwb_ui_common.h"

#include "../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewFormat.h"
#include "../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewWidgets.h"

#include <QApplication>
#include <QFont>
#include <QHelpEvent>
#include <QHoverEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QToolTip>

#include <iostream>
#include <memory>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexViewSegmented;

        // MakeSegmented：按文字构造一个分段按钮、调到建议尺寸并显示。
        // 传入：各段文字；传出：已显示的控件。
        std::unique_ptr<HexViewSegmented> MakeSegmented(const QStringList& labels)
        {
            auto segmented = std::make_unique<HexViewSegmented>(labels);
            segmented->resize(segmented->sizeHint());
            segmented->show();
            return segmented;
        }

        // ClickSegment：在某一段的中心点左键点击。
        void ClickSegment(HexViewSegmented& segmented, int index)
        {
            QTest::mouseClick(&segmented, Qt::LeftButton, Qt::NoModifier, segmented.segmentRect(index).center());
        }

        // PressedAndAccepted：向某一段发一次手工构造的鼠标按下事件，返回控件是否接受了它。
        // 用途：区分"被控件吞掉"（接受）与"没人处理、继续向父控件冒泡"（未接受）。
        bool PressedAndAccepted(HexViewSegmented& segmented, int index)
        {
            const QPoint position = segmented.segmentRect(index).center();
            QMouseEvent press(
                QEvent::MouseButtonPress,
                QPointF(position),
                QPointF(segmented.mapToGlobal(position)),
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier);
            QApplication::sendEvent(&segmented, &press);
            return press.isAccepted();
        }

        // KeyAndAccepted：向控件发一次手工构造的按键事件，返回控件是否接受了它。
        bool KeyAndAccepted(HexViewSegmented& segmented, Qt::Key key)
        {
            QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
            QApplication::sendEvent(&segmented, &press);
            return press.isAccepted();
        }

        // TipShownFor：向控件某点发真实的 ToolTip 事件，返回 QToolTip 实际弹出的文字（没弹出为空串）。
        // 为什么先弹一个哨兵文字：QToolTip::hideText 是延迟 300 ms 才真正隐藏的，
        // 上一次的提示文字会残留，直接读 QToolTip::text() 分不清"这次弹出的"与"上次残留的"。
        // 预先放一个哨兵文字，事件之后文字仍是哨兵，就说明这次没有任何提示被安装。
        QString TipShownFor(QWidget* widget, const QPoint& position)
        {
            const QString sentinel = QStringLiteral("sentinel-tip-not-from-segmented");
            QToolTip::showText(widget->mapToGlobal(position), sentinel, widget);
            QHelpEvent event(QEvent::ToolTip, position, widget->mapToGlobal(position));
            QApplication::sendEvent(widget, &event);
            const QString shown = QToolTip::isVisible() ? QToolTip::text() : QString();
            QToolTip::hideText();
            return (shown == sentinel) ? QString() : shown;
        }

        // SendHover：向控件发一次悬停移动事件（离屏平台不会自己产生悬停，只能手工投递）。
        void SendHover(QWidget* widget, const QPoint& position)
        {
            QHoverEvent hover(
                QEvent::HoverMove,
                QPointF(position),
                QPointF(widget->mapToGlobal(position)),
                QPointF(-1.0, -1.0));
            QApplication::sendEvent(widget, &hover);
        }

        // SendHoverLeave：向控件发一次悬停离开事件。
        void SendHoverLeave(QWidget* widget)
        {
            QHoverEvent leave(
                QEvent::HoverLeave,
                QPointF(-1.0, -1.0),
                QPointF(widget->mapToGlobal(QPoint(0, 0))),
                QPointF(0.0, 0.0));
            QApplication::sendEvent(widget, &leave);
        }

        // RenderOnSentinel：把控件渲染到一张以"哨兵色"预填的图里，不画窗口背景。
        // 哨兵色选洋红：它不是任何主题色，所以"某处还是哨兵色"就等于"控件在那里什么也没画"。
        QImage RenderOnSentinel(HexViewSegmented& segmented, const QColor& sentinel)
        {
            QImage image(segmented.size(), QImage::Format_ARGB32);
            image.fill(sentinel);
            segmented.render(&image, QPoint(), QRegion(), QWidget::RenderFlags());
            return image;
        }

        // CountClose：某一段内部与给定颜色逐通道差之和不超过容差的像素数。
        // 传入：图、段矩形、颜色、容差。段矩形先向内收缩（左右 3 像素、上下 4 像素），
        //       避开分隔线与圆角外框——它们的颜色与墨色无关，却可能碰巧接近。
        int CountClose(const QImage& image, const QRect& segmentRect, const QColor& color, int tolerance)
        {
            int count = 0;
            const QRect clipped = segmentRect.adjusted(3, 4, -3, -4).intersected(image.rect());
            for (int y = clipped.top(); y <= clipped.bottom(); ++y)
            {
                for (int x = clipped.left(); x <= clipped.right(); ++x)
                {
                    count += ColorsClose(image.pixelColor(x, y), color, tolerance) ? 1 : 0;
                }
            }
            return count;
        }

        // PlateSample：某一段"左边内侧、垂直居中"的取样点（离文字、分隔线、边框都有距离，只会是底板色）。
        QPoint PlateSample(const HexViewSegmented& segmented, int index)
        {
            return QPoint(segmented.segmentRect(index).left() + 3, segmented.height() / 2);
        }

        // 默认状态与越界：全部可用；越界查询为假、越界设置什么也不做。
        void TestDefaultsAndBounds()
        {
            ApplyTheme(false);
            auto segmented = MakeSegmented({ QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D") });
            for (int index = 0; index < 4; ++index)
            {
                CHECK(segmented->isSegmentEnabled(index));
            }
            CHECK(!segmented->isSegmentEnabled(-1));
            CHECK(!segmented->isSegmentEnabled(4));
            CHECK(!segmented->isSegmentEnabled(1000));

            // 越界设置：既不崩也不影响任何一段。
            segmented->setSegmentEnabled(-1, false, QStringLiteral("x"));
            segmented->setSegmentEnabled(4, false, QStringLiteral("x"));
            for (int index = 0; index < 4; ++index)
            {
                CHECK(segmented->isSegmentEnabled(index));
            }

            // 空文字列表退化成一个空段，该段同样默认可用。
            HexViewSegmented single{ QStringList() };
            CHECK(single.count() == 1);
            CHECK(single.isSegmentEnabled(0));
            single.setSegmentEnabled(0, false);
            CHECK(!single.isSegmentEnabled(0));
        }

        // 点击禁用段无效：不切换、不发信号、点击被吞掉；点启用段照常切换。
        void TestClickOnDisabledSegment()
        {
            ApplyTheme(false);
            auto segmented = MakeSegmented({ QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D") });
            QSignalSpy spy(segmented.get(), &HexViewSegmented::currentIndexChanged);
            segmented->setSegmentEnabled(2, false, QStringLiteral("not available"));

            // 点禁用段：当前段仍是 0，没有信号。
            ClickSegment(*segmented, 2);
            CHECK(segmented->currentIndex() == 0);
            CHECK(spy.count() == 0);

            // 点击事件被控件吞掉（接受），而不是冒泡给父控件；启用段的点击同样被接受。
            CHECK(PressedAndAccepted(*segmented, 2));
            CHECK(segmented->currentIndex() == 0);
            CHECK(PressedAndAccepted(*segmented, 1));
            CHECK(segmented->currentIndex() == 1);
            CHECK(spy.count() == 1);
            CHECK(spy.count() == 1 && spy.at(0).at(0).toInt() == 1);

            // 先选中别的段再点禁用段：当前段保持，仍没有新信号。
            ClickSegment(*segmented, 2);
            CHECK(segmented->currentIndex() == 1);
            CHECK(spy.count() == 1);

            // 启用段 3 照常可点。
            ClickSegment(*segmented, 3);
            CHECK(segmented->currentIndex() == 3);
            CHECK(spy.count() == 2);
            CHECK(spy.count() == 2 && spy.at(1).at(0).toInt() == 3);

            // 设置启用状态本身不发信号。
            segmented->setSegmentEnabled(0, false);
            segmented->setSegmentEnabled(0, true);
            CHECK(spy.count() == 2);
        }

        // 键盘左右键跳过禁用段：连续多个、单个、两端没有可用段不回绕。
        void TestKeyboardSkipsDisabledSegments()
        {
            ApplyTheme(false);
            auto segmented = MakeSegmented({ QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D") });
            QSignalSpy spy(segmented.get(), &HexViewSegmented::currentIndexChanged);

            // 禁用 1 和 2：从 0 向右一步直达 3，再向右不动（不回绕），向左一步回到 0，再向左不动。
            segmented->setSegmentEnabled(1, false);
            segmented->setSegmentEnabled(2, false);
            QTest::keyClick(segmented.get(), Qt::Key_Right);
            CHECK(segmented->currentIndex() == 3);
            CHECK(spy.count() == 1);
            CHECK(spy.count() == 1 && spy.at(0).at(0).toInt() == 3);
            QTest::keyClick(segmented.get(), Qt::Key_Right);
            CHECK(segmented->currentIndex() == 3);
            CHECK(spy.count() == 1);
            QTest::keyClick(segmented.get(), Qt::Key_Left);
            CHECK(segmented->currentIndex() == 0);
            CHECK(spy.count() == 2);
            QTest::keyClick(segmented.get(), Qt::Key_Left);
            CHECK(segmented->currentIndex() == 0);
            CHECK(spy.count() == 2);

            // 只禁用 1：0 向右跳到 2，2 向左跳回 0。
            segmented->setSegmentEnabled(1, false);
            segmented->setSegmentEnabled(2, true);
            QTest::keyClick(segmented.get(), Qt::Key_Right);
            CHECK(segmented->currentIndex() == 2);
            QTest::keyClick(segmented.get(), Qt::Key_Left);
            CHECK(segmented->currentIndex() == 0);

            // 禁用末段：停在 2 时向右没有可用段，不动。
            segmented->setSegmentEnabled(1, true);
            segmented->setSegmentEnabled(3, false);
            segmented->setCurrentIndex(2);
            spy.clear();
            QTest::keyClick(segmented.get(), Qt::Key_Right);
            CHECK(segmented->currentIndex() == 2);
            CHECK(spy.count() == 0);

            // 当前段自己被禁用：从它出发仍能按方向找到下一个可用段（可用的只有 0 和 3：向右到 3，向左跳过禁用的 1 到 0）。
            segmented->setSegmentEnabled(3, true);
            segmented->setSegmentEnabled(2, false);
            segmented->setSegmentEnabled(1, false);
            segmented->setCurrentIndex(2);
            spy.clear();
            QTest::keyClick(segmented.get(), Qt::Key_Right);
            CHECK(segmented->currentIndex() == 3);
            segmented->setCurrentIndex(2);
            QTest::keyClick(segmented.get(), Qt::Key_Left);
            CHECK(segmented->currentIndex() == 0);

            // 按键事件被控件接受（左右键即使没有可移动处也不外传）；其它键不接受。
            CHECK(KeyAndAccepted(*segmented, Qt::Key_Left));
            CHECK(KeyAndAccepted(*segmented, Qt::Key_Right));
            CHECK(!KeyAndAccepted(*segmented, Qt::Key_Up));
        }

        // 全部禁用：点击与按键都无效、当前段不变、控件本身仍启用；重新启用一段后恢复。
        void TestAllSegmentsDisabled()
        {
            ApplyTheme(false);
            auto segmented = MakeSegmented({ QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C") });
            QSignalSpy spy(segmented.get(), &HexViewSegmented::currentIndexChanged);
            segmented->setCurrentIndex(1);
            spy.clear();
            for (int index = 0; index < 3; ++index)
            {
                segmented->setSegmentEnabled(index, false, QStringLiteral("none available"));
            }
            for (int index = 0; index < 3; ++index)
            {
                CHECK(!segmented->isSegmentEnabled(index));
            }
            CHECK(segmented->isEnabled());

            // 点击每一段、按左右键：都不变，且没有信号；按键仍被接受。
            for (int index = 0; index < 3; ++index)
            {
                ClickSegment(*segmented, index);
            }
            QTest::keyClick(segmented.get(), Qt::Key_Left);
            QTest::keyClick(segmented.get(), Qt::Key_Right);
            CHECK(segmented->currentIndex() == 1);
            CHECK(spy.count() == 0);
            CHECK(KeyAndAccepted(*segmented, Qt::Key_Right));
            CHECK(segmented->currentIndex() == 1);

            // 重新启用段 2：点击生效；它的左边没有可用段，右边也没有，按键仍不动。
            segmented->setSegmentEnabled(2, true);
            ClickSegment(*segmented, 2);
            CHECK(segmented->currentIndex() == 2);
            CHECK(spy.count() == 1);
            QTest::keyClick(segmented.get(), Qt::Key_Left);
            QTest::keyClick(segmented.get(), Qt::Key_Right);
            CHECK(segmented->currentIndex() == 2);
            CHECK(spy.count() == 1);
        }

        // 重新启用后一切恢复：可点击、键盘不再跳过、提示回到常规提示。
        void TestReenableRestores()
        {
            ApplyTheme(false);
            auto segmented = MakeSegmented({ QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C") });
            segmented->setSegmentToolTip(1, QStringLiteral("normal tip B"));
            segmented->setSegmentEnabled(1, false, QStringLiteral("reason B"));
            ClickSegment(*segmented, 1);
            CHECK(segmented->currentIndex() == 0);
            QTest::keyClick(segmented.get(), Qt::Key_Right);
            CHECK(segmented->currentIndex() == 2);
            QTest::keyClick(segmented.get(), Qt::Key_Left);
            CHECK(segmented->currentIndex() == 0);

            segmented->setSegmentEnabled(1, true);
            CHECK(segmented->isSegmentEnabled(1));
            CHECK(segmented->segmentToolTip(1) == QStringLiteral("normal tip B"));
            ClickSegment(*segmented, 1);
            CHECK(segmented->currentIndex() == 1);
            QTest::keyClick(segmented.get(), Qt::Key_Left);
            CHECK(segmented->currentIndex() == 0);
            QTest::keyClick(segmented.get(), Qt::Key_Right);
            CHECK(segmented->currentIndex() == 1);
        }

        // 悬停提示：禁用段显示原因，原因为空退回常规提示，启用时清掉原因，禁用期间改常规提示不影响显示。
        void TestDisabledToolTips()
        {
            ApplyTheme(false);
            auto segmented = MakeSegmented({ QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C") });
            segmented->setSegmentToolTip(0, QStringLiteral("normal tip A"));
            segmented->setSegmentToolTip(1, QStringLiteral("normal tip B"));
            const QPoint centerOne = segmented->segmentRect(1).center();
            const QPoint centerZero = segmented->segmentRect(0).center();

            // 启用时：常规提示（经纯文本转义）。
            CHECK(TipShownFor(segmented.get(), centerOne) == ks::ui::hexview_format::PlainToolTip(QStringLiteral("normal tip B")));

            // 禁用并给原因：显示原因；accessor 与真实 ToolTip 事件一致；其它段不受影响。
            segmented->setSegmentEnabled(1, false, QStringLiteral("reason: driver not loaded <b>"));
            CHECK(segmented->segmentToolTip(1) == QStringLiteral("reason: driver not loaded <b>"));
            CHECK(TipShownFor(segmented.get(), centerOne)
                == ks::ui::hexview_format::PlainToolTip(QStringLiteral("reason: driver not loaded <b>")));
            CHECK(TipShownFor(segmented.get(), centerZero) == ks::ui::hexview_format::PlainToolTip(QStringLiteral("normal tip A")));

            // 禁用期间改常规提示：显示的仍是原因；启用后立即换成新的常规提示。
            segmented->setSegmentToolTip(1, QStringLiteral("new normal tip B"));
            CHECK(segmented->segmentToolTip(1) == QStringLiteral("reason: driver not loaded <b>"));
            segmented->setSegmentEnabled(1, true, QStringLiteral("ignored while enabling"));
            CHECK(segmented->segmentToolTip(1) == QStringLiteral("new normal tip B"));
            CHECK(TipShownFor(segmented.get(), centerOne) == ks::ui::hexview_format::PlainToolTip(QStringLiteral("new normal tip B")));

            // 再次禁用但不给原因：退回常规提示，不沿用上一次的原因，也不沿用启用时传入的"被忽略"的文字。
            segmented->setSegmentEnabled(1, false);
            CHECK(segmented->segmentToolTip(1) == QStringLiteral("new normal tip B"));
            CHECK(TipShownFor(segmented.get(), centerOne) == ks::ui::hexview_format::PlainToolTip(QStringLiteral("new normal tip B")));

            // 原因被改写：重复禁用同一段、给新原因，立即生效。
            segmented->setSegmentEnabled(1, false, QStringLiteral("second reason"));
            CHECK(segmented->segmentToolTip(1) == QStringLiteral("second reason"));
            segmented->setSegmentEnabled(1, false, QStringLiteral("third reason"));
            CHECK(segmented->segmentToolTip(1) == QStringLiteral("third reason"));

            // 没有常规提示、有原因的禁用段也能弹出原因；没有提示的启用段不弹。
            segmented->setSegmentEnabled(2, false, QStringLiteral("reason C"));
            CHECK(TipShownFor(segmented.get(), segmented->segmentRect(2).center())
                == ks::ui::hexview_format::PlainToolTip(QStringLiteral("reason C")));
            segmented->setSegmentEnabled(2, true);
            CHECK(segmented->segmentToolTip(2).isEmpty());
            CHECK(TipShownFor(segmented.get(), segmented->segmentRect(2).center()).isEmpty());

            // 越界读取仍是空串。
            CHECK(segmented->segmentToolTip(-1).isEmpty());
            CHECK(segmented->segmentToolTip(3).isEmpty());
        }

        // 代码通路：setCurrentIndex 仍可选中禁用段；禁用当前段不会替调用方换走，也不发信号。
        void TestProgrammaticSelection()
        {
            ApplyTheme(false);
            auto segmented = MakeSegmented({ QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C") });
            QSignalSpy spy(segmented.get(), &HexViewSegmented::currentIndexChanged);

            // 禁用当前段 0：当前段不变、没有信号（"记住上次通道但此刻不可用"要靠这一点）。
            segmented->setSegmentEnabled(0, false, QStringLiteral("unavailable"));
            CHECK(segmented->currentIndex() == 0);
            CHECK(spy.count() == 0);

            // 代码选中另一个禁用段：成功并发信号。
            segmented->setSegmentEnabled(2, false, QStringLiteral("unavailable"));
            segmented->setCurrentIndex(2);
            CHECK(segmented->currentIndex() == 2);
            CHECK(spy.count() == 1);
            CHECK(spy.count() == 1 && spy.at(0).at(0).toInt() == 2);

            // 重复选中同一段不再发信号；越界仍被忽略。
            segmented->setCurrentIndex(2);
            segmented->setCurrentIndex(-1);
            segmented->setCurrentIndex(3);
            CHECK(segmented->currentIndex() == 2);
            CHECK(spy.count() == 1);

            // 用户点击禁用段仍无效：当前段（禁用的 2）不被改走。
            ClickSegment(*segmented, 0);
            CHECK(segmented->currentIndex() == 2);
            CHECK(spy.count() == 1);
        }

        // 悬停光标：禁用段上箭头，可用段与离开后手形；段状态在悬停期间变化时立即跟随。
        void TestHoverCursor()
        {
            ApplyTheme(false);
            auto segmented = MakeSegmented({ QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C") });
            CHECK(segmented->cursor().shape() == Qt::PointingHandCursor);
            segmented->setSegmentEnabled(1, false);

            SendHover(segmented.get(), segmented->segmentRect(1).center());
            CHECK(segmented->cursor().shape() == Qt::ArrowCursor);
            SendHover(segmented.get(), segmented->segmentRect(2).center());
            CHECK(segmented->cursor().shape() == Qt::PointingHandCursor);
            SendHover(segmented.get(), segmented->segmentRect(1).center());
            CHECK(segmented->cursor().shape() == Qt::ArrowCursor);
            SendHoverLeave(segmented.get());
            CHECK(segmented->cursor().shape() == Qt::PointingHandCursor);

            // 鼠标停在段 2 上时把它禁用，光标立刻变箭头；再启用又变回手形。
            SendHover(segmented.get(), segmented->segmentRect(2).center());
            CHECK(segmented->cursor().shape() == Qt::PointingHandCursor);
            segmented->setSegmentEnabled(2, false);
            CHECK(segmented->cursor().shape() == Qt::ArrowCursor);
            segmented->setSegmentEnabled(2, true);
            CHECK(segmented->cursor().shape() == Qt::PointingHandCursor);
        }

        // 像素实测（深浅两套主题）：禁用段文字是禁用灰、选中的禁用段底色权重减半、禁用段没有悬停高亮。
        void TestDisabledPaint(bool dark)
        {
            ApplyTheme(dark);

            // 用很大的字号：粗笔画里一定有"覆盖率 100%"的像素，这些像素的颜色就是墨色本身，可以精确比对。
            auto segmented = MakeSegmented({ QStringLiteral("HHH"), QStringLiteral("HHH"), QStringLiteral("HHH") });
            QFont bigFont = segmented->font();
            bigFont.setPixelSize(30);
            segmented->setFont(bigFont);
            segmented->resize(segmented->sizeHint());

            const QColor sentinel(255, 0, 255);
            const QColor primaryText = KswordTheme::TextPrimaryColor();
            const QColor disabledText = KswordTheme::TextDisabledColor();
            const QColor surface = KswordTheme::SurfaceAltColor();
            const QColor accent = KswordTheme::PrimaryAccentColor();
            const QColor borderStrong = KswordTheme::BorderStrongColor();

            // 前置：两种墨色在这套主题里彼此足够不同，下面按颜色计数才有区分力。
            CHECK(ColorDistance(primaryText, disabledText) > 24);

            // 基线（全部启用）：段 0 选中，段 1、2 未选中。
            const QImage enabledImage = RenderOnSentinel(*segmented, sentinel);
            const QRect rectOne = segmented->segmentRect(1);
            const QRect rectTwo = segmented->segmentRect(2);
            CHECK(CountClose(enabledImage, rectOne, primaryText, 3) > 0);
            CHECK(CountClose(enabledImage, rectOne, disabledText, 3) == 0);
            // 选中的启用段：底板 = 表面色与强调色按权重 110 混合。
            const QColor selectedEnabledPlate = KswordTheme::BlendColors(surface, accent, 110);
            CHECK(ColorsClose(enabledImage.pixelColor(PlateSample(*segmented, 0)), selectedEnabledPlate, 2));
            // 未选中的启用段没有底板：取样点还是哨兵色。
            CHECK(ColorsClose(enabledImage.pixelColor(PlateSample(*segmented, 1)), sentinel, 2));

            // 禁用段 1 与（当前选中的）段 0。
            segmented->setSegmentEnabled(1, false, QStringLiteral("reason"));
            segmented->setSegmentEnabled(0, false, QStringLiteral("reason"));
            const QImage disabledImage = RenderOnSentinel(*segmented, sentinel);

            // 禁用段 1：文字是禁用灰（有这种颜色的像素），没有任何正文色像素；启用的段 2 仍是正文色。
            CHECK(CountClose(disabledImage, rectOne, disabledText, 3) > 0);
            CHECK(CountClose(disabledImage, rectOne, primaryText, 3) == 0);
            CHECK(CountClose(disabledImage, rectTwo, primaryText, 3) > 0);
            CHECK(CountClose(disabledImage, rectTwo, disabledText, 3) == 0);

            // 选中的禁用段 0：仍有底板，但权重减半（与启用时的底板不同、等于权重 50 的混合），文字也是禁用灰。
            const QColor selectedDisabledPlate = KswordTheme::BlendColors(surface, accent, 50);
            const QColor selectedPixel = disabledImage.pixelColor(PlateSample(*segmented, 0));
            CHECK(ColorsClose(selectedPixel, selectedDisabledPlate, 2));
            CHECK(!ColorsClose(selectedPixel, selectedEnabledPlate, 2));
            CHECK(!ColorsClose(selectedPixel, sentinel, 2));
            CHECK(CountClose(disabledImage, segmented->segmentRect(0), disabledText, 3) > 0);
            CHECK(CountClose(disabledImage, segmented->segmentRect(0), primaryText, 3) == 0);

            // 悬停高亮：悬停在启用的未选中段 2 上有底板；悬停在禁用的段 1 上没有（还是哨兵色）。
            const QColor hoverPlate = KswordTheme::BlendColors(surface, borderStrong, 90);
            SendHover(segmented.get(), rectTwo.center());
            const QImage hoverEnabled = RenderOnSentinel(*segmented, sentinel);
            CHECK(ColorsClose(hoverEnabled.pixelColor(PlateSample(*segmented, 2)), hoverPlate, 2));
            SendHover(segmented.get(), rectOne.center());
            const QImage hoverDisabled = RenderOnSentinel(*segmented, sentinel);
            CHECK(ColorsClose(hoverDisabled.pixelColor(PlateSample(*segmented, 1)), sentinel, 2));
            CHECK(ColorsClose(hoverDisabled.pixelColor(PlateSample(*segmented, 2)), sentinel, 2));
            SendHoverLeave(segmented.get());

            // 重新启用后外观恢复：段 1 回到正文色、段 0 的底板回到权重 110。
            segmented->setSegmentEnabled(1, true);
            segmented->setSegmentEnabled(0, true);
            const QImage restoredImage = RenderOnSentinel(*segmented, sentinel);
            CHECK(CountClose(restoredImage, rectOne, primaryText, 3) > 0);
            CHECK(CountClose(restoredImage, rectOne, disabledText, 3) == 0);
            CHECK(ColorsClose(restoredImage.pixelColor(PlateSample(*segmented, 0)), selectedEnabledPlate, 2));
        }
    }

    // 入口：禁用段的全部验证，单独报告这一组的断言数与失败数。
    void RunSegmentedTests()
    {
        const int checksBefore = g_checks;
        const int failuresBefore = g_failures;
        TestDefaultsAndBounds();
        TestClickOnDisabledSegment();
        TestKeyboardSkipsDisabledSegments();
        TestAllSegmentsDisabled();
        TestReenableRestores();
        TestDisabledToolTips();
        TestProgrammaticSelection();
        TestHoverCursor();
        TestDisabledPaint(false);
        TestDisabledPaint(true);
        ApplyTheme(false);
        std::cout << "segmented tests: " << (g_checks - checksBefore) << " checks, "
                  << (g_failures - failuresBefore) << " failures" << std::endl;
    }
}
