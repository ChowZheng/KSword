#include "PhysicalPageAttributionPage.h"
#include "MemoryAttributionChart.h"
#include "../ArkDriverClient/ArkDriverPfn.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/AdaptivePageScroll.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/TableInteractionSupport.h"
#include <QDateTime>
#include <QEvent>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <thread>

namespace {
using namespace ksword::pfn;
QString L(const char* text) { return ks::i18n::packedSourceText(QString::fromUtf8(text)); }
constexpr std::array<const char*, useCount> useNames{
    "Process private", "Mapped file / cache", "Shareable / pagefile section", "Page tables",
    "Paged pool", "Nonpaged pool", "System PTE", "Session private", "Metafile",
    "AWE", "Driver locked / MDL", "Kernel stacks", "Image pages", "Compression process private", "Free / zeroed (no owner)", "True unknown"
};
constexpr std::array<const char*, 8> stateNames{
    "Zeroed", "Free", "Standby", "Modified", "Modified no-write", "Bad", "Active", "Transition"
};
QString bytes(std::uint64_t amount)
{
    if (amount >= (1ULL << 30)) { return QStringLiteral("%1 GiB").arg(static_cast<double>(amount) / (1ULL << 30), 0, 'f', 2); }
    return QStringLiteral("%1 MiB").arg(static_cast<double>(amount) / (1ULL << 20), 0, 'f', 2);
}
QString hex(std::uint64_t value) { return QStringLiteral("0x%1").arg(value, 0, 16); }
QString status(long value) { return QStringLiteral("0x%1").arg(static_cast<quint32>(value), 8, 16, QLatin1Char('0')); }
QTableWidget* table(QWidget* parent)
{
    auto* value = new ks::ui::VisibleTableWidget(parent);
    value->setEditTriggers(QAbstractItemView::NoEditTriggers);
    value->setSelectionBehavior(QAbstractItemView::SelectRows);
    value->setSelectionMode(QAbstractItemView::SingleSelection);
    value->verticalHeader()->hide();
    value->horizontalHeader()->setStretchLastSection(true);
    value->setAlternatingRowColors(true);
    return value;
}
void headers(QTableWidget* table, const QStringList& labels)
{
    table->setColumnCount(static_cast<int>(labels.size()));
    table->setHorizontalHeaderLabels(labels);
}
void number(QTableWidget* table, int row, int column, std::uint64_t value, bool memory = true)
{
    auto* item = new ks::ui::NumericTableItem(memory ? bytes(value) : QString::number(value), static_cast<qulonglong>(value));
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    table->setItem(row, column, item);
}
}

struct PhysicalPageAttributionPage::Inspection {
    std::atomic_bool done{false};
    long status = 0;
    std::uint64_t pfn = 0;
    ksword::pfn::Identity identity;
    QString sampledAt;
};

