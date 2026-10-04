"""Exercise the production FileDock menu and Windows run-as backend.

Qt offscreen checks use the actual menu construction/filter, substituting only
selection, Oplock/plugin state and permission availability. The token probe
runs the real backend against harmless copies of this harness. SYSTEM and
TrustedInstaller launches are opt-in (the latter can start its Windows service).
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-dir', type=Path, default=Path('D:/Software/Qt/6.9.3/msvc2022_64'))
    parser.add_argument('--vcvars', type=Path, default=Path('D:/Software/VS/VC/Auxiliary/Build/vcvars64.bat'))
    parser.add_argument('--privileged', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'Ksword5.1/Ksword5.1/FileDock/FileDock.cpp').read_text(encoding='utf-8-sig')
    helper = between(source, '    bool hideUnavailableFileMenuActions(', '\n}\n\nvoid FileDock::showPanelContextMenu')
    presets = between(source, '    struct FileIntegrityLevelPreset', '    // isSupportedFileMandatoryIntegrityRid')
    menu = between(source[source.index('void FileDock::showPanelContextMenu'):],
                   '    QMenu menu(this);', '    QAction* selectedAction = menu.exec(')
    harness = r'''
#include <QtWidgets>
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <vector>
#include "process_run_as.h"
void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << std::endl; std::exit(1); }
}
namespace ks::i18n { QString displayText(const QString& value) { return value; } }
namespace ks::plugin_host {
enum class TargetKind { File };
struct InvocationContext { TargetKind targetKind; QString filePath; };
bool plugins = false;
void populateTargetMenu(QMenu* menu, QWidget*, const InvocationContext&) {
    menu->addAction("plugin")->setEnabled(plugins);
}
}
namespace ks::process {
RunAsAvailability availability;
RunAsAvailability FakeQueryRunAsAvailability() { return availability; }
}
#ifndef SECURITY_MANDATORY_MEDIUM_PLUS_RID
#define SECURITY_MANDATORY_MEDIUM_PLUS_RID 0x2100
#endif
''' + presets + helper + r'''
class MenuHarness : public QWidget {
public:
    bool oplock = false;
    bool hasActiveOplockForPath(const QString&) { return oplock; }
    std::uint64_t activeOplockBreakCountForPath(const QString&) { return 7; }
    std::size_t activeOplockAccessProcessCountForPath(const QString&) { return 2; }
    std::size_t activeOplockCount() { return oplock ? 1 : 0; }
    QString buildContextMenuStyle() { return {}; }
    void check(const std::vector<QString>& menuPaths, bool expectExe) {
        const bool hasSelection = !menuPaths.empty();
        const bool isSingleSelection = menuPaths.size() == 1;
        const QString firstPath = isSingleSelection ? menuPaths.front() : QString();
        bool hasAnyFile = false;
        for (const auto& path : menuPaths) hasAnyFile |= QFileInfo(path).isFile();
        QStringList linkTargetList;
        QString firstLinkTarget, firstFileIntegrityDetailText;
        bool firstFileIntegrityKnown = false, firstFileIntegrityImplicitMedium = false;
        DWORD firstFileIntegrityRid = 0;
        QString copyToPanelText = "copy", moveToPanelText = "move";
#define QueryRunAsAvailability FakeQueryRunAsAvailability
''' + menu + r'''
#undef QueryRunAsAvailability
        require(newFileAction->isEnabled() && newFolderAction->isEnabled() &&
            openTerminalAction->isEnabled() && columnAction->isEnabled(), "background actions remain available");
        menu.show();
        QApplication::processEvents();
        require(runAsMenu->menuAction()->isVisible() == expectExe, "run-as visibility");
        if (expectExe) {
            require(runAsSystemAction->isVisible() == ks::process::availability.system, "SYSTEM availability");
            require(runAsTrustedInstallerAction->isVisible() == ks::process::availability.trustedInstaller, "TI availability");
            require(runAsAdministratorAction->isVisible() == ks::process::availability.administrator, "admin availability");
            require(runAsStandardAction->isVisible() == ks::process::availability.standardUser, "standard availability");
        }
        const auto all = menu.actions();
        require(all.indexOf(unlockByDriverAction) == all.indexOf(mappedProcessScanAction) + 1,
            "mapping scan and unlocker must be adjacent");
        if (hasSelection) require(all.back() == detailAction && detailAction->isVisible(), "properties must be last");
        require(pluginMenu->menuAction()->isVisible() == (ks::plugin_host::plugins && singleFileOnly), "empty plugin menu");
        require(showOplockRecordsAction->isVisible() == (singleFileOnly && oplock), "Oplock record visibility");
        require(!copyLinkTargetAction->isVisible() && !openLinkTargetAction->isVisible(), "non-link actions hidden");
        std::function<void(QMenu*)> verify = [&](QMenu* current) {
            bool separator = true;
            for (auto* action : current->actions()) {
                if (!action->isVisible() || current->actionGeometry(action).isEmpty()) continue;
                require(action->isEnabled(), "disabled visible action");
                if (action->isSeparator()) { require(!separator, "leading or duplicate separator"); separator = true; }
                else { separator = false; if (action->menu()) verify(action->menu()); }
            }
            require(!separator, "empty menu or trailing separator");
        };
        verify(&menu);
        menu.hide();
    }
};

std::string tokenSnapshot(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    require(GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE, "TokenUser");
    const bool system = IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, WinLocalSystemSid) != FALSE;
    TOKEN_ELEVATION elevated{};
    require(GetTokenInformation(token, TokenElevation, &elevated, sizeof(elevated), &size) != FALSE, "TokenElevation");
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<BYTE> label(size);
    require(GetTokenInformation(token, TokenIntegrityLevel, label.data(), size, &size) != FALSE, "TokenIntegrityLevel");
    PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
    DWORD rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
    DWORD session = 0;
    require(GetTokenInformation(token, TokenSessionId, &session, sizeof(session), &size) != FALSE, "TokenSessionId");
    DWORD sidLength = 0, domainLength = 0; SID_NAME_USE use{};
    LookupAccountNameW(nullptr, L"NT SERVICE\\TrustedInstaller", nullptr, &sidLength, nullptr, &domainLength, &use);
    std::vector<BYTE> ti(sidLength); std::vector<wchar_t> domain(domainLength);
    bool trustedInstaller = false;
    if (sidLength && LookupAccountNameW(nullptr, L"NT SERVICE\\TrustedInstaller", ti.data(), &sidLength,
        domain.data(), &domainLength, &use)) {
        GetTokenInformation(token, TokenGroups, nullptr, 0, &size);
        std::vector<BYTE> data(size);
        require(GetTokenInformation(token, TokenGroups, data.data(), size, &size) != FALSE, "TokenGroups");
        auto* groups = reinterpret_cast<TOKEN_GROUPS*>(data.data());
        for (DWORD i = 0; i < groups->GroupCount; ++i)
            if ((groups->Groups[i].Attributes & SE_GROUP_ENABLED) && EqualSid(groups->Groups[i].Sid, ti.data())) trustedInstaller = true;
    }
    return std::to_string(system) + " " + std::to_string(elevated.TokenIsElevated) + " " +
        std::to_string(rid) + " " + std::to_string(session) + " " + std::to_string(trustedInstaller);
}

int main(int argc, char** argv) {
    // Probe bypasses Qt; its only side effect is one output file in the test directory.
    if (argc >= 3 && std::string(argv[1]) == "--probe") {
        HANDLE token = nullptr;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE, "probe token");
        std::ofstream file(argv[2]); file << tokenSnapshot(token); CloseHandle(token);
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--nested-standard") {
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        const auto output = QString::fromLocal8Bit(argv[2]).toStdWString();
        const auto result = ks::process::RunExecutableAs(self, ks::process::RunAsIdentity::StandardUser,
            L"--probe \"" + output + L"\"");
        require(result.success, "launch from already unelevated caller");
        HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, result.processId);
        if (child) { require(WaitForSingleObject(child, 10000) == WAIT_OBJECT_0, "nested probe timeout"); CloseHandle(child); }
        return 0;
    }
    QApplication app(argc, argv);
    QString work = QCoreApplication::applicationDirPath();
    QFile text(work + "/plain.txt"); require(text.open(QIODevice::WriteOnly), "create txt"); text.close();
    MenuHarness menu;
    menu.check({}, false);
    menu.check({work}, false);
    menu.check({text.fileName()}, false);
    menu.check({QCoreApplication::applicationFilePath(), text.fileName()}, false);
    ks::process::availability = {true, true, true, true};
    menu.check({QCoreApplication::applicationFilePath()}, true);
    ks::process::availability = {false, false, true, false};
    menu.check({QCoreApplication::applicationFilePath()}, true);
    menu.oplock = true; ks::plugin_host::plugins = true;
    menu.check({text.fileName()}, false);

    using ks::process::RunAsIdentity;
    using ks::process::RunExecutableAs;
    const auto image = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    require(!RunExecutableAs(L"", RunAsIdentity::StandardUser).success, "empty path rejected");
    require(!RunExecutableAs(text.fileName().toStdWString(), RunAsIdentity::StandardUser).success, "non-exe rejected");
    require(!RunExecutableAs(L"C:\\nonexistent\\missing.exe", RunAsIdentity::System).success, "missing exe rejected");
    require(!RunExecutableAs(image, static_cast<RunAsIdentity>(99)).success, "invalid identity rejected");
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token) != FALSE, "caller token");
    const auto before = tokenSnapshot(token);
    auto probe = [&](RunAsIdentity identity, bool expectSystem, bool expectTi, bool expectAdmin = false, bool nested = false) {
        QFile::remove(work + "/probe.txt");
        const QString output = QDir::toNativeSeparators(work + "/probe.txt");
        auto result = RunExecutableAs(image, identity, (nested ? L"--nested-standard \"" : L"--probe \"") + output.toStdWString() + L"\"");
        if (!result.success) std::wcerr << result.detail << L" error=" << result.error << std::endl;
        require(result.success, "token launch failed");
        HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, result.processId);
        if (child) { require(WaitForSingleObject(child, 10000) == WAIT_OBJECT_0, "probe timeout"); CloseHandle(child); }
        QFile file(output); require(file.open(QIODevice::ReadOnly), "probe output missing");
        const auto fields = file.readAll().split(' ');
        require(fields.size() == 5, "probe output format");
        require(fields[0].toInt() == int(expectSystem), "wrong user SID");
        require(fields[4].toInt() == int(expectTi), "wrong TI SID");
        if (!expectSystem && !expectAdmin) require(fields[1].toInt() == 0 && fields[2].toInt() <= SECURITY_MANDATORY_MEDIUM_RID,
            "standard child must be unelevated medium or lower");
        if (expectAdmin) require(fields[1].toInt() != 0 && fields[2].toInt() >= SECURITY_MANDATORY_HIGH_RID,
            "administrator child must be elevated");
        DWORD session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &session);
        require(fields[3].toUInt() == session, "wrong child session");
        require(tokenSnapshot(token) == before, "caller process identity changed");
        HANDLE threadToken = nullptr;
        require(!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &threadToken) && GetLastError() == ERROR_NO_TOKEN,
            "temporary thread token leaked");
        std::cout << "PASS identity=" << int(identity) << " token=" << fields.join(' ').constData() << std::endl;
    };
    probe(RunAsIdentity::StandardUser, false, false);
    probe(RunAsIdentity::StandardUser, false, false, false, true);
    const auto available = ks::process::QueryRunAsAvailability();
    if (!available.system) {
        require(!available.trustedInstaller, "TI unavailable without elevation");
        for (auto identity : {RunAsIdentity::System, RunAsIdentity::TrustedInstaller}) {
            const auto denied = RunExecutableAs(image, identity);
            require(!denied.success && denied.error == ERROR_ELEVATION_REQUIRED, "privileged launch rejects unelevated caller");
        }
    }
    // Existing caller impersonation must survive a launch and rejected input.
    HANDLE impersonation = nullptr;
    require(DuplicateTokenEx(token, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr,
        SecurityImpersonation, TokenImpersonation, &impersonation) != FALSE, "duplicate test thread token");
    require(SetThreadToken(nullptr, impersonation) != FALSE, "install test thread token");
    const auto threadBefore = tokenSnapshot(impersonation);
    const auto restoredLaunch = RunExecutableAs(image, RunAsIdentity::StandardUser, L"--probe \"" +
        QDir::toNativeSeparators(work + "/restored.txt").toStdWString() + L"\"");
    require(restoredLaunch.success, "launch with existing impersonation");
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, restoredLaunch.processId);
    if (child) { WaitForSingleObject(child, 10000); CloseHandle(child); }
    require(!RunExecutableAs(image, static_cast<RunAsIdentity>(99)).success, "reject invalid identity while impersonating");
    HANDLE restored = nullptr;
    require(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &restored) != FALSE,
        "existing impersonation lost");
    require(tokenSnapshot(restored) == threadBefore, "wrong restored thread identity");
    CloseHandle(restored); SetThreadToken(nullptr, nullptr); CloseHandle(impersonation);
    if (argc >= 2 && std::string(argv[1]) == "--privileged" && available.system) {
        probe(RunAsIdentity::Administrator, false, false, true);
        probe(RunAsIdentity::System, true, false);
        probe(RunAsIdentity::TrustedInstaller, true, true);
    }
    else std::cout << "SKIP elevated admin / SYSTEM / TrustedInstaller live probes" << std::endl;
    CloseHandle(token);
    std::cout << "PASS menu cases and identity restoration" << std::endl;
}
'''
    with tempfile.TemporaryDirectory(prefix='ksword-menu-test-') as temp:
        out = Path(temp)
        cpp = out / 'test.cpp'
        cpp.write_text(harness, encoding='utf-8')
        qt = args.qt_dir.resolve()
        process = root / 'Ksword5.1/Ksword5.1/ksword/process'
        build = out / 'build.cmd'
        build.write_text(
            f'@echo off\ncall "{args.vcvars.resolve()}" >nul\nif errorlevel 1 exit /b %errorlevel%\n'
            'cl /nologo /std:c++17 /permissive- /Zc:__cplusplus /utf-8 /EHsc /MD /W4 /WX /O2 /DNOMINMAX /DUNICODE /D_UNICODE '
            f'/I"{process}" /external:W0 '
            + ' '.join(f'/external:I"{qt / folder}"' for folder in ('include', 'include/QtCore', 'include/QtGui', 'include/QtWidgets'))
            + f' "{cpp}" "{process / "process_run_as.cpp"}" /Fe"{out / "test.exe"}" /link '
            f'/LIBPATH:"{qt / "lib"}" Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib user32.lib\nexit /b %errorlevel%\n',
            encoding='utf-8')
        subprocess.run(['cmd.exe', '/d', '/c', str(build)], check=True, cwd=out)
        environment = dict(os.environ, QT_QPA_PLATFORM='offscreen')
        environment['PATH'] = os.pathsep.join([str(qt / 'bin'), str(root / 'Ksword5.1/x64/Release'), environment['PATH']])
        platforms = out / 'platforms'
        platforms.mkdir()
        shutil.copy2(qt / 'plugins/platforms/qoffscreen.dll', platforms)
        # Make the unelevated probe self-contained; do not rely on the admin caller's PATH.
        for name in ('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll'):
            shutil.copy2(qt / 'bin' / name, out)
        for dll in (root / 'Ksword5.1/x64/Release').glob('*140*.dll'):
            shutil.copy2(dll, out)
        subprocess.run([str(out / 'test.exe')] + (['--privileged'] if args.privileged else []),
                       check=True, env=environment, timeout=90)


if __name__ == '__main__':
    main()
