#include <Poseidon/Core/Game/GameLoop.hpp> // LockstepPacingActive (SIM-815)
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/Weather/MudField.hpp>
#include <Poseidon/World/Weather/SandField.hpp>
#include <Poseidon/World/Weather/RainWaterField.hpp>
#include <Poseidon/World/Weather/RainVolume.hpp>
#include <Poseidon/Dev/Diag/FixedStepStats.hpp>
#include <Poseidon/Dev/Diag/PhysicsProbe.hpp>
#include <Poseidon/Dev/Diag/TerrainBrush.hpp>
#include <Poseidon/Dev/Diag/CaveEditor.hpp>
#include <Poseidon/UI/Locale/Stringtable/CodepageTranscode.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Core/Config/UserConfig.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/UI/Settings/AspectRatio.hpp>
#include <Poseidon/Graphics/Cursor/GameCursorOverlay.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Audio/Speaker.hpp>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Memtype.h>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>
#ifdef _WIN32
#include <windows.h>
#endif

#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/Visibility.hpp>
#include <Poseidon/World/Model/ModelDerivedCache.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/World/Scene/Camera/CamEffects.hpp>
#include <Poseidon/Game/Scripting/Scripts.hpp>

#include <Poseidon/Core/Progress.hpp>

#include <Poseidon/World/Entities/Vehicles/SeaGull.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>

#include <Poseidon/World/Entities/Weapons/Shots.hpp>
#include <Poseidon/AI/AI.hpp>

#include <Poseidon/IO/Serialization/ThreadSync.hpp>

#include <Poseidon/Network/Network.hpp>

#include <Poseidon/Game/Chat.hpp>

#include <Poseidon/AI/ArcadeTemplate.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/World/Scene/ObjectClasses.hpp>

#include <Poseidon/Game/Commands/GameStateExt.hpp>

#include <Poseidon/UI/Locale/StringtableExt.hpp>

#include <Poseidon/World/WorldShared.hpp>

#undef GetObject
#undef DrawText

#include <Poseidon/Core/resincl.hpp>

using namespace Poseidon;
extern char LoadFile[];
extern RString GMapOnSingleClick;

namespace Poseidon
{
void RunInitScript();
}

extern const char DefLoadFile[];

#ifdef _MSC_VER
#pragma warning(disable : 4355)
#endif

#define BACKGROUND_AI 0

World::World(Engine* engine, bool editor)
    : _engine(engine), _editor(editor), _camTypeMain(CamInternal), _camType(CamInternal), _gridOffsetX(0),
      _gridOffsetY(0)
{
    // A new World means a new run of the simulation clock, so the fixed-step
    // panel starts counting again. Without this the averages mix two missions
    // and every loading screen's catch-up burst is carried into the next one's
    // numbers -- which is exactly the reading that made the cap look alarming
    // before the counters were reset by hand.
    Dev::ResetFixedStepStats();

#define PRINT_SIZE(type) Log(#type " size %d", sizeof(type))
    PRINT_SIZE(Object);
    PRINT_SIZE(Frame);
    PRINT_SIZE(RemoveLinks);
    PRINT_SIZE(OLink<Object>);
    PRINT_SIZE(Entity);
    PRINT_SIZE(LODShapeWithShadow);
    PRINT_SIZE(Shape);
    if (editor)
    {
        _camType = _camTypeMain = CamInternal;
    }
#if _PIII
    PoseidonAssert(((int)this & 0xf) == 0);
#endif
#if BACKGROUND_AI
    _secThread = new SecondaryThread;
#endif

    _radio = new RadioChannel(CCGlobal, nullptr, RNRadio);
    //	_speaker = toIntFloor(1000 * GRandGen.RandomValue()) + 1;
    SetSpeaker((Pars >> "CfgVoice" >> "voices")[0], 1.0);

    _scene.Init(_engine, nullptr);

    GLandscape = _scene.GetLandscape();
    GScene = GetScene();

    _firstFrame = false;
    _latitude = -40 * (H_PI / 180); // -40 - Croatia, -90 - north pole;
    _longitude = +15 * (H_PI / 180);

    Vector3 lightDirection(+1, -0.5, +1);
    LightSun* mainLight = new LightSun();

    // SIM-815: this wall-clock reseed is what kept two "identical" runs on different RNG
    // streams even after the constructor's seed was pinned -- it runs at world init and
    // silently undid the fix, which is why the tick-hash experiment still parted at the
    // same tick. Under lockstep pacing the seed is fixed here too; normal play keeps the
    // wall clock, because missions repeating identically would be a gameplay change.
    if (Poseidon::LockstepPacingActive())
    {
        GRandGen.SetSeed(1337);
    }
    else
    {
#ifdef _WIN32
        SYSTEMTIME time;
        GetLocalTime(&time);
        GRandGen.SetSeed(Poseidon::Foundation::GlobalTickCount() + time.wSecond);
#else
        GRandGen.SetSeed(Poseidon::Foundation::GlobalTickCount() + (unsigned)time(nullptr));
#endif
    }
    Glob.clock.SetTimeInYear(8 * OneHour + 130 * OneDay);
    mainLight->Recalculate(this);
    _timeToSkip = 0;

    // Arm the single wind authority for this world. Deliberately NOT seeded from
    // GRandGen (which was just seeded off the wall clock two lines up, and is a
    // shared mutable stream): the wind must be reproducible from mission state
    // alone, or two machines in one session would blow smoke in different
    // directions. SimulateLandscape ticks it from here on.
    GWind.Init();

    _scene.SetMainLight(mainLight);
    _scene.MainLightChanged();
    _noDisplay = false;
    _enableSimulation = true;
    _simulationFocus = true;

    // Skip UI creation for dedicated server or editor.  Viewer mode
    // also skips it: the menu Display chain (DisplayMain) would
    // otherwise paint its Arrow cursor on top of the viewer's own
    // ring cursor, and DrawHUDNonAI would paint the seagull aim
    // sprite.  Leaving `_options` and `_ui` null here lets the
    // existing `if (_options)` / `if (_ui)` checks at draw sites
    // skip those passes naturally — no IsViewerMode() gate at the
    // draw sites themselves.  See ViewerCursorOverlay for what
    // paints the cursor instead.
    bool skipUI = editor || ENGINE_CONFIG.doCreateDedicatedServer || AppConfig::Instance().IsViewerMode();
    if (!skipUI)
    {
        CreateMainOptions();
        // Install the menu cursor strategy.  Stays the default for
        // any non-viewer mode that has a UI (game, menus, mission).
        // Viewer mode installs ViewerCursorOverlay later in
        // World::StartViewer.
        if (!_cursorOverlay)
            _cursorOverlay = new GameCursorOverlay(this);
    }

    _userInputDisabled = 0;

    Clear();

    _mode = GModeArcade;

    _cameraExternal = false;

    _showCompass = false;
    _showWatch = false;

    _forceMap = false;

    if (GSoundScene)
    {
        GSoundScene->Reset();
    }
}

