#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Input/CheatCode.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Config/UserConfig.hpp>
#include <Poseidon/UI/Map/UIMap.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/UI/UITestEngine.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Core/Progress.hpp>
#include <Poseidon/UI/Settings/GameSettingsConfig.hpp> // kMaxViewDistance for the visibility slider
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Poseidon/Core/SaveVersion.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/UI/Locale/Stringtable/CodepageTranscode.hpp>
#include <Poseidon/Game/Scripting/Scripts.hpp>
#include <SDL3/SDL_keycode.h>
#include <Poseidon/Foundation/Common/Win.h>
#include <stdio.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#endif
#include <Poseidon/Network/Network.hpp>
#include <Poseidon/Game/Chat.hpp>
#include <Poseidon/UI/Locale/MissionHtmlLocalization.hpp>
#include <Poseidon/UI/Locale/MissionLanguageDetector.hpp>
#include <Poseidon/Audio/DynSound.hpp>
#include <Poseidon/UI/DisplayUI.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>
#include <Poseidon/Foundation/Strings/Mbcs.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Foundation/Strings/StrFormat.hpp>
#include <Poseidon/UI/OptionsUICommon.hpp>

// Defined at global scope in World/World.cpp.
void ShowCinemaBorder(bool show);

extern void ReleaseVBuffers();
extern void RestoreVBuffers();
namespace Poseidon
{

void SetHWTL(bool val)
{
    if (ENGINE_CONFIG.enableHWTL == val)
    {
        return;
    }
    ENGINE_CONFIG.enableHWTL = val;
    if (!ENGINE_CONFIG.enableHWTL)
    {
        ::ReleaseVBuffers();
    }
    else
    {
        ::RestoreVBuffers();
    }
}

// Fetch an options-page toggle label, tolerating a control the loaded config does not declare.
//
// These pages address a fixed set of legacy IDCs — including Direct3D-era toggles (hardware T&L,
// multitexturing, W-buffer) that a config is free not to declare. The original code
// static_cast'd GetCtrl's result and dereferenced it immediately, with no null check, in ~23
// places. A single absent control therefore killed the whole page in its CONSTRUCTOR: the
// display never finished being created, so clicking the menu entry appeared to do nothing at
// all. No message, no crash dialog — just a button that does not work.
//
// Every other block in these constructors already used dynamic_cast + `if (ctrl)`; these did
// not. The warning names the IDC so a missing control is a one-line log entry instead of an
// afternoon. See .agents/README.md RULE 5.
static C3DActiveText* OptionalActiveText(Display* page, int idc, const char* what)
{
    C3DActiveText* ctrl = dynamic_cast<C3DActiveText*>(page->GetCtrl(idc));
    if (!ctrl)
    {
        LOG_WARN(UI, "Options/{}: control IDC {} is not declared by the config; skipping it", what, idc);
    }
    return ctrl;
}

// The uniform "ENABLED / DISABLED" toggle label these pages are made of.
static void SetToggleText(Display* page, int idc, const char* what, bool on)
{
    if (C3DActiveText* ctrl = OptionalActiveText(page, idc, what))
    {
        ctrl->SetText(LocalizeString(on ? IDS_ENABLED : IDS_DISABLED));
    }
}

// Video options display

DisplayOptionsVideo::DisplayOptionsVideo(ControlsContainer* parent, bool enableSimulation) : Display(parent)
{
    _enableSimulation = enableSimulation;
    Load("RscDisplayOptionsVideo");

    _oldBright = GEngine->GetBrightness();
    _oldGamma = GEngine->GetGamma();
    _oldFrameRate = GScene->GetFrameRateSettings();
    _oldQuality = GScene->GetQualitySettings();

    _oldResX = GEngine->Width();
    _oldResY = GEngine->Height();
    _oldBpp = GEngine->PixelSize();
    _oldWindowed = GEngine->IsWindowed();
    _oldRefreshRate = GEngine->RefreshRate();

    _oldObjShadows = GScene->GetObjectShadows();
    _oldVehShadows = GScene->GetVehicleShadows();
    _oldSmokes = GScene->GetCloudlets();
    //_oldOcclusions;

    _oldBlood = ENGINE_CONFIG.blood;

    _oldHWTL = ENGINE_CONFIG.enableHWTL;
    _oldMultitexturing = GEngine->IsMultitexturing();

    _oldWBuffer = GEngine->IsWBuffer();

    _oldVisibility = GScene->GetPreferredViewDistance();

    _oldTerrain = GScene->GetPreferredTerrainGrid();

    {
        C3DSlider* ctrl = dynamic_cast<C3DSlider*>(GetCtrl(IDC_OPTIONS_BRIGHT_SLIDER));
        if (ctrl)
        {
            OnSliderPosChanged(ctrl->IDC(), ctrl->GetThumbPos());
        }
        ctrl = dynamic_cast<C3DSlider*>(GetCtrl(IDC_OPTIONS_GAMMA_SLIDER));
        if (ctrl)
        {
            OnSliderPosChanged(ctrl->IDC(), ctrl->GetThumbPos());
        }
        ctrl = dynamic_cast<C3DSlider*>(GetCtrl(IDC_OPTIONS_RATE_SLIDER));
        if (ctrl)
        {
            OnSliderPosChanged(ctrl->IDC(), ctrl->GetThumbPos());
        }
        ctrl = dynamic_cast<C3DSlider*>(GetCtrl(IDC_OPTIONS_QUALITY_SLIDER));
        if (ctrl)
        {
            OnSliderPosChanged(ctrl->IDC(), ctrl->GetThumbPos());
        }
        ctrl = dynamic_cast<C3DSlider*>(GetCtrl(IDC_OPTIONS_VISIBILITY_SLIDER));
        if (ctrl)
        {
            OnSliderPosChanged(ctrl->IDC(), ctrl->GetThumbPos());
        }
    }
    {
        SetToggleText(this, IDC_OPTIONS_OBJSHADOWS, "OBJSHADOWS", _oldObjShadows);

        SetToggleText(this, IDC_OPTIONS_VEHSHADOWS, "VEHSHADOWS", _oldVehShadows);

        SetToggleText(this, IDC_OPTIONS_CLOUDLETS, "CLOUDLETS", _oldSmokes);

        SetToggleText(this, IDC_OPTIONS_BLOOD, "BLOOD", _oldBlood);

        SetToggleText(this, IDC_OPTIONS_HWTL, "HWTL", _oldHWTL);

        SetToggleText(this, IDC_OPTIONS_MULTITEXTURING, "MULTITEXTURING", _oldMultitexturing);

        SetToggleText(this, IDC_OPTIONS_WBUFFER, "WBUFFER", _oldWBuffer);
        // W-buffering is a fixed-function D3D feature no modern backend offers, so this control
        // is the likeliest of the lot to be absent from a config.
        if (C3DActiveText* wbuf = OptionalActiveText(this, IDC_OPTIONS_WBUFFER, "WBUFFER"))
        {
            wbuf->EnableCtrl(GEngine->CanWBuffer());
        }
    }

    _initializingControls = true;
    int res = InitResolutions();
    if (res >= 0)
    {
        UpdateRefreshRates(res);
    }
    _initializingControls = false;
}

struct TerrainInterpolationInfo
{
    float value;
    int& ids;

