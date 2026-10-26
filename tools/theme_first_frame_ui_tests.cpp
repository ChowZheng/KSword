// Actual ThemeBinding queue/window events and Qt layouts; no KSword business UI.
#include "UI/ThemeBinding.h"
#include "UI/MainSurfaceStyle.h"
#include "UI/GroupBoxStyle.h"
#include "UI/ToolbarMetrics.h"
#include "UI/SettingsPresentation.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QGroupBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyleFactory>
#include <QTableView>
#include <QVBoxLayout>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

// The fixture has no MainWindow/ADS surface or standalone-input role. The
// registry, queue, callback lifetimes, QWidget events and layouts are real.
namespace ks::ui
{
    bool UsesMainSurfaceHierarchy(const QWidget*) { return false; }
    bool UsesTransparentMainSurface(const QWidget*) { return false; }
    MainSurfaceColors MainWindowSurfaceColors() { return {}; }
    QString MainDataSurfaceStyleColor(const QColor& color, bool) { return color.name(); }
    QString WithStandaloneInputGeometry(QString style) { return style; }
    QString WithParameterInputGeometry(QString style) { return style; }
}

namespace
{
    int checks = 0;
    void require(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            std::cerr << "THEME_FIRST_FRAME_FAIL: " << message << '\n';
            std::exit(1);
        }
    }
    void pump()
    {
        for (int pass = 0; pass < 12; ++pass) QApplication::processEvents();
    }
    class PaintGeometry final : public QObject
    {
    public:
        PaintGeometry(QWidget* owner, std::function<QRect()> read)
            : m_read(std::move(read)) { owner->installEventFilter(this); }
        std::vector<QRect> frames;
    protected:
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (event->type() == QEvent::Paint) frames.push_back(m_read());
            return false;
        }
    private:
        std::function<QRect()> m_read;
    };
    class CloseOnLayout final : public QObject
    {
    public:
        explicit CloseOnLayout(QWidget* owner) : m_owner(owner) { owner->installEventFilter(this); }
        bool armed = false;
    protected:
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (armed && event->type() == QEvent::LayoutRequest)
            {
                armed = false;
                delete m_owner.data();
                return true;
            }
            return false;
        }
    private:
        QPointer<QWidget> m_owner;
    };
    void stable(const PaintGeometry& probe, QRect final, const char* message)
    {
        require(!probe.frames.empty(), "an actual window Paint was observed");
        for (const QRect& frame : probe.frames) require(frame == final, message);
    }
    void slowBinding(QWidget& owner)
    {
        // Keep one ordinary queued theme callback over the scheduler's slice.
        // This makes the first window paint arrive with another binding pending.
        ks::ui::BindWidgetTheme(&owner, [] {
            QElapsedTimer elapsed;
            elapsed.start();
            while (elapsed.elapsed() < 8) {}
        });
    }
    void groupGeometry(bool backlog, bool explicitPreparation, bool dialog = false)
    {
        QWidget queued;
        if (backlog) slowBinding(queued);
        std::unique_ptr<QWidget> owner = dialog ? std::make_unique<QDialog>() : std::make_unique<QWidget>();
        QWidget& host = *owner;
        host.resize(480, 220);
        auto* outer = new QVBoxLayout(&host);
        auto* group = new QGroupBox(QStringLiteral("Section"), &host);
        outer->addWidget(group);
        auto* inner = new QVBoxLayout(group);
        inner->setContentsMargins(25, 25, 25, 25);
        auto* button = new QPushButton(QStringLiteral("Action"), group);
        inner->addWidget(button);
        int calls = 0;
        ks::ui::BindWidgetTheme(group, [group, &calls] {
            ++calls;
            group->setStyleSheet(QStringLiteral(
                "QGroupBox{padding:45px 16px 16px 16px;border:0;}"));
            ks::ui::ConfigureGroupBoxLayout(group);
        });
        require(calls == 0, "registration does not run callbacks inside construction");
        host.show();
        require(calls == 0, "initial Show does not run an unknown callback");
        PaintGeometry geometry(group, [button] { return button->geometry(); });
        if (explicitPreparation) ks::ui::PrepareWidgetThemeBindingsForShow(&host);
        pump();
        stable(geometry, button->geometry(), "group content has final geometry on its first frame");
        require(calls == 1, "first preparation and the old queue consume one callback");
        host.update();
        pump();
        require(calls == 1, "steady paints do not rerun a completed theme binding");
    }
    void lateVisibleGroup()
    {
        QWidget host;
        host.resize(480, 220);
        auto* outer = new QVBoxLayout(&host);
        host.show();
        pump();
        auto* group = new QGroupBox(QStringLiteral("Late section"), &host);
        auto* inner = new QVBoxLayout(group);
        inner->setContentsMargins(25, 25, 25, 25);
        auto* action = new QPushButton(QStringLiteral("Action"), group);
        inner->addWidget(action);
        ks::ui::BindWidgetTheme(group, [group] {
            group->setStyleSheet(QStringLiteral("QGroupBox{padding:45px 16px 16px 16px;border:0;}"));
            ks::ui::ConfigureGroupBoxLayout(group);
        });
        outer->addWidget(group);
        group->show();
        outer->activate();
        PaintGeometry geometry(group, [action] { return action->geometry(); });
        host.repaint();
        pump();
        stable(geometry, action->geometry(), "a late visible group is prepared before an immediate repaint");
    }
    template<class Area> void viewportGeometry(bool ownStyle, bool backlog)
    {
        QWidget queued;
        if (backlog) slowBinding(queued);
        QWidget host;
        host.resize(480, 260);
        auto* outer = new QVBoxLayout(&host);
        auto* group = new QGroupBox(QStringLiteral("Section"), &host);
        outer->addWidget(group);
        auto* inner = new QVBoxLayout(group);
        inner->setContentsMargins(25, 25, 25, 25);
        auto* area = new Area;
        inner->addWidget(area);
        if constexpr (std::is_same_v<Area, QScrollArea>)
        {
            auto* content = new QWidget;
            content->resize(1000, 1000);
            area->setWidget(content);
        }
        else
        {
            area->setModel(new QStandardItemModel(20, 20, area));
        }
        QWidget* owner = ownStyle ? static_cast<QWidget*>(area) : group;
        ks::ui::BindWidgetTheme(owner, [group, area, ownStyle] {
            if (ownStyle)
                area->setStyleSheet(QStringLiteral("QAbstractScrollArea{border:0;}"
                    "QScrollBar:vertical{width:0px;}QScrollBar:horizontal{height:0px;}"));
            else
            {
                group->setStyleSheet(QStringLiteral("QGroupBox{padding:45px 16px 16px 16px;border:0;}"));
                ks::ui::ConfigureGroupBoxLayout(group);
            }
        });
        PaintGeometry geometry(&host, [area] { return area->viewport()->geometry(); });
        PaintGeometry viewport(area->viewport(), [area] { return area->viewport()->geometry(); });
        host.show();
        pump();
        stable(geometry, area->viewport()->geometry(), "scroll/table viewport is settled on the first window frame");
        // A QScrollArea's content can cover its viewport entirely; QTableView
        // paints that viewport itself, so also check its actual paint stream.
        if constexpr (std::is_same_v<Area, QTableView>)
            stable(viewport, area->viewport()->geometry(), "table viewport never paints an intermediate size");
    }
    void toolbarGeometry()
    {
        QWidget host;
        host.resize(640, 180);
        auto* row = new QHBoxLayout(&host);
        auto* action = new QPushButton(QStringLiteral("Action"), &host);
        auto* field = new QLineEdit(&host);
        auto* choice = new QComboBox(&host);
        choice->addItem(QStringLiteral("Choice"));
        row->addWidget(action);
        row->addWidget(field);
        row->addWidget(choice);
        ks::ui::NormalizeToolbarRow(row);
        ks::ui::BindWidgetTheme(&host, [choice] {
            QFont font = choice->font();
            font.setPixelSize(56);
            choice->setFont(font);
        });
        PaintGeometry actionGeometry(&host, [action] { return action->geometry(); });
        PaintGeometry fieldGeometry(&host, [field] { return field->geometry(); });
        PaintGeometry choiceGeometry(&host, [choice] { return choice->geometry(); });
        host.show();
        pump();
        stable(actionGeometry, action->geometry(), "toolbar action height is final before first paint");
        stable(fieldGeometry, field->geometry(), "toolbar field height is final before first paint");
        stable(choiceGeometry, choice->geometry(), "toolbar combo height is final before first paint");
        require(action->height() == field->height() && field->height() == choice->height(),
            "actual ToolbarRowMetrics keeps one common row height");
        require(choice->height() >= QFontMetrics(choice->font()).height() + 12,
            "the row uses its final font metrics");
    }
    void settingsFormGeometry(bool backlog, bool dialog)
    {
        QWidget queued;
        if (backlog) slowBinding(queued);
        std::unique_ptr<QWidget> owner = dialog ? std::make_unique<QDialog>() : std::make_unique<QWidget>();
        QWidget& host = *owner;
        host.resize(760, 300);
        auto* outer = new QVBoxLayout(&host);
        auto* panel = new QWidget(&host);
        outer->addWidget(panel);
        auto* form = new QFormLayout(panel);
        // This is the normal "style then populate" construction order. The
        // actual SettingsFormMetrics observes the newly added row on Show.
        ks::ui::StyleSettingsForm(form);
        auto* label = new QLabel(QStringLiteral("Option"), panel);
        auto* field = new QLineEdit(panel);
        form->addRow(label, field);
        ks::ui::BindWidgetTheme(panel, [field] {
            QFont font = field->font();
            font.setPixelSize(56);
            field->setFont(font);
        });
        PaintGeometry labelGeometry(&host, [label] { return label->geometry(); });
        PaintGeometry fieldGeometry(&host, [field] { return field->geometry(); });
        PaintGeometry rowMinimums(&host, [label, field] {
            return QRect(0, 0, label->minimumHeight(), field->minimumHeight());
        });
        host.show();
        pump();
        stable(labelGeometry, label->geometry(), "settings label has final row geometry before first paint");
        stable(fieldGeometry, field->geometry(), "settings field has final row geometry before first paint");
        stable(rowMinimums, QRect(0, 0, label->minimumHeight(), field->minimumHeight()),
            "settings row metrics consume the initial theme font before first paint");
        require(label->minimumHeight() == field->minimumHeight()
            && field->minimumHeight() >= QFontMetrics(field->font()).height() + 12,
            "actual SettingsFormMetrics uses the initial theme's final font");
    }
    void hiddenAndPopupBoundaries()
    {
        QWidget host;
        auto* outer = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        outer->addWidget(stack);
        auto* current = new QWidget;
        auto* hidden = new QWidget;
        stack->addWidget(current);
        stack->addWidget(hidden);
        int currentCalls = 0, hiddenCalls = 0;
        ks::ui::BindWidgetTheme(current, [&] { ++currentCalls; });
        ks::ui::BindWidgetTheme(hidden, [&] { ++hiddenCalls; });
        host.show();
        QEvent update(QEvent::UpdateRequest);
        QApplication::sendEvent(&host, &update);
        require(currentCalls == 1 && hiddenCalls == 0,
            "window preparation does not consume hidden stacked pages");
        pump();

        for (Qt::WindowType type : {Qt::Popup, Qt::ToolTip})
        {
            QWidget popup(nullptr, type);
            int calls = 0;
            ks::ui::BindWidgetTheme(&popup, [&] { ++calls; });
            popup.show();
            QApplication::sendEvent(&popup, &update);
            require(calls == 0, "Popup/ToolTip retains queued preparation outside its Show stack");
            pump();
            require(calls == 1, "Popup/ToolTip still receives its existing queued refresh");
        }
    }
    void refreshAndLifetimes()
    {
        QWidget host;
        int calls = 0;
        ks::ui::BindWidgetTheme(&host, [&] { ++calls; });
        host.show();
        pump();
        require(calls == 1, "ordinary first callback runs once");
        ks::ui::RefreshWidgetThemeBindings();
        QEvent update(QEvent::UpdateRequest);
        QApplication::sendEvent(&host, &update);
        require(calls == 1, "window update does not turn established hot refreshes synchronous");
        pump();
        require(calls == 2, "hot refresh still drains normally");

        QPointer<QWidget> closing = new QWidget;
        ks::ui::BindWidgetTheme(closing, [closing] { delete closing.data(); });
        closing->show();
        QApplication::sendEvent(closing, &update);
        require(closing.isNull(), "a first-preparation callback can close its owner safely");
        pump();

        QWidget parent;
        QPointer<QWidget> child = new QWidget(&parent);
        int childCalls = 0;
        ks::ui::BindWidgetTheme(&parent, [child] { delete child.data(); });
        ks::ui::BindWidgetTheme(child, [&] { ++childCalls; });
        parent.show();
        QApplication::sendEvent(&parent, &update);
        require(child.isNull() && childCalls == 0, "parent-first preparation tolerates a deleted later child");
        pump();

        QPointer<QWidget> closingDuringLayout = new QWidget;
        new QVBoxLayout(closingDuringLayout);
        CloseOnLayout closeOnLayout(closingDuringLayout);
        ks::ui::BindWidgetTheme(closingDuringLayout, [closingDuringLayout, &closeOnLayout] {
            closeOnLayout.armed = true;
            QCoreApplication::postEvent(closingDuringLayout, new QEvent(QEvent::LayoutRequest));
        });
        closingDuringLayout->show();
        QApplication::sendEvent(closingDuringLayout, &update);
        require(closingDuringLayout.isNull(), "a posted layout handler can close a prepared owner safely");
        pump();
    }
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    if (argc > 1 && std::string_view(argv[1]) == "--settings-only")
    {
        for (bool backlog : {false, true})
            for (bool dialog : {false, true}) settingsFormGeometry(backlog, dialog);
        std::cout << "THEME_SETTINGS_INTEGRATION_PASS checks=" << checks << '\n';
        return 0;
    }
    for (bool backlog : {false, true})
        for (bool explicitPreparation : {false, true})
            groupGeometry(backlog, explicitPreparation);
    groupGeometry(false, false, true);
    lateVisibleGroup();
    for (bool ownStyle : {false, true})
        for (bool backlog : {false, true})
        {
            viewportGeometry<QScrollArea>(ownStyle, backlog);
            viewportGeometry<QTableView>(ownStyle, backlog);
        }
    toolbarGeometry();
    for (bool backlog : {false, true})
        for (bool dialog : {false, true}) settingsFormGeometry(backlog, dialog);
    hiddenAndPopupBoundaries();
    refreshAndLifetimes();
    std::cout << "THEME_FIRST_FRAME_PASS checks=" << checks << '\n';
    return 0;
}
