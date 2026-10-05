#pragma once

// ============================================================
// HexViewFormat.h
// 作用：
// - HexView 一族（工具栏、查找条、跳转条、状态条、导出）共用的几个纯函数：
//   地址位数选择、"UTF-8 字节偏移 -> 字符序号"换算、悬停提示转义。
// - 全部无状态，只依赖 Qt Core，可以脱离任何控件在夹具里直接测试。
// - 地址文本格式本身直接复用 HexCanvasFormat.h 的 FormatAddress，保证与画布地址列完全一致，
//   本文件不重复实现。
// ============================================================

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <cstdint>

namespace ks::ui::hexview_format
{
    // AddressDigitsFor：为一段地址范围选择显示位数。
    // 传入：范围两端（含）；传出：两端都不超过 0xFFFFFFFF 时为 8，否则为 16。
    // 与画布地址列的"8 位或 16 位"规则一致，使状态条、错误提示里的地址与地址列对得上。
    int AddressDigitsFor(std::uint64_t first, std::uint64_t last);

    // CharIndexFromUtf8Offset：把"UTF-8 字节偏移"换算成 QString 的字符序号（0 起）。
    // 用途：ParseError / ExprResult 给出的位置是原始输入的 UTF-8 字节偏移，界面要显示
    //       "第 N 个字符"，输入里有中文时字节偏移与字符序号不同。
    // 传入：输入文本的 UTF-8 字节、字节偏移（超过长度时按长度处理）；传出：字符序号。
    std::size_t CharIndexFromUtf8Offset(const QByteArray& utf8, std::size_t byteOffset);

    // PlainToolTip：把纯文本变成"原样显示"的悬停提示。
    // 为什么需要：Qt 会把"看起来像 HTML"的提示当富文本渲染，而提示文字里可能含有用户输入的内容
    //            （例如查找框里的 "<b>"），必须转义后放进 white-space:pre 的块里。
    // 传入：纯文本；传出：可直接交给 setToolTip 的文字，空串原样返回空串。
    QString PlainToolTip(const QString& plainText);
}