    TerrainInterpolationInfo(float v, int& i) : ids(i) { value = v; }
};
static TerrainInterpolationInfo terrainInfos[] = {
    TerrainInterpolationInfo(3.125, IDS_TERRAIN_3_125), TerrainInterpolationInfo(6.25, IDS_TERRAIN_6_25),
    TerrainInterpolationInfo(12.5, IDS_TERRAIN_12_5),   TerrainInterpolationInfo(25, IDS_TERRAIN_25),
    TerrainInterpolationInfo(50, IDS_TERRAIN_50),
};

Control* DisplayOptionsVideo::OnCreateCtrl(int type, int idc, const ParamEntry& cls)
{
    switch (idc)
    {
        case IDC_OPTIONS_GAMMA_SLIDER:
        {
            PoseidonAssert(type == CT_3DSLIDER);
            C3DSlider* ctrl = new C3DSlider(this, idc, cls);
            ctrl->SetRange(0.5, 2.3);
            ctrl->SetSpeed(0.05, 0.25);
            ctrl->SetThumbPos(GEngine->GetGamma());
            //			ctrl->OnPosChanged(ctrl->GetPos());
            return ctrl;
        }
        case IDC_OPTIONS_BRIGHT_SLIDER:
        {
            PoseidonAssert(type == CT_3DSLIDER);
            C3DSlider* ctrl = new C3DSlider(this, idc, cls);
            ctrl->SetRange(0.4, 1.8);
            ctrl->SetSpeed(0.05, 0.25);
            ctrl->SetThumbPos(GEngine->GetBrightness());
            //			ctrl->OnPosChanged(ctrl->GetPos());
            return ctrl;
        }
        case IDC_OPTIONS_RATE_SLIDER:
        {
            PoseidonAssert(type == CT_3DSLIDER);
            C3DSlider* ctrl = new C3DSlider(this, idc, cls);
            //			ctrl->OnPosChanged(ctrl->GetPos());
            ctrl->SetRange(10, 30);
            // ctrl->SetSpeed(0.05, 0.25);
            ctrl->SetThumbPos(GScene->GetFrameRateSettings());
            return ctrl;
        }
        case IDC_OPTIONS_QUALITY_SLIDER:
        {
            PoseidonAssert(type == CT_3DSLIDER);
            C3DSlider* ctrl = new C3DSlider(this, idc, cls);
            // ctrl->SetRange(-log(1.0), -log(0.005));
            ctrl->SetRange(-log(0.05), -log(0.005));
            float value = GScene->GetQualitySettings();
            ctrl->SetThumbPos(-log(value));
            return ctrl;
        }
        case IDC_OPTIONS_VISIBILITY_SLIDER:
        {
            PoseidonAssert(type == CT_3DSLIDER);
            C3DSlider* ctrl = new C3DSlider(this, idc, cls);
            // Was a hardcoded log(5000). It has to track kMaxViewDistance, or the classic
            // Options page silently clamps a 10 km setting made on the modern page back down
            // the moment this slider is built -- and the modern page is the one that is
            // absent on an install that skipped Install.ps1, so the classic page is what a
            // fair number of people actually use.
            ctrl->SetRange(log(500), log(GameSettingsConfig::kMaxViewDistance));
            float range = ctrl->GetRange();
            ctrl->SetSpeed(0.01 * range, 0.1 * range);
            float value = GScene->GetPreferredViewDistance();
            ctrl->SetThumbPos(log(value));
            return ctrl;
        }
        case IDC_OPTIONS_TERRAIN:
        {
            PoseidonAssert(type == CT_3DLISTBOX);
            C3DListBox* lbox = new C3DListBox(this, idc, cls);

            lbox->ClearStrings();
            int sel = 0, selI = 0;
            float value = GScene->GetPreferredTerrainGrid();
            float minValue = GScene->GetMinimalTerrainGrid();
            float selDiff = 1e10;
            for (int i = 0; i < sizeof(terrainInfos) / sizeof(TerrainInterpolationInfo); i++)
            {
                if (terrainInfos[i].value < minValue)
                {
                    continue;
                }
                int index = lbox->AddString(LocalizeString(terrainInfos[i].ids));
                lbox->SetValue(index, i);
                float diff = fabs(terrainInfos[i].value - value);
                if (diff < selDiff)
                {
                    selDiff = diff, sel = selI;
                }
                selI++;
            }
            lbox->SetCurSel(sel);
            return lbox;
        }
        default:
            return Display::OnCreateCtrl(type, idc, cls);
    }
}

void DisplayOptionsVideo::OnSliderPosChanged(int idc, float pos)
{
    C3DStatic* ctrl;
    char buffer[128];
    switch (idc)
    {
        case IDC_OPTIONS_GAMMA_SLIDER:
            GEngine->SetGamma(pos);
            ctrl = dynamic_cast<C3DStatic*>(GetCtrl(IDC_OPTIONS_GAMMA_VALUE));
            snprintf(buffer, sizeof(buffer), "%.1f", pos);
            if (ctrl)
            {
                ctrl->SetText(buffer);
            }
            break;
        case IDC_OPTIONS_BRIGHT_SLIDER:
            GEngine->SetBrightness(pos);
            ctrl = dynamic_cast<C3DStatic*>(GetCtrl(IDC_OPTIONS_BRIGHT_VALUE));
            snprintf(buffer, sizeof(buffer), "%.1f", pos);
            if (ctrl)
            {
                ctrl->SetText(buffer);
            }
            break;
        case IDC_OPTIONS_RATE_SLIDER:
            GScene->SetFrameRateSettings(pos);
            ctrl = dynamic_cast<C3DStatic*>(GetCtrl(IDC_OPTIONS_RATE_VALUE));
            if (ctrl)
            {
                ctrl->SetText(GScene->GetFrameRateText());
            }
            break;
        case IDC_OPTIONS_QUALITY_SLIDER:
            GScene->SetQualitySettings(exp(-pos));
            ctrl = dynamic_cast<C3DStatic*>(GetCtrl(IDC_OPTIONS_QUALITY_VALUE));
            if (ctrl)
            {
                ctrl->SetText(GScene->GetQualityText());
            }
            break;
        case IDC_OPTIONS_VISIBILITY_SLIDER:
            GScene->SetPreferredViewDistance(exp(pos));
            ctrl = dynamic_cast<C3DStatic*>(GetCtrl(IDC_OPTIONS_VISIBILITY_VALUE));
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%.0f", GScene->GetPreferredViewDistance());
            if (ctrl)
            {
                ctrl->SetText(buffer);
            }
            UpdateTerrainGrids();
            break;
    }
}

void DisplayOptionsVideo::OnButtonClicked(int idc)
{
    switch (idc)
    {
        case IDC_OK:
        {
            ParamFile userCfg;
            userCfg.Parse(GetUserParams());
            userCfg.Add("blood", ENGINE_CONFIG.blood);
            userCfg.Save(GetUserParams());
        }
            Exit(idc);
            break;
        case IDC_CANCEL:
            GEngine->SetWindowMode(_oldWindowed ? WindowMode::Windowed : WindowMode::Borderless);
            if (!_oldWindowed)
            {
                GEngine->SwitchRes(_oldResX, _oldResY, _oldBpp);
            }
            GEngine->SwitchRefreshRate(_oldRefreshRate);
            GEngine->SetBrightness(_oldBright);
            GEngine->SetGamma(_oldGamma);
            GScene->SetFrameRateSettings(_oldFrameRate);
            GScene->SetQualitySettings(_oldQuality);
            SetHWTL(_oldHWTL);
            GScene->SetObjectShadows(_oldObjShadows);
            GScene->SetVehicleShadows(_oldVehShadows);
            GScene->SetCloudlets(_oldSmokes);
            ENGINE_CONFIG.blood = _oldBlood;
            GEngine->SetMultitexturing(_oldMultitexturing);
            if (GEngine->CanWBuffer())
            {
                GEngine->SetWBuffer(_oldWBuffer);
            }
            GScene->SetPreferredViewDistance(_oldVisibility);
            GScene->SetPreferredTerrainGrid(_oldTerrain);
            Exit(idc);
            break;
        case IDC_OPTIONS_OBJSHADOWS:
        {
            C3DActiveText* ctrl = OptionalActiveText(this, IDC_OPTIONS_OBJSHADOWS, "OBJSHADOWS");
            bool set = !GScene->GetObjectShadows();
            GScene->SetObjectShadows(set);
            if (ctrl)
            {
                ctrl->SetText(LocalizeString(set ? IDS_ENABLED : IDS_DISABLED));
            }
        }
        break;
        case IDC_OPTIONS_VEHSHADOWS:
        {
            C3DActiveText* ctrl = OptionalActiveText(this, IDC_OPTIONS_VEHSHADOWS, "VEHSHADOWS");
            bool set = !GScene->GetVehicleShadows();
            GScene->SetVehicleShadows(set);
            if (ctrl)
            {
                ctrl->SetText(LocalizeString(set ? IDS_ENABLED : IDS_DISABLED));
            }
        }
        break;
        case IDC_OPTIONS_CLOUDLETS:
        {
            C3DActiveText* ctrl = OptionalActiveText(this, IDC_OPTIONS_CLOUDLETS, "CLOUDLETS");
            bool set = !GScene->GetCloudlets();
            GScene->SetCloudlets(set);
            if (ctrl)
            {
                ctrl->SetText(LocalizeString(set ? IDS_ENABLED : IDS_DISABLED));
            }
        }
        break;
        case IDC_OPTIONS_BLOOD:
        {
            C3DActiveText* ctrl = OptionalActiveText(this, IDC_OPTIONS_BLOOD, "BLOOD");
            ENGINE_CONFIG.blood = !ENGINE_CONFIG.blood;
            if (ctrl)
            {
                ctrl->SetText(LocalizeString(ENGINE_CONFIG.blood ? IDS_ENABLED : IDS_DISABLED));
            }
        }
        break;
        case IDC_OPTIONS_HWTL:
        {
            // only indication: impossible to toggle
            // C3DActiveText *ctrl = static_cast<C3DActiveText *>(GetCtrl(IDC_OPTIONS_HWTL));

            // ctrl->SetText(LocalizeString(EnableHWTL ? IDS_ENABLED : IDS_DISABLED));
        }
        break;
        case IDC_OPTIONS_MULTITEXTURING:
        {
            C3DActiveText* ctrl = OptionalActiveText(this, IDC_OPTIONS_MULTITEXTURING, "MULTITEXTURING");
            bool set = !GEngine->IsMultitexturing();
            GEngine->SetMultitexturing(set);
            if (ctrl)
            {
                ctrl->SetText(LocalizeString(set ? IDS_ENABLED : IDS_DISABLED));
            }
        }
        break;
        case IDC_OPTIONS_WBUFFER:
            if (GEngine->CanWBuffer())
            {
                C3DActiveText* ctrl = OptionalActiveText(this, IDC_OPTIONS_WBUFFER, "WBUFFER");
                bool set = !GEngine->IsWBuffer();
                GEngine->SetWBuffer(set);
                if (ctrl)
                {
                    ctrl->SetText(LocalizeString(set ? IDS_ENABLED : IDS_DISABLED));
                }
            }
            break;
        default:
            Display::OnButtonClicked(idc);
    }
}

void DisplayOptionsVideo::OnLBSelChanged(int idc, int curSel)
{
    if (curSel < 0 || _initializingControls)
    {
        return;
    }

    switch (idc)
    {
        case IDC_OPTIONS_RESOLUTION:
        {
            ResolutionInfo& info = _resolutions[curSel];
            if (info.w == 0 && info.h == 0 && info.bpp == 0)
            {
                GEngine->SetWindowMode(WindowMode::Windowed);
            }
            else
            {
                GEngine->SetWindowMode(WindowMode::Borderless);
                GEngine->SwitchRes(info.w, info.h, info.bpp);
            }
            UpdateRefreshRates(curSel);
        }
        break;
        case IDC_OPTIONS_REFRESH:
        {
            C3DListBox* lbox = static_cast<C3DListBox*>(GetCtrl(IDC_OPTIONS_REFRESH));
            if (lbox)
            {
                GEngine->SwitchRefreshRate(lbox->GetValue(curSel));
            }
        }
        break;
        case IDC_OPTIONS_TERRAIN:
        {
            C3DListBox* lbox = static_cast<C3DListBox*>(GetCtrl(IDC_OPTIONS_TERRAIN));
            if (lbox)
            {
                int value = lbox->GetValue(curSel);
                GScene->SetPreferredTerrainGrid(terrainInfos[value].value);
            }
        }
        break;
    }
}

int DisplayOptionsVideo::InitResolutions()
{
    C3DListBox* lbox = static_cast<C3DListBox*>(GetCtrl(IDC_OPTIONS_RESOLUTION));
    if (!lbox)
    {
        return -1;
    }

    GEngine->ListResolutions(_resolutions);
    int sel = 0;
    for (int i = 0; i < _resolutions.Size(); i++)
    {
        ResolutionInfo& info = _resolutions[i];
        char buffer[256];
        snprintf(buffer, sizeof(buffer), LocalizeString(IDS_RESOLUTION_FORMAT), info.w, info.h, info.bpp);
        lbox->AddString(buffer);
        if (info.w == _oldResX && info.h == _oldResY && info.bpp == _oldBpp)
        {
            sel = i;
        }
    }
    if (_resolutions.Size() == 0)
    {
        return -1;
    }
    lbox->SetCurSel(sel);
    return sel;
}

void DisplayOptionsVideo::UpdateRefreshRates(int res)
{
    C3DListBox* lbox = static_cast<C3DListBox*>(GetCtrl(IDC_OPTIONS_REFRESH));
    if (!lbox)
    {
        return;
    }

    int rate;
    int sel = lbox->GetCurSel();
    if (sel >= 0)
    {
        rate = lbox->GetValue(sel);
    }
    else
    {
        rate = _oldRefreshRate;
    }

    lbox->ClearStrings();
    GEngine->ListRefreshRates(_refreshRates);
    if (_refreshRates.Size() == 0)
    {
        return;
    }
    sel = -1;
    for (int i = 0; i < _refreshRates.Size(); i++)
    {
        int value = _refreshRates[i];
        char buffer[256];
        if (value == 0)
        {
            snprintf(buffer, sizeof(buffer), LocalizeString(IDS_DISP_DEFAULT), value);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), LocalizeString(IDS_REFRESH_RATE_FORMAT), value);
        }
        int index = lbox->AddString(buffer);
        lbox->SetValue(index, value);
        if (value == rate)
        {
            sel = i;
        }
    }
    if (sel >= 0)
    {
        lbox->SetCurSel(sel);
    }
    else
    {
        lbox->SetCurSel(0);
        GEngine->SwitchRefreshRate(lbox->GetValue(0));
    }
}

