// wpH_tests.DisasmRegression2.cpp
// 作用：第二轮独立审核（review2-wpH.md）针对 WorkbenchDisasmView 的补测，第 1 部分。
// 并入自审核者补测 extra2.cpp 的 T01/T02/T03/T04/T05/T06/T11/T12/T13，改写成本仓库的
// WPH_CHECK 断言风格，与既有 wpH_tests.DisasmRegression.cpp 共用同一套 Rig/辅助函数惯例
// （各 .cpp 是独立编译单元，各自一份匿名命名空间辅助函数，不新增共享头——与文件里其它
// 同名小工具函数的既有做法一致）。
// - T01：en-US 下"超出已读取窗口"提示行的正文与悬停提示都必须经运行期翻译（杀 rA08）。
// - T02：显示上限（4096 字节）——展示的字节数恰好是前 4096 字节、必有提示行、覆盖不变量
//   成立（杀 rA13/rC05/rC07）。
// - T03：有效字节恰好 4096、末尾被截断的 mov 残留字节不得解出 adc 幻影（杀 rC06）。
// - T04：applyTailGiveUp 的覆盖不变量——行字节拼起来必须恰好等于输入前缀、地址连续，不重
//   不漏（杀 rC03/rC04）。
// - T05：Enter 跟随跳转对 call/jmp 仍然生效（杀 rC09，白名单回归哨兵）。
// - T06：N1 修复——白名单补齐 jnb/jnbe/jnl/jnle 之后，Zydis 对 73/77/7D/7F 与
//   0F 83/87/8D/8F 这四组条件跳转的真实输出助记符必须都能被 Enter 跟随。
// - T11：后退栈容量 64——第 65 次入栈会淘汰最旧的一项，终点必须真的是"淘汰了最旧"而不是
//   停在原地或淘汰了别的（杀 rC17）。
// - T12：右键菜单打开期间指令被换成同地址同长度但不同内容的另一条指令，预览对话框必须
//   拒绝打开（D10 的"只比长度不比内容"回归哨兵，杀 rC18）。
// - T13：setEditable(false) 之后，右键菜单里的"汇编编辑"项必须是禁用状态（杀 rC20）。

#include "wpH_common.h"

#include "../../../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QMenu>
#include <QTableView>
#include <QTimer>
#include <QtTest/QtTest>

#include <algorithm>
#include <iostream>
#include <optional>

using ks::ui::WorkbenchDisasmView;

namespace wpH_test
{
    namespace
    {
        std::vector<std::uint8_t> toVec2(const QByteArray& bytes)
        {
            return std::vector<std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
        }

        void loadBytes2(FakeBytesProvider& provider, const std::uint64_t base, const QByteArray& bytes)
        {
            provider.overlay().LoadBaseline(
                QStringLiteral("rig2").toStdString(), base, toVec2(bytes), std::vector<std::uint8_t>(static_cast<std::size_t>(bytes.size()), 1));
        }

        // Rig2：与 DisasmRegression.cpp 的 Rig 同一套构造惯例，各自一份（不同编译单元）。
        struct Rig2
        {
            FakeBytesProvider provider;
            ks::ui::WorkbenchDisasmView view;
            std::uint64_t base;

            explicit Rig2(const QByteArray& bytes, const int bits = 64, const std::uint64_t baseAddress = 0x140001000ULL)
                : provider(bits), base(baseAddress)
            {
                loadBytes2(provider, base, bytes);
                view.setBytesProvider(&provider);
                view.setDecodeBackend(MakeRealZydisDecodeBackend());
                view.setAssembleBackend(MakeRealAssembleBackend());
                view.resize(760, 420);
                view.show();
                static_cast<void>(QTest::qWaitForWindowExposed(&view));
                view.jumpTo(base);
                QCoreApplication::processEvents();
            }
        };

        bool hasHan2(const QString& s)
        {
            for (const QChar ch : s)
            {
                if (ch.unicode() >= 0x3400 && ch.unicode() <= 0x9FFF)
                {
                    return true;
                }
            }
            return false;
        }

