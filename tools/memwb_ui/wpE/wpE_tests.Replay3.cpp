// wpE_tests.R3.cpp —— 第三轮验证补测（叠加在 wpE 夹具副本上，不改仓库）。
// 每个测试注明"杀死哪个第二轮幸存变异 / 哪条修复的回退"。对未变异代码必须全部 PASS。

#include "wpE_common.h"

#include "../../../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"
#include "../../../Ksword5.1/Ksword5.1/theme.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewWidgets.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QMenu>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTableView>
#include <QTimer>

#include <cmath>
#include <functional>
#include <optional>
#include <vector>

#include <windows.h>

using ks::ui::AddressBookModel;
using ks::ui::AddressBookPanel;
using ks::ui::AddressBookStore;
using ks::ui::HexViewMessageLabel;
using ksword::memwb::EntryKind;
using namespace wpe_test;

namespace
{
    bool HasHan(const QString& text)
    {
        for (const QChar ch : text)
        {
            if (ch.unicode() >= 0x4E00 && ch.unicode() <= 0x9FFF)
            {
                return true;
            }
        }
        return false;
    }

    HexViewMessageLabel* FindMessageLabel(AddressBookPanel& panel)
    {
        for (QWidget* const widget : panel.findChildren<QWidget*>())
        {
            if (HexViewMessageLabel* const label = dynamic_cast<HexViewMessageLabel*>(widget))
            {
                return label;
            }
        }
        return nullptr;
    }

    QPushButton* FindButton(AddressBookPanel& panel, const QString& text)
    {
        for (QPushButton* const button : panel.findChildren<QPushButton*>())
        {
            if (button->text() == text)
            {
                return button;
            }
        }
        return nullptr;
    }

    bool ColorsClose(const QColor& a, const QColor& b, const int tolerance = 3)
    {
        return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance
            && std::abs(a.blue() - b.blue()) <= tolerance;
    }

    struct MenuItem
    {
        QString text;
        bool enabled = true;
    };