void DisplayOptionsVideo::UpdateTerrainGrids()
{
    C3DListBox* lbox = static_cast<C3DListBox*>(GetCtrl(IDC_OPTIONS_TERRAIN));
    if (!lbox)
    {
        return;
    }

    lbox->ClearStrings();
    int sel = 0;
    float value = GScene->GetPreferredTerrainGrid();
    float minValue = GScene->GetMinimalTerrainGrid();
    float selDiff = 1e10;
    int selI = 0;
    for (int i = 0; i < sizeof(terrainInfos) / sizeof(TerrainInterpolationInfo); i++)
    {
        if (terrainInfos[i].value < minValue)
        {
            continue;
        }
        int index = lbox->AddString(LocalizeString(terrainInfos[i].ids));
        lbox->SetValue(index, i);
        float diff = fabs(terrainInfos[i].value - value);
        if (diff < selDiff)
        {
            selDiff = diff, sel = selI;
        }
        selI++;
    }
    LOG_DEBUG(UI, "Sel {} of {}", sel, selI);
    lbox->SetCurSel(sel);
}

// Audio options display
DisplayOptionsAudio::DisplayOptionsAudio(ControlsContainer* parent, bool enableSimulation) : Display(parent)
{
    _enableSimulation = enableSimulation;
    Load("RscDisplayOptionsAudio");

    if (GSoundsys)
    {
        _oldMusic = GSoundsys->GetCDVolume();
        _oldEffects = GSoundsys->GetWaveVolume();
        _oldVoices = GSoundsys->GetSpeechVolume();
        // _oldSamplingRate;
        _oldHWAcc = GSoundsys->GetHWAccel();
        _oldEAX = GSoundsys->GetEAX();
    }
    _oldSingleVoice = ENGINE_CONFIG.singleVoice;

    {
        C3DSlider* ctrl = dynamic_cast<C3DSlider*>(GetCtrl(IDC_OPTIONS_MUSIC_SLIDER));
        if (ctrl)
        {
            OnSliderPosChanged(ctrl->IDC(), ctrl->GetThumbPos());
        }
        ctrl = dynamic_cast<C3DSlider*>(GetCtrl(IDC_OPTIONS_EFFECTS_SLIDER));
        if (ctrl)
        {
            OnSliderPosChanged(ctrl->IDC(), ctrl->GetThumbPos());
        }
        ctrl = dynamic_cast<C3DSlider*>(GetCtrl(IDC_OPTIONS_VOICES_SLIDER));
        if (ctrl)
        {
            OnSliderPosChanged(ctrl->IDC(), ctrl->GetThumbPos());
        }
    }
    {
        SetToggleText(this, IDC_OPTIONS_HWACC, "HWACC", _oldHWAcc);

        SetToggleText(this, IDC_OPTIONS_EAX, "EAX", _oldEAX);

        // Not an ENABLED/DISABLED pair, so it keeps its own labels rather than SetToggleText.
        if (C3DActiveText* voice = OptionalActiveText(this, IDC_OPTIONS_SINGLE_VOICE, "SINGLE_VOICE"))
        {
            voice->SetText(LocalizeString(_oldSingleVoice ? IDS_SINGLE_VOICE : IDS_ALL_VOICES));
        }
    }
}

