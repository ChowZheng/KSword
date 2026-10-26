#include "StartupSplash.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Gdiplus.lib")

namespace
{
    constexpr wchar_t kWindowClass[] = L"KswordFrameworkSplashWindow";
    // 横向品牌摘要采用10:3逻辑画布；外沿方角，底轨与窗口同宽。
    constexpr int kBaseWidth = 900;
    constexpr int kBaseHeight = 270;
    constexpr float kContentX = 44.0f; // 品牌与元信息共用左对齐线。
    constexpr float kTrackX = 0.0f;
    constexpr float kTrackY = 268.0f;
    constexpr float kTrackWidth = static_cast<float>(kBaseWidth);
    constexpr float kTrackHeight = 2.0f;
    constexpr float kPercentX = 810.0f; // 数字独立保留宽度，长状态不能挤入。
    constexpr float kStatusY = 166.0f;
    constexpr float kPercentWidth = 46.0f;
    constexpr float kStatusHeight = 24.0f;
    constexpr UINT kShowMessage = WM_APP + 1;
    constexpr UINT kHideMessage = WM_APP + 2;
    constexpr UINT kRedrawMessage = WM_APP + 3;
    constexpr UINT kResetAnimationMessage = WM_APP + 4;

    std::wstring utf8ToWide(const std::string& text)
    {
        if (text.empty()) return {};
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.c_str(), -1, nullptr, 0);
        if (count <= 0) return {};
        std::wstring result(static_cast<size_t>(count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1,
            result.data(), count) == 0) return {};
        result.pop_back();
        return result;
    }

    std::wstring splashAssetPath(const wchar_t* name)
    {
        std::vector<wchar_t> buffer(MAX_PATH);
        for (;;)
        {
            const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(),
                static_cast<DWORD>(buffer.size()));
            if (copied == 0) return {};
            if (copied < buffer.size() - 1)
            {
                std::wstring path(buffer.data(), copied);
                const size_t slash = path.find_last_of(L"\\/");
                if (slash == std::wstring::npos) return {};
                return path.substr(0, slash + 1) + L"Style\\" + name;
            }
            if (buffer.size() >= 32768) return {};
            buffer.resize(buffer.size() * 2);
        }
    }

    void enableSplashThreadDpiAwareness()
    {
        // The splash precedes QApplication: set physical coordinates only on
        // its UI thread, leaving Qt's process/thread DPI setup to Qt.
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        using SetThreadDpiAwarenessContextFn = HANDLE(WINAPI*)(HANDLE);
        const auto setDpiContext = reinterpret_cast<SetThreadDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
        if (setDpiContext != nullptr)
        {
            if (setDpiContext(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4))) == nullptr)
                setDpiContext(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-3)));
        }
    }

    UINT windowDpi(HWND window)
    {
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (window != nullptr && getDpi != nullptr)
        {
            const UINT dpi = getDpi(window);
            if (dpi != 0) return dpi;
        }
        HDC screen = GetDC(nullptr);
        const UINT dpi = screen != nullptr ? static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX)) : 96;
        if (screen != nullptr) ReleaseDC(nullptr, screen);
        return std::max(96U, dpi);
    }

    void roundedRectangle(Gdiplus::GraphicsPath& outline, float x, float y,
        float width, float height, float radius)
    {
        const float diameter = 2.0f * std::min(radius, std::min(width, height) * 0.5f);
        outline.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
        outline.AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
        outline.AddArc(x + width - diameter, y + height - diameter,
            diameter, diameter, 0.0f, 90.0f);
        outline.AddArc(x, y + height - diameter, diameter, diameter, 90.0f, 90.0f);
        outline.CloseFigure();
    }

    void fillRoundedRectangle(Gdiplus::Graphics& graphics, const Gdiplus::Brush& brush,
        float x, float y, float width, float height, float radius)
    {
        if (width <= 0.0f || height <= 0.0f) return;
        Gdiplus::GraphicsPath outline;
        roundedRectangle(outline, x, y, width, height, radius);
        graphics.FillPath(&brush, &outline);
    }

    // 统一原生文本度量与省略行为；多行元信息只在静态缓存重建时排版。
    void drawSplashText(Gdiplus::Graphics& graphics, const std::wstring& text,
        const Gdiplus::Font& font, const Gdiplus::RectF& bounds,
        const Gdiplus::Brush& brush, Gdiplus::StringAlignment alignment,
        bool multiline = false)
    {
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic()); // 使用同一字体测量模型。
        format.SetAlignment(alignment);
        format.SetLineAlignment(multiline ? Gdiplus::StringAlignmentNear : Gdiplus::StringAlignmentCenter);
        if (!multiline)
            format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        graphics.DrawString(text.c_str(), -1, &font, bounds, &format, &brush);
    }

    // 版本直接取发行元数据；数字与阶段后缀分开显示，不解析本地化产品标题。
    struct SplashVersion final
    {
        std::wstring number; // 发行数字部分。
        std::wstring stage; // PRE/RC等原版本后缀，空值不显示徽标。
    };

    SplashVersion splitSplashVersion(const std::wstring& version)
    {
        const size_t suffix = version.find_first_not_of(L"0123456789."); // 只处理标准数字版本前缀。
        if (suffix == 0 || suffix == std::wstring::npos)
            return {version, {}};
        SplashVersion result{version.substr(0, suffix), version.substr(suffix)};
        const size_t first = result.stage.find_first_not_of(L" -+"); // 分隔符不是阶段名的一部分。
        result.stage = first == std::wstring::npos ? std::wstring() : result.stage.substr(first);
        for (wchar_t& letter : result.stage)
            if (letter >= L'a' && letter <= L'z') letter = static_cast<wchar_t>(letter - L'a' + L'A');
        return result;
    }

    // 只做展示换行：编译时间保留日期/时间两行，两个作者按原分隔符折行。
    std::wstring splashBuildLines(std::wstring text)
    {
        const size_t separator = text.find_last_of(L' '); // 末尾空格后应是hh:mm:ss，而不是本地化标签。
        if (separator != std::wstring::npos && text.find(L':', separator) != std::wstring::npos)
            text[separator] = L'\n';
        return text;
    }

    std::wstring splashAuthorLines(std::wstring text)
    {
        const size_t separator = text.find(L" · "); // 与ReleaseMetadata共用的作者列表分隔符。
        if (separator != std::wstring::npos) text.replace(separator, 3, L"\n");
        return text;
    }

    std::wstring splashWebsiteLabel(std::wstring text)
    {
        const size_t scheme = text.find(L"://"); // 启动摘要仅显示域名/路径，完整URL仍由调用方保存。
        if (scheme != std::wstring::npos) text.erase(0, scheme + 3);
        while (!text.empty() && text.back() == L'/') text.pop_back();
        return text;
    }

    // Own selected DIBs for the splash UI thread; pixels stay premultiplied.
    struct DibSurface final
    {
        ~DibSurface() { reset(); }
        DibSurface() = default;
        DibSurface(const DibSurface&) = delete;
        DibSurface& operator=(const DibSurface&) = delete;

        void reset()
        {
            if (dc != nullptr && oldBitmap != nullptr) SelectObject(dc, oldBitmap);
            if (bitmap != nullptr) DeleteObject(bitmap);
            if (dc != nullptr) DeleteDC(dc);
            dc = nullptr; bitmap = nullptr; oldBitmap = nullptr; pixels = nullptr;
            width = height = 0;
        }
        bool ensure(int requestedWidth, int requestedHeight)
        {
            if (dc != nullptr && width == requestedWidth && height == requestedHeight) return true;
            HDC nextDc = CreateCompatibleDC(nullptr);
            if (nextDc == nullptr) return false;
            BITMAPINFO info = {};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = requestedWidth;
            info.bmiHeader.biHeight = -requestedHeight;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* nextPixels = nullptr;
            HBITMAP nextBitmap = CreateDIBSection(nextDc, &info, DIB_RGB_COLORS, &nextPixels, nullptr, 0);
            if (nextBitmap == nullptr) { DeleteDC(nextDc); return false; }
            HGDIOBJ nextOldBitmap = SelectObject(nextDc, nextBitmap);
            if (nextOldBitmap == nullptr || nextOldBitmap == HGDI_ERROR)
            { DeleteObject(nextBitmap); DeleteDC(nextDc); return false; }
            reset();
            dc = nextDc; bitmap = nextBitmap; oldBitmap = nextOldBitmap; pixels = nextPixels;
            width = requestedWidth; height = requestedHeight;
            return true;
        }
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ oldBitmap = nullptr;
        void* pixels = nullptr;
        int width = 0, height = 0;
    };

    // 从静态DIB恢复一个逻辑矩形的全部预乘像素，数字更新与细轨动画都不会留残影。
    void restoreSplashRectangle(DibSurface& frame, const DibSurface& background,
        const Gdiplus::RectF& bounds, float scale)
    {
        const int left = std::clamp(static_cast<int>(std::floor(bounds.X * scale)) - 2, 0, frame.width);
        const int right = std::clamp(static_cast<int>(std::ceil((bounds.X + bounds.Width) * scale)) + 2, left, frame.width);
        const int top = std::clamp(static_cast<int>(std::floor(bounds.Y * scale)) - 2, 0, frame.height);
        const int bottom = std::clamp(static_cast<int>(std::ceil((bounds.Y + bounds.Height) * scale)) + 2, top, frame.height);
        const size_t stride = static_cast<size_t>(frame.width) * 4; // 两个DIB已由ensure保证相同尺寸。
        for (int row = top; row < bottom; ++row)
        {
            const size_t offset = static_cast<size_t>(row) * stride + static_cast<size_t>(left) * 4;
            std::memcpy(static_cast<BYTE*>(frame.pixels) + offset,
                static_cast<const BYTE*>(background.pixels) + offset,
                static_cast<size_t>(right - left) * 4);
        }
    }

    double cubic(double t, double p1, double p2)
    {
        const double inverse = 1.0 - t;
        return 3.0 * inverse * inverse * t * p1 +
            3.0 * inverse * t * t * p2 + t * t * t;
    }

    double winUiTravel(double elapsedFraction)
    {
        // WinUI's progress-bar spline is cubic-bezier(0.4, 0, 0.6, 1).
        double low = 0.0;
        double high = 1.0;
        for (int index = 0; index < 16; ++index)
        {
            const double middle = (low + high) * 0.5;
            if (cubic(middle, 0.4, 0.6) < elapsedFraction) low = middle;
            else high = middle;
        }
        return cubic((low + high) * 0.5, 0.0, 1.0);
    }
}