        // coversExactly：行字节拼起来恰好等于 expected、地址连续（不重不漏）；提示行不计入。
        bool coversExactly(ks::ui::WorkbenchDisasmView& v, const std::uint64_t base, const QByteArray& expected, QString* why)
        {
            QByteArray joined;
            std::uint64_t next = base;
            for (int i = 0; i < v.model()->rowCount(); ++i)
            {
                if (v.model()->isEndOfWindowRow(i))
                {
                    continue;
                }
                const auto row = v.model()->rowAt(i);
                if (!row)
                {
                    continue;
                }
                if (row->address != next)
                {
                    *why = QStringLiteral("第 %1 行地址 0x%2，期望 0x%3").arg(i).arg(row->address, 0, 16).arg(next, 0, 16);
                    return false;
                }
                joined += row->bytes;
                next += static_cast<std::uint64_t>(row->bytes.size());
            }
            if (joined != expected)
            {
                *why = QStringLiteral("拼接得到 %1 字节，期望 %2 字节").arg(joined.size()).arg(expected.size());
                return false;
            }
            return true;
        }

        bool anyRowContains2(ks::ui::WorkbenchDisasmView& v, const QString& needle)
        {
            for (int i = 0; i < v.model()->rowCount(); ++i)
            {
                const auto row = v.model()->rowAt(i);
                if (row && row->mnemonic.contains(needle, Qt::CaseInsensitive))
                {
                    return true;
                }
            }
            return false;
        }

        bool hasNoteRow2(ks::ui::WorkbenchDisasmView& v)
        {
            for (int i = 0; i < v.model()->rowCount(); ++i)
            {
                if (v.model()->isEndOfWindowRow(i))
                {
                    return true;
                }
            }
            return false;
        }

        // ---------------- T01（杀 rA08）：提示行文字/悬停提示必须经运行期翻译 ----------------
        void runNoteRowI18nTests()
        {
            QString err;
            WPH_CHECK_NOTE(ks::i18n::LanguageManager::instance().initialize(QStringLiteral("en-US"), &err), err);
            Rig2 rig(QByteArray::fromHex("554889E590")); // 5 字节，窗口必不满，必有提示行
            const int n = rig.view.model()->rowCount();
            WPH_CHECK_NOTE(rig.view.model()->isEndOfWindowRow(n - 1), QStringLiteral("最后一行应为提示行"));
            const QString text = rig.view.model()->index(n - 1, 0).data().toString();
            const QString tip = rig.view.model()->index(n - 1, 0).data(Qt::ToolTipRole).toString();
            WPH_CHECK_NOTE(!hasHan2(text), text);
            WPH_CHECK_NOTE(!hasHan2(tip), tip);
            ks::i18n::LanguageManager::instance().initialize(QStringLiteral("zh-CN"));
            ks::i18n::LanguageManager::instance().retranslateAll();
        }

        // ---------------- T02（杀 rA13/rC05/rC07）：显示上限裁剪 ----------------
        void runDisplayCapTests()
        {
            QByteArray movs;
            for (int i = 0; i < 1200; ++i)
            {
                movs.append(QByteArray::fromHex("488B0510000000")); // mov rax,[rip+0x10]，7 字节
            }
            Rig2 rig(movs); // 8400 字节全部有效，远超 4096 的显示上限
            std::uint64_t maxEnd = 0;
            for (int i = 0; i < rig.view.model()->rowCount(); ++i)
            {
                const auto row = rig.view.model()->rowAt(i);
                if (row)
                {
                    maxEnd = std::max<std::uint64_t>(maxEnd, row->address - rig.base + static_cast<std::uint64_t>(row->bytes.size()));
                }
            }
            WPH_CHECK_NOTE(maxEnd == 4096, QStringLiteral("展示的行应恰好覆盖 4096 字节，实得 %1").arg(maxEnd));
            WPH_CHECK_NOTE(hasNoteRow2(rig.view), QStringLiteral("显示上限截断必须出现提示行"));
            QString why;
            WPH_CHECK_NOTE(coversExactly(rig.view, rig.base, movs.left(4096), &why), why);
        }

        // ---------------- T03（杀 rC06）：有效字节恰好 4096，末尾截断不得解出幻影 ----------------
        void runTailAt4096Tests()
        {
            QByteArray b;
            for (int i = 0; i < 584; ++i)
            {
                b.append(QByteArray::fromHex("488B0510000000"));
            }
            b.append(QByteArray::fromHex("9090"));
            b.append(QByteArray::fromHex("488B05100000")); // 残缺的 mov，缺最后 1 字节
            WPH_CHECK_NOTE(b.size() == 4096, QString::number(b.size()));
            Rig2 rig(b);
            WPH_CHECK_NOTE(!anyRowContains2(rig.view, QStringLiteral("adc")), QStringLiteral("恰好 4096 有效字节时也不得解出 adc 幻影"));
            QString why;
            WPH_CHECK_NOTE(coversExactly(rig.view, rig.base, b, &why), why);
        }

