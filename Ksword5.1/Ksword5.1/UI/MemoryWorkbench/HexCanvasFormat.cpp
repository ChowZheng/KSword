// HexCanvasFormat.cpp
// 作用：HexCanvas 文本格式化与十六进制文本解析的实现，见 HexCanvasFormat.h。

#include "HexCanvasFormat.h"

#include <QChar>
#include <QRegularExpression>
#include <QStringList>

namespace ks::ui::hexcanvas_format
{
    namespace
    {
        // kHexDigits：大写十六进制字符表，按半字节取值索引。
        const char kHexDigits[] = "0123456789ABCDEF";

        // ToHexPair：把一个字节转成两个大写十六进制字符。
        // 传入：字节值；传出：两字符文本。
        QString ToHexPair(std::uint8_t value)
        {
            // pair：返回的两位文本，高半字节在前。
            QString pair;
            pair.reserve(2);
            pair.append(QLatin1Char(kHexDigits[(value >> 4) & 0x0F]));
            pair.append(QLatin1Char(kHexDigits[value & 0x0F]));
            return pair;
        }

        // IsHexDigitChar：判断字符是否是十六进制数字字符（大小写均可）。
        // 传入：字符；传出：是否是 0-9/a-f/A-F。
        bool IsHexDigitChar(QChar ch)
        {
            const char16_t code = ch.unicode();
            return (code >= u'0' && code <= u'9')
                || (code >= u'a' && code <= u'f')
                || (code >= u'A' && code <= u'F');
        }

        // HexDigitValue：把十六进制数字字符换成 0..15。
        // 调用前必须已用 IsHexDigitChar 校验；传出：数值。
        int HexDigitValue(QChar ch)
        {
            const char16_t code = ch.unicode();
            if (code >= u'0' && code <= u'9')
            {
                return static_cast<int>(code - u'0');
            }
            if (code >= u'a' && code <= u'f')
            {
                return static_cast<int>(code - u'a') + 10;
            }
            return static_cast<int>(code - u'A') + 10;
        }
    }

    // 判断一个字节是否是可见 ASCII。
    bool IsPrintableAscii(std::uint8_t value)
    {
        return value >= 0x20U && value <= 0x7EU;
    }

    // 大写空格分隔的十六进制。
    QString FormatHexText(const QByteArray& bytes)
    {
        // text：结果文本，预留每字节三个字符的容量。
        QString text;
        text.reserve(static_cast<qsizetype>(bytes.size()) * 3);

        // 逐字节追加，字节之间放一个空格，首字节前不放。
        for (qsizetype index = 0; index < bytes.size(); ++index)
        {
            if (index != 0)
            {
                text.append(QLatin1Char(' '));
            }
            text.append(ToHexPair(static_cast<std::uint8_t>(bytes.at(index))));
        }
        return text;
    }

    // 所见即所得的 ASCII 文本。
    QString FormatAsciiText(const QByteArray& bytes)
    {
        // text：结果文本，与字节数等长。
        QString text;
        text.reserve(static_cast<qsizetype>(bytes.size()));
        for (qsizetype index = 0; index < bytes.size(); ++index)
        {
            const std::uint8_t value = static_cast<std::uint8_t>(bytes.at(index));
            text.append(IsPrintableAscii(value) ? QChar(static_cast<char16_t>(value)) : QLatin1Char('.'));
        }
        return text;
    }

    // C 数组初始化列表。
    QString FormatCArray(const QByteArray& bytes)
    {
        // 空序列输出带空格的一对花括号，保持可直接粘贴。
        if (bytes.isEmpty())
        {
            return QStringLiteral("{ }");
        }

        // kBytesPerLine：多行输出时每行的字节数。
        constexpr qsizetype kBytesPerLine = 16;

        // 不超过一行：单行 "{ 0x41, 0x42 }"。
        QString text;
        if (bytes.size() <= kBytesPerLine)
        {
            text = QStringLiteral("{ ");
            for (qsizetype index = 0; index < bytes.size(); ++index)
            {
                if (index != 0)
                {
                    text.append(QStringLiteral(", "));
                }
                text.append(QStringLiteral("0x"));
                text.append(ToHexPair(static_cast<std::uint8_t>(bytes.at(index))));
            }
            text.append(QStringLiteral(" }"));
            return text;
        }

        // 超过一行：花括号独占一行，每行 16 个字节缩进四个空格，末行不带逗号。
        text = QStringLiteral("{\n");
        for (qsizetype index = 0; index < bytes.size(); ++index)
        {
            // 行首：缩进四个空格。
            if (index % kBytesPerLine == 0)
            {
                text.append(QStringLiteral("    "));
            }
            text.append(QStringLiteral("0x"));
            text.append(ToHexPair(static_cast<std::uint8_t>(bytes.at(index))));

            // 行尾：最后一个字节只换行，其余字节先放逗号。
            const bool isLast = (index + 1 == bytes.size());
            const bool isLineEnd = ((index + 1) % kBytesPerLine == 0);
            if (!isLast)
            {
                text.append(QLatin1Char(','));
                text.append(isLineEnd ? QLatin1Char('\n') : QLatin1Char(' '));
            }
            else
            {
                text.append(QLatin1Char('\n'));
            }
        }
        text.append(QLatin1Char('}'));
        return text;
    }