#ifdef _MSC_VER
#pragma warning(default : 4355)
#endif

void World::InitGeneral()
{
    _wantedOvercast = 0.3;
    _actualOvercast = _wantedOvercast;
    _wantedFog = 0.0;
    _actualFog = _wantedFog;
    _speedOvercast = 1.0 / (60 * 60);
    _speedFog = 1.0 / (60 * 60);
    _weatherTime = 0;
    _nextWeatherChange = 0;
    _firstFrame = true;

    Glob.clock.SetTimeInYear(8 * OneHour + 130 * OneDay);
    LightSun* sun = _scene.MainLight();
    sun->Recalculate(this);
}

void World::InitGeneral(const ParamEntry& cfg)
{
    _actualOvercast = cfg >> "startWeather";
    _wantedOvercast = cfg >> "forecastWeather";
    _actualFog = cfg >> "startFog";
    _wantedFog = cfg >> "forecastFog";
    _speedOvercast = fabs(_wantedOvercast - _actualOvercast) / (30 * 60);
    _speedFog = fabs(_wantedFog - _actualFog) / (30 * 60);
    _weatherTime = 0;
    _nextWeatherChange = 30 * 60;
    _nearImportanceDistributionTime = Poseidon::Foundation::Time(0);
    _farImportanceDistributionTime = Poseidon::Foundation::Time(0);
    _firstFrame = true;

    RString time = cfg >> "startTime";
    RString date = cfg >> "startDate";
    int year;
    float timeInYear = Glob.clock.ScanDateTime(date, time, year);
    if (year < 100)
    {
        year += 1900;
    }
    Glob.clock.SetTimeInYear(timeInYear);
    Glob.clock.SetYear(year);
    LightSun* sun = _scene.MainLight();
    sun->Recalculate(this);
    _scene.MainLightChanged();
    Log("World set time %s, %s", (const char*)date, (const char*)time);
}

namespace Poseidon
{
int GetDaysInMonth(int year, int month);
}

void World::InitGeneral(ArcadeIntel& intel)
{
    _actualOvercast = intel.weather;
    _wantedOvercast = intel.weatherForecast;
    _actualFog = intel.fog;
    _wantedFog = intel.fogForecast;
    _speedOvercast = fabs(_wantedOvercast - _actualOvercast) / (30 * 60);
    _speedFog = fabs(_wantedFog - _actualFog) / (30 * 60);
    _weatherTime = 0;
    _nextWeatherChange = 30 * 60;
    _nearImportanceDistributionTime = Poseidon::Foundation::Time(0);
    _farImportanceDistributionTime = Poseidon::Foundation::Time(0);

    int day = intel.day - 1;
    int year = intel.year;
    for (int m = 0; m < intel.month - 1; m++)
    {
        day += Poseidon::GetDaysInMonth(year, m);
    }
    float time = intel.hour * OneHour + intel.minute * OneMinute + day * OneDay + 0.5 * OneSecond;
    Glob.clock.SetTimeInYear(time);
    Glob.clock.SetTimeOfDay(intel.hour, intel.minute);
    Glob.clock.SetYear(year);
    LightSun* sun = _scene.MainLight();
    sun->Recalculate(this);
    _scene.MainLightChanged();
}

bool World::GetILS(Vector3& pos, Vector3& dir, TargetSide side)
{
    pos = _ilsPosWest;
    dir = _ilsDirWest;
    return true;
}

const AutoArray<Vector3> World::GetTaxiInPath(TargetSide side)
{
    return _taxiInPathsWest;
}

const AutoArray<Vector3> World::GetTaxiOffPath(TargetSide side)
{
    return _taxiOffPathsWest;
}

#define PLAYER_ONLY 0

void World::InsertSeaGulls() {}

