#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>

static volatile LONG counter;
static HWND details;

#pragma code_seg(push, ".kswdemo")
__declspec(noinline) void increment(void)
{
    ++counter;
}
#pragma code_seg(pop)

static int prepare_page(void)
{
    volatile unsigned char *code = (volatile unsigned char *)(uintptr_t)&increment;
    DWORD original, ignored;
    PSAPI_WORKING_SET_EX_INFORMATION page = {0};
    unsigned char value;
    if (!VirtualProtect((void *)code, 1, PAGE_EXECUTE_WRITECOPY, &original)) return 0;
    value = *code;
    *code = value;
    if (!VirtualProtect((void *)code, 1, original, &ignored)) return 0;
    if (!FlushInstructionCache(GetCurrentProcess(), (void *)code, 1)) return 0;
    page.VirtualAddress = (void *)code;
    if (!QueryWorkingSetEx(GetCurrentProcess(), &page, sizeof(page))) return 0;
    return page.VirtualAttributes.Valid && !page.VirtualAttributes.Shared;
}

static void update_details(void)
{
    char text[512];
    const unsigned char *code = (const unsigned char *)(uintptr_t)&increment;
    (void)sprintf_s(text, sizeof(text),
        "PID: %lu\r\nExecution address: %p\r\nData address: %p (4 bytes)\r\n"
        "Code backing: COW private\r\nOriginal read: %02X %02X %02X %02X\r\nCounter: %ld",
        GetCurrentProcessId(), (void *)(uintptr_t)&increment, (void *)&counter,
        code[0], code[1], code[2], code[3], counter);
    SetWindowTextA(details, text);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE:
        details = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE,
            18, 18, 530, 150, window, NULL, NULL, NULL);
        CreateWindowExA(0, "BUTTON", "Increment", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            18, 184, 180, 40, window, (HMENU)(uintptr_t)1, NULL, NULL);
        update_details();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == 1 && HIWORD(wparam) == BN_CLICKED) {
            increment();
            update_details();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(window, message, wparam, lparam);
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    WNDCLASSA type = {0};
    HWND window;
    MSG message;
    (void)previous; (void)command;
    if (!prepare_page()) {
        MessageBoxA(NULL, "Cannot prepare private code page.", "Breakpoint GUI Demo", MB_ICONERROR);
        return 1;
    }
    type.lpfnWndProc = window_proc;
    type.hInstance = instance;
    type.lpszClassName = "KSwordBreakpointGuiDemo";
    type.hCursor = LoadCursorA(NULL, IDC_ARROW);
    type.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassA(&type)) return 1;
    window = CreateWindowExA(0, type.lpszClassName, "KSword Breakpoint GUI Demo",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 590, 290,
        NULL, NULL, instance, NULL);
    if (!window) return 1;
    ShowWindow(window, show);
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
    return (int)message.wParam;
}
