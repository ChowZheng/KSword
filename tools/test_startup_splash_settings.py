"""在隔离exe目录调用实际AppearanceSettings读写；不创建GUI、不访问用户配置。"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "SettingsDock/AppearanceSettings.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <cstdlib>

// 测试只写由runner创建的exe同级Style目录；路径不符时在首次保存前退出。
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString expected = QDir::cleanPath(QCoreApplication::applicationDirPath()
        + QStringLiteral("/Style/appearance_settings.json"));
    const QString actual = QDir::cleanPath(ks::settings::resolveSettingsJsonPathForWrite());
    if (actual.compare(expected, Qt::CaseInsensitive) != 0) return 90;
    int checks = 0;
    auto check = [&](bool ok) { ++checks; if (!ok) { std::cerr << "failed=" << checks << '\n'; std::exit(1); } };
    auto writeJson = [&](const QJsonObject& object) {
        QFile file(expected);
        check(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray bytes = QJsonDocument(object).toJson();
        check(file.write(bytes) == bytes.size());
        file.close();
    };

    // 六种选择组合走生产保存/读取路径，同时确认其它常用字段没有丢失。
    for (int layout = 0; layout < 3; ++layout)
    {
        for (int background = 0; background < 2; ++background)
        {
            ks::settings::AppearanceSettings input;
            input.startupSplashLayout = static_cast<SplashLayoutStyle>(layout);
            input.startupSplashBackground = static_cast<SplashBackgroundStyle>(background);
            input.backgroundOpacityPercent = 17;
            input.startupWindowScaleFactor = 1.25;
            QString error;
            check(ks::settings::saveAppearanceSettings(input, &error));
            check(QDir::cleanPath(ks::settings::resolveSettingsJsonPathForRead())
                .compare(expected, Qt::CaseInsensitive) == 0);
            const auto loaded = ks::settings::loadAppearanceSettings();
            check(static_cast<int>(loaded.startupSplashLayout) == layout);
            check(static_cast<int>(loaded.startupSplashBackground) == background);
            check(loaded.backgroundOpacityPercent == 17);
            check(loaded.startupWindowScaleFactor == 1.25);
        }
    }

    // 旧配置缺项、错误类型与越界值都回到兼容默认，不选择任意资源或崩溃。
    for (const QJsonObject& object : {
        QJsonObject{},
        QJsonObject{{QStringLiteral("startup_splash_layout"), 99},
                    {QStringLiteral("startup_splash_background"), -7}},
        QJsonObject{{QStringLiteral("startup_splash_layout"), QStringLiteral("bad")},
                    {QStringLiteral("startup_splash_background"), QStringLiteral("bad")}}
    })
    {
        writeJson(object);
        const auto loaded = ks::settings::loadAppearanceSettings();
        check(loaded.startupSplashLayout == SplashLayoutStyle::VerticalCompact);
        check(loaded.startupSplashBackground == SplashBackgroundStyle::Code);
    }
    ks::settings::AppearanceSettings invalid;
    invalid.startupSplashLayout = static_cast<SplashLayoutStyle>(99);
    invalid.startupSplashBackground = static_cast<SplashBackgroundStyle>(99);
    check(ks::settings::saveAppearanceSettings(invalid));
    const auto normalized = ks::settings::loadAppearanceSettings();
    check(normalized.startupSplashLayout == SplashLayoutStyle::VerticalCompact);
    check(normalized.startupSplashBackground == SplashBackgroundStyle::Code);
    std::cout << "SPLASH_SETTINGS_CHECKS=" << checks << " FAILURES=0\n";
}
'''


def main() -> int:
    """用标准x64 MSVC及项目Qt编译真实生产配置模块，临时输出限定在仓库日志目录。"""
    qt = ROOT / '.deps/Qt/6.9.3/msvc2022_64'
    if not qt.is_dir():
        qt = Path('D:/Software/Qt/6.9.3/msvc2022_64')
    vcvars = Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat')
    if not vcvars.is_file():
        vcvars = Path('D:/Software/VS/VC/Auxiliary/Build/vcvars64.bat')
    if not vcvars.is_file() or not qt.is_dir():
        raise RuntimeError('Configured MSVC x64/Qt paths are unavailable')
    base = ROOT / '.codex-build-logs/startup-splash-settings'
    base.mkdir(parents=True, exist_ok=True)
    work = base / ('case-' + uuid.uuid4().hex)
    work.mkdir()
    if not work.resolve().is_relative_to(base.resolve()) or not base.resolve().is_relative_to(ROOT.resolve()):
        raise RuntimeError('Test output escaped the repository')
    (work / 'Style').mkdir()
    source, binary = work / 'settings.cpp', work / 'settings.exe'
    source.write_text(HARNESS, encoding='utf-8')
    args = ['cl', '/nologo', '/std:c++20', '/Zc:__cplusplus', '/permissive-', '/utf-8', '/EHsc', '/MD', '/W4', '/WX', '/external:W0',
            '/external:I' + str(qt / 'include'), '/external:I' + str(qt / 'include/QtCore'),
            '/external:I' + str(qt / 'include/QtGui'), '/I' + str(ROOT / 'Ksword5.1/Ksword5.1'),
            str(source), str(ROOT / 'Ksword5.1/Ksword5.1/SettingsDock/AppearanceSettings.cpp'),
            '/Fe' + str(binary), '/Fo' + str(work) + os.sep, '/link',
            '/LIBPATH:' + str(qt / 'lib'), 'Qt6Core.lib', 'Qt6Gui.lib']
    batch = work / 'build.cmd'
    batch.write_text('@echo off\ncall "' + str(vcvars) + '" >nul\nif errorlevel 1 exit /b %errorlevel%\n'
                     + subprocess.list2cmdline(args) + '\nexit /b %errorlevel%\n', encoding='utf-8')
    build = subprocess.run(['cmd', '/d', '/c', str(batch)], cwd=work, capture_output=True, text=True,
                           errors='replace', timeout=180, creationflags=subprocess.CREATE_NO_WINDOW)
    (work / 'build.log').write_text(build.stdout + build.stderr, encoding='utf-8')
    if build.returncode:
        print((build.stdout + build.stderr)[-12000:])
        return build.returncode
    env = dict(os.environ)
    env['PATH'] = str(qt / 'bin') + os.pathsep + env.get('PATH', '')
    result = subprocess.run([str(binary)], cwd=work, env=env, capture_output=True, text=True,
                            timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
    print(result.stdout, end='')
    if result.returncode:
        print(result.stderr)
    print('TEST_OUTPUT=' + str(work))
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
