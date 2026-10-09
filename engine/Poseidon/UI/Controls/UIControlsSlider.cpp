#include <algorithm>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/Locale/Stringtable/CodepageTranscode.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/World.hpp>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_keycode.h>
#include <Poseidon/UI/Locale/AutoComplete.hpp>
#include <Poseidon/Foundation/Common/Win.h>
#include <Poseidon/Core/Application.hpp>

#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>

#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>

#include <Poseidon/Foundation/Algorithms/Qsort.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/BoolArray.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

using namespace Poseidon;
namespace Poseidon
{
using Poseidon::Foundation::QSort;

} // namespace Poseidon
#include <Poseidon/Foundation/Strings/Mbcs.hpp>

namespace Poseidon
{
RString FindPicture(RString name);
}
extern DrawCoord SceneToScreen(Vector3Par pos);

#define SCROLL_SPEED 100.0
#define SCROLL_MIN 2.0
#define SCROLL_MAX 10.0

#define ISSPACE(c) ((c) >= 0 && (c) <= 32)

#define CX(x) (toInt((x) * w) + 0.5)
#define CY(y) (toInt((y) * h) + 0.5)

#define DrawBottom(i, color) \
    GLOB_ENGINE->DrawLine(Line2DPixel(xx + i, yy + hh - 1 - i, xx + ww - i, yy + hh - 1 - i), color, color);
#define DrawRight(i, color) \
    GLOB_ENGINE->DrawLine(Line2DPixel(xx + ww - 1 - i, yy + hh - 1 - i, xx + ww - 1 - i, yy + 0 + i), color, color);
#define DrawLeft(i, color) GLOB_ENGINE->DrawLine(Line2DPixel(xx + i, yy + hh - 1 - i, xx + i, yy + i), color, color);
#define DrawTop(i, color) GLOB_ENGINE->DrawLine(Line2DPixel(xx + i, yy + i, xx + ww - 1 - i, yy + i), color, color);

inline PackedColor ModAlpha(PackedColor color, float alpha)
{
    int a = toInt(alpha * color.A8());
    saturate(a, 0, 255);
    return PackedColorRGB(color, a);
}

inline bool PointInRect(float x, float y, float left, float top, float width, float height)
{
    return x >= left && x <= left + width && y >= top && y <= top + height;
}

CActiveText::CActiveText(ControlsContainer* parent, int idc, const ParamEntry& cls)
    : Control(parent, CT_ACTIVETEXT, idc, cls)
{
    _textCls = &cls;
    _text = DecodeLegacyTextToRString(cls >> "text", GLanguage);
    _font = GLOB_ENGINE->LoadFont(GetFontID(cls >> "font"));
    const ParamEntry* entry = cls.FindEntry("size");
    if (entry)
    {
        _size = (float)(*entry) * _font->Height();
    }
    else
    {
        _size = cls >> "sizeEx";
    }
    _color = GetPackedColor(cls >> "color");
    _colorActive = GetPackedColor(cls >> "colorActive");
    // Optional focus-highlight background.  Default transparent so
    // controls without colorFocusBg[] in their resource render the
    // same as before this attribute existed.
    _colorFocusBg = PackedColor(0, 0, 0, 0);
    if (const ParamEntry* fbg = cls.FindEntry("colorFocusBg"))
    {
        _colorFocusBg = GetPackedColor(*fbg);
    }

    GetValue(_enterSound, cls >> "soundEnter");
    GetValue(_pushSound, cls >> "soundPush");
    GetValue(_clickSound, cls >> "soundClick");
    GetValue(_escapeSound, cls >> "soundEscape");

    _active = false;

    entry = cls.FindEntry("action");
    if (entry)
    {
        _action = *entry;
    }
}

bool CActiveText::OnKeyDown(unsigned nChar, unsigned nRepCnt, unsigned nFlags)
{
    if (IsUiConfirmKey(nChar))
    {
        if (IsEnabled())
        {
            _returnDown = true;
            PlaySound(_pushSound);
            return true;
        }
    }
    return false;
}

bool CActiveText::OnKeyUp(unsigned nChar, unsigned nRepCnt, unsigned nFlags)
{
    if (IsUiConfirmKey(nChar))
    {
        // Orphan KEY_UP — KEY_DOWN went to a different control (typically
        // a since-destroyed display) and only the release reached us.  Do
        // NOT synthesize a click; that's how a single Enter tap on a
        // closing screen would re-open the parent's selected entry.
        if (!_returnDown)
            return false;
        _returnDown = false;
        if (IsEnabled())
        {
            PlaySound(_clickSound);
            if (_action.GetLength() > 0)
            {
                GameState* gstate = GWorld->GetGameState();
                gstate->Execute(_action);
                GWorld->SimulateScripts();
            }
            _parent->OnButtonClicked(IDC());
            return true;
        }
    }
    return false;
}

