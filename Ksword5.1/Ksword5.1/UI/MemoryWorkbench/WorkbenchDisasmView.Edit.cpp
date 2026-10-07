// WorkbenchDisasmView.Edit.cpp
// 作用：行内编辑委托（双击/F2/Enter 进入，Enter 编译提交，失败不关编辑框）与右键
// "汇编编辑"预览对话框（内核流程参考旧 MemoryEditorWidget.cpp:821-937，本页用自己的类重写，
// 不依赖旧控件；覆盖长度/NOP 填充/边界校验三条规则原样保留，即不变式 10）。
// 两条路径最终都只发 stageRequested 信号，本文件不直接写任何内存。

#include "WorkbenchDisasmView.h"

#include "HexCanvasFormat.h"

#include "../../Internationalization/LanguageManager.h"
#include "../../theme.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace ks::ui
{
    namespace
    {
        // sizeDialogResponsively：按屏幕可用区域夹取对话框尺寸，避免在小屏/远程桌面被撑出工作区。
        // 本组件自包含、不引入主程序的 UI_All.h 聚合头，因此不复用其
        // applyResponsiveWindowGeometry，自己实现等价的最小版本。
        // 入参：对话框、期望尺寸、最小尺寸、用于取屏幕的锚点控件；无传出。
        void sizeDialogResponsively(QDialog* dialog, const QSize& preferred, const QSize& minimum, QWidget* anchor)
        {
            const QScreen* screen = (anchor != nullptr && anchor->screen() != nullptr) ? anchor->screen() : QGuiApplication::primaryScreen();
            const QRect available = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
            const QSize bounded(
                std::min(preferred.width(), std::max(minimum.width(), available.width() - 80)),
                std::min(preferred.height(), std::max(minimum.height(), available.height() - 80)));
            dialog->setMinimumSize(minimum);
            dialog->resize(bounded);
        }

        // DisasmEditDelegate：行内编辑委托，整条指令放进跨"指令/操作数"两列的单行编辑框。
        // 创建/提交两件事都通过构造时传入的回调完成，回调由 WorkbenchDisasmView 绑定，
        // 委托自身不持有任何与目标相关的状态，只负责"编辑框生命周期 + Enter/Esc 分支"。
        class DisasmEditDelegate final : public QStyledItemDelegate
        {
        public:
            using CreateEditorFn = std::function<QLineEdit*(QWidget*, const QModelIndex&)>;
            // CommitFn：尝试编译并提交；成功返回空串，失败返回给用户看的原因（不关编辑框）。
            using CommitFn = std::function<QString(QLineEdit*)>;

            DisasmEditDelegate(QObject* parent, CreateEditorFn createEditor, CommitFn commit)
                : QStyledItemDelegate(parent), m_createEditor(std::move(createEditor)), m_commit(std::move(commit))
            {
            }

            QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override
            {
                return m_createEditor(parent, index);
            }

            // setEditorData：留空——createEditor 已经按行上下文填好初始文本，默认实现会把它
            // 按"指令/操作数"两列分别覆盖，破坏跨列的整条汇编文本。
            void setEditorData(QWidget*, const QModelIndex&) const override
            {
            }

            // updateEditorGeometry：几何跨"指令"到"操作数"列（到视口右缘），容纳完整汇编文本。
            void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const override
            {
                QRect geometry = option.rect;
                const auto* table = qobject_cast<QTableView*>(parent());
                if (table != nullptr)
                {
                    geometry.setRight(table->viewport()->width() - 1);
                }
                editor->setGeometry(geometry);
            }

            // setModelData：不通过模型写数据——提交结果只经 stageRequested 信号交给宿主，
            // 模型本身只读展示，这里留空避免误触发模型变更。
            void setModelData(QWidget*, QAbstractItemModel*, const QModelIndex&) const override
            {
            }

        protected:
            // eventFilter：拦截 Return/Enter（编译提交，失败不关）、Escape（取消）、
            // FocusOut（视为取消，防止点击别处时静默提交半成品输入）。
            bool eventFilter(QObject* editorObject, QEvent* event) override
            {
                auto* lineEdit = qobject_cast<QLineEdit*>(editorObject);
                if (lineEdit == nullptr)
                {
                    return QStyledItemDelegate::eventFilter(editorObject, event);
                }
                if (event->type() == QEvent::KeyPress)
                {
                    const auto* keyEvent = static_cast<QKeyEvent*>(event);
                    if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)
                    {
                        // stageRequested 可同步进入写确认的嵌套事件循环并销毁宿主。
                        // 两个守卫保证返回后不再向已销毁的委托或编辑器发送收尾信号。
                        const QPointer<DisasmEditDelegate> delegateGuard(this);
                        const QPointer<QLineEdit> editorGuard(lineEdit);
                        const auto commit = m_commit;
                        const QString error = commit(lineEdit);
                        if (!delegateGuard || !editorGuard)
                        {
                            return true;
                        }
                        if (!error.isEmpty())
                        {
                            // 编译失败：吞掉事件，编辑框保持打开，原因已经由回调显示在下方。
                            return true;
                        }
                        emit commitData(lineEdit);
                        if (delegateGuard && editorGuard)
                        {
                            emit closeEditor(lineEdit);
                        }
                        return true;
                    }
                    if (keyEvent->key() == Qt::Key_Escape)
                    {
                        emit closeEditor(lineEdit, QAbstractItemDelegate::RevertModelCache);
                        return true;
                    }
                }
                if (event->type() == QEvent::FocusOut)
                {
                    emit closeEditor(lineEdit, QAbstractItemDelegate::RevertModelCache);
                    return true;
                }
                return QStyledItemDelegate::eventFilter(editorObject, event);
            }

        private:
            CreateEditorFn m_createEditor; // 按行上下文创建并预填编辑框
            CommitFn m_commit;              // 编译并提交，失败时返回原因
        };
    }

    // installEditDelegate：构造委托并装到表格上，构造函数里只调一次。
    void WorkbenchDisasmView::installEditDelegate()
    {
        const auto createEditor = [this](QWidget* parent, const QModelIndex& index) -> QLineEdit* {
            hideInlineEditError();
            // 可疑点 1：这里是唯一真正创建编辑器的地方——F2/Enter 的路径经 beginRowEdit 调
            // m_table->edit(index) 最终也会走到这里，双击更是只能走这里（Qt 的
            // DoubleClicked 编辑触发器直接调用委托，根本不经过 beginRowEdit）。只在
            // beginRowEdit 里挡 m_editable 不够——那挡不住原生双击触发器，必须在这个唯一
            // 的汇合点上再挡一次，这里才是只读模式真正生效的地方。
            if (!m_editable || index.column() < 2 || m_model->isEndOfWindowRow(index.row()))
            {
                return nullptr;
            }
            const std::optional<DecodedRow> row = m_model->rowAt(index.row());
            if (!row.has_value())
            {
                return nullptr;
            }
            auto* editor = new QLineEdit(parent);
            editor->setObjectName(QStringLiteral("ksMemwbDisasmInlineEditor"));
            editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            const QString originalSource = (row->mnemonic + QLatin1Char(' ') + row->operands).trimmed();
            editor->setText(originalSource);
            // 冻结上下文：提交时按这些属性重建地址/原字节/架构，不依赖委托外部的可变状态。
            editor->setProperty("ksAddress", QVariant::fromValue<qulonglong>(row->address));
            editor->setProperty("ksOldBytes", row->bytes);
            editor->setProperty("ksX64", isX64());
            editor->setProperty("ks_edit_context_revision", QVariant::fromValue<qulonglong>(m_editContextRevision));
            // ksOriginalSource：D1——记住进入编辑时的原文，提交时如果一字未改，直接关闭
            // 编辑框而不发任何信号（旧版 MemoryEditorWidget.InlineAssembly.cpp 有这一步，
            // 新实现丢了它：Enter 进编辑、手滑再按一次 Enter，也会把原指令重新编译一遍，
            // 等价但编码不同的机器码被当成"修改"暂存，立即写入模式下等于静默改写目标）。
            editor->setProperty("ksOriginalSource", originalSource);
            editor->selectAll();
            // QAbstractItemView::state()/EditingState 是 protected，宿主/夹具都够不到，
            // 这里自己记一份可查询的编辑中标志（isEditing）。
            m_editingActive = true;
            return editor;
        };

        const auto commit = [this](QLineEdit* editor) -> QString {
            const QString source = editor->text().trimmed();
            const QString originalSource = editor->property("ksOriginalSource").toString();
            if (source == originalSource)
            {
                // D1：文本一字未改，直接关闭编辑框，不编译、不比较字节、不发信号。
                hideInlineEditError();
                return QString();
            }
            if (!m_assembleOne)
            {
                return QStringLiteral("未设置汇编后端，无法编译。");
            }
            const std::uint64_t address = editor->property("ksAddress").toULongLong();
            const QByteArray oldBytes = editor->property("ksOldBytes").toByteArray();
            const bool x64 = editor->property("ksX64").toBool();
            // 会话复用同一 provider、架构或权限变化时，旧编辑器冻结的地址不能被新目标继承。
            const std::uint64_t editRevision = editor->property("ks_edit_context_revision").toULongLong();
            if (!m_editable || m_provider == nullptr || editRevision != m_editContextRevision || x64 != isX64())
            {
                const QString error = QStringLiteral("数据已刷新，原指令不再位于当前视图，已取消本次汇编编辑。");
                showInlineEditError(editor->geometry(), error);
                return error;
            }
            // 编辑期间实时重读可能已更新基线。逐字节复核原指令，防止按旧指令长度覆盖新代码。
            const WorkbenchByteWindow currentWindow = m_provider->FetchWindow(address, static_cast<std::uint64_t>(oldBytes.size()));
            bool originalBytesMatch = currentWindow.ok && currentWindow.address == address
                && currentWindow.bytes.size() >= static_cast<std::size_t>(oldBytes.size())
                && currentWindow.validMask.size() >= static_cast<std::size_t>(oldBytes.size());
            for (qsizetype i = 0; originalBytesMatch && i < oldBytes.size(); ++i)
            {
                const std::size_t index = static_cast<std::size_t>(i);
                originalBytesMatch = currentWindow.validMask[index] != 0
                    && currentWindow.bytes[index] == static_cast<std::uint8_t>(oldBytes[i]);
            }
            if (!originalBytesMatch)
            {
                const QString error = QStringLiteral("数据已刷新，原指令不再位于当前视图，已取消本次汇编编辑。");
                showInlineEditError(editor->geometry(), error);
                return error;
            }
            const WorkbenchAssembleResult result = m_assembleOne(source, address, x64);
            if (!result.success)
            {
                showInlineEditError(editor->geometry(), result.error);
                return result.error;
            }
            // 行内只接受一条完整指令：先复核未填充的机器码，不能把多条指令或尾部残片
            // 当成一条短指令补 NOP；右键汇编编辑仍使用自己的多行预览与边界校验。
            // decodedInstruction：按编辑开始时冻结的真实地址和架构解出的首条指令。
            std::optional<DecodedRow> decodedInstruction;
            if (m_decodeOne && !result.bytes.isEmpty())
            {
                decodedInstruction = m_decodeOne(
                    reinterpret_cast<const std::uint8_t*>(result.bytes.constData()),
                    static_cast<std::size_t>(result.bytes.size()), address, x64);
            }
            if (!decodedInstruction.has_value() || !decodedInstruction->decoded
                || decodedInstruction->bytes != result.bytes)
            {
                const QString error = ks::i18n::sourceText(QStringLiteral(
                    "行内编辑只接受一条完整指令；多行汇编请使用右键汇编编辑。"));
                showInlineEditError(editor->geometry(), error);
                return error;
            }
            if (result.bytes.size() > oldBytes.size())
            {
                const QString error = QStringLiteral("新指令为 %1 字节，超出原指令的 %2 字节；请使用右键"
                    "“汇编编辑”并明确调整覆盖长度。").arg(result.bytes.size()).arg(oldBytes.size());
                showInlineEditError(editor->geometry(), error);
                return error;
            }
            // 较短的新指令用 NOP 补满原指令长度，保持后续指令的边界不变（不变式 10）。
            QByteArray payload = result.bytes;
            payload.append(QByteArray(oldBytes.size() - payload.size(), static_cast<char>(0x90)));
            hideInlineEditError();
            // D1 后半：文本变了，但编译后的字节跟原字节逐位相同（例如换了一种写法但编码
            // 一样），同样不算真正的修改，不发信号——否则宿主会把"没有变化"的补丁也暂存。
            if (payload != oldBytes)
            {
                emit stageRequested(address, payload);
            }
            return QString();
        };

        auto* delegate = new DisasmEditDelegate(m_table, createEditor, commit);
        // 编辑框关闭（提交成功、Escape、失焦）都会发这个信号，统一在这里清掉编辑中标志、
        // 隐藏残留的错误提示（D7），并在刷新被推迟过的情况下（D2）补上这一次刷新。
        // N2（第二轮审核）：这个 lambda 是在 setItemDelegate(delegate) 之前 connect 的，
        // 比 QAbstractItemView 自己对同一个 closeEditor 信号挂的内部槛先执行——如果这里
        // 同步调用 rebuildRowsNow()（会 beginResetModel/endResetModel），表格会在视图自己
        // 的 closeEditor 处理（把键盘焦点还给表格）跑之前就被整表重建，编辑器已经被模型
        // 重置注销掉，视图随后找不到登记过的编辑器，"把焦点还给表格"这一步被悄悄跳过——
        // 表现为提交/取消编辑后键盘焦点落到了别处，F2/Enter/Backspace 全部失灵，必须再用
        // 鼠标点一下表格才能恢复。改成 QTimer::singleShot(0, ...) 把真正的刷新推迟到下一次
        // 事件循环，让视图先走完它自己那一份 closeEditor 收尾（焦点已经正确处理过），我们
        // 的刷新再安全地重建模型，不会撞上视图内部还没走完的状态机。
        connect(delegate, &QAbstractItemDelegate::closeEditor, this, [this]() {
            m_editingActive = false;
            hideInlineEditError();
            if (m_refreshPending)
            {
                m_refreshPending = false;
                QTimer::singleShot(0, this, [this]() { rebuildRows(); });
            }
        });
        m_table->setItemDelegate(delegate);
    }

    // cancelInlineEdit：见头文件声明处的契约说明（N3/N4）——只关闭编辑框本身，不处理
    // "推迟的刷新"，调用方各自决定是否需要在调用之后补一次 rebuildRowsNow()。
    void WorkbenchDisasmView::cancelInlineEdit()
    {
        m_table->closePersistentEditor(m_table->currentIndex());
        m_editingActive = false;
        hideInlineEditError();
    }

    // beginRowEdit：双击/F2/Enter 的统一入口，从"指令"列开始编辑（跨到"操作数"列）。
    void WorkbenchDisasmView::beginRowEdit(const int row)
    {
        // 可疑点 1：只读模式下任何入口都不得进入编辑，哪怕调用方（eventFilter/双击槛）
        // 漏查了 m_editable，这里是最终防线。
        if (!m_editable || row < 0 || m_model->isEndOfWindowRow(row))
        {
            return;
        }
        const QModelIndex index = m_model->index(row, 2);
        m_table->setCurrentIndex(index);
        m_table->edit(index);
    }

    // showInlineEditError：把编译失败原因显示在编辑框正下方（不带"第 1 行"前缀）。
    void WorkbenchDisasmView::showInlineEditError(const QRect& editorRect, const QString& message)
    {
        m_inlineError->setText(message);
        const QRect viewportRect = m_table->viewport()->rect();
        const int top = std::min(editorRect.bottom() + 2, viewportRect.bottom() - m_inlineError->sizeHint().height());
        m_inlineError->setGeometry(editorRect.left(), std::max(0, top), editorRect.width(), m_inlineError->sizeHint().height());
        m_inlineError->raise();
        m_inlineError->show();
    }

    void WorkbenchDisasmView::hideInlineEditError()
    {
        m_inlineError->hide();
    }

    // showAssemblyPreviewDialog：右键"汇编编辑"。覆盖长度可调、可选 NOP 填充，
    // 边界校验要求覆盖范围正好落在完整旧指令边界上，不留半条可执行的尾巴。
    // D10：expectedRow 是菜单打开时冻结的快照；这里先按地址在当前模型里重新找一次，
    // 核对字节是否仍一致——菜单是模态的，exec() 期间数据可能已经被 refreshView 刷新，
    // 行号/行内容都可能变了，不能再用旧行号盲取。
    void WorkbenchDisasmView::showAssemblyPreviewDialog(const DecodedRow& expectedRow)
    {
        if (!m_editable || m_provider == nullptr || !m_decodeOne || !m_assembleOne)
        {
            return;
        }
        int matchedRow = -1;
        for (int i = 0; i < m_model->rowCount(); ++i)
        {
            const std::optional<DecodedRow> candidate = m_model->rowAt(i);
            if (candidate.has_value() && candidate->address == expectedRow.address)
            {
                matchedRow = i;
                break;
            }
        }
        const std::optional<DecodedRow> current = matchedRow >= 0 ? m_model->rowAt(matchedRow) : std::nullopt;
        if (!current.has_value() || current->bytes != expectedRow.bytes)
        {
            m_status->setText(QStringLiteral("数据已刷新，原指令不再位于当前视图，已取消本次汇编编辑。"));
            emit statusMessage(m_status->text());
            return;
        }
        const std::uint64_t address = current->address;
        const bool x64 = isX64();
        const std::uint64_t editRevision = m_editContextRevision; // 冻结宿主身份，即使 provider 地址不变也可检测切换

        // 拉一段足够长的窗口用于边界扫描（右键编辑很少需要覆盖超过这个范围）。
        const WorkbenchByteWindow window = m_provider->FetchWindow(address, 4096);
        // 可疑点 4：防御性 min 夹取，避免 validMask 与 bytes 长度不一致时越界读。
        const std::size_t effectiveLength = std::min(window.bytes.size(), window.validMask.size());
        std::size_t validLength = 0;
        while (validLength < effectiveLength && window.validMask[validLength] != 0)
        {
            ++validLength;
        }
        if (validLength == 0)
        {
            return;
        }
        const QByteArray snapshot(reinterpret_cast<const char*>(window.bytes.data()), static_cast<qsizetype>(validLength));

        // 可疑点 9：堆分配 + WA_DeleteOnClose，不再用栈上 QDialog——如果 exec() 的嵌套事件
        // 循环期间 this（父窗口）被销毁，Qt 会先同步销毁子对象（包括这个对话框），exec()
        // 据此退出；用 QPointer<WorkbenchDisasmView> 自guard，退出后先判空再访问 this 的
        // 任何成员，不解引用悬空指针。
        auto* dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName(QStringLiteral("ksMemwbAssemblyDialog"));
        // 显式设置不透明背景样式：父容器若用了透明/特殊样式，弹窗默认样式可能继承出黑底黑字。
        dialog->setStyleSheet(KswordTheme::OpaqueDialogStyle(dialog->objectName()));
        dialog->setWindowTitle(QStringLiteral("汇编编辑"));
        auto* layout = new QVBoxLayout(dialog);
        auto* form = new QFormLayout;
        form->addRow(QStringLiteral("起始地址"), new QLabel(hexcanvas_format::FormatAddress(address, 16), dialog));
        form->addRow(QStringLiteral("指令架构"), new QLabel(x64 ? QStringLiteral("x64") : QStringLiteral("x86"), dialog));
        auto* span = new QSpinBox(dialog);
        span->setRange(1, static_cast<int>(std::min<qsizetype>(snapshot.size(), 256)));
        span->setValue(static_cast<int>(std::max<qsizetype>(1, current->bytes.size())));
        form->addRow(QStringLiteral("覆盖长度（字节）"), span);
        auto* pad = new QCheckBox(QStringLiteral("用 NOP 填充剩余覆盖空间"), dialog);
        pad->setChecked(true);
        form->addRow(pad);
        layout->addLayout(form);

        auto* hint = new QLabel(QStringLiteral(
            "每行一条 Intel 指令。数字默认十六进制，十进制用 0d 前缀；覆盖长度须包含完整指令；"
            "编译只生成预览，确认无误后点“填入暂存”，由外层写事务统一写入。"), dialog);
        hint->setWordWrap(true);
        layout->addWidget(hint);

        auto* source = new QPlainTextEdit(dialog);
        source->setObjectName(QStringLiteral("ksMemwbAssemblySource"));
        source->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        source->setPlainText((current->mnemonic + QLatin1Char(' ') + current->operands).trimmed());
        layout->addWidget(source, 1);

        auto* preview = new QPlainTextEdit(dialog);
        preview->setObjectName(QStringLiteral("ksMemwbAssemblyPreview"));
        preview->setReadOnly(true);
        preview->setFont(source->font());
        layout->addWidget(preview, 1);

        auto* status = new QLabel(dialog);
        status->setWordWrap(true);
        layout->addWidget(status);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
        auto* compile = buttons->addButton(QStringLiteral("编译并预览"), QDialogButtonBox::ActionRole);
        auto* stage = buttons->addButton(QStringLiteral("填入暂存"), QDialogButtonBox::AcceptRole);
        stage->setEnabled(false);
        layout->addWidget(buttons);

        // payload 仍然是本函数的局部变量、按引用捕获进 lambda——dialog->exec() 同步阻塞，
        // 函数返回前 payload 一直在作用域内有效；WA_DeleteOnClose 只在 exec() 返回、dialog
        // 隐藏之后才触发 deleteLater，不会在 exec() 运行期间提前把 dialog 连带它的信号
        // 连接一起销毁，所以这里的按引用捕获跟堆分配与否无关，仍然安全。
        QByteArray payload;
        const auto invalidate = [=, &payload]() {
            payload.clear();
            stage->setEnabled(false);
            preview->clear();
            status->clear();
        };
        connect(source, &QPlainTextEdit::textChanged, dialog, invalidate);
        connect(span, &QSpinBox::valueChanged, dialog, invalidate);
        connect(pad, &QCheckBox::toggled, dialog, invalidate);

        connect(compile, &QPushButton::clicked, dialog, [=, &payload]() {
            invalidate();
            const WorkbenchAssembleResult result = m_assembleOne(source->toPlainText(), address, x64);
            if (!result.success)
            {
                // D6：用后端给出的真实出错行号，不再恒为"第 1 行"（行内编辑路径仍然是单行
                // 源码，不走这里，不受影响）；errorLine<=0（旧后端/夹具假后端没给）时按
                // 第 1 行显示，不展示"第 0 行"这种没有意义的数字。
                status->setText(QStringLiteral("第 %1 行：%2").arg(result.errorLine > 0 ? result.errorLine : 1).arg(result.error));
                return;
            }
            if (result.bytes.isEmpty() || result.bytes.size() > span->value())
            {
                status->setText(QStringLiteral("机器码为 %1 字节，超出覆盖长度 %2；请明确扩大覆盖范围后重新预览。")
                    .arg(result.bytes.size()).arg(span->value()));
                return;
            }
            // 边界校验（不变式 10）：覆盖长度必须恰好落在完整旧指令边界上，不能截断。
            const std::vector<std::uint8_t> boundaryBytes(
                reinterpret_cast<const std::uint8_t*>(snapshot.constData()),
                reinterpret_cast<const std::uint8_t*>(snapshot.constData()) + std::min<qsizetype>(snapshot.size(), span->value() + 15));
            const QVector<DecodedRow> oldRows = DecodeWindowResynced(boundaryBytes, address, m_decodeOne, 65536, x64);
            qsizetype boundary = 0;
            bool boundaryOk = false;
            for (const DecodedRow& decodedRow : oldRows)
            {
                if (!decodedRow.decoded)
                {
                    status->setText(QStringLiteral("覆盖范围包含无法解码的字节；请调整范围或使用十六进制编辑。"));
                    return;
                }
                boundary += decodedRow.bytes.size();
                if (boundary >= span->value())
                {
                    boundaryOk = boundary == span->value();
                    break;
                }
            }
            if (!boundaryOk)
            {
                status->setText(QStringLiteral("覆盖长度截断了原指令，请选择完整指令边界（下一边界为 %1 字节）。").arg(boundary));
                return;
            }
            if (!pad->isChecked() && result.bytes.size() != span->value())
            {
                status->setText(QStringLiteral("关闭 NOP 填充时，机器码长度必须等于覆盖长度。"));
                return;
            }
            payload = result.bytes;
            payload.append(QByteArray(span->value() - payload.size(), static_cast<char>(0x90)));
            QString text = QStringLiteral("原始：%1\n替换：%2\n")
                .arg(hexcanvas_format::FormatHexText(snapshot.left(span->value()))).arg(hexcanvas_format::FormatHexText(payload));
            const std::vector<std::uint8_t> payloadBytes(
                reinterpret_cast<const std::uint8_t*>(payload.constData()),
                reinterpret_cast<const std::uint8_t*>(payload.constData()) + payload.size());
            const QVector<DecodedRow> newRows = DecodeWindowResynced(payloadBytes, address, m_decodeOne, 65536, x64);
            for (const DecodedRow& decodedRow : newRows)
            {
                text += hexcanvas_format::FormatAddress(decodedRow.address, 16) + QStringLiteral("  ")
                    + hexcanvas_format::FormatHexText(decodedRow.bytes) + QStringLiteral("  ")
                    + decodedRow.mnemonic + QLatin1Char(' ') + decodedRow.operands + QLatin1Char('\n');
            }
            preview->setPlainText(text);
            status->setText(QStringLiteral("预览完成：%1 字节；点“填入暂存”交给外层写事务。").arg(payload.size()));
            stage->setEnabled(true);
        });
        connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);

        sizeDialogResponsively(dialog, QSize(760, 620), QSize(480, 360), this);
        const QPointer<WorkbenchDisasmView> self(this);
        const int result = dialog->exec();
        if (!self)
        {
            // this 已经在 exec() 期间被销毁：dialog 作为它的子对象也已经/正在被销毁，
            // 不能再访问 m_status、不能再 emit 本对象的信号。
            return;
        }
        if (result != QDialog::Accepted || payload.isEmpty())
        {
            return;
        }
        // exec 允许自动附加/分离、改通道与实时重读；QPointer 只能证明对象仍活着，
        // 不能证明旧预览仍属于当前目标。先检查身份/架构/权限，再复核整个覆盖范围。
        bool snapshotMatches = m_editable && m_provider != nullptr
            && editRevision == m_editContextRevision && x64 == isX64();
        if (snapshotMatches)
        {
            const WorkbenchByteWindow currentWindow = m_provider->FetchWindow(address, static_cast<std::uint64_t>(payload.size()));
            snapshotMatches = currentWindow.ok && currentWindow.address == address
                && currentWindow.bytes.size() >= static_cast<std::size_t>(payload.size())
                && currentWindow.validMask.size() >= static_cast<std::size_t>(payload.size())
                && snapshot.size() >= payload.size();
            for (qsizetype i = 0; snapshotMatches && i < payload.size(); ++i)
            {
                const std::size_t index = static_cast<std::size_t>(i);
                snapshotMatches = currentWindow.validMask[index] != 0
                    && currentWindow.bytes[index] == static_cast<std::uint8_t>(snapshot[i]);
            }
        }
        if (!snapshotMatches)
        {
            m_status->setText(QStringLiteral("数据已刷新，原指令不再位于当前视图，已取消本次汇编编辑。"));
            emit statusMessage(m_status->text());
            return;
        }
        emit stageRequested(address, payload);
    }
}
