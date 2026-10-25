#include <Poseidon/UI/Arma3Menu.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>

#include <Poseidon/World/World.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <SDL3/SDL_scancode.h>
#include <map>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>

using namespace Poseidon;
namespace Poseidon
{
RString FindPicture(RString name);
}

static const ParamEntry* FindResource(RString name)
{
    // find in mission
    const ParamEntry* entry = ExtParsMission.FindEntry(name);
    if (entry)
    {
        return entry;
    }

    // find in campaign
    entry = ExtParsCampaign.FindEntry(name);
    if (entry)
    {
        return entry;
    }

    // find in resource
    entry = Res.FindEntry(name);
    if (entry)
    {
        return entry;
    }

    Foundation::WarningMessage("Resource %s not found", (const char*)name);
    return nullptr;
}

static ControlsContainer* GetUserDialog()
{
    ControlsContainer* dlg = dynamic_cast<ControlsContainer*>(GWorld->UserDialog());
    if (dlg)
    {
        while (dlg->Child())
        {
            dlg = dlg->Child();
        }
    }
    return dlg;
}

static IControl* GetCtrl(const GameState* state, GameValuePar oper1)
{
    int idc = toInt((float)oper1);
    ControlsContainer* dlg = GetUserDialog();
    if (!dlg)
    {
        return nullptr;
    }
    IControl* ctrl = dlg->GetCtrl(idc);
    return ctrl;
}

class UserDisplay : public Display
{
  protected:
    // partial exit codes: Exit fires only once both halves (key + button) match
    int _exitKey;
    int _exitVK;

  public:
    UserDisplay(ControlsContainer* parent) : Display(parent)
    {
        InputSubsystem::Instance().ChangeGameFocus(+1);
        _exitKey = -1;
        _exitVK = -1;
    }
    ~UserDisplay() override { InputSubsystem::Instance().ChangeGameFocus(-1); }
    void DestroyHUD(int exit) override;

    void OnButtonClicked(int idc) override;
    void OnSimulate(EntityAI* vehicle) override;
};

// Arma 1 "Unload" handlers (displaySetEventHandler), by IDD.
static std::map<int, RString>& A1UnloadHandlers()
{
    static std::map<int, RString> handlers;
    return handlers;
}

void UserDisplay::DestroyHUD(int exit)
{
    RString unload;
    if (const auto it = A1UnloadHandlers().find(IDD()); it != A1UnloadHandlers().end())
    {
        unload = it->second;
        A1UnloadHandlers().erase(it);
    }
    GWorld->DestroyUserDialog();
    if (unload.GetLength() > 0)
        GWorld->GetGameState()->Execute(unload);
}

void UserDisplay::OnButtonClicked(int idc)
{
    switch (idc)
    {
        case IDC_CANCEL:
            _exitVK = idc;
            if (_exitKey == idc)
            {
                Exit(idc);
            }
            break;
        default:
            Display::OnButtonClicked(idc);
            break;
    }
}

void UserDisplay::OnSimulate(EntityAI* vehicle)
{
    // GetKeysToDo always returns false in this context; poll the key directly
    if (InputSubsystem::Instance().ConsumeKeyPress(SDL_SCANCODE_ESCAPE))
    {
        _exitKey = IDC_CANCEL;
        if (_exitVK == IDC_CANCEL)
        {
            Exit(IDC_CANCEL);
        }
        return;
    }
    Display::OnSimulate(vehicle);
}

AbstractOptionsUI* CreateUserDialog(RString name)
{
    Display* dlg = new UserDisplay(nullptr);
    dlg->Load(name);
    return dlg;
}

GameValue DialogCreate(const GameState* state, GameValuePar oper1)
{
    GameStringType name = oper1;
    const ParamEntry* cls = FindResource(name);
    if (!cls)
    {
        return false;
    }

    ControlsContainer* parent = GetUserDialog();

    Display* dlg = nullptr;
    if (parent)
    {
        dlg = new Display(parent);
    }
    else
    {
        dlg = new UserDisplay(parent);
    }
    dlg->Load(*cls);
    if (parent)
    {
        parent->CreateChild(dlg);
    }
    else
    {
        GWorld->SetUserDialog(dlg);
    }
    return true;
}

