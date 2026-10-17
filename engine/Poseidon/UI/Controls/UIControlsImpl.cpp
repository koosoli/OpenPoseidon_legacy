#include <algorithm>
#include <Poseidon/UI/Controls/UIControls.hpp>
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
#include <stdio.h>
#include <string.h>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>

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

const float sbWidth = 0.02;

static void DrawRect(float x1, float y1, float x2, float y2, PackedColor color)
{
    GEngine->DrawLine(Line2DPixel(x1, y1, x2, y1), color, color);
    GEngine->DrawLine(Line2DPixel(x2, y1, x2, y2), color, color);
    GEngine->DrawLine(Line2DPixel(x2, y2, x1, y2), color, color);
    GEngine->DrawLine(Line2DPixel(x1, y2, x1, y1), color, color);
}

CListBoxContainer::CListBoxContainer(const ParamEntry& cls)
{
    _ftColor = GetPackedColor(cls >> "colorText");
    _selColor = GetPackedColor(cls >> "colorSelect");

    _selString = -1;
    _topString = 0;

    _showSelected = true;
    _readOnly = false;
}

static int CompareItems(const CListBoxItem* str0, const CListBoxItem* str1)
{
    return stricmp(str0->text, str1->text);
}

void CListBoxContainer::SortItems()
{
    QSort(Data(), Size(), CompareItems);
}

static int CompareItemsByValues(const CListBoxItem* str0, const CListBoxItem* str1)
{
    int diff = str0->value - str1->value;
    if (diff != 0)
    {
        return diff;
    }
    return stricmp(str0->text, str1->text);
}

void CListBoxContainer::SortItemsByValue()
{
    QSort(Data(), Size(), CompareItemsByValues);
}

void CListBoxContainer::InsertString(int i, RString text)
{
    if (i < 0 || i > GetSize())
    {
        return;
    }
    Insert(i);
    Set(i).text = text;
    Set(i).data = "";
    Set(i).value = 0;
    Set(i).ftColor = _ftColor;
    Set(i).selColor = _selColor;
    if (i <= _selString)
    {
        _selString++;
    }
}

void CListBoxContainer::DeleteString(int i)
{
    if (i < 0 || i >= GetSize())
    {
        return;
    }
    Delete(i);
    if (i == GetSize())
    {
        _selString = GetSize() - 1;
        if (i == 0)
        {
            _topString = 0;
        }
    }
}

void CListBoxContainer::OnMouseZChanged(float dz)
{
    if (dz == 0)
    {
        return;
    }
    if (_topString > 0 && dz > 0)
    {
        // scroll up
        float scroll = -SCROLL_SPEED * dz;
        saturate(scroll, -SCROLL_MAX, -SCROLL_MIN);
        _topString += 0.1 * scroll;
        if (_topString <= 1.0)
        {
            _topString = 0;
        }
    }
    else if (_topString + NRows() <= GetSize() && dz < 0)
    {
        // scroll down
        float scroll = -SCROLL_SPEED * dz;
        saturate(scroll, SCROLL_MIN, SCROLL_MAX);
        _topString += 0.1 * scroll;
        if (_topString <= 1.0)
        {
            _topString += 1.0;
        }
        if (_topString + NRows() > GetSize())
        {
            _topString = GetSize() - NRows();
        }
    }
}

void CListBoxContainer::Check()
{
    saturateMin(_topString, GetSize() - NRows());
    saturateMax(_topString, 0);
}

CListBox::CListBox(ControlsContainer* parent, int idc, const ParamEntry& cls)
    : Control(parent, CT_LISTBOX, idc, cls), CListBoxContainer(cls)
{
    if (const ParamEntry* selBg = cls.FindEntry("colorSelectBackground"); selBg && selBg->IsArray())
    {
        _hasSelBg = true;
        _selBgColor = GetPackedColor(*selBg);
    }
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
    float height = cls >> "rowHeight";
    SetRowHeight(height);

    _scrollUp = false;
    _scrollDown = false;

    _showStrings = _h / _rowHeight;
    saturateMax(_showStrings, 0);

    _scrollbar.SetPosition((_x + _w - sbWidth) * _scale, _y * _scale, sbWidth * _scale, _h * _scale);
    _scrollbar.SetColor(_ftColor);
    _scrollbar.SetRange(0, 0, _showStrings);
    _scrollbar.SetPos(0);
    _scrollbar.SetSpeed(1, floatMax(_showStrings - 1, 1));
}

