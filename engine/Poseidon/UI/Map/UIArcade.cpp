#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/UI/Map/UIMap.hpp>
#include <Poseidon/Foundation/Common/Win.h>

#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Foundation/Algorithms/Qsort.hpp>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <string>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/RStringArray.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>

namespace Poseidon
{

// #include "landscape.hpp"
} // namespace Poseidon
#include <Random/randomGen.hpp>
#include <Poseidon/World/Scene/Camera/CamEffects.hpp>
#include <Poseidon/Game/TitEffects.hpp>

#include <Evaluator/express.hpp>
#include <Poseidon/World/Entities/Vehicles/House.hpp>

#include <Poseidon/UI/Locale/StringtableExt.hpp>
namespace Poseidon
{

// Arcade Map subdisplays

void CStaticAzimut::OnDraw(float alpha)
{
    CStatic::OnDraw(alpha);

    CEdit* edit = dynamic_cast<CEdit*>(_parent->GetCtrl(_idcEdit));
    PoseidonAssert(edit);
    if (!edit)
    {
        return;
    }
    float azimut = edit->GetText() ? atof(edit->GetText()) : 0;
    azimut = HDegree(azimut);

    PackedColor color(Color(0.08, 0.08, 0.12, alpha));
    float xc = _x + 0.5 * _w;
    float zc = _y + 0.5 * _h;
    const float r1 = 0.32;
    const float r2 = 0.26;
    const float diff = 0.08;
    float x1 = xc + sin(azimut) * r1 * _w;
    float z1 = zc - cos(azimut) * r1 * _h;
    float x2 = xc + sin(azimut - diff) * r2 * _w;
    float z2 = zc - cos(azimut - diff) * r2 * _h;
    float x3 = xc + sin(azimut + diff) * r2 * _w;
    float z3 = zc - cos(azimut + diff) * r2 * _h;

    float w = GLOB_ENGINE->Width2D();
    float h = GLOB_ENGINE->Height2D();

    int xx1 = toIntFloor(x1 * w + 0.5);
    int zz1 = toIntFloor(z1 * h + 0.5);
    int xx2 = toIntFloor(x2 * w + 0.5);
    int zz2 = toIntFloor(z2 * h + 0.5);
    int xx3 = toIntFloor(x3 * w + 0.5);
    int zz3 = toIntFloor(z3 * h + 0.5);

    GLOB_ENGINE->DrawLine(Line2DPixel(xx1, zz1, xx2, zz2), color, color);
    GLOB_ENGINE->DrawLine(Line2DPixel(xx2, zz2, xx3, zz3), color, color);
    GLOB_ENGINE->DrawLine(Line2DPixel(xx3, zz3, xx1, zz1), color, color);
}

void CStaticAzimut::OnLButtonClick(float x, float y)
{
    CStatic::OnLButtonClick(x, y);

    CEdit* edit = dynamic_cast<CEdit*>(_parent->GetCtrl(_idcEdit));
    PoseidonAssert(edit);
    if (!edit)
    {
        return;
    }

    float xc = _x + 0.5 * _w;
    float yc = _y + 0.5 * _h;
    float azimut = atan2(x - xc, -y + yc) * 180.0 / H_PI;
    int roundAzimut = 5 * toInt(0.2 * azimut);
    if (roundAzimut < 0)
    {
        roundAzimut += 360;
    }

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", roundAzimut);
    edit->SetText(buffer);
}

void DisplayArcadeUnit::OnComboSelChanged(int idc, int curSel)
{
    switch (idc)
    {
        case IDC_ARCUNIT_SIDE:
        {
            CCombo* combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_SIDE));
            int side = combo ? combo->GetValue(curSel) : TWest;
            bool enable = side == TWest || side == TEast || side == TGuerrila || side == TCivilian;
            IControl* ctrl;
            ctrl = GetCtrl(IDC_ARCUNIT_RANK);
            if (ctrl)
            {
                ctrl->ShowCtrl(enable);
            }
            ctrl = GetCtrl(IDC_ARCUNIT_CTRL);
            if (ctrl)
            {
                ctrl->ShowCtrl(enable);
            }

            RString vehicle;
            combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_VEHICLE));
            if (combo)
            {
                int i = combo->GetCurSel();
                if (i < 0)
                {
                    vehicle = "";
                }
                else
                {
                    vehicle = combo->GetData(i);
                }
            }
            combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_CLASS));
            UpdateClasses(combo, vehicle);
        }
            // continue
        case IDC_ARCUNIT_CLASS:
        {
            CCombo* combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_VEHICLE));
            if (combo)
            {
                int i = combo->GetCurSel();
                RString vehicle;
                if (i < 0)
                {
                    vehicle = "";
                }
                else
                {
                    vehicle = combo->GetData(i);
                }
                UpdateVehicles(combo, vehicle);
            }
        }
        break;
        case IDC_ARCUNIT_VEHICLE:
        {
            CCombo* combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_CTRL));
            if (combo)
            {
                UpdatePlayer(combo, (ArcadeUnitPlayer)combo->GetValue(combo->GetCurSel()));
            }
        }
        break;
        case IDC_ARCUNIT_CTRL:
        {
            CCombo* combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_SIDE));
            if (combo)
            {
                if (curSel != 0 && combo->GetSize() == 4)
                {
                    // Playable
                    // combo content WEST, EAST, GUERRILA, CIVILIAN - OK
                    break;
                }
                if (curSel == 0 && combo->GetSize() == 6)
                {
                    // non Playable
                    // combo content WEST, EAST, GUERRILA, CIVILIAN, LOGIC, EMPTY - OK
                    break;
                }
                int sel = combo->GetCurSel();
                combo->ClearStrings();
                int index;
                index = combo->AddString(LocalizeString(IDS_WEST));
                combo->SetValue(index, TWest);
                index = combo->AddString(LocalizeString(IDS_EAST));
                combo->SetValue(index, TEast);
                index = combo->AddString(LocalizeString(IDS_GUERRILA));
                combo->SetValue(index, TGuerrila);
                index = combo->AddString(LocalizeString(IDS_CIVILIAN));
                combo->SetValue(index, TCivilian);
                if (curSel == 0)
                {
                    index = combo->AddString(LocalizeString(IDS_LOGIC));
                    combo->SetValue(index, TLogic);
                    index = combo->AddString(LocalizeString(IDS_EMPTY));
                    combo->SetValue(index, TEmpty);
                }
                if (sel >= combo->GetSize())
                {
                    sel = 0;
                }
                combo->SetCurSel(sel);
            }
        }
        break;
    }
    Display::OnComboSelChanged(idc, curSel);
}