GameValue DialogClose(const GameState* state, GameValuePar oper1)
{
    ControlsContainer* dlg = GetUserDialog();
    if (dlg)
    {
        int idc = toInt((float)oper1);
        dlg->Exit(idc);
    }
    return NOTHING;
}

GameValue IsDialog(const GameState* state)
{
    ControlsContainer* dlg = GetUserDialog();
    return dlg != nullptr;
}

GameValue CtrlVisible(const GameState* state, GameValuePar oper1)
{
    IControl* ctrl = GetCtrl(state, oper1);
    if (!ctrl)
    {
        return false;
    }
    return ctrl->IsVisible();
}

GameValue CtrlEnabled(const GameState* state, GameValuePar oper1)
{
    IControl* ctrl = GetCtrl(state, oper1);
    if (!ctrl)
    {
        return false;
    }
    return ctrl->IsEnabled();
}

GameValue CtrlShow(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameBool)
    {
        return NOTHING;
    }

    IControl* ctrl = GetCtrl(state, array[0]);
    if (!ctrl)
    {
        return NOTHING;
    }
    ctrl->ShowCtrl(array[1]);
    return NOTHING;
}

GameValue CtrlEnable(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameBool)
    {
        return NOTHING;
    }

    IControl* ctrl = GetCtrl(state, array[0]);
    if (!ctrl)
    {
        return NOTHING;
    }
    ctrl->EnableCtrl(array[1]);
    return NOTHING;
}

// Arma 3's controls made by script (ctrlCreate) are numbers of their own (Arma3Menu::Script); a
// control command given one acts on it, any other number is a dialog control's idc as before.
namespace A3 = Poseidon::Arma3Menu::Script;
static bool IsA3(GameValuePar control)
{
    return control.GetType() == GameScalar && A3::IsControl(static_cast<float>(control));
}
static int A3Handle(GameValuePar control)
{
    return toInt(static_cast<float>(control));
}
static bool Color4(GameValuePar value, float rgba[4])
{
    if (value.GetType() != GameArray)
        return false;
    const GameArrayType& a = value;
    if (a.Size() < 4)
        return false;
    for (int i = 0; i < 4; i++)
        rgba[i] = a[i].GetType() == GameScalar ? static_cast<float>(a[i]) : 1.0f;
    return true;
}

GameValue CtrlGetText(const GameState* state, GameValuePar oper1)
{
    if (IsA3(oper1))
        return GameValue(A3::Text(A3Handle(oper1)));
    IControl* ctrl = GetCtrl(state, oper1);
    if (!ctrl)
    {
        return "";
    }
    if (RString a3; Arma3Menu::ControlText(ctrl, a3))
        return a3;

    int type = ctrl->GetType();
    switch (type)
    {
        case CT_STATIC:
        {
            CStatic* s = dynamic_cast<CStatic*>(ctrl);
            return s ? s->GetText() : "";
        }
        case CT_BUTTON:
        {
            CButton* s = dynamic_cast<CButton*>(ctrl);
            return s ? s->GetText() : "";
        }
        case CT_EDIT:
        {
            CEdit* s = dynamic_cast<CEdit*>(ctrl);
            return s ? s->GetText() : "";
        }
        case CT_ACTIVETEXT:
        {
            CActiveText* s = dynamic_cast<CActiveText*>(ctrl);
            return s ? s->GetText() : "";
        }
        case CT_3DSTATIC:
        {
            C3DStatic* s = dynamic_cast<C3DStatic*>(ctrl);
            return s ? s->GetText() : "";
        }
        case CT_3DACTIVETEXT:
        {
            C3DActiveText* s = dynamic_cast<C3DActiveText*>(ctrl);
            return s ? s->GetText() : "";
        }
        case CT_3DEDIT:
        {
            C3DEdit* s = dynamic_cast<C3DEdit*>(ctrl);
            return s ? s->GetText() : "";
        }
        default:
            return "";
    }
}

