// The runner inserts the two complete production classes at this marker.
#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QRadioButton>
#include <QPushButton>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QStyleFactory>
#include <QStyleOptionButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <QTest>
#include <cstdio>
#include <memory>

// KSWORD_PARAMETER_GEOMETRY_CLASSES

namespace {
int checks = 0, failures = 0;
QString scenario;
void check(bool ok, const char* message)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL [%s] %s\n", qPrintable(scenario), message); }
}
void flush() { for (int i = 0; i != 18; ++i) QApplication::processEvents(); }
struct Snapshot {
    QRect owner;
    QList<QRect> children;
    QList<int> minimumHeights;
    bool operator==(const Snapshot& other) const {
        return owner == other.owner && children == other.children && minimumHeights == other.minimumHeights;
    }
};
class PaintTrace final : public QObject {
public:
    QWidget* owner = nullptr;
    QList<QWidget*> children;
    bool seen = false;
    int paints = 0;
    int resizes = 0;
    Snapshot first;
    Snapshot current() const {
        Snapshot result; result.owner = owner->geometry();
        for (QWidget* child : children) {
            result.children.append(QRect(child->mapTo(owner, QPoint()), child->size()));
            result.minimumHeights.append(child->minimumHeight());
        }
        return result;
    }
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Paint) { if (!seen) { first = current(); seen = true; } ++paints; }
        if (event->type() == QEvent::Resize) ++resizes;
        return false;
    }
    void reset() { seen = false; paints = 0; }
    void verify() {
        check(seen, "no actual Paint event was observed");
        const Snapshot settled = current();
        const bool stable = first == settled;
        check(stable, "natural first Paint geometry differs from settled geometry");
        if (!stable) {
            std::fprintf(stderr, "  owner first=(%d,%d %dx%d) settled=(%d,%d %dx%d) paints=%d\n",
                first.owner.x(), first.owner.y(), first.owner.width(), first.owner.height(),
                settled.owner.x(), settled.owner.y(), settled.owner.width(), settled.owner.height(), paints);
            for (qsizetype i = 0; i < settled.children.size(); ++i) {
                const QRect a = first.children.value(i), b = settled.children[i];
                std::fprintf(stderr, "  child %lld first=(%d,%d %dx%d minH=%d) settled=(%d,%d %dx%d minH=%d)\n",
                    static_cast<long long>(i), a.x(), a.y(), a.width(), a.height(), first.minimumHeights.value(i),
                    b.x(), b.y(), b.width(), b.height(), settled.minimumHeights[i]);
            }
        }
    }
};
QWidget* section(QWidget* parent, int ordinal, bool longText, int fontSize)
{
    auto* group = new QGroupBox(QStringLiteral("Section %1").arg(ordinal), parent);
    auto* layout = new QVBoxLayout(group);
    auto* label = new QLabel(longText
        ? QStringLiteral("这里是用于实际参数页面的长中文说明，测试窄窗口、宽窗口和大字体下各分区的首次布局是否稳定。")
        : QStringLiteral("A short parameter section"), group);
    label->setWordWrap(true);
    QFont font = label->font(); font.setPixelSize(fontSize); label->setFont(font);
    layout->addWidget(label);
    group->setMinimumHeight(70 + ordinal * 5);
    return group;
}
void columns(int width, bool dynamic, bool longText, int fontSize)
{
    scenario = QStringLiteral("columns-%1-%2-%3-font%4")
        .arg(width).arg(dynamic ? "dynamic" : "show").arg(longText ? "long" : "short").arg(fontSize);
    QWidget host; host.resize(width, 800);
    auto* layout = new QVBoxLayout(&host); layout->setAlignment(Qt::AlignTop);
    if (dynamic) { host.show(); flush(); }
    QWidget *a = section(&host, 0, longText, fontSize), *b = section(&host, 1, longText, fontSize),
        *c = section(&host, 2, longText, fontSize), *d = section(&host, 3, longText, fontSize);
    auto* widget = new ParameterColumns(&host, {a, c}, {b, d}, {a, b, c, d}, 300);
    layout->addWidget(widget);
    PaintTrace trace; trace.owner = widget; trace.children = {a, b, c, d}; widget->installEventFilter(&trace);
    if (dynamic) widget->show(); else host.show();
    flush(); trace.verify();
    check(a->parentWidget() == c->parentWidget(), "primary sections lost their common column");
    check((width >= 638) == (a->parentWidget() != b->parentWidget()), "settled wide/narrow column choice is incorrect");
    scenario += QStringLiteral("-reshow-opposite-width");
    host.hide(); host.resize(width < 638 ? 1040 : 360, 800); trace.reset(); host.show(); flush(); trace.verify();
    scenario += QStringLiteral("-visible-resize-back");
    trace.reset(); host.resize(width, 800); flush(); trace.verify();
}
void choices(int width, bool dynamic, bool radio, bool longText, int fontSize)
{
    scenario = QStringLiteral("choice-%1-%2-%3-%4-font%5").arg(width)
        .arg(dynamic ? "dynamic" : "show", radio ? "radio" : "check", longText ? "long" : "short").arg(fontSize);
    QWidget host; host.resize(width, 600);
    auto* layout = new QVBoxLayout(&host); layout->setAlignment(Qt::AlignTop);
    if (dynamic) { host.show(); flush(); }
    std::unique_ptr<QAbstractButton> button = radio
        ? std::unique_ptr<QAbstractButton>(new QRadioButton(&host))
        : std::unique_ptr<QAbstractButton>(new QCheckBox(&host));
    button->setText(longText ? QStringLiteral("用于验证参数选择项的长中文说明：点击多行正文仍然切换同一个真实复选框或单选框，并且首次绘制应有足够的文字高度，不应在显示后再次跳动或被截断。")
        : QStringLiteral("A short choice"));
    QFont font = button->font(); font.setPixelSize(fontSize); button->setFont(font);
    layout->addWidget(button.get());
    new ParameterChoiceWrap(button.get());
    PaintTrace trace; trace.owner = button.get(); trace.children = {button.get()}; button->installEventFilter(&trace);
    if (dynamic) button->show(); else host.show();
    flush(); trace.verify();
    check(button->minimumHeight() >= 28, "choice text did not receive its measured minimum height");
    const bool initial = button->isChecked();
    const QPoint labelPoint(button->width() - 8, button->height() - 8);
    QTest::mouseClick(host.windowHandle(), Qt::LeftButton, Qt::NoModifier, button->mapTo(&host, labelPoint), 1);
    flush();
    check(button->isChecked() != initial, "real mouse input on wrapped label did not activate the original button");
    scenario += QStringLiteral("-reshow-opposite-width");
    host.hide(); host.resize(width < 500 ? 1000 : 280, 600); trace.reset(); host.show(); flush(); trace.verify();
    scenario += QStringLiteral("-visible-larger-font");
    trace.reset(); font.setPixelSize(fontSize + 10); button->setFont(font); flush(); trace.verify();
}

