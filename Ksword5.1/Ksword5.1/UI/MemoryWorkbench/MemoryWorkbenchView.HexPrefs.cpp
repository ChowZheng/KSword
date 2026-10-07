// ============================================================
// MemoryWorkbenchView.HexPrefs.cpp
// 作用：十六进制画布自适应相关的两件事——
// - connectHexViewMenu()：子页签那一行右侧"视图"菜单钮的接线（菜单内容、徽标/悬停说明刷新、显隐）；
// - loadHexPreferences() / saveHexPreferences()：行宽（自动/手选）、分组、字号的持久化，
//   分别由 loadSettings / saveSettings 各调用一次。
// 单独成文件的原因：MemoryWorkbenchView.cpp 已经超过项目 1000 行上限，不再往里塞新内容。
//
// 默认值与持久化规则（用户拍板）：
// - 自适应行宽只在 loadHexPreferences 这条路径里打开（WorkbenchSettings::LoadRowWidthAuto 默认 true）。
//   WorkbenchHexPane/MemoryWorkbenchView 的构造函数里不打开它，所以不调 loadSettings 的宿主/夹具
//   仍是固定 16 字节行宽；HexView 等缓冲型宿主默认也不是自适应。
// - saveHexPreferences 总是存"自动"状态；只在非自动时才存 bytesPerRow——自适应期间画布的实际行宽
//   随窗口变化，把它写进去会覆盖用户上次手选的值（下次切回手动时就不是用户选的那一档了）。
// - 非权威视图的保存由 saveSettings 的权威检查整体挡住，本文件不重复判断。
// ============================================================

#include "MemoryWorkbenchView.h"

#include "HexViewWidgets.h"
#include "WorkbenchHexPane.h"
#include "WorkbenchSettings.h"

#include <QMenu>
#include <QStackedWidget>

namespace ks::ui
{
    // connectHexViewMenu：把"视图"菜单钮接上十六进制页。
    // 作用：①给按钮装一个菜单（按钮的子对象），每次弹出前（aboutToShow）由十六进制页按当前状态与主题重建；
    //       ②行宽模式变化（含自适应换档）时刷新按钮的徽标与悬停说明；
    //       ③只在十六进制子页（子页签第 0 页）显示，切到别的子页就隐藏。
    // 调用时机：connectPanelSignals 里调用一次（那时 hexPane_/subTabStack_ 都已存在）。
    void MemoryWorkbenchView::connectHexViewMenu()
    {
        if (hexViewMenuButton_ == nullptr || hexPane_ == nullptr || subTabStack_ == nullptr)
        {
            return;
        }

        // 菜单以按钮为父对象（随按钮销毁）；内容由十六进制页在每次弹出前重建，这里只负责触发。
        // 菜单设给按钮之后按钮的建议宽度要加上下拉箭头，主动通知布局重算一次。
        QMenu* const menu = new QMenu(hexViewMenuButton_);
        hexViewMenuButton_->setMenu(menu);
        hexViewMenuButton_->updateGeometry();
        connect(menu, &QMenu::aboutToShow, this, [this, menu]() {
            if (hexPane_ != nullptr)
            {
                hexPane_->rebuildViewMenu(menu);
            }
        });

        // refreshButton：按十六进制页此刻的行宽状态刷新徽标与悬停说明。
        // 徽标与说明文字由十六进制页经 ks::i18n::sourceText 翻译好（按钮是自绘控件，运行期整树扫描够不到）。
        const auto refreshButton = [this]() {
            if (hexViewMenuButton_ == nullptr || hexPane_ == nullptr)
            {
                return;
            }
            hexViewMenuButton_->setBadgeText(hexPane_->viewButtonBadgeText());
            hexViewMenuButton_->setToolTip(hexPane_->viewButtonToolTip());
        };
        connect(hexPane_, &WorkbenchHexPane::rowWidthModeChanged, this, [refreshButton](int, bool) {
            refreshButton();
        });
        refreshButton();

        // 显隐：菜单只对十六进制子页有意义（反汇编/文本/对比页有各自的工具条）。
        const auto updateVisibility = [this](int subTabIndex) {
            if (hexViewMenuButton_ != nullptr)
            {
                hexViewMenuButton_->setVisible(subTabIndex == 0);
            }
        };
        connect(subTabStack_, &QStackedWidget::currentChanged, this, updateVisibility);
        updateVisibility(subTabStack_->currentIndex());
    }

    // loadHexPreferences：把已存的分组、字号、行宽偏好灌给十六进制页（loadSettings 里调用一次）。
    // 顺序：分组与字号先设（它们都会改变每行内容宽度），行宽偏好最后设，自适应据此只按最终值选一次档。
    // 行宽"自动"默认真：自适应在这里（而且只在这里）被打开；手选值（bytesPerRow 键）作为
    // "用户上次手选的行宽"交给十六进制页记下，自适应期间不应用、不丢失。
    void MemoryWorkbenchView::loadHexPreferences()
    {
        using namespace ks::ui::workbench_settings;

        if (hexPane_ == nullptr || hexPane_->canvas() == nullptr)
        {
            return;
        }
        HexCanvas* const canvas = hexPane_->canvas();
        canvas->setGroupSize(LoadGroupSize());
        canvas->setZoomLevel(LoadHexZoom());
        hexPane_->setRowWidthPreference(LoadRowWidthAuto(), LoadBytesPerRow());
    }

    // saveHexPreferences：把十六进制页的分组、字号、行宽偏好写盘（saveSettings 里调用一次）。
    // 自适应状态下不写 bytesPerRow（见文件头），否则会把自适应此刻选出的档当成用户的手选值。
    void MemoryWorkbenchView::saveHexPreferences() const
    {
        using namespace ks::ui::workbench_settings;

        if (hexPane_ == nullptr || hexPane_->canvas() == nullptr)
        {
            return;
        }
        const HexCanvas* const canvas = hexPane_->canvas();
        const bool automatic = hexPane_->rowWidthAutomatic();
        SaveRowWidthAuto(automatic);
        if (!automatic)
        {
            SaveBytesPerRow(canvas->bytesPerRow());
        }
        SaveGroupSize(canvas->groupSize());
        SaveHexZoom(canvas->zoomLevel());
    }
}