GameValue CtrlSetText(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameString)
    {
        return NOTHING;
    }

    IControl* ctrl = GetCtrl(state, array[0]);
    if (!ctrl)
    {
        return NOTHING;
    }

    RString text = array[1];
    if (Arma3Menu::SetControlText(ctrl, text))
        return NOTHING;

    int type = ctrl->GetType();
    switch (type)
    {
        case CT_STATIC:
        {
            CStatic* s = dynamic_cast<CStatic*>(ctrl);
            if (s)
                s->SetText(text);
            break;
        }
        case CT_BUTTON:
        {
            CButton* s = dynamic_cast<CButton*>(ctrl);
            if (s)
                s->SetText(text);
            break;
        }
        case CT_EDIT:
        {
            CEdit* s = dynamic_cast<CEdit*>(ctrl);
            if (s)
                s->SetText(text);
            break;
        }
        case CT_ACTIVETEXT:
        {
            CActiveText* s = dynamic_cast<CActiveText*>(ctrl);
            if (s)
                s->SetText(text);
            break;
        }
        case CT_3DSTATIC:
        {
            C3DStatic* s = dynamic_cast<C3DStatic*>(ctrl);
            if (s)
                s->SetText(text);
            break;
        }
        case CT_3DACTIVETEXT:
        {
            C3DActiveText* s = dynamic_cast<C3DActiveText*>(ctrl);
            if (s)
                s->SetText(text);
            break;
        }
        case CT_3DEDIT:
        {
            C3DEdit* s = dynamic_cast<C3DEdit*>(ctrl);
            if (s)
                s->SetText(text);
            break;
        }
        default:
            break;
    }
    return NOTHING;
}

GameValue ButtonGetAction(const GameState* state, GameValuePar oper1)
{
    IControl* ctrl = GetCtrl(state, oper1);
    if (!ctrl)
    {
        return "";
    }

    int type = ctrl->GetType();
    switch (type)
    {
        case CT_BUTTON:
        {
            CButton* s = dynamic_cast<CButton*>(ctrl);
            return s ? s->GetAction() : "";
        }
        case CT_ACTIVETEXT:
        {
            CActiveText* s = dynamic_cast<CActiveText*>(ctrl);
            return s ? s->GetAction() : "";
        }
        case CT_3DACTIVETEXT:
        {
            C3DActiveText* s = dynamic_cast<C3DActiveText*>(ctrl);
            return s ? s->GetAction() : "";
        }
        default:
            return "";
    }
}

GameValue ButtonSetAction(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameString)
    {
        return NOTHING;
    }

    IControl* ctrl = GetCtrl(state, array[0]);
    if (!ctrl)
    {
        return NOTHING;
    }

    RString text = array[1];

    int type = ctrl->GetType();
    switch (type)
    {
        case CT_BUTTON:
        {
            CButton* s = dynamic_cast<CButton*>(ctrl);
            if (s)
                s->SetAction(text);
            break;
        }
        case CT_ACTIVETEXT:
        {
            CActiveText* s = dynamic_cast<CActiveText*>(ctrl);
            if (s)
                s->SetAction(text);
            break;
        }
        case CT_3DACTIVETEXT:
        {
            C3DActiveText* s = dynamic_cast<C3DActiveText*>(ctrl);
            if (s)
                s->SetAction(text);
            break;
        }
        default:
            break;
    }
    return NOTHING;
}

