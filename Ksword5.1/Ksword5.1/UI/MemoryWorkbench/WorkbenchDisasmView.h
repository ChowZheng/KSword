#pragma once

// ============================================================
// WorkbenchDisasmView.h
// 作用：
// - 内存工作台 Phase 3 的"反汇编"子页（见 docs/内存工作台Phase3集成设计.md 的 WP-H）。
//   取代旧 MemoryEditorWidget 里基于 QTableWidget 的指令表，改用 QTableView + 自带模型，
//   数据来源统一走只读接口 IWorkbenchBytesProvider，本文件及其配套 .cpp **不做任何 I/O、
//   不持有任何目标句柄**——叠加后的字节（含暂存补丁）由外部喂入，写入也只通过信号交出去，
//   真正的读/写由宿主（MemoryWorkbenchView 及其下的写事务）负责。
//
// ------------------------------------------------------------
// 一、只读数据源接口（本页定义，供反汇编/文本/对比三个子页共用）
// ------------------------------------------------------------
// - WorkbenchByteWindow 对应 shared/evidence/memory_workbench/MemoryDiffOverlay 的三套读数：
//     bytes/validMask         = Materialize()，叠加暂存补丁后的"现在看到的值"；
//     baselineBytes/...Mask   = BaselineByte()，未叠加补丁的目标基线值（Pending 的"旧值"）；
//     previousBytes/...Mask   = PreviousByte()，上一次读取的值（ExternalChange 的"旧值"）；
//     changeKinds             = ChangeKind()，逐字节变化种类，直接决定着色与对比页分组。
// - IWorkbenchBytesProvider 是三个子页唯一的数据来源；实现者（宿主）负责把它接到真正的
//   MemoryDiffOverlay 实例上。本接口只读，没有任何写方法。
//
// ------------------------------------------------------------
// 二、解码/汇编后端用依赖注入，不在本文件内直接链接 Zydis 或语言包
// ------------------------------------------------------------
// - DecodeOneFn / AssembleOneFn 把"调用哪个具体解码器/汇编器"的决定留给调用方：
//     生产环境应传入包装 ks::ui::InstructionDecoder::decode（KernelDisassemblyDialog.h）
//     与 ks::ui::InstructionAssembler::assemble（MemoryAssembly.h）的实现；
//     离屏夹具改传直接调用 third_party/zydis 的实现，两端共用的是本文件里的重同步/边界算法，
//     不是具体后端——这样本组件保持自包含（不 include KernelDisassemblyDialog.h /
//     MemoryAssembly.h，也就不会把 ArkDriverClient、语言包这些重依赖带进链接）。
// - 未设置后端时视图显示"未设置解码/汇编后端"状态，不崩溃、不猜测。
//
// ------------------------------------------------------------
// 三、Zydis 解不了时的重同步（修旧缺陷 E-02：不得整段降级到残缺解码器）
// ------------------------------------------------------------
// - DecodeWindowResynced 是本文件导出的纯函数：对一段已知"全部有效"的字节，逐条调用
//   decodeOne；某个位置失败时**只把那一个字节标成 db**，从下一个字节重新调用 decodeOne
//   （天然重新同步），不会像旧代码一样一旦失败就整段切到不区分指令边界的降级解码器。
// - 调用方负责先把字节截到"已读取窗口"内（遇到 validMask=0 就停），本函数不处理有效性。
//
// ------------------------------------------------------------
// 四、交互（ux.md 4.2）
// ------------------------------------------------------------
// - 单击只选择；双击/F2 或"行未在编辑且操作数不是单个可跟随地址时"按 Enter 才进入行内编辑；
//   操作数是单个立即数/绝对地址时 Enter 改为跟随跳转（后退栈 64 项，Backspace 返回）。
// - 行内编辑：Enter 编译并提交（失败不关编辑框，原因显示在编辑框下方，不带"第 1 行"前缀）；
//   新机器码不得超过原指令字节数，更短则用 0x90 (NOP) 补齐，保持后续指令边界不变。
// - 右键"汇编编辑"：预览对话框，可调整覆盖长度与 NOP 填充、校验指令边界（不变式 10），
//   内核流程参考旧 MemoryEditorWidget.cpp:821-937，但本页用自己的类实现，不依赖旧控件。
// - 两种提交路径最终都发 stageRequested(address, bytes)，由宿主调用
//   MemoryDiffOverlay::Stage，本组件自己绝不写任何内存。
//
// ------------------------------------------------------------
// 五、自包含与文件分工
// ------------------------------------------------------------
// - 不包含 Framework.h；只依赖 Qt Core/Gui/Widgets、theme.h、HexViewWidgets.h（分段按钮）、
//   HexCanvasFormat.h（地址/字节文本格式化）与 shared/evidence/memory_workbench。
// - WorkbenchDisasmView.cpp：模型、布局、数据流、导航栈、右键菜单（非编辑项）。
// - WorkbenchDisasmView.Edit.cpp：行内编辑委托、汇编预览对话框。
// ============================================================

