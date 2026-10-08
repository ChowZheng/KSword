#include "wpH_common.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/MemoryRowCanvas.h"
#include <QCoreApplication>
#include <QLineEdit>
#include <QtTest/QtTest>

namespace wpH_test
{
    void RunTextTests(const QString& shotsDir)
    {
        using View = ks::ui::WorkbenchTextView;
        FakeBytesProvider provider;
        std::vector<std::uint8_t> bytes(15, 'A');
        bytes.insert(bytes.end(), {0xE4, 0xB8, 0xAD, 0xF0, 0x9F, 0x98, 0x80, 'B'});
        provider.overlay().LoadBaseline("text", 0x2000, bytes, std::vector<std::uint8_t>(bytes.size(), 1));
        View view;
        view.setBytesProvider(&provider);
        view.setEncoding(View::Encoding::Utf8);
        view.setWindow(0x2000, bytes.size());
        WPH_CHECK(view.canvas()->rows().size() == 2);
        WPH_CHECK(view.canvas()->rows().front().bytes.size() == 18);
        const auto token = view.canvas()->rows().front().tokens.back();
        WPH_CHECK(token.text == QStringLiteral("中"));
        WPH_CHECK(token.address == 0x200F && token.length == 3);
        WPH_CHECK(view.canvas()->rows()[1].tokens.front().text == QString::fromUtf8("😀"));
        WPH_CHECK(view.canvas()->rows()[1].tokens.front().length == 4);
        view.canvas()->selectRange(token.address, token.address + token.length - 1);
        const auto range = view.selectedByteRange();
        WPH_CHECK(range.has_value() && range->first == 0x200F && range->second == 0x2011);
        bool complete = false;
        WPH_CHECK(view.canvas()->selectedBytes(&complete) == QByteArray::fromHex("E4B8AD"));
        WPH_CHECK(complete);
        const QString original = view.renderedText();
        view.setBytesVisible(false);
        WPH_CHECK(!view.bytesVisible() && view.renderedText() == original);
        view.setWrapText(true);
        WPH_CHECK(view.wrapText());
        auto* search = view.findChild<QLineEdit*>(QStringLiteral("ksMemwbTextFind"));
        search->setText(QStringLiteral("中"));
        QTest::keyClick(search, Qt::Key_Return);
        WPH_CHECK(view.selectedByteRange()->first == 0x200F && view.selectedByteRange()->second == 0x2011);
        view.setBytesPerRow(8);
        WPH_CHECK(view.renderedText().contains(QStringLiteral("中")));
        WPH_CHECK(view.renderedText().contains(QString::fromUtf8("😀")));
        if (!shotsDir.isEmpty())
        {
            view.resize(760, 320); view.show(); QCoreApplication::processEvents();
            GrabWidget(&view).save(shotsDir + QStringLiteral("/wpH_text_light.png"));
            ApplyTheme(true); QCoreApplication::processEvents();
            GrabWidget(&view).save(shotsDir + QStringLiteral("/wpH_text_dark.png"));
            ApplyTheme(false);
        }
        view.reset();
        WPH_CHECK(view.renderedText().isEmpty() && view.canvas()->rows().isEmpty());
        view.setBytesProvider(nullptr);
        WPH_CHECK(view.canvas()->rows().isEmpty());
    }
}
