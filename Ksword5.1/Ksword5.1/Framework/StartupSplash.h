#pragma once

#include <memory>
#include <string>

enum class SplashProgressMode
{
    Determinate,
    Indeterminate
};

// The splash owns its Win32 UI thread so animation continues during startup work.
// The API deliberately matches the existing kStartupSplash call sites.
class kStartupSplash final
{
public:
    kStartupSplash();
    ~kStartupSplash();
    kStartupSplash(const kStartupSplash&) = delete;
    kStartupSplash& operator=(const kStartupSplash&) = delete;

    // If unset, loads the bundled Style\StartupLandscape.png next to the host executable.
    // Set before show(); the complete image determines the canvas aspect ratio without cropping or tinting.
    void setBackgroundImagePath(const std::wstring& path);
    // 发行版本独立于产品名称，数字和PRE/RC后缀在紧凑信息行显示。
    void setVersionText(const std::wstring& version);
    void setBrandingText(const std::wstring& title, const std::wstring& buildTime,
        const std::wstring& authors);
    void setLegalText(const std::wstring& copyright, const std::wstring& license,
        const std::wstring& thirdParty, const std::wstring& website,
        const std::wstring& repository);
    void setProgressMode(SplashProgressMode mode);
    // RGB value in 0xRRGGBB form. Defaults to #4C8DEB.
    void setAccentColorRgb(unsigned int rgb);
    bool show(const std::string& initialStatusText = std::string());
    void hide();
    void progress(const std::string& operationName, int progressPercent);
    bool ready() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

extern kStartupSplash kSplash;
