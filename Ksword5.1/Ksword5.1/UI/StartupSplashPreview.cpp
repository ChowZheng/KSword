#include "StartupSplashPreview.h"
#include "../Framework/StartupSplashLayout.h"
#include "../Framework/ReleaseMetadata.h"
#include "../Internationalization/LanguageManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFutureWatcher>
#include <QImage>
#include <QPainter>
#include <QShowEvent>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

namespace
{
    struct PreviewFrame
    {
        quint64 revision = 0; // 只接受仍与当前待应用选项一致的结果。
        QImage image; // worker独占写入，完成后以隐式共享传给GUI线程。
    };

    // 每个后台任务持有自己的GDI+会话，图片和字体均在会话结束前释放。
    class GdiSession final
    {
    public:
        GdiSession()
        {
            Gdiplus::GdiplusStartupInput input;
            m_ready = Gdiplus::GdiplusStartup(&m_token, &input, nullptr) == Gdiplus::Ok;
        }
        ~GdiSession()
        {
            if (m_ready) Gdiplus::GdiplusShutdown(m_token);
        }
        bool ready() const
        {
            return m_ready;
        }
    private:
        ULONG_PTR m_token = 0; // 只由本任务关闭的GDI+令牌。
        bool m_ready = false; // 初始化失败时向预览返回明确空结果。
    };

    // worker只接收值快照，不访问LanguageManager、QWidget或运行中的启动窗口。
    PreviewFrame renderPreview(quint64 revision, const QString& directory,
        SplashBackgroundStyle background, const ks::splash::SplashText& text,
        const std::wstring& status, qreal deviceRatio)
    {
        PreviewFrame result{revision, {}};
        GdiSession session;
        if (!session.ready()) return result;
        const QString artPath = QDir(directory).filePath(
            QString::fromWCharArray(ks::splash::backgroundFileName(background)));
        const QString logoPath = QDir(directory).filePath(QStringLiteral("KswordLogo.png"));
        std::unique_ptr<Gdiplus::Image> art(Gdiplus::Image::FromFile(artPath.toStdWString().c_str(), FALSE));
        if (!art || art->GetLastStatus() != Gdiplus::Ok || art->GetWidth() == 0 || art->GetHeight() == 0)
            return result;
        std::unique_ptr<Gdiplus::Image> logo(Gdiplus::Image::FromFile(logoPath.toStdWString().c_str(), FALSE));
        if (logo && logo->GetLastStatus() != Gdiplus::Ok) logo.reset(); // 与原生窗口共用Logo缺失回退。

        // 与实际窗口共用逻辑画布；物理缓存跟随DPI，图片比例来自当前原图。
        const float width = static_cast<float>(ks::splash::CanvasWidth);
        const float height = width * static_cast<float>(art->GetHeight()) / static_cast<float>(art->GetWidth());
        const int pixelWidth = qRound(width * qBound(1.0, deviceRatio, 4.0));
        const qreal scale = static_cast<qreal>(pixelWidth) / width;
        const int pixelHeight = qRound(height * scale);
        QImage image(pixelWidth, pixelHeight, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) return result;
        image.fill(Qt::transparent);
        {
            Gdiplus::Bitmap bitmap(pixelWidth, pixelHeight, static_cast<INT>(image.bytesPerLine()),
                PixelFormat32bppPARGB, image.bits());
            Gdiplus::Graphics graphics(&bitmap);
            if (bitmap.GetLastStatus() != Gdiplus::Ok || graphics.GetLastStatus() != Gdiplus::Ok)
                return result;
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.ScaleTransform(static_cast<float>(scale), static_cast<float>(scale));
            ks::splash::drawBackground(graphics, width, height, *art);
            const auto statusArea = ks::splash::drawStatic(graphics, width, height, logo.get(), text);
            ks::splash::drawTrack(graphics, width, height);
            ks::splash::drawStatus(graphics, statusArea, status);
            ks::splash::drawDeterminateProgress(graphics, width, height, 72); // 静态示意，不启动动画定时器。
            graphics.Flush(Gdiplus::FlushIntentionSync);
        }
        result.image = std::move(image);
        return result;
    }
}

namespace ks::ui
{
    class StartupSplashPreview::Impl final
    {
    public:
        explicit Impl(StartupSplashPreview& owner) : m_owner(owner), m_watcher(&owner)
        {
            QObject::connect(&m_watcher, &QFutureWatcher<PreviewFrame>::finished, &owner, [this]()
            {
                m_running = false; // 直到完成信号处理后，才允许给watcher绑定下一任务。
                const PreviewFrame result = m_watcher.result(); // 过期结果不能盖回用户刚选择的底图。
                if (result.revision == m_revision)
                {
                    m_image = result.image;
                    m_renderedRevision = result.revision;
                    m_failed = m_image.isNull();
                    m_owner.updateGeometry();
                    m_owner.update();
                }
                schedule(); // 多次快速选择只补绘最后一个待应用状态。
            });
        }

        void invalidate()
        {
            ++m_revision;
            m_image = {};
            m_failed = false;
            m_owner.update();
            schedule();
        }

        // 隐藏页不解码，同一轮设置回填/连续选择合并到一个零延迟请求。
        void schedule()
        {
            if (!m_owner.isVisible() || m_scheduled || m_running || m_revision == m_renderedRevision)
                return;
            m_scheduled = true;
            QTimer::singleShot(0, &m_owner, [this]()
            {
                m_scheduled = false;
                if (m_owner.isVisible() && !m_running && m_revision != m_renderedRevision)
                    launch();
            });
        }

