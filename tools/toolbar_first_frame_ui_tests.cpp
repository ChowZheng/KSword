// Exercises the real header-only ToolbarMetrics with actual Qt layout/paint events.
#include "UI/ToolbarMetrics.h"
#include <QApplication>
#include <QCheckBox>
#include <QRadioButton>
#include <QStyleFactory>
#include <cstdio>
#include <functional>

namespace {
int checks = 0, failures = 0;
QString stage;
void check(bool ok, const char* message)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL [%s] %s\n", qPrintable(stage), message); }
}
struct Snapshot {
    QList<QRect> geometry;
    QList<QSize> icons;
    int spacing = 0;
    bool operator==(const Snapshot& other) const { return geometry == other.geometry && icons == other.icons && spacing == other.spacing; }
};
class PaintCapture final : public QObject {
public:
    QHBoxLayout* row;
    QList<QWidget*> controls;
    bool armed = false, painted = false;
    int styleEvents = 0;
    Snapshot first;
    std::function<void()> prepare;
    Snapshot capture() const {
        Snapshot result; result.spacing = row->spacing();
        for (QWidget* control : controls) {
            result.geometry.append(control->geometry());
            if (auto* button = qobject_cast<QAbstractButton*>(control)) result.icons.append(button->iconSize());
        }
        return result;
    }
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::StyleChange) ++styleEvents;
        if (event->type() == QEvent::UpdateRequest && prepare) {
            // Model the new preparation boundary: font setters return through
            // each widget's changeEvent before the owner settles its layout.
            // Mutating fonts inside a Paint event is deliberately not supported.
            auto callback = std::move(prepare); callback();
            row->activate();
        }
        if (event->type() == QEvent::Paint && armed) {
            armed = false;
            first = capture(); painted = true;
        }
        return false;
    }
    void arm(std::function<void()> callback = {}) { armed = true; painted = false; prepare = std::move(callback); }
};
void flush() { for (int i = 0; i != 12; ++i) QApplication::processEvents(); }
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QWidget host; host.resize(1400, 240);
    auto* row = new QHBoxLayout(&host);
    QToolButton tool(&host); tool.setToolButtonStyle(Qt::ToolButtonIconOnly);
    QPushButton icon(&host), text(QStringLiteral("Action"), &host);
    QLineEdit input(&host); QComboBox combo(&host); combo.addItem(QStringLiteral("Choice"));
    QCheckBox checkbox(QStringLiteral("Check"), &host); QRadioButton radio(QStringLiteral("Radio"), &host);
    QPixmap glyph(16, 16); glyph.fill(Qt::blue); tool.setIcon(QIcon(glyph)); icon.setIcon(QIcon(glyph)); icon.setFixedSize(28, 28);
    QList<QWidget*> controls{&tool, &icon, &text, &input, &combo, &checkbox, &radio};
    for (QWidget* widget : controls) row->addWidget(widget);
    const QString checkStyle = QStringLiteral("QCheckBox{color:#123456;}");
    const QString radioStyle = QStringLiteral("QRadioButton{color:#654321;}");
    checkbox.setStyleSheet(checkStyle); radio.setStyleSheet(radioStyle);
    ks::ui::NormalizeToolbarRow(row, 8);
    PaintCapture capture; capture.row = row; capture.controls = controls;
    host.installEventFilter(&capture);
    for (QWidget* widget : controls) widget->installEventFilter(&capture);
    const auto verify = [&] {
        check(capture.painted, "no real first Paint event was observed");
        const Snapshot final = capture.capture();
        check(capture.first == final, "first painted geometry differs from the geometry after event draining");
        if (!(capture.first == final)) {
            for (qsizetype i = 0; i < final.geometry.size(); ++i) {
                const QRect a = capture.first.geometry.value(i), b = final.geometry[i];
                std::fprintf(stderr, "  control %lld first=(%d,%d %dx%d) final=(%d,%d %dx%d)\n",
                    static_cast<long long>(i), a.x(), a.y(), a.width(), a.height(), b.x(), b.y(), b.width(), b.height());
            }
        }
        int height = 28;
        for (QWidget* widget : controls) height = std::max(height, QFontMetrics(widget->font()).height() + 12);
        for (QWidget* widget : controls) check(widget->height() == height, "row control height does not fit the largest actual font");
        check(tool.width() == tool.height(), "tool icon lost its square geometry");
        check(icon.width() == icon.height(), "push icon lost its square geometry after a later font change");
        if (icon.width() != icon.height())
            std::fprintf(stderr, "  push icon=%dx%d font-row-height=%d icon-role=%d\n", icon.width(), icon.height(), height,
                icon.property("ksword_toolbar_icon_only").toBool());
        check(row->spacing() == 8, "normalization changed the declared row spacing");
        check(checkbox.styleSheet() == checkStyle && radio.styleSheet() == radioStyle,
            "checkbox/radio received irrelevant QPushButton geometry QSS");
        const QString styles = tool.styleSheet() + icon.styleSheet() + text.styleSheet();
        const int styleEvents = capture.styleEvents;
        flush();
        check(capture.styleEvents == styleEvents && tool.styleSheet() + icon.styleSheet() + text.styleSheet() == styles,
            "steady controls kept changing style or appending duplicate geometry rules");
    };
    stage = QStringLiteral("font-before-show");
    QFont large = text.font(); large.setPixelSize(28); text.setFont(large);
    capture.arm(); host.show(); host.repaint(); flush(); verify();
    stage = QStringLiteral("larger-font-before-reshow");
    host.hide(); large.setPixelSize(40); text.setFont(large);
    capture.arm(); host.show(); host.repaint(); flush(); verify();
    stage = QStringLiteral("css-font-before-reshow");
    host.hide(); input.setStyleSheet(QStringLiteral("QLineEdit{font-size:48px;}"));
    capture.arm(); host.show(); host.repaint(); flush(); verify();
    stage = QStringLiteral("theme-update-request-before-paint");
    capture.arm([&] { QFont font = combo.font(); font.setPixelSize(56); combo.setFont(font); });
    host.update(); QApplication::sendPostedEvents(&host, QEvent::UpdateRequest); flush(); verify();
    stage = QStringLiteral("repeated-font-style-notifications");
    for (int i = 0; i < 8; ++i) { QEvent event(QEvent::FontChange); QApplication::sendEvent(&text, &event); }
    capture.arm(); host.repaint(); flush(); verify();
    std::printf("TOOLBAR_FIRST_FRAME_CHECKS=%d FAILURES=%d REAL_QTWIDGETS=1 MAIN_GUI=0\n", checks, failures);
    return failures ? 1 : 0;
}