// note: if "men" class is present, we want it to be first
// other classes should be sorted alphabetically
static int CmpClass(const RStringB* a, const RStringB* b)
{
    if (!strcmpi(*a, *b))
    {
        return 0;
    }
    if (!strcmpi(*a, "men"))
    {
        return -1;
    }
    if (!strcmpi(*b, "men"))
    {
        return +1;
    }
    return strcmpi(*a, *b);
}

void DisplayArcadeUnit::UpdateClasses(CCombo* combo, const char* vehicle)
{
    Arma1Compat::PrepareEditorClasses();
    CCombo* combo2 = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_SIDE));
    if (!combo2)
    {
        return;
    }
    int side = combo2->GetValue(combo2->GetCurSel());

    const ParamEntry& clsVeh = Pars >> "CfgVehicles";
    int n = clsVeh.GetEntryCount();
    FindArrayRStringBCI vehClasses;
    const ParamClass* manCls = static_cast<const ParamClass*>(clsVeh.FindEntry("Man"));
    // scan all CfgVehicles for vehicleClass list
    for (int i = 0; i < n; i++)
    {
        const ParamEntry& vehEntry = clsVeh.GetEntry(i);
        if (!vehEntry.IsClass())
        {
            continue;
        }
        int scope = vehEntry >> "scope";
        if (scope != 2)
        {
            continue;
        }
        int vehSide = vehEntry >> "side";
        if (side == TEmpty)
        {
            if (vehSide == TLogic)
            {
                continue;
            }
            const ParamClass* vehCls = static_cast<const ParamClass*>(&vehEntry);
            if (vehCls->IsDerivedFrom(*manCls))
            {
                continue;
            }
        }
        else
        {
            if (side != vehSide)
            {
                continue;
            }
            // note: some objects may be inserted only as empty
            bool hasDriver = vehEntry >> "hasDriver";
            bool hasGunner = vehEntry >> "hasGunner";
            bool hasCommander = vehEntry >> "hasCommander";
            if (!hasDriver && !hasGunner && !hasCommander)
            {
                // simulation should be static or thing
                RStringB sim = vehEntry >> "simulation";
                if (strcmpi(sim, "house") && strcmpi(sim, "thing") && strcmpi(sim, "fire") &&
                    strcmpi(sim, "fountain") && strcmpi(sim, "flagcarrier") && strcmpi(sim, "church"))
                {
                    RptF("Empty vehicle %s - simulation %s (this simulation is considered non-empty only)",
                         (const char*)vehEntry.GetName(), (const char*)sim);
                }
                continue;
            }
        }
        RStringB name = vehEntry >> "vehicleClass";
        if (name.GetLength() == 0)
        {
            continue;
        }
        int index = vehClasses.FindKey(name);
        if (index < 0)
        {
            index = vehClasses.AddUnique(name);
        }
    }

    RStringB vehicleClass;
    if (vehicle && *vehicle)
    {
        vehicleClass = Pars >> "CfgVehicles" >> vehicle >> "vehicleClass";
    }

    // note: if "men" class is present, we want it to be first
    // other classes should be sorted alphabetically
    QSort(vehClasses.Data(), vehClasses.Size(), CmpClass);

    combo->ClearStrings();
    int sel = 0;
    n = vehClasses.Size();
    for (int i = 0; i < n; i++)
    {
        // if (classCounts[i] == 0) continue;
        RStringB name = vehClasses[i];
        // Localize the displayed label via STR_DISP_ARCUNIT_CLASS_<UPPER(name)>;
        // unknown classes (modder-added) fall back to the raw config string.
        // The combo's data stays as the raw `name` because UpdateVehicles()
        // and other call sites below compare it against CfgVehicles entries.
        std::string key = "STR_DISP_ARCUNIT_CLASS_";
        for (const char* p = (const char*)name; *p; ++p)
        {
            key.push_back(static_cast<char>(toupper(static_cast<unsigned char>(*p))));
        }
        RString label = LocalizeString(key.c_str());
        if (label.GetLength() == 0)
        {
            label = (const char*)name;
        }
        int index = combo->AddString(label);
        combo->SetData(index, name);
        if (vehicleClass == name)
        {
            sel = index;
        }
    }
    combo->SetCurSel(sel);
}

void DisplayArcadeUnit::UpdateVehicles(CCombo* combo, const char* vehicle)
{
    Arma1Compat::PrepareEditorClasses();
    bool isVehicle = vehicle && *vehicle;

    CCombo* combo2 = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_SIDE));
    PoseidonAssert(combo2);
    int side = combo2->GetValue(combo2->GetCurSel());

    combo2 = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_CLASS));
    PoseidonAssert(combo2);
    int sel2 = combo2->GetCurSel();
    if (sel2 < 0)
    {
        return;
    }
    RString vehicleClass = combo2->GetData(sel2);

    combo->ClearStrings();
    const ParamClass* cls = Pars.GetClass("CfgVehicles");
    RString firstVehicle;
    for (int i = 0; i < cls->GetEntryCount(); i++)
    {
        const ParamEntry& vehEntry = cls->GetEntry(i);
        const ParamClass* vehCls = dynamic_cast<const ParamClass*>(&vehEntry);
        if (!vehCls)
        {
            continue;
        }
        int scope = vehEntry >> "scope";
        if (scope != 2)
        {
            continue;
        }
        int vehSide = vehEntry >> "side";
        if (side == TEmpty)
        {
            if (vehSide == TLogic)
            {
                continue;
            }
            if (vehCls->IsDerivedFrom(*cls->GetClass("Man")))
            {
                continue;
            }
        }
        else if (side != vehSide)
        {
            continue;
        }

        RString name = vehEntry >> "vehicleClass";
        if (name.GetLength() == 0)
        {
            continue;
        }
        if (strcmp(name, vehicleClass) != 0)
        {
            continue;
        }

        RString displayName = vehEntry >> "displayName";
        int index = combo->AddString(displayName);
        combo->SetData(index, vehEntry.GetName());

        if (firstVehicle.GetLength() == 0)
        {
            firstVehicle = vehEntry.GetName();
        }
    }
    combo->SortItems();

    int sel = -1;
    if (isVehicle)
    {
        for (int i = 0; i < combo->GetSize(); i++)
        {
            if (strcmp(combo->GetData(i), vehicle) == 0)
            {
                sel = i;
                break;
            }
        }
    }
    if (sel < 0 && firstVehicle.GetLength() > 0)
    {
        for (int i = 0; i < combo->GetSize(); i++)
        {
            if (strcmp(combo->GetData(i), firstVehicle) == 0)
            {
                sel = i;
                break;
            }
        }
    }
    saturateMax(sel, 0);
    combo->SetCurSel(sel);
}