const int NEnvSoundPars = 6;
SoundPars EnvSoundPars[NEnvSoundPars];
SoundPars EnvSoundParsNight[NEnvSoundPars];

static void InitEnvSounds(const ParamEntry& cfg)
{
    GetValue(EnvSoundPars[0], cfg >> "Sea" >> "sound");
    GetValue(EnvSoundPars[1], cfg >> "Trees" >> "sound");
    GetValue(EnvSoundPars[2], cfg >> "Meadows" >> "sound");
    GetValue(EnvSoundPars[3], cfg >> "Hills" >> "sound");
    GetValue(EnvSoundPars[4], cfg >> "Rain" >> "sound");
    GetValue(EnvSoundPars[5], cfg >> "Default" >> "sound");
    GetValue(EnvSoundParsNight[0], cfg >> "Sea" >> "soundNight");
    GetValue(EnvSoundParsNight[1], cfg >> "Trees" >> "soundNight");
    GetValue(EnvSoundParsNight[2], cfg >> "Meadows" >> "soundNight");
    GetValue(EnvSoundParsNight[3], cfg >> "Hills" >> "soundNight");
    GetValue(EnvSoundParsNight[4], cfg >> "Rain" >> "soundNight");
    GetValue(EnvSoundParsNight[5], cfg >> "Default" >> "sound");
}

SRef<EntityAI> GDummyVehicle;
void AIGlobalInit();

void World::InitEditor(Landscape* landscape, Entity* cursor)
{
    _scene.Init(_engine, landscape);

    GScene = GetScene();
    GLandscape = _scene.GetLandscape();

    InitGeneral();

    InitFinish();
    _scene.GetLandscape()->InitObjectVehicles();

    VehicleTypes.Preload();

    EnvSoundPars[5].name = "SOUND\\$DEFAULT$.WSS";
    EnvSoundParsNight[5].name = "SOUND\\$DEFAULT$.WSS";

    const char* ext = strrchr(LoadFile, '.');
    if (ext && QIFStreamB::FileExist(LoadFile))
    {
        if (!strcmpi(ext, ".p3d"))
        {
            ReloadViewer(LoadFile, "");
            InitCameraPars();

            GLOB_ENGINE->ReinitCounters();
            return;
        }
        else
        {
            SwitchLandscape(LoadFile);
        }
    }

    {
        Vector3 cPos(2525, 5, 2925);
        cursor->SetPosition(cPos);
        AddVehicle(cursor);
        GetGameState()->VarSet("bis_buldozer_cursor", GameValueExt(cursor), true);
        _cameraOn = cursor;

        InitCameraPars();
        _scene.GetLandscape()->InitGeography();
    }

    RString initScript = RString("scripts\\editor.sqs");
    if (QIFStreamB::FileExist(initScript))
    {
        Script* script = new Script("editor.sqs", GameValue(), INT_MAX);
        AddScript(script);
        SimulateScripts();
    }
}

void World::InitCameraPars()
{
    Camera camera;
    if (_cameraOn != nullptr)
    {
        for (int i = 0; i < MaxCameraType; i++)
        {
            _cameraOn->InitVirtual((CameraType)i, _camHeading[i], _camDive[i], _camFOV[i]);
            _camHeadingWanted[i] = _camHeading[i];
            _camDiveWanted[i] = _camDive[i];
            _camFOVWanted[i] = _camFOV[i];
        }
        Matrix4 transform;

        transform = _cameraOn->WorldTransform() * _cameraOn->InsideCamera(_camType);
        transform.SetOrientation(transform.Orientation());

        camera.SetTransform(transform);
        camera.SetSpeed(_cameraOn->ObjectSpeed());
        float fov = _camFOV[_camType];
        float cNear = 0.067f / fov; // near plane scales inversely with fov; typical soldier fov ~0.85
        saturate(cNear, 0.07f, 0.2f);

        camera.SetPerspectiveForView(GEngine, cNear, _scene.GetFogMaxRange(), fov);
        camera.Adjust(GEngine);
        _scene.SetCamera(camera);
    }
}