#include "../../../../shared/evidence/memory_workbench/MemoryDiffOverlay.h"

#include <QAbstractTableModel>
#include <QByteArray>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class QEvent;
class QKeyEvent;
class QLabel;
class QPoint;
class QRect;
class QTableView;

namespace ks::ui
{
    class HexViewSegmented;

    // WorkbenchByteWindow：一次 FetchWindow 的结果，见文件头"一"。
    struct WorkbenchByteWindow
    {
        bool ok = false;                                            // false 表示该区间此刻完全不可用（已跳出已读取窗口等）
        std::uint64_t address = 0;                                  // 窗口起始地址
        std::vector<std::uint8_t> bytes;                            // 现值（含暂存补丁）
        std::vector<std::uint8_t> validMask;                        // 现值是否有效，1=有效
        std::vector<std::uint8_t> baselineBytes;                    // 基线值（未叠加补丁）
        std::vector<std::uint8_t> baselineValidMask;                // 基线值是否有效
        std::vector<std::uint8_t> previousBytes;                    // 上次读取值
        std::vector<std::uint8_t> previousValidMask;                // 上次读取值是否有效
        std::vector<ksword::memwb::ByteChangeKind> changeKinds;     // 逐字节变化种类，与 bytes 等长
    };

    // IWorkbenchBytesProvider：反汇编/文本/对比三页共用的只读数据源，见文件头"一"。
    //
    // 生命周期契约（可疑点 4）：三个子页只保存裸指针（非拥有），不做任何引用计数或
    // QPointer 包裹——本接口不是 QObject，无法用 QPointer 自动探活。调用方（宿主）必须
    // 保证：provider 的生存期覆盖它被设置（setBytesProvider）之后、到下一次
    // setBytesProvider(另一个指针或 nullptr) 之前的全部时间；销毁 provider 前必须先对
    // 每个仍持有它的子页调用 setBytesProvider(nullptr)，否则下一次 refreshView/jumpTo
    // 等触发的 FetchWindow 调用会是悬空指针解引用。
    class IWorkbenchBytesProvider
    {
    public:
        virtual ~IWorkbenchBytesProvider() = default;

        // FetchWindow：取 [address, address+length) 的叠加字节快照（同步调用，不做真正 I/O，
        // 只是从宿主已经持有的 MemoryDiffOverlay 读出来）。length 为 0 时返回 ok=true 的空窗口。
        virtual WorkbenchByteWindow FetchWindow(std::uint64_t address, std::uint64_t length) const = 0;

        // AddressBits：当前会话位数（32 或 64），作为反汇编页默认架构分段的依据。
        virtual int AddressBits() const = 0;

        // HasPreviousRead：对比页的"两次读取之间"分组是否有数据可比（没有上次读取时整段隐藏）。
        virtual bool HasPreviousRead() const = 0;
    };

