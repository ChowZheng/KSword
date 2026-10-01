#pragma once
#include <Windows.h>
#include <string>

namespace ksword::ce_log
{
    HWND create(HWND parent);
    void resize(HWND parent);
    bool message(HWND parent, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
    void setSession(const std::wstring& logPath);
    void pollState();
    void destroy();
}
