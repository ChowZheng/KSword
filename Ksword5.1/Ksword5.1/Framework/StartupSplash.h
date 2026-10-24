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
    // Set before show(); only the right artwork panel changes.
    void setBackgroundImagePath(const std::wstring& path);
    void setLegalText(const std::wstring& copyright, const std::wstring& license,
        const std::wstring& thirdParty, const std::wstring& website,
        const std::wstring& repository);
    void setProgressMode(SplashProgressMode mode);
    // RGB value in 0xRRGGBB form. Defaults to Ksword's #43A0FF.
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