static CListBoxContainer* GetListbox(const GameState* state, GameValuePar oper1)
{
    IControl* ctrl = GetCtrl(state, oper1);
    if (!ctrl)
    {
        return nullptr;
    }

    int type = ctrl->GetType();
    switch (type)
    {
        case CT_COMBO:
        {
            CCombo* lb = dynamic_cast<CCombo*>(ctrl);
            return lb;
        }
        case CT_LISTBOX:
        {
            CListBox* lb = dynamic_cast<CListBox*>(ctrl);
            return lb;
        }
        case CT_3DLISTBOX:
        {
            C3DListBox* lb = dynamic_cast<C3DListBox*>(ctrl);
            return lb;
        }
        default:
            return nullptr;
    }
}

GameValue LBGetSize(const GameState* state, GameValuePar oper1)
{
    CListBoxContainer* lb = GetListbox(state, oper1);
    if (!lb)
    {
        return 0.0f;
    }

    return (float)lb->GetSize();
}

GameValue LBGetCurSel(const GameState* state, GameValuePar oper1)
{
    CListBoxContainer* lb = GetListbox(state, oper1);
    if (!lb)
    {
        return -1.0f;
    }

    return (float)lb->GetCurSel();
}

GameValue LBSetCurSel(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar)
    {
        return NOTHING;
    }

    IControl* ctrl = GetCtrl(state, array[0]);
    if (!ctrl)
    {
        return NOTHING;
    }

    int sel = toInt((float)array[1]);
    int type = ctrl->GetType();
    switch (type)
    {
        case CT_COMBO:
        {
            CCombo* lb = dynamic_cast<CCombo*>(ctrl);
            if (lb)
                lb->SetCurSel(sel);
            break;
        }
        case CT_LISTBOX:
        {
            CListBox* lb = dynamic_cast<CListBox*>(ctrl);
            if (lb)
                lb->SetCurSel(sel);
            break;
        }
        case CT_3DLISTBOX:
        {
            C3DListBox* lb = dynamic_cast<C3DListBox*>(ctrl);
            if (lb)
                lb->SetCurSel(sel);
            break;
        }
    }
    return NOTHING;
}

GameValue LBClear(const GameState* state, GameValuePar oper1)
{
    CListBoxContainer* lb = GetListbox(state, oper1);
    if (!lb)
    {
        return NOTHING;
    }

    lb->ClearStrings();
    return NOTHING;
}

GameValue LBAdd(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameString)
    {
        return 0.0f;
    }

    CListBoxContainer* lb = GetListbox(state, array[0]);
    if (!lb)
    {
        return 0.0f;
    }

    RString text = array[1];
    return (float)lb->AddString(text);
}

GameValue LBDelete(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar)
    {
        return NOTHING;
    }

    CListBoxContainer* lb = GetListbox(state, array[0]);
    if (!lb)
    {
        return NOTHING;
    }

    int index = toInt((float)array[1]);
    lb->DeleteString(index);
    return NOTHING;
}

GameValue LBGetText(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar)
    {
        return "";
    }

    CListBoxContainer* lb = GetListbox(state, array[0]);
    if (!lb)
    {
        return "";
    }

    int index = toInt((float)array[1]);
    return lb->GetText(index);
}

GameValue LBGetData(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar)
    {
        return "";
    }

    CListBoxContainer* lb = GetListbox(state, array[0]);
    if (!lb)
    {
        return "";
    }

    int index = toInt((float)array[1]);
    return lb->GetData(index);
}

GameValue LBSetData(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 3 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar ||
        array[2].GetType() != GameString)
    {
        return NOTHING;
    }

    CListBoxContainer* lb = GetListbox(state, array[0]);
    if (!lb)
    {
        return NOTHING;
    }

    int index = toInt((float)array[1]);
    RString text = array[2];
    lb->SetData(index, text);
    return NOTHING;
}

GameValue LBGetValue(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar)
    {
        return 0.0f;
    }

    CListBoxContainer* lb = GetListbox(state, array[0]);
    if (!lb)
    {
        return 0.0f;
    }

    int index = toInt((float)array[1]);
    return (float)lb->GetValue(index);
}