PhysicalPageAttributionPage::PhysicalPageAttributionPage(QWidget* parent) : QWidget(parent)
{
    // 页面自带内部滚动壳：固定高度的归因图加四页签表格约五百像素，嵌在系统内存审计页里
    // 放不下时在本页内滚动，不再把外层的审计页撑高。根布局建在壳的内容容器上。
    auto* root = new QVBoxLayout(ks::ui::EnablePageInnerScroll(this));
    root->setContentsMargins(0, 0, 0, 0);
    auto* actions = new QHBoxLayout;
    m_scanButton = new QPushButton(this);
    m_cancelButton = new QPushButton(this);
    m_mappingButton = new QPushButton(this);
    m_exportButton = new QPushButton(this);
    m_budget = new QSpinBox(this);
    m_budget->setRange(15, 600);
    m_budget->setValue(90);
    m_filter = new QLineEdit(this);
    m_filter->setClearButtonEnabled(true);
    actions->addWidget(m_scanButton);
    actions->addWidget(m_cancelButton);
    actions->addWidget(m_mappingButton);
    actions->addWidget(m_budget);
    actions->addStretch();
    root->addLayout(actions);
    auto* filters = new QHBoxLayout;
    filters->addWidget(m_filter, 1);
    filters->addWidget(m_exportButton);
    root->addLayout(filters);
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_summary);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(5);
    root->addWidget(m_progress);
    m_chart = new MemoryAttributionChart(this);
    root->addWidget(m_chart);
    m_tabs = new QTabWidget(this);
    m_categories = table(m_tabs);
    m_categories->horizontalHeader()->setSortIndicator(2, Qt::DescendingOrder);
    m_groups = table(m_tabs);
    m_tabs->addTab(m_categories, {});
    m_tabs->addTab(m_groups, {});
    auto* pages = new QWidget(m_tabs);
    auto* pageLayout = new QVBoxLayout(pages);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    auto* lookup = new QHBoxLayout;
    m_pfn = new QLineEdit(pages);
    m_inspectButton = new QPushButton(pages);
    lookup->addWidget(m_pfn, 1);
    lookup->addWidget(m_inspectButton);
    pageLayout->addLayout(lookup);
    auto* pageSplit = new QSplitter(Qt::Horizontal, pages);
    m_examples = table(pageSplit);
    m_mappings = table(pageSplit);
    pageSplit->addWidget(m_examples);
    pageSplit->addWidget(m_mappings);
    pageSplit->setStretchFactor(1, 2);
    pageLayout->addWidget(pageSplit, 1);
    m_pageEvidence = new QPlainTextEdit(pages);
    m_pageEvidence->setReadOnly(true);
    m_pageEvidence->setMaximumHeight(90);
    pageLayout->addWidget(m_pageEvidence);
    m_tabs->addTab(pages, {});
    m_evidence = new QPlainTextEdit(m_tabs);
    m_evidence->setReadOnly(true);
    m_tabs->addTab(m_evidence, {});
    root->addWidget(m_tabs, 1);
    connect(m_scanButton, &QPushButton::clicked, this, [this] { startScan(); });
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        if (m_job) { m_job->cancel.store(true); }
        if (m_mappingJob) { m_mappingJob->cancel.store(true); }
    });
    connect(m_mappingButton, &QPushButton::clicked, this, [this] { startMappings(); });
    connect(m_inspectButton, &QPushButton::clicked, this, [this] { inspectPfn(); });
    connect(m_pfn, &QLineEdit::returnPressed, this, [this] { inspectPfn(); });
    connect(m_exportButton, &QPushButton::clicked, this, [this] { exportEvidence(); });
    connect(m_filter, &QLineEdit::textChanged, this, [this] { rebuildGroups(); });
    connect(m_categories, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (const auto* item = m_categories->item(row, 0)) { selectCategory(item->data(Qt::UserRole).toInt()); }
    });
    connect(m_groups, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const auto* item = m_groups->item(row, 0);
        if (!item) { return; }
        m_pfn->setText(hex(item->data(Qt::UserRole).toULongLong()));
        m_tabs->setCurrentIndex(2);
        inspectPfn();
    });
    connect(m_examples, &QTableWidget::cellClicked, this, [this](int row, int) {
        const auto* item = m_examples->item(row, 0);
        if (!item) { return; }
        m_pfn->setText(item->text());
        inspectPfn();
    });
    m_chart->selected = [this](int key) {
        if (key >= 0 && key < static_cast<int>(useCount)) { selectCategory(key); m_tabs->setCurrentIndex(2); }
        else { m_tabs->setCurrentIndex(key == 17 ? 0 : 3); }
    };
    auto* timer = new QTimer(this);
    timer->setInterval(150);
    connect(timer, &QTimer::timeout, this, [this] { poll(); });
    timer->start();
    retranslate();
    rebuild();
}

PhysicalPageAttributionPage::~PhysicalPageAttributionPage()
{
    if (m_job) { m_job->cancel.store(true); }
    if (m_mappingJob) { m_mappingJob->cancel.store(true); }
}

void PhysicalPageAttributionPage::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && m_scanButton) { retranslate(); rebuild(); }
}

