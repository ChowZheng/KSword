// ============================================================
// wpJ6_tests.DarkLabels.cpp
// 作用：深色模式下"文字与底色无差别（空白标签）"的客观探针。
//   对 MemoryWorkbenchView 里每个可见、有文字的控件（QLabel / QAbstractButton），把整个视图渲染成图，
//   裁出该控件的矩形，取出现最多的颜色当底色，再找离底色最远的像素：
//     - 最远距离 < kBlankDistance          → 判"空白"（文字与底色几乎同色）；
//     - 文字像素与底色的对比度 < kLowContrast → 判"低对比"（能看见但很费劲）。
//   覆盖：深/浅两套主题 × 构造期就在该主题 与 "先在另一主题构造、再运行期切换" 两种路径，
//   × 十六进制/反汇编/文本/对比四个子页 × 状态条各种文案/待写入芯片/int3 条目/地址簿条目/窄宽度。
// 现状（写给下一个人）：这个"深色下标签空白"任务的起点是一句话标题，没有给出具体现象。把探针跑遍
//   9 条环境路径（含真实全局样式块、嵌进主窗口 QSS、运行期切主题、自定义主背景色）× 约 40 个界面状态
//   共约四五千个文字单元后，**没有发现任何一处因颜色造成的空白**（最差的配对对比度 3.14）。唯一真实的
//   "空白标签"是窄宽度下状态条的通道·范围段被压成 1px，与主题无关（深浅一致），已在 WorkbenchStatusBar
//   修掉，回归见 wpJ6_tests.Narrow.cpp 的 TestStatusBarSegmentsNeverBlank。本文件留作守卫：以后谁引入
//   写死的静态颜色/新的空白标签，这里会直接红。
// 变异验证（注入"文字与底同色"后守卫必须红）：状态条 6 条（含 wpG 的 s5g）+ 守卫 4 条 + 对话框背景 1 条全部被抓；唯一幸存的是把
//   OpaqueDialogStyle 里 QDialog 自身的 color 改成 palette(window)——这是等价变异：Qt 样式表默认不把
//   color 继承给子控件（QMessageBox 里的标签不读它），而同一规则里的 background 一改立刻被抓
//   （DIALOG 深/浅各 4 处）。所以别为让它变红去改探针。
//   已知盲区：int3 面板的 Diverged 警示行、地址簿 Read/Stale 取值色只有数值配对（PairContrast）把关，没有逐像素渲染。
// 入口：RunDarkLabelTests（由 wpJ6_main.cpp 调用）。
// ============================================================
#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/theme.h"
#include "../../../Ksword5.1/Ksword5.1/UI/GlobalUiBaseStyle.h"
#include "../../../Ksword5.1/Ksword5.1/UI/UI.css/UI_css.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvas.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/Int3Controller.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchConfirmations.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchStringWriteDialog.h"
#include "../../../shared/evidence/memory_workbench/MemoryWritePolicy.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchHexPane.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchShared.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QHeaderView>
#include <QMainWindow>
#include <QApplication>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <memory>
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTextDocumentFragment>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <tuple>

namespace wpj6_test
{
    namespace
    {
        // 判据阈值。
        constexpr int kBlankDistance = 40;     // 曼哈顿色距（|dR|+|dG|+|dB|）小于它：文字与底色几乎同色。
        constexpr double kLowContrast = 2.0;   // WCAG 对比度小于它：能看见但很费劲（只报告，不判失败）。

        struct Finding
        {
            QString scenario;
            QString className;
            QString objectName;
            QString text;
            QRect rect;
            QColor background;
            QColor farthest;
            int distance = 0;
            double ratio = 0.0;
            bool blank = false;
        };

