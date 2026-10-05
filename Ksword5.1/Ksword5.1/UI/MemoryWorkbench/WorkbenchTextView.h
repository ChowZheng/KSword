#pragma once

// ============================================================
// WorkbenchTextView.h
// 作用：
// - 内存工作台 Phase 3 的"文本"子页（WP-H）。把一段叠加后的字节按 ANSI/UTF-8/UTF-16LE
//   解码成文本，用项目内置 CodeEditorWidget 只读展示（AGENTS.md：文本/日志/返回内容类窗口
//   必须优先使用 CodeEditorWidget，不用 QPlainTextEdit）。目标内容是用户数据，不是程序界面
//   文案，因此必须走 CodeEditorWidget::setRawText（不翻译、LanguageChange 时不重绘）。
// - 数据来源同样是 IWorkbenchBytesProvider（见 WorkbenchDisasmView.h），本文件不做任何 I/O、
//   不持有目标；只读取一次 FetchWindow 的结果用于展示。
//
// 行宽跟随十六进制页：
// - setBytesPerRow 由宿主在十六进制页改变行宽时同步调用。解码按"每 bytesPerRow 个原始字节
//   为一行"独立切块、独立解码（与旧 MemoryEditorWidget.cpp:657 的按 16 字节分行解码同一惯例），
//   这样文本页的换行位置始终与十六进制页的行对齐，代价是极少数情况下一个多字节字符恰好跨越
//   行边界时该字符在两侧各显示为占位符（旧代码在 UTF-16 分支已有同样的已知限制）。
//
// 三种编码（本节已按第二轮审核的"头文件注释过期"更正，与 .cpp 实现逐条对齐）：
// - ANSI：只认可见 ASCII（0x20..0x7E）原样显示，其余字节（含 0x80..0xFF 的 Latin-1 扩展区、
//   0x00..0x1F/0x7F 控制区）统一显示为点号——不是 Latin-1 整段映射，不会画出 'ÿ' 这类真实
//   的扩展字符（D11c）；
// - UTF-8：手写的小型状态机逐字符解码（不用 QStringDecoder），因为需要按 validMask 逐字节
//   判断有效性，并区分三种失败原因：引导字节/续体字节"压根没读到"用不可读占位符；引导字节
//   不合法，或续体字节都读到了但形态不是 10xxxxxx（"可读但非法"），用 U+FFFD（替换字符）；
//   序列被这段字节的末尾截断（缺续体）也算数据缺口，归不可读，不是编码错误（D11b）。
//   非 BMP（> U+FFFF）码位目前统一显示为点号（已知简化，未处理代理对）；
// - UTF-16LE：一个无效/不可读字节会让所在的码元整体显示为不可读占位符，奇数尾字节单独处理
//   （可读则点号，不可读则占位符）；Zl/Zp（行/段分隔符）与 Cf（格式字符）一律退化成点号，
//   不当作"可打印"原样输出，避免 CodeEditorWidget 把它们当真换行拆散逻辑行（D11a）。
//   N7（第二轮审核）：行宽按 bytesPerRow 对齐切块是为了让换行位置跟十六进制页一致，与
//   UTF-16 的 2 字节码元边界是两套独立的网格；窗口起点不是偶数时，首行按对齐边界裁短后
//   可能是奇数长度，这会让从第二行开始的所有后续行的码元配对整体错一位。DecodeTextChunkForTest
//   新增的 oddLeadingByte 参数就是用来修这个：调用方按"本块相对窗口起点的字节偏移是否为奇数"
//   传入，为真时先把块内第一个字节当成上一块末尾被拆开的那个码元的后半（与"奇数尾字节"同一
//   惯例处理），再从第二个字节开始正常两两配对，让错位不会向后传染。
// - 不可读字节统一用 U+00D7（乘号）占位，与 HexCanvasFormat 的"不可读"字形一致；
//   可读但不可打印的字节用 '.'（与旧代码一致，这是"看不出字符"而不是"没有数据"）。
// ============================================================

#include "WorkbenchDisasmView.h"

#include <QWidget>

#include <cstdint>
#include <vector>

class CodeEditorWidget;
class QComboBox;
class QLabel;

namespace ks::ui
{
    // WorkbenchTextView：文本子页，顶部编码选择条 + CodeEditorWidget 只读内容区。
    class WorkbenchTextView final : public QWidget
    {
        Q_OBJECT

