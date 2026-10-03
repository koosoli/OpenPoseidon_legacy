#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Dev/Diag/OpDiag.hpp> // DIAG-001 hooks, empty unless POSEIDON_DIAG
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/Weather/MudField.hpp>
#include <Poseidon/World/Weather/SandField.hpp>
#include <Poseidon/World/Weather/RainWaterField.hpp>
#include <Poseidon/World/Weather/SunlightDrying.hpp>
#include <Poseidon/World/Weather/RainVolume.hpp>
#include <Poseidon/World/Entities/Infantry/UniformWetness.hpp>
#include <Poseidon/World/Terrain/TerrainSubdivision.hpp>
#include "SimVehicleCost.hpp" // PERF-023
#include <Poseidon/Input/CheatCode.hpp>
#include <Poseidon/Input/ControllerUiLayout.hpp>
#include <SDL3/SDL_scancode.h>

#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Config/UserConfig.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/UI/UIActiveDisplay.hpp>
#include <Poseidon/UI/OptionsUICommon.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/AI/AICostAccum.hpp> // PERF-014: the AI stage's per-bucket accumulator
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Dev/Diag/DiagPause.hpp>
#include <Poseidon/World/Effects/SmokeWorldQuery.hpp> // IndoorRoomAt/IndoorClearanceAt: the SERoom producer
#include <algorithm>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Audio/Speaker.hpp>
#include <Poseidon/Audio/EnvironmentClassify.hpp>
#include <stdio.h>
#include <string.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/RStringArray.hpp>
#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Memory/CheckMem.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Memtype.h>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>
#ifdef _WIN32
#include <windows.h>
#endif
#include <Poseidon/Graphics/Rendering/Primitives/ClipVert.hpp>

namespace
{
Poseidon::DisplayArcadeMap* FindEditorOwner(ControlsContainer* display)
{
    for (ControlsContainer* cur = display; cur; cur = cur->Parent())
    {
        if (auto* editor = dynamic_cast<Poseidon::DisplayArcadeMap*>(cur))
            return editor;
    }
    return nullptr;
}

Poseidon::DisplayMission* FindMissionOwner(ControlsContainer* display)
{
    for (ControlsContainer* cur = display; cur; cur = cur->Parent())
    {
        if (auto* mission = dynamic_cast<Poseidon::DisplayMission*>(cur))
            return mission;
    }
    return nullptr;
}

bool DoEditorChildControllerUiAction(ControlsContainer* display, Poseidon::ControllerUiAction action)
{
    Poseidon::ControllerEditorUiLayout layout;
    const Poseidon::ControllerUiDispatch dispatch = layout.Map(action);
    if (dispatch.kind == Poseidon::ControllerUiDispatchKind::KeyTap)
    {
        display->OnKeyDown(dispatch.key, 1, 0);
        display->OnKeyUp(dispatch.key, 1, 0);
        return true;
    }
    return false;
}
} // namespace

#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/World/Effects/SmokeSystem.hpp>
#include <cstdio>
#include <cstring>
#include <Poseidon/World/Terrain/Visibility.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/World/Scene/Camera/CamEffects.hpp>
#include <Poseidon/Game/TitEffects.hpp>
#include <Poseidon/Game/Scripting/Scripts.hpp>
#include <Poseidon/UI/GameModule.hpp>
#include <Poseidon/Dev/Debug/DebugTrap.hpp>

#include <Poseidon/Dev/Diag/DiagModes.hpp>

void MemoryCleanUp();
extern bool showCinemaBorder;
extern SoundPars EnvSoundPars[];
extern SoundPars EnvSoundParsNight[];
extern char LoadFile[];
extern RString GMapOnSingleClick;
ControlsContainer* CreateWarningMessageBox(RString text);
void SetVisibility(float distance);
LSError SerializeMapInfo(ParamArchive& ar, RString name, int minVersion);

extern bool AutoTest;

namespace Poseidon
{
using namespace Dev;
} // namespace Poseidon

#include <Poseidon/Core/Progress.hpp>

#include <Poseidon/World/Entities/Vehicles/SeaGull.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>

#include <Poseidon/Game/Commands/GameStateExt.hpp>

#include <Poseidon/World/Entities/Weapons/Shots.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/Input/KeyLights.hpp>

#include <Poseidon/IO/Serialization/ThreadSync.hpp>

#include <Poseidon/Game/Chat.hpp>

#include <Poseidon/Network/Network.hpp>

#include <Poseidon/AI/ArcadeTemplate.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/Map/UIMap.hpp>

#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Poseidon/World/Scene/ObjectClasses.hpp>

#include <Poseidon/World/Detection/Detector.hpp>

#include <Poseidon/UI/Locale/StringtableExt.hpp>

#include <Poseidon/Core/resincl.hpp>

namespace Poseidon
{
bool StartAutoTest();
bool ProcessFullName(RString);
} // namespace Poseidon

namespace Poseidon
{
void SetBaseSubdirectory(RString);
void SetCampaign(RString);
void StartRandomCutscene(RString);
} // namespace Poseidon