GameValue LBSetValue(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 3 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar ||
        array[2].GetType() != GameScalar)
    {
        return NOTHING;
    }

    CListBoxContainer* lb = GetListbox(state, array[0]);
    if (!lb)
    {
        return NOTHING;
    }

    int index = toInt((float)array[1]);
    int value = toInt((float)array[2]);
    lb->SetValue(index, value);
    return NOTHING;
}

GameValue LBGetPicture(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar)
    {
        return "";
    }

    CListBoxContainer* lb = GetListbox(state, array[0]);
    if (!lb)
    {
        return "";
    }

    int index = toInt((float)array[1]);
    Texture* texture = lb->GetTexture(index);
    return texture ? texture->GetName() : "";
}

GameValue LBSetPicture(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 3 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar ||
        array[2].GetType() != GameString)
    {
        return NOTHING;
    }

    CListBoxContainer* lb = GetListbox(state, array[0]);
    if (!lb)
    {
        return NOTHING;
    }

    int index = toInt((float)array[1]);
    RString name = Poseidon::FindPicture(array[2]);
    if (name.GetLength() > 0)
    {
        name.Lower();
        lb->SetTexture(index, GlobLoadTexture(name));
    }
    return NOTHING;
}

static bool ReadColor(PackedColor& color, GameValuePar value)
{
    const GameArrayType& array = value;
    if (array.Size() != 4 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar ||
        array[2].GetType() != GameScalar || array[3].GetType() != GameScalar)
    {
        return false;
    }

    color = PackedColor(Color(array[0], array[1], array[2], array[3]));
    return true;
}

static void WriteColor(GameArrayType& array, PackedColor color)
{
    const float coef = 1.0f / 255.0f;

    array.Clear();
    array.Add(coef * color.R8());
    array.Add(coef * color.G8());
    array.Add(coef * color.B8());
    array.Add(coef * color.A8());
}

GameValue LBGetColor(const GameState* state, GameValuePar oper1)
{
    GameValue value = state->CreateGameValue(GameArray);
    GameArrayType& result = value;

    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar)
    {
        return value;
    }

    CListBoxContainer* lb = GetListbox(state, array[0]);
    if (!lb)
    {
        return value;
    }

    int index = toInt((float)array[1]);
    PackedColor color = lb->GetFtColor(index);
    WriteColor(result, color);

    return value;
}

GameValue LBSetColor(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 3 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar ||
        array[2].GetType() != GameArray)
    {
        return NOTHING;
    }

    CListBoxContainer* lb = GetListbox(state, array[0]);
    if (!lb)
    {
        return NOTHING;
    }

    int index = toInt((float)array[1]);
    PackedColor color;
    if (!ReadColor(color, array[2]))
    {
        return NOTHING;
    }

    lb->SetFtColor(index, color);
    lb->SetSelColor(index, color);
    return NOTHING;
}

static CSliderContainer* GetSlider(const GameState* state, GameValuePar oper1)
{
    IControl* ctrl = GetCtrl(state, oper1);
    if (!ctrl)
    {
        return nullptr;
    }

    int type = ctrl->GetType();
    switch (type)
    {
        case CT_SLIDER:
        {
            CSlider* slider = dynamic_cast<CSlider*>(ctrl);
            return slider;
        }
        case CT_3DSLIDER:
        {
            C3DSlider* slider = dynamic_cast<C3DSlider*>(ctrl);
            return slider;
        }
        default:
            return nullptr;
    }
}

GameValue SliderGetPosition(const GameState* state, GameValuePar oper1)
{
    CSliderContainer* slider = GetSlider(state, oper1);
    if (!slider)
    {
        return 0.0f;
    }

    return slider->GetThumbPos();
}

GameValue SliderSetPosition(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 2 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar)
    {
        return NOTHING;
    }

    CSliderContainer* slider = GetSlider(state, array[0]);
    if (slider)
    {
        slider->SetThumbPos(array[1]);
    }
    return NOTHING;
}

