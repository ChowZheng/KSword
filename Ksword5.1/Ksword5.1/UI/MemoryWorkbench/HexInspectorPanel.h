#pragma once

// ============================================================
// HexInspectorPanel.h
// 作用：
// - 内存工作台的"数据解释器"面板，常驻在 HexCanvas 右侧（宽度由宿主的 QSplitter 决定，可拖动）。
//   跟随画布的插入点：以插入点为起点取最多 16 个字节，按 MemoryValueDecode 的固定 17 行顺序列出
//   各类型一行（类型名 | 值 | 十六进制），并允许直接在行内改值、复制值。
// - 逻辑全部复用 Phase 0 的 ksword::memwb::DecodeAll / EncodeValue / MemoryDiffOverlay::Stage，
//   本类只做"取字节 -> 解释 -> 显示"与"文本 -> 编码 -> 暂存"的衔接，不重写任何解释或暂存规则。
//
// ------------------------------------------------------------
// 一、字节从哪来（绝不伪造数据）
// ------------------------------------------------------------
// - 字节来自 HexCanvas::cellStateAt：即画布"所见值"（页缓存叠加暂存补丁之后的结果）。
//   画布显示什么，解释器就解释什么，暂存中的字节也会被解释出来。
// - 从插入点起逐字节取，遇到第一个"没有值"的字节就停（未加载 / 不可读 / 已到地址空间末尾）。
//   只把这段连续的前缀交给 DecodeAll：需要更多字节的行是"不可用"，绝不补 0 充数。
//   某行不可用时，悬停提示说明需要几个字节、实际连续可用几个、停在哪个原因上。
// - 字符串类（ASCII / UTF-16）只看这 16 个字节；16 个字节全是字符、没有结束符时，
//   值末尾加"…"，提示里注明"仅解析前 16 字节"。
//
// ------------------------------------------------------------
// 二、三列怎么算
// ------------------------------------------------------------
// - 整数：值 = 十进制，十六进制 = DecodeAll 给出的补零大写位模式（有符号数显示补码位模式）。
// - f32/f64：值 = 最短往返文本（NaN/Inf/-0 同 DecodeAll），十六进制 = 位模式（按所选字节序拼出）。
// - 指针：十六进制 = 补零到 2*宽度 位的地址；值 = namer 给出的描述（如 module.dll+0x1A40），
//   没有描述时与十六进制相同。"复制值"得到的恒为纯十六进制地址（不含描述）。
// - FILETIME / time_t32 / time_t64：值 = 时间文本，范围外显示"超出范围"，十六进制 = 位模式。
// - GUID / ASCII / UTF-16：十六进制 = 起点处最多 8 个原始字节（内存顺序），更长时末尾加"…"。
// - 复制：每行右键 -> 复制值 / 复制十六进制 / 复制整行；悬停行尾出现复制图标，点击复制值；
//   选中行按 Ctrl+C 复制值。复制出去的是 DecodeAll 的 copyText（ptr 为纯十六进制，时间为 ISO 8601）。
//
// ------------------------------------------------------------
// 三、字节序与指针宽度（图标化切换，持久化）
// ------------------------------------------------------------
// - 顶栏右侧四个自绘图标按钮：小端 / 大端（竖条由矮到高 / 由高到矮 + 向右箭头）、指针 32 / 64 位（徽标），
//   每个都有悬停提示。选择写入 QSettings：键 memwb/inspector/byteOrder（0 小端 / 1 大端）与
//   memwb/inspector/pointerWidth（4 / 8）。读取失败（状态非 NoError、键缺失或值非法）一律退回默认
//   （小端、8 字节）。默认使用应用默认的 QSettings；离屏夹具用 setSettingsFile 指到临时 INI。
// - 字节序只影响整数、浮点、指针、时间；GUID / ASCII / UTF-16 恒按 Windows 内存布局，不随它变。
//
// ------------------------------------------------------------
// 四、行内编辑（双击 / Enter / F2；整数、浮点、指针行）
// ------------------------------------------------------------
// - 编辑器盖在值列与十六进制列上，预填该行的"复制值"，全选。
// - Enter：EncodeValue(类型, 文本, 字节序, 指针宽度) 编码；失败（BadNumber / OutOfRange / Overflow）
//   编辑器保持打开、边框变红、底部状态条显示原因并发面板的 editRejected，没有任何静默吞掉。
//   编码成功后：新值与画布上显示的当前值逐字节相同（且画布可编辑）则不暂存、状态条注明"值没有变化"，
//   也不发任何画布信号；否则调用画布公开的 canvas->stageBytes(插入点地址, 字节, &原因)——
//   与键盘编辑、粘贴、填充共用同一个入口，只读/范围/未加载/叠加层窗口等检查与原因文案都在画布一处。
//   stageBytes 被拒绝：状态条显示原因、编辑器保持打开（画布已发 editRejected，面板不再重复发）；
//   成功：画布自己发 editStaged 并排队 contentChanged，面板关闭编辑器、状态条显示结果。
// - Esc 或点到别处：取消，不产生任何暂存。插入点移动也会取消正在进行的编辑。
// - 只读画布（未 setEditable 或没有 overlay）下：双击/Enter/右键里的"编辑"都不会打开编辑器，
//   状态条与 editRejected 说明"当前为只读视图，不能编辑"，菜单项置灰并在提示里写明原因。
//   画布的可编辑状态变化（editableChanged）会让面板立即重建各行的"可编辑"状态与提示。
// - 注意：本面板只"暂存"。真正写入目标内存由宿主的写事务负责（D1：改完立即写入的模式下，
//   宿主只要连画布的 editStaged 一处——面板的编辑也经画布 stageBytes 发出它，面板没有自己的
//   editStaged，所以不会触发两次写事务），面板自身从不读写目标内存。
//
// ------------------------------------------------------------
// 五、刷新时机
// ------------------------------------------------------------
// - 完全由画布信号驱动，面板内没有任何定时器：
//   * caretMoved / selectionChanged：同步重读（插入点与选区变化）；
//   * contentChanged：画布显示的值可能变了（页异步回填、refresh 换代次、换地址空间/叠加层、暂存、丢弃、
//     宿主 notifyOverlayChanged）。画布把同一轮事件循环内的多次变化合并成一个信号，面板收到就重读，
//     只有 16 字节窗口真的变了才重建行；
//   * editableChanged（同步信号）：画布可编辑状态变了，清掉过时的错误消息（例如"当前为只读视图"），
//     重建各行的"可编辑"状态、悬停提示与状态条的空闲提示。
// - 手动：宿主在指针命名器的数据变化之后（例如模块列表刷新）应调用 refreshFromCanvas()——
//   命名器不是画布的一部分，画布不会为它发信号。
//
// - 所有接口只允许在 UI 线程调用（与画布一致）。
//
// ------------------------------------------------------------
// 六、主题与控件
// ------------------------------------------------------------
// - 全部自绘（行列表、顶栏、状态条、图标按钮），每次 paintEvent 现取 KswordTheme 静态颜色，
//   切换主题后下一次绘制即是新主题，无需任何通知。右键菜单每次弹出前新建，显式设置不透明
//   背景/文字/选中态/禁用态样式并开启悬停提示。所有按钮、动作、行、列标题都有悬停提示；
//   提示文字按纯文本转义后显示（目标内存里的字符串不会被当成 HTML 渲染）。
//
// ------------------------------------------------------------
// 七、文件分工与接入（本阶段不登记工程文件）
// ------------------------------------------------------------
//   HexInspectorPanel.h               （Q_OBJECT，需 moc）本文件：公开接口
//   HexInspectorPanel.cpp             构造、画布绑定（订阅画布信号）、设置、刷新
//   HexInspectorPanel.Rows.cpp        取字节窗口、DecodeAll 结果到三列文字
//   HexInspectorPanel.Edit.cpp        行内编辑：校验、编码、暂存、错误显示
//   HexInspectorPanel.Menu.cpp        复制、右键菜单、菜单样式与画出来的铅笔图标
//   HexInspectorRowView.h             （Q_OBJECT，需 moc）自绘行列表
//   HexInspectorRowView.cpp / .Paint.cpp  行列表的输入/布局/编辑器与绘制
//   HexInspectorWidgets.h/.cpp        图标按钮、顶栏、状态条（不含 Q_OBJECT）
// - 依赖：Qt Core/Gui/Widgets、theme.h、HexCanvas.h、shared/evidence/memory_workbench/MemoryValueDecode.*
//   与 MemoryDiffOverlay.*。不包含 Framework.h，保持自包含，可被离屏夹具单独编译。
// - 宿主接入示例：
//     auto* splitter = new QSplitter(Qt::Horizontal);
//     splitter->addWidget(canvas);
//     auto* inspector = new ks::ui::HexInspectorPanel;
//     inspector->setCanvas(canvas);
//     inspector->setPointerNamer(moduleNamer);   // 可选
//     splitter->addWidget(inspector);
//     splitter->setStretchFactor(0, 1);
//     splitter->setStretchFactor(1, 0);
//   inspector->sizeHint().width() 约 380 像素，可作为 splitter 初始宽度；最小约 260 像素。
// - 用户可见文案（约 60 条，由仓库 i18n 提取器对本组文件实际抽取后逐条比对：暂存失败的五条原因与
//   "目标字节中有…"两条随行内编辑改走画布 stageBytes 而从面板源码消失，前者已在画布的文案里）与审计要求
//   补恒等词条的头文件名，均已写入语言包，由 tools/i18n_language_pack.py audit 校验。
//   类型名（int8/uint32/float/ptr64/FILETIME/GUID/ASCII/UTF-16 等）是纯标识符，不翻译、不进语言包。
// - 离屏验证：tools/memwb_ui/ 下 memwb_ui_inspector.*、memwb_ui_tests.Inspector*.cpp，
//   已接入 build-memwb-ui-tests.cmd（新增两条 moc、七个面板源文件、MemoryValueDecode.cpp、四个夹具源文件；
//   新增环境变量 MEMWB_OUT 可改产物目录，避免多个验证互相覆盖）。第二轮新增的信号接线测试在
//   memwb_ui_tests.Signals.Inspector.cpp（公共设施见 memwb_ui_signals.h）。
//
// ------------------------------------------------------------
// 八、验证读数与已知限制（Phase 1 本组实测）
// ------------------------------------------------------------
// - 离屏夹具（MSVC /std:c++latest /W4 /WX，Qt 6.9.3 offscreen）：面板部分 1235 条断言 0 失败，
//   全夹具 1889 条 0 失败，零编译警告；theme_token_audit 通过。覆盖：跟随插入点（12 个偏移逐行对照
//   DecodeAll）、字节序/指针宽度切换与 INI 持久化（含非法值/不可写位置回退）、未读/不可读/末尾不补 0、
//   页异步到达后由 contentChanged 驱动重读（第一轮是定时器兜底，第二轮已删除）、
//   字符串截断与 HTML 转义提示、真实拖动分隔条、竖向滚动、行内编辑（小端/大端、整数/浮点/指针、
//   -Inf、七种拒绝原因、Esc/失焦/移动插入点取消、提交时字节变成未加载）、只读四种情形、复制全部路径、菜单像素实测、
//   主题切换像素跟随与逐像素还原。
// - 变异验证：24 个人为缺陷（补零、字节序忽略、复制含描述、跳过暂存、错误不显示、只读检查移除、设置不落盘、
//   指针宽度写错、Esc 不取消、Enter 事件外溢重开编辑器、截断标记、移动插入点不取消编辑、颜色缓存、
//   菜单样式移除、点击命中失效、去重忽略字节、定时器不启动、命中忽略滚动等）全部被测试杀死。
// - 数量级（本机，与其它会话并行构建时有波动）：插入点移动一次的刷新约 85~160 微秒；整面板绘制约 0.85~1.5 毫秒/帧；
//   一次行内编辑提交（含暂存与重建）约 2.5~5 毫秒。第二轮在安静机器上复测：83 微秒/次、0.90 毫秒/帧、2.5 毫秒/次，
//   与第一轮同量级（改走 stageBytes 与删定时器没有带来可见开销）。
// - 画布侧三个缺口已在第二轮补齐（上一轮的限制不再成立）：
//   1) 画布新增 contentChanged（排队合并）与 editableChanged，面板订阅它们，150 ms 轮询定时器已删除；
//   2) 画布新增公开的 stageBytes，面板的行内编辑改走它，面板的 editStaged 信号随之删除
//      （宿主只连画布的 editStaged 一处，不再有"两处都要连"的陷阱）；
//   3) cellStateAt 每次调用都会构造两个 QString（hexText 等），面板每次刷新调用 16 次，实测可以忽略。
// - 第二轮验证读数（同一夹具，MSVC /W4 /WX 零警告）：全夹具 2173 条断言 0 失败（第一轮 1889 条一条不少，
//   新增 284 条）；信号相关 281 条。27 个人为缺陷全部被抓到、未变异的对照组全绿，面板侧六个：
//   面板不订阅 contentChanged、再加一个轮询定时器、画布拒绝的原因由面板重发一遍、关掉"值没有变化"短路、
//   可编辑状态翻转后不清过时的错误消息、不订阅 editableChanged。
//   关键判据：定时器是值成员、不在对象树里，所以"对象树里没有 QTimer"只能挡退化，真正的判据是行为测试——
//   绕过画布直接暂存后等 500 ms（旧轮询间隔的三倍多）面板纹丝不动，宿主 notifyOverlayChanged() 之后才跟上。
// - 解释器侧的限制：DecodeAll 的 ASCII 行最多看 32 字节，面板按"最多 16 字节"的约定只给 16 字节，
//   字符串在 16 字节处被截断（值末尾 … 并在提示里说明）；解释器对"超出范围"的时间给固定英文文本
//   "out of range"，面板按 label 与 valid 把它换成中文，若解释器改了这个常量，换成中文的判据仍然成立
//   （判据是 valid=false，不是比对文本）。
// ============================================================