        double Luminance(const QColor& color)
        {
            const auto channel = [](const double value) {
                return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
        }

        double Contrast(const QColor& a, const QColor& b)
        {
            const double la = Luminance(a);
            const double lb = Luminance(b);
            const double hi = std::max(la, lb);
            const double lo = std::min(la, lb);
            return (hi + 0.05) / (lo + 0.05);
        }

        // PlainText：把控件文字规整成纯文本（富文本 QLabel 要去标签），空白串视为无文字。
        QString PlainText(QWidget* widget)
        {
            QString text;
            if (auto* label = qobject_cast<QLabel*>(widget))
            {
                text = label->text();
                if (label->textFormat() == Qt::RichText || (label->textFormat() == Qt::AutoText && text.contains(QLatin1Char('<'))))
                {
                    text = QTextDocumentFragment::fromHtml(text).toPlainText();
                }
            }
            else if (auto* button = qobject_cast<QAbstractButton*>(widget))
            {
                text = button->text();
            }
            return text.trimmed();
        }

        // g_scanned：本进程累计检查过的"有文字且可见"的控件数（探针自检：数字为 0 就说明探针没扫到东西）。
        int g_scanned = 0;

        // SaveShot：把窗口渲染图存成 PNG，仅当设置了环境变量 MEMWB_DARKLABEL_SHOTS 才落盘（存几十张图要花
        // 十几秒，日常回归不需要，排查时再打开）。传入：widget 要截的窗口；name 文件名（不含扩展名）。
        void SaveShot(QWidget* widget, const QString& name)
        {
            if (qEnvironmentVariableIsEmpty("MEMWB_DARKLABEL_SHOTS"))
            {
                return;
            }
            const QString dir = qEnvironmentVariable("MEMWB_OUT") + QStringLiteral("/dark_shots");
            QDir().mkpath(dir);
            widget->grab().save(dir + QStringLiteral("/") + name + QStringLiteral(".png"));
        }

        // Evaluate：判定渲染图里某个矩形（文字所在区域）是空白还是低对比，命中则追加一条发现。
        // 传入：image 整个视图的渲染图；rect 文字所在矩形（视图坐标）；text 该处应显示的文字；
        //       className/objectName 来源控件标识；scenario 场景名；findings 输出。
        void Evaluate(
            const QImage& image,
            QRect rect,
            const QString& text,
            const QString& className,
            const QString& objectName,
            const QString& scenario,
            std::vector<Finding>& findings)
        {
            rect = rect.intersected(image.rect());
            if (rect.width() < 6 || rect.height() < 6)
            {
                return;
            }
            ++g_scanned;
            // 底色 = 矩形内出现最多的颜色。
            std::map<QRgb, int> histogram;
            for (int y = rect.top(); y <= rect.bottom(); ++y)
            {
                for (int x = rect.left(); x <= rect.right(); ++x)
                {
                    ++histogram[image.pixel(x, y)];
                }
            }
            QRgb background = 0;
            int best = -1;
            for (const auto& [rgb, count] : histogram)
            {
                if (count > best)
                {
                    best = count;
                    background = rgb;
                }
            }
            QRgb farthest = background;
            int farthestDistance = 0;
            for (const auto& [rgb, count] : histogram)
            {
                Q_UNUSED(count);
                const int distance = std::abs(qRed(rgb) - qRed(background)) + std::abs(qGreen(rgb) - qGreen(background))
                    + std::abs(qBlue(rgb) - qBlue(background));
                if (distance > farthestDistance)
                {
                    farthestDistance = distance;
                    farthest = rgb;
                }
            }
            const double ratio = Contrast(QColor(background), QColor(farthest));
            const bool blank = farthestDistance < kBlankDistance;
            if (!blank && ratio >= kLowContrast)
            {
                return;
            }
            Finding finding;
            finding.scenario = scenario;
            finding.className = className;
            finding.objectName = objectName;
            finding.text = text.left(28);
            finding.rect = rect;
            finding.background = QColor(background);
            finding.farthest = QColor(farthest);
            finding.distance = farthestDistance;
            finding.ratio = ratio;
            finding.blank = blank;
            findings.push_back(finding);
        }

        // ScanItemViews：表格/树/列表的单元格与表头分节（它们不是 QLabel，文字由委托自绘，
        // 颜色来自 ForegroundRole/调色板，最容易出现"写死的静态色在深色下与底色同色"）。
        // 传入：view 被检视的视图（枚举子控件的根）；top 渲染图所属的最顶层窗口（view 嵌在祖先里时，
        //       祖先的样式/底色也要一起渲染，才和用户真实看到的一致）；image 是 top 的渲染图。
        void ScanItemViews(
            QWidget* view, QWidget* top, const QImage& image, const QString& scenario, std::vector<Finding>& findings)
        {
            for (QAbstractItemView* itemView : view->findChildren<QAbstractItemView*>())
            {
                if (!itemView->isVisibleTo(view) || itemView->model() == nullptr)
                {
                    continue;
                }
                const QString viewClass = QStringLiteral("ItemView:") + QString::fromLatin1(itemView->metaObject()->className());
                QAbstractItemModel* model = itemView->model();
                const QRect viewportRect = itemView->viewport()->rect();
                const int rows = std::min(model->rowCount(itemView->rootIndex()), 60);
                const int cols = std::min(model->columnCount(itemView->rootIndex()), 16);
                for (int row = 0; row < rows; ++row)
                {
                    for (int col = 0; col < cols; ++col)
                    {
                        const QModelIndex index = model->index(row, col, itemView->rootIndex());
                        const QString text = index.data(Qt::DisplayRole).toString().trimmed();
                        if (text.isEmpty())
                        {
                            continue;
                        }
                        QRect cell = itemView->visualRect(index).intersected(viewportRect);
                        if (cell.width() < 6 || cell.height() < 6)
                        {
                            continue;
                        }
                        cell.moveTopLeft(itemView->viewport()->mapTo(top, cell.topLeft()));
                        Evaluate(image, cell, text, viewClass, itemView->objectName(), scenario, findings);
                    }
                }
                // 表头分节。
                if (auto* table = qobject_cast<QTableView*>(itemView))
                {
                    QHeaderView* header = table->horizontalHeader();
                    if (header != nullptr && header->isVisibleTo(view))
                    {
                        for (int logical = 0; logical < std::min(header->count(), 16); ++logical)
                        {
                            if (header->isSectionHidden(logical))
                            {
                                continue;
                            }
                            const QString text = model->headerData(logical, Qt::Horizontal).toString().trimmed();
                            if (text.isEmpty())
                            {
                                continue;
                            }
                            QRect section(header->sectionViewportPosition(logical), 0, header->sectionSize(logical), header->height());
                            section = section.intersected(header->rect());
                            if (section.width() < 6)
                            {
                                continue;
                            }
                            section.moveTopLeft(header->mapTo(top, section.topLeft()));
                            Evaluate(image, section, text, QStringLiteral("Header:") + QString::fromLatin1(table->metaObject()->className()),
                                table->objectName(), scenario, findings);
                        }
                    }
                }
                else if (auto* tree = qobject_cast<QTreeView*>(itemView))
                {
                    QHeaderView* header = tree->header();
                    if (header != nullptr && header->isVisibleTo(view))
                    {
                        for (int logical = 0; logical < std::min(header->count(), 16); ++logical)
                        {
                            const QString text = model->headerData(logical, Qt::Horizontal).toString().trimmed();
                            if (text.isEmpty() || header->isSectionHidden(logical))
                            {
                                continue;
                            }
                            QRect section(header->sectionViewportPosition(logical), 0, header->sectionSize(logical), header->height());
                            section = section.intersected(header->rect());
                            if (section.width() < 6)
                            {
                                continue;
                            }
                            section.moveTopLeft(header->mapTo(top, section.topLeft()));
                            Evaluate(image, section, text, QStringLiteral("Header:") + QString::fromLatin1(tree->metaObject()->className()),
                                tree->objectName(), scenario, findings);
                        }
                    }
                }
            }
        }

        // Scan：渲染 view，逐个有文字的可见控件（标签/按钮/表格单元格/表头）判空白/低对比。
        // 传入：view 被检视的视图；top 渲染图所属的最顶层窗口（view 就是顶层时两者相同）；scenario 场景名。
        std::vector<Finding> Scan(QWidget* view, QWidget* top, const QString& scenario)
        {
            std::vector<Finding> findings;
            const QImage image = top->grab().toImage().convertToFormat(QImage::Format_RGB32);
            for (QWidget* widget : view->findChildren<QWidget*>())
            {
                if (!widget->isVisibleTo(view))
                {
                    continue;
                }
                const QString text = PlainText(widget);
                if (text.isEmpty())
                {
                    continue;
                }
                // 被布局压成（近乎）零尺寸的文字控件：渲染出来必然是空白，与主题无关，单独记一类。
                if (widget->width() < 6 || widget->height() < 6)
                {
                    // 单个标点（状态条里的 "|" 分隔符本来就只有 3px 宽）不算文字被压没。
                    if (text.size() == 1 && !text.at(0).isLetterOrNumber())
                    {
                        continue;
                    }
                    Finding collapsed;
                    collapsed.scenario = scenario;
                    collapsed.className = QString::fromLatin1(widget->metaObject()->className());
                    collapsed.objectName = widget->objectName();
                    collapsed.text = text.left(28);
                    collapsed.rect = QRect(widget->mapTo(top, QPoint(0, 0)), widget->size());
                    collapsed.blank = true;
                    collapsed.distance = -1;   // -1 = 尺寸塌缩（不是颜色问题）
                    findings.push_back(collapsed);
                    continue;
                }
                const QRect rect(widget->mapTo(top, QPoint(0, 0)), widget->size());
                Evaluate(image, rect, text, QString::fromLatin1(widget->metaObject()->className()), widget->objectName(),
                    scenario, findings);
            }
            ScanItemViews(view, top, image, scenario, findings);
            return findings;
        }

        // Exercise：把视图推到"尽量多的文字都在屏幕上"的状态，再扫描。返回本场景全部发现。
        // 传入：harness 持有视图；tag 路径名；top 最顶层窗口（视图嵌在祖先里时是那个祖先，否则就是视图本身）。
        std::vector<Finding> Exercise(Harness& harness, const QString& tag, QWidget* top)
        {
            std::vector<Finding> all;
            auto* view = harness.view.get();
            top->resize(900, 700);
            top->show();
            PumpFor(120);

            const auto collect = [&](const QString& step) {
                PumpFor(60);
                // 截图落盘供人工复核（文件名 = 路径 + 步骤；默认不存，见 SaveShot）。
                {
                    QString name = tag + QStringLiteral("_") + step;
                    name.replace(QLatin1Char('>'), QLatin1Char('_')).replace(QLatin1Char('-'), QLatin1Char('_'));
                    SaveShot(top, name);
                }
                for (const auto& finding : Scan(view, top, tag + QStringLiteral("/") + step))
                {
                    all.push_back(finding);
                }
            };

            collect(QStringLiteral("hex"));

            // 状态条：各种文案（读结果警告、写结果、窗口范围、保护、诊断）。
            auto* status = view->statusBarForTest();
            status->setReadResultText(QStringLiteral("读取失败（3 处可重试）"), true);
            status->setWriteResultText(QStringLiteral("已写入 2 字节，回读一致"));
            status->setWindowRangeText(QStringLiteral("窗口 0x0–0x1FFF"));
            status->setProtection(QStringLiteral("RWX"), ks::ui::StatusRole::Error);
            status->setDiagnosticsText(QStringLiteral("诊断：目标可能不可读"), true);
            status->reportScratchAreaDirty(true);
            status->setReadModifyWriteWindow(true);
            status->setNeedsReread(true);
            collect(QStringLiteral("status-all"));
            status->setProtection(QStringLiteral("RW"), ks::ui::StatusRole::Success);
            collect(QStringLiteral("status-protect-ok"));
            status->setProtection(QStringLiteral("X"), ks::ui::StatusRole::Warning);
            collect(QStringLiteral("status-protect-warn"));

            // 会话条：待写入芯片。
            view->sessionBarForTest()->setPendingPatches(12, 3);
            collect(QStringLiteral("pending-chip"));

            // 地址栏错误态（标红）。
            view->findChild<QLineEdit*>()->setText(QStringLiteral("not a valid ((expr"));
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::sendEvent(view->findChild<QLineEdit*>(), &press);
            collect(QStringLiteral("address-error"));

            // "切换并跳转"钮：内核半区地址在进程范围下。
            ks::ui::NavRequest kernel;
            kernel.address = 0xFFFFF78000000000ULL;
            view->openAt(kernel);
            collect(QStringLiteral("reroute-button"));

            // int3 条目与地址簿条目。
            const std::uint64_t address = 0xB0ULL;
            WaitForStageable(view->hexPaneForTest(), address);
            QMenu* menu = view->hexPaneForTest()->canvas()->buildContextMenu(address, true);
            if (menu != nullptr)
            {
                for (auto* action : menu->actions())
                {
                    if (action->text().contains(QStringLiteral("int3")))
                    {
                        emit action->triggered();
                        break;
                    }
                }
                delete menu;
            }
            collect(QStringLiteral("int3-entry"));

            // 反汇编页要有真实行才能测到"变化底色上的文字"：注入一个只认 00 00 的假解码器
            // （其余字节解不出来 = 退化成 db 占位行，走 TextSecondary 前景），再造出三种变化底色：
            //   外部变化（改假内存后重读）、暂存、自己写入（提交后）。
            view->setDisasmBackends(
                [](const std::uint8_t* bytes, const std::size_t available, const std::uint64_t address, const bool) {
                    std::optional<ks::ui::DecodedRow> row;
                    if (available >= 2U && bytes[0] == 0U && bytes[1] == 0U)
                    {
                        ks::ui::DecodedRow decoded;
                        decoded.address = address;
                        decoded.bytes = QByteArray(2, '\0');
                        decoded.mnemonic = QStringLiteral("add");
                        decoded.operands = QStringLiteral("byte ptr [rax], al");
                        decoded.decoded = true;
                        row = decoded;
                    }
                    return row;
                },
                ks::ui::AssembleOneFn());
            {
                auto& backend = ConfigureSharedOnce();
                std::lock_guard<std::mutex> lock(backend.backing->mutex);
                backend.backing->bytes[0x10] = 0x90;   // 外部变化：重读时这一字节与上次不同
            }
            view->hexPaneForTest()->rereadWindow();
            PumpFor(150);
            // 反汇编页要"定位"之后才有行：先把插入点落到 0x20（baseline 窗口覆盖到它），再切到反汇编页并刷新。
            {
                ks::ui::NavRequest nav;
                nav.address = 0x20ULL;
                (void)view->openAt(nav);
                (void)WaitForStageable(view->hexPaneForTest(), 0x20ULL);
            }
            view->subTabStackForTest()->setCurrentIndex(1);
            if (auto* disasm = qobject_cast<ks::ui::WorkbenchDisasmView*>(view->subTabStackForTest()->widget(1)))
            {
                // 从 0x0C 解码：0x10 那个被外部改成 0x90 的字节解不出来 → db 占位行（TextSecondary 前景）
                // 压在"外部变化"的青色底上，正是最容易字与底接近的配对，并且必须在可见区里。
                (void)disasm->jumpTo(0x0CULL);
            }
            PumpFor(150);
            collect(QStringLiteral("disasm-rows"));
            if (view->writeControllerForTest()->mode() == ksword::memwb::WriteMode::Immediate)
            {
                (void)view->writeControllerForTest()->requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply);
            }
            if (WaitForStageable(view->hexPaneForTest(), 0x24ULL))
            {
                QString reason;
                view->hexPaneForTest()->canvas()->stageBytes(0x24ULL, QByteArray(1, '\x77'), &reason);
            }
            if (auto* disasm = qobject_cast<ks::ui::WorkbenchDisasmView*>(view->subTabStackForTest()->widget(1)))
            {
                (void)disasm->jumpTo(0x20ULL);   // 0x23/0x24 附近：暂存的橙色底行与 db 占位行
            }
            PumpFor(100);
            collect(QStringLiteral("disasm-pending"));
            (void)view->writeControllerForTest()->commitPendingNow();
            PumpFor(150);
            if (auto* disasm = qobject_cast<ks::ui::WorkbenchDisasmView*>(view->subTabStackForTest()->widget(1)))
            {
                (void)disasm->jumpTo(0x20ULL);   // 同一处：提交后变成"自己写入"的绿色底
            }
            PumpFor(100);
            collect(QStringLiteral("disasm-self-written"));
            view->subTabStackForTest()->setCurrentIndex(0);
            collect(QStringLiteral("hex-change-colors"));

            // 其它三个子页。
            for (int index = 1; index <= 3; ++index)
            {
                view->subTabStackForTest()->setCurrentIndex(index);
                collect(QStringLiteral("subtab-%1").arg(index));
            }
            view->subTabStackForTest()->setCurrentIndex(0);

            // 范围与通道：内核/物理范围（没有进程的目标 chip 文案）、HVM/DDMA 通道（门不可用时的警告）。
            // 切换身份会触发离开守卫；账本里还有未还原的 int3 补丁时守卫会弹真实的 QMessageBox，
            // 离屏环境没人点它，整个进程会永远阻塞在 exec()——所以切换之前先清空账本。
            {
                auto& int3Ledger = ks::ui::WorkbenchShared::Instance().Int3();
                const auto pendingEntries = int3Ledger.Entries();
                for (const auto& entry : pendingEntries)
                {
                    int3Ledger.Discard(entry.id);
                }
            }
            for (const auto scope : {ksword::memwb::Scope::KernelVirtual, ksword::memwb::Scope::Physical})
            {
                (void)view->target().requestScope(scope);
                collect(scope == ksword::memwb::Scope::KernelVirtual ? QStringLiteral("scope-kernel") : QStringLiteral("scope-physical"));
            }
            (void)view->target().requestScope(ksword::memwb::Scope::ProcessVirtual);
            for (const auto channel : {ksword::memwb::Channel::StandardDriver, ksword::memwb::Channel::Hvm, ksword::memwb::Channel::Ddma})
            {
                (void)view->target().requestChannel(channel);
                collect(QStringLiteral("channel-%1").arg(static_cast<int>(channel)));
            }
            (void)view->target().requestChannel(ksword::memwb::Channel::UserMode);

            // 窄宽度。
            top->resize(360, 700);
            collect(QStringLiteral("narrow"));

            // 清理共享 int3 账本与字节，免得污染后面的场景。
            auto& backend = ConfigureSharedOnce();
            auto& int3 = ks::ui::WorkbenchShared::Instance().Int3();
            const auto entries = int3.Entries();
            for (const auto& entry : entries)
            {
                int3.Discard(entry.id);
            }
            {
                std::lock_guard<std::mutex> lock(backend.backing->mutex);
                backend.backing->bytes[address - backend.backing->base] = 0x00;
                backend.backing->bytes[0x10] = 0x00;   // 外部变化场景改过
                backend.backing->bytes[0x24] = 0x00;   // 暂存/提交场景改过
            }
            top->hide();
            return all;
        }