void CListBox::SetPos(float x, float y, float w, float h)
{
    Control::SetPos(x, y, w, h);
    // update _showStrings
    _showStrings = _h / _rowHeight;
    saturateMax(_showStrings, 0);
    _scrollbar.SetSpeed(1, floatMax(_showStrings - 1, 1));
}

void CListBox::SetCurSel(int sel, bool sendUpdate)
{
    if (!sendUpdate && sel == _selString)
    {
        saturateMin(_topString, GetSize() - _showStrings);
        saturateMax(_topString, 0);
        return;
    }

    if (GetSize() == 0)
    {
        _selString = -1;
    }
    else if (sel < 0)
    {
        _selString = 0;
    }
    else if (sel >= GetSize())
    {
        _selString = GetSize() - 1;
    }
    else
    {
        _selString = sel;
    }

    if (_selString < _topString)
    {
        _topString = _selString;
        saturateMin(_topString, GetSize() - _showStrings);
        saturateMax(_topString, 0);
    }
    else if (_selString > _topString + _showStrings - 1)
    {
        _topString = _selString - _showStrings + 1;
    }

    if (sendUpdate)
    {
        OnSelChanged(_selString);
        _parent->OnLBSelChanged(IDC(), _selString);
    }
    Check();
}

bool CListBox::OnKillFocus()
{
    return Control::OnKillFocus();
}

bool CListBox::OnKeyDown(unsigned nChar, unsigned nRepCnt, unsigned nFlags)
{
    if (IsReadOnly())
    {
        return false;
    }

    switch (nChar)
    {
        case SDLK_UP:
            SetCurSel(GetCurSel() - 1);
            if (GetCurSel() < _topString)
            {
                _topString = GetCurSel();
            }
            return true;
        case SDLK_DOWN:
            SetCurSel(GetCurSel() + 1);
            if (GetCurSel() > _topString + _showStrings - 1)
            {
                _topString = GetCurSel() - _showStrings + 1;
            }
            return true;
    }
    return false;
}

void CListBox::OnMouseMove(float x, float y, bool active)
{
    //	if (IsReadOnly()) return;

    if (!InputSubsystem::Instance().IsMouseLeftDown())
    {
        return;
    }

    OnMouseHold(x, y, active);
}

void CListBox::OnMouseHold(float x, float y, bool active)
{
    //	if (IsReadOnly()) return;

    if (!InputSubsystem::Instance().IsMouseLeftDown())
    {
        return;
    }

    if (_scrollbar.IsEnabled() && _scrollbar.IsLocked())
    {
        _scrollbar.SetPos(_topString);
        _scrollbar.OnMouseHold(x, y);
        _topString = _scrollbar.GetPos();
    }
    else if (_scrollbar.IsEnabled() && _scrollbar.IsInside(x, y))
    {
    }
    else if (!IsReadOnly())
    {
        float index = (y - _y) / _rowHeight;
        {
            _scrollUp = _scrollDown = false;
            if (index >= 0 && index < _showStrings)
            {
                SetCurSel(toIntFloor(_topString + index));
            }
        }
    }
}

void CListBox::OnMouseZChanged(float dz)
{
    //	if (IsReadOnly()) return;

    CListBoxContainer::OnMouseZChanged(dz);
}

void CListBox::OnLButtonDown(float x, float y)
{
    //	if (IsReadOnly()) return;

    if (_scrollbar.IsEnabled() && _scrollbar.IsInside(x, y))
    {
        _scrollbar.SetPos(_topString);
        _scrollbar.OnLButtonDown(x, y);
        _topString = _scrollbar.GetPos();
    }
}

void CListBox::OnLButtonUp(float x, float y)
{
    //	if (IsReadOnly()) return;

    if (_scrollbar.IsLocked())
    {
        _scrollbar.OnLButtonUp(x, y);
    }
}