bool CActiveText::IsInside(float x, float y)
{
    if (_text.GetLength() == 0 || !_font)
        return Control::IsInside(x, y);
    // The same placement OnDraw uses, so the hot area is exactly the label.
    const float size = _scale * _size;
    const float wText = GLOB_ENGINE->GetTextWidth(size, _font, _text);
    float left = _x, top = _y + 0.5f * (_h - size), w = wText, h = size;
    switch (_style & ST_HPOS)
    {
        case ST_UP:
            left = _x + 0.5f * (_w - size), top = _y, w = size, h = wText;
            break;
        case ST_VCENTER:
            left = _x + 0.5f * (_w - size), top = _y + 0.5f * (_h - wText), w = size, h = wText;
            break;
        case ST_DOWN:
            left = _x + 0.5f * (_w - size), top = _y + _h - wText, w = size, h = wText;
            break;
        case ST_RIGHT:
            left = _x + _w - wText;
            break;
        case ST_CENTER:
            left = _x + 0.5f * (_w - wText);
            break;
        default:
            break;
    }
    // A hair of slack so the edge of a glyph still counts; never past the control.
    constexpr float pad = 0.004f;
    const float x0 = std::max(_x, left - pad), x1 = std::min(_x + _w, left + w + pad);
    const float y0 = std::max(_y, top - pad), y1 = std::min(_y + _h, top + h + pad);
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

void CActiveText::OnMouseMove(float x, float y, bool active)
{
    OnMouseHold(x, y, active);
}

void CActiveText::OnMouseHold(float x, float y, bool active)
{
    bool newActive = active && IsInside(x, y);
    if (newActive && !_active)
    {
        PlaySound(_enterSound);
    }
    _active = newActive;
}

void CActiveText::OnLButtonDown(float x, float y)
{
    _pressX = _x;
    _pressY = _y;
    _pressW = _w;
    _pressH = _h;
    PlaySound(_pushSound);
}

void CActiveText::OnLButtonUp(float x, float y)
{
    // Released over the label it was pressed on. (The press rectangle is kept for
    // a control that moved between press and release, but only where its label was.)
    if (IsInside(x, y) || (PointInRect(x, y, _pressX, _pressY, _pressW, _pressH) &&
                           IsInside(x - (_pressX - _x), y - (_pressY - _y))))
    {
        PlaySound(_clickSound);
        if (_action.GetLength() > 0)
        {
            GameState* gstate = GWorld->GetGameState();
            gstate->Execute(_action);
            GWorld->SimulateScripts();
        }
        _parent->OnButtonClicked(IDC());
    }
    else
    {
        PlaySound(_escapeSound);
    }
}

void CActiveText::OnDraw(float alpha)
{
    float size = _scale * _size;
    bool vertical = false;
    float height = size;
    float left = _x + 0.5 * (_w - height);
    float top = _y + 0.5 * (_h - height);
    float wText = GLOB_ENGINE->GetTextWidth(size, _font, _text);
    switch (_style & ST_HPOS)
    {
        case ST_UP:
            top = _y;
            vertical = true;
            break;
        case ST_VCENTER:
            top = _y + 0.5 * (_h - wText);
            vertical = true;
            break;
        case ST_DOWN:
            top = _y + _h - wText;
            vertical = true;
            break;
        case ST_RIGHT:
            left = _x + _w - wText;
            break;
        case ST_CENTER:
            left = _x + 0.5 * (_w - wText);
            break;
        default:
            PoseidonAssert((_style & ST_HPOS) == ST_LEFT) left = _x;
            break;
    }
    PackedColor color = ModAlpha(_active ? _colorActive : _color, alpha);
    if (!IsEnabled())
    {
        color = ModAlpha(color, 0.25);
    }
    bool focused = IsFocused();
    bool selected = IsDefault() && !_parent->GetFocused()->CanBeDefault();
    const float w = GLOB_ENGINE->Width2D();
    const float h = GLOB_ENGINE->Height2D();
    // Optional focus highlight — filled rect behind the text drawn from
    // colorFocusBg.  Skipped when alpha is zero so controls without the
    // attribute set render exactly as before.  Goes first so the text
    // and underline land on top.
    if (focused && _colorFocusBg.A8() > 0)
    {
        PackedColor bg = ModAlpha(_colorFocusBg, alpha);
        MipInfo mip = GLOB_ENGINE->TextBank()->UseMipmap(nullptr, 0, 0);
        GEngine->Draw2D(mip, bg, Rect2DPixel(_x * w, _y * h, _w * w, _h * h));
    }
    if (_text.GetLength() > 0)
    {
        if (vertical)
        {
            if (focused || selected)
            {
                PackedColor col;
                if (focused)
                {
                    col = color;
                }
                else
                {
                    col = ModAlpha(color, 0.5);
                }
                GEngine->DrawLine(Line2DPixel(_x * w, top * h, _x * w, top * h + wText * w), col, col);
            }
            GLOB_ENGINE->DrawTextVertical(Point2DFloat(left, top), size, Rect2DFloat(_x, _y, _w, _h), _font, color,
                                          _text);
        }
        else
        {
            if (focused || selected)
            {
                PackedColor col;
                if (focused)
                {
                    col = color;
                }
                else
                {
                    col = ModAlpha(color, 0.5);
                }
                GEngine->DrawLine(Line2DPixel(left * w, (_y + _h) * h, (left + wText) * w, (_y + _h) * h), col, col);
            }
            GLOB_ENGINE->DrawText(Point2DFloat(left, top), size, Rect2DFloat(_x, _y, _w, _h), _font, color, _text);
        }
    }
    else
    {
        if (selected)
        {
            GEngine->DrawLine(Line2DPixel(_x * w, _y * h, _x * w, (_y + _h) * h), color, color);
            GEngine->DrawLine(Line2DPixel(_x * w, (_y + _h) * h, (_x + _w) * w, (_y + _h) * h), color, color);
            GEngine->DrawLine(Line2DPixel((_x + _w) * w, (_y + _h) * h, (_x + _w) * w, _y * h), color, color);
            GEngine->DrawLine(Line2DPixel((_x + _w) * w, _y * h, _x * w, _y * h), color, color);
        }
    }
}

CToolBox::CToolBox(ControlsContainer* parent, int idc, const ParamEntry& cls) : Control(parent, CT_TOOLBOX, idc, cls)
{
    _textCls = &cls;
    int i, n = (cls >> "strings").GetSize();
    _strings.Resize(n);
    for (i = 0; i < n; i++)
    {
        _strings[i] = (cls >> "strings")[i];
    }

    _rows = cls >> "rows";
    _columns = cls >> "columns";

    _ftColor = GetPackedColor(cls >> "colorText");
    _bgColor = GetPackedColor(cls >> "color");
    _ftSelectColor = GetPackedColor(cls >> "colorTextSelect");
    _bgSelectColor = GetPackedColor(cls >> "colorSelect");
    _ftDisabledColor = GetPackedColor(cls >> "colorTextDisable");
    _bgDisabledColor = GetPackedColor(cls >> "colorDisable");
    _font = GLOB_ENGINE->LoadFont(GetFontID(cls >> "font"));
    const ParamEntry* entry = cls.FindEntry("size");
    if (entry)
    {
        _size = (float)(*entry) * _font->Height();
    }
    else
    {
        _size = cls >> "sizeEx";
    }

    SetCurSel(0);
}

bool CToolBox::IsSelected(int i) const
{
    return i == _selected;
}

void CToolBox::ChangeSelection(int i)
{
    SetCurSel(i);
    if (_parent)
    {
        _parent->OnToolBoxSelChanged(_idc, _selected);
    }
}

bool CToolBox::OnKeyDown(unsigned nChar, unsigned nRepCnt, unsigned nFlags)
{
    switch (nChar)
    {
        case SDLK_LEFT:
        case SDLK_UP:
            ChangeSelection(_selected - 1);
            return true;
        case SDLK_RIGHT:
        case SDLK_DOWN:
            ChangeSelection(_selected + 1);
            return true;
    }
    return false;
}

void CToolBox::OnLButtonDown(float x, float y)
{
    float w = _w / _columns;
    float h = _h / _rows;

    int c = toIntFloor((x - _x) / w);
    int r = toIntFloor((y - _y) / h);
    if (c < 0 || c >= _columns)
    {
        return;
    }
    if (r < 0 || r >= _rows)
    {
        return;
    }

    int i = r * _columns + c;
    if (i >= 0 && i < _strings.Size())
    {
        ChangeSelection(i);
    }
}

void CToolBox::OnDraw(float alpha)
{
    PackedColor colorBg;
    if (!IsEnabled())
    {
        colorBg = ModAlpha(_bgDisabledColor, alpha);
    }
    else if (IsFocused())
    {
        colorBg = ModAlpha(_bgSelectColor, alpha);
    }
    else
    {
        colorBg = ModAlpha(_bgColor, alpha);
    }
    float wScr = GLOB_ENGINE->Width2D();
    float hScr = GLOB_ENGINE->Height2D();

    float w = _w / _columns;
    float h = _h / _rows;

    const float border = 0.005;
    int sel = GetCurSel();
    int row = sel / _columns;
    int col = sel % _columns;
    float yy = _y + row * h + border;
    float xx = _x + col * w + border;
    float ww = w - 2 * border;
    float hh = h - 2 * border;
    GLOB_ENGINE->DrawLine(Line2DPixel(wScr * xx, hScr * yy, wScr * (xx + ww), hScr * yy), colorBg, colorBg);
    GLOB_ENGINE->DrawLine(Line2DPixel(wScr * (xx + ww), hScr * yy, wScr * (xx + ww), hScr * (yy + hh)), colorBg,
                          colorBg);
    GLOB_ENGINE->DrawLine(Line2DPixel(wScr * (xx + ww), hScr * (yy + hh), wScr * xx, hScr * (yy + hh)), colorBg,
                          colorBg);
    GLOB_ENGINE->DrawLine(Line2DPixel(wScr * xx, hScr * (yy + hh), wScr * xx, hScr * yy), colorBg, colorBg);

    float hText = _size;

    int i = 0, n = _strings.Size();
    for (int y = 0; y < _rows; y++)
    {
        for (int x = 0; x < _columns; x++)
        {
            if (i >= n)
            {
                return;
            }

            float leftBound = _x + x * w;
            float left = leftBound;
            switch (_style & ST_HPOS)
            {
                case ST_RIGHT:
                    left += w - GLOB_ENGINE->GetTextWidth(_size, _font, _strings[i]);
                    break;
                case ST_CENTER:
                    left += 0.5 * (w - GLOB_ENGINE->GetTextWidth(_size, _font, _strings[i]));
                    break;
                default:
                    PoseidonAssert((_style & ST_HPOS) == ST_LEFT) break;
            }
            float topBound = _y + y * h;
            float top = topBound + 0.5 * (h - hText);

            PackedColor colorFt;
            if (IsSelected(i))
            {
                colorFt = ModAlpha(_ftSelectColor, alpha);
            }
            else
            {
                colorFt = ModAlpha(_ftColor, alpha);
            }
            GLOB_ENGINE->DrawText(Point2DFloat(left, top), _size, Rect2DFloat(leftBound, topBound, w, h), _font,
                                  colorFt, _strings[i]);
            i++;
        }
    }
}

CCheckBoxes::CCheckBoxes(ControlsContainer* parent, int idc, const ParamEntry& cls) : CToolBox(parent, idc, cls)
{
    _type = CT_CHECKBOXES;
}

bool CCheckBoxes::OnKeyDown(unsigned nChar, unsigned nRepCnt, unsigned nFlags)
{
    switch (nChar)
    {
        case SDLK_LEFT:
        case SDLK_UP:
            SetCurSel(_selected - 1);
            return true;
        case SDLK_RIGHT:
        case SDLK_DOWN:
            SetCurSel(_selected + 1);
            return true;
        case SDLK_SPACE:
            ChangeSelection(_selected);
            return true;
    }
    return false;
}

bool CCheckBoxes::IsSelected(int i) const
{
    return GetArray().Get(i);
}

void CCheckBoxes::ChangeSelection(int i)
{
    _array.Toggle(i);
    SetCurSel(i);
    if (_parent)
    {
        _parent->OnCheckBoxesSelChanged(_idc, _selected, _array.Get(i));
    }
}

CSliderContainer::CSliderContainer(const ParamEntry& cls)
{
    SetRange(0, 10);
    SetThumbPos(0);
    SetSpeed(1, 3);
    _thumbLocked = false;

    _color = GetPackedColor(cls >> "color");
}

CSlider::CSlider(ControlsContainer* parent, int idc, const ParamEntry& cls)
    : Control(parent, CT_SLIDER, idc, cls), CSliderContainer(cls)
{
}

void CSlider::OnLButtonDown(float x, float y)
{
    if ((_style & SL_DIR) == SL_VERT)
    {
        float spinHeight = (1.33 * 0.6) * _w;
        const float thumbHeight = 0.02;
        float top = _y + spinHeight + 0.5 * thumbHeight;
        float fieldHeight = _h - 2 * spinHeight - thumbHeight;

        float coef = 0;
        if (_maxPos > _minPos)
        {
            coef = 1.0 / (_maxPos - _minPos);
        }
        float thumbPos = top + fieldHeight * (_curPos - _minPos) * coef;

        if (y - _y < spinHeight)
        {
            SetThumbPos(_curPos - _lineStep);
            if (_parent)
            {
                _parent->OnSliderPosChanged(IDC(), _curPos);
            }
        }
        else if (y - _y > _h - spinHeight)
        {
            SetThumbPos(_curPos + _lineStep);
            if (_parent)
            {
                _parent->OnSliderPosChanged(IDC(), _curPos);
            }
        }
        else if (y < thumbPos - 0.5 * thumbHeight)
        {
            SetThumbPos(_curPos - _pageStep);
            if (_parent)
            {
                _parent->OnSliderPosChanged(IDC(), _curPos);
            }
        }
        else if (y > thumbPos + 0.5 * thumbHeight)
        {
            SetThumbPos(_curPos + _pageStep);
            if (_parent)
            {
                _parent->OnSliderPosChanged(IDC(), _curPos);
            }
        }
        else
        {
            _thumbLocked = true;
            _thumbOffset = y - thumbPos;
        }
    }
    else
    {
        float spinWidth = (0.75 * 0.6) * _h;
        const float thumbWidth = 0.015;
        float left = _x + spinWidth + 0.5 * thumbWidth;
        float fieldWidth = _w - 2 * spinWidth - thumbWidth;

        float coef = 0;
        if (_maxPos > _minPos)
        {
            coef = 1.0 / (_maxPos - _minPos);
        }
        float thumbPos = left + fieldWidth * (_curPos - _minPos) * coef;

        if (x - _x < spinWidth)
        {
            SetThumbPos(_curPos - _lineStep);
            if (_parent)
            {
                _parent->OnSliderPosChanged(IDC(), _curPos);
            }
        }
        else if (x - _x > _w - spinWidth)
        {
            SetThumbPos(_curPos + _lineStep);
            if (_parent)
            {
                _parent->OnSliderPosChanged(IDC(), _curPos);
            }
        }
        else if (x < thumbPos - 0.5 * thumbWidth)
        {
            SetThumbPos(_curPos - _pageStep);
            if (_parent)
            {
                _parent->OnSliderPosChanged(IDC(), _curPos);
            }
        }
        else if (x > thumbPos + 0.5 * thumbWidth)
        {
            SetThumbPos(_curPos + _pageStep);
            if (_parent)
            {
                _parent->OnSliderPosChanged(IDC(), _curPos);
            }
        }
        else
        {
            _thumbLocked = true;
            _thumbOffset = x - thumbPos;
        }
    }
}

void CSlider::OnLButtonUp(float x, float y)
{
    _thumbLocked = false;
}

void CSlider::OnMouseMove(float x, float y, bool active)
{
    if (_thumbLocked)
    {
        if ((_style & SL_DIR) == SL_VERT)
        {
            float spinHeight = (1.33 * 0.6) * _w;
            const float thumbHeight = 0.02;
            float top = _y + spinHeight + 0.5 * thumbHeight;
            float fieldHeight = _h - 2 * spinHeight - thumbHeight;

            float coef = 0;
            if (_maxPos > _minPos)
            {
                coef = _maxPos - _minPos;
            }

            if (y - _thumbOffset < top)
            {
                SetThumbPos(_minPos);
                if (_parent)
                {
                    _parent->OnSliderPosChanged(IDC(), _curPos);
                }
            }
            else if (y - _thumbOffset > top + fieldHeight)
            {
                SetThumbPos(_maxPos);
                if (_parent)
                {
                    _parent->OnSliderPosChanged(IDC(), _curPos);
                }
            }
            else
            {
                SetThumbPos(_minPos + (y - _thumbOffset - top) * coef / fieldHeight);
                if (_parent)
                {
                    _parent->OnSliderPosChanged(IDC(), _curPos);
                }
            }
        }
        else
        {
            float spinWidth = (0.75 * 0.6) * _h;
            const float thumbWidth = 0.015;
            float left = _x + spinWidth + 0.5 * thumbWidth;
            float fieldWidth = _w - 2 * spinWidth - thumbWidth;

            float coef = 0;
            if (_maxPos > _minPos)
            {
                coef = _maxPos - _minPos;
            }

            if (x - _thumbOffset < left)
            {
                SetThumbPos(_minPos);
                if (_parent)
                {
                    _parent->OnSliderPosChanged(IDC(), _curPos);
                }
            }
            else if (x - _thumbOffset > left + fieldWidth)
            {
                SetThumbPos(_maxPos);
                if (_parent)
                {
                    _parent->OnSliderPosChanged(IDC(), _curPos);
                }
            }
            else
            {
                SetThumbPos(_minPos + (x - _thumbOffset - left) * coef / fieldWidth);
                if (_parent)
                {
                    _parent->OnSliderPosChanged(IDC(), _curPos);
                }
            }
        }
    }
}

void CSlider::OnDraw(float alpha)
{
    const int w = GLOB_ENGINE->Width2D();
    const int h = GLOB_ENGINE->Height2D();

    PackedColor color = ModAlpha(_color, alpha);

    if ((_style & SL_DIR) == SL_VERT)
    {
        float bigLine = 0.6 * _w;
        float smallLine = 0.3 * _w;

        float spinHeight = 1.33 * bigLine;
        const float thumbHeight = 0.02;
        float fieldHeight = _h - 2 * spinHeight - thumbHeight;

        float top = _y + spinHeight + 0.5 * thumbHeight;
        float bottom = top + fieldHeight;
        float right = _x + bigLine;

        float coef = 0;
        if (_maxPos > _minPos)
        {
            coef = 1.0 / (_maxPos - _minPos);
        }
        float thumbPos = top + fieldHeight * (_curPos - _minPos) * coef;

        // draw top spin
        float center = _x + 0.5 * bigLine;
        GLOB_ENGINE->DrawLine(Line2DPixel(center * w, _y * h, _x * w, (_y + spinHeight) * h), color, color);
        GLOB_ENGINE->DrawLine(Line2DPixel(_x * w, (_y + spinHeight) * h, (_x + bigLine) * w, (_y + spinHeight) * h),
                              color, color);
        GLOB_ENGINE->DrawLine(Line2DPixel((_x + bigLine) * w, (_y + spinHeight) * h, center * w, _y * h), color, color);

        // draw right spin
        GLOB_ENGINE->DrawLine(Line2DPixel(center * w, (_y + _h) * h, _x * w, (_y + _h - spinHeight) * h), color, color);
        GLOB_ENGINE->DrawLine(
            Line2DPixel(_x * w, (_y + _h - spinHeight) * h, (_x + bigLine) * w, (_y + _h - spinHeight) * h), color,
            color);
        GLOB_ENGINE->DrawLine(Line2DPixel((_x + bigLine) * w, (_y + _h - spinHeight) * h, center * w, (_y + _h) * h),
                              color, color);

        // draw background
        GLOB_ENGINE->DrawLine(
            Line2DPixel(toInt(right * w) + 0.5, toInt(top * h) + 0.5, toInt(right * w), toInt(bottom * h) + 1.5), color,
            color);
        int lines = 1;
        float lineDist = fieldHeight;
        while (lineDist > 0.02)
        {
            lines *= 2;
            lineDist *= 0.5;
        }

        for (int i = 0; i <= lines; i++)
        {
            float pos = toInt((top + i * lineDist) * h) + 0.5;
            float left = right - ((i % 2 == 0 || lines == 1) ? bigLine : smallLine);
            GLOB_ENGINE->DrawLine(Line2DPixel(left * w, pos, right * w, pos), color, color);
        }

        // draw thumb
        GLOB_ENGINE->DrawLine(
            Line2DPixel((_x + bigLine) * w, thumbPos * h, (_x + _w) * w, (thumbPos - 0.5 * thumbHeight) * h), color,
            color);
        GLOB_ENGINE->DrawLine(Line2DPixel((_x + _w) * w, (thumbPos - 0.5 * thumbHeight) * h, (_x + _w) * w,
                                          (thumbPos + 0.5 * thumbHeight) * h),
                              color, color);
        GLOB_ENGINE->DrawLine(
            Line2DPixel((_x + _w) * w, (thumbPos + 0.5 * thumbHeight) * h, (_x + bigLine) * w, thumbPos * h), color,
            color);
    }
    else
    {
        float bigLine = 0.6 * _h;
        float smallLine = 0.3 * _h;
        float space = 0.15 * _h;

        float spinWidth = 0.75 * bigLine;
        const float thumbWidth = 0.015;
        float fieldWidth = _w - 2 * spinWidth - thumbWidth;

        float left = _x + spinWidth + 0.5 * thumbWidth;
        float right = left + fieldWidth;
        float bottom = _y + bigLine;

        float coef = 0;
        if (_maxPos > _minPos)
        {
            coef = 1.0 / (_maxPos - _minPos);
        }
        float thumbPos = left + fieldWidth * (_curPos - _minPos) * coef;

        // draw left spin
        float center = _y + 0.5 * bigLine;
        GLOB_ENGINE->DrawLine(Line2DPixel(_x * w, center * h, (_x + spinWidth) * w, _y * h), color, color);
        GLOB_ENGINE->DrawLine(Line2DPixel((_x + spinWidth) * w, _y * h, (_x + spinWidth) * w, (_y + bigLine) * h),
                              color, color);
        GLOB_ENGINE->DrawLine(Line2DPixel((_x + spinWidth) * w, (_y + bigLine) * h, _x * w, center * h), color, color);

        // draw right spin
        GLOB_ENGINE->DrawLine(Line2DPixel((_x + _w) * w, center * h, (_x + _w - spinWidth) * w, _y * h), color, color);
        GLOB_ENGINE->DrawLine(
            Line2DPixel((_x + _w - spinWidth) * w, _y * h, (_x + _w - spinWidth) * w, (_y + bigLine) * h), color,
            color);
        GLOB_ENGINE->DrawLine(Line2DPixel((_x + _w - spinWidth) * w, (_y + bigLine) * h, (_x + _w) * w, center * h),
                              color, color);

        // draw background
        GLOB_ENGINE->DrawLine(Line2DPixel(toInt(left * w) + 0.5, toInt(bottom * h) + 0.5, toInt(right * w) + 1.5,
                                          toInt(bottom * h) + 0.5),
                              color, color);
        int lines = 1;
        float lineDist = fieldWidth;
        while (lineDist > 0.015)
        {
            lines *= 2;
            lineDist *= 0.5;
        }

        for (int i = 0; i <= lines; i++)
        {
            float pos = toInt((left + i * lineDist) * w) + 0.5;
            float top = bottom - ((i % 2 == 0 || lines == 1) ? bigLine : smallLine);
            GLOB_ENGINE->DrawLine(Line2DPixel(pos, toInt(top * h) + 0.5, pos, toInt(bottom * h) + 0.5), color, color);
        }

        // draw thumb
        GLOB_ENGINE->DrawLine(
            Line2DPixel(thumbPos * w, (_y + bigLine + space) * h, (thumbPos - 0.5 * thumbWidth) * w, (_y + _h) * h),
            color, color);
        GLOB_ENGINE->DrawLine(Line2DPixel((thumbPos - 0.5 * thumbWidth) * w, (_y + _h) * h,
                                          (thumbPos + 0.5 * thumbWidth) * w, (_y + _h) * h),
                              color, color);
        GLOB_ENGINE->DrawLine(
            Line2DPixel((thumbPos + 0.5 * thumbWidth) * w, (_y + _h) * h, thumbPos * w, (_y + bigLine + space) * h),
            color, color);
    }
}

const float sbWidth = 0.02;

CScrollBar::CScrollBar()
{
    _thumbLocked = false;
}

void CScrollBar::OnLButtonDown(float x, float y)
{
    float spinSize = 0.8 * _w;
    if (2.0 * spinSize >= _h)
    {
        return; // too width scrollbar
    }
    if (_maxPos <= _minPos)
    {
        return;
    }

    y -= _y;
    float invRange = (_h - 2.0f * spinSize) / (_maxPos - _minPos);
    float thumbSize = _page * invRange;
    float thumbPos = spinSize + _curPos * invRange;

    if (y <= spinSize)
    {
        // lineUp
        _curPos -= _lineStep;
    }
    else if (y <= thumbPos)
    {
        // pageUp
        _curPos -= _pageStep;
    }
    else if (y <= thumbPos + thumbSize)
    {
        // thumb
        _thumbLocked = true;
        _thumbOffset = y - thumbPos;
    }
    else if (y <= _h - spinSize)
    {
        // pageDown
        _curPos += _pageStep;
    }
    else
    {
        // lineDown
        _curPos += _lineStep;
    }
    saturate(_curPos, _minPos, _maxPos - _page);
}

void CScrollBar::OnLButtonUp(float x, float y)
{
    _thumbLocked = false;
}

void CScrollBar::OnMouseHold(float x, float y)
{
    if (_thumbLocked)
    {
        float spinSize = 0.8 * _w;
        if (2.0 * spinSize >= _h)
        {
            return; // too width scrollbar
        }
        if (_maxPos <= _minPos)
        {
            return;
        }

        y -= _y;
        float range = (_maxPos - _minPos) / (_h - 2.0f * spinSize);
        float thumbPos = y - _thumbOffset;
        _curPos = (thumbPos - spinSize) * range;
        saturate(_curPos, _minPos, _maxPos - _page);
    }
}

static void DrawRect(float x1, float y1, float x2, float y2, PackedColor color)
{
    GEngine->DrawLine(Line2DPixel(x1, y1, x2, y1), color, color);
    GEngine->DrawLine(Line2DPixel(x2, y1, x2, y2), color, color);
    GEngine->DrawLine(Line2DPixel(x2, y2, x1, y2), color, color);
    GEngine->DrawLine(Line2DPixel(x1, y2, x1, y1), color, color);
}

void CScrollBar::OnDraw(float alpha)
{
    const int w = GLOB_ENGINE->Width2D();
    const int h = GLOB_ENGINE->Height2D();

    PackedColor color = ModAlpha(_color, alpha);

    // frame
    float xx = toInt(_x * w) + 0.5;
    float yy = toInt(_y * h) + 0.5;
    float ww = toInt(_w * w);
    float hh = toInt(_h * h);

    DrawRect(xx, yy, xx + ww, yy + hh, color);

    // spins
    float spinSize = 0.8 * _w;
    if (2.0 * spinSize >= _h)
    {
        return; // too width scrollbar
    }
    float borderX = toInt(0.25 * _w * w);
    float centerX = toInt(0.5 * _w * w);
    float borderY = toInt(0.2 * _w * h);
    float top = _y + spinSize;
    float tt = toInt(top * h) + 0.5;
    GEngine->DrawLine(Line2DPixel(xx, tt, xx + ww, tt), color, color);
    GEngine->DrawLine(Line2DPixel(xx + borderX, tt - borderY, xx + centerX, yy + borderY), color, color);
    GEngine->DrawLine(Line2DPixel(xx + centerX, yy + borderY, xx + ww - borderX, tt - borderY), color, color);
    float bottom = _y + _h - spinSize;
    float bb = toInt(bottom * h) + 0.5;
    GEngine->DrawLine(Line2DPixel(xx, bb, xx + ww, bb), color, color);
    GEngine->DrawLine(Line2DPixel(xx + borderX, bb + borderY, xx + centerX, yy + hh - borderY), color, color);
    GEngine->DrawLine(Line2DPixel(xx + centerX, yy + hh - borderY, xx + ww - borderX, bb + borderY), color, color);

    // thumb
    if (_maxPos <= _minPos)
    {
        return;
    }
    float invRange = (bottom - top) / (_maxPos - _minPos);
    float thumbSize = _page * invRange;
    float thumbPos = top + _curPos * invRange;
    DrawRect(xx + 2, thumbPos * h + 2, xx + ww - 2, (thumbPos + thumbSize) * h - 2, color);
}

CProgressBar::CProgressBar(ControlsContainer* parent, int idc, const ParamEntry& cls)
    : Control(parent, CT_PROGRESS, idc, cls)
{
    SetRange(0, 1);
    SetPos(0);

    _frameColor = GetPackedColor(cls >> "colorFrame");
    _barColor = GetPackedColor(cls >> "colorBar");
}

void CProgressBar::OnDraw(float alpha)
{
    const int w = GLOB_ENGINE->Width2D();
    const int h = GLOB_ENGINE->Height2D();

    float xx = toInt(_x * w) + 0.5;
    float yy = toInt(_y * h) + 0.5;
    float ww = toInt(_w * w);
    float hh = toInt(_h * h);

    PackedColor frameColor = ModAlpha(_frameColor, alpha);
    PackedColor barColor = ModAlpha(_barColor, alpha);

    // draw frame
    DrawLeft(0, frameColor);
    DrawTop(0, frameColor);
    DrawBottom(0, frameColor);
    DrawRight(0, frameColor);

    // draw bar
    if (_minPos < _curPos && _curPos <= _maxPos)
    {
        float coef = (_curPos - _minPos) / (_maxPos - _minPos);
        int width = toInt(coef * (ww - 2));

        Texture* texture = GLOB_SCENE->Preloaded(TextureWhite);
        MipInfo mip = GLOB_ENGINE->TextBank()->UseMipmap(texture, 0, 0);
        GLOB_ENGINE->Draw2D(mip, barColor, Rect2DPixel(xx + 1, yy + 1, width, hh - 2));
        // GLOB_ENGINE->TextBank()->ReleaseMipmap();
    }
}
