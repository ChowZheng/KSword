// HexViewFormat.cpp
// 作用：HexViewFormat.h 里三个纯函数的实现。

#include "HexViewFormat.h"

namespace ks::ui::hexview_format
{
    // 地址位数：两端都放得进 32 位用 8 位，否则 16 位。
    int AddressDigitsFor(std::uint64_t first, std::uint64_t last)
    {
        // kLimit：32 位地址的上界，超过它就必须用 16 位显示。
        constexpr std::uint64_t kLimit = 0xFFFFFFFFULL;
        return (first > kLimit || last > kLimit) ? 16 : 8;
    }

    // 字节偏移 -> 字符序号：把偏移之前的 UTF-8 字节解码，数出得到多少个 QChar。
    std::size_t CharIndexFromUtf8Offset(const QByteArray& utf8, std::size_t byteOffset)
    {
        // clamped：夹取到输入长度之内，越界偏移按"指向末尾"处理。
        const std::size_t clamped = byteOffset > static_cast<std::size_t>(utf8.size())
            ? static_cast<std::size_t>(utf8.size())
            : byteOffset;
        const QString prefix = QString::fromUtf8(utf8.constData(), static_cast<qsizetype>(clamped));
        return static_cast<std::size_t>(prefix.size());
    }

    // 纯文本悬停提示：先转义再包进 pre 块，保留换行与连续空格。
    QString PlainToolTip(const QString& plainText)
    {
        if (plainText.isEmpty())
        {
            return QString();
        }
        return QStringLiteral("<div style=\"white-space:pre\">%1</div>").arg(plainText.toHtmlEscaped());
    }
}