void PhysicalPageAttributionPage::retranslate()
{
    m_scanButton->setText(L("Scan physical pages"));
    m_cancelButton->setText(L("Cancel scan"));
    m_mappingButton->setText(L("Resolve PFN mappings"));
    m_mappingButton->setToolTip(L("Optional, slower R0 scan of accessible process working sets. Resolves file paths, observed sharing, large pages and locked pages without adding references to physical totals."));
    m_budget->setSuffix(L(" s"));
    m_budget->setToolTip(L("Time budget for mapping resolution; at most two million mapping references are retained."));
    m_exportButton->setText(L("Export PFN evidence"));
    m_filter->setPlaceholderText(L("Filter backing identity, process or PID"));
    m_pfn->setPlaceholderText(L("PFN number, decimal or 0x hexadecimal (not a byte address)"));
    m_inspectButton->setText(L("Inspect PFN"));
    m_tabs->setTabText(0, L("Usage by page state"));
    m_tabs->setTabText(1, L("Largest physical owners"));
    m_tabs->setTabText(2, L("PFN and mappings"));
    m_tabs->setTabText(3, L("Coverage and evidence"));
    QStringList categoryHeaders{L("Primary classification"), L("In-use pages"), L("Unique bytes")};
    for (const char* name : stateNames) { categoryHeaders << L(name); }
    headers(m_categories, categoryHeaders);
    headers(m_groups, {L("Primary classification"), L("Backing / owner evidence"), L("PID"), L("Unique bytes"), L("Active"), L("First PFN")});
    headers(m_examples, {L("PFN sample"), L("Physical state"), L("Physical address")});
    headers(m_mappings, {L("PID"), L("Virtual address"), L("Process"), L("Backing file"), L("Page size"), L("Locked"), L("Windows share count (capped)")});
}

void PhysicalPageAttributionPage::startScan()
{
    if (m_job || m_mappingJob) { return; }
    m_job = std::make_shared<ScanJob>();
    // Workers own only shared state; page destruction never invalidates a callback.
    const auto job = m_job;
    try { std::thread([job] { collectPhysicalPages(job); }).detach(); }
    catch (...) { m_job.reset(); m_summary->setText(L("Unable to start the scan worker.")); return; }
    poll();
}

void PhysicalPageAttributionPage::startMappings()
{
    if (m_job || m_mappingJob) { return; }
    m_mappingJob = std::make_shared<MappingJob>();
    const auto job = m_mappingJob;
    const auto seconds = static_cast<unsigned>(m_budget->value());
    try { std::thread([job, seconds] { collectPhysicalMappings(job, seconds); }).detach(); }
    catch (...) { m_mappingJob.reset(); m_summary->setText(L("Unable to start the scan worker.")); return; }
    poll();
}

void PhysicalPageAttributionPage::poll()
{
    if (m_job && m_job->done.load()) {
        { std::lock_guard<std::mutex> lock(m_job->mutex); m_scan = m_job->result; }
        m_job.reset();
        if (snapshotReady) { snapshotReady(m_scan); }
        rebuild();
    }
    if (m_mappingJob && m_mappingJob->done.load()) {
        { std::lock_guard<std::mutex> lock(m_mappingJob->mutex); m_mappingScan = m_mappingJob->result; }
        m_mappingJob.reset();
        rebuild();
        bool ok = false;
        const auto pfn = m_pfn->text().toULongLong(&ok, 0);
        if (ok) { showMappings(pfn); }
    }
    const bool busy = m_job || m_mappingJob;
    m_scanButton->setEnabled(!busy);
    m_mappingButton->setEnabled(!busy && m_scan && m_scan->accounting.valid != 0);
    m_cancelButton->setEnabled(busy);
    m_exportButton->setEnabled(m_scan != nullptr && !busy);
    if (m_job) {
        const auto total = m_job->total.load();
        const auto visited = m_job->visited.load();
        m_summary->setText(L("Scanning PFNs: %1 / %2. Previous results remain a separate snapshot.").arg(bytes(visited * pageBytes), bytes(total * pageBytes)));
        m_progress->setValue(total ? static_cast<int>(visited * 1000 / total) : 0);
    } else if (m_mappingJob) {
        m_summary->setText(L("Resolving mappings: %1 resident references checked. Physical totals are unchanged.").arg(m_mappingJob->tested.load()));
    }
    if (m_inspection && m_inspection->done.load()) {
        const auto result = std::move(m_inspection);
        m_inspectButton->setEnabled(true);
        if (result->status < 0 || result->identity.frame == ~0ULL) {
            m_pageEvidence->setPlainText(L("PFN query unavailable: %1").arg(status(result->status)));
        } else {
            const auto use = classify(result->identity);
            QString description = L("PFN %1 | physical %2 | %3 | %4\nOwner key %5 | backing / VA %6 | sampled %7")
                .arg(hex(result->pfn), hex(result->pfn * pageBytes), L(useNames[static_cast<std::size_t>(use)]),
                    L(stateNames[state(result->identity)]),
                    nativeUse(result->identity) == 0 && state(result->identity) >= 2 && processKey(result->identity)
                        ? hex(processKey(result->identity)) : L("Unavailable"),
                    hex(result->identity.backing), result->sampledAt);
            description += L("\nMapping observations were collected separately and can change while the system runs. A Windows share count is capped and is not the complete list of owners.");
            m_pageEvidence->setPlainText(description);
        }
        showMappings(result->pfn);
    }
}