GameValue SliderGetRange(const GameState* state, GameValuePar oper1)
{
    GameValue value = state->CreateGameValue(GameArray);
    GameArrayType& result = value;

    float minPos = 0, maxPos = 0;

    CSliderContainer* slider = GetSlider(state, oper1);
    if (slider)
    {
        slider->GetRange(minPos, maxPos);
    }

    result.Add(minPos);
    result.Add(maxPos);
    return result;
}

GameValue SliderSetRange(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 3 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar ||
        array[2].GetType() != GameScalar)
    {
        return NOTHING;
    }

    CSliderContainer* slider = GetSlider(state, array[0]);
    if (slider)
    {
        slider->SetRange(array[1], array[2]);
    }
    return NOTHING;
}

GameValue SliderGetSpeed(const GameState* state, GameValuePar oper1)
{
    GameValue value = state->CreateGameValue(GameArray);
    GameArrayType& result = value;

    float line = 0, page = 0;

    CSliderContainer* slider = GetSlider(state, oper1);
    if (slider)
    {
        slider->GetSpeed(line, page);
    }

    result.Add(line);
    result.Add(page);
    return result;
}

GameValue SliderSetSpeed(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (array.Size() != 3 || array[0].GetType() != GameScalar || array[1].GetType() != GameScalar ||
        array[2].GetType() != GameScalar)
    {
        return NOTHING;
    }

    CSliderContainer* slider = GetSlider(state, array[0]);
    if (slider)
    {
        slider->SetSpeed(array[1], array[2]);
    }
    return NOTHING;
}

// ---------------------------------------------------------------------------
// Arma 1 display/control commands. Arma 1 passes Display and Control values; here a
// display is its IDD and a control its IDC (a scalar), and the binary forms map onto the
// OFP IDC commands above, which act on the topmost user dialog.
// ---------------------------------------------------------------------------
namespace
{
GameValue Pair(GameValuePar a, GameValuePar b)
{
    GameArrayType args;
    args.Add(a);
    args.Add(b);
    return GameValue(args);
}

std::map<int, float>& PendingFade()
{
    static std::map<int, float> fade;
    return fade;
}
} // namespace

GameValue A1FindDisplay(const GameState* state, GameValuePar oper1)
{
    // The mission's own display (46 in Arma 2 and 3: RscDisplayMission) is there while a
    // mission runs. Arma 3 waits for it before anything else of its user interface
    // (BIS_fnc_displayMission: the showcases' title card and establishing shot, every
    // displayAddEventHandler "KeyDown"); as -1 it kept those scripts waiting for good.
    if (toInt((float)oper1) == 46 && GWorld && Poseidon::Arma1Compat::Generation() >= 2)
        return GameValue(46.0f);
    // The topmost user dialog stands for any IDD asked for; -1 (displayNull) when none
    // is open, which no control lookup matches.
    return GameValue(GetUserDialog() ? (float)oper1 : -1.0f);
}

GameValue A1DisplayCtrl(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (const int handle = A3::Find(toInt((float)oper1), toInt((float)oper2)); handle >= 0)
        return GameValue(static_cast<float>(handle));
    return GameValue((float)oper2);
}

GameValue A1DisplaySetEventHandler(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper2;
    if (array.Size() != 2 || array[0].GetType() != GameString || array[1].GetType() != GameString)
        return NOTHING;
    if (stricmp(RString(array[0]), "Unload") == 0)
        A1UnloadHandlers()[toInt((float)oper1)] = RString(array[1]);
    return NOTHING;
}

GameValue A1CtrlShow(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (IsA3(oper1))
    {
        A3::Show(A3Handle(oper1), oper2.GetType() == GameBool && (bool)oper2);
        return NOTHING;
    }
    return CtrlShow(state, Pair(oper1, oper2));
}

GameValue A1CtrlEnable(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return CtrlEnable(state, Pair(oper1, oper2));
}

GameValue A1CtrlSetText(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (IsA3(oper1))
    {
        A3::SetText(A3Handle(oper1), RString(oper2));
        return NOTHING;
    }
    return CtrlSetText(state, Pair(oper1, oper2));
}