#include "../../../../shared/evidence/memory_workbench/MemoryValueDecode.h"
#include "HexCanvas.h"
#include "HexInspectorRowView.h"

#include <QPointer>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

class QButtonGroup;
class QMenu;
class QSettings;

namespace ks::ui
{
    class HexInspectorGlyphButton;
    class HexInspectorStatusBar;
    class HexInspectorTopBar;

    // HexInspectorPanel：数据解释器面板，详见文件头。
    class HexInspectorPanel : public QWidget
    {
        Q_OBJECT

    public:
        // kWindowBytes：每次从插入点起最多取多少字节参与解释。
        static constexpr int kWindowBytes = 16;

        // 构造：parent 为父控件。读取默认 QSettings 里的字节序与指针宽度，没有关联画布时所有行不可用。
        explicit HexInspectorPanel(QWidget* parent = nullptr);

        // 析构：成员均为子控件或值对象，无额外释放。
        ~HexInspectorPanel() override;

        // ---------------- 数据源 ----------------

        // setCanvas：关联要跟随的画布（非拥有）。传空表示解除关联。
        // 作用：重新连接 caretMoved/selectionChanged/contentChanged/editableChanged，并立即刷新。
        void setCanvas(HexCanvas* canvas);

        // canvas：当前关联的画布，可为空。
        HexCanvas* canvas() const;

