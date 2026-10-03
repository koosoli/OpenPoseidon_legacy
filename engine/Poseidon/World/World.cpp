#include <SDL3/SDL_scancode.h>
#include <Poseidon/Dev/Diag/OpDiag.hpp> // DIAG-001 hooks, empty unless POSEIDON_DIAG

#include <Poseidon/AI/AITimeline.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Config/UserConfig.hpp>
#include <Poseidon/Core/Game/FixedStepAccumulator.hpp>
#include <Poseidon/Core/TickStateHash.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <cstdlib>
#include <Poseidon/Dev/Diag/FixedStepStats.hpp>
#include <Poseidon/Dev/Diag/PhysicsCorpus.hpp>
#include <Poseidon/Dev/Diag/PhysicsProbe.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/SimStageGraph.hpp>
#include <Poseidon/AI/AICostAccum.hpp> // PERF-014: the AI stage's per-bucket accumulator
#include "SimVehicleCost.hpp" // PERF-023: the Vehicles stage, by sub-stage and kind
#include "Weather/RainWaterCost.hpp"
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/WorldChatInput.hpp>
#include <Poseidon/World/WorldInputContext.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/Input/CheatCode.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Graphics/Rendering/Frame/WorldFrameObserver.hpp>
#include <Poseidon/Graphics/Rendering/Frame/PresentationSnapshot.hpp>
#include <Poseidon/Audio/Speaker.hpp>
#include <stdint.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Memory/CheckMem.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Memtype.h>

using namespace Poseidon;
extern void SDLGamepad_SetEngine(float mag);
extern void SDLGamepad_PlayRamp(float beg, float end, float dur);
#include <Poseidon/Graphics/Rendering/Primitives/ClipVert.hpp>

#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Entities/Infantry/SwimView.hpp>
#include <Poseidon/World/Terrain/Visibility.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/World/Scene/Camera/CamEffects.hpp>
#include <Poseidon/Game/TitEffects.hpp>
#include <Poseidon/Game/Scripting/Scripts.hpp>
#include <Poseidon/Dev/Debug/DebugCheats.hpp>
#include <Poseidon/Dev/Debug/DebugTrap.hpp>
#include <Poseidon/Dev/Diag/FrameProfiler.hpp>
#include <Poseidon/Dev/Diag/StreamingDiag.hpp>

#include <Poseidon/Dev/Diag/DiagModes.hpp>
#include <Poseidon/Dev/Diag/DiagPause.hpp>
#include <Poseidon/Dev/Diag/BallisticsRecorder.hpp>
#include <chrono>

#include <Poseidon/Core/Progress.hpp>

#include <Poseidon/Graphics/GraphicsEngineFactory.hpp>
#include <Poseidon/World/Scene/Camera/AerialRange.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Camera/CameraHold.hpp>
#include <Poseidon/World/Entities/Vehicles/Air/Airplane.hpp>
#include <Poseidon/World/Entities/Vehicles/Air/Helicopter.hpp>
#include <Poseidon/World/Entities/Vehicles/Ground/Car.hpp>
#include <Poseidon/World/Entities/Vehicles/Ground/Tank.hpp>
#include <Poseidon/World/Entities/Vehicles/Misc/Ship.hpp>

#include <Poseidon/Game/Commands/GameStateExt.hpp>

#include <Poseidon/AI/AI.hpp>

#include <Poseidon/Game/Chat.hpp>

#include <Poseidon/Network/Network.hpp>

#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Graphics/Cursor/ICursorOverlay.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/Map/UIMap.hpp>
#include <Poseidon/UI/Map/UIMapCommon.hpp>

#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>

#include <Poseidon/Dev/Debug/WtrTestHarness.hpp>

#include <Poseidon/UI/Locale/StringtableExt.hpp>

#include <Poseidon/World/WorldShared.hpp>
AbstractOptionsUI* CreateChannelUI();

namespace Poseidon
{
#undef GetObject
#undef DrawText
// #undef LoadString

} // namespace Poseidon
#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Graphics/Cursor/ICursorOverlay.hpp>
using namespace Poseidon::Dev;
using Poseidon::Foundation::IsOutOfMemory;
namespace Poseidon
{
extern int gPerfDrawCalls;
}

namespace
{

/// FAR-001. Resolve the altitude-dependent far plane, and push the matching haze
/// range into the scene. Returns the far plane to hand to
/// `Camera::SetPerspectiveForView`.
///
/// This is the ONE place the two numbers are decided, and it is deliberately the
/// same statement that used to conflate them (World.cpp, the camera setup at the
/// end of the per-frame camera update) rather than something buried in the scene:
/// the far plane is a camera property, so the camera's own setup is where a
/// reader will look for it.
///
/// Altitude is measured ABOVE THE TERRAIN, not above sea level. Fifty metres over
/// a nine-hundred-metre mountain is low flying and has to keep looking like low
/// flying; only genuine altitude buys reach.
///
/// Below the policy's start altitude this returns `scene.GetFogMaxRange()`
/// unchanged and does not touch the scene at all -- not even to write the same
/// value back -- so ordinary ground play runs exactly the code it ran before, and
/// the fog tables are not rebuilt. That is what the ground-level control capture
/// is checking.
float ApplyAerialRange(Poseidon::Scene& scene, Vector3Val camPos)
{
    // The range the engine would have used without any of this: the value
    // `ResetFog` derives from the view-distance slider. Asking the scene for its
    // BASE range (rather than its current one) matters, because the current one
    // may already carry a previous frame's aerial override and feeding that back
    // in would ratchet.
    const float fogBase = scene.GetBaseFogMaxRange();

    float altAboveGround = camPos.Y();
    if (Poseidon::GLandscape)
    {
        // SurfaceYAboveWater rather than SurfaceY: over the sea the relevant floor
        // is the water, and a camera 3 m above the waves must not be treated as
        // 3 m + the depth of the seabed.
        altAboveGround = camPos.Y() - Poseidon::GLandscape->SurfaceYAboveWater(camPos.X(), camPos.Z());
    }

    // wgpu overrides the projection to INFINITE-FAR REVERSED-Z, where depth
    // precision is governed by the near plane and is entirely insensitive to the
    // far plane -- opening it to 24 km there costs nothing. GL33 keeps a finite
    // forward-Z projection, where precision is a far/near ratio and 24 km over a
    // 0.08 m near plane is exactly what z-fights, so it gets the much smaller
    // `finiteFarReach` instead. The backend cannot be asked about this through
    // any Engine capability flag, hence ActiveBackendCode().
    const char* backend = Poseidon::GraphicsEngineFactory::ActiveBackendCode();
    const bool infiniteFarZ = backend != nullptr && std::strcmp(backend, "wgpu") == 0;

    const Poseidon::Aerial::AerialRange r = Poseidon::Aerial::Resolve(fogBase, altAboveGround, infiniteFarZ);

    // Inert: leave the scene alone entirely. `SetAerialFogMaxRange(0)` is still
    // cheap (it early-outs when already zero) and is what releases a previous
    // frame's override as an aircraft descends.
    scene.SetAerialFogMaxRange(r.active ? r.fogMaxRange : 0.0f);
    return r.farPlane;
}

} // namespace

// Time acceleration limits (PageUp/PageDown in-game)
constexpr double kTimeAccMin = 1.0;
constexpr double kTimeAccMax = 4.0;

#include <Poseidon/World/WorldSimHelpers.inc>

// Test/debug render-view override.  When active, World::Simulate forces the
// scene camera to an exact world-space transform instead of the player-derived
// view.  Driven by the `triSetView` test verb; lets a dev-cheats position dump
// reproduce a captured view without a mission camera object.
static bool s_triViewActive = false;
static Matrix4 s_triViewTransform(MIdentity);

void World_SetTriViewOverride(Vector3Par pos, Vector3Par dir, Vector3Par up)
{
    s_triViewTransform.SetDirectionAndUp(dir, up);
    s_triViewTransform.SetPosition(pos);
    s_triViewActive = true;
}

void World_ClearTriViewOverride()
{
    s_triViewActive = false;
}

void World::UpdateInputContext()
{
    InputSubsystem::Instance().SetContext(ResolveInputContext());
}

void World::TogglePersonView()
{
    // Body moved verbatim from the UAPersonView key handler in Simulate, so the
    // dev-panel button and the key share one behaviour. The difficulty gate is
    // NOT checked here -- SetCamera already forces the camera back to
    // CamInternal every frame when the active profile disables DT3rdPersonView,
    // so an ungated toggle cannot bypass it; it just changes what the gate is
    // applied to, exactly as the key does.
    if (_cameraExternal)
    {
        if (_camTypeMain == CamExternal && !_showMap)
        {
            _cameraExternal = false;
            _camTypeMain = CamInternal;
        }
        else
        {
            _camTypeMain = CamExternal;
        }
    }
    else
    {
        if (_camTypeMain == CamInternal && !_showMap)
        {
            _cameraExternal = true;
            _camTypeMain = CamExternal;
        }
        else
        {
            _camTypeMain = CamInternal;
        }
    }
    _showMap = false;
}

