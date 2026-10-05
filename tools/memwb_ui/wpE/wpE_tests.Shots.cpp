// wpE_tests.Shots.cpp
// 作用：AddressBookPanel 的截图——深/浅主题 × 窄/宽窗口 × 四个 kind 分段，供人工核对
// 文字可读、A/B 列组、载入失败横幅等外观是否符合设计。截图本身不是断言，只落盘。

#include "wpE_common.h"

#include <QApplication>
#include <QDir>
#include <QImage>

namespace wpe_test
{
    namespace
    {
        using ks::ui::AddressBookModel;
        using ks::ui::AddressBookPanel;
        using ks::ui::AddressBookStore;
        using ksword::memwb::EntryKind;
        using ksword::memwb::ValueType;

        // PopulateSampleEntries：灌入一组跨三种 kind、跨模块/绝对地址、跨值状态的样例条目，
        // 让截图里能同时看到类型图标三种样式、地址两种格式、值列五种状态的颜色。
        void PopulateSampleEntries(AddressBookStore& store, AddressBookModel& model)
        {
            const std::uint64_t idSearch = store.add(MakeAbsoluteEntry(EntryKind::Search, "proc", 0x7FF600001000ULL));
            const std::uint64_t idBookmark = store.add(MakeModuleEntry(EntryKind::Bookmark, "proc", "client.dll", 0x1A40));
            const std::uint64_t idWatch1 = store.add(MakeModuleEntry(EntryKind::Watch, "proc", "kernel32.dll", 0x3F20));
            const std::uint64_t idWatch2 = store.add(MakeAbsoluteEntry(EntryKind::Watch, "proc", 0x7FF600002000ULL));

            model.setData(model.indexForId(idBookmark, AddressBookModel::ColumnNote), QStringLiteral("玩家生命值"), Qt::EditRole);
            model.setData(model.indexForId(idWatch1, AddressBookModel::ColumnValueType), static_cast<int>(ValueType::F32), Qt::EditRole);

            model.setValueText(idSearch, QStringLiteral("00 00 80 3F"), AddressBookModel::ValueState::Read);
            model.setValueText(idBookmark, QStringLiteral("100"), AddressBookModel::ValueState::Read);
            model.setValueText(idWatch1, QString(), AddressBookModel::ValueState::Unreadable);
            model.setValueText(idWatch2, QStringLiteral("42"), AddressBookModel::ValueState::Stale);

            model.setTargetDisplayName(QStringLiteral("proc"), QStringLiteral("game.exe · PID 4321"));
        }

        // SaveShot：截取整窗并保存 PNG；失败只记一次断言，不中断其它截图。
        // 先 processEvents：kind 分段切换会重建 HexViewSegmented 并重新塞回 FlowLayout
        // （见 AddressBookPanel::refreshKindSegmentLabelsAndCounts），布局重新计算几何是
        // 排队的 LayoutRequest 事件，grab() 不会主动替布局管理器把这一步跑完，不等的话
        // 截图里刚重建的分段控件会是 0 尺寸（截不出来）。
        void SaveShot(QWidget& widget, const QString& shotsDir, const QString& name)
        {
            QApplication::processEvents();
            QDir().mkpath(shotsDir);
            const QImage image = widget.grab().toImage();
            const bool saved = image.save(QDir(shotsDir).filePath(name + QStringLiteral(".png")));
            WPE_CHECK_NOTE(saved, name);
        }
    }

    void RunShotTests(const QString& shotsDir)
    {
        for (const bool dark : { false, true })
        {
            ApplyTheme(dark);
            const QString themeTag = dark ? QStringLiteral("dark") : QStringLiteral("light");

            for (const bool narrow : { false, true })
            {
                const QString widthTag = narrow ? QStringLiteral("narrow") : QStringLiteral("wide");

                AddressBookStore store(QString{});
                AddressBookModel model(&store);
                PopulateSampleEntries(store, model);
                AddressBookPanel panel(&model);
                panel.resize(narrow ? 280 : 640, 360);
                panel.show();

                // 全部（默认分段）：展示四种 kind 混排、A 列组默认文字可读性。
                SaveShot(panel, shotsDir, QStringLiteral("panel_%1_%2_all").arg(themeTag, widthTag));

                // 切到 B 列组：地址·值类型·模块+RVA·目标。
                panel.applyColumnGroup(AddressBookPanel::ColumnGroup::PresetB);
                SaveShot(panel, shotsDir, QStringLiteral("panel_%1_%2_presetB").arg(themeTag, widthTag));
                panel.applyColumnGroup(AddressBookPanel::ColumnGroup::PresetA);

                // 各 kind 分段（仅在"宽"尺寸下截，避免产物翻倍，窄尺寸已由上面两张覆盖换行效果）。
                if (!narrow)
                {
                    panel.setKindFilterIndex(0);  // 搜索
                    SaveShot(panel, shotsDir, QStringLiteral("panel_%1_search").arg(themeTag));
                    panel.setKindFilterIndex(1);  // 书签
                    SaveShot(panel, shotsDir, QStringLiteral("panel_%1_bookmark").arg(themeTag));
                    panel.setKindFilterIndex(2);  // 监视
                    SaveShot(panel, shotsDir, QStringLiteral("panel_%1_watch").arg(themeTag));
                    panel.setKindFilterIndex(-1);

                    // 载入失败横幅。
                    panel.showLoadFailure(QStringLiteral("第 7 行：unknown entry kind"));
                    SaveShot(panel, shotsDir, QStringLiteral("panel_%1_load_failure").arg(themeTag));

                    // 修复 D2 的可视核验：带备份路径的那一版——之前用 "\n" 换行会被
                    // HexViewMessageLabel（单行高度、单行省略绘制）直接吃掉下半截，备份
                    // 路径完全看不见；现在改成不换行、把路径放在最前面，这张截图应该能
                    // 看到完整的一行，开头是备份路径，后面接着"载入失败："与原因。
                    panel.showLoadFailure(
                        QStringLiteral("第 7 行：unknown entry kind"),
                        QStringLiteral("C:/Users/Example/book.addrbook.bad-20261004-120000"));
                    SaveShot(panel, shotsDir, QStringLiteral("panel_%1_load_failure_with_backup").arg(themeTag));
                    panel.showLoadFailure(QString());
                }

                panel.hide();
            }
        }
        ApplyTheme(false);
    }
}