void DisplayArcadeUnit::UpdatePlayer(CCombo* combo, ArcadeUnitPlayer player)
{
    combo->ClearStrings();
    int index = combo->AddString(LocalizeString(IDS_NONPLAYABLE));
    combo->SetValue(index, APNonplayable);

    CCombo* combo2 = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_SIDE));
    if (!combo2)
    {
        return;
    }
    int side = combo2->GetValue(combo2->GetCurSel());
    if (side != TWest && side != TEast && side != TGuerrila && side != TCivilian)
    {
        combo->SetCurSel(0);
        return;
    }

    // West, East, Guerrila, Civilian
    combo2 = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_VEHICLE));
    if (!combo2)
    {
        return;
    }
    int i = combo2->GetCurSel();
    if (i < 0)
    {
        return;
    }
    VehicleType* type = dynamic_cast<VehicleType*>(VehicleTypes.New(combo2->GetData(i)));
    if (!type)
    {
        Fail("Non-AI type");
        return;
    }

    int sel = 0;
    bool commander = type->HasCommander();
    bool gunner = type->HasGunner();
    bool driver = type->HasDriver();
    bool air = type->IsKindOf(GWorld->Preloaded(VTypeAir));

    // find nearest valid position
    if (!commander)
    {
        if (!gunner)
        {
            switch (player)
            {
                case APPlayerDriver:
                case APPlayerGunner:
                    player = APPlayerCommander;
                    break;
                case APPlayableC:
                case APPlayableD:
                case APPlayableG:
                case APPlayableCD:
                case APPlayableCG:
                case APPlayableDG:
                    player = APPlayableCDG;
                    break;
            }
        }
        else
        {
            switch (player)
            {
                case APPlayerCommander:
                    player = APPlayerDriver;
                    break;
                case APPlayableC:
                    player = APPlayableD;
                    break;
                case APPlayableCD:
                    player = APPlayableD;
                    break;
                case APPlayableCG:
                    player = APPlayableG;
                    break;
                case APPlayableCDG:
                    player = APPlayableDG;
                    break;
            }
        }
    }
    else if (!gunner)
    {
        switch (player)
        {
            case APPlayerGunner:
                player = APPlayerDriver;
                break;
            case APPlayableG:
                player = APPlayableD;
                break;
            case APPlayableCG:
                player = APPlayableC;
                break;
            case APPlayableDG:
                player = APPlayableD;
                break;
            case APPlayableCDG:
                player = APPlayableCG;
                break;
        }
    }
    if (!driver && gunner)
    {
        if (player != APNonplayable)
        {
            switch (player)
            {
                case APPlayerDriver:
                case APPlayerGunner:
                case APPlayerCommander:
                    player = APPlayerGunner;
                    break;
                default:
                    player = APPlayableG;
                    break;
            }
        }
    }

    if (commander)
    {
        // Player as commander
        index = combo->AddString(LocalizeString(IDS_PLAYER_COMMANDER));
        combo->SetValue(index, APPlayerCommander);
        if (player == APPlayerCommander)
        {
            sel = index;
        }
        // Player as pilot / driver
        if (driver)
        {
            if (air)
            {
                index = combo->AddString(LocalizeString(IDS_PLAYER_PILOT));
            }
            else
            {
                index = combo->AddString(LocalizeString(IDS_PLAYER_DRIVER));
            }
            combo->SetValue(index, APPlayerDriver);
            if (player == APPlayerDriver)
            {
                sel = index;
            }
        }
        // Player as gunner
        if (gunner)
        {
            index = combo->AddString(LocalizeString(IDS_PLAYER_GUNNER));
            combo->SetValue(index, APPlayerGunner);
            if (player == APPlayerGunner)
            {
                sel = index;
            }
        }
        // Playable as commander
        index = combo->AddString(LocalizeString(IDS_PLAYABLE_C));
        combo->SetValue(index, APPlayableC);
        if (player == APPlayableC)
        {
            sel = index;
        }
        if (driver)
        {
            // Playable as pilot / driver
            if (air)
            {
                index = combo->AddString(LocalizeString(IDS_PLAYABLE_P));
            }
            else
            {
                index = combo->AddString(LocalizeString(IDS_PLAYABLE_D));
            }
            combo->SetValue(index, APPlayableD);
            if (player == APPlayableD)
            {
                sel = index;
            }
        }
        // Playable as gunner
        if (gunner)
        {
            index = combo->AddString(LocalizeString(IDS_PLAYABLE_G));
            combo->SetValue(index, APPlayableG);
            if (player == APPlayableG)
            {
                sel = index;
            }
        }
        if (driver)
        {
            // Playable as commander and pilot / driver
            if (air)
            {
                index = combo->AddString(LocalizeString(IDS_PLAYABLE_CP));
            }
            else
            {
                index = combo->AddString(LocalizeString(IDS_PLAYABLE_CD));
            }
            combo->SetValue(index, APPlayableCD);
            if (player == APPlayableCD)
            {
                sel = index;
            }
        }
        if (gunner)
        {
            // Playable as commander and gunner
            index = combo->AddString(LocalizeString(IDS_PLAYABLE_CG));
            combo->SetValue(index, APPlayableCG);
            if (player == APPlayableCG)
            {
                sel = index;
            }
            // Playable as pilot / driver and gunner
            if (driver)
            {
                if (air)
                {
                    index = combo->AddString(LocalizeString(IDS_PLAYABLE_PG));
                }
                else
                {
                    index = combo->AddString(LocalizeString(IDS_PLAYABLE_DG));
                }
                combo->SetValue(index, APPlayableDG);
                if (player == APPlayableDG)
                {
                    sel = index;
                }
                // Playable as commander, pilot / driver and gunner
                if (air)
                {
                    index = combo->AddString(LocalizeString(IDS_PLAYABLE_CPG));
                }
                else
                {
                    index = combo->AddString(LocalizeString(IDS_PLAYABLE_CDG));
                }
                combo->SetValue(index, APPlayableCDG); // all available
                if (player == APPlayableCDG)
                {
                    sel = index;
                }
            }
        }
        else
        {
            combo->SetValue(index, APPlayableCDG); // all available
            if (player == APPlayableCDG)
            {
                sel = index;
            }
        }
    }
    else if (gunner)
    {
        // Player as pilot / driver
        if (driver)
        {
            if (air)
            {
                index = combo->AddString(LocalizeString(IDS_PLAYER_PILOT));
            }
            else
            {
                index = combo->AddString(LocalizeString(IDS_PLAYER_DRIVER));
            }
            combo->SetValue(index, APPlayerDriver);
            if (player == APPlayerDriver)
            {
                sel = index;
            }
        }
        // Player as gunner
        index = combo->AddString(LocalizeString(IDS_PLAYER_GUNNER));
        combo->SetValue(index, APPlayerGunner);
        if (player == APPlayerGunner)
        {
            sel = index;
        }
        if (driver)
        {
            // Playable as pilot / driver
            if (air)
            {
                index = combo->AddString(LocalizeString(IDS_PLAYABLE_P));
            }
            else
            {
                index = combo->AddString(LocalizeString(IDS_PLAYABLE_D));
            }
            combo->SetValue(index, APPlayableD);
            if (player == APPlayableD)
            {
                sel = index;
            }
        }
        // Playable as gunner
        index = combo->AddString(LocalizeString(IDS_PLAYABLE_G));
        combo->SetValue(index, APPlayableG);
        if (player == APPlayableG)
        {
            sel = index;
        }
        // Playable as pilot / driver and gunner
        if (driver)
        {
            if (air)
            {
                index = combo->AddString(LocalizeString(IDS_PLAYABLE_PG));
            }
            else
            {
                index = combo->AddString(LocalizeString(IDS_PLAYABLE_DG));
            }
            combo->SetValue(index, APPlayableCDG); // all available
            if (player == APPlayableCDG)
            {
                sel = index;
            }
        }
    }
    else
    {
        // Player
        index = combo->AddString(LocalizeString(IDS_PLAYER));
        combo->SetValue(index, APPlayerCommander);
        if (player == APPlayerCommander)
        {
            sel = index;
        }
        // Playable
        index = combo->AddString(LocalizeString(IDS_PLAYABLE));
        combo->SetValue(index, APPlayableCDG); // all available
        if (player == APPlayableCDG)
        {
            sel = index;
        }
    }

    combo->SetCurSel(sel);
}