        void Print(QStringList& lines, const std::vector<Finding>& findings)
        {
            for (const auto& f : findings)
            {
                lines << QStringLiteral("%1 | %2%3 | \"%4\" | rect=%5,%6 %7x%8 | bg=%9 text=%10 dist=%11 ratio=%12 | %13")
                             .arg(f.scenario, f.className)
                             .arg(f.objectName.isEmpty() ? QString() : QStringLiteral("#") + f.objectName)
                             .arg(f.text)
                             .arg(f.rect.x()).arg(f.rect.y()).arg(f.rect.width()).arg(f.rect.height())
                             .arg(f.background.name(), f.farthest.name())
                             .arg(f.distance)
                             .arg(f.ratio, 0, 'f', 2)
                             .arg(f.blank ? QStringLiteral("BLANK") : QStringLiteral("low"));
            }
        }

        // SelfCheck：探针对"已知坏"和"已知好"的判别力。造一个文字与底色同色的 QLabel（必须判空白）
        // 和一个正常对比的 QLabel（必须不报）。
        void SelfCheck()
        {
            QWidget host;
            host.setStyleSheet(QStringLiteral("QWidget{background:#202024;}"));
            host.resize(220, 80);
            auto* bad = new QLabel(QStringLiteral("blank label"), &host);
            bad->setStyleSheet(QStringLiteral("color:#202024;background:transparent;"));
            bad->setGeometry(5, 5, 200, 30);
            auto* good = new QLabel(QStringLiteral("good label"), &host);
            good->setStyleSheet(QStringLiteral("color:#E6E6E8;background:transparent;"));
            good->setGeometry(5, 40, 200, 30);
            host.show();
            PumpFor(50);
            const auto found = Scan(&host, &host, QStringLiteral("selfcheck"));
            bool badFlagged = false;
            bool goodFlagged = false;
            for (const auto& f : found)
            {
                badFlagged = badFlagged || (f.text == QStringLiteral("blank label") && f.blank);
                goodFlagged = goodFlagged || f.text == QStringLiteral("good label");
            }
            WPJ6_CHECK_NOTE(badFlagged, QStringLiteral("探针自检：文字与底色同色的标签必须判空白"));
            WPJ6_CHECK_NOTE(!goodFlagged, QStringLiteral("探针自检：正常对比的标签不得被报"));
            host.hide();
        }