class kStartupSplash::Impl final
{
public:
    ~Impl()
    {
        const HWND window = m_window.load();
        if (window != nullptr) PostMessageW(window, WM_CLOSE, 0, 0);
        if (m_thread.joinable()) m_thread.join();
    }

    void setBackgroundImagePath(const std::wstring& path)
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_thread.joinable()) m_imagePath = path;
    }

    void setBrandingText(const std::wstring& title, const std::wstring& buildTime,
        const std::wstring& authors)
    {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_title == title && m_buildTime == buildTime && m_authors == authors) return;
            m_title = title;
            m_buildTime = buildTime;
            m_authors = authors;
            ++m_stateRevision;
            ++m_staticRevision;
        }
        requestRedraw();
    }

    // 调用方提供原始发行版本；仅真实变化时使静态信息行失效。
    void setVersionText(const std::wstring& version)
    {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_version == version) return;
            m_version = version;
            ++m_stateRevision;
            ++m_staticRevision;
        }
        requestRedraw();
    }

    void setLegalText(const std::wstring& copyright, const std::wstring& license,
        const std::wstring& thirdParty, const std::wstring& website,
        const std::wstring& repository)
    {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_copyright == copyright && m_license == license &&
                m_thirdParty == thirdParty && m_website == website &&
                m_repository == repository) return;
            m_copyright = copyright;
            m_license = license;
            m_thirdParty = thirdParty;
            m_website = website;
            m_repository = repository;
            ++m_stateRevision;
            ++m_staticRevision;
        }
        requestRedraw();
    }

    void setProgressMode(SplashProgressMode mode)
    {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_mode == mode) return;
            m_mode = mode;
            ++m_stateRevision;
        }
        const HWND window = m_window.load();
        if (window != nullptr) PostMessageW(window, kResetAnimationMessage, 0, 0);
    }

    void setAccentColorRgb(unsigned int rgb)
    {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const unsigned int accent = rgb & 0x00FFFFFFU;
            if (m_accentRgb == accent) return;
            m_accentRgb = accent;
            ++m_stateRevision;
            ++m_staticRevision;
        }
        requestRedraw();
    }

    bool show(const std::string& initialStatusText)
    {
        const std::wstring status = utf8ToWide(initialStatusText);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const std::wstring nextStatus = status.empty() ? L"Ksword" : status;
            if (m_status != nextStatus) ++m_staticRevision;
            m_status = nextStatus;
            m_percent = 0;
            ++m_stateRevision;
        }
        if (!m_thread.joinable())
        {
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_threadReady = false;
                m_threadSucceeded = false;
            }
            // Rescue launches select the caller's desktop before splash startup.
            m_desktop = GetThreadDesktop(GetCurrentThreadId());
            m_thread = std::thread(&Impl::run, this);
            std::unique_lock<std::mutex> lock(m_stateMutex);
            m_created.wait(lock, [this] { return m_threadReady; });
            if (!m_threadSucceeded)
            {
                lock.unlock();
                m_thread.join();
                return false;
            }
        }
        const HWND window = m_window.load();
        if (window == nullptr) return false;
        SendMessageW(window, kShowMessage, 0, 0);
        return IsWindowVisible(window) != FALSE;
    }

    void hide()
    {
        const HWND window = m_window.load();
        if (window != nullptr) SendMessageW(window, kHideMessage, 0, 0);
    }

    void progress(const std::string& operationName, int percent)
    {
        if (m_window.load() == nullptr) return;
        const std::wstring status = utf8ToWide(operationName);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const int clamped = std::clamp(percent, 0, 100);
            if ((status.empty() || m_status == status) && m_percent == clamped) return;
            if (!status.empty() && m_status != status)
            {
                m_status = status;
                ++m_staticRevision;
            }
            m_percent = clamped;
            ++m_stateRevision;
        }
        requestRedraw();
    }

    bool ready() const { return m_window.load() != nullptr; }