void World::Simulate(float deltaT, bool& enableDraw)
{
    // Frame-phase profiler — feeds the dev panel Perf tab and triPerfStats.
    Dev::FrameProfiler& perf = Dev::GFrameProfiler();
    perf.BeginFrame();
    float noAccDeltaT = deltaT;
    UpdateInputContext();
    auto& input = InputSubsystem::Instance();

    // Viewer-mode controls run BEFORE the rest of the simulation step
    // so the viewer can consume scancodes (Esc, F5, Space, R, O, ?)
    // before any game-side handler sees them.  No-op outside viewer mode.
    TickViewerControls(deltaT);

    OLink<Object> cameraVehicle = _cameraOn;
    OLink<Entity> camVehicle = dyn_cast<Entity, Object>(cameraVehicle);
    OLink<EntityAI> camAI = dyn_cast<EntityAI, Entity>(camVehicle);
    OLink<Person> person = FocusOn() ? FocusOn()->GetPerson() : nullptr;

    if (input.CheatActivated() == CheatCrash)
    {
        input.CheatServed();
        volatile int a = *(int*)nullptr;
        (void)a;
    }

    if (_editor)
    {
#if _ENABLE_CHEATS
        if (input.GetCheat1ToDo(SDL_SCANCODE_C))
        {
            AbstractOptionsUI* CreateDebugConsole();
            if (!_options)
                _options = CreateDebugConsole();
        }
#endif
    }
    else
    {
        ProcessNetwork();

#if _ENABLE_CHEATS
        if (input.GetCheat1ToDo(SDL_SCANCODE_R))
        {
            USER_CONFIG.easyMode = !USER_CONFIG.easyMode;
            Foundation::GlobalShowMessage(500, "%s", USER_CONFIG.easyMode ? "Cadet" : "Veteran");
        }
        if (input.GetCheat2ToDo(SDL_SCANCODE_Z))
        {
            ENGINE_CONFIG.super = !ENGINE_CONFIG.super;
            Foundation::GlobalShowMessage(500, "Immortality %s", ENGINE_CONFIG.super ? "On" : "Off");
        }
        if (input.GetCheat1ToDo(SDL_SCANCODE_M))
        {
            showCinemaBorder = !showCinemaBorder;
        }
        if (input.GetCheat2ToDo(SDL_SCANCODE_SLASH))
        {
            void ExportOperMaps(RString prefix);
            ExportOperMaps(Glob.header.worldname);
        }
#endif

        if (_mode == GModeNetware)
        {
            if (AppConfig::Instance().IsSimulateMode())
                _acceleratedTime = (float)AppConfig::Instance().GetTimeScale();
            else
                _acceleratedTime = 1;
        }
        else
#if !_ENABLE_CHEATS
            if (!_cameraEffect)
#endif
        {
            if (input.GetActionToDo(UATimeInc))
            {
                _acceleratedTime *= 2;
                saturate(_acceleratedTime, kTimeAccMin, kTimeAccMax);
                Foundation::GlobalShowMessage(1000, LocalizeString(IDS_TIME_ACC_FORMAT), _acceleratedTime);
                // SetActiveChannels();
            }
            if (input.GetActionToDo(UATimeDec))
            {
                _acceleratedTime *= 0.5;
                input.ConsumeKeyPress(SDL_SCANCODE_PAGEDOWN);
                saturate(_acceleratedTime, kTimeAccMin, kTimeAccMax);
                Foundation::GlobalShowMessage(1000, LocalizeString(IDS_TIME_ACC_FORMAT), _acceleratedTime);
                // SetActiveChannels();
            }
        }
        deltaT *= _acceleratedTime;
    }

    if (input.CheatActivated() == CheatGodMode)
    {
        if (_mode != GModeNetware)
            Dev::DebugCheats::Cmd_God::SetActive(true);
        input.CheatServed();
    }

    if (input.CheatActivated() == CheatSaveGame)
    {
        if (_mode != GModeNetware)
        {
            RString name = GetSaveDirectory() + RString("save.fps");
            GWorld->SaveBin(name, IDS_SAVE_GAME);
        }
        input.CheatServed();
    }
#if _ENABLE_CHEATS
    if (input.GetCheat1ToDo(SDL_SCANCODE_P))
    {
        _enableSimulation = !_enableSimulation;
    }
    if (input.GetCheat1ToDo(SDL_SCANCODE_S))
    {
        RString dir = GetTmpSaveDirectory();
        char filename[256];
        for (int i = 1; i <= 99999; i++)
        {
            snprintf(filename, sizeof(filename), "%sTMP%05d.fps", (const char*)dir, i);
            if (!QIFStream::FileExists(filename))
            {
                SaveBin(filename, IDS_SAVE_GAME);
                break;
            }
        }
    }
    if (input.GetCheat1ToDo(SDL_SCANCODE_L))
    {
        RString dir = GetTmpSaveDirectory();
        char filename[256];
        snprintf(filename, sizeof(filename), "%sTMP%05d.fps", (const char*)dir, 1);
        if (QIFStream::FileExists(filename))
        {
            // some binary save exist
            for (int i = 1; i <= 99999; i++)
            {
                snprintf(filename, sizeof(filename), "%sTMP%05d.fps", (const char*)dir, i);
                if (!QIFStream::FileExists(filename))
                {
                    if (i > 1)
                    {
                        snprintf(filename, sizeof(filename), "%sTMP%05d.fps", (const char*)dir, i - 1);
                        LoadBin(filename, IDS_LOAD_GAME);
                    }
                    break;
                }
            }
        }
        else
        {
            for (int i = 1; i <= 99999; i++)
            {
                snprintf(filename, sizeof(filename), "%sTMP%05d.sqg", (const char*)dir, i);
                if (!QIFStream::FileExists(filename))
                {
                    if (i > 1)
                    {
                        snprintf(filename, sizeof(filename), "%sTMP%05d.sqg", (const char*)dir, i - 1);
                        Load(filename, IDS_LOAD_GAME);
                    }
                    break;
                }
            }
        }
    }
    if (input.GetCheat2ToDo(SDL_SCANCODE_B))
    {
        DebugOperMapTrouble();
    }
    if (input.GetCheat2ToDo(SDL_SCANCODE_M))
    {
        DebugOperMap();
    }
    if (input.GetCheat2ToDo(SDL_SCANCODE_X))
    {
        EntityAI* veh = dyn_cast<EntityAI>(CameraOn());
        AIUnit* unit = veh ? veh->PilotUnit() : nullptr;
        DebugOperMap(unit);
    }
    if (input.GetCheat2ToDo(SDL_SCANCODE_PERIOD))
    {
        Pars.SaveBin("bin\\config.bin");
        Res.SaveBin("bin\\resource.bin");
    }
    if (input.GetCheat2ToDo(SDL_SCANCODE_BACKSLASH))
    {
        forceControlsPaused = !forceControlsPaused;
        if (forceControlsPaused)
            GEngine->ShowMessage(500, "controls paused on");
        else
            GEngine->ShowMessage(500, "controls paused off");
    }
    const static int cheatVar[] = {SDL_SCANCODE_0, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
                                   SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9};
    for (int i = 0; i < sizeof(cheatVar) / sizeof(*cheatVar); i++)
    {
        if (input.GetCheat1ToDo(cheatVar[i]))
        {
            char varName[64];
            snprintf(varName, sizeof(varName), "cheat%d", i);
            GetGameState()->VarSet(varName, true, false);
            break;
        }
    }
#endif

    // Diagnostic pause hotkey — Ctrl+P, dev builds only.
    //
    // Ctrl is tested first and the P edge is consumed only when it is held, so
    // a plain P press still reaches whatever else wants it.  ConsumeDevKeyPress
    // reports nothing without --dev, so a release player cannot reach this.
    // (SDL_SCANCODE_PAUSE is already taken by the T&L toggle in
    // Core/Game/GameLoop.cpp:100, which is why this is not on the Pause key.)
    if (input.IsKeyDown(SDL_SCANCODE_LCTRL) || input.IsKeyDown(SDL_SCANCODE_RCTRL))
    {
        if (input.ConsumeDevKeyPress(SDL_SCANCODE_P))
        {
            const bool paused = Dev::ToggleDiagPause();
            Foundation::GlobalShowMessage(800, "Simulation %s", paused ? "PAUSED (free-fly live)" : "running");
        }
    }

    // Ballistics diagnostic sampling.  Returns immediately unless the dev-panel
    // switch is on; placed before the draw phase so the trails submitted below
    // are this frame's positions rather than last frame's.
    Dev::Ballistics::Sample();

    Glob.uiTime += noAccDeltaT;
    // multiplayer chat control
    if (GetNetworkManager().GetGameState() >= NGSCreate && _chat)
    {
        const Poseidon::ChatInputActions actions = Poseidon::PollChatInputActions(input);
        if (!IsPlayerDead())
        {
            if (actions.previousChannel)
            {
                PrevChatChannel();
                if (_channel)
                {
                    _channel->ResetHUD();
                }
                if (_voiceChat)
                {
                    _voiceChat->ResetHUD();
                }
                OnChannelChanged();
            }
            if (actions.nextChannel)
            {
                NextChatChannel();
                if (_channel)
                {
                    _channel->ResetHUD();
                }
                if (_voiceChat)
                {
                    _voiceChat->ResetHUD();
                }
                OnChannelChanged();
            }
        }
        if (actions.historyUp)
            GChatList.BrowseUp();
        if (actions.historyDown)
            GChatList.BrowseDown();
    }

    if (GetNetworkManager().GetGameState() >= NGSCreate &&
        Poseidon::ShouldHandleMultiplayerChatShortcut(_chat != nullptr))
    {
        if (IsPlayerDead())
        {
            if (ActualChatChannel() != CCGlobal)
            {
                SetChatChannel(CCGlobal);
                if (_channel)
                {
                    _channel->ResetHUD();
                }
                if (_voiceChat)
                {
                    _voiceChat->ResetHUD();
                }
                OnChannelChanged();
            }
        }
        else
        {
            if (input.GetActionToDo(UAPrevChannel, true, false))
            {
                PrevChatChannel();
                if (_channel)
                {
                    _channel->ResetHUD();
                }
                if (_voiceChat)
                {
                    _voiceChat->ResetHUD();
                }
                OnChannelChanged();
            }

            if (input.GetActionToDo(UANextChannel, true, false))
            {
                NextChatChannel();
                if (_channel)
                {
                    _channel->ResetHUD();
                }
                if (_voiceChat)
                {
                    _voiceChat->ResetHUD();
                }
                OnChannelChanged();
            }
        }

        HandleVoiceChatShortcuts();
    }

    if (_chat || _voiceChat || _channelChanged >= Glob.uiTime - 3.0f)
    {
        if (!_channel)
        {
            _channel = CreateChannelUI();
        }
    }
    else
    {
        if (_channel)
        {
            _channel = nullptr;
        }
    }

    DisplayMap* map = static_cast<DisplayMap*>((AbstractOptionsUI*)_map);
    _showCompass = map && map->IsShownCompass() && input.GetAction(UACompass) > 0;
    _showWatch = map && map->IsShownWatch() && input.GetAction(UAWatch) > 0;

    bool enableOptics = false;
    bool forceOptics = false;

    if (camAI && !_cameraEffect)
    {
        if (!camAI->DisableWeapons())
        {
            if (person)
            {
                enableOptics = camAI->GetOpticsModel(person) != nullptr;
                forceOptics = camAI->GetForceOptics(person);
            }
        }
    }

    bool isNV = GEngine->GetNightVision();
    bool enableNV = false;
    bool wantNV = false;
    if (person)
    {
        enableNV = person->IsNVEnabled();
        wantNV = person->IsNVWanted();
    }
    if (!_cameraEffect && IsSimulationEnabled())
    {
        if (!HasMap() || _map->IsTopmost())
        {
            if (input.GetActionToDo(UAPersonView))
            {
                TogglePersonView();
            }
            if (input.GetActionToDo(UAOptics))
            {
                if (_camTypeMain == CamGunner && !_showMap)
                {
                    if (_cameraExternal)
                    {
                        _camTypeMain = CamExternal;
                    }
                    else
                    {
                        _camTypeMain = CamInternal;
                    }
                }
                else
                {
                    _camTypeMain = CamGunner;
                }
                _showMap = false;
            }
            if (input.GetActionToDo(UATacticalView))
            {
                if (_camTypeMain == CamGroup && !_showMap)
                {
                    if (_cameraExternal)
                    {
                        _camTypeMain = CamExternal;
                    }
                    else
                    {
                        _camTypeMain = CamInternal;
                    }
                }
                else if (FocusOn() && FocusOn()->IsGroupLeader() && FocusOn()->GetGroup()->NUnits() > 1)
                {
                    _camTypeMain = CamGroup;
                    if (UI())
                    {
                        UI()->ShowMe();
                    }
                }
                _showMap = false;
            }
            if (input.GetActionToDo(UAMap))
            {
                if (_showMap)
                {
                    _showMap = false;
                }
                else
                {
                    if (_map)
                    {
                        _map->ResetHUD();
                    }
                    _showMap = true;
                }
            }
        }
    }
    if (_cameraEffect)
    {
        _showMap = false;
    }

    bool enableExternal = USER_CONFIG.IsEnabled(DT3rdPersonView);
    if (!enableExternal)
    {
        _cameraExternal = false;
        if (_camTypeMain == CamExternal)
        {
            _camTypeMain = CamInternal;
        }
    }

    bool forcedZoom = false;
    if (FocusOn())
    {
#if _ENABLE_CHEATS
        static bool enableAnyCamera = false;
        if (input.GetCheat2ToDo(SDL_SCANCODE_P))
        {
            enableAnyCamera = !enableAnyCamera;
        }
        if (enableAnyCamera)
        {
            _camTypeMain = CamGroup;
            goto CameraOK;
        }
#endif
        if (_camTypeMain == CamGroup)
        {
            if (FocusOn()->IsGroupLeader() && FocusOn()->GetGroup()->NUnits() > 1 && enableExternal)
            {
                goto CameraOK;
            }
            else if (_cameraExternal)
            {
                _camTypeMain = CamExternal;
            }
            else
            {
                _camTypeMain = CamInternal;
            }
        }
        // _camTypeMain != CamGroup

    CameraOK:

        _camType = _camTypeMain;
        if (_camType == CamGunner)
        {
            if (enableOptics)
            {
            }
            else if (_cameraExternal)
            {
                _camType = CamExternal;
            }
            else
            {
                _camType = CamInternal;
            }
        }
        // _camType != CamGunner
        if ((_camType == CamInternal || _camType == CamExternal) && forceOptics)
        {
            _camType = CamGunner;
        }
        if (input.GetAction(UALockTarget) && (_camType == CamInternal || _camType == CamExternal))
        {
            EntityAI* veh = FocusOn()->GetVehicle();
            PoseidonAssert(veh);
            int curWeapon = veh->SelectedWeapon();
            bool allowZoom = true;
            if (curWeapon >= 0 && curWeapon < veh->NMagazineSlots())
            {
                const MagazineSlot& slot = veh->GetMagazineSlot(curWeapon);
                const MuzzleType* muzzle = slot._muzzle;
                bool canLock =
                    muzzle->_canBeLocked == 2 || muzzle->_canBeLocked == 1 && USER_CONFIG.IsEnabled(DTAutoGuideAT);
                if (canLock)
                {
                    allowZoom = false;
                }
            }
            if (allowZoom)
            {
                // zoom in
                forcedZoom = true;
            }
        }
    }
    else
    {
        if (_cameraExternal)
        {
            _camTypeMain = CamExternal;
        }
        else
        {
            _camTypeMain = CamInternal;
        }
        _camType = _camTypeMain;
        if (input.GetAction(UALockTarget) && (_camType == CamInternal || _camType == CamExternal))
        {
            // zoom in
            forcedZoom = true;
        }
    }

    if (!IsSimulationEnabled() || _showMap || !enableNV)
    {
        wantNV = false;
    }

    {
        if (isNV != wantNV)
        {
            GEngine->SetNightVision(wantNV);
            _scene.MainLightChanged();
        }
    }

    {
#if _ENABLE_CHEATS
        {
            if (input.GetCheat1(SDL_SCANCODE_U))
            {
                _actualOvercast += noAccDeltaT * 0.1 * input.GetCheat1(SDL_SCANCODE_U);
                saturate(_actualOvercast, 0, 1);
                _wantedOvercast = _actualOvercast;
                GLOB_LAND->SetOvercast(_actualOvercast);
            }
            if (input.GetCheat1(SDL_SCANCODE_I))
            {
                _actualOvercast -= noAccDeltaT * 0.1 * input.GetCheat1(SDL_SCANCODE_I);
                saturate(_actualOvercast, 0, 1);
                _wantedOvercast = _actualOvercast;
                GLOB_LAND->SetOvercast(_actualOvercast);
            }
            if (input.GetCheat1(SDL_SCANCODE_COMMA))
            {
                _actualFog += noAccDeltaT * 0.1 * input.GetCheat1(SDL_SCANCODE_COMMA);
                saturate(_actualFog, 0, 1);
                _wantedFog = _actualFog;
                GLOB_LAND->SetFog(_actualFog);
            }
            if (input.GetCheat1(SDL_SCANCODE_PERIOD))
            {
                _actualFog -= noAccDeltaT * 0.1 * input.GetCheat1(SDL_SCANCODE_PERIOD);
                saturate(_actualFog, 0, 1);
                _wantedFog = _actualFog;
                GLOB_LAND->SetFog(_actualFog);
            }

#if _ENABLE_CHEATS
            if (!_showMap && input.GetCheat1ToDo(SDL_SCANCODE_V))
            {
                // cycle through diagnostic modes
                if (DiagMode != 0)
                {
                    DiagMode = 0;
                    Foundation::GlobalShowMessage(100, "Diag off");
                }
                else
                {
                    DiagMode = (1 << DECombat) | (1 << DEPath);
                    Foundation::GlobalShowMessage(100, "Diag: Combat + Path");
                }
            }
#endif

            if (!_showMap && input.GetCheat1ToDo(SDL_SCANCODE_X))
            {
                DisableTextures = !DisableTextures;
            }
        }

        if (input.GetCheat1ToDo(SDL_SCANCODE_J))
        {
            BrowseCamera(-1);
            InitCameraPars();
        }
        if (input.GetCheat1ToDo(SDL_SCANCODE_K))
        {
            BrowseCamera(+1);
            InitCameraPars();
        }
        if (input.GetCheat2ToDo(SDL_SCANCODE_L))
        {
            for (int i = 0; i < NAnimals(); i++)
            {
                Entity* vehicle = GetAnimal(i);
                if (!vehicle->GetName())
                    continue;
                if (!strcmpi(vehicle->GetName(), "SeaGull"))
                {
                    BrowseCamera(vehicle);
                    break;
                }
            }
        }
        if (input.GetCheat2ToDo(SDL_SCANCODE_K))
        {
            QOFStream out;
            Object* camOn = _cameraOn;
            if (camOn)
            {
                char buf[1024];
                Vector3Val pos = camOn->Position();
                Vector3Val dir = camOn->Direction();
                float posSY = GLandscape->SurfaceYAboveWater(pos.X(), pos.Z());
                snprintf(buf, sizeof(buf), "'%s' setPos [%.3f,%.3f,%.3f]\r\n", (const char*)camOn->GetDebugName(),
                         pos.X(), pos.Z(), pos.Y() - posSY);
                out.write(buf, strlen(buf));
                float head = atan2(dir.X(), dir.Z());
                snprintf(buf, sizeof(buf), "'%s' setDir %.0f\r\n", (const char*)camOn->GetDebugName(),
                         head * (180 / H_PI));
                out.write(buf, strlen(buf));
                out.export_clip("clipboard.txt");
            }
        }

        if (input.GetCheat1ToDo(SDL_SCANCODE_F))
        {
            int fps = GLOB_ENGINE->ShowFps() + 1;
            if (fps > 3)
                fps = 0;
            GLOB_ENGINE->ToggleFps(fps);
        }
#endif
    }

    Entity* camInsideVehicle = nullptr;

    if (_cameraOn != nullptr)
    {
#if _ENABLE_CHEATS
        bool manToggle = input.GetCheat1ToDo(SDL_SCANCODE_SCROLLLOCK);
#endif
        bool manual = false;
        {
            Object* camOn = _cameraOn;
            EntityAI* ai = dyn_cast<EntityAI>(camOn);
            if (ai)
            {
                manual = ai->QIsManual();
#if _ENABLE_CHEATS
                if (manToggle)
                {
                    manual = !manual;
                    if (PlayerOn())
                    {
                        Log("SetManual(%d) %s", manual, (const char*)PlayerOn()->GetDebugName());
                    }
                    if (manual)
                    {
                        Transport* transp = dyn_cast<Transport>(ai);
                        if (transp && transp->Driver())
                        {
                            SwitchPlayerTo(transp->Driver());
                        }
                        else
                        {
                            Person* soldier = dyn_cast<Person>(ai);
                            PoseidonAssert(soldier);
                            SwitchPlayerTo(soldier);
                        }
                    }
                    else
                    {
                        AIUnit* unit = ai->CommanderUnit();
                        if (unit)
                            DoVerify(unit->SetState(AIUnit::Wait));
                    }
                    SetPlayerManual(manual);
                }
                KeyState.SetScrollLock(!manual);
#endif
            }
            else
            {
                CameraHolder* camHolder = dyn_cast<CameraHolder, Object>(_cameraOn);
                if (camHolder)
                {
                    manual = camHolder->GetManual();
#if _ENABLE_CHEATS
                    if (manToggle)
                    {
                        manual = !manual;
                        camHolder->SetManual(manual);
                    }
                    KeyState.SetScrollLock(!manual);
#endif
                }
            }
        }
    }

#if _ENABLE_CHEATS
    if (input.GetCheat2ToDo(SDL_SCANCODE_R))
    {
        _noDisplay = !_noDisplay;
    }
#endif

    bool updateVisibility = true;
#if _ENABLE_CHEATS
    static bool disableVis = false;
    if (input.GetCheat2ToDo(SDL_SCANCODE_Y))
    {
        disableVis = !disableVis;
        Foundation::GlobalShowMessage(500, "VisTests %s", disableVis ? "Off" : "On");
    }
    updateVisibility = !disableVis;
#endif

    // Advance gameplay before deriving this frame's camera and draw lists so
    // presentation always sees the latest completed simulation tick.
    // PHY-010 census (POSEIDON_PHYSICS_CORPUS). Deferred to the first simulated
    // frame rather than run at load: object streaming fills the landscape grid
    // after the world is nominally ready, and censusing too early counts an empty
    // map as a clean pass -- the most flattering possible wrong answer.
    if (Dev::PhysicsCorpusRequested())
    {
        static bool corpusDone = false;
        if (!corpusDone && IsSimulationEnabled())
        {
            corpusDone = true;
            Dev::RunPhysicsCorpus();
        }
    }

    // Presentation side of PHY-020: wind is refreshed and probes are drawn once
    // per FRAME, not per tick -- drawing at the tick rate would blink at any frame
    // rate that is not exactly 60.
    Dev::ReportPhysicsCost();
    Dev::AutoDropPhysicsProbes();
    Dev::UpdatePhysicsProbeWind();

    Dev::FixedStepFrame stepFrame;
    stepFrame.stepSeconds = static_cast<float>(_simulationSteps.StepSeconds());
    // PERF-012: close the setup bucket here so the fixed-step loop below is its own phase.
    // Everything before this point is the input/network/camera work the name promises.
    perf.Mark(Dev::FrameProfiler::PhaseSetup);
#if POSEIDON_DIAG
    // DIAG-001 diag_step: lets exactly one fixed tick through the dev pause this frame (EndStepTick below
    // engages the pause again before anything else in the frame asks).
    const bool diagStepTick = Dev::OpDiag::BeginStepTick();
#endif
    if (IsSimulationEnabled())
    {
        const float fixedDeltaT = static_cast<float>(_simulationSteps.StepSeconds());
        const float fixedNoAccDeltaT = fixedDeltaT / std::max(_acceleratedTime, 0.01f);
#if POSEIDON_DIAG
        const std::size_t steps = diagStepTick ? 1 : _simulationSteps.Advance(deltaT);
#else
        const std::size_t steps = _simulationSteps.Advance(deltaT);
#endif
        for (std::size_t step = 0; step < steps; ++step)
        {
            // REN-INTERP-001: remember where every moving entity is BEFORE this tick, so the
            // draw can blend from the last tick's state to this one. Before EACH tick, so a
            // two-tick frame interpolates between its last two ticks, not across both.
            CaptureRenderPrevFrames();
            StepSimulation(fixedDeltaT, fixedNoAccDeltaT, camVehicle, updateVisibility);
        }
        stepFrame.steps = static_cast<std::uint32_t>(steps);
        stepFrame.running = true;
    }
    else
    {
        _simulationSteps.Reset();
    }
#if POSEIDON_DIAG
    Dev::OpDiag::EndStepTick(diagStepTick);
#endif
    perf.Mark(Dev::FrameProfiler::PhaseSimStep);
    // PERF-013: sim:step is the engine's largest CPU cost (30 ms, 48% of a combat frame).
    // The runner times each stage; this is where the window is closed and reported, once a
    // second, so the split rides in the same log as every other per-frame accounting line.
    // Denominator included: a per-stage mean is meaningless without the tick count, and a
    // tick count of zero is a paused simulation rather than a free one.
    {
        static double sinceReportSec = 0.0;
        sinceReportSec += deltaT;
        if (sinceReportSec >= 1.0)
        {
            sinceReportSec = 0.0;
            Sim::SimStageCostAccum& acc = Sim::SimStageCosts();
            if (acc.ticks > 0)
            {
                const double inv = 1.0 / static_cast<double>(acc.ticks);
                double total = 0.0;
                for (const double v : acc.ms)
                    total += v;
                LOG_INFO(World,
                         "Sim stage ms/tick over {} ticks (total {:.3f}): clock={:.3f} scripts={:.3f} ai={:.3f} "
                         "vehicles={:.3f} visibility={:.3f} physics={:.3f}",
                         acc.ticks, total * inv, acc.ms[0] * inv, acc.ms[1] * inv, acc.ms[2] * inv, acc.ms[3] * inv,
                         acc.ms[4] * inv, acc.ms[5] * inv);
                // How much world time the catch-up cap threw away, against the 60 ticks a
                // real second nominally buys. A tick rate well under 60 with zero discards
                // means the frames were simply long; a nonzero discard count means the
                // simulation is behind and the world is running slow.
                static std::size_t lastDiscarded = 0;
                static std::size_t lastEvents = 0;
                const std::size_t discarded = _simulationSteps.DiscardedSteps() - lastDiscarded;
                const std::size_t events = _simulationSteps.DiscardEvents() - lastEvents;
                lastDiscarded = _simulationSteps.DiscardedSteps();
                lastEvents = _simulationSteps.DiscardEvents();
                LOG_INFO(World,
                         "Sim pacing: {} ticks in the last second of frame time (nominal {:.0f}); catch-up cap "
                         "discarded {} steps in {} frames (cap {})",
                         acc.ticks, 1.0 / _simulationSteps.StepSeconds(), discarded, events,
                         _simulationSteps.MaxCatchUpSteps());
            }
            Sim::ResetSimStageCosts();

            // PERF-014: the same window, one level down. Printed here and not in its own
            // timer so the two lines share a denominator by construction -- `aiTicks`
            // below and the `over N ticks` above are the same window, and a discrepancy
            // between them means the AI stage was skipped on some ticks rather than that
            // one of the two clocks drifted.
            AICost::CostAccum& ai = AICost::AICosts();
            if (ai.ticks > 0)
            {
                const double aiInv = 1.0 / static_cast<double>(ai.ticks);
                LOG_INFO(World,
                         "AI ms/tick over {} aiTicks: radio={:.3f} exposure={:.3f} map={:.3f} guarding={:.3f} "
                         "support={:.3f} groupScan={:.3f} [groupThink={:.3f} [groupExpensive={:.3f} "
                         "subThink={:.3f} [unitThink={:.3f}]]] endMission={:.3f}",
                         ai.ticks, ai.ms[0] * aiInv, ai.ms[1] * aiInv, ai.ms[2] * aiInv, ai.ms[3] * aiInv,
                         ai.ms[4] * aiInv, ai.ms[5] * aiInv, ai.ms[6] * aiInv, ai.ms[7] * aiInv, ai.ms[8] * aiInv,
                         ai.ms[9] * aiInv, ai.ms[10] * aiInv);
                LOG_INFO(World,
                         "AI counts/tick over {} aiTicks: centers={:.2f} radioGroups={:.2f} scanIters={:.2f} "
                         "groupsPerScan={:.2f} groupThinks={:.2f} groupExpensives={:.2f} subThinks={:.2f} "
                         "unitThinks={:.2f}",
                         ai.ticks, ai.centers * aiInv, ai.radioGroups * aiInv, ai.scanIters * aiInv,
                         ai.scanIters > 0 ? static_cast<double>(ai.groupsSeen) / static_cast<double>(ai.scanIters)
                                          : 0.0,
                         ai.groupThinks * aiInv, ai.groupExpensives * aiInv, ai.subThinks * aiInv,
                         ai.unitThinks * aiInv);

                // PERF-015: the same window, one level further down -- inside
                // `AIGroup::Think`'s own body, which PERF-014 measured at 79% of the AI
                // stage and could not subdivide. `residual` is printed rather than left to
                // be worked out, because it IS the check that the six segments enclose the
                // body: a residual that is a large share of `full` means they do not, and
                // nothing else on the line should be believed.
                //
                // Named lookups rather than the literal indices the two lines above use:
                // these buckets were appended to the enum, and a literal index here would
                // silently point at the wrong bucket the first time somebody inserts one in
                // the middle.
                const auto aiMs = [&ai](AICost::Bucket b) { return ai.ms[static_cast<std::size_t>(b)]; };
                const double bodyThink = aiMs(AICost::Bucket::GroupThink);
                const double bodyEarly = aiMs(AICost::Bucket::GroupThinkEarly);
                const double bodyFull = aiMs(AICost::Bucket::GroupThinkFull);
                const double bodyFlee = aiMs(AICost::Bucket::GroupFlee);
                const double bodyFsm = aiMs(AICost::Bucket::GroupFsm);
                const double bodyJoin = aiMs(AICost::Bucket::GroupJoin);
                const double bodyTrack = aiMs(AICost::Bucket::GroupTrack);
                const double bodyExp = aiMs(AICost::Bucket::GroupExpensive);
                const double bodySub = aiMs(AICost::Bucket::SubThink);
                const double residual = bodyFull - (bodyFlee + bodyFsm + bodyJoin + bodyTrack + bodyExp + bodySub);
                LOG_INFO(World,
                         "AI group body ms/tick over {} aiTicks: groupThink={:.3f} [early={:.3f} full={:.3f} "
                         "[flee={:.3f} fsm={:.3f} join={:.3f} track={:.3f} expensive={:.3f} subThink={:.3f} "
                         "residual={:.3f}]]",
                         ai.ticks, bodyThink * aiInv, bodyEarly * aiInv, bodyFull * aiInv, bodyFlee * aiInv,
                         bodyFsm * aiInv, bodyJoin * aiInv, bodyTrack * aiInv, bodyExp * aiInv, bodySub * aiInv,
                         residual * aiInv);
                // The five exit counts sum to `groupThinks` by construction; printing all
                // of them is how that stays checkable rather than asserted. `trackCalls /
                // trackScans` is the mean engaged-unit count per tracking group, which is
                // the multiplier on the one unrationed walk in the body.
                LOG_INFO(World,
                         "AI group body counts/tick over {} aiTicks: groupThinks={:.2f} [noLeader={:.2f} "
                         "logic={:.2f} remote={:.2f} destroyed={:.2f} full={:.2f} (ofWhichTrue={:.2f})] "
                         "trackScans={:.2f} trackCalls={:.2f} trackPairs={:.2f} targetsPerCall={:.2f}",
                         ai.ticks, ai.groupThinks * aiInv, ai.groupThinksNoLeader * aiInv,
                         ai.groupThinksLogic * aiInv, ai.groupThinksRemote * aiInv, ai.groupThinksDestroyed * aiInv,
                         ai.groupThinksFull * aiInv, ai.groupThinksTrue * aiInv, ai.trackScans * aiInv,
                         ai.trackCalls * aiInv, ai.trackPairs * aiInv,
                         ai.trackCalls > 0 ? static_cast<double>(ai.trackPairs) / static_cast<double>(ai.trackCalls)
                                           : 0.0);

                // PERF-019: the same window, inside `AIUnit::Think`. `residual` is again
                // the printed check: unitThinkFull minus the seven disjoint segments. And
                // `early + full` should equal the `unitThink` on the first line to within
                // one extra clock pair per call, because that pair is inside the function
                // the PERF-014 pair wraps.
                const double uEarly = aiMs(AICost::Bucket::UnitThinkEarly);
                const double uFull = aiMs(AICost::Bucket::UnitThinkFull);
                const double uAttack = aiMs(AICost::Bucket::UnitAttack);
                const double uWatch = aiMs(AICost::Bucket::UnitWatch);
                const double uExpensive = aiMs(AICost::Bucket::UnitExpensive);
                const double uGetInOut = aiMs(AICost::Bucket::UnitGetInOut);
                const double uStrat = aiMs(AICost::Bucket::UnitStrat);
                const double uOperTarget = aiMs(AICost::Bucket::UnitOperTarget);
                const double uOperPlan = aiMs(AICost::Bucket::UnitOperPlan);
                const double uResidual =
                    uFull - (uAttack + uWatch + uExpensive + uGetInOut + uStrat + uOperTarget + uOperPlan);
                LOG_INFO(World,
                         "AI unit think ms/tick over {} aiTicks: unitThink={:.3f} [early={:.3f} full={:.3f} "
                         "[attack={:.3f} watch={:.3f} expensive={:.3f} getInOut={:.3f} strat={:.3f} "
                         "operTarget={:.3f} operPlan={:.3f} residual={:.3f}]]",
                         ai.ticks, aiMs(AICost::Bucket::UnitThink) * aiInv, uEarly * aiInv, uFull * aiInv,
                         uAttack * aiInv, uWatch * aiInv, uExpensive * aiInv, uGetInOut * aiInv, uStrat * aiInv,
                         uOperTarget * aiInv, uOperPlan * aiInv, uResidual * aiInv);
                // `early + full == unitThinks` by construction; `pilot` of the full thinks
                // reached the planning switch. Parenthesised counts are subsets of the one
                // they follow.
                LOG_INFO(World,
                         "AI unit think counts/tick over {} aiTicks: unitThinks={:.2f} [early={:.2f} full={:.2f} "
                         "(pilot={:.2f})] attackThinks={:.2f} watchSelects={:.2f} (deferred={:.2f}) "
                         "watchPairs={:.2f} expensives={:.2f} getInOuts={:.2f} strats={:.2f} (search={:.2f}) "
                         "operTargets={:.2f} operPlans={:.2f} (init={:.2f})",
                         ai.ticks, ai.unitThinks * aiInv, ai.unitThinksEarly * aiInv, ai.unitThinksFull * aiInv,
                         ai.unitThinksPilot * aiInv, ai.unitAttackThinks * aiInv, ai.unitWatchSelects * aiInv,
                         ai.unitWatchDeferred * aiInv, ai.unitWatchPairs * aiInv, ai.unitExpensives * aiInv,
                         ai.unitGetInOuts * aiInv,
                         ai.unitStrats * aiInv, ai.unitStratSearches * aiInv, ai.unitOperTargets * aiInv,
                         ai.unitOperPlans * aiInv, ai.unitOperInits * aiInv);
            }
            AICost::ResetAICosts();
            // PERF-023: the Vehicles stage, by sub-stage and by entity kind (see SimVehicleCost.hpp).
            SimVehCost::Accum& veh = SimVehCost::Costs();
            if (veh.ticks > 0)
            {
                const double vInv = 1.0 / static_cast<double>(veh.ticks);
                const auto st = [&veh](SimVehCost::Stage x) { return veh.stageMs[static_cast<std::size_t>(x)]; };
                LOG_INFO(World,
                         "Sim vehicles ms/tick over {} ticks: importance={:.3f} cloudlets={:.3f} slow={:.3f} "
                         "fast={:.3f} buildings={:.3f} attached={:.3f} | slow.moveout={:.3f} "
                         "slow.vehicles={:.3f} slow.animals={:.3f} | man.pilot={:.3f} man.base={:.3f}",
                         veh.ticks, st(SimVehCost::Stage::Importance) * vInv, st(SimVehCost::Stage::Cloudlets) * vInv,
                         st(SimVehCost::Stage::Slow) * vInv, st(SimVehCost::Stage::Fast) * vInv,
                         st(SimVehCost::Stage::Buildings) * vInv, st(SimVehCost::Stage::Attached) * vInv,
                         st(SimVehCost::Stage::SlowMoveOut) * vInv, st(SimVehCost::Stage::SlowVehicles) * vInv,
                         st(SimVehCost::Stage::SlowAnimals) * vInv, st(SimVehCost::Stage::ManPilot) * vInv,
                         st(SimVehCost::Stage::ManBase) * vInv);
                if (SimVehCost::Enabled())
                {
                    for (const auto stage : {SimVehCost::Stage::CloudletSoftSurfaces,
                            SimVehCost::Stage::CloudletRunoffSource, SimVehCost::Stage::CloudletRunoffAdvance,
                            SimVehCost::Stage::CloudletRunoffNatural, SimVehCost::Stage::CloudletRemainingEffects})
                    {
                        const auto index = static_cast<std::size_t>(stage);
                        LOG_INFO(World, "Sim cloudlet cost over {} ticks: stage={} calls={} total_ms={:.6f} "
                                        "ms_per_tick={:.6f} max_call_ms={:.6f} scope=nested-cloudlets",
                                 veh.ticks, SimVehCost::StageName(stage), veh.substageCalls[index],
                                 veh.stageMs[index], veh.stageMs[index] * vInv, veh.substageMaxMs[index]);
                    }
                }
                const auto km = [&veh](SimVehCost::Kind k) { return veh.kindMs[static_cast<std::size_t>(k)]; };
                const auto kc = [&veh](SimVehCost::Kind k) { return veh.kindCalls[static_cast<std::size_t>(k)]; };
                LOG_INFO(World,
                         "Sim vehicles by kind ms/tick (calls/tick): man={:.3f} ({:.1f}) tank={:.3f} ({:.1f}) "
                         "car={:.3f} ({:.1f}) air={:.3f} ({:.1f}) ship={:.3f} ({:.1f}) static={:.3f} ({:.1f}) "
                         "other={:.3f} ({:.1f})",
                         km(SimVehCost::Kind::Man) * vInv, kc(SimVehCost::Kind::Man) * vInv,
                         km(SimVehCost::Kind::Tank) * vInv, kc(SimVehCost::Kind::Tank) * vInv,
                         km(SimVehCost::Kind::Car) * vInv, kc(SimVehCost::Kind::Car) * vInv,
                         km(SimVehCost::Kind::Air) * vInv, kc(SimVehCost::Kind::Air) * vInv,
                         km(SimVehCost::Kind::Ship) * vInv, kc(SimVehCost::Kind::Ship) * vInv,
                         km(SimVehCost::Kind::Static) * vInv, kc(SimVehCost::Kind::Static) * vInv,
                         km(SimVehCost::Kind::Other) * vInv, kc(SimVehCost::Kind::Other) * vInv);
            }
            SimVehCost::Reset();
            if (RainWaterCost::Enabled())
            {
                const auto& water=RainWaterCost::Costs();
                if (water.advances)
                {
                    LOG_INFO(World, "Rain water coarse cost: advances={} steps={} empty_steps={} no_step={} multi_step={} "
                                    "wall_total_ms={:.6f} wall_max_ms={:.6f} cpu_samples={} cpu_total_ms={:.6f} "
                                    "cpu_at_wall_max_ms={:.6f} steps_at_wall_max={} slow_50ms={} phase_count={} scope=coarse-advance-thread-elapsed",
                             water.advances,water.steps,water.emptySteps,water.noStepAdvances,water.multiStepAdvances,
                             water.wallMs,water.maxWallMs,water.cpuSamples,water.cpuMs,
                             water.cpuAtMaxWallMs,water.stepsAtMaxWall,water.slowAdvances,RainWaterCost::PhaseCount);
                    for (size_t i=0;i<RainWaterCost::PhaseCount;++i)
                        LOG_INFO(World, "Rain water coarse phase: phase={} calls={} total_ms={:.6f} max_ms={:.6f} scope=nested-coarse-step",
                                 RainWaterCost::Name(static_cast<RainWaterCost::Phase>(i)),water.phaseCalls[i],
                                 water.phaseMs[i],water.phaseMaxMs[i]);
                }
                RainWaterCost::Reset();
            }
        }
    }
    // Read AFTER the loop: Alpha() is the remainder the ticks did not consume, so
    // sampling it earlier would report the previous frame's leftover.
    stepFrame.alpha = static_cast<float>(_simulationSteps.Alpha());
    {
        // Cumulative, so a published frame can name the simulation state it belongs to --
        // the only alignment key that survives the frame hash moving to the render thread.
        static std::uint64_t totalTicks = 0;
        totalTicks += stepFrame.steps;
        stepFrame.totalTicks = totalTicks;
    }
    Dev::PublishFixedStepFrame(stepFrame);
    // REN-INTERP-001: a paused world draws its current state (alpha 1); a running one draws
    // the accumulator's leftover fraction into the last tick. Set once per frame, here,
    // so every draw of this frame -- objects, shadows, camera -- agrees on the same time.
    Object::SetRenderInterpAlpha(stepFrame.running ? stepFrame.alpha : 1.0f);

    if (IsSimulationEnabled())
    {
        if (_titleEffect && _titleEffect->IsTerminated())
        {
            _titleEffect.Free();
            Log("_titleEffect.Free()");
        }

        if (_cutEffect && _cutEffect->IsTerminated())
        {
            _cutEffect.Free();
        }

        if (_titleEffect)
        {
            _titleEffect->Simulate(deltaT);
        }
        if (_cutEffect)
        {
            _cutEffect->Simulate(deltaT);
        }
    }

    if (camVehicle)
    {
        if (IsSimulationEnabled())
        {
            FFEffects eff;
            camVehicle->PerformFF(eff);
            SDLGamepad_SetEngine(eff.engineMag);
        }
        else
        {
            SDLGamepad_SetEngine(0);
        }
    }

    {
        // Frame-driven scripts are the DEFAULT and the safe path. The opt-in moves
        // them into the fixed tick, but only while the simulation is actually
        // running: this call has always been outside IsSimulationEnabled() on
        // purpose, because camera scripts must keep running through cutscenes and
        // pauses, where that check is false. So when the simulation is off, the
        // once-per-frame call stays no matter what the switch says.
        if (!(Dev::FixedStepScriptsEnabled() && IsSimulationEnabled()))
        {
            SimulateScripts();
        }

        if (_cameraEffect && _cameraEffect->IsTerminated())
        {
            _cameraEffect.Free();
            _playerSuspended = false;
        }

        // -------------------------------------------------------------------
        // Free-fly stays live while the simulation is frozen.
        //
        // The free camera is a CameraVehicle entity, and entities are stepped by
        // SimulateAllVehicles, which IsSimulationEnabled() gates — so a paused
        // world freezes the camera along with everything else, which is exactly
        // the thing that makes a paused world useless for inspection.  Step the
        // manual camera holder here, on its own, before its transform is read
        // below.  Only manual holders: a scripted camEffect or a player-attached
        // view must stay frozen with the thing it is attached to.
        //
        // CameraVehicle::Simulate divides its delta by the time-acceleration
        // factor so manual flight stays real-time under time compression
        // (CameraHold.cpp:336); multiply it back out here so the camera moves at
        // real wall-clock speed while paused.
        if (!IsSimulationEnabled())
        {
            CameraHolder* freeCam = dyn_cast<CameraHolder, Object>(_cameraOn);
            if (freeCam && freeCam->GetManual())
            {
                const float accel = _acceleratedTime > 0.01f ? _acceleratedTime : 1.0f;
                freeCam->Simulate(noAccDeltaT * accel, SimulateVisibleNear);
            }
        }

        Camera& camera = *_scene.GetCamera();
        float fov = 0.7;
        Matrix4 transform = camera.Transform();
        float cameraRotate = 0;
        // TW-WATER W5c: a camera the mission scripts or flies by hand (camCreate + cameraEffect,
        // the free camera) may go under the sea for cut-scenes; see the clamp below.
        bool mayDive = false;
        if (const CameraHolder* holder = dyn_cast<CameraHolder, Object>(_cameraOn))
        {
            mayDive = holder->GetManual();
        }

        if (_cameraEffect)
        {
            mayDive = mayDive || dyn_cast<CameraHolder, Object>(_cameraEffect->GetObject()) != nullptr;
            _cameraEffect->Simulate(deltaT);
            fov = _cameraEffect->GetFOV();
            if (fov < 0)
            { // default fov
                Object* object = _cameraEffect->GetObject();
                fov = object ? object->CamEffectFOV() : 0.7f;
            }
            transform = _cameraEffect->GetTransform();

            if (_cameraEffect->IsInside())
            {
                camInsideVehicle = dyn_cast<Entity, Object>(_cameraEffect->GetObject());
            }
        }
        else if (cameraVehicle)
        {
            fov = _camFOV[_camType];
            // REN-INTERP-001: the camera rides the vehicle's DRAWN frame, never a different
            // time than the body it is attached to (the switch below re-derives every case
            // from RenderWorldTransform; this is the base for the paths that skip it).
            transform = cameraVehicle->RenderWorldTransform();
            // if any vehicle control key is pressed, set to manual control
            // check vehicle control keys

            if (!HasOptions())
            {
                cameraVehicle->SimulateHUD(_camType, deltaT);
                bool isVirtual = cameraVehicle->IsVirtual(_camType);
                //// isGunner was: cameraVehicle->IsGunner(_camType);
                bool isGunner = cameraVehicle->IsGunner(_camType);
                if (isGunner || isVirtual)
                { // gunner camera
                    // if( !isGunner || camAI->GetType()->IsKindOf(GWorld->Preloaded(VTypeMan)) )
                    if (_ui && !_showMap)
                    {
                        _ui->SetCursorMode(cameraVehicle->GetCursorRelMode(_camType) == CMouseAbs);
                        Vector3 cursorDir = _ui->GetCursorDirection();
                        float scale = _camFOV[_camType];
                        float moveX = input.ConsumeCursorDeltaX() * scale;
                        float moveY = -input.ConsumeCursorDeltaY() * scale;
                        const float maxRotX = H_PI / 2;
                        const float maxRotY = H_PI / 2;
                        saturate(moveX, -maxRotX, +maxRotX);
                        saturate(moveY, -maxRotY, +maxRotY);
                        Vector3 rot(moveX, moveY, 0);
                        // user moves cursor in camera space
                        // we need to convert it into world space
                        Vector3 curCam = GScene->GetCamera()->Transform().Rotate(rot);

                        // Matrix3 cursorOrient(MDirection,cursorDir,VUp);
                        // cursorDir=cursorOrient*rot;
                        cursorDir += curCam;
                        cursorDir.Normalize();

                        cameraVehicle->LimitCursorHard(_camType, cursorDir);
                        cursorDir.Normalize();

                        _ui->SetCursorDirection(cursorDir);
                    }
                }

                if (cameraVehicle->IsVirtual(_camType))
                {
                    const float diag = 0.5 * H_SQRT2;
                    float headSpeed = (input.GetAction(UALookLeft) + input.GetAction(UALookLeftUp) * diag +
                                       input.GetAction(UALookLeftDown) * diag - input.GetAction(UALookRight) -
                                       input.GetAction(UALookRightUp) * diag - input.GetAction(UALookRightDown) * diag);
                    float diveSpeed = (input.GetAction(UALookDown) + input.GetAction(UALookLeftDown) * diag +
                                       input.GetAction(UALookRightDown) * diag - input.GetAction(UALookUp) -
                                       input.GetAction(UALookRightUp) * diag - input.GetAction(UALookLeftUp) * diag);
                    float headChange = noAccDeltaT * headSpeed;
                    float diveChange = noAccDeltaT * diveSpeed;
                    if (!cameraVehicle->IsContinuous(_camType) && !cameraVehicle->IsExternal(_camType) &&
                        (cameraVehicle->GetCursorRelMode(_camType) == CKeyboard || input.IsJoystickActive()))
                    {
                        headChange = 0;
                        // discrete movement
                        // float dummy=0;
                        float initDive, initHead, initFOV;
                        cameraVehicle->InitVirtual(_camType, initHead, initDive, initFOV);
                        if (input.GetAction(UALookLeftUp))
                        {
                            _camHeadingWanted[_camType] = initHead + H_PI / 4;
                            diveChange = 0;
                        }
                        else if (input.GetAction(UALookLeft))
                        {
                            _camHeadingWanted[_camType] = initHead + H_PI / 2;
                            diveChange = 0;
                        }
                        else if (input.GetAction(UALookLeftDown))
                        {
                            _camHeadingWanted[_camType] = initHead + H_PI * 0.99;
                            diveChange = 0;
                        }
                        else if (input.GetAction(UALookRightUp))
                        {
                            _camHeadingWanted[_camType] = initHead - H_PI / 4;
                            diveChange = 0;
                        }
                        else if (input.GetAction(UALookRight))
                        {
                            _camHeadingWanted[_camType] = initHead - H_PI / 2;
                            diveChange = 0;
                        }
                        else if (input.GetAction(UALookRightDown))
                        {
                            _camHeadingWanted[_camType] = initHead - H_PI * 0.99;
                            diveChange = 0;
                        }
                        else
                        {
                            _camHeadingWanted[_camType] = initHead;
                        }
                    }
                    if (input.GetActionToDo(UALookCenter))
                    {
                        cameraVehicle->InitVirtual(_camType, _camHeadingWanted[_camType], _camDiveWanted[_camType],
                                                   _camFOVWanted[_camType]);
                        _camNear[_camType] = 1.0;
                        _camMaxDist[_camType] = 1e10;
                        headChange = 0;
                        diveChange = 0;

                        if (_ui)
                        {
                            // cursor must be set to the screen center
                            _ui->SetCursorDirection(camera.Direction());
                            _ui->SetCursorMode(false);
                        }
                    }
                    // if mouse cursor is on screen edge, rotate view
                    if (headChange || diveChange)
                    {
                        // keep ui cursor in screen range
                        if (_ui)
                        {
                            cameraRotate = fabs(headSpeed) + fabs(diveSpeed);
                            _camHeading[_camType] += headChange;
                            _camDive[_camType] += diveChange;
                        }
                        _camHeadingWanted[_camType] = _camHeading[_camType];
                        _camDiveWanted[_camType] = _camDive[_camType];
                    }
                    if (_ui && !_showMap)
                    {
                        bool cursorMode = _ui->GetCursorMode();
                        _ui->SetCursorMode(false);
                        if (cursorMode && !ENGINE_CONFIG.landEditor)
                        {
                            // rotate camera so that cursor stays in the neutral zone
                            Vector3 curDir = _ui->GetCursorDirection();
                            cameraVehicle->LimitCursor(GetCameraType(), curDir);

                            Matrix4Val camInvTransform = camera.GetInvTransform();

                            Vector3 pos = camInvTransform.Rotate(curDir);
                            float cursorX = 0, cursorY = 0;
                            if (pos.Z() > 0)
                            {
                                float invZ = 1.0 / pos.Z();

                                cursorX = pos.X() * invZ * camera.InvLeft();
                                cursorY = -pos.Y() * invZ * camera.InvTop();

                                saturate(cursorX, -0.95, +0.95);
                                saturate(cursorY, -0.95, +0.95);
                            }

                            if (cameraVehicle->IsVirtualX(_camType))
                            {
                                KeepNZone(_camHeading[_camType], cursorX, 0, 0.8, camera.Left());
                                _camHeadingWanted[_camType] = _camHeading[_camType];
                            }
                            else
                            {
                                float initDive, initHead, initFOV;
                                cameraVehicle->InitVirtual(_camType, initHead, initDive, initFOV);
                                _camHeadingWanted[_camType] = initHead;
                            }
                            KeepNZone(_camDive[_camType], -cursorY, 0, 0.5, camera.Top());
                            _camDiveWanted[_camType] = _camDive[_camType];
                        }
                        Vector3 cursor = _ui->GetModelCursor();

                        saturateMax(cursor[2], 0.01);
                        cursor *= 1 / cursor[2];
                        float xLimit = camera.Left();
                        float yLimit = camera.Top();
                        if (!cursorMode)
                        {
                            xLimit *= 0.8, yLimit *= 0.8;
                        }
                        saturate(cursor[0], -xLimit, +xLimit);
                        saturate(cursor[1], -yLimit, +yLimit);
                        cursor.Normalize();

                        _ui->SetModelCursor(cursor);
                        // return cursor mode
                        _ui->SetCursorMode(cursorMode);
                    }
                    cameraVehicle->LimitVirtual(_camType, _camHeading[_camType], _camDive[_camType], _camFOV[_camType]);
                    cameraVehicle->LimitVirtual(_camType, _camHeadingWanted[_camType], _camDiveWanted[_camType],
                                                _camFOVWanted[_camType]);
                }
            }
            if (cameraVehicle->IsExternal(_camType))
            {
                float expChange = input.GetAction(UAZoomOut) - input.GetAction(UAZoomIn);
                if (expChange)
                {
                    float change = pow(ZoomSpeed, expChange * noAccDeltaT);
                    _camNear[_camType] *= change;
                    if (!ENGINE_CONFIG.landEditor)
                    {
                        saturate(_camNear[_camType], 0.25, 4);
                    }
                    else
                    {
                        saturate(_camNear[_camType], 0.01, 100);
                    }
                }
            }
            else
            {
                if (cameraVehicle->IsContinuous(_camType))
                {
                    float expChange = input.GetAction(UAZoomOut) - input.GetAction(UAZoomIn);
                    if (expChange)
                    {
                        float change = pow(ZoomSpeed, expChange * noAccDeltaT);
                        _camFOV[_camType] *= change;
                        _camFOVWanted[_camType] = _camFOV[_camType];
                        cameraVehicle->LimitVirtual(_camType, _camHeading[_camType], _camDive[_camType],
                                                    _camFOV[_camType]);
                        cameraVehicle->LimitVirtual(_camType, _camHeadingWanted[_camType], _camDiveWanted[_camType],
                                                    _camFOVWanted[_camType]);
                    }
                }
                else
                {
                    float initHead, initDive, initFOV;
                    cameraVehicle->InitVirtual(_camType, initHead, initDive, initFOV);
                    if (input.GetAction(UAZoomIn) || forcedZoom)
                    {
                        initFOV *= 0.25;
                    }
                    else if (input.GetAction(UAZoomOut))
                    {
                        initFOV *= 4;
                    }
                    cameraVehicle->LimitVirtual(_camType, initHead, initDive, initFOV);
                    _camFOVWanted[_camType] = initFOV;
                }
            }

            // if it is, you can use AngleDifference instead of operator -
            float delta;
            delta = _camHeadingWanted[_camType] - _camHeading[_camType];
            saturate(delta, -4 * deltaT, +4 * deltaT);
            _camHeading[_camType] += delta;

            delta = _camDiveWanted[_camType] - _camDive[_camType];
            saturate(delta, -2 * deltaT, +2 * deltaT);
            _camDive[_camType] += delta;

            delta = _camFOVWanted[_camType] / _camFOV[_camType];

            float changeMax = pow(ZoomSpeed, noAccDeltaT);
            saturate(delta, 1 / changeMax, changeMax);
            _camFOV[_camType] *= delta;

            _camMaxDist[_camType] += deltaT;
            saturateMin(_camMaxDist[_camType], 1e10);

            {
                Matrix3 camChange = CameraChange(_camHeading[_camType], _camDive[_camType]);
                // REN-INTERP-001: EVERY branch below derives the camera from the vehicle's DRAWN
                // frame. The first version set `transform` once above and every case here
                // overwrote it with the tick-time WorldTransform(), so the normal game camera
                // was never smoothed (review of 683b8f9f). renderDelta moves tick-time points
                // and directions (CameraPosition, GetCameraDirection) onto the drawn body.
                const Matrix4 renderWorld = cameraVehicle->RenderWorldTransform();
                const Matrix4 renderDelta = cameraVehicle->RenderDelta();
                switch (_camType)
                {
                    case CamGunner:
                    {
                        transform = renderWorld * cameraVehicle->InsideCamera(_camType);
                        camInsideVehicle = dyn_cast<Entity, Object>(cameraVehicle);
                    }
                    break;
                    default: // case CamInternal:
                    {
                        transform = renderWorld * cameraVehicle->InsideCamera(_camType);
                        camInsideVehicle = dyn_cast<Entity, Object>(cameraVehicle);
                        transform.SetOrientation(transform.Orientation() * camChange);
                    }
                    break;
                    case CamExternal:
                    {
                        Matrix3 vehOrient;
                        Vector3Val dist = cameraVehicle->ExternalCameraPosition(_camType);
                        Vector3 dir = renderDelta.Rotate(cameraVehicle->GetCameraDirection(_camType));
                        vehOrient.SetUpAndDirection(VUp, dir);
                        vehOrient = vehOrient * camChange;
                        transform.SetOrientation(vehOrient);
                        Vector3 focPos = renderDelta.FastTransform(cameraVehicle->CameraPosition());
                        Vector3 camPos = vehOrient * dist + focPos;
                        ClipCamera(camPos, cameraVehicle, focPos, _camMaxDist[_camType]);
                        transform.SetPosition(camPos);
                        camChange = M3Identity;
                        camInsideVehicle = nullptr;
                    }
                    break;
                    case CamGroup:
                    {
                        Matrix3 vehOrient;
                        vehOrient.SetUpAndDirection(VUp, renderDelta.Rotate(_cameraOn->Direction()));
                        float dist = cameraVehicle->OutsideCameraDistance(_camType) * _camNear[_camType];
                        Matrix3Val orient = ENGINE_CONFIG.landEditor ? camChange : vehOrient * camChange;
                        transform.SetOrientation(orient);
                        Vector3 focPos = renderDelta.FastTransform(cameraVehicle->CameraPosition());
                        Vector3 camPos = focPos - orient.Direction() * dist;
                        transform.SetPosition(camPos);
                        camChange = M3Identity;
                        camInsideVehicle = nullptr;
                    }
                    break;
                }
            }
        }
        {
            // Every view stays above the ground. Player, vehicle and trigger-effect views also stay
            // above the sea; a scripted or free camera (mayDive, TW-WATER W5c) may go under it -- the
            // renderer's underwater compositor takes over there -- and is held only above the sea bed.
            //
            // Sinkhole W1 (Malprave terrainHole4): inside a terrain hole the floor is the hole's floor, and a
            // 3rd person camera that swung into the earth around a cellar or cave mouth is first pulled back
            // into the hole, toward what it follows (the camera effect's object, else the camera vehicle).
            Vector3 camPos = transform.Position();
            Object* camFocusObj = _cameraEffect ? _cameraEffect->GetObject() : cameraVehicle.GetLink();
            Vector3 camFocus;
            if (camFocusObj)
            {
                camFocus = camFocusObj->CameraPosition();
            }
            // Sinkhole W3: on a swimmer the floor is the waves, not the flat mean sea -- in first person none
            // at all (the swim keeps his eyes over the water; a trough lifted the view into his head), in 3rd
            // person the wave surface under the camera, so it rides over the crests.
            float swimWaterY = 0;
            const bool swimView =
                !mayDive && SwimmerCameraWater(cameraVehicle.GetLink(), camPos.X(), camPos.Z(), swimWaterY);
            float minCamY = _scene.GetLandscape()->CameraFloorY(camPos, camFocusObj ? &camFocus : nullptr,
                                                                !mayDive && !swimView);
            if (swimView && _camType != CamInternal)
            {
                minCamY = std::max(minCamY, swimWaterY + 0.15f);
            }
            minCamY += 0.1;
            if (camPos.Y() < minCamY)
            {
                camPos[1] = minCamY;
            }
            transform.SetPosition(camPos);
        }
        if (s_triViewActive)
        {
            transform = s_triViewTransform;
        }
        // WTR-004 — standard water test harness. Runs after every other camera source (player,
        // camera effect, triView) so an active harness preset owns the view: deterministic camera
        // path plus edge-triggered interaction events, both driven from here (per frame, before
        // BeginObjects) rather than from the overlay. A paused harness returns false and leaves
        // the camera alone, so Pause hands control back to the player camera.
        {
            auto& wtrHarness = WtrTestHarness::Instance();
            if (wtrHarness.IsActive())
            {
                auto wtrSettings = GEngine->GetWaterSettings();
                Vector3 wtrCamPos(VZero), wtrCamRot(VZero);
                if (wtrHarness.Update(deltaT, wtrSettings, wtrCamPos, wtrCamRot))
                {
                    // camRot is (pitch, yaw, roll) in degrees; CameraChange takes radians.
                    Matrix3 wtrOrient =
                        Matrix3(MRotationY, HDegree(wtrCamRot.Y())) * Matrix3(MRotationX, HDegree(wtrCamRot.X()));
                    wtrOrient = wtrOrient * Matrix3(MRotationZ, HDegree(wtrCamRot.Z()));
                    transform.SetOrientation(wtrOrient);
                    transform.SetPosition(wtrCamPos);
                }
            }
        }
        camera.SetTransform(transform);

        if (cameraVehicle)
        {
            camera.SetSpeed(cameraVehicle->ObjectSpeed());
        }
        else
        {
            camera.SetSpeed(VZero);
        }
        if (camVehicle)
        {
            float visualSpeed = camVehicle->Speed().SquareSize() * (1.0 / 900.0);
            float visualRotate = camVehicle->AngVelocity().SquareSize() * (2.5);
            saturateMax(visualSpeed, visualRotate + cameraRotate);
            Glob.dropDown = visualSpeed;
            saturate(Glob.dropDown, 0, 1);
        }
        Glob.fullDropDown += Glob.fullDropDownChange;
        Glob.fullDropDown -= noAccDeltaT * 0.33;
        saturate(Glob.fullDropDown, 0, 1);
        Glob.fullDropDownChange = 0;

        // normal soldier fov is about 0.85
        float cNear = 0.067f / fov;
        saturate(cNear, 0.07f, 0.2f);
        // FAR-001: the far plane and the fog range were literally this one
        // expression -- `GetFogMaxRange()` was handed straight in as `cFar`, which
        // is why an aircraft above ~900 m flew over a world that had stopped being
        // drawn (see World/Scene/Camera/AerialRange.hpp for the measurements).
        // They are resolved separately now. `ApplyAerialRange` returns the far
        // plane and, as a side effect, pushes the matching haze range into the
        // scene; below the policy's start altitude it returns `GetFogMaxRange()`
        // itself and touches nothing, so ordinary ground play is unchanged.
        const float cFar = ApplyAerialRange(_scene, camera.Position());
        camera.SetPerspectiveForView(GEngine, cNear, cFar, fov);
        camera.Adjust(GEngine);
    }

#if BACKGROUND_AI
    SecondaryContext context;
    context.deltaT = deltaT;
    context.noAccDeltaT = noAccDeltaT;
    context.cameraVehicle = camInsideVehicle;
    context.insideVehicle = camInsideVehicle != nullptr;
    context.world = this;
#endif

    bool clear = true;

    if (_warningMessage)
    {
        _warningMessage->OnSimulate(nullptr);
        if (_warningMessage->GetExitCode() >= 0)
        {
            _warningMessage = nullptr;
        }
    }
    else
    {
        if (_voiceChat)
        {
            _voiceChat->SimulateHUD(nullptr);
        }
        if (!HasOptions() && _cameraOn != nullptr)
        {
            if (_userDlg)
            {
                _userDlg->SimulateHUD(nullptr);
            }
            else if (_map && _showMap)
            {
                _map->SimulateHUD(camAI);
            }
        }
        if (_options)
        {
            _options->SimulateHUD(nullptr);
        }
        if (!HasOptions() && _cameraOn != nullptr && !_userDlg)
        {
            if (_ui && IsUIEnabled())
            {
                if (camAI)
                {
                    _ui->SimulateHUD(*_scene.GetCamera(), camAI, _camType, deltaT);
                }
                else if (camVehicle)
                {
                    _ui->SimulateHUDNonAI(*_scene.GetCamera(), camVehicle, _camType, deltaT);
                }
            }
        }
    }
    input.ConsumeCursorScroll();

    bool quiet = false;
    // bool isSimEnabled = IsSimulationEnabled();
    // bool simEnabledEdge = isSimEnabled && !wasSimEnabled;

    if (_firstFrame /*|| simEnabledEdge*/)
    {
        // LOG_DEBUG(World, "_firstFrame {}, simEnabledEdge {}",_firstFrame,simEnabledEdge);
        enableDraw = false, _firstFrame = false, quiet = true;

        // Static landscape objects (street lamps) evaluated their on/off state
        // against the world's default daytime clock during InitLandscape, before
        // the mission start time was applied.  Re-broadcast a time-skip on the
        // first simulated frame so they sync to the actual mission clock — auto
        // lamps would otherwise stay dark until they happened to re-simulate
        // (which is why they only appeared after a mission retry).
        if (GLandscape)
        {
            GLandscape->OnTimeSkipped();
        }
    }
    if (enableDraw)
    {
        enableDraw = GEngine->IsAbleToDraw();
    }
    perf.Mark(Dev::FrameProfiler::PhaseSetup);
    static const bool slowFrameTrace = [] {
        const char* value = std::getenv("POSEIDON_SLOW_FRAME_TRACE");
        return value && std::strcmp(value, "0") != 0;
    }();
    double residencyMs = 0, snapshotMs = 0, rendererInitMs = 0, beginObjectsMs = 0, activeLightsMs = 0;
    auto initProbe = slowFrameTrace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto markInitProbe = [&](double& elapsed) {
        if (!slowFrameTrace) return;
        const auto now = std::chrono::steady_clock::now();
        elapsed = std::chrono::duration<double, std::milli>(now - initProbe).count();
        initProbe = now;
    };
    // States where the 3D world must NOT be shown even though its resources stay resident:
    // the mission editor (DisplayArcadeMap; its display gate is lost because the editor world
    // has no _options, so IsDisplayEnabled() wrongly returns true), any covering full-screen UI
    // / loading progress (!IsDisplayEnabled()), and shutdown (Glob.exit / m_closeRequest). In
    // these we skip the 3D scene submission below and clear to black — otherwise the world keeps
    // rendering behind the UI and leaks through the letterboxed sides (HUD aspect limit).
    const bool suppressWorld = _editor || !IsDisplayEnabled() || Glob.exit || (GApp && GApp->m_closeRequest);
    // The GPU-driven backend keeps a GPU-resident world set that draws every frame regardless
    // of the per-frame 3D lists we skip below, so it needs the suppression signalled explicitly.
    GEngine->SuppressWorldObjects(suppressWorld);
    bool farResidencySupported = false;
    if (enableDraw)
    {
        PackedColor color(GEngine->FogColor());
        // Viewer mode: solid charcoal background — no fog-color cast on the model
        // (same reasoning as Blender/Maya default mid-grey backgrounds).
        if (AppConfig::Instance().IsViewerMode())
            color = PackedColor(Color(0.18f, 0.18f, 0.20f, 1.0f));
        if (suppressWorld)
            color = PackedColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        // RenderSnapshot S4: the streamed-object residency update is world-state
        // mutation and belongs on the simulation side of the frame, not inside
        // TerrainWgpu::DrawTerrain where it lived (and billed object streaming to the
        // land:gnd CPU phase). The gate mirrors the old call site's exact reach: only
        // the WGPU engine ever ran it, and GetObjectStats is the existing capability
        // probe that answers true precisely for that engine. Renderer fixed at boot,
        // so probing once is safe.
        if (GLandscape && GEngine)
        {
            static const bool modernObjectsEngine = []
            {
                Engine::ObjectStatsOut probe;
                return GEngine->GetObjectStats(probe);
            }();
            Camera* residencyCamera = _scene.GetCamera();
            if (modernObjectsEngine && residencyCamera)
            {
                farResidencySupported = true;
                GLandscape->UpdateModernObjectResidency(residencyCamera->Position());
            }
        }
        // RenderSnapshot Phase 5 step 1: simulation publishes the frame's presentation
        // snapshot at its own draw boundary; the renderer only consumes it. InitDraw
        // keeps a capture-if-stale fallback for frames no world drives (menu, tools).
        markInitProbe(residencyMs);
        render::frame::CapturePresentationSnapshot();
        markInitProbe(snapshotMs);
        GEngine->InitDraw(clear, color);
        markInitProbe(rendererInitMs);
    }

#if BACKGROUND_AI
    if (IsSimulationEnabled())
    {
        _secThread->StartSecondary(DoBackgroundSimulate, &context);
    }
#endif

    if (enableDraw)
    {
        _scene.BeginObjects();
        DrawViewerSceneAddons();
        // Projectile trails.  Must sit inside the BeginObjects/EndObjects
        // window — the same one VehicleAI's path diagnostic draws its lines in.
        // Returns immediately unless the dev-panel switch is on.
        Dev::Ballistics::Draw();
        // PHY-020 probes, for the same reason and in the same window. Submitting
        // outside BeginObjects/EndObjects silently draws nothing, which reads as
        // "the ball does not exist" while the physics log shows it rolling.
        Dev::DrawPhysicsProbes();
    }

    markInitProbe(beginObjectsMs);
    _drawInsideVehicle = camInsideVehicle;
    if (camInsideVehicle)
    {
        _scene.SelectActiveLights(camInsideVehicle);
    }
    else
    {
        _scene.SelectActiveLights(nullptr);
    }
    markInitProbe(activeLightsMs);
    perf.Mark(Dev::FrameProfiler::PhaseDrawInit);

    if (_scene.GetLandscape())
    {
        // remove all vehicles that should be removed
        MoveOutAndDelete(_vehicles, 0);
        MoveOutAndDelete(_animals, 0);
        MoveOutAndDelete(_fastVehicles, 0);
        MoveOutAndDelete(_buildings, 0);

        if (enableDraw)
        {
            if (!_showMap && IsDisplayEnabled() && !suppressWorld)
            {
                {
                    LandBegEnd objBegEnd;
                    Landscape::CalculBoundingRect(objBegEnd, *_scene.GetCamera(), _scene.GetFogMaxRange(), ObjGrid);

                    int x, z;
                    int xMin = objBegEnd.xBeg, xMax = objBegEnd.xEnd;
                    int zMin = objBegEnd.zBeg, zMax = objBegEnd.zEnd;
                    if (GScene->GetObjectShadows() || GScene->GetVehicleShadows())
                    {
#define SHADOW_BORDER 2
                        xMin -= SHADOW_BORDER, xMax += SHADOW_BORDER;
                        zMin -= SHADOW_BORDER, zMax += SHADOW_BORDER;
                    }
                    if (xMin < 0)
                    {
                        xMin = 0;
                    }
                    if (xMin > ObjRange - 1)
                    {
                        xMin = ObjRange - 1;
                    }
                    if (xMax < 0)
                    {
                        xMax = 0;
                    }
                    if (xMax > ObjRange - 1)
                    {
                        xMax = ObjRange - 1;
                    }
                    if (zMin < 0)
                    {
                        zMin = 0;
                    }
                    if (zMin > ObjRange - 1)
                    {
                        zMin = ObjRange - 1;
                    }
                    if (zMax < 0)
                    {
                        zMax = 0;
                    }
                    if (zMax > ObjRange - 1)
                    {
                        zMax = ObjRange - 1;
                    }
#define RECT_CLIPPERS 1
                    for (z = zMin; z <= zMax; z++)
                    {
                        for (x = xMin; x <= xMax; x++)
                        {
                            // build if necessary
                            const ObjectList& list = GLandscape->GetObjects(z, x);
                            if (list.Null())
                            {
                                continue;
                            }
                            Vector3Val bCenter = list->GetBSphereCenter();
                            float bRadius = list->GetBSphereRadius();
                            // Point3 cPos(VFastTransform,GScene->ScaledInvTransform(),bCenter);
                            const Camera& cam = *GScene->GetCamera();
#if RECT_CLIPPERS
                            ClipFlags andClip = cam.IsClipped(bCenter, bRadius, 1);
                            if (andClip && list->GetNonStaticCount() <= 0)
                            {
                                continue;
                            }
#endif
                            ClipFlags orClip = cam.MayBeClipped(bCenter, bRadius, 1);
                            int n = list.Size();
                            for (int i = 0; i < n; i++)
                            {
                                Object* obj = list[i];
                                PoseidonAssert(obj);
                                if (obj->Invisible())
                                {
                                    continue;
                                }
                                ClipFlags clip = orClip;
#if RECT_CLIPPERS
                                if (obj->Static())
                                {
                                    if (andClip)
                                    {
                                        continue;
                                    }
                                }
                                else
                                {
                                    clip = ClipAll;
                                }
#endif
                                if (obj == camInsideVehicle)
                                {
                                    _scene.ObjectForDrawing(obj, obj->InsideLOD(_camType), clip);
#if _ENABLE_CHEATS
                                    if (DiagMode)
                                    {
                                        obj->DrawDiags();
                                    }
#endif
                                }
                                else
                                {
                                    _scene.ObjectForDrawing(obj, -1, clip);
#if _ENABLE_CHEATS
                                    if (DiagMode)
                                    {
                                        obj->DrawDiags();
                                    }
#endif
                                }
                            }
                        }
                    }
                    for (int i = 0; i < NCloudlets(); i++)
                    {
                        _scene.CloudletForDrawing(GetCloudlet(i));
                    }
                    _scene.EndObjects(); // prepare objects for drawing

                    GEngine->EnableReorderQueues(true);
                }

                perf.Mark(Dev::FrameProfiler::PhaseDrawObjPrep);
                _scene.GetLandscape()->Draw(_scene);
                perf.Mark(Dev::FrameProfiler::PhaseDrawLandscape);

                GEngine->EnableReorderQueues(false);
                GEngine->FlushQueues();

                _scene.ObjectsDrawn();
                GEngine->FlushQueues();

                // Frame validation — ExtractSceneInputs → BuildFrame →
                // ValidateFrame + runtime checks, after the world's
                // pixels have landed.  See the matching call below.
                if (GEngine)
                    render::frame::ObserveRenderedFrame(*GEngine, _scene);
                perf.Mark(Dev::FrameProfiler::PhaseDrawObjects);
                GEngine->EnableNightEye(0);

                if (_cameraEffect)
                {
                    _cameraEffect->Draw();
                }
            }
            if (!IsDisplayEnabled())
            {
                ProgressDraw();
            }
        } // if (enableDraw)
        else
        {
            _scene.CleanUp();
        }
    }
    else
    {
        if (enableDraw)
        {
            _scene.EndObjects(); // prepare objects for drawing
            _scene.DrawObjectsAndShadowsPass1();
            _scene.DrawObjectsAndShadowsPass2();
            _scene.ObjectsDrawn();

            // Frame validation — ExtractSceneInputs → BuildFrame →
            // ValidateFrame + runtime checks.  The wrapper lives in
            // WorldFrameObserver.cpp so this translation unit doesn't
            // need EngineGL33.hpp (which collides with appGlobalsShim's
            // GApp->m_keepFocus macro).  Per-frame stats are surfaced
            // via --render-frame-log (AppConfig flag), default off.
            if (GEngine)
                render::frame::ObserveRenderedFrame(*GEngine, _scene);
        }
        else
        {
            _scene.CleanUp();
        }
    }

    perf.Mark(Dev::FrameProfiler::PhaseDrawPost);

    // Scene->UI seam: the 3D world is done; HUD/menus/3D-in-UI follow. On the HDR
    // renderer this resolves (tonemaps) the offscreen scene to the swapchain so the
    // UI composites display-referred. No-op on GL33.
    if (GEngine)
        GEngine->ResolveSceneToDisplay();

    if (enableDraw)
    {
        if (_map && (_showMap || _forceMap))
        {
            _map->DrawHUD(camAI, 1);
        }

        if (_ui && IsUIEnabled() && !_cameraEffect)
        {
            if (camAI)
            {
                if (_camType == CamInternal)
                {
                    camAI->DrawCameraCockpit();
                }

                if (person && person->IsNVWanted())
                {
                    person->DrawNVOptics();
                }

                _ui->DrawHUD(*_scene.GetCamera(), camAI, _camType);
            }
            else if (camVehicle)
            {
                camVehicle->DrawCameraCockpit();
            }
        }

        NetworkGameState state = GetNetworkManager().GetGameState();
        // MP score table: auto-shown at debriefing, and held-to-show during play while the
        // NetworkStats action (I) is down — the original behaviour. Drawn here inside the
        // HUD/UI 2D pass; drawing it after the cursor overlay (which ends the UI passes)
        // renders nothing.
        if (state == NGSDebriefing || (state == NGSPlay && input.GetAction(UANetworkStats, false)))
        {
            GStats.DrawMPTable(1.0f);
        }
#if _ENABLE_CHEATS
        void DrawNetworkStatistics();
        DrawNetworkStatistics();
#endif

        if (_options)
        {
            _options->DrawHUD(nullptr, 1);
        }
        if (_userDlg)
        {
            _userDlg->DrawHUD(nullptr, 1);
        }
        if (_channel)
        {
            _channel->DrawHUD(nullptr, 1);
        }
        if (_chat)
        {
            _chat->DrawHUD(nullptr, 1);
        }
        if (_voiceChat)
        {
            _voiceChat->DrawHUD(nullptr, 1);
        }
        GChatList.OnDraw();
        if (_warningMessage)
        {
            _warningMessage->OnDraw(nullptr, 1);
        }

        // Polymorphic cursor overlay — drawn last so the cursor
        // sits on top of all UI passes.  ViewerCursorOverlay paints
        // a ring; GameCursorOverlay walks the dialog stack and
        // calls the topmost ControlsContainer's DrawCursor.
        if (_cursorOverlay)
            _cursorOverlay->Draw(_engine);

        if (_mode == GModeNetware)
        {
            DrawConnectionQuality(GetNetworkManager().GetConnectionQuality());
        }

        if (IsSimulationEnabled())
        {
            if (_cutEffect)
            {
                _cutEffect->Draw();
            }
            if (_titleEffect)
            {
                _titleEffect->Draw();
            }
        }
    } // if (enableDraw)

    if (enableDraw)
    {
        GEngine->FinishDraw();
    }

    perf.Mark(Dev::FrameProfiler::PhaseHud);

    if (IsSimulationEnabled())
    {
        PerformSound(camInsideVehicle, deltaT);
    }
    GSoundScene->AdvanceAll(deltaT, !IsSimulationEnabled() || quiet); // sort and activate sounds
    if (GSoundsys)
    {
        GSoundsys->Commit(); // commit deferred settings
    }

    // PERF-013: this used to be Mark(PhaseAiVehicles) followed IMMEDIATELY by
    // Mark(PhaseSound) with nothing between them, so the sound work above was booked as
    // "ai+veh" and "sound" measured the gap between two adjacent statements -- 0.000 ms,
    // every frame. The two readings that made the profiler look sane (ai+veh 0.16 ms,
    // sound 0.000) were the same bug seen from both ends. The AI has never been here: it
    // runs inside the fixed-step loop, which is now `sim:step`.
    perf.Mark(Dev::FrameProfiler::PhaseSound);

    if (enableDraw)
    {
        // Far placements are render-only, not simulation Objects. Cold model
        // registration may acquire the exclusive renderer window; do it after
        // traversal so it cannot serialize the whole producer at InitDraw.
        // NextFrame still owns queue publication and GPU-side mutation ordering.
        if (farResidencySupported && GLandscape && _scene.GetCamera())
            GLandscape->UpdateNativeFarResidency(_scene.GetCamera()->Position());
        GEngine->NextFrame();
    }

    perf.Mark(Dev::FrameProfiler::PhaseSwap);
    perf.EndFrame(Poseidon::gPerfDrawCalls);
#if POSEIDON_DIAG
    {
        // DIAG-001: frame_slow / perf events, the pollers and diag_camera follow
        const Dev::FrameProfiler::FrameRecord& fr = perf.Frame(0);
        Dev::OpDiag::OnFrameEnd(fr.totalMs, fr.ms.data(), Dev::FrameProfiler::PhaseCount, fr.drawCalls,
                                _scene.GetFogMaxRange());
    }
#endif
    if (slowFrameTrace && perf.Frame(0).totalMs >= 100.0f)
    {
        // Individual records, not independent maxima from a rolling window.
        // These are elapsed phases including waits, not exclusive CPU work.
        using Phase = Dev::FrameProfiler;
        const auto& r = perf.Frame(0);
        LOG_INFO(Core, "Slow frame: frame={} total={:.3f} setup={:.3f} sim={:.3f} "
                      "init={:.3f} prep={:.3f} ground={:.3f} objects={:.3f} "
                      "landscape={:.3f} draw={:.3f} post={:.3f} hud={:.3f} sound={:.3f} swap={:.3f}",
                 GEngine ? GEngine->GetFrameCounter() : 0, r.totalMs, r.ms[Phase::PhaseSetup], r.ms[Phase::PhaseSimStep],
                 r.ms[Phase::PhaseDrawInit], r.ms[Phase::PhaseDrawObjPrep], r.ms[Phase::PhaseDrawLandGround],
                 r.ms[Phase::PhaseDrawLandObjects], r.ms[Phase::PhaseDrawLandscape], r.ms[Phase::PhaseDrawObjects],
                 r.ms[Phase::PhaseDrawPost], r.ms[Phase::PhaseHud], r.ms[Phase::PhaseSound], r.ms[Phase::PhaseSwap]);
        LOG_INFO(Core, "Slow frame init split: frame={} residency={:.3f} snapshot={:.3f} "
                      "renderer={:.3f} objects={:.3f} lights={:.3f}",
                 GEngine ? GEngine->GetFrameCounter() : 0, residencyMs, snapshotMs, rendererInitMs,
                 beginObjectsMs, activeLightsMs);
    }
    Poseidon::gPerfDrawCalls = 0;
    // Per-frame streaming upload figures roll here; also emits Perfetto counter tracks
    // when --perf-trace is active.
    Dev::StreamingFrameRoll();

#if _ENABLE_CHEATS
#ifndef _DEBUG
#define REGULAR_FOOTPRINT 1
#endif

#if REGULAR_FOOTPRINT
    static DWORD lastSample = 0;
    if (GlobalTickCount() > lastSample + 60000)
    {
        void MemoryFootprint();
        MemoryFootprint();
        lastSample = GlobalTickCount();
    }
    if (input.GetCheat1ToDo(SDL_SCANCODE_O))
    {
        void MemoryFootprint();
        MemoryFootprint();
        lastSample = UINT_MAX;
    }
#else
    if (input.GetCheat1ToDo(SDL_SCANCODE_O))
    {
        void MemoryFootprint();
        MemoryFootprint();
    }
#endif

#endif
}