        // setPointerNamer：设置指针描述器（非拥有，生命周期须长于面板或先传空）。
        // 说明：每次刷新最多调用一次 Describe（仅当指针行字节足够时），在 UI 线程同步执行，实现需保持轻量。
        void setPointerNamer(ksword::memwb::IPointerNamer* namer);

        // ---------------- 字节序与指针宽度 ----------------

        // byteOrder / setByteOrder：当前字节序；设置时同步按钮、保存设置并刷新。
        ksword::memwb::ByteOrder byteOrder() const;
        void setByteOrder(ksword::memwb::ByteOrder order);

        // pointerWidthBytes / setPointerWidthBytes：当前指针宽度（4 或 8）；设置非法值被忽略。
        std::uint32_t pointerWidthBytes() const;
        void setPointerWidthBytes(std::uint32_t widthBytes);

        // setSettingsFile：把设置改存到指定 INI 文件（空串表示回到默认 QSettings），并立即重读。
        // 用途：离屏夹具避免污染注册表；正式宿主不需要调用。
        void setSettingsFile(const QString& iniPath);

        // ---------------- 菜单与诊断 ----------------

        // buildRowMenu：构造某行的右键菜单（调用方负责 exec/popup 与释放）。row 越界返回空指针。
        // 传出：新建菜单，父对象为本面板。
        QMenu* buildRowMenu(int row);

