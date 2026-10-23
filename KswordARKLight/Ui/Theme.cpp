#include "Theme.h"

namespace Ksword::Ui {

Theme::Theme() : windowBrush_(nullptr), panelBrush_(nullptr), accentBrush_(nullptr), borderPen_(nullptr) {}

Theme::~Theme() {
    shutdown();
}

void Theme::ensure() {
    if (!windowBrush_) {
        windowBrush_ = ::CreateSolidBrush(windowColor);
    }
    if (!panelBrush_) {
        panelBrush_ = ::CreateSolidBrush(panelColor);
    }
    if (!accentBrush_) {
        accentBrush_ = ::CreateSolidBrush(accentColor);
    }
    if (!borderPen_) {
        borderPen_ = ::CreatePen(PS_SOLID, 1, borderColor);
    }
}

void Theme::shutdown() {
    if (windowBrush_) {
        ::DeleteObject(windowBrush_);
        windowBrush_ = nullptr;
    }
    if (panelBrush_) {
        ::DeleteObject(panelBrush_);
        panelBrush_ = nullptr;
    }
    if (accentBrush_) {
        ::DeleteObject(accentBrush_);
        accentBrush_ = nullptr;
    }
    if (borderPen_) {
        ::DeleteObject(borderPen_);
        borderPen_ = nullptr;
    }
}

HBRUSH Theme::windowBrush() const { return windowBrush_; }
HBRUSH Theme::panelBrush() const { return panelBrush_; }
HBRUSH Theme::accentBrush() const { return accentBrush_; }

HPEN Theme::borderPen() const { return borderPen_; }

Theme& AppTheme() {
    static Theme theme;
    theme.ensure();
    return theme;
}

} // namespace Ksword::Ui