Control* DisplayArcadeUnit::OnCreateCtrl(int type, int idc, const ParamEntry& cls)
{
    switch (idc)
    {
        case IDC_ARCUNIT_TITLE:
        {
            CStatic* text = new CStatic(this, idc, cls);
            if (_index < 0)
            {
                text->SetText(LocalizeString(IDS_ARCUNIT_TITLE2));
            }
            else
            {
                text->SetText(LocalizeString(IDS_ARCUNIT_TITLE4));
            }
            return text;
        }
        case IDC_ARCUNIT_AZIMUT_PICTURE:
            return new CStaticAzimut(this, idc, cls, IDC_ARCUNIT_AZIMUT);
        case IDC_ARCUNIT_SIDE:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            int index, sel = 0;
            index = combo->AddString(LocalizeString(IDS_WEST));
            combo->SetValue(index, TWest);
            if (_unit.side == TWest)
            {
                sel = index;
            }
            index = combo->AddString(LocalizeString(IDS_EAST));
            combo->SetValue(index, TEast);
            if (_unit.side == TEast)
            {
                sel = index;
            }
            index = combo->AddString(LocalizeString(IDS_GUERRILA));
            combo->SetValue(index, TGuerrila);
            if (_unit.side == TGuerrila)
            {
                sel = index;
            }
            index = combo->AddString(LocalizeString(IDS_CIVILIAN));
            combo->SetValue(index, TCivilian);
            if (_unit.side == TCivilian)
            {
                sel = index;
            }
            if (_unit.player == APNonplayable)
            {
                index = combo->AddString(LocalizeString(IDS_LOGIC));
                combo->SetValue(index, TLogic);
                if (_unit.side == TLogic)
                {
                    sel = index;
                }
                index = combo->AddString(LocalizeString(IDS_EMPTY));
                combo->SetValue(index, TEmpty);
                if (_unit.side == TEmpty)
                {
                    sel = index;
                }
            }
            combo->SetCurSel(sel);
            if (_index >= 0)
            {
                // cannot change side when edited
                combo->EnableCtrl(false);
            }
            return combo;
        }
        case IDC_ARCUNIT_CLASS:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            UpdateClasses(combo, _unit.vehicle);
            return combo;
        }
        case IDC_ARCUNIT_VEHICLE:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            UpdateVehicles(combo, _unit.vehicle);
            return combo;
        }
        case IDC_ARCUNIT_TEXT:
        {
            CEdit* text = new CEdit(this, idc, cls);
            text->SetText(_unit.name);
            return text;
        }
        case IDC_ARCUNIT_INIT:
        {
            CEdit* text = new CEdit(this, idc, cls);
            text->SetText(_unit.init);
            return text;
        }
        case IDC_ARCUNIT_RANK:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            for (int i = RankPrivate; i <= RankColonel; i++)
            {
                RString rank = LocalizeString(IDS_PRIVATE + i);
                combo->AddString(rank);
            }
            combo->SetCurSel(_unit.rank - RankPrivate);
            if (_unit.side == TEmpty || _unit.side == TLogic)
            {
                combo->SetCurSel(0);
                combo->ShowCtrl(false);
            }
            return combo;
        }
        case IDC_ARCUNIT_AZIMUT:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%g", _unit.azimut);
            edit->SetText(buffer);
            return edit;
        }
        case IDC_ARCUNIT_SPECIAL:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            combo->AddString(LocalizeString(IDS_SPECIAL_NONE));
            combo->AddString(LocalizeString(IDS_SPECIAL_CARGO));
            combo->AddString(LocalizeString(IDS_SPECIAL_FLYING));
            combo->AddString(LocalizeString(IDS_SPECIAL_FORM));
            combo->SetCurSel(_unit.special);
            return combo;
        }
        case IDC_ARCUNIT_CTRL:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            UpdatePlayer(combo, _unit.player);
            return combo;
        }
        case IDC_ARCUNIT_AGE:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            for (int i = 0; i < AAN; i++)
            {
                combo->AddString(LocalizeString(IDS_AGE_ACTUAL + i));
            }
            combo->SetCurSel(_unit.age);
            return combo;
        }
        case IDC_ARCUNIT_SKILL:
        {
            CSlider* slider = new CSlider(this, idc, cls);
            slider->SetRange(0.2, 1);
            slider->SetSpeed(0.01, 0.1);
            slider->SetThumbPos(_unit.skill);
            return slider;
        }
        case IDC_ARCUNIT_HEALTH:
        {
            CSlider* slider = new CSlider(this, idc, cls);
            slider->SetRange(0, 1);
            slider->SetSpeed(0.01, 0.1);
            slider->SetThumbPos(_unit.health);
            return slider;
        }
        case IDC_ARCUNIT_FUEL:
        {
            CSlider* slider = new CSlider(this, idc, cls);
            slider->SetRange(0, 1);
            slider->SetSpeed(0.01, 0.1);
            slider->SetThumbPos(_unit.fuel);
            return slider;
        }
        case IDC_ARCUNIT_AMMO:
        {
            CSlider* slider = new CSlider(this, idc, cls);
            slider->SetRange(0, 1);
            slider->SetSpeed(0.01, 0.1);
            slider->SetThumbPos(_unit.ammo);
            return slider;
        }
        case IDC_ARCUNIT_PRESENCE:
        {
            CSlider* slider = new CSlider(this, idc, cls);
            slider->SetRange(0, 1);
            slider->SetSpeed(0.01, 0.1);
            slider->SetThumbPos(_unit.presence);
            return slider;
        }
        case IDC_ARCUNIT_PRESENCE_COND:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            edit->SetText(_unit.presenceCondition);
            return edit;
        }
        case IDC_ARCUNIT_PLACE:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%g", _unit.placement);
            edit->SetText(buffer);
            return edit;
        }
        case IDC_ARCUNIT_LOCK:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            int sel = 1; // default
            int index = combo->AddString(LocalizeString(IDS_VEHICLE_UNLOCKED));
            combo->SetValue(index, LSUnlocked);
            if (_unit.lock == LSUnlocked)
            {
                sel = index;
            }
            index = combo->AddString(LocalizeString(IDS_VEHICLE_DEFAULT));
            combo->SetValue(index, LSDefault);
            if (_unit.lock == LSDefault)
            {
                sel = index;
            }
            index = combo->AddString(LocalizeString(IDS_VEHICLE_LOCKED));
            combo->SetValue(index, LSLocked);
            if (_unit.lock == LSLocked)
            {
                sel = index;
            }
            combo->SetCurSel(sel);
            return combo;
        }
        default:
            return Display::OnCreateCtrl(type, idc, cls);
    }
}