Control* DisplayOptionsAudio::OnCreateCtrl(int type, int idc, const ParamEntry& cls)
{
    switch (idc)
    {
        case IDC_OPTIONS_MUSIC_SLIDER:
        {
            PoseidonAssert(type == CT_3DSLIDER);
            C3DSlider* ctrl = new C3DSlider(this, idc, cls);
            ctrl->SetThumbPos(GSoundsys->GetCDVolume());
            //			ctrl->OnPosChanged(ctrl->GetPos());
            return ctrl;
        }
        case IDC_OPTIONS_EFFECTS_SLIDER:
        {
            PoseidonAssert(type == CT_3DSLIDER);
            C3DSlider* ctrl = new C3DSlider(this, idc, cls);
            ctrl->SetThumbPos(GSoundsys->GetWaveVolume());
            //			ctrl->OnPosChanged(ctrl->GetPos());
            return ctrl;
        }
        case IDC_OPTIONS_VOICES_SLIDER:
        {
            PoseidonAssert(type == CT_3DSLIDER);
            C3DSlider* ctrl = new C3DSlider(this, idc, cls);
            ctrl->SetThumbPos(GSoundsys->GetSpeechVolume());
            //			ctrl->OnPosChanged(ctrl->GetPos());
            return ctrl;
        }
        default:
            return Display::OnCreateCtrl(type, idc, cls);
    }
}