void CListBox::OnLButtonDblClick(float x, float y)
{
    if (IsReadOnly())
    {
        return;
    }

    if (_selString < 0)
    {
        return;
    }
    _parent->OnLBDblClick(IDC(), _selString);
}

void CListBox::OnDraw(float alpha)
{
    const int w = GLOB_ENGINE->Width2D();
    const int h = GLOB_ENGINE->Height2D();

    float xx = toInt(_x * w) + 0.5;
    float yy = toInt(_y * h) + 0.5;
    float ww = toInt(_w * w);
    float hh = toInt(_h * h);

    PackedColor ftColor = ModAlpha(_ftColor, alpha);
    PackedColor selColor = ModAlpha(_selColor, alpha);

    // draw list
    DrawRect(xx, yy, xx + ww, yy + hh, ftColor);

    // draw scrollbar
    Rect2DFloat rect(_x, _y, _w, _h);
    if (GetSize() > _showStrings)
    {
        _scrollbar.Enable(true);
        _scrollbar.SetPosition((_x + _w - sbWidth) * _scale, _y * _scale, sbWidth * _scale, _h * _scale);
        _scrollbar.SetRange(0, GetSize(), _showStrings);
        _scrollbar.SetPos(_topString);
        _scrollbar.OnDraw(alpha);
        rect.w -= sbWidth;
    }
    else
    {
        _scrollbar.Enable(false);
    }
    int i = toIntFloor(_topString);
    float top = _y - (_topString - i) * _rowHeight;
    bool canSelect = IsEnabled() /* && !IsReadOnly()*/;
    while (top < _y + _h && i < GetSize())
    {
        DrawItem(alpha, i, i == GetCurSel() && canSelect, top, rect);
        top += _rowHeight;
        i++;
    }
}

void CListBox::DrawItem(float alpha, int i, bool selected, float top, Rect2DFloat& rect)
{
    PackedColor ftColor = ModAlpha(GetFtColor(i), alpha);
    PackedColor selColor = ModAlpha(GetSelColor(i), alpha);
    PackedColor color;

    const int w = GLOB_ENGINE->Width2D();
    const int h = GLOB_ENGINE->Height2D();
    const float border = 0.005;

    if (selected && _showSelected)
    {
        MipInfo mip = GLOB_ENGINE->TextBank()->UseMipmap(nullptr, 0, 0);
        GLOB_ENGINE->Draw2D(mip, _hasSelBg ? ModAlpha(_selBgColor, alpha) : ftColor,
                            Rect2DPixel(rect.x * w, top * h, rect.w * w, _rowHeight * h),
                            Rect2DPixel(rect.x * w, rect.y * h, rect.w * w, rect.h * h));
        color = selColor;
    }
    else
    {
        color = ftColor;
    }
    float left = _x + border;

    Texture* texture = GetTexture(i);
    if (texture)
    {
        MipInfo mip = GLOB_ENGINE->TextBank()->UseMipmap(texture, 0, 0);
        float width = _rowHeight * (texture->AWidth() * h) / (texture->AHeight() * w);
        GLOB_ENGINE->Draw2D(mip, PackedWhite,
                            Rect2DPixel(rect.x * w, toInt(top * h) + 0.5, toInt(width * w), toInt(_rowHeight * h)),
                            Rect2DPixel(rect.x * w, rect.y * h, rect.w * w, rect.h * h));
        left += width;
    }

    const char* text = GetText(i);
    if (text)
    {
        float t = top + 0.5 * (_rowHeight - _size);
        GLOB_ENGINE->DrawText(Point2DFloat(left, t), _size, rect, _font, color, text);
    }
}

void CListBox::OnSelChanged(int curSel) {}