        // ApplyRealAppTheme：尽量贴近真实应用的主题环境（MainWindow::applyAppearanceSettings）：
        // ① 调色板按 KswordTheme 令牌逐项设置（与 MainWindow 同一批角色）；
        // ② QApplication 样式表装上真实的全局基础控件样式块（含 QLabel[ksword_status_role] 状态色规则、
        //    QPushButton/QLineEdit/QHeaderView 基线）——夹具原先的 ApplyTheme 只改调色板，这些规则全不生效，
        //    所以靠状态角色上色的标签在夹具里永远看起来"正常"，真实应用里可能不是。
        // 传入：dark 目标主题。
        // mainBackground：用户自定义主背景色（"#RRGGBB"），空串=用默认主题背景。
        void ApplyRealAppTheme(const bool dark, const QString& mainBackground = QString())
        {
            KswordTheme::SetDarkModeEnabled(dark);
            KswordTheme::SetMainBackgroundColor(mainBackground);
            QPalette palette = QApplication::palette();
            palette.setColor(QPalette::Window, KswordTheme::MainBackgroundColor());
            palette.setColor(QPalette::WindowText, KswordTheme::MainBackgroundTextColor());
            palette.setColor(QPalette::Base, KswordTheme::SurfaceColor());
            palette.setColor(QPalette::AlternateBase, KswordTheme::SurfaceAltColor());
            palette.setColor(QPalette::Mid, KswordTheme::BorderColor());
            palette.setColor(QPalette::Midlight, KswordTheme::BorderStrongColor());
            palette.setColor(QPalette::Dark, KswordTheme::PaletteDarkColor());
            palette.setColor(QPalette::Text, KswordTheme::TextPrimaryColor());
            palette.setColor(QPalette::PlaceholderText, KswordTheme::TextSecondaryColor());
            palette.setColor(QPalette::Button, KswordTheme::SurfaceAltColor());
            palette.setColor(QPalette::ButtonText, KswordTheme::TextPrimaryColor());
            palette.setColor(QPalette::Highlight, KswordTheme::PrimaryBlueColor);
            palette.setColor(QPalette::HighlightedText, KswordTheme::OnAccentColor());
            QApplication::setPalette(palette);
            qApp->setStyleSheet(ks::ui::BuildGlobalBaseControlStyleBlock());
        }