    // Python bytes 字面量。
    QString FormatPythonBytes(const QByteArray& bytes)
    {
        // text：结果文本，b'...' 的形式，所有字节都转义，避免引号冲突。
        // 前缀用单字符拼接而不是 "b'" 字面量：后者会被 i18n 审计误当成待翻译文案。
        QString text;
        text.append(QLatin1Char('b'));
        text.append(QLatin1Char('\''));
        for (qsizetype index = 0; index < bytes.size(); ++index)
        {
            text.append(QStringLiteral("\\x"));
            text.append(ToHexPair(static_cast<std::uint8_t>(bytes.at(index))));
        }
        text.append(QLatin1Char('\''));
        return text;
    }

    // C 风格转义字符串。
    QString FormatEscapedString(const QByteArray& bytes)
    {
        // text：结果文本；previousWasHexEscape：上一个输出是否是 \xNN（影响下一个十六进制字符的写法）。
        QString text = QStringLiteral("\"");
        bool previousWasHexEscape = false;

        for (qsizetype index = 0; index < bytes.size(); ++index)
        {
            const std::uint8_t value = static_cast<std::uint8_t>(bytes.at(index));
            const QChar asChar(static_cast<char16_t>(value));

            // 引号与反斜杠必须转义，其余可见字符原样输出。
            // 例外：紧跟在 \xNN 后面的十六进制数字字符会被 C 编译器吞进同一个转义，改写为 \xNN。
            const bool printable = IsPrintableAscii(value);
            const bool ambiguous = previousWasHexEscape && printable && IsHexDigitChar(asChar);
            if (printable && !ambiguous)
            {
                if (value == static_cast<std::uint8_t>('"'))
                {
                    text.append(QStringLiteral("\\\""));
                }
                else if (value == static_cast<std::uint8_t>('\\'))
                {
                    text.append(QStringLiteral("\\\\"));
                }
                else
                {
                    text.append(asChar);
                }
                previousWasHexEscape = false;
                continue;
            }

            // 其余字节统一用 \xNN。
            text.append(QStringLiteral("\\x"));
            text.append(ToHexPair(value));
            previousWasHexEscape = true;
        }
        text.append(QLatin1Char('"'));
        return text;
    }

    // 地址文本。
    QString FormatAddress(std::uint64_t address, int digits)
    {
        // body：十六进制数字部分，大写并左补零到 digits 位。
        QString body = QString::number(static_cast<qulonglong>(address), 16).toUpper();
        if (body.size() < digits)
        {
            body = body.rightJustified(digits, QLatin1Char('0'));
        }
        return QStringLiteral("0x") + body;
    }

    // 解析粘贴来的十六进制文本。
    bool ParseHexText(const QString& text, QByteArray* bytesOut, QString* errorOut)
    {
        // 先把允许的分隔符统一成空格，再按空白切记号。
        // normalized：统一分隔符之后的文本。
        QString normalized = text;
        for (QChar& ch : normalized)
        {
            if (ch == QLatin1Char(',') || ch == QLatin1Char(';') || ch == QLatin1Char('{') || ch == QLatin1Char('}'))
            {
                ch = QLatin1Char(' ');
            }
        }
        const QStringList tokens = normalized.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);

        // 没有任何记号：视为失败，空粘贴不应该悄悄成功。
        if (tokens.isEmpty())
        {
            if (errorOut != nullptr)
            {
                *errorOut = QStringLiteral("剪贴板里没有可粘贴的十六进制内容");
            }
            return false;
        }

        // result：解析出来的字节；任何一个记号失败都整体丢弃。
        QByteArray result;
        for (const QString& rawToken : tokens)
        {
            // 去掉可选的 0x/0X 前缀；前缀后必须还有数字。
            QString token = rawToken;
            if (token.size() >= 2 && token.at(0) == QLatin1Char('0')
                && (token.at(1) == QLatin1Char('x') || token.at(1) == QLatin1Char('X')))
            {
                token = token.mid(2);
            }

            // 逐字符校验：出现非十六进制字符立即整体失败。
            for (const QChar ch : token)
            {
                if (!IsHexDigitChar(ch))
                {
                    if (errorOut != nullptr)
                    {
                        *errorOut = QStringLiteral("包含非十六进制内容：%1").arg(rawToken);
                    }
                    return false;
                }
            }

            // 位数必须为偶数且非空：奇数位无法确定哪一半字节缺失，不猜测。
            if (token.isEmpty() || (token.size() % 2) != 0)
            {
                if (errorOut != nullptr)
                {
                    *errorOut = QStringLiteral("十六进制记号的位数必须是偶数：%1").arg(rawToken);
                }
                return false;
            }

            // 每两位组成一个字节。
            for (qsizetype index = 0; index < token.size(); index += 2)
            {
                const int high = HexDigitValue(token.at(index));
                const int low = HexDigitValue(token.at(index + 1));
                result.append(static_cast<char>((high << 4) | low));
            }
        }

        if (bytesOut != nullptr)
        {
            *bytesOut = result;
        }
        return true;
    }
}
