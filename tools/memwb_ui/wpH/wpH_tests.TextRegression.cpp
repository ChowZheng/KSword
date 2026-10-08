#include "wpH_common.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/MemoryRowCanvas.h"
#include <QCoreApplication>
#include <QLabel>
#include <QScrollBar>
#include <QtTest/QtTest>

namespace wpH_test
{
    void RunTextRegressionTests()
    {
        using View = ks::ui::WorkbenchTextView;
        FakeBytesProvider provider;
        std::vector<std::uint8_t> bytes(4096, 'A');
        bytes[4094] = 0xE4; bytes[4095] = 0xB8;
        bytes.insert(bytes.end(), {0xAD, 'B'});
        provider.overlay().LoadBaseline("text-regression", 0x1000, bytes, std::vector<std::uint8_t>(bytes.size(), 1));
        View view;
        view.setEncoding(View::Encoding::Utf8);
        view.setBytesProvider(&provider);
        view.setWindow(0x1000, 4096);
        WPH_CHECK(view.canvas()->rows().back().tokens.back().text == QStringLiteral("中"));
        WPH_CHECK(view.canvas()->rows().back().tokens.back().length == 3);
        view.resize(720, 300); view.show(); QCoreApplication::processEvents();
        view.canvas()->verticalScrollBar()->setValue(20);
        view.canvas()->selectRange(0x1120, 0x1120, false);
        const int scroll = view.canvas()->verticalScrollBar()->value();
        view.refreshView();
        WPH_CHECK(view.canvas()->verticalScrollBar()->value() == scroll);
        WPH_CHECK(view.selectedByteRange()->first == 0x1120);
        int requests = 0;
        std::uint64_t next = 0;
        QObject::connect(&view, &View::windowRequested, &view, [&](quint64 address, quint64) { ++requests; next = address; });
        view.canvas()->requestMore(1, 3);
        WPH_CHECK(requests == 1 && next > 0x1000);
        view.canvas()->requestMore(1, 3);
        WPH_CHECK(requests == 2); // a synchronously available window can browse again
        view.setWindow(next, 1024);
        view.canvas()->requestMore(1, 3);
        WPH_CHECK(requests == 3);
        view.setBytesPerRow(16);
        view.setWindow(0x1004, 5);
        WPH_CHECK(view.renderedText() == QStringLiteral("AAAAA"));
        view.setWindow(0x100F, 1);
        WPH_CHECK(view.renderedText() == QStringLiteral("A"));
        const auto unchanged = provider.overlay().Materialize(0x1000, bytes.size());
        view.setEncoding(View::Encoding::Gbk);
        WPH_CHECK(provider.overlay().Materialize(0x1000, bytes.size()).bytes == unchanged.bytes);
        view.setEncoding(static_cast<View::Encoding>(999));
        WPH_CHECK(view.encoding() == View::Encoding::Gbk);
        view.setBytesPerRow(0);
        WPH_CHECK(view.bytesPerRow() == 16);
    }
}