    public:
        // Encoding：三种解码方式，与持久化设置 memwb/workbench/text/encoding 的取值一一对应。
        enum class Encoding : int
        {
            Ansi = 0,
            Utf8 = 1,
            Utf16LE = 2
        };

        explicit WorkbenchTextView(QWidget* parent = nullptr);

        // setBytesProvider：设置数据源（非拥有）；传空等价于清空视图。
        void setBytesProvider(IWorkbenchBytesProvider* provider);

        // setWindow：设置要展示的字节区间（通常跟随十六进制页当前可见范围）。
        void setWindow(std::uint64_t address, std::uint64_t length);

        // setBytesPerRow：改变换行宽度（跟随十六进制页，见文件头）；只接受 >=1。
        void setBytesPerRow(int bytesPerRow);

        // bytesPerRow：当前换行宽度。
        int bytesPerRow() const;

        // setEncoding：切换解码方式；真的变了才重新渲染。
        void setEncoding(Encoding encoding);

        // encoding：当前解码方式。
        Encoding encoding() const;

        // refreshView：数据源内容变化后由宿主调用，重新拉取并渲染。
        void refreshView();

        // editor：供宿主/夹具做只读诊断查询（例如截图前确认内容），不对外转移所有权。
        CodeEditorWidget* editor() const;

        // minimumSizeHint（Wave 3 修复缺陷 1 增量，任务书明确允许的最小修改：
        // 只改这一个覆盖，不动其它任何行为）：QWidget 默认把 minimumSizeHint
        // 委托给自己的 layout()，而 m_editor（CodeEditorWidget）内部的编辑区域
        // 对长行/宽内容会报出一个相当大的首选宽度，一路向上传播会让装配本页
        // 的 QStackedWidget（进而宿主顶层窗口）被钉在一个拖不动的下限上——与
        // WorkbenchHexPane::minimumSizeHint 同一处理方式（见该函数注释）：
        // 内容本身可以横向滚动，不应该让宿主窗口因为本页内部编辑区域的尺寸
        // 偏好被钉死。
        QSize minimumSizeHint() const override;

    private:
        // kMaxWindowBytes：单次展示的字节数防御上限（1 MiB），避免宿主传入过大区间时卡顿。
        static constexpr std::uint64_t kMaxWindowBytes = 1024ULL * 1024ULL;

        // rebuildText：按当前窗口、编码、行宽重新渲染；无数据源/未设窗口时显示占位状态。
        void rebuildText();

        IWorkbenchBytesProvider* m_provider = nullptr;   // 数据源（非拥有）
        QComboBox* m_encodingCombo = nullptr;             // 编码选择
        QLabel* m_status = nullptr;                       // 状态行（窗口范围、截断提示）
        CodeEditorWidget* m_editor = nullptr;              // 只读内容区
        std::uint64_t m_address = 0;                       // 当前窗口起始地址
        std::uint64_t m_length = 0;                        // 当前窗口长度
        bool m_hasWindow = false;                          // 是否已经设置过窗口
        int m_bytesPerRow = 16;                            // 换行宽度
        Encoding m_encoding = Encoding::Ansi;               // 当前编码
    };

    // DecodeTextChunkForTest：解码单个"行"（一段字节 + 对应有效掩码），导出供夹具直接测试
    // 三种编码与 validMask 的交互，不依赖 CodeEditorWidget 或任何界面状态。
    // 传入：原始字节、等长有效掩码（1=有效）、编码、oddLeadingByte；传出：该行的解码文本
    // （不含换行符）。
    // oddLeadingByte：N7（第二轮审核）——只影响 UTF-16LE：为真表示这一块在真正的码元网格上
    // 起点是奇数偏移（即上一块末尾被拆开的某个码元，这一块的第一个字节正是它的后半），此时
    // 先把第一个字节当成孤立的奇数尾字节处理（可读则点号、不可读则占位符），再从第二个字节
    // 开始正常两两配对；默认 false（偶数起点，按原有逻辑从第一个字节直接开始配对），不影响
    // 旧调用点与旧夹具用例。ANSI/UTF-8 两条路径忽略本参数（它们没有"码元边界"这个维度）。
    QString DecodeTextChunkForTest(
        const std::vector<std::uint8_t>& bytes,
        const std::vector<std::uint8_t>& validMask,
        WorkbenchTextView::Encoding encoding,
        bool oddLeadingByte = false);
}