CCombo::CCombo(ControlsContainer* parent, int idc, const ParamEntry& cls)
    : Control(parent, CT_COMBO, idc, cls), CListBoxContainer(cls)
{
    _mouseSel = _selString;
    _wholeHeight = cls >> "wholeHeight";
    _bgColor = GetPackedColor(cls >> "colorBackground");
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

    _expanded = false;
    _scrollUp = false;
    _scrollDown = false;

    _showStrings = (_wholeHeight - _h) / (_size);
    if (_showStrings < 0)
    {
        _showStrings = 0;
    }

    _scrollbar.SetPosition((_x + _w - sbWidth) * _scale, (_y + _h) * _scale, sbWidth * _scale,
                           (_wholeHeight - _h) * _scale);
    _scrollbar.SetColor(_ftColor);
    _scrollbar.SetRange(0, 0, _showStrings);
    _scrollbar.SetPos(0);
    _scrollbar.SetSpeed(1, floatMax(_showStrings - 1, 1));
}

void CCombo::SetPos(float x, float y, float w, float h)
{
    Control::SetPos(x, y, w, h);
    // update _showStrings
    _showStrings = (_wholeHeight - _h) / (_size);
    saturateMax(_showStrings, 0);
    _scrollbar.SetSpeed(1, floatMax(_showStrings - 1, 1));
}

const float border = 0.005;

bool CCombo::IsInsideExt(float x, float y)
{
    if (!_expanded)
    {
        return false;
    }

    float height = floatMin(_wholeHeight - _h, GetSize() * _size) + _h;

    float width = 0;
    for (int i = 0; i < GetSize(); i++)
    {
        const char* text = GetText(i);
        if (text)
        {
            saturateMax(width, GLOB_ENGINE->GetTextWidth(_size, _font, text));
        }
    }
    width += 2 * border;
    if (GetSize() > _showStrings)
    {
        width += sbWidth;
    }
    saturateMax(width, _w);
    return x >= _x && x < _x + width && y >= _y + _h && y < _y + height;
}

bool CCombo::IsInside(float x, float y)
{
    /*
        return Control::IsInside(x, y) || IsInsideExt(x, y);
    */
    if (_expanded)
    {
        return true;
    }
    else
    {
        return Control::IsInside(x, y);
    }
}

void CCombo::SetTopString()
{
    if (_mouseSel < _topString)
    {
        _topString = std::max(_mouseSel, 0);
    }
    else if (_mouseSel > _topString + _showStrings - 1)
    {
        _topString = _mouseSel - _showStrings + 1;
    }
}

void CCombo::SetCurSel(int sel, bool sendUpdate)
{
    if (GetSize() == 0)
    {
        _selString = -1;
    }
    else if (sel < 0)
    {
        _selString = 0;
    }
    else if (sel >= GetSize())
    {
        _selString = GetSize() - 1;
    }
    else
    {
        _selString = sel;
    }

    _mouseSel = _selString;

    SetTopString();

    if (sendUpdate)
    {
        OnSelChanged(_selString);
        _parent->OnComboSelChanged(IDC(), _selString);
    }
    Check();
}

bool CCombo::OnKillFocus()
{
    _expanded = false;
    return Control::OnKillFocus();
}

bool CCombo::OnKeyDown(unsigned nChar, unsigned nRepCnt, unsigned nFlags)
{
    if (!_expanded && _showStrings > 0 && nChar == SDLK_SPACE)
    {
        _expanded = true;
        SetTopString();
        return true;
    }
    if (_expanded && !InputSubsystem::Instance().IsMouseLeftDown())
    {
        switch (nChar)
        {
            case SDLK_SPACE:
                _expanded = false;
                return true;
            case SDLK_UP:
                SetCurSel(GetCurSel() - 1);
                return true;
            case SDLK_DOWN:
                SetCurSel(GetCurSel() + 1);
                return true;
        }
    }
    return false;
}

void CCombo::OnLButtonDown(float x, float y)
{
    if (_expanded && _scrollbar.IsEnabled() && _scrollbar.IsInside(x, y))
    {
        _scrollbar.SetPos(_topString);
        _scrollbar.OnLButtonDown(x, y);
        _topString = _scrollbar.GetPos();
    }
    else if (_showStrings > 0 && !IsInsideExt(x, y))
    {
        _expanded = !_expanded;
        SetTopString();
    }
}

