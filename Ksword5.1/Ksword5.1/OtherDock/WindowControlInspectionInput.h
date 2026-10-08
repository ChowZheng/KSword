#pragma once
#include <Windows.h>
#include <QPoint>
#include <functional>
class QWidget;

namespace ks::control_inspection
{
    // Input state is independent of rendering/COM and survives the owning page
    // just long enough to consume any paired mouse/key release.
    void ConfigureInput(QWidget* page, HWND root, HWND detail,
        std::function<void()> toggle, std::function<void()> cancel,
        std::function<void(QPoint)> pick);
    void ReleaseInput(QWidget* page);
    bool ArmPicker(bool armed);
    bool PickerShortcutAvailable();
}
