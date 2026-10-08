#include "../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/MemoryRowCanvas.h"
#include "../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchDisasmView.h"
#include "../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTextView.h"
#include "../Ksword5.1/Ksword5.1/UI/MemorySnapshotBytesProvider.h"
#include "../Ksword5.1/Ksword5.1/UI/MemoryAssembly.h"
#include "../Ksword5.1/Ksword5.1/UI/KernelDisassemblyDialog.h"
#include "../Ksword5.1/Ksword5.1/UI/GlobalUiBaseStyle.h"
#include "../Ksword5.1/Ksword5.1/UI/SmoothScrollSupport.h"
#include "../Ksword5.1/Ksword5.1/theme.h"
#include "Zydis.h"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QLineEdit>
#include <QPointer>
#include <QScrollBar>
#include <QShortcut>
#include <QWheelEvent>
#include <QtTest/QtTest>
#include <iostream>
#include <cstdlib>

namespace
{
    int checks = 0;
    int failures = 0;
    class UnavailableBytes final : public ks::ui::IWorkbenchBytesProvider
    {
    public:
        std::uint8_t state = 2;
        ks::ui::WorkbenchByteWindow FetchWindow(std::uint64_t address, std::uint64_t length) const override
        {
            ks::ui::WorkbenchByteWindow window;
            window.ok = true; window.address = address;
            if (length > 64) length = 64;
            if (length && length - 1 > UINT64_MAX - address) length = UINT64_MAX - address + 1;
            window.bytes.resize(length, 0); window.validMask.resize(length, state);
            return window;
        }
        int AddressBits() const override { return 64; }
        bool HasPreviousRead() const override { return false; }
    };
    QLineEdit* VisibleEditor(QWidget& view)
    {
        for (auto* editor : view.findChildren<QLineEdit*>(QStringLiteral("ksMemwbDisasmInlineEditor")))
            if (editor->isVisible()) return editor;
        return nullptr;
    }
    void Check(const bool value, const char* message)
    {
        ++checks;
        if (!value) { ++failures; std::cerr << "FAIL " << message << '\n'; }
    }
    void Theme(const bool dark)
    {
        KswordTheme::SetDarkModeEnabled(dark);
        QPalette palette;
        palette.setColor(QPalette::Window, KswordTheme::WindowColor());
        palette.setColor(QPalette::Base, KswordTheme::SurfaceColor());
        palette.setColor(QPalette::AlternateBase, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::WindowText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Text, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::ButtonText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Button, KswordTheme::SurfaceColor());
        palette.setColor(QPalette::Highlight, KswordTheme::PrimaryAccentColor());
        palette.setColor(QPalette::HighlightedText, QColor(KswordTheme::OnAccentHex()));
        qApp->setPalette(palette);
        qApp->setStyleSheet(ks::ui::BuildGlobalBaseControlStyleBlock());
        QCoreApplication::processEvents();
    }
    ks::ui::DecodeOneFn Decoder()
    {
        return [](const std::uint8_t* bytes, const std::size_t length, const std::uint64_t address, const bool x64)
            -> std::optional<ks::ui::DecodedRow> {
            ZydisDisassembledInstruction instruction{};
            if (!ZYAN_SUCCESS(ZydisDisassembleIntel(x64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32,
                address, bytes, length, &instruction))) return std::nullopt;
            const QString source = QString::fromLatin1(instruction.text);
            const qsizetype separator = source.indexOf(QLatin1Char(' '));
            return ks::ui::DecodedRow{address, QByteArray(reinterpret_cast<const char*>(bytes), instruction.info.length),
                separator < 0 ? source : source.left(separator), separator < 0 ? QString() : source.mid(separator + 1), true};
        };
    }
    ks::ui::AssembleOneFn Assembler()
    {
        return [](const QString& source, const std::uint64_t address, const bool x64) {
            const auto result = ks::ui::InstructionAssembler::assemble(source, address,
                x64 ? ks::ui::DisassemblyArchitecture::X64 : ks::ui::DisassemblyArchitecture::X86);
            return ks::ui::WorkbenchAssembleResult{result.success, result.bytes, result.error, result.errorLine};
        };
    }
    void CanvasChecks()
    {
        using namespace ks::ui;
        MemoryRowCanvas canvas;
        QVector<MemoryDisplayRow> rows;
        for (int index = 0; index < 150; ++index)
        {
            MemoryDisplayRow row;
            row.address = 0x1000 + static_cast<std::uint64_t>(index) * 3;
            row.bytes = QByteArray::fromHex("E4B8AD"); row.validMask = {1, 1, 1};
            row.tokens.push_back({QStringLiteral("中"), row.address, 3, MemoryTokenRole::Plain});
            rows.push_back(row);
        }
        canvas.setRows(rows, false); canvas.resize(640, 320); canvas.show(); QCoreApplication::processEvents();
        const QRect text = canvas.contentRect(0);
        QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, text.topLeft() + QPoint(4, text.height() / 2));
        Check(canvas.selectedRange().has_value() && canvas.selectedRange()->first == 0x1000 && canvas.selectedRange()->second == 0x1002,
            "clicking a CJK token selects all three original bytes");
        Check(canvas.selectedText() == QStringLiteral("中"), "token copy retains original character");
        bool complete = false;
        Check(canvas.selectedBytes(&complete) == QByteArray::fromHex("E4B8AD") && complete, "byte copy is exact");
        const auto secondCharacter = canvas.contentRect(1).topLeft() + QPoint(4, canvas.contentRect(1).height() / 2);
        const auto firstCharacter = canvas.contentRect(0).topLeft() + QPoint(4, canvas.contentRect(0).height() / 2);
        QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, secondCharacter);
        QTest::mouseMove(canvas.viewport(), firstCharacter);
        QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, firstCharacter);
        Check(canvas.selectedRange() && canvas.selectedRange()->first == 0x1000 && canvas.selectedRange()->second == 0x1005,
            "backward character dragging retains all bytes of the original anchor character");
        canvas.verticalScrollBar()->setValue(20);
        const QPoint center = canvas.viewport()->rect().center();
        QWheelEvent wheel(center, canvas.viewport()->mapToGlobal(center), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas.viewport(), &wheel);
        Check(canvas.verticalScrollBar()->value() == 23, "global smooth scroll still advances exactly three rows");
        canvas.setRows(rows, true);
        Check(canvas.verticalScrollBar()->value() == 23, "same rows refresh preserves reading position");
        QTest::keyClick(&canvas, Qt::Key_Down);
        Check(canvas.selectedRow() == 1, "Down advances exactly one address-backed row");
        const auto selection = canvas.selectedRange();
        canvas.zoomBy(1);
        Check(canvas.zoomSteps() == 1 && canvas.selectedRange() == selection, "zoom preserves address selection");
        canvas.setBytesVisible(false);
        Check(canvas.contentRect(0).left() < text.left(), "hiding bytes gives width to text");
        Check(canvas.contentRect(0).left() - 8 >= QFontMetrics(canvas.font()).horizontalAdvance(QStringLiteral("0x0000000000001000")) + 16,
            "address column includes full hexadecimal prefix and digits");
        auto partial = rows.front(); partial.validMask = {1};
        canvas.setRows({partial}, false); canvas.selectRange(partial.address, partial.address + 2);
        canvas.selectedBytes(&complete);
        Check(!complete, "absent byte validity cannot become successful evidence");
        MemoryDisplayRow wrapped;
        wrapped.address = 0x8000; wrapped.bytes = QByteArray(320, 'A'); wrapped.validMask.fill(1, 320);
        for (int offset = 0; offset < 320; ++offset)
            wrapped.tokens.push_back({QStringLiteral("A"), wrapped.address + offset, 1, MemoryTokenRole::Plain});
        canvas.setTextPriority(true); canvas.setWrapContent(true); canvas.resize(260, 160);
        canvas.setRows({wrapped}, false); QCoreApplication::processEvents();
        Check(canvas.verticalScrollBar()->maximum() > 3, "a wrapped row supports scrolling within its visual lines");
        canvas.verticalScrollBar()->setValue(0);
        QWheelEvent wrappedWheel(center, canvas.viewport()->mapToGlobal(center), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas.viewport(), &wrappedWheel);
        Check(canvas.verticalScrollBar()->value() == 3 && canvas.addressForVisualLine() && *canvas.addressForVisualLine() > wrapped.address,
            "wrapped text wheel advances three displayed lines rather than three whole byte rows");
        wrapped.address = 0xFFFF800000008000ULL;
        for (int offset = 0; offset < wrapped.tokens.size(); ++offset) wrapped.tokens[offset].address = wrapped.address + offset;
        canvas.setAddressBits(32); canvas.setRows({wrapped}, false);
        Check(canvas.contentRect(0).left() - 8 >= QFontMetrics(canvas.font()).horizontalAdvance(QStringLiteral("0xFFFF800000008000")) + 16,
            "32-bit decoding cannot truncate a high physical or kernel address");
    }
    void TextChecks(const QString& shots)
    {
        using namespace ks::ui;
        MemorySnapshotBytesProvider provider;
        QByteArray bytes(15, 'A'); bytes += QByteArray::fromHex("E4B8ADF09F988042");
        provider.setSnapshot(0x2000, bytes, bytes, {}, 64, true);
        WorkbenchTextView view;
        view.setBytesProvider(&provider); view.setEncoding(WorkbenchTextView::Encoding::Utf8); view.setWindow(0x2000, bytes.size());
        view.resize(760, 340); view.show(); QCoreApplication::processEvents();
        Check(view.canvas()->rows().size() == 2 && view.canvas()->rows().front().bytes.size() == 18, "row cut does not split UTF8");
        Check(view.canvas()->rows().front().tokens.back().text == QStringLiteral("中"), "cross-row CJK decodes");
        Check(view.canvas()->rows()[1].tokens.front().text == QString::fromUtf8("😀"), "supplementary Unicode renders");
        auto* search = view.findChild<QLineEdit*>(QStringLiteral("ksMemwbTextFind"));
        search->setText(QStringLiteral("中")); QTest::keyClick(search, Qt::Key_Return);
        Check(view.selectedByteRange().has_value() && view.selectedByteRange()->first == 0x200F && view.selectedByteRange()->second == 0x2011, "search selects original scalar bytes");
        view.canvas()->selectRange(0x2000, 0x2016);
        Check(view.selectedDecodedText() == QString(15, QLatin1Char('A')) + QStringLiteral("中") + QString::fromUtf8("😀") + QLatin1Char('B'),
            "copy across byte rows adds no display newline");
        QTest::keyClick(view.canvas(), Qt::Key_C, Qt::ControlModifier);
        Check(QApplication::clipboard()->text() == view.selectedDecodedText(), "CtrlC uses true decoded text");
        const QString original = view.renderedText(); view.setBytesVisible(false); view.setWrapText(true);
        Check(view.renderedText() == original && !view.bytesVisible() && view.wrapText(), "layout controls never transform target bytes");
        view.grab().save(shots + QStringLiteral("/text-light.png"));
        Theme(true); view.grab().save(shots + QStringLiteral("/text-dark.png"));
        view.resize(260, 420); QCoreApplication::processEvents();
        Check(view.width() == 260 && view.canvas()->width() > 100, "text view remains usable at narrow width");
        view.grab().save(shots + QStringLiteral("/text-narrow-dark.png"));
        Theme(false);
        provider.setSnapshot(0x3001, QByteArray::fromHex("FEFF0041D83DDE00"), {}, {}, 64, true);
        view.reset(); view.setEncoding(WorkbenchTextView::Encoding::Auto); view.setWindow(0x3001, 8);
        Check(view.hasBom() && view.effectiveEncoding() == WorkbenchTextView::Encoding::Utf16BE
            && view.renderedText() == QStringLiteral("⟨BOM⟩A") + QString::fromUtf8("😀"), "BOM chooses UTF16BE with full surrogate pair");
        view.setEncoding(WorkbenchTextView::Encoding::Utf8);
        Check(!view.hasBom() && view.effectiveEncoding() == WorkbenchTextView::Encoding::Utf8, "manual encoding wins");
        provider.setSnapshot(0x5000, QByteArray("a\r\nb\tc", 6), {}, {}, 64, true);
        view.setWindow(0x5000, 6); view.canvas()->selectRange(0x5000, 0x5005);
        Check(view.selectedDecodedText() == QString::fromUtf8("a\r\nb\tc"), "copy restores CRLF and tab from presentation markers");
        view.setControlCharactersVisible(false);
        Check(!view.renderedText().contains(QStringLiteral("↵")) && !view.renderedText().contains(QStringLiteral("⇥")),
            "control character markers can be hidden without removing source bytes");
        Check(view.selectedDecodedText() == QString::fromUtf8("a\r\nb\tc"), "hiding control markers preserves real text copying");
        view.setControlCharactersVisible(true);
        QByteArray browseBytes(8192, 'A'); browseBytes.replace(46, 3, QByteArray::fromHex("E4B8AD"));
        provider.setSnapshot(0x6000, browseBytes, {}, {}, 64, true);
        view.setWindow(0x6000, 4096);
        int requested = 0;
        QObject::connect(&view, &WorkbenchTextView::windowRequested, &view, [&](quint64, quint64) { ++requested; });
        const auto forwardScalar = view.canvas()->addressForVisualLine(3);
        view.canvas()->requestMore(1, 3);
        Check(requested == 1 && forwardScalar && view.windowAddress() == *forwardScalar, "forward request advances to a whole scalar boundary on the requested visual line");
        view.canvas()->requestMore(-1, 3);
        Check(requested == 2 && view.windowAddress() == 0x6000, "backward browsing restores the previous visual lines without escaping captured data");
        Check(view.renderedText().contains(QStringLiteral("中")), "backward rebase retains multibyte text");
        provider.setSnapshot(UINT64_MAX, QByteArray("Z", 1), {}, {}, 64, true);
        view.setAddressBounds(UINT64_MAX, UINT64_MAX); view.setWindow(UINT64_MAX, 1);
        Check(view.renderedText() == QStringLiteral("Z"), "last UINT64 address is a readable one-byte window");
        const int beforeEdge = requested;
        view.canvas()->requestMore(1, 3); view.canvas()->requestMore(-1, 3);
        Check(requested == beforeEdge && view.windowAddress() == UINT64_MAX, "closed singleton bounds prevent either edge request");
        view.clearAddressBounds(); view.setAddressBits(32);
        provider.setSnapshot(0xFFFFFFF0ULL, QByteArray(16, 'X'), {}, {}, 32, true);
        view.setWindow(0xFFFFFFFFULL, 1); view.canvas()->requestMore(1, 3);
        Check(view.windowAddress() == 0xFFFFFFFFULL && requested == beforeEdge, "32-bit end cannot browse into 64-bit addresses");
        view.setAddressRange(0xFFFFFFF0ULL, 16); view.canvas()->requestMore(-1, 3);
        Check(view.windowAddress() == 0xFFFFFFF0ULL && requested == beforeEdge + 1, "backward request clamps to captured first byte");
        view.canvas()->requestMore(-1, 3);
        Check(requested == beforeEdge + 1, "captured lower edge emits no new request");
        view.clearAddressBounds(); view.setAddressBits(64); view.setWindow(0x9000, 4096);
        view.canvas()->requestMore(-1, 3);
        Check(requested == beforeEdge + 2 && view.windowAddress() < 0x9000, "unavailable window still allows navigation back");
        view.reset(); Check(view.renderedText().isEmpty() && view.canvas()->rows().isEmpty(), "reset clears old target text");
        MemorySnapshotBytesProvider largeProvider;
        const QByteArray largeBytes(9000, 'A');
        largeProvider.setSnapshot(0x6000, largeBytes, largeBytes, {}, 64, true);
        WorkbenchTextView continuous;
        continuous.setBytesProvider(&largeProvider); continuous.setEncoding(WorkbenchTextView::Encoding::Utf8);
        continuous.setWindow(0x6000, 4096); continuous.resize(760, 220); continuous.show(); QCoreApplication::processEvents();
        continuous.canvas()->verticalScrollBar()->setValue(continuous.canvas()->verticalScrollBar()->maximum());
        const auto nextAddress = continuous.canvas()->addressForVisualLine(3);
        Check(nextAddress && *nextAddress > 0x6000 + 3000, "text reaches the far end of its loaded window");
        continuous.canvas()->requestMore(1, 3);
        Check(nextAddress && continuous.windowAddress() == *nextAddress,
            "forward window extension continues from the visible edge instead of rewinding to the old start");
        largeProvider.setSnapshot(0x6000, QByteArray("one one one", 11), {}, {}, 64, true);
        continuous.setWindow(0x6000, 11);
        auto* repeatSearch = continuous.findChild<QLineEdit*>(QStringLiteral("ksMemwbTextFind"));
        repeatSearch->setText(QStringLiteral("one")); QTest::keyClick(repeatSearch, Qt::Key_Return);
        Check(continuous.selectedByteRange() && continuous.selectedByteRange()->first == 0x6000, "text search selects the first match");
        QTest::keyClick(repeatSearch, Qt::Key_F3);
        Check(continuous.selectedByteRange() && continuous.selectedByteRange()->first == 0x6004, "F3 advances while the text search field has focus");
        QTest::keyClick(repeatSearch, Qt::Key_F3, Qt::ShiftModifier);
        Check(continuous.selectedByteRange() && continuous.selectedByteRange()->first == 0x6000, "Shift F3 restores the previous text match");
    }
    void AdversarialChecks()
    {
        using namespace ks::ui;
        MemorySnapshotBytesProvider provider;
        const QByteArray bytes(9000, static_cast<char>(0x90));
        provider.setSnapshot(0x1000, bytes, bytes, {}, 64, true);
        WorkbenchDisasmView disasm;
        disasm.setBytesProvider(&provider); disasm.setDecodeBackend(Decoder());
        disasm.setAddressRange(0x1000, bytes.size()); disasm.jumpTo(0x1000);
        disasm.resize(760, 320); disasm.show(); QCoreApplication::processEvents();
        disasm.canvas()->setSelectedRow(disasm.model()->rowCount() - 2);
        Check(disasm.selectedInstruction() && disasm.selectedInstruction()->address == 0x1FFF,
            "adversarial disassembly starts on the last real instruction before its footer");
        QTest::keyClick(disasm.canvas(), Qt::Key_Down); QCoreApplication::processEvents();
        Check(disasm.selectedInstruction() && disasm.selectedInstruction()->address == 0x2000,
            "one Down skips the footer and selects the next instruction across a window boundary");
        QTest::keyClick(disasm.canvas(), Qt::Key_Up);
        Check(disasm.selectedInstruction() && disasm.selectedInstruction()->address == 0x1FFF,
            "Up returns to the previous instruction after forward window continuation");

        provider.setSnapshot(0x1000, QByteArray(9000, 'A'), {}, {}, 64, true);
        WorkbenchTextView text;
        text.setBytesProvider(&provider); text.setEncoding(WorkbenchTextView::Encoding::Ascii);
        text.setAddressRange(0x1000, 9000); text.setWindow(0x1000, 4096);
        text.resize(760, 320); text.show(); QCoreApplication::processEvents();
        text.canvas()->setSelectedRow(static_cast<int>(text.canvas()->rows().size()) - 1);
        QTest::keyClick(text.canvas(), Qt::Key_Down); QCoreApplication::processEvents();
        Check(text.selectedByteRange() && text.selectedByteRange()->first == 0x2000,
            "one Down selects the next text row across a window boundary");

        MemoryRowCanvas delayed;
        MemoryDisplayRow first; first.address = 0x80; first.bytes = QByteArray("AB", 2); first.validMask = {1, 1};
        MemoryDisplayRow footer; footer.address = 0x82; footer.selectable = false;
        delayed.setRows({first, footer}, false); delayed.setSelectedRow(0);
        int requested = 0;
        QObject::connect(&delayed, &MemoryRowCanvas::requestMore, &delayed, [&](int, int) { ++requested; });
        QTest::keyClick(&delayed, Qt::Key_Down);
        Check(requested == 1 && delayed.selectedRange() && delayed.selectedRange()->first == 0x80,
            "keyboard continuation retains the source selection while asynchronous bytes are pending");
        MemoryDisplayRow next = first; next.address = 0x82;
        footer.address = 0x84; delayed.setRows({first, next, footer}, true);
        Check(delayed.selectedRange() && delayed.selectedRange()->first == 0x82,
            "the requested keyboard address is selected when delayed rows arrive");
        QTest::keyClick(&delayed, Qt::Key_Down); delayed.setSelectedRow(0);
        MemoryDisplayRow later = first; later.address = 0x84;
        delayed.setRows({first, next, later}, true);
        Check(delayed.selectedRange() && delayed.selectedRange()->first == 0x80,
            "explicit selection cancels pending keyboard navigation before delayed bytes arrive");

        auto* host = new WorkbenchTextView;
        host->setBytesProvider(&provider); host->setEncoding(WorkbenchTextView::Encoding::Ascii);
        host->setWindow(0x1000, 256); host->resize(760, 320); host->show(); QCoreApplication::processEvents();
        const QPointer<WorkbenchTextView> guard(host);
        auto* canvas = host->canvas();
        const QPoint pos = canvas->contentRect(0).topLeft() + QPoint(3, canvas->contentRect(0).height() / 2);
        QObject::connect(host, &WorkbenchTextView::contextMenuAboutToShow, qApp,
            [host](QMenu*, quint64, bool) { delete host; });
        QContextMenuEvent menu(QContextMenuEvent::Mouse, pos, canvas->viewport()->mapToGlobal(pos));
        QApplication::sendEvent(canvas->viewport(), &menu);
        Check(!guard, "destroying a text host during menu extension returns without accessing destroyed members");
    }
    void DisasmChecks(const QString& shots)
    {
        using namespace ks::ui;
        {
            UnavailableBytes unavailable;
            WorkbenchDisasmView pending;
            pending.setBytesProvider(&unavailable); pending.setDecodeBackend(Decoder());
            pending.setAddressRange(0x7000, 4096); pending.jumpTo(0x7000);
            Check(!pending.model()->rowAt(0) && pending.canvas()->rows()[0].validMask[0] == 2,
                "loading bytes are rendered without fabricating instructions");
            unavailable.state = 0; pending.refreshView();
            Check(!pending.model()->rowAt(0) && pending.canvas()->rows()[0].validMask[0] == 0,
                "unreadable bytes are distinct from loading and decoded instructions");
            pending.canvas()->requestMore(1, 3);
            Check(pending.anchorAddress() > 0x7000, "unreadable window does not trap forward browsing");
            pending.setAddressBounds(UINT64_MAX, UINT64_MAX); pending.jumpTo(UINT64_MAX);
            Check(pending.canvas()->rows().back().address == UINT64_MAX, "last disassembly placeholder address cannot wrap to zero");
        }
        MemorySnapshotBytesProvider provider;
        QByteArray bytes = QByteArray::fromHex("4883EC2848B8010000000000000090E804000000EB02CCCC4883C428C3");
        bytes += QByteArray(500, static_cast<char>(0x90));
        provider.setSnapshot(0x4000, bytes, bytes, {}, 64, true);
        WorkbenchDisasmView view;
        view.setBytesProvider(&provider); view.setDecodeBackend(Decoder()); view.setAssembleBackend(Assembler());
        view.setAddressRange(0x4000, bytes.size()); view.jumpTo(0x4000);
        view.resize(900, 430); view.show(); QCoreApplication::processEvents();
        Check(view.model()->rowAt(0).has_value() && view.model()->rowAt(0)->bytes.size() == 4, "first instruction uses four actual bytes");
        Check(view.model()->rowAt(1).has_value() && view.model()->rowAt(1)->bytes.size() == 10, "long instruction keeps ten bytes");
        Check(view.canvas()->rows()[0].address == 0x4000 && view.canvas()->rows()[1].address == 0x4004, "variable instruction rows advance by true lengths");
        view.canvas()->setSelectedRow(3); QTest::keyClick(view.canvas(), Qt::Key_Return);
        Check(view.anchorAddress() == 0x4018, "Enter follows the decoded absolute call operand");
        QTest::keyClick(view.canvas(), Qt::Key_Backspace);
        Check(view.anchorAddress() == 0x4000, "Backspace restores the prior instruction anchor");
        int stageCount = 0; QByteArray staged;
        QObject::connect(&view, &WorkbenchDisasmView::stageRequested, &view, [&](quint64, const QByteArray& output) { ++stageCount; staged = output; });
        view.canvas()->setSelectedRow(0); QTest::keyClick(view.canvas(), Qt::Key_F2);
        auto* editor = VisibleEditor(view);
        Check(editor != nullptr && view.isEditing(), "F2 explicitly opens inline assembly");
        QTest::keyClick(editor, Qt::Key_Return); QCoreApplication::processEvents();
        std::cerr << "Unchanged Enter: stage=" << stageCount << " editing=" << view.isEditing()
            << " visibleEditor=" << (VisibleEditor(view) != nullptr) << '\n';
        Check(stageCount == 0 && !view.isEditing(), "unchanged editor submits nothing");
        view.beginSelectedInstructionEdit(); editor = VisibleEditor(view);
        provider.setSnapshot(0x4000, QByteArray(bytes.size(), static_cast<char>(0x90)), bytes, {}, 64, true); view.refreshView();
        Check(view.isEditing() && view.model()->rowAt(0)->bytes.size() == 4, "refresh cannot replace active editing row");
        QTest::keyClick(editor, Qt::Key_Escape); QCoreApplication::processEvents();
        Check(!view.isEditing() && view.model()->rowAt(0)->bytes.size() == 1, "Escape applies deferred refresh");
        provider.setSnapshot(0x4000, bytes, bytes, {}, 64, true); view.refreshView();
        view.canvas()->setSelectedRow(0); view.beginSelectedInstructionEdit(); editor = VisibleEditor(view);
        editor->setText(QStringLiteral("nop")); QTest::keyClick(editor, Qt::Key_Return); QCoreApplication::processEvents();
        Check(stageCount == 1 && staged == QByteArray::fromHex("90909090"), "short assembly pads whole original instruction with NOP");
        view.beginSelectedInstructionEdit(); view.setEditable(false); QCoreApplication::processEvents();
        Check(!view.isEditing() && stageCount == 1, "read-only change cancels editing without a write");
        int ancestorFind = 0;
        QShortcut competingFind(QKeySequence(Qt::CTRL | Qt::Key_F), &view);
        QObject::connect(&competingFind, &QShortcut::activated, &view, [&]() { ++ancestorFind; });
        QTest::keyClick(view.canvas(), Qt::Key_F, Qt::ControlModifier);
        Check(ancestorFind == 0, "disassembly find owns its shortcut instead of opening a hidden ancestor page");
        auto* search = view.findChild<QLineEdit*>(QStringLiteral("ksMemwbDisasmFind"));
        Check(search && search->isVisible(), "CtrlF opens instruction search in read-only mode");
        search->setText(QStringLiteral("call")); QTest::keyClick(search, Qt::Key_Return);
        Check(view.selectedInstruction().has_value() && view.selectedInstruction()->address == 0x400F,
            "instruction search selects the matching address");
        QTest::keyClick(search, Qt::Key_Escape);
        Check(!search->isVisible(), "Escape closes instruction search without changing target");
        view.grab().save(shots + QStringLiteral("/disasm-light.png"));
        Theme(true); view.grab().save(shots + QStringLiteral("/disasm-dark.png")); Theme(false);
    }
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/consola.ttf"));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
    Theme(false);
    ks::ui::InstallGlobalSmoothScrollSupport(&app); ks::ui::SetGlobalSmoothScrollingEnabled(true);
    const QString shots = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    CanvasChecks(); std::cerr << "Canvas checks completed\n";
    TextChecks(shots); std::cerr << "Text checks completed\n";
    DisasmChecks(shots); std::cerr << "Disasm checks completed\n";
    AdversarialChecks(); std::cerr << "Adversarial checks completed\n";
    std::cout << "Portable production row views: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