// SIM-811 / roadmap Phase 8 step 10S. The order of a tick is `Sim::kSimStageOrder` --
// data, in World/SimStageGraph.hpp, next to the resource declarations that say which
// stage produces what the next one reads. The bodies live here; the SEQUENCE does not.
//
// This is the serial graph and nothing more. One thread, no barriers, no scheduler, and
// the stages run in exactly the order the call sequence used to run them in -- SIM-810
// recorded that order first precisely so this change could not quietly alter it. In
// particular scripts still run BEFORE the AI, which is where the engine has always put
// them and where Phase 8's diagram does not; see the note in SimStageGraph.hpp.
// REN-INTERP-001: the entities the tick loop moves. Buildings and static world objects
// never move and are not captured, so they keep drawing at their own frame for free.
void World::CaptureRenderPrevFrames()
{
    if (!Object::RenderInterpEnabled())
        return;
    for (int i = 0; i < _vehicles.Size(); i++)
        if (Entity* e = _vehicles.Get(i))
            e->CaptureRenderPrev();
    for (int i = 0; i < _animals.Size(); i++)
        if (Entity* e = _animals.Get(i))
            e->CaptureRenderPrev();
    for (int i = 0; i < _fastVehicles.Size(); i++)
        if (Entity* e = _fastVehicles[i])
            e->CaptureRenderPrev();
}