void PhysicalPageAttributionPage::rebuild()
{
    if (!m_scan) {
        m_summary->setText(L("Run a PFN scan to replace the snapshot remainder with physical-page evidence. No scan has completed yet."));
        m_chart->setSegments({}, L("Physical page attribution"));
        return;
    }
    const auto& scan = *m_scan;
    const auto& counts = scan.accounting;
    const auto unknown = counts.inUse(Use::Unknown);
    std::uint64_t known = 0;
    std::vector<MemoryAttributionChart::Segment> segments;
    for (std::size_t i = 0; i < useCount; ++i) {
        const auto amount = counts.inUse(static_cast<Use>(i));
        if (i != static_cast<std::size_t>(Use::Unknown)) { known += amount; }
        if (amount) { segments.push_back({L(useNames[i]), amount * pageBytes, static_cast<int>(i)}); }
    }
    if (counts.availablePages) { segments.push_back({L("Available"), counts.availablePages * pageBytes, 17}); }
    if (counts.bad()) { segments.push_back({L("Bad"), counts.bad() * pageBytes, -1}); }
    if (counts.unreadable) { segments.push_back({L("Query failed"), counts.unreadable * pageBytes, -2}); }
    if (counts.notScanned()) { segments.push_back({L("Not scanned"), counts.notScanned() * pageBytes, -3}); }
    m_chart->setSegments(std::move(segments), L("One physical page, one category. Click a category to inspect PFNs."));
    m_summary->setText(L("%1 | NT RAM %2 | attributed in use %3 | true unknown %4 | unreadable / unscanned %5 | coverage %6%")
        .arg(scan.complete ? L("Scan complete") : L("Partial / unavailable"), bytes(counts.expected * pageBytes), bytes(known * pageBytes),
            bytes(unknown * pageBytes), bytes((counts.unreadable + counts.notScanned()) * pageBytes))
        .arg(counts.expected ? 100.0 * static_cast<double>(counts.valid) / static_cast<double>(counts.expected) : 0.0, 0, 'f', 2));
    if (!counts.expected) {
        m_summary->setText(L("PFN scan unavailable: %1. No unknown-byte total can be calculated.").arg(status(scan.rangesStatus)));
    }
    m_progress->setValue(counts.expected ? static_cast<int>(counts.valid * 1000 / counts.expected) : 0);
    m_categories->setSortingEnabled(false);
    m_categories->setRowCount(static_cast<int>(useCount));
    for (std::size_t i = 0; i < useCount; ++i) {
        const int row = static_cast<int>(i);
        auto* label = new QTableWidgetItem(L(useNames[i]));
        label->setData(Qt::UserRole, row);
        m_categories->setItem(row, 0, label);
        number(m_categories, row, 1, counts.inUse(static_cast<Use>(i)), false);
        number(m_categories, row, 2, counts.inUse(static_cast<Use>(i)) * pageBytes);
        for (unsigned list = 0; list < 8; ++list) { number(m_categories, row, static_cast<int>(list) + 3, counts.byUseAndState[i][list] * pageBytes); }
    }
    m_categories->setSortingEnabled(true);
    m_categories->resizeColumnsToContents();
    rebuildGroups();
    if (m_selectedCategory >= 0) { selectCategory(m_selectedCategory); }
    QStringList evidence;
    evidence << L("Collection interval: %1 to %2 (%3 ms)").arg(scan.started, scan.finished).arg(scan.elapsedMs);
    evidence << L("Native batches %1 | R0 batches %2 | failed batches %3").arg(scan.nativeBatches).arg(scan.driverBatches).arg(scan.failedBatches);
    evidence << L("Range status %1 | owner status %2 | page status %3").arg(status(scan.rangesStatus), status(scan.ownersStatus), status(scan.lastPageStatus));
    evidence << L("Ledger reconciliation: %1. Unknown, unreadable and unscanned are separate; none is assigned to a guessed owner.")
        .arg(counts.reconciles() && counts.expected ? L("Passed") : L("Unavailable / failed"));
    evidence << L("Windows usable before / after: %1 / %2. PFN range total: %3.").arg(bytes(scan.totalBefore), bytes(scan.totalAfter), bytes(counts.expected * pageBytes));
    const auto difference = static_cast<qint64>(counts.expected * pageBytes) - static_cast<qint64>(scan.totalBefore);
    evidence << L("PFN range minus Windows usable: %1 bytes (different scopes or changing memory; not an owner).").arg(difference);
    evidence << L("Available before / after: %1 / %2. The scan is time-skewed, not an atomic snapshot.").arg(bytes(scan.availableBefore), bytes(scan.availableAfter));
    evidence << L("Installed minus Windows usable: %1. This is an aggregate reserved estimate, not a map of firmware pages.")
        .arg(scan.totalBefore && scan.installed >= scan.totalBefore ? bytes(scan.installed - scan.totalBefore) : L("Unavailable"));
    evidence << L("Hypervisor present: %1 | Secure kernel running: %2. Their physical byte counts are not exposed by this query.")
        .arg(scan.hypervisor ? L("Yes") : L("No"), scan.secureKnown ? (scan.secureKernel ? L("Yes") : L("No")) : L("Unavailable"));
    evidence << L("Pinned %1 | non-tradeable %2. These are overlapping attributes, not additional physical consumption.").arg(bytes(counts.pinned * pageBytes), bytes(counts.nonTradeable * pageBytes));
    evidence << L("File/cache names are resolved from observed process mappings. A file key without a path still has a known primary classification.");
    evidence << L("Compression is identified only for private PFNs linked to the MemCompression source; the entire compression store is not inferred from a counter.");
    evidence << L("PFN database, secure memory and inaccessible mappings are not guessed from residual bytes or hard-coded private offsets. Existing snapshot counters remain diagnostic only.");
    evidence << L("The table shows up to 300 matching backing identities; export retains all collected groups. PFN samples are bounded to 256 per category and do not limit the accounting scan.");
    if (scan.groupedOverflowPages) { evidence << L("Backing-group retention limit reached: %1 still-counted pages lack a retained group.").arg(scan.groupedOverflowPages); }
    if (scan.rangesChanged) { evidence << L("RAM ranges changed or could not be rechecked; treat this scan as partial."); }
    if (scan.cancelled) { evidence << L("Scan cancelled. Remaining pages are explicitly unscanned."); }
    if (scan.resourceFailure) { evidence << L("Collection failed because a worker could not retain its result."); }
    if (m_mappingScan) {
        const auto& maps = *m_mappingScan;
        evidence << L("Mappings sampled %1 | tested %2 | distinct observed PFNs %3 | failures %4 | processes scanned %5/%6 | inaccessible %7")
            .arg(maps.sampledAt).arg(maps.tested).arg(maps.distinct).arg(maps.failed).arg(maps.scannedProcesses).arg(maps.processes).arg(maps.inaccessible);
        evidence << L("Observed large-page bytes %1 | locked bytes %2 | multiply mapped bytes %3. All are subsets, never added to the PFN ledger.")
            .arg(bytes(maps.large * pageBytes), bytes(maps.locked * pageBytes), bytes(maps.multiplyMapped * pageBytes));
        evidence << L("Mapping coverage is limited to accessible working sets; AWE, large-page and protected mappings may be absent. Unobserved does not mean zero.");
        evidence << L("Mapping status %1 | driver available %2 | cancelled %3 | budget reached %4")
            .arg(status(maps.status), maps.driverAvailable ? L("Yes") : L("No"), maps.cancelled ? L("Yes") : L("No"), maps.budgetReached ? L("Yes") : L("No"));
    } else { evidence << L("Mapping resolution has not run. Large-page and per-PFN reference coverage is unavailable."); }
    m_evidence->setPlainText(evidence.join(QLatin1Char('\n')));
}