        // ---------------- T04（杀 rC03/rC04）：applyTailGiveUp 覆盖不变量 ----------------
        void runGiveUpCoverageTests()
        {
            const QByteArray cases[] = {
                QByteArray::fromHex("909048" "8B05" "1000"),        // 真截断（mov 少了 2 字节）
                QByteArray::fromHex("9090069090C3"),                // 06 非法编码之后的短序列
                QByteArray::fromHex("90E8") + QByteArray(2, '\x01') // E8（call rel32）被截断
            };
            for (const QByteArray& c : cases)
            {
                Rig2 rig(c);
                QString why;
                WPH_CHECK_NOTE(coversExactly(rig.view, rig.base, c, &why),
                    QStringLiteral("输入 %1：%2").arg(QString::fromLatin1(c.toHex()), why));
            }
        }

        // ---------------- T05（杀 rC09）：call/jmp 的跟随回归哨兵 ----------------
        void runFollowCallTests()
        {
            Rig2 rig(QByteArray::fromHex("E805000000") + QByteArray(16, '\x90'));
            rig.view.canvas()->setFocus();
            rig.view.canvas()->setSelectedRow(0);
            const auto before = rig.view.anchorAddress();
            QTest::keyClick(rig.view.canvas(), Qt::Key_Return);
            WPH_CHECK_NOTE(rig.view.anchorAddress() != before, QStringLiteral("call rel32 必须被 Enter 跟随"));
        }

        // ---------------- T06（N1 修复）：全部条件跳转的 Zydis 真实助记符都要能跟随 ----------------
        void runFollowAllConditionalJumpsTests()
        {
            // 73/77/7D/7F 与对应的 0F 8x 近跳转，Zydis 的真实输出是 jnb/jnbe/jnl/jnle，
            // 不是常见写法 jae/ja/jge/jg（同一条指令的两种拼法，Zydis 固定选了后一种）。
            const char* hexes[] = {"7305", "7705", "7D05", "7F05", "0F8305000000", "0F8705000000", "0F8D05000000", "0F8F05000000"};
            for (const char* h : hexes)
            {
                const QByteArray bytes = QByteArray::fromHex(h) + QByteArray(32, '\x90');
                Rig2 rig(bytes);
                rig.view.canvas()->setFocus();
                rig.view.canvas()->setSelectedRow(0);
                const auto before = rig.view.anchorAddress();
                QTest::keyClick(rig.view.canvas(), Qt::Key_Return);
                WPH_CHECK_NOTE(rig.view.anchorAddress() != before,
                    QStringLiteral("条件跳转 %1 未被跟随（isEditing=%2）").arg(QString::fromLatin1(h)).arg(rig.view.isEditing()));
                if (rig.view.isEditing() && QApplication::focusWidget())
                {
                    // 兜底：如果误入了编辑（回归时才会发生），把编辑框关掉再继续下一组，
                    // 避免一次断言失败连带污染后面的用例。
                    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
                }
            }
        }

        // ---------------- T11（杀 rC17）：后退栈淘汰最旧，终点要核对准确 ----------------
        void runBackStackEvictionTests()
        {
            Rig2 rig(QByteArray(256, '\x90'));
            rig.view.reset();
            for (int i = 0; i < 70; ++i)
            {
                rig.view.jumpTo(rig.base + static_cast<std::uint64_t>(i));
            }
            for (int i = 0; i < 64; ++i)
            {
                rig.view.navigateBack();
            }
            WPH_CHECK_NOTE(rig.view.anchorAddress() == rig.base + 5,
                QStringLiteral("容量 64 淘汰最旧后，最老保留的锚点应为 base+5，实得 base+%1").arg(rig.view.anchorAddress() - rig.base));
        }