        // rowView / topBar / statusBar：内部控件，供宿主微调与离屏测试读取，不改变行为。
        HexInspectorRowView* rowView() const;
        HexInspectorTopBar* topBar() const;
        HexInspectorStatusBar* statusBar() const;

        // byteOrderButton / pointerWidthButton：四个图标按钮，供测试点击与读取。
        HexInspectorGlyphButton* byteOrderButton(ksword::memwb::ByteOrder order) const;
        HexInspectorGlyphButton* pointerWidthButton(std::uint32_t widthBytes) const;

        // editBlockedReason：某行当前为什么不能编辑；返回空串表示可以编辑。
        QString editBlockedReason(int row) const;

        // sizeHint / minimumSizeHint：建议宽度约 380 像素，最小约 260 像素。
        QSize sizeHint() const override;
        QSize minimumSizeHint() const override;

    public slots:
        // refreshFromCanvas：重新读取画布并重建全部行。宿主在画布 refresh()/setEditable() 之后调用。
        void refreshFromCanvas();

        // beginEditRow：在指定行打开行内编辑器（与双击同一条路径）。
        // 传出：true 表示编辑器已打开；false 表示被拒绝（只读、不可用、类型不支持），原因已显示并发 editRejected。
        bool beginEditRow(int row);

    signals:
        // editRejected：行内编辑被面板自己拒绝（编码失败、不能开始编辑），reason 是可直接给用户看的原因。
        // 注意：行内编辑"暂存成功"没有面板自己的信号——宿主连画布的 HexCanvas::editStaged 即可
        // （面板的编辑经画布的 stageBytes 暂存，由它发出）。画布 stageBytes 拒绝的原因（只读、未加载、
        // 叠加层窗口外等）画布已发过 HexCanvas::editRejected，面板只显示在状态条、不再发第二遍。
        void editRejected(const QString& reason);

