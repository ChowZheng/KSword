// memwb_ui_inspector.cpp
// 作用：HexInspectorPanel 离屏夹具的公共设施实现（场景、测试数据、输入模拟）与总入口 RunInspectorTests。
// 见 memwb_ui_inspector.h。

#include "memwb_ui_inspector.h"

#include <QApplication>
#include <QDir>
#include <QLineEdit>

#include <algorithm>
#include <initializer_list>
#include <iostream>

namespace memwb_test
{
    namespace
    {
        // Put：把一组字节写进数据的指定偏移。
        void Put(QByteArray& data, int offset, std::initializer_list<int> values)
        {
            int index = offset;
            for (const int value : values)
            {
                data[index] = static_cast<char>(value);
                ++index;
            }
        }

        // PutText：把一段 Latin-1 文本写进数据的指定偏移（不含结尾 NUL，由调用方另行放置）。
        void PutText(QByteArray& data, int offset, const QByteArray& text)
        {
            for (int index = 0; index < text.size(); ++index)
            {
                data[offset + index] = text.at(index);
            }
        }
    }

    // 假命名器：只认一个地址。
    bool FakeNamer::Describe(std::uint64_t address, std::string& out)
    {
        ++calls;
        if (address == kKnownAddress)
        {
            out = "kernel32.dll+0x1A40";
            return true;
        }
        if (address == kParenAddress)
        {
            out = "ntdll!Foo (bar)";
            return true;
        }
        return false;
    }

    // 设置 INI 路径。
    QString InspectorScene::settingsPath() const
    {
        return settingsDir.filePath(QStringLiteral("inspector.ini"));
    }

    // 测试数据。偏移表：
    //   0x10 整数：78 56 34 12 EF CD AB 90 | 01..08
    //   0x20 ASCII：十六个可见字符 "Hello, KSword!!!"（没有结束符，用于验证截断提示）
    //   0x30 ASCII："Hi" + NUL + 不可见字节（有结束符）
    //   0x40 指针：40 1A 34 12 F6 7F 00 00（= kKnownAddress）+ 8 个字节
    //   0x50 浮点：00 00 C0 3F（f32 1.5）
    //   0x60 FILETIME：2024-01-01 00:00:00 UTC（0x01DA3C457689C000 的小端字节）
    //   0x70 GUID：33 22 11 00 55 44 77 66 88 99 AA BB CC DD EE FF
    //   0x80 UTF-16LE："AB" + 汉字"字" + NUL
    //   0x90 含 HTML 标记的 ASCII："<b>x</b>"（验证悬停提示按纯文本显示）
    //   0xA0 描述含括号的指针：23 01 00 00 F6 7F 00 00（= kParenAddress）+ 8 个字节
    //   0xB0 全 FF：验证有符号数显示 -1 且位模式是 0xFF..FF
    QByteArray MakeInspectorData()
    {
        QByteArray data = MakePattern(0x2000);
        Put(data, 0x10, { 0x78, 0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x90, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 });
        PutText(data, 0x20, QByteArray("Hello, KSword!!!"));
        PutText(data, 0x30, QByteArray("Hi"));
        Put(data, 0x32, { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D });
        Put(data, 0x40, { 0x40, 0x1A, 0x34, 0x12, 0xF6, 0x7F, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 });
        Put(data, 0x50, { 0x00, 0x00, 0xC0, 0x3F, 0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xB9, 0x3F, 0x01, 0x02, 0x03, 0x04 });
        Put(data, 0x60, { 0x00, 0xC0, 0x89, 0x76, 0x45, 0x3C, 0xDA, 0x01, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C });
        Put(data, 0x70, { 0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF });
        Put(data, 0x80, { 0x41, 0x00, 0x42, 0x00, 0x57, 0x5B, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 });
        PutText(data, 0x90, QByteArray("<b>x</b>"));
        Put(data, 0x98, { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 });
        Put(data, 0xA0, { 0x23, 0x01, 0x00, 0x00, 0xF6, 0x7F, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 });
        Put(data, 0xB0, { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF });
        return data;
    }

    // 把场景的公共部分搭好：splitter + 画布 + 面板，面板用场景自己的设置文件。
    // 传入：场景、是否可编辑、是否挂叠加层、尺寸、面板宽度。
    static void BuildSceneWidgets(
        InspectorScene& scene,
        bool editable,
        bool withOverlay,
        const QSize& size,
        int panelWidth)
    {
        scene.splitter = std::make_unique<QSplitter>(Qt::Horizontal);
        scene.canvas = new ks::ui::HexCanvas(scene.splitter.get());
        scene.panel = new ks::ui::HexInspectorPanel(scene.splitter.get());
        scene.splitter->addWidget(scene.canvas);
        scene.splitter->addWidget(scene.panel);
        scene.splitter->setStretchFactor(0, 1);
        scene.splitter->setStretchFactor(1, 0);
        scene.splitter->resize(size);
        scene.splitter->show();
        scene.splitter->setSizes({ std::max(200, size.width() - panelWidth), panelWidth });

        // 面板先指到场景自己的 INI（保证每个场景的设置互不干扰，也不碰注册表），再设命名器。
        scene.panel->setSettingsFile(scene.settingsPath());
        scene.panel->setPointerNamer(&scene.namer);
        scene.canvas->setEditable(editable);
        if (withOverlay)
        {
            scene.canvas->setOverlay(&scene.overlay);
        }
    }

