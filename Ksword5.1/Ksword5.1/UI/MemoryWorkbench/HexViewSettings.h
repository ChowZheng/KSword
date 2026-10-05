#pragma once

// ============================================================
// HexViewSettings.h
// 作用：
// - HexView 一族要持久化的两项偏好，集中在这里读写，键名只有这一处定义：
//     memwb/hexview/inspectorVisible  解释器面板是否显示（bool，默认 false）
//     memwb/hexview/gotoHistory       跳转条最近输入历史（字符串列表，最多 10 条，最近的在最前）
// - 全部使用应用默认的 QSettings（与 HexInspectorPanel 的字节序/指针宽度同一个后端），
//   离屏夹具用 QSettings::setPath + setDefaultFormat 把它重定向到临时目录。
//
// 失败语义（任何失败都退回默认，绝不让偏好读写影响控件使用）：
// - 读：QSettings 状态非 NoError、键缺失、值类型/内容非法，一律退回默认
//   （解释器面板 false、历史空列表）。历史里的空串、重复项、超出上限的尾部也在读取时被清理。
// - 写：写失败（只读位置、没有组织名）静默忽略，下次读取自然退回默认。
//
// 谁来写：
// - 解释器面板显隐只在用户通过工具栏按钮切换时写入；宿主用代码调用 setInspectorVisible 不写入，
//   否则一个嵌入式宿主把它强制为 false 会覆盖内存工作台里用户的偏好（两个宿主共用同一个键）。
// - 跳转历史只在一次跳转成功后写入（输入错误或超出范围的文本不进历史）。
// ============================================================

#include <QString>
#include <QStringList>

namespace ks::ui::hexview_settings
{
    // kGotoHistoryLimit：跳转历史最多保留的条数。
    inline constexpr int kGotoHistoryLimit = 10;

    // LoadInspectorVisible：读取解释器面板显隐偏好。传出：偏好值，读失败退回 false。
    bool LoadInspectorVisible();

    // SaveInspectorVisible：保存解释器面板显隐偏好。传入：新值。写失败静默忽略。
    void SaveInspectorVisible(bool visible);

    // LoadGotoHistory：读取跳转历史。传出：清理后的列表（去空、去重、截断到上限），读失败为空列表。
    QStringList LoadGotoHistory();

    // SaveGotoHistory：保存跳转历史。传入：列表（保存前同样清理）。写失败静默忽略。
    void SaveGotoHistory(const QStringList& history);

    // PushHistory：纯函数，把一条新输入放到历史最前面。
    // 传入：当前历史、新输入（两端空白会被剪掉，剪完为空则不变）；
    // 传出：新历史——同文本旧项被移除（去重并提到最前），长度不超过 kGotoHistoryLimit。
    QStringList PushHistory(const QStringList& history, const QString& entry);

    // NormalizeHistory：纯函数，清理历史列表（剪空白、去空、去重保留先出现者、截断到上限）。
    QStringList NormalizeHistory(const QStringList& history);
}