void DisplayOptionsAudio::OnSliderPosChanged(int idc, float pos)
{
    C3DStatic* ctrl;
    char buffer[128];
    switch (idc)
    {
        case IDC_OPTIONS_MUSIC_SLIDER:
            GSoundsys->SetCDVolume(pos);
            GSoundsys->StartPreview();
            ctrl = dynamic_cast<C3DStatic*>(GetCtrl(IDC_OPTIONS_MUSIC_VALUE));
            snprintf(buffer, sizeof(buffer), "%.1f", pos);
            if (ctrl)
            {
                ctrl->SetText(buffer);
            }
            break;
        case IDC_OPTIONS_EFFECTS_SLIDER:
            GSoundsys->SetWaveVolume(pos);
            GSoundsys->StartPreview();
            ctrl = dynamic_cast<C3DStatic*>(GetCtrl(IDC_OPTIONS_EFFECTS_VALUE));
            snprintf(buffer, sizeof(buffer), "%.1f", pos);
            if (ctrl)
            {
                ctrl->SetText(buffer);
            }
            break;
        case IDC_OPTIONS_VOICES_SLIDER:
            GSoundsys->SetSpeechVolume(pos);
            GSoundsys->StartPreview();
            ctrl = dynamic_cast<C3DStatic*>(GetCtrl(IDC_OPTIONS_VOICES_VALUE));
            snprintf(buffer, sizeof(buffer), "%.1f", pos);
            if (ctrl)
            {
                ctrl->SetText(buffer);
            }
            break;
    }
}