        // byteOrderChanged：字节序变化（0 小端，1 大端）。
        void byteOrderChanged(int order);

        // pointerWidthChanged：指针宽度变化（4 或 8）。
        void pointerWidthChanged(quint32 widthBytes);

    private slots:
        // 画布信号的接收槽：caretMoved/selectionChanged -> onCaretOrSelectionChanged；
        // contentChanged -> onCanvasDataChanged；editableChanged -> onCanvasEditableChanged。
        void onCaretOrSelectionChanged();
        void onCanvasDataChanged();
        void onCanvasEditableChanged();

        // 行列表信号的接收槽。
        void onRowActivated(int row);
        void onCopyRequested(int row, int kind);
        void onContextMenuRequested(int row, const QPoint& globalPos);
        void onEditCommitted(int row, const QString& text);
        void onEditCancelled(int row);

    private:
        // GapReason：连续字节在哪里断了。
        enum class GapReason : int
        {
            None = 0,       // 取满了 kWindowBytes 个字节
            NoData,         // 没有画布或画布没有地址空间
            NotLoaded,      // 下一个字节尚未加载 / 在途
            Unreadable,     // 下一个字节读取失败
            EndOfSpace      // 已到地址空间末尾
        };

        // WindowSnapshot：一次取字节的结果，同时是"没有变化就不重建"的比较依据。
        struct WindowSnapshot
        {
            bool hasData = false;                   // 画布是否有地址空间
            std::uint64_t address = 0;              // 起点（插入点）地址
            std::vector<std::uint8_t> bytes;        // 从起点起连续可用的字节（最多 kWindowBytes 个）
            GapReason gap = GapReason::NoData;      // 连续字节在哪里断了
            std::uint64_t selectionBytes = 0;       // 选区字节数（无数据为 0）
            bool canEdit = false;                   // 画布当前是否允许编辑

