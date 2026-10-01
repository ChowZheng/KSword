#include "LogSurface.h"
#include "../DebuggerBackend/KswordDebuggerFileProtocol.h"

#include <algorithm>
#include <cwchar>
#include <fstream>
#include <sstream>

namespace ksword::ce_log
{
    namespace
    {
        constexpr int kToggleId = 7101;
        HWND gLog = nullptr, gToggle = nullptr, gStatus = nullptr;
        HFONT gFont = nullptr;
        HBRUSH gWindowBrush = nullptr, gSurfaceBrush = nullptr;
        COLORREF gWindow = RGB(16, 24, 34), gSurface = RGB(20, 30, 42);
        COLORREF gText = RGB(220, 225, 232), gBorder = RGB(69, 83, 101), gAccent = RGB(51, 143, 231);
        bool gEnglish = false, gUseHvm = false;
        unsigned long gRevision = 0;
        std::wstring gControlPath, gStatePath;

        const wchar_t* text(const wchar_t* zh, const wchar_t* en) { return gEnglish ? en : zh; }
        COLORREF color(const wchar_t* name, COLORREF fallback)
        {
            wchar_t value[16]{};
            if (GetEnvironmentVariableW(name, value, _countof(value)) != 7 || value[0] != L'#') return fallback;
            wchar_t* end = nullptr;
            const unsigned long rgb = std::wcstoul(value + 1, &end, 16);
            if (end == nullptr || *end != 0 || rgb > 0xFFFFFFUL) return fallback;
            return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
        }
        void drawToggle(const DRAWITEMSTRUCT& item)
        {
            FillRect(item.hDC, &item.rcItem, gWindowBrush);
            const int left = item.rcItem.left + 4, top = item.rcItem.top + 8;
            RECT box{left, top, left + 16, top + 16};
            HBRUSH border = CreateSolidBrush(gBorder);
            FrameRect(item.hDC, &box, border); DeleteObject(border);
            if (gUseHvm)
            {
                HPEN pen = CreatePen(PS_SOLID, 2, gAccent);
                HGDIOBJ previous = SelectObject(item.hDC, pen);
                MoveToEx(item.hDC, left + 3, top + 8, nullptr);
                LineTo(item.hDC, left + 7, top + 12); LineTo(item.hDC, left + 13, top + 4);
                SelectObject(item.hDC, previous); DeleteObject(pen);
            }
            SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, gText);
            HGDIOBJ previous = SelectObject(item.hDC, gFont);
            RECT label = item.rcItem; label.left += 28;
            DrawTextW(item.hDC, text(L"使用 HVM 后端", L"Use HVM backend"), -1, &label,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            if ((item.itemState & ODS_FOCUS) != 0) DrawFocusRect(item.hDC, &label);
            SelectObject(item.hDC, previous);
        }
        void requestToggle()
        {
            if (gControlPath.empty()) return;
            const bool requested = !gUseHvm;
            std::ofstream request(gControlPath + L".new", std::ios::binary | std::ios::trunc);
            request << ++gRevision << ' ' << (requested ? 1 : 0) << '\n';
            request.close();
            if (!request || !MoveFileExW((gControlPath + L".new").c_str(), gControlPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                SetWindowTextW(gStatus, text(L"HVM 请求写入失败", L"Cannot write HVM selection"));
                return;
            }
            EnableWindow(gToggle, FALSE);
            SetWindowTextW(gStatus, text(L"等待 CE 确认后端选择…", L"Waiting for CE to confirm the selection..."));
        }
    }

