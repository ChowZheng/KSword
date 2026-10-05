// HexGotoBar.cpp
// 作用：HexGotoBar 的输入解析（Resolve）、界面搭建、历史与错误提示。

#include "HexGotoBar.h"

#include "../../../../shared/evidence/memory_workbench/HexViewport.h"
#include "../../../../shared/evidence/memory_workbench/MemoryAddressExpr.h"

#include "../../theme.h"
#include "HexCanvasFormat.h"
#include "HexViewFormat.h"
#include "HexViewSettings.h"

#include <QAction>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QVBoxLayout>

#include <algorithm>

namespace ks::ui
{
    namespace
    {
        // EmptyInputText：输入为空时的提示，按模式给出"请输入什么"。
        QString EmptyInputText(HexGotoBar::Mode mode)
        {
            if (mode == HexGotoBar::Mode::Row)
            {
                return QStringLiteral("请输入行号");
            }
            if (mode == HexGotoBar::Mode::Offset)
            {
                return QStringLiteral("请输入偏移");
            }
            return QStringLiteral("请输入地址");
        }

        // OutOfRangeText：范围外提示。传入：区间两端的文本。传出："超出数据范围 [起点, 终点]"。
        QString OutOfRangeText(const QString& low, const QString& high)
        {
            return QStringLiteral("超出数据范围 [%1, %2]").arg(low, high);
        }

        // ExprErrorText：把地址表达式的错误码翻译成中文。
        // 传入：求值结果与输入的 UTF-8 字节；传出：提示文字。
        QString ExprErrorText(const ksword::memwb::ExprResult& result, const QByteArray& utf8, HexGotoBar::Mode mode)
        {
            // 位置换算成 1 起的字符序号；输入含中文时字节偏移与字符序号不同。
            const qulonglong charNumber =
                static_cast<qulonglong>(hexview_format::CharIndexFromUtf8Offset(utf8, result.errorPosition)) + 1ULL;
            switch (result.error)
            {
            case ksword::memwb::ExprError::Empty:
                return EmptyInputText(mode);
            case ksword::memwb::ExprError::BadNumber:
                return QStringLiteral("数字格式不正确（第 %1 个字符处）").arg(charNumber);
            case ksword::memwb::ExprError::Overflow:
                return QStringLiteral("数值超过 64 位范围");
            case ksword::memwb::ExprError::BadSyntax:
                return QStringLiteral("表达式语法错误（第 %1 个字符处）：只支持十六进制数字用加号相加，不支持减法").arg(charNumber);
            case ksword::memwb::ExprError::UnknownModule:
            case ksword::memwb::ExprError::AmbiguousModule:
            case ksword::memwb::ExprError::NeedsProcess:
            case ksword::memwb::ExprError::DerefFailed:
                return QStringLiteral("此处不支持模块名与方括号解引用，请直接输入十六进制数字");
            case ksword::memwb::ExprError::None:
                break;
            }
            return QStringLiteral("无法解析输入");
        }
    }

    // 解析一段输入：见头文件第一节。
    bool HexGotoBar::Resolve(
        Mode mode,
        const QString& text,
        const Space& space,
        std::uint64_t* addressOut,
        QString* errorOut)
    {
        // fail：统一的失败出口。
        const auto fail = [errorOut](const QString& reason) {
            if (errorOut != nullptr)
            {
                *errorOut = reason;
            }
            return false;
        };

        if (!space.valid || space.first > space.last)
        {
            return fail(QStringLiteral("没有可跳转的数据"));
        }
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty())
        {
            return fail(EmptyInputText(mode));
        }

        // ---- 行号：十进制非负整数 ----
        if (mode == Mode::Row)
        {
            // 只认 ASCII 数字：QString::toULongLong 会接受前导正负号与空白，这里先挡住。
            for (const QChar ch : trimmed)
            {
                if (ch.unicode() < u'0' || ch.unicode() > u'9')
                {
                    return fail(QStringLiteral("行号必须是十进制数字"));
                }
            }
            bool parsed = false;
            const qulonglong row = trimmed.toULongLong(&parsed, 10);
            if (!parsed)
            {
                return fail(QStringLiteral("数值超过 64 位范围"));
            }

            // 用与画布同一个几何模型换算行号：行按绝对地址对齐，首行可能含补空位。
            const ksword::memwb::HexViewport geometry(
                space.first, space.last, static_cast<std::uint32_t>(space.bytesPerRow), 1U);
            if (!geometry.IsValid())
            {
                return fail(QStringLiteral("没有可跳转的数据"));
            }
            const std::uint64_t rowCount = geometry.RowCount();
            if (row >= rowCount)
            {
                return fail(OutOfRangeText(
                    QStringLiteral("0"),
                    QString::number(static_cast<qulonglong>(rowCount - 1ULL))));
            }
            const std::optional<std::uint64_t> rowStart = geometry.RowStartAddress(row);
            if (!rowStart.has_value())
            {
                return fail(QStringLiteral("没有可跳转的数据"));
            }
            if (addressOut != nullptr)
            {
                // 首行的补空位地址早于数据起点，夹取到起点。
                *addressOut = (std::max)(*rowStart, space.first);
            }
            return true;
        }