void DisplayOptionsAudio::OnButtonClicked(int idc)
{
    switch (idc)
    {
        case IDC_OK:
        {
            GSoundsys->TerminatePreview();
            Exit(idc);
        }
        break;
        case IDC_CANCEL:
            GSoundsys->SetCDVolume(_oldMusic);
            GSoundsys->SetWaveVolume(_oldEffects);
            GSoundsys->SetSpeechVolume(_oldVoices);
            GSoundsys->EnableEAX(_oldEAX);
            GSoundsys->EnableHWAccel(_oldHWAcc);
            ENGINE_CONFIG.singleVoice = _oldSingleVoice;
            GSoundsys->TerminatePreview();
            Exit(idc);
            break;
        case IDC_OPTIONS_HWACC:
        {
            C3DActiveText* ctrl = OptionalActiveText(this, IDC_OPTIONS_HWACC, "HWACC");
            bool val = GSoundsys->GetHWAccel();
            val = GSoundsys->EnableHWAccel(!val);
            if (ctrl)
            {
                ctrl->SetText(LocalizeString(val ? IDS_ENABLED : IDS_DISABLED));
            }
        }
        break;
        case IDC_OPTIONS_EAX:
        {
            C3DActiveText* ctrl = OptionalActiveText(this, IDC_OPTIONS_EAX, "EAX");
            bool val = GSoundsys->GetEAX();
            val = GSoundsys->EnableEAX(!val);
            if (ctrl)
            {
                ctrl->SetText(LocalizeString(val ? IDS_ENABLED : IDS_DISABLED));
            }
        }
        break;
        case IDC_OPTIONS_SINGLE_VOICE:
        {
            ENGINE_CONFIG.singleVoice = !ENGINE_CONFIG.singleVoice;
            C3DActiveText* ctrl = OptionalActiveText(this, IDC_OPTIONS_SINGLE_VOICE, "SINGLE_VOICE");
            if (ENGINE_CONFIG.singleVoice)
            {
                if (ctrl)
                {
                    ctrl->SetText(LocalizeString(IDS_SINGLE_VOICE));
                }
            }
            else
            {
                if (ctrl)
                {
                    ctrl->SetText(LocalizeString(IDS_ALL_VOICES));
                }
            }
        }
        break;
        default:
            Display::OnButtonClicked(idc);
    }
}