    // DecodedRow：反汇编单行，既可能是真解码的指令，也可能是重同步插入的单字节 db 占位。
    struct DecodedRow
    {
        std::uint64_t address = 0;      // 本行起始地址
        QByteArray bytes;               // 本行覆盖的原始字节
        QString mnemonic;               // 指令助记符；db 占位行固定为 "db"
        QString operands;               // 操作数文本；db 占位行是该字节的十六进制
        bool decoded = false;           // 是否被解码器识别（db 占位行恒为 false）
    };

    // WorkbenchAssembleResult：一次单指令汇编的结果，与 ks::ui::AssemblyResult 字段对应。
    struct WorkbenchAssembleResult
    {
        bool success = false;   // 是否编译成功
        QByteArray bytes;       // 成功时的机器码；失败时为空
        QString error;          // 失败时的原因（不带"第 N 行"前缀，调用方是单行编辑器）
        // errorLine：D6——失败时的源码行号（1 基）；来源跟 ks::ui::AssemblyResult::errorLine
        // 一样由汇编后端给出，宿主注入的后端负责透传，不在这里猜测。行内编辑路径永远是
        // 单行源码，不展示行号前缀；只有预览对话框（多行源码）会用它。0 表示后端没有给出
        // （旧后端/夹具假后端），调用方应当把它当成"第 1 行"而不是显示 "第 0 行"。
        int errorLine = 0;
    };

    // DecodeOneFn：尝试解码 [address, address+available) 的第一条指令。
    // 传入：起始字节指针、可用字节数、绝对地址、是否 x64；传出：解码结果（nullopt 表示该后端
    // 在此处失败，调用方据此退化为单字节 db 并前进一个字节重试）。绝不抛异常。
    using DecodeOneFn = std::function<std::optional<DecodedRow>(
        const std::uint8_t* bytes, std::size_t available, std::uint64_t address, bool x64)>;

    // AssembleOneFn：把一行 Intel 汇编源码编译成机器码。
    using AssembleOneFn = std::function<WorkbenchAssembleResult(const QString& source, std::uint64_t address, bool x64)>;

    // DecodeWindowResynced：核心重同步算法，见文件头"三"；纯函数，供生产代码与离屏夹具共用。
    // 传入：已确认全部有效的字节、这段字节的起始地址、单条解码回调、最多解码的行数上限、是否 x64。
    // 传出：解码行列表，真实指令与 db 占位行混排，覆盖范围之和恰好等于 bytes.size()
    //       （除非达到 maxInstructions 提前停止）。
    QVector<DecodedRow> DecodeWindowResynced(
        const std::vector<std::uint8_t>& bytes,
        std::uint64_t baseAddress,
        const DecodeOneFn& decodeOne,
        std::uint32_t maxInstructions,
        bool x64);

    // WorkbenchDisasmModel：反汇编表格的模型，四列：地址/字节/助记符/操作数。
    // 行数据来自宿主调用 setRows 时一次性整体替换（窗口有界，通常几百行以内）。
    class WorkbenchDisasmModel final : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        explicit WorkbenchDisasmModel(QObject* parent = nullptr);

        // setRows：整体替换显示的行；rowKinds 与 rows 等长，给出每行的底色变化种类
        // （Unchanged 表示不着色）；endOfWindowNote 非空时追加一条"超出已读取窗口"提示行
        // （decoded=false，mnemonic 即该提示文案，bytes 为空，提示行不计入 rowKinds）。
        void setRows(
            const QVector<DecodedRow>& rows,
            const QVector<ksword::memwb::ByteChangeKind>& rowKinds,
            const QString& endOfWindowNote);

        // rowAt：取某一行的数据；越界返回 nullopt。
        std::optional<DecodedRow> rowAt(int row) const;

        // isEndOfWindowRow：该行是不是"超出已读取窗口"提示行（不可编辑、不可跟随）。
        bool isEndOfWindowRow(int row) const;