        // ClearRealAppStyle：还原成夹具默认（无应用样式表），不让真实样式污染后面的测试。
        void ClearRealAppStyle()
        {
            qApp->setStyleSheet(QString());
            KswordTheme::SetMainBackgroundColor(QString());
        }

        // PairContrast：代码里出现的"文字色 / 底色"配对在当前主题下的对比度（数值判据，不依赖渲染）。
        // 配对清单来自对 MemoryWorkbench 目录的逐文件阅读：状态角色标签、反汇编变化行、int3 面板警示行、
        // 地址簿取值状态、会话条目标芯片。返回人类可读的行（对比度从低到高排序）。
        QStringList PairContrast(const QString& themeName)
        {
            struct Pair
            {
                QString what;
                QColor foreground;
                QColor background;
            };
            std::vector<Pair> pairs;
            const QColor window = KswordTheme::MainBackgroundColor();
            const QColor surface = KswordTheme::SurfaceColor();
            const QColor surfaceAlt = KswordTheme::SurfaceAltColor();
            const std::vector<std::pair<QString, QColor>> grounds = {
                {QStringLiteral("window"), window}, {QStringLiteral("surface"), surface}, {QStringLiteral("surfaceAlt"), surfaceAlt}};

            const std::vector<std::pair<QString, QColor>> roles = {
                {QStringLiteral("Info"), KswordTheme::InfoColor()},
                {QStringLiteral("Success"), KswordTheme::SuccessColor()},
                {QStringLiteral("Warning"), KswordTheme::WarningColor()},
                {QStringLiteral("Error"), KswordTheme::ErrorColor()},
                {QStringLiteral("TextSecondary"), KswordTheme::TextSecondaryColor()},
                {QStringLiteral("TextPrimary"), KswordTheme::TextPrimaryColor()},
                {QStringLiteral("TextDisabled"), KswordTheme::TextDisabledColor()}};
            for (const auto& role : roles)
            {
                for (const auto& ground : grounds)
                {
                    pairs.push_back({QStringLiteral("%1 on %2").arg(role.first, ground.first), role.second, ground.second});
                }
            }
            // 反汇编/画布变化底色（BlendColors(surface, 强调色, 权重)）上的文字。
            const std::vector<std::tuple<QString, KswordTheme::AccentRole, int>> kinds = {
                {QStringLiteral("pending(orange,120)"), KswordTheme::AccentRole::Orange, 120},
                {QStringLiteral("external(cyan,105)"), KswordTheme::AccentRole::Cyan, 105},
                {QStringLiteral("selfWritten(green,70)"), KswordTheme::AccentRole::Green, 70}};
            for (const auto& [name, accent, weight] : kinds)
            {
                const QColor bg = KswordTheme::BlendColors(surface, KswordTheme::AccentColor(accent), weight);
                pairs.push_back({QStringLiteral("disasm TextPrimary on %1").arg(name), KswordTheme::TextPrimaryColor(), bg});
                pairs.push_back({QStringLiteral("disasm TextSecondary(db rows) on %1").arg(name), KswordTheme::TextSecondaryColor(), bg});
            }
            // int3 面板被改过的警示行、孤立分组标题与孤立行。
            pairs.push_back({QStringLiteral("int3 diverged: Warning on WarningBackground"), KswordTheme::WarningColor(),
                KswordTheme::WarningBackgroundColor()});
            pairs.push_back({QStringLiteral("int3 diverged: TextPrimary on WarningBackground"), KswordTheme::TextPrimaryColor(),
                KswordTheme::WarningBackgroundColor()});
            pairs.push_back({QStringLiteral("int3 orphan header: TextDisabled on surface"), KswordTheme::TextDisabledColor(), surface});

            QStringList lines;
            std::vector<std::pair<double, QString>> scored;
            for (const Pair& pair : pairs)
            {
                const double ratio = Contrast(pair.foreground, pair.background);
                scored.emplace_back(ratio, QStringLiteral("%1 | %2 | fg=%3 bg=%4 ratio=%5")
                                               .arg(themeName, pair.what, pair.foreground.name(), pair.background.name())
                                               .arg(ratio, 0, 'f', 2));
            }
            std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& entry : scored)
            {
                lines << entry.second;
            }
            return lines;
        }