            // 逐字段比较。
            bool operator==(const WindowSnapshot& other) const = default;
        };

        // ---------- HexInspectorPanel.cpp：构造、绑定、设置、刷新 ----------
        void buildUi();
        void applyToggleState();
        void loadSettings();
        void saveSettings();
        std::unique_ptr<QSettings> openSettings() const;
        void refresh(bool force);
        void updateTopBar();

        // ---------- HexInspectorPanel.Rows.cpp：取字节与行内容 ----------
        WindowSnapshot collectWindow() const;
        std::vector<HexInspectorRowData> buildRows() const;
        QString unavailableTip(const QString& typeName, std::size_t needBytes) const;

        // ---------- HexInspectorPanel.Edit.cpp：行内编辑 ----------
        bool stageEdit(const HexInspectorRowData& row, const std::vector<std::uint8_t>& bytes, const QString& typedText);
        void showEditError(const QString& reason, bool emitSignal);

        // ---------- HexInspectorPanel.Menu.cpp：复制与菜单 ----------
        QString menuStyleSheet() const;

        // ---- 关联对象 ----
        QPointer<HexCanvas> m_canvas;                       // 被跟随的画布（非拥有）
        ksword::memwb::IPointerNamer* m_namer = nullptr;    // 指针描述器（非拥有）

        // ---- 子控件 ----
        HexInspectorTopBar* m_topBar = nullptr;             // 顶栏：地址、选区提示、四个图标按钮
        HexInspectorRowView* m_rows = nullptr;              // 行列表
        HexInspectorStatusBar* m_status = nullptr;          // 底部状态条
        HexInspectorGlyphButton* m_littleButton = nullptr;  // 小端按钮
        HexInspectorGlyphButton* m_bigButton = nullptr;     // 大端按钮
        HexInspectorGlyphButton* m_ptr32Button = nullptr;   // 4 字节指针按钮
        HexInspectorGlyphButton* m_ptr64Button = nullptr;   // 8 字节指针按钮
        QButtonGroup* m_orderGroup = nullptr;               // 字节序互斥组
        QButtonGroup* m_pointerGroup = nullptr;             // 指针宽度互斥组

        // ---- 状态 ----
        ksword::memwb::ByteOrder m_order = ksword::memwb::ByteOrder::Little;   // 当前字节序
        std::uint32_t m_pointerWidth = 8;                   // 当前指针宽度
        QString m_settingsFile;                             // 设置 INI 路径，空表示默认 QSettings
        WindowSnapshot m_window;                            // 最近一次取到的字节窗口
        bool m_hasBuilt = false;                            // 是否已经建过一次行（首次必须建）
        int m_editRow = -1;                                 // 正在编辑的行，-1 表示没有
        std::uint64_t m_editAddress = 0;                    // 编辑开始时的插入点地址（提交时写到这里）
    };
}
