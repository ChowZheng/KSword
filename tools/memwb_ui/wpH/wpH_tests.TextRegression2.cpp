#include "wpH_common.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/MemoryRowCanvas.h"

namespace wpH_test
{
    void RunTextRegressionTests2()
    {
        using View = ks::ui::WorkbenchTextView;
        using ks::ui::DecodeTextChunkForTest;
        FakeBytesProvider provider;
        std::vector<std::uint8_t> bytes{0xFE, 0xFF, 0, 0x41, 0xD8, 0x3D, 0xDE, 0};
        provider.overlay().LoadBaseline("bom", 0x3001, bytes, std::vector<std::uint8_t>(bytes.size(), 1));
        View view;
        view.setBytesProvider(&provider);
        view.setEncoding(View::Encoding::Auto);
        view.setWindow(0x3001, bytes.size());
        WPH_CHECK(view.hasBom() && view.effectiveEncoding() == View::Encoding::Utf16BE);
        WPH_CHECK(view.renderedText() == QStringLiteral("⟨BOM⟩A") + QString::fromUtf8("😀"));
        WPH_CHECK(view.canvas()->rows()[0].tokens[0].address == 0x3001 && view.canvas()->rows()[0].tokens[0].length == 2);
        view.setEncoding(View::Encoding::Utf8);
        WPH_CHECK(!view.hasBom() && view.effectiveEncoding() == View::Encoding::Utf8);
        WPH_CHECK(view.canvas()->rows().front().tokens[0].text == QString(QChar(0xFFFDU)));
        const QString invalid = DecodeTextChunkForTest({0xE4, 0x41}, {1, 1}, View::Encoding::Utf8);
        WPH_CHECK(invalid == QString(QChar(0xFFFDU)) + QLatin1Char('A'));
        WPH_CHECK(DecodeTextChunkForTest({0xE4, 0xB8}, {1, 1}, View::Encoding::Utf8) == QString(QChar(0x2026U)));
        WPH_CHECK(DecodeTextChunkForTest({0xE4, 0xB8, 0xAD}, {1, 0, 1}, View::Encoding::Utf8) == QString(QChar(0x00D7U)));
        WPH_CHECK(DecodeTextChunkForTest({0xE4, 0xB8, 0xAD}, {1, 2, 1}, View::Encoding::Utf8) == QString(QChar(0x00B7U)));
        WPH_CHECK(DecodeTextChunkForTest({0x41, 0, 0x42, 0}, {0, 1, 1, 1}, View::Encoding::Utf16LE)
            == QString(QChar(0x00D7U)) + QLatin1Char('B'));
        WPH_CHECK(DecodeTextChunkForTest({0x0B, 0x20}, {1, 1}, View::Encoding::Utf16LE) == QStringLiteral("⟨U+200B⟩"));
        WPH_CHECK(DecodeTextChunkForTest({0}, {1}, View::Encoding::Ascii) == QStringLiteral("\\0"));
        WPH_CHECK(DecodeTextChunkForTest({1, 2}, {1}, View::Encoding::Utf8).isEmpty());
        view.reset();
        WPH_CHECK(!view.hasBom() && view.renderedText().isEmpty());
    }
}