        // ---------------- T12（杀 rC18）：菜单打开期间指令变成同长度不同内容必须拒绝 ----------------
        // 写法与既有 runD10Tests（wpH_tests.DisasmRegression.cpp）保持一致，只用"一层"
        // QTimer::singleShot，不嵌套第二层：D10 正确生效时对话框根本不会打开，
        // customContextMenuRequested 这行会早早返回，若还注册了一个按引用捕获本函数局部
        // 变量的"第二层"定时器，它会在没有任何东西续命的情况下，于函数早已返回、栈帧已经
        // 销毁之后才真正触发——对一个不存在的栈变量写入，是真正的悬空引用（已用这一支
        // 写法在本机实测复现过一次崩溃：exit=-1073741819/0xC0000005）。统一只用
        // "触发一次按键 + emit 返回后 processEvents/qWait 排空事件循环 + 直接查询全局状态"
        // 这一套没有悬空风险的做法。
        void runMenuRefreshSameLengthTests()
        {
            Rig2 rig(QByteArray::fromHex("554889E590C3"));
            // 延时 0ms：不赌固定的毫秒数，menu->exec() 进入它自己的嵌套循环后第一轮就会
            // 处理这个回调（见 T13 同款说明），机器负载再重也不影响触发时机。
            QTimer::singleShot(0, [&rig]() {
                if (auto* popup = QApplication::activePopupWidget())
                {
                    loadBytes2(rig.provider, rig.base, QByteArray::fromHex("554889E390C3")); // mov rbp,rsp -> mov rbx,rsp（同长度）
                    rig.view.refreshView();
                    QTest::keyClick(popup, Qt::Key_Down);
                    QTest::keyClick(popup, Qt::Key_Return);
                }
            });
            // 看门狗只调用静态方法，不捕获任何本函数的局部变量——即使函数早已返回，
            // 它触发时访问的也只是 QApplication 的全局状态，没有悬空引用风险。
            QTimer::singleShot(8000, []() {
                if (auto* m = QApplication::activeModalWidget()) { m->close(); }
                if (auto* p = QApplication::activePopupWidget()) { p->close(); }
            });
            auto* canvas = rig.view.canvas();
            canvas->setSelectedRow(1);
            emit canvas->contextMenuRequested(canvas->contentRect(1).center());
            QCoreApplication::processEvents();
            QTest::qWait(300);
            QCoreApplication::processEvents();
            WPH_CHECK_NOTE(QApplication::activeModalWidget() == nullptr,
                QStringLiteral("指令内容已经变了，不应打开预览对话框"));
            const QString status = rig.view.findChild<QLabel*>(QStringLiteral("ksMemwbDisasmStatus"))->text();
            WPH_CHECK_NOTE(status.contains(QStringLiteral("已取消")), status);
            // 保险：如果上面的判据居然失败、对话框真的开着，主动关掉它，避免这个用例的
            // 失败连带卡住后面的用例（它们也会调用 menu->exec()/dialog->exec()）。
            if (auto* lingering = QApplication::activeModalWidget())
            {
                lingering->close();
            }
        }

        // ---------------- T13（杀 rC20）：只读模式下菜单项必须禁用 ----------------
        // 探测回调改用延时 0ms 的定时器，不用固定的"等多少毫秒"——固定延时在机器负载重、
        // CPU 调度被别的进程挤占时本质上是在赌时间窗口，实测会间歇性错过菜单真正可见的
        // 那个瞬间（活动弹出控件读到 null）。0ms 定时器仍然要经过一次事件循环分派，但只要
        // menu->exec() 已经进入它自己的嵌套循环，这个回调就会在循环的第一轮就被处理，
        // 不依赖任何具体的墙钟时长，因此不会受机器忙闲影响。
        void runReadOnlyMenuTests()
        {
            Rig2 rig(QByteArray::fromHex("554889E590E900000000"));
            rig.view.setEditable(false);
            bool found = false;
            bool enabled = true;
            QTimer::singleShot(0, [&]() {
                if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
                {
                    for (auto* action : menu->actions())
                    {
                        if (action->text().contains(QStringLiteral("汇编")))
                        {
                            found = true;
                            enabled = action->isEnabled();
                        }
                    }
                    menu->close();
                }
            });
            // 看门狗：极端情况下 0ms 回调仍然没赶上（例如 exec() 还没真正显示出菜单），
            // 2 秒后兜底关掉——只读全局状态，不捕获任何本函数的局部变量，没有悬空风险。
            QTimer::singleShot(2000, []() {
                if (auto* p = QApplication::activePopupWidget()) { p->close(); }
            });
            auto* canvas = rig.view.canvas();
            canvas->setSelectedRow(1);
            emit canvas->contextMenuRequested(canvas->contentRect(1).center());
            WPH_CHECK_NOTE(found && !enabled,
                QStringLiteral("只读模式下汇编编辑项必须禁用（found=%1 enabled=%2）").arg(found).arg(enabled));
        }
    }

    void RunDisasmRegressionTests2()
    {
        const int before = g_checks;
        const int beforeFail = g_failures;
        runNoteRowI18nTests();
        runDisplayCapTests();
        runTailAt4096Tests();
        runGiveUpCoverageTests();
        runFollowCallTests();
        runFollowAllConditionalJumpsTests();
        runBackStackEvictionTests();
        runMenuRefreshSameLengthTests();
        runReadOnlyMenuTests();
        std::cerr << "[DisasmRegression2] checks=" << (g_checks - before) << " failures=" << (g_failures - beforeFail) << std::endl;
    }
}
