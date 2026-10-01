#include "LogSurface.h"
#include "../TitanEnginePlugin/ControlProtocol.h"
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    constexpr wchar_t kWindowClass[] = L"KSwordX96dbgLogSurface";
    constexpr UINT kStartMessage = WM_APP + 1;
    constexpr UINT_PTR kPollTimer = 1;
    struct Arguments { std::wstring command; DWORD pid = 0, hostPid = 0; HWND parent = nullptr; bool valid = false; };
    HWND gWindow = nullptr;
    HANDLE gDebugger = nullptr, gHost = nullptr;
    std::wstring gLogPath, gSessionDirectory;
    std::string gSessionId, gPendingLog;
    std::streamoff gLogOffset = 0;

    std::string utf8(const std::wstring& value)
    {
        const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0) return {};
        std::string result(static_cast<size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
        return result;
    }
    std::wstring wide(const std::string& value)
    {
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (count <= 0) return L"[Invalid UTF-8 log line]";
        std::wstring result(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
        return result;
    }
    std::string json(const std::string& value)
    {
        const char digits[] = "0123456789abcdef";
        std::string result;
        for (unsigned char c : value)
        {
            if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
            else if (c < 32) { result += "\\u00"; result += digits[c >> 4]; result += digits[c & 15]; }
            else result += static_cast<char>(c);
        }
        return result;
    }
    void emit(const char* event, const std::string& fields)
    {
        const std::string line = "{\"protocol\":\"ksword-plugin/1\",\"plugin_id\":\"x96dbg\",\"event\":\"" +
            std::string(event) + "\"" + (fields.empty() ? "" : "," + fields) + "}\n";
        const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output != nullptr && output != INVALID_HANDLE_VALUE)
        { DWORD written = 0; (void)WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr); }
    }
    void log(const std::string& line)
    {
        if (line.empty()) return;
        // Never forward malformed UTF-8 into PluginHost's strict JSON parser.
        const std::wstring rendered = wide(line.substr(0, 4096));
        const std::string clipped = utf8(rendered);
        ksword::x96_log::append(rendered);
        emit("log", "\"source\":\"titan_engine\",\"message\":\"" + json(clipped) + "\"");
    }
    std::string failure(const std::string& operation, DWORD error)
    {
        wchar_t message[512]{};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, message, _countof(message), nullptr);
        std::wstring detail(message);
        while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n')) detail.pop_back();
        return operation + " (Win32 error " + std::to_string(error) + (detail.empty() ? ")" : ": " + utf8(detail) + ")");
    }
    std::wstring directory()
    {
        std::vector<wchar_t> path(32768);
        const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (count == 0 || count >= path.size()) return {};
        std::wstring result(path.data(), count);
        return result.substr(0, result.find_last_of(L"\\/"));
    }
    bool number(const wchar_t* value, unsigned long long maximum, unsigned long long& output)
    {
        if (value == nullptr || *value == 0 || *value == L'-' || *value == L'+') return false;
        for (const wchar_t* c = value; *c != 0; ++c) if (*c < L'0' || *c > L'9') return false;
        wchar_t* end = nullptr; errno = 0;
        output = std::wcstoull(value, &end, 10);
        return errno == 0 && end != value && *end == 0 && output != 0 && output <= maximum;
    }
    Arguments parse(int argc, wchar_t* argv[])
    {
        Arguments args;
        if (argc < 3 || std::wstring(argv[1]) != L"--ksword-plugin") return args;
        args.command = argv[2];
        if (args.command != L"tab" && args.command != L"attach" && args.command != L"check" && args.command != L"info") return args;
        for (int index = 3; index < argc; ++index)
        {
            const std::wstring option = argv[index];
            if (option == L"--") continue;
            if (index + 1 == argc) return args;
            unsigned long long value = 0;
            if (option == L"--pid") { if (!number(argv[++index], MAXDWORD, value)) return args; args.pid = static_cast<DWORD>(value); }
            else if (option == L"--host-pid") { if (!number(argv[++index], MAXDWORD, value)) return args; args.hostPid = static_cast<DWORD>(value); }
            else if (option == L"--parent-hwnd") { if (!number(argv[++index], UINTPTR_MAX, value)) return args; args.parent = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value)); }
            else if (option == L"--target-kind") { if (std::wstring(argv[++index]) != L"process") return args; }
            else if (option == L"--path" || option == L"--process-name") ++index;
            else return args;
        }
        if (args.command == L"tab")
        {
            DWORD owner = 0;
            args.valid = args.parent != nullptr && args.hostPid != 0 && IsWindow(args.parent) &&
                GetWindowThreadProcessId(args.parent, &owner) != 0 && owner == args.hostPid && args.pid == 0;
        }
        else args.valid = args.parent == nullptr && args.hostPid == 0 && (args.command == L"attach" ? args.pid != 0 : args.pid == 0);
        return args;
    }
    bool amd64(const std::wstring& path, std::string& error)
    {
        std::ifstream stream(path, std::ios::binary);
        IMAGE_DOS_HEADER dos{}; DWORD signature = 0; IMAGE_FILE_HEADER header{};
        if (!stream.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos))
        { error = "Missing or invalid x64 PE: " + utf8(path); return false; }
        stream.seekg(dos.e_lfanew);
        if (!stream.read(reinterpret_cast<char*>(&signature), sizeof(signature)) || signature != IMAGE_NT_SIGNATURE ||
            !stream.read(reinterpret_cast<char*>(&header), sizeof(header)) || header.Machine != IMAGE_FILE_MACHINE_AMD64)
        { error = "Only AMD64 payloads are supported: " + utf8(path); return false; }
        return true;
    }
    bool payload(std::string& error)
    {
        const std::wstring root = directory() + L"\\payload\\x64dbg\\";
        for (const auto file : {L"x64dbg.exe", L"x64dbg.dll", L"x64gui.dll", L"x64bridge.dll", L"TitanEngine.dll", L"KSword\\TitanEngine.dll"})
            if (!amd64(root + file, error)) return false;
        return true;
    }
    bool session(std::string& error)
    {
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) { error = "Cannot allocate a debugger session identity."; return false; }
        wchar_t token[40]{};
        if (StringFromGUID2(id, token, _countof(token)) == 0) { error = "Cannot format the debugger session identity."; return false; }
        std::wstring value(token);
        gSessionId = utf8(value.substr(1, value.size() - 2));
        const std::wstring sessions = directory() + L"\\sessions";
        if (!CreateDirectoryW(sessions.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        { error = failure("Cannot create debugger session directory", GetLastError()); return false; }
        gSessionDirectory = sessions + L"\\" + value.substr(1, value.size() - 2);
        if (!CreateDirectoryW(gSessionDirectory.c_str(), nullptr))
        { error = failure("Cannot create isolated debugger user directory", GetLastError()); return false; }
        // Canonical PR #3974 uses the user directory INI before checked engine loading.
        // The narrow KSword core patch reserves DEBUG_ENGINE value 4.
        std::ofstream ini(gSessionDirectory + L"\\x64dbg.ini", std::ios::binary | std::ios::trunc);
        ini << "[Engine]\r\nDebugEngine=4\r\n"; ini.close();
        if (!ini) { error = "Cannot write isolated debugger engine selection."; return false; }
        gLogPath = gSessionDirectory + L"\\backend.log";
        std::ofstream empty(gLogPath, std::ios::binary | std::ios::trunc);
        if (!empty) { error = "Cannot create debugger session log: " + utf8(gLogPath); return false; }
        auto options = ksword::titan::control::defaults();
        std::ifstream preferences(directory() + L"\\x96dbg-options.ini", std::ios::binary);
        std::string packet;
        if (preferences && (!std::getline(preferences, packet) || !ksword::titan::control::preferences(packet, options)))
            empty << "Invalid saved x96dbg policy; using documented normal defaults.\n";
        empty.close();
        // Persist options, never HVM activation. The proxy applies this packet
        // outside DllMain and publishes an actual-state ACK before UI changes.
        std::ofstream control(gLogPath + L".control", std::ios::binary | std::ios::trunc);
        control << ksword::titan::control::request(gSessionId, 1, 0, options); control.close();
        if (!control) { error = "Cannot write initial debugger policy request."; return false; }
        return true;
    }
    std::vector<wchar_t> environment()
    {
        std::vector<std::wstring> entries;
        LPWCH inherited = GetEnvironmentStringsW();
        if (inherited != nullptr)
        {
            for (const wchar_t* p = inherited; *p != 0; p += std::wcslen(p) + 1)
            {
                if (_wcsnicmp(p, L"KSWORD_DEBUGGER_", 16) != 0) entries.emplace_back(p);
            }
            FreeEnvironmentStringsW(inherited);
        }
        const std::wstring sessionId(gSessionId.begin(), gSessionId.end());
        entries.push_back(L"KSWORD_DEBUGGER_LOG_FILE=" + gLogPath);
        entries.push_back(L"KSWORD_DEBUGGER_CONTROL_FILE=" + gLogPath + L".control");
        entries.push_back(L"KSWORD_DEBUGGER_STATE_FILE=" + gLogPath + L".state");
        entries.push_back(L"KSWORD_DEBUGGER_SESSION_ID=" + sessionId);
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        std::vector<wchar_t> block;
        for (const auto& entry : entries) { block.insert(block.end(), entry.begin(), entry.end()); block.push_back(0); }
        block.push_back(0); return block;
    }
    bool launch(DWORD pid, DWORD& debuggerPid, std::string& error)
    {
        if (!payload(error) || !session(error)) return false;
        const std::wstring root = directory() + L"\\payload\\x64dbg";
        const std::wstring exe = root + L"\\x64dbg.exe";
        std::wstring command = L"\"" + exe + L"\" -userdir \"" + gSessionDirectory + L"\"";
        if (pid != 0) command += L" -p " + std::to_wstring(pid);
        std::vector<wchar_t> mutableCommand(command.begin(), command.end()); mutableCommand.push_back(0);
        auto block = environment();
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP, block.data(), root.c_str(), &startup, &process))
        { error = failure("Cannot start standalone x64dbg", GetLastError()); return false; }
        CloseHandle(process.hThread); gDebugger = process.hProcess; debuggerPid = process.dwProcessId;
        return true;
    }
    void pollLog()
    {
        if (gLogPath.empty()) return;
        std::ifstream stream(gLogPath, std::ios::binary);
        if (!stream) return;
        stream.seekg(0, std::ios::end); const auto end = stream.tellg();
        if (end < 0) return;
        if (end < gLogOffset) { gLogOffset = 0; gPendingLog.clear(); }
        if (end - gLogOffset > 65536) { gLogOffset = end - std::streamoff(65536); gPendingLog.clear(); log("Older debugger log entries omitted."); }
        stream.seekg(gLogOffset);
        char data[4096];
        while (stream && gLogOffset < end)
        {
            stream.read(data, static_cast<std::streamsize>((std::min)(end - std::streampos(gLogOffset), std::streamoff(sizeof(data)))));
            const auto count = stream.gcount(); if (count <= 0) break; gLogOffset += count;
            for (std::streamsize i = 0; i < count; ++i)
            {
                if (data[i] == '\n')
                { if (!gPendingLog.empty() && gPendingLog.back() == '\r') gPendingLog.pop_back(); log(gPendingLog); gPendingLog.clear(); }
                else if (gPendingLog.size() < 4096) gPendingLog += data[i];
            }
        }
    }
    void closeHandle(HANDLE& handle) { if (handle != nullptr) { CloseHandle(handle); handle = nullptr; } }
    void removeSessionFiles()
    {
        if (gLogPath.empty()) return;
        for (const auto suffix : {L"", L".control", L".control.new", L".state", L".state.new"}) DeleteFileW((gLogPath + suffix).c_str());
        gLogPath.clear(); gSessionId.clear(); gLogOffset = 0; gPendingLog.clear();
    }
    LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        LRESULT result = 0;
        if (ksword::x96_log::message(window, message, wParam, lParam, result)) return result;
        switch (message)
        {
        case WM_CREATE: return ksword::x96_log::create(window) == nullptr ? -1 : 0;
        case WM_SIZE: ksword::x96_log::resize(window); return 0;
        case kStartMessage:
        {
            DWORD pid = 0; std::string error;
            if (!launch(0, pid, error))
            { log(error); ksword::x96_log::setStatus(wide(error)); emit("warning", "\"code\":\"launch_failed\",\"message\":\"" + json(error) + "\""); }
            else { ksword::x96_log::setSession(gLogPath, gSessionId, directory() + L"\\x96dbg-options.ini"); log("Standalone x64dbg PID " + std::to_string(pid) + " started using the KSword engine."); emit("debugger_started", "\"debugger_pid\":" + std::to_string(pid)); }
            SetTimer(window, kPollTimer, 200, nullptr); return 0;
        }
        case WM_TIMER:
            if (wParam != kPollTimer) return 0;
            if (gHost == nullptr || WaitForSingleObject(gHost, 0) == WAIT_OBJECT_0) { DestroyWindow(window); return 0; }
            pollLog(); ksword::x96_log::pollState();
            if (gDebugger != nullptr && WaitForSingleObject(gDebugger, 0) == WAIT_OBJECT_0)
            {
                DWORD exitCode = 0; GetExitCodeProcess(gDebugger, &exitCode);
                pollLog(); log("x64dbg exited, code " + std::to_string(exitCode) + ".");
                emit("debugger_exited", "\"exit_code\":" + std::to_string(exitCode));
                closeHandle(gDebugger); removeSessionFiles(); ksword::x96_log::setSession(L"", "");
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window, kPollTimer); pollLog();
            // Closing the Tab releases observer handles. It never terminates the debugger.
            closeHandle(gDebugger); closeHandle(gHost); ksword::x96_log::destroy(); gWindow = nullptr;
            PostQuitMessage(0); return 0;
        default: return DefWindowProcW(window, message, wParam, lParam);
        }
    }
    int tab(const Arguments& args)
    {
        gHost = OpenProcess(SYNCHRONIZE, FALSE, args.hostPid);
        if (gHost == nullptr) { emit("error", "\"message\":\"Cannot observe the KSword host process.\""); return 2; }
        WNDCLASSEXW windowClass{}; windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = procedure; windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW); windowClass.lpszClassName = kWindowClass;
        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        { closeHandle(gHost); emit("error", "\"message\":\"Cannot register the Tab window class.\""); return 2; }
        gWindow = CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, L"KSword x96dbg",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1,
            args.parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (gWindow == nullptr) { closeHandle(gHost); ksword::x96_log::destroy(); emit("error", "\"message\":\"Cannot create the Tab child window.\""); return 2; }
        emit("tab_ready", "\"hwnd\":\"" + std::to_string(reinterpret_cast<std::uintptr_t>(gWindow)) + "\"");
        PostMessageW(gWindow, kStartMessage, 0, 0);
        MSG message{}; int result = 0;
        while ((result = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0)
        { if (!IsDialogMessageW(gWindow, &message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
        return result == -1 ? 2 : 0;
    }
}

int wmain(int argc, wchar_t* argv[])
{
    const auto args = parse(argc, argv);
    if (!args.valid) { emit("error", "\"code\":\"invalid_arguments\",\"message\":\"Expected --ksword-plugin info|check|attach|tab with valid target/host arguments.\""); return 2; }
    if (args.command == L"tab") return tab(args);
    std::string error;
    if (args.command == L"info" || args.command == L"check")
    {
        const bool ready = payload(error);
        emit(args.command == L"info" ? "info" : "check", "\"architecture\":\"x64\",\"plugin_type\":\"hybrid\",\"targets\":[\"process\",\"tab\"],\"presentation\":\"standalone_with_control_log_tab\",\"engine\":\"ksword\",\"native_without_driver\":true,\"hvm_switch\":true,\"payload_ready\":" +
            std::string(ready ? "true" : "false") + ",\"message\":\"" + json(error) + "\"");
        return args.command == L"info" || ready ? 0 : 2;
    }
    DWORD pid = 0;
    if (!launch(args.pid, pid, error)) { emit("error", "\"code\":\"launch_failed\",\"message\":\"" + json(error) + "\""); return 2; }
    emit("launch_complete", "\"debugger_pid\":" + std::to_string(pid) + ",\"target_pid\":" + std::to_string(args.pid));
    closeHandle(gDebugger); return 0;
}