bool DisplayArcadeUnit::CanDestroy()
{
    if (!Display::CanDestroy())
    {
        return false;
    }

    if (_exit == IDC_OK)
    {
        CEdit* edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCUNIT_TEXT));
        if (edit)
        {
            RString text = edit->GetText();
            if (text.GetLength() > 0)
            {
                if (!GWorld->GetGameState()->IdtfGoodName(text))
                {
                    CreateMsgBox(MB_BUTTON_OK, LocalizeString(IDS_MSG_VEH_NAME));
                    return false;
                }
                // check if text is not used
                for (int i = 0; i < _template->groups.Size(); i++)
                {
                    ArcadeGroupInfo& gInfo = _template->groups[i];
                    for (int j = 0; j < gInfo.units.Size(); j++)
                    {
                        if (i == _indexGroup && j == _index)
                        {
                            continue;
                        }
                        if (stricmp(text, gInfo.units[j].name) == 0)
                        {
                            CreateMsgBox(MB_BUTTON_OK, LocalizeString(IDS_MSG_VEH_TEXT_USED));
                            return false;
                        }
                    }
                    for (int j = 0; j < gInfo.sensors.Size(); j++)
                    {
                        if (stricmp(text, gInfo.sensors[j].name) == 0)
                        {
                            CreateMsgBox(MB_BUTTON_OK, LocalizeString(IDS_MSG_VEH_TEXT_USED));
                            return false;
                        }
                    }
                }
                for (int j = 0; j < _template->emptyVehicles.Size(); j++)
                {
                    if (_indexGroup == -1 && j == _index)
                    {
                        continue;
                    }
                    if (stricmp(text, _template->emptyVehicles[j].name) == 0)
                    {
                        CreateMsgBox(MB_BUTTON_OK, LocalizeString(IDS_MSG_VEH_TEXT_USED));
                        return false;
                    }
                }
                for (int j = 0; j < _template->sensors.Size(); j++)
                {
                    if (stricmp(text, _template->sensors[j].name) == 0)
                    {
                        CreateMsgBox(MB_BUTTON_OK, LocalizeString(IDS_MSG_VEH_TEXT_USED));
                        return false;
                    }
                }
            }
        }

        GameState* gstate = GWorld->GetGameState();

        edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCUNIT_INIT));
        if (edit)
        {
            if (!gstate->CheckExecute(edit->GetText()))
            {
                FocusCtrl(IDC_ARCUNIT_INIT);
                CreateMsgBox(MB_BUTTON_OK, gstate->GetLastErrorText());
                edit->SetCaretPos(gstate->GetLastErrorPos());
                return false;
            }
        }

        edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCUNIT_PRESENCE_COND));
        if (edit)
        {
            if (!gstate->CheckEvaluateBool(edit->GetText()))
            {
                FocusCtrl(IDC_ARCUNIT_PRESENCE_COND);
                CreateMsgBox(MB_BUTTON_OK, gstate->GetLastErrorText());
                edit->SetCaretPos(gstate->GetLastErrorPos());
                return false;
            }
        }

        CCombo* combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_VEHICLE));
        if (combo)
        {
            if (combo->GetCurSel() < 0)
            {
                CreateMsgBox(MB_BUTTON_OK, LocalizeString(IDS_MSG_NO_VEH_SELECT));
                return false;
            }
        }
    }
    return true;
}

void DisplayArcadeUnit::Destroy()
{
    Display::Destroy();

    if (_exit != IDC_OK)
    {
        return;
    }

    CCombo* combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_SIDE));
    if (combo)
    {
        _unit.side = (TargetSide)combo->GetValue(combo->GetCurSel());
    }
    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_CTRL));
    if (combo)
    {
        _unit.player = (ArcadeUnitPlayer)combo->GetValue(combo->GetCurSel());
        if (_unit.side == TEmpty || _unit.side == TLogic)
        {
            _unit.player = APNonplayable;
        }
    }
    CSlider* slider = dynamic_cast<CSlider*>(GetCtrl(IDC_ARCUNIT_HEALTH));
    if (slider)
    {
        _unit.health = slider->GetThumbPos();
    }
    slider = dynamic_cast<CSlider*>(GetCtrl(IDC_ARCUNIT_FUEL));
    if (slider)
    {
        _unit.fuel = slider->GetThumbPos();
    }
    slider = dynamic_cast<CSlider*>(GetCtrl(IDC_ARCUNIT_AMMO));
    if (slider)
    {
        _unit.ammo = slider->GetThumbPos();
    }
    slider = dynamic_cast<CSlider*>(GetCtrl(IDC_ARCUNIT_SKILL));
    if (slider)
    {
        _unit.skill = slider->GetThumbPos();
    }
    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_VEHICLE));
    if (combo)
    {
        int indexVehicle = combo->GetCurSel();
        PoseidonAssert(indexVehicle >= 0);
        _unit.vehicle = combo->GetData(indexVehicle);
    }
    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_RANK));
    if (combo)
    {
        _unit.rank = (Rank)(RankPrivate + combo->GetCurSel());
        if (_unit.side == TEmpty || _unit.side == TLogic)
        {
            _unit.rank = RankPrivate;
        }
    }
    CEdit* edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCUNIT_AZIMUT));
    if (edit)
    {
        _unit.azimut = edit->GetText() ? atof(edit->GetText()) : 0;
    }
    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_SPECIAL));
    if (combo)
    {
        _unit.special = (ArcadeUnitSpecial)combo->GetCurSel();
    }
    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_AGE));
    if (combo)
    {
        _unit.age = (ArcadeUnitAge)combo->GetCurSel();
    }
    slider = dynamic_cast<CSlider*>(GetCtrl(IDC_ARCUNIT_PRESENCE));
    if (slider)
    {
        _unit.presence = slider->GetThumbPos();
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCUNIT_PRESENCE_COND));
    if (edit)
    {
        _unit.presenceCondition = edit->GetText();
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCUNIT_PLACE));
    if (edit)
    {
        _unit.placement = edit->GetText() ? atof(edit->GetText()) : 0;
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCUNIT_TEXT));
    if (edit)
    {
        _unit.name = edit->GetText();
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCUNIT_INIT));
    if (edit)
    {
        _unit.init = edit->GetText();
    }
    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCUNIT_LOCK));
    if (combo)
    {
        int sel = combo->GetCurSel();
        if (sel >= 0)
        {
            _unit.lock = (LockState)combo->GetValue(sel);
        }
    }
}

