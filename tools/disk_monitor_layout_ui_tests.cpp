// Complete production section helper and actual shared theme/queue. The tables
// are ordinary Qt fixtures: no DiskMonitorPage sampler, Windows ETW or volume
// device is created. This does not validate the whole monitor page or its data.
#include "UI/StandardWidgetTheme.h"
#include "UI/FlatButtonTheme.h"
#include "UI/GlobalUiBaseStyle.h"
#include "UI/MainSurfaceStyle.h"
#include "UI/ThemeBinding.h"
#include "UI/PrimaryPageStyle.h"
#include "UI/PageControlStyle.h"
#include "UI/ThemeControlGlyphs.h"
#include "UI/ThemeStatusRole.h"
#include "UI/ThemedSlider.h"
#include "UI/ParameterWorkspace.h"
#include "UI/ToolbarMetrics.h"
#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace ks::ui
{
    // No instances of these unrelated roles are present. Button, toolbar,
    // section title, geometry and ThemeBinding implementations remain real.
    void StyleParameterSection(QGroupBox*) {}
    void StylePageTabs(QTabWidget*) {}
    void StylePageTabBar(QTabBar*) {}
    QString BuildPageTabsStyleSheet(bool) { return {}; }
    QString BuildStandardSliderStyle() { return {}; }
    QString BuildStatusRoleStyleRules() { return {}; }
    QString ThemedControlGlyphPath(const QString&, const QColor&, const QString&) { return {}; }
}

namespace
{
#include "DiskMonitorPalette.inc"

// PRODUCTION_DISK_MONITOR_SECTION
// PRODUCTION_SECTION_ADAPTER

    int checks = 0;
    int failures = 0;
    QString caseName;
    QString outputDirectory;