void choiceInteractionAndLifetime(bool radio)
{
    scenario = radio ? QStringLiteral("radio-native-keys-repeat-destruction")
        : QStringLiteral("checkbox-native-keys-repeat-destruction");
    // Pin the platform's actual sole-radio behavior instead of assuming that
    // every auto-exclusive radio has a multi-button group.
    bool nativeAfterSecondSpace = false;
    {
        QWidget nativeHost; nativeHost.resize(220, 100);
        std::unique_ptr<QAbstractButton> native = radio
            ? std::unique_ptr<QAbstractButton>(new QRadioButton(&nativeHost))
            : std::unique_ptr<QAbstractButton>(new QCheckBox(&nativeHost));
        native->setText(QStringLiteral("Native reference")); native->setGeometry(10, 10, 190, 40);
        nativeHost.show(); nativeHost.activateWindow(); flush(); native->setFocus(Qt::TabFocusReason);
        QTest::keyClick(native.get(), Qt::Key_Space, Qt::NoModifier, 1);
        QTest::keyClick(native.get(), Qt::Key_Space, Qt::NoModifier, 1); flush();
        nativeAfterSecondSpace = native->isChecked();
    }
    QWidget host; host.resize(350, 400);
    auto* layout = new QVBoxLayout(&host); layout->setAlignment(Qt::AlignTop);
    QPushButton previous(QStringLiteral("Previous"), &host); layout->addWidget(&previous);
    QAbstractButton* choice = radio ? static_cast<QAbstractButton*>(new QRadioButton(&host))
        : static_cast<QAbstractButton*>(new QCheckBox(&host));
    choice->setText(QStringLiteral("长中文参数说明：保留真实键盘选择与对象生命周期，不创建另一份选择状态。"));
    QFont font = choice->font(); font.setPixelSize(24); choice->setFont(font);
    layout->addWidget(choice);
    auto* wrapper = new ParameterChoiceWrap(choice);
    PaintTrace trace; trace.owner = choice; trace.children = {choice}; choice->installEventFilter(&trace);
    QWidget::setTabOrder(&previous, choice);
    host.show(); host.activateWindow(); flush();
    previous.setFocus(Qt::OtherFocusReason);
    QTest::keyClick(&previous, Qt::Key_Tab, Qt::NoModifier, 1); flush();
    check(choice->hasFocus(), "Tab did not reach the original wrapped choice");
    int clicks = 0;
    QObject::connect(choice, &QAbstractButton::clicked, &host, [&] { ++clicks; });
    QTest::keyClick(choice, Qt::Key_Space, Qt::NoModifier, 1); flush();
    check(choice->isChecked(), "Space did not select the wrapped native button");
    QTest::keyClick(choice, Qt::Key_Space, Qt::NoModifier, 1); flush();
    check(choice->isChecked() == nativeAfterSecondSpace, "wrapper changed native checkbox toggle or radio exclusivity");
    check(clicks == 2, "wrapper lost or duplicated native keyboard click signals");
    const Snapshot stable = trace.current();
    const int resizes = trace.resizes;
    const QString style = choice->styleSheet();
    for (int i = 0; i != 8; ++i) QCoreApplication::postEvent(choice, new QEvent(QEvent::LayoutRequest));
    flush();
    check(trace.current() == stable && trace.resizes == resizes,
        "duplicate metric requests changed stable geometry or generated a resize cycle");
    check(choice->styleSheet() == style, "choice measurements changed the native style sheet");
    // Queue a real font-driven metric request, then close its owning control
    // before delivery. QObject must remove the pending event with the wrapper.
    font.setPixelSize(32); choice->setFont(font);
    const QPointer<QAbstractButton> liveChoice(choice);
    const QPointer<ParameterChoiceWrap> liveWrapper(wrapper);
    delete choice; flush();
    check(!liveChoice && !liveWrapper && layout->count() == 1,
        "destroyed wrapped choice retained queued geometry work or a layout item");
}