// Difficulty display
DisplayDifficulty::DisplayDifficulty(ControlsContainer* parent, bool enableSimulation) : Display(parent)
{
    _enableSimulation = enableSimulation;
    _diff = nullptr;
    Load("RscDisplayDifficulty");

    _oldTitles = USER_CONFIG.showTitles;
    _oldRadio = GChatList.Enabled();

    {
        C3DActiveText* ctrl = OptionalActiveText(this, IDC_OPTIONS_SUBTITLES, "SUBTITLES");
        if (USER_CONFIG.showTitles)
        {
            if (ctrl)
            {
                ctrl->SetText(LocalizeString(IDS_OPT_SUBTITLES_ENABLED));
            }
        }
        else
        {
            if (ctrl)
            {
                ctrl->SetText(LocalizeString(IDS_OPT_SUBTITLES_DISABLED));
            }
        }

        ctrl = OptionalActiveText(this, IDC_OPTIONS_RADIO, "RADIO");
        if (GChatList.Enabled())
        {
            if (ctrl)
            {
                ctrl->SetText(LocalizeString(IDS_OPT_RADIO_ENABLED));
            }
        }
        else
        {
            if (ctrl)
            {
                ctrl->SetText(LocalizeString(IDS_OPT_RADIO_DISABLED));
            }
        }
    }
}

Control* DisplayDifficulty::OnCreateCtrl(int type, int idc, const ParamEntry& cls)
{
    switch (idc)
    {
        case IDC_DIFFICULTIES_DIFFICULTIES:
            _diff = new CDifficulties(this, idc, cls);
            for (int i = 0; i < DTN; i++)
            {
                _diff->AddString(LocalizeString(Config::diffDesc[i].stringId));
                _diff->cadetDifficulty[i] = USER_CONFIG.cadetDifficulty[i];
                _diff->veteranDifficulty[i] = USER_CONFIG.veteranDifficulty[i];
            }
            _diff->SetCurSel(0);
            return _diff;
        default:
            return Display::OnCreateCtrl(type, idc, cls);
    }
}

void DisplayDifficulty::OnButtonClicked(int idc)
{
    switch (idc)
    {
        case IDC_DIFFICULTIES_DEFAULT:
            for (int i = 0; i < DTN; i++)
            {
                _diff->cadetDifficulty[i] = Config::diffDesc[i].defaultCadet;
                _diff->veteranDifficulty[i] = Config::diffDesc[i].defaultVeteran;
            }
            break;
        case IDC_OK:
        {
            for (int i = 0; i < DTN; i++)
            {
                USER_CONFIG.cadetDifficulty[i] = _diff->cadetDifficulty[i];
                USER_CONFIG.veteranDifficulty[i] = _diff->veteranDifficulty[i];
            }
            UserConfig_SaveDifficulties(USER_CONFIG);
        }
            Exit(idc);
            break;
        case IDC_CANCEL:
            USER_CONFIG.showTitles = _oldTitles;
            GChatList.Enable(_oldRadio);
            Exit(idc);
            break;
        case IDC_OPTIONS_SUBTITLES:
        {
            USER_CONFIG.showTitles = !USER_CONFIG.showTitles;
            C3DActiveText* ctrl = OptionalActiveText(this, IDC_OPTIONS_SUBTITLES, "SUBTITLES");
            if (USER_CONFIG.showTitles)
            {
                if (ctrl)
                {
                    ctrl->SetText(LocalizeString(IDS_OPT_SUBTITLES_ENABLED));
                }
            }
            else
            {
                if (ctrl)
                {
                    ctrl->SetText(LocalizeString(IDS_OPT_SUBTITLES_DISABLED));
                }
            }
        }
        break;
        case IDC_OPTIONS_RADIO:
        {
            GChatList.Enable(!GChatList.Enabled());
            C3DActiveText* ctrl = OptionalActiveText(this, IDC_OPTIONS_RADIO, "RADIO");
            if (GChatList.Enabled())
            {
                if (ctrl)
                {
                    ctrl->SetText(LocalizeString(IDS_OPT_RADIO_ENABLED));
                }
            }
            else
            {
                if (ctrl)
                {
                    ctrl->SetText(LocalizeString(IDS_OPT_RADIO_DISABLED));
                }
            }
        }
        break;
        default:
            Display::OnButtonClicked(idc);
    }
}

CDifficulties::CDifficulties(ControlsContainer* parent, int idc, const ParamEntry& cls) : C3DListBox(parent, idc, cls)
{
    _sb3DWidth = 0.05;
    _column = 0;
}

bool CDifficulties::OnKeyDown(unsigned nChar, unsigned nRepCnt, unsigned nFlags)
{
    switch (nChar)
    {
        case SDLK_LEFT:
            _column--;
            if (_column < 0)
            {
                _column = 2;
            }
            return true;
        case SDLK_RIGHT:
            _column++;
            if (_column > 2)
            {
                _column = 0;
            }
            return true;
        case SDLK_SPACE:
            Toggle(_column, GetCurSel());
            return true;
        default:
            return C3DListBox::OnKeyDown(nChar, nRepCnt, nFlags);
    }
}

