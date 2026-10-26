#include "StartupSplashLayout.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace ks::splash
{
    namespace
    {
        constexpr float kLeft = 44.0f; // 品牌、信息行和窗口右侧采用相同边距。
        constexpr float kPieceGap = 10.0f; // 信息片段之间只使用固定间距。
        constexpr float kMetadataBaseline = FooterHeight - 30.0f; // 信息与网站共用240基线。
        constexpr float kCopyrightBaseline = 216.0f; // 版权独占网站上方一行。
        constexpr float kStatusBaseline = 190.0f; // 状态与百分比共用190基线。

        enum class PieceKind
        {
            Text,
            Badge,
            Slash,
            Authors
        };

        // 每个片段的可见文字与绘制角色分开记录，避免在排版中猜测分隔符。
        struct Piece final
        {
            PieceKind kind;
            std::wstring value;
        };

        // 格式对象在测量与绘制时复用同一排版规则，强制单行且不引入隐式缩排。
        void configureOneLine(Gdiplus::StringFormat& format)
        {
            format.SetAlignment(Gdiplus::StringAlignmentNear);
            format.SetLineAlignment(Gdiplus::StringAlignmentNear);
            format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
            format.SetTrimming(Gdiplus::StringTrimmingNone);
        }

        // 实际字体的cell ascent决定基线位置，不能以固定矩形顶边冒充基线。
        float fontAscent(const Gdiplus::Font& font)
        {
            Gdiplus::FontFamily family;
            if (font.GetFamily(&family) != Gdiplus::Ok)
                return font.GetSize();

            const INT style = font.GetStyle();
            const UINT16 emHeight = family.GetEmHeight(style);
            const UINT16 ascent = family.GetCellAscent(style);
            if (emHeight == 0)
                return font.GetSize();
            return font.GetSize() * static_cast<float>(ascent) / static_cast<float>(emHeight);
        }

        // MeasureString使用与DrawString相同的GenericTypographic格式，返回逻辑宽度。
        float textWidth(Gdiplus::Graphics& graphics, const std::wstring& value,
            const Gdiplus::Font& font, const Gdiplus::StringFormat& format)
        {
            if (value.empty())
                return 0.0f;
            Gdiplus::RectF measured;
            if (graphics.MeasureString(value.c_str(), -1, &font,
                    Gdiplus::PointF(0.0f, 0.0f), &format, &measured) != Gdiplus::Ok)
                return 0.0f;
            return std::max(0.0f, measured.Width);
        }

        // 文本顶点由实际字体上升量反推，使不同字重的可见基线一致。
        void drawAtBaseline(Gdiplus::Graphics& graphics, const std::wstring& value,
            const Gdiplus::Font& font, const Gdiplus::StringFormat& format,
            const Gdiplus::Brush& brush, float x, float baseline)
        {
            if (value.empty())
                return;
            graphics.DrawString(value.c_str(), -1, &font,
                Gdiplus::PointF(x, baseline - fontAscent(font)), &format, &brush);
        }

        // 对Unicode代理项边界做保护，省略符不会拆开一个补充平面字符。
        size_t safePrefixLength(const std::wstring& value, size_t length)
        {
            if (length > 0 && length < value.size() &&
                value[length - 1] >= 0xD800 && value[length - 1] <= 0xDBFF &&
                value[length] >= 0xDC00 && value[length] <= 0xDFFF)
                --length;
            return length;
        }

        // 最后一个可见片段在容量不足时才添加单个省略符，始终保持单行。
        std::wstring ellipsize(Gdiplus::Graphics& graphics, const std::wstring& value,
            const Gdiplus::Font& font, const Gdiplus::StringFormat& format, float width)
        {
            if (value.empty() || width <= 0.0f)
                return {};
            if (textWidth(graphics, value, font, format) <= width)
                return value;

            constexpr wchar_t ellipsis[] = L"\u2026";
            if (textWidth(graphics, ellipsis, font, format) > width)
                return {};
            size_t low = 0;
            size_t high = value.size();
            while (low < high)
            {
                const size_t middle = low + (high - low + 1) / 2;
                const std::wstring candidate = value.substr(0, safePrefixLength(value, middle)) + ellipsis;
                if (textWidth(graphics, candidate, font, format) <= width)
                    low = middle;
                else
                    high = middle - 1;
            }
            return value.substr(0, safePrefixLength(value, low)) + ellipsis;
        }

        // 外部元信息偶有换行或制表符时改成空格，页脚永远不被输入拆行。
        std::wstring oneLine(std::wstring value)
        {
            for (wchar_t& character : value)
                if (character == L'\r' || character == L'\n' || character == L'\t')
                    character = L' ';
            return value;
        }

        // 发行版本以数字前缀和阶段后缀组成；仅阶段标签转成大写。
        std::pair<std::wstring, std::wstring> splitVersion(const std::wstring& raw)
        {
            const std::wstring version = oneLine(raw);
            const size_t suffix = version.find_first_not_of(L"0123456789.");
            if (suffix == 0 || suffix == std::wstring::npos)
                return {version, {}};
            std::wstring stage = version.substr(suffix);
            const size_t first = stage.find_first_not_of(L" -+");
            stage = first == std::wstring::npos ? std::wstring() : stage.substr(first);
            for (wchar_t& character : stage)
                if (character >= L'a' && character <= L'z')
                    character = static_cast<wchar_t>(character - L'a' + L'A');
            return {version.substr(0, suffix), stage};
        }

        // 构建标签保留原本地化内容，日期与时间之间显示一处分隔斜杠。
        std::wstring buildLabel(std::wstring value)
        {
            value = oneLine(std::move(value));
            const size_t separator = value.find_last_of(L' ');
            if (separator != std::wstring::npos &&
                value.find(L':', separator) != std::wstring::npos)
                value.replace(separator, 1, L" / ");
            return value;
        }

        // 发行元数据中的作者点分隔符统一换成紧凑单行斜杠。
        std::wstring authorLabel(std::wstring value)
        {
            value = oneLine(std::move(value));
            size_t position = 0;
            while ((position = value.find(L" \u00b7 ", position)) != std::wstring::npos)
            {
                value.replace(position, 3, L" / ");
                position += 3;
            }
            return value;
        }

        // 网站只移除协议与尾斜杠；绘制宽度仍由真实剩余文字测得。
        std::wstring websiteLabel(std::wstring value)
        {
            value = oneLine(std::move(value));
            const size_t scheme = value.find(L"://");
            if (scheme != std::wstring::npos)
                value.erase(0, scheme + 3);
            while (!value.empty() && value.back() == L'/')
                value.pop_back();
            return value;
        }

        // 用片段身份生成版本、构建与作者之间必要的斜杠，空片段不会留下孤立分隔符。
        std::vector<Piece> makePieces(const FooterText& text)
        {
            const auto version = splitVersion(text.version);
            const std::wstring build = buildLabel(text.buildTime);
            const std::wstring authors = authorLabel(text.authors);
            std::vector<Piece> pieces;
            if (!version.first.empty())
                pieces.push_back({PieceKind::Text, version.first});
            if (!version.second.empty())
                pieces.push_back({PieceKind::Badge, version.second});
            if (!build.empty())
            {
                if (!pieces.empty())
                    pieces.push_back({PieceKind::Slash, L"/"});
                pieces.push_back({PieceKind::Text, build});
            }
            if (!authors.empty())
            {
                if (!pieces.empty())
                    pieces.push_back({PieceKind::Slash, L"/"});
                pieces.push_back({PieceKind::Authors, authors});
            }
            return pieces;
        }

        // PRE徽标按当前字号的真实字宽加左右各6px，而非为某个版本写死宽度。
        float pieceWidth(Gdiplus::Graphics& graphics, const Piece& piece,
            const Gdiplus::Font& font, const Gdiplus::StringFormat& format)
        {
            const float width = textWidth(graphics, piece.value, font, format);
            return piece.kind == PieceKind::Badge ? width + 12.0f : width;
        }

        // 测量整个元信息行，只有全行超出网站留下的空间才统一降至11px。
        float piecesWidth(Gdiplus::Graphics& graphics, const std::vector<Piece>& pieces,
            const Gdiplus::Font& font, const Gdiplus::StringFormat& format)
        {
            float width = 0.0f;
            for (size_t index = 0; index < pieces.size(); ++index)
            {
                if (index != 0)
                    width += kPieceGap;
                width += pieceWidth(graphics, pieces[index], font, format);
            }
            return width;
        }

        // 缺失Logo的方块与原生页脚保持相同尺寸及轻微内部圆角。
        void drawLogoFallback(Gdiplus::Graphics& graphics)
        {
            Gdiplus::GraphicsPath shape;
            shape.AddArc(kLeft, 136.0f, 20.0f, 20.0f, 180.0f, 90.0f);
            shape.AddArc(kLeft + 32.0f, 136.0f, 20.0f, 20.0f, 270.0f, 90.0f);
            shape.AddArc(kLeft + 32.0f, 168.0f, 20.0f, 20.0f, 0.0f, 90.0f);
            shape.AddArc(kLeft, 168.0f, 20.0f, 20.0f, 90.0f, 90.0f);
            shape.CloseFigure();
            Gdiplus::SolidBrush fill(Gdiplus::Color(255, 28, 70, 125));
            graphics.FillPath(&fill, &shape);
        }

        // 标题保留图标右侧320px；所有品牌文字都受该矩形限制。
        void drawBrand(Gdiplus::Graphics& graphics, Gdiplus::Image* logo,
            const std::wstring& title)
        {
            if (logo != nullptr)
                graphics.DrawImage(logo, Gdiplus::RectF(kLeft, 136.0f, 52.0f, 52.0f));
            else
                drawLogoFallback(graphics);

            Gdiplus::Font font(L"Segoe UI", 44.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
            format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
            format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::SolidBrush brush(Gdiplus::Color(255, 245, 248, 253));
            graphics.DrawString(title.c_str(), -1, &font,
                Gdiplus::RectF(112.0f, 128.0f, 320.0f, 68.0f), &format, &brush);
        }
    }

    void drawFooter(Gdiplus::Graphics& graphics, float width, Gdiplus::Image* logo,
        const FooterText& text)
    {
        drawBrand(graphics, logo, oneLine(text.title));

        // 当前元信息与右站点共用字体、基线和真实宽度，网站先占右侧空间。
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        configureOneLine(format);
        const std::vector<Piece> originalPieces = makePieces(text);
        const std::wstring rawSite = websiteLabel(text.website);
        const float rightEdge = width - kLeft;
        float fontSize = 12.5f;
        Gdiplus::Font normalFont(L"Microsoft YaHei UI", fontSize,
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        const float siteLimit = std::max(0.0f, width * 0.5f - kLeft);
        std::wstring site = ellipsize(graphics, rawSite, normalFont, format, siteLimit);
        float siteWidth = textWidth(graphics, site, normalFont, format);
        float metaRight = rightEdge - (site.empty() ? 0.0f : siteWidth + kPieceGap);
        float available = std::max(0.0f, metaRight - kLeft);

        // 第一档放不下才整体改用11px，包括右网站，不单独压缩某一列。
        if (piecesWidth(graphics, originalPieces, normalFont, format) > available)
            fontSize = 11.0f;
        Gdiplus::Font font(L"Microsoft YaHei UI", fontSize,
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        site = ellipsize(graphics, rawSite, font, format, siteLimit);
        siteWidth = textWidth(graphics, site, font, format);
        metaRight = rightEdge - (site.empty() ? 0.0f : siteWidth + kPieceGap);
        available = std::max(0.0f, metaRight - kLeft);

        // 优先保留版本、阶段和完整构建信息；最后的作者按剩余空间末尾省略。
        std::vector<Piece> pieces = originalPieces;
        if (!pieces.empty() && pieces.back().kind == PieceKind::Authors)
        {
            const Piece authors = pieces.back();
            pieces.pop_back();
            const float prefixWidth = piecesWidth(graphics, pieces, font, format);
            const float authorRoom = available - prefixWidth - (pieces.empty() ? 0.0f : kPieceGap);
            pieces.push_back({PieceKind::Authors,
                ellipsize(graphics, authors.value, font, format, authorRoom)});
            if (pieces.back().value.empty())
            {
                pieces.pop_back();
                if (!pieces.empty() && pieces.back().kind == PieceKind::Slash)
                    pieces.pop_back();
            }
        }

        // 最后的裁剪保护极端外部版本输入；当前发行字段全宽显示且不侵入站点。
        const Gdiplus::GraphicsState metadataState = graphics.Save();
        graphics.SetClip(Gdiplus::RectF(kLeft, 205.0f, available, 49.0f),
            Gdiplus::CombineModeIntersect);
        Gdiplus::SolidBrush metadata(Gdiplus::Color(255, 174, 187, 205));
        Gdiplus::SolidBrush separator(Gdiplus::Color(255, 93, 108, 130));
        const Gdiplus::Color accent(255,
            static_cast<BYTE>((text.accentRgb >> 16) & 0xffU),
            static_cast<BYTE>((text.accentRgb >> 8) & 0xffU),
            static_cast<BYTE>(text.accentRgb & 0xffU));
        Gdiplus::SolidBrush stageBrush(accent);
        float x = kLeft;
        for (size_t index = 0; index < pieces.size(); ++index)
        {
            const Piece& piece = pieces[index];
            const float currentWidth = pieceWidth(graphics, piece, font, format);
            if (piece.kind == PieceKind::Badge)
            {
                const float top = kMetadataBaseline - fontAscent(font) - 3.0f;
                const float height = fontSize + 7.0f;
                Gdiplus::Pen border(accent, 0.8f);
                Gdiplus::GraphicsPath shape;
                const float diameter = 6.0f;
                shape.AddArc(x, top, diameter, diameter, 180.0f, 90.0f);
                shape.AddArc(x + currentWidth - diameter, top, diameter, diameter, 270.0f, 90.0f);
                shape.AddArc(x + currentWidth - diameter, top + height - diameter,
                    diameter, diameter, 0.0f, 90.0f);
                shape.AddArc(x, top + height - diameter, diameter, diameter, 90.0f, 90.0f);
                shape.CloseFigure();
                graphics.DrawPath(&border, &shape);
                drawAtBaseline(graphics, piece.value, font, format, stageBrush,
                    x + 6.0f, kMetadataBaseline);
            }
            else
            {
                const Gdiplus::Brush& brush = piece.kind == PieceKind::Slash
                    ? static_cast<const Gdiplus::Brush&>(separator)
                    : static_cast<const Gdiplus::Brush&>(metadata);
                drawAtBaseline(graphics, piece.value, font, format, brush,
                    x, kMetadataBaseline);
            }
            x += currentWidth + (index + 1 < pieces.size() ? kPieceGap : 0.0f);
        }
        graphics.Restore(metadataState);

        // 版权与站点从同一右边缘回排，网站实测宽度已用于元信息裁剪。
        Gdiplus::SolidBrush legal(Gdiplus::Color(255, 153, 168, 188));
        const float legalLimit = std::max(0.0f, width * 0.5f - kLeft);
        const std::wstring compactCopyright = ellipsize(graphics,
            oneLine(text.copyright), font, format, legalLimit);
        drawAtBaseline(graphics, compactCopyright, font, format, legal,
            rightEdge - textWidth(graphics, compactCopyright, font, format),
            kCopyrightBaseline);
        drawAtBaseline(graphics, site, font, format, legal,
            rightEdge - siteWidth, kMetadataBaseline);
    }

    Gdiplus::RectF statusBounds(float width)
    {
        // 以右半区固定矩形恢复状态与数字，不依赖当前文案的可见宽度。
        return Gdiplus::RectF(width * 0.5f, 166.0f,
            std::max(0.0f, width * 0.5f - kLeft), 34.0f);
    }

    void drawStatus(Gdiplus::Graphics& graphics, float width,
        const std::wstring& status, bool showPercent, int percent)
    {
        // 百分比仅在确定模式存在；状态与数字以同一14px字体和190基线测量。
        Gdiplus::Font font(L"Microsoft YaHei UI", 14.0f,
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        configureOneLine(format);
        const std::wstring percentText = showPercent
            ? std::to_wstring(std::clamp(percent, 0, 100)) + L"%" : std::wstring();
        const float percentWidth = textWidth(graphics, percentText, font, format);
        const float groupLimit = std::max(0.0f, width * 0.5f - kLeft);
        const float gap = !percentText.empty() && !status.empty() ? kPieceGap : 0.0f;
        const float statusRoom = std::max(0.0f, groupLimit - percentWidth - gap);
        const std::wstring visibleStatus = ellipsize(graphics, oneLine(status),
            font, format, statusRoom);
        const float statusWidth = textWidth(graphics, visibleStatus, font, format);
        const float visibleGap = !visibleStatus.empty() && !percentText.empty() ? kPieceGap : 0.0f;
        float x = width - kLeft - statusWidth - visibleGap - percentWidth;

        // 裁剪固定动态区域，使极端长文本不会污染缓存之外的静态像素。
        const Gdiplus::GraphicsState state = graphics.Save();
        graphics.SetClip(statusBounds(width), Gdiplus::CombineModeIntersect);
        Gdiplus::SolidBrush statusBrush(Gdiplus::Color(255, 211, 220, 233));
        Gdiplus::SolidBrush percentBrush(Gdiplus::Color(255, 169, 184, 205));
        drawAtBaseline(graphics, visibleStatus, font, format,
            statusBrush, x, kStatusBaseline);
        x += statusWidth + visibleGap;
        drawAtBaseline(graphics, percentText, font, format,
            percentBrush, x, kStatusBaseline);
        graphics.Restore(state);
    }
}
