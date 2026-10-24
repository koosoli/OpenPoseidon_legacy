// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/UI/Arma3Menu.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/DisplayUI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Game/Commands/GameStateExtA3Rsc.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Audio/SoundScene.hpp>
#include <Poseidon/World/World.hpp>
#include <Evaluator/express.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>

int Arma2BriefingRevision();                               // GameStateExtA2.cpp
std::vector<std::array<std::string, 3>> Arma2DiaryRecords(); // GameStateExtA2.cpp

namespace Poseidon::Arma3Menu
{
namespace
{
// Arma 3's control types and styles this file draws (a3\ui_f\hpp\defineCommon.inc).
constexpr int kStatic = 0, kButton = 1, kListBox = 5, kHtml = 9, kActiveText = 11, kTree = 12,
              kStructuredText = 13, kControlsGroup = 15, kCheckBox = 77, kListNBox = 102,
              kShortcutButton = 16, kControlsGroupNoScroll = 81, kButtonPicture = 0x30, kKeepAspect = 0x800;

// RscDisplayMain's idcs (a3\ui_f\hpp\defineResinclDesign.inc).
constexpr int kGroupFirst = 1001, kGroupLast = 1005, kTitleFirst = 1011, kTitleLast = 1015, kIconFirst = 1111,
              kIconLast = 1115, kSpotlightFirst = 1020, kSpotlightLast = 1022, kSpotlightHover = 1025,
              kSpotlightButton = 1026, kSpotlightPrev = 1060, kSpotlightNext = 1061;
// RscDisplayInterrupt: Save is CWA's idc, Load ("revert") is not.
constexpr int kInterruptSave = 103, kInterruptLoad = 119;
// The info panels RscDisplayMain.sqf fills from Steam and the network (loaded mods, DLC icons,
// the news feed, the beta-branch ribbon), hidden there when there is nothing to show; here there
// never is. The version panel stays.
constexpr int kInfoPanels[] = {1030, 1031, 10311, 1032, 156};

PackedColor Fade(PackedColor color, float alpha)
{
    int a = toInt(alpha * color.A8());
    saturate(a, 0, 255);
    return PackedColorRGB(color, a);
}

PackedColor Mul(PackedColor a, PackedColor b)
{
    return PackedColor(Color(a.R8() * b.R8() / 65025.0f, a.G8() * b.G8() / 65025.0f, a.B8() * b.B8() / 65025.0f,
                             a.A8() * b.A8() / 65025.0f));
}

// The Arma 3 safe zone a screen is evaluated in (Arma3Rsc::UiSafeZone): Arma 3 lays its screens out
// in it, this engine draws in 0..1 of the screen. Positions and sizes are mapped with these.
Arma3Rsc::SafeZone g_zone{0.0f, 0.0f, 1.0f, 1.0f};
float ToX(float x) { return (x - g_zone.x) / g_zone.w; }
float ToY(float y) { return (y - g_zone.y) / g_zone.h; }
float ToW(float w) { return w / g_zone.w; }
float ToH(float h) { return h / g_zone.h; }

// While alive, scripts answer safeZone* and pixelW/H as Arma 3 would (Arma3Rsc::SetUiCoordinates).
struct InArmaCoordinates
{
    InArmaCoordinates()
    {
        Arma3Rsc::SetUiCoordinates(true);
        g_zone = Arma3Rsc::UiSafeZone();
    }
    ~InArmaCoordinates() { Arma3Rsc::SetUiCoordinates(false); }
};

// A member that is a plain value (a number, or an expression string the config evaluates).
const ParamEntry* Value(const ParamEntry& cls, const char* name)
{
    const ParamEntry* e = cls.FindEntry(name);
    return e && !e->IsClass() && !e->IsArray() ? e : nullptr;
}

float Number(const ParamEntry& cls, const char* name, float fallback)
{
    const ParamEntry* e = Value(cls, name);
    return e ? static_cast<float>(*e) : fallback;
}

int Integer(const ParamEntry& cls, const char* name, int fallback)
{
    const ParamEntry* e = Value(cls, name);
    return e ? toInt(static_cast<float>(*e)) : fallback;
}

RString Text(const ParamEntry& cls, const char* name)
{
    const ParamEntry* e = Value(cls, name);
    return e ? RString(*e) : RString();
}

PackedColor ColorOf(const ParamEntry& cls, const char* name, PackedColor fallback)
{
    const ParamEntry* e = cls.FindEntry(name);
    return e && e->IsArray() && e->GetSize() == 4 ? GetPackedColor(*e) : fallback;
}

// "#E5E5E5" / "#80ffffff" (structured text attributes).
PackedColor HexColor(const char* text, PackedColor fallback)
{
    if (!text || text[0] != '#')
        return fallback;
    unsigned v = 0;
    const size_t n = strlen(text + 1);
    if (sscanf(text + 1, "%x", &v) != 1 || (n != 6 && n != 8))
        return fallback;
    const unsigned a = n == 8 ? v >> 24 : 255;
    return PackedColor(Color(((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f, a / 255.0f));
}

// Structured text as plain text: tags dropped, <br/> a space (a line break with `lineBreaks`),
// entities decoded.
RString PlainText(const RString& text, bool lineBreaks = false)
{
    std::string out;
    for (const char* p = text; *p; p++)
    {
        if (*p == '<')
        {
            const char* end = strchr(p, '>');
            if (!end)
                break;
            if (_strnicmp(p, "<br", 3) == 0)
                out += lineBreaks ? '\n' : ' ';
            p = end;
            continue;
        }
        if (*p == '&')
        {
            static const std::pair<const char*, char> entities[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            bool decoded = false;
            for (const auto& [name, c] : entities)
                if (_strnicmp(p, name, strlen(name)) == 0)
                {
                    out += c;
                    p += strlen(name) - 1;
                    decoded = true;
                    break;
                }
            if (decoded)
                continue;
        }
        out += *p;
    }
    return RString(out.c_str());
}

// UTF-8 as Latin-1 ("\xC2\xA9" -> '\xA9'); a character past U+00FF becomes '?'.
RString Latin1(const RString& utf8)
{
    std::string out;
    const unsigned char* p = reinterpret_cast<const unsigned char*>((const char*)utf8);
    while (*p)
    {
        unsigned code = *p;
        int extra = code >= 0xF0 ? 3 : code >= 0xE0 ? 2 : code >= 0xC0 ? 1 : 0;
        if (extra > 0)
            code &= 0x3Fu >> extra;
        ++p;
        for (; extra > 0 && (*p & 0xC0) == 0x80; --extra, ++p)
            code = (code << 6) | (*p & 0x3Fu);
        out += code <= 0xFF ? static_cast<char>(code) : '?';
    }
    return RString(out.c_str());
}

// A picture of Arma 3's: a file, or a procedural "#(argb,8,8,3)color(r,g,b,a)" drawn as that colour.
struct Picture
{
    Ref<Texture> texture;
    PackedColor tint = PackedWhite;
    bool solid = false; // a colour, no file
    bool any = false;

    void Set(const RString& name)
    {
        texture = nullptr;
        tint = PackedWhite;
        solid = false;
        any = false;
        if (name.GetLength() == 0)
            return;
        const char* text = name;
        if (text[0] == '#')
        {
            float r = 1, g = 1, b = 1, a = 1;
            if (const char* color = strstr(text, "color("))
                if (sscanf(color, "color(%f,%f,%f,%f", &r, &g, &b, &a) >= 3)
                {
                    tint = PackedColor(Color(r, g, b, a));
                    solid = true;
                    any = true;
                }
            return;
        }
        RString file = FindPicture(name);
        file.Lower();
        if (file.GetLength() == 0)
            return;
        texture = GlobLoadTexture(file);
        any = texture != nullptr;
        static std::set<std::string> reported;
        if (!any && reported.insert((const char*)file).second)
            LOG_WARN(UI, "Arma 3 menu: picture {} did not load", (const char*)file);
    }

    // Draws over `rect` (pixels), keeping the picture's proportions when asked.
    void Draw(PackedColor color, Rect2DPixel rect, bool keepAspect, const Rect2DPixel& clip) const
    {
        if (!any)
            return;
        Texture* drawn = solid ? GLOB_SCENE->Preloaded(TextureWhite) : texture.GetRef();
        if (!drawn)
            return;
        if (keepAspect && !solid && drawn->AWidth() > 0 && drawn->AHeight() > 0)
        {
            const float aspect = float(drawn->AWidth()) / float(drawn->AHeight());
            if (rect.w / std::max(rect.h, 1.0f) > aspect)
            {
                const float w = rect.h * aspect;
                rect.x += (rect.w - w) * 0.5f;
                rect.w = w;
            }
            else
            {
                const float h = rect.w / aspect;
                rect.y += (rect.h - h) * 0.5f;
                rect.h = h;
            }
        }
        MipInfo mip = GLOB_ENGINE->TextBank()->UseMipmap(drawn, 0, 0);
        GLOB_ENGINE->Draw2D(mip, solid ? Mul(color, tint) : color, rect, clip);
    }
};

// What every Arma 3 control here shares: its rectangle, the group it is clipped to, its font.
class A3Control : public Control
{
  protected:
    Rect2DFloat _clip;
    Ref<Font> _font;
    float _size = 0.03f;
    RString _text;

  public:
    A3Control(ControlsContainer* parent, int type, int idc, const ParamEntry& cls, float x, float y, float w,
              float h, const Rect2DFloat& clip)
        : Control(parent, type, idc, Integer(cls, "style", 0), x, y, w, h), _clip(clip)
    {
        RString tooltip = Text(cls, "tooltip");
        if (tooltip.GetLength() > 0)
            SetTooltip(tooltip);
        _text = Text(cls, "text");
    }

    RString GetText() const { return _text; }
    void SetText(const RString& text) { _text = text; }

  protected:
    void LoadFont(const RString& family, float size)
    {
        _size = size > 0 ? size : 0.03f;
        const RString file = FontFile(family, _size);
        _font = GLOB_ENGINE->LoadFont(
            GetFontID(file.GetLength() > 0 ? file : family.GetLength() > 0 ? family : RString("tahomaB24")));
    }

    // Arma 3's CfgFontFamilies lists each family once per size ("...\Purista-Light\Purista-Light9" ..
    // "...46", each with fallbacks for other scripts); the size whose line comes closest to the text's
    // height on this screen is the one drawn. Its lines are ~1.7x the number in its name
    // (Purista-Light24: 41 px). "" when Arma 3 has no such family.
    static RString FontFile(const RString& family, float size)
    {
        const ParamEntry* root = Arma1Compat::ConfigRoot();
        const ParamEntry* families = root ? root->FindEntry("CfgFontFamilies") : nullptr;
        const ParamEntry* cls = families && family.GetLength() > 0 ? families->FindEntry(family) : nullptr;
        const ParamEntry* fonts = cls && cls->IsClass() ? cls->FindEntry("fonts") : nullptr;
        if (!fonts || !fonts->IsArray())
            return RString();
        const float wantPx = size * float(GLOB_ENGINE->Height2D()) / 1.7f;
        RString best;
        float bestError = 1e9f;
        for (int i = 0; i < fonts->GetSize(); i++)
        {
            const IParamArrayValue& item = (*fonts)[i];
            const RString path = item.IsArrayValue() && item.GetItemCount() > 0 ? RString(item[0].GetValue())
                                                                               : RString(item.GetValue());
            const char* digits = path;
            const char* end = digits + strlen(digits);
            while (end > digits && isdigit(static_cast<unsigned char>(end[-1])))
                --end;
            const float px = static_cast<float>(atoi(end));
            if (px <= 0)
                continue;
            // a size a little above what is wanted reads better scaled down than one below scaled up
            const float error = px >= wantPx ? px - wantPx : (wantPx - px) * 1.5f;
            if (error < bestError)
            {
                bestError = error;
                best = path;
            }
        }
        return best;
    }

    Rect2DPixel Pixels(float x, float y, float w, float h) const
    {
        const float sw = float(GLOB_ENGINE->Width2D()), sh = float(GLOB_ENGINE->Height2D());
        return Rect2DPixel(x * sw, y * sh, w * sw, h * sh);
    }
    Rect2DPixel ClipPixels() const { return Pixels(_clip.x, _clip.y, _clip.w, _clip.h); }

    // Word-wrapped lines of `text` in `font` no wider than `width`; a '\n' ends a line.
    static std::vector<std::string> WrapLines(Font* font, float size, float width, const std::string& text)
    {
        std::vector<std::string> lines;
        if (!font)
            return lines;
        std::string line, word;
        for (size_t i = 0; i <= text.size(); i++)
        {
            const char c = i < text.size() ? text[i] : ' ';
            if (c != ' ' && c != '\n')
            {
                word += c;
                continue;
            }
            const std::string candidate = line.empty() ? word : line + " " + word;
            const RString measured = font->IsFreeType() ? RString(candidate.c_str()) : Latin1(candidate.c_str());
            if (!line.empty() && GLOB_ENGINE->GetTextWidth(size, font, measured) > width)
            {
                lines.push_back(line);
                line = word;
            }
            else
                line = candidate;
            word.clear();
            if (c == '\n')
            {
                lines.push_back(line);
                line.clear();
            }
        }
        if (!line.empty())
            lines.push_back(line);
        return lines;
    }

    // Text in [x, x + w] (alignment from `align`: 0 left, 1 right, 2 centre), vertically centred; in
    // `font` when given, else the control's.
    void DrawLabel(const RString& utf8, float x, float y, float w, float h, int align, PackedColor color,
                   float size, Font* font = nullptr) const
    {
        if (!font)
            font = _font;
        if (utf8.GetLength() == 0 || !font)
            return;
        // Arma 3's strings are UTF-8; its bitmap fonts are drawn a byte per Latin-1 character
        const RString text = font->IsFreeType() ? utf8 : Latin1(utf8);
        const float width = GLOB_ENGINE->GetTextWidth(size, font, text);
        float left = x;
        if (align == ST_RIGHT)
            left = x + w - width;
        else if (align == ST_CENTER)
            left = x + 0.5f * (w - width);
        const float top = y + 0.5f * (h - size);
        // clipped to the control and to its group, as Arma 3 clips a group's controls
        const float cx = std::max(_x, _clip.x), cy = std::max(_y, _clip.y);
        const float cw = std::min(_x + _w, _clip.x + _clip.w) - cx, ch = std::min(_y + _h, _clip.y + _clip.h) - cy;
        if (cw <= 0 || ch <= 0)
            return;
        GLOB_ENGINE->DrawText(Point2DFloat(left, top), size, Rect2DFloat(cx, cy, cw, ch), font, color, text);
    }
};

// RscText / RscPicture / RscStructuredText: a background, then a picture or a line of text.
class A3Static : public A3Control
{
    PackedColor _background, _color;
    Picture _picture;
    int _align = ST_LEFT;
    bool _wrap = false, _frame = false;

  public:
    A3Static(ControlsContainer* parent, int type, int idc, const ParamEntry& cls, float x, float y, float w,
             float h, const Rect2DFloat& clip)
        : A3Control(parent, type, idc, cls, x, y, w, h, clip)
    {
        _enabled = false;
        _background = ColorOf(cls, "colorBackground", PackedColor(Color(0, 0, 0, 0)));
        _color = ColorOf(cls, "colorText", PackedWhite);
        _align = _style & ST_HPOS;
        if (type == kStructuredText)
        {
            // the text's own attributes: font, colour, alignment
            RString family = "tahomaB24";
            if (const ParamEntry* attributes = cls.FindEntry("Attributes"); attributes && attributes->IsClass())
            {
                family = Text(*attributes, "font");
                _color = Mul(_color, HexColor(Text(*attributes, "color"), PackedWhite));
                const RString align = Text(*attributes, "align");
                _align = _stricmp(align, "center") == 0 ? ST_CENTER : _stricmp(align, "right") == 0 ? ST_RIGHT : ST_LEFT;
            }
            LoadFont(family, ToH(Number(cls, "size", 0.04f)));
            _text = PlainText(_text);
            _wrap = true;
        }
        else
        {
            LoadFont(Text(cls, "font"), ToH(Number(cls, "sizeEx", Number(cls, "size", 0.04f))));
            _wrap = (_style & ST_TYPE) == ST_MULTI;
        }
        if ((_style & ST_TYPE) == kButtonPicture && type != kStructuredText)
            _picture.Set(_text);
        // a frame (RscFrame) draws its outline in Arma 3; here it is left out rather than drawn as text
        _frame = (_style & ST_TYPE) == ST_FRAME;
    }

    // Structured text is shown as plain text; a multi-line or structured control wraps it.
    void SetLabel(const RString& text) { _text = GetType() == kStructuredText ? PlainText(text) : text; }

    bool IsPicture() const { return (_style & ST_TYPE) == kButtonPicture; }
    void SetPicture(const RString& name)
    {
        _text = name;
        _picture.Set(name);
    }

    void OnDraw(float alpha) override
    {
        const Rect2DPixel rect = Pixels(_x, _y, _w, _h);
        if (_background.A8() > 0)
            Picture{nullptr, PackedWhite, true, true}.Draw(Fade(_background, alpha), rect, false, ClipPixels());
        if (_frame)
            return;
        if (IsPicture())
            _picture.Draw(Fade(_color, alpha), rect, (_style & kKeepAspect) != 0, ClipPixels());
        else if (_wrap)
            DrawWrapped(Fade(_color, alpha));
        else
            DrawLabel(_text, _x + 0.005f, _y, _w - 0.01f, _h, _align, Fade(_color, alpha), _size);
    }

    // RscDisplayMain.sqf centres a spotlight's text in its tile (ctrlTextHeight); others start at the top.
    void CentreVertically() { _centred = true; }

  private:
    bool _centred = false;

    // Word-wrapped lines, as many as fit, from the top (or centred as a block).
    void DrawWrapped(PackedColor color) const
    {
        if (!_font || _text.GetLength() == 0)
            return;
        const float width = _w - 0.01f;
        const std::vector<std::string> lines = WrapLines(_font, _size, width, (const char*)_text);
        const float pitch = _size * 1.05f;
        float top = _centred ? _y + std::max(0.0f, 0.5f * (_h - pitch * lines.size())) : _y;
        for (const std::string& text : lines)
        {
            if (top + _size <= _y + _h + 0.001f)
                DrawLabel(RString(text.c_str()), _x + 0.005f, top, width, _size, _align, color, _size);
            top += pitch;
        }
    }
};

// RscHTML (9), as the diary's page: a heading in its H1 font over word-wrapped paragraphs in its P
// font, scrolled by the mouse wheel. Arma 3's links, images and colours are drawn as their text.
class A3Html : public A3Control
{
    Ref<Font> _headingFont;
    float _headingSize = 0.04f;
    PackedColor _color;
    RString _heading;
    float _scroll = 0; // lines from the top

  public:
    A3Html(ControlsContainer* parent, int type, int idc, const ParamEntry& cls, float x, float y, float w, float h,
           const Rect2DFloat& clip)
        : A3Control(parent, type, idc, cls, x, y, w, h, clip)
    {
        // Arma 3 sizes the page taller than its group and scrolls the group; here the page is the
        // group's height and scrolls itself, and does not reach over what lies below the group
        _h = std::max(0.0f, std::min(_y + _h, clip.y + clip.h) - _y);
        _color = ColorOf(cls, "colorText", PackedWhite);
        const ParamEntry* p = cls.FindEntry("P");
        const ParamEntry* h1 = cls.FindEntry("H1");
        LoadFont(p && p->IsClass() ? Text(*p, "font") : RString(), ToH(p && p->IsClass() ? Number(*p, "sizeEx", 0.03f) : 0.03f));
        _headingSize = ToH(h1 && h1->IsClass() ? Number(*h1, "sizeEx", 0.045f) : 0.045f);
        const RString headingFont = FontFile(h1 && h1->IsClass() ? Text(*h1, "font") : RString(), _headingSize);
        _headingFont = _font;
        if (headingFont.GetLength() > 0)
            _headingFont = GLOB_ENGINE->LoadFont(GetFontID(headingFont));
        _text = RString();
    }

    // The page: `structured` is Arma 3's structured text (<br/> a line break, other tags dropped).
    void SetPage(const RString& heading, const RString& structured)
    {
        _heading = heading;
        _text = PlainText(structured, true);
        _scroll = 0;
    }

    void OnMouseZChanged(float dz) override { _scroll = std::max(0.0f, _scroll - dz * 3.0f); }

    void OnDraw(float alpha) override
    {
        if (!_font)
            return;
        const PackedColor color = Fade(_color, alpha);
        const float width = _w - 0.01f, pitch = _size * 1.15f;
        float top = _y + 0.005f - _scroll * pitch;
        const auto line = [&](const std::string& text, const Ref<Font>& font, float size, float step)
        {
            if (top >= _y - 0.001f && top + size <= _y + _h + 0.001f)
                DrawLabel(RString(text.c_str()), _x + 0.005f, top, width, size, ST_LEFT, color, size, font);
            top += step;
        };
        if (_heading.GetLength() > 0)
        {
            for (const std::string& text : WrapLines(_headingFont, _headingSize, width, (const char*)_heading))
                line(text, _headingFont, _headingSize, _headingSize * 1.15f);
            top += pitch * 0.5f;
        }
        for (const std::string& text : WrapLines(_font, _size, width, (const char*)_text))
            line(text, _font, _size, pitch);
        // the wheel stops at the last line
        const float rows = (top + _scroll * pitch - _y) / pitch;
        _scroll = std::min(_scroll, std::max(0.0f, rows - _h / pitch));
    }
};

// RscButton (1), RscActiveText (11) and RscShortcutButton / RscButtonMenu (16). A button is
// pressed on release over it and reports `clickIdc` to the display.
class A3Button : public A3Control
{
    // background: plain colours (1), or the animTexture* pictures tinted (16)
    PackedColor _background, _backgroundActive, _backgroundDisabled;
    Picture _textureNormal, _textureOver, _texturePressed;
    // text / picture
    PackedColor _color, _colorActive, _colorDisabled;
    Picture _picture;
    float _textLeft = 0.005f, _textTop = 0;
    int _align = ST_LEFT;
    int _clickIdc;
    bool _over = false, _pressed = false;

  public:
    A3Button(ControlsContainer* parent, int type, int idc, const ParamEntry& cls, float x, float y, float w,
             float h, const Rect2DFloat& clip, int clickIdc)
        : A3Control(parent, type, idc, cls, x, y, w, h, clip), _clickIdc(clickIdc)
    {
        LoadSounds(cls);
        _align = _style & ST_HPOS;
        if (type == kShortcutButton)
        {
            _background = ColorOf(cls, "colorBackground", PackedColor(Color(0, 0, 0, 0.8f)));
            _backgroundActive = ColorOf(cls, "colorBackgroundFocused", _background);
            _backgroundDisabled = _background;
            _textureNormal.Set(Text(cls, "animTextureNormal"));
            _textureOver.Set(Text(cls, "animTextureOver"));
            _texturePressed.Set(Text(cls, "animTexturePressed"));
            _color = ColorOf(cls, "color", PackedWhite);
            _colorActive = ColorOf(cls, "colorFocused", _color);
            _colorDisabled = ColorOf(cls, "colorDisabled", Fade(_color, 0.25f));
            RString family = Text(cls, "font");
            if (const ParamEntry* attributes = cls.FindEntry("Attributes"); attributes && attributes->IsClass())
            {
                if (const RString font = Text(*attributes, "font"); font.GetLength() > 0)
                    family = font;
                const RString align = Text(*attributes, "align");
                _align = _stricmp(align, "center") == 0 ? ST_CENTER : _stricmp(align, "right") == 0 ? ST_RIGHT : ST_LEFT;
            }
            LoadFont(family, ToH(Number(cls, "size", Number(cls, "sizeEx", 0.04f))));
            if (const ParamEntry* pos = cls.FindEntry("TextPos"); pos && pos->IsClass())
            {
                _textLeft = ToW(Number(*pos, "left", 0.0f));
                _textTop = ToH(Number(*pos, "top", 0.0f));
            }
            _text = PlainText(_text);
        }
        else
        {
            _background = ColorOf(cls, "colorBackground", PackedColor(Color(0, 0, 0, 0)));
            _backgroundActive = ColorOf(cls, "colorBackgroundActive", _background);
            _backgroundDisabled = ColorOf(cls, "colorBackgroundDisabled", _background);
            _color = ColorOf(cls, type == kActiveText ? "color" : "colorText", PackedWhite);
            _colorActive = ColorOf(cls, type == kActiveText ? "colorActive" : "colorFocused", _color);
            // Arma 3's picture buttons turn their icon black on the white hover background
            // (onMouseEnter: ctrlSetTextColor [0,0,0,1]); colorFocused says the same where set
            if ((_style & ST_TYPE) == kButtonPicture && type == kButton)
                _colorActive = ColorOf(cls, "colorFocused", _color);
            _colorDisabled = ColorOf(cls, "colorDisabled", Fade(_color, 0.25f));
            LoadFont(Text(cls, "font"), ToH(Number(cls, "sizeEx", Number(cls, "size", 0.04f))));
            if ((_style & ST_TYPE) == kButtonPicture)
                _picture.Set(_text);
        }
    }

    bool CanBeDefault() const override { return true; }

    void OnMouseMove(float x, float y, bool active) override { Hover(active && IsInside(x, y)); }
    void OnMouseHold(float x, float y, bool active) override { Hover(active && IsInside(x, y)); }
    void OnLButtonDown(float x, float y) override
    {
        _pressed = true;
        PlaySound(_pushSound);
    }
    void OnLButtonUp(float x, float y) override
    {
        const bool click = _pressed && IsInside(x, y);
        _pressed = false;
        if (!click)
        {
            PlaySound(_escapeSound);
            return;
        }
        PlaySound(_clickSound);
        _parent->OnButtonClicked(_clickIdc);
    }
    bool OnKeyUp(unsigned nChar, unsigned nRepCnt, unsigned nFlags) override
    {
        if (!IsUiConfirmKey(nChar))
            return false;
        PlaySound(_clickSound);
        _parent->OnButtonClicked(_clickIdc);
        return true;
    }

    // Arma 3's own button sounds (soundEnter / soundPush / soundClick / soundEscape)
    void LoadSounds(const ParamEntry& cls)
    {
        const auto load = [&](SoundPars& pars, const char* name)
        {
            if (const ParamEntry* e = cls.FindEntry(name); e && e->IsArray())
                GetValue(pars, *e);
            // Arma 3 names them without an extension ("\A3\ui_f\data\sound\RscButton\soundEnter")
            if (pars.name.GetLength() > 0)
                if (const RString file = Arma1Compat::FindArmaSoundFile(pars.name); file.GetLength() > 0)
                    pars.name = file;
        };
        load(_enterSound, "soundEnter");
        load(_pushSound, "soundPush");
        load(_clickSound, "soundClick");
        load(_escapeSound, "soundEscape");
    }

  private:
    SoundPars _enterSound, _pushSound, _clickSound, _escapeSound;
    void Hover(bool over)
    {
        if (over && !_over)
            PlaySound(_enterSound);
        _over = over;
    }

  public:

    void OnDraw(float alpha) override
    {
        const Rect2DPixel rect = Pixels(_x, _y, _w, _h);
        const bool active = IsEnabled() && (_over || IsFocused());
        const PackedColor background =
            !IsEnabled() ? _backgroundDisabled : active ? _backgroundActive : _background;
        if (GetType() == kShortcutButton)
        {
            const Picture& texture = _pressed && _texturePressed.any ? _texturePressed
                                     : active && _textureOver.any  ? _textureOver
                                                                   : _textureNormal;
            texture.Draw(Fade(background, alpha), rect, false, ClipPixels());
        }
        else if (background.A8() > 0)
            Picture{nullptr, PackedWhite, true, true}.Draw(Fade(background, alpha), rect, false, ClipPixels());

        const PackedColor color = Fade(!IsEnabled() ? _colorDisabled : active ? _colorActive : _color, alpha);
        if ((_style & ST_TYPE) == kButtonPicture)
            _picture.Draw(color, rect, (_style & kKeepAspect) != 0, ClipPixels());
        else if (GetType() == kShortcutButton)
            DrawLabel(_text, _x + _textLeft, _y + _textTop, _w - 2 * _textLeft, _h - 2 * _textTop, _align, color,
                      _size);
        else
            DrawLabel(_text, _x + 0.005f, _y, _w - 0.01f, _h, _align, color, _size);
    }
};

// RscListBox (5) and RscTree (12): one column of rows, a selection, a mouse wheel. Arma 3's trees
// nest, its scripts fill them (tvAdd); the screens of this file fill them flat (SetRows).
class A3List : public A3Control
{
    std::vector<RString> _rows;
    int _selected = -1;
    float _first = 0; // the top row
    float _rowHeight;
    PackedColor _background, _color, _selectColor, _selectBackground;

  public:
    A3List(ControlsContainer* parent, int type, int idc, const ParamEntry& cls, float x, float y, float w, float h,
           const Rect2DFloat& clip)
        : A3Control(parent, type, idc, cls, x, y, w, h, clip)
    {
        _background = ColorOf(cls, "colorBackground", PackedColor(Color(0, 0, 0, 0)));
        _color = ColorOf(cls, "colorText", PackedWhite);
        _selectColor = ColorOf(cls, "colorSelectText", ColorOf(cls, "colorSelect", PackedColor(Color(0, 0, 0, 1))));
        _selectBackground = ColorOf(cls, "colorSelectBackground", PackedColor(Color(1, 1, 1, 0.7f)));
        // Arma 3 paints a tree's selected row in colorSelect over the background
        if (type == kTree)
            _selectBackground = ColorOf(cls, "colorSelect", _selectBackground);
        LoadFont(Text(cls, "font"), ToH(Number(cls, "sizeEx", 0.04f)));
        _rowHeight = std::max(_size * 1.15f, ToH(Number(cls, "rowHeight", 0.0f)));
    }

    void SetRows(const std::vector<RString>& rows, int selected)
    {
        _rows = rows;
        _first = 0;
        Select(selected, false);
    }
    int Selected() const { return _selected; }

    bool OnKeyDown(unsigned nChar, unsigned nRepCnt, unsigned nFlags) override
    {
        if (nChar == SDLK_UP || nChar == SDLK_DOWN)
        {
            Select(std::clamp(_selected + (nChar == SDLK_UP ? -1 : 1), 0, static_cast<int>(_rows.size()) - 1), true);
            return true;
        }
        if (IsUiConfirmKey(nChar) && _selected >= 0)
        {
            _parent->OnLBDblClick(IDC(), _selected);
            return true;
        }
        return false;
    }
    void OnLButtonDown(float x, float y) override
    {
        const int row = static_cast<int>(_first + (y - _y) / _rowHeight);
        if (row >= 0 && row < static_cast<int>(_rows.size()))
            Select(row, true);
    }
    void OnLButtonDblClick(float x, float y) override
    {
        if (_selected >= 0)
            _parent->OnLBDblClick(IDC(), _selected);
    }
    void OnMouseZChanged(float dz) override
    {
        _first = std::clamp(_first - dz * 3.0f, 0.0f, std::max(0.0f, float(_rows.size()) - Visible()));
    }

    void OnDraw(float alpha) override
    {
        const Rect2DPixel clip = ClipPixels();
        if (_background.A8() > 0)
            Picture{nullptr, PackedWhite, true, true}.Draw(Fade(_background, alpha), Pixels(_x, _y, _w, _h), false, clip);
        const int first = static_cast<int>(_first);
        for (int i = first; i < static_cast<int>(_rows.size()) && i < first + static_cast<int>(Visible()); i++)
        {
            const float top = _y + (i - first) * _rowHeight;
            const bool selected = i == _selected;
            if (selected)
                Picture{nullptr, PackedWhite, true, true}.Draw(Fade(_selectBackground, alpha),
                                                              Pixels(_x, top, _w, _rowHeight), false, clip);
            DrawLabel(_rows[i], _x + 0.006f, top, _w - 0.012f, _rowHeight, ST_LEFT,
                      Fade(selected ? _selectColor : _color, alpha), _size);
        }
    }

  private:
    float Visible() const { return std::floor(_h / _rowHeight); }
    void Select(int row, bool notify)
    {
        _selected = _rows.empty() ? -1 : std::clamp(row, 0, static_cast<int>(_rows.size()) - 1);
        if (_selected >= 0)
        {
            if (_selected < _first)
                _first = float(_selected);
            else if (_selected >= _first + Visible())
                _first = float(_selected) - Visible() + 1;
        }
        if (notify)
            _parent->OnLBSelChanged(IDC(), _selected);
    }
};

// RscCheckBox (77): its checked or unchecked picture; a click toggles it and reports its idc to the
// display (OnButtonClicked), where the screen's own rules apply (Old Man's exclusive pair).
class A3CheckBox : public A3Control
{
    Picture _checkedPicture, _uncheckedPicture;
    PackedColor _color, _colorHover;
    bool _checked = false, _hover = false;

  public:
    A3CheckBox(ControlsContainer* parent, int type, int idc, const ParamEntry& cls, float x, float y, float w,
               float h, const Rect2DFloat& clip)
        : A3Control(parent, type, idc, cls, x, y, w, h, clip)
    {
        _checkedPicture.Set(Text(cls, "textureChecked"));
        _uncheckedPicture.Set(Text(cls, "textureUnchecked"));
        _color = ColorOf(cls, "color", PackedWhite);
        _colorHover = ColorOf(cls, "colorHover", _color);
        _checked = Integer(cls, "checked", 0) != 0;
    }

    bool Checked() const { return _checked; }
    void SetChecked(bool checked) { _checked = checked; }

    void OnLButtonUp(float x, float y) override
    {
        if (x < _x || x > _x + _w || y < _y || y > _y + _h)
            return;
        _checked = !_checked;
        _parent->OnButtonClicked(IDC());
    }
    bool OnKeyDown(unsigned nChar, unsigned nRepCnt, unsigned nFlags) override
    {
        if (nChar != SDLK_SPACE)
            return false;
        _checked = !_checked;
        _parent->OnButtonClicked(IDC());
        return true;
    }
    void OnMouseMove(float x, float y, bool active) override
    {
        _hover = x >= _x && x <= _x + _w && y >= _y && y <= _y + _h;
    }

    void OnDraw(float alpha) override
    {
        (_checked ? _checkedPicture : _uncheckedPicture)
            .Draw(Fade(_hover ? _colorHover : _color, alpha), Pixels(_x, _y, _w, _h), true, ClipPixels());
    }
};

// A group of the screen: its rectangle (where its rows unfold) and what it holds.
struct Group
{
    int idc = -1;
    int parent = -1; // index in the groups, -1 at the top
    bool shown = true;
    float x = 0, y = 0, w = 0, h = 0;
};

struct Member
{
    Control* control;
    int group;   // index in the groups, -1 at the top
    bool shown;  // its own show / fade
    bool background; // one of the screen's ControlsBackground (its backdrop)
};

// The Arma 3 main menu on screen, for the screens opened over it (DrawMenuBackdrop).
Screen* g_mainMenu = nullptr;
} // namespace

struct Screen::Impl
{
    ControlsContainer* display = nullptr;
    std::vector<Group> groups;
    std::vector<Member> members;
    std::map<std::string, int> skipped; // why a control was left out -> how often
    int open = -1;                      // the open row (group index), -1 none
    bool pinned = false;                // opened by a click: stays until another title is entered
    bool loadingBackground = false;     // LoadList is reading ControlsBackground
    RefArray<Control>* foreground = nullptr;
    Action centreAction = Action::Unavailable; // what the middle spotlight (CfgMainMenuSpotlight) opens
    // the diary (InitDiary): {subject, title, text} per record, the subjects listed, what is chosen
    std::vector<std::array<std::string, 3>> records;
    std::vector<std::string> subjects;
    int subject = 0, record = 0, revision = -1;
    std::vector<int> shownRecords; // indices in `records` of the rows of the record list

    // Shown when it and every group around it are.
    void Refresh()
    {
        for (const Member& m : members)
        {
            bool shown = m.shown;
            for (int g = m.group; shown && g >= 0; g = groups[g].parent)
                shown = groups[g].shown;
            m.control->ShowCtrl(shown);
        }
    }

    int FindGroup(int idc) const
    {
        for (size_t i = 0; i < groups.size(); i++)
            if (groups[i].idc == idc)
                return static_cast<int>(i);
        return -1;
    }

    Control* Find(int idc) const
    {
        for (const Member& m : members)
            if (m.control->IDC() == idc)
                return m.control;
        return nullptr;
    }

    bool Inside(const Group& g, float x, float y) const { return x >= g.x && x <= g.x + g.w && y >= g.y && y <= g.y + g.h; }

    void LoadList(const ParamEntry& list, float ox, float oy, const Rect2DFloat& clip, int group,
                  RefArray<Control>& out, int topGroupIdc)
    {
        for (int i = 0; i < list.GetEntryCount(); i++)
        {
            const ParamEntry& cls = list.GetEntry(i);
            if (cls.IsClass())
                LoadControl(cls, ox, oy, clip, group, out, topGroupIdc);
        }
    }

    void LoadControl(const ParamEntry& cls, float ox, float oy, const Rect2DFloat& clip, int group,
                     RefArray<Control>& out, int topGroupIdc)
    {
        const int type = Integer(cls, "type", kStatic);
        const int idc = Integer(cls, "idc", -1);
        // Arma 3 coordinates (ox, oy, ax, ay); x, y, w, h are the screen's
        const float ax = ox + Number(cls, "x", 0), ay = oy + Number(cls, "y", 0);
        const float x = ToX(ax), y = ToY(ay), w = ToW(Number(cls, "w", 0)), h = ToH(Number(cls, "h", 0));
        const bool shown = Integer(cls, "show", 1) != 0 && Number(cls, "fade", 0) < 1;
        if (type == kControlsGroup || type == kControlsGroupNoScroll)
        {
            Group g;
            g.idc = idc;
            g.parent = group;
            g.shown = shown;
            g.x = x, g.y = y, g.w = w, g.h = h;
            groups.push_back(g);
            const int index = static_cast<int>(groups.size()) - 1;
            // its controls are clipped to it, and to the groups around it
            const float cx = std::max(clip.x, x), cy = std::max(clip.y, y);
            const Rect2DFloat inner(cx, cy, std::max(0.0f, std::min(clip.x + clip.w, x + w) - cx),
                                    std::max(0.0f, std::min(clip.y + clip.h, y + h) - cy));
            if (const ParamEntry* controls = cls.FindEntry("Controls"); controls && controls->IsClass())
                LoadList(*controls, ax, ay, inner, index, out, topGroupIdc >= 0 ? topGroupIdc : idc);
            return;
        }
        Control* control = nullptr;
        const RString text = Text(cls, "text");
        if (type == kStatic || type == kStructuredText)
        {
            // Arma 3 plays .ogv videos in a picture control (the spotlights); this engine has no
            // video player, the still picture under it stands in
            if (Lower(text).find(".ogv") != std::string::npos)
            {
                skipped["video"]++;
                return;
            }
            control = new A3Static(display, type, idc, cls, x, y, w, h, clip);
        }
        else if (type == kListBox || type == kTree || type == kListNBox)
            control = new A3List(display, type, idc, cls, x, y, w, h, clip);
        else if (type == kHtml)
            control = new A3Html(display, type, idc, cls, x, y, w, h, clip);
        else if (type == kCheckBox)
            control = new A3CheckBox(display, type, idc, cls, x, y, w, h, clip);
        else if (type == kButton || type == kActiveText || type == kShortcutButton)
        {
            // a spotlight's button carries the same idc in every spotlight: it reports its spotlight
            const int clickIdc = idc == kSpotlightButton && topGroupIdc >= 0 ? topGroupIdc : idc;
            control = new A3Button(display, type, idc, cls, x, y, w, h, clip, clickIdc);
        }
        else
        {
            // edit boxes, combos, check boxes, sliders, HTML: Arma 3's scripts drive them (search,
            // sort, the debug console); they are left out
            skipped["control type " + std::to_string(type)]++;
            return;
        }
        out.Add(control);
        members.push_back({control, group, shown, loadingBackground});
    }

    static std::string Lower(const RString& text)
    {
        std::string s = (const char*)text;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }
};

Screen::Screen() : _impl(std::make_unique<Impl>()) {}
Screen::~Screen()
{
    if (g_mainMenu == this)
        g_mainMenu = nullptr;
}

void Screen::DrawBackdrop(float alpha)
{
    for (const Member& m : _impl->members)
        if (m.background && m.control->IsVisible())
            m.control->OnDraw(alpha);
}

void Screen::Draw(float alpha)
{
    for (const Member& m : _impl->members)
        if (m.control->IsVisible())
            m.control->OnDraw(alpha);
}

void DrawMenuBackdrop(float alpha)
{
    if (g_mainMenu)
        g_mainMenu->DrawBackdrop(alpha);
}

bool Active()
{
    static const bool active = []
    {
        if (!Foundation::AppConfig::Instance().GetArmaOriginalUi() || !Arma1Compat::Active() ||
            Arma1Compat::Generation() != 3)
            return false;
        Arma1Compat::LoadConfig();
        const ParamEntry* root = Arma1Compat::ConfigRoot();
        const ParamEntry* menu = root ? root->FindEntry("RscDisplayMain") : nullptr;
        if (!menu || !menu->IsClass())
        {
            LOG_WARN(UI, "Arma 3 menu: RscDisplayMain is not in Arma 3's config; keeping CWA's menu");
            return false;
        }
        LOG_INFO(UI, "Arma 3 menu: the main menu is drawn from Arma 3's RscDisplayMain");
        return true;
    }();
    return active;
}

bool Screen::Load(ControlsContainer& display, const char* name, RefArray<Control>& background,
                  RefArray<Control>& foreground, bool backdrop)
{
    // "Outer/Inner": a class inside another (RscInGameUI's)
    const ParamEntry* screen = Arma1Compat::ConfigRoot();
    std::string path = name;
    for (size_t start = 0; screen && start <= path.size();)
    {
        const size_t end = std::min(path.find('/', start), path.size());
        screen = screen->IsClass() ? screen->FindEntry(path.substr(start, end - start).c_str()) : nullptr;
        start = end + 1;
    }
    if (!screen || !screen->IsClass())
        return false;
    Impl& s = *_impl;
    s.display = &display;
    s.foreground = &foreground;
    const InArmaCoordinates coordinates;
    const Rect2DFloat everything(-1, -1, 3, 3);
    const int before = static_cast<int>(s.members.size());
    if (const ParamEntry* list = screen->FindEntry("ControlsBackground"); backdrop && list && list->IsClass())
    {
        s.loadingBackground = true;
        s.LoadList(*list, 0, 0, everything, -1, background, -1);
        s.loadingBackground = false;
    }
    if (const ParamEntry* list = screen->FindEntry("Controls"); list && list->IsClass())
        s.LoadList(*list, 0, 0, everything, -1, foreground, -1);
    else if (list && list->IsArray())
    {
        // controls[] = {"Name", ...}: the classes of those names, members of the screen (RscInGameUI's)
        for (int i = 0; i < list->GetSize(); i++)
        {
            const ParamEntry* cls = screen->FindEntry(RString((*list)[i].GetValue()));
            if (cls && cls->IsClass())
                s.LoadControl(*cls, 0, 0, everything, -1, foreground, -1);
        }
    }
    std::string skipped;
    for (const auto& [why, count] : s.skipped)
        skipped += (skipped.empty() ? "" : ", ") + std::to_string(count) + " " + why;
    LOG_INFO(UI, "Arma 3 menu: {} loaded: {} controls in {} groups{}{}", name,
             static_cast<int>(s.members.size()) - before, s.groups.size(), skipped.empty() ? "" : "; skipped ",
             skipped);
    s.skipped.clear();
    s.Refresh();
    return true;
}

void Screen::InitMainMenu()
{
    Impl& s = *_impl;
    // The rows under the titles start folded (RscDisplayMain.sqf: height 0 until a title is entered).
    for (Group& g : s.groups)
        if (g.idc >= kGroupFirst && g.idc <= kGroupLast)
            g.shown = false;
    // Each spotlight's hover frame (fade = 1) shows only under the cursor; the middle spotlight
    // (1020) is the slot the script cycles the CfgMainMenuSpotlight entries through, hidden by config.
    // The previous / next arrows cycle those entries: there is nothing to cycle here.
    for (const int idc : {kSpotlightPrev, kSpotlightNext})
        if (Control* arrow = s.Find(idc))
            for (Member& m : s.members)
                if (m.control == arrow)
                    m.shown = false;
    for (const int idc : kInfoPanels)
        if (const int g = s.FindGroup(idc); g >= 0)
            s.groups[g].shown = false;
    g_mainMenu = this;
    AddCentreSpotlight();
    // spotlightInit: QUICK PLAY / EDITOR upper-cased, every spotlight's text centred in its tile
    for (Member& m : s.members)
        if (m.control->IDC() == 1028)
            if (auto* text = dynamic_cast<A3Static*>(m.control))
            {
                std::string upper = (const char*)text->GetText();
                std::transform(upper.begin(), upper.end(), upper.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                text->SetLabel(RString(upper.c_str()));
                text->CentreVertically();
            }
    // The script fades the spotlight tiles in (their class says fade = 1), the middle one included.
    for (int idc = kSpotlightFirst; idc <= kSpotlightLast; idc++)
        if (const int g = s.FindGroup(idc); g >= 0)
            s.groups[g].shown = true;
    s.Refresh();
}

// RscDisplayMain.sqf's spotlightInit: the middle slot (Spotlight2, idc 1020) shows an entry of
// CfgMainMenuSpotlight -- an RscMainMenuSpotlight filled with its picture, quoted name and action
// text. Arma 3 picks by Steam statistics (Bootcamp until it is played, then the East Wind campaign)
// and cycles them; here the campaign when its missions are installed, else Bootcamp.
void Screen::AddCentreSpotlight()
{
    Impl& s = *_impl;
    const ParamEntry* root = Arma1Compat::ConfigRoot();
    const ParamEntry* list = root ? root->FindEntry("CfgMainMenuSpotlight") : nullptr;
    const ParamEntry* tile = root ? root->FindEntry("RscMainMenuSpotlight") : nullptr;
    Control* slot = s.Find(kSpotlightFirst);
    if (!list || !list->IsClass() || !tile || !tile->IsClass() || !slot || !s.foreground)
        return;
    const ParamEntry* patches = root->FindEntry("CfgPatches");
    const bool campaign = patches && patches->FindEntry("A3_Missions_F_EPA");
    const ParamEntry* entry = list->FindEntry(campaign ? "EastWind" : "Bootcamp");
    if (!entry || !entry->IsClass())
        return;
    const auto upper = [](const RString& text)
    {
        std::string out = (const char*)text;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return RString(out.c_str());
    };
    const size_t first = s.members.size();
    const InArmaCoordinates coordinates;
    // the slot's place back in Arma 3's coordinates, less where the tile's class puts itself
    const float ox = slot->X() * g_zone.w + g_zone.x - Number(*tile, "x", 0);
    const float oy = slot->Y() * g_zone.h + g_zone.y - Number(*tile, "y", 0);
    s.LoadControl(*tile, ox, oy, Rect2DFloat(-1, -1, 3, 3), -1, *s.foreground, kSpotlightFirst);
    if (s.members.size() == first)
        return;
    for (size_t g = s.groups.size(); g-- > 0;)
        if (s.groups[g].parent < 0)
        {
            s.groups[g].idc = kSpotlightFirst; // the tile's own class carries no idc
            break;
        }
    RString text = Text(*entry, "text");
    if (Integer(*entry, "textIsQuote", 0) != 0)
        text = RString("\"") + text + RString("\"");
    if (Integer(*entry, "disableUpperCase", 0) == 0)
        text = upper(text);
    for (size_t i = first; i < s.members.size(); i++)
        if (auto* label = dynamic_cast<A3Static*>(s.members[i].control))
        {
            if (label->IDC() == kSpotlightFirst + 3) // 1023: the picture
                label->SetPicture(Text(*entry, "picture"));
            else if (label->IDC() == 1028)
                label->SetText(text);
            else if (label->IDC() == 1027)
                label->SetText(upper(Text(*entry, "actionText")));
        }
    s.centreAction = Action::Campaigns;
    LOG_INFO(UI, "Arma 3 menu: middle spotlight is CfgMainMenuSpotlight >> {}", campaign ? "EastWind" : "Bootcamp");
}

void Screen::UpdateMainMenu(float mouseX, float mouseY)
{
    Impl& s = *_impl;
    // Which row the cursor asks for: the title (or its icon) it is over, else the open row while
    // the cursor is still over it or its title.
    int want = -1;
    for (int title = kTitleFirst; title <= kTitleLast && want < 0; title++)
        for (const int idc : {title, title + (kIconFirst - kTitleFirst)})
            if (Control* c = s.Find(idc); c && c->IsVisible() && c->IsInside(mouseX, mouseY))
            {
                want = s.FindGroup(title - 10);
                break;
            }
    if (want < 0 && s.open >= 0 && s.pinned)
        want = s.open;
    if (want < 0 && s.open >= 0)
    {
        const Group& g = s.groups[s.open];
        Control* title = s.Find(g.idc + 10);
        // a little slack: the row hangs one pixel under its title
        const bool overRow = s.Inside(g, mouseX, mouseY) || (mouseX >= g.x && mouseX <= g.x + g.w &&
                                                             mouseY >= g.y - 0.01f && mouseY <= g.y + g.h);
        if (overRow || (title && title->IsInside(mouseX, mouseY)))
            want = s.open;
    }
    bool changed = false;
    if (want != s.open)
    {
        if (s.open >= 0)
            s.groups[s.open].shown = false;
        if (want >= 0)
            s.groups[want].shown = true;
        s.open = want;
        s.pinned = false;
        changed = true;
    }
    // spotlight hover frames
    for (size_t i = 0; i < s.groups.size(); i++)
    {
        Group& g = s.groups[i];
        if (g.idc != kSpotlightHover || g.parent < 0)
            continue;
        const Group& spot = s.groups[g.parent];
        const bool hover = spot.shown && s.Inside(spot, mouseX, mouseY);
        if (hover != g.shown)
        {
            g.shown = hover;
            changed = true;
        }
    }
    if (changed)
        s.Refresh();
}

void Screen::SetText(int idc, const RString& text)
{
    for (const Member& m : _impl->members)
        if (m.control->IDC() == idc)
        {
            if (auto* label = dynamic_cast<A3Static*>(m.control))
                label->SetLabel(text);
            else if (auto* control = dynamic_cast<A3Control*>(m.control))
                control->SetText(text);
        }
}

void Screen::Show(int idc, bool shown)
{
    Impl& s = *_impl;
    for (Group& g : s.groups)
        if (g.idc == idc)
            g.shown = shown;
    for (Member& m : s.members)
        if (m.control->IDC() == idc)
            m.shown = shown;
    s.Refresh();
}

void Screen::SetPicture(int idc, const RString& name)
{
    for (const Member& m : _impl->members)
        if (m.control->IDC() == idc)
            if (auto* picture = dynamic_cast<A3Static*>(m.control))
                picture->SetPicture(name);
}

void Screen::SetRows(int idc, const std::vector<RString>& rows, int selected)
{
    for (const Member& m : _impl->members)
        if (m.control->IDC() == idc)
            if (auto* list = dynamic_cast<A3List*>(m.control))
                list->SetRows(rows, selected);
}

int Screen::Selected(int idc) const
{
    for (const Member& m : _impl->members)
        if (m.control->IDC() == idc)
            if (auto* list = dynamic_cast<A3List*>(m.control))
                return list->Selected();
    return -1;
}

bool SetControlText(IControl* control, const RString& text)
{
    if (auto* html = dynamic_cast<A3Html*>(control))
        html->SetPage(RString(), text);
    else if (auto* label = dynamic_cast<A3Static*>(control))
        label->SetLabel(text);
    else if (auto* other = dynamic_cast<A3Control*>(control))
        other->SetText(text);
    else
        return false;
    return true;
}

bool ControlText(IControl* control, RString& text)
{
    auto* a3 = dynamic_cast<A3Control*>(control);
    if (!a3)
        return false;
    text = a3->GetText();
    return true;
}

bool Screen::Checked(int idc) const
{
    for (const Member& m : _impl->members)
        if (m.control->IDC() == idc)
            if (auto* box = dynamic_cast<A3CheckBox*>(m.control))
                return box->Checked();
    return false;
}

void Screen::SetChecked(int idc, bool checked)
{
    for (const Member& m : _impl->members)
        if (m.control->IDC() == idc)
            if (auto* box = dynamic_cast<A3CheckBox*>(m.control))
                box->SetChecked(checked);
}

bool Screen::Has(int idc) const
{
    return _impl->Find(idc) != nullptr || _impl->FindGroup(idc) >= 0;
}

// RscDisplayInterrupt.sqf's onLoad, natively: what does not apply here is hidden -- the
// background tiles it animates, the debug console, the hub, cutscene and hint buttons -- and Save
// or Load is shown as CWA's own pause menu decides (one save slot).
// Arma 3's menu music: its intro scenes play Track14_MainMenu at 0.7 and start it again 12 s after
// it stops (map_*_scenes_f, initIntro.sqf). The menu here has no intro scene, so it does that itself.
void TickMenuMusic()
{
    static bool started = false, waiting = false;
    static Foundation::UITime stoppedAt;
    if (!GSoundScene || !GWorld || !GWorld->GetGameState())
        return;
    if (GSoundScene->MusicPlaying())
    {
        waiting = false;
        return;
    }
    if (started && !waiting)
    {
        waiting = true;
        stoppedAt = Glob.uiTime;
    }
    if (started && Glob.uiTime - stoppedAt < 12.0f)
        return;
    started = true;
    waiting = false;
    GWorld->GetGameState()->Execute("0 fadeMusic 0.7; playMusic \"Track14_MainMenu\"");
}

void Screen::InitInterrupt(bool multiplayer, bool saveExists, const RString& missionName)
{
    // Video / Audio / Controls / Game (301..307) unfold under Options in Arma 3; Options opens this
    // engine's Options screen, so they stay folded.
    for (const int idc : {115099, 13184, 2408, 1002, 1003, 122, 121, 301, 302, 303, 307})
        Show(idc, false);
    Show(kInterruptSave, !multiplayer && !saveExists);
    Show(kInterruptLoad, !multiplayer && saveExists);
    SetText(120, missionName);
    SetText(1005, RString("OpenPoseidon"));
    SetText(109, RString());

    // RscDisplayInterrupt.sqf stacks the buttons that are shown from Continue down, without gaps.
    Impl& s = *_impl;
    Control* first = s.Find(IDC_CANCEL);
    if (!first)
        return;
    std::vector<Control*> column;
    for (const Member& m : s.members)
        if (m.control->IsVisible() && dynamic_cast<A3Button*>(m.control) &&
            std::abs(m.control->X() - first->X()) < 0.002f && m.control->Y() >= first->Y() - 0.001f)
            column.push_back(m.control);
    std::sort(column.begin(), column.end(), [](Control* a, Control* b) { return a->Y() < b->Y(); });
    float y = first->Y();
    for (Control* c : column)
    {
        c->SetPos(c->X(), y, c->W(), c->H());
        y += c->H() * 1.1f;
    }
}

int Screen::InterruptClick(int idc) const
{
    switch (idc)
    {
        case kInterruptLoad: // Arma 3's Load is CWA's 102
            return IDC_INT_LOAD;
        case 301: // Video, Audio, Controls, Game: the Options screen
        case 302:
        case 303:
        case 307:
            return IDC_INT_OPTIONS;
        default:
            return idc;
    }
}

Action Screen::MainMenuClick(int idc, const char*& message)
{
    Impl& s = *_impl;
    message = nullptr;
    if ((idc >= kTitleFirst && idc <= kTitleLast) || (idc >= kIconFirst && idc <= kIconLast))
    {
        // a click on a title opens its row too (keyboard, touch)
        const int title = idc >= kIconFirst ? idc - (kIconFirst - kTitleFirst) : idc;
        const int g = s.FindGroup(title - 10);
        if (g >= 0)
        {
            if (s.open >= 0)
                s.groups[s.open].shown = false;
            s.groups[g].shown = true;
            s.open = g;
            s.pinned = true;
            s.Refresh();
        }
        return Action::Handled;
    }
    switch (idc)
    {
        case 101: // Campaign
        case 149: // Bootcamp (a campaign of its own)
            return Action::Campaigns;
        case 104: // All Missions
        case 103: // Scenarios
        case 150: // Showcases
        case 151: // Challenges
            return Action::Missions;
        case 152: // VR Training
            return Action::Tutorials;
        case 301: // Video
        case 302: // Audio
        case 303: // Controls
        case 307: // Game
            return Action::Options;
        case 142: // Editor (Eden)
            message = "Arma 3's Eden editor is not available in OpenPoseidon. Its scenarios can be played from "
                      "SINGLEPLAYER > SCENARIOS.";
            return Action::Unavailable;
        case 154: // Quick Play
        case 206: // Apex co-op campaign
            message = "Arma 3 multiplayer is not available in OpenPoseidon.";
            return Action::Unavailable;
        case 203: // Virtual Arsenal
            message = "The Virtual Arsenal is not available in OpenPoseidon: Arma 3 builds it from scripts and "
                      "controls this engine does not have.";
            return Action::Unavailable;
        case 140: // Expansions
        case 143: // Field Manual
        case 204: // Community Guides
        case 155: // Units
            message = "This Arma 3 screen is not available in OpenPoseidon.";
            return Action::Unavailable;
        default:
            break;
    }
    if (idc == kSpotlightFirst && s.centreAction != Action::Unavailable)
        return s.centreAction;
    if (idc >= kSpotlightFirst && idc <= kSpotlightLast)
    {
        message = idc == kSpotlightLast ? "Arma 3's Eden editor is not available in OpenPoseidon."
                                        : "Arma 3 multiplayer is not available in OpenPoseidon.";
        return Action::Unavailable;
    }
    return Action::None; // Quit (106), Player (109), Server Browser (105): DisplayMain's own
}

namespace
{
// RscDisplayMainMap's diary: subjects (1001), the subject's records (1002), the record's page (1003)
// and the controls this engine has nothing behind (a3\ui_f\hpp\RscDiary).
constexpr int kDiarySubjects = 1001, kDiaryRecords = 1002, kDiaryPage = 1003, kDiaryMission = 112,
              kDiaryPlayer = 111, kDiaryClock = 101;
// the tooltip and mouse-over labels, the briefing's intro layers, the players / mute controls, the GPS
// and radio panels (CWA's own stay in the engine), the fade-in, the respawn and high-command panels
constexpr int kDiaryHidden[] = {2350, 1016, 107, 117, 118, 119, 106, 103, 1099, 1100, 1101, 1102,
                                1124, 1125, 88800, 88850};

const char* SubjectName(const std::string& subject)
{
    if (_stricmp(subject.c_str(), "Diary") == 0)
        return "Briefing";
    return subject.c_str();
}
} // namespace

void Screen::InitDiary(const RString& missionName, const RString& playerName)
{
    for (const int idc : kDiaryHidden)
        Show(idc, false);
    SetText(kDiaryMission, missionName);
    SetText(kDiaryPlayer, playerName);
    _impl->revision = -1;
    UpdateDiary();
}

void Screen::UpdateDiary()
{
    Impl& s = *_impl;
    const float day = Glob.clock.GetTimeOfDay();
    const int minutes = static_cast<int>(day * 24 * 60) % (24 * 60);
    char clock[8];
    snprintf(clock, sizeof(clock), "%02d:%02d", minutes / 60, minutes % 60);
    SetText(kDiaryClock, RString(clock));

    const int revision = Arma2BriefingRevision();
    if (revision == s.revision)
        return;
    s.revision = revision;
    s.records = Arma2DiaryRecords();
    // the briefing first, then the subjects as scripts made them, the tasks last
    const std::string chosen = s.subject >= 0 && s.subject < static_cast<int>(s.subjects.size())
                                   ? s.subjects[s.subject]
                                   : std::string();
    s.subjects.clear();
    s.subjects.push_back("Diary");
    for (const auto& r : s.records)
        if (r[0] != "Tasks" && std::find(s.subjects.begin(), s.subjects.end(), r[0]) == s.subjects.end())
            s.subjects.push_back(r[0]);
    s.subjects.push_back("Tasks");
    s.subject = 0;
    for (size_t i = 0; i < s.subjects.size(); i++)
        if (s.subjects[i] == chosen)
            s.subject = static_cast<int>(i);
    std::vector<RString> rows;
    for (const std::string& subject : s.subjects)
        rows.push_back(RString(SubjectName(subject)));
    SetRows(kDiarySubjects, rows, s.subject);
    DiaryEvent(kDiarySubjects, s.subject);
}

bool Screen::DiaryEvent(int idc, int row)
{
    Impl& s = *_impl;
    if (idc == kDiarySubjects)
    {
        if (row < 0 || row >= static_cast<int>(s.subjects.size()))
            return true;
        s.subject = row;
        // a subject's records newest first, as Arma 3 lists them; its tasks in the order they were given
        const std::string& subject = s.subjects[row];
        s.shownRecords.clear();
        for (int i = static_cast<int>(s.records.size()) - 1; i >= 0; i--)
            if (s.records[i][0] == subject)
                s.shownRecords.push_back(i);
        if (subject == "Tasks")
            std::reverse(s.shownRecords.begin(), s.shownRecords.end());
        std::vector<RString> rows;
        for (const int i : s.shownRecords)
            rows.push_back(PlainText(RString(s.records[i][1].c_str())));
        SetRows(kDiaryRecords, rows, 0);
        return DiaryEvent(kDiaryRecords, 0);
    }
    if (idc == kDiaryRecords)
    {
        s.record = row;
        RString heading, page;
        if (row >= 0 && row < static_cast<int>(s.shownRecords.size()))
        {
            const auto& r = s.records[s.shownRecords[row]];
            heading = PlainText(RString(r[1].c_str()));
            page = RString(r[2].c_str());
        }
        for (const Member& m : s.members)
            if (m.control->IDC() == kDiaryPage)
                if (auto* html = dynamic_cast<A3Html*>(m.control))
                    html->SetPage(heading, page);
        return true;
    }
    return false;
}

namespace
{
// RscInGameUI's idcs: the weapon panel (RscUnitInfo) and the stance indicator (RscStanceInfo).
constexpr int kHudWeapon = 118, kHudAmmo = 184, kHudMagazines = 185, kHudAmmoType = 155, kHudGrenade = 152,
              kHudGrenades = 151, kHudGunner = 150, kHudModeTexture = 187, kHudWeaponGroup = 2302, kHudStance = 188;

// The HUD's controls belong to no display: they are held here and drawn by DrawHud.
class HudHolder : public ControlsContainer
{
  public:
    HudHolder() : ControlsContainer(nullptr) {}
    RefArray<Control>& Background() { return _controlsBackground; }
    RefArray<Control>& Foreground() { return _controlsForeground; }
};

Screen* LoadHud()
{
    static Screen* hud = []() -> Screen*
    {
        // never destroyed: its textures and fonts would be released after the engine is gone
        auto* holder = new HudHolder();
        holder->AddRef();
        auto* screen = new Screen();
        if (!screen->Load(*holder, "RscInGameUI/RscUnitInfoSoldier", holder->Background(), holder->Foreground()))
        {
            LOG_WARN(UI, "Arma 3 HUD: RscInGameUI/RscUnitInfoSoldier is not in Arma 3's config; CWA's HUD stays");
            return nullptr;
        }
        screen->Load(*holder, "RscInGameUI/RscStanceInfo", holder->Background(), holder->Foreground());
        // the fire mode's picture and the gunner's weapon are its vehicles'
        screen->Show(kHudModeTexture, false);
        screen->Show(kHudGunner, false);
        return screen;
    }();
    return hud;
}
} // namespace

bool HudActive()
{
    return Active() && LoadHud() != nullptr;
}

void DrawHud(const HudState& state, float alpha)
{
    Screen* hud = LoadHud();
    if (!hud)
        return;
    hud->Show(kHudWeaponGroup, state.weaponShown);
    hud->SetText(kHudWeapon, state.weapon);
    hud->SetText(kHudAmmo, state.ammo);
    hud->SetText(kHudMagazines, state.magazines);
    hud->SetText(kHudAmmoType, state.ammoType);
    hud->SetText(kHudGrenade, state.grenade);
    hud->SetText(kHudGrenades, state.grenades);
    static const char* const stances[] = {"\\A3\\Ui_f\\data\\IGUI\\RscIngameUI\\RscUnitInfo\\SI_stand_ca.paa",
                                          "\\A3\\Ui_f\\data\\IGUI\\RscIngameUI\\RscUnitInfo\\SI_crouch_ca.paa",
                                          "\\A3\\Ui_f\\data\\IGUI\\RscIngameUI\\RscUnitInfo\\SI_prone_ca.paa"};
    static int shownStance = -1;
    if (state.stance != shownStance)
    {
        shownStance = std::clamp(state.stance, 0, 2);
        hud->SetPicture(kHudStance, RString(stances[shownStance]));
    }
    hud->Draw(alpha);
}
} // namespace Poseidon::Arma3Menu