void CDifficulties::OnLButtonDown(float x, float y)
{
    IsInside(x, y);
    if (_scrollbar.IsEnabled())
    {
        if (_u > 1.0 - _sb3DWidth)
        {
            _scrollbar.SetPos(_topString);
            _scrollbar.OnLButtonDown(_v);
            _topString = _scrollbar.GetPos();
            return;
        }
        else
        {
            _u *= 1.0 / (1.0 - _sb3DWidth);
        }
    }

    float index = _v * _rows;
    if (index >= 0 && index < _rows)
    {
        _parent->OnLBDrag(IDC(), toIntFloor(_topString + index));
        _dragging = true;
    }
    if (_u < 0.6)
    {
        _column = 0;
    }
    else if (_u < 0.8)
    {
        _column = 1;
        Toggle(1, index);
    }
    else
    {
        _column = 2;
        Toggle(2, index);
    }
}

inline PackedColor ModAlpha(PackedColor color, float alpha)
{
    int a = toInt(alpha * color.A8());
    saturate(a, 0, 255);
    return PackedColorRGB(color, a);
}

void CDifficulties::DrawItem(Vector3Par position, Vector3Par down, int i, float alpha)
{
    float y1c = 0;
    float y2c = 1;
    if (i < _topString)
    {
        y1c = _topString - i;
    }
    if (i > _topString + _rows - 1)
    {
        y2c = _topString + _rows - i;
    }

    Vector3 normal = _down.CrossProduct(_right).Normalized();

    Vector3 rightSB = _right;
    if (GetSize() > _rows)
    {
        rightSB = (1.0 - _sb3DWidth) * _right;
    }
    float rightSBSize = rightSB.Size();
    Vector3 border = 0.02 * _right;

    bool selected = i == GetCurSel() && IsEnabled();
    PackedColor color = ModAlpha(GetFtColor(i), alpha);
    PackedColor selColor = color;
    if (selected && _showSelected)
    {
        PackedColor selBgColor = ModAlpha(_selBgColor, alpha);
        switch (_column)
        {
            case 0:
                GEngine->Draw3D(position, down, 0.6 * rightSB, ClipAll, selBgColor, DisableSun, nullptr, 0, y1c, 1,
                                y2c);
                break;
            case 1:
                GEngine->Draw3D(position + 0.6 * rightSB, down, 0.2 * rightSB, ClipAll, selBgColor, DisableSun, nullptr,
                                0, y1c, 1, y2c);
                break;
            case 2:
                GEngine->Draw3D(position + 0.8 * rightSB, down, 0.2 * rightSB, ClipAll, selBgColor, DisableSun, nullptr,
                                0, y1c, 1, y2c);
                break;
        }
        selColor = ModAlpha(GetSelColor(i), alpha);
    }

    Vector3 curPos = position - 0.002 * normal;

    Texture* texture = GetTexture(i);
    if (texture)
    {
        Vector3 right = (float)texture->AWidth() / (float)texture->AHeight() * down.Size() * _right.Normalized();
        float rightSize = right.Size();
        float x2c = 1;
        if (rightSize > rightSBSize)
        {
            x2c = rightSBSize / rightSize;
        }
        GEngine->Draw3D(curPos, down, right, ClipAll, color, // PackedColor(Color(1, 1, 1, alpha)),
                        DisableSun, texture, 0, y1c, x2c, y2c);
        curPos += right;
        rightSBSize -= rightSize;
        if (rightSBSize <= 0)
        {
            return;
        }
    }

    float top = 0.5 * (1.0 - _size);
    Vector3 pos = curPos + top * down + border;
    Vector3 up = -_size * down;
    Vector3 right = 0.75 * up.Size() * _right.Normalized();
    float invRightSize = 1.0 / right.Size();
    float x2ct = rightSBSize * invRightSize;
    float y1ct = 0;
    float y2ct = 1;
    if (y1c > top)
    {
        y1ct = (y1c - top) / _size;
    }
    if (y2c < top + _size)
    {
        y2ct = (y2c - top) / _size;
    }

    RString text = GetText(i);
    float x2c = 0.6 * x2ct - 2.0 * invRightSize * border.Size();
    PackedColor col = _column == 0 ? selColor : color;
    GEngine->DrawText3D(pos, up, right, ClipAll, _font, col, DisableSun, text, 0, y1ct, x2c, y2ct);
    pos += 0.6 * rightSB;

    x2c = 0.2 * x2ct - 2.0 * invRightSize * border.Size();
    text = cadetDifficulty[i] ? LocalizeString(IDS_ENABLED) : LocalizeString(IDS_DISABLED);
    col = _column == 1 ? selColor : color;
    GEngine->DrawText3D(pos, up, right, ClipAll, _font, col, DisableSun, text, 0, y1ct, x2c, y2ct);
    pos += 0.2 * rightSB;

    text = veteranDifficulty[i] ? LocalizeString(IDS_ENABLED) : LocalizeString(IDS_DISABLED);
    if (Config::diffDesc[i].enabledInVeteran)
    {
        col = _column == 2 ? selColor : color;
    }
    else
    {
        col = PackedColor(Color(1, 0, 0, alpha));
    }
    GEngine->DrawText3D(pos, up, right, ClipAll, _font, col, DisableSun, text, 0, y1ct, x2c, y2ct);
}

} // namespace Poseidon
