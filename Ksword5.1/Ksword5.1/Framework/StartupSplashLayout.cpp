#include "StartupSplashLayout.h"

#include <algorithm>

namespace ks::splash
{
    namespace
    {
        constexpr float kReferenceWidth = 1600.0f; // 用户设计稿宽度，所有字体与留白按同一尺度缩放。
        constexpr float kReferenceHeight = 900.0f; // 用户设计稿高度，背景仍由宿主完整等比显示。
        constexpr float kStatusBandHeight = 44.0f; // 动态状态区域在参考稿中的高度。

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

        // 中央版本是一行连续文本，阶段保留发行元数据原有大小写，不再画PRE徽标。
        std::wstring versionLabel(const std::wstring& raw)
        {
            const std::wstring value = oneLine(raw);
            const size_t suffix = value.find_first_not_of(L"0123456789.");
            if (suffix == 0 || suffix == std::wstring::npos)
                return value;
            const size_t stage = value.find_first_not_of(L" -+", suffix);
            return stage == std::wstring::npos ? value.substr(0, suffix)
                : value.substr(0, suffix) + L" " + value.substr(stage);
        }

        // 图标缺失时保留既有蓝色回退，但不在背景上增加面板或遮罩。
        void drawLogoFallback(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds)
        {
            const float diameter = std::min(bounds.Width, bounds.Height) * 0.2f;
            Gdiplus::GraphicsPath shape;
            shape.AddArc(bounds.X, bounds.Y, diameter, diameter, 180.0f, 90.0f);
            shape.AddArc(bounds.X + bounds.Width - diameter, bounds.Y,
                diameter, diameter, 270.0f, 90.0f);
            shape.AddArc(bounds.X + bounds.Width - diameter, bounds.Y + bounds.Height - diameter,
                diameter, diameter, 0.0f, 90.0f);
            shape.AddArc(bounds.X, bounds.Y + bounds.Height - diameter,
                diameter, diameter, 90.0f, 90.0f);
            shape.CloseFigure();
            Gdiplus::SolidBrush fill(Gdiplus::Color(255, 28, 70, 125));
            graphics.FillPath(&fill, &shape);
        }

        // 标题和版本使用实际可见字形宽度居中，长外部文本保持单行省略。
        void drawCenteredLine(Gdiplus::Graphics& graphics, const std::wstring& value,
            const Gdiplus::Font& font, const Gdiplus::StringFormat& format,
            const Gdiplus::Brush& brush, float width, float centerY, float margin)
        {
            const std::wstring visible = ellipsize(graphics, oneLine(value), font, format,
                std::max(0.0f, width - 2.0f * margin));
            const float visibleWidth = textWidth(graphics, visible, font, format);
            drawAtVisualCenter(graphics, visible, font, format, brush,
                (width - visibleWidth) * 0.5f, centerY);
        }
        // 单行元信息按真实字宽省略；日期时间和姓名不插入斜杠或硬换行。
        void drawLeftLine(Gdiplus::Graphics& graphics, const std::wstring& text,
            const Gdiplus::Font& font, const Gdiplus::StringFormat& format,
            const Gdiplus::Brush& brush, float left, float center, float width)
        {
            const std::wstring visible = ellipsize(graphics, oneLine(text), font, format, width);
            drawAtVisualCenter(graphics, visible, font, format, brush, left, center);
        }

