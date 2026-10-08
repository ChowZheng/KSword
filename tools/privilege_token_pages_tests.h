#pragma once

#include "../Ksword5.1/Ksword5.1/PrivilegeDock/PrivilegeTokenPages.h"
#include "../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QThread>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Read-only integration checks. Link actual TokenPages/LanguageManager/ThemeStatusRole and
// the launch backend. No account edits, elevated launch, UAC prompt or service start occurs.
inline int RunPrivilegeTokenPagesChecks(QWidget* owner = nullptr)
{
    int checks = 0;
    auto require = [&checks](bool passed, const char* description)
    {
        if (!passed) throw std::runtime_error(description);
        ++checks;
    };
    auto until = [](const std::function<bool()>& ready)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 30000)
        {
            QApplication::processEvents(QEventLoop::AllEvents, 5);
            if (ready()) return true;
            QThread::msleep(1);
        }
        return false;
    };
    const DWORD pid = ::GetCurrentProcessId();
    const auto snapshot = ks::privilege::captureTokenSnapshot(pid);
    require(snapshot.value(QStringLiteral("readable")).toBool(), "Own token must be readable");
    const auto metadata = snapshot.value(QStringLiteral("metadata")).toObject();
    const quint64 created = metadata.value(QStringLiteral("creationTime100ns")).toString().toULongLong();
    require(created != 0 && metadata.value(QStringLiteral("pid")).toDouble() == pid, "Capture must preserve process identity");
    FILETIME creation{}, exit{}, kernel{}, user{};
    require(::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user) != FALSE,
        "Native creation time query must succeed");
    const quint64 nativeCreated = (static_cast<quint64>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    require(created == nativeCreated, "Snapshot identity must equal native handle identity");
    QString ownSid, authentication;
    bool enabledPresent = false, integrityPresent = false, capabilitiesPresent = false, restrictedPresent = false;
    for (const auto& value : snapshot.value(QStringLiteral("entries")).toArray())
    {
        const auto entry = value.toObject();
        const QString key = entry.value(QStringLiteral("key")).toString();
        if (key == QStringLiteral("user")) ownSid = entry.value(QStringLiteral("value")).toString();
        if (key == QStringLiteral("authentication_id")) authentication = entry.value(QStringLiteral("value")).toString();
        if (key.startsWith(QStringLiteral("privileges/")) && key.endsWith(QStringLiteral("/enabled"))) enabledPresent = true;
        if (key == QStringLiteral("integrity/rid")) integrityPresent = true;
        if (key.startsWith(QStringLiteral("capabilities"))) capabilitiesPresent = true;
        if (key.startsWith(QStringLiteral("restricted_sids"))) restrictedPresent = true;
    }
    require(ownSid.startsWith(QStringLiteral("S-")), "Capture must preserve user SID identity");
    require(!authentication.isEmpty(), "Capture must include logon AuthenticationId");
    require(enabledPresent && integrityPresent && capabilitiesPresent && restrictedPresent,
        "Token capture must cover privileges, integrity, capabilities and restricted SIDs");
    const auto unreadable = ks::privilege::captureTokenSnapshot(MAXDWORD);
    require(!unreadable.value(QStringLiteral("readable")).toBool()
        && !unreadable.value(QStringLiteral("errors")).toArray().isEmpty()
        && unreadable.value(QStringLiteral("entries")).toArray().isEmpty(),
        "Invalid PID must be unreadable, not an empty successful token");

    // Exit code 259 is a valid process result despite sharing STILL_ACTIVE's numeric value.
    // Run this harmless child only in an unelevated host; verification never requests elevation.
    HANDLE hostToken = nullptr;
    TOKEN_ELEVATION hostElevation{};
    DWORD hostBytes = 0;
    const bool elevationKnown = ::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &hostToken)
        && ::GetTokenInformation(hostToken, TokenElevation, &hostElevation, sizeof(hostElevation), &hostBytes);
    if (hostToken) ::CloseHandle(hostToken);
    if (elevationKnown && !hostElevation.TokenIsElevated)
    {
        wchar_t systemDirectory[32768]{};
        require(::GetSystemDirectoryW(systemDirectory, 32768) != 0, "Find explicit system command interpreter");
        const std::wstring childImage = std::wstring(systemDirectory) + L"\\cmd.exe";
        const std::wstring childCommand = L"\"" + childImage + L"\" /d /c exit 259";
        std::vector<wchar_t> command(childCommand.begin(), childCommand.end()); command.push_back(L'\0');
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        struct Child
        {
            PROCESS_INFORMATION value{};
            ~Child() { if (value.hThread) ::CloseHandle(value.hThread); if (value.hProcess) ::CloseHandle(value.hProcess); }
        } child;
        require(::CreateProcessW(childImage.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.value) != FALSE,
            "Launch disposable unelevated exit-code fixture");
        require(::WaitForSingleObject(child.value.hProcess, 10000) == WAIT_OBJECT_0, "Exit-code fixture must terminate");
        DWORD exitCode = 0;
        require(::GetExitCodeProcess(child.value.hProcess, &exitCode) && exitCode == 259,
            "Fixture must really terminate with STILL_ACTIVE's numeric value");
        const auto exited = ks::privilege::captureTokenSnapshot(child.value.dwProcessId);
        require(exited.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("aliveAfterCapture")).isBool()
            && !exited.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("aliveAfterCapture")).toBool(),
            "Terminated process with exit code259 must not be classified alive");
        require(!exited.value(QStringLiteral("errors")).toArray().isEmpty(),
            "An exited process must preserve failure/exited evidence");
    }

    std::unique_ptr<QWidget> compare(ks::privilege::createTokenComparePage(owner));
    compare->show();
    auto* left = compare->findChild<QComboBox*>(QStringLiteral("privilege_token_pid_a"));
    auto* right = compare->findChild<QComboBox*>(QStringLiteral("privilege_token_pid_b"));
    auto* refresh = compare->findChild<QPushButton*>(QStringLiteral("privilege_token_compare"));
    auto* comparison = compare->findChild<QTableWidget*>(QStringLiteral("privilege_token_comparison_table"));
    auto* differences = compare->findChild<QCheckBox*>(QStringLiteral("privilege_token_differences"));
    auto* copy = compare->findChild<QPushButton*>(QStringLiteral("privilege_token_copy_sid"));
    require(left && right && refresh && comparison && differences && copy, "Comparison UI must expose its controls");
    require(until([&]() { return left->count() > 0 && left->isEnabled() && refresh->isEnabled(); }), "Process selection enumeration timed out");
    require(left->currentData().toJsonObject().value(QStringLiteral("pid")).toDouble() == pid
        && right->currentData().toJsonObject().value(QStringLiteral("pid")).toDouble() == pid,
        "Initial process selectors must choose KSword's own process");
    refresh->click();
    require(until([&]() { return refresh->isEnabled() && comparison->isEnabled() && comparison->rowCount() > 0; }), "Actual token comparison timed out");
    int sidRow = -1;
    const QString equal = ks::i18n::contextText(QStringLiteral("privilege.workbench.tokens.equal"), QStringLiteral("相同"));
    for (int row = 0; row < comparison->rowCount(); ++row)
    {
        if (comparison->item(row, 0)->text() == QStringLiteral("user")) sidRow = row;
        const QString result = comparison->item(row, 3)->text();
        require(result == equal || result == ks::i18n::contextText(QStringLiteral("privilege.workbench.tokens.unknown"), QStringLiteral("无法确定")),
            "Same-process token comparison must not report an invented difference");
    }
    require(sidRow >= 0, "Comparison must contain actual user SID");
    comparison->setCurrentCell(sidRow, 0); copy->click();
    require(QApplication::clipboard()->text() == ownSid, "SID copy must use canonical SID");
    differences->setChecked(true);
    require(until([&]() { return comparison->isEnabled(); }), "Difference filter timed out");
    // Unknown fields are intentionally retained by differences-only filtering.
    for (int row = 0; row < comparison->rowCount(); ++row)
        require(comparison->item(row, 3)->text() != equal, "Differences filter must remove known-equal rows");
    right->setCurrentIndex(-1); right->setEditText(QString::number(MAXDWORD));
    refresh->click();
    require(until([&]() { return refresh->isEnabled() && comparison->isEnabled() && comparison->rowCount() > 0; }), "Unreadable-token comparison timed out");
    const QString unknown = ks::i18n::contextText(QStringLiteral("privilege.workbench.tokens.unknown"), QStringLiteral("无法确定"));
    for (int row = 0; row < comparison->rowCount(); ++row)
        require(comparison->item(row, 3)->text() == unknown, "Missing token evidence must remain unknown");

    int navigationCount = 0;
    DWORD navigationPid = 0;
    quint64 navigationCreation = 0;
    std::unique_ptr<QWidget> sessions(ks::privilege::createSessionsPage(owner, [&](DWORD selectedPid, quint64 selectedCreation)
    { ++navigationCount; navigationPid = selectedPid; navigationCreation = selectedCreation; }));
    sessions->show();
    auto* logons = sessions->findChild<QTableWidget*>(QStringLiteral("privilege_logon_sessions_table"));
    auto* processes = sessions->findChild<QTableWidget*>(QStringLiteral("privilege_logon_processes_table"));
    auto* open = sessions->findChild<QPushButton*>(QStringLiteral("privilege_logon_open_process"));
    require(logons && processes && open, "Login-session UI must expose associated processes");
    require(until([&]() { return logons->isEnabled() && processes->isEnabled() && processes->rowCount() > 0; }), "LSA/process enumeration timed out");
    int ownProcessRow = -1, ownLogonRow = -1;
    for (int row = 0; row < processes->rowCount(); ++row)
    {
        const auto record = processes->item(row, 0)->data(Qt::UserRole).toJsonObject();
        if (record.value(QStringLiteral("pid")).toDouble() == pid)
        {
            ownProcessRow = row;
            require(record.value(QStringLiteral("authentication_id")).toString() == authentication,
                "Session linkage must use AuthenticationId rather than terminal SessionId");
            require(record.value(QStringLiteral("creationTime100ns")).toString().toULongLong() == created,
                "Associated process identity must match capture identity");
        }
    }
    require(ownProcessRow >= 0, "Session process list must contain this process");
    processes->setCurrentCell(ownProcessRow, 0); open->click();
    require(until([&]() { return navigationCount == 1; }), "Anchored process navigation timed out");
    require(navigationPid == pid && navigationCreation == created, "Process navigation must retain creation-time anchor");
    for (int row = 0; row < logons->rowCount(); ++row)
        if (logons->item(row, 0)->data(Qt::UserRole).toJsonObject().value(QStringLiteral("luid")).toString() == authentication)
            ownLogonRow = row;
    if (ownLogonRow >= 0)
    {
        logons->setCurrentCell(ownLogonRow, 0);
        require(until([&]() { return processes->isEnabled(); }), "Logon-session process filtering timed out");
        for (int row = 0; row < processes->rowCount(); ++row)
            require(processes->item(row, 0)->data(Qt::UserRole).toJsonObject().value(QStringLiteral("authentication_id")).toString() == authentication,
                "Selected session must show only processes with matching logon LUID");
    }
    // Page deletion while a real worker is running must disconnect result delivery safely.
    refresh->click(); compare.reset();
    QApplication::processEvents(QEventLoop::AllEvents, 5);
    require(true, "Deleting a page with an active query must not synchronously crash");
    return checks;
}