void World::FreelookChange(bool active)
{
    if (!active)
    {
        OLink<Object> cameraVehicle = _cameraOn;

        if (cameraVehicle && !cameraVehicle->IsContinuous(_camType) && !cameraVehicle->IsExternal(_camType) &&
            !cameraVehicle->IsVirtualX(_camType))
        {
            if (_ui)
            {
                const Camera& camera = *GScene->GetCamera();
                _ui->SetCursorDirection(camera.Direction());
                _ui->SetCursorMode(false);
            }
        }
    }
}
void World::InitCenter(AICenterMode mode, AICenter* cnt)
{
    for (int i = 0; i < cnt->NGroups(); i++)
    {
        AIGroup* grp = cnt->GetGroup(i);
        if (!grp)
        {
            continue;
        }
        for (int j = 0; j < MAX_UNITS_PER_GROUP; j++)
        {
            AIUnit* unit = grp->UnitWithID(j + 1);
            if (!unit)
            {
                continue;
            }
            unit->GetPerson()->GetInfo()._initExperience = unit->GetPerson()->GetInfo()._experience;
            if (unit->IsFreeSoldier())
            {
                unit->GetPerson()->ResetMovement(0);
            }
            if (unit->IsUnit())
            {
                unit->GetVehicle()->AutoReloadAll();
            }
        }
        grp->GetRadio().SilentProcess();
    }
    if (mode == AICMNetwork)
    {
        GetNetworkManager().CreateCenter(cnt);
    }
}
bool World::InitVehicles(GameMode gameMode, ArcadeTemplate& t)
{
    DWORD tInitVehicles = GetTickCount();
    ProgressReset();
    _firstFrame = true;

    Display* disp = new Display(nullptr);
    disp->Load("RscDisplayLoadMission");
    disp->SetCursor(nullptr);
    CStatic* cName = static_cast<CStatic*>(disp->GetCtrl(IDC_LOAD_MISSION_NAME));
    CStatic* cDate = static_cast<CStatic*>(disp->GetCtrl(IDC_LOAD_MISSION_DATE));
    RString name;
    bool showTime;
    if (gameMode == GModeIntro)
    {
        const ParamEntry* entry = ExtParsMission.FindEntry("onLoadIntro");
        if (entry)
        {
            name = *entry;
        }
        else
        {
            name = LocalizeString(IDS_LOAD_INTRO);
        }
        entry = ExtParsMission.FindEntry("onLoadIntroTime");
        if (entry)
        {
            showTime = *entry;
        }
        else
        {
            showTime = false;
        }
    }
    else
    {
        const ParamEntry* entry = ExtParsMission.FindEntry("onLoadMission");
        if (entry)
        {
            name = *entry;
        }
        else
        {
            name = LocalizeString(IDS_LOAD_MISSION);
        }
        entry = ExtParsMission.FindEntry("onLoadMissionTime");
        if (entry)
        {
            showTime = *entry;
        }
        else
        {
            showTime = true;
        }
    }
    cName->SetText(DecodeLegacyTextToRString(name, GLanguage));

    if (showTime)
    {
        char buffer[256];
        Glob.clock.FormatDate(LocalizeString(IDS_DATE_FORMAT), buffer);
        float time = Glob.clock.GetTimeOfDay();
        if (time > 1.0)
        {
            time--;
        }
        time *= 24;
        int hour = toIntFloor(time);
        time -= hour;
        time *= 60;
        int min = toIntFloor(time);
        sprintf(buffer + strlen(buffer), ", % 2d:%02d", hour, min);
        cDate->SetText(buffer);
    }
    else
    {
        cDate->SetText("");
    }

    ProgressStart(disp);

    _mode = gameMode;
    // Pillarbox / "4:3 dark-around overlay" treatment is meaningful
    // only during gameplay.  Menu intro (random cutscene) shares
    // CameraEffect::Draw but should keep its scene full-width.
    AspectRatio::SetGameplayActive(gameMode != GModeIntro);
    _endMission = EMContinue;
    EnableRadio();

    AdjustSubdivision(gameMode);

    ArcadeUnitInfo* uInfo = t.FindPlayer();
    if (uInfo)
    {
        Glob.header.playerSide = (TargetSide)uInfo->side;
    }

    if (_ui)
    {
        _ui->ResetHUD();
    }

    if (!GDummyVehicle)
    {
        GDummyVehicle = NewVehicle("PaperCar");
    }

    DestroyUserDialog();

    ProgressRefresh();

    AIGlobalInit();

    AICenterMode mode = TranslateMode(gameMode);
    ProgressRefresh();
    _eastCenter.Free();
    _westCenter.Free();
    _civilianCenter.Free();
    _guerrilaCenter.Free();
    _logicCenter.Free();

    DWORD tCenters = GetTickCount();
    _eastCenter = ::CreateCenter(t, TEast, mode);
    ProgressRefresh();
    PoseidonAssert(CheckVehicleStructure());

    _westCenter = ::CreateCenter(t, TWest, mode);
    ProgressRefresh();
    PoseidonAssert(CheckVehicleStructure());

    _guerrilaCenter = ::CreateCenter(t, TGuerrila, mode);
    ProgressRefresh();
    PoseidonAssert(CheckVehicleStructure());

    _civilianCenter = ::CreateCenter(t, TCivilian, mode);
    ProgressRefresh();
    PoseidonAssert(CheckVehicleStructure());

    _logicCenter = ::CreateCenter(t, TLogic, mode);
    LOG_DEBUG(Core, "LOAD: CreateCenters {}ms", GetTickCount() - tCenters);
    ProgressRefresh();
    PoseidonAssert(CheckVehicleStructure());

    AUTO_STATIC_ARRAY(VehicleInitCmd, inits, 128);

    if (_eastCenter)
    {
        _eastCenter->Init(t, inits);
    }
    ProgressRefresh();
    if (_westCenter)
    {
        _westCenter->Init(t, inits);
    }
    ProgressRefresh();
    if (_guerrilaCenter)
    {
        _guerrilaCenter->Init(t, inits);
    }
    ProgressRefresh();
    if (_civilianCenter)
    {
        _civilianCenter->Init(t, inits);
    }
    ProgressRefresh();
    if (_logicCenter)
    {
        _logicCenter->Init(t, inits);
    }
    ProgressRefresh();
    InitNoCenters(t, inits, mode == AICMNetwork);

    PoseidonAssert(CheckVehicleStructure());

    for (int i = 0; i < inits.Size(); i++)
    {
        GGameState.VarSet("this", GameValueExt(inits[i].vehicle), true);
        GGameState.Execute(inits[i].init);
        PoseidonAssert(CheckVehicleStructure());
    }

    if (gameMode == GModeArcade)
    {
        Poseidon::RunInitScript();
    }
    PoseidonAssert(CheckVehicleStructure());

    for (int i = 0; i < inits.Size(); i++)
    {
        inits[i].vehicle->InitUnits();
        PoseidonAssert(CheckVehicleStructure());
    }

    PoseidonAssert(CheckVehicleStructure());
    if (_eastCenter)
    {
        InitCenter(mode, _eastCenter);
    }
    if (_westCenter)
    {
        InitCenter(mode, _westCenter);
    }
    if (_guerrilaCenter)
    {
        InitCenter(mode, _guerrilaCenter);
    }
    if (_civilianCenter)
    {
        InitCenter(mode, _civilianCenter);
    }
    if (_logicCenter)
    {
        InitCenter(mode, _logicCenter);
    }

    PoseidonAssert(CheckVehicleStructure());

    if (gameMode == GModeNetware)
    {
        for (int i = 0; i < inits.Size(); i++)
        {
            GetNetworkManager().VehicleInit(inits[i]);
        }
    }

    if (_eastCenter)
    {
        _eastCenter->InitSensors();
    }
    if (_westCenter)
    {
        _westCenter->InitSensors();
    }
    if (_guerrilaCenter)
    {
        _guerrilaCenter->InitSensors();
    }
    if (_civilianCenter)
    {
        _civilianCenter->InitSensors();
    }
    ProgressRefresh();
    GetSensorList()->UpdateAll();
    if (_eastCenter)
    {
        _eastCenter->InitSensors(true);
    }
    if (_westCenter)
    {
        _westCenter->InitSensors(true);
    }
    if (_guerrilaCenter)
    {
        _guerrilaCenter->InitSensors(true);
    }
    if (_civilianCenter)
    {
        _civilianCenter->InitSensors(true);
    }
    ProgressRefresh();
    VehicleTypes.UnlockAllTypes();
    GEngine->TextBank()->UnlockAllTextures();
    extern void ManCompact();
    ManCompact();

    DWORD tPreload = GetTickCount();
    _engine->TextBank()->Preload();
    LOG_DEBUG(Core, "LOAD: TextBank Preload {}ms", GetTickCount() - tPreload);
    DWORD tOpt = GetTickCount();
    Shapes.OptimizeAll();
    LOG_DEBUG(Core, "LOAD: Shapes.OptimizeAll {}ms", GetTickCount() - tOpt);

    // Pre-fill terrain segment cache in parallel before first frame. FillCache /
    // CreateNearVBuffers self-skip when a GPU terrain renderer is active (see Landscape.cpp).
    if (_cameraOn && _scene.GetLandscape())
    {
        auto& camPos = _cameraOn->Position();
        LOG_DEBUG(Core, "LOAD: Terrain FillCache start pos=({},{},{})", camPos.X(), camPos.Y(), camPos.Z());
        DWORD tFill = GetTickCount();
        _scene.GetLandscape()->FillCache(*_cameraOn);
        float warmRadius = _scene.GetPreferredViewDistance() * 0.5f;
        _scene.GetLandscape()->CreateNearVBuffers(camPos.X(), camPos.Z(), warmRadius);
        LOG_DEBUG(Core, "LOAD: Terrain FillCache+NearVBs {}ms", GetTickCount() - tFill);
    }

    _sensorList->UpdateAll();
    EntityAI* ai = dyn_cast<EntityAI, Object>(_cameraOn);
    if (_ui && ai)
    {
        _ui->ResetVehicle(ai);
    }
    ProgressRefresh();

    EnvSoundPars[5].name = "SOUND\\$DEFAULT$.WSS";
    EnvSoundParsNight[5].name = "SOUND\\$DEFAULT$.WSS";

    // Mission audio manifest preload — queued onto TaskPool workers while
    // the loading screen still owns the frame; first say/playSound then
    // hits the decoded-PCM cache instead of decoding on the main thread.
    if (GSoundScene)
    {
        GSoundScene->PreloadMissionAudio();
    }

    // Let the first in-game frames upload every touched mip at full wanted
    // level — without this the per-frame copy budget spreads the catch-up
    // over the first visible second (textures sharpen / LODs flip in view).
    if (GLOB_ENGINE && GLOB_ENGINE->TextBank())
    {
        GLOB_ENGINE->TextBank()->BoostLoadBudget(60);
    }

    ProgressFinish();
    LOG_DEBUG(Core, "LOAD: InitVehicles total {}ms", GetTickCount() - tInitVehicles);
    GLOB_ENGINE->ReinitCounters();

    return GetMaxError() < Poseidon::Foundation::EMError;
}

