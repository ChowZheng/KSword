// wpE_common.cpp
// 作用：wpE_common.h 声明的实现。

#include "wpE_common.h"

#include "../../../Ksword5.1/Ksword5.1/theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QPalette>
#include <QStyleFactory>
#include <QTimer>

#include <iostream>

namespace wpe_test
{
    int g_checks = 0;
    int g_failures = 0;

    void Report(const bool ok, const char* expression, const char* file, const int line, const QString& note)
    {
        ++g_checks;
        if (ok)
        {
            return;
        }
        ++g_failures;
        std::cerr << "FAIL: " << expression << "  (" << file << ":" << line << ")";
        if (!note.isEmpty())
        {
            std::cerr << "  " << note.toStdString();
        }
        std::cerr << std::endl;
    }

    void ApplyTheme(const bool dark)
    {
        KswordTheme::SetDarkModeEnabled(dark);
        qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

        QPalette palette;
        palette.setColor(QPalette::Window, KswordTheme::WindowColor());
        palette.setColor(QPalette::WindowText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Base, KswordTheme::SurfaceColor());
        palette.setColor(QPalette::AlternateBase, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::Text, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Button, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::ButtonText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::PlaceholderText, KswordTheme::TextSecondaryColor());
        palette.setColor(QPalette::Highlight, KswordTheme::PrimaryAccentColor());
        palette.setColor(QPalette::HighlightedText, KswordTheme::OnAccentColor());
        palette.setColor(QPalette::Mid, KswordTheme::BorderColor());
        palette.setColor(QPalette::Midlight, KswordTheme::BorderStrongColor());
        palette.setColor(QPalette::Dark, KswordTheme::PaletteDarkColor());
        palette.setColor(QPalette::ToolTipBase, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::ToolTipText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Disabled, QPalette::Text, KswordTheme::TextDisabledColor());
        palette.setColor(QPalette::Disabled, QPalette::WindowText, KswordTheme::TextDisabledColor());
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, KswordTheme::TextDisabledColor());
        qApp->setPalette(palette);
    }

    QString ScratchFilePath(const QString& tag)
    {
        const QString dirPath = QStringLiteral(".codex-tmp/memwb-wpE/scratch");
        QDir().mkpath(dirPath);
        return QDir(dirPath).filePath(tag + QStringLiteral(".addrbook"));
    }

    ksword::memwb::AddressEntry MakeModuleEntry(
        const ksword::memwb::EntryKind kind,
        const std::string& targetKey,
        const std::string& moduleName,
        const std::uint64_t rva)
    {
        ksword::memwb::AddressEntry entry;
        entry.kind = kind;
        entry.targetKey = targetKey;
        entry.moduleName = moduleName;
        entry.rva = rva;
        return entry;
    }

    ksword::memwb::AddressEntry MakeAbsoluteEntry(
        const ksword::memwb::EntryKind kind,
        const std::string& targetKey,
        const std::uint64_t absoluteAddress)
    {
        ksword::memwb::AddressEntry entry;
        entry.kind = kind;
        entry.targetKey = targetKey;
        entry.absoluteAddress = absoluteAddress;
        return entry;
    }

    void WaitMs(const int milliseconds)
    {
        // 用一个单次定时器 + 局部事件循环驱动等待：AddressBookStore 的防抖定时器挂在
        // Qt 事件循环上，直接 Sleep 不会让它触发，必须真正 processEvents。
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    }
}