        // ProbeDialogs：真实的确认弹窗（WorkbenchConfirmations 的默认 QMessageBox 执行器，夹具平时用
        // 假执行器把它们整个替掉，所以弹窗里的标签深色下是否可读从来没被测过）和字符串写入对话框。
        // 做法：起一个轮询定时器，弹窗一出现就渲染+扫描+落盘，再点"取消/拒绝"关掉，模态调用随之返回。
        // 传入：lines 输出行；dark 目标主题。
        void ProbeDialogs(QStringList& lines, const bool dark)
        {
            ApplyRealAppTheme(dark);
            const QString themeName = dark ? QStringLiteral("DIALOG dark") : QStringLiteral("DIALOG light");
            std::vector<Finding> all;
            int captured = 0;
            QString currentName;
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, [&]() {
                QWidget* modal = QApplication::activeModalWidget();
                if (modal == nullptr)
                {
                    return;
                }
                ++captured;
                PumpFor(40);
                SaveShot(modal, themeName + QLatin1Char('_') + currentName);
                for (const auto& finding : Scan(modal, modal, themeName + QLatin1Char('/') + currentName))
                {
                    all.push_back(finding);
                }
                // 关闭弹窗：QMessageBox 用 reject（Esc 同效），让模态调用返回。
                modal->close();
            });
            poll.start(60);

            ksword::memwb::MemoryWritePolicy policy;
            ks::ui::WorkbenchConfirmations confirmations(nullptr, &policy);
            confirmations.SetTargetDescription(QStringLiteral("测试进程 · PID 4242 · x64"));
            confirmations.SetCurrentContext(
                ksword::memwb::WriteMode::StagedThenApply, ksword::memwb::Scope::ProcessVirtual, ksword::memwb::Channel::UserMode);

