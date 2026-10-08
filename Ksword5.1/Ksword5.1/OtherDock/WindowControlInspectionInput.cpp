#include "WindowControlInspectionInput.h"
#include "WindowControlInspection.h"
#include <QApplication>
#include <QPointer>
#include <QTimer>
#include <QWidget>

namespace ks::control_inspection
{
    namespace
    {
        // Hook callbacks run on their installing UI thread, but do not enumerate
        // UIA or process Qt events. All follow-up UI work is queued.
        class InputRouter final : public QObject
        {
        public:
            explicit InputRouter(QObject* parent) : QObject(parent) {}
            ~InputRouter() override
            { if (m_mouse) ::UnhookWindowsHookEx(m_mouse); if (m_keyboard) ::UnhookWindowsHookEx(m_keyboard); s_router = nullptr; }
            static InputRouter& instance()
            { static QPointer<InputRouter> router; if (!router) router = new InputRouter(qApp); return *router; }
            void configure(QWidget* page, HWND root, HWND detail, std::function<void()> toggle,
                std::function<void()> cancel, std::function<void(QPoint)> pick)
            {
                ++m_session; m_gate.cancel(); m_page = page; m_root = root; m_detail = detail;
                m_toggle = std::move(toggle); m_cancel = std::move(cancel); m_pick = std::move(pick);
                s_router = this;
                if (!m_keyboard) m_keyboard = ::SetWindowsHookExW(WH_KEYBOARD_LL, keyboard,
                    ::GetModuleHandleW(nullptr), 0);
                updateMouse();
            }
            void release(QWidget* page)
            {
                if (m_page != page) return;
                ++m_session; m_gate.cancel(); m_page.clear(); m_toggle = {}; m_cancel = {}; m_pick = {};
                // A swallowed down must still have its up swallowed, even when
                // the owning page closes or pauses in the meantime.
                updateMouse();
                if (m_keyboard && !m_consumedC && !m_consumedEscape)
                { ::UnhookWindowsHookEx(m_keyboard); m_keyboard = nullptr; }
            }
            bool arm(bool armed)
            { m_gate.armed = armed; updateMouse(); return !armed || m_mouse != nullptr; }
            bool shortcutAvailable() const { return m_keyboard != nullptr; }
        private:
            void updateMouse()
            {
                if ((m_gate.armed || m_gate.consuming) && !m_mouse)
                    m_mouse = ::SetWindowsHookExW(WH_MOUSE_LL, mouse, ::GetModuleHandleW(nullptr), 0);
                if (!m_gate.armed && !m_gate.consuming && m_mouse)
                { ::UnhookWindowsHookEx(m_mouse); m_mouse = nullptr; }
                if (m_gate.armed && !m_mouse) m_gate.cancel();
            }
            bool shortcutScope() const
            {
                if (!m_page) return false;
                const HWND foreground = ::GetForegroundWindow();
                if (foreground == m_detail) return m_page->isVisible();
                return belongs(foreground, m_root, m_detail);
            }
            static LRESULT CALLBACK mouse(int code, WPARAM message, LPARAM argument)
            {
                auto* router = s_router;
                if (code >= 0 && router)
                {
                    const auto* data = reinterpret_cast<MSLLHOOKSTRUCT*>(argument);
                    const QPoint point(data->pt.x, data->pt.y);
                    const HWND hit = ::WindowFromPhysicalPoint(data->pt);
                    const bool inScope = router->m_page && belongs(hit, router->m_root, router->m_detail);
                    bool completed = false;
                    if (router->m_gate.mouse(static_cast<UINT>(message), point, inScope, completed))
                    {
                        if (completed)
                        {
                            const QPoint picked = router->m_gate.pressed;
                            const quint64 session = router->m_session;
                            QTimer::singleShot(0, router, [router, picked, session] {
                                router->updateMouse(); if (router->m_session == session && router->m_page && router->m_pick) router->m_pick(picked);
                            });
                        }
                        else if (!router->m_gate.consuming)
                            QTimer::singleShot(0, router, [router] { router->updateMouse(); });
                        return 1;
                    }
                }
                return ::CallNextHookEx(nullptr, code, message, argument);
            }
            static LRESULT CALLBACK keyboard(int code, WPARAM message, LPARAM argument)
            {
                auto* router = s_router;
                if (code >= 0 && router)
                {
                    const auto* data = reinterpret_cast<KBDLLHOOKSTRUCT*>(argument);
                    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
                    bool* consumed = data->vkCode == 'C' ? &router->m_consumedC
                        : data->vkCode == VK_ESCAPE ? &router->m_consumedEscape : nullptr;
                    if (down && consumed && *consumed) return 1;
                    if (up && consumed && *consumed)
                    {
                        *consumed = false;
                        if (!router->m_page) QTimer::singleShot(0, router, [router] {
                            if (!router->m_page && !router->m_consumedC && !router->m_consumedEscape && router->m_keyboard)
                            { ::UnhookWindowsHookEx(router->m_keyboard); router->m_keyboard = nullptr; }
                        });
                        return 1;
                    }
                    const bool shortcut = data->vkCode == 'C' && (::GetAsyncKeyState(VK_CONTROL) & 0x8000)
                        && (::GetAsyncKeyState(VK_SHIFT) & 0x8000) && !(::GetAsyncKeyState(VK_MENU) & 0x8000);
                    if (down && consumed && ((shortcut && router->shortcutScope())
                        || (data->vkCode == VK_ESCAPE && router->m_gate.armed && router->shortcutScope())))
                    {
                        if (!*consumed)
                        {
                            *consumed = true;
                            const bool escape = data->vkCode == VK_ESCAPE;
                            const quint64 session = router->m_session;
                            if (escape) router->m_gate.cancel();
                            QTimer::singleShot(0, router, [router, escape, session] {
                                if (router->m_session == session && router->m_page) {
                                    if (escape && router->m_cancel) router->m_cancel();
                                    else if (!escape && router->m_toggle) router->m_toggle();
                                }
                            });
                        }
                        return 1;
                    }
                }
                return ::CallNextHookEx(nullptr, code, message, argument);
            }
            inline static InputRouter* s_router = nullptr;
            QPointer<QWidget> m_page;
            HWND m_root = nullptr, m_detail = nullptr;
            HHOOK m_mouse = nullptr, m_keyboard = nullptr;
            PickGate m_gate;
            quint64 m_session = 0;
            bool m_consumedC = false, m_consumedEscape = false;
            std::function<void()> m_toggle, m_cancel;
            std::function<void(QPoint)> m_pick;
        };

    }
    void ConfigureInput(QWidget* page, HWND root, HWND detail, std::function<void()> toggle,
        std::function<void()> cancel, std::function<void(QPoint)> pick)
    { InputRouter::instance().configure(page, root, detail, std::move(toggle), std::move(cancel), std::move(pick)); }
    void ReleaseInput(QWidget* page) { InputRouter::instance().release(page); }
    bool ArmPicker(bool armed) { return InputRouter::instance().arm(armed); }
    bool PickerShortcutAvailable() { return InputRouter::instance().shortcutAvailable(); }
}
