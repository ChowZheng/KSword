#pragma once

#include "../Framework/StartupSplash.h"
#include <QWidget>
#include <memory>

namespace ks::ui
{
    // 设置页的启动图预览；复用原生绘制函数，只显示待应用选项，不创建启动窗口。
    class StartupSplashPreview final : public QWidget
    {
    public:
        explicit StartupSplashPreview(QWidget* parent = nullptr);
        ~StartupSplashPreview() override;
        // 更新布局与底图；重复值不重绘，实际保存仍由设置页的应用/取消流程负责。
        void setPresentation(SplashLayoutStyle layout, SplashBackgroundStyle background);
        void refresh(); // 图片补回后允许显式重新读取；不修改配置。
        QSize sizeHint() const override;
        QSize minimumSizeHint() const override;
        int heightForWidth(int width) const override;

    protected:
        // 仅显示缓存；选择、语言或DPI变化才异步生成一次图片，缩放不重做解码。
        bool event(QEvent* event) override;
        void paintEvent(QPaintEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;
        void showEvent(QShowEvent* event) override;
        void changeEvent(QEvent* event) override;

    private:
        class Impl;
        void syncPreviewHeight(); // 只同步本控件的自然高度，避免滚动页将画布压扁或居中到视口外。
        std::unique_ptr<Impl> m_impl; // 拥有本预览的异步状态和唯一图像缓存。
    };
}