        // ---- 绝对地址 / 偏移：地址表达式，默认十六进制 ----
        // 没有进程上下文，resolver 为空：模块名与解引用会得到 NeedsProcess，被翻译成"此处不支持"。
        const QByteArray utf8 = trimmed.toUtf8();
        const ksword::memwb::ExprResult result = ksword::memwb::EvaluateAddressExpr(
            std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())), 8U, nullptr);
        if (!result.ok)
        {
            return fail(ExprErrorText(result, utf8, mode));
        }

        if (mode == Mode::Absolute)
        {
            if (result.value < space.first || result.value > space.last)
            {
                const int digits = hexview_format::AddressDigitsFor(space.first, space.last);
                return fail(OutOfRangeText(
                    hexcanvas_format::FormatAddress(space.first, digits),
                    hexcanvas_format::FormatAddress(space.last, digits)));
            }
            if (addressOut != nullptr)
            {
                *addressOut = result.value;
            }
            return true;
        }

        // 偏移：目标 = first + 偏移；先比较后相加，不会回绕。
        const std::uint64_t span = space.last - space.first;
        if (result.value > span)
        {
            const int digits = hexview_format::AddressDigitsFor(0ULL, span);
            return fail(OutOfRangeText(
                hexcanvas_format::FormatAddress(0ULL, digits),
                hexcanvas_format::FormatAddress(span, digits)));
        }
        if (addressOut != nullptr)
        {
            *addressOut = space.first + result.value;
        }
        return true;
    }

    // 构造：读历史、搭界面。
    HexGotoBar::HexGotoBar(QWidget* parent)
        : HexViewBarFrame(Edge::Bottom, parent)
    {
        m_history = hexview_settings::LoadGotoHistory();
        buildUi();
    }

    // 搭建界面：第一行 [模式三段] [输入框] [历史] [执行] [关闭]，第二行是错误行。
    void HexGotoBar::buildUi()
    {
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(8, 4, 8, 4);
        column->setSpacing(2);
        auto* row = new QHBoxLayout();
        row->setSpacing(6);
        column->addLayout(row);

        // 模式三段按钮：说明放在逐段悬停提示里。
        m_modeSegment = new HexViewSegmented(
            QStringList{ QStringLiteral("地址"), QStringLiteral("偏移"), QStringLiteral("行号") }, this);
        m_modeSegment->setSegmentToolTip(
            0, QStringLiteral("绝对地址：十六进制，0x 前缀可有可无（1233 就是 0x1233），可用加号相加"));
        m_modeSegment->setSegmentToolTip(
            1, QStringLiteral("相对偏移：从数据起点算起的字节偏移，十六进制"));
        m_modeSegment->setSegmentToolTip(
            2, QStringLiteral("行号：十进制，从 0 开始，对应视图里的第 N 行"));
        m_modeSegment->setAccessibleName(QStringLiteral("跳转模式"));
        row->addWidget(m_modeSegment);

        // 输入框：回车 = 跳转；Up/Down 翻历史（事件过滤器）。
        m_edit = new QLineEdit(this);
        m_edit->setClearButtonEnabled(true);
        m_edit->setMinimumWidth(140);
        m_edit->setAccessibleName(QStringLiteral("跳转目标"));
        m_edit->installEventFilter(this);
        row->addWidget(m_edit, 1);

        // 历史按钮：点击弹出历史菜单。
        m_historyButton = new HexViewGlyphButton(HexViewGlyphButton::Glyph::History, this);
        m_historyButton->setToolTip(QStringLiteral("最近的跳转输入（最多 10 条）；输入框里按上下方向键也可以翻"));
        m_historyMenu = new QMenu(this);
        m_historyButton->setMenu(m_historyMenu);
        m_historyButton->setPopupMode(QToolButton::InstantPopup);
        row->addWidget(m_historyButton);

        // 执行按钮。
        m_goButton = new HexViewGlyphButton(HexViewGlyphButton::Glyph::Go, this);
        m_goButton->setToolTip(QStringLiteral("跳转（回车）"));
        row->addWidget(m_goButton);

        // 关闭按钮。
        m_closeButton = new HexViewGlyphButton(HexViewGlyphButton::Glyph::Close, this);
        m_closeButton->setToolTip(QStringLiteral("关闭跳转条（Esc）"));
        row->addWidget(m_closeButton);

        // 错误行：输入框下方，没有错误时隐藏。
        m_error = new HexViewMessageLabel(this);
        m_error->setAccessibleName(QStringLiteral("跳转错误提示"));
        m_error->hide();
        column->addWidget(m_error);

        // 连接：回车与按钮触发跳转；输入变化清错误；菜单弹出前按最新历史重建。
        connect(m_edit, &QLineEdit::returnPressed, this, [this]() { submit(); });
        connect(m_goButton, &QToolButton::clicked, this, [this]() { submit(); });
        connect(m_closeButton, &QToolButton::clicked, this, [this]() { emit closeRequested(); });
        connect(m_modeSegment, &HexViewSegmented::currentIndexChanged, this, [this](int) { onModeChanged(); });
        connect(m_edit, &QLineEdit::textEdited, this, [this](const QString&) {
            m_historyCursor = -1;
            clearError();
        });
        connect(m_historyMenu, &QMenu::aboutToShow, this, [this]() { rebuildHistoryMenu(); });
        onModeChanged();
    }

    // 设置目标空间：非法范围或不被接受的行宽等同没有数据。
    void HexGotoBar::setSpace(std::uint64_t first, std::uint64_t last, int bytesPerRow)
    {
        if (first > last || bytesPerRow < 0
            || !ksword::memwb::HexViewport::IsSupportedBytesPerRow(static_cast<std::uint32_t>(bytesPerRow)))
        {
            clearSpace();
            return;
        }
        m_space.valid = true;
        m_space.first = first;
        m_space.last = last;
        m_space.bytesPerRow = bytesPerRow;
    }

    // 没有数据。
    void HexGotoBar::clearSpace()
    {
        m_space = Space();
    }

    // 目标空间。
    HexGotoBar::Space HexGotoBar::space() const
    {
        return m_space;
    }

    // 打开：显示、重读历史、聚焦、全选。
    void HexGotoBar::open()
    {
        reloadHistory();
        show();
        m_edit->setFocus(Qt::ShortcutFocusReason);
        m_edit->selectAll();
    }

    // 当前模式。
    HexGotoBar::Mode HexGotoBar::mode() const
    {
        switch (m_modeSegment->currentIndex())
        {
        case 1:
            return Mode::Offset;
        case 2:
            return Mode::Row;
        default:
            return Mode::Absolute;
        }
    }

    // 设置模式。
    void HexGotoBar::setMode(Mode newMode)
    {
        m_modeSegment->setCurrentIndex(static_cast<int>(newMode));
    }

    // 模式切换：更新占位提示、清错误。
    void HexGotoBar::onModeChanged()
    {
        const Mode current = mode();
        if (current == Mode::Absolute)
        {
            m_edit->setPlaceholderText(QStringLiteral("目标地址（十六进制），例如 0x1000 或 1000+20"));
        }
        else if (current == Mode::Offset)
        {
            m_edit->setPlaceholderText(QStringLiteral("相对数据起点的偏移（十六进制）"));
        }
        else
        {
            m_edit->setPlaceholderText(QStringLiteral("行号（十进制，从 0 开始）"));
        }
        clearError();
    }

    // 输入框文字。
    QString HexGotoBar::inputText() const
    {
        return m_edit->text();
    }

    // 设置输入框文字。
    void HexGotoBar::setInputText(const QString& text)
    {
        m_edit->setText(text);
    }

    // 显示错误：错误行可见。
    void HexGotoBar::showError(const QString& text)
    {
        m_error->setMessage(HexViewMessageLabel::Kind::Error, text);
        m_error->setVisible(!text.isEmpty());
    }

    // 清除错误：错误行隐藏。
    void HexGotoBar::clearError()
    {
        m_error->clearMessage();
        m_error->hide();
    }

    // 错误文字。
    QString HexGotoBar::errorText() const
    {
        return m_error->text();
    }

    // 解析当前输入并跳转。
    bool HexGotoBar::submit()
    {
        std::uint64_t address = 0;
        QString reason;
        if (!Resolve(mode(), m_edit->text(), m_space, &address, &reason))
        {
            showError(reason);
            return false;
        }

        // 成功：清错误，输入进历史并持久化，全选方便连续输入下一个目标，最后通知宿主。
        clearError();
        m_history = hexview_settings::PushHistory(m_history, m_edit->text());
        hexview_settings::SaveGotoHistory(m_history);
        m_historyCursor = -1;
        m_edit->selectAll();
        emit gotoRequested(address);
        return true;
    }

    // 历史。
    QStringList HexGotoBar::history() const
    {
        return m_history;
    }

    // 重读历史。
    void HexGotoBar::reloadHistory()
    {
        m_history = hexview_settings::LoadGotoHistory();
        m_historyCursor = -1;
    }

    // 历史菜单。
    QMenu* HexGotoBar::historyMenu() const
    {
        return m_historyMenu;
    }

    // 历史菜单样式：背景、文字、选中态、禁用态、分隔线全部用主题静态色，菜单每次弹出前重新生成。
    QString HexGotoBar::menuStyleSheet() const
    {
        return QStringLiteral(
            "QMenu{background-color:%1;color:%2;border:1px solid %3;padding:3px;}"
            "QMenu::item{color:%2;background-color:transparent;padding:5px 20px 5px 14px;}"
            "QMenu::item:selected{background-color:%4;color:%5;}"
            "QMenu::item:disabled{color:%6;background-color:transparent;}"
            "QMenu::separator{height:1px;background-color:%3;margin:3px 6px;}")
            .arg(KswordTheme::SurfaceColorHex())
            .arg(KswordTheme::TextPrimaryColorHex())
            .arg(KswordTheme::BorderColorHex())
            .arg(KswordTheme::ThemeColorName(KswordTheme::PrimaryAccentColor()))
            .arg(KswordTheme::OnAccentHex())
            .arg(KswordTheme::TextDisabledColorHex());
    }

    // 重建历史菜单：每条历史一个菜单项，点击填入输入框并聚焦；没有历史时放一个禁用的说明项。
    void HexGotoBar::rebuildHistoryMenu()
    {
        // 不透明样式：显式关闭透明背景、铺满背景、开启悬停提示。
        m_historyMenu->clear();
        m_historyMenu->setAttribute(Qt::WA_TranslucentBackground, false);
        m_historyMenu->setAutoFillBackground(true);
        m_historyMenu->setStyleSheet(menuStyleSheet());
        m_historyMenu->setToolTipsVisible(true);

        if (m_history.isEmpty())
        {
            QAction* empty = m_historyMenu->addAction(QStringLiteral("没有历史记录"));
            empty->setEnabled(false);
            empty->setToolTip(QStringLiteral("成功跳转过的输入会出现在这里"));
            return;
        }
        for (const QString& entry : m_history)
        {
            QAction* action = m_historyMenu->addAction(entry);
            action->setToolTip(QStringLiteral("填入输入框"));
            connect(action, &QAction::triggered, this, [this, entry]() {
                m_edit->setText(entry);
                m_edit->setFocus(Qt::OtherFocusReason);
                m_edit->selectAll();
                clearError();
            });
        }
    }

    // 在历史里翻：delta=+1 更旧，-1 更新；翻过最新一条恢复草稿。
    void HexGotoBar::browseHistory(int delta)
    {
        if (m_history.isEmpty())
        {
            return;
        }

        // 刚开始翻时记下草稿。
        if (m_historyCursor < 0)
        {
            if (delta < 0)
            {
                return;
            }
            m_draft = m_edit->text();
        }
        const int next = m_historyCursor + delta;
        if (next < 0)
        {
            m_historyCursor = -1;
            m_edit->setText(m_draft);
            return;
        }
        m_historyCursor = std::min(next, static_cast<int>(m_history.size()) - 1);
        m_edit->setText(m_history[m_historyCursor]);
        m_edit->selectAll();
    }

    // 事件过滤：输入框的 Up/Down 翻历史，其余交给输入框。
    bool HexGotoBar::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == m_edit && event->type() == QEvent::KeyPress)
        {
            const QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Up)
            {
                browseHistory(1);
                return true;
            }
            if (keyEvent->key() == Qt::Key_Down)
            {
                browseHistory(-1);
                return true;
            }
        }
        return HexViewBarFrame::eventFilter(watched, event);
    }

    // 内部控件访问器。
    QLineEdit* HexGotoBar::lineEdit() const
    {
        return m_edit;
    }

    HexViewSegmented* HexGotoBar::modeSegment() const
    {
        return m_modeSegment;
    }

    HexViewGlyphButton* HexGotoBar::goButton() const
    {
        return m_goButton;
    }

    HexViewGlyphButton* HexGotoBar::historyButton() const
    {
        return m_historyButton;
    }

    HexViewGlyphButton* HexGotoBar::closeButton() const
    {
        return m_closeButton;
    }

    HexViewMessageLabel* HexGotoBar::errorLabel() const
    {
        return m_error;
    }
}
