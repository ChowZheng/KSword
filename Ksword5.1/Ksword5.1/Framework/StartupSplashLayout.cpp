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
        constexpr float kMetadataBaseline = FooterHeight - 30.0f; // 元信息与版权共用距底30px的基线。
        constexpr float kGroupGap = 20.0f; // 底部左右两组之间留出明确间隔。
        constexpr float kBrandRowTop = 128.0f; // 标题、图标和状态共用上行的垂直中心。
        constexpr float kBrandRowHeight = 68.0f;
        constexpr float kBrandCenter = kBrandRowTop + kBrandRowHeight * 0.5f;

        enum class PieceKind
        {
            Text,
            Badge,
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

        // 不同字号按同一文字单元中心对齐，不能把大标题和小状态强行放在同一顶边。
        float centeredBaseline(const Gdiplus::Font& font, float center)
        {
            Gdiplus::FontFamily family;
            if (font.GetFamily(&family) != Gdiplus::Ok)
                return center + font.GetSize() * 0.3f;
            const INT style = font.GetStyle();
            const UINT16 emHeight = family.GetEmHeight(style);
            if (emHeight == 0)
                return center + font.GetSize() * 0.3f;
            const float ascent = static_cast<float>(family.GetCellAscent(style));
            const float descent = static_cast<float>(family.GetCellDescent(style));
            return center + (ascent - descent) * font.GetSize() /
                (2.0f * static_cast<float>(emHeight));
        }

        // 字体单元包含不可见留白；对齐必须使用随后实际填充的同一份字形轮廓。
        bool textOutline(const std::wstring& value, const Gdiplus::Font& font,
            const Gdiplus::StringFormat& format, Gdiplus::GraphicsPath& path,
            Gdiplus::RectF& ink)
        {
            if (value.empty())
                return false;
            Gdiplus::FontFamily family; // 当前三个字号均为UnitPixel，轮廓坐标与页脚逻辑像素一致。
            if (font.GetFamily(&family) != Gdiplus::Ok ||
                path.AddString(value.c_str(), -1, &family, font.GetStyle(), font.GetSize(),
                    Gdiplus::PointF(0.0f, 0.0f), &format) != Gdiplus::Ok ||
                path.GetPointCount() == 0 || path.GetBounds(&ink) != Gdiplus::Ok)
                return false;
            return ink.Width > 0.0f && ink.Height > 0.0f;
        }

        // 测量和绘制共用字形边界，不再把GDI+附加的前后留白当成片段宽度。
        float textWidth(Gdiplus::Graphics& graphics, const std::wstring& value,
            const Gdiplus::Font& font, const Gdiplus::StringFormat& format)
        {
            if (value.empty())
                return 0.0f;
            Gdiplus::GraphicsPath path;
            Gdiplus::RectF ink;
            if (textOutline(value, font, format, path, ink))
                return ink.Width;
            Gdiplus::RectF measured; // 字体异常或无轮廓时保留既有原生文本回退。
            if (graphics.MeasureString(value.c_str(), -1, &font,
                    Gdiplus::PointF(0.0f, 0.0f), &format, &measured) != Gdiplus::Ok)
                return 0.0f;
            return std::max(0.0f, measured.Width);
        }

        // 元信息保持共同基线，横向起点则精确落在可见字形左沿。
        void drawAtBaseline(Gdiplus::Graphics& graphics, const std::wstring& value,
            const Gdiplus::Font& font, const Gdiplus::StringFormat& format,
            const Gdiplus::Brush& brush, float x, float baseline)
        {
            if (value.empty())
                return;
            Gdiplus::GraphicsPath path;
            Gdiplus::RectF ink;
            if (textOutline(value, font, format, path, ink))
            {
                Gdiplus::Matrix placement(1.0f, 0.0f, 0.0f, 1.0f,
                    x - ink.X, baseline - fontAscent(font)); // 保留字体基线，不把下伸部挤进同一黑框。
                path.Transform(&placement);
                graphics.FillPath(&brush, &path);
                return;
            }
            graphics.DrawString(value.c_str(), -1, &font,
                Gdiplus::PointF(x, baseline - fontAscent(font)), &format, &brush);
        }

        // 品牌与状态按可见墨迹中心定位；CJK与百分比字高不同也能真正居中。
        void drawAtVisualCenter(Gdiplus::Graphics& graphics, const std::wstring& value,
            const Gdiplus::Font& font, const Gdiplus::StringFormat& format,
            const Gdiplus::Brush& brush, float x, float center)
        {
            if (value.empty())
                return;
            Gdiplus::GraphicsPath path;
            Gdiplus::RectF ink;
            if (textOutline(value, font, format, path, ink))
            {
                Gdiplus::Matrix placement(1.0f, 0.0f, 0.0f, 1.0f,
                    x - ink.X, center - ink.Y - ink.Height * 0.5f);
                path.Transform(&placement);
                graphics.FillPath(&brush, &path);
                return;
            }
            drawAtBaseline(graphics, value, font, format, brush, x,
                centeredBaseline(font, center)); // 仅字体无法提供轮廓时使用字体单元回退。
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

        // 元信息只按内容和间距分组，不向日期、时间或作者中插入装饰性斜杠。
        std::vector<Piece> makePieces(const FooterText& text)
        {
            const auto version = splitVersion(text.version);
            std::vector<Piece> pieces;
            if (!version.first.empty())
                pieces.push_back({PieceKind::Text, version.first});
            if (!version.second.empty())
                pieces.push_back({PieceKind::Badge, version.second});
            if (!text.buildTime.empty())
                pieces.push_back({PieceKind::Text, oneLine(text.buildTime)});
            if (!text.authors.empty())
                pieces.push_back({PieceKind::Authors, oneLine(text.authors)});
            return pieces;
        }

        // PRE徽标按当前字号的真实字宽加左右各6px，而非为某个版本写死宽度。
        float pieceWidth(Gdiplus::Graphics& graphics, const Piece& piece,
            const Gdiplus::Font& font, const Gdiplus::StringFormat& format)
        {
            const float width = textWidth(graphics, piece.value, font, format);
            return piece.kind == PieceKind::Badge ? width + 12.0f : width;
        }

        // 测量整个元信息行，只有全行超出版权留下的空间才统一降至11px。
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

        // 标题宽度止于左半区，与右侧状态保留16px安全缝。
        void drawBrand(Gdiplus::Graphics& graphics, Gdiplus::Image* logo,
            const std::wstring& title, float width)
        {
            if (logo != nullptr)
                graphics.DrawImage(logo, Gdiplus::RectF(kLeft, 136.0f, 52.0f, 52.0f));
            else
                drawLogoFallback(graphics);

            Gdiplus::Font font(L"Segoe UI", 44.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
            configureOneLine(format);
            const float titleWidth = std::max(0.0f, width * 0.5f - 16.0f - 112.0f); // 随窗口宽度收缩，而非侵入状态组。
            const std::wstring visible = ellipsize(graphics, title, font, format, titleWidth);
            const Gdiplus::GraphicsState state = graphics.Save(); // 保留标题左半区边界，长标题不侵入加载状态。
            graphics.SetClip(Gdiplus::RectF(112.0f, kBrandRowTop, titleWidth, kBrandRowHeight),
                Gdiplus::CombineModeIntersect);
            Gdiplus::SolidBrush brush(Gdiplus::Color(255, 245, 248, 253));
            drawAtVisualCenter(graphics, visible, font, format, brush,
                112.0f, kBrandCenter);
            graphics.Restore(state);
        }
    }

    void drawFooter(Gdiplus::Graphics& graphics, float width, Gdiplus::Image* logo,
        const FooterText& text)
    {
        drawBrand(graphics, logo, oneLine(text.title), width);

        // 元信息和版权属于同一底行，统一字体及基线，版权按实际字宽占据右侧。
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        configureOneLine(format);
        const std::vector<Piece> originalPieces = makePieces(text);
        const std::wstring rawCopyright = oneLine(text.copyright);
        const float rightEdge = width - kLeft;
        const float legalLimit = std::max(0.0f, width * 0.5f - kLeft);
        float fontSize = 12.5f;
        Gdiplus::Font normalFont(L"Microsoft YaHei UI", fontSize,
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        std::wstring compactCopyright = ellipsize(graphics, rawCopyright, normalFont, format, legalLimit);
        float copyrightWidth = textWidth(graphics, compactCopyright, normalFont, format);
        float available = std::max(0.0f, rightEdge - kLeft - copyrightWidth -
            (compactCopyright.empty() ? 0.0f : kGroupGap));

        // 首档放不下才对整行统一使用11px；换字号后重新测量左右两组。
        if (piecesWidth(graphics, originalPieces, normalFont, format) > available)
            fontSize = 11.0f;
        Gdiplus::Font font(L"Microsoft YaHei UI", fontSize,
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        compactCopyright = ellipsize(graphics, rawCopyright, font, format, legalLimit);
        copyrightWidth = textWidth(graphics, compactCopyright, font, format);
        available = std::max(0.0f, rightEdge - kLeft - copyrightWidth -
            (compactCopyright.empty() ? 0.0f : kGroupGap));

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
            }
        }

        // 最后的裁剪保护极端外部版本输入；当前发行字段全宽显示且不侵入右侧版权。
        const Gdiplus::GraphicsState metadataState = graphics.Save();
        graphics.SetClip(Gdiplus::RectF(kLeft, 205.0f, available, 49.0f),
            Gdiplus::CombineModeIntersect);
        Gdiplus::SolidBrush metadata(Gdiplus::Color(255, 174, 187, 205));
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
                Gdiplus::GraphicsPath glyphs; // 标签边框围住实际PRE墨迹，而不是整块字体单元。
                Gdiplus::RectF ink;
                const bool measured = textOutline(piece.value, font, format, glyphs, ink);
                const float inkTop = kMetadataBaseline - fontAscent(font) + (measured ? ink.Y : 0.0f);
                const float top = inkTop - 3.0f;
                const float height = (measured ? ink.Height : fontSize) + 6.0f;
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
                drawAtBaseline(graphics, piece.value, font, format, metadata,
                    x, kMetadataBaseline);
            }
            x += currentWidth + (index + 1 < pieces.size() ? kPieceGap : 0.0f);
        }
        graphics.Restore(metadataState);

        // 版权替代原网站位置，与左侧元信息共用最后一行；不再绘制任何网址。
        Gdiplus::SolidBrush legal(Gdiplus::Color(255, 153, 168, 188));
        drawAtBaseline(graphics, compactCopyright, font, format, legal,
            rightEdge - copyrightWidth, kMetadataBaseline);
    }

    Gdiplus::RectF statusBounds(float width)
    {
        // 以右半区固定矩形恢复状态与数字，不依赖当前文案的可见宽度。
        return Gdiplus::RectF(width * 0.5f, kBrandRowTop,
            std::max(0.0f, width * 0.5f - kLeft), kBrandRowHeight);
    }

    void drawStatus(Gdiplus::Graphics& graphics, float width,
        const std::wstring& status, bool showPercent, int percent)
    {
        // 百分比仅在确定模式存在；状态与数字同14px常规字体、同前景色，并按真实可见字形与品牌行共用中心。
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
        drawAtVisualCenter(graphics, visibleStatus, font, format,
            statusBrush, x, kBrandCenter);
        x += statusWidth + visibleGap;
        drawAtVisualCenter(graphics, percentText, font, format,
            statusBrush, x, kBrandCenter);
        graphics.Restore(state);
    }
}