void World::InitClient()
{
    LOG_INFO(Network, "[InitClient] setting _mode=GModeNetware (was {})", (int)_mode);
    _mode = GModeNetware;
    AspectRatio::SetGameplayActive(true);
    _endMission = EMContinue;
    EnableRadio();

    if (_ui)
    {
        _ui->ResetHUD();
    }

    if (!GDummyVehicle)
    {
        GDummyVehicle = NewVehicle("PaperCar");
    }

    AIGlobalInit();

    _eastCenter = nullptr;
    _westCenter = nullptr;
    _guerrilaCenter = nullptr;
    _civilianCenter = nullptr;
    _logicCenter = nullptr;

    EnvSoundPars[5].name = "SOUND\\$DEFAULT$.WSS";
    EnvSoundParsNight[5].name = "SOUND\\$DEFAULT$.WSS";
}

VehicleType* NewAIType(const char* name)
{
    EntityType* vType = VehicleTypes.New(name);
    if (!vType)
    {
        return nullptr;
    }
    EntityAIType* type = dynamic_cast<EntityAIType*>(vType);
    if (!type)
    {
        LOG_ERROR(World, "Type {} is not EntityAIType", (const char*)vType->GetName());
    }
    return type;
}

static void GetPosArray(AutoArray<Vector3>& tgt, const ParamEntry& cfg)
{
    tgt.Resize(0);
    for (int i = 0; i < cfg.GetSize() - 1; i += 2)
    {
        float x = cfg[i];
        float z = cfg[i + 1];
        float y = GLandscape->SurfaceYAboveWater(x, z);
        Vector3& pos = tgt[tgt.Add()];
        pos = Vector3(x, y, z);
    }
}