namespace Poseidon
{

#define LOG_ADD_REMOVE_VEHICLE 0
#define PERF_SIM 0

static const int NEnvSoundPars = 6;
struct SoundPars;
} // namespace Poseidon
namespace Poseidon
{

const float PreferredGridSizeMP = 25;

bool World::IsDisplayEnabled() const
{
    if (ENGINE_CONFIG.landEditor)
    {
        return true;
    }
    if (_noDisplay)
    {
        return false;
    }
    if (_showMap && _map && !_map->IsDisplayEnabled())
    {
        return false;
    }
    if (GetGProgress().Active())
    {
        return false;
    }
    // No menu attached → display is always on (viewer / dedicated /
    // editor paths skip CreateMainOptions, so `_options == nullptr`
    // is the natural signal for "no menu in this mode").
    if (!_options)
    {
        return true;
    }
    return _options->IsDisplayEnabled();
}

bool World::IsSimulationEnabled() const
{
    if (ENGINE_CONFIG.landEditor)
    {
        return true;
    }
    if (_mode == GModeNetware)
    {
        return GetNetworkManager().GetGameState() >= NGSPlay;
    }
    else
    {
        // Dev diagnostic pause (Dev/Diag/DiagPause.hpp).  Installed inside the
        // single-player branch on purpose: the GModeNetware branch above never
        // reaches here, so a local pause is structurally unable to touch a
        // network game.  Default false — with it false this is one bool load
        // and the function behaves exactly as it always did.
        if (Dev::DiagPauseActive())
        {
            return false;
        }
        if (_warningMessage)
        {
            return false;
        }
        if (!_enableSimulation)
        {
            return false;
        }
        if (!_simulationFocus)
        {
            return false;
        }
        // No menu (viewer / dedicated / editor) → simulation runs
        // without a pause overlay gating it.  Lets ObjectViewer's
        // animation phase keep advancing in viewer mode without
        // a special-case IsViewerMode() check here.
        if (!_options)
        {
            return true;
        }
        return _options->IsSimulationEnabled();
    }
}

bool World::HasOptions() const
{
    if (!_options)
    {
        return false;
    }
    return !_options->IsSimulationEnabled();
}

bool World::IsUIEnabled() const
{
    if (!IsSimulationEnabled())
    {
        return false;
    }
    if (!_options)
    {
        return false;
    }
    return _options->IsUIEnabled();
}

bool World::HasCompass() const
{
    return _showCompass && !_showMap;
}

bool World::HasWatch() const
{
    return _showWatch && !_showMap;
}

void World::OnChannelChanged()
{
    _channelChanged = Glob.uiTime;
}

void World::CreateMainOptions()
{
    if (!_options)
    {
        _options = CreateMainOptionsUI();
        if (GEngine)
        {
            GEngine->ReinitCounters();
        }
    }
}

void World::CreateEndOptions(int mode)
{
    if (!_options)
    {
        _options = CreateEndOptionsUI(mode);
    }
}
void World::CreateChat()
{
    if (!_chat)
    {
        InputSubsystem::Instance().ChangeGameFocus(+1);
        _chat = CreateChatUI();
    }
}
void World::CreateVoiceChat(bool pushToTalk)
{
    if (!_voiceChat)
    {
        _voiceChat = CreateVoiceChatUI(pushToTalk);
    }
}
void World::CreateMainMap()
{
    if (!_map)
    {
        _map = CreateMainMapUI();
    }
}

void World::CreateWarningMessage(RString text)
{
    if (!_warningMessage)
    {
        _warningMessage = CreateWarningMessageBox(text);
    }
}

void World::DestroyOptions(int exitCode)
{
    if (!_options)
    {
        return;
    }
    _options.Free();
    /*
        switch( exitCode )
        {
            case IDC_CANCEL:
            case IDC_MAIN_GAME:
            break;
            case IDC_MAIN_QUIT:
                Glob.exit=true;
            break;
            case IDC_OK:
                CreateMainOptions();
            break;
        }
    */
    if (exitCode == IDC_MAIN_QUIT)
    {
        Glob.exit = true;
    }

    GEngine->ReinitCounters();
}

void World::DestroyMap(int exitCode)
{
    _map.Free();
}

void World::DestroyChat(int exitCode)
{
    if (_chat)
    {
        _chat.Free();
        InputSubsystem::Instance().ChangeGameFocus(-1);
    }
}

void World::DestroyVoiceChat(int exitCode)
{
    _voiceChat.Free();
}

Transport* World::FindFreeVehicle(Person* driver) const
{
    const float maxDist = 20;
    float minDist2 = Square(maxDist);
    Transport* free = nullptr;
    int xMin, xMax, zMin, zMax;
    Vector3Val pos = driver->Position();
    ObjRadiusRectangle(xMin, xMax, zMin, zMax, pos, pos, maxDist);
    for (int x = xMin; x <= xMax; x++)
    {
        for (int z = zMin; z <= zMax; z++)
        {
            const ObjectList& list = _scene.GetLandscape()->GetObjects(z, x);
            for (int i = 0; i < list.Size(); i++)
            {
                Object* obj = list[i];
                Transport* vehicle = dyn_cast<Transport>(obj);
                if (!vehicle)
                {
                    continue;
                }
                float dist2 = vehicle->Position().Distance2(driver->Position());
                if (minDist2 > dist2)
                {
                    if (vehicle->QCanIGetInAny(driver))
                    {
                        Vector3Val relPos = driver->PositionWorldToModel(vehicle->Position());
                        //					if( relPos.Z()>0 && fabs(relPos.X())<relPos.Z() )
                        if (relPos.Z() > 0)
                        {
                            minDist2 = dist2;
                            free = vehicle;
                        }
                    }
                }
            }
        }
    }
    return free;
}

#if _ENABLE_CHEATS
bool disableAI = false;
bool disableUnitAI = false;
bool disableSimpleSim = false;
#endif

namespace
{
/// PERF-014. The per-group radio walk, which `PerformAI` performs identically for each of
/// the four fighting centers. Extracted only so the timer and the count are written once;
/// the body is the 2001 loop unchanged, and it still runs where it always ran -- AFTER the
/// center's `Think()`, not before it.
void SimulateGroupRadios(AICenter* center, float deltaT)
{
    AICost::ScopedCost cost(AICost::Bucket::Radio);
    for (int i = 0; i < center->NGroups(); i++)
    {
        AIGroup* grp = center->GetGroup(i);
        if (grp)
        {
            AICost::AICosts().radioGroups++;
            grp->GetRadio().Simulate(deltaT);
        }
    }
}
} // namespace

void World::PerformAI(float deltaT, float noAccDeltaT)
{
    auto& input = InputSubsystem::Instance();
#if _ENABLE_CHEATS
    if (input.GetCheat2ToDo(SDL_SCANCODE_T))
    {
        disableAI = !disableAI;
        GlobalShowMessage(500, "Group AI %s", disableAI ? "Off" : "On");
    }
    if (input.GetCheat2ToDo(SDL_SCANCODE_U))
    {
        disableUnitAI = !disableUnitAI;
        GlobalShowMessage(500, "Unit AI %s", disableUnitAI ? "Off" : "On");
    }
    if (input.GetCheat2ToDo(SDL_SCANCODE_I))
    {
        disableSimpleSim = !disableSimpleSim;
        GlobalShowMessage(500, "Simple sim %s", disableSimpleSim ? "Off" : "On");
    }
#endif
    if (deltaT > 0
#if _ENABLE_CHEATS
        && !disableAI
#endif
    )
    {
        // PERF-014: the stage's own tick count, so every bucket below has a denominator.
        // Counted here rather than at the top of the function because a tick with
        // `deltaT == 0` (or `disableAI`) runs none of this, and a mean over ticks that did
        // not run the work is a lie of exactly the kind PERF-013 was written to stop.
        AICost::AICosts().ticks++;
        {
            AICost::ScopedCost cost(AICost::Bucket::Radio);
            GetRadio().Simulate(deltaT);
        }
        if (_eastCenter)
        {
            {
                AICost::ScopedCost cost(AICost::Bucket::Radio);
                _eastCenter->GetRadio().Simulate(deltaT);
            }
            AICost::AICosts().centers++;
            _eastCenter->Think();
            SimulateGroupRadios(_eastCenter, deltaT);
        }
        if (_westCenter)
        {
            {
                AICost::ScopedCost cost(AICost::Bucket::Radio);
                _westCenter->GetRadio().Simulate(deltaT);
            }
            AICost::AICosts().centers++;
            _westCenter->Think();
            SimulateGroupRadios(_westCenter, deltaT);
        }
        if (_guerrilaCenter)
        {
            {
                AICost::ScopedCost cost(AICost::Bucket::Radio);
                _guerrilaCenter->GetRadio().Simulate(deltaT);
            }
            AICost::AICosts().centers++;
            _guerrilaCenter->Think();
            SimulateGroupRadios(_guerrilaCenter, deltaT);
        }
        if (_civilianCenter)
        {
            {
                AICost::ScopedCost cost(AICost::Bucket::Radio);
                _civilianCenter->GetRadio().Simulate(deltaT);
            }
            AICost::AICosts().centers++;
            _civilianCenter->Think();
            SimulateGroupRadios(_civilianCenter, deltaT);
        }
        if (_logicCenter)
        {
            AICost::AICosts().centers++;
            _logicCenter->Think();
        }

        // PERF-014: the end-mission scan walks every sensor in the mission once a tick. It
        // is not AI, but it is inside `PerformAI` and therefore inside the 21-34 ms
        // PERF-013 attributed to the AI stage, so it gets its own bucket rather than
        // silently inflating one.
        AICost::ScopedCost endMissionCost(AICost::Bucket::EndMission);
        if (_endMission == EMContinue)
        {
            if (input.CheatActivated() == CheatWinMission)
            {
                if (_mode != GModeNetware)
                    _endMission = EMEnd1;
                input.CheatServed();
            }
            else
#if _ENABLE_CHEATS
                if (input.GetCheat2ToDo(SDL_SCANCODE_1))
            {
                _endMission = EMEnd1;
            }
            else if (input.GetCheat2ToDo(SDL_SCANCODE_2))
            {
                _endMission = EMEnd2;
            }
            else if (input.GetCheat2ToDo(SDL_SCANCODE_3))
            {
                _endMission = EMEnd3;
            }
            else if (input.GetCheat2ToDo(SDL_SCANCODE_4))
            {
                _endMission = EMEnd4;
            }
            else if (input.GetCheat2ToDo(SDL_SCANCODE_5))
            {
                _endMission = EMEnd5;
            }
            else if (input.GetCheat2ToDo(SDL_SCANCODE_6))
            {
                _endMission = EMEnd6;
            }
            else if (input.GetCheat2ToDo(SDL_SCANCODE_0))
            {
                _endMission = EMLoser;
            }
            else
#endif
            {
                switch (_mode)
                {
                    case GModeArcade:
                    {
                        Person* veh = GetRealPlayer();
                        AIUnit* unit = veh ? veh->Brain() : nullptr;
                        if (!unit || unit->GetLifeState() == AIUnit::LSDead)
                        {
                            _endMission = EMKilled;
                            return;
                        }
                    }
                    case GModeIntro:
                    case GModeNetware:
                    {
                        int nEnd1 = 0, cEnd1 = 0;
                        int nEnd2 = 0, cEnd2 = 0;
                        int nEnd3 = 0, cEnd3 = 0;
                        int nEnd4 = 0, cEnd4 = 0;
                        int nEnd5 = 0, cEnd5 = 0;
                        int nEnd6 = 0, cEnd6 = 0;

                        for (int i = 0; i < sensorsMap.Size(); i++)
                        {
                            Entity* veh = sensorsMap[i];
                            if (!veh)
                            {
                                continue;
                            }
                            Detector* sensor = dyn_cast<Detector>(veh);
                            PoseidonAssert(sensor);
                            if (!sensor)
                            {
                                continue;
                            }
                            switch (sensor->GetAction())
                            {
                                case ASTLoose:
                                    if (sensor->IsActive())
                                    {
                                        _endMission = EMLoser;
                                        return;
                                    }
                                    break;
                                case ASTEnd1:
                                    nEnd1++;
                                    if (sensor->IsActive())
                                    {
                                        cEnd1++;
                                    }
                                    break;
                                case ASTEnd2:
                                    nEnd2++;
                                    if (sensor->IsActive())
                                    {
                                        cEnd2++;
                                    }
                                    break;
                                case ASTEnd3:
                                    nEnd3++;
                                    if (sensor->IsActive())
                                    {
                                        cEnd3++;
                                    }
                                    break;
                                case ASTEnd4:
                                    nEnd4++;
                                    if (sensor->IsActive())
                                    {
                                        cEnd4++;
                                    }
                                    break;
                                case ASTEnd5:
                                    nEnd5++;
                                    if (sensor->IsActive())
                                    {
                                        cEnd5++;
                                    }
                                    break;
                                case ASTEnd6:
                                    nEnd6++;
                                    if (sensor->IsActive())
                                    {
                                        cEnd6++;
                                    }
                                    break;
                            }
                        }
                        if (nEnd1 > 0 && cEnd1 == nEnd1)
                        {
                            _endMission = EMEnd1;
                        }
                        else if (nEnd2 > 0 && cEnd2 == nEnd2)
                        {
                            _endMission = EMEnd2;
                        }
                        else if (nEnd3 > 0 && cEnd3 == nEnd3)
                        {
                            _endMission = EMEnd3;
                        }
                        else if (nEnd4 > 0 && cEnd4 == nEnd4)
                        {
                            _endMission = EMEnd4;
                        }
                        else if (nEnd5 > 0 && cEnd5 == nEnd5)
                        {
                            _endMission = EMEnd5;
                        }
                        else if (nEnd6 > 0 && cEnd6 == nEnd6)
                        {
                            _endMission = EMEnd6;
                        }
                        else
                        {
                            _endMission = EMContinue;
                        }
                    }
                    break;
                    default:
                        Fail("Unknown mode");
                        _endMission = EMContinue;
                        break;
                }
            }
        }
    }
}

void World::SimulateVehicles(float deltaT, VehicleSimulation simul, Entity* insideVehcile)
{
    {
        SimVehCost::ScopedStage stage(SimVehCost::Stage::SlowMoveOut);
        MoveOutAndDelete(_vehicles, deltaT, false);
        MoveOutAndDelete(_animals, deltaT, false);
    }
    {
        SimVehCost::ScopedStage stage(SimVehCost::Stage::SlowVehicles);
        SimulateOnly(_vehicles, deltaT, simul, insideVehcile, SimulateVisibleNear);
    }
    {
        SimVehCost::ScopedStage stage(SimVehCost::Stage::SlowAnimals);
        SimulateOnly(_animals, deltaT, simul, insideVehcile, SimulateVisibleNear);
    }
    {
        SimVehCost::ScopedStage stage(SimVehCost::Stage::SlowMoveOut);
        MoveOutAndDelete(_vehicles, deltaT, true);
        MoveOutAndDelete(_animals, deltaT, true);
    }

#if PERF_SIM
#endif
}

void World::SimulateBuildings(float deltaT, VehicleSimulation simul)
{
    MoveOutAndDelete(_buildings, deltaT, false);
    SimulateOnly(_buildings, deltaT, simul, nullptr, SimulateVisibleNear);
#if PERF_SIM
#endif
    MoveOutAndDelete(_buildings, deltaT, true);
}

void World::SimulateFastVehicles(float deltaT, VehicleSimulation simul)
{
    MoveOutAndDelete(_fastVehicles, deltaT, false);
    SimulateOnly(_fastVehicles, deltaT, simul, nullptr, SimulateVisibleNear);
#if PERF_SIM
#endif
    MoveOutAndDelete(_fastVehicles, deltaT, true);
}

void World::SimulateCloudlets(float deltaT)
{
    SimVehCost::ScopedCloudletStage softCost(SimVehCost::Stage::CloudletSoftSurfaces);
    GSnow().Advance(deltaT);
    if (GLandscape)
    {
        GMud().Advance(deltaT, UniformWettingDensity(GLandscape->GetRainDensity(),
            GRain.EffectiveDensity(), GRain.Params().snowflakes));
        GSand().Advance(deltaT, UniformWettingDensity(GLandscape->GetRainDensity(),
            GRain.EffectiveDensity(), GRain.Params().snowflakes));
        softCost.Stop();
        const float liquidRain = UniformWettingDensity(GLandscape->GetRainDensity(),
            GRain.EffectiveDensity(), GRain.Params().snowflakes);
        auto& runoff = GRainWater();
        // Opt-in source/query gate only until the matched renderer and installed
        // tests are accepted. Ordinary missions retain the legacy backend.
        static const bool fineSupport=[] {
            const char* value=std::getenv("POSEIDON_RAIN_WATER_FINE");
            return value&&value[0]=='1'&&value[1]=='\0';
        }();
        static uint64_t bedRevision = 0;
        const int terrainRange = GLandscape->GetTerrainRange();
        const float extent = (terrainRange - 1) * GLandscape->GetTerrainGrid();
        if (terrainRange >= 2 && std::isfinite(extent) && extent > 0 &&
            (runoff.Width() != 0 || liquidRain > 0))
        {
            SimVehCost::ScopedCloudletStage sourceCost(SimVehCost::Stage::CloudletRunoffSource);
            // Keep source vertices aligned through native terrain subdivision.
            // The bounded grid uses a power-of-two stride, never a stretched
            // spacing that moves every sample away from the authored triangles.
            const auto grid = RainWaterField::AlignedSourceGrid(terrainRange,GLandscape->GetTerrainGrid());
            const int side = grid.side;
            const float spacing = grid.spacing;
            if (!runoff.MatchesDomain(side,side,spacing))
            {
                std::vector<float> bed(static_cast<size_t>(side) * side);
                for (int z=0; z<side; ++z) for (int x=0; x<side; ++x)
                    bed[static_cast<size_t>(z)*side+x] = GLandscape->ClippedData(z*grid.stride,x*grid.stride);
                bool configured=false;
                if(runoff.FineActive()) {
                    configured=runoff.ReconfigureFine(side,side,spacing,bed);
                    LOG_INFO(Graphics,"RAIN_WATER_FINE_REGRID world={} width={} spacing={:.3f} volume_m3={:.6f} retained={} shoreline=redistributed",
                        GLandscape->GetName(),side,spacing,runoff.Volume(),configured);
                }
                else if (runoff.Width() != 0)
                {
                    // A runtime setTerrainGrid may change both the number and
                    // area of water cells. The existing equal-area node storage
                    // has no conservative regrid operator; retire it explicitly
                    // instead of silently moving retained water to wrong places.
                    LOG_INFO(Graphics,"RAIN_WATER_DOMAIN_RESET world={} oldWidth={} oldHeight={} oldSpacing={:.3f} newWidth={} newSpacing={:.3f} removedVolume_m3={:.6f}",
                        GLandscape->GetName(),runoff.Width(),runoff.Height(),runoff.Spacing(),side,spacing,runoff.Volume());
                    runoff.Reset();
                }
                if (!configured&&!runoff.FineActive())
                    configured=runoff.Configure(side,side,spacing,std::move(bed),0,0,GLandscape->GetSeaLevel());
                if (configured)
                {
                    bedRevision = GLandscape->HeightRevision();
                    LOG_INFO(Graphics,"RAIN_WATER_INIT world={} width={} spacing={:.3f}",
                        GLandscape->GetName(),side,spacing);
                }
            }
            else if (bedRevision != GLandscape->HeightRevision())
            {
                // Real craters/terrain edits change the bed while retained water
                // volume survives. A new outlet then drains through normal flow.
                for (int z=0; z<side; ++z) for (int x=0; x<side; ++x)
                    runoff.SetBed(x,z,GLandscape->ClippedData(z*grid.stride,x*grid.stride));
                bedRevision = GLandscape->HeightRevision();
            }
            // A failed domain rebuild retains mass but must not revalidate the
            // old coordinates against a different terrain-grid identity.
            const auto fine=runoff.MatchesDomain(side,side,spacing)
                ? runoff.UpdateTerrainSource(GLandscape,terrainRange,GLandscape->GetTerrainGrid(),
                    GLandscape->HeightRevision(),fineSupport,[](int x,int z){return GLandscape->GetHeight(z,x);})
                : Poseidon::RainWaterField::FineUpdate{};
            if(fine.changed)
                LOG_INFO(Graphics,"RAIN_WATER_FINE_SOURCE world={} sourceRevision={} native={} admitted={} updated={} refused={} coverageLost={} valid={} tiles={} enabled={} presentation=pending",
                    GLandscape->GetName(),GLandscape->HeightRevision(),fine.native,fine.admitted,fine.updated,fine.refused,
                    fine.coverageLost,fine.valid,runoff.FineTileCount(),fineSupport);
            sourceCost.Stop();
            const auto* sun = _scene.MainLight();
            const float overcast = std::clamp(GLandscape->GetOvercast(),0.0f,1.0f);
            const float solar = sun ? SunlightDryingExposure(sun->SunDirection().Y(), overcast,
                sun->NightEffect()) : 0;
            if (runoff.MatchesDomain(side,side,spacing))
            {
                runoff.SetSeaLevel(GLandscape->GetSeaLevel());
                {
                    SimVehCost::ScopedCloudletStage advanceCost(SimVehCost::Stage::CloudletRunoffAdvance);
                    runoff.Advance(deltaT,liquidRain,solar,GWind.IsActive() ? GWind.Sample().speed/20.0f : 0);
                }
                SimVehCost::ScopedCloudletStage naturalCost(SimVehCost::Stage::CloudletRunoffNatural);
                const auto natural=runoff.UpdateNaturalTerrain(GLandscape,terrainRange,GLandscape->GetTerrainGrid(),
                    GLandscape->HeightRevision(),fineSupport,deltaT,[](int x,int z){return GLandscape->GetHeight(z,x);});
                if(natural.changed||natural.refused)
                    LOG_INFO(Graphics,"RAIN_WATER_NATURAL_SOURCE world={} sourceRevision={} admitted={} retired={} refused={} examined={} valid={} tiles={} enabled={} presentation=pending",
                        GLandscape->GetName(),GLandscape->HeightRevision(),natural.admitted,natural.retired,natural.refused,
                        natural.examined,natural.valid,runoff.FineTileCount(),fineSupport);
                naturalCost.Stop();
            }
            else if(runoff.FineActive())
            {
                // Failed reconfiguration marks the fine source stale; Advance
                // only retains pending time until actual coordinates recover.
                SimVehCost::ScopedCloudletStage advanceCost(SimVehCost::Stage::CloudletRunoffAdvance);
                runoff.Advance(deltaT,liquidRain,solar,GWind.IsActive() ? GWind.Sample().speed/20.0f : 0);
            }
        }
    }
    softCost.Stop(); // also closes the no-landscape path; never counts twice
    SimVehCost::ScopedCloudletStage remainingCost(SimVehCost::Stage::CloudletRemainingEffects);
    SimulationImportance prec = SimulateVisibleFar;

    // POSEIDON_TEST_SMOKE=1 drops one new-system plume in front of the camera a
    // couple of seconds into the mission. The dev panel is the normal way in,
    // but the panel cannot be driven from a script or a headless run, so there
    // was no way to reproduce "the smoke does nothing" without a human at the
    // keyboard. Same env-var convention as POSEIDON_WIND_BALLISTICS.
    {
        // 1 = new system, 2 = legacy source (so the legacy path can be verified
        // without the panel too — it was silently spawning an explosion).
        static const int wanted = []
        {
            const char* value = std::getenv("POSEIDON_TEST_SMOKE");
            if (value == nullptr || value[0] == '\0' || value[0] == '0')
            {
                return 0;
            }
            return value[0] == '2' ? 2 : (value[0] == '3' ? 3 : (value[0] == '4' ? 4 : (value[0] == '5' ? 5 : 1)));
        }();
        static bool spawned = false;
        if (wanted != 0 && !spawned && CameraOn() != nullptr && Glob.time.toFloat() > 2.0f)
        {
            spawned = true;
            Vector3 position = CameraOn()->Position() + CameraOn()->Direction() * 10.0f;
            // POSEIDON_TEST_SMOKE_AT="x y z" pins the emitter to a world position,
            // so a scripted run can put a plume INSIDE a specific building — the
            // only way to test wall/ceiling behaviour without a hand on the panel.
            if (const char* at = std::getenv("POSEIDON_TEST_SMOKE_AT"))
            {
                float x = 0.0f, y = 0.0f, z = 0.0f;
                if (std::sscanf(at, "%f %f %f", &x, &y, &z) == 3)
                {
                    position = Vector3(x, y, z);
                }
            }
            LOG_INFO(World, "POSEIDON_TEST_SMOKE spawn at {:.1f},{:.1f},{:.1f}", position.X(), position.Y(),
                     position.Z());
            // POSEIDON_SMOKE_THICKNESS=<x> multiplies emission rate AND the
            // particle cap together, which is the only pair that actually makes
            // a plume denser: raising the rate alone stops at the cap, and
            // raising the cap alone leaves nothing to fill it. Opacity is not
            // touched -- 1.0 is already the physical maximum for one billboard,
            // so thickness past that is overlap, i.e. count.
            //
            // Exists so a thick plume can be CAPTURED. --auto-screenshot cannot
            // drag a slider, same reason as POSEIDON_PANEL_SEARCH.
            const float thicken = []
            {
                const char* v = std::getenv("POSEIDON_SMOKE_THICKNESS");
                const float parsed = v != nullptr ? static_cast<float>(std::atof(v)) : 1.0f;
                return parsed > 0.0f ? std::min(parsed, 64.0f) : 1.0f;
            }();
            const auto thicken_params = [thicken](SmokeParams& p)
            {
                if (thicken <= 1.0f)
                {
                    return;
                }
                p.rate *= thicken;
                p.fireRate *= thicken;
                // The cap has to move with the rate or it simply throttles it
                // straight back; 200000 mirrors the panel's own ceiling.
                p.maxParticles = static_cast<int>(std::min(static_cast<float>(p.maxParticles) * thicken, 200000.0f));
                p.maxFlames = static_cast<int>(std::min(static_cast<float>(p.maxFlames) * thicken, 40000.0f));
                LOG_INFO(World, "POSEIDON_SMOKE_THICKNESS={:.1f}x -> rate {:.0f}/s, cap {} particles", thicken, p.rate,
                         p.maxParticles);
            };
            // POSEIDON_TEST_SMOKE_HOUSE=1: find the nearest ENTERABLE building on
            // the map (a shape with a Roadway LOD is one you can walk inside) and
            // put the emitter inside it, 1 m off its floor. This is the only way
            // to test wall / ceiling / indoor-shelter behaviour from a script
            // without knowing the map's coordinates in advance; the log line
            // below tells you where it went so the camera can be aimed there on
            // the next run.
            const char* houseEnv = std::getenv("POSEIDON_TEST_SMOKE_HOUSE");
            const bool wantHouse = houseEnv != nullptr && houseEnv[0] != '\0' && houseEnv[0] != '0';
            if (wantHouse && GLandscape != nullptr)
            {
                const Object* best = nullptr;
                float bestD2 = 1e30f;
                for (int z = 0; z < LandRange; ++z)
                {
                    for (int x = 0; x < LandRange; ++x)
                    {
                        const ObjectList& list = GLandscape->GetObjects(z, x);
                        for (int i = 0; i < list.Size(); ++i)
                        {
                            const Object* obj = list[i];
                            if (obj == nullptr || obj->GetShape() == nullptr)
                            {
                                continue;
                            }
                            const LODShape* shape = obj->GetShape();
                            if (shape->FindRoadwayLevel() < 0 || shape->FindFireGeometryLevel() < 0)
                            {
                                continue;
                            }
                            if (obj->GetRadius() < 4.0f)
                            {
                                continue;
                            }
                            const float d2 = (obj->Position() - position).SquareSizeXZ();
                            if (d2 < bestD2)
                            {
                                bestD2 = d2;
                                best = obj;
                            }
                        }
                    }
                }
                if (best != nullptr)
                {
                    // Inside the footprint, low. The centre of a house model is
                    // usually inside it; 1 m above its floor keeps the emitter
                    // out of the ground plane.
                    position = best->Position();
                    // Probe DOWN from 2 m above the model origin: the topmost-surface variant
                    // returned the roof, and SurfaceY the ground under the slab. The floor
                    // of the storey the origin sits in is what a "spawn inside" wants.
                    {
                        Vector3 probe = position;
                        probe[1] = best->Position().Y() + 2.0f;
                        position[1] = GLandscape->RoadSurfaceY(probe) + 0.4f;
                    }
                    LOG_INFO(World,
                             "POSEIDON_TEST_SMOKE_HOUSE: nearest enterable building '{}' at {:.1f},{:.1f},{:.1f} "
                             "r={:.1f} ({:.0f} m away)",
                             (const char*)best->GetShape()->Name(), best->Position().X(), best->Position().Y(),
                             best->Position().Z(), best->GetRadius(), std::sqrt(bestD2));
                }
                else
                {
                    LOG_WARN(World, "POSEIDON_TEST_SMOKE_HOUSE: no enterable building found on this map");
                }
            }
            if (wanted == 2)
            {
                GSmokeSystem.SpawnLegacy(position, 1.0f, 1.5f, -1.0f);
            }
            else if (wanted == 5)
            {
                // Balloons, for scripted capture.
                SmokeParams b;
                b.balloons = true;
                b.rate = 9.0f;
                b.particleLifetime = 60.0f;
                b.maxParticles = 700; // 9/s x 60 s = 540, plus lifetime jitter
                b.emitterRadius = 1.4f;
                b.initialSpeed = 0.6f;
                b.opacity = 1.0f;
                b.fadeIn = 0.02f;
                b.fadeOut = 0.06f;
                b.drag = 0.5f;
                b.turbulence = 0.0f;
                b.diffusion = 0.0f;
                b.densityFalloff = 0.0f;
                b.restitution = 0.55f;
                b.collisionRadiusScale = 1.0f;
                b.groundShadow = 0.0f;
                b.selfShadowStrength = 0.0f;
                thicken_params(b);
                GSmokeSystem.Spawn(position, b, -1.0f);
            }
            else if (wanted == 4)
            {
                // Inferno: the panel's INFERNO preset, for scripted capture.
                SmokeParams inf;
                ApplySmokeColorPreset(inf, SmokeColorPreset::Black);
                inf.fire = true;
                inf.opacity = 0.8f;
                inf.selfShadowStrength = 3.2f;
                inf.groundShadow = 2.6f;
                inf.emitterRadius = 4.0f;
                inf.fireRate = 220.0f;
                inf.maxFlames = 400;
                inf.fireStartRadius = 1.2f;
                inf.fireEndRadius = 3.5f;
                inf.fireRiseSpeed = 5.0f;
                inf.fireLifetime = 2.2f;
                inf.fireIntensity = 2.2f;
                inf.fireStretch = 2.2f;
                inf.fireSmokeYield = 0.9f;
                inf.fireSmokeLift = 5.0f;
                inf.startRadius = 1.5f;
                inf.endRadius = 26.0f;
                inf.particleLifetime = 150.0f;
                inf.maxParticles = 6400; // 35/s x 150 s = 5250; 2000 throttled it to a third
                inf.diffusion = 1.6f;
                inf.densityFalloff = 0.95f;
                inf.buoyancyDecay = 0.6f;
                inf.calmRiseBoost = 5.0f;
                inf.fireLightRadius = 22.0f;
                inf.fireLightIntensity = 1.8f;
                inf.fireColumnHeight = 10.0f;
                inf.fireColumnCohesion = 3.5f;
                thicken_params(inf);
                GSmokeSystem.Spawn(position, inf, -1.0f);
            }
            else if (wanted == 3)
            {
                // Burning wreck: black smoke with fire at the base.
                SmokeParams wreck;
                ApplySmokeColorPreset(wreck, SmokeColorPreset::Black);
                wreck.fire = true;
                wreck.opacity = 0.68f;
                wreck.selfShadowStrength = 3.0f;
                wreck.groundShadow = 2.2f;
                wreck.rate = 45.0f;
                wreck.emitterRadius = 1.2f;
                wreck.startRadius = 0.8f;
                wreck.endRadius = 14.0f;
                wreck.particleLifetime = 95.0f;
                wreck.maxParticles = 5200; // 45/s x 95 s = 4275; 900 throttled it to a fifth
                wreck.diffusion = 1.3f;
                wreck.densityFalloff = 0.9f;
                wreck.buoyancyDecay = 0.5f;
                wreck.calmRiseBoost = 4.0f;
                thicken_params(wreck);
                GSmokeSystem.Spawn(position, wreck, -1.0f);
            }
            else
            {
                SmokeParams plain;
                thicken_params(plain);
                GSmokeSystem.Spawn(position, plain, -1.0f);
            }
        }
        if (wanted == 2 && spawned)
        {
            // Legacy cloudlets are independent entities, so the only visible
            // sign that the source is emitting is the world's cloudlet count
            // climbing. Flat at 1 means the source is alive but producing
            // nothing.
            static Foundation::Time nextLog;
            if (Glob.time > nextLog)
            {
                nextLog = Glob.time + 1.0f;
                LOG_INFO(World, "Legacy smoke probe: world cloudlets={}", NCloudlets());
            }
        }
    }

    for (int i = 0; i < _cloudlets.Size();)
    {
        Entity* vehicle = _cloudlets[i];
        vehicle->Simulate(deltaT, prec);
        if (!vehicle->ToDelete())
        {
            i++;
        }
        else
        {
            _cloudlets.Delete(i);
        }
    }
}

void World::SimulateAllVehicles(float deltaT, float noAccDeltaT, Entity* cameraVehicle)
{
    SimVehCost::Costs().ticks++; // PERF-023
    float farValidFor = 1.5;
    {
        SimVehCost::ScopedStage stage(SimVehCost::Stage::Importance);
        if (Glob.time > _farImportanceDistributionTime + farValidFor)
        {
            DistributeFarImportances();
        }
        if (Glob.time > _nearImportanceDistributionTime + 1.0)
        {
            DistributeNearImportances();
        }
    }
    SetActiveChannels();
#define MAX_SIM_STEP_VEHICLES (1.0 / 15)
#define MAX_SIM_STEP_FAST (0.001)

    for (int i = 0; i < _fastVehicles.Size(); i++)
    {
        Entity* vehicle = _fastVehicles[i];
        vehicle->StartFrame();
    }

    {
        SimVehCost::ScopedStage stage(SimVehCost::Stage::Cloudlets);
        SimulateCloudlets(deltaT);
    }
    float toSimVehicles = deltaT;
    float toSimFast = deltaT;
    while (toSimVehicles > MAX_SIM_STEP_VEHICLES)
    {
        {
            SimVehCost::ScopedStage stage(SimVehCost::Stage::Slow);
            SimulateVehicles(MAX_SIM_STEP_VEHICLES, &Entity::SimulateOptimized, cameraVehicle);
        }
        toSimVehicles -= MAX_SIM_STEP_VEHICLES;
        while (toSimFast > toSimVehicles && toSimFast > MAX_SIM_STEP_FAST)
        {
            SimVehCost::ScopedStage stage(SimVehCost::Stage::Fast);
            SimulateFastVehicles(MAX_SIM_STEP_FAST, &Entity::SimulateOptimized);
            toSimFast -= MAX_SIM_STEP_FAST;
        }
    }
    {
        SimVehCost::ScopedStage stage(SimVehCost::Stage::Slow);
        SimulateVehicles(toSimVehicles, &Entity::SimulateRest, cameraVehicle);
    }
    {
        SimVehCost::ScopedStage stage(SimVehCost::Stage::Fast);
        while (toSimFast > MAX_SIM_STEP_FAST)
        {
            SimulateFastVehicles(MAX_SIM_STEP_FAST, &Entity::SimulateOptimized);
            toSimFast -= MAX_SIM_STEP_FAST;
        }
        SimulateFastVehicles(toSimFast, &Entity::SimulateRest);
    }
    {
        SimVehCost::ScopedStage stage(SimVehCost::Stage::Buildings);
        SimulateBuildings(deltaT, &Entity::SimulateOptimized);
    }
    {
        SimVehCost::ScopedStage stage(SimVehCost::Stage::Attached);
        for (int i = 0; i < _attached.Size(); i++)
        {
            _attached[i]->UpdatePosition();
        }
    }
}

float World::Visibility(AIUnit* from, Object* to) const
{
    EntityAI* ai = dyn_cast<EntityAI>(to);
    if (!ai)
    {
        return 1;
    }
    Person* me = from->GetPerson();
    AIUnit* aiUnit = ai->CommanderUnit();
    if (aiUnit || ai->GetType()->IsKindOf(GLOB_WORLD->Preloaded(VTypeStrategic)) ||
        ai->GetType()->IsKindOf(GLOB_WORLD->Preloaded(VTypeAllVehicles)))
    {
        if (!ai->IsInLandscape())
        {
            LOG_DEBUG(World, "Patch: vanished vehicle visibility queried ({} to {})", (const char*)me->GetDebugName(),
                      (const char*)ai->GetDebugName());
            return 1;
        }
        if (aiUnit)
        {
            AIGroup* grp = from->GetGroup();
            if (aiUnit && aiUnit->GetGroup() == grp && aiUnit->GetLifeState() == AIUnit::LSAlive)
            {
                return 1;
            }
        }

        return _sensorList->GetVisibility(me, ai);
    }
    return 1;
}

Foundation::Time World::VisibilityTime(AIUnit* from, Object* to) const
{
    EntityAI* ai = dyn_cast<EntityAI>(to);
    if (!ai)
    {
        return Glob.time;
    }
    Person* me = from->GetPerson();
    AIUnit* aiUnit = ai->CommanderUnit();
    if (aiUnit || ai->GetType()->IsKindOf(GLOB_WORLD->Preloaded(VTypeStrategic)) ||
        ai->GetType()->IsKindOf(GLOB_WORLD->Preloaded(VTypeAllVehicles)))
    {
        if (!ai->IsInLandscape())
        {
            return Glob.time;
        }
        if (aiUnit)
        {
            AIGroup* grp = from->GetGroup();
            if (aiUnit && aiUnit->GetGroup() == grp && aiUnit->GetLifeState() == AIUnit::LSAlive)
            {
                return Glob.time;
            }
        }

        return _sensorList->GetVisibilityTime(me, ai);
    }
    return Glob.time;
}

static const char* MapWaveSound(bool night, int index, float& v)
{
    if (index < 0)
    {
        return nullptr;
    }
    if (index > NEnvSoundPars - 1)
    {
        return nullptr;
    }
    if (v <= 0)
    {
        return nullptr;
    }
    // note: return string must be lowercase
    const SoundPars& pars = night ? EnvSoundParsNight[index] : EnvSoundPars[index];
    v *= pars.vol;
    return pars.name;
}

void World::PerformSound(VehicleList& list, Entity* inside, float deltaT)
{
    if (inside)
    {
        for (int i = 0; i < list.Size(); i++)
        {
            Entity* vehicle = list[i];
            vehicle->Sound(inside == vehicle, deltaT);
        }
    }
    else
    {
        for (int i = 0; i < list.Size(); i++)
        {
            Entity* vehicle = list[i];
            vehicle->Sound(false, deltaT);
        }
    }
}

void World::PerformSound(VehiclesDistributed& list, Entity* inside, float deltaT)
{
    PerformSound(list._visibleNear, inside, deltaT);
    PerformSound(list._visibleFar, inside, deltaT);
    PerformSound(list._invisibleNear, inside, deltaT);
    PerformSound(list._invisibleFar, inside, deltaT);
}

void World::PerformSound(Entity* inside, float deltaT)
{
    int i;
    if (!GSoundsys)
    {
        return;
    }
    const Camera& cam = *_scene.GetCamera();
    Vector3Val pos = cam.Position();
    int x = toIntFloor(pos.X() * InvLandGrid);
    int z = toIntFloor(pos.Z() * InvLandGrid);
    GeographyInfo geogr = GLandscape->GetGeography(x, z);
    float sy = GLandscape->SurfaceYAboveWater(pos.X(), pos.Z());
#define SHOW_ENV 0
    // EXPERIMENTAL (POSEIDON_EXPERIMENTAL_EAX=1, default OFF): the owner's verdict on
    // the first audible version was "klingt nicht richtig und nicht gut", so the SERoom
    // producer below and the EFX parameter blending in SoundSystemOAL are parked behind
    // this flag until the tuning pass happens. Off = the shipped EAX behaviour is
    // EXACTLY the pre-2026-08-30 path: four environments, hard preset switches.
    static const bool experimentalEax = []
    {
        const char* v = std::getenv("POSEIDON_EXPERIMENTAL_EAX");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    // SERoom producer (roadmap 1.4): the classifier covered forest, city,
    // mountains and plain since 2001, and the fifth environment the backend ships was
    // reachable only from the Audio screen's EAX preview. IndoorRoomAt is the strict
    // test -- true only inside a building's fire-geometry room -- NOT IsSheltered, whose
    // documented crudeness (bridges and tree canopy read sheltered) would put a forest
    // canopy into a room reverb. Room size from the clearance to the nearest boundary:
    // a stairwell and a barn hall should not share a decay time. One query per frame at
    // the camera; the same probe the rain occlusion path already leans on.
    // Rate-limited to 4 Hz: IndoorRoomAt walks nearby building interiors, and running
    // it every frame was measurable on the bench (part of a reproducible -0.8 fps on
    // the abel arm). A listener does not cross a doorway in a quarter second the ear
    // can resolve, and the EFX blend smooths the transition anyway.
    static float sRoomNextQuery = -1.0f;
    static bool sRoomInside = false;
    static float sRoomSize = 4.0f;
    const float now = Glob.time.toFloat();
    if (experimentalEax && (now >= sRoomNextQuery || now < sRoomNextQuery - 1.0f))
    {
        sRoomNextQuery = now + 0.25f;
        int listenerRoom = -1;
        sRoomInside = GSmokeWorldQuery().IndoorRoomAt(pos, listenerRoom);
        if (sRoomInside)
        {
            // The env size is in the EAX preset's own scale, NOT metres: ApplyEFXPreset
            // multiplies the preset decay by size/defaultSize, and the "room" preset's
            // default is 1.9. The first version fed clearance*2 (up to 30) straight in,
            // which scaled a 0.4 s room decay toward the 20 s ceiling -- a cathedral in
            // every barn, and exactly the owner's "unrealistically much reverb" report.
            // Map clearance to a modest band instead: a 1 m stairwell ~2.0, a 10 m hall
            // ~6.5, capped at 8 (decay x4.2) so no interior ever exceeds a church.
            const float clearance = GSmokeWorldQuery().IndoorClearanceAt(pos, listenerRoom, 15.0f);
            // Floor is 2.0, not the preset's 1.9: DoSetEAXEnvironment clamps the size
            // to [2, 100] before using it, so anything below 2 was never reachable and
            // a 1.9 floor only made the band look smaller than it is.
            sRoomSize = std::clamp(1.5f + clearance * 0.5f, 2.0f, 8.0f);
        }
    }
    // One place decides which environment this is; ClassifyEnvironment
    // (Poseidon/Audio/EnvironmentClassify.hpp) holds the decision so it can be pinned
    // by unit tests without a landscape or a sound device. The branch bodies moved
    // verbatim -- forest/city/mountains/plain are the 2001 sizes and densities.
    // Sinkhole W1 (Malprave terrainHole4/7): a listener inside the room a hole-cutting object makes -- a cellar,
    // a cave below its mouth, floor to the object's roof -- hears a room. Not behind POSEIDON_EXPERIMENTAL_EAX:
    // there is no older behaviour underground to preserve, and an open-air reverb in a cave is plainly wrong.
    // Same 4 Hz rate as the building probe; InTerrainHoleRoom costs nothing until a hole-cutting model is seen.
    static float sHoleNextQuery = -1.0f;
    static bool sHoleRoom = false;
    if (now >= sHoleNextQuery || now < sHoleNextQuery - 1.0f)
    {
        sHoleNextQuery = now + 0.25f;
        sHoleRoom = GLandscape->InTerrainHoleRoom(pos);
    }
    Audio::EnvironmentSurroundings surroundings;
    surroundings.insideRoom = sRoomInside || sHoleRoom;
    surroundings.roomSize = sRoomInside ? sRoomSize : 6.0f; // cave/cellar: a hall-sized decay, within the 2..8 band
    surroundings.forest = geogr.u.forestInner || geogr.u.forestOuter;
    surroundings.hardObjects = geogr.u.howManyHardObjects;
    surroundings.objects = geogr.u.howManyObjects;
    surroundings.surfaceYAboveWater = sy;

    const SoundEnvironment env = Audio::ClassifyEnvironment(surroundings);
    GSoundsys->SetEnvironment(env);
#if SHOW_ENV
    {
        static const char* const kNames[] = {"Plain", "City", "Forest", "Mountains", "Room"};
        GlobalShowMessage(100, "%s %.1f %.1f", kNames[env.type], env.size, env.density);
    }
#endif

    GSoundsys->SetListener(cam.Position(), cam.Speed(), cam.Direction(), cam.DirectionUp());
    PerformSound(_vehicles, inside, deltaT);
    PerformSound(_animals, inside, deltaT);
    PerformSound(_buildings, inside, deltaT);
    PerformSound(_fastVehicles, inside, deltaT);
#if 1
    int s[5];
    float v[5];
    const Vector3& camPos = cam.Position();
    float camHeight = camPos[1] - _scene.GetLandscape()->SurfaceY(camPos[0], camPos[2]);
    float canHear = 1 - camHeight * 1.0 / 200;
    saturate(canHear, 0, 1);
    float vCoef = 0.5 * canHear;

    if (stricmp(EnvSoundPars[5].name, "SOUND\\$DEFAULT$.WSS") != 0)
    {
        s[0] = 5;
        v[0] = 2.0 * vCoef;
        s[1] = -1;
        v[1] = 0;
        s[2] = -1;
        v[2] = 0;
        s[3] = -1;
        v[3] = 0;
    }
    else
    {
        float xGrid = camPos.X() * InvLandGrid - 0.5;
        float zGrid = camPos.Z() * InvLandGrid - 0.5;
        int x = toIntFloor(xGrid);
        int z = toIntFloor(zGrid);
        float xFrac = xGrid - x;
        float zFrac = zGrid - z;
        s[0] = GLOB_LAND->GetSound(x, z);
        s[1] = GLOB_LAND->GetSound(x + 1, z);
        s[2] = GLOB_LAND->GetSound(x, z + 1);
        s[3] = GLOB_LAND->GetSound(x + 1, z + 1);
        // volume: bilinear by xFrac,zFrac
        v[0] = (1 - xFrac + 1 - zFrac) * vCoef;
        v[1] = (xFrac + 1 - zFrac) * vCoef;
        v[2] = (1 - xFrac + zFrac) * vCoef;
        v[3] = (xFrac + zFrac) * vCoef;
    }
    s[4] = -1;
    v[4] = 0;
    const float thold = 0.1;
    float rain = _scene.GetLandscape()->GetRainDensity() - thold;
    if (rain >= 0)
    {
        float coef = rain * (1 / (1 - thold));
        v[0] *= 1 - coef;
        v[1] *= 1 - coef;
        v[2] *= 1 - coef;
        v[3] *= 1 - coef;
        s[4] = 4, v[4] = coef;
    }
    // sum of all v[x] is 1
    // merge idenl sXX
    {
        for (int i = 0; i < 5; i++)
        {
            for (int j = 0; j < i; j++)
            {
                if (s[j] == s[i])
                {
                    s[j] = -1;
                    v[i] += v[j];
                }
            }
        }
    }
    const char* ss[5];
    bool night = _scene.MainLight()->NightEffect() > 0.5;
    for (i = 0; i < 5; i++)
    {
        ss[i] = MapWaveSound(night, s[i], v[i]);
    }
    GSoundScene->SetEnvSound(ss[0], v[0]);
    GSoundScene->SetEnvSound(ss[1], v[1]);
    GSoundScene->SetEnvSound(ss[2], v[2]);
    GSoundScene->SetEnvSound(ss[3], v[3]);
    GSoundScene->SetEnvSound(ss[4], v[4]);
    GSoundScene->AdvanceEnvSounds();
#endif
}

void World::UnloadSounds(VehicleList& list)
{
    for (int i = 0; i < list.Size(); i++)
    {
        list[i]->UnloadSound();
    }
}

void World::UnloadSounds(VehiclesDistributed& list)
{
    UnloadSounds(list._visibleNear);
    UnloadSounds(list._visibleFar);
    UnloadSounds(list._invisibleNear);
    UnloadSounds(list._invisibleFar);
}

void World::UnloadSounds()
{
    UnloadSounds(_vehicles);
    UnloadSounds(_buildings);
    UnloadSounds(_animals);
    UnloadSounds(_fastVehicles);
    if (GSoundScene)
        GSoundScene->Reset();
}

void World::AdjustSubdivisionGrid(float gridSize)
{
    const float invLog2 = 1 / log(2);
    float coefLog = log(LandGrid / gridSize) * invLog2;
    int coefLogInt = toInt(coefLog);
    saturate(coefLogInt, 0, 8);
    const int requestedLog = coefLogInt;
    coefLogInt = BoundedTerrainSubdivision(LandRange, coefLogInt);
    LOG_DEBUG(World, "Terrain subdivision wanted: {} ({:.3f})", coefLogInt, coefLog);
    int currentLog = TerrainRangeLog - LandRangeLog;
    int terrainChange = coefLogInt - currentLog;
    if (requestedLog != coefLogInt && terrainChange != 0)
        LOG_INFO(World, "Terrain refinement capped for {} land cells: requested {}m, effective {}m",
                 LandRange, gridSize, LandGrid / static_cast<float>(1 << coefLogInt));

    if (terrainChange > 0)
    {
        if (!GLandscape->LoadSubdivCache(coefLogInt))
        {
            GLandscape->SubdivideTerrain(terrainChange);
            GLandscape->SaveSubdivCache(coefLogInt);
        }
    }
    else if (terrainChange < 0)
    {
        GLandscape->ResampleTerrain(-terrainChange);
    }
}

void World::AdjustSubdivision(GameMode mode)
{
    DWORD tSub = GetTickCount();
    float gridSize = GScene->GetPreferredTerrainGrid();
    float viewDist = GScene->GetPreferredViewDistance();
    // MP requires all clients use the same grid size.
    if (mode == GModeNetware)
    {
        gridSize = PreferredGridSizeMP;
        viewDist = 900;
    }
    AdjustSubdivisionGrid(gridSize);
    SetVisibility(viewDist);
    LOG_DEBUG(Core, "LOAD: AdjustSubdivision {}ms (grid={} vd={})", GetTickCount() - tSub, gridSize, viewDist);
}

void World::ActivateAddons(const FindArrayRStringCI& addons)
{
    _activeAddons.Clear();
    const ParamEntry& def = Pars >> "CfgAddons" >> "PreloadAddons";
    for (int c = 0; c < def.GetEntryCount(); c++)
    {
        const ParamEntry& cc = def.GetEntry(c);
        if (!cc.IsClass())
        {
            continue;
        }
        if (!cc.FindEntry("list"))
        {
            continue;
        }
        const ParamEntry& cl = cc >> "list";
        for (int i = 0; i < cl.GetSize(); i++)
        {
            RString addon = cl[i];
            _activeAddons.Add(addon);
        }
    }
    for (int i = 0; i < addons.Size(); i++)
    {
        RString addon = addons[i];
        LOG_DEBUG(World, "Activating addon {}", (const char*)addon);
        _activeAddons.Add(addon);
    }
}

bool World::CheckAddon(const ParamEntry& entry)
{
    bool visible = entry.CheckVisible(_activeAddons);
    if (visible)
    {
        return true;
    }
    if (!_options)
    {
        return false;
    }
    RString addon = entry.GetOwner();
    bool registered = _options->DoUnregisteredAddonUsed(addon);
    if (registered)
    {
        _activeAddons.Add(addon);
    }
    return registered;
}

void World::SwitchLandscape(const char* name)
{
    DWORD t0 = GetTickCount();
    GSoundScene->Reset();
    GDebugger.NextAliveExpected(15 * 60 * 1000);
    VehicleTypes.LockAllTypes();
    GEngine->TextBank()->LockAllTextures();
    CleanUp();
    Landscape* land = _scene.GetLandscape();
    if (!land)
    {
        Fail("No landscape");
        return;
    }

    const char* lName = land->GetName();
    if (!lName || strcmpi(name, lName))
    {
        char islandName[256];
        const char* fname = strrchr(name, '\\');
        if (!fname)
        {
            fname = name;
        }
        else
        {
            fname++;
        }
        snprintf(islandName, sizeof(islandName), "%s", (const char*)fname);
        char* ext = strchr(islandName, '.');
        if (ext)
        {
            *ext = 0;
        }
        strcpy(Glob.header.worldname, islandName);

        const ParamEntry& cls = Pars >> "CfgWorlds" >> Glob.header.worldname;
        float grid = cls >> "LandGrid";
        DWORD tLoad = GetTickCount();
        land->LoadData(name, grid);
        LOG_INFO(Core, "World load phase: Landscape::LoadData {} ms -- '{}'", GetTickCount() - tLoad, name);

        DWORD tCfg = GetTickCount();
        ParseCfgWorld();
        LOG_INFO(Core, "World load phase: ParseCfgWorld {} ms", GetTickCount() - tCfg);

        DWORD tInit = GetTickCount();
        InitLandscape(land);
        const DWORD initLandscapeMs = GetTickCount() - tInit;
        DWORD tIds = GetTickCount();
        land->RebuildIDCache();
        LOG_INFO(Core, "World load phase: InitLandscape {} ms, RebuildIDCache {} ms", initLandscapeMs,
                 GetTickCount() - tIds);
    }
    else
    {
        land->ResetState();
        Reset();
        land->FlushCache();
        if (!_map)
        {
            _showMap = false;
        }
    }

    ENGINE_CONFIG.tacticalZ = 900;
    ENGINE_CONFIG.horizontZ = 900;
    ENGINE_CONFIG.objectsZ = 600;
    ENGINE_CONFIG.shadowsZ = 250;

    InputSubsystem::Instance().ResetLookAroundToggle();

    DWORD tSim = GetTickCount();
    GLandscape->Simulate(0);
    GScene->ResetFog();
    LOG_INFO(Core, "World load phase: Landscape::Simulate(0)+ResetFog {} ms", GetTickCount() - tSim);

    DWORD tFlush = GetTickCount();
    _engine->TextBank()->FlushTextures();
    GetNetworkManager().CleanUpMemory();
    MemoryCleanUp();
    LOG_INFO(Core, "World load phase: FlushTextures+MemoryCleanUp {} ms; SwitchLandscape total {} ms",
             GetTickCount() - tFlush, GetTickCount() - t0);
}

void World::AddVehicle(Entity* vehicle)
{
#if LOG_ADD_REMOVE_VEHICLE
    LOG_DEBUG(World, "World::AddVehicle {}", (const char*)vehicle->GetDebugName());
#endif
    DoAssert(vehicle->RefCounter() == 0 || _vehicles.Find(vehicle) < 0);
    _vehicles.Add(vehicle);
}
void World::RemoveVehicle(Entity* vehicle)
{
#if LOG_ADD_REMOVE_VEHICLE
    LOG_DEBUG(World, "World::RemoveVehicle {}", (const char*)vehicle->GetDebugName());
#endif
    DoAssert(_vehicles.Find(vehicle) >= 0);
    _vehicles.Remove(vehicle);
}
void World::InsertVehicle(Entity* vehicle)
{
#if LOG_ADD_REMOVE_VEHICLE
    LOG_DEBUG(World, "World::InsertVehicle {}", (const char*)vehicle->GetDebugName());
#endif
    DoAssert(vehicle->RefCounter() == 0 || _vehicles.Find(vehicle) < 0);
    _vehicles.Insert(vehicle);
}
void World::DeleteVehicle(Entity* vehicle)
{
#if LOG_ADD_REMOVE_VEHICLE
    LOG_DEBUG(World, "World::DeleteVehicle {}", (const char*)vehicle->GetDebugName());
#endif
    DoAssert(_vehicles.Find(vehicle) >= 0);
    _vehicles.Delete(vehicle);
}

void World::AddOutVehicle(Entity* vehicle)
{
#if LOG_ADD_REMOVE_VEHICLE
    LOG_DEBUG(World, "World::AddOutVehicle {}", (const char*)vehicle->GetDebugName());
#endif
    DoAssert(vehicle->RefCounter() == 0 || _outVehicles.Find(vehicle) < 0);
    _outVehicles.Add(vehicle);
}

void World::RemoveOutVehicle(Entity* vehicle)
{
#if LOG_ADD_REMOVE_VEHICLE
    LOG_DEBUG(World, "World::RemoveOutVehicle {}", (const char*)vehicle->GetDebugName());
#endif
    DoAssert(_outVehicles.Find(vehicle) >= 0);
    _outVehicles.Delete(vehicle);
}

bool World::ValidateOutVehicle(Entity* veh, bool complex) const
{
    bool ok = true;
    for (int i = 0; i < NVehicles(); i++)
    {
        Entity* v = GetVehicle(i);
        if (v == veh)
        {
            ok = false;
            RptF("Out Vehicle %s in normal list", (const char*)veh->GetDebugName());
        }
    }

    for (int zz = 0; zz < LandRange; zz++)
    {
        for (int xx = 0; xx < LandRange; xx++)
        {
            const ObjectList& list = GLandscape->GetObjects(zz, xx);
            for (int i = 0; i < list.Size(); i++)
            {
                if (list[i] == veh)
                {
                    RptF("Out Vehicle %s in landscape (%d,%d)", (const char*)veh->GetDebugName(), xx, zz);
                }
            }
        }
    }

    return ok;
}

bool World::ValidateOutVehicles(bool complex) const
{
    bool ok = true;
    for (int i = 0; i < NOutVehicles(); i++)
    {
        Entity* veh = GetOutVehicle(i);
        if (!ValidateOutVehicle(veh, complex))
        {
            ok = false;
        }
    }
    return ok;
}

bool World::CheckVehicleStructure() const
{
    bool ok = true;
    for (int i = 0; i < NVehicles(); i++)
    {
        Entity* vehicle = GetVehicle(i);
        EntityAI* veh = dyn_cast<EntityAI>(vehicle);
        AIUnit* unit = veh->CommanderUnit();
        if (unit)
        {
            if (!unit->AssertValid())
            {
                ok = false;
            }
        }
        unit = veh->PilotUnit();
        if (unit)
        {
            if (!unit->AssertValid())
            {
                ok = false;
            }
        }
        unit = veh->GunnerUnit();
        if (unit)
        {
            if (!unit->AssertValid())
            {
                ok = false;
            }
        }
    }
    for (int i = 0; i < NOutVehicles(); i++)
    {
        Entity* vehicle = GetOutVehicle(i);
        EntityAI* veh = dyn_cast<EntityAI>(vehicle);
        AIUnit* unit = veh->CommanderUnit();
        if (unit)
        {
            if (!unit->AssertValid())
            {
                ok = false;
            }
        }
        unit = veh->PilotUnit();
        if (unit)
        {
            if (!unit->AssertValid())
            {
                ok = false;
            }
        }
        unit = veh->GunnerUnit();
        if (unit)
        {
            if (!unit->AssertValid())
            {
                ok = false;
            }
        }
    }
    return ok;
}

inline bool IsPrimary(Object* vehicle)
{
    return (vehicle->GetType() == Primary || vehicle->GetType() == Network);
}

void World::ResetIDs() const
{
    Log("World::ResetIDs");
    _scene.GetLandscape()->ResetObjectIDs();
    int i;
    for (i = 0; i < NVehicles(); i++)
    {
        Entity* vehicle = GetVehicle(i);
        if (IsPrimary(vehicle))
        {
            PoseidonAssert(vehicle->ID() >= 0);
            continue;
        }
        int id = _scene.GetLandscape()->NewObjectID();
        vehicle->SetID(id);
    }
    for (i = 0; i < NAnimals(); i++)
    {
        Entity* vehicle = GetAnimal(i);
        if (IsPrimary(vehicle))
        {
            PoseidonAssert(vehicle->ID() >= 0);
            continue;
        }
        int id = _scene.GetLandscape()->NewObjectID();
        vehicle->SetID(id);
    }
    for (i = 0; i < NBuildings(); i++)
    {
        Entity* vehicle = GetBuilding(i);
        if (IsPrimary(vehicle))
        {
            PoseidonAssert(vehicle->ID() >= 0);
            continue;
        }
        int id = _scene.GetLandscape()->NewObjectID();
        vehicle->SetID(id);
    }
    for (i = 0; i < NFastVehicles(); i++)
    {
        Entity* vehicle = GetFastVehicle(i);
        if (IsPrimary(vehicle))
        {
            Fail("Fast primary vehicle");
            PoseidonAssert(vehicle->ID() >= 0);
            continue;
        }
        int id = _scene.GetLandscape()->NewObjectID();
        vehicle->SetID(id);
    }
    for (i = 0; i < NOutVehicles(); i++)
    {
        Entity* vehicle = GetOutVehicle(i);
        if (IsPrimary(vehicle))
        {
            Fail("Out primary vehicle");
            PoseidonAssert(vehicle->ID() >= 0);
            continue;
        }
        int id = _scene.GetLandscape()->NewObjectID();
        vehicle->SetID(id);
    }
}
void World::RemoveIDs() const
{
    Fail("Obsolete - do not use");
    Log("World::RemoveIDs");
    _scene.GetLandscape()->ResetObjectIDs();
    int i;
    for (i = 0; i < NVehicles(); i++)
    {
        Entity* vehicle = GetVehicle(i);
        if (IsPrimary(vehicle))
        {
            continue;
        }
        vehicle->SetID(-1);
    }
    for (i = 0; i < NAnimals(); i++)
    {
        Entity* vehicle = GetAnimal(i);
        if (IsPrimary(vehicle))
        {
            continue;
        }
        vehicle->SetID(-1);
    }
    for (i = 0; i < NBuildings(); i++)
    {
        Entity* vehicle = GetBuilding(i);
        if (IsPrimary(vehicle))
        {
            continue;
        }
        vehicle->SetID(-1);
    }
    for (i = 0; i < NOutVehicles(); i++)
    {
        Entity* vehicle = GetOutVehicle(i);
        if (IsPrimary(vehicle))
        {
            continue;
        }
        vehicle->SetID(-1);
    }
}

} // namespace Poseidon
#include <Poseidon/Core/SaveVersion.hpp>
#include <Poseidon/UI/Settings/AspectRatio.hpp>

namespace Poseidon
{
RString GetBaseDirectory();
RString GetBaseSubdirectory();
RString GetCampaignSaveDirectory(RString campaign);
bool ParseMission(bool);
void StartCampaign(RString, Display*);
void OpenEditor();
} // namespace Poseidon

LSError SerializeWorldSimulationTime(ParamArchive& ar)
{
    using namespace Poseidon;
    int simulationTimeMs = Glob.time.toInt();
    PARAM_CHECK(ar.Serialize("SimulationTimeMs", simulationTimeMs, 13, 0))
    Glob.time = Foundation::Time(simulationTimeMs);
    return LSOK;
}
namespace Poseidon
{

LSError World::Load(const char* name, int message)
{
    Fail("Text load obsolete");
    GDebugger.NextAliveExpected(15 * 60 * 1000);
    ParamArchiveLoad ar(name);
    ar.FirstPass();
    LSError err = Serialize(ar, message);
    if (err == LSOK)
    {
        ar.SecondPass();
        err = Serialize(ar, message);
    }
    if (err == LSOK)
    {
    }
    else
    {
        ErrorMessage("Cannot load '%s'. Error '%s' at '%s'.", name, ar.GetErrorName(err),
                     (const char*)ar.GetErrorContext());
    }
    return err;
}

LSError World::Save(const char* name, int message) const
{
    Fail("Text save obsolete");
    GDebugger.NextAliveExpected(15 * 60 * 1000);
    ParamArchiveSave ar(WorldSerializeVersion);
    World* w = const_cast<World*>(this);
    PARAM_CHECK(w->Serialize(ar, message))
    return ar.Save(name);
}

bool World::LoadBin(const char* name, int message)
{
    LOG_DEBUG(World, "LoadBin: Start - Total allocated: {} MB", Foundation::MemoryUsed() / (1024 * 1024));
    LSError err;
    {
        GDebugger.NextAliveExpected(15 * 60 * 1000);
        ParamArchiveLoad ar;
        bool result = ar.LoadBin(name);
        if (!result)
        {
            return false;
        }

        LOG_DEBUG(World, "Load: Total allocated after ar.LoadBin: {} MB", Foundation::MemoryUsed() / (1024 * 1024));
        ar.FirstPass();
        err = Serialize(ar, message);
        if (err == LSOK)
        {
            ar.SecondPass();
            err = Serialize(ar, message);
        }
        if (err == LSOK)
        {
        }
        else
        {
            ErrorMessage("Cannot load '%s'. Error '%s' at '%s'.", name, ar.GetErrorName(err),
                         (const char*)ar.GetErrorContext());
        }
        LOG_DEBUG(World, "Load: Total allocated after World::Serialize: {} MB",
                  Foundation::MemoryUsed() / (1024 * 1024));
    }
    LOG_DEBUG(World, "Total allocated after ~ParamArchive: {} MB", Foundation::MemoryUsed() / (1024 * 1024));
    MemoryCleanUp();
    LOG_DEBUG(World, "Total allocated after MemoryCleanUp: {} MB", Foundation::MemoryUsed() / (1024 * 1024));
    if (err == LSOK)
        AspectRatio::SetGameplayActive(true);
    return err == LSOK;
}

bool World::SaveBin(const char* name, int message) const
{
    bool ret;
    {
        GDebugger.NextAliveExpected(15 * 60 * 1000);
        ParamArchiveSave ar(WorldSerializeVersion);
        World* w = const_cast<World*>(this);
        LOG_DEBUG(World, "SaveBin: Start - Total allocated: {} MB", Foundation::MemoryUsed() / (1024 * 1024));
        if (w->Serialize(ar, message) != LSOK)
        {
            return false;
        }

        LOG_DEBUG(World, "Total allocated after World::Serialize: {} MB", Foundation::MemoryUsed() / (1024 * 1024));
        ret = ar.SaveBin(name);
        LOG_DEBUG(World, "Total allocated after ar.SaveBin: {} MB", Foundation::MemoryUsed() / (1024 * 1024));
    }

    LOG_DEBUG(World, "Total allocated after ~ParamArchive: {} MB", Foundation::MemoryUsed() / (1024 * 1024));
    MemoryCleanUp();
    LOG_DEBUG(World, "Total allocated after MemoryCleanUp: {} MB", Foundation::MemoryUsed() / (1024 * 1024));
    return ret;
}

LSError World::SerializeVehicles(ParamArchive& ar)
{
    // Note: PARAM_CHECK(ar.Serialize("Cloudlets", _cloudlets, 1))
    PARAM_CHECK(ar.Serialize("FastVehicles", _fastVehicles, 1))
    PARAM_CHECK(ar.Serialize("Vehicles", _vehicles, 1))
    PARAM_CHECK(ar.Serialize("Animals", _animals, 1))
    PARAM_CHECK(ar.Serialize("Buildings", _buildings, 1))
    PARAM_CHECK(ar.Serialize("OutVehicles", _outVehicles, 1))
    if (ar.IsLoading())
    {
        for (int i = 0; i < _outVehicles.Size(); i++)
        {
            Entity* veh = _outVehicles[i];
            veh->SetMoveOutFlag();
        }
    }
    PARAM_CHECK(ar.Serialize("NearImportance", _nearImportanceDistributionTime, 1))
    PARAM_CHECK(ar.Serialize("FarImportance", _farImportanceDistributionTime, 1))
    return LSOK;
}

template <>
const ::Poseidon::Foundation::EnumName* ::Poseidon::Foundation::GetEnumNames(GameMode dummy)
{
    static const ::Poseidon::Foundation::EnumName GameModeNames[] = {
        ::Poseidon::Foundation::EnumName(GModeNetware, "NETWARE"),
        ::Poseidon::Foundation::EnumName(GModeArcade, "ARCADE"),
        ::Poseidon::Foundation::EnumName(GModeIntro, "INTRO"),
        ::Poseidon::Foundation::EnumName(GModeArcade, "NORMAL"),
        ::Poseidon::Foundation::EnumName(GModeArcade, "TRAINING"),
        ::Poseidon::Foundation::EnumName()};
    return GameModeNames;
}

bool ProcessTemplateName(RString name);
bool ProcessFullName(RString name);
void StartIntro();
void StartMission();
} // namespace Poseidon
namespace Poseidon
{
LSError World::Serialize(ParamArchive& ar, int message)
{
    // Refuse unknown layouts before changing campaign, globals or live entities.
    PARAM_CHECK(ar.CheckMaxVersion(WorldSerializeVersion))
    if (ar.IsSaving())
    {
        ProgressReset();
        ProgressClear(false);
        ProgressStart(LocalizeString(message));
        PARAM_CHECK(ar.Serialize("CurrentCampaign", CurrentCampaign, 1, ""))
    }
    else if (ar.GetPass() == ParamArchive::PassFirst)
    {
        RString campaign;
        PARAM_CHECK(ar.Serialize("CurrentCampaign", campaign, 1, ""))
        SetCampaign(campaign);
    }

    PARAM_CHECK(ar.Serialize("CurrentBattle", CurrentBattle, 1, ""))
    PARAM_CHECK(ar.Serialize("CurrentMission", CurrentMission, 1, ""))

    PARAM_CHECK(ar.SerializeEnum("mode", _mode, 1))
    PARAM_CHECK(ar.SerializeEnum("endMission", _endMission, 1, (EndMode)EMContinue))

    PARAM_CHECK(ar.Serialize("cadetMode", USER_CONFIG.easyMode, 1, false))

    PARAM_CHECK(ar.Serialize("nextMagazineID", _nextMagazineID, 1, 0))

    PARAM_CHECK(ar.Serialize("enableRadio", _enableRadio, 1, true))
    PARAM_CHECK(ar.Serialize("Radio", *_radio, 1))
    //	PARAM_CHECK(ar.Serialize("Map", *_map, 11))
    PARAM_CHECK(SerializeMapInfo(ar, "Map", 11))

    Landscape* land = _scene.GetLandscape();
    if (ar.IsSaving())
    {
        ResetIDs();
        land->RebuildIDCache();

        RString worldName = land->GetName();
        PARAM_CHECK(ar.Serialize("worldName", worldName, 1))

        AutoArray<RString> addons;
        for (int i = 0; i < _activeAddons.GetSize(); i++)
        {
            addons.Add(_activeAddons.Get(i));
        }
        PARAM_CHECK(ar.SerializeArray("addons", addons, 1))
    }
    else if (ar.GetPass() == ParamArchive::PassFirst)
    {
        VehicleTypes.LockAllTypes();
        GEngine->TextBank()->LockAllTextures();

        Clear();
        CurrentTemplate.Clear();

        RString worldName;
        PARAM_CHECK(ar.Serialize("worldName", worldName, 1))
        SwitchLandscape(worldName);
        AdjustSubdivision(GModeArcade);

        ProgressReset();
        ProgressStart(LocalizeString(message));
        FindArrayRStringCI addons;
        PARAM_CHECK(ar.SerializeArray("addons", addons, 1))
        ActivateAddons(addons);
    }
    DWORD tLand = GetTickCount();
    PARAM_CHECK(ar.Serialize("Landscape", *land, 1))
    LOG_DEBUG(Core, "LOAD: Serialize Landscape {}ms", GetTickCount() - tLand);

    DWORD tVeh = GetTickCount();
    PARAM_CHECK(SerializeVehicles(ar))
    LOG_DEBUG(Core, "LOAD: SerializeVehicles {}ms", GetTickCount() - tVeh);
    PARAM_CHECK(ar.Serialize("SensorList", _sensorList, 1))
    // A save written without a SensorList subclass deserializes the SRef as null
    // (ParamArchive::Serialize sets value = nullptr when the named entry is absent). The
    // world invariant is "always has a (possibly empty) sensor list" — World::Simulate
    // dereferences GetSensorList() unconditionally, so a null here is a latent crash
    // (SensorList::CheckPos, this = null). Restore an empty list on the final load pass:
    // doing it earlier would leave _sensorList non-null while the entry is still absent,
    // which trips the second-pass serialize's OpenSubclass into LSNoEntry.
    if (!ar.IsSaving() && ar.GetPass() == ParamArchive::PassSecond && !_sensorList)
    {
        _sensorList = new SensorList;
    }
    PARAM_CHECK(ar.Serialize("Clock", Glob.clock, 1))
    PARAM_CHECK(SerializeWorldSimulationTime(ar))
    PARAM_CHECK(ar.Serialize("GameState", GGameState, 1))

    PARAM_CHECK(ar.Serialize("actualOvercast", _actualOvercast, 1))
    PARAM_CHECK(ar.Serialize("wantedOvercast", _wantedOvercast, 1))
    PARAM_CHECK(ar.Serialize("actualFog", _actualFog, 1))
    PARAM_CHECK(ar.Serialize("wantedFog", _wantedFog, 1))
    PARAM_CHECK(ar.Serialize("speedOvercast", _speedOvercast, 1))
    PARAM_CHECK(ar.Serialize("weatherTime", _weatherTime, 1))
    PARAM_CHECK(ar.Serialize("nextWeatherChange", _nextWeatherChange, 1))

    // Wind authority. Only the AUTHORED conditions are stored: the live vector is
    // a closed form of (conditions, overcast, clock), and both overcast and the
    // clock are already in this archive above — so a restored save resumes the
    // exact wind it was saved with, with no extra state and no drift.
    // Every field carries a default equal to the authored constant, so archives
    // written before this block load unchanged.
    {
        WindConditions wind = GWind.Conditions();
        int windSeed = static_cast<int>(wind.seed);
        PARAM_CHECK(ar.Serialize("windDirection", wind.baseDirectionRad, 1, 0.46364760f))
        PARAM_CHECK(ar.Serialize("windCalmSpeed", wind.calmSpeed, 1, 1.5f))
        PARAM_CHECK(ar.Serialize("windOvercastSpeed", wind.overcastSpeed, 1, 7.5f))
        PARAM_CHECK(ar.Serialize("windGustiness", wind.gustiness, 1, 0.55f))
        PARAM_CHECK(ar.Serialize("windVeer", wind.veerAmplitudeRad, 1, 0.55f))
        PARAM_CHECK(ar.Serialize("windSeed", windSeed, 1, 1337))
        if (!ar.IsSaving())
        {
            wind.seed = static_cast<uint32_t>(windSeed);
            GWind.SetConditions(wind);
        }
    }

    PARAM_CHECK(ar.Serialize("horizontZ", ENGINE_CONFIG.horizontZ, 1, 900))
    PARAM_CHECK(ar.Serialize("tacticalZ", ENGINE_CONFIG.tacticalZ, 1, 900))
    PARAM_CHECK(ar.Serialize("objectsZ", ENGINE_CONFIG.objectsZ, 1, 600))
    PARAM_CHECK(ar.Serialize("shadowsZ", ENGINE_CONFIG.shadowsZ, 1, 250))

    PARAM_CHECK(ar.SerializeRef("playerOn", _playerOn, 1))
    PARAM_CHECK(ar.SerializeRef("cameraOn", _cameraOn, 1))
    PARAM_CHECK(ar.SerializeRef("realPlayer", _realPlayer, 1))
    PARAM_CHECK(ar.Serialize("playerManual", _playerManual, 1, true))
    PARAM_CHECK(ar.Serialize("playerSuspended", _playerSuspended, 1, false))

    PARAM_CHECK(ar.Serialize("EastCenter", _eastCenter, 1))
    PARAM_CHECK(ar.Serialize("WestCenter", _westCenter, 1))
    PARAM_CHECK(ar.Serialize("GuerrilaCenter", _guerrilaCenter, 1))
    PARAM_CHECK(ar.Serialize("CivilianCenter", _civilianCenter, 1))
    PARAM_CHECK(ar.Serialize("LogicCenter", _logicCenter, 1))
    PARAM_CHECK(AIGlobalSerialize(ar))

    PARAM_CHECK(ar.Serialize("Scripts", _scripts, 3))

    PARAM_CHECK(ar.Serialize("OnMapSingleClick", GMapOnSingleClick, 1, RString()))

    PARAM_CHECK(ar.Serialize("CameraEffect", _cameraEffect, 1))

    if (ar.IsSaving())
    {
        RString dir = GetBaseDirectory();
        PARAM_CHECK(ar.Serialize("directory", dir, 1, ""))
        dir = GetBaseSubdirectory();
        PARAM_CHECK(ar.Serialize("subdirectory", dir, 1, ""))
        RString mission = Glob.header.filename;
        PARAM_CHECK(ar.Serialize("mission", mission, 1, ""))
        PARAM_CHECK(ar.Serialize("filenameReal", Glob.header.filenameReal, 1, ""))
        ProgressFinish();
    }
    else if (ar.GetPass() == ParamArchive::PassFirst)
    {
        char wname[256];
        snprintf(wname, sizeof(wname), "%s", (const char*)land->GetName());
        char* ext = strrchr(wname, '.');
        if (ext)
        {
            *ext = 0;
        }
        char* world = strrchr(wname, '\\');
        if (world)
        {
            world++;
        }
        else
        {
            world = wname;
        }

        RString dir;
        PARAM_CHECK(ar.Serialize("directory", dir, 1, ""))
        SetBaseDirectory(dir == GetUserMissionsBase(), dir);
        RString mission;
        PARAM_CHECK(ar.Serialize("mission", mission, 1, ""))
        SetMission(world, mission);
        PARAM_CHECK(ar.Serialize("subdirectory", dir, 1, ""))
        SetBaseSubdirectory(dir);
        PARAM_CHECK(ar.Serialize("filenameReal", Glob.header.filenameReal, 1, ""))

        ParseMission(false);
    }
    else
    {
        DWORD tFinal = GetTickCount();
        _camType = _camTypeMain = CamInternal;
        InitCameraPars();

        _scene.MainLight()->Recalculate(this);
        _scene.MainLightChanged();

        AIUnit* player = _playerOn ? _playerOn->Brain() : nullptr;
        AIGroup* grp = player ? player->GetGroup() : nullptr;
        AICenter* center = grp ? grp->GetCenter() : nullptr;
        Glob.header.playerSide = center ? center->GetSide() : TSideUnknown;

        VehicleTypes.UnlockAllTypes();
        GEngine->TextBank()->UnlockAllTextures();
        DWORD tPreload = GetTickCount();
        GEngine->TextBank()->Preload();
        LOG_DEBUG(Core, "LOAD: TextBank Preload {}ms", GetTickCount() - tPreload);

        DWORD tOpt = GetTickCount();
        Shapes.OptimizeAll();
        LOG_DEBUG(Core, "LOAD: Shapes.OptimizeAll {}ms", GetTickCount() - tOpt);

        DisplayMap* map = dynamic_cast<DisplayMap*>((AbstractOptionsUI*)_map);
        if (map)
        {
            map->UpdatePlan();
        }

        LOG_DEBUG(Core, "LOAD: Final pass total {}ms", GetTickCount() - tFinal);
        ProgressFinish();
    }

    return LSOK;
}

void World::DoKeyDown(unsigned wParam, unsigned nRepCnt, unsigned nFlags)
{
    if (!IsUserInputEnabled())
    {
        return;
    }

    if (_warningMessage)
    {
        _warningMessage->OnKeyDown(wParam, nRepCnt, nFlags);
        if (_warningMessage->GetExitCode() >= 0)
        {
            _warningMessage = nullptr;
        }
        return;
    }
    if (_voiceChat)
    {
        if (_voiceChat->DoKeyDown(wParam, nRepCnt, nFlags))
        {
            return;
        }
    }
    if (_chat)
    {
        if (_chat->DoKeyDown(wParam, nRepCnt, nFlags))
        {
            return;
        }
    }
    if (!HasOptions())
    {
        if (_userDlg)
        {
            if (_userDlg->DoKeyDown(wParam, nRepCnt, nFlags))
            {
                return;
            }
        }
        else if (_map && _showMap)
        {
            if (_map->DoKeyDown(wParam, nRepCnt, nFlags))
            {
                return;
            }
        }
    }
    if (_options)
    {
        if (_options->DoKeyDown(wParam, nRepCnt, nFlags))
        {
            return;
        }
    }
}

bool World::DoControllerUiAction(ControllerUiAction action)
{
    if (!IsUserInputEnabled())
        return false;

    ControlsContainer* topmost = UIActiveDisplay::FindTopmost(this);
    DisplayArcadeMap* editor = dynamic_cast<DisplayArcadeMap*>(topmost);
    if (editor)
        return editor->DoControllerUiAction(action);
    DisplayMission* mission = dynamic_cast<DisplayMission*>(topmost);
    if (mission)
        return mission->DoControllerUiAction(action);
    if (topmost && FindMissionOwner(topmost))
    {
        if (auto* display = dynamic_cast<Display*>(topmost))
            return display->DoControllerUiAction(action);
    }
    if (topmost && FindEditorOwner(topmost))
        return DoEditorChildControllerUiAction(topmost, action);
    if (auto* display = dynamic_cast<Display*>(topmost))
        return display->DoControllerUiAction(action);
    return false;
}

ControllerUiScene World::GetControllerUiScene() const
{
    ControlsContainer* topmost = UIActiveDisplay::FindTopmost(const_cast<World*>(this));
    DisplayArcadeMap* editor = dynamic_cast<DisplayArcadeMap*>(topmost);
    if (editor)
        return editor->GetControllerUiScene();

    DisplayMission* mission = dynamic_cast<DisplayMission*>(topmost);
    if (mission)
        return mission->GetControllerUiScene();

    if (topmost && FindMissionOwner(topmost))
    {
        if (auto* display = dynamic_cast<Display*>(topmost))
            return display->GetControllerUiScene();
    }

    if (topmost && FindEditorOwner(topmost))
        return EditorDialogControllerScene();

    if (auto* display = dynamic_cast<Display*>(topmost))
        return display->GetControllerUiScene();

    return GameplayControllerScene();
}

bool World::IsEditorControllerUiActive()
{
    const ControllerUiScene scene = GetControllerUiScene();
    return scene.kind == ControllerSceneKind::EditorMap || scene.kind == ControllerSceneKind::EditorDialog;
}

void World::DoKeyUp(unsigned wParam, unsigned nRepCnt, unsigned nFlags)
{
    if (!IsUserInputEnabled())
    {
        return;
    }

    if (_warningMessage)
    {
        _warningMessage->OnKeyUp(wParam, nRepCnt, nFlags);
        if (_warningMessage->GetExitCode() >= 0)
        {
            _warningMessage = nullptr;
        }
        return;
    }
    if (_voiceChat)
    {
        if (_voiceChat->DoKeyUp(wParam, nRepCnt, nFlags))
        {
            return;
        }
    }
    if (_chat)
    {
        if (_chat->DoKeyUp(wParam, nRepCnt, nFlags))
        {
            return;
        }
    }
    if (!HasOptions())
    {
        if (_userDlg)
        {
            if (_userDlg->DoKeyUp(wParam, nRepCnt, nFlags))
            {
                return;
            }
        }
        else if (_map && _showMap)
        {
            if (_map->DoKeyUp(wParam, nRepCnt, nFlags))
            {
                return;
            }
        }
    }
    if (_options)
    {
        if (_options->DoKeyUp(wParam, nRepCnt, nFlags))
        {
            return;
        }
    }
}

void World::DoChar(unsigned nChar, unsigned nRepCnt, unsigned nFlags)
{
    if (!IsUserInputEnabled())
    {
        return;
    }

    if (_warningMessage)
    {
        _warningMessage->OnChar(nChar, nRepCnt, nFlags);
        if (_warningMessage->GetExitCode() >= 0)
        {
            _warningMessage = nullptr;
        }
        return;
    }
    if (_voiceChat)
    {
        if (_voiceChat->DoChar(nChar, nRepCnt, nFlags))
        {
            return;
        }
    }
    if (_chat)
    {
        if (_chat->DoChar(nChar, nRepCnt, nFlags))
        {
            return;
        }
    }
    if (!HasOptions())
    {
        if (_userDlg)
        {
            if (_userDlg->DoChar(nChar, nRepCnt, nFlags))
            {
                return;
            }
        }
        else if (_map && _showMap)
        {
            if (_map->DoChar(nChar, nRepCnt, nFlags))
            {
                return;
            }
        }
    }
    if (_options)
    {
        if (_options->DoChar(nChar, nRepCnt, nFlags))
        {
            return;
        }
    }
}

void World::DoIMEChar(unsigned nChar, unsigned nRepCnt, unsigned nFlags)
{
    if (!IsUserInputEnabled())
    {
        return;
    }

    if (_warningMessage)
    {
        _warningMessage->OnIMEChar(nChar, nRepCnt, nFlags);
        if (_warningMessage->GetExitCode() >= 0)
        {
            _warningMessage = nullptr;
        }
        return;
    }
    if (_voiceChat)
    {
        if (_voiceChat->DoIMEChar(nChar, nRepCnt, nFlags))
        {
            return;
        }
    }
    if (_chat)
    {
        if (_chat->DoIMEChar(nChar, nRepCnt, nFlags))
        {
            return;
        }
    }
    if (!HasOptions())
    {
        if (_userDlg)
        {
            if (_userDlg->DoIMEChar(nChar, nRepCnt, nFlags))
            {
                return;
            }
        }
        else if (_map && _showMap)
        {
            if (_map->DoIMEChar(nChar, nRepCnt, nFlags))
            {
                return;
            }
        }
    }
    if (_options)
    {
        if (_options->DoIMEChar(nChar, nRepCnt, nFlags))
        {
            return;
        }
    }
}

void World::DoIMEComposition(unsigned nChar, unsigned nFlags)
{
    if (!IsUserInputEnabled())
    {
        return;
    }

    if (_warningMessage)
    {
        _warningMessage->OnIMEComposition(nChar, nFlags);
        if (_warningMessage->GetExitCode() >= 0)
        {
            _warningMessage = nullptr;
        }
        return;
    }
    if (_voiceChat)
    {
        if (_voiceChat->DoIMEComposition(nChar, nFlags))
        {
            return;
        }
    }
    if (_chat)
    {
        if (_chat->DoIMEComposition(nChar, nFlags))
        {
            return;
        }
    }
    if (!HasOptions())
    {
        if (_userDlg)
        {
            if (_userDlg->DoIMEComposition(nChar, nFlags))
            {
                return;
            }
        }
        else if (_map && _showMap)
        {
            if (_map->DoIMEComposition(nChar, nFlags))
            {
                return;
            }
        }
    }
    if (_options)
    {
        if (_options->DoIMEComposition(nChar, nFlags))
        {
            return;
        }
    }
}

void World::SaveCrash() const
{
    SaveBin("$_crash_$.fps", IDS_SAVE_GAME);
}

void SaveCrash()
{
    GWorld->SaveCrash();
}

CameraEffect::CameraEffect(Object* object) : _object(object) {}
CameraEffect::~CameraEffect() = default;

void CameraEffect::Draw() const
{
    if (showCinemaBorder)
    {
        Object cinema(GScene->Preloaded(CinemaBorder), -1);
        cinema.Draw2D(0);
        // The CinemaBorder model is 4:3-designed; on wider viewports
        // its bars don't reach the screen edges.  See Object::
        // DrawWidescreenPillarbox for the why.
        Object::DrawWidescreenPillarbox();
    }
}

void World::StartIntro()
{
    CurrentBattle = "";
    CurrentMission = "";

    const char* ext = strrchr(LoadFile, '.');
    if (ext && QIFStreamB::FileExist(LoadFile))
    {
        if (!strcmpi(ext, ".fps"))
        {
            LoadBin(LoadFile, IDS_LOAD_GAME);
            StartMission();
        }
        else if (!strcmpi(ext, ".sqg"))
        {
            Load(LoadFile, IDS_LOAD_GAME);
            StartMission();
        }
        else if (GameModuleRegistry::IsRegistered(GameModuleId::Editor) && !strcmpi(ext, ".sqm"))
        {
            if (ProcessFullName(LoadFile))
            {
                if (AutoTest)
                {
                    const bool autoTestBooted = StartAutoTest();
#if POSEIDON_DIAG
                    // DIAG-001: boot result; on failure the error level and the problems recorded while loading
                    // (classes missing from addOns[], script errors, sounds that failed)
                    Dev::OpDiag::OnBoot(autoTestBooted, static_cast<const char*>(LoadFile),
                                        autoTestBooted ? -1 : static_cast<int>(GetMaxError()));
#endif
                    if (!autoTestBooted)
                    {
                        LOG_ERROR(Core, "StartAutoTest could not boot '{}'", LoadFile);
                        // The usual cause is InitVehicles' error latch
                        // (GetMaxError() >= EMError), and the message behind it
                        // lives only in WarningText -- WarningMessageLevel
                        // stores it and shows a dialog, but never logs it, so a
                        // headless run failed with no reason on record. Say it.
                        if (GetMaxError() >= Poseidon::Foundation::EMError)
                        {
                            LOG_ERROR(Core, "StartAutoTest error latch: {}",
                                      (const char*)GetMaxErrorMessage());
                        }
                        if (AppConfig::Instance().IsMissionSmokeCheck())
                        {
                            LOG_ERROR(Core, "Mission smoke check failed: StartAutoTest could not boot '{}'", LoadFile);
                            GApp->m_exitCode = 1;
                            GApp->m_closeRequest = true;
                        }
                    }
                }
                else
                {
                    OpenEditor();
                }
            }
        }
    }
    else
    {
        RString campaign = Pars >> "CfgIntro" >> "firstCampaign";
        if (campaign.GetLength() > 0 &&
            QIFStream::FileExists(GetCampaignSaveDirectory(campaign) + RString("continue.fps")))
        {
            StartCampaign(campaign, nullptr);
        }
        else
        {
            RString world = GetMenuInitWorld();
            StartRandomCutscene(world);
        }
    }
}

void World::StopIntro() {}

void World::StartLogo() {}

void World::StopLogo() {}
} // namespace Poseidon
