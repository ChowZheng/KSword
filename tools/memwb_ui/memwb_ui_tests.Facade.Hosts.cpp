// memwb_ui_tests.Facade.Hosts.cpp
// 作用：HexEditorWidget 门面的离屏验证（第三组：宿主使用形态与销毁安全）——
//   类名链（UI/TextSearchReplaceSupport.cpp 靠它判断"自带查找栏"，复刻其判断并带反例）、
//   FileDock 使用形态回放（只读、16 字节行宽、setByteArray(窗口, 文件偏移作基址)、jumpToAbsoluteAddress(文件偏移)、
//   查找仅针对已加载数据、openFindPanel 作为按钮槽）、
//   DiskEditor 使用形态回放（setEditable 随只读开关切换、byteEdited 置脏、data() 整缓冲取回、
//   宿主自己的快照不被编辑污染、重新读取、clearData、regionSize）、
//   销毁安全（从未显示、查找进行中、槽内 deleteLater、菜单对象随控件销毁）。

#include "memwb_ui_facade.h"

#include <QApplication>
#include <QCoreApplication>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <cstdint>
#include <vector>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using ks::ui::HexFindBar;
        using ks::ui::HexView;
        using ChangeKind = HexCanvas::ChangeKind;

        // HasOwnFindPanelLikeHost：
        // - 作用：逐字复刻 UI/TextSearchReplaceSupport.cpp 的 hasOwnFindPanel 判断——
        //   沿 QObject 父链向上，任一层的类名包含 CodeEditorWidget / HexEditorWidget / CodeTextEdit 即认为自带查找栏；
        // - 传入：起点对象；传出：是否命中。全局查找面板据此对内存编辑器让位。
        bool HasOwnFindPanelLikeHost(const QObject* start)
        {
            const QObject* currentObject = start;
            while (currentObject != nullptr)
            {
                const QString className = QString::fromLatin1(currentObject->metaObject()->className());
                if (className.contains(QStringLiteral("CodeEditorWidget"))
                    || className.contains(QStringLiteral("HexEditorWidget"))
                    || className.contains(QStringLiteral("CodeTextEdit")))
                {
                    return true;
                }
                currentObject = currentObject->parent();
            }
            return false;
        }

        // PutText：把文本字节写进数据的指定偏移。
        void PutText(QByteArray& data, int offset, const QByteArray& text)
        {
            for (int index = 0; index < text.size(); ++index)
            {
                data[offset + index] = text.at(index);
            }
        }

        // SearchText：
        // - 作用：在门面的查找条里按 UTF-8 文本搜索（区分大小写）并等结果；
        // - 传入：门面、要找的文本；传出：是否命中。搜索没能启动或超时按未命中处理。
        bool SearchText(HexEditorWidget& widget, const QString& text)
        {
            HexFindBar& bar = *ViewOf(widget)->findBar();
            bar.setMode(HexFindBar::Mode::TextUtf8);
            bar.setCaseSensitive(true);
            bar.setPatternText(text);
            QSignalSpy done(&bar, &HexFindBar::searchCompleted);
            if (!bar.findNext())
            {
                return false;
            }
            if (!PumpUntil([&]() { return done.count() >= 1; }, 8000))
            {
                return false;
            }
            return done.at(0).at(0).toBool();
        }

        // 类名链：门面内所有部件的父链上都能找到 HexEditorWidget；脱离门面的 HexView 与普通控件找不到（反例）。
        void TestClassChain()
        {
            ApplyTheme(false);
            auto widget = MakeFacade(0x1000, MakePattern(1024), true, QSize(900, 400));
            HexView* view = ViewOf(*widget);
            view->setInspectorVisible(true);
            Flush();
            CHECK(view->panel() != nullptr);

            // 画布、视口、查找条与跳转条的输入框、状态条、工具栏、解释器面板：全部命中。
            CHECK(HasOwnFindPanelLikeHost(view->canvas()));
            CHECK(HasOwnFindPanelLikeHost(view->canvas()->viewport()));
            CHECK(HasOwnFindPanelLikeHost(view->findBar()->lineEdit()));
            CHECK(HasOwnFindPanelLikeHost(view->gotoBar()->lineEdit()));
            CHECK(HasOwnFindPanelLikeHost(view->statusBar()));
            CHECK(HasOwnFindPanelLikeHost(view->toolbar()));
            CHECK(HasOwnFindPanelLikeHost(view->panel()));
            CHECK(HasOwnFindPanelLikeHost(widget.get()));
            CHECK(widget->findChildren<QWidget*>().size() > 10);

            // 命中的必须是门面这一层：父链上第一个含 HexEditorWidget 的对象就是门面本身。
            const QObject* current = view->canvas();
            const QObject* matched = nullptr;
            while (current != nullptr && matched == nullptr)
            {
                if (QString::fromLatin1(current->metaObject()->className()).contains(QStringLiteral("HexEditorWidget")))
                {
                    matched = current;
                }
                current = current->parent();
            }
            CHECK(matched == widget.get());

            // 反例：脱离门面的 HexView 与普通控件不命中——证明上面的判断确实依赖门面这一层，而不是恒为真。
            HexView standalone;
            standalone.setBuffer(0, MakePattern(64));
            CHECK(!HasOwnFindPanelLikeHost(standalone.canvas()));
            CHECK(!HasOwnFindPanelLikeHost(standalone.canvas()->viewport()));
            CHECK(!HasOwnFindPanelLikeHost(standalone.findBar()->lineEdit()));
            QWidget plain;
            QWidget plainChild(&plain);
            CHECK(!HasOwnFindPanelLikeHost(&plainChild));
        }

        // FileDock 使用形态：文件详情页自己做分页读取，门面只负责显示窗口；基址就是文件偏移；查找仅针对已加载窗口。
        void TestFileDockShape()
        {
            ApplyTheme(false);

            // 文件镜像：0x30123 字节（最后一个窗口不满），两个标记各只出现在自己的窗口里。
            QByteArray file = MakePattern(0x30123, 4);
            PutText(file, 0x1234, QByteArrayLiteral("FILEMARK1"));
            PutText(file, 0x21000, QByteArrayLiteral("OTHERWIN!"));

            // 构造：与 FileDock::buildHexTab 一致——先只读、16 字节行宽，再塞进页面布局。
            auto page = std::make_unique<QWidget>();
            auto* pageLayout = new QVBoxLayout(page.get());
            HexEditorWidget* hex = new HexEditorWidget(page.get());
            hex->setEditable(false);
            hex->setBytesPerRow(16);
            pageLayout->addWidget(hex, 1);
            page->resize(1000, 600);
            page->show();
            QApplication::processEvents();
            const QPointer<HexEditorWidget> hexGuard(hex);
            CHECK(!hex->isEditable() && hex->bytesPerRow() == 16);

            // 窗口 1 [0, 0x10000)：基址就是文件偏移；跳转到文件偏移成功并选中。
            const QByteArray window1 = file.mid(0, 0x10000);
            hex->setByteArray(window1, 0);
            CHECK(hex->regionSize() == 0x10000 && hex->baseAddress() == 0 && hex->data() == window1);
            CHECK(hex->jumpToAbsoluteAddress(0x1234));
            CHECK(hex->selectedAbsoluteAddress() == 0x1234);

            // 查找仅针对已加载数据：本窗口里的标记命中（命中起点就是文件偏移），别的窗口里的标记找不到。
            CHECK(SearchText(*hex, QStringLiteral("FILEMARK1")));
            CHECK(hex->selectedAbsoluteAddress() == 0x1234);
            CHECK(!SearchText(*hex, QStringLiteral("OTHERWIN!")));
            CHECK(ViewOf(*hex)->findBar()->resultText() == QStringLiteral("未找到"));

            // 只读：键入被忽略，data() 不变，不发 byteEdited。
            QSignalSpy edited(hex, &HexEditorWidget::byteEdited);
            HexCanvas& canvas = *CanvasOf(*hex);
            canvas.setFocus();
            ClickAddress(canvas, 0x1300);
            Type(canvas, QStringLiteral("AB"));
            CHECK(edited.isEmpty() && hex->data() == window1);

            // 窗口 2 [0x20000, 0x30000)：换数据后旧高亮清空；基址、跳转与查找都以文件偏移为准。
            const QByteArray window2 = file.mid(0x20000, 0x10000);
            hex->setByteArray(window2, 0x20000);
            CHECK(hex->baseAddress() == 0x20000 && hex->data() == window2);
            CHECK(!ViewOf(*hex)->findBar()->highlightActive());
            CHECK(hex->selectedAbsoluteAddress() == 0x20000);
            CHECK(hex->jumpToAbsoluteAddress(0x21000));
            CHECK(hex->selectedAbsoluteAddress() == 0x21000 && hex->selectedOffset() == 0x1000);
            CHECK(SearchText(*hex, QStringLiteral("OTHERWIN!")));
            CHECK(hex->selectedAbsoluteAddress() == 0x21000);
            CHECK(!SearchText(*hex, QStringLiteral("FILEMARK1")));

            // 窗口之外的地址（FileDock 的跳转会先换窗口再跳，所以这里必须失败而不是静默）。
            CHECK(!hex->jumpToAbsoluteAddress(0x30000));
            CHECK(!hex->jumpToAbsoluteAddress(0x1234));

            // 最后一个不满的窗口：末字节成立，再往后失败。
            const QByteArray window3 = file.mid(0x30000);
            hex->setByteArray(window3, 0x30000);
            CHECK(hex->regionSize() == 0x123);
            CHECK(hex->jumpToAbsoluteAddress(0x30122) && hex->selectedAbsoluteAddress() == 0x30122);
            CHECK(!hex->jumpToAbsoluteAddress(0x30123));

            // 空文件：窗口为空，选中地址回退为基址，数据为空（FileDock 此时不再跳转）。
            hex->setByteArray(QByteArray(), 0);
            CHECK(hex->regionSize() == 0 && hex->data().isEmpty() && hex->selectedAbsoluteAddress() == 0);

            // "查找当前范围"按钮：openFindPanel 直接作为按钮 clicked 的接收函数（FileDock 的接线形态）。
            hex->setByteArray(window1, 0);
            auto* findButton = new QPushButton(page.get());
            QObject::connect(findButton, &QPushButton::clicked, hex, &HexEditorWidget::openFindPanel);
            ViewOf(*hex)->closeFindBar();
            Flush();
            CHECK(!ViewOf(*hex)->findBar()->isVisible());
            findButton->click();
            Flush();
            CHECK(ViewOf(*hex)->findBar()->isVisible());

            // 页面销毁时门面随之销毁（FileDock 用 QPointer 守护异步回调）。
            page.reset();
            CHECK(hexGuard.isNull());
        }

        // DiskEditor 使用形态：只读开关驱动 setEditable；byteEdited 置脏；data() 取回整个缓冲写盘；宿主快照不被污染。
        void TestDiskEditorShape()
        {
            ApplyTheme(false);

            // 宿主状态：只读开关、脏标记、收到的编辑记录。
            bool readOnly = true;
            bool dirty = false;
            struct EditRecord
            {
                std::uint64_t address;  // 被改字节的绝对地址
                int oldValue;           // 旧值
                int newValue;           // 新值
            };
            std::vector<EditRecord> records;

            // 构造：与 DiskEditorTab 一致——先只读、16 字节行宽，再接 byteEdited。
            HexEditorWidget editor;
            editor.setEditable(false);
            editor.setBytesPerRow(16);
            editor.resize(1000, 520);
            editor.show();
            QObject::connect(&editor, &HexEditorWidget::byteEdited, &editor,
                [&](std::uint64_t address, std::uint8_t oldValue, std::uint8_t newValue) {
                    dirty = true;
                    records.push_back(EditRecord{ address, oldValue, newValue });
                });
            HexCanvas& canvas = *CanvasOf(editor);

            // 读取一个扇区窗口：宿主自己的快照 loaded 与控件共享同一份字节；随后按只读开关设置可编辑。
            const std::uint64_t baseOffset = 0x200000;
            const QByteArray loaded = MakePattern(4096, 7);
            editor.setByteArray(loaded, baseOffset);
            editor.setEditable(!readOnly);
            QApplication::processEvents();
            CHECK(!editor.isEditable());
            CHECK(editor.regionSize() == 4096 && editor.baseAddress() == baseOffset && editor.data() == loaded);

            // 只读保护开启：键入无效，不置脏。
            canvas.setFocus();
            ClickAddress(canvas, baseOffset + 0x10);
            Type(canvas, QStringLiteral("FF"));
            CHECK(!dirty && records.empty() && editor.data() == loaded);

            // 关闭只读保护：可以编辑；byteEdited 置脏，记录里是绝对地址与旧值/新值。
            readOnly = false;
            editor.setEditable(!readOnly);
            CHECK(editor.isEditable());
            const int old10 = ByteValue(loaded, 0x10);
            const int new10 = OtherByte(old10, 0xFE);
            ClickAddress(canvas, baseOffset + 0x10);
            Type(canvas, HexByteText(new10));
            CHECK(dirty && records.size() == 1);
            if (records.size() == 1)
            {
                CHECK(records[0].address == baseOffset + 0x10 && records[0].oldValue == old10 && records[0].newValue == new10);
            }

            // 写盘时整缓冲取回：与快照只在被改的字节上不同，长度不变；宿主自己的快照没有被编辑污染。
            QByteArray expected = loaded;
            expected[0x10] = static_cast<char>(new10);
            CHECK(editor.data() == expected && editor.regionSize() == static_cast<std::size_t>(loaded.size()));
            CHECK(loaded == MakePattern(4096, 7));

            // 再改一个字节，再把第一个字节改回去：每次变化各一条记录，最终只剩第二处不同。
            const int old20 = ByteValue(loaded, 0x20);
            const int new20 = OtherByte(old20, 0x31);
            ClickAddress(canvas, baseOffset + 0x20);
            Type(canvas, HexByteText(new20));
            ClickAddress(canvas, baseOffset + 0x10);
            Type(canvas, HexByteText(old10));
            CHECK(records.size() == 3);
            if (records.size() == 3)
            {
                CHECK(records[1].address == baseOffset + 0x20 && records[1].oldValue == old20 && records[1].newValue == new20);
                CHECK(records[2].address == baseOffset + 0x10 && records[2].oldValue == new10 && records[2].newValue == old10);
            }
            expected = loaded;
            expected[0x20] = static_cast<char>(new20);
            CHECK(editor.data() == expected);

            // 重新读取：换基址与数据，宿主清脏并按只读开关设置可编辑；选中地址回到新基址，旧编辑不残留。
            dirty = false;
            records.clear();
            const std::uint64_t newBase = 0x300000;
            const QByteArray reloaded = MakePattern(4096, 11);
            editor.setByteArray(reloaded, newBase);
            editor.setEditable(!readOnly);
            QApplication::processEvents();
            CHECK(editor.data() == reloaded && editor.baseAddress() == newBase);
            CHECK(editor.selectedAbsoluteAddress() == newBase);
            CHECK(!dirty && records.empty());
            for (const std::uint64_t address : { newBase, newBase + 1, newBase + 0x10 })
            {
                CHECK(FacadeCell(editor, address).hasValue && FacadeCell(editor, address).change == ChangeKind::Unchanged);
            }
            ClickAddress(canvas, newBase + 0x30);
            Type(canvas, HexByteText(OtherByte(ByteValue(reloaded, 0x30), 0x7D)));
            CHECK(dirty && records.size() == 1 && records[0].address == newBase + 0x30);

            // 行宽设置跨读取保持。
            CHECK(editor.bytesPerRow() == 16);

            // 重新打开只读保护：之后的键入无效，已有记录不增加。
            readOnly = true;
            editor.setEditable(!readOnly);
            const QByteArray lockedSnapshot = editor.data();
            ClickAddress(canvas, newBase + 0x40);
            Type(canvas, QStringLiteral("12"));
            CHECK(records.size() == 1 && editor.data() == lockedSnapshot);

            // 断开设备 / 切换目标：clearData 清空；可编辑状态不受影响（旧控件语义），regionSize 归零。
            readOnly = false;
            editor.setEditable(true);
            editor.clearData();
            CHECK(editor.regionSize() == 0 && editor.data().isEmpty());
            CHECK(editor.isEditable());
            CHECK(editor.selectedAbsoluteAddress() == newBase && editor.selectedOffset() == 0);

            // 清空之后宿主写回路径的判空：regionSize 为 0 时没有可写的缓冲。
            editor.setByteArray(loaded, baseOffset);
            CHECK(editor.regionSize() == 4096 && editor.data() == loaded);
        }

        // 销毁安全：各种时机销毁门面都不崩溃、不留悬空对象。
        void TestLifetime()
        {
            ApplyTheme(false);

            // 从未显示就销毁：内部 HexView 随对象树一起销毁。
            {
                auto widget = std::make_unique<HexEditorWidget>();
                widget->setByteArray(MakePattern(1024), 0x1000);
                const QPointer<HexView> viewGuard(ViewOf(*widget));
                CHECK(!viewGuard.isNull());
                widget.reset();
                CHECK(viewGuard.isNull());
            }

            // 变更参照与未提交的编辑还在时销毁。
            {
                auto widget = MakeFacade(0x1000, MakePattern(1024), true, QSize(900, 400));
                QByteArray original = MakePattern(1024);
                original[5] = static_cast<char>(~original.at(5));
                widget->setChangeReferences(original, original);
                CanvasOf(*widget)->setFocus();
                ClickAddress(*CanvasOf(*widget), 0x1010);
                Type(*CanvasOf(*widget), HexByteText(OtherByte(ByteValue(MakePattern(1024), 0x10), 0x42)));
                const QPointer<HexView> viewGuard(ViewOf(*widget));
                widget.reset();
                CHECK(viewGuard.isNull());
                PumpFor(30);
            }

            // 查找进行中销毁：HexView 取消并等待后台线程；排队中的结果回调不会落到已销毁的对象上。
            {
                QByteArray big(8 * 1024 * 1024, '\0');
                for (int index = 0; index < big.size(); index += 4096)
                {
                    big[index] = static_cast<char>(index >> 12);
                }
                auto widget = MakeFacade(0, big, false, QSize(900, 400));
                HexFindBar& bar = *ViewOf(*widget)->findBar();
                bar.setMode(HexFindBar::Mode::Hex);
                bar.setPatternText(QStringLiteral("DE AD BE EF 01 02 03 04 05"));
                const bool started = bar.findNext();
                CHECK(started);
                const QPointer<HexView> viewGuard(ViewOf(*widget));
                widget.reset();
                CHECK(viewGuard.isNull());
                PumpFor(100);
            }

            // 槽内 deleteLater：byteEdited 的槽里请求销毁自己（宿主页面被关闭的典型时序），事件循环里安全销毁。
            {
                auto owned = MakeFacade(0x1000, MakePattern(1024), true, QSize(900, 400));
                HexEditorWidget* raw = owned.release();
                const QPointer<HexEditorWidget> guard(raw);
                QObject::connect(raw, &HexEditorWidget::byteEdited, raw,
                    [raw](std::uint64_t, std::uint8_t, std::uint8_t) { raw->deleteLater(); });
                CanvasOf(*raw)->setFocus();
                ClickAddress(*CanvasOf(*raw), 0x1020);
                Type(*CanvasOf(*raw), HexByteText(OtherByte(ByteValue(MakePattern(1024), 0x20), 0x66)));
                CHECK(!guard.isNull());
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QApplication::processEvents();
                CHECK(guard.isNull());
            }

            // 菜单对象随控件销毁：画布构造的右键菜单以画布为父对象。
            {
                auto widget = MakeFacade(0x1000, MakePattern(1024), false, QSize(900, 400));
                QMenu* menu = CanvasOf(*widget)->buildContextMenu(0x1010, true);
                const QPointer<QMenu> menuGuard(menu);
                CHECK(!menuGuard.isNull());
                widget.reset();
                CHECK(menuGuard.isNull());
            }
        }
    }

    // 第三组：宿主使用形态与销毁安全。
    void RunFacadeHostTests()
    {
        TestClassChain();
        TestFileDockShape();
        TestDiskEditorShape();
        TestLifetime();
    }
}