void World::InitLandscape(Landscape* landscape)
{
    Dev::ClearEditorCaves();
    Dev::ClearTerrainEditBaseline();
    Dev::EndPhysicsShowcase();
    GSnow().Reset();
    GMud().Reset();
    GSand().Reset();
    GRainWater().Reset();
    GSnowFlakes.ClearParticles();
    GSnow().enabled = false;
    if (const char* depth = std::getenv("POSEIDON_SNOW_TEST_DEPTH"))
    {
        GSnow().enabled = true;
        GSnow().falling = false;
        GSnow().Deposit(std::strtof(depth, nullptr));
        if (const char* density = std::getenv("POSEIDON_SNOW_TEST_FLAKES"))
            GSnow().flakeMultiplier = std::strtof(density, nullptr);
        if (const char* rate = std::getenv("POSEIDON_SNOW_TEST_RATE"))
        {
            GSnow().falling = true;
            GSnow().metresPerMinute = std::clamp(std::strtof(rate, nullptr), 0.0f, 1.0f);
        }
    }
    // AST-004 -- the derived model cache's only eviction point.
    //
    // It is HERE, at a level change, and not on every Put, for two reasons.
    // Prune() walks the whole tree and sorts it; paying that inside a model load
    // puts a directory enumeration in the middle of a frame. And this is the
    // moment it is most useful: the previous world's models are the coldest
    // entries in the store and the next world is about to write its own, so
    // making room before the fill is what stops the budget being enforced by
    // evicting things this world has just cached.
    //
    // Before the fill, not after: an eviction pass that ran at the END of a load
    // would be sorting entries whose mtimes are all seconds old and would throw
    // away exactly what is about to be asked for again.
    const DWORD tInitLandscape = Poseidon::Foundation::GlobalTickCount();
    DWORD tStep = tInitLandscape;
    ModelDerivedCache::Instance().Prune();
    LOG_INFO(Core, "InitLandscape phase: ModelDerivedCache::Prune {} ms",
             Poseidon::Foundation::GlobalTickCount() - tStep);

    tStep = Poseidon::Foundation::GlobalTickCount();
    _preloadedVType[VTypeStatic] = NewAIType("Static");
    _preloadedVType[VTypeBuilding] = NewAIType("Building");
    _preloadedVType[VTypeStrategic] = NewAIType("Strategic");
    _preloadedVType[VTypeNonStrategic] = NewAIType("NonStrategic");
    _preloadedVType[VTypeTarget] = NewAIType("Target");
    _preloadedVType[VTypeAllVehicles] = NewAIType("AllVehicles");
    _preloadedVType[VTypeAir] = NewAIType("Air");
    _preloadedVType[VTypePlane] = NewAIType("Plane");
    _preloadedVType[VTypeShip] = NewAIType("Ship");
    _preloadedVType[VTypeBigShip] = NewAIType("BigShip");
    _preloadedVType[VTypeAPC] = NewAIType("APC");
    _preloadedVType[VTypeTank] = NewAIType("Tank");
    _preloadedVType[VTypeCar] = NewAIType("Car");
    _preloadedVType[VTypeMan] = NewAIType("Man");
    LOG_INFO(Core, "InitLandscape phase: preload 14 AI types {} ms",
             Poseidon::Foundation::GlobalTickCount() - tStep);

    tStep = Poseidon::Foundation::GlobalTickCount();
    Clear();
    LOG_INFO(Core, "InitLandscape phase: World::Clear {} ms", Poseidon::Foundation::GlobalTickCount() - tStep);

    tStep = Poseidon::Foundation::GlobalTickCount();
    if (!ENGINE_CONFIG.landEditor || (Pars >> "CfgWorlds").FindEntry(Glob.header.worldname))
    {
        const ParamEntry& world = Pars >> "CfgWorlds" >> Glob.header.worldname;
        InitGeneral(world);

        ProgressRefresh();

        const ParamEntry& ilsPos = world >> "ilsPosition";
        const ParamEntry& ilsDir = world >> "ilsDirection";
        Vector3 mainIlsPos;
        Vector3 mainIlsDir;
        mainIlsPos.Init();
        mainIlsDir.Init();
        mainIlsPos[0] = ilsPos[0];
        mainIlsPos[2] = ilsPos[1];
        mainIlsPos[1] = GLOB_LAND->SurfaceY(mainIlsPos[0], mainIlsPos[2]);
        mainIlsDir[0] = ilsDir[0];
        mainIlsDir[1] = ilsDir[1];
        mainIlsDir[2] = ilsDir[2];
        mainIlsDir.Normalize();

        _ilsPosWest = mainIlsPos;
        _ilsDirWest = mainIlsDir;

        GetPosArray(_taxiInPathsWest, world >> "ilsTaxiIn");
        GetPosArray(_taxiOffPathsWest, world >> "ilsTaxiOff");
    }
    LOG_INFO(Core, "InitLandscape phase: CfgWorlds InitGeneral+ILS {} ms",
             Poseidon::Foundation::GlobalTickCount() - tStep);

    tStep = Poseidon::Foundation::GlobalTickCount();
    _scene.Init(_engine, landscape);
    LOG_INFO(Core, "InitLandscape phase: Scene::Init {} ms", Poseidon::Foundation::GlobalTickCount() - tStep);

    ProgressRefresh();

    if (!_ui && !ENGINE_CONFIG.doCreateDedicatedServer && !AppConfig::Instance().IsViewerMode())
    {
        _ui = CreateInGameUI();
    }

    GScene = GetScene();
    GLandscape = _scene.GetLandscape();

    if (!ENGINE_CONFIG.landEditor || (Pars >> "CfgWorlds").FindEntry(Glob.header.worldname))
    {
        const ParamEntry& world = Pars >> "CfgWorlds" >> Glob.header.worldname;
        tStep = Poseidon::Foundation::GlobalTickCount();
        _scene.GetLandscape()->InitDynSounds(world >> "Sounds");
        LOG_INFO(Core, "InitLandscape phase: InitDynSounds {} ms",
                 Poseidon::Foundation::GlobalTickCount() - tStep);

        ProgressRefresh();

        tStep = Poseidon::Foundation::GlobalTickCount();
        _scene.GetLandscape()->InitGeography();
        LOG_INFO(Core, "InitLandscape phase: InitGeography {} ms",
                 Poseidon::Foundation::GlobalTickCount() - tStep);

        ProgressRefresh();

        tStep = Poseidon::Foundation::GlobalTickCount();
        InitEnvSounds(world >> "EnvSounds");
        InsertSeaGulls();
        LOG_INFO(Core, "InitLandscape phase: InitEnvSounds+SeaGulls {} ms",
                 Poseidon::Foundation::GlobalTickCount() - tStep);

        ProgressRefresh();
    }

    ProgressRefresh();

    tStep = Poseidon::Foundation::GlobalTickCount();
    InitFinish();
    LOG_INFO(Core, "InitLandscape phase: InitFinish {} ms", Poseidon::Foundation::GlobalTickCount() - tStep);

    tStep = Poseidon::Foundation::GlobalTickCount();
    _scene.GetLandscape()->InitObjectVehicles();
    LOG_INFO(Core, "InitLandscape phase: InitObjectVehicles {} ms",
             Poseidon::Foundation::GlobalTickCount() - tStep);

    ProgressRefresh();

    _showMap = false;
    _forceMap = false;
    Log("InitLandscape ResetIDs");
    tStep = Poseidon::Foundation::GlobalTickCount();
    ResetIDs();
    LOG_INFO(Core, "InitLandscape phase: ResetIDs {} ms; InitLandscape total {} ms",
             Poseidon::Foundation::GlobalTickCount() - tStep,
             Poseidon::Foundation::GlobalTickCount() - tInitLandscape);
}