DisplayArcadeGroup::DisplayArcadeGroup(ControlsContainer* parent, Vector3Par position, RString side, RString type,
                                       RString name, float azimut)
    : Display(parent)
{
    _enableSimulation = false;
    _enableDisplay = false;

    _position = position;
    _azimut = azimut;

    Load("RscDisplayArcadeGroup");

    UpdateTypes();
    UpdateNames();

    CCombo* combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCGRP_SIDE));
    if (combo)
    {
        for (int i = 0; i < combo->GetSize(); i++)
        {
            if (combo->GetData(i) == side)
            {
                combo->SetCurSel(i);
                break;
            }
        }
    }

    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCGRP_TYPE));
    if (combo)
    {
        for (int i = 0; i < combo->GetSize(); i++)
        {
            if (combo->GetData(i) == type)
            {
                combo->SetCurSel(i);
                break;
            }
        }
    }

    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCGRP_NAME));
    if (combo)
    {
        for (int i = 0; i < combo->GetSize(); i++)
        {
            if (combo->GetData(i) == name)
            {
                combo->SetCurSel(i);
                break;
            }
        }
    }
}

Control* DisplayArcadeGroup::OnCreateCtrl(int type, int idc, const ParamEntry& cls)
{
    switch (idc)
    {
        case IDC_ARCGRP_SIDE:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            const ParamEntry& groups = Pars >> "CfgGroups";
            for (int i = 0; i < groups.GetEntryCount(); i++)
            {
                const ParamEntry& entry = groups.GetEntry(i);
                int index = combo->AddString(entry >> "name");
                combo->SetData(index, entry.GetName());
            }
            combo->SetCurSel(0);
            return combo;
        }
        case IDC_ARCGRP_AZIMUT:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%g", _azimut);
            edit->SetText(buffer);
            return edit;
        }
        case IDC_ARCGRP_AZIMUT_PICTURE:
            return new CStaticAzimut(this, idc, cls, IDC_ARCGRP_AZIMUT);
    }
    return Display::OnCreateCtrl(type, idc, cls);
}

void DisplayArcadeGroup::OnComboSelChanged(int idc, int curSel)
{
    switch (idc)
    {
        case IDC_ARCGRP_SIDE:
            UpdateTypes();
            UpdateNames();
            break;
        case IDC_ARCGRP_TYPE:
            UpdateNames();
            break;
    }
}

void DisplayArcadeGroup::UpdateTypes()
{
    CCombo* combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCGRP_SIDE));
    if (!combo)
    {
        return;
    }
    int i = combo->GetCurSel();
    if (i < 0)
    {
        return;
    }
    RString side = combo->GetData(i);

    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCGRP_TYPE));
    if (!combo)
    {
        return;
    }
    combo->ClearStrings();
    const ParamEntry& types = Pars >> "CfgGroups" >> side;
    for (int i = 0; i < types.GetEntryCount(); i++)
    {
        const ParamEntry& entry = types.GetEntry(i);
        if (!entry.IsClass())
        {
            continue;
        }
        int index = combo->AddString(entry >> "name");
        combo->SetData(index, entry.GetName());
    }
    combo->SetCurSel(0);
}

void DisplayArcadeGroup::UpdateNames()
{
    CCombo* combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCGRP_SIDE));
    if (!combo)
    {
        return;
    }
    int i = combo->GetCurSel();
    if (i < 0)
    {
        return;
    }
    RString side = combo->GetData(i);

    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCGRP_TYPE));
    if (!combo)
    {
        return;
    }
    i = combo->GetCurSel();
    if (i < 0)
    {
        return;
    }
    RString type = combo->GetData(i);

    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCGRP_NAME));
    if (!combo)
    {
        return;
    }
    combo->ClearStrings();
    const ParamEntry& names = Pars >> "CfgGroups" >> side >> type;
    for (int i = 0; i < names.GetEntryCount(); i++)
    {
        const ParamEntry& entry = names.GetEntry(i);
        if (!entry.IsClass())
        {
            continue;
        }
        int index = combo->AddString(entry >> "name");
        combo->SetData(index, entry.GetName());
    }
    combo->SetCurSel(0);
}