    HWND create(HWND parent)
    {
        wchar_t language[32]{};
        (void)GetEnvironmentVariableW(L"KSWORD_PLUGIN_LANGUAGE", language, _countof(language));
        gEnglish = std::wcsncmp(language, L"en", 2) == 0;
        gWindow = color(L"KSWORD_PLUGIN_COLOR_WINDOW", gWindow);
        gSurface = color(L"KSWORD_PLUGIN_COLOR_SURFACE", gSurface);
        gText = color(L"KSWORD_PLUGIN_COLOR_TEXT_PRIMARY", gText);
        gBorder = color(L"KSWORD_PLUGIN_COLOR_BORDER", gBorder);
        gAccent = color(L"KSWORD_PLUGIN_COLOR_ACCENT", gAccent);
        gWindowBrush = CreateSolidBrush(gWindow); gSurfaceBrush = CreateSolidBrush(gSurface);
        HDC dc = GetDC(parent); const int dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(parent, dc);
        gFont = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
        gToggle = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            8, 4, 240, 32, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToggleId)), GetModuleHandleW(nullptr), nullptr);
        gStatus = CreateWindowExW(0, L"STATIC", text(L"等待 CE 调试后端…", L"Waiting for the CE debugger backend..."),
            WS_CHILD | WS_VISIBLE | SS_LEFT, 8, 38, 1, 22, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        gLog = CreateWindowExW(0, L"EDIT", text(L"CE 保持独立窗口；此页接收后端日志。\r\n", L"CE runs in its own window; this page receives backend logs.\r\n"),
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 64, 1, 1, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (gToggle == nullptr || gStatus == nullptr || gLog == nullptr || gFont == nullptr) return nullptr;
        for (const HWND child : {gToggle, gStatus, gLog})
            (void)SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
        EnableWindow(gToggle, FALSE);
        resize(parent);
        return gLog;
    }

    void resize(HWND parent)
    {
        RECT client{}; GetClientRect(parent, &client);
        const int width = (std::max)(1L, client.right), height = (std::max)(1L, client.bottom);
        MoveWindow(gToggle, 8, 4, (std::min)(260, (std::max)(1, width - 16)), 32, TRUE);
        MoveWindow(gStatus, 8, 38, (std::max)(1, width - 16), 22, TRUE);
        MoveWindow(gLog, 0, 64, width, (std::max)(1, height - 64), TRUE);
    }

    bool message(HWND parent, UINT messageId, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        if (messageId == WM_COMMAND && LOWORD(wParam) == kToggleId && HIWORD(wParam) == BN_CLICKED)
        { requestToggle(); result = 0; return true; }
        if (messageId == WM_DRAWITEM && wParam == kToggleId)
        { drawToggle(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)); result = TRUE; return true; }
        if (messageId == WM_CTLCOLORSTATIC || messageId == WM_CTLCOLOREDIT)
        {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            const bool editor = reinterpret_cast<HWND>(lParam) == gLog;
            SetTextColor(dc, gText); SetBkColor(dc, editor ? gSurface : gWindow);
            result = reinterpret_cast<LRESULT>(editor ? gSurfaceBrush : gWindowBrush); return true;
        }
        if (messageId == WM_ERASEBKGND)
        {
            RECT client{}; GetClientRect(parent, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, gWindowBrush);
            result = TRUE; return true;
        }
        return false;
    }

    void setSession(const std::wstring& logPath)
    {
        gControlPath = logPath.empty() ? L"" : logPath + L".control";
        gStatePath = logPath.empty() ? L"" : logPath + L".state"; gRevision = 0;
        if (logPath.empty())
        {
            EnableWindow(gToggle, FALSE);
            SetWindowTextW(gStatus, text(L"CE 已退出", L"CE has exited"));
        }
    }

    void pollState()
    {
        if (gStatePath.empty()) return;
        std::string packet;
        if (!ksword::debugger::readControlPacket(gStatePath, packet)) return;
        std::istringstream state(packet);
        unsigned long revision = 0, error = 0, selected = 0, window = 0, resident = 0, protocol = 0;
        if (!(state >> revision >> error >> selected >> window >> resident >> protocol)) return;
        if (revision < gRevision) return;
        gRevision = revision; gUseHvm = selected != 0;
        std::wstring status;
        if (error != 0)
            status = std::wstring(text(L"后端切换失败，错误 ", L"Backend selection failed, error ")) + std::to_wstring(error);
        else if (gUseHvm)
            status = text(L"HVM：私有内存窗口", L"HVM: private memory window") +
                std::wstring(protocol != 0 ? text(L" | EPT 调试可用", L" | EPT debugger available") : text(L" | EPT 调试不可用", L" | EPT debugger unavailable")) +
                (resident != 0 ? text(L" | 常驻中", L" | resident active") : text(L" | 尚未常驻", L" | not resident yet"));
        else
            status = text(L"R0：KSword 驱动内存后端", L"R0: KSword driver memory backend");
        if (window == 0 && gUseHvm) status += text(L" | 私有窗口已失效", L" | private window unavailable");
        SetWindowTextW(gStatus, status.c_str());
        EnableWindow(gToggle, error != ERROR_MOD_NOT_FOUND && error != ERROR_DEVICE_NOT_CONNECTED);
        InvalidateRect(gToggle, nullptr, TRUE);
    }

    void destroy()
    {
        if (gFont != nullptr) DeleteObject(gFont);
        if (gWindowBrush != nullptr) DeleteObject(gWindowBrush);
        if (gSurfaceBrush != nullptr) DeleteObject(gSurfaceBrush);
        gFont = nullptr; gWindowBrush = nullptr; gSurfaceBrush = nullptr;
        gToggle = nullptr; gStatus = nullptr; gLog = nullptr;
    }
}