        int rowCount(const QModelIndex& parent = QModelIndex()) const override;
        int columnCount(const QModelIndex& parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& index, int role) const override;
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
        Qt::ItemFlags flags(const QModelIndex& index) const override;

    private:
        QVector<DecodedRow> m_rows;                                  // 当前显示的行
        QVector<ksword::memwb::ByteChangeKind> m_rowKinds;           // 与 m_rows 等长的底色变化种类
        QString m_endOfWindowNote;                                   // 非空表示末尾追加了一条提示行
    };

    // WorkbenchDisasmView：反汇编子页，组合分段按钮、表格与状态行。
    class WorkbenchDisasmView final : public QWidget
    {
        Q_OBJECT

    public:
        explicit WorkbenchDisasmView(QWidget* parent = nullptr);
        ~WorkbenchDisasmView() override;

        // setBytesProvider：设置数据源（非拥有）；传空等价于清空视图。
        void setBytesProvider(IWorkbenchBytesProvider* provider);

        // setDecodeBackend / setAssembleBackend：注入解码/汇编后端，见文件头"二"。
        void setDecodeBackend(DecodeOneFn backend);
        void setAssembleBackend(AssembleOneFn backend);

        // setAddressBits：外部覆盖默认架构（物理范围需要手选，不跟随 provider 的位数）。
        // D3：只有"用户点了分段按钮"才算显式覆盖；程序化调用本函数不算覆盖，且目标切换
        // （setBytesProvider 换了一个不同的 provider）会清掉上一个目标下的覆盖，重新按新
        // provider->AddressBits() 取默认值——否则 64 位目标会被卡死在上一个 32 位目标的选择上。
        // 可疑点 2（第二轮审核，文档修正）：上一句"外部覆盖默认架构"只在 m_x64Override 还
        // 没被置位时成立——用户已经手动点过分段钮之后，本函数会静默什么都不做（见 .cpp
        // 实现里的 if (!m_x64Override) 判断），直到目标切换把覆盖清掉为止。这是 D3"用户
        // 显式选择优先"设计本身的结果，不是缺陷；这里只是把文档改到跟实现一致，宿主如果
        // 需要在用户已覆盖的情况下仍然强制指定架构（例如物理范围场景下没有"用户覆盖"这个
        // 概念），应该先确认这条约束是否适用，必要时再补一个显式的"强制覆盖"入口。
        void setAddressBits(int bits);

        // isX64：当前分段按钮选中的架构。
        bool isX64() const;

        // isEditable：当前是否允许编辑（见 setEditable）。
        bool isEditable() const;

        // setEditable：可疑点 1——切换本页是否允许编辑；宿主在只读通道/写入忙时应调用
        // false，行内编辑（F2/Enter/双击）与右键"汇编编辑"菜单项据此禁止/置灰并在菜单
        // 提示里说明原因；传 false 时若正有编辑框打开，立即结束（Esc 语义，不提交）。
        void setEditable(bool editable);

        // jumpTo：外部跳转入口（地址栏 / Ctrl+D 在反汇编页打开插入点）；会记入后退栈。
        // 传出：false 表示没有数据源。
        bool jumpTo(std::uint64_t address);

        // refreshView：重新从数据源拉取当前窗口并重新解码（F5、暂存变化后由宿主调用）。
        // D2：若此刻正有行内编辑框打开，不会销毁它——真正的刷新推迟到编辑结束后才做，
        // 只记一个"待刷新"标志（见 m_refreshPending），避免 QAbstractItemView::reset()
        // 静默吞掉用户正在输入的内容、且不发 closeEditor 信号导致 m_editingActive 卡死。
        void refreshView();

        // reset：可疑点 5——显式清空视图到"尚未定位"的初始状态：锚点、后退栈、暂存的刷新
        // 请求全部清掉。换目标时如果宿主想要真正的"空白"（而不是沿用上一个目标残留的锚点
        // /后退栈），应显式调用本函数，而不是只调用 setBytesProvider(nullptr)。
        void reset();

        // anchorAddress：当前窗口起始地址。
        std::uint64_t anchorAddress() const;

        // table / model：供夹具与宿主做诊断查询；不对外公开所有权。
        QTableView* table() const;
        WorkbenchDisasmModel* model() const;

        // isEditing：当前是否有行内编辑器打开。QAbstractItemView::state()/EditingState 是
        // protected 成员，宿主与夹具都够不到，这里单独暴露一个可查询的布尔状态。
        bool isEditing() const;

        // minimumSizeHint（Wave 3 修复缺陷 1 增量，任务书明确允许的最小修改：
        // 只改这一个覆盖，不动其它任何行为）：m_table（四列指令表）默认的
        // minimumSizeHint 会按四列各自的最小列宽求和，一路向上传播会让装配
        // 本页的 QStackedWidget（进而宿主顶层窗口）被钉在一个拖不动的下限上
        // ——与 WorkbenchHexPane::minimumSizeHint 同一处理方式（见该函数注释）：
        // 表格本身支持横向滚动，不应该让宿主窗口因为本页内部表格的列宽偏好
        // 被钉死。
        QSize minimumSizeHint() const override;

    public slots:
        // navigateBack：Backspace 的槛外入口，供宿主的全局快捷键转发。
        void navigateBack();

    signals:
        // stageRequested：一次编辑（行内或预览对话框）已编译成功，交由宿主暂存，本组件不写内存。
        void stageRequested(quint64 address, QByteArray bytes);

        // requestHexLocate：右键"在十六进制视图中定位"。
        void requestHexLocate(quint64 address);

        // statusMessage：状态行文字变化，供宿主统一状态条订阅（可选）。
        void statusMessage(const QString& text);

    protected:
        // eventFilter：装在 m_table 上，拦截 F2/Enter/Backspace 实现"单击只选择，
        // 双击/F2/Enter 才进入编辑；可跟随地址时 Enter 改为跳转"的分支逻辑（文件头"四"）。
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        // kDecodeWindowBytes：单次拉取并解码的字节数上限，控制表格对象数量级（O(可见行)）。
        static constexpr std::uint64_t kDecodeWindowBytes = 4096;
        // kLookaheadBytes：D9——额外多取的字节数（x86/x64 单条指令最长 15 字节），只用来
        // 正确判断"窗口末尾那条指令是否完整"，绝不把这段额外字节当作已展示的内容；
        // 没有它时，kDecodeWindowBytes 的硬切点恰好落在某条指令中间会被按"新指令"重同步
        // 解码，解出与原指令完全不相关的幻影指令（例如被截断的位移字节碰巧拼成另一条指令）。
        static constexpr std::uint64_t kLookaheadBytes = 15;
        // kMaxInstructionRows：单窗口最多解码的行数（防御：全部 1 字节 db 的极端输入）。
        static constexpr std::uint32_t kMaxInstructionRows = 1024;
        // kMaxBackStack：跟随跳转的后退栈容量。
        static constexpr int kMaxBackStack = 64;

        // rebuildRows：对外统一入口。D2——编辑中时只记"待刷新"标志并直接返回，不触碰模型，
        // 真正的刷新推迟到编辑结束（见 .cpp 里 closeEditor 的处理）；非编辑中直接转发给
        // rebuildRowsNow。
        void rebuildRows();

        // rebuildRowsNow：真正的刷新实现，按 m_anchor 从 provider 拉一次窗口并重新解码，
        // 刷新模型与状态行；刷新前后按地址保持选中行（D2 的另一半要求）。
        void rebuildRowsNow();

        // pushBackStack：把地址压入后退栈（跳转前调用，容量上限丢弃最旧的一项）。
        void pushBackStack(std::uint64_t address);

        // tryFollowOperand：行的操作数是否是单个可跟随的立即数/绝对地址，且助记符属于
        // call/jmp/条件跳转/loop/jrcxz 等分支类指令（可疑点 2）；传出命中的地址。
        bool tryFollowOperand(const DecodedRow& row, std::uint64_t* addressOut) const;

        // showContextMenu：右键菜单（复制地址/字节/整条指令、在十六进制视图中定位、汇编编辑）。
        void showContextMenu(const QPoint& viewportPos);

        // installEditDelegate：构造并安装行内编辑委托（一次性，构造函数调用）；定义在 .Edit.cpp。
        void installEditDelegate();

        // beginRowEdit：双击/F2/Enter 触发，进入行内编辑；定义在 .Edit.cpp。
        void beginRowEdit(int row);

        // showAssemblyPreviewDialog：右键"汇编编辑"；定义在 .Edit.cpp。D10——传入菜单打开时
        // 冻结的那一行快照（地址+原字节），对话框打开前会按地址重新核对一次，数据已变则
        // 放弃并提示，不会把编辑对话框打在刷新之后的另一条指令上。
        void showAssemblyPreviewDialog(const DecodedRow& expectedRow);

        // reportEditError：行内编辑委托编译失败时回调，显示到编辑框下方；定义在 .Edit.cpp 里设置。
        void showInlineEditError(const QRect& editorRect, const QString& message);

        // hideInlineEditError：行内编辑结束（提交或取消）时隐藏错误提示（D7）。
        void hideInlineEditError();

        // cancelInlineEdit：N3/N4（第二轮审核）——立即结束正在进行的行内编辑（Esc 语义，
        // 不提交），关闭持久编辑器、复位编辑中标志、清掉残留的错误提示框。供三个调用点
        // 共用：setEditable(false)、setBytesProvider 换目标、reset()。注意：本函数**不**
        // 处理 m_refreshPending——closePersistentEditor 不会像正常结束编辑那样发出委托的
        // closeEditor 信号，不会自动走到 .Edit.cpp 里补刷新的那条路径；调用方各自决定要不要
        // 在调用后自己补一次 rebuildRowsNow()（setEditable 需要，因为没有别的地方会再刷新；
        // setBytesProvider/reset 后面本来就会无条件 rebuildRows()，不需要再补）。定义见
        // .Edit.cpp（与 hideInlineEditError 等编辑生命周期函数放在一起）。
        void cancelInlineEdit();

        IWorkbenchBytesProvider* m_provider = nullptr;   // 数据源（非拥有，生命周期契约见头文件"一"）
        DecodeOneFn m_decodeOne;                          // 解码后端
        AssembleOneFn m_assembleOne;                      // 汇编后端
        HexViewSegmented* m_archSegmented = nullptr;      // x86/x64 分段按钮
        QTableView* m_table = nullptr;                    // 指令表
        WorkbenchDisasmModel* m_model = nullptr;           // 表格模型（本控件拥有，parent 关系释放）
        QLabel* m_status = nullptr;                        // 状态行（解码来源、行数、提示）
        QLabel* m_inlineError = nullptr;                   // 行内编辑错误提示（覆盖在编辑框下方）
        std::uint64_t m_anchor = 0;                        // 当前窗口起始地址
        bool m_hasAnchor = false;                          // 是否已经跳转过（没有则显示空状态）
        bool m_editingActive = false;                      // 是否有行内编辑器打开（见 isEditing）
        bool m_refreshPending = false;                     // D2：编辑期间被推迟的刷新请求
        bool m_editable = true;                             // 可疑点 1：是否允许编辑
        bool m_x64Override = false;                        // 是否存在外部显式覆盖的架构
        bool m_x64OverrideValue = true;                    // 外部覆盖的架构取值
        bool m_programmaticArchChange = false;              // D3：区分程序化 setCurrentIndex 与用户点击
        std::vector<std::uint64_t> m_backStack;            // 跟随跳转的后退栈
    };
}