    // 静态数据场景。
    std::unique_ptr<InspectorScene> MakeInspectorScene(
        const QByteArray& data,
        bool editable,
        bool withOverlay,
        const QSize& size,
        int panelWidth)
    {
        auto scene = std::make_unique<InspectorScene>();
        scene->data = data;
        BuildSceneWidgets(*scene, editable, withOverlay, size, panelWidth);
        scene->canvas->setStaticData(kInspectorBase, data);

        // 叠加层基线：与静态数据逐字节一致，掩码全 1。
        if (withOverlay)
        {
            const std::vector<std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(data.constData()),
                reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size());
            const std::vector<std::uint8_t> mask(bytes.size(), 1);
            scene->overlay.LoadBaseline(std::string("memwb-inspector-test"), kInspectorBase, bytes, mask);
        }

        // 最后才关联画布：此时画布已有数据，面板的首次读取就是真实内容。
        scene->panel->setCanvas(scene->canvas);
        QApplication::processEvents();
        return scene;
    }

    // 异步场景：地址空间 4 页，不自动供页，页由测试手动投递。
    std::unique_ptr<InspectorScene> MakeAsyncInspectorScene(bool editable, const QSize& size, int panelWidth)
    {
        auto scene = std::make_unique<InspectorScene>();
        BuildSceneWidgets(*scene, editable, true, size, panelWidth);
        scene->canvas->setAddressSpace(kInspectorBase, kInspectorBase + 0x3FFFULL);
        scene->canvas->setPageProvider(&scene->provider);
        scene->panel->setCanvas(scene->canvas);
        QApplication::processEvents();
        return scene;
    }

    // 按类型键找行号。
    int RowIndexOf(const ks::ui::HexInspectorPanel& panel, const QString& typeKey)
    {
        const ks::ui::HexInspectorRowView* rows = panel.rowView();
        for (int index = 0; index < rows->rowCount(); ++index)
        {
            if (rows->rowAt(index).typeKey == typeKey)
            {
                return index;
            }
        }
        return -1;
    }

    // 按类型键取行数据。
    const ks::ui::HexInspectorRowData& RowOf(const ks::ui::HexInspectorPanel& panel, const QString& typeKey)
    {
        return panel.rowView()->rowAt(RowIndexOf(panel, typeKey));
    }

    // 滚动到可见后点击。
    void ClickAddress(
        ks::ui::HexCanvas& canvas,
        std::uint64_t address,
        ks::ui::HexCanvas::ActivePane pane,
        Qt::KeyboardModifiers modifiers)
    {
        if (canvas.cellRect(address, pane).isNull())
        {
            canvas.scrollToAddress(address, ks::ui::HexCanvas::ScrollAlign::Center);
            QApplication::processEvents();
        }
        Click(canvas, address, pane, modifiers);
    }

    // 真实双击：点在值列的单元格里（避开行尾的复制图标）。
    void DoubleClickRow(ks::ui::HexInspectorPanel& panel, const QString& typeKey)
    {
        ks::ui::HexInspectorRowView* rows = panel.rowView();
        const int row = RowIndexOf(panel, typeKey);
        const QPoint position = rows->valueCellRect(row).center();
        QTest::mouseDClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, position);
    }

    // 向编辑器键入文字：先全选，再逐字符发送，等价于用户覆盖输入。
    void TypeIntoEditor(ks::ui::HexInspectorPanel& panel, const QString& text)
    {
        QLineEdit* editor = panel.rowView()->editor();
        if (editor == nullptr)
        {
            return;
        }
        editor->selectAll();
        QTest::keyClicks(editor, text);
    }

    // 向编辑器发一次按键。
    void PressInEditor(ks::ui::HexInspectorPanel& panel, Qt::Key key)
    {
        QLineEdit* editor = panel.rowView()->editor();
        if (editor == nullptr)
        {
            return;
        }
        QTest::keyClick(editor, key);
    }

    // 处理事件并执行延迟删除。
    void FlushDeferred()
    {
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    }

    // 总入口：视图类、编辑类、截图与基准依次执行。
    void RunInspectorTests(const QString& shotsDir)
    {
        // 记下进入前的断言数，结束时单独报告面板这一部分执行了多少条断言与失败。
        const int checksBefore = g_checks;
        const int failuresBefore = g_failures;
        RunInspectorViewTests();
        RunInspectorEditTests();
        RunInspectorShotsAndBench(shotsDir);
        std::cout << "inspector tests: " << (g_checks - checksBefore) << " checks, "
                  << (g_failures - failuresBefore) << " failures" << std::endl;
    }
}