            currentName = QStringLiteral("mode-switch");
            confirmations.PromptModeSwitch(
                ksword::memwb::WriteMode::StagedThenApply, ksword::memwb::WriteMode::Immediate, 12, 3);
            currentName = QStringLiteral("leave-with-pending");
            confirmations.PromptLeaveWithPending(12, 3, QStringLiteral("退出程序"));

            ksword::memwb::UiConfirmRequest uiRequest;
            uiRequest.targetIdentity = "pid:4242@1";
            uiRequest.blocksTotal = 2;
            uiRequest.bytesTotal = 5;
            currentName = QStringLiteral("ui-confirm");
            confirmations.ConfirmUi(uiRequest);

            ksword::memwb::ApprovalRequest approval;
            approval.targetIdentity = "pid:4242@1";
            approval.blocksTotal = 2;
            approval.address = 0x7FF600001000ULL;
            approval.length = 4;
            approval.backendText = "kernel write needs explicit approval";
            currentName = QStringLiteral("approval");
            confirmations.ConfirmApproval(approval);

            // 字符串写入对话框：普通态 + ANSI 有损的错误态（预览标签走状态角色红字）。
            {
                ks::ui::WorkbenchStringWriteDialog dialog;
                dialog.setText(QStringLiteral("hello"));
                dialog.show();
                PumpFor(80);
                SaveShot(&dialog, themeName + QStringLiteral("_string-write"));
                for (const auto& finding : Scan(&dialog, &dialog, themeName + QStringLiteral("/string-write")))
                {
                    all.push_back(finding);
                }
                dialog.setEncoding(ks::ui::WorkbenchStringWriteDialog::Encoding::Ansi);
                dialog.setText(QStringLiteral("日本語🙂"));
                PumpFor(80);
                SaveShot(&dialog, themeName + QStringLiteral("_string-write-lossy"));
                for (const auto& finding : Scan(&dialog, &dialog, themeName + QStringLiteral("/string-write-lossy")))
                {
                    all.push_back(finding);
                }
                dialog.hide();
            }
            poll.stop();
            // 四个真实模态框（模式切换/离开暂存/UI 确认/协议同意）必须都被捕获到，否则"没发现问题"只是没测到。
            WPJ6_CHECK_NOTE(captured == 4, QStringLiteral("%1：应捕获 4 个模态确认框，实际 %2").arg(themeName).arg(captured));
            WPJ6_CHECK_NOTE(
                all.empty(),
                QStringLiteral("%1：弹窗里有 %2 处文字空白/低对比，例如 %3").arg(themeName).arg(all.size())
                    .arg(all.empty() ? QString() : all.front().className + QLatin1Char(' ') + all.front().text));
            Print(lines, all);
            ClearRealAppStyle();
        }

        // 判据常量：所有"文字色/底色"配对在默认深/浅主题下的最低对比度（WCAG 大号文字/界面元素 3:1）。
        // 实测最差一对是浅色主题的 TextDisabled on surfaceAlt = 3.14，留一点余量。
        constexpr double kMinPairContrast = 3.0;

        void Probe()
        {
            SelfCheck();
            QStringList lines;
            // 真实确认弹窗 + 字符串写入对话框：深/浅各一遍（断言在 ProbeDialogs 内部：必须捕获到 4 个模态框，全部可读）。
            ProbeDialogs(lines, true);
            ProbeDialogs(lines, false);
            // 数值配对：真实主题令牌下，默认深/浅各一份；最差一对必须 >= kMinPairContrast。
            for (const bool dark : {true, false})
            {
                ApplyRealAppTheme(dark);
                const QStringList pairLines = PairContrast(dark ? QStringLiteral("PAIR dark") : QStringLiteral("PAIR light"));
                // PairContrast 按对比度升序返回，第 0 条就是最差的一对。
                WPJ6_CHECK(!pairLines.isEmpty());
                if (!pairLines.isEmpty())
                {
                    const double worst = pairLines.first().section(QStringLiteral("ratio="), 1).toDouble();
                    WPJ6_CHECK_NOTE(
                        worst >= kMinPairContrast,
                        QStringLiteral("%1 最差配对对比度 %2 低于 %3：%4").arg(dark ? QStringLiteral("深色") : QStringLiteral("浅色"))
                            .arg(worst).arg(kMinPairContrast).arg(pairLines.first()));
                }
                for (int index = 0; index < std::min<int>(3, pairLines.size()); ++index)
                {
                    lines << pairLines.at(index);   // 报告文件里只留最差的 3 对，便于人看
                }
            }
            ClearRealAppStyle();
            struct Path
            {
                QString name;
                bool constructDark;   // 构造时的主题
                bool finalDark;       // 扫描时的主题（与构造不同 = 运行期切换）
                bool realStyle;       // true=真实应用环境（KswordTheme 调色板 + 全局样式块）
                bool docked = false;  // true=嵌进带真实主窗口 QSS 的 QMainWindow → QTabWidget → 容器页（MemoryDock 的真实祖先链）
                QString mainBg;       // 非空=用户自定义主背景色（只在 realStyle 路径下生效）
            };
            // 路径清单（9 条，从最初 20 条的探索矩阵里挑出各自不可替代的）：
            //  - "plain"：夹具默认的纯调色板主题（不装应用样式表），构造期就在目标主题；
            //  - "REAL"：真实应用环境（KswordTheme 调色板 + 全局基础控件样式块，状态角色颜色来自它）；
            //    含"先在另一个主题构造、再运行期切换"两个方向——静态颜色快照没跟上主题时只会在这里露馅；
            //  - "DOCK"：再嵌进带真实主窗口 QSS 的 QMainWindow → QTabWidget → 容器页（MemoryDock 的真实祖先链）；
            //  - "CUSTOM"：用户自定义主背景色，中灰与纯黑是文字/表面对比度最难处理的组合。
            const std::vector<Path> allPaths = {
                {QStringLiteral("plain dark-built"), true, true, false},
                {QStringLiteral("REAL dark-built"), true, true, true},
                {QStringLiteral("REAL light-built"), false, false, true},
                {QStringLiteral("REAL light-built->dark"), false, true, true},
                {QStringLiteral("REAL dark-built->light"), true, false, true},
                {QStringLiteral("DOCK light-built->dark"), false, true, true, true},
                {QStringLiteral("CUSTOM dark bg=#808080"), true, true, true, false, QStringLiteral("#808080")},
                {QStringLiteral("CUSTOM dark bg=#000000"), true, true, true, false, QStringLiteral("#000000")},
                {QStringLiteral("CUSTOM light bg=#808080"), false, false, true, false, QStringLiteral("#808080")},
            };
            for (const Path& path : allPaths)
            {
                const auto applyTheme = [&path](const bool dark) {
                    if (path.realStyle)
                    {
                        ApplyRealAppTheme(dark, path.mainBg);
                    }
                    else
                    {
                        ClearRealAppStyle();
                        ApplyTheme(dark);
                    }
                };
                applyTheme(path.constructDark);
                Harness harness;
                harness.AttachProcess();
                PumpUntil([]() { return true; }, 10);

                // docked：视图放进"主窗口 → QTabWidget → 容器页"这条祖先链，主窗口挂真实的
                // QSS_MainWindow_TabWidget + QSS_MainWindow_dockStyle（UI_css.h，与 MainWindow 同源）。
                std::unique_ptr<QMainWindow> mainWindow;
                QWidget* top = harness.view.get();
                if (path.docked)
                {
                    mainWindow = std::make_unique<QMainWindow>();
                    mainWindow->setStyleSheet(QSS_MainWindow_TabWidget + QSS_MainWindow_dockStyle);
                    auto* tabs = new QTabWidget(mainWindow.get());
                    tabs->setDocumentMode(true);
                    auto* container = new QWidget(tabs);
                    auto* layout = new QVBoxLayout(container);
                    layout->setContentsMargins(0, 0, 0, 0);
                    layout->addWidget(harness.view.get());
                    tabs->addTab(container, QStringLiteral("内存工作台"));
                    mainWindow->setCentralWidget(tabs);
                    top = mainWindow.get();
                }
                top->resize(900, 700);
                top->show();
                PumpFor(100);
                if (path.constructDark != path.finalDark)
                {
                    applyTheme(path.finalDark);
                    // 模拟真实应用的切换：调色板变化事件会发给所有顶层窗口并逐级传递。
                    QEvent paletteChange(QEvent::ApplicationPaletteChange);
                    QCoreApplication::sendEvent(qApp, &paletteChange);
                    for (QWidget* widget : harness.view->findChildren<QWidget*>())
                    {
                        QEvent change(QEvent::PaletteChange);
                        QCoreApplication::sendEvent(widget, &change);
                    }
                    PumpFor(120);
                }
                const int scannedBefore = g_scanned;
                const auto findings = Exercise(harness, path.name, top);
                Print(lines, findings);
                // 探针自检：每条路径都必须真的扫到足够多的文字单元，"零发现"才有意义。
                WPJ6_CHECK_NOTE(
                    g_scanned - scannedBefore >= 300,
                    QStringLiteral("%1：只扫描到 %2 个文字单元，探针没覆盖到界面").arg(path.name).arg(g_scanned - scannedBefore));
                WPJ6_CHECK_NOTE(
                    findings.empty(),
                    QStringLiteral("%1：%2 处文字空白/塌缩/低对比，第一处：%3 %4 @ %5")
                        .arg(path.name).arg(findings.size())
                        .arg(findings.empty() ? QString() : findings.front().scenario)
                        .arg(findings.empty() ? QString() : findings.front().className + QStringLiteral(" \"") + findings.front().text + QStringLiteral("\""))
                        .arg(findings.empty() ? QString() : QStringLiteral("%1,%2 %3x%4").arg(findings.front().rect.x()).arg(findings.front().rect.y())
                                                                .arg(findings.front().rect.width()).arg(findings.front().rect.height())));
                top->hide();
                if (path.docked)
                {
                    // 视图归 Harness 管理：先从祖先链上摘下来，免得主窗口析构时重复删除。
                    harness.view->setParent(nullptr);
                }
            }
            ClearRealAppStyle();
            ApplyTheme(false);

            // 报告文件（诊断用）：有发现时才写，路径 = $MEMWB_OUT/dark_labels_probe.txt。
            // 设 MEMWB_DARKLABEL_SHOTS=1 还会把每个步骤的截图存到 $MEMWB_OUT/dark_shots/ 供人工复核。
            if (!lines.isEmpty())
            {
                const QString out = qEnvironmentVariable("MEMWB_OUT");
                QDir().mkpath(out);
                QFile file(out + QStringLiteral("/dark_labels_probe.txt"));
                if (file.open(QIODevice::WriteOnly | QIODevice::Text))
                {
                    QTextStream stream(&file);
                    stream.setEncoding(QStringConverter::Utf8);
                    stream << lines.join(QLatin1Char('\n')) << '\n';
                }
            }
            std::printf("DARKLABEL scanned=%d text cells, report lines=%d\n", g_scanned, static_cast<int>(lines.size()));
        }
    }

    void RunDarkLabelTests()
    {
        Probe();
    }
}
