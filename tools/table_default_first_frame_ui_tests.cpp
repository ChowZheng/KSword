// Production discovery/queue/paint-gate fragments are inserted by the runner.
// The view, viewport layout, floating scrollbars and ThemeBinding are real.
// The large business action bar is represented only by its reserved strip.
#include "UI/VisibleTableWidget.h"
#include "UI/FloatingScrollbars.h"
#include "UI/ThemeBinding.h"
#include "UI/MainSurfaceStyle.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
#include <QHash>
#include <QPointer>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <new>
#include <vector>

namespace ks::ui
{
    bool AreUiAnimationsEnabled() { return false; }
    bool UsesMainSurfaceHierarchy(const QWidget*) { return false; }
    bool UsesTransparentMainSurface(const QWidget*) { return false; }
    MainSurfaceColors MainWindowSurfaceColors() { return {}; }
    QString MainDataSurfaceStyleColor(const QColor& color, bool) { return color.name(); }
    QString WithStandaloneInputGeometry(QString style) { return style; }
    QString WithParameterInputGeometry(QString style) { return style; }
    bool PreservesCustomTablePresentation(const QAbstractItemView* view)
    {
        return view->property("ksword_preserve_custom_table_presentation").toBool();
    }
}

namespace
{
    using ks::ui::TableActionBarMode;
    constexpr char kConfigurationPendingProperty[] = "ksword_table_interaction_configuration_pending";
    constexpr char kInitialConfigurationProperty[] = "ksword_table_interaction_initial_configuration_done";
    constexpr char kInitialPaintWaitingProperty[] = "ksword_table_interaction_initial_paint_waiting";
    constexpr char kInitialPaintReleasedProperty[] = "ksword_table_interaction_initial_paint_released";
    int checks = 0;
    int attempts = 0;
    std::function<void(QTableView*)> rejected;

    void require(bool ok, const char* message)
    {
        ++checks;
        if (!ok)
        {
            std::cerr << "TABLE_DEFAULT_FIRST_FRAME_FAIL: " << message << '\n';
            std::exit(1);
        }
    }
    void pump()
    {
        for (int pass = 0; pass < 16; ++pass) QApplication::processEvents();
    }
    void configureTable(QTableView* table)
    {
        ++attempts;
        if (!table || !table->model()) return;
        if (table->property("fixture_reject_configuration").toBool())
        {
            if (rejected) rejected(table);
            return;
        }
        if (table->property("fixture_configured").toBool()) return;
        table->setProperty("fixture_configured", true);
        if (auto* host = ks::ui::TableActionBarHostFor(table);
            host && ks::ui::EffectiveTableActionBarMode(table) != TableActionBarMode::None)
        {
            host->setTopActionBarHeight(32);
            auto* bar = new QFrame(table);
            bar->setObjectName("fixture_bar");
            bar->setGeometry(host->topActionBarGeometry());
            const QPointer<QFrame> target(bar);
            ks::ui::BindWidgetTheme(bar, [target] {
                if (target)
                {
                    target->setStyleSheet("QFrame{background:#445566;border:none;}");
                    target->setProperty("fixture_theme_ready", true);
                }
            });
            bar->show();
        }
        table->setProperty(kInitialConfigurationProperty, true);
    }