void PhysicalPageAttributionPage::rebuildGroups()
{
    m_groups->setRowCount(0);
    if (!m_scan) { return; }
    const QString filter = m_filter->text().trimmed();
    for (const auto& group : m_scan->groups) {
        QString owner = group.name.isEmpty() ? (group.key ? hex(group.key) : L("Unavailable")) : group.name;
        if (m_mappingScan && (group.use == Use::MappedFile || group.use == Use::Image || group.use == Use::Metafile)) {
            const auto file = m_mappingScan->observedFileNames.find(group.key);
            if (file != m_mappingScan->observedFileNames.end()) { owner += QStringLiteral("  ") + file->second; }
        }
        const QString kind = L(useNames[static_cast<std::size_t>(group.use)]);
        if (!filter.isEmpty() && !(owner + kind + QString::number(group.pid)).contains(filter, Qt::CaseInsensitive)) { continue; }
        const int row = m_groups->rowCount();
        if (row >= 300) { break; }
        m_groups->insertRow(row);
        auto* item = new QTableWidgetItem(kind);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(group.firstPfn));
        m_groups->setItem(row, 0, item);
        m_groups->setItem(row, 1, new QTableWidgetItem(owner));
        if (group.pid) { number(m_groups, row, 2, group.pid, false); }
        else { m_groups->setItem(row, 2, new QTableWidgetItem(L("Unavailable"))); }
        number(m_groups, row, 3, group.pages * pageBytes);
        number(m_groups, row, 4, group.activePages * pageBytes);
        m_groups->setItem(row, 5, new QTableWidgetItem(hex(group.firstPfn)));
    }
    m_groups->resizeColumnsToContents();
}

