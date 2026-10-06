// HexViewSettings.cpp
// 作用：HexViewSettings.h 的实现——两个偏好键的读写与历史列表的清理。

#include "HexViewSettings.h"

#include <QSettings>
#include <QVariant>

namespace ks::ui::hexview_settings
{
    namespace
    {
        // InspectorVisibleKey：解释器面板显隐的键。
        QString InspectorVisibleKey()
        {
            return QStringLiteral("memwb/hexview/inspectorVisible");
        }

        // GotoHistoryKey：跳转历史的键。
        QString GotoHistoryKey()
        {
            return QStringLiteral("memwb/hexview/gotoHistory");
        }
    }

    // 读解释器面板显隐偏好：状态异常、键缺失、值不是合法布尔都退回 false。
    bool LoadInspectorVisible()
    {
        // settings：默认 QSettings；状态非 NoError 说明后端不可用，直接退回默认。
        const QSettings settings;
        if (settings.status() != QSettings::NoError)
        {
            return false;
        }

        // 键缺失按默认处理。
        const QVariant stored = settings.value(InspectorVisibleKey());
        if (!stored.isValid())
        {
            return false;
        }

        // 严格解析：布尔型直接取值；INI 里布尔存成文本，只认 "true"/"1" 为真。
        // 乱写的文本不能走 QVariant::toBool（它把任何非空非 "0"/"false" 的文本当真），否则损坏的偏好会让面板被打开。
        if (stored.typeId() == QMetaType::Bool)
        {
            return stored.toBool();
        }
        const QString text = stored.toString().trimmed().toLower();
        return text == QStringLiteral("true") || text == QStringLiteral("1");
    }

    // 写解释器面板显隐偏好。
    void SaveInspectorVisible(bool visible)
    {
        QSettings settings;
        settings.setValue(InspectorVisibleKey(), visible);
        settings.sync();
    }

    // 清理历史：逐项剪空白、跳过空串与重复项、只留前 kGotoHistoryLimit 条。
    QStringList NormalizeHistory(const QStringList& history)
    {
        QStringList cleaned;
        for (const QString& raw : history)
        {
            const QString entry = raw.trimmed();
            if (entry.isEmpty() || cleaned.contains(entry))
            {
                continue;
            }
            cleaned.push_back(entry);
            if (cleaned.size() >= kGotoHistoryLimit)
            {
                break;
            }
        }
        return cleaned;
    }

    // 读跳转历史。
    QStringList LoadGotoHistory()
    {
        const QSettings settings;
        if (settings.status() != QSettings::NoError)
        {
            return QStringList();
        }

        // 键缺失返回空；单条历史在 INI 里可能读回成 QString，toStringList 对两种形态都给出正确列表。
        const QVariant stored = settings.value(GotoHistoryKey());
        if (!stored.isValid())
        {
            return QStringList();
        }
        return NormalizeHistory(stored.toStringList());
    }

    // 写跳转历史：保存前清理，空列表直接移除键，读回时就是"没有历史"。
    void SaveGotoHistory(const QStringList& history)
    {
        QSettings settings;
        const QStringList cleaned = NormalizeHistory(history);
        if (cleaned.isEmpty())
        {
            settings.remove(GotoHistoryKey());
        }
        else
        {
            settings.setValue(GotoHistoryKey(), cleaned);
        }
        settings.sync();
    }

    // 把新输入放到最前面。
    QStringList PushHistory(const QStringList& history, const QString& entry)
    {
        const QString trimmed = entry.trimmed();
        if (trimmed.isEmpty())
        {
            return NormalizeHistory(history);
        }

        // 新项排最前，旧项里与它相同的会在 NormalizeHistory 的去重里被丢掉（保留先出现者，即新项）。
        QStringList merged;
        merged.push_back(trimmed);
        merged.append(history);
        return NormalizeHistory(merged);
    }
}