        void launch()
        {
            auto& language = ks::i18n::LanguageManager::instance(); // 当前GUI线程捕获与真实启动相同的元数据。
            ks::splash::SplashText text;
            text.layout = m_layout;
            text.title = L"KSword";
            text.version = ks::app::releaseVersionText().toStdWString();
            text.buildTime = language.text(QStringLiteral("main.startup.brand.build_compact"),
                QStringLiteral("构建于 %1")).arg(ks::app::releaseBuildTimeText()).toStdWString();
            text.authors = ks::app::releaseAuthorsText().toStdWString();
            text.copyright = language.text(QStringLiteral("main.startup.legal.copyright_compact"),
                QStringLiteral("© 2023–2026 KSword contributors")).toStdWString();
            text.license = language.text(QStringLiteral("main.startup.legal.license"),
                QStringLiteral("Licensed under the KSword Community Source License 1.6 (source-available). See LICENSE for the full terms.")).toStdWString();
            text.thirdParty = language.text(QStringLiteral("main.startup.legal.third_party"),
                QStringLiteral("Third-party components are listed in the third_party/ directory.")).toStdWString();
            const auto status = language.text(QStringLiteral("main.startup.progress.qt_runtime"),
                QStringLiteral("正在准备运行环境...")).toStdWString();
            const auto directory = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Style"));
            const auto revision = m_revision;
            const auto background = m_background;
            const auto ratio = m_owner.devicePixelRatioF();
            m_running = true;
            m_watcher.setFuture(QtConcurrent::run([revision, directory, background, text, status, ratio]()
            {
                return renderPreview(revision, directory, background, text, status, ratio);
            }));
        }

        StartupSplashPreview& m_owner; // Qt负责宿主生命周期，后台任务从不捕获该引用。
        QFutureWatcher<PreviewFrame> m_watcher; // 每个预览最多一个在途任务；析构不等待任务。
        SplashLayoutStyle m_layout = SplashLayoutStyle::VerticalCompact; // 待应用排版。
        SplashBackgroundStyle m_background = SplashBackgroundStyle::Code; // 待应用底图。
        quint64 m_revision = 1; // 包含布局、底图、语言与DPI的请求身份。
        quint64 m_renderedRevision = 0; // 当前缓存或错误状态对应的请求。
        QImage m_image; // GUI线程只读绘制的最近一次有效缓存。
        bool m_scheduled = false; // 合并同一事件轮次的重复请求。
        bool m_running = false; // 包含已完成但尚未处理完成信号的任务，防止覆盖watcher。
        bool m_failed = false; // 区分尚未生成和图片不可用。
    };

    StartupSplashPreview::StartupSplashPreview(QWidget* parent)
        : QWidget(parent), m_impl(std::make_unique<Impl>(*this))
    {
        setObjectName(QStringLiteral("startup_splash_preview"));
        setMaximumWidth(ks::splash::CanvasWidth);
        QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }
    StartupSplashPreview::~StartupSplashPreview() = default;

    void StartupSplashPreview::setPresentation(SplashLayoutStyle layout, SplashBackgroundStyle background)
    {
        layout = normalizeSplashLayout(static_cast<int>(layout));
        background = normalizeSplashBackground(static_cast<int>(background));
        if (m_impl->m_layout == layout && m_impl->m_background == background) return;
        m_impl->m_layout = layout;
        m_impl->m_background = background;
        m_impl->invalidate();
    }
    void StartupSplashPreview::refresh()
    {
        m_impl->invalidate();
    }
    QSize StartupSplashPreview::sizeHint() const
    {
        return QSize(640, heightForWidth(640));
    }
    QSize StartupSplashPreview::minimumSizeHint() const
    {
        return QSize(0, 0);
    }
    int StartupSplashPreview::heightForWidth(int width) const
    {
        const QSize source = m_impl->m_image.isNull() ? QSize(832, 468) : m_impl->m_image.size();
        return qRound(static_cast<qreal>(qMax(0, width)) * source.height() / source.width());
    }

    bool StartupSplashPreview::event(QEvent* event)
    {
        const bool handled = QWidget::event(event); // DPR事件不依赖changeEvent的事件类型分派。
        if (event->type() == QEvent::DevicePixelRatioChange && m_impl)
            m_impl->invalidate();
        return handled;
    }

    void StartupSplashPreview::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        if (!m_impl->m_image.isNull())
        {
            const QSize fitted = m_impl->m_image.size().scaled(size(), Qt::KeepAspectRatio);
            const QRect area(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(area, m_impl->m_image); // 方角原图，等比缩放，不覆盖任何颜色遮罩。
            return;
        }
        painter.setPen(palette().color(QPalette::Text)); // 错误与加载文字跟随设置页当前深浅主题。
        const QString label = m_impl->m_failed
            ? ks::i18n::text(QStringLiteral("settings.startup.splash.preview.failed"),
                QStringLiteral("无法生成预览。请检查 Style 目录中的图片，然后刷新预览。"))
            : ks::i18n::text(QStringLiteral("settings.startup.splash.preview.loading"),
                QStringLiteral("正在生成预览..."));
        painter.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, label);
    }

    void StartupSplashPreview::showEvent(QShowEvent* event)
    {
        QWidget::showEvent(event);
        m_impl->schedule();
    }
    void StartupSplashPreview::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            m_impl->invalidate();
    }
}
