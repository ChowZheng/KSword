// 隐藏的Win32子窗回归：链接两份真实日志表面，不启动调试器或访问后端文件。
#include "../DebuggerBackend/KswordPluginTheme.h"
#include "../CheatEngineExecutablePlugin/LogSurface.h"
#include "../X96dbgExecutablePlugin/LogSurface.h"
#include <iostream>
#include <string>

namespace
{
    int assertions = 0; // 真实断言数，便于确认夹具完整执行。
    int failures = 0; // 累计失败，最终退出码不能掩盖断言失败。

    void check(bool condition, const char* description)
    {
        ++assertions;
        if (!condition)
        {
            ++failures;
            std::cerr << description << '\n';
        }
    }

    // 两个窗口过程只把消息路由给生产表面，不运行Launcher的初始化/后端轮询。
    template<bool CheatEngine>
    LRESULT CALLBACK surfaceProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_CREATE)
        {
            HWND log = nullptr;
            if constexpr (CheatEngine) log = ksword::ce_log::create(window);
            else log = ksword::x96_log::create(window);
            return log == nullptr ? -1 : 0;
        }
        if (message == WM_DESTROY)
        {
            if constexpr (CheatEngine) ksword::ce_log::destroy();
            else ksword::x96_log::destroy();
            return 0;
        }
        LRESULT result = 0;
        bool handled = false;
        if constexpr (CheatEngine) handled = ksword::ce_log::message(window, message, wParam, lParam, result);
        else handled = ksword::x96_log::message(window, message, wParam, lParam, result);
        return handled ? result : DefWindowProcW(window, message, wParam, lParam);
    }

    // 找真实只读多行编辑框，不依赖两个产品各自的控件ID分配。
    BOOL CALLBACK findLog(HWND child, LPARAM context)
    {
        wchar_t className[32]{};
        GetClassNameW(child, className, 32);
        const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
        if (std::wstring(className) == L"Edit" && (style & ES_MULTILINE) != 0)
        {
            *reinterpret_cast<HWND*>(context) = child;
            return FALSE;
        }
        return TRUE;
    }

    // 模拟宿主的同步COPYDATA传输，仍调用生产receiver而不是复写求色算法。
    LRESULT send(HWND window, HWND sender, const ksword::plugin_theme::Packet& packet,
        DWORD size = sizeof(ksword::plugin_theme::Packet), ULONG_PTR tag = ksword::plugin_theme::kCopyDataTag)
    {
        COPYDATASTRUCT data{};
        data.dwData = tag;
        data.cbData = size;
        data.lpData = const_cast<ksword::plugin_theme::Packet*>(&packet);
        DWORD_PTR result = 0;
        const LRESULT delivered = SendMessageTimeoutW(window, WM_COPYDATA,
            reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&data),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result);
        check(delivered != 0, "theme message was not delivered");
        return static_cast<LRESULT>(result);
    }

    // 用真实WM_CTLCOLOR取实际文字/底色和画刷，确认旧编辑框已更新。
    void verifyColors(HWND window, HWND log, const ksword::plugin_theme::Packet& packet)
    {
        HDC dc = GetDC(log);
        const HBRUSH brush = reinterpret_cast<HBRUSH>(SendMessageW(window, WM_CTLCOLORSTATIC,
            reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(log)));
        LOGBRUSH details{};
        check(GetObjectW(brush, sizeof(details), &details) != 0, "surface brush is invalid");
        check(details.lbColor == packet.surface, "existing editor retained old brush");
        check(GetTextColor(dc) == packet.text, "existing editor retained old text");
        check(GetBkColor(dc) == packet.surface, "existing editor retained old background");
        ReleaseDC(log, dc);
        wchar_t contents[64]{};
        GetWindowTextW(log, contents, 64);
        check(std::wstring(contents) == L"persisted log", "theme update replaced log contents");
        DWORD first = 0, last = 0;
        SendMessageW(log, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
        check(first == 2 && last == 6, "theme update changed log selection");
    }

    template<bool CheatEngine>
    void exercise(HWND host, const wchar_t* className)
    {
        WNDCLASSW definition{};
        definition.hInstance = GetModuleHandleW(nullptr);
        definition.lpfnWndProc = surfaceProcedure<CheatEngine>;
        definition.lpszClassName = className;
        check(RegisterClassW(&definition) != 0, "cannot register fixture window");
        HWND window = CreateWindowExW(0, className, L"", WS_CHILD,
            0, 0, 640, 480, host, nullptr, definition.hInstance, nullptr);
        check(window != nullptr, "cannot create production surface");
        if (window == nullptr) return;
        HWND log = nullptr;
        EnumChildWindows(window, findLog, reinterpret_cast<LPARAM>(&log));
        check(log != nullptr, "actual production editor was not created");
        if (log == nullptr)
        {
            DestroyWindow(window);
            return;
        }
        SetWindowTextW(log, L"persisted log");
        SendMessageW(log, EM_SETSEL, 2, 6);
        DWORD steadyGdiCount = 0; // 首次绘制预热之后的进程GDI数量，排除系统控件惰性缓存。
        // 循环A/B/C颜色，防止只覆盖首次换色或积累旧GDI画刷。
        for (DWORD cycle = 0; cycle < 100; ++cycle)
        {
            ksword::plugin_theme::Packet packet;
            packet.window = RGB(cycle, 255 - cycle, 17);
            packet.surface = RGB(255 - cycle, cycle, 29);
            packet.text = RGB(cycle, 37, 255 - cycle);
            packet.border = RGB(19, cycle, 33);
            packet.accent = RGB(29, 41, cycle);
            check(send(window, host, packet) == TRUE, "valid snapshot rejected");
            verifyColors(window, log, packet);
            // 非父宿主、错长度、错版本、错标签和特殊COLORREF均不得改写旧色。
            check(send(window, nullptr, packet) == FALSE, "non-parent sender accepted");
            check(send(window, host, packet, sizeof(packet) - 1) == FALSE, "short snapshot accepted");
            check(send(window, host, packet, sizeof(packet), 0) == FALSE, "wrong tag accepted");
            auto invalid = packet;
            invalid.version = 2;
            check(send(window, host, invalid) == FALSE, "unknown version accepted");
            invalid = packet;
            invalid.accent |= 0xFF000000UL;
            check(send(window, host, invalid) == FALSE, "non-RGB color accepted");
            verifyColors(window, log, packet);
            if (cycle == 0)
            {
                steadyGdiCount = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            }
        }
        check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == steadyGdiCount,
            "repeated theme refresh leaked GDI resources");
        DestroyWindow(window);
        UnregisterClassW(className, definition.hInstance);
    }
}

int main()
{
    // 根窗始终隐藏，避免干扰桌面；GDI稳定性在每个表面的实际换色循环内验证。
    HWND host = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(host != nullptr, "cannot create hidden host");
    if (host != nullptr)
    {
        exercise<true>(host, L"KSwordThemeCEFixture");
        exercise<false>(host, L"KSwordThemeX96Fixture");
        DestroyWindow(host);
    }
    std::cout << "PLUGIN_THEME_ASSERTIONS=" << assertions << '\n'
        << "PLUGIN_THEME_FAILURES=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