void CCombo::OnLButtonUp(float x, float y)
{
    if (_scrollbar.IsLocked())
    {
        _scrollbar.OnLButtonUp(x, y);
    }
    else if (_scrollbar.IsEnabled() && _scrollbar.IsInside(x, y))
    {
    }
    else
    {
        if (_expanded && IsInsideExt(x, y))
        {
            float index = (y - (_y + _h)) / (_size);
            if (index >= 0 && index < _showStrings)
            {
                SetCurSel(toIntFloor(_topString + index));
            }
        }
        if (!Control::IsInside(x, y))
        {
            _expanded = false;
        }
    }
}

void CCombo::OnMouseMove(float x, float y, bool active)
{
    OnMouseHold(x, y, active);
}

void CCombo::OnMouseHold(float x, float y, bool active)
{
    if (_expanded)
    {
        if (_scrollbar.IsEnabled() && _scrollbar.IsLocked())
        {
            _scrollUp = _scrollDown = false;
            _scrollbar.SetPos(_topString);
            _scrollbar.OnMouseHold(x, y);
            _topString = _scrollbar.GetPos();
        }
        else if (_scrollbar.IsEnabled() && _scrollbar.IsInside(x, y))
        {
            _scrollUp = _scrollDown = false;
        }
        else
        {
            float dy;
            if (InputSubsystem::Instance().IsMouseLeftDown() && _topString > 0 && (dy = _y + _h - y) > 0)
            {
                _scrollDown = false;
                // scroll up
                if (_scrollUp)
                {
                    float scroll = SCROLL_SPEED * dy;
                    saturate(scroll, SCROLL_MIN, SCROLL_MAX);
                    _topString -= scroll * (Glob.uiTime - _scrollTime);
                    saturateMax(_topString, 0);
                    _scrollTime = Glob.uiTime;
                }
                else
                {
                    _scrollUp = true;
                    _scrollTime = Glob.uiTime;
                }
            }
            else if (InputSubsystem::Instance().IsMouseLeftDown() && _topString + _showStrings < GetSize() &&
                     (dy = y - (_y + _wholeHeight)) > 0)
            {
                _scrollUp = false;
                // scroll down
                if (_scrollDown)
                {
                    float scroll = SCROLL_SPEED * dy;
                    saturate(scroll, SCROLL_MIN, SCROLL_MAX);
                    _topString += scroll * (Glob.uiTime - _scrollTime);
                    saturateMin(_topString, GetSize() - _showStrings);
                    _scrollTime = Glob.uiTime;
                }
                else
                {
                    _scrollDown = true;
                    _scrollTime = Glob.uiTime;
                }
            }
            else
            {
                _scrollUp = _scrollDown = false;
                if (IsInsideExt(x, y))
                {
                    float index = (y - (_y + _h)) / (_size);
                    if (index >= 0 && index < _showStrings)
                    {
                        _mouseSel = toIntFloor(_topString + index);
                        if (GetSize() == 0)
                        {
                            _mouseSel = -1;
                        }
                        else
                        {
                            saturate(_mouseSel, 0, GetSize() - 1);
                        }
                    }
                }
            }
        }
    }
    else
    {
        _scrollUp = _scrollDown = false;
    }
}

void CCombo::OnMouseZChanged(float dz)
{
    if (dz == 0 || !_expanded)
    {
        return;
    }

    if (_topString > 0 && dz > 0)
    {
        // scroll up
        float scroll = -SCROLL_SPEED * dz;
        saturate(scroll, -SCROLL_MAX, -SCROLL_MIN);
        _topString += 0.1 * scroll;
        saturateMax(_topString, 0);
    }
    else if (_topString + _showStrings < GetSize() && dz < 0)
    {
        // scroll down
        float scroll = -SCROLL_SPEED * dz;
        saturate(scroll, SCROLL_MIN, SCROLL_MAX);
        _topString += 0.1 * scroll;
        saturateMin(_topString, GetSize() - _showStrings);
    }
}