Control* DisplayArcadeSensor::OnCreateCtrl(int type, int idc, const ParamEntry& cls)
{
    switch (idc)
    {
        case IDC_ARCSENS_TITLE:
        {
            CStatic* text = new CStatic(this, idc, cls);
            if (_index < 0)
            {
                text->SetText(LocalizeString(IDS_ARCSENS_TITLE1));
            }
            else
            {
                text->SetText(LocalizeString(IDS_ARCSENS_TITLE2));
            }
            return text;
        }
        case IDC_ARCSENS_A:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%g", _sensor.a);
            edit->SetText(buffer);
            return edit;
        }
        case IDC_ARCSENS_B:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%g", _sensor.b);
            edit->SetText(buffer);
            return edit;
        }
        case IDC_ARCSENS_ANGLE:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%g", _sensor.angle);
            edit->SetText(buffer);
            return edit;
        }
        case IDC_ARCSENS_RECT:
        {
            CToolBox* toolbox = new CToolBox(this, idc, cls);
            if (_sensor.rectangular)
            {
                toolbox->SetCurSel(1);
            }
            else
            {
                toolbox->SetCurSel(0);
            }
            return toolbox;
        }
        case IDC_ARCSENS_ACTIV:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            if (_ig >= 0)
            {
                combo->AddString(LocalizeString(IDS_SENSORACTIV_GROUP));
                combo->SetCurSel(0);
            }
            else if (_sensor.idStatic >= 0)
            {
                combo->AddString(LocalizeString(IDS_SENSORACTIV_STATIC));
                combo->SetCurSel(0);
            }
            else if (_sensor.idVehicle >= 0)
            {
                int ig;
                int index;
                if (!_t->FindUnit(_sensor.idVehicle, ig, index))
                {
                    // vehicle doesn't exist
                    _sensor.idVehicle = -1;
                    goto NoVehicle;
                }
                else if (ig >= 0)
                {
                    // vehicle in group
                    combo->AddString(LocalizeString(IDS_SENSORACTIV_VEHICLE));
                    combo->AddString(LocalizeString(IDS_SENSORACTIV_GROUP));
                    combo->AddString(LocalizeString(IDS_SENSORACTIV_LEADER));
                    combo->AddString(LocalizeString(IDS_SENSORACTIV_MEMBER));
                    int sel = _sensor.activationBy - ASAVehicle;
                    saturate(sel, 0, combo->GetSize() - 1);
                    combo->SetCurSel(sel);
                }
                else
                {
                    // empty vehicle
                    combo->AddString(LocalizeString(IDS_SENSORACTIV_VEHICLE));
                    combo->SetCurSel(0);
                }
            }
            else
            {
            NoVehicle:
                combo->AddString(LocalizeString(IDS_SENSORACTIV_NONE));
                combo->AddString(LocalizeString(IDS_EAST));
                combo->AddString(LocalizeString(IDS_WEST));
                combo->AddString(LocalizeString(IDS_GUERRILA));
                combo->AddString(LocalizeString(IDS_CIVILIAN));
                combo->AddString(LocalizeString(IDS_LOGIC));
                combo->AddString(LocalizeString(IDS_SENSORACTIV_ANYBODY));
                combo->AddString(LocalizeString(IDS_SENSORACTIV_ALPHA));
                combo->AddString(LocalizeString(IDS_SENSORACTIV_BRAVO));
                combo->AddString(LocalizeString(IDS_SENSORACTIV_CHARLIE));
                combo->AddString(LocalizeString(IDS_SENSORACTIV_DELTA));
                combo->AddString(LocalizeString(IDS_SENSORACTIV_ECHO));
                combo->AddString(LocalizeString(IDS_SENSORACTIV_FOXTROT));
                combo->AddString(LocalizeString(IDS_SENSORACTIV_GOLF));
                combo->AddString(LocalizeString(IDS_SENSORACTIV_HOTEL));
                combo->AddString(LocalizeString(IDS_SENSORACTIV_INDIA));
                combo->AddString(LocalizeString(IDS_SENSORACTIV_JULIET));
                if (_sensor.activationBy > ASAJuliet)
                {
                    _sensor.activationBy = ASANone;
                }
                combo->SetCurSel(_sensor.activationBy);
            }
            return combo;
        }
        case IDC_ARCSENS_PRESENCE:
        {
            CToolBox* toolbox = new CToolBox(this, idc, cls);
            toolbox->SetCurSel(_sensor.activationType);
            return toolbox;
        }
        case IDC_ARCSENS_REPEATING:
        {
            CToolBox* toolbox = new CToolBox(this, idc, cls);
            if (_sensor.repeating)
            {
                toolbox->SetCurSel(1);
            }
            else
            {
                toolbox->SetCurSel(0);
            }
            return toolbox;
        }
        case IDC_ARCSENS_INTERRUPT:
        {
            CToolBox* toolbox = new CToolBox(this, idc, cls);
            if (_sensor.interruptable)
            {
                toolbox->SetCurSel(1);
            }
            else
            {
                toolbox->SetCurSel(0);
            }
            return toolbox;
        }
        case IDC_ARCSENS_TIMEOUT_MIN:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%g", _sensor.timeoutMin);
            edit->SetText(buffer);
            return edit;
        }
        case IDC_ARCSENS_TIMEOUT_MID:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%g", _sensor.timeoutMid);
            edit->SetText(buffer);
            return edit;
        }
        case IDC_ARCSENS_TIMEOUT_MAX:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%g", _sensor.timeoutMax);
            edit->SetText(buffer);
            return edit;
        }
        case IDC_ARCSENS_TYPE:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            for (int i = 0; i < ASTN; i++)
            {
                combo->AddString(LocalizeString(IDS_SENSORTYPE_NONE + i));
            }
            combo->SetCurSel(_sensor.type);
            return combo;
        }
        case IDC_ARCSENS_OBJECT:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            const ParamEntry& objects = Pars >> "CfgDetectors" >> "objects";
            int sel = 0;
            for (int i = 0; i < objects.GetSize(); i++)
            {
                RString objectName = objects[i];
                if (stricmp(objectName, _sensor.object) == 0)
                {
                    sel = i;
                }
                const ParamEntry& objectCls = Pars >> "CfgNonAIVehicles" >> objectName;
                RString displayName = objectCls >> "displayName";
                int index = combo->AddString(displayName);
                combo->SetData(index, objectName);
            }
            combo->SetCurSel(sel);
            return combo;
        }
        case IDC_ARCSENS_TEXT:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            edit->SetText(_sensor.text);
            return edit;
        }
        case IDC_ARCSENS_NAME:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            edit->SetText(_sensor.name);
            return edit;
        }
        case IDC_ARCSENS_EXPCOND:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            edit->SetText(_sensor.expCond);
            return edit;
        }
        case IDC_ARCSENS_EXPACTIV:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            edit->SetText(_sensor.expActiv);
            return edit;
        }
        case IDC_ARCSENS_EXPDESACTIV:
        {
            CEdit* edit = new CEdit(this, idc, cls);
            edit->SetText(_sensor.expDesactiv);
            return edit;
        }
        case IDC_ARCSENS_AGE:
        {
            CCombo* combo = new CCombo(this, idc, cls);
            for (int i = 0; i < AAN; i++)
            {
                combo->AddString(LocalizeString(IDS_AGE_ACTUAL + i));
            }
            combo->SetCurSel(_sensor.age);
            return combo;
        }
    }

    return Display::OnCreateCtrl(type, idc, cls);
}

void DisplayArcadeSensor::OnButtonClicked(int idc)
{
    if (idc == IDC_ARCSENS_EFFECTS)
    {
        CreateChild(new DisplayArcadeEffects(this, _sensor.effects, _advanced));
    }
    else
    {
        Display::OnButtonClicked(idc);
    }
}

void DisplayArcadeSensor::OnChildDestroyed(int idd, int exit)
{
    if (idd == IDD_ARCADE_EFFECTS && exit == IDC_OK)
    {
        DisplayArcadeEffects* display = dynamic_cast<DisplayArcadeEffects*>((ControlsContainer*)_child);
        PoseidonAssert(display);
        _sensor.effects = display->_effects;
    }
    Display::OnChildDestroyed(idd, exit);
}