GameValue A1ButtonSetAction(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return ButtonSetAction(state, Pair(oper1, oper2));
}

GameValue A1CtrlSetFade(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (IsA3(oper1))
    {
        A3::SetFade(A3Handle(oper1), (float)oper2);
        return NOTHING;
    }
    PendingFade()[toInt((float)oper1)] = (float)oper2;
    return NOTHING;
}

GameValue A1CtrlCommit(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (IsA3(oper1))
    {
        A3::Commit(A3Handle(oper1), (float)oper2);
        return NOTHING;
    }
    // No fading here: a committed fade shows or hides the control at once.
    const int idc = toInt((float)oper1);
    if (const auto it = PendingFade().find(idc); it != PendingFade().end())
    {
        if (IControl* ctrl = GetCtrl(state, oper1))
            ctrl->ShowCtrl(it->second < 0.99f);
        PendingFade().erase(it);
    }
    return NOTHING;
}

GameValue A1CtrlIgnored(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return NOTHING;
}

// control ctrlSetPosition [x, y, w, h] (or [x, y]): an Arma 3 script control's, committed later
GameValue A1CtrlSetPosition(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (!IsA3(oper1) || oper2.GetType() != GameArray)
        return NOTHING;
    const GameArrayType& a = oper2;
    float x, y, w, h;
    A3::Position(A3Handle(oper1), x, y, w, h);
    float v[4] = {x, y, w, h};
    for (int i = 0; i < 4 && i < a.Size(); i++)
        if (a[i].GetType() == GameScalar)
            v[i] = static_cast<float>(a[i]);
    A3::SetPosition(A3Handle(oper1), v[0], v[1], v[2], v[3]);
    return NOTHING;
}

GameValue A1CtrlSetTextColor(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    float c[4];
    if (IsA3(oper1) && Color4(oper2, c))
        A3::SetTextColor(A3Handle(oper1), c[0], c[1], c[2], c[3]);
    return NOTHING;
}

GameValue A1CtrlSetBackgroundColor(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    float c[4];
    if (IsA3(oper1) && Color4(oper2, c))
        A3::SetBackgroundColor(A3Handle(oper1), c[0], c[1], c[2], c[3]);
    return NOTHING;
}

GameValue A1CtrlCommitted(const GameState* state, GameValuePar oper1)
{
    if (IsA3(oper1))
        return GameValue(A3::Committed(A3Handle(oper1)));
    return GameValue(true);
}

// ctrlPosition control -> [x, y, w, h]
GameValue A1CtrlPosition(const GameState* state, GameValuePar oper1)
{
    GameArrayType out;
    if (float x, y, w, h; IsA3(oper1) && A3::Position(A3Handle(oper1), x, y, w, h))
    {
        for (const float v : {x, y, w, h})
            out.Add(GameValue(v));
        return GameValue(out);
    }
    Control* ctrl = dynamic_cast<Control*>(GetCtrl(state, oper1));
    out.Add(GameValue(ctrl ? ctrl->X() : 0.0f));
    out.Add(GameValue(ctrl ? ctrl->Y() : 0.0f));
    out.Add(GameValue(ctrl ? ctrl->W() : 0.0f));
    out.Add(GameValue(ctrl ? ctrl->H() : 0.0f));
    return GameValue(out);
}

// control ctrlSetStructuredText text: structured text is plain text here (already untagged)
GameValue A1CtrlSetStructuredText(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (IsA3(oper1))
    {
        A3::SetText(A3Handle(oper1), RString(oper2));
        return NOTHING;
    }
    return CtrlSetText(state, Pair(oper1, oper2));
}

// ctrlParent control -> its display (the topmost user dialog stands for any)
GameValue A1CtrlParent(const GameState* state, GameValuePar oper1)
{
    if (IsA3(oper1))
        return GameValue(static_cast<float>(A3::DisplayOf(A3Handle(oper1))));
    return GameValue(GetUserDialog() ? 0.0f : -1.0f);
}