    void check(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s\n", qPrintable(caseName), message);
        }
    }
    void pump()
    {
        for (int pass = 0; pass < 12; ++pass) QApplication::processEvents();
    }
    void mouseClick(QAbstractButton* button)
    {
        QTest::mouseClick(button->window()->windowHandle(), Qt::LeftButton, Qt::NoModifier,
            button->mapTo(button->window(), button->rect().center()), 1);
        pump();
    }
    QToolButton* arrow(QWidget* section)
    {
        for (auto* button : section->findChildren<QToolButton*>())
            if (button->arrowType() != Qt::NoArrow) return button;
        return nullptr;
    }
    struct Geometry
    {
        QRect section;
        QRect header;
        QRect title;
        QRect toggle;
        QRect table;
        bool operator==(const Geometry& other) const
        {
            return section == other.section && header == other.header && title == other.title
                && toggle == other.toggle && table == other.table;
        }
    };
    class FirstPaint final : public QObject
    {
    public:
        FirstPaint(QWidget* section, QLabel* title, QToolButton* toggle, QWidget* content)
            : m_section(section), m_title(title), m_toggle(toggle), m_content(content)
        { section->installEventFilter(this); }
        Geometry current() const
        {
            QWidget* header = m_toggle->parentWidget();
            return {m_section->geometry(), header->geometry(), m_title ? m_title->geometry() : QRect(),
                m_toggle->geometry(), m_content->geometry()};
        }
        std::vector<Geometry> frames;
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (event->type() == QEvent::Paint) frames.push_back(current());
            return false;
        }
    private:
        QWidget* m_section;
        QLabel* m_title;
        QToolButton* m_toggle;
        QWidget* m_content;
    };
    void stable(const FirstPaint& probe, const char* message)
    {
        check(!probe.frames.empty(), "no actual section Paint was observed");
        const Geometry final = probe.current();
        // One assertion per phase, not a count dependent on native paint count.
        check(std::all_of(probe.frames.begin(), probe.frames.end(),
            [&final](const Geometry& geometry) { return geometry == final; }), message);
    }
    void capture(QWidget& host, const QString& suffix)
    {
        if (!outputDirectory.isEmpty())
            host.grab().save(outputDirectory + QLatin1Char('/') + caseName + suffix + QStringLiteral(".png"));
    }

    void exercise(bool dark, int width, int fontPixels)
    {
        using namespace ks::ui;
        caseName = QStringLiteral("%1-%2px-font%3").arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
            .arg(width).arg(fontPixels);
        KswordTheme::SetDarkModeEnabled(dark);
        // The product's appearance setting changes the application font.
        // Parent-only setFont is insufficient under application QSS inheritance.
        QFont font = QApplication::font();
        font.setPixelSize(fontPixels);
        QApplication::setFont(font);
        QApplication::setPalette(buildApplicationAppearancePalette(QApplication::palette()));
        qApp->setStyleSheet(BuildGlobalBaseControlStyleBlock());
        QMainWindow host;
        host.setProperty("ksword_main_surface_hierarchy", true);
        const QString style = BuildMainSurfaceStyle(false);
        host.setStyleSheet(style);
        RegisterSharedMainSurfaceButtonStyle(&host, style);
        host.setFont(font);
        host.resize(width, 640);
        auto* body = new QWidget(&host);
        host.setCentralWidget(body);
        auto* root = new QVBoxLayout(body);
        root->setContentsMargins(6, 6, 6, 6);
        root->setSpacing(6);
        auto* previous = new QPushButton(QStringLiteral("Refresh"), body);
        previous->setFixedWidth(120);
        root->addWidget(previous, 0, Qt::AlignLeft);
        auto* splitter = new QSplitter(Qt::Vertical, body);
        root->addWidget(splitter, 1);
        auto* processes = new QTableWidget(12, 2);
        processes->setHorizontalHeaderLabels({QStringLiteral("PID"), QStringLiteral("Process")});
        for (int row = 0; row < processes->rowCount(); ++row)
        {
            processes->setItem(row, 0, new QTableWidgetItem(QString::number(1000 + row)));
            processes->setItem(row, 1, new QTableWidgetItem(QStringLiteral("sample-%1.exe").arg(row)));
        }
        processes->selectRow(3);
        auto* activity = new QTableWidget(10, 2);
        activity->setHorizontalHeaderLabels({QStringLiteral("PID"), QStringLiteral("File")});
        activity->setItem(0, 0, new QTableWidgetItem(QStringLiteral("1003")));
        activity->setItem(0, 1, new QTableWidgetItem(QStringLiteral("C:/sample.dat")));
        QLabel* processTitle = nullptr;
        QLabel* activityTitle = nullptr;
        QWidget* processSection = createFixtureSection(splitter, &processTitle,
            QStringLiteral("磁盘活动的进程 / Processes with disk activity"), processes);
        QWidget* activitySection = createFixtureSection(splitter, &activityTitle,
            QStringLiteral("磁盘活动 / Disk activity"), activity);
        splitter->addWidget(processSection);
        splitter->addWidget(activitySection);
        splitter->setSizes({300, 260});
        QToolButton* fold = arrow(processSection);
        QToolButton* secondFold = arrow(activitySection);
        check(fold && secondFold, "a section has no actual arrow button");
        if (!fold || !secondFold) return;
        FirstPaint firstPaint(processSection, processTitle, fold, processes);
        FirstPaint secondPaint(activitySection, activityTitle, secondFold, activity);
        host.show();
        host.activateWindow();
        pump();
        QTest::mouseMove(host.windowHandle(), QPoint(host.width() - 15, 5), 1);
        pump();
        stable(firstPaint, "section geometry changed after its first actual Paint");
        stable(secondPaint, "activity geometry changed after its first actual Paint");
        check(host.width() == width, "section minimum sizes forced the requested narrow window wider");
        check(processTitle && activityTitle, "section titles are still full-width action buttons");
        check(processSection->findChildren<QToolButton*>().size() == 1,
            "section contains a second full-width title button");
        check(!fold->isCheckable() && !fold->isChecked(), "fold state incorrectly uses selected-button styling");
        check(fold->width() == 28 && fold->height() == 28, "the fold action lost the shared compact icon metrics");
        check(fold->parentWidget()->property("ksword_primary_toolbar").toBool(),
            "section header did not use the actual shared toolbar owner");
        if (processTitle)
        {
            check(processTitle->font().pixelSize() == fontPixels, "the requested application font never reached the title");
            check(processTitle->property("ksword_primary_title").toBool(), "title did not use the shared section-title role");
            check(processTitle->geometry().right() < fold->geometry().left(), "long title overlaps its fold action");
            check(processTitle->height() >= QFontMetrics(processTitle->font()).height(), "section title clips a line of text");
        }
        check(processes->viewport()->height() > 100 && activity->viewport()->height() > 100,
            "expanded data tables have no useful viewport height");
        const QImage image = host.grab().toImage();
        QWidget* header = fold->parentWidget();
        const QPoint point = header->mapTo(&host, QPoint(header->width() - 55, header->height() - 6));
        check(image.pixelColor(point).rgb() == MainWindowSurfaceColors().toolbar.rgb(),
            "expanded section header is highlighted instead of painting the neutral shared toolbar");
        capture(host, QStringLiteral("-initial"));

        auto* model = processes->model();
        const int selectedRow = processes->currentRow();
        const QString selectedPid = processes->item(selectedRow, 0)->text();
        const int expandedHeight = splitter->sizes().front();
        mouseClick(fold);
        check(!processes->isVisible() && activity->isVisible(), "mouse fold changed the wrong content visibility");
        check(fold->arrowType() == Qt::RightArrow, "collapsed arrow did not point right");
        check(processSection->maximumHeight() == processSection->minimumHeight(), "collapsed section did not release splitter space");
        check(processSection->property("kswordDiskMonitorExpandedHeight").toInt() == expandedHeight,
            "fold discarded the prior expanded splitter height");
        check(splitter->sizes().front() < expandedHeight, "fold left the section occupying its expanded height");
        mouseClick(fold);
        check(processes->isVisible() && fold->arrowType() == Qt::DownArrow,
            "second mouse click did not restore section content");
        check(processSection->maximumHeight() == QWIDGETSIZE_MAX, "expanded section retained the collapsed height cap");
        check(processes->model() == model && processes->currentRow() == selectedRow
            && processes->item(selectedRow, 0)->text() == selectedPid, "fold rebuilt data or lost the selected PID");

        QWidget::setTabOrder(previous, fold);
        mouseClick(previous);
        QTest::mouseMove(host.windowHandle(), QPoint(host.width() - 15, 5), 1);
        QTest::keyClick(previous, Qt::Key_Tab, Qt::NoModifier, 1);
        pump();
        check(fold->hasFocus() && HasFlatButtonKeyboardFocus(fold), "real Tab cannot reach the independent fold action");
        QTest::keyClick(fold, Qt::Key_Space, Qt::NoModifier, 1);
        pump();
        check(!processes->isVisible(), "Space did not fold the real focused control");
        QTest::keyClick(fold, Qt::Key_Space, Qt::NoModifier, 1);
        pump();
        check(processes->isVisible(), "Space did not expand the real focused control");
        check(!fold->isChecked(), "keyboard expansion left the action permanently selected");

        host.hide();
        host.resize(width == 680 ? 1120 : 680, 640);
        firstPaint.frames.clear();
        secondPaint.frames.clear();
        host.show();
        pump();
        stable(firstPaint, "section geometry changed after re-show at another width");
        stable(secondPaint, "activity geometry changed after re-show at another width");
        check(processes->model() == model && processes->currentRow() == selectedRow,
            "re-show at a different width replaced table data or selection");
        capture(host, QStringLiteral("-reshown"));
    }
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    if (argc > 1) { outputDirectory = QString::fromLocal8Bit(argv[1]); QDir().mkpath(outputDirectory); }
    ks::ui::InstallStandardWidgetTheme(&application, false);
    ks::ui::InstallGlobalFlatButtonTheme(&application);
    for (bool dark : {false, true})
    {
        exercise(dark, 1120, 14);
        exercise(dark, 680, 20);
    }
    std::printf("DISK_MONITOR_LAYOUT_CHECKS=%d FAILURES=%d REAL_QTWIDGETS=1 MAIN_GUI=0 SECTION_HELPER_ONLY=1\n",
        checks, failures);
    return failures ? 1 : 0;
}