        // 左右版复用完整源图作为右侧视觉区；文字集中于左列，不加半透明底板。
        void drawLeftRight(Gdiplus::Graphics& graphics, float width, float height,
            Gdiplus::Image* logo, const SplashText& text, float scale)
        {
            const float left = 64.0f * scale; // 与其它布局相同的参考稿外边距。
            const float columnWidth = std::min(600.0f * scale, width * 0.43f - left);
            const float logoSize = 96.0f * scale;
            const Gdiplus::RectF logoBounds(left, 60.0f * scale, logoSize, logoSize);
            if (logo != nullptr)
                graphics.DrawImage(logo, logoBounds);
            else
                drawLogoFallback(graphics, logoBounds);
            Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
            configureOneLine(format);
            Gdiplus::Font titleFont(L"Segoe UI", 50.0f * scale,
                Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush titleBrush(Gdiplus::Color(255, 241, 243, 245));
            drawLeftLine(graphics, text.title, titleFont, format, titleBrush,
                left + 120.0f * scale, 108.0f * scale, columnWidth - 120.0f * scale);

            Gdiplus::Font metadataFont(L"Microsoft YaHei UI", 24.0f * scale,
                Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush metadata(Gdiplus::Color(255, 174, 184, 198));
            drawLeftLine(graphics, versionLabel(text.version), metadataFont, format, metadata,
                left, 220.0f * scale, columnWidth);
            drawLeftLine(graphics, text.buildTime, metadataFont, format, metadata,
                left, 260.0f * scale, columnWidth);
            drawLeftLine(graphics, text.authors, metadataFont, format, metadata,
                left, 300.0f * scale, columnWidth);

            // 许可正文有独立段落宽度和自然高度，不改变下方状态区的可用高度。
            Gdiplus::StringFormat paragraph(Gdiplus::StringFormat::GenericTypographic());
            paragraph.SetAlignment(Gdiplus::StringAlignmentNear);
            paragraph.SetLineAlignment(Gdiplus::StringAlignmentNear);
            paragraph.SetFormatFlags(paragraph.GetFormatFlags() & ~Gdiplus::StringFormatFlagsNoWrap);
            paragraph.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
            float top = 380.0f * scale;
            for (const std::wstring* body : {&text.license, &text.thirdParty})
            {
                if (body->empty()) continue;
                const float available = std::max(0.0f, height - 160.0f * scale - top);
                if (available <= 0.0f) break;
                const Gdiplus::RectF area(left, top, columnWidth, available);
                Gdiplus::RectF measured;
                graphics.MeasureString(body->c_str(), -1, &metadataFont, area, &paragraph, &measured);
                const float bodyHeight = std::min(available, measured.Height + 2.0f * scale);
                graphics.DrawString(body->c_str(), -1, &metadataFont,
                    Gdiplus::RectF(left, top, columnWidth, bodyHeight), &paragraph, &metadata);
                top += bodyHeight + 18.0f * scale;
            }
        }

    }

    Gdiplus::RectF drawStatic(Gdiplus::Graphics& graphics, float width, float height,
        Gdiplus::Image* logo, const SplashText& text)
    {
        if (width <= 0.0f || height <= 0.0f)
            return {};
        const float scale = std::min(width / kReferenceWidth, height / kReferenceHeight);
        const float margin = 64.0f * scale; // 左下状态和右下版权采用相同的外边距。
        const float footerCenter = height - 62.0f * scale;
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        configureOneLine(format);

        if (text.layout == SplashLayoutStyle::LeftRight)
        {
            drawLeftRight(graphics, width, height, logo, text, scale);
        }
        else
        {
            // 参考稿中央图标位于297/900高度，完整显示原有透明Logo资源。
            const float logoSize = 168.0f * scale;
            const Gdiplus::RectF logoBounds((width - logoSize) * 0.5f,
                height * (297.0f / kReferenceHeight) - logoSize * 0.5f, logoSize, logoSize);
            if (logo != nullptr)
                graphics.DrawImage(logo, logoBounds);
            else
                drawLogoFallback(graphics, logoBounds);

            // 品牌与版本纵向居中排列；字体和色值取自用户提供的参考布局。
            Gdiplus::Font titleFont(L"Segoe UI", 88.0f * scale,
                Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush titleBrush(Gdiplus::Color(255, 241, 243, 245));
            drawCenteredLine(graphics, text.title, titleFont, format, titleBrush,
                width, height * (487.5f / kReferenceHeight), margin);
            Gdiplus::Font versionFont(L"Consolas", 28.0f * scale,
                Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush versionBrush(Gdiplus::Color(255, 140, 147, 158));
            drawCenteredLine(graphics, versionLabel(text.version), versionFont, format, versionBrush,
                width, height * (578.5f / kReferenceHeight), margin);

            if (text.layout == SplashLayoutStyle::Vertical)
            {
                Gdiplus::Font metadataFont(L"Microsoft YaHei UI", 24.0f * scale,
                    Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush metadata(Gdiplus::Color(255, 140, 147, 158));
                drawCenteredLine(graphics, text.buildTime, metadataFont, format, metadata,
                    width, height * (660.0f / kReferenceHeight), margin);
                drawCenteredLine(graphics, text.authors, metadataFont, format, metadata,
                    width, height * (710.0f / kReferenceHeight), margin);
            }
        }

        // 版权右对齐并与状态共用可见中心线；动态状态区域按其真实宽度避让。
        Gdiplus::Font copyrightFont(L"Microsoft YaHei UI", 24.0f * scale,
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        const std::wstring copyright = ellipsize(graphics, oneLine(text.copyright),
            copyrightFont, format, width * 0.48f);
        const float copyrightWidth = textWidth(graphics, copyright, copyrightFont, format);
        const float copyrightX = width - margin - copyrightWidth;
        Gdiplus::SolidBrush copyrightBrush(Gdiplus::Color(255, 125, 132, 143));
        drawAtVisualCenter(graphics, copyright, copyrightFont, format,
            copyrightBrush, copyrightX, footerCenter);
        const float statusRight = copyright.empty() ? width - margin : copyrightX - 40.0f * scale;
        return Gdiplus::RectF(margin, footerCenter - kStatusBandHeight * scale * 0.5f,
            std::max(0.0f, statusRight - margin), kStatusBandHeight * scale);
    }

    void drawStatus(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds,
        const std::wstring& status)
    {
        if (bounds.Width <= 0.0f || bounds.Height <= 0.0f)
            return;
        const float scale = bounds.Height / kStatusBandHeight; // 与静态版权共用参考稿尺度。
        Gdiplus::Font font(L"Microsoft YaHei UI", 26.0f * scale,
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        configureOneLine(format);
        std::wstring label = oneLine(status);
        if (label.size() >= 3 && label.compare(label.size() - 3, 3, L"...") == 0)
            label.replace(label.size() - 3, 3, L"\u2026");
        label = ellipsize(graphics, label, font, format, bounds.Width);

        // 状态仅左对齐显示操作名称，不额外附加数字百分比或构建/作者信息。
        const Gdiplus::GraphicsState state = graphics.Save();
        graphics.SetClip(bounds, Gdiplus::CombineModeIntersect);
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, 163, 170, 180));
        drawAtVisualCenter(graphics, label, font, format, brush,
            bounds.X, bounds.Y + bounds.Height * 0.5f);
        graphics.Restore(state);
    }
}