void World::InitFinish()
{
    InitCameraPars();

    _scene.GetLandscape()->MakeShadows(_scene);

    const ParamEntry& entry = Pars >> "CfgEnvSounds" >> "Sea" >> "sound";
    RString waveName = GetSoundName(entry[0]);
    if (GSoundsys)
    {
        float duration = GSoundsys->GetWaveDuration(waveName);
        if (duration > 0.1)
        {
            _scene.GetLandscape()->SetSeaWaveSpeed(1 / duration);
        }
        else
        {
            _scene.GetLandscape()->SetSeaWaveSpeed(1 / 2.0);
        }
    }
    else
    {
        _scene.GetLandscape()->SetSeaWaveSpeed(1 / 2.0);
    }
}

const GridInfo* World::GetGridInfo(float zoom) const
{
    for (int i = 0; i < _gridInfo.Size(); i++)
    {
        if (zoom <= _gridInfo[i].zoomMax)
        {
            return &_gridInfo[i];
        }
    }
    // Zoom exceeds all configured grid levels — return the coarsest one
    if (_gridInfo.Size() > 0)
    {
        return &_gridInfo[_gridInfo.Size() - 1];
    }
    return nullptr;
}

template <class Type>
Type LoadValue(const ParamEntry& cfg, const char* name, const Type& defVal)
{
    return cfg.ReadValue(name, defVal);
}