void World::StepSimulation(float deltaT, float noAccDeltaT, Entity* cameraVehicle, bool updateVisibility)
{
    struct StepContext
    {
        World*  world;
        float   deltaT;
        float   noAccDeltaT;
        Entity* cameraVehicle;
    };

    StepContext context{this, deltaT, noAccDeltaT, cameraVehicle};

    std::uint32_t enabled = Sim::kAllSimStages;
    // Opt-in (POSEIDON_FIXEDSTEP_SCRIPTS / the Fixed Step tab), off by default -- see
    // the note on FixedStepScriptsEnabled().
    if (!Dev::FixedStepScriptsEnabled())
    {
        enabled &= ~Sim::SimStageMask(Sim::SimStage::Scripts);
    }
    // The Ctrl-Y cheat toggle. True in normal play.
    if (!updateVisibility)
    {
        enabled &= ~Sim::SimStageMask(Sim::SimStage::Visibility);
    }
    // No physics world, no physics stage. `GetPhysicsWorld` is a plain accessor over a
    // global, so asking here rather than inside the stage costs nothing and keeps the
    // executed trace honest about what actually ran.
    if (Physics::GetPhysicsWorld() == nullptr)
    {
        enabled &= ~Sim::SimStageMask(Sim::SimStage::Physics);
    }

    Sim::RunSimStagesSerial(enabled, &context,
                            [](void* ctx, Sim::SimStage stage)
                            {
                                StepContext& step = *static_cast<StepContext*>(ctx);
                                World&       world = *step.world;

                                switch (stage)
                                {
                                case Sim::SimStage::Clock:
                                    // Keep the world clock, weather and wind in the same tick as the
                                    // vehicles that consume them. Rendering observes the most recently
                                    // completed tick.
                                    Glob.time += step.deltaT;
                                    world.SimulateLandscape(step.deltaT);
                                    break;

                                case Sim::SimStage::Scripts:
                                    // After the clock so a script observing `time` sees this tick's
                                    // value, and before the AI so orders a script issues are acted on
                                    // within the same tick rather than the next one.
                                    world.SimulateScripts();
                                    break;

                                case Sim::SimStage::AI:
#if BACKGROUND_AI
                                    world._secThread->FinishSecondary();
#else
                                    world.PerformAI(step.deltaT, step.noAccDeltaT);
#endif
                                    if (GLandscape && (GLandscape->ModernSimulationResidencyEnabled() ||
                                        GLandscape->HasModernSourceDiagnosticDemand()))
                                    {
                                        Streaming::SimulationResidencyOwnerBoundary boundary;
                                        GLandscape->PumpModernSimulationResidency(boundary);
                                    }
                                    // Inside the fixed step, after the stage that decides.
                                    // Recording per FRAME instead would make the number of
                                    // entries depend on framerate, and two runs would not
                                    // even be the same length. Off unless
                                    // POSEIDON_AI_TIMELINE names a file.
                                    AIDiag::RecordAITick(&world);
                                    break;

                                case Sim::SimStage::Vehicles:
                                    world.SimulateAllVehicles(step.deltaT, step.noAccDeltaT, step.cameraVehicle);
                                    break;

                                case Sim::SimStage::Visibility:
                                    world.GetSensorList()->SmartUpdateAll();
                                    break;

                                case Sim::SimStage::Physics:
                                    // PHY-020: exactly one physics step per fixed simulation tick.
                                    // Deliberately NOT its own accumulator -- a second clock would
                                    // drift against this one and reintroduce the frame-rate dependence
                                    // the whole ARCH-001 item removed. It also inherits the catch-up
                                    // cap and the pause behaviour for free.
                                    //
                                    // The proxy moves BEFORE the step, so its sweep is resolved in the
                                    // same tick as the bodies it pushes. Moving it after would leave
                                    // every shove one tick late and make contacts look sticky.
                                    // The PLAYER, not the camera's vehicle. In free-fly there is no
                                    // camera vehicle at all, so the proxy silently never moved --
                                    // which reads exactly like "objects ignore me".
                                    if (Person* player = world.PlayerOn())
                                    {
                                        Dev::UpdatePhysicsPlayerProxy(player->WorldTransform().Position());
                                    }
                                    else if (step.cameraVehicle)
                                    {
                                        Dev::UpdatePhysicsPlayerProxy(
                                            step.cameraVehicle->WorldTransform().Position());
                                    }
                                    Physics::GetPhysicsWorld()->Step(step.deltaT);
                                    break;
                                }
                            });

    // SIM-815: after every stage of the tick has run, fold the load-bearing sim
    // state into one log line. Off unless POSEIDON_TICK_HASH=1; costs one branch
    // per tick when off.
    RecordTickStateHash();
}