void PhysicalPageAttributionPage::selectCategory(int use)
{
    if (!m_scan || use < 0 || use >= static_cast<int>(useCount)) { return; }
    m_selectedCategory = use;
    const auto& examples = m_scan->examples[static_cast<std::size_t>(use)];
    m_examples->setRowCount(static_cast<int>(examples.size()));
    for (std::size_t i = 0; i < examples.size(); ++i) {
        const int row = static_cast<int>(i);
        m_examples->setItem(row, 0, new QTableWidgetItem(hex(examples[i].first)));
        m_examples->setItem(row, 1, new QTableWidgetItem(L(stateNames[state(examples[i].second)])));
        m_examples->setItem(row, 2, new QTableWidgetItem(hex(examples[i].first * pageBytes)));
    }
    m_examples->resizeColumnsToContents();
}

void PhysicalPageAttributionPage::inspectPfn()
{
    if (m_inspection) { return; }
    bool ok = false;
    const auto pfn = m_pfn->text().trimmed().toULongLong(&ok, 0);
    if (!ok || !m_scan || std::none_of(m_scan->ranges.begin(), m_scan->ranges.end(), [pfn](const Range& range) { return pfn >= range.first && pfn - range.first < range.count; })) {
        m_pageEvidence->setPlainText(L("Enter a PFN inside a collected NT RAM range. Physical address holes are not RAM."));
        return;
    }
    m_inspection = std::make_shared<Inspection>();
    m_inspection->pfn = pfn;
    const auto job = m_inspection;
    m_inspectButton->setEnabled(false);
    try { std::thread([job] {
        try {
            ksword::ark::PfnQueryClient client;
            std::vector<KSWORD_ARK_PFN_IDENTITY> pages;
            job->status = client.pages(job->pfn, 1, pages);
            if (!pages.empty()) { job->identity = {pages[0].frame, pages[0].backing}; }
            job->sampledAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        } catch (...) { job->status = static_cast<long>(0xC000009AUL); }
        job->done.store(true);
    }).detach(); } catch (...) { m_inspection.reset(); m_inspectButton->setEnabled(true); }
}

