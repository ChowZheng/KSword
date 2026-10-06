#pragma once

// ============================================================
// WorkbenchStringWriteDialog.h
// 作用：
// - 画布右键"写入字符串…"对话框（ux.md 第 4.1 节）：输入一段文本，选编码
//   （ANSI/UTF-8/UTF-16LE），可勾选末尾写入 NUL 结束符，实时预览"将写入 N 字节"。
// - 本对话框只产出字节，不做任何 I/O：resultBytes() 取编码后的最终字节，写入
//   目标的管线仍是 HexCanvas::stageBytes -> MemoryWriteTransaction，不在本类里。
// - QDialog 背景在父容器使用透明/特殊样式时可能变黑底，按仓库规范显式设置
//   不透明的背景/文字/选中态/禁用态样式（实现在 .cpp 的 ApplyOpaqueDialogStyle）。
// ============================================================

#include <QByteArray>
#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace ks::ui
{
    // WorkbenchStringWriteDialog：写入字符串对话框。
    class WorkbenchStringWriteDialog final : public QDialog
    {
        Q_OBJECT

    public:
        // Encoding：三种受支持的编码，数值即 WorkbenchMessages::StringWriteEncodingLabel
        // 的下标，顺序固定。
        enum class Encoding
        {
            Ansi = 0,
            Utf8 = 1,
            Utf16Le = 2,
        };

        explicit WorkbenchStringWriteDialog(QWidget* parent = nullptr);

        // text / setText：对话框当前输入的文本。
        QString text() const;
        void setText(const QString& text);

        // encoding / setEncoding：当前选中的编码。
        Encoding encoding() const;
        void setEncoding(Encoding encoding);

        // appendNul / setAppendNul：是否在末尾追加编码对应的 NUL 结束符
        //（ANSI/UTF-8 为 1 个 0x00 字节，UTF-16LE 为 2 个 0x00 字节）。
        bool appendNul() const;
        void setAppendNul(bool appendNul);

        // resultBytes：按当前文本/编码/NUL 勾选编码出的最终字节；可在对话框
        // accept 之后调用，也可在测试里不弹框直接调用核对预览逻辑。
        QByteArray resultBytes() const;

        // previewText：当前"将写入 N 字节"的预览文字，供测试核对不必解析界面。
        QString previewText() const;

    private slots:
        // refreshPreview：文本/编码/NUL 勾选任一变化时重新计算预览与字节数，并按
        // B1/B10 的规则刷新"写入"按钮是否可点。
        void refreshPreview();

    private:
        // isAnsiLossy：B1——当前文本在 ANSI（本机代码页）编码下是否有损（往返
        // 校验：编码再解码回来是否还等于原文本）。非 ANSI 编码恒返回 false。
        // resultBytes()/previewText()/refreshPreview() 三处共用同一个判定，不重复
        // 实现判断逻辑。
        bool isAnsiLossy() const;

        QLineEdit* m_textEdit = nullptr;
        QComboBox* m_encodingCombo = nullptr;
        QCheckBox* m_appendNulCheckBox = nullptr;
        QLabel* m_previewLabel = nullptr;
        QPushButton* m_okButton = nullptr;
        QPushButton* m_cancelButton = nullptr;
    };
}
