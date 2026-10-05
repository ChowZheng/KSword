// wpH_common.cpp
// 作用：wpH_common.h 的实现——断言框架、主题切换、FakeBytesProvider 与真实解码/汇编后端。

#include "wpH_common.h"

#include "../../../Ksword5.1/Ksword5.1/theme.h"
#include "../../../Ksword5.1/Ksword5.1/UI/KernelDisassemblyDialog.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryAssembly.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

#include <iostream>

#if __has_include(<Zydis.h>)
#include <Zydis.h>
#define WPH_HAS_ZYDIS 1
#else
#define WPH_HAS_ZYDIS 0
#endif

namespace wpH_test
{
    int g_checks = 0;
    int g_failures = 0;

    // Report：记一条断言；失败打印位置、表达式与备注，方便人工核对。
    void Report(bool ok, const char* expression, const char* file, int line, const QString& note)
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

    // ApplyTheme：与其它 memwb_ui 夹具同一套惯例——完整调色板，避免默认浅色 palette 制造
    // 错误的深色截图（含 disabled/placeholder 等角色）。
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

    // GrabWidget：整控件抓图并转 ARGB32，便于逐像素/采样检查。
    QImage GrabWidget(QWidget* widget)
    {
        if (widget == nullptr)
        {
            return QImage();
        }
        return widget->grab().toImage().convertToFormat(QImage::Format_ARGB32);
    }

    // ---------------- FakeBytesProvider ----------------

    FakeBytesProvider::FakeBytesProvider(const int addressBits) : m_addressBits(addressBits)
    {
    }

    // FetchWindow：把真实 MemoryDiffOverlay 的三套读数（现值/基线/上次读取）与变化种类
    // 逐字节翻译成 WorkbenchByteWindow；不做任何缓存或额外判断，如实转发。
    ks::ui::WorkbenchByteWindow FakeBytesProvider::FetchWindow(const std::uint64_t address, const std::uint64_t length) const
    {
        ks::ui::WorkbenchByteWindow window;
        const auto materialized = m_overlay.Materialize(address, length);
        if (!materialized.ok)
        {
            return window;
        }
        window.ok = true;
        window.address = address;
        window.bytes = materialized.bytes;
        window.validMask = materialized.validMask;
        window.baselineBytes.resize(static_cast<std::size_t>(length));
        window.baselineValidMask.resize(static_cast<std::size_t>(length));
        window.previousBytes.resize(static_cast<std::size_t>(length));
        window.previousValidMask.resize(static_cast<std::size_t>(length));
        window.changeKinds.resize(static_cast<std::size_t>(length));
        for (std::uint64_t i = 0; i < length; ++i)
        {
            const std::uint64_t byteAddress = address + i;
            const auto baseline = m_overlay.BaselineByte(byteAddress);
            window.baselineBytes[static_cast<std::size_t>(i)] = baseline.value_or(0);
            window.baselineValidMask[static_cast<std::size_t>(i)] = baseline.has_value() ? 1 : 0;
            const auto previous = m_overlay.PreviousByte(byteAddress);
            window.previousBytes[static_cast<std::size_t>(i)] = previous.value_or(0);
            window.previousValidMask[static_cast<std::size_t>(i)] = previous.has_value() ? 1 : 0;
            window.changeKinds[static_cast<std::size_t>(i)] = m_overlay.ChangeKind(byteAddress);
        }
        return window;
    }

    int FakeBytesProvider::AddressBits() const
    {
        return m_addressBits;
    }

    bool FakeBytesProvider::HasPreviousRead() const
    {
        return m_overlay.HasPreviousRead();
    }

    ksword::memwb::MemoryDiffOverlay& FakeBytesProvider::overlay()
    {
        return m_overlay;
    }

    void FakeBytesProvider::setAddressBits(const int bits)
    {
        m_addressBits = bits;
    }

    // ---------------- 真实后端 ----------------

    // MakeRealZydisDecodeBackend：直接调用 Zydis 解码单条指令，文本切分规则（第一个空格前为
    // 助记符）与生产端 KernelDisassemblyDialog.cpp 的 tryZydis 完全一致，保证夹具验证的"重同步
    // 算法"吃到的是与生产同形状的数据。
    ks::ui::DecodeOneFn MakeRealZydisDecodeBackend()
    {
#if WPH_HAS_ZYDIS
        return [](const std::uint8_t* bytes, const std::size_t available, const std::uint64_t address, const bool x64)
            -> std::optional<ks::ui::DecodedRow> {
            if (available == 0)
            {
                return std::nullopt;
            }
            ZydisDisassembledInstruction instruction{};
            const ZydisMachineMode mode = x64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32;
            const ZyanStatus status = ZydisDisassembleIntel(
                mode, address, bytes, static_cast<ZyanUSize>(available), &instruction);
            if (!ZYAN_SUCCESS(status) || instruction.info.length == 0U)
            {
                return std::nullopt;
            }
            const std::uint32_t length = instruction.info.length;
            if (length > available)
            {
                return std::nullopt;
            }
            const QString text = QString::fromLatin1(instruction.text).trimmed();
            const qsizetype separator = text.indexOf(QChar(' '));
            ks::ui::DecodedRow row;
            row.address = address;
            row.bytes = QByteArray(reinterpret_cast<const char*>(bytes), static_cast<qsizetype>(length));
            if (separator < 0)
            {
                row.mnemonic = text;
            }
            else
            {
                row.mnemonic = text.left(separator);
                row.operands = text.mid(separator + 1).trimmed();
            }
            row.decoded = true;
            return row;
        };
#else
        return ks::ui::DecodeOneFn();
#endif
    }

    // MakeRealAssembleBackend：包装生产端 InstructionAssembler::assemble（真实 Zydis 编码器 +
    // 语言包错误文案），不是简化替身。
    ks::ui::AssembleOneFn MakeRealAssembleBackend()
    {
        return [](const QString& source, const std::uint64_t address, const bool x64) -> ks::ui::WorkbenchAssembleResult {
            const auto architecture = x64 ? ks::ui::DisassemblyArchitecture::X64 : ks::ui::DisassemblyArchitecture::X86;
            const ks::ui::AssemblyResult result = ks::ui::InstructionAssembler::assemble(source, address, architecture);
            ks::ui::WorkbenchAssembleResult out;
            out.success = result.success;
            out.bytes = result.bytes;
            out.error = result.error;
            out.errorLine = result.errorLine; // D6：透传真实出错行号，不再让调用方猜成"第 1 行"。
            return out;
        };
    }
}
