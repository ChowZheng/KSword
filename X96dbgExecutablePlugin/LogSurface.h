#pragma once
#include <Windows.h>
#include <string>

namespace ksword::x96_log
{
    HWND create(HWND parent);
    void resize(HWND parent);
    bool message(HWND parent, UINT messageId, WPARAM wParam, LPARAM lParam, LRESULT& result);
    void setSession(const std::wstring& logPath, const std::string& sessionId, const std::wstring& preferencesPath = L"");
    void pollState();
    void setStatus(const std::wstring& value);
    void append(const std::wstring& value);
    void destroy();
}