void hiddenSectionsAndLifetime()
{
    scenario = QStringLiteral("columns-hidden-section-width-change-destruction");
    QWidget host; host.resize(1000, 700);
    auto* layout = new QVBoxLayout(&host); layout->setAlignment(Qt::AlignTop);
    QWidget *a = section(&host, 0, false, 14), *b = section(&host, 1, false, 14),
        *c = section(&host, 2, false, 14), *d = section(&host, 3, false, 14);
    d->setMinimumHeight(220);
    auto* columns = new ParameterColumns(&host, {a, c}, {b, d}, {a, b, c, d}, 300);
    layout->addWidget(columns);
    PaintTrace trace; trace.owner = columns; trace.children = {a, b, c, d}; columns->installEventFilter(&trace);
    host.show(); flush();
    const int visibleHeight = columns->height();
    trace.reset(); d->hide(); flush(); trace.verify();
    check(d->isHidden() && columns->height() < visibleHeight,
        "explicitly hidden section still occupied its previous column height");
    trace.reset(); host.resize(360, 700); flush(); trace.verify();
    check(d->isHidden(), "column transition reopened a deliberately hidden section");
    host.resize(1000, 700);
    const QPointer<ParameterColumns> liveColumns(columns);
    delete columns; flush();
    check(!liveColumns && layout->count() == 0,
        "column destruction retained layout work or a dead root layout item");
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    for (bool dynamic : {false, true}) {
        for (int width : {360, 720, 1120}) for (bool longText : {false, true})
            columns(width, dynamic, longText, longText ? 24 : 14);
        for (int width : {280, 1000}) for (bool radio : {false, true}) for (bool longText : {false, true})
            choices(width, dynamic, radio, longText, longText ? 24 : 14);
    }
    choiceInteractionAndLifetime(false);
    choiceInteractionAndLifetime(true);
    hiddenSectionsAndLifetime();
    std::printf("PARAMETER_GEOMETRY_CHECKS=%d FAILURES=%d REAL_QTWIDGETS=1 MAIN_GUI=0\n", checks, failures);
    return failures ? 1 : 0;
}