    // CaptureMenu：定时器轮询抓取弹出的 QMenu（顶层动作）的文本/使能，随后关掉菜单让 exec() 返回。
    std::vector<MenuItem> CaptureMenu(const std::function<void()>& openMenu)
    {
        std::vector<MenuItem> items;
        int ticks = 0;
        QTimer poller;
        poller.setInterval(60);
        QObject::connect(&poller, &QTimer::timeout, &poller, [&]() {
            ++ticks;
            QMenu* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menu != nullptr)
            {
                for (QAction* const action : menu->actions())
                {
                    if (!action->isSeparator())
                    {
                        items.push_back({ action->text(), action->isEnabled() });
                    }
                }
                menu->close();
                poller.stop();
                return;
            }
            if (ticks >= 40)
            {
                poller.stop();
            }
        });
        poller.start();
        openMenu();
        return items;
    }

    std::optional<bool> EnabledOf(const std::vector<MenuItem>& items, const QString& prefix)
    {
        for (const MenuItem& item : items)
        {
            if (item.text.startsWith(prefix))
            {
                return item.enabled;
            }
        }
        return std::nullopt;
    }

    void WriteRaw(const QString& path, const QByteArray& bytes)
    {
        QFile file(path);
        file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.write(bytes);
        file.close();
    }

    // ---- 杀 RA12e：载入失败横幅（"载入失败：%1" 外层模板 + 备份句）en-US 下不得残留汉字。
    // 第二轮补测 TestI18n_EnglishUiHasNoHan 调了 showLoadFailure 但**从未读取横幅文本**
    // （原版有 noHan(MsgLabel(panel)->text())，并入夹具时丢了这一行），撤回 i18n 照样全绿。
    void TestR3_BannerIsTranslatedInEnglish()
    {
        QString err;
        ks::i18n::LanguageManager::instance().initialize(QStringLiteral("zh-CN"), &err);
        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("en-US"), &err);
        QApplication::processEvents();
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            HexViewMessageLabel* const label = FindMessageLabel(panel);
            WPE_CHECK(label != nullptr);
            if (label != nullptr)
            {
                panel.showLoadFailure(QStringLiteral("line 3: bad"));
                WPE_CHECK_NOTE(!HasHan(label->text()), label->text());
                WPE_CHECK(label->text().contains(QStringLiteral("line 3: bad")));
                panel.showLoadFailure(QStringLiteral("line 3: bad"), QStringLiteral("C:/x.bad"));
                WPE_CHECK_NOTE(!HasHan(label->text()), label->text());
                WPE_CHECK(label->text().contains(QStringLiteral("C:/x.bad")));
            }
        }
        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("zh-CN"), &err);
        QApplication::processEvents();
    }

    // ---- 杀 RA01-C10（唯一可分辨的场景）：D11 的 changeEvent 会在每次 ApplicationPaletteChange
    // 时重新生成样式表，所以只要"主题 token 先切、调色板后切"（MainWindow.cpp:11959/11985
    // 与夹具 ApplyTheme 的顺序），固化字面色与 palette(...) 行为相同。调色板先变、token 滞后
    // 时，固化字面色（重生成那一刻读到的仍是旧 token）才会停在旧主题上，palette(base) 则
    // 已经是新调色板。本测试只用来证明两种写法并非严格等价，不是用户可见回退。
    void TestR3_UncheckedButtonFollowsPaletteWhenTokensLag()
    {
        ApplyTheme(false);
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        panel.resize(420, 180);
        panel.show();
        QApplication::processEvents();
        QPushButton* const buttonB = FindButton(panel, QStringLiteral("B"));  // 默认 A 选中，B 未选中
        WPE_CHECK(buttonB != nullptr);
        if (buttonB == nullptr)
        {
            panel.hide();
            return;
        }
        KswordTheme::SetDarkModeEnabled(true);
        const QColor darkSurface = KswordTheme::SurfaceColor();
        QPalette darkPalette = qApp->palette();
        darkPalette.setColor(QPalette::Base, darkSurface);
        darkPalette.setColor(QPalette::Text, KswordTheme::TextPrimaryColor());
        darkPalette.setColor(QPalette::Mid, KswordTheme::BorderColor());
        KswordTheme::SetDarkModeEnabled(false);   // token 仍停在亮色
        qApp->setPalette(darkPalette);            // 调色板先变成暗色
        QApplication::processEvents();
        QApplication::processEvents();
        const QColor pixel = buttonB->grab().toImage().pixelColor(3, buttonB->height() / 2);
        WPE_CHECK_NOTE(ColorsClose(pixel, darkSurface),
            QStringLiteral("未选中 B 钮底色应跟随调色板 %1，实际 %2").arg(darkSurface.name(), pixel.name()));
        panel.hide();
        ApplyTheme(false);
    }

    // ---- 杀 NC18：备份名格式 "<文件名>.bad-<yyyyMMdd-HHmmss>"（AddressBookStore.h 里写明的
    // 契约；D5 加了撞名后缀以后，把时间戳降到按天也不会再撞名，唯一性测试看不出来）。
    void TestR3_BackupNameKeepsSecondPrecisionTimestamp()
    {
        const QString dirPath = QFileInfo(ScratchFilePath(QStringLiteral("probe"))).absolutePath();
        const QString path = dirPath + QStringLiteral("/r3_bkfmt.addrbook");
        for (const QString& name : QDir(dirPath).entryList(QStringList() << QStringLiteral("r3_bkfmt*"), QDir::Files))
        {
            QFile::remove(QDir(dirPath).filePath(name));
        }
        WriteRaw(path, "KSWORD-ADDRESS-BOOK 1\nbad\n");
        AddressBookStore store(path);
        WPE_CHECK(!store.load());
        const QString name = QFileInfo(store.lastLoadBackupPath()).fileName();
        const QRegularExpression format(QStringLiteral("^r3_bkfmt\\.addrbook\\.bad-\\d{8}-\\d{6}(-\\d+)?$"));
        WPE_CHECK_NOTE(format.match(name).hasMatch(), name);
    }

    // ---- 杀 NC17：右键菜单里"复制值"的使能必须随值状态变化（非 Read 置灰，Read 可点）。
    // 第二轮 TestCopyValueMenuItemTracksValueState 名为"菜单项"，实际只调了 previewCopyText，
    // 从未读过菜单项的 isEnabled()。
    void TestR3_CopyValueMenuActionEnabledTracksValueState()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 0x1000));
        panel.resize(520, 220);
        panel.show();
        QApplication::processEvents();
        QTableView* const view = panel.findChild<QTableView*>();
        WPE_CHECK(view != nullptr);
        if (view == nullptr)
        {
            panel.hide();
            return;
        }
        const QPoint rowPos = view->visualRect(view->model()->index(0, AddressBookModel::ColumnAddress)).center();
        const AddressBookModel::ValueState staleStates[] = {
            AddressBookModel::ValueState::NotRead, AddressBookModel::ValueState::Reading,
            AddressBookModel::ValueState::Unreadable, AddressBookModel::ValueState::Stale
        };
        for (const AddressBookModel::ValueState state : staleStates)
        {
            model.setValueText(id, state == AddressBookModel::ValueState::Stale ? QStringLiteral("old") : QString(), state);
            const std::vector<MenuItem> items =
                CaptureMenu([view, rowPos]() { emit view->customContextMenuRequested(rowPos); });
            WPE_CHECK(!items.empty());
            WPE_CHECK_NOTE(EnabledOf(items, QStringLiteral("复制值")).value_or(true) == false,
                QStringLiteral("状态 %1 下复制值应置灰").arg(static_cast<int>(state)));
            WPE_CHECK(EnabledOf(items, QStringLiteral("复制地址")).value_or(false) == true);
        }
        model.setValueText(id, QStringLiteral("5"), AddressBookModel::ValueState::Read);
        const std::vector<MenuItem> readItems =
            CaptureMenu([view, rowPos]() { emit view->customContextMenuRequested(rowPos); });
        WPE_CHECK(EnabledOf(readItems, QStringLiteral("复制值")).value_or(false) == true);
        panel.hide();
    }

    // ---- 杀 X2（D3 的又一处漏网）："第 0 行：无法打开文件（%1）" 经 i18n。文件被另一进程以
    // 独占方式持有时 QFile::open 失败；errorString 是 Windows 本地化文本，可能含汉字，
    // 所以只断言 en-US 的固定前缀 "Line 0:"（语言包里该键的译文为
    // "Line 0: failed to open the file (%1)"）。
    void TestR3_OpenFailureMessageIsTranslated()
    {
        QString err;
        ks::i18n::LanguageManager::instance().initialize(QStringLiteral("zh-CN"), &err);
        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("en-US"), &err);
        const QString path = ScratchFilePath(QStringLiteral("r3_openfail"));
        QFile::remove(path);
        WriteRaw(path, "KSWORD-ADDRESS-BOOK 1\n");
        const std::wstring widePath = QDir::toNativeSeparators(path).toStdWString();
        const HANDLE lockHandle = CreateFileW(
            widePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        WPE_CHECK(lockHandle != INVALID_HANDLE_VALUE);
        if (lockHandle != INVALID_HANDLE_VALUE)
        {
            AddressBookStore store(path);
            WPE_CHECK(!store.load());
            WPE_CHECK(store.lastLoadFailed());
            WPE_CHECK_NOTE(store.lastLoadErrorText().startsWith(QStringLiteral("Line 0:")),
                store.lastLoadErrorText());
            CloseHandle(lockHandle);
        }
        QFile::remove(path);
        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("zh-CN"), &err);
    }

    // ---- 杀 X6（D6 第二部分的"当前格"一半）：批量变更（removeMany -> reset）之后，没被删的
    // 那一行不仅要仍在选区里，当前格也必须恢复——第二轮 TestD6b 只断言了 selectedIds()。
    void TestR3_ResetRestoresCurrentCellNotJustSelection()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        const std::uint64_t a = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        const std::uint64_t b = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 3));
        panel.resize(480, 200);
        panel.show();
        QApplication::processEvents();
        QTableView* const view = panel.findChild<QTableView*>();
        WPE_CHECK(view != nullptr);
        if (view == nullptr)
        {
            panel.hide();
            return;
        }
        view->selectionModel()->setCurrentIndex(
            view->model()->index(1, AddressBookModel::ColumnAddress),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        WPE_CHECK(view->currentIndex().data(AddressBookModel::IdRole).value<quint64>() == b);
        store.removeMany({ a });  // 整表 reset，与 b 无关
        QApplication::processEvents();
        WPE_CHECK(panel.selectedIds().size() == 1 && panel.selectedIds().front() == b);
        WPE_CHECK_NOTE(view->currentIndex().isValid()
                && view->currentIndex().data(AddressBookModel::IdRole).value<quint64>() == b,
            QStringLiteral("reset 之后当前格应恢复到仍存活的那一行"));
        panel.hide();
    }

    // ---- 杀 D1b（flushNow 路径的退避重试被撤回）：第二轮的 D1 回归测试只走了"定时器路径"
    // 写失败（500ms 防抖到期），显式 flushNow() 写失败后是否重新排期没有任何断言。
    void TestR3_FlushNowFailureKeepsRetryPending()
    {
        const QString path = ScratchFilePath(QStringLiteral("r3_flushretry"));
        QFile::remove(path);
        QDir(path).removeRecursively();
        QDir().mkpath(path);  // 把正式路径占成目录：QSaveFile 打不开，写必失败。
        AddressBookStore store(path);
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x42));
        QSignalSpy failed(&store, &AddressBookStore::saveFailed);
        WPE_CHECK(!store.flushNow());
        WPE_CHECK(failed.count() == 1);
        WPE_CHECK_NOTE(store.pendingSave(), QStringLiteral("flushNow 写失败后应重新排一次退避重试"));
        QDir().rmdir(path);  // 外部原因消除。
        WaitMs(2300);        // kSaveRetryBackoffMs = 2000
        WPE_CHECK_NOTE(QFile::exists(path), QStringLiteral("退避重试到点后应已成功落盘"));
        WPE_CHECK(store.writeCount() == 1);
        WPE_CHECK(!store.pendingSave());
        QFile::remove(path);
    }

    // ---- 杀 D3 的一处漏网：onSaveTimerTimeout（定时器路径）里的 saveFailed 文案 en-US 下
    // 不得含汉字——第二轮 TestI18n_StoreMessagesAreTranslated 只覆盖了 flushNow 路径的那一句。
    void TestR3_TimerPathSaveFailedMessageIsTranslated()
    {
        QString err;
        ks::i18n::LanguageManager::instance().initialize(QStringLiteral("zh-CN"), &err);
        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("en-US"), &err);
        const QString path = ScratchFilePath(QStringLiteral("r3_timerfail"));
        QFile::remove(path);
        QDir(path).removeRecursively();
        QDir().mkpath(path);
        {
            AddressBookStore store(path);
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x43));
            QSignalSpy failed(&store, &AddressBookStore::saveFailed);
            WaitMs(700);  // 500ms 防抖到期，定时器路径写失败
            WPE_CHECK(failed.count() == 1);
            if (failed.count() >= 1)
            {
                const QString text = failed.first().first().toString();
                WPE_CHECK_NOTE(!HasHan(text), text);
            }
            QDir().rmdir(path);
        }
        QFile::remove(path);
        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("zh-CN"), &err);
    }
}

namespace wpe_test
{
    void RunR3Tests()
    {
        TestR3_BannerIsTranslatedInEnglish();
        TestR3_UncheckedButtonFollowsPaletteWhenTokensLag();
        TestR3_BackupNameKeepsSecondPrecisionTimestamp();
        TestR3_CopyValueMenuActionEnabledTracksValueState();
        TestR3_FlushNowFailureKeepsRetryPending();
        TestR3_TimerPathSaveFailedMessageIsTranslated();
        TestR3_OpenFailureMessageIsTranslated();
        TestR3_ResetRestoresCurrentCellNotJustSelection();
    }
}