bool DisplayArcadeSensor::CanDestroy()
{
    if (_exit != IDC_OK)
    {
        return true;
    }

    GameState* gstate = GWorld->GetGameState();

    CEdit* edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_EXPCOND));
    if (edit)
    {
        if (!gstate->CheckEvaluateBool(edit->GetText()))
        {
            FocusCtrl(IDC_ARCSENS_EXPCOND);
            CreateMsgBox(MB_BUTTON_OK, gstate->GetLastErrorText());
            edit->SetCaretPos(gstate->GetLastErrorPos());
            return false;
        }
    }

    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_EXPACTIV));
    if (edit)
    {
        if (!gstate->CheckExecute(edit->GetText()))
        {
            FocusCtrl(IDC_ARCSENS_EXPACTIV);
            CreateMsgBox(MB_BUTTON_OK, gstate->GetLastErrorText());
            edit->SetCaretPos(gstate->GetLastErrorPos());
            return false;
        }
    }

    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_EXPDESACTIV));
    if (edit)
    {
        if (!gstate->CheckExecute(edit->GetText()))
        {
            FocusCtrl(IDC_ARCSENS_EXPDESACTIV);
            CreateMsgBox(MB_BUTTON_OK, gstate->GetLastErrorText());
            edit->SetCaretPos(gstate->GetLastErrorPos());
            return false;
        }
    }

    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_NAME));
    if (edit)
    {
        RString text = edit->GetText();
        if (text.GetLength() > 0)
        {
            if (!GWorld->GetGameState()->IdtfGoodName(text))
            {
                CreateMsgBox(MB_BUTTON_OK, LocalizeString(IDS_MSG_VEH_NAME));
                return false;
            }
            // check if text is not used
            for (int i = 0; i < _t->groups.Size(); i++)
            {
                ArcadeGroupInfo& gInfo = _t->groups[i];
                for (int j = 0; j < gInfo.units.Size(); j++)
                {
                    if (stricmp(text, gInfo.units[j].name) == 0)
                    {
                        CreateMsgBox(MB_BUTTON_OK, LocalizeString(IDS_MSG_VEH_TEXT_USED));
                        return false;
                    }
                }
                for (int j = 0; j < gInfo.sensors.Size(); j++)
                {
                    if (i == _ig && j == _index)
                    {
                        continue;
                    }
                    if (stricmp(text, gInfo.sensors[j].name) == 0)
                    {
                        CreateMsgBox(MB_BUTTON_OK, LocalizeString(IDS_MSG_VEH_TEXT_USED));
                        return false;
                    }
                }
            }
            for (int j = 0; j < _t->emptyVehicles.Size(); j++)
            {
                if (stricmp(text, _t->emptyVehicles[j].name) == 0)
                {
                    CreateMsgBox(MB_BUTTON_OK, LocalizeString(IDS_MSG_VEH_TEXT_USED));
                    return false;
                }
            }
            for (int j = 0; j < _t->sensors.Size(); j++)
            {
                if (_ig == -1 && j == _index)
                {
                    continue;
                }
                if (stricmp(text, _t->sensors[j].name) == 0)
                {
                    CreateMsgBox(MB_BUTTON_OK, LocalizeString(IDS_MSG_VEH_TEXT_USED));
                    return false;
                }
            }
        }
    }

    return true;
}

void DisplayArcadeSensor::Destroy()
{
    Display::Destroy();

    if (_exit != IDC_OK)
    {
        return;
    }

    CEdit* edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_A));
    if (edit)
    {
        _sensor.a = edit->GetText() ? atof(edit->GetText()) : 0;
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_B));
    if (edit)
    {
        _sensor.b = edit->GetText() ? atof(edit->GetText()) : 0;
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_ANGLE));
    if (edit)
    {
        _sensor.angle = edit->GetText() ? atof(edit->GetText()) : 0;
    }
    CToolBox* toolbox = dynamic_cast<CToolBox*>(GetCtrl(IDC_ARCSENS_RECT));
    if (toolbox)
    {
        _sensor.rectangular = toolbox->GetCurSel() == 1;
    }

    CCombo* combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCSENS_ACTIV));
    if (combo)
    {
        if (_ig >= 0)
        {
            _sensor.activationBy = ASAGroup;
        }
        else if (_sensor.idStatic >= 0)
        {
            _sensor.activationBy = ASAStatic;
        }
        else if (_sensor.idVehicle >= 0)
        {
            _sensor.activationBy = (ArcadeSensorActivation)(ASAVehicle + combo->GetCurSel());
        }
        else
        {
            _sensor.activationBy = (ArcadeSensorActivation)(combo->GetCurSel());
        }
    }
    toolbox = dynamic_cast<CToolBox*>(GetCtrl(IDC_ARCSENS_PRESENCE));
    if (toolbox)
    {
        _sensor.activationType = (ArcadeSensorActivationType)(toolbox->GetCurSel());
    }
    toolbox = dynamic_cast<CToolBox*>(GetCtrl(IDC_ARCSENS_REPEATING));
    if (toolbox)
    {
        _sensor.repeating = toolbox->GetCurSel() == 1;
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_TIMEOUT_MIN));
    if (edit)
    {
        _sensor.timeoutMin = edit->GetText() ? atof(edit->GetText()) : 0;
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_TIMEOUT_MID));
    if (edit)
    {
        _sensor.timeoutMid = edit->GetText() ? atof(edit->GetText()) : 0;
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_TIMEOUT_MAX));
    if (edit)
    {
        _sensor.timeoutMax = edit->GetText() ? atof(edit->GetText()) : 0;
    }
    toolbox = dynamic_cast<CToolBox*>(GetCtrl(IDC_ARCSENS_INTERRUPT));
    if (toolbox)
    {
        _sensor.interruptable = toolbox->GetCurSel() == 1;
    }
    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCSENS_TYPE));
    if (combo)
    {
        _sensor.type = (ArcadeSensorType)(combo->GetCurSel());
    }
    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCSENS_OBJECT));
    if (combo)
    {
        _sensor.object = combo->GetData(combo->GetCurSel());
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_TEXT));
    if (edit)
    {
        _sensor.text = edit->GetText();
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_NAME));
    if (edit)
    {
        _sensor.name = edit->GetText();
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_EXPCOND));
    if (edit)
    {
        _sensor.expCond = edit->GetText();
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_EXPACTIV));
    if (edit)
    {
        _sensor.expActiv = edit->GetText();
    }
    edit = dynamic_cast<CEdit*>(GetCtrl(IDC_ARCSENS_EXPDESACTIV));
    if (edit)
    {
        _sensor.expDesactiv = edit->GetText();
    }
    combo = dynamic_cast<CCombo*>(GetCtrl(IDC_ARCSENS_AGE));
    if (combo)
    {
        _sensor.age = (ArcadeUnitAge)(combo->GetCurSel());
    }
}

} // namespace Poseidon