    // PRODUCTION_TABLE_LOOKUP
    // PRODUCTION_READY_EVENT_TYPE
    class GlobalTableInteractionSupportFilter final : public QObject
    {
    public:
        // PRODUCTION_TABLE_QUEUE
        // PRODUCTION_TABLE_GATE
        QHash<QTableView*, QPointer<QTableView>> m_pendingTables;
        bool m_hasInitialPending = false;
    protected:
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            // PRODUCTION_READY_EVENT
            // PRODUCTION_WINDOW_UPDATE
            const auto eventType = eventObject->type();
            QTableView* tableView = tableForEventObject(watchedObject);
            if (!tableView) return false;
            // PRODUCTION_TABLE_DISCOVERY
            return false;
        }
    };

    class ReadyEvents final : public QObject
    {
    public:
        QHash<const QObject*, int> counts;
    protected:
        bool eventFilter(QObject* target, QEvent* event) override
        {
            if (event->type() == initialTablePaintReadyEventType()) ++counts[target];
            return false;
        }
    };
    class Probe final : public ks::ui::TableActionTableView
    {
    public:
        using TableActionTableView::TableActionTableView;
        std::vector<QRect> frames;
        bool firstConfigured = false;
        bool firstThemeReady = false;
        int firstBarHeight = -1;
    protected:
        void paintEvent(QPaintEvent* event) override
        {
            if (frames.empty())
            {
                firstConfigured = property("fixture_configured").toBool();
                firstBarHeight = topActionBarHeight();
                const auto* bar = findChild<QFrame*>("fixture_bar", Qt::FindDirectChildrenOnly);
                firstThemeReady = bar && bar->property("fixture_theme_ready").toBool();
            }
            frames.push_back(viewport()->geometry());
            TableActionTableView::paintEvent(event);
        }
    };

    void firstFrame(bool alreadyVisible, bool popup, ReadyEvents& ready,
        GlobalTableInteractionSupportFilter& filter)
    {
        QWidget window(nullptr, popup ? Qt::Popup : Qt::Window);
        ready.counts.remove(&window);
        auto* layout = new QVBoxLayout(&window);
        window.resize(500, 320);
        if (alreadyVisible) { window.show(); pump(); }
        auto* table = new Probe(&window);
        QStandardItemModel model(100, 10);
        table->setModel(&model);
        layout->addWidget(table);
        table->show();
        window.show();
        layout->activate();
        if (alreadyVisible) window.repaint();
        pump();
        require(!table->frames.empty(), "actual table Paint occurred");
        if (!popup || !alreadyVisible)
        {
            require(table->firstConfigured, "the first frame already has default interactions");
            require(table->firstBarHeight == 32, "action strip reserved before the first frame");
            if (!popup) require(table->firstThemeReady,
                "new action strip theme prepared before its parent paints");
            for (const auto& frame : table->frames)
                require(frame == table->viewport()->geometry(), "native scrollbar/strip viewport stable from first Paint");
        }
        else
        {
            require(ready.counts.value(&window) == 0, "Popup keeps its original deferred setup");
        }
        require(table->topActionBarHeight() == 32, "settled strip height correct");
        require(table->findChildren<QFrame*>("fixture_bar", Qt::FindDirectChildrenOnly).size() == 1,
            "only one action strip installed");
        require(ready.counts.value(&window) <= 1, "at most one ready boundary per initial batch");
        const int before = attempts;
        const int barriers = ready.counts.value(&window);
        for (int repeat = 0; repeat < 100; ++repeat) window.repaint();
        pump();
        require(attempts == before, "stable paints do not reconfigure tables");
        require(ready.counts.value(&window) == barriers, "stable paints do not post more ready events");
        filter.scheduleTableConfiguration(table);
        window.repaint();
        require(attempts == before, "later refresh remains queued");
        pump();
        require(attempts > before, "the original later-refresh queue still runs");
    }

    void exemptions(ReadyEvents& ready)
    {
        for (int mode = 0; mode < 6; ++mode)
        {
            QWidget window;
            ready.counts.remove(&window);
            auto* layout = new QVBoxLayout(&window);
            window.resize(420, 240);
            window.show(); pump();
            auto* table = new Probe(&window);
            QStandardItemModel model(10, 3);
            if (mode != 0) table->setModel(&model);
            if (mode == 1) ks::ui::SetTableActionBarMode(table, TableActionBarMode::None);
            if (mode == 2) table->setProperty("ksword_preserve_custom_table_presentation", true);
            if (mode == 3) table->setProperty("KSWORD_TABLE_INTERACTION_FROZEN_PANE_AUXILIARY", true);
            if (mode == 4) table->setProperty("ksword_detail_keep_layout", true);
            layout->addWidget(table); table->show();
            if (mode == 5) table->hide();
            window.repaint(); pump();
            require(ready.counts.value(&window) == 0,
                "no-model, absent bar, professional, frozen and hidden cases never block paint");
        }
    }

    void completedDefaults(ReadyEvents& ready)
    {
        QWidget window;
        ready.counts.remove(&window);
        auto* layout = new QVBoxLayout(&window);
        auto* table = new Probe(&window);
        QStandardItemModel model(10, 3);
        table->setModel(&model); layout->addWidget(table);
        pump(); // The original default queue completed while this owner was hidden.
        require(table->property(kInitialConfigurationProperty).toBool(), "default queue completed before show");
        window.show(); window.repaint(); pump();
        require(ready.counts.value(&window) == 0, "completed defaults add no paint boundary");
    }

    void lifetimes(ReadyEvents& ready, GlobalTableInteractionSupportFilter& filter)
    {
        {
            QWidget window;
            ready.counts.remove(&window);
            auto* layout = new QVBoxLayout(&window);
            window.resize(420, 240); window.show(); pump();
            auto* table = new Probe(&window);
            QStandardItemModel model(10, 3); table->setModel(&model);
            table->setProperty("fixture_reject_configuration", true);
            int retries = 3;
            rejected = [&](QTableView* view) { if (retries-- > 0) filter.scheduleTableConfiguration(view); };
            layout->addWidget(table); table->show(); window.repaint(); pump();
            rejected = {};
            require(!table->frames.empty(), "refused/requeued configuration cannot starve Paint");
            require(ready.counts.value(&window) <= 1, "a refused initial batch has one ready boundary");
            require(filter.m_pendingTables.isEmpty(), "refused pending requests are consumed");
        }
        {
            auto* window = new QWidget;
            window->resize(420, 240); window->show(); pump();
            auto* table = new Probe(window);
            auto* model = new QStandardItemModel(10, 3, table); table->setModel(model);
            table->setGeometry(0, 0, 400, 220); table->show(); window->repaint();
            delete window; pump();
            require(filter.m_pendingTables.isEmpty(), "destroyed owner/target leaves no pending discovery");
        }
        {
            alignas(Probe) std::byte storage[sizeof(Probe)];
            auto* first = new (storage) Probe;
            filter.scheduleTableConfiguration(first);
            first->~Probe();
            auto* replacement = new (storage) Probe;
            QStandardItemModel model(10, 3); replacement->setModel(&model);
            filter.scheduleTableConfiguration(replacement); pump();
            require(replacement->property("fixture_configured").toBool(),
                "an old callback cannot consume a new view reusing its address");
            replacement->~Probe(); pump();
            require(filter.m_pendingTables.isEmpty(), "reused address leaves no retained pending view");
        }
    }

    void cancelledOrHidden(ReadyEvents& ready, GlobalTableInteractionSupportFilter& filter)
    {
        for (int mode = 0; mode < 3; ++mode)
        {
            QWidget window;
            ready.counts.remove(&window);
            auto* layout = new QVBoxLayout(&window);
            window.resize(420, 240); window.show(); pump();
            auto* table = new Probe(&window);
            auto* model = new QStandardItemModel(10, 3, table);
            table->setModel(model); layout->addWidget(table); table->show();
            if (mode == 0) filter.m_pendingTables.remove(table); // Cancellation before dispatch.
            window.repaint();
            if (mode == 1) delete model; // Eligibility disappears before the original callback.
            if (mode == 2) window.hide(); // The ready event must never re-show its owner.
            pump();
            require(filter.m_pendingTables.isEmpty(), "cancelled/changed/hidden discovery is released");
            require(ready.counts.value(&window) <= 1, "cancelled/changed/hidden batch has one boundary");
            if (mode == 2)
            {
                require(!window.isVisible(), "ready event does not show a hidden owner");
                window.show(); pump();
            }
            require(!table->frames.empty(), "cancelled/changed/hidden owner can still paint");
        }
    }
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    ks::ui::InstallGlobalFloatingScrollbars(&app);
    GlobalTableInteractionSupportFilter filter;
    app.installEventFilter(&filter);
    QWidget registered;
    ks::ui::BindWidgetTheme(&registered, [] {}); // Registry installs after table filter.
    pump();
    ReadyEvents ready;
    app.installEventFilter(&ready);
    for (int order = 0; order < 2; ++order)
    {
        if (order == 1) { app.removeEventFilter(&filter); app.installEventFilter(&filter); }
        for (bool visible : {false, true}) for (bool popup : {false, true})
            firstFrame(visible, popup, ready, filter);
    }
    exemptions(ready);
    completedDefaults(ready);
    lifetimes(ready, filter);
    cancelledOrHidden(ready, filter);
    std::cout << "TABLE_DEFAULT_FIRST_FRAME_PASS checks=" << checks << "\n";
}
