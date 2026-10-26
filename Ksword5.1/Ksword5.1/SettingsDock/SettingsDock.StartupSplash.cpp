#include "SettingsDock.h"
#include "SettingsDock.Layout.h"

#include <QComboBox>
#include <QGroupBox>
#include <QLabel>

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

    auto* hint = new QLabel(section); // 重启语义直接显示，避免把保存偏好误认为当前进度窗预览。
    hint->setWordWrap(true);
    language.bindText(hint, QStringLiteral("settings.startup.splash.hint"),
        QStringLiteral("布局和底图可分别选择，下次启动生效。"));
    content->addWidget(hint);
    ks::settings::ui::Section(section);
    layout->addWidget(section);

    // 只响应用户选择，回填配置或切换语言不会制造待保存状态。
    connect(m_startupSplashLayoutCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        markPendingChanges(QStringLiteral("startup_splash_layout_changed"));
    });
    connect(m_startupSplashBackgroundCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        markPendingChanges(QStringLiteral("startup_splash_background_changed"));
    });
}
