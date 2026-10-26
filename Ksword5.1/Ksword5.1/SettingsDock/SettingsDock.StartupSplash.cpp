#include "SettingsDock.h"
#include "SettingsDock.Layout.h"
#include "../UI/StartupSplashPreview.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/ThemeAccentIcon.h"
#include "../theme.h"

#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QToolButton>

// 设置→启动→启动画面：复用SecondaryPageLayout的表单、分区和父页独立滚动。
void SettingsDock::initializeStartupSplashSection(QVBoxLayout* layout)
{
    auto& language = ks::i18n::LanguageManager::instance(); // 显示文本与枚举值分开，热换语言不改变选项身份。
    auto* section = new QGroupBox(m_startupTab);
    language.bindText(section, QStringLiteral("settings.startup.splash.group"), QStringLiteral("启动画面"));
    auto* content = new QVBoxLayout(section);
    auto* form = ks::settings::ui::Form(content, 220); // 左侧标签宽度与同页启动行为一致。

    m_startupSplashLayoutCombo = new QComboBox(section);
    m_startupSplashLayoutCombo->addItem(QStringLiteral("左右布局"), static_cast<int>(SplashLayoutStyle::LeftRight));
    m_startupSplashLayoutCombo->addItem(QStringLiteral("上下布局"), static_cast<int>(SplashLayoutStyle::Vertical));
    m_startupSplashLayoutCombo->addItem(QStringLiteral("上下布局简化版"), static_cast<int>(SplashLayoutStyle::VerticalCompact));
    language.bindComboBoxItem(m_startupSplashLayoutCombo, 0,
        QStringLiteral("settings.startup.splash.layout.left_right"), QStringLiteral("左右布局"));
    language.bindComboBoxItem(m_startupSplashLayoutCombo, 1,
        QStringLiteral("settings.startup.splash.layout.vertical"), QStringLiteral("上下布局"));
    language.bindComboBoxItem(m_startupSplashLayoutCombo, 2,
        QStringLiteral("settings.startup.splash.layout.compact"), QStringLiteral("上下布局简化版"));
    language.bindToolTip(m_startupSplashLayoutCombo, QStringLiteral("settings.startup.splash.layout.tooltip"),
        QStringLiteral("左右布局显示完整说明，上下布局显示构建时间和作者，简化版只保留名称、版本和状态。"));
    form->addRow(ks::settings::ui::Label(section,
        QStringLiteral("settings.startup.splash.layout.label"), QStringLiteral("布局")), m_startupSplashLayoutCombo);

    m_startupSplashBackgroundCombo = new QComboBox(section);
    m_startupSplashBackgroundCombo->addItem(QStringLiteral("代码背景"), static_cast<int>(SplashBackgroundStyle::Code));
    m_startupSplashBackgroundCombo->addItem(QStringLiteral("蓝色背景"), static_cast<int>(SplashBackgroundStyle::BlueParticles));
    language.bindComboBoxItem(m_startupSplashBackgroundCombo, 0,
        QStringLiteral("settings.startup.splash.background.code"), QStringLiteral("代码背景"));
    language.bindComboBoxItem(m_startupSplashBackgroundCombo, 1,
        QStringLiteral("settings.startup.splash.background.blue"), QStringLiteral("蓝色背景"));
    language.bindToolTip(m_startupSplashBackgroundCombo, QStringLiteral("settings.startup.splash.background.tooltip"),
        QStringLiteral("底图完整等比显示，不裁切、不加遮罩。"));
    form->addRow(ks::settings::ui::Label(section,
        QStringLiteral("settings.startup.splash.background.label"), QStringLiteral("底图")), m_startupSplashBackgroundCombo);

    auto* hint = new QLabel(section); // 明确区分待应用预览和下一次实际启动。
    hint->setWordWrap(true);
    language.bindText(hint, QStringLiteral("settings.startup.splash.hint"),
        QStringLiteral("预览随选择更新；点击“应用”后，下次启动生效。"));
    content->addWidget(hint);

    // 预览占自然高度，保留原生方角和原始比例；整个启动设置页共用外层纵向滚动。
    content->addSpacing(8);
    auto* previewToolbar = new QHBoxLayout();
    previewToolbar->addWidget(ks::settings::ui::Label(section,
        QStringLiteral("settings.startup.splash.preview.title"), QStringLiteral("预览")), 1);
    auto* refreshPreview = new QToolButton(section); // 图片补回后的显式重试入口。
    refreshPreview->setObjectName(QStringLiteral("startup_splash_preview_refresh"));
    refreshPreview->setToolButtonStyle(Qt::ToolButtonIconOnly);
    KswordTheme::ApplyStandardIconButtonMetrics(refreshPreview);
    ks::ui::ApplyFlatButtonTheme(refreshPreview, ks::ui::FlatButtonTone::Neutral, ks::ui::FlatButtonAppearance::Solid);
    refreshPreview->setIcon(ks::ui::MakeThemeButtonAccentIcon(
        QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QColor(), refreshPreview));
    language.bindText(refreshPreview, QStringLiteral("settings.startup.splash.preview.refresh"), QStringLiteral("刷新预览"));
    language.bindToolTip(refreshPreview, QStringLiteral("settings.startup.splash.preview.refresh"), QStringLiteral("刷新预览"));
    previewToolbar->addWidget(refreshPreview);
    ks::ui::NormalizeToolbarRow(previewToolbar);
    content->addLayout(previewToolbar);
    m_startupSplashPreview = new ks::ui::StartupSplashPreview(section);
    content->addWidget(m_startupSplashPreview, 0, Qt::AlignHCenter);
    connect(refreshPreview, &QToolButton::clicked, m_startupSplashPreview, &ks::ui::StartupSplashPreview::refresh);

    ks::settings::ui::Section(section);
    layout->addWidget(section);

    // 只响应用户选择，回填配置或切换语言不会制造待保存状态。
    connect(m_startupSplashLayoutCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        markPendingChanges(QStringLiteral("startup_splash_layout_changed"));
    });
    connect(m_startupSplashBackgroundCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        markPendingChanges(QStringLiteral("startup_splash_background_changed"));
    });
    // 程序回填/恢复默认同样更新预览，但不制造待保存标记。
    connect(m_startupSplashLayoutCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &SettingsDock::updateStartupSplashPreview);
    connect(m_startupSplashBackgroundCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &SettingsDock::updateStartupSplashPreview);
    updateStartupSplashPreview();
}

void SettingsDock::updateStartupSplashPreview()
{
    if (!m_startupSplashPreview || !m_startupSplashLayoutCombo || !m_startupSplashBackgroundCombo) return;
    m_startupSplashPreview->setPresentation(
        normalizeSplashLayout(m_startupSplashLayoutCombo->currentData().toInt()),
        normalizeSplashBackground(m_startupSplashBackgroundCombo->currentData().toInt()));
}