void CCombo::OnDraw(float alpha)
{
    const int w = GLOB_ENGINE->Width2D();
    const int h = GLOB_ENGINE->Height2D();

    float xx = toInt(_x * w) + 0.5;
    float yy = toInt(_y * h) + 0.5;
    float ww = toInt(_w * w);
    float hh = toInt(_h * h);

    PackedColor ftColor = ModAlpha(_ftColor, alpha);
    PackedColor bgColor = ModAlpha(_bgColor, alpha);
    PackedColor selColor = ModAlpha(_selColor, alpha);

    PackedColor color;
    Texture* texture = GLOB_SCENE->Preloaded(TextureWhite);

    // draw bar
    DrawLeft(0, ftColor);
    DrawTop(0, ftColor);
    DrawBottom(0, ftColor);
    DrawRight(0, ftColor);

    if (IsFocused())
    {
        MipInfo mip = GLOB_ENGINE->TextBank()->UseMipmap(texture, 0, 0);
        GLOB_ENGINE->Draw2D(mip, ftColor, Rect2DPixel(xx, yy, ww, hh));
        color = selColor;
    }
    else
    {
        color = ftColor;
    }
    const char* text = GetText(GetCurSel());
    if (text)
    {
        float top = _y + 0.5 * (_h - _size);
        GLOB_ENGINE->DrawText(Point2DFloat(_x + border, top), _size, Rect2DFloat(_x, _y, _w, _h), _font, color, text);
    }

    // draw list
    if (_expanded)
    {
        float height = _size;
        float top = _y + _h;
        float curHeight = floatMin(_wholeHeight - _h, GetSize() * height);

        yy = toInt(top * h) + 0.5;
        hh = toInt(curHeight * h);

        float w2 = 0;
        for (int i = 0; i < GetSize(); i++)
        {
            const char* text = GetText(i);
            if (text)
            {
                saturateMax(w2, GLOB_ENGINE->GetTextWidth(_size, _font, text));
            }
        }
        w2 += 2 * border;
        float w2SB = w2;
        if (GetSize() > _showStrings)
        {
            w2 += sbWidth;
            saturateMax(w2, _w);
            w2SB = w2 - sbWidth;
            _scrollbar.Enable(true);
            _scrollbar.SetPosition((_x + w2SB) * _scale, top * _scale, sbWidth * _scale, curHeight * _scale);
            _scrollbar.SetRange(0, GetSize(), _showStrings);
            _scrollbar.SetPos(_topString);
        }
        else
        {
            saturateMax(w2, _w);
            w2SB = w2;
            _scrollbar.Enable(false);
        }
        ww = toInt(w2 * w);
        float wwSB = toInt(w2SB * w);

        MipInfo mip = GLOB_ENGINE->TextBank()->UseMipmap(texture, 0, 0);
        GLOB_ENGINE->Draw2D(mip, bgColor, Rect2DPixel(xx, yy, ww, hh));

        DrawLeft(0, ftColor);
        DrawTop(0, ftColor);
        DrawBottom(0, ftColor);
        DrawRight(0, ftColor);

        if (GetSize() > _showStrings)
        {
            _scrollbar.OnDraw(alpha);
        }

        float topClip = top;
        float bottomClip = _y + _h + curHeight;

        int i = toIntFloor(_topString);

        top = topClip - (_topString - i) * height;
        while (top < bottomClip && i < GetSize())
        {
            if (i == _mouseSel)
            {
                float realTop = floatMax(top, topClip);
                float realHeight = floatMin(top + height, bottomClip) - realTop;
                saturateMin(realHeight, height);
                MipInfo mip = GLOB_ENGINE->TextBank()->UseMipmap(texture, 0, 0);
                GLOB_ENGINE->Draw2D(mip, ftColor, Rect2DPixel(xx, realTop * h, wwSB, realHeight * h));
                color = selColor;
            }
            else
            {
                color = ftColor;
            }
            const char* text = GetText(i);
            if (text)
            {
                GLOB_ENGINE->DrawText(Point2DFloat(_x + border, top), _size,
                                      Rect2DFloat(_x, topClip, w2, bottomClip - topClip), _font, color, text);
            }
            top += height;
            i++;
        }
    }
    else
    {
        _scrollbar.Enable(false);
    }
}

void CCombo::OnSelChanged(int curSel) {}
