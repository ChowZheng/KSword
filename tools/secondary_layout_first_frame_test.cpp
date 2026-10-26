// Real QtWidgets geometry regression; production helpers are linked by the runner.
#include "SecondaryPageLayout.h"
#include "SettingsPresentation.h"
#include "ThemeBinding.h"
#include "FlatButtonTheme.h"
#include "PageTabMotion.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace ks::ui
{
    // The fixture isolates geometry. Real theme/binding first-frame behavior has
    // its own regression; no Windows services or business callbacks are loaded.
    bool BindWidgetTheme(QWidget* widget, std::function<void()> refresh,
        ThemePalettePolicy, bool)
    {
        widget->setProperty("test_theme_bound", true);
        refresh();
        return true;
    }
    bool HasWidgetThemeBinding(const QWidget* widget)
    { return widget->property("test_theme_bound").toBool(); }
    void ApplyFlatButtonTheme(QAbstractButton* button, FlatButtonTone tone, FlatButtonAppearance appearance)
    {
        button->setProperty("ksword_flat_button_managed", true);
        button->setProperty("ksword_flat_button_tone", static_cast<int>(tone));
        button->setProperty("ksword_flat_button_appearance", static_cast<int>(appearance));
    }
    void InstallPageTabMotion(QTabBar*, std::function<PageTabMotionColors()>) {}
}

namespace
{
    int checks = 0;
    void require(bool value, const char* message)
    {
        ++checks;
        if (!value) throw std::runtime_error(message);
    }