void PhysicalPageAttributionPage::showMappings(std::uint64_t pfn)
{
    m_mappings->setRowCount(0);
    if (!m_mappingScan) { return; }
    const auto& rows = m_mappingScan->rows;
    auto first = std::lower_bound(rows.begin(), rows.end(), pfn, [](const Mapping& row, auto value) { return row.pfn < value; });
    auto last = first;
    while (last != rows.end() && last->pfn == pfn) { ++last; }
    m_pageEvidence->appendPlainText(L("Observed mappings for this PFN: %1 (sampled %2; first 300 shown).").arg(std::distance(first, last)).arg(m_mappingScan->sampledAt));
    for (auto entry = first; entry != last && m_mappings->rowCount() < 300; ++entry) {
        const int row = m_mappings->rowCount();
        m_mappings->insertRow(row);
        const auto& backing = m_mappingScan->backing[entry->backingIndex];
        number(m_mappings, row, 0, entry->pid, false);
        m_mappings->setItem(row, 1, new QTableWidgetItem(hex(entry->address)));
        m_mappings->setItem(row, 2, new QTableWidgetItem(backing.process));
        m_mappings->setItem(row, 3, new QTableWidgetItem(backing.path));
        number(m_mappings, row, 4, entry->pageSize);
        m_mappings->setItem(row, 5, new QTableWidgetItem(entry->attributesKnown ? (entry->locked ? L("Yes") : L("No")) : L("Unavailable")));
        if (entry->attributesKnown) { number(m_mappings, row, 6, entry->shareCount, false); }
        else { m_mappings->setItem(row, 6, new QTableWidgetItem(L("Unavailable"))); }
    }
    m_mappings->resizeColumnsToContents();
}

void PhysicalPageAttributionPage::exportEvidence()
{
    if (!m_scan) { return; }
    const QString path = QFileDialog::getSaveFileName(this, L("Export PFN evidence"), QStringLiteral("pfn-evidence.json"), L("JSON files (*.json)"));
    if (path.isEmpty()) { return; }
    const auto& scan = *m_scan;
    QJsonObject root;
    root.insert(QStringLiteral("schema"), 1);
    root.insert(QStringLiteral("pageBytes"), static_cast<int>(pageBytes));
    root.insert(QStringLiteral("started"), scan.started);
    root.insert(QStringLiteral("finished"), scan.finished);
    root.insert(QStringLiteral("complete"), scan.complete);
    root.insert(QStringLiteral("cancelled"), scan.cancelled);
    root.insert(QStringLiteral("reconciles"), scan.accounting.reconciles());
    root.insert(QStringLiteral("expectedPages"), QString::number(scan.accounting.expected));
    root.insert(QStringLiteral("unreadablePages"), QString::number(scan.accounting.unreadable));
    root.insert(QStringLiteral("unscannedPages"), QString::number(scan.accounting.notScanned()));
    root.insert(QStringLiteral("interpretation"), m_evidence->toPlainText());
    QJsonArray categories;
    for (std::size_t i = 0; i < useCount; ++i) {
        QJsonArray states;
        for (const auto pages : scan.accounting.byUseAndState[i]) { states.append(QString::number(pages)); }
        QJsonObject entry;
        entry.insert(QStringLiteral("use"), QString::fromLatin1(useNames[i]));
        entry.insert(QStringLiteral("pagesByState"), states);
        categories.append(entry);
    }
    root.insert(QStringLiteral("categories"), categories);
    QJsonArray ranges;
    for (const auto& range : scan.ranges) {
        QJsonObject entry;
        entry.insert(QStringLiteral("firstPfn"), hex(range.first));
        entry.insert(QStringLiteral("pageCount"), QString::number(range.count));
        ranges.append(entry);
    }
    root.insert(QStringLiteral("ranges"), ranges);
    QJsonArray groups;
    for (const auto& group : scan.groups) {
        QJsonObject entry;
        entry.insert(QStringLiteral("use"), QString::fromLatin1(useNames[static_cast<std::size_t>(group.use)]));
        entry.insert(QStringLiteral("key"), hex(group.key));
        entry.insert(QStringLiteral("pid"), static_cast<qint64>(group.pid));
        entry.insert(QStringLiteral("name"), group.name);
        entry.insert(QStringLiteral("pages"), QString::number(group.pages));
        entry.insert(QStringLiteral("firstPfn"), hex(group.firstPfn));
        if (m_mappingScan && (group.use == Use::MappedFile || group.use == Use::Image || group.use == Use::Metafile)) {
            const auto file = m_mappingScan->observedFileNames.find(group.key);
            if (file != m_mappingScan->observedFileNames.end()) { entry.insert(QStringLiteral("observed_path"), file->second); }
        }
        groups.append(entry);
    }
    root.insert(QStringLiteral("groups"), groups);
    QSaveFile file(path);
    const auto data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        m_summary->setText(L("Evidence export failed: %1").arg(file.errorString()));
    } else { m_summary->setText(L("PFN evidence saved: %1").arg(path)); }
}