private:
    void signalCreated(bool succeeded)
    {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_threadSucceeded = succeeded;
            m_threadReady = true;
        }
        m_created.notify_one();
    }

    void run()
    {
        if (m_desktop == nullptr || !SetThreadDesktop(m_desktop))
        {
            signalCreated(false);
            return;
        }
        enableSplashThreadDpiAwareness();
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &input, nullptr) != Gdiplus::Ok)
        {
            signalCreated(false);
            return;
        }
        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            path = m_imagePath.empty() ? splashAssetPath(L"StartupLandscape.png") : m_imagePath;
        }
        m_image.reset(Gdiplus::Image::FromFile(path.c_str(), FALSE));
        if (!m_image || m_image->GetLastStatus() != Gdiplus::Ok ||
            m_image->GetWidth() == 0 || m_image->GetHeight() == 0)
        {
            m_image.reset();
            Gdiplus::GdiplusShutdown(m_gdiplusToken);
            m_gdiplusToken = 0;
            signalCreated(false);
            return;
        }
        m_imageWidth = static_cast<int>(m_image->GetWidth());
        m_imageHeight = static_cast<int>(m_image->GetHeight());
        const std::wstring logoPath = splashAssetPath(L"KswordLogo.png");
        m_logo.reset(Gdiplus::Image::FromFile(logoPath.c_str(), FALSE));
        if (m_logo && m_logo->GetLastStatus() != Gdiplus::Ok) m_logo.reset();

        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Impl::windowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kWindowClass;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        const bool registered = RegisterClassExW(&windowClass) != 0 ||
            GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        if (registered)
        {
            layout();
            const HWND window = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                kWindowClass, L"Ksword", WS_POPUP, m_x, m_y, m_width, m_height,
                nullptr, nullptr, GetModuleHandleW(nullptr), this);
            m_window.store(window);
        }
        const HWND window = m_window.load();
        signalCreated(window != nullptr);
        if (window != nullptr)
        {
            MSG message = {};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            m_window.store(nullptr);
        }
        m_frame.reset();
        m_staticSurface.reset();
        m_image.reset();
        m_logo.reset();
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }

    void layout()
    {
        const HWND window = m_window.load();
        const HMONITOR monitor = window != nullptr
            ? MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY)
            : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info = {sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) return;
        const int oldX = m_x, oldY = m_y, oldWidth = m_width, oldHeight = m_height;
        const UINT oldDpi = m_dpi;
        m_dpi = windowDpi(window);
        const int workWidth = info.rcWork.right - info.rcWork.left;
        const int workHeight = info.rcWork.bottom - info.rcWork.top;
        m_scale = std::min({static_cast<float>(m_dpi) / 96.0f,
            static_cast<float>(workWidth) * 0.90f / kBaseWidth,
            static_cast<float>(workHeight) * 0.88f / kBaseHeight});
        m_width = std::max(1, static_cast<int>(std::lround(kBaseWidth * m_scale)));
        m_height = std::max(1, static_cast<int>(std::lround(kBaseHeight * m_scale)));
        m_x = info.rcWork.left + (workWidth - m_width) / 2;
        m_y = info.rcWork.top + (workHeight - m_height) / 2;
        if (oldX != m_x || oldY != m_y || oldWidth != m_width || oldHeight != m_height || oldDpi != m_dpi)
            ++m_geometryRevision;
        if (window != nullptr)
            SetWindowPos(window, HWND_TOPMOST, m_x, m_y, m_width, m_height,
                SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    void requestRedraw()
    {
        const HWND window = m_window.load();
        if (window != nullptr && !m_redrawQueued.exchange(true))
        {
            if (!PostMessageW(window, kRedrawMessage, 0, 0)) m_redrawQueued.store(false);
        }
    }

    void updateAnimationTimer(bool visible)
    {
        const HWND window = m_window.load();
        BOOL animations = TRUE;
        if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) animations = TRUE;
        m_animationsEnabled = animations != FALSE;
        UINT refreshRate = 60;
        MONITORINFOEXW info = {};
        info.cbSize = sizeof(info);
        const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
        if (GetMonitorInfoW(monitor, &info))
        {
            DEVMODEW mode = {};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmDisplayFrequency >= 24 && mode.dmDisplayFrequency <= 360)
                refreshRate = mode.dmDisplayFrequency;
        }
        // Win32 timers have a minimum interval; animation position uses elapsed
        // time, so a late/coalesced tick never slows the progress cycle.
        const UINT interval = std::max<UINT>(USER_TIMER_MINIMUM, (1000U + refreshRate / 2U) / refreshRate);
        SplashProgressMode mode;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            mode = m_mode;
        }
        if (window != nullptr)
        {
            if (visible && m_animationsEnabled && mode == SplashProgressMode::Indeterminate)
                SetTimer(window, 1, interval, nullptr);
            else
                KillTimer(window, 1);
        }
    }

    bool redraw(bool force = false)
    {
        const HWND window = m_window.load();
        return window != nullptr && paint(nullptr, force);
    }

    bool paint(HDC target, bool force = false)
    {
        std::wstring status;
        std::wstring title;
        std::wstring version; // 原始数字版本与阶段标签，不含本地化产品标题。
        std::wstring buildTime;
        std::wstring authors;
        std::wstring copyright;
        std::wstring website;
        SplashProgressMode mode;
        unsigned int accentRgb;
        int percent;
        std::uint64_t revision, staticRevision;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            status = m_status;
            title = m_title;
            version = m_version;
            buildTime = m_buildTime;
            authors = m_authors;
            copyright = m_copyright;
            website = m_website;
            mode = m_mode;
            accentRgb = m_accentRgb;
            percent = m_percent;
            revision = m_stateRevision;
            staticRevision = m_staticRevision;
        }
        // Layered windows retain their submitted pixels. Repeated state messages
        // and WM_PAINT do not require resubmitting an unchanged determinate frame.
        if (!force && target == nullptr && (mode == SplashProgressMode::Determinate || !m_animationsEnabled) &&
            revision == m_paintedRevision && m_geometryRevision == m_paintedGeometryRevision)
            return true;

        const bool resized = m_staticSurface.width != m_width || m_staticSurface.height != m_height ||
            m_frame.width != m_width || m_frame.height != m_height;
        if (!m_staticSurface.ensure(m_width, m_height) || !m_frame.ensure(m_width, m_height)) return false;
        const bool rebuildStatic = resized || staticRevision != m_cachedStaticRevision ||
            m_geometryRevision != m_cachedStaticGeometryRevision;
        if (rebuildStatic)
        {
            // UpdateLayeredWindow requires premultiplied RGB and per-pixel alpha.
            // Drawing into this bitmap preserves alpha for both PNG and overlays.
            Gdiplus::Bitmap surface(m_width, m_height, m_width * 4,
                PixelFormat32bppPARGB, static_cast<BYTE*>(m_staticSurface.pixels));
            Gdiplus::Graphics graphics(&surface);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
            graphics.ScaleTransform(m_scale, m_scale);

            // 原图等比裁成横幅并靠上取景，避免把16:9素材横向拉伸。
            const float aspect = static_cast<float>(kBaseWidth) / kBaseHeight; // 目标横幅宽高比。
            const float imageWidth = static_cast<float>(m_imageWidth); // 源图物理宽度只在采样入口显式转为浮点。
            const float sourceHeight = std::min(static_cast<float>(m_imageHeight), imageWidth / aspect);
            const float sourceWidth = sourceHeight * aspect;
            const float sourceX = (imageWidth - sourceWidth) * 0.5f;
            graphics.DrawImage(m_image.get(), Gdiplus::RectF(0.0f, 0.0f,
                static_cast<float>(kBaseWidth), static_cast<float>(kBaseHeight)),
                sourceX, 0.0f, sourceWidth, sourceHeight, Gdiplus::UnitPixel);
            // 渐变只负责保证下半部文字对比，不再画独立左侧卡片或外框。
            Gdiplus::LinearGradientBrush verticalShade(Gdiplus::PointF(0.0f, 0.0f),
                Gdiplus::PointF(0.0f, static_cast<float>(kBaseHeight)),
                Gdiplus::Color(35, 7, 13, 23), Gdiplus::Color(248, 7, 12, 20));
            graphics.FillRectangle(&verticalShade, 0.0f, 0.0f,
                static_cast<float>(kBaseWidth), static_cast<float>(kBaseHeight));
            Gdiplus::LinearGradientBrush leftShade(Gdiplus::PointF(0.0f, 0.0f),
                Gdiplus::PointF(static_cast<float>(kBaseWidth), 0.0f),
                Gdiplus::Color(180, 6, 11, 19), Gdiplus::Color(0, 6, 11, 19));
            graphics.FillRectangle(&leftShade, 0.0f, 0.0f,
                static_cast<float>(kBaseWidth), static_cast<float>(kBaseHeight));

            // 品牌下移到画布下半部，保留上方背景留白；图标与标题共用中线。
            if (m_logo)
                graphics.DrawImage(m_logo.get(), Gdiplus::RectF(kContentX, 136.0f, 52.0f, 52.0f));
            else
            {
                Gdiplus::SolidBrush fallback(Gdiplus::Color(255, 28, 70, 125)); // 图标缺失沿用既有回退能力。
                fillRoundedRectangle(graphics, fallback, kContentX, 136.0f, 52.0f, 52.0f, 10.0f);
            }
            Gdiplus::SolidBrush heading(Gdiplus::Color(255, 245, 248, 253)); // 品牌文字保持不透明。
            Gdiplus::Font headingFont(L"Segoe UI", 44.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            drawSplashText(graphics, title, headingFont,
                Gdiplus::RectF(112.0f, 128.0f, 420.0f, 68.0f), heading, Gdiplus::StringAlignmentNear);

            // 版本、阶段、编译日期/时间和两位作者使用一排紧凑列，避免堆叠法律正文。
            const SplashVersion release = splitSplashVersion(version); // 数字与PRE后缀有独立身份。
            Gdiplus::Font metadataFont(L"Consolas", 12.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush metadata(Gdiplus::Color(255, 174, 187, 205));
            Gdiplus::SolidBrush separator(Gdiplus::Color(255, 93, 108, 130));
            Gdiplus::StringFormat measureFormat(Gdiplus::StringFormat::GenericTypographic());
            Gdiplus::RectF measured; // 根据版本实际宽度推进后续列，不依赖当前版本位数。
            graphics.MeasureString(release.number.c_str(), -1, &metadataFont,
                Gdiplus::PointF(0.0f, 0.0f), &measureFormat, &measured);
            const float numberWidth = std::clamp(measured.Width + 2.0f, 56.0f, 100.0f);
            drawSplashText(graphics, release.number, metadataFont,
                Gdiplus::RectF(kContentX, 209.0f, numberWidth, 20.0f), metadata, Gdiplus::StringAlignmentNear);
            float nextX = kContentX + numberWidth + 10.0f; // 阶段为空时不保留空徽标。
            if (!release.stage.empty())
            {
                const Gdiplus::Color accent(255, static_cast<BYTE>((accentRgb >> 16) & 0xffU),
                    static_cast<BYTE>((accentRgb >> 8) & 0xffU), static_cast<BYTE>(accentRgb & 0xffU));
                Gdiplus::SolidBrush stageBrush(accent); // 阶段框与真实进度共用主题强调色。
                Gdiplus::Pen stagePen(accent, 0.8f);
                Gdiplus::GraphicsPath badge; // 小徽标可圆角，窗口和底轨仍为方角。
                roundedRectangle(badge, nextX, 209.0f, 38.0f, 19.0f, 3.0f);
                graphics.DrawPath(&stagePen, &badge);
                drawSplashText(graphics, release.stage, metadataFont,
                    Gdiplus::RectF(nextX + 2.0f, 209.0f, 34.0f, 19.0f), stageBrush, Gdiplus::StringAlignmentCenter);
                nextX += 50.0f;
            }
            drawSplashText(graphics, L"/", metadataFont, Gdiplus::RectF(nextX, 209.0f, 10.0f, 20.0f),
                separator, Gdiplus::StringAlignmentCenter);
            const float buildX = nextX + 18.0f; // 日期和时间占同一列的两行。
            Gdiplus::Font bodyFont(L"Microsoft YaHei UI", 12.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            drawSplashText(graphics, splashBuildLines(buildTime), bodyFont,
                Gdiplus::RectF(buildX, 208.0f, 150.0f, 38.0f), metadata, Gdiplus::StringAlignmentNear, true);
            drawSplashText(graphics, L"/", metadataFont, Gdiplus::RectF(buildX + 158.0f, 209.0f, 10.0f, 20.0f),
                separator, Gdiplus::StringAlignmentCenter);
            drawSplashText(graphics, splashAuthorLines(authors), metadataFont,
                Gdiplus::RectF(buildX + 178.0f, 208.0f, 162.0f, 38.0f), metadata, Gdiplus::StringAlignmentNear, true);

            // 状态与百分比分槽；精简版权和域名在同一右对齐线，不再显示段落与双链接。
            Gdiplus::Font statusFont(L"Microsoft YaHei UI", 14.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush statusBrush(Gdiplus::Color(255, 211, 220, 233));
            drawSplashText(graphics, status, statusFont,
                Gdiplus::RectF(574.0f, kStatusY, 224.0f, kStatusHeight), statusBrush, Gdiplus::StringAlignmentFar);
            Gdiplus::SolidBrush legalBrush(Gdiplus::Color(255, 153, 168, 188));
            drawSplashText(graphics, copyright, bodyFont,
                Gdiplus::RectF(568.0f, 205.0f, 288.0f, 19.0f), legalBrush, Gdiplus::StringAlignmentFar);
            drawSplashText(graphics, splashWebsiteLabel(website), bodyFont,
                Gdiplus::RectF(568.0f, 225.0f, 288.0f, 19.0f), legalBrush, Gdiplus::StringAlignmentFar);
            Gdiplus::SolidBrush track(Gdiplus::Color(105, 159, 177, 202)); // 全宽细底轨不描窗口外沿。
            graphics.FillRectangle(&track, kTrackX, kTrackY, kTrackWidth, kTrackHeight);
            graphics.Flush(Gdiplus::FlushIntentionSync);
            m_cachedStaticRevision = staticRevision;
            m_cachedStaticGeometryRevision = m_geometryRevision;
        }
        const size_t stride = static_cast<size_t>(m_width) * 4;
        if (rebuildStatic)
            std::memcpy(m_frame.pixels, m_staticSurface.pixels, stride * m_height);
        else
        {
            // 百分比和底轨各自恢复完整预乘像素；其余背景和文字复用原静态缓存。
            restoreSplashRectangle(m_frame, m_staticSurface,
                Gdiplus::RectF(kTrackX, kTrackY, kTrackWidth, kTrackHeight), m_scale);
            restoreSplashRectangle(m_frame, m_staticSurface,
                Gdiplus::RectF(kPercentX, kStatusY, kPercentWidth, kStatusHeight), m_scale);
        }
        {
            Gdiplus::Bitmap surface(m_width, m_height, m_width * 4,
                PixelFormat32bppPARGB, static_cast<BYTE*>(m_frame.pixels));
            Gdiplus::Graphics graphics(&surface);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.ScaleTransform(m_scale, m_scale);
            const int accentRed = static_cast<int>((accentRgb >> 16) & 0xFFU);
            const int accentGreen = static_cast<int>((accentRgb >> 8) & 0xFFU);
            const int accentBlue = static_cast<int>(accentRgb & 0xFFU);
            Gdiplus::SolidBrush indicator(Gdiplus::Color(240,
                static_cast<BYTE>(accentRed), static_cast<BYTE>(accentGreen),
                static_cast<BYTE>(accentBlue)));
            // 仅确定模式显示真实百分比；不定进度阶段不制造假的完成数值。
            if (mode == SplashProgressMode::Determinate)
            {
                const std::wstring percentage = std::to_wstring(percent) + L"%";
                Gdiplus::Font percentFont(L"Consolas", 12.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush percentBrush(Gdiplus::Color(255, 169, 184, 205));
                drawSplashText(graphics, percentage, percentFont,
                    Gdiplus::RectF(kPercentX, kStatusY, kPercentWidth, kStatusHeight),
                    percentBrush, Gdiplus::StringAlignmentFar);
            }
            graphics.SetClip(Gdiplus::RectF(kTrackX, kTrackY, kTrackWidth, kTrackHeight));
            if (mode == SplashProgressMode::Indeterminate)
            {
                constexpr ULONGLONG kCycleMs = 1150;
                constexpr ULONGLONG kTravelMs = 1000;
                const ULONGLONG phase = m_animationsEnabled
                    ? (GetTickCount64() - m_animationStart) % kCycleMs : kTravelMs / 2;
                if (phase < kTravelMs)
                {
                    const float progress = static_cast<float>(winUiTravel(
                        static_cast<double>(phase) / kTravelMs));
                    const float segmentWidth = kTrackWidth * (0.10f +
                        0.16f * 4.0f * progress * (1.0f - progress)); // 滑块宽度跟随全宽底轨。
                    const float segmentX = kTrackX +
                        (kTrackWidth - segmentWidth) * progress;
                    const float fadeIn = std::min(1.0f, phase / 90.0f);
                    const float fadeOut = std::min(1.0f, (kTravelMs - phase) / 90.0f);
                    Gdiplus::SolidBrush movingIndicator(Gdiplus::Color(
                        static_cast<BYTE>(240.0f * std::min(fadeIn, fadeOut)),
                        static_cast<BYTE>(accentRed), static_cast<BYTE>(accentGreen),
                        static_cast<BYTE>(accentBlue)));
                    graphics.FillRectangle(&movingIndicator, segmentX, kTrackY, segmentWidth, kTrackHeight);
                }
            }
            else if (percent > 0)
            {
                graphics.FillRectangle(&indicator, kTrackX, kTrackY,
                    kTrackWidth * percent / 100.0f, kTrackHeight);
            }
            graphics.ResetClip();
            graphics.Flush(Gdiplus::FlushIntentionSync);
        }
        // Optional DC output is used by the offscreen rendering fixture.
        if (target != nullptr)
            BitBlt(target, 0, 0, m_width, m_height, m_frame.dc, 0, 0, SRCCOPY);
        const HWND window = m_window.load();
        POINT position = {m_x, m_y};
        POINT source = {};
        SIZE size = {m_width, m_height};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const bool submitted = window != nullptr && UpdateLayeredWindow(window, nullptr,
            &position, &size, m_frame.dc, &source, 0, &blend, ULW_ALPHA) != FALSE;
        if (submitted)
        {
            m_paintedRevision = revision;
            m_paintedGeometryRevision = m_geometryRevision;
        }
        return submitted;
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message)
        {
        case kShowMessage:
            if (self != nullptr)
            {
                self->m_animationStart = GetTickCount64();
                self->layout();
                self->updateAnimationTimer(false);
                if (self->redraw(true))
                {
                    ShowWindow(window, SW_SHOWNOACTIVATE);
                    self->updateAnimationTimer(true);
                }
            }
            return 0;
        case kHideMessage:
            KillTimer(window, 1);
            ShowWindow(window, SW_HIDE);
            return 0;
        case kResetAnimationMessage:
            if (self != nullptr)
            {
                self->m_animationStart = GetTickCount64();
                const bool visible = IsWindowVisible(window) != FALSE;
                self->updateAnimationTimer(visible);
                if (visible) self->redraw();
            }
            return 0;
        case kRedrawMessage:
            if (self != nullptr)
            {
                self->m_redrawQueued.store(false);
                if (IsWindowVisible(window)) self->redraw();
            }
            return 0;
        case WM_TIMER:
            if (wParam == 1 && self != nullptr && IsWindowVisible(window)) self->redraw();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (self != nullptr)
            {
                PAINTSTRUCT paint = {};
                BeginPaint(window, &paint);
                if (IsWindowVisible(window)) self->paint(nullptr);
                EndPaint(window, &paint);
                return 0;
            }
            break;
        case WM_SETTINGCHANGE:
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            if (self != nullptr)
            {
                self->layout();
                const bool visible = IsWindowVisible(window) != FALSE;
                self->updateAnimationTimer(visible);
                if (visible) self->redraw(true);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    std::thread m_thread;
    HDESK m_desktop = nullptr; // Borrowed from caller for UI thread startup.
    std::atomic<HWND> m_window{nullptr};
    std::atomic<bool> m_redrawQueued{false};
    mutable std::mutex m_stateMutex;
    std::condition_variable m_created;
    bool m_threadReady = false;
    bool m_threadSucceeded = false;
    std::wstring m_imagePath;
    std::wstring m_status = L"Ksword";
    std::wstring m_title = L"KSword";
    std::wstring m_version; // 与标题分开缓存，避免版本过长压缩品牌字号。
    std::wstring m_buildTime;
    std::wstring m_authors;
    std::wstring m_copyright;
    std::wstring m_license;
    std::wstring m_thirdParty;
    std::wstring m_website;
    std::wstring m_repository;
    SplashProgressMode m_mode = SplashProgressMode::Determinate;
    unsigned int m_accentRgb = 0x43A0FFU;
    int m_percent = 0;
    std::uint64_t m_stateRevision = 1; // Protected by m_stateMutex.
    std::uint64_t m_staticRevision = 1; // Background/text inputs only, under the same mutex.
    std::uint64_t m_paintedRevision = 0; // Splash UI thread only.
    std::uint64_t m_geometryRevision = 1;
    std::uint64_t m_paintedGeometryRevision = 0;

    std::uint64_t m_cachedStaticRevision = 0;
    std::uint64_t m_cachedStaticGeometryRevision = 0;
    DibSurface m_staticSurface;
    DibSurface m_frame;
    bool m_animationsEnabled = true;
    ULONG_PTR m_gdiplusToken = 0;
    std::unique_ptr<Gdiplus::Image> m_image;
    std::unique_ptr<Gdiplus::Image> m_logo;
    int m_imageWidth = 0;
    int m_imageHeight = 0;
    int m_width = kBaseWidth;
    int m_height = kBaseHeight;
    int m_x = 0;
    int m_y = 0;
    UINT m_dpi = 96;
    float m_scale = 1.0f;
    ULONGLONG m_animationStart = 0;
};

kStartupSplash::kStartupSplash() : m_impl(std::make_unique<Impl>()) {}
kStartupSplash::~kStartupSplash() = default;
void kStartupSplash::setBackgroundImagePath(const std::wstring& path) { m_impl->setBackgroundImagePath(path); }
void kStartupSplash::setVersionText(const std::wstring& version) { m_impl->setVersionText(version); }
void kStartupSplash::setBrandingText(const std::wstring& title, const std::wstring& buildTime,
    const std::wstring& authors) { m_impl->setBrandingText(title, buildTime, authors); }
void kStartupSplash::setLegalText(const std::wstring& copyright, const std::wstring& license,
    const std::wstring& thirdParty, const std::wstring& website,
    const std::wstring& repository)
{ m_impl->setLegalText(copyright, license, thirdParty, website, repository); }
void kStartupSplash::setProgressMode(SplashProgressMode mode) { m_impl->setProgressMode(mode); }
void kStartupSplash::setAccentColorRgb(unsigned int rgb) { m_impl->setAccentColorRgb(rgb); }
bool kStartupSplash::show(const std::string& initialStatusText) { return m_impl->show(initialStatusText); }
void kStartupSplash::hide() { m_impl->hide(); }
void kStartupSplash::progress(const std::string& operationName, int percent)
{ m_impl->progress(operationName, percent); }
bool kStartupSplash::ready() const { return m_impl->ready(); }

kStartupSplash kSplash;