void World::RecordTickStateHash()
{
    static const bool on = []
    {
        const char* v = std::getenv("POSEIDON_TICK_HASH");
        return v && v[0] != '0' && v[0] != '\0';
    }();
    if (!on)
    {
        return;
    }

    using namespace Poseidon::Determinism;
    // Process-lifetime on purpose: the tick index then counts every fixed step
    // the process ever ran, and two runs are aligned by the tms= column (under
    // lockstep Glob.time is affine in the tick index), not by assuming both
    // worlds started on the same tick.
    static TickStateHash recorder;

    recorder.BeginTick();
    std::uint64_t ents = 0;
    auto foldEntity = [&](Entity* veh)
    {
        if (!veh)
        {
            return;
        }
        const Vector3 pos = veh->Position();
        recorder.FoldVec3(TickFieldClass::Positions, pos.X(), pos.Y(), pos.Z());
        const Vector3 spd = veh->ObjectSpeed();
        recorder.FoldVec3(TickFieldClass::Velocities, spd.X(), spd.Y(), spd.Z());
        ++ents;
    };
    // The lists the simulation itself steps, in their stored order -- a change
    // of ORDER hashes differently, and that is a finding, not a false positive
    // (DistributeImportances is a pure function of state, so two identical runs
    // partition identically). Buildings are deliberately excluded: they do not
    // move, and their residency is already visible through the shape count.
    for (int i = 0; i < _vehicles.Size(); i++)
    {
        foldEntity(_vehicles.Get(i));
    }
    for (int i = 0; i < _fastVehicles.Size(); i++)
    {
        foldEntity(_fastVehicles[i]);
    }
    for (int i = 0; i < _outVehicles.Size(); i++)
    {
        foldEntity(_outVehicles[i]);
    }
    if (Camera* cam = _scene.GetCamera())
    {
        const Vector3 cp = cam->Position();
        recorder.FoldVec3(TickFieldClass::Camera, cp.X(), cp.Y(), cp.Z());
    }

    recorder.SetCounter(TickCounter::RngDraws, GRandGen.SequentialDraws());
    recorder.SetCounter(TickCounter::ShapeCount, static_cast<std::uint64_t>(Shapes.Size()));
    recorder.SetCounter(TickCounter::EntityCount, ents);
    recorder.SetCounter(TickCounter::Cloudlets, static_cast<std::uint64_t>(_cloudlets.Size()));
    recorder.SetCounter(TickCounter::TimeMs, static_cast<std::uint64_t>(static_cast<std::int64_t>(Glob.time.toInt())));

    const TickStateRow row = recorder.EndTick();
    LOG_INFO(World, "{}", TickStateHash::FormatRow(row));
}

void World::HandleVoiceChatShortcuts()
{
    if (GetNetworkManager().GetGameState() < NGSCreate ||
        !Poseidon::ShouldHandleMultiplayerChatShortcut(_chat != nullptr))
    {
        return;
    }

    auto& input = InputSubsystem::Instance();
    if (input.GetActionToDo(UAChat, true, false))
    {
        CreateChat();
    }

    if (!_voiceChat && input.GetAction(UAVoiceOverNetPushToTalk, false) > 0)
    {
        CreateVoiceChat(true);
    }

    if (!_voiceChat && input.GetActionToDo(UAVoiceOverNet, true, false))
    {
        CreateVoiceChat();
    }
}