    using Snapshot = std::vector<int>;
    class FirstPaint final : public QObject
    {
    public:
        explicit FirstPaint(QWidget* target, std::function<Snapshot()> take)
            : QObject(target), m_take(std::move(take))
        { target->installEventFilter(this); }
        Snapshot first;
        void reset() { first.clear(); }
    protected:
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (event->type() == QEvent::Paint && first.empty()) first = m_take();
            return false;
        }
    private:
        std::function<Snapshot()> m_take;
    };

    void paint(QWidget& widget)
    {
        QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        widget.render(&image); // Synchronous real Paint, before zero-timers drain.
    }
    void settle()
    {
        for (int i = 0; i < 12; ++i)
        {
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents();
        }
    }
    Snapshot geometry(QWidget* widget)
    {
        const QRect rect = widget->geometry();
        return {rect.x(), rect.y(), rect.width(), rect.height(),
            widget->minimumWidth(), widget->minimumHeight(), widget->maximumHeight()};
    }
    Snapshot rows(QLabel* label, QWidget* field)
    {
        Snapshot result = geometry(label);
        const Snapshot other = geometry(field);
        result.insert(result.end(), other.begin(), other.end());
        return result;
    }
    void compareFrame(QWidget& window, FirstPaint& recorder, const std::function<Snapshot()>& take)
    {
        window.show();
        paint(window);
        require(!recorder.first.empty(), "first Paint was not observed");
        const Snapshot first = recorder.first;
        settle();
        paint(window);
        require(first == take(), "geometry changed after the first Paint");
    }

    void secondaryForm(bool rowsBeforeStyle)
    {
        QWidget window;
        window.resize(680, 180);
        auto* form = new QFormLayout(&window);
        auto* label = new QLabel("Name", &window);
        auto* field = new QLineEdit(&window);
        if (rowsBeforeStyle) form->addRow(label, field);
        ks::ui::StyleSecondaryForm(form, 120);
        if (!rowsBeforeStyle) form->addRow(label, field);
        FirstPaint recorder(&window, [=]() { return rows(label, field); });
        window.show();
        paint(window);
        require(label->minimumWidth() == 120, "secondary label width was deferred");
        require(field->minimumHeight() >= 28 && field->minimumHeight() == field->maximumHeight(),
            "secondary input height was deferred");
        const Snapshot first = recorder.first;
        settle();
        require(first == rows(label, field), "secondary form moved after first Paint");
        const auto childCount = form->children().size();
        ks::ui::StyleSecondaryForm(form, 140);
        require(label->minimumWidth() == 140, "re-registration did not use the new label width");
        require(form->children().size() == childCount, "re-registration created another metrics owner");
    }

    void unparentedForm()
    {
        QWidget window;
        window.resize(600, 180);
        auto* outer = new QVBoxLayout(&window);
        auto* form = new QFormLayout;
        auto* label = new QLabel("Name", &window);
        auto* field = new QLineEdit(&window);
        form->addRow(label, field);
        ks::ui::StyleSecondaryForm(form, 150);
        outer->addLayout(form);
        FirstPaint recorder(&window, [=]() { return rows(label, field); });
        compareFrame(window, recorder, [=]() { return rows(label, field); });
        require(label->minimumWidth() == 150 && field->minimumHeight() >= 28,
            "a form styled before addLayout lost its geometry");
    }

    void settingsForm()
    {
        QWidget window;
        window.resize(720, 180);
        auto* form = new QFormLayout(&window);
        ks::ui::StyleSettingsForm(form, 180); // Real settings pages register empty forms.
        auto* label = new QLabel("Label", &window);
        auto* field = new QCheckBox("Enabled", &window);
        form->addRow(label, field);
        FirstPaint recorder(&window, [=]() { return rows(label, field); });
        window.show();
        paint(window);
        require(label->minimumWidth() == 180, "settings label width was deferred");
        require(field->minimumHeight() >= 28 && label->minimumHeight() == field->minimumHeight(),
            "settings row alignment was deferred");
        const Snapshot first = recorder.first;
        settle();
        require(first == rows(label, field), "settings form moved after first Paint");
        const auto childCount = form->children().size();
        ks::ui::StyleSettingsForm(form, 200);
        require(form->children().size() == childCount && label->minimumWidth() == 200,
            "settings form re-registration was not idempotent");
        QFont font = window.font();
        font.setPointSize(20);
        window.setFont(font);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        recorder.reset();
        paint(window);
        require(field->minimumHeight() >= field->fontMetrics().height() + 12,
            "font change missed the pre-paint layout boundary");
        const Snapshot changed = recorder.first;
        settle();
        require(changed == rows(label, field), "font metrics changed after Paint");
        const Snapshot stable = rows(label, field);
        QEvent style(QEvent::StyleChange);
        QCoreApplication::sendEvent(&window, &style);
        QEvent layout(QEvent::LayoutRequest);
        QCoreApplication::sendEvent(&window, &layout);
        window.hide();
        recorder.reset();
        compareFrame(window, recorder, [=]() { return rows(label, field); });
        require(stable == rows(label, field), "unchanged Show/style/layout reapplied different settings geometry");
    }

    void footer(bool buttonBox)
    {
        QWidget window;
        window.resize(640, 140);
        auto* outer = new QVBoxLayout(&window);
        QWidget* body = buttonBox ? static_cast<QWidget*>(new QDialogButtonBox(&window)) : new QWidget(&window);
        outer->addWidget(body);
        QHBoxLayout* actions = buttonBox ? nullptr : new QHBoxLayout(body);
        ks::ui::StyleSecondaryFooter(body); // Register before adding business actions.
        auto* button = new QPushButton("Run", body);
        if (buttonBox) static_cast<QDialogButtonBox*>(body)->addButton(button, QDialogButtonBox::AcceptRole);
        else actions->addWidget(button);
        FirstPaint recorder(&window, [=]() { return geometry(button); });
        window.show();
        paint(window);
        require(button->minimumHeight() >= 28, "footer button metrics were deferred");
        require(button->property("ksword_flat_button_appearance").toInt()
            == static_cast<int>(ks::ui::FlatButtonAppearance::Solid), "footer action role was deferred");
        const Snapshot first = recorder.first;
        settle();
        require(first == geometry(button), "footer button moved after first Paint");
        QFont font = window.font();
        font.setPointSize(18);
        window.setFont(font);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        recorder.reset();
        paint(window);
        require(button->height() >= button->fontMetrics().height() + 12,
            "footer font change missed its layout boundary");
        const Snapshot changed = recorder.first;
        settle();
        require(changed == geometry(button), "footer font change moved geometry after Paint");
        if (buttonBox) require(static_cast<QDialogButtonBox*>(body)->buttonRole(button)
            == QDialogButtonBox::AcceptRole, "footer styling changed the business button role");
    }

    void dataColorFooter()
    {
        QWidget footer;
        auto* layout = new QHBoxLayout(&footer);
        auto* button = new QPushButton("Data", &footer);
        const QString style = QStringLiteral("QPushButton{background:#a02030;}");
        button->setStyleSheet(style);
        layout->addWidget(button);
        ks::ui::StyleSecondaryFooter(&footer);
        footer.show();
        paint(footer);
        settle();
        require(button->styleSheet().contains(style), "footer took ownership of a data color");
        require(!button->property("ksword_flat_button_managed").toBool(),
            "footer overwrote an explicitly owned data color");
    }

    void columns(bool wide)
    {
        QWidget window;
        window.resize(wide ? 1500 : 580, 340);
        auto* outer = new QVBoxLayout(&window);
        auto* primary = new QWidget;
        auto* secondary = new QWidget;
        primary->setFixedHeight(70);
        secondary->setFixedHeight(50);
        auto* columns = ks::ui::CreateSecondaryColumns(primary, secondary, &window, 900, 3, 2);
        outer->addWidget(columns);
        const auto take = [=]() {
            Snapshot result = geometry(primary);
            const auto other = geometry(secondary);
            result.insert(result.end(), other.begin(), other.end());
            return result;
        };
        FirstPaint recorder(&window, take);
        compareFrame(window, recorder, take);
        require(wide ? primary->y() == secondary->y() && secondary->x() > primary->x()
            : secondary->y() > primary->y() && primary->x() == secondary->x(),
            "columns used the wrong first-frame width");
        if (wide)
        {
            window.resize(580, 340);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            recorder.reset();
            paint(window);
            require(secondary->y() > primary->y(), "visible resize missed its pre-paint layout request");
            const Snapshot resized = recorder.first;
            settle();
            require(resized == take(), "visible resize moved columns after Paint");
            window.hide();
            window.resize(1500, 340);
            recorder.reset();
            compareFrame(window, recorder, take);
            require(primary->y() == secondary->y(), "reshow retained the previous narrow layout");
            window.hide();
            window.resize(580, 340);
            recorder.reset();
            compareFrame(window, recorder, take);
            require(secondary->y() > primary->y(), "reshow retained the previous wide layout");
        }
    }

    void lifetime()
    {
        for (int index = 0; index < 30; ++index)
        {
            auto* window = new QWidget;
            auto* form = new QFormLayout(window);
            ks::ui::StyleSecondaryForm(form);
            ks::ui::StyleSettingsForm(form);
            delete window; // Destroy before any constructor timer runs.
        }
        settle();
        require(true, "queued preparation lifetime");
    }
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setFont(QFont(QStringLiteral("DejaVu Sans"), 10));
    int failures = 0;
    const std::pair<const char*, std::function<void()>> cases[] = {
        {"secondary form: style before rows", []() { secondaryForm(false); }},
        {"secondary form: style after rows", []() { secondaryForm(true); }},
        {"unparented form", unparentedForm},
        {"settings form and font change", settingsForm},
        {"footer late action", []() { footer(false); }},
        {"button box late action", []() { footer(true); }},
        {"footer data-color ownership", dataColorFooter},
        {"narrow columns", []() { columns(false); }},
        {"wide columns and narrow reshow", []() { columns(true); }},
        {"queued preparation destruction", lifetime},
    };
    for (const auto& entry : cases)
    {
        try { entry.second(); }
        catch (const std::exception& error)
        { ++failures; std::cerr << entry.first << ": " << error.what() << '\n'; }
    }
    std::cout << "SECONDARY_FIRST_FRAME_CHECKS=" << checks << " FAILURES=" << failures << '\n';
    return failures ? 1 : 0;
}