void World::ParseCfgWorld()
{
    const ParamEntry& cls = Pars >> "CfgWorlds" >> Glob.header.worldname;

    _latitude = LoadValue(cls, "latitude", -40) * (H_PI / 180); // -40 - Croatia, -90 - north pole;
    _longitude = LoadValue(cls, "longitude", +15) * (H_PI / 180);

    const ParamEntry& clsGrid = cls >> "Grid";
    _gridOffsetX = clsGrid >> "offsetX";
    _gridOffsetY = clsGrid >> "offsetY";
    _gridInfo.Resize(0);
    for (int i = 0; i < clsGrid.GetEntryCount(); i++)
    {
        const ParamEntry& entry = clsGrid.GetEntry(i);
        if (entry.IsClass())
        {
            int index = _gridInfo.Add();
            GridInfo& info = _gridInfo[index];
            info.zoomMax = entry >> "zoomMax";
            info.format = entry >> "format";
            info.formatX = entry >> "formatX";
            info.formatY = entry >> "formatY";
            info.stepX = entry >> "stepX";
            info.stepY = entry >> "stepY";
            info.invStepX = info.stepX == 0 ? 0 : 1.0f / info.stepX;
            info.invStepY = info.stepY == 0 ? 0 : 1.0f / info.stepY;
        }
    }
    _gridInfo.Compact();
}

extern RString GBriefingOnPlan;
extern RString GBriefingOnNotes;
extern RString GBriefingOnGear;
extern RString GBriefingOnGroup;

const float PreferredGridSizeMP = 25;

void World::CleanUpDeinit()
{
    Dev::ClearEditorCaves();
    Dev::ClearTerrainEditBaseline();
    // Release procedural precipitation textures while the renderer bank is alive.
    GSnowFlakes.ClearParticles();
    GRain.ClearParticles();
    _timeToSkip = 0;

    _cameraEffect.Free();
    _players.Clear();
    Log("CleanUp world");

    // ProgressScript holds a reference to the cut/title effects; destroying them early
    // aborts in-progress cutscenes on load screens.
    if (!Poseidon::ProgressScript)
    {
        _titleEffect.Free();
        _cutEffect.Free();
        if (GSoundScene)
        {
            GSoundScene->StopMusicTrack();
        }
    }

    _scripts.Clear();
    _playerSuspended = false;

    _sensorList.Free();
    _sensorList = new SensorList;
    GGameState.Reset();

    GBriefingOnPlan = RString();
    GBriefingOnNotes = RString();
    GBriefingOnGear = RString();
    GBriefingOnGroup = RString();
    GMapOnSingleClick = RString();

    GScene->CleanUp();

    GDummyVehicle = nullptr;

    _vehicles.Clear();
    _fastVehicles.Clear();
    _animals.Clear();
    _buildings.Clear();
    _outVehicles.Clear();
    _cloudlets.Clear();
    _eastCenter.Free();
    _westCenter.Free();
    _civilianCenter.Free();
    _guerrilaCenter.Free();
    _logicCenter.Free();

    extern RefArray<Object> MapDiags;
    MapDiags.Clear();

    _engine->TextBank()->FlushTextures();
}

void World::CleanUpInit()
{
    _nextMagazineID = 0;

    if (GLandscape)
    {
        _eastCenter = new AICenter(TEast, AICMDisabled);
        _westCenter = new AICenter(TWest, AICMDisabled);
        _guerrilaCenter = new AICenter(TGuerrila, AICMDisabled);
        _civilianCenter = new AICenter(TCivilian, AICMDisabled);
        _logicCenter = new AICenter(TLogic, AICMDisabled);
    }

    ResetErrors();
    _players.Clear();

    _showMap = false;
    _forceMap = false;
    Glob.clock.SetTimeInYear(8 * OneHour + 130 * OneDay);
    _scene.MainLight()->Recalculate(this);
    _scene.MainLightChanged();

    for (int i = 0; i < MaxCameraType; i++)
    {
        _camHeading[i] = _camDive[i] = 0;
        _camHeadingWanted[i] = _camHeadingWanted[i] = 0;
        _camNear[i] = 1.0;
        _camMaxDist[i] = 1e10;
    }
    _camType = _camTypeMain = CamInternal;
    _cameraExternal = false;

    _cameraEffect = nullptr;

    _playerManual = false;
    _playerSuspended = false;

#if PROFILE
    _acceleratedTime = 0.5;
#else
    _acceleratedTime = 1.0;
#endif

    Glob.time = Poseidon::Foundation::Time(0);
    _channelChanged = UITIME_MIN;

#ifdef _WIN32
    Poseidon::Foundation::FastCAlloc::CleanUpAll();
#endif
}

void World::CleanUp()
{
    if (GEngine)
        GEngine->SetMenuSeaSceneActive(false);
    CleanUpDeinit();
    CleanUpInit();
}

void World::Reset()
{
    const ParamEntry& world = Pars >> "CfgWorlds" >> Glob.header.worldname;
    CleanUp();
    _scene.GetLandscape()->InitObjectVehicles();
    _scene.GetLandscape()->InitDynSounds(world >> "Sounds");
    InsertSeaGulls();
    GSoundScene->Reset();
}

void World::Clear()
{
    CleanUp();
}

void World::DeleteAnyVehicle(Entity* vehicle)
{
    VehiclesDistributed* list = vehicle->GetList();
    if (list)
    {
        list->Delete(vehicle);
    }
    else
    {
        LOG_DEBUG(World, "DeleteAnyVehicle: Unknown list ({})", (const char*)vehicle->GetDebugName());
        DeleteVehicle(vehicle);
    }
}

void World::RemoveAnyVehicle(Entity* vehicle)
{
    VehiclesDistributed* list = vehicle->GetList();
    if (list)
    {
        list->Remove(vehicle);
    }
    else
    {
        LOG_DEBUG(World, "RemoveAnyVehicle: Unknown list ({})", (const char*)vehicle->GetDebugName());
        RemoveVehicle(vehicle);
    }
}
