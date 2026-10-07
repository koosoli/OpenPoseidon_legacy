#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_scancode.h>
#include <stdint.h>
#include <algorithm>
#include <map>
#include <string_view>
#include <system_error>
#include <utility>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/platform.hpp>

// The PCH pulls in Logging.hpp which #defines DebugLog() as a logging macro.
// That collides with the method ImGui::DebugLog().  Undef before including
// ImGui headers — none of our code in this TU uses the DebugLog macro.
#ifdef DebugLog
#undef DebugLog
#endif

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#include <SDL3/SDL.h>
#include <glad/gl.h>

#include <Poseidon/Dev/Debug/DebugOverlay.hpp>
#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/Dev/Debug/DebugCheats.hpp>
#include <Poseidon/Dev/Debug/DebugCommands.hpp>
#include <Poseidon/Dev/Debug/WtrTestHarness.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Config/UserConfig.hpp>
#include <Poseidon/Input/ControlsCategory.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/Input/UserAction.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/LicensePlateTextTuning.hpp>
#include <Poseidon/Graphics/Rendering/Draw/FontMapping.hpp>
#include <Poseidon/UI/Locale/MissionLanguageDetector.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>
#include <Poseidon/Dev/Diag/FrameProfiler.hpp>
#include <Poseidon/Dev/Diag/FlightTab.hpp>
#include <Poseidon/Dev/Diag/LightingDiag.hpp>
#include <Poseidon/Dev/Diag/BallisticsTab.hpp>
#include <Poseidon/Dev/Diag/StreamingTab.hpp>
#include <Poseidon/Dev/Diag/AITab.hpp>
#include <Poseidon/Dev/Diag/FixedStepTab.hpp>
#include <Poseidon/Dev/Diag/PhysicsTab.hpp>
#include <Poseidon/Dev/Diag/TerrainBrush.hpp>
#include <Poseidon/Dev/Diag/CaveEditor.hpp>
#include <Poseidon/World/Terrain/TerrainCrater.hpp>
#include <Poseidon/Dev/Diag/MapEditorTab.hpp>
#include <Poseidon/Dev/Diag/PictureModeTab.hpp>
#include <Poseidon/Dev/Diag/WeatherTab.hpp>
#include <Poseidon/Dev/Diag/SmokeTab.hpp>
#include <Poseidon/Dev/Diag/OpDiag.hpp> // DIAG-001 3D overlay (--diag-draw)
#include <Poseidon/UI/Settings/GameSettingsConfig.hpp>
#include <Poseidon/UI/Settings/AspectRatio.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Core/FoliageDusk.hpp>
#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Foundation/Memory/CheckMem.hpp>
#include <Poseidon/Foundation/Memory/MemFreeReq.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp> // ZEUS-THROW: Shot, NewShot
#include <Poseidon/World/WorldInputContext.hpp>
#include <Poseidon/World/Scene/Camera/CamEffects.hpp>
#include <Poseidon/World/Scene/Camera/CameraHold.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/WaterBodies.hpp>
#include <Poseidon/AI/AICenter.hpp>
#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/AI/EntityAI.hpp>
#include <Poseidon/Network/Network.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/UI/Settings/GraphicsApply.hpp>
#include <Poseidon/UI/Settings/GraphicsConfig.hpp>
#include <Poseidon/Graphics/Textures/EnfusionTextureName.hpp> // RFG-070 layer-tint switch
#include <Evaluator/express.hpp>
#include <filesystem>

// Voice-language + visibility helpers — extern decls so we don't
// have to pull the locale/audio/world internals into this TU.  All
// three are linked in from Poseidon.lib at the unified-build stage.
extern const std::string& GetSelectedVoiceLanguage();
extern void SetSelectedVoiceLanguage(const std::string&);
extern void SetVisibility(float distance);

#include <string>
#include <cstring>
#include <cstdio>
#include <functional>
#include <array>
#include <vector>

// The cutscene letterbox state, defined at global scope in WorldSetup.cpp. Declared here rather
// than pulled in via a header because only the Zeus camera needs it, and it must be at global
// scope: declared inside the namespaces below, these would be new symbols that never link.
extern bool showCinemaBorder;
void ShowCinemaBorder(bool show);

namespace Poseidon::Dev
{
namespace DebugOverlay
{

namespace
{
// Which renderer backend composites ImGui: imgui_impl_opengl3 (GL33) or the
// Engine overlay virtuals (wgpu). The SDL3 platform backend is shared.
enum class RenderBackend
{
    OpenGL3,
    Engine
};
RenderBackend s_backend = RenderBackend::OpenGL3;

bool s_initialized = false;
bool s_visible = false;
bool s_selectShadowsTab = false; // one-shot: force-select the Shadows tab next draw
bool s_selectMemoryTab = false;  // one-shot: force-select the Memory tab next draw
SDL_Window* s_window = nullptr;
// Saved mouse-grab state while the dev panel holds the cursor released.
bool s_mouseReleasedByPanel = false;
bool s_savedMouseGrab = false;

bool MultiplayerSession()
{
    return GWorld && GWorld->GetMode() == GModeNetware;
}

// Deferred actions — populated by UI button click handlers, drained
// AFTER ImGui::Render() returns each frame.  Why deferred:
//
// Some cheat invocations call deep into the engine and run code that
// mutates state ImGui still needs in the current frame.  The worst
// offender is Cmd_SaveGame, whose World::SaveBin path ends with
// `MemoryCleanUp()` (engine/poseidon/Memory/JimboAllocator.cpp:252)
// — that shrinks the engine's memory pool back to the OS and can
// invalidate buffers some ImGui widget still references later in
// the same DrawCheatsTab pass.  The result was a confirmed crash
// stack:
//   ImGui::ButtonEx+0x38
//   DrawCheatsTab+0x275
//   DrawMainWindow+0x6d
//   EngineGL33::BackToFront+0xd
//
// Deferring keeps the click handlers tiny (just enqueue a closure)
// and runs the actual cheat after ImGui::Render() — by which point
// no ImGui internal data is still in flight, so any engine
// reallocation is safe.
std::vector<std::function<void()>> s_pendingActions;

void Defer(std::function<void()> action)
{
    if (!MultiplayerSession())
        s_pendingActions.push_back(std::move(action));
}

void ApplyDevPanelMouseState();

// Zeus is deliberately a dev-panel feature, rather than a mission-script
// command.  The camera is the engine's native manual CameraVehicle, which
// already implements collision-safe free flight and the normal movement
// bindings.  Keeping it here also means it cannot leak into release builds.
OLink<CameraVehicle> s_zeusCamera;
bool s_terrainBrushDragging = false;
Uint64 s_terrainBrushTickMs = 0;
// showCinemaBorder is a GLOBAL that defaults to true, so ANY active camera effect draws the
// cutscene letterbox -- the CinemaBorder model plus the widescreen pillarbox bars. Zeus free-fly
// installs a camera effect, so it inherited the letterbox and the view shrank to a cinematic band
// the moment you enabled it. That is right for a cutscene and wrong for a developer camera.
//
// Saved and restored rather than forced off, because `showCinemaBorder` is also an SQF command a
// mission may have set deliberately; Zeus is a temporary takeover and must give it back.
//
// Read through the global rather than a getter: ShowCinemaBorder(bool) has no matching reader,
// and adding one for a dev panel is more surface than this needs. Both are declared at GLOBAL
// scope above -- declaring them in here would name new symbols inside this namespace instead.
bool s_zeusPrevCinemaBorder = true;
std::string s_zeusStatus;
// "Go to coordinates": whatever the owner pasted, verbatim. Sized for a full readout
// line with room to spare; the parser below ignores everything that is not a number.
char s_zeusGotoBuf[256] = {};
int s_zeusSide = 0;
int s_zeusSpawnKind = 0;
int s_zeusPreset = 0;
int s_zeusCount = 1;
float s_zeusDistance = 25.0f;
float s_zeusHeading = 0.0f;
char s_zeusClassName[96] = "SoldierWB";
bool s_zeusClickPlacement = false;
bool s_zeusConsumeMouseEvent = false;
bool s_zeusSuppressNextMouseUp = false;
bool s_zeusRotateDrag = false;
bool s_zeusMoveDrag = false;
bool s_zeusLassoDrag = false;
bool s_zeusConsumeKeyboardEvent = false;
bool s_zeusConsumeShortcutKeyUp = false;
float s_zeusLassoStartX = 0.0f;
float s_zeusLassoStartY = 0.0f;
float s_zeusLassoEndX = 0.0f;
float s_zeusLassoEndY = 0.0f;
Ref<ControlsContainer> s_zeusCursor;

struct ZeusSpawnRecord
{
    OLink<Entity> object;
    std::string className;
    int kind;
    TargetSide side;
};
std::vector<ZeusSpawnRecord> s_zeusSpawned;
std::vector<ZeusSpawnRecord> s_zeusSelection;
std::vector<ZeusSpawnRecord> s_zeusClipboard;
std::vector<Vector3> s_zeusMoveOffsets;

constexpr std::array<const char*, 4> kZeusSideNames = {"West", "East", "Resistance", "Civilian"};
constexpr std::array<TargetSide, 4> kZeusSides = {TWest, TEast, TGuerrila, TCivilian};
constexpr std::array<const char*, 6> kZeusUnitPresets = {"SoldierWB", "SoldierEB", "SoldierGB",
                                                         "OfficerW",  "OfficerE",  "Civilian"};
constexpr std::array<const char*, 7> kZeusVehiclePresets = {"Jeep", "UAZ", "M1A1", "T72", "BMP", "UH60", "Mi17"};

bool ZeusWorldAvailable()
{
    return !MultiplayerSession() && GWorld && GLandscape && GWorld->CameraOn();
}

void SetZeusClassFromPreset()
{
    if (s_zeusSpawnKind == 0)
    {
        const int preset = std::clamp(s_zeusPreset, 0, static_cast<int>(kZeusUnitPresets.size()) - 1);
        snprintf(s_zeusClassName, sizeof(s_zeusClassName), "%s", kZeusUnitPresets[preset]);
    }
    else
    {
        const int preset = std::clamp(s_zeusPreset, 0, static_cast<int>(kZeusVehiclePresets.size()) - 1);
        snprintf(s_zeusClassName, sizeof(s_zeusClassName), "%s", kZeusVehiclePresets[preset]);
    }
}

void EnableZeusCamera()
{
    if (!ZeusWorldAvailable())
    {
        s_zeusStatus = "Zeus requires a loaded world.";
        return;
    }
    if (s_zeusCamera)
    {
        s_zeusStatus = "Free-fly camera is already active.";
        return;
    }

    Object* source = GWorld->CameraOn();
    auto* camera = new CameraVehicle();
    camera->SetPosition(source->CameraPosition());
    camera->SetDirectionAndUp(source->Direction(), VUp);
    camera->SetManual(true);
    camera->SetAltitudeSpeedScaling(true);
    camera->SetMouseLookRequiresRightButton(true);
    camera->SetCrossHairs(false);
    camera->ResetTargets();
    GWorld->AddAnimal(camera);
    GWorld->SetCameraEffect(CreateCameraEffect(camera, "Internal", CamEffectTop, true));
    s_zeusCamera = camera;
    s_zeusPrevCinemaBorder = showCinemaBorder;
    ShowCinemaBorder(false);
    s_zeusCursor = new ControlsContainer(nullptr);
    s_zeusStatus = "Free-fly active: WASD move, Q/Z up/down, Shift doubles speed; hold RMB to look. Click Zeus objects "
                   "to select. Press Ctrl+` to reopen Zeus.";
    // The panel captures mouse input while it is visible. Hide it immediately
    // so the camera becomes controllable as soon as Zeus is enabled.
    SetVisible(false);
}

// Take over a free-fly camera someone else built. `--test-world-freefly` creates
// exactly the CameraVehicle EnableZeusCamera does and then forgets it, so the panel
// had no idea a free-fly camera was flying: the Game tab showed no free-fly
// coordinates and the Zeus tab offered to enable a camera that was already active.
// Adopting it makes both work, and makes "Beam player here" available from a
// command-line flight -- which is the case where knowing where you are and being
// able to stand there matters most.
//
// The cinema-border state is captured now rather than assumed, so ending the
// flight restores what was actually on screen when it started.
void AdoptFreeFlyCameraImpl(CameraVehicle* camera)
{
    if (s_zeusCamera == camera)
        return;
    s_zeusCamera = camera;
    if (camera)
    {
        s_zeusPrevCinemaBorder = showCinemaBorder;
        s_zeusStatus = "Free-fly camera adopted from --test-world-freefly. Coordinates are on the Game tab.";
    }
}

void DisableZeusCamera()
{
    if (!s_zeusCamera)
    {
        s_zeusStatus = "Free-fly camera is not active.";
        return;
    }
    GWorld->SetCameraEffect(nullptr);
    ShowCinemaBorder(s_zeusPrevCinemaBorder);
    s_zeusCamera->SetDelete();
    s_zeusCamera = nullptr;
    s_zeusClickPlacement = false;
    s_zeusRotateDrag = false;
    s_zeusMoveDrag = false;
    s_zeusLassoDrag = false;
    s_zeusCursor = nullptr;
    ApplyDevPanelMouseState();
    s_zeusStatus = "Free-fly ended; returned to the normal camera.";
}

// Put the real player exactly where the free-fly camera is, but keep the
// character physically valid: standing on the terrain/road rather than at the
// camera's arbitrary altitude.  This is deliberately a separate action from
// spawning so it cannot accidentally replace or duplicate the player.
// Pull the numbers out of whatever was pasted, ignoring everything else.
//
// The point is that the panel's OWN readout can be pasted straight back in. That line is
//     freefly [2721.96, 944.30, 99.33]  az 306.9  el -5.4  (active)
// so brackets, commas, the word `freefly`, the `az`/`el` labels and a trailing `(active)`
// all have to be skipped -- and a bare `2721.96 944.30 99.33` has to work too, because
// that is what a log line and the --test-world-freefly flag look like.
//
// ORDER IS X, Z, ALTITUDE. That is what the readout prints and what
// --test-world-freefly takes, and it is NOT the (x, altitude, z) order the engine uses
// internally -- copying a pose from an engine-internal log put a camera at 6099 m
// photographing clouds once already. This function is the one place that conversion
// happens, so the two orders cannot be confused anywhere else.
//
// A leading minus is part of its number (elevations are negative); anything else
// non-numeric just ends the current number.
struct ZeusGotoPose
{
    float x = 0.0f;
    float z = 0.0f;
    float altitude = 0.0f;
    float azimuth = 0.0f;
    float elevation = 0.0f;
    int count = 0; // how many numbers were found, so the caller knows what is authored
};

ZeusGotoPose ParseZeusGoto(const char* text)
{
    ZeusGotoPose pose;
    if (!text)
        return pose;

    float values[5] = {};
    int found = 0;
    const char* p = text;
    while (*p && found < 5)
    {
        const bool numeric = (*p >= '0' && *p <= '9') || *p == '.' ||
                             ((*p == '-' || *p == '+') && p[1] && ((p[1] >= '0' && p[1] <= '9') || p[1] == '.'));
        if (!numeric)
        {
            ++p;
            continue;
        }
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) // strtof refused it after all; do not spin on the same byte
        {
            ++p;
            continue;
        }
        values[found++] = v;
        p = end;
    }

    pose.count = found;
    if (found >= 1)
        pose.x = values[0];
    if (found >= 2)
        pose.z = values[1];
    if (found >= 3)
        pose.altitude = values[2];
    if (found >= 4)
        pose.azimuth = values[3];
    if (found >= 5)
        pose.elevation = values[4];
    return pose;
}

// Move the free-fly camera to a pasted pose, enabling free-fly first if it is not already
// running, so one paste + one click is the whole interaction.
//
// Altitude is taken as ABSOLUTE when it was authored, matching the readout and
// --test-world-freefly. Two numbers alone mean "this spot, sensible height": the terrain
// there plus an eye offset, because a paste of just X/Z otherwise buries the camera.
void GoToZeusCoordinates()
{
    const ZeusGotoPose pose = ParseZeusGoto(s_zeusGotoBuf);
    if (pose.count < 2)
    {
        s_zeusStatus = "Paste at least X and Z -- e.g. `2721.96 944.30 99.33 306.9 -5.4`, or the freefly readout.";
        return;
    }
    if (!ZeusWorldAvailable())
    {
        s_zeusStatus = "Go to coordinates requires a loaded world.";
        return;
    }
    if (!s_zeusCamera)
    {
        EnableZeusCamera();
        if (!s_zeusCamera)
            return; // EnableZeusCamera already explained why
    }

    const float groundY = GLandscape ? GLandscape->RoadSurfaceYAboveWater(pose.x, pose.z) : 0.0f;
    const float altitude = pose.count >= 3 ? pose.altitude : groundY + 1.8f;

    Vector3 position(pose.x, altitude, pose.z);
    s_zeusCamera->SetPosition(position);
    // The camera aims at a target point, not at its own orientation: ResetTargets()
    // re-derives that point from the CURRENT position and direction (CameraHold.cpp,
    // `Position() + Direction() * 1e5`). Teleporting without it leaves the camera locked
    // on the aim point it had before the jump, so the view is rotated toward wherever you
    // came from and mouse-look starts from that wrong basis -- which is exactly the
    // "wrongly orientated / panning not right" this had on its first outing. Both the
    // engine's own mouse-look and EnableZeusCamera call it after re-orienting; so does
    // this now, once, after the final position AND direction are set.

    // Azimuth is compass-style: 0 = +Z (north), increasing clockwise, which is what the
    // readout prints. Elevation is degrees above the horizon, so a negative value looks
    // down. Only applied when both were authored; a two- or three-number paste keeps the
    // heading you were already flying, which is what you want when nudging a position.
    if (pose.count >= 5)
    {
        const float az = pose.azimuth * (H_PI / 180.0f);
        const float el = pose.elevation * (H_PI / 180.0f);
        const float cosEl = std::cos(el);
        Vector3 direction(std::sin(az) * cosEl, std::sin(el), std::cos(az) * cosEl);
        direction.Normalize();
        s_zeusCamera->SetDirectionAndUp(direction, VUp);
    }
    s_zeusCamera->ResetTargets();

    char status[192];
    std::snprintf(status, sizeof(status), "Moved free-fly to %.2f, %.2f, %.2f%s (terrain %.1f m).", pose.x, pose.z,
                  altitude, pose.count >= 5 ? " with heading" : "", groundY);
    s_zeusStatus = status;
}

void BeamPlayerToZeusCamera()
{
    if (!s_zeusCamera || !GWorld || !GLandscape)
    {
        s_zeusStatus = "Beam requires an active free-fly camera and a loaded landscape.";
        return;
    }

    Person* player = GWorld->GetRealPlayer();
    if (!player)
    {
        s_zeusStatus = "No real player character is available to beam.";
        return;
    }

    Vector3 position = s_zeusCamera->Position();
    position[1] = GLandscape->RoadSurfaceYAboveWater(position[0], position[2]);
    Vector3 direction = s_zeusCamera->Direction();
    direction[1] = 0.0f;
    if (direction.SquareSize() < 1e-4f)
        direction = player->Direction();
    direction.Normalize();

    Matrix4 transform = player->Transform();
    transform.SetPosition(position);
    transform.SetDirectionAndUp(direction, VUp);
    Vector3 normal;
    if (EntityAI* entity = dyn_cast<EntityAI>(player))
        AIUnit::FindFreePosition(position, normal, true, entity);
    transform.SetPosition(position);
    player->PlaceOnSurface(transform);
    player->SetTransform(transform);

    // The camera must be released before switching back to the player.  This
    // returns input and view control in the same operation, rather than
    // leaving the user in a detached camera after the teleport.
    GWorld->SetCameraEffect(nullptr);
    ShowCinemaBorder(s_zeusPrevCinemaBorder);
    s_zeusCamera->SetDelete();
    s_zeusCamera = nullptr;
    s_zeusClickPlacement = false;
    s_zeusRotateDrag = false;
    s_zeusMoveDrag = false;
    s_zeusLassoDrag = false;
    s_zeusCursor = nullptr;
    GWorld->SwitchCameraTo(player, CamInternal);
    ApplyDevPanelMouseState();
    s_zeusStatus = "Player beamed to the free-fly position; free-fly ended.";
}

// --- Zeus cursor coordinates ------------------------------------------------
//
// The focused Zeus viewport draws the engine's own cursor sprite
// (ControlsContainer::DrawCursor).  That sprite is placed from the engine's
// normalised cursor (InputSubsystem::GetCursorX/Y, -1..+1 across the 2D
// surface), which is accumulated from *relative* mouse motion and scaled by
// the engine's own cursor sensitivity and aspect correction.  SDL button and
// motion events carry absolute window pixels instead: a different origin, a
// different scale, and — under relative mouse mode — no meaningful value at
// all.  The two spaces drift apart, so a click lands somewhere other than
// where the visible cursor points.
//
// Every Zeus interaction (pick, lasso, group move, paste, cursor spawning)
// therefore resolves its position through ZeusCursorPixel() below, and never
// from raw SDL event coordinates.  ZeusCursorPixel returns *framebuffer
// pixels*, which is the space camera->Projection() maps world positions into,
// so picking and the ImGui overlay share one frame of reference.
struct ZeusPoint
{
    float x = 0.0f;
    float y = 0.0f;
};

ZeusPoint ZeusCursorPixel()
{
    ZeusPoint point;
    if (!GEngine)
        return point;
    const auto& input = InputSubsystem::Instance();
    point.x = (input.GetCursorX() * 0.5f + 0.5f) * static_cast<float>(GEngine->Width());
    point.y = (input.GetCursorY() * 0.5f + 0.5f) * static_cast<float>(GEngine->Height());
    return point;
}

// Framebuffer pixels -> ImGui overlay coordinates.  ImGui works in window
// logical units, which differ from framebuffer pixels whenever the display
// scale is not 1.
ImVec2 ZeusPixelToImGui(float pixelX, float pixelY)
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scaleX = (GEngine && GEngine->Width() > 0) ? display.x / static_cast<float>(GEngine->Width()) : 1.0f;
    const float scaleY = (GEngine && GEngine->Height() > 0) ? display.y / static_cast<float>(GEngine->Height()) : 1.0f;
    return ImVec2(pixelX * scaleX, pixelY * scaleY);
}

// World position -> framebuffer pixels, or false when the position is behind
// the near plane.  Shared by picking, lasso hit-testing and marker drawing so
// they can never disagree about where an object appears.
bool ZeusProjectToPixel(Vector3Par position, ZeusPoint& pixel)
{
    const Camera* camera = GScene ? GScene->GetCamera() : nullptr;
    if (!camera)
        return false;
    Vector3 projected = GScene->ScaledInvTransform() * position;
    if (projected.Z() < camera->Near())
        return false;
    const Matrix4& projection = camera->Projection();
    const float invZ = 1.0f / projected.Z();
    pixel.x = projection(0, 2) + projection(0, 0) * projected[0] * invZ;
    pixel.y = projection(1, 2) + projection(1, 1) * projected[1] * invZ;
    return true;
}

Vector3 ZeusSpawnPosition(int index, int count)
{
    Object* source = s_zeusCamera ? static_cast<Object*>(s_zeusCamera) : GWorld->CameraOn();
    Vector3 pos = source->Position() + source->Direction() * s_zeusDistance;
    const float offset = (static_cast<float>(index) - (static_cast<float>(count) - 1.0f) * 0.5f) * 4.0f;
    pos += source->DirectionAside() * offset;
    pos[1] = GLandscape->RoadSurfaceYAboveWater(pos[0], pos[2]);
    return pos;
}

Vector3 ZeusSpawnPositionFromAnchor(Vector3Par anchor, int index, int count)
{
    // A single placement must land exactly under the cursor.  More than one
    // infantry/vehicle cannot safely occupy exactly the same collision volume,
    // so only a multi-spawn uses a compact lateral formation around that
    // literal cursor anchor.
    if (count <= 1)
        return anchor;
    Object* source = s_zeusCamera ? static_cast<Object*>(s_zeusCamera) : GWorld->CameraOn();
    const float offset = (static_cast<float>(index) - (static_cast<float>(count) - 1.0f) * 0.5f) * 4.0f;
    Vector3 pos = anchor + source->DirectionAside() * offset;
    pos[1] = GLandscape->RoadSurfaceYAboveWater(pos[0], pos[2]);
    return pos;
}

bool ZeusClickPositionAtPixel(Vector3& position, float pixelX, float pixelY)
{
    if (!s_zeusCamera || !GLandscape)
        return false;
    const Camera* camera = GScene ? GScene->GetCamera() : nullptr;
    if (!camera)
        return false;
    // Invert exactly the projection ZeusProjectToPixel applies.  Doing the
    // unprojection this way (rather than assuming the world fills the
    // framebuffer) keeps the ray correct when AspectSettings renders the world
    // into a sub-rectangle, and makes the ray agree with the pick test.
    const Matrix4& projection = camera->Projection();
    if (projection(0, 0) == 0.0f || projection(1, 1) == 0.0f)
        return false;
    const float ndcX = (pixelX - projection(0, 2)) / projection(0, 0);
    // projection(1, 1) is negative (screen Y grows downwards), so ndcY comes
    // out positive-up and adds to DirectionUp directly.
    const float ndcY = (pixelY - projection(1, 2)) / projection(1, 1);
    const Vector3 direction = (s_zeusCamera->Direction() + s_zeusCamera->DirectionAside() * ndcX * camera->Left() +
                               s_zeusCamera->DirectionUp() * ndcY * camera->Top())
                                  .Normalized();
    return GLandscape->IntersectWithGroundOrSea(&position, s_zeusCamera->Position(), direction, 0.0f, 10000.0f) >= 0.0f;
}

bool ZeusClickPosition(Vector3& position)
{
    const ZeusPoint cursor = ZeusCursorPixel();
    return ZeusClickPositionAtPixel(position, cursor.x, cursor.y);
}

void RememberZeusSpawn(Entity* object, const char* className, int kind, TargetSide side)
{
    if (object)
        s_zeusSpawned.push_back({object, className, kind, side});
}

void PruneZeusRecords(std::vector<ZeusSpawnRecord>& records)
{
    records.erase(
        std::remove_if(records.begin(), records.end(), [](const ZeusSpawnRecord& record) { return !record.object; }),
        records.end());
}

// cursorX/cursorY are framebuffer pixels from ZeusCursorPixel().
void SelectZeusAtCursor(float cursorX, float cursorY)
{
    PruneZeusRecords(s_zeusSpawned);
    const Camera* camera = GScene ? GScene->GetCamera() : nullptr;
    if (!camera)
        return;
    ZeusSpawnRecord* closest = nullptr;
    float closestDistance2 = Square(30.0f);
    for (auto& record : s_zeusSpawned)
    {
        ZeusPoint pixel;
        if (!ZeusProjectToPixel(record.object->Position(), pixel))
            continue;
        const float distance2 = Square(pixel.x - cursorX) + Square(pixel.y - cursorY);
        if (distance2 < closestDistance2)
        {
            closest = &record;
            closestDistance2 = distance2;
        }
    }
    if (closest)
    {
        // Clicking a member of an existing lasso selection starts a group
        // move.  Do not collapse the selection to the clicked unit: the
        // move-drag code deliberately keeps every member's offset from the
        // anchor and places the group atomically on mouse release.
        const bool alreadySelected =
            std::any_of(s_zeusSelection.begin(), s_zeusSelection.end(), [closest](const ZeusSpawnRecord& record)
                        { return record.object.GetLink() == closest->object.GetLink(); });
        if (!alreadySelected)
        {
            s_zeusSelection.clear();
            s_zeusSelection.push_back(*closest);
        }
        s_zeusStatus = alreadySelected
                           ? "Moving " + std::to_string(s_zeusSelection.size()) + " selected Zeus object(s)."
                           : "Selected " + closest->className + ".";
    }
    else
    {
        s_zeusSelection.clear();
        s_zeusStatus = "No Zeus-spawned object under the cursor.";
    }
}

// The rectangle corners are framebuffer pixels from ZeusCursorPixel().
void SelectZeusInRect(float startX, float startY, float endX, float endY)
{
    PruneZeusRecords(s_zeusSpawned);
    const Camera* camera = GScene ? GScene->GetCamera() : nullptr;
    if (!camera)
        return;
    const float left = std::min(startX, endX);
    const float right = std::max(startX, endX);
    const float top = std::min(startY, endY);
    const float bottom = std::max(startY, endY);
    s_zeusSelection.clear();
    for (const auto& record : s_zeusSpawned)
    {
        ZeusPoint pixel;
        if (!ZeusProjectToPixel(record.object->Position(), pixel))
            continue;
        if (pixel.x >= left && pixel.x <= right && pixel.y >= top && pixel.y <= bottom)
            s_zeusSelection.push_back(record);
    }
    s_zeusStatus = "Selected " + std::to_string(s_zeusSelection.size()) + " Zeus object(s).";
}

void StabilizeZeusInfantry(Person* person, Vector3Val direction)
{
    if (!person)
        return;
    if (AIUnit* unit = person->Brain())
    {
        // Replan after a Zeus move so the Arcade mission does not retain a
        // stale move-function queue. Target acquisition remains enabled:
        // opposing Zeus-spawned sides are expected to detect and engage.
        unit->ForceReplan(true);
        unit->SetAIDisabled(AIUnit::DAMove);
        unit->SetWatchDirection(direction);
    }
}

void RotateZeusSelectionBy(float degrees)
{
    PruneZeusRecords(s_zeusSelection);
    for (const auto& record : s_zeusSelection)
    {
        Matrix4 transform = record.object->Transform();
        transform.SetOrientation(Matrix3(MRotationY, degrees * (H_PI / 180.0f)) * transform.Orientation());
        record.object->MoveNetAware(transform);
        StabilizeZeusInfantry(dyn_cast<Person>(record.object.GetLink()), transform.Direction());
    }
}

void BeginZeusMoveDrag()
{
    PruneZeusRecords(s_zeusSelection);
    s_zeusMoveOffsets.clear();
    if (s_zeusSelection.empty())
        return;
    const Vector3 anchor = s_zeusSelection.front().object->Position();
    for (const auto& record : s_zeusSelection)
        s_zeusMoveOffsets.push_back(record.object->Position() - anchor);
    s_zeusMoveDrag = true;
    s_zeusStatus = "Drag to move the selected Zeus object(s).";
}

// pixelX/pixelY are framebuffer pixels from ZeusCursorPixel().
void MoveZeusSelectionAtPixel(float pixelX, float pixelY)
{
    Vector3 target;
    if (!ZeusClickPositionAtPixel(target, pixelX, pixelY))
        return;
    PruneZeusRecords(s_zeusSelection);
    for (int i = 0; i < static_cast<int>(s_zeusSelection.size()) && i < static_cast<int>(s_zeusMoveOffsets.size()); ++i)
    {
        const auto& record = s_zeusSelection[i];
        Matrix4 transform = record.object->Transform();
        Vector3 position = target + s_zeusMoveOffsets[i];
        position[1] = GLandscape->RoadSurfaceYAboveWater(position[0], position[2]);
        transform.SetPosition(position);
        if (Person* person = dyn_cast<Person>(record.object.GetLink()))
        {
            StabilizeZeusInfantry(person, transform.Direction());
            Vector3 normal;
            EntityAI* entity = dyn_cast<EntityAI>(record.object.GetLink());
            if (entity && AIUnit::FindFreePosition(position, normal, true, entity))
                transform.SetPosition(position);
            person->PlaceOnSurface(transform);
            person->SetTransform(transform);
        }
        else
            record.object->MoveNetAware(transform);
    }
}

void MoveZeusSelectionVertical(float deltaY)
{
    PruneZeusRecords(s_zeusSelection);
    if (s_zeusSelection.empty() || !GLandscape)
        return;

    int moved = 0;
    for (const auto& record : s_zeusSelection)
    {
        Matrix4 transform = record.object->Transform();
        Vector3 position = transform.Position();
        const float groundY = GLandscape->RoadSurfaceYAboveWater(position[0], position[2]);
        position[1] = std::max(groundY, position[1] + deltaY);
        transform.SetPosition(position);

        if (Person* person = dyn_cast<Person>(record.object.GetLink()))
        {
            // Do not call PlaceOnSurface here: Game Master elevation is an
            // explicit vertical transform, while terrain placement should
            // remain reserved for spawn and horizontal drag/drop.
            person->SetTransform(transform);
            StabilizeZeusInfantry(person, transform.Direction());
        }
        else
        {
            record.object->MoveNetAware(transform);
        }
        ++moved;
    }
    s_zeusStatus = "Raised/lowered " + std::to_string(moved) + " Zeus object(s).";
}

void RotateZeusSelection()
{
    PruneZeusRecords(s_zeusSelection);
    for (const auto& record : s_zeusSelection)
    {
        Matrix4 transform = record.object->Transform();
        transform.SetOrientation(Matrix3(MRotationY, s_zeusHeading * (H_PI / 180.0f)));
        record.object->MoveNetAware(transform);
    }
    s_zeusStatus = "Rotated " + std::to_string(s_zeusSelection.size()) + " selected Zeus object(s).";
}

void DeleteZeusSelection()
{
    PruneZeusRecords(s_zeusSelection);
    for (const auto& record : s_zeusSelection)
    {
        // AIUnit owns links from its group and sensor to the Person.  Removing
        // only the vehicle leaves a live AIUnit with no Person, which crashes
        // on the next AI think.
        if (Person* person = dyn_cast<Person>(record.object.GetLink()))
        {
            if (AIUnit* unit = person->Brain())
                unit->DestroyObject();
        }
        record.object->SetDelete();
    }
    const int count = static_cast<int>(s_zeusSelection.size());
    s_zeusSelection.clear();
    s_zeusStatus = "Deleted " + std::to_string(count) + " selected Zeus object(s).";
}

bool SpawnZeusVehicle(const char* className, Vector3Par position, float heading)
{
    Ref<Entity> vehicle = NewNonAIVehicle(className);
    if (!vehicle)
        return false;

    EntityAI* aiVehicle = dyn_cast<EntityAI>(vehicle.GetRef());
    Matrix4 transform;
    transform.SetPosition(position);
    transform.SetOrientation(Matrix3(MRotationY, heading * (H_PI / 180.0f)));
    if (aiVehicle)
        aiVehicle->PlaceOnSurface(transform);
    vehicle->SetTransform(transform);
    vehicle->Init(transform);

    if (aiVehicle)
    {
        if (aiVehicle->GetNonAIType()->IsKindOf(GWorld->Preloaded(VTypeStatic)))
        {
            GWorld->AddBuilding(vehicle);
            if (GWorld->GetMode() == GModeNetware)
                GetNetworkManager().CreateVehicle(vehicle, VLTBuilding, "", -1);
        }
        else
        {
            GWorld->AddVehicle(vehicle);
            if (GWorld->GetMode() == GModeNetware)
                GetNetworkManager().CreateVehicle(vehicle, VLTVehicle, "", -1);
        }
    }
    else
    {
        GWorld->AddAnimal(vehicle);
        if (GWorld->GetMode() == GModeNetware)
            GetNetworkManager().CreateVehicle(vehicle, VLTAnimal, "", -1);
    }

    RememberZeusSpawn(vehicle, className, 1, TLogic);

    return true;
}

bool SpawnZeusUnit(const char* className, TargetSide side, Vector3Par position, float heading)
{
    AICenter* center = GWorld->GetCenter(side);
    if (!center)
        center = GWorld->CreateCenter(side);
    if (!center || center->NGroups() >= MaxGroups)
        return false;

    // A Zeus unit must never be a hostile target to an AI centre of its own
    // side.  This is normally established by mission loading, but also makes
    // runtime-spawned units safe when a centre was created after the mission.
    // Do not alter cross-side relationships: authored mission alliances stay
    // authoritative.
    center->SetFriendship(side, 1.0f);

    Ref<EntityAI> vehicle = NewVehicle(className);
    Person* soldier = dyn_cast<Person>(vehicle.GetRef());
    if (!soldier)
        return false;

    Ref<AIGroup> group = new AIGroup();
    center->AddGroup(group);
    group->AddFirstWaypoint(position);
    Mission mission;
    mission._action = Mission::Arcade;
    center->SendMission(group, mission);
    if (GWorld->GetMode() == GModeNetware)
        GetNetworkManager().CreateObject(group);

    // Follow the regular createUnit path: find a collision-free spot before
    // initializing the soldier, which prevents invalid move/action state.
    Vector3 normal;
    Vector3 safePosition = position;
    if (AIUnit::FindFreePosition(safePosition, normal, true, vehicle))
    {
        float dx, dz;
        safePosition[1] = GLandscape->RoadSurfaceYAboveWater(safePosition[0], safePosition[2], &dx, &dz);
    }

    Matrix4 transform;
    transform.SetPosition(safePosition);
    transform.SetOrientation(Matrix3(MRotationY, heading * (H_PI / 180.0f)));
    vehicle->PlaceOnSurface(transform);
    vehicle->SetTransform(transform);
    vehicle->Init(transform);
    vehicle->SetTargetSide(side);
    GWorld->AddVehicle(vehicle);
    if (GWorld->GetMode() == GModeNetware)
        GetNetworkManager().CreateVehicle(vehicle, VLTVehicle, "", -1);
    GWorld->AddSensor(soldier);

    AIUnit* unit = soldier->Brain();
    if (!unit)
        return false;
    unit->Load(center->NextSoldierIdentity(soldier->IsWoman()));
    AIUnitInfo& aiInfo = soldier->GetInfo();
    aiInfo._rank = RankPrivate;
    aiInfo._initExperience = aiInfo._experience = AI::ExpForRank(RankPrivate);
    unit->SetAbility(0.5f);
    group->AddUnit(unit);
    if (GWorld->GetMode() == GModeNetware)
    {
        // Adding the unit may create the subgroup. Register the actual
        // resulting object, rather than the pre-add null pointer, so Zeus
        // units have the same network ownership chain as regular units.
        if (AISubgroup* subgroup = group->MainSubgroup())
            GetNetworkManager().CreateObject(subgroup);
        GetNetworkManager().CreateObject(unit);
    }
    if (!group->Leader())
        center->SelectLeader(group);
    StabilizeZeusInfantry(soldier, transform.Direction());
    RememberZeusSpawn(vehicle, className, 0, side);
    return true;
}

void SpawnZeusSelection()
{
    if (!ZeusWorldAvailable())
    {
        s_zeusStatus = "Spawning requires a loaded world.";
        return;
    }
    if (s_zeusClassName[0] == '\0')
    {
        s_zeusStatus = "Enter a config class name first.";
        return;
    }

    const int count = std::clamp(s_zeusCount, 1, 32);
    Vector3 anchor;
    const bool cursorAnchor = ZeusClickPosition(anchor);
    int spawned = 0;
    for (int i = 0; i < count; ++i)
    {
        // Preserve the former camera-forward placement only if the cursor ray
        // has no terrain/sea intersection (for example, when pointed beyond
        // the map); normal Zeus spawning is cursor-anchored.
        const Vector3 position =
            cursorAnchor ? ZeusSpawnPositionFromAnchor(anchor, i, count) : ZeusSpawnPosition(i, count);
        const bool didSpawn = s_zeusSpawnKind == 0
                                  ? SpawnZeusUnit(s_zeusClassName, kZeusSides[s_zeusSide], position, s_zeusHeading)
                                  : SpawnZeusVehicle(s_zeusClassName, position, s_zeusHeading);
        spawned += didSpawn ? 1 : 0;
    }
    s_zeusStatus = "Spawned " + std::to_string(spawned) + " / " + std::to_string(count) + " " + s_zeusClassName + ".";
}

void SpawnZeusAtClick()
{
    Vector3 position;
    if (!ZeusClickPosition(position))
    {
        s_zeusStatus = "No terrain was under the Zeus crosshair.";
        return;
    }
    const bool spawned = s_zeusSpawnKind == 0
                             ? SpawnZeusUnit(s_zeusClassName, kZeusSides[s_zeusSide], position, s_zeusHeading)
                             : SpawnZeusVehicle(s_zeusClassName, position, s_zeusHeading);
    s_zeusStatus = spawned ? "Placed " + std::string(s_zeusClassName) + "."
                           : "Could not place " + std::string(s_zeusClassName) + ".";
}

void PasteZeusAtCursor()
{
    Vector3 position;
    if (!ZeusClickPosition(position))
    {
        s_zeusStatus = "No terrain was under the Zeus cursor.";
        return;
    }
    int pasted = 0;
    for (int i = 0; i < static_cast<int>(s_zeusClipboard.size()); ++i)
    {
        const auto& record = s_zeusClipboard[i];
        Vector3 pastePosition = position + s_zeusCamera->DirectionAside() * (static_cast<float>(i) * 4.0f);
        pastePosition[1] = GLandscape->RoadSurfaceYAboveWater(pastePosition[0], pastePosition[2]);
        const bool placed = record.kind == 0
                                ? SpawnZeusUnit(record.className.c_str(), record.side, pastePosition, s_zeusHeading)
                                : SpawnZeusVehicle(record.className.c_str(), pastePosition, s_zeusHeading);
        pasted += placed ? 1 : 0;
    }
    s_zeusStatus = "Pasted " + std::to_string(pasted) + " Zeus object(s).";
}

void DrawZeusInteractionOverlay()
{
    if (!s_zeusCamera || !GEngine || !GScene)
        return;

    const Camera* camera = GScene->GetCamera();
    if (!camera)
        return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (Dev::CaveEditor().enabled)
    {
        char text[256];
        const auto& cave = Dev::CaveEditor();
        std::snprintf(text,sizeof(text),"MAP EDITOR | %s %.2f x %.2f m | length %.1f m | LMB place | Wheel width | Shift+Wheel length",
                      Dev::ExcavationToolName(),cave.width,cave.height,cave.length);
        draw->AddRectFilled(ImVec2(12,64),ImVec2(12+ImGui::CalcTextSize(text).x+16,92),IM_COL32(0,0,0,190));
        draw->AddText(ImVec2(20,70),IM_COL32(255,230,150,255),text);
        draw->AddText(ImVec2(20,98),IM_COL32(255,230,150,255),Dev::EditorCaveStatus());
    }
    if (Dev::TerrainBrush().enabled)
    {
        char text[192];
        const auto& brush = Dev::TerrainBrush();
        const bool raise = brush.raise != ((SDL_GetModState() & SDL_KMOD_LALT) != 0);
        std::snprintf(text, sizeof(text), "MAP EDITOR | %s | radius %.1f m | %.1f m/s | LMB paint | Wheel radius | Shift faster | Left Alt reverse",
                      raise ? "RAISE" : "LOWER", brush.radius, brush.metresPerSecond);
        draw->AddRectFilled(ImVec2(12, 64), ImVec2(12 + ImGui::CalcTextSize(text).x + 16, 92), IM_COL32(0,0,0,190));
        draw->AddText(ImVec2(20, 70), IM_COL32(255,230,150,255), text);
        if (brush.showRadius)
        {
            const ZeusPoint cursor = ZeusCursorPixel();
            Dev::TerrainBrushTarget target;
            if (Dev::TerrainBrushTargetAtPixel(cursor.x, cursor.y, target))
            {
                const ImU32 colour = !target.editable ? IM_COL32(255,85,85,255) :
                    raise ? IM_COL32(100,255,150,255) : IM_COL32(255,195,65,255);
                const float edge = (GLandscape->GetTerrainRange()-1)*GLandscape->GetTerrainGrid();
                ZeusPoint previous;
                bool previousValid = false;
                // One pick ray and a bounded terrain-following ring, not one ray per segment.
                for (int i = 0; i <= 96; ++i)
                {
                    const float angle = float(i) * (6.28318530718f / 96.0f);
                    const float x = target.x + std::cos(angle)*target.radius;
                    const float z = target.z + std::sin(angle)*target.radius;
                    ZeusPoint pixel;
                    const bool valid = x >= 0 && z >= 0 && x <= edge && z <= edge &&
                        ZeusProjectToPixel(Vector3(x, GLandscape->SurfaceY(x,z)+0.15f, z), pixel);
                    if (valid && previousValid)
                    {
                        const ImVec2 a = ZeusPixelToImGui(previous.x,previous.y);
                        const ImVec2 b = ZeusPixelToImGui(pixel.x,pixel.y);
                        draw->AddLine(a,b,IM_COL32(0,0,0,210),4.0f);
                        draw->AddLine(a,b,colour,2.0f);
                    }
                    previous = pixel;
                    previousValid = valid;
                }
                const ImVec2 centre = ZeusPixelToImGui(cursor.x,cursor.y);
                draw->AddCircle(centre,4.0f,colour,12,2.0f);
                std::snprintf(text,sizeof(text),"Radius %.1f m%s",target.radius,
                              target.editable ? "" : " | not editable here");
                draw->AddText(ImVec2(centre.x+13,centre.y+15),IM_COL32(0,0,0,255),text);
                draw->AddText(ImVec2(centre.x+12,centre.y+14),colour,text);
            }
        }
    }
    if (s_zeusLassoDrag)
    {
        // Track the engine cursor per frame rather than per SDL motion event:
        // the engine cursor is integrated once per input tick, so the last
        // event of a frame can still carry the previous tick's position.
        const ZeusPoint cursor = ZeusCursorPixel();
        s_zeusLassoEndX = cursor.x;
        s_zeusLassoEndY = cursor.y;
        const ImVec2 min = ZeusPixelToImGui(std::min(s_zeusLassoStartX, s_zeusLassoEndX),
                                            std::min(s_zeusLassoStartY, s_zeusLassoEndY));
        const ImVec2 max = ZeusPixelToImGui(std::max(s_zeusLassoStartX, s_zeusLassoEndX),
                                            std::max(s_zeusLassoStartY, s_zeusLassoEndY));
        draw->AddRectFilled(min, max, IM_COL32(80, 220, 255, 40));
        draw->AddRect(min, max, IM_COL32(80, 220, 255, 255), 0.0f, 0, 1.5f);
    }
    PruneZeusRecords(s_zeusSelection);
    for (const auto& record : s_zeusSelection)
    {
        ZeusPoint pixel;
        if (!ZeusProjectToPixel(record.object->Position(), pixel))
            continue;
        const ImVec2 center = ZeusPixelToImGui(pixel.x, pixel.y);
        const ImU32 selectionColor = IM_COL32(80, 220, 255, 255);
        draw->AddCircle(center, 20.0f, selectionColor, 20, 2.0f);
        draw->AddLine(center, ImVec2(center.x + 30.0f, center.y), selectionColor, 2.0f);
        draw->AddTriangleFilled(ImVec2(center.x + 35.0f, center.y), ImVec2(center.x + 27.0f, center.y - 5.0f),
                                ImVec2(center.x + 27.0f, center.y + 5.0f), selectionColor);
    }
}

// Live weather and clock control for the Zeus tab.
//
// Everything here goes through DebugCheats rather than touching World or
// Landscape directly, so the dev panel, the console commands and the tri
// harness all drive the same code path. The sliders read back from the engine
// every frame while they are not being dragged, so a value changed by a
// mission script or by the console shows up here instead of the panel showing
// a stale local copy.
void DrawZeusWeatherAndTime(bool worldAvailable)
{
    // Engine-authored values, refreshed from the landscape unless the user is
    // mid-drag on the corresponding widget.
    static float overcast = 0.0f;
    static float fog = 0.0f;
    static float transition = 0.0f;
    static float hour = 12.0f;
    static bool editingOvercast = false;
    static bool editingFog = false;
    static bool editingHour = false;

    // Both read back from the engine, so a value changed by a mission script or the
    // console shows up here instead of the panel holding a stale local copy.
    const Landscape* land = GLandscape;
    if (land != nullptr)
    {
        if (!editingOvercast)
            overcast = land->GetOvercast();
        if (!editingFog)
            fog = land->GetFog();
    }
    if (!editingHour)
        hour = Glob.clock.GetTimeOfDay() * 24.0f;

    if (!ImGui::CollapsingHeader("Weather and time##zeus"))
    {
        // Collapsed: no widget ran this frame, so nothing can still be mid-drag.
        editingOvercast = false;
        editingFog = false;
        editingHour = false;
        return;
    }

    const bool available = worldAvailable && DebugCheats::Cmd_SetWeather::Available();
    ImGui::BeginDisabled(!available);

    std::string out;
    bool applyWeather = false;
    applyWeather |= Dev::SliderFloat("Overcast##zeus", &overcast, 0.0f, 1.0f, "%.2f");
    editingOvercast = ImGui::IsItemActive();
    Dev::PanelTooltip("0 = clear, 1 = fully overcast. Drives cloud cover and the sky's light.");

    applyWeather |= Dev::SliderFloat("Fog##zeus", &fog, 0.0f, 1.0f, "%.2f");
    editingFog = ImGui::IsItemActive();
    Dev::PanelTooltip("0 = clear, 1 = thickest. Independent of overcast — the console 'weather' "
                      "command and triCheatWeather both force this to 0, this slider does not.");

    Dev::SliderFloat("Transition (s)##zeus", &transition, 0.0f, 600.0f, "%.0f");
    Dev::PanelTooltip("0 applies the change on the next frame. Anything higher lets the engine "
                      "interpolate, which is what you want while someone is watching.");

    if (applyWeather)
    {
        DebugCheats::Cmd_SetWeather::InvokeWeather(overcast, fog, transition, out);
        s_zeusStatus = out;
    }

    // Presets are the common case: nobody wants to hunt for 0.75 on a slider.
    Dev::PanelHelp("  Presets");
    Dev::PanelSameLine();
    const struct
    {
        const char* label;
        float overcast;
        float fog;
    } presets[] = {
        {"Clear", 0.0f, 0.0f},  {"Cloudy", 0.5f, 0.05f}, {"Overcast", 0.85f, 0.10f},
        {"Storm", 1.0f, 0.25f}, {"Foggy", 0.3f, 0.8f},
    };
    ImGui::PushID("zeus_weather_presets");
    for (const auto& preset : presets)
    {
        if (Dev::SmallButton(preset.label))
        {
            overcast = preset.overcast;
            fog = preset.fog;
            DebugCheats::Cmd_SetWeather::InvokeWeather(overcast, fog, transition, out);
            s_zeusStatus = out;
        }
        Dev::PanelSameLine();
    }
    ImGui::PopID();
    ImGui::NewLine();

    ImGui::Spacing();
    // The clock is display-only until released: the engine only offers a
    // relative skip, so applying every frame of a drag would fire dozens of
    // skips and sail past the target.
    Dev::SliderFloat("Time of day##zeus", &hour, 0.0f, 24.0f, "%05.2f h");
    const bool hourActive = ImGui::IsItemActive();
    const bool hourReleased = editingHour && !hourActive;
    editingHour = hourActive;
    Dev::PanelTooltip("Applied when you release the slider, not while dragging. The engine only "
                      "offers a relative skip, so the clock moves FORWARD to the requested hour "
                      "— asking for an earlier time wraps through midnight.");
    if (hourReleased && DebugCheats::Cmd_SetTimeOfDay::Available())
    {
        DebugCheats::Cmd_SetTimeOfDay::InvokeHour(hour, out);
        s_zeusStatus = out;
    }

    Dev::PanelHelp("  Jump to");
    Dev::PanelSameLine();
    const struct
    {
        const char* label;
        float hour;
    } times[] = {
        {"Dawn", 5.5f}, {"Morning", 9.0f}, {"Noon", 12.0f}, {"Dusk", 19.0f}, {"Night", 23.0f},
    };
    ImGui::PushID("zeus_time_presets");
    for (const auto& time : times)
    {
        if (Dev::SmallButton(time.label))
        {
            hour = time.hour;
            DebugCheats::Cmd_SetTimeOfDay::InvokeHour(hour, out);
            s_zeusStatus = out;
        }
        Dev::PanelSameLine();
    }
    ImGui::PopID();
    ImGui::NewLine();

    float multiplier = DebugCheats::Cmd_TimeMultiplier::Get();
    if (Dev::SliderFloat("Time scale##zeus", &multiplier, 0.1f, 60.0f, "%.1fx", ImGuiSliderFlags_Logarithmic))
    {
        DebugCheats::Cmd_TimeMultiplier::SetValue(multiplier, out);
        s_zeusStatus = out;
    }
    Dev::PanelTooltip("How fast the world clock runs. The engine clamps this to its own range, "
                      "and the value shown is read back from the engine, so what you see is what "
                      "it accepted.");

    ImGui::EndDisabled();
}

// ZEUS-THROW: launch an ammo class from the scene camera -- the free-fly camera when Zeus
// is active, otherwise whatever the player looks through. The recipe is the engine's own
// FireMissile: NewNonAIVehicle resolves the AmmoType by config name and NewShot builds the
// right Shot subclass (shell, missile, rocket, bomb...), then orient, speed, position and
// AddFastVehicle. No parent: the shot has no owner, exactly like a scripted createVehicle.
namespace
{
struct ZeusThrowPreset
{
    const char* label;
    const char* className;
    float       speed; // m/s along the view direction
    float       loft;  // m/s added straight up (a lob for hand grenades)
};
const ZeusThrowPreset kZeusThrowPresets[] = {
    {"Hand grenade", "GrenadeHand", 18.0f, 4.0f}, {"40 mm grenade", "Grenade", 70.0f, 0.0f},
    {"LAW rocket", "AT4", 120.0f, 0.0f},          {"Hellfire", "Hellfire", 150.0f, 0.0f},
    {"Zuni rocket", "Zuni", 200.0f, 0.0f},        {"Maverick", "Maverick", 200.0f, 0.0f},
    {"73 mm shell", "Shell73", 500.0f, 0.0f},     {"120 mm shell", "Shell120", 800.0f, 0.0f},
    {"Bomb", "Bomb", 5.0f, 0.0f},
};
int   s_zeusThrowPreset = 0;
char  s_zeusThrowClass[64] = "GrenadeHand";
float s_zeusThrowSpeed = 18.0f;
float s_zeusThrowLoft = 4.0f;
bool  s_zeusThrowHotkey = true;
int   s_zeusThrowCount = 0;

// LGT-013: a head torch on the camera. The owner asked for one, and it is also the rig this
// whole feature was missing: a light you AIM, that goes where you look, in free-fly as well
// as on foot -- so a shadow can be put where it can actually be judged instead of hoping a
// street lamp stands in a useful place.
//
// A cone, not a point: only a cone needs one shadow map instead of six, so a point-light
// torch could not cast at all. It follows the SCENE camera (GScene->GetCamera()), which is
// what is actually being rendered in every mode -- CameraOn() is the entity the view is
// attached to, and in free-fly that is the player standing wherever they were left.
// LGT-017: seeded from the environment so a capture can turn the torch on. It is a
// camera-following light, which makes it the only way to aim a beam at a chosen wall from a
// scripted pose -- without this the torch could only ever be judged by hand.
bool s_headlampOn = [] { const char* e = std::getenv("POSEIDON_HEADLAMP"); return e && std::atoi(e) != 0; }();
// LGT-020: STRENGTH, a plain multiplier on the emitted colour. It used to be fed to
// SetBrightness, which is a reach in disguise -- so turning the torch up made it carry across
// a whole village instead of making it brighter.
float s_headlampBrightness = [] {
    const char* e = std::getenv("POSEIDON_HEADLAMP_BRIGHTNESS");
    const double d = e ? std::atof(e) : 0.0;
    return d > 0.0 ? static_cast<float>(d) : 1.0f;
}();
// LGT-020: REACH, in metres, and it is a torch: "ist ja nur eine kleine taschenlampe". This is
// the flat-core radius; the shader keeps full strength inside it and falls off inverse-square
// after, so what the beam lands on is as bright indoors as out -- a wall at 3 m and a wall at
// 8 m both sit inside the core. That is the "so hell drinnen wie draussen" half of the ask;
// what stops it lighting the next village is the range, not the strength.
float s_headlampRange = [] {
    const char* e = std::getenv("POSEIDON_HEADLAMP_RANGE");
    const double d = e ? std::atof(e) : 0.0;
    return d > 0.0 ? static_cast<float>(d) : 9.0f;
}();
float s_headlampCone = [] {
    const char* e = std::getenv("POSEIDON_HEADLAMP_CONE");
    const double d = e ? std::atof(e) : 0.0;
    return d > 0.0 ? static_cast<float>(d) : 0.55f;
}();
float s_headlampColor[3] = {1.00f, 0.96f, 0.88f};
Ref<LightReflector> s_headlamp;

void UpdateHeadlamp()
{
    if (GScene == nullptr)
    {
        return;
    }
    if (!s_headlampOn)
    {
        if (s_headlamp)
        {
            // Left in the scene's list but black: removing and re-adding a light per toggle
            // is how a light list gets corrupted, and a black light costs nothing.
            s_headlamp->SetDiffuse(HBlack);
            s_headlamp->SetAmbient(HBlack);
        }
        return;
    }
    const Camera* cam = GScene->GetCamera();
    if (cam == nullptr)
    {
        return;
    }
    if (!s_headlamp)
    {
        s_headlamp = new LightReflector(GScene->Preloaded(HalfLight), HBlack, HBlack, s_headlampCone);
        s_headlamp->SetDrawHalo(false); // LGT-017: the viewer's own bulb is in the viewer's eye
        GScene->AddLight(s_headlamp);
    }
    const Color diffuse(s_headlampColor[0], s_headlampColor[1], s_headlampColor[2], 1.0f);
    s_headlamp->SetDiffuse(diffuse * s_headlampBrightness);
    s_headlamp->SetAmbient(diffuse * (0.04f * s_headlampBrightness));
    s_headlamp->SetRange(s_headlampRange); // LGT-020: reach, not strength
    s_headlamp->SetAngle(s_headlampCone);
    // A little ahead of the eye so the camera's own near plane cannot sit inside the lamp.
    const Vector3 dir = cam->Direction();
    s_headlamp->SetPosition(cam->Position() + dir * 0.35f);
    const Vector3 up = (std::fabs(dir.Y()) > 0.99f) ? Vector3(0, 0, 1) : Vector3(0, 1, 0);
    s_headlamp->SetOrient(dir, up);
}
// Harness seed: POSEIDON_ZEUS_THROW=<class>[:<speed>[:<loft>]] presets the throw so --auto-keys
// can fire a chosen ammo without a hand on the tab.
const bool s_zeusThrowSeeded = []
{
    const char* v = std::getenv("POSEIDON_ZEUS_THROW");
    if (!v || !*v)
        return false;
    std::string spec(v);
    const size_t c1 = spec.find(':');
    std::snprintf(s_zeusThrowClass, sizeof(s_zeusThrowClass), "%s", spec.substr(0, c1).c_str());
    if (c1 != std::string::npos)
    {
        const size_t c2 = spec.find(':', c1 + 1);
        s_zeusThrowSpeed = static_cast<float>(std::atof(spec.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1).c_str()));
        s_zeusThrowLoft = c2 == std::string::npos ? 0.0f : static_cast<float>(std::atof(spec.substr(c2 + 1).c_str()));
    }
    return true;
}();

void ZeusThrow()
{
    if (MultiplayerSession())
        return;
    if (!GWorld || !GScene || !GLandscape || GLandscape->GetLandRange() <= 0)
    {
        s_zeusStatus = "Throw needs a loaded world.";
        return;
    }
    const Camera* camera = GScene->GetCamera();
    if (!camera)
        return;
    Entity* entity = NewNonAIVehicle(RString(s_zeusThrowClass));
    Shot* shot = entity ? dyn_cast<Shot>(entity) : nullptr;
    if (!shot)
    {
        if (entity)
            entity->SetDelete();
        s_zeusStatus = std::string("'") + s_zeusThrowClass + "' is not an ammo class (CfgAmmo).";
        return;
    }
    const Vector3 forward = camera->Direction().Normalized();
    const Vector3 origin = camera->Position() + forward * 2.0f;
    shot->SetOrient(forward, VUp);
    shot->SetSpeed(forward * s_zeusThrowSpeed + VUp * s_zeusThrowLoft);
    shot->SetPosition(origin);
    GWorld->AddFastVehicle(shot);
    ++s_zeusThrowCount;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Threw %s #%d at %.0f m/s from [%.1f %.1f %.1f].", s_zeusThrowClass,
                  s_zeusThrowCount, s_zeusThrowSpeed, origin[0], origin[2], origin[1]);
    s_zeusStatus = buf;
    LOG_INFO(World, "ZEUS-THROW: {}", buf);
}

} // namespace

void DrawZeusTab()
{
    const bool worldAvailable = ZeusWorldAvailable();
    Dev::PanelHeading("Zeus Mode");
    Dev::PanelSameLine();
    Dev::PanelHelp("native free-fly camera and live spawning");
    Dev::PanelSeparator();

    // The tab is a long wall of controls, so every group below is a collapsing
    // section and only the two the owner reaches for first open by default.
    // Nothing here changes what a control does -- this is grouping only.
    if (ImGui::CollapsingHeader("Free-fly camera##zeus", ImGuiTreeNodeFlags_DefaultOpen))
    {
    ImGui::BeginDisabled(!worldAvailable);
    if (!s_zeusCamera)
    {
        if (Dev::Button("Enable free-fly"))
            EnableZeusCamera();
    }
    else
    {
        if (Dev::Button("Exit free-fly"))
            DisableZeusCamera();
        Dev::PanelSameLine();
        if (Dev::Button("Beam player here"))
            Defer([] { BeamPlayerToZeusCamera(); });
        Dev::PanelTooltip(
            "Places the real player at the current free-fly X/Z position on the terrain, then exits free-fly.");
    }
    ImGui::EndDisabled();
    if (Dev::PanelItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("WASD moves, Q/Z changes altitude, and the mouse looks around.\nKeypad +/- changes zoom.");
    }

    if (ImGui::CollapsingHeader("Player camera##zeus"))
    {
        // Third person (the Numpad-Enter "Person view" toggle) is not a
        // keybinding problem when it does nothing -- World.cpp forces the
        // camera back to CamInternal whenever the active difficulty profile
        // disables DT3rdPersonView. This flips that flag on the ACTIVE profile
        // (cadet or veteran, whichever the player runs), in memory only:
        // nothing here calls SaveToFile, so the player's UserInfo.cfg keeps
        // whatever they chose in Options. A Zeus takeover gives things back.
        UserConfig& uc = USER_CONFIG;
        const bool serverOwned = uc.HasServerDifficulty();
        ImGui::BeginDisabled(serverOwned);
        bool thirdPerson = uc.IsEnabled(DT3rdPersonView);
        if (Dev::Checkbox("Allow third person view", &thirdPerson))
        {
            bool* flags = uc.easyMode ? uc.cadetDifficulty : uc.veteranDifficulty;
            flags[DT3rdPersonView] = thirdPerson;
        }
        ImGui::EndDisabled();
        if (serverOwned)
            Dev::PanelTooltip("The server's difficulty override is active; third person is the server's call.");
        else
            Dev::PanelTooltip("Enables the Numpad-Enter person-view toggle even on a profile that forbids it.\n"
                              "Session-only: your saved Options difficulty is not touched.");

        // The toggle itself, for keyboards with no numpad (owner request: the
        // default Person-view binding is Numpad Enter and unrebindable from
        // here). Same code path as the key -- World::TogglePersonView -- so the
        // difficulty gate above still decides whether external view sticks.
        ImGui::BeginDisabled(!worldAvailable);
        if (Dev::Button("Toggle third person now"))
            Defer([] {
                if (GWorld)
                    GWorld->TogglePersonView();
            });
        ImGui::EndDisabled();
        Dev::PanelTooltip("Exactly what Numpad Enter does. If the checkbox above is off (or the server\n"
                          "forbids it), the camera snaps back to first person -- the gate wins, as with the key.");
    }

    if (ImGui::CollapsingHeader("Go to coordinates##zeus"))
    {
    ImGui::BeginDisabled(!worldAvailable);
    ImGui::SetNextItemWidth(-1.0f);
    // Enter in the box is the same as pressing the button: pasting a pose and hitting
    // return is the whole gesture this exists for.
    const bool submitted =
        Dev::InputTextWithHint("##zeusgoto", "paste a pose, e.g. 2721.96 944.30 99.33 306.9 -5.4", s_zeusGotoBuf,
                               sizeof(s_zeusGotoBuf), ImGuiInputTextFlags_EnterReturnsTrue);
    Dev::PanelTooltip("X Z ALTITUDE [azimuth elevation] -- the same order this panel's freefly readout prints,\n"
                      "so you can paste that line back in verbatim, brackets and labels and all.\n"
                      "Two numbers = X and Z at head height on the terrain.");
    if (Dev::Button("Go") || submitted)
        Defer([] { GoToZeusCoordinates(); });
    Dev::PanelSameLine();
    if (Dev::Button("Copy current pose"))
    {
        if (s_zeusCamera)
        {
            const Vector3 p = s_zeusCamera->Position();
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", p[0], p[2], p[1]);
            ImGui::SetClipboardText(buf);
            std::snprintf(s_zeusGotoBuf, sizeof(s_zeusGotoBuf), "%s", buf);
            s_zeusStatus = "Current position copied to the clipboard (X Z altitude).";
        }
        else
        {
            s_zeusStatus = "No free-fly camera active to copy a pose from.";
        }
    }
    Dev::PanelTooltip("Puts the current free-fly position on the clipboard in the order this box expects.");
    ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Throw from camera##zeus"))
    {
    ImGui::BeginDisabled(!worldAvailable);
    {
        const char* labels[IM_ARRAYSIZE(kZeusThrowPresets)];
        for (int i = 0; i < IM_ARRAYSIZE(kZeusThrowPresets); ++i)
            labels[i] = kZeusThrowPresets[i].label;
        ImGui::SetNextItemWidth(160.0f);
        if (Dev::Combo("Ammo", &s_zeusThrowPreset, labels, IM_ARRAYSIZE(kZeusThrowPresets)))
        {
            const ZeusThrowPreset& preset = kZeusThrowPresets[s_zeusThrowPreset];
            std::snprintf(s_zeusThrowClass, sizeof(s_zeusThrowClass), "%s", preset.className);
            s_zeusThrowSpeed = preset.speed;
            s_zeusThrowLoft = preset.loft;
        }
        Dev::PanelSameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::InputText("class##zeusthrow", s_zeusThrowClass, sizeof(s_zeusThrowClass));
        Dev::PanelTooltip("Any CfgAmmo class name: shells, missiles, rockets, bombs, grenades, mines.");
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Speed (m/s)", &s_zeusThrowSpeed, 1.0f, 1000.0f, "%.0f");
        Dev::PanelSameLine();
        ImGui::SetNextItemWidth(120.0f);
        Dev::SliderFloat("Loft (m/s up)", &s_zeusThrowLoft, 0.0f, 20.0f, "%.1f");
        if (Dev::Button("Throw"))
            ZeusThrow();
        Dev::PanelSameLine();
        Dev::Checkbox("Hotkey: G in free-fly, Ctrl+G always", &s_zeusThrowHotkey);
        Dev::PanelTooltip("Launches from the scene camera 2 m ahead, along the view direction, in free-fly\n"
                          "and in normal play alike. Missiles fly straight (no target); shells and bombs\n"
                          "follow ballistics; hand grenades fuse as in the game.");
    }
    ImGui::EndDisabled();
    }

    // LGT-013: the head torch.
    if (ImGui::CollapsingHeader("Head torch##zeus"))
    {
    ImGui::BeginDisabled(!worldAvailable);
    {
        if (Dev::Checkbox("Head torch (follows the view)", &s_headlampOn))
        {
            UpdateHeadlamp();
        }
        Dev::PanelTooltip("A cone light on the CAMERA, in free-fly as well as on foot. A cone and not\n"
                          "a point because only a cone needs one shadow map instead of six, so a point\n"
                          "torch could not cast a shadow at all.\n"
                          "\n"
                          "Shadows still need POSEIDON_LOCAL_SHADOWS=1; without it this lights the\n"
                          "world and occludes nothing, which is what every local light did before\n"
                          "LGT-010.");
        if (s_headlampOn)
        {
            ImGui::SetNextItemWidth(140.0f);
            Dev::SliderFloat("Torch strength", &s_headlampBrightness, 0.1f, 6.0f, "%.2f");
            Dev::PanelSameLine();
            ImGui::SetNextItemWidth(140.0f);
            Dev::SliderFloat("Torch cone", &s_headlampCone, 0.08f, 1.2f, "%.2f");
            ImGui::SetNextItemWidth(140.0f);
            Dev::SliderFloat("Torch reach m", &s_headlampRange, 2.0f, 60.0f, "%.0f");
            ImGui::SetNextItemWidth(160.0f);
            ImGui::ColorEdit3("Torch colour", s_headlampColor, ImGuiColorEditFlags_NoInputs);
        }

    }
    ImGui::EndDisabled();
    }

    // Tooltips kept to one line each on purpose: a C++ string literal containing a newline
    // escape, written through a script, is the edit that has broken this build four times.
    static const char* const kLampPoolTip =
        "The lamp's bright core, as a fraction of the authored radius. Small is what makes a pool instead of a "
        "floodlight -- and it is also why a town looks dark from the air, because reach falls as the square of it. "
        "The spill below is what carries the far field.";
    static const char* const kLampSpillTip =
        "A second, dim, wide light at the same bulb. It is what you see when you fly over a lit town; the core "
        "alone is almost nothing at 100 m and that is correct inverse-square behaviour, not a bug.";
    static const char* const kSpillAmbientTip =
        "The spill's direction-independent term. Zero keeps the spill directional, so it can be strong enough to "
        "carry the far field without lifting every wall and roof uniformly -- a flat lift is exactly what reads as "
        "floodlight rather than as street lamps.";
    static const char* const kBulbGlowTip =
        "POSEIDON_BULB_GLOW=0. The bulb's own appearance -- a peak-white core plus a soft halo "
        "sprite -- and nothing about the light it CASTS. A wgpu night frame has no value above "
        "the 1.0 bloom knee, so this halo is drawn, not bloomed; see LGT-025.";
    static const char* const kShadowCacheTip =
        "POSEIDON_LOCAL_SHADOW_CACHE=0. A view is re-rendered only when its light, its casters or the "
        "shadow settings change; a settled village re-uses all 24. Measured 2.90 ms -> 0.000, and the "
        "whole frame 13.38 -> 8.17 ms, because the per-view cull dispatches go with it.";
    static const char* const kShadowForceTip =
        "POSEIDON_LOCAL_SHADOW_FORCE_REFRESH=1. Re-renders every view every frame with the cache still "
        "on. This is the switch for telling a STALE shadow apart from a real one: if a shadow in the "
        "wrong place survives this, the cache is not what put it there.";
    static const char* const kLocalShadowTip =
        "Point lights get six shadow faces each, spots one; all of them are tiles in one depth layer. Four lights "
        "is twenty-four of the twenty-four views, so this is the whole budget.";

    // LGT-018: the street-lamp and local-shadow levers. These shipped as environment
    // variables only, which means the owner could not taste them -- and every one of them is
    // a taste call with a real trade-off behind it, so they belong on a slider.
    if (ImGui::CollapsingHeader("Street lamps and light shadows##zeus"))
    {
        if (Dev::Button("Reset lamps, glow and shadows"))
            Poseidon::Dev::ResetLightingSettings();
        Poseidon::Dev::LampLightSettings& lamp = Poseidon::Dev::GLampLightSettings();
        const float beforeRadius = lamp.radiusScale;
        const float beforeSpill = lamp.spill;
        const float beforeSpillR = lamp.spillRadius;
        const float beforeSpillA = lamp.spillAmbient;
        const float beforeBright = lamp.brightnessScale;
        const float beforeAmbient = lamp.ambientScale;
        bool colourChanged = Dev::Checkbox("Override lamp colour", &lamp.colorOverride);
        ImGui::BeginDisabled(!lamp.colorOverride);
        colourChanged |= Dev::Checkbox("Use colour temperature", &lamp.useColorTemperature);
        ImGui::BeginDisabled(!lamp.useColorTemperature);
        colourChanged |= Dev::SliderFloat("Lamp temperature (K)", &lamp.colorTemperature, 1000.0f, 12000.0f, "%.0f K");
        Dev::PanelTooltip("Approximate warm-to-cool lamp colour. Intensity is controlled separately. Disable temperature mode for custom RGB.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(lamp.useColorTemperature);
        colourChanged |= Dev::ColorEdit3("Lamp colour", lamp.color);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        Dev::PanelTooltip("Applies to existing street lamps immediately. Disable the override to use each lamp's authored colour.");
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Pool size", &lamp.radiusScale, 0.05f, 1.0f, "%.2f");
        Dev::PanelTooltip(kLampPoolTip);
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Spill strength", &lamp.spill, 0.0f, 1.0f, "%.2f");
        Dev::PanelTooltip(kLampSpillTip);
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Spill reach", &lamp.spillRadius, 1.0f, 12.0f, "%.2f");
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Spill ambient", &lamp.spillAmbient, 0.0f, 1.0f, "%.2f");
        Dev::PanelTooltip(kSpillAmbientTip);
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Lamp intensity", &lamp.brightnessScale, 0.0f, 8.0f, "%.2f");
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Lamp ambient", &lamp.ambientScale, 0.0f, 1.0f, "%.2f");
        if (colourChanged || lamp.radiusScale != beforeRadius || lamp.spill != beforeSpill || lamp.spillRadius != beforeSpillR ||
            lamp.spillAmbient != beforeSpillA || lamp.brightnessScale != beforeBright ||
            lamp.ambientScale != beforeAmbient)
        {
            // StreetLamp::CreateLight rebuilds a lamp whose generation moved; without this
            // the sliders do nothing until a lamp happens to be re-created.
            lamp.generation++;
        }

        ImGui::Separator();
        // LGT-025: how much a bulb GLOWS, as opposed to how much it lights. Both sliders
        // touch only the marker and its halo; the light a lamp or a headlight casts on the
        // ground is the Light object above and is not reachable from here.
        BulbGlowSettings& glow = GBulbGlow();
        Dev::Checkbox("Bulbs glow (LGT-025)", &glow.enabled);
        Dev::PanelTooltip(kBulbGlowTip);
        ImGui::BeginDisabled(!glow.enabled);
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Glow strength", &glow.strength, 0.0f, 8.0f, "%.2f");
        Dev::PanelTooltip("POSEIDON_BULB_GLOW_STRENGTH. Scales the halo's alpha cap "
                          "(1.0 = the old wgpu 0.10, hard ceiling 0.75).");
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Glow size", &glow.size, 0.0f, 16.0f, "%.2f");
        Dev::PanelTooltip("POSEIDON_BULB_GLOW_SIZE. Scales the halo's screen size "
                          "(1.0 = the old wgpu 0.030, about two pixels).");
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Glow off-beam", &glow.sideGlow, 0.0f, 1.0f, "%.2f");
        Dev::PanelTooltip("POSEIDON_BULB_GLOW_SIDE. How much halo a HEADLIGHT keeps when you "
                          "are not standing in its beam. 0 is the 2001 hard cut at 12 degrees, "
                          "which is why a parked car's lamps read as off from the side.");
        Dev::Checkbox("Bulb core at peak white", &glow.peakNormalised);
        Dev::PanelTooltip("POSEIDON_BULB_GLOW_PEAK=0 restores the unit-length colour "
                          "normalisation, which asks a warm bulb for ~66% of white.");
        ImGui::EndDisabled();

        ImGui::Separator();
        Poseidon::Dev::LocalShadowSettings& ls = Poseidon::Dev::GLocalShadowSettings();
        Dev::Checkbox("Local lights cast shadows", &ls.enabled);
        Dev::PanelTooltip(kLocalShadowTip);
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderInt("Shadowing lights", &ls.maxLights, 0, 4);
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Shadow darkness", &ls.darkness, 0.0f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Self radius m", &ls.selfRadius, 0.0f, 4.0f, "%.2f");
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Beam shape", &Poseidon::Dev::GSpotBeam().shape, 0.15f, 1.5f, "%.2f");
        // LGT-026: a street lamp never moves and neither does the house beside it, so its six
        // cube faces were being recomputed sixty times a second to produce an identical depth
        // map. Caching them takes the local-shadow pass from 2.90 ms to 0.000.
        Dev::Checkbox("Cache static light shadows", &ls.cacheStatic);
        Dev::PanelTooltip(kShadowCacheTip);
        Dev::Checkbox("Force shadow re-render", &ls.forceRefresh);
        Dev::PanelTooltip(kShadowForceTip);
    }

    // DST-003: how far a wrecked building comes down.
    if (ImGui::CollapsingHeader("Building destruction##zeus"))
    {
    ImGui::BeginDisabled(!worldAvailable);
    {
        DestructionLook& destruct = GDestructionLook();
        Dev::Checkbox("Buildings collapse further (DST-003)", &destruct.enabled);
        Dev::PanelTooltip("POSEIDON_DESTRUCT_LOOK=0 restores the 2001 collapse exactly. The destruction\n"
                          "squashes a model's vertices towards the ground, but the 2001 height term takes\n"
                          "at most 20% off, so a hit house shears and sags and then stands there. This\n"
                          "scales that term alone; the sideways scatter is left as authored, because\n"
                          "scaling THAT pulls the walls apart rather than down.\n"
                          "The destroyed shape is cached per model, so a change here applies to buildings\n"
                          "that have not been destroyed yet.");
        ImGui::SetNextItemWidth(160.0f);
        Dev::SliderFloat("Collapse x", &destruct.squash, 1.0f, 6.0f, "%.1f");
        Dev::PanelTooltip("POSEIDON_DESTRUCT_SQUASH. 1.0 is the authored 20% maximum height loss.");
    }
    ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Spawn at camera aim##zeus", ImGuiTreeNodeFlags_DefaultOpen))
    {
    ImGui::BeginDisabled(!worldAvailable);
    if (Dev::RadioButton("Unit", s_zeusSpawnKind == 0))
    {
        s_zeusSpawnKind = 0;
        s_zeusPreset = 0;
        SetZeusClassFromPreset();
    }
    Dev::PanelSameLine();
    if (Dev::RadioButton("Vehicle", s_zeusSpawnKind == 1))
    {
        s_zeusSpawnKind = 1;
        s_zeusPreset = 0;
        SetZeusClassFromPreset();
    }

    const auto presetName = [&]() -> const char*
    {
        return s_zeusSpawnKind == 0
                   ? kZeusUnitPresets[std::clamp(s_zeusPreset, 0, static_cast<int>(kZeusUnitPresets.size()) - 1)]
                   : kZeusVehiclePresets[std::clamp(s_zeusPreset, 0, static_cast<int>(kZeusVehiclePresets.size()) - 1)];
    };
    if (Dev::BeginCombo("Preset", presetName()))
    {
        const int count = s_zeusSpawnKind == 0 ? static_cast<int>(kZeusUnitPresets.size())
                                               : static_cast<int>(kZeusVehiclePresets.size());
        for (int i = 0; i < count; ++i)
        {
            const char* name = s_zeusSpawnKind == 0 ? kZeusUnitPresets[i] : kZeusVehiclePresets[i];
            if (ImGui::Selectable(name, s_zeusPreset == i))
            {
                s_zeusPreset = i;
                SetZeusClassFromPreset();
            }
        }
        ImGui::EndCombo();
    }
    if (s_zeusSpawnKind == 0)
        Dev::Combo("Side", &s_zeusSide, kZeusSideNames.data(), static_cast<int>(kZeusSideNames.size()));
    Dev::InputText("Config class", s_zeusClassName, sizeof(s_zeusClassName));
    Dev::PanelTooltip("Any loaded CfgVehicles class can be entered here. Presets use original CWA class names.");
    Dev::SliderFloat("Distance (m)", &s_zeusDistance, 2.0f, 250.0f, "%.0f");
    Dev::SliderFloat("Heading (deg)", &s_zeusHeading, 0.0f, 359.0f, "%.0f");
    Dev::PanelTooltip("Sets the facing direction before spawning. This is safer than changing a live AI unit.");
    Dev::SliderInt("Count", &s_zeusCount, 1, 32);
    if (Dev::Button("Spawn"))
        Defer([] { SpawnZeusSelection(); });
    Dev::PanelSameLine();
    ImGui::BeginDisabled(!s_zeusCamera);
    if (Dev::Button(s_zeusClickPlacement ? "Stop click placement" : "Place with clicks"))
    {
        s_zeusClickPlacement = !s_zeusClickPlacement;
        s_zeusStatus = s_zeusClickPlacement
                           ? "Click placement armed. Close the panel and left-click the crosshair target to place more."
                           : "Click placement stopped.";
        if (s_zeusClickPlacement && s_zeusCamera)
            SetVisible(false);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Selection and clipboard##zeus"))
    {
    ImGui::BeginDisabled(!s_zeusCamera);
    if (Dev::Button("Select under cursor"))
        Defer(
            []
            {
                const ZeusPoint cursor = ZeusCursorPixel();
                SelectZeusAtCursor(cursor.x, cursor.y);
            });
    Dev::PanelSameLine();
    if (Dev::Button("Select all Zeus objects"))
        Defer(
            []
            {
                PruneZeusRecords(s_zeusSpawned);
                s_zeusSelection = s_zeusSpawned;
                s_zeusStatus = "Selected " + std::to_string(s_zeusSelection.size()) + " Zeus object(s).";
            });
    Dev::PanelHelp("Selected: %d", static_cast<int>(s_zeusSelection.size()));
    Dev::PanelHelp("With a selection: wheel raises/lowers (Shift = 5m), Shift+drag rotates,");
    Dev::PanelHelp("Ctrl+C / Ctrl+V copies at the cursor, Delete removes.");
    ImGui::BeginDisabled(s_zeusSelection.empty());
    if (Dev::Button("Rotate selected to heading"))
        Defer([] { RotateZeusSelection(); });
    Dev::PanelSameLine();
    if (Dev::Button("Delete selected"))
        Defer([] { DeleteZeusSelection(); });
    if (Dev::Button("Copy selected"))
    {
        s_zeusClipboard = s_zeusSelection;
        s_zeusStatus = "Copied " + std::to_string(s_zeusClipboard.size()) + " Zeus object(s).";
    }
    Dev::PanelSameLine();
    ImGui::BeginDisabled(s_zeusClipboard.empty());
    if (Dev::Button("Paste at crosshair"))
        Defer(
            []
            {
                Vector3 position;
                if (!ZeusClickPosition(position))
                {
                    s_zeusStatus = "No terrain was under the Zeus crosshair.";
                    return;
                }
                int pasted = 0;
                for (int i = 0; i < static_cast<int>(s_zeusClipboard.size()); ++i)
                {
                    const auto& record = s_zeusClipboard[i];
                    Vector3 pastePosition = position + s_zeusCamera->DirectionAside() * (static_cast<float>(i) * 4.0f);
                    pastePosition[1] = GLandscape->RoadSurfaceYAboveWater(pastePosition[0], pastePosition[2]);
                    const bool placed =
                        record.kind == 0
                            ? SpawnZeusUnit(record.className.c_str(), record.side, pastePosition, s_zeusHeading)
                            : SpawnZeusVehicle(record.className.c_str(), pastePosition, s_zeusHeading);
                    pasted += placed ? 1 : 0;
                }
                s_zeusStatus = "Pasted " + std::to_string(pasted) + " Zeus object(s).";
            });
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    }

    DrawZeusWeatherAndTime(worldAvailable);

    if (!worldAvailable)
        Dev::PanelHelp("Load a mission or world to use Zeus.");
    if (!s_zeusStatus.empty())
    {
        Dev::PanelSeparator();
        ImGui::TextWrapped("%s", s_zeusStatus.c_str());
    }
}

// One mutable copy per (slot, role) shown by the tuner.  Pulled from the
// active mapping on first Render() and pushed back to font.cpp via
// SetFontMappingTuning on every slider change.
struct RoleEditState
{
    const char* prefix;
    const char* alias; // legacy alias prefix kept in sync (or nullptr)
    int renderPx;
    float widthScale;
    float baselineOffset;
    float syntheticBold;
    float letterSpacing;
};

struct TuningState
{
    bool loaded = false;
    RoleEditState roles[5]; // title, body, mono, serif, hand
};

TuningState s_tuning;
int s_currentRole = 0; // which face's tuning the panel is editing

static const char* const kRoleNames[5] = {"Title", "Body", "Mono", "Serif", "Hand"};
static const char* const kRolePrefixes[5] = {"cwrtitle", "cwrbody", "cwrmono", "cwrserif", "cwrhand"};
static const char* const kRoleAliases[5] = {"steelfishb", "tahomab", "couriernewb", "garamond", "audreyshand"};

void LoadTuningIfNeeded()
{
    if (s_tuning.loaded)
        return;
    // Parse the dump to fill renderPx / widthScale for each role.
    // Format per line: '  {"prefix", "ttfPath", maxH, renderPx, widthScalef, oblique},'
    const char* dump = DumpFontTable();
    for (int r = 0; r < 5; r++)
    {
        s_tuning.roles[r].prefix = kRolePrefixes[r];
        s_tuning.roles[r].alias = kRoleAliases[r];
        s_tuning.roles[r].renderPx = 24;
        s_tuning.roles[r].widthScale = 1.0f;
        s_tuning.roles[r].baselineOffset = 0.0f;
        s_tuning.roles[r].syntheticBold = 0.0f;
        s_tuning.roles[r].letterSpacing = 0.0f;
        char needle[64];
        snprintf(needle, sizeof(needle), "{\"%s\",", kRolePrefixes[r]);
        const char* p = strstr(dump, needle);
        if (!p)
            continue;
        // Skip past prefix + ttfPath + maxH by counting commas.  DumpFontTable
        // emits:  prefix, ttfPath, maxH, renderPx, widthScalef, obliqueBool,
        //         baselineOffsetf, syntheticBoldf, letterSpacingf
        int commas = 0;
        const char* q = p;
        while (*q && commas < 3)
        {
            if (*q == ',')
                commas++;
            q++;
        }
        // q now points just after the 3rd comma — next is space + renderPx
        int rpx = 0;
        float ws = 1.0f;
        char oblique[16] = "false";
        float baseline = 0.0f, bold = 0.0f, spacing = 0.0f;
        if (sscanf(q, " %d, %ff, %15[^,], %ff, %ff, %ff", &rpx, &ws, oblique, &baseline, &bold, &spacing) >= 2)
        {
            s_tuning.roles[r].renderPx = rpx;
            s_tuning.roles[r].widthScale = ws;
            s_tuning.roles[r].baselineOffset = baseline;
            s_tuning.roles[r].syntheticBold = bold;
            s_tuning.roles[r].letterSpacing = spacing;
        }
    }
    s_tuning.loaded = true;
}

void DrawFontTab()
{
    // ── Face picker ────────────────────────────────────────────────
    // A single shipping font set; this picks which face the
    // size/stretch/spacing sliders below tune.
    ImGui::Text("Face:");
    for (int r = 0; r < 5; r++)
    {
        if (r > 0)
            Dev::PanelSameLine();
        bool active = (s_currentRole == r);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
        if (Dev::Button(kRoleNames[r]))
            s_currentRole = r;
        if (active)
            ImGui::PopStyleColor();
    }
    Dev::PanelSeparator();

    // ── Sliders for the selected face ─────────────────────────────
    LoadTuningIfNeeded();
    auto& role = s_tuning.roles[s_currentRole];

    ImGui::Text("%s - %s (alias %s)", kRoleNames[s_currentRole], role.prefix, role.alias ? role.alias : "(none)");

    bool changed = false;
    changed |= Dev::SliderInt("renderPx", &role.renderPx, 8, 128);
    changed |= Dev::SliderFloat("widthScale", &role.widthScale, 0.3f, 1.6f, "%.3f");
    changed |= Dev::SliderFloat("baseline", &role.baselineOffset, -16.0f, 16.0f, "%.1f px");
    changed |= Dev::SliderFloat("bold", &role.syntheticBold, -2.0f, 4.0f, "%.1f px");
    changed |= Dev::SliderFloat("spacing", &role.letterSpacing, -4.0f, 8.0f, "%.1f px");
    if (changed)
    {
        SetFontMappingTuning(role.prefix, role.renderPx, role.widthScale, role.baselineOffset, role.syntheticBold,
                             role.letterSpacing, nullptr);
        if (role.alias)
            SetFontMappingTuning(role.alias, role.renderPx, role.widthScale, role.baselineOffset, role.syntheticBold,
                                 role.letterSpacing, nullptr);
    }
    Dev::PanelSeparator();

    if (Dev::Button("Dump font table to log"))
    {
        LOG_INFO(Graphics, "\n{}", DumpFontTable());
    }

    Dev::PanelSeparator();
    Dev::PanelHeading("License plates");
    LicensePlateTextTuning plate = GetLicensePlateTextTuning();
    bool plateChanged = false;
    plateChanged |= Dev::SliderFloat("plate width", &plate.widthScale, 0.30f, 1.20f, "%.3f");
    plateChanged |= Dev::SliderFloat("plate x offset", &plate.horizontalOffset, -5.00f, 1.00f, "%.2f em");
    plateChanged |= Dev::SliderFloat("plate y offset", &plate.verticalOffset, -1.00f, 2.00f, "%.2f em");
    plateChanged |= Dev::SliderFloat("plate surface offset", &plate.surfaceOffset, 0.000f, 0.050f, "%.3f m");
    plateChanged |= Dev::SliderFloat("plate softness", &plate.softness, 0.000f, 0.050f, "%.3f em");
    if (plateChanged)
        SetLicensePlateTextTuning(plate);
    Dev::PanelSameLine();
    if (Dev::Button("Reset plate"))
        ResetLicensePlateTextTuning();
    Dev::PanelHelp("  session-only override; defaults come from CfgLicensePlateText");
}

// Last command output for the Cheats tab — shown under the buttons so
// the user gets a visible confirmation that a click did something.
std::string s_cheatsStatus;

template <typename ClickFn>
void CheatButton(const char* label, bool enabled, const char* tooltip, ClickFn&& click)
{
    ImGui::BeginDisabled(!enabled);
    if (Dev::Button(label))
        click();
    ImGui::EndDisabled();
    if (tooltip && Dev::PanelItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
}

void DrawCheatsTab()
{
    Dev::PanelHeading("Mission");
    Dev::PanelSeparator();

    // One-button End Mission, matching the original OFP ENDMISSION word
    // cheat shape — no outcome picker; the cheat just wins the mission.
    // Power users who want a specific outcome use the `triEndMission
    // "lose"` / `endmission end3` / etc. paths through tri or the
    // console; Cmd_EndMission::Invoke keeps all the outcome strings.
    //
    // Re-queried every frame so the button greys instantly when there's
    // no mission running or the mission has already entered teardown.
    const bool canEnd = DebugCheats::Cmd_EndMission::Available();
    CheatButton("End Mission (win)", canEnd,
                canEnd ? "Force the active mission to win (sets EndMode=EMEnd1).\n"
                         "Matches the 1999 ENDMISSION word-cheat semantics.\n"
                         "Closes the dev panel afterwards — the engine starts\n"
                         "tearing down the world over the next several frames\n"
                         "and keeping any in-mission UI alive during that\n"
                         "transition risks crashes (textures get evicted while\n"
                         "we'd still be querying them).\n"
                         "Other outcomes still available via `triEndMission` /\n"
                         "the dev console (lose, killed, end1..end6)."
                       : "Requires an active mission that has not already ended.",
                []
                {
                    // Hide first (cheap, just sets a bool) — the deferred
                    // Invoke can then run after ImGui::Render with no
                    // panel-render side effects to worry about.
                    SetVisible(false);
                    Defer([] { DebugCheats::Cmd_EndMission::Invoke("win", s_cheatsStatus); });
                });

    ImGui::Spacing();
    Dev::PanelHeading("System");
    Dev::PanelSeparator();

    // Full in-process reload — re-mounts all banks/addons/config and rebuilds
    // the world on the same window (the mod "Apply" path).  Gated to outside a
    // mission: re-mounting mid-mission would evict assets the simulation still
    // references.  Hide the panel first, then run after ImGui::Render — the
    // reload tears down the very world/UI we'd otherwise be drawing this frame.
    const bool canReload = Poseidon::GApp != nullptr && Poseidon::GApp->m_canRender && GWorld != nullptr &&
                           GWorld->GetMode() == GModeIntro;
    CheatButton("Reload game", canReload,
                canReload ? "Reload all game content (mods + config) in place.\n"
                            "Keeps the window, shows the loading screen, and lands\n"
                            "back on a fresh main menu."
                          : "Available from the main menu (not during a mission).",
                []
                {
                    SetVisible(false);
                    // Queue the reload for the next AppIdle (before simulate/draw) rather than
                    // running it inside the swap — see RequestRemount / RequestDeferredReload.
                    if (Poseidon::GApp != nullptr)
                        Poseidon::GApp->RequestRemount();
                });

    CheatButton("Exit game", true, "Exits game.",
                []
                {
                    SetVisible(false);
                    // Queue the close
                    if (Poseidon::GApp != nullptr)
                        Poseidon::GApp->m_closeRequest = 1;
                });

    ImGui::Spacing();
    Dev::PanelHeading("Player");
    Dev::PanelSeparator();

    // God mode — sticky toggle.  Disabled state mirrors EndMission's
    // gating: needs a mission with a real player.  The toggle itself
    // is persisted by DebugCheats; we just round-trip the bool here.
    const bool canGod = DebugCheats::Cmd_God::Available();
    bool god = DebugCheats::Cmd_God::IsActive();
    ImGui::BeginDisabled(!canGod);
    if (Dev::Checkbox("God mode", &god))
        DebugCheats::Cmd_God::SetActive(god);
    ImGui::EndDisabled();
    if (Dev::PanelItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", canGod ? "Silently drop all damage applied to the real player.\n"
                                         "Hooked in Object::SetDammage so every weapon / explosion /\n"
                                         "fall is covered, not just script-driven damage."
                                       : "Requires an active mission.");

    // Infinite ammo — same gating shape as god mode.
    const bool canAmmo = DebugCheats::Cmd_InfiniteAmmo::Available();
    bool infammo = DebugCheats::Cmd_InfiniteAmmo::IsActive();
    ImGui::BeginDisabled(!canAmmo);
    if (Dev::Checkbox("Infinite ammo", &infammo))
        DebugCheats::Cmd_InfiniteAmmo::SetActive(infammo);
    ImGui::EndDisabled();
    if (Dev::PanelItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", canAmmo ? "Refund the burst the real player just fired so the current\n"
                                          "magazine never depletes.  Hooked in EntityAI::FireWeapon.\n"
                                          "AI weapons fire normally; magazine swaps are unaffected."
                                        : "Requires an active mission.");

    ImGui::Spacing();
    Dev::PanelHeading("Vehicle (player's current)");
    Dev::PanelSeparator();

    // Infinite fuel — hooked in Transport::ConsumeFuel.  Only meaningful
    // when the player is inside a vehicle; the gating logic itself
    // still allows toggling on foot (the cheat just has no effect
    // until the player mounts something).
    const bool canFuel = DebugCheats::Cmd_InfiniteFuel::Available();
    bool inffuel = DebugCheats::Cmd_InfiniteFuel::IsActive();
    ImGui::BeginDisabled(!canFuel);
    if (Dev::Checkbox("Infinite fuel", &inffuel))
        DebugCheats::Cmd_InfiniteFuel::SetActive(inffuel);
    ImGui::EndDisabled();
    if (Dev::PanelItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", canFuel ? "Refund fuel consumption on the vehicle the player is in.\n"
                                          "Hook lives in Transport::ConsumeFuel.  Refuel still works.\n"
                                          "No effect when on foot or in an AI-driven vehicle."
                                        : "Requires an active mission.");

    // Infinite armor — second SetDammage gate, targeting the player's
    // vehicle rather than the player's own body.
    const bool canArmor = DebugCheats::Cmd_InfiniteArmor::Available();
    bool infarmor = DebugCheats::Cmd_InfiniteArmor::IsActive();
    ImGui::BeginDisabled(!canArmor);
    if (Dev::Checkbox("Infinite armor", &infarmor))
        DebugCheats::Cmd_InfiniteArmor::SetActive(infarmor);
    ImGui::EndDisabled();
    if (Dev::PanelItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", canArmor ? "Drop damage to the vehicle the player is currently in.\n"
                                           "Hooked alongside god mode in Object::SetDammage.\n"
                                           "Covers tanks, jeeps, planes, helicopters."
                                         : "Requires an active mission.");

    ImGui::Spacing();
    Dev::PanelHeading("Actions");
    Dev::PanelSeparator();

    // Store position — log + clipboard.  The original OFP INSERT dev
    // hotkey.  Always available when a scene is up (works in the menu
    // demo loop too — camera dump is useful even without a mission
    // player).
    const bool canStore = DebugCheats::Cmd_StorePosition::Available();
    CheatButton("Store position (log + clipboard)", canStore,
                canStore ? "Dump camera + player position to the log AND copy a\n"
                           "ready-to-paste block (triSetView for the exact render\n"
                           "view + this setPos for the player) to the clipboard.\n"
                           "Replaces the old INSERT hotkey."
                         : "Requires a scene with an active camera.",
                [] { Defer([] { DebugCheats::Cmd_StorePosition::Invoke("", s_cheatsStatus); }); });

    // Save game — one-shot action.  Always usable when a mission is
    // running, including missions that normally disallow save.
    const bool canSave = DebugCheats::Cmd_SaveGame::Available();
    CheatButton("Save game now", canSave,
                canSave ? "Force-save the current world state to <SaveDir>/save.fps.\n"
                          "No 'save allowed' gating — bypasses mission-script restrictions."
                        : "Requires an active mission.",
                [] { Defer([] { DebugCheats::Cmd_SaveGame::Invoke("", s_cheatsStatus); }); });

    // Load game — inverse.  Grey out when the save file isn't on disk.
    const bool canLoad = DebugCheats::Cmd_LoadGame::Available();
    Dev::PanelSameLine();
    CheatButton("Load game now", canLoad,
                canLoad ? "Restore from <SaveDir>/save.fps via World::LoadBin.\n"
                          "Same engine path the normal 'Load Game' menu uses;\n"
                          "rehydrates the world in place (player, vehicles,\n"
                          "ammo, damage, time).  Deferred to run after ImGui\n"
                          "finishes the frame, same reason as Save."
                        : "Requires an active mission.  If <SaveDir>/save.fps doesn't\n"
                          "exist the click reports it in the status line; no crash.",
                [] { Defer([] { DebugCheats::Cmd_LoadGame::Invoke("", s_cheatsStatus); }); });

    // Skip time — four buttons.  The original OFP SCANCODE_T/Y/G/H
    // cheats are +1h / -1h continuous and +24h / -24h one-shot;
    // discrete buttons match the dev panel's click-driven UI better.
    const bool canTime = DebugCheats::Cmd_SkipTime::Available();
    ImGui::BeginDisabled(!canTime);
    if (Dev::Button("Time -1h"))
        DebugCheats::Cmd_SkipTime::InvokeHours(-1.0f, s_cheatsStatus);
    Dev::PanelSameLine();
    if (Dev::Button("Time +1h"))
        DebugCheats::Cmd_SkipTime::InvokeHours(+1.0f, s_cheatsStatus);
    Dev::PanelSameLine();
    if (Dev::Button("Time -24h"))
        DebugCheats::Cmd_SkipTime::InvokeHours(-24.0f, s_cheatsStatus);
    Dev::PanelSameLine();
    if (Dev::Button("Time +24h"))
        DebugCheats::Cmd_SkipTime::InvokeHours(+24.0f, s_cheatsStatus);
    ImGui::EndDisabled();
    if (Dev::PanelItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !canTime)
        ImGui::SetTooltip("Requires an active mission.");

    // Precise time-of-day seek.  The slider re-reads the clock every frame, so it
    // tracks the advancing time when idle and, when dragged, seeks by skipping the
    // delta to the target hour — reusing SkipTime so _timeInYear (sun angle/season)
    // stays consistent.  Ctrl+click the slider to type an exact hour (e.g. 18.50).
    ImGui::BeginDisabled(!canTime);
    float todHours = Glob.clock.GetTimeOfDay() * 24.0f;
    ImGui::SetNextItemWidth(220.0f);
    if (Dev::SliderFloat("Time of day", &todHours, 0.0f, 24.0f, "%05.2f h"))
    {
        const float cur = Glob.clock.GetTimeOfDay() * 24.0f;
        DebugCheats::Cmd_SkipTime::InvokeHours(todHours - cur, s_cheatsStatus);
    }
    Dev::PanelTooltip("Seek the time of day. Ctrl+click to type an exact hour.\n"
                      "Sun angle & season stay consistent (skips real time).");
    ImGui::EndDisabled();

    // Skipping time advances the overcast/fog forecast (World::SimulateLandscape), which
    // rolls new weather and shrinks the view range as you scrub. Freeze it while tuning.
    if (GWorld)
    {
        bool freeze = GWorld->IsWeatherFrozen();
        if (Dev::Checkbox("Freeze weather while scrubbing time", &freeze))
            GWorld->SetFreezeWeather(freeze);
        Dev::PanelTooltip("Stops the overcast/fog forecast from advancing when you skip time,\n"
                          "so scrubbing changes only the sun/sky — not rain or view distance.");
    }

    // Weather presets — instant overcast change.  No active-value
    // highlight: there's no public World::GetOvercast() to read back,
    // so we can't reliably show which preset is in effect.
    const bool canWeather = DebugCheats::Cmd_SetWeather::Available();
    Dev::PanelHeading("Weather:");
    Dev::PanelSameLine();
    ImGui::BeginDisabled(!canWeather);
    struct WeatherPreset
    {
        const char* label;
        float overcast;
    };
    static const WeatherPreset kWeather[] = {
        {"Clear", 0.0f},
        {"Cloudy", 0.3f},
        {"Overcast", 0.7f},
        {"Storm", 1.0f},
    };
    for (int i = 0; i < (int)(sizeof(kWeather) / sizeof(kWeather[0])); i++)
    {
        if (i > 0)
            Dev::PanelSameLine();
        if (Dev::Button(kWeather[i].label))
            DebugCheats::Cmd_SetWeather::InvokeOvercast(kWeather[i].overcast, s_cheatsStatus);
    }
    ImGui::EndDisabled();
    if (Dev::PanelItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !canWeather)
        ImGui::SetTooltip("Requires an active mission.");

    // Time multiplier — preset list.  Highlights the active value so
    // the user sees what's currently selected.  Engine saturates to
    // [kTimeAccMin, kTimeAccMax]; reading back via Get() reports the
    // clamped value.
    const bool canMult = DebugCheats::Cmd_TimeMultiplier::Available();
    const float currentMult = canMult ? DebugCheats::Cmd_TimeMultiplier::Get() : 1.0f;
    Dev::PanelHeading("Time multiplier:");
    Dev::PanelSameLine();
    ImGui::BeginDisabled(!canMult);
    static const float kPresets[] = {0.5f, 1.0f, 2.0f, 4.0f};
    for (int i = 0; i < (int)(sizeof(kPresets) / sizeof(kPresets[0])); i++)
    {
        const float v = kPresets[i];
        // Highlight the active preset (within 0.01 — float compare).
        const bool isActive = canMult && (currentMult > v - 0.01f) && (currentMult < v + 0.01f);
        if (isActive)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
        char label[16];
        snprintf(label, sizeof(label), "%.1fx", v);
        if (i > 0)
            Dev::PanelSameLine();
        if (Dev::Button(label))
            DebugCheats::Cmd_TimeMultiplier::SetValue(v, s_cheatsStatus);
        if (isActive)
            ImGui::PopStyleColor();
    }
    ImGui::EndDisabled();

    // Unlock campaign — writes <TmpSaveDir>/<campaign>.sqc files
    // directly so the unlock survives reopening the campaign load
    // screen.  Works from any display.
    if (Dev::Button("Unlock all campaigns"))
        Defer([] { DebugCheats::Cmd_UnlockCampaign::Invoke("", s_cheatsStatus); });
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("%s", "Mark every mission of every installed campaign as available.\n"
                                "Writes the unlock to <TmpSaveDir>/<campaign>.sqc so the\n"
                                "next Campaign Load open sees it.  Refreshes the live list\n"
                                "if the Campaign Load screen happens to be open right now.");

    ImGui::Spacing();
    Dev::PanelHeading("Map");
    Dev::PanelSeparator();

    // Show all units — independent of the _showUnits flag (which is
    // _ENABLE_CHEATS-gated).  Adds an unconditional DrawUnits pass in
    // CStaticMapMain::DrawExt across every AICenter.
    const bool canShowAll = DebugCheats::Cmd_ShowAllUnits::Available();
    bool showAll = DebugCheats::Cmd_ShowAllUnits::IsActive();
    ImGui::BeginDisabled(!canShowAll);
    if (Dev::Checkbox("Show all units on map", &showAll))
        DebugCheats::Cmd_ShowAllUnits::SetActive(showAll);
    ImGui::EndDisabled();
    if (Dev::PanelItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", canShowAll ? "Draw every unit of every side on the in-mission map,\n"
                                             "bypassing fog-of-war and side filtering.  Hooked in\n"
                                             "CStaticMapMain::DrawExt."
                                           : "Requires an active mission.");

    // Click-to-teleport — left-click on the in-mission map teleports
    // the player's vehicle to the clicked spot instead of issuing the
    // normal move/watch order.
    const bool canTeleport = DebugCheats::Cmd_MapTeleport::Available();
    bool teleport = DebugCheats::Cmd_MapTeleport::IsActive();
    ImGui::BeginDisabled(!canTeleport);
    if (Dev::Checkbox("Click-on-map to teleport", &teleport))
        DebugCheats::Cmd_MapTeleport::SetActive(teleport);
    ImGui::EndDisabled();
    if (Dev::PanelItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", canTeleport ? "Open the in-mission map (M) and left-click anywhere to\n"
                                              "teleport the player's vehicle.  Snaps to the ground\n"
                                              "surface height; if you're in a tank/chopper, the whole\n"
                                              "vehicle goes along.  Hooked in CStaticMapMain::OnLButtonClick."
                                            : "Requires an active mission.");

    if (!s_cheatsStatus.empty())
    {
        Dev::PanelSeparator();
        ImGui::TextWrapped("Last: %s", s_cheatsStatus.c_str());
    }
}

// Game tab — text / voice language pickers + view distance slider.
// Reuses the same kLangRotation list the F12 / F11 dev hotkeys use
// (engine/poseidon/UI/optionsUI.cpp:2333) so picker + hotkey stay
// consistent.
namespace
{
static const char* const kGameLangs[] = {
    "English", "Czech", "French", "German", "Italian", "Spanish", "Russian",
};
constexpr int kGameLangsCount = (int)(sizeof(kGameLangs) / sizeof(kGameLangs[0]));

int FindLangIndex(const char* current)
{
    if (!current)
        return 0;
    for (int i = 0; i < kGameLangsCount; i++)
        if (stricmp(current, kGameLangs[i]) == 0)
            return i;
    return 0;
}

const char* DebugBool(bool value)
{
    return value ? "true" : "false";
}

RString DebugObjectName(Object* object)
{
    if (!object)
        return "<null>";
    return object->GetDebugName();
}

ControlsCategory DebugSettingsCategoryForContext(InputContext context)
{
    switch (context)
    {
        case InputContext::Infantry:
            return ControlsCategoryOnFoot;
        case InputContext::CarDriver:
        case InputContext::TankDriver:
        case InputContext::ShipDriver:
            return ControlsCategoryVehicles;
        case InputContext::HeliPilot:
        case InputContext::PlanePilot:
            return ControlsCategoryPilot;
        case InputContext::TankGunner:
        case InputContext::Gunner:
            return ControlsCategoryGunner;
        default:
            return ControlsCategoryCount;
    }
}

void DrawInputContextDiagnostics()
{
    Dev::PanelHeading("Input context");
    if (!GWorld)
    {
        Dev::PanelHelp("world not loaded");
        return;
    }

    const auto& input = InputSubsystem::Instance();
    const InputContextResolution resolution = GWorld->ResolveInputContextResolution();
    const InputContext liveWorld = resolution.context;
    const InputContext cached = input.GetContext();

    Person* player = GWorld->PlayerOn();

    AIUnit* focus = GWorld->FocusOn();

    ImGui::Text("World: live=%s cached=%s manual=%s map=%s options=%s", InputContextName(liveWorld),
                InputContextName(cached), DebugBool(GWorld->PlayerManual()), DebugBool(GWorld->HasMap()),
                DebugBool(GWorld->HasOptions()));
    const ControlsCategory settingsCategory = DebugSettingsCategoryForContext(liveWorld);
    ImGui::Text("Settings: %s",
                settingsCategory == ControlsCategoryCount ? "<none>" : GetControlsCategoryName(settingsCategory));
    ImGui::Text("Resolved: %s | %s", static_cast<const char*>(DebugObjectName(resolution.transport)),
                InputSeatContextName(resolution.seat));
    ImGui::Text("Player: %s", static_cast<const char*>(DebugObjectName(player)));
    ImGui::Text("Focus:  %s", focus ? static_cast<const char*>(focus->GetDebugName()) : "<null>");
    ImGui::Text("Camera: %s", static_cast<const char*>(DebugObjectName(GWorld->CameraOn())));

    const InputContext ctx = resolution.context;
    ImGui::Text("Actions [%s]: F %.2f B %.2f L %.2f R %.2f Up %.2f Dn %.2f TL %.2f TR %.2f", InputContextName(ctx),
                input.GetAction(ctx, UAMoveForward, true), input.GetAction(ctx, UAMoveBack, true),
                input.GetAction(ctx, UAMoveLeft, true), input.GetAction(ctx, UAMoveRight, true),
                input.GetAction(ctx, UAMoveUp, true), input.GetAction(ctx, UAMoveDown, true),
                input.GetAction(ctx, UATurnLeft, true), input.GetAction(ctx, UATurnRight, true));
}
} // namespace

// Position diagnostics.
//
// "Where am I, where am I looking, and what am I looking at" was one line: the player's position
// and heading. That answers the first question only, and the questions that actually come up while
// debugging a render are the other two -- a cloud artifact, a material, a LOD pop all need the
// CAMERA's position and the point it is aimed at, which is not the player's.
//
// Reported in SQF's axis order [east, north, elevation] throughout. mission.sqm stores
// {east, elevation, north}; mixing the two up is an easy way to place something a long way from
// where it was meant to go, so the order is stated in the UI rather than assumed.
namespace
{
int gPosPrecision = 2;
bool gPosCompactHud = false;

// The look-at point: where the view ray first meets the terrain. A camera readout that stops at
// "facing 199.9 degrees" cannot tell you which hill you are pointed at, and the terrain hit is the
// answer to that. Coarse fixed-step march then a bisection refine -- this runs once per frame for a
// debug panel, so clarity beats a cleverer intersector.
bool CameraTargetPoint(const Vector3& origin, const Vector3& dir, float maxDist, Vector3& hit, float& hitDist)
{
    if (!GLandscape)
        return false;
    const float step = 25.0f;
    float prev = 0.0f;
    bool prevAbove = true;
    for (float t = step; t <= maxDist; t += step)
    {
        const Vector3 p = origin + dir * t;
        const bool above = p.Y() > GLandscape->SurfaceY(p.X(), p.Z());
        if (!above && prevAbove && t > step)
        {
            float lo = prev, hi = t;
            for (int i = 0; i < 24; ++i)
            {
                const float mid = 0.5f * (lo + hi);
                const Vector3 pm = origin + dir * mid;
                if (pm.Y() > GLandscape->SurfaceY(pm.X(), pm.Z()))
                    lo = mid;
                else
                    hi = mid;
            }
            hitDist = 0.5f * (lo + hi);
            hit = origin + dir * hitDist;
            return true;
        }
        prevAbove = above;
        prev = t;
    }
    return false;
}

void FormatVec(char* out, size_t n, const char* label, const Vector3& v, int prec)
{
    // SQF order: [east, north, elevation] == [X, Z, Y] internally.
    snprintf(out, n, "%s [%.*f, %.*f, %.*f]", label, prec, v.X(), prec, v.Z(), prec, v.Y());
}

struct PositionLines
{
    char player[192] = {};
    char cam[192] = {};
    char freefly[192] = {};
    char tgt[192] = {};
    char time[128] = {};
    bool valid = false;
    bool hasFreefly = false;
};

PositionLines BuildPositionLines(int prec)
{
    PositionLines out;
    if (!GWorld || !GWorld->CameraOn())
        return out;
    out.valid = true;
    const Object* player = GWorld->PlayerOn();
    const Object* camObj = GWorld->CameraOn();

    // The RENDER camera where one exists: that is the transform the frame was drawn with, and it is
    // not always the camera object's own (attached views, effects). Falls back to the object.
    Vector3 camPos = camObj->Position();
    Vector3 camDir = camObj->Direction();
    const char* camSource = "object";
    if (GScene && GScene->GetCamera())
    {
        camPos = GScene->GetCamera()->Position();
        camDir = GScene->GetCamera()->Direction();
        camSource = "scene";
    }

    if (player)
    {
        const Vector3 p = player->Position();
        // Heading the same way getDir does it: atan2(dir.x, dir.z) in degrees, wrapped to [0,360)
        // so it matches what the console reports.
        const Vector3 d = player->Direction();
        float heading = atan2(d.X(), d.Z()) * (180.0f / H_PI);
        if (heading < 0.0f)
            heading += 360.0f;
        char v[128];
        FormatVec(v, sizeof(v), "player", p, prec);
        snprintf(out.player, sizeof(out.player), "%s  dir %.1f", v, heading);
    }
    else
    {
        snprintf(out.player, sizeof(out.player), "player <none>");
    }

    // Azimuth on the same convention as the player heading; elevation is the signed angle out of the
    // horizontal plane, positive looking up.
    float az = atan2(camDir.X(), camDir.Z()) * (180.0f / H_PI);
    if (az < 0.0f)
        az += 360.0f;
    const float horiz = sqrtf(camDir.X() * camDir.X() + camDir.Z() * camDir.Z());
    const float el = atan2(camDir.Y(), horiz > 1e-6f ? horiz : 1e-6f) * (180.0f / H_PI);

    {
        char v[128];
        FormatVec(v, sizeof(v), "cam", camPos, prec);
        snprintf(out.cam, sizeof(out.cam), "%s  az %.1f  el %.1f  (%s)", v, az, el, camSource);
    }

    // Zeus free-fly deliberately leaves CameraOn() on the player while a
    // camera effect drives the rendered view.  Keep the player row above for
    // teleport/debug context, but expose the independently-moving free-fly
    // vehicle as its own copyable coordinate line.  Reading it directly also
    // avoids any ambiguity from effects that may temporarily offset the scene
    // camera (for example a cutscene shake).
    if (s_zeusCamera)
    {
        const Vector3 freeflyPos = s_zeusCamera->Position();
        const Vector3 freeflyDir = s_zeusCamera->Direction();
        float freeflyAz = atan2(freeflyDir.X(), freeflyDir.Z()) * (180.0f / H_PI);
        if (freeflyAz < 0.0f)
            freeflyAz += 360.0f;
        const float freeflyHoriz = sqrtf(freeflyDir.X() * freeflyDir.X() + freeflyDir.Z() * freeflyDir.Z());
        const float freeflyEl = atan2(freeflyDir.Y(), freeflyHoriz > 1e-6f ? freeflyHoriz : 1e-6f) * (180.0f / H_PI);
        char v[128];
        FormatVec(v, sizeof(v), "freefly", freeflyPos, prec);
        snprintf(out.freefly, sizeof(out.freefly), "%s  az %.1f  el %.1f  (active)", v, freeflyAz, freeflyEl);
        out.hasFreefly = true;
    }

    Vector3 tgt;
    float tgtDist = 0.0f;
    const bool hasTgt = CameraTargetPoint(camPos, camDir, 12000.0f, tgt, tgtDist);
    if (hasTgt)
    {
        char v[128];
        FormatVec(v, sizeof(v), "tgt", tgt, prec);
        snprintf(out.tgt, sizeof(out.tgt), "%s  dist %.1f m", v, tgtDist);
    }
    else
    {
        snprintf(out.tgt, sizeof(out.tgt), "tgt <sky>  (no terrain within 12 km)");
    }

    // Seconds since mission epoch. Time has no direct accessor; differencing against a
    // default-constructed Time is how the rest of the engine turns it into a float.
    snprintf(out.time, sizeof(out.time), "t %.3f s", Glob.time - Foundation::Time());
    return out;
}

void DrawPositionDiagnostics()
{
    Dev::PanelHeading("Position");
    Dev::PanelSeparator();
    const PositionLines out = BuildPositionLines(gPosPrecision);
    if (!out.valid)
    {
        Dev::PanelHelp("no world loaded");
        return;
    }

    Dev::PanelHeading(out.player);
    Dev::PanelSameLine();
    if (Dev::Button("Copy##pl"))
        ImGui::SetClipboardText(out.player);

    Dev::PanelHeading(out.cam);
    Dev::PanelSameLine();
    if (Dev::Button("Copy##cam"))
        ImGui::SetClipboardText(out.cam);

    if (out.hasFreefly)
    {
        Dev::PanelHeading(out.freefly);
        Dev::PanelSameLine();
        if (Dev::Button("Copy##freefly"))
            ImGui::SetClipboardText(out.freefly);
    }

    Dev::PanelHeading(out.tgt);
    Dev::PanelSameLine();
    if (Dev::Button("Copy##tgt"))
        ImGui::SetClipboardText(out.tgt);

    Dev::PanelHeading(out.time);

    char all[960];
    if (out.hasFreefly)
        snprintf(all, sizeof(all), "%s | %s | %s | %s | %s", out.player, out.cam, out.freefly, out.tgt, out.time);
    else
        snprintf(all, sizeof(all), "%s | %s | %s | %s", out.player, out.cam, out.tgt, out.time);
    if (Dev::Button("Copy all##pos"))
        ImGui::SetClipboardText(all);
    Dev::PanelSameLine();
    ImGui::SetNextItemWidth(110.0f);
    Dev::SliderInt("decimals", &gPosPrecision, 0, 4);
    Dev::Checkbox("Compact HUD overlay", &gPosCompactHud);
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Draw the same numbers as a small always-on overlay,\nfor lining a shot up without the "
                          "panel covering it.");
}
// The compact always-on readout, drawn on the foreground list when the panel is closed. Same
// numbers, same builder — a second copy of the maths would be a second thing to keep correct.
void DrawPositionHudOverlay()
{
    if (!gPosCompactHud)
        return;
    const PositionLines out = BuildPositionLines(gPosPrecision);
    if (!out.valid)
        return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImU32 fg = IM_COL32(255, 255, 255, 220);
    const ImU32 bg = IM_COL32(0, 0, 0, 110);
    const char* rows[5] = {out.player, out.cam, out.freefly, out.tgt, out.time};
    const int rowCount = out.hasFreefly ? 5 : 4;
    if (!out.hasFreefly)
    {
        rows[2] = out.tgt;
        rows[3] = out.time;
    }
    const float lh = ImGui::GetTextLineHeightWithSpacing();
    float wmax = 0.0f;
    for (int i = 0; i < rowCount; ++i)
        wmax = std::max(wmax, ImGui::CalcTextSize(rows[i]).x);
    const ImVec2 org(10.0f, 10.0f);
    draw->AddRectFilled(ImVec2(org.x - 4.0f, org.y - 3.0f),
                        ImVec2(org.x + wmax + 6.0f, org.y + lh * float(rowCount) + 3.0f), bg);
    for (int i = 0; i < rowCount; ++i)
        draw->AddText(ImVec2(org.x, org.y + lh * float(i)), fg, rows[i]);
}
} // namespace

void DrawGameTab()
{
    // Player/camera position, with a copy button. Reading coordinates out of the
    // SQF console meant `getPos player`, which returns an array — and until the
    // formatter above, arrays printed as nothing. A readout is also simply less
    // work than typing a command to answer "where am I".
    DrawPositionDiagnostics();
    ImGui::Spacing();

    Dev::PanelHeading("Language");
    Dev::PanelSeparator();

    // Text language — drives stringtables, mission briefings, UI.
    const char* currentText = GLanguage;
    int textIdx = FindLangIndex(currentText);
    if (Dev::Combo("Text", &textIdx, kGameLangs, kGameLangsCount))
        Defer([picked = std::string(kGameLangs[textIdx])] { SetLanguage(RString(picked.c_str())); });
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Stringtables, mission briefings, UI labels.\nSame as the F12 dev-only hotkey.");

    // Voice language — drives <base>.<voiceLang>.<ext> sound lookups.
    const std::string voiceLang = GetSelectedVoiceLanguage();
    int voiceIdx = FindLangIndex(voiceLang.c_str());
    if (Dev::Combo("Voice", &voiceIdx, kGameLangs, kGameLangsCount))
        Defer([picked = std::string(kGameLangs[voiceIdx])] { SetSelectedVoiceLanguage(picked); });
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Voice-over track for say / playSound / radio.\nSame as the F11 dev-only hotkey.\nIn-flight "
                          "audio is unaffected; lookup applies on the next play.");

    ImGui::Spacing();
    Dev::PanelHeading("View distance");
    Dev::PanelSeparator();

    // VD slider — clamped to the same range as the Options UI.  Engine
    // saturates internally too; we mirror so the slider can't request
    // a value that just gets clipped silently.
    static float s_vd = ENGINE_CONFIG.tacticalZ;
    s_vd = ENGINE_CONFIG.tacticalZ; // sync with whatever else set it
    if (Dev::SliderFloat("VD (m)", &s_vd, GameSettingsConfig::kMinViewDistance, GameSettingsConfig::kMaxViewDistance,
                         "%.0f m"))
    {
        const float v = s_vd;
        Defer([v] { SetVisibility(v); });
    }
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Terrain / horizon distance (the master).\nRange %.0f..%.0f m.  Bypasses the "
                          "per-tier graphics preset.",
                          GameSettingsConfig::kMinViewDistance, GameSettingsConfig::kMaxViewDistance);
    // Object and shadow distances are derived from VD (ViewDistanceResolver), so
    // there are no separate sliders — moving VD moves all three.

    ImGui::Spacing();
    Dev::PanelHeading("Diagnostics");
    Dev::PanelSeparator();

    DrawInputContextDiagnostics();
    ImGui::Spacing();

    // The TXT / VO / VD localization-status block in the mission preview is a
    // diagnostic overlay, hidden from players by default.  Off shows the plain
    // mission overview; on prepends the per-language text/voice/view-distance table.
    bool showLoc = MissionLanguageDetector::ShowLocalizationDebugInfo();
    if (Dev::Checkbox("Mission localization info (TXT/VO/VD)", &showLoc))
        MissionLanguageDetector::SetShowLocalizationDebugInfo(showLoc);
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Prepend the per-language text/voice availability and view-distance\n"
                          "table to the mission preview.  Re-select a mission to refresh.");
}

// Console tab — SQF / DebugCommand runner.  Bare lines without a `:`
// prefix dispatch through DebugCommands::Run first (so "save",
// "endmission win", etc. work), then fall back to SQF evaluation.
// Lines starting with `:` are forced to SQF (use this if a SQF name
// happens to collide with a DebugCommand name).
namespace
{
struct ConsoleState
{
    char input[512] = "";
    std::vector<std::string> scrollback;
    bool autoScroll = true;
    bool focusOnShow = true;
};
ConsoleState s_console;

void ConsoleAppend(const std::string& line)
{
    s_console.scrollback.push_back(line);
    if (s_console.scrollback.size() > 200)
        s_console.scrollback.erase(s_console.scrollback.begin(),
                                   s_console.scrollback.begin() + (s_console.scrollback.size() - 200));
}

// GameValue::GetText() renders an array as nothing, so `getPos player` came back
// blank and the console could not report a position at all. Format arrays
// recursively instead; scalars keep GetText's own formatting.
std::string FormatConsoleValue(const GameValue& value)
{
    if (value.GetType() == GameArray)
    {
        const GameArrayType& items = value;
        std::string out = "[";
        for (int i = 0; i < items.Size(); ++i)
        {
            if (i != 0)
                out += ", ";
            out += FormatConsoleValue(items[i]);
        }
        return out + "]";
    }
    const char* text = static_cast<const char*>(value.GetText());
    return text ? std::string(text) : std::string();
}

void ConsoleRun(std::string_view line)
{
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);
    if (line.empty())
        return;
    ConsoleAppend(std::string("> ") + std::string(line));

    const bool forceSqf = !line.empty() && line.front() == ':';
    if (forceSqf)
        line.remove_prefix(1);

    if (!forceSqf)
    {
        std::string out;
        if (DebugCommands::Run(line, out))
        {
            if (!out.empty())
                ConsoleAppend(out);
            return;
        }
    }

    // SQF path.  Mirrors the Evaluator/express.hpp idiom — runs in the
    // current game state's evaluation context.  No-op + error log when
    // no world is up (e.g. main-menu, before mission load).
    if (!GWorld || !GWorld->GetGameState())
    {
        ConsoleAppend("(no game state — SQF unavailable here)");
        return;
    }
    GameValue result = GWorld->GetGameState()->EvaluateMultiple(std::string(line).c_str());
    if (result.GetType() != GameNothing)
        ConsoleAppend(std::string("= ") + FormatConsoleValue(result));
}
} // namespace

void DrawConsoleTab()
{
    Dev::PanelHeading("SQF / DebugCommands console");
    Dev::PanelSeparator();

    // Scrollback region.  Reserve room for the input row at the bottom.
    const float inputRowH = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("ConsoleScroll", ImVec2(0, -inputRowH), true, ImGuiWindowFlags_HorizontalScrollbar))
    {
        for (const auto& line : s_console.scrollback)
            Dev::PanelHeading(line.c_str());
        if (s_console.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    // Input row: text box + Run button.  Enter inside the text box
    // also runs the line.  EnterReturnsTrue makes the InputText
    // produce true when Enter is hit, so we don't need a separate
    // key check.
    if (s_console.focusOnShow)
    {
        ImGui::SetKeyboardFocusHere();
        s_console.focusOnShow = false;
    }
    bool entered = Dev::InputText("##ConsoleInput", s_console.input, sizeof(s_console.input),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    Dev::PanelSameLine();
    bool clicked = Dev::Button("Run");
    if (entered || clicked)
    {
        std::string line(s_console.input);
        s_console.input[0] = 0;
        if (!line.empty())
            Defer([line] { ConsoleRun(line); });
        ImGui::SetKeyboardFocusHere(-1); // refocus the text box
    }
    Dev::PanelSameLine();
    if (Dev::Button("Clear"))
        s_console.scrollback.clear();
    Dev::PanelSameLine();
    Dev::Checkbox("Auto-scroll", &s_console.autoScroll);

    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Bare lines dispatch through DebugCommands first\n"
                          "(save, endmission win, weather 0.5, …).\n"
                          "Prefix `:` to force SQF (e.g. `:hint \"hi\"`)");
}

// Profile tab — FPS gauge + frame-time history graph.  Reads the
// engine's _lastFrameDuration each draw, keeps a 240-frame ring
// buffer (~4 seconds at 60 fps) for the plot.
namespace
{
constexpr int kProfHistory = 240;
float s_frameMs[kProfHistory] = {};
int s_frameMsHead = 0;

void ProfileSample()
{
    if (!GEngine)
        return;
    const uint32_t lastMs = GEngine->GetLastFrameDuration();
    const float ms = static_cast<float>(lastMs);
    s_frameMs[s_frameMsHead] = ms;
    s_frameMsHead = (s_frameMsHead + 1) % kProfHistory;
}

float ProfileFps()
{
    if (!GEngine)
        return 0.0f;
    const uint32_t lastMs = GEngine->GetLastFrameDuration();
    return lastMs > 0 ? 1000.0f / static_cast<float>(lastMs) : 0.0f;
}

float ProfileFrameMs()
{
    if (!GEngine)
        return 0.0f;
    return static_cast<float>(GEngine->GetLastFrameDuration());
}
} // namespace

void DrawProfileTab()
{
    ProfileSample(); // pump every draw

    Dev::PanelHeading("Frame stats");
    Dev::PanelSeparator();

    const float fps = ProfileFps();
    const float ms = ProfileFrameMs();
    ImGui::Text("FPS:   %.1f", fps);
    ImGui::Text("Frame: %.2f ms", ms);
    if (GEngine)
    {
        // Keep the selected renderer visible in the always-available Profile
        // tab. This is intentionally sourced from the live engine rather than
        // the requested command-line backend, which can differ after a
        // fallback or a failed backend construction.
        ImGui::Text("Renderer: %s", static_cast<const char*>(GEngine->GetRendererName()));
        Dev::PanelHelp("Runtime: %s", static_cast<const char*>(GEngine->GetDebugName()));
        const uint32_t caps = GEngine->GetRuntimeCapabilityFlags();
        if (caps)
        {
            Dev::PanelHelp("Capabilities: BC=%s bindless=%s timestamps=%s in-pass=%s HDR=%s MSAA=%s",
                           (caps & (1u << 0)) ? "yes" : "no", "yes", (caps & (1u << 4)) ? "yes" : "no",
                           (caps & (1u << 5)) ? "yes" : "no", (caps & (1u << 6)) ? "yes" : "no",
                           (caps & (1u << 7)) ? "yes" : "no");
        }
    }

    // Frame-time plot.  PlotLines is fine for ring-buffered floats;
    // ImGui handles the visual stride.  Y-axis fixed 0..50 ms (~20fps
    // floor) so spikes are visible without auto-rescaling jitter.
    Dev::PanelSeparator();
    ImGui::PlotLines("##frame_ms", s_frameMs, kProfHistory, s_frameMsHead, "frame ms (last 240)", 0.0f, 50.0f,
                     ImVec2(0, 80));

    if (Dev::Button("Reset history"))
    {
        for (int i = 0; i < kProfHistory; i++)
            s_frameMs[i] = 0.0f;
        s_frameMsHead = 0;
    }
}

// Memory tab — live MemoryUsed() value + peak tracker + history plot.
namespace
{
constexpr int kMemHistory = 240;
float s_memMb[kMemHistory] = {};
int s_memHead = 0;
size_t s_memPeak = 0;

void MemorySample()
{
    const size_t used = Foundation::MemoryUsed();
    if (used > s_memPeak)
        s_memPeak = used;
    s_memMb[s_memHead] = static_cast<float>(used) / (1024.0f * 1024.0f);
    s_memHead = (s_memHead + 1) % kMemHistory;
}
} // namespace

inline float ToMB(size_t bytes)
{
    return static_cast<float>(bytes) / (1024.0f * 1024.0f);
}

void DrawMemoryTab()
{
    MemorySample();

    const Foundation::ProcessMemoryStats stats = Foundation::MemoryProcessStats();
    const float mb = ToMB(stats.used);
    const float peakMb = ToMB(s_memPeak);

    Dev::PanelHeading("Process heap");
    Dev::PanelSeparator();
    ImGui::Text("Current: %.1f MB", mb);
    ImGui::Text("Peak:    %.1f MB", peakMb);
    if (stats.softLimit || stats.hardLimit)
    {
        ImGui::Text("Soft (trim):  %.0f MB%s", ToMB(stats.softLimit),
                    stats.softLimit && stats.used > stats.softLimit ? "  (OVER — trimming to budgets)" : "");
        if (Dev::PanelItemHovered())
            ImGui::SetTooltip("Pressure watermark. Over it, each cache is trimmed back to its own\n"
                              "declared budget once per frame (FrameMaintenance). Never refuses.");
        ImGui::Text("Hard (evict): %.0f MB%s", ToMB(stats.hardLimit),
                    stats.hardLimit && stats.used > stats.hardLimit ? "  (OVER — evicting caches)" : "");
        if (Dev::PanelItemHovered())
            ImGui::SetTooltip("Eviction target, not a wall. Over it the allocator additionally claws\n"
                              "memory back with cost-ordered cache eviction — but never refuses an\n"
                              "allocation: refusing would crash the engine's many unchecked `new` sites.");
    }
    else
    {
        Dev::PanelHelp("No process limit set (unlimited).");
    }

    // Plot — same shape as the profile tab.  Y-axis floats around the
    // peak; ImPlot would give a nicer presentation but PlotHistogram
    // is sufficient and zero-dependency.
    char overlay[64];
    snprintf(overlay, sizeof(overlay), "MB used (last %d frames)", kMemHistory);
    ImGui::PlotHistogram("##mem_mb", s_memMb, kMemHistory, s_memHead, overlay, 0.0f, peakMb * 1.1f + 1.0f,
                         ImVec2(0, 80));

    if (Dev::Button("Reset peak / history"))
    {
        for (int i = 0; i < kMemHistory; i++)
            s_memMb[i] = 0.0f;
        s_memHead = 0;
        s_memPeak = stats.used;
    }

    // ── Per-subsystem residency (the FreeOnDemand registry) ────────────────
    // One snapshot drives both the count and the table so they can't disagree.
    Foundation::MemoryDomainStat domains[32];
    const int n = Foundation::MemorySnapshotDomains(domains, 32);

    ImGui::Spacing();
    ImGui::Text("Subsystems (%d registered)", n);
    Dev::PanelSameLine();
    if (Dev::Button("Trim caches now"))
        Defer([] { Foundation::MemoryEnforceBudgets(); });
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Evict every registered cache back to its declared budget\n"
                          "(FreeOnDemand). Domains with no budget are untouched.");
    Dev::PanelSeparator();

    if (n == 0)
    {
        Dev::PanelHelp("(no subsystems registered yet)");
    }
    else if (ImGui::BeginTable("mem_domains", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("domain");
        ImGui::TableSetupColumn("held");
        ImGui::TableSetupColumn("budget / usage");
        ImGui::TableHeadersRow();
        for (int i = 0; i < n; i++)
        {
            const Foundation::MemoryDomainStat& d = domains[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Dev::PanelHeading(d.name);
            ImGui::TableNextColumn();
            // Byte-accounted caches show MB; count-only registries (shapes,
            // materials) show their item count instead of a misleading 0 MB.
            if (d.heldBytes > 0 || d.heldItems == 0)
                ImGui::Text("%.1f MB", ToMB(d.heldBytes));
            else
                ImGui::Text("%zu items", d.heldItems);
            ImGui::TableNextColumn();
            if (d.budgetBytes > 0)
            {
                const float frac = static_cast<float>(d.heldBytes) / static_cast<float>(d.budgetBytes);
                char label[48];
                snprintf(label, sizeof(label), "%.0f / %.0f MB", ToMB(d.heldBytes), ToMB(d.budgetBytes));
                ImGui::ProgressBar(frac > 1.0f ? 1.0f : frac, ImVec2(-1, 0), label);
            }
            else
            {
                Dev::PanelHelp("— (no budget)");
            }
        }
        ImGui::EndTable();
    }

    // ── Live limit controls — set a watermark and watch the tick trim/evict ──
    Engine::GpuMemoryStatsOut gpu{};
    if (GEngine && GEngine->GetGpuMemoryStats(gpu))
    {
        ImGui::Spacing();
        Dev::PanelHeading("WGPU dynamic residency (portable lower bound)");
        Dev::PanelSeparator();
        ImGui::Text("Tracked allocation: %.1f / %.0f MB%s", ToMB(gpu.trackedBytes), ToMB(gpu.budgetBytes),
                    gpu.overBudget ? "  (OVER BUDGET)" : "");
        ImGui::Text("Object textures:   %.1f MB in %u textures", ToMB(gpu.objectTextureBytes), gpu.objectTextureCount);
        ImGui::Text("Geometry payload:  %.1f MB live", ToMB(gpu.geometryLiveBytes));
        ImGui::Text("Geometry buffers:  %.1f MB capacity + %.1f MB retired in-flight", ToMB(gpu.geometryCapacityBytes),
                    ToMB(gpu.geometryRetiredBytes));
        if (Dev::PanelItemHovered())
            ImGui::SetTooltip("WebGPU does not expose driver-private alignment and metadata.\n"
                              "These are exact resource payload/buffer sizes and therefore\n"
                              "a lower bound, not total physical VRAM usage.");
    }

    ImGui::Spacing();
    Dev::PanelHelp("Set process limits (MB; soft=trim, hard=evict; 0 = unlimited):");
    static int s_softMB = -1, s_hardMB = -1;
    if (s_softMB < 0) // seed once from the live values
    {
        s_softMB = static_cast<int>(ToMB(stats.softLimit));
        s_hardMB = static_cast<int>(ToMB(stats.hardLimit));
    }
    ImGui::SetNextItemWidth(120);
    Dev::InputInt("soft (trim) MB", &s_softMB, 32, 256);
    Dev::PanelSameLine();
    ImGui::SetNextItemWidth(120);
    Dev::InputInt("hard (evict) MB", &s_hardMB, 32, 256);
    if (s_softMB < 0)
        s_softMB = 0;
    if (s_hardMB < 0)
        s_hardMB = 0;
    if (Dev::Button("Apply limits"))
    {
        const size_t soft = static_cast<size_t>(s_softMB) * (1024 * 1024);
        const size_t hard = static_cast<size_t>(s_hardMB) * (1024 * 1024);
        Defer([soft, hard] { Foundation::SetProcessMemoryLimits(soft, hard); });
    }
    Dev::PanelSameLine();
    Dev::PanelHelp("session-only; persist via EngineConfig");
}

// Live mod picker + in-process reload. Lists the @<mod> folders actually present
// in the user's mods folder, lets you tick a set, and re-mounts the game with
// exactly that set — only from the main menu (re-mounting mid-mission would evict
// assets the simulation still references).
void DrawModsTab()
{
    namespace fs = std::filesystem;
    const std::string modsRoot = GamePaths::Instance().ModsDir();

    Dev::PanelHeading("Mods folder (scanned live):");
    ImGui::TextWrapped("%s", modsRoot.c_str());
    Dev::PanelSeparator();

    // Checkbox state persists across frames: modId ("@foo") -> checked.
    static std::map<std::string, bool> s_modChecked;

    // Live scan for @<mod> folders each frame.
    std::vector<std::string> mods;
    std::error_code ec;
    if (fs::is_directory(modsRoot, ec))
    {
        for (const auto& entry : fs::directory_iterator(modsRoot, ec))
        {
            std::error_code dirEc;
            if (!entry.is_directory(dirEc))
                continue;
            const std::string name = entry.path().filename().string();
            if (!name.empty() && name[0] == '@')
                mods.push_back(name);
        }
    }
    std::sort(mods.begin(), mods.end());

    if (mods.empty())
        Dev::PanelHelp("(no @<mod> folders here yet — drop one in and it shows up)");

    for (const std::string& modId : mods)
    {
        auto it = s_modChecked.find(modId);
        bool checked = (it != s_modChecked.end()) ? it->second : false;
        if (Dev::Checkbox(modId.c_str(), &checked) || it == s_modChecked.end())
            s_modChecked[modId] = checked;
    }

    // Build the mod path from the checked set — semicolon-separated absolute
    // mod-folder paths (empty string = base game only). modsRoot ends with a sep.
    std::string modPath;
    for (const std::string& modId : mods)
    {
        auto it = s_modChecked.find(modId);
        if (it != s_modChecked.end() && it->second)
        {
            if (!modPath.empty())
                modPath += ';';
            modPath += modsRoot + modId;
        }
    }

    ImGui::Spacing();
    Dev::PanelSeparator();
    ImGui::TextWrapped("Apply set: %s", modPath.empty() ? "(none — base game only)" : modPath.c_str());

    const bool canReload = Poseidon::GApp != nullptr && Poseidon::GApp->m_canRender && GWorld != nullptr &&
                           GWorld->GetMode() == GModeIntro;
    CheatButton("Reload with selected mods", canReload,
                canReload ? "Re-mount the game with exactly the checked mods.\n"
                            "Keeps the window, shows the loading screen, and lands\n"
                            "back on a fresh main menu with the new mod set.\n"
                            "Uncheck everything to reload the base game."
                          : "Available from the main menu only (not during a mission).",
                [modPath]
                {
                    SetVisible(false);
                    // Queue for the next AppIdle (before simulate/draw); running the reload
                    // inside the swap crashed the rebuilt world's first Simulate.
                    RequestDeferredReload(modPath.c_str());
                });
}

void AspectReapply()
{
    // Re-resolve + apply the aspect settings for the current viewport.
    // Deferred so the engine mutation runs after ImGui::Render returns.
    Defer(
        []
        {
            if (GEngine)
                GEngine->FireResizePostHook(GEngine->Width(), GEngine->Height());
        });
}

// Release the game's mouse grab while the panel is open so the cursor can
// leave the window (to drag-resize it); restore on close.  The game keeps
// simulating — this only frees the cursor.
void ApplyDevPanelMouseState()
{
    if (!GEngine)
        return;
    const bool overlayOwnsMouse = s_visible || s_zeusCamera;
    if (overlayOwnsMouse && !s_mouseReleasedByPanel)
    {
        s_savedMouseGrab = GEngine->IsMouseGrabbed();
        s_mouseReleasedByPanel = true;
    }

    if (overlayOwnsMouse)
    {
        // Panel open: the OS pointer must be free, both to reach ImGui widgets
        // and to drag-resize the window.
        //
        // Panel hidden with Zeus active: grab.  Zeus resolves every position
        // from the engine's own cursor sprite (see ZeusCursorPixel), never from
        // absolute SDL coordinates, so relative mouse mode is exactly what that
        // cursor wants — it cannot stall against the window edge, and SDL keeps
        // the desktop pointer hidden while it is engaged.
        GEngine->SetMouseGrab(!s_visible);
    }
    else if (s_mouseReleasedByPanel)
    {
        GEngine->SetMouseGrab(s_savedMouseGrab);
        s_mouseReleasedByPanel = false;
    }

    // SDL can still restore the desktop pointer across focus transitions, so
    // assert the intended state explicitly: the focused game viewport must show
    // one cursor, not the OS cursor over the game's cursor sprite.
    if (s_visible)
        SDL_ShowCursor();
    else if (s_zeusCamera)
        SDL_HideCursor();
}

// Recheck the live session at every input/frame boundary. A panel opened in
// single player must neither keep its camera nor drain queued cheats after joining MP.
bool SuspendMultiplayerTools()
{
    if (!MultiplayerSession())
        return false;
    s_visible = false;
    s_pendingActions.clear();
    if (s_zeusCamera)
    {
        CameraEffect* effect = GWorld->GetCameraEffect();
        if (effect && effect->GetObject() == s_zeusCamera.GetLink())
        {
            GWorld->SetCameraEffect(nullptr);
            ShowCinemaBorder(s_zeusPrevCinemaBorder);
        }
        s_zeusCamera->SetDelete();
        s_zeusCamera = nullptr;
    }
    s_zeusCursor = nullptr;
    s_zeusClickPlacement = false;
    s_zeusRotateDrag = s_zeusMoveDrag = s_zeusLassoDrag = false;
    s_zeusSuppressNextMouseUp = false;
    s_zeusConsumeMouseEvent = s_zeusConsumeKeyboardEvent = s_zeusConsumeShortcutKeyUp = false;
    s_zeusSelection.clear();
    s_zeusClipboard.clear();
    s_zeusMoveOffsets.clear();
    s_terrainBrushDragging = false;
    Dev::TerrainBrush().enabled = false;
    Dev::CaveEditor().enabled = false;
    DebugCheats::Cmd_God::SetActive(false);
    DebugCheats::Cmd_InfiniteAmmo::SetActive(false);
    DebugCheats::Cmd_InfiniteFuel::SetActive(false);
    DebugCheats::Cmd_InfiniteArmor::SetActive(false);
    DebugCheats::Cmd_ShowAllUnits::SetActive(false);
    DebugCheats::Cmd_MapTeleport::SetActive(false);
    s_headlampOn = false;
    UpdateHeadlamp();
    if (s_mouseReleasedByPanel)
        ApplyDevPanelMouseState();
    return true;
}

// Resize the window to the largest box of the given aspect ratio that fits
// the current monitor's usable area, then center it (so it stays fully
// visible).  Drives the normal SDL resize path → aspect re-resolves.
void ResizeWindowToRatio(float ratio)
{
    if (!s_window || ratio <= 0.0f)
        return;
    int availW = 1920, availH = 1080;
    const SDL_DisplayID disp = SDL_GetDisplayForWindow(s_window);
    SDL_Rect ub{};
    if (SDL_GetDisplayUsableBounds(disp, &ub) && ub.w > 0 && ub.h > 0)
    {
        availW = ub.w;
        availH = ub.h;
    }
    const float margin = 0.90f;
    float w = static_cast<float>(availW) * margin;
    float h = w / ratio;
    if (h > static_cast<float>(availH) * margin)
    {
        h = static_cast<float>(availH) * margin;
        w = h * ratio;
    }
    int iw = static_cast<int>(w + 0.5f);
    int ih = static_cast<int>(h + 0.5f);
    if (iw < 320)
        iw = 320;
    if (ih < 240)
        ih = 240;
    SDL_SetWindowSize(s_window, iw, ih);
    SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED_DISPLAY(disp), SDL_WINDOWPOS_CENTERED_DISPLAY(disp));
}

void DrawAspectTab()
{
    AspectRatio::LiveControls& live = AspectRatio::Live();
    bool changed = false;

    // --- Window size + monitor info + resize-to-ratio presets ---
    if (s_window)
    {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(s_window, &ww, &wh);
        ImGui::Text("window : %d x %d  (%.3f)", ww, wh,
                    wh > 0 ? static_cast<float>(ww) / static_cast<float>(wh) : 0.0f);
        const SDL_DisplayID disp = SDL_GetDisplayForWindow(s_window);
        SDL_Rect ub{};
        if (SDL_GetDisplayUsableBounds(disp, &ub) && ub.h > 0)
            ImGui::Text("monitor: %d x %d  (%.3f)", ub.w, ub.h, static_cast<float>(ub.w) / static_cast<float>(ub.h));
        Dev::PanelHelp("resize window (fits monitor, centered):");
        struct RatioPreset
        {
            const char* label;
            float ratio;
        };
        static const RatioPreset presets[] = {
            {"32:9", 32.0f / 9.0f}, {"21:9", 21.0f / 9.0f}, {"16:9", 16.0f / 9.0f}, {"16:10", 16.0f / 10.0f},
            {"3:2", 3.0f / 2.0f},   {"4:3", 4.0f / 3.0f},   {"5:4", 5.0f / 4.0f},
        };
        for (int i = 0; i < static_cast<int>(sizeof(presets) / sizeof(presets[0])); ++i)
        {
            if (i > 0 && i != 4) // row break before "3:2"
                Dev::PanelSameLine();
            if (Dev::Button(presets[i].label))
            {
                const float r = presets[i].ratio;
                Defer([r] { ResizeWindowToRatio(r); });
            }
        }
        Dev::PanelSeparator();
    }

    changed |= Dev::Checkbox("Override enabled", &live.overrideEnabled);
    Dev::PanelSameLine();
    Dev::PanelHelp("(off = display.cfg policy)");
    Dev::PanelSeparator();

    int style = static_cast<int>(live.style);
    if (Dev::Combo("Display style", &style, "Modern\0Legacy\0"))
    {
        live.style = (style == 1) ? AspectRatio::Legacy : AspectRatio::Modern;
        changed = true;
    }
    int clamp = static_cast<int>(live.clamp);
    if (Dev::Combo("Ultrawide clamp", &clamp,
                   "Off\0"
                   "21:9\0"
                   "16:9\0"))
    {
        live.clamp = static_cast<AspectRatio::UltrawideClamp>(clamp);
        changed = true;
    }

    Dev::PanelSeparator();
    changed |= Dev::Checkbox("Pillarbox  (crop world to band + black bars)", &live.pillarbox);
    changed |= Dev::Checkbox("HUD clamp  (center UI in band, world full)", &live.hudClamp);

    Dev::PanelSeparator();
    changed |= Dev::Checkbox("Manual viewport (noodle)", &live.manualRect);
    changed |= Dev::SliderFloat("rect Left", &live.rectL, 0.0f, 1.0f, "%.3f");
    changed |= Dev::SliderFloat("rect Top", &live.rectT, 0.0f, 1.0f, "%.3f");
    changed |= Dev::SliderFloat("rect Right", &live.rectR, 0.0f, 1.0f, "%.3f");
    changed |= Dev::SliderFloat("rect Bottom", &live.rectB, 0.0f, 1.0f, "%.3f");
    if (Dev::Button("Reset rect to full"))
    {
        live.rectL = 0.0f;
        live.rectT = 0.0f;
        live.rectR = 1.0f;
        live.rectB = 1.0f;
        live.manualRect = false;
        changed = true;
    }

    if (changed)
        AspectReapply();

    Dev::PanelSeparator();
    if (GEngine)
    {
        Poseidon::AspectSettings a;
        GEngine->GetAspectSettings(a);
        ImGui::Text("viewport   %d x %d", GEngine->Width(), GEngine->Height());
        ImGui::Text("FOV        L=%.3f  T=%.3f", a.leftFOV, a.topFOV);
        ImGui::Text("UI rect    x[%.3f..%.3f] y[%.3f..%.3f]", a.uiTopLeftX, a.uiBottomRightX, a.uiTopLeftY,
                    a.uiBottomRightY);
        ImGui::Text("world rect x[%.3f..%.3f] y[%.3f..%.3f]", a.worldLeft, a.worldRight, a.worldTop, a.worldBottom);
    }
}

// "Reset to defaults" for any settings struct that has a default-constructed instance to copy
// from -- which is every settings tab. Owner request (2026-08-17): the buttons that existed sat at
// the bottom of tabs long enough to need scrolling (Amb. Occlusion, Interior Sky) or were missing
// (Material Debug, Culling, auto exposure), and were named differently on every tab. This one goes
// at the TOP of each tab under the same label. The struct's own initialisers are the source of
// truth, so a new field is covered the moment it exists. Returns true when pressed; the caller's
// existing `changed` path pushes the struct.
template <typename T>
bool ResetToDefaultsButton(T& settings, const char* label = "Reset to defaults",
                           const char* note = "(every control on this tab back to the engine's built-in defaults)")
{
    const bool pressed = Dev::Button(label);
    if (pressed)
        settings = T{};
    if (note != nullptr && *note != 0)
    {
        Dev::PanelSameLine();
        Dev::PanelHelp("%s", note);
    }
    return pressed;
}

void DrawGrassTab()
{
    if (!GEngine)
    {
        Dev::PanelHelp("No engine.");
        return;
    }

    struct GrassMapChoice
    {
        const char* label;
        const char* worldKey;
    };
    // CWA's legacy internal world names differ from their displayed island
    // names: Eden is Everon, Abel is Malden, Cain is Kolgujev, and Noe is
    // Nogova. Keep both names visible so this tool works with the actual
    // installed WRP files, not a guessed display name.
    static constexpr GrassMapChoice maps[] = {
        {"Intro", "Intro"},          {"Everon (Eden)", "Eden"}, {"Malden (Abel)", "Abel"},
        {"Kolgujev (Cain)", "Cain"}, {"Nogova (Noe)", "Noe"},
    };
    static int selectedMap = 0;
    static int selectedSurface = 0;
    static std::string observedWorld;

    // The mission header can remain on the intro world while an in-game map
    // has already switched. TerrainWgpu records the actual WRP it uploaded,
    // which is the only name safe to use for grass layer selection.
    const char* loadedMapName = GEngine->GetGrassLoadedMapName();
    const std::string activeMapFile = loadedMapName && *loadedMapName ? loadedMapName : Glob.header.worldname;
    std::string activeWorld = activeMapFile;
    const size_t lastSlash = activeWorld.find_last_of("\\/");
    if (lastSlash != std::string::npos)
        activeWorld.erase(0, lastSlash + 1);
    const size_t extension = activeWorld.find_last_of('.');
    if (extension != std::string::npos)
        activeWorld.erase(extension);
    if (activeWorld != observedWorld)
    {
        observedWorld = activeWorld;
        selectedSurface = 0;
        for (int i = 0; i < static_cast<int>(std::size(maps)); ++i)
        {
            if (strcmpi(activeWorld.c_str(), maps[i].worldKey) == 0)
            {
                selectedMap = i;
                break;
            }
        }
    }

    Engine::GrassSettings grass = GEngine->GetGrassSettings();
    bool changed = false;

    changed |= ResetToDefaultsButton(grass);
    if (Dev::Button("Copy grass settings"))
    {
        char copy[4096];
        snprintf(copy, sizeof(copy),
                 "Grass settings\n"
                 "enabled=%d; photoClumpCards=%d; clumpRenderer=%d; castShadows=%d; applyFog=%d; ignoreExclusions=%d\n"
                 "density=%.3f; densityBoost=%.3f; spacing=%.3f; nearRadius=%.3f; midRadius=%.3f; farRadius=%.3f\n"
                 "densityNoiseScale=%.4f; densityNoiseStrength=%.3f; height=%.3f; bladeWidth=%.3f; "
                 "bladeTextureStrength=%.3f\n"
                 "saturation=%.3f; dryPatches=%.3f; dryPatchScale=%.4f; weedPercent=%.3f; flowerPercent=%.3f\n"
                 "shapeVariety=%.3f; taperJitter=%.3f; bendJitter=%.3f; bladeArch=%.3f; clumping=%.3f; "
                 "colorVariation=%.3f; transmission=%.3f\n"
                 "alphaCards=%d; alphaCutoff=%.3f; cardWiden=%.3f\n"
                 "photoBrightness=%.3f; photoMix=%.3f; photoPatchSize=%.3f\n"
                 "photoContrast=%.3f; photoContour=%.3f; photoSelfShadow=%.3f; photoRootAo=%.3f\n"
                 "photoAlphaCutoff=%.3f; photoSaturation=%.3f; photoForceLayer=%d; lodBlend=%.2f\n"
                 "nearDensity=%.3f; midDensity=%.3f; farDensity=%.3f\n"
                 "tintProcedural=%.2f/%.2f/%.2f; tintPhoto=%.2f/%.2f/%.2f\n"
                 "photoLayerWeights=%.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f\n"
                 "liveWind=%d; windStrength=%.3f; windDirection=%.3f\n"
                 "photoCoverage=%.2f (auto=%d); bladeSelfShadow=%.2f; bladeContrast=%.2f; bladeHueVariation=%.2f; "
                 "bladeRootShade=%.2f; catchAllCoverage=%.2f; catchAllHeight=%.2f; photoCardScale=%.2f",
                 grass.enabled, grass.midPhotoTuft, grass.clumpRenderer, grass.castShadows, grass.applyFog,
                 grass.ignoreGeographyExclusions, grass.density, grass.densityBoost, grass.spacing, grass.radius,
                 grass.midRadius, grass.farRadius, grass.densityNoiseScale, grass.densityNoiseStrength, grass.height,
                 grass.bladeWidth, grass.bladeTextureStrength, grass.saturation, grass.dryPatches, grass.dryPatchScale,
                 grass.weedPercent, grass.flowerPercent, grass.shapeVariety, grass.taperJitter, grass.bendJitter,
                 grass.bladeArch, grass.clumping, grass.colorVariation, grass.transmission, grass.alphaCards,
                 grass.alphaCutoff, grass.cardWiden, grass.photoTuftBrightness, grass.photoTuftMix,
                 grass.photoTuftPatchSize, grass.photoContrast, grass.photoContour, grass.photoSelfShadow,
                 grass.photoRootAo, grass.photoAlphaCutoff, grass.photoSaturation, grass.photoForceLayer,
                 grass.lodBlend, grass.nearDensity, grass.midDensity, grass.farDensity, grass.tintProcedural[0],
                 grass.tintProcedural[1], grass.tintProcedural[2], grass.tintPhoto[0], grass.tintPhoto[1],
                 grass.tintPhoto[2], grass.photoLayerWeights[0], grass.photoLayerWeights[1], grass.photoLayerWeights[2],
                 grass.photoLayerWeights[3], grass.photoLayerWeights[4], grass.photoLayerWeights[5],
                 grass.photoLayerWeights[6], grass.photoLayerWeights[7], grass.useLiveWind, grass.windStrength,
                 grass.windDirection, grass.photoCoverage, grass.photoCoverageAuto ? 1 : 0, grass.bladeSelfShadow,
                 grass.bladeContrast, grass.bladeHueVariation, grass.bladeRootShade, grass.catchAllCoverage,
                 grass.catchAllHeight, grass.photoCardScale);
        ImGui::SetClipboardText(copy);
    }
    Dev::PanelSameLine();
    Dev::PanelHelp("Copies a pasteable snapshot for sharing with Codex.");

    Dev::PanelHeading("Map and terrain surface");
    ImGui::SetNextItemWidth(220.0f);
    if (Dev::BeginCombo("Map", maps[selectedMap].label))
    {
        for (int i = 0; i < static_cast<int>(std::size(maps)); ++i)
        {
            const bool selected = selectedMap == i;
            if (ImGui::Selectable(maps[i].label, selected))
                selectedMap = i;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    const bool selectedMapLoaded = strcmpi(activeWorld.c_str(), maps[selectedMap].worldKey) == 0;
    Dev::PanelSameLine();
    const bool canSwitchMap = GWorld != nullptr && GWorld->GetMode() == GModeIntro;
    bool loadMap = false;
    if (canSwitchMap)
    {
        ImGui::BeginDisabled(selectedMapLoaded);
        loadMap = Dev::Button("Load selected map");
        ImGui::EndDisabled();
    }
    else if (!selectedMapLoaded && GWorld != nullptr)
    {
        loadMap = Dev::Button("Force-load selected map (dev)");
    }
    if (loadMap)
    {
        // Resolve through CfgWorlds. This is the same authoritative mapping as
        // mission loading and handles modded/relocated WRP paths correctly.
        const RString resolvedWorld = GetWorldName(maps[selectedMap].worldKey);
        const std::string worldFile = static_cast<const char*>(resolvedWorld);
        SetVisible(false);
        // Landscape switching invalidates textures and the scene, so do it only
        // after ImGui has finished this frame, exactly like the reload control.
        Defer(
            [worldFile]
            {
                if (GWorld != nullptr)
                    GWorld->SwitchLandscape(worldFile.c_str());
            });
    }
    Dev::PanelHelp("Active terrain WRP: %s. Everon uses the internal name Eden.", activeMapFile.c_str());
    const RString selectedWorldFile = GetWorldName(maps[selectedMap].worldKey);
    Dev::PanelHelp("Selected WRP: %s", static_cast<const char*>(selectedWorldFile));
    if (!canSwitchMap && !selectedMapLoaded)
        Dev::PanelHelp("Force-load replaces the active mission landscape; use it only for grass testing.");

    // The terrain combo is deliberately separate from the map combo. It always
    // comes from the loaded map, so an Everon selection cannot accidentally
    // apply Eden layer indices to the active geography texture.
    const int surfaceCount = GEngine->GetGrassSurfaceCount();
    if (surfaceCount == 0)
    {
        Dev::PanelHelp("Loading map terrain materials...");
    }
    else
    {
        selectedSurface = std::clamp(selectedSurface, 0, surfaceCount - 1);
        ImGui::SetNextItemWidth(390.0f);
        if (Dev::BeginCombo("Terrain surface", GEngine->GetGrassSurfaceName(selectedSurface)))
        {
            for (int i = 0; i < surfaceCount; ++i)
            {
                const bool selected = selectedSurface == i;
                char label[512];
                snprintf(label, sizeof(label), "%d: %s", i, GEngine->GetGrassSurfaceName(i));
                if (ImGui::Selectable(label, selected))
                    selectedSurface = i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        bool selectedEnabled = GEngine->IsGrassSurfaceEnabled(selectedSurface);
        if (Dev::Checkbox("Spawn on selected terrain", &selectedEnabled))
            GEngine->SetGrassSurfaceEnabled(selectedSurface, selectedEnabled);
        Dev::PanelSameLine();
        if (Dev::Button("Use selected only"))
        {
            for (int i = 0; i < surfaceCount; ++i)
                GEngine->SetGrassSurfaceEnabled(i, i == selectedSurface);
        }
        if (Dev::Button("Clear all surfaces"))
        {
            for (int i = 0; i < surfaceCount; ++i)
                GEngine->SetGrassSurfaceEnabled(i, false);
        }
        Dev::PanelSameLine();
        if (Dev::Button("Enable all surfaces"))
        {
            for (int i = 0; i < surfaceCount; ++i)
                GEngine->SetGrassSurfaceEnabled(i, true);
        }
        Dev::PanelHelp("Select a material, then toggle it. The selector contains every terrain layer of this map.");
    }

    Dev::PanelSeparator();
    Dev::PanelHelp("GPU-generated terrain blades. Placement follows the terrain grass pass and excludes water, "
                   "roads, forests and buildings.");
    Dev::PanelSeparator();
    changed |= Dev::Checkbox("Enabled", &grass.enabled);
    changed |= Dev::Checkbox("Cast close grass shadows", &grass.castShadows);
    changed |= Dev::Checkbox("Apply grass distance fog", &grass.applyFog);
    Dev::PanelHelp("Both are grass-only visual controls; turn either off to inspect the procedural field.");
    changed |= Dev::Checkbox("Ignore terrain exclusions (diagnostic)", &grass.ignoreGeographyExclusions);
    Dev::PanelHelp("Relaxes only the FOREST flags, which some legacy Everon WRPs set across ordinary ground. "
                   "Water, roads, tracks and buildings stay excluded either way -- there is no setting that "
                   "puts grass on a road.");
    changed |= Dev::SliderFloat("Coverage", &grass.density, 0.05f, 2.0f, "%.2f");
    Dev::PanelHelp("Default 0.50. Up to 1.00 retains more candidates; above 1.00 adds candidates on a finer "
                   "near/mid grid. Extra density increases GPU cost.");
    changed |= Dev::SliderFloat("Coverage: near", &grass.nearDensity, 0.02f, 1.0f, "%.2f");
    Dev::PanelHelp("Multiplies the master coverage for the near ring only. Procedural blades: the near clump "
                   "count. Photo cards: a plain multiplier on top of the card coverage below (cards place on "
                   "their own 1.11 m clutter grid now, not the 0.16 m blade grid, so this is no longer the "
                   "overdraw lever it was).");
    changed |= Dev::SliderFloat("Coverage: mid", &grass.midDensity, 0.02f, 50.0f, "%.2f");
    changed |= Dev::SliderFloat("Coverage: far", &grass.farDensity, 0.02f, 1.0f, "%.2f");
    Dev::PanelHelp("Default mid coverage 4.00; up to 50.00. Above 1.00 adds candidates on a finer grid. "
                   "At the fixed grid budget, extra coverage broadens clumps instead of shortening the ring "
                   "or growing GPU buffers. Very dense cover increases overdraw. Far coverage retains proxy cells.");
    changed |= Dev::SliderFloat("Density boost", &grass.densityBoost, 1.0f, 64.0f, "%.1fx");
    Dev::PanelHelp("Divides the base spacing by its square root, so this is the coverage multiplier: 4x is the "
                   "old ceiling, 64x is eight times finer placement again. Cost scales with it -- watch the "
                   "benchmark table below, and note that the near instance buffer caps at 1,048,576 clumps.");
    changed |= Dev::SliderFloat("Base spacing (m)", &grass.spacing, 0.02f, 0.75f, "%.3f");
    Dev::PanelHelp(
        "Distance between candidates before the density boost; lower values make grass substantially denser. "
        "This is the ONLY near-density control -- it is independent of the radius below.");
    changed |= Dev::SliderFloat("Near clump radius (m)", &grass.radius, 8.0f, 200.0f, "%.0f");
    Dev::PanelHelp("Close-field reach. Density is unaffected: the placement grid is sized from this radius "
                   "rather than the spacing being stretched to fit a fixed grid, which is what used to make "
                   "this slider look dead past ~30 m while quietly thinning the field.");
    changed |= Dev::SliderFloat("Mid clump radius (m)", &grass.midRadius, 18.0f, 1000.0f, "%.0f");
    Dev::PanelHelp("Opaque mid-clump reach, at one fixed placement spacing, so this is also coverage-only: "
                   "the mid placement grid is sized from this radius, and mid density is identical at 400 m "
                   "and at 60 m.");
    changed |= Dev::SliderFloat("Far radius (m)", &grass.farRadius, 0.0f, 5000.0f, "%.0f");
    Dev::PanelHelp("Outer terrain-cover proxy. 0 = off. Any other value is floored "
                   "past the mid ring, and its dispatch is skipped entirely when off.");
    changed |= Dev::SliderFloat("LOD dissolve (m)", &grass.lodBlend, 0.0f, 40.0f, "%.1f");
    Dev::PanelHelp("Width of the crossfade at EVERY LOD join. Each ring thins out stochastically across this "
                   "band while the next thickens, and blades shrink as they go, so grass no longer changes "
                   "representation on a hard circle. The dissolve is world-space deterministic, so a clump "
                   "vanishes once at its own distance rather than blinking as the camera moves. 0 restores "
                   "the abrupt joins. Wider bands overlap two rings over more ground, which costs "
                   "proportionally more instances.");
    changed |= Dev::SliderFloat("Density noise scale", &grass.densityNoiseScale, 0.002f, 0.5f, "%.3f");
    Dev::PanelHelp(
        "Patch frequency in 1/metres. 0.075 gives ~13 m patches; lower = broader sweeps, higher = finer mottling.");
    changed |= Dev::SliderFloat("Density noise strength", &grass.densityNoiseStrength, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("0 = perfectly uniform coverage; 1 = bare ground between dense clumps. Applies to every LOD.");

    changed |= Dev::ColorEdit3("Tint: procedural grass", grass.tintProcedural);
    Dev::PanelHelp("Multiplies albedo before lighting, so the sun still does what it did. Covers near blades, "
                   "mid ribbons and the far proxy together. White = untinted.");
    changed |= Dev::ColorEdit3("Tint: photographed cards", grass.tintPhoto);
    Dev::PanelHelp("The same, for the photographed clump cards only. Separate from the procedural tint so the "
                   "two paths can be matched to each other rather than moving together.");
    changed |= Dev::SliderFloat("Colour saturation", &grass.saturation, 0.0f, 2.0f, "%.2f");
    Dev::PanelHelp("1.00 = untouched, 0.00 = greyscale. Pushed about the luma axis, so brightness is "
                   "unchanged -- this pulls colour out without darkening. Applies to near blades, mid "
                   "ribbons/clump cards and the far proxy together.");
    changed |= Dev::SliderFloat("Dry patches", &grass.dryPatches, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("Fraction of the field that bleaches toward dry straw. 0 = uniformly green. Tips dry "
                   "before roots, and it uses its own noise field so dry ground does not line up with "
                   "thin ground.");
    changed |= Dev::SliderFloat("Dry patch size", &grass.dryPatchScale, 0.002f, 0.3f, "%.3f");
    Dev::PanelHelp("Noise frequency in 1/metres: default 0.003 gives ~333 m patches with broad fades; higher values break the field "
                   "into smaller dry spots.");
    changed |= Dev::SliderFloat("Blade width", &grass.bladeWidth, 0.25f, 6.0f, "%.2fx");
    Dev::PanelHelp("1.00 = the long-standing look. The near-LOD blade texture only becomes visible above "
                   "roughly 3x: a stock 3 cm blade is about 4 pixels wide on screen, and a 64-pixel-wide "
                   "texture is averaged down to flat colour before it is ever drawn. Wider blades read as "
                   "broad leaves rather than fine grass, so this is a look choice, not a fix.");

    Dev::PanelSeparator();
    Dev::PanelHeading("Blade shape (procedural blades only -- ignored by photographed clump cards)");
    changed |= Dev::SliderFloat("Shape variety", &grass.shapeVariety, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("0 = the legacy look, where all FOUR grass species shared one silhouette and the field "
                   "read as the same blade repeated. 1 = eight distinct width/height/taper profiles. This "
                   "is geometry, not texture, so it costs nothing either way.");
    changed |= Dev::SliderFloat("Taper jitter", &grass.taperJitter, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("Per-blade spread on the taper exponent, so neighbouring blades of the same species do "
                   "not narrow identically. Multiplicative about 1.0: 0 leaves the chosen profile alone "
                   "rather than shifting it.");
    changed |= Dev::SliderFloat("Arch", &grass.bladeArch, 0.0f, 3.0f, "%.2f");
    Dev::PanelHelp("How far a blade arcs over, as a multiple of its own height. 0 = rigid spikes standing "
                   "to attention, which is what the stock bend gave: it moved a tip 5-19 cm on a ~0.8 m "
                   "blade, about ten degrees. Taller blades arc further, so this scales with height rather "
                   "than being a fixed distance. Costs nothing -- it is a vertex-shader curve.");
    changed |= Dev::Checkbox("GRS-030 clump renderer", &grass.clumpRenderer);
    Dev::PanelHelp("On = sparse twelve-blade meadow clumps. Off restores legacy six-ribbon grass.");
    changed |= Dev::SliderFloat("Bend jitter", &grass.bendJitter, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("Per-blade spread on the resting lean. Also multiplicative about the stock range, so "
                   "turning it up widens the spread in both directions instead of leaning the whole field "
                   "further over.");
    changed |= Dev::SliderFloat("Photo texture strength", &grass.bladeTextureStrength, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("Scales the near-LOD photo atlas on top of its distance fade. 0 keeps the procedural "
                   "surface even when photo layers are installed -- the quickest way to tell whether a look "
                   "problem is the texture or the geometry. No effect when no photo atlas is loaded.");

    Dev::PanelSeparator();
    Dev::PanelHeading("Blade shading (procedural blades and mid ribbons only -- cards ignore these)");
    Dev::PanelHelp("Defaults are the DEFINED look (self shadow 0.55, contrast 1.30, hue 0.15, root 0.78); the "
                   "legacy flat wash is 0 / 1.00 / 0 / 0.70, reachable in one go with WGR_GRASS_CONTRAST=0. "
                   "These sliders override that env default live.");
    changed |= Dev::SliderFloat("Blade self shadow", &grass.bladeSelfShadow, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("Blade-on-blade shadowing, the cheap way: occlusion toward the root of a tuft (the base of "
                   "a blade sees ~40%% of the sky a tip does at full strength) plus a sample of the cascade "
                   "the near grass itself writes, so a clump in front shades the one behind. The cascade half "
                   "needs 'Cast close grass shadows' on and fades out past 30-60 m, where a shadow texel is "
                   "bigger than a tuft. 0 = off (the old look).");
    changed |= Dev::SliderFloat("Blade contrast", &grass.bladeContrast, 0.25f, 3.0f, "%.2f");
    Dev::PanelHelp("Contrast about the palette's own mid-tone, before the tint and the lighting. 1.00 = the "
                   "untouched palette; higher separates dark roots from bright tips, lower flattens the field.");
    changed |= Dev::SliderFloat("Blade hue variation", &grass.bladeHueVariation, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("Per-patch hue drift (~11 m field of its own): warm straw one way, cool blue-green the "
                   "other, on top of the luminance-only 'Colour variation' below. 0 = none.");
    changed |= Dev::SliderFloat("Blade root shade", &grass.bladeRootShade, 0.0f, 0.95f, "%.2f");
    Dev::PanelHelp("How much darker a blade is at its root than at its tip. 0.70 is the long-standing value; "
                   "0 = a flat blade.");

    // ------------------------------------------------------------------------
    // Everything below the header up to the next separator applies ONLY while
    // photographed clump cards are on. They were previously mixed in with the
    // procedural blade-shape controls, where there was no way to tell which
    // sliders were inert for the look actually on screen.
    Dev::PanelSeparator();
    Dev::PanelHeading("PHOTOGRAPHED CLUMP CARDS");
    const bool photoClumpsAvailable = GEngine->HasGrassPhotoClumps();
    if (!photoClumpsAvailable)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                           "Photo clump assets are not installed -- procedural grass is in use.");
        Dev::PanelHelp("assets/grass/meadow-grass-clump-alpha-1024.png (or $WGR_GRASS_TUFT) was not found. "
                       "This is a supported configuration, not an error: the near ring keeps its individual "
                       "textured blades and the mid ring its procedural ribbons. The toggle and the settings "
                       "below stay editable, and take effect the moment the assets are present.");
    }
    // WHICH grass is drawn. Three sources, and the label below says which one is actually on
    // screen for THIS world -- the owner's complaint was a checkbox that read unchecked while
    // the map's cards drew, because an automatic enable bypassed it.
    {
        const bool mapClutter = GEngine->HasGrassMapClutter();
        // Saved settings that predate the selector still carry midPhotoTuft.
        if (grass.midPhotoTuft && grass.clutterSource == 0)
        {
            grass.clutterSource = 2;
            grass.midPhotoTuft = false;
            changed = true;
        }
        const char* sources[] = {"Auto: the map's own clutter where it has one, else procedural blades",
                                 "Procedural blades (own implementation) everywhere",
                                 "Photographed clump cards everywhere"};
        int src = grass.clutterSource < 0 ? 0 : (grass.clutterSource > 2 ? 2 : grass.clutterSource);
        if (Dev::Combo("Grass source", &src, sources, 3))
        {
            // No near-coverage hack on the switch any more: cards place on their own 1.11 m
            // clutter grid (fb3d83b) and have their own coverage control below, so a 0.15 near
            // coverage would now put the near ring at 0.15 against the mid ring's 0.95. A stale
            // 0.15 left by the old hack is lifted back to the default on the way through.
            const bool wasCards = grass.clutterSource == 2 || (grass.clutterSource == 0 && mapClutter);
            const bool nowCards = src == 2 || (src == 0 && mapClutter);
            if (wasCards != nowCards && (grass.nearDensity == 0.15f || grass.nearDensity == 0.40f))
                grass.nearDensity = 1.0f;
            grass.clutterSource = src;
            changed = true;
        }
        const bool cardsOnScreen = src == 2 || (src == 0 && mapClutter);
        if (cardsOnScreen)
            ImGui::TextColored(
                ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                mapClutter ? "ON SCREEN NOW: the map's own clutter (photo cards from its CfgSurfaces)"
                           : "ON SCREEN NOW: photo cards (loose primary card; this world has no clutter atlas)");
        else
            ImGui::TextColored(
                ImVec4(0.85f, 0.85f, 0.55f, 1.0f),
                mapClutter ? "ON SCREEN NOW: procedural blades (this world HAS its own clutter; Auto would draw it)"
                           : "ON SCREEN NOW: procedural blades (this world has no clutter of its own)");
    }
    Dev::PanelHelp("Photo cards are alpha-tested clump plates for close and mid grass; procedural is stable "
                   "individual textured blades nearby and ribbons in the mid ring. Everything in THIS section "
                   "does nothing unless cards are on screen.");
    // RFG-062: where the map atlas COMES FROM on a natively loaded Reforger world.
    {
        const bool nativeWorld = GEngine->HasEnfusionSurfaces();
        ImGui::BeginDisabled(!nativeWorld);
        if (Dev::Checkbox("Enfusion map clutter (native Reforger worlds)", &grass.enfusionClutter))
        {
            changed = true;
        }
        // RFG-089: the blade IMAGES, from the PlantMat atlas of the world's commonest grass.
        if (Dev::Checkbox("Enfusion blade atlas on the near ring (native Reforger worlds)", &grass.enfusionBlades))
        {
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Reforger's own ground cover is a procedural PlantMat: a blade atlas plus the\n"
                              "rectangles it picks tufts from. On, the near ring's eight blade layers are cut\n"
                              "from that atlas; off, the stock meadow photos. POSEIDON_ENFUSION_BLADES=0.");
        }
        // RFG-065. The GROUND, not the grass -- it sits here because it is the same
        // world's own `.emat` data read out of the same archives, and the panel has no
        // other home for a native-Reforger terrain switch.
        if (Dev::Checkbox("Enfusion ground material (native Reforger worlds)", &grass.enfusionGround))
        {
            changed = true;
        }
        // RFG-027 x RFG-063: the land cells under the draped road ribbons get no clutter.
        // A Landscape-side flag rather than a GrassSettings field -- the road mask is a
        // world fact -- so it is NOT written by "Save as default"; the terrain re-bakes
        // on the next frame when it moves. Does nothing on a world without native roads.
        {
            bool clearRoads = Landscape::EnfusionRoadClutterClear();
            if (Dev::Checkbox("Clutter cleared under Enfusion roads", &clearRoads))
            {
                Landscape::SetEnfusionRoadClutterClear(clearRoads);
            }
        }
        if (nativeWorld)
        {
            Dev::PanelHelp("Tiles each surface at the ScaleUV its own .emat declares (2..8 m; 36 of the 51 "
                           "shipped surfaces name one) instead of stretching one image over each 12.5 m "
                           "land cell, and crossfades in the surface's BCRMiddleMap past DetailMaxDistance. Off is "
                           "the previous ground, bit for bit -- it is a live per-frame weight, so this is an "
                           "A/B with no reload. The .emat's NHOMap and SatMapBlend are NOT bound yet; the "
                           "boot log's 'Wgpu Enfusion ground:' line counts what was found against what was "
                           "bound.");
        }
        ImGui::EndDisabled();
        if (nativeWorld)
        {
            Dev::PanelHelp("Walks the world's own ClutterConfig -> clutter set -> collection -> plant chain out of "
                           "the mounted .pak archives and builds the per-surface card atlas from it, exactly where "
                           "the Arma path uses CfgSurfaces. Off falls back to the procedural blades. Rebuilding the "
                           "atlas takes a moment on the frame the switch moves.");
        }
        else
        {
            Dev::PanelHelp("This world is not a natively loaded Enfusion (Arma Reforger) one, so the switch has "
                           "nothing to act on. A CONVERTED Reforger world is an Arma-format world and takes the "
                           "CfgSurfaces path above instead.");
        }
    }
    ImGui::BeginDisabled(!(grass.clutterSource == 2 || (grass.clutterSource == 0 && GEngine->HasGrassMapClutter())));
    changed |= Dev::SliderFloat("Photo clump brightness", &grass.photoTuftBrightness, 0.50f, 2.50f, "%.2fx");
    Dev::PanelHelp("1.00 = the calibrated source plate; raise it to lift dark clumps.");
    changed |= Dev::SliderFloat("Photo clump saturation", &grass.photoSaturation, 0.0f, 2.0f, "%.2f");
    Dev::PanelHelp("Saturation of the photographed cards about their own luma. 0.62 is the long-standing "
                   "value, 0 is greyscale, above 1 pushes past the source plate. Separate from the field-wide "
                   "Colour saturation above so the cards can be matched to the procedural grass rather than "
                   "moving with it -- the global control still applies on top.");
    changed |= Dev::SliderFloat("Photo clump contrast", &grass.photoContrast, 0.5f, 2.5f, "%.2f");
    Dev::PanelHelp("Contrast about each plate's own mean colour, which is what separates individual stems from "
                   "the mass behind them. The source photographs are flatly lit by design, so 1.00 (the raw "
                   "photograph) is exactly the flat look; higher gives the silhouette contour. The pivot is "
                   "measured per plate at load, so this cannot double as a brightness change.");
    changed |= Dev::SliderFloat("Photo clump contour lighting", &grass.photoContour, 0.0f, 2.0f, "%.2f");
    Dev::PanelHelp("Rebuilds a per-texel surface normal from the plate's luma gradient, so stems inside one "
                   "card catch the sun separately instead of the whole plate taking one value. 0 = the former "
                   "single flat upright normal. Costs four extra texture taps on card fragments.");
    changed |= Dev::SliderFloat("Photo clump self-shadowing", &grass.photoSelfShadow, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("Grass shadowing grass: the cards are already in the cascade the grass writes, so this is a "
                   "real occlusion test rather than a fake. Needs 'Cast close grass shadows' on. 1.00 is a "
                   "full-strength shadow; foliage usually wants contact contrast, not solid-black cards.");
    changed |= Dev::SliderFloat("Photo clump root occlusion", &grass.photoRootAo, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("Darkens the base of each clump, where light does not reach. This is what gives a card a "
                   "bottom -- without it a plate reads as floating on the terrain.");
    changed |= Dev::SliderFloat("Photo card alpha cutoff", &grass.photoAlphaCutoff, 0.05f, 0.95f, "%.2f");
    Dev::PanelHelp("Texels below this alpha are discarded. 0.50 is neutral -- the atlas mip chain preserves "
                   "coverage against it. Several local family plates carry JPEG opacity maps, whose "
                   "compression noise around thin stems is a genuine flicker source; raising this trims "
                   "those partial texels away. The shadow pass uses the same value.");

    Dev::PanelSeparator();
    Dev::PanelHeading("Photo card coverage (density of the card path)");
    {
        // IS THE CARD PATH EVEN DRAWING? Every control in this block is inert
        // unless it is, and nothing on screen said so -- which is exactly how an
        // afternoon goes into "the slider does nothing". Mirrors
        // EngineWgpu::SetGrassSettings' photoTuft, which is the same expression
        // the renderer evaluates.
        const bool mapAtlas = GEngine->HasGrassMapClutter();
        const bool cardsWanted = grass.clutterSource == 2 || (grass.clutterSource == 0 && mapAtlas);
        if (cardsWanted)
        {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                               "Card path ON (source %d, map atlas %d) - the controls below apply.",
                               grass.clutterSource, mapAtlas ? 1 : 0);
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                               "Card path OFF (source %d, map atlas %d) - you are looking at PROCEDURAL blades and "
                               "every control below does nothing. Set Grass source to the photo clumps.",
                               grass.clutterSource, mapAtlas ? 1 : 0);
        }
        Dev::PanelHelp("The game log settles it either way: \"near ring cards\" vs \"near ring blades\".");
    }
    changed |=
        Dev::Checkbox("Auto: the map's own clutter density where the map declares one", &grass.photoCoverageAuto);
    changed |= Dev::SliderFloat("Photo card coverage", &grass.photoCoverage, 0.01f, 1.0f, "%.2f");
    {
        // Mirrors card_coverage_for / card_placement_spacing in grass.wgsl: cards place on a
        // 1.11 m clutter grid (WGR_GRASS_CARD_SPACING) and this is the fraction of those cells
        // that grow one, so mean spacing = grid / sqrt(coverage) and density = coverage / grid^2.
        const float grid = 1.11f;
        const float c = std::clamp(grass.photoCoverage, 0.01f, 1.0f);
        Dev::PanelHelp("Coverage <-> spacing: cards sit on the map's 1.11 m clutter grid; %.2f means a card in "
                       "one cell in %.1f, i.e. one card every %.2f m on average, %.2f cards per square metre "
                       "(1.00 = every cell, the map's own maximum).",
                       c, 1.0f / c, grid / std::sqrt(c), c / (grid * grid));
    }
    // DENSITY, not spacing. The metres control this replaced ran the wrong way --
    // dragging it up made the field sparser, so "max everything out" produced less
    // grass than the default and read as the slider doing nothing.
    changed |= Dev::SliderFloat("Photo card density", &grass.photoCardDensity, 1.0f, 300.0f, "%.1fx",
                                ImGuiSliderFlags_Logarithmic);
    {
        // Live arithmetic beats a paragraph: the two densities side by side, in the
        // same unit, so "as dense as procedural" is a number you can match rather
        // than a feel you have to chase.
        const float g = std::clamp(1.11f / std::sqrt(std::max(grass.photoCardDensity, 1.0f)), 0.06f, 4.0f);
        const float cov = std::clamp(grass.photoCoverage, 0.01f, 1.0f);
        const float cards = cov / (g * g);
        const float bladeSp = grass.spacing * (grass.clumpRenderer ? 3.20f : 1.72f);
        const float blades = 1.0f / std::max(bladeSp * bladeSp, 1e-6f);
        Dev::PanelHelp("Coverage is a fraction OF this grid, so the grid is the ceiling coverage cannot pass.\n"
                       "  photo cards now: %.2f /m2   (coverage %.2f on a %.3f m grid)\n"
                       "  procedural near: %.2f /m2   (spacing %.2f x %.2f = %.3f m)\n"
                       "1x is the map's own clutterGrid; the slider goes to 300x (a 0.06 m grid).",
                       cards, cov, g, blades, grass.spacing, grass.clumpRenderer ? 3.20f : 1.72f, bladeSp);
        if (cards < blades * 0.9f)
        {
            const float need = blades / std::max(cards, 1e-6f);
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                               "Cards are %.1fx SPARSER than the procedural near ring. Set density to %.0fx to match.",
                               need, std::max(grass.photoCardDensity, 1.0f) * need);
        }
        // Mirror bounded_mid_spacing: the grid stays finite and the requested
        // reach is retained. Residual coverage broadens the rendered mid clump.
        {
            const float midWanted = std::clamp(grass.midRadius, 8.0f, 1000.0f);
            const float requestedGrid = g / std::sqrt(std::clamp(grass.density, 1.0f, 2.0f) *
                                                     std::clamp(grass.midDensity, 1.0f, 50.0f));
            const float actualGrid = std::max(requestedGrid, midWanted / 1218.0f);
            Dev::PanelHelp("Mid placement %.3f m; clump breadth %.2fx at the fixed candidate budget. "
                           "Requested reach %.0f m is preserved.", actualGrid, actualGrid / requestedGrid,
                           midWanted);
        }
        Dev::PanelHelp("Cost: above ~8x the old flicker returns (every plate edge shimmers in wind); at "
                       "0.16 m there are ~90 plates over every point of ground, which is what 'far too dense' "
                       "was. NOTE this only does anything when the card path is actually drawing -- the Grass "
                       "log line says 'near ring cards' when it is, 'blades' when it is not.");
    }
    changed |= Dev::SliderFloat("Photo card size", &grass.photoCardScale, 0.25f, 2.0f, "%.2fx");
    Dev::PanelHelp("Size of each card. 1.00 = the stock card, 0.5-2.3 m tall and wider than tall; Arma's own "
                   "clutter plants are ~0.3-0.9 m, so at the map's own density the stock card is two to three "
                   "plants' worth of plate over every clutter cell -- try ~0.5 if the field still reads as a "
                   "solid mat at the map's density. Shadows follow.");
    Dev::PanelHelp("Auto ON: wherever the map's geography bake answered (Arma worlds with a CfgSurfaces "
                   "atlas: Takistan, Stratis...) the map's OWN clutter density is used -- one card per "
                   "clutter cell thinned by the surface's authored probability, which is what Arma draws -- "
                   "and this slider is only the fallback where nothing is known (every OFP world with cards "
                   "forced on). Auto OFF: the slider multiplies everywhere, on top of the map's thinning, so "
                   "1.00 is the map's density and 0.24 is a quarter of it. 0.24 is the owner's default for "
                   "an unknown clutter density.");

    Dev::PanelSeparator();
    Dev::PanelHeading("Photographed family mix");
    changed |= Dev::SliderFloat("Photo grass mix", &grass.photoTuftMix, 0.0f, 1.0f, "%.0f%%");
    changed |= Dev::SliderFloat("Photo grass patch size (m)", &grass.photoTuftPatchSize, 4.0f, 80.0f, "%.0f");
    Dev::PanelHelp("Fraction of world-space patches that draw from the FULL variety. 0%% = ONE variety "
                   "everywhere: on a map-clutter world each surface's first clutter class, on a loose-card "
                   "world the primary clump -- the 'single variety' setting that stopped the flicker. 100%% = "
                   "every patch picks freely (a map surface's own classes, or the weighted families below). "
                   "It used to be consulted only on the loose-card path, so on Takistan and Stratis it did "
                   "nothing; both paths honour it now, shadows included.");
    // Isolating one plate is the fastest way to identify a flickering source, so
    // the override comes before the weights rather than being buried under them.
    {
        // Names come from what the renderer actually discovered in
        // assets/grass/more_grass, so a machine with a different grass pack
        // installed gets that pack's families here, and one with none gets only
        // the primary clump. Nothing about the list is fixed in code.
        const int familyCount = std::min(GEngine->GetGrassPhotoFamilyCount(), 9);
        std::vector<std::string> labels;
        labels.reserve(static_cast<size_t>(familyCount) + 1);
        labels.emplace_back("Off (weighted mix)");
        for (int i = 0; i < familyCount; ++i)
        {
            labels.push_back(std::to_string(i) + ": " + GEngine->GetGrassPhotoFamilyName(i));
        }
        std::vector<const char*> labelPtrs;
        labelPtrs.reserve(labels.size());
        for (const std::string& label : labels)
            labelPtrs.push_back(label.c_str());

        if (familyCount <= 1)
        {
            Dev::PanelHelp("No additional grass families installed. Drop a pack into "
                           "assets/grass/more_grass -- any colour plate with a matching opacity plate beside "
                           "it (albedo/opacity or diff/alpha naming) is picked up automatically, up to eight.");
        }
        int forced = std::clamp(grass.photoForceLayer, -1, familyCount - 1) + 1;
        ImGui::SetNextItemWidth(390.0f);
        if (Dev::Combo("Force one family", &forced, labelPtrs.data(), static_cast<int>(labelPtrs.size())))
        {
            grass.photoForceLayer = forced - 1;
            changed = true;
        }
        Dev::PanelHelp("Draws every photographed clump from a single atlas layer, ignoring the mix and the "
                       "weights. Step through the families to find which plate flickers, then zero its "
                       "weight below to remove just that one.");
        // The map clutter atlas has up to 32 layers behind ONE named family ("map clutter (per
        // surface)"), so the combo cannot reach them; this slider can. -1 = off. The game log's
        // "Wgpu grass atlas alpha edges" line (worst plates first) says which layer to try.
        int forcedLayer = std::clamp(grass.photoForceLayer, -1, 31);
        if (Dev::SliderInt("Force atlas layer (-1 = off, 0..31)", &forcedLayer, -1, 31))
        {
            grass.photoForceLayer = forcedLayer;
            changed = true;
        }
        Dev::PanelHelp("Same override by atlas layer number, for map-clutter worlds whose classes are not "
                       "listed above (Takistan: 26 layers). Which layer flickers is measured at upload: grep "
                       "the game log for 'Wgpu grass atlas alpha edges' -- plates are listed worst first by "
                       "speckle (isolated one-texel stem fragments) and partial-alpha fraction.");
        // Weights cover layers 1..8; layer 0 is reached through the mix slider.
        for (int i = 1; i < familyCount; ++i)
        {
            changed |= Dev::SliderFloat(labelPtrs[i + 1], &grass.photoLayerWeights[i - 1], 0.0f, 1.0f, "%.2f");
        }
        if (familyCount > 1)
        {
            Dev::PanelHelp("Relative selection weights, not percentages: they are normalised against each "
                           "other. 0 removes a family entirely. All at 0 falls back to the primary clump.");
        }
        if (Dev::Button("All families on"))
        {
            for (float& w : grass.photoLayerWeights)
                w = 1.0f;
            grass.photoForceLayer = -1;
            changed = true;
        }
        Dev::PanelSameLine();
        if (Dev::Button("Primary clump only"))
        {
            for (float& w : grass.photoLayerWeights)
                w = 0.0f;
            grass.photoForceLayer = -1;
            changed = true;
        }
    }
    ImGui::EndDisabled();

    Dev::PanelSeparator();
    Dev::PanelHeading("Alpha cut-out cards (experimental, procedural blades only)");
    changed |= Dev::Checkbox("Silhouette from texture alpha", &grass.alphaCards);
    Dev::PanelHelp("Off = the blade outline is geometry (default). On = the quad is widened and the outline "
                   "is cut out of the texture's alpha, so one card can carry several blade shapes. This is "
                   "the Reforger-style approach and it buys shape variety without more geometry -- but it "
                   "discards, which loses the early-Z the solid path relies on, and the wider quad adds "
                   "overdraw. Watch the Grass rows in the benchmark table when turning it on.");
    changed |= Dev::SliderFloat("Alpha cutoff", &grass.alphaCutoff, 0.05f, 0.95f, "%.2f");
    Dev::PanelHelp("Texels below this alpha are discarded. Lower keeps more of the blade and its soft edge; "
                   "higher trims harder and thins the silhouette. Only used when cards are on.");
    changed |= Dev::SliderFloat("Card widening", &grass.cardWiden, 1.0f, 4.0f, "%.2fx");
    Dev::PanelHelp("How much wider the quad is than the blade it draws. The cutout needs material to "
                   "remove: at 1.00x there is nothing spare and the card reads as a rectangle again. "
                   "Directly proportional to the overdraw this path costs.");

    Dev::PanelSeparator();
    Dev::PanelHeading("Species mix");
    changed |= Dev::SliderFloat("Weed %", &grass.weedPercent, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("Broad flat leaves (clover, ragged weed). Wider and shorter than grass.");
    changed |= Dev::SliderFloat("Flower %", &grass.flowerPercent, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("Daisy and poppy heads on slim untapered stems. Clamped so weed + flower never exceeds 100%%.");
    // Mirror the renderer's clamp so the readout cannot claim an impossible mix.
    {
        const float weed = std::clamp(grass.weedPercent, 0.0f, 1.0f);
        const float flower = std::clamp(grass.flowerPercent, 0.0f, 1.0f - weed);
        Dev::PanelHelp("Effective mix: %.0f%% grass, %.0f%% weed, %.0f%% flower. Chosen per clump (~25 m "
                       "patches), so these are area fractions, not per-blade odds.",
                       (1.0f - weed - flower) * 100.0f, weed * 100.0f, flower * 100.0f);
    }
    changed |= Dev::SliderFloat("Blade height", &grass.height, 0.10f, 3.0f, "%.2fx");
    changed |= Dev::Checkbox("Use live world wind", &grass.useLiveWind);
    changed |= Dev::SliderFloat("Wind strength", &grass.windStrength, 0.0f, 3.0f, "%.2f");
    changed |= Dev::SliderFloat("Wind direction", &grass.windDirection, -180.0f, 180.0f, "%.0f deg");
    Dev::PanelHelp("Live wind follows weather. Disable it to test a manual direction; 0 degrees points east (+X).");
    changed |= Dev::Checkbox("Auto gust speed (from wind)", &grass.windScrollAuto);
    changed |= Dev::SliderFloat("Wind gust speed", &grass.windScroll, 0.02f, 4.0f, "%.2fx");
    Dev::PanelTooltip("How fast gusts travel across the field, NOT how far blades bend.\n"
                      "With Auto on (the shipped behaviour) this follows the live wind and tops out at\n"
                      "1.00 in a storm, so the game never goes above it; calm air scrolls at 0.15.\n"
                      "The slider is the manual override and the only way past 1.00.\n"
                      "Why it matters: the scroll speeds in the shader are constants unrelated to the\n"
                      "wind speed, so at 1.00 the gust field crosses the world at ~48 m/s against a\n"
                      "6 m/s wind -- which reads as shivering rather than leaning and releasing.");
    changed |= Dev::Checkbox("Travelling gust waves", &grass.windGustWaves);
    changed |= Dev::SliderFloat("Local gust variation", &grass.windGustVariation, 0.0f, 1.0f, "%.2f");
    changed |= Dev::SliderFloat("Gust front size", &grass.windGustSize, 10.0f, 200.0f, "%.0f m");
    Dev::PanelHelp("Waves travel with the wind, with stronger and weaker patches across a meadow. "
                   "Size sets their length along the wind; fronts extend three times farther across it. "
                   "Variation 0 gives even sway. Disable waves for the previous round gust patches.");
    changed |= Dev::SliderFloat("Field clumping", &grass.clumping, 0.0f, 1.0f, "%.2f");
    changed |= Dev::SliderFloat("Colour variation", &grass.colorVariation, 0.0f, 1.0f, "%.2f");
    changed |= Dev::SliderFloat("Backlight transmission", &grass.transmission, 0.0f, 1.0f, "%.2f");
    const float requestedNearRadius = std::clamp(grass.radius, 8.0f, 200.0f);
    // Mirrors EngineWgpu::SetGrassSettings. Deliberately NOT a function of the
    // radius any more: near density is radius-independent.
    const float effectiveSpacing =
        std::clamp(grass.spacing / std::sqrt(std::clamp(grass.densityBoost, 1.0f, 64.0f)), 0.02f, 0.75f);
    const float nearDetailRadius = requestedNearRadius;
    const float midReach = std::clamp(grass.midRadius, nearDetailRadius + 10.0f, 1000.0f);
    {
        // Mirrors the renderer's grid sizing so the panel can say plainly when a
        // radius/density pair has hit the 1536-cell candidate grid, which is the
        // one case where the field stops short of the requested radius.
        const float clumpSpacing = effectiveSpacing * (grass.clumpRenderer ? 3.20f : 1.72f);
        const float wantCells = std::ceil(2.0f * nearDetailRadius / clumpSpacing);
        const float cells = std::clamp(wantCells, 64.0f, 1536.0f);
        const float reach = cells * clumpSpacing * 0.5f;
        Dev::PanelHelp("Near placement: %.0f cm clump spacing, %.0f x %.0f candidate grid.", clumpSpacing * 100.0f,
                       cells, cells);
        if (wantCells > 1536.0f)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                               "Candidate grid capped: near grass reaches ~%.0f m of the requested %.0f m at this "
                               "density. Lower the density boost to reach further.",
                               reach, nearDetailRadius);
    }
    if (grass.farRadius <= 0.0f)
        Dev::PanelHelp("LOD field: near clumps %.0f m, mid clumps to %.0f m, no far ring.", nearDetailRadius, midReach);
    else
    {
        const float farReach = std::clamp(grass.farRadius, midReach + 8.0f, 5000.0f);
        Dev::PanelHelp("LOD field: near clumps %.0f m, mid clumps to %.0f m, far cover %.0f-%.0f m.", nearDetailRadius,
                       midReach, midReach, farReach);
    }
    Dev::PanelSeparator();
    Dev::PanelHeading("Walked imprints (both grass modes)");
    changed |= Dev::SliderFloat("Imprint lifetime (s)", &grass.trackLifetime, 5.0f, 3600.0f, "%.0f");
    Dev::PanelHelp("How long a footprint survives; it recovers over the last third, so a trail thins out "
                   "rather than blinking away. The record is a ring of 256 stamps consumed by DISTANCE "
                   "walked, not by time, so standing still no longer erases the trail you just made -- which "
                   "is what used to make imprints expire far sooner than their stated lifetime.");
    changed |= Dev::SliderFloat("Imprint depth", &grass.imprintDepth, 0.0f, 0.95f, "%.2f");
    Dev::PanelHelp("How far a crushed plant is pressed down, as a fraction of its own height. 0.55 is the "
                   "long-standing value. Photographed clump cards scale this up internally by 1.45x and "
                   "rotate 1.35x further: one plate stands for a whole clump nearly two metres across, so "
                   "tipping it the ~30 degrees that flattens a blade would leave an obviously upright bush. "
                   "The shadow pass uses the same numbers, so a flattened clump casts a flattened shadow.");
    Dev::PanelHelp("Wind: travelling direction field plus local gusts; roots stay pinned.");

    Dev::PanelSeparator();
    Dev::PanelHeading("Catch-all worlds (one terrain material, no clutter classification)");
    {
        // Mirrors EngineWgpu::SetGrassSettings: a world whose terrain is ONE material (a converted
        // Enfusion world such as Reforger's Everon) has no per-surface clutter answer, so the whole
        // map is grass and the stock density/height read as a knee-high meadow everywhere. These
        // scale coverage and height there only. Proof in the game log: "Wgpu grass catch-all".
        const int materials = GEngine->GetGrassSurfaceCount();
        if (materials == 1)
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                               "ACTIVE on this world: 1 terrain material -> coverage x%.2f, blade height x%.2f "
                               "(WGR_GRASS_CATCHALL=0 disables)",
                               grass.catchAllCoverage, grass.catchAllHeight);
        else
            Dev::PanelHelp("Not active here: this world has %d terrain materials, so its own surface "
                           "classification decides where grass grows. The two values below are 1.0-neutral.",
                           materials);
    }
    changed |= Dev::SliderFloat("Catch-all coverage", &grass.catchAllCoverage, 0.02f, 1.0f, "%.2f");
    Dev::PanelHelp("Multiplies the master coverage on a catch-all world. 0.24 is the owner's default for an "
                   "unknown clutter density; 1.00 = the stock field.");
    changed |= Dev::SliderFloat("Catch-all blade height", &grass.catchAllHeight, 0.10f, 1.0f, "%.2fx");
    Dev::PanelHelp("Multiplies 'Blade height' on a catch-all world. 0.55 turns the waist-high uniform meadow "
                   "into ankle-to-shin grass; 1.00 = the stock height.");

    Dev::PanelSeparator();
    if (Dev::Button("Reset to defaults##grassBottom"))
    {
        grass = Engine::GrassSettings{};
        changed = true;
    }
    Dev::PanelSameLine();
    if (Dev::Button("Ultra dense"))
    {
        grass.enabled = true;
        grass.density = 1.0f;
        grass.densityBoost = 4.0f;
        grass.spacing = 0.20f;
        grass.radius = 30.0f;
        grass.midRadius = 100.0f;
        grass.farRadius = 172.0f;
        grass.densityNoiseScale = 0.075f;
        grass.densityNoiseStrength = 0.55f;
        grass.bladeWidth = 1.0f;
        grass.saturation = 0.78f;
        grass.dryPatches = 0.35f;
        grass.dryPatchScale = 0.006f;
        grass.weedPercent = 0.12f;
        grass.flowerPercent = 0.05f;
        grass.shapeVariety = 1.0f;
        grass.taperJitter = 0.35f;
        grass.bendJitter = 0.30f;
        grass.bladeTextureStrength = 1.0f;
        grass.alphaCards = false;
        grass.alphaCutoff = 0.5f;
        grass.cardWiden = 1.6f;
        grass.bladeArch = 1.0f;
        grass.photoTuftBrightness = 1.25f;
        grass.midPhotoTuft = false;
        grass.clutterSource = 0;
        grass.photoTuftMix = 0.0f;
        grass.photoTuftPatchSize = 18.0f;
        grass.photoContrast = 1.35f;
        grass.photoContour = 1.85f;
        grass.photoSelfShadow = 1.0f;
        grass.photoRootAo = 0.45f;
        grass.photoAlphaCutoff = 0.5f;
        grass.photoSaturation = 0.62f;
        grass.trackLifetime = 600.0f;
        grass.imprintDepth = 0.55f;
        grass.photoForceLayer = -1;
        for (float& w : grass.photoLayerWeights)
            w = 1.0f;
        grass.lodBlend = 30.0f;
        grass.nearDensity = 1.0f;
        grass.midDensity = 0.95f;
        grass.farDensity = 1.0f;
        for (int i = 0; i < 3; ++i)
        {
            grass.tintProcedural[i] = 1.0f;
            grass.tintPhoto[i] = 1.0f;
        }
        grass.height = 1.25f;
        grass.useLiveWind = true;
        grass.windStrength = 1.2f;
        grass.windScroll = 1.0f;
        grass.windScrollAuto = true;
        grass.clumping = 0.55f;
        grass.colorVariation = 0.35f;
        grass.transmission = 0.10f;
        grass.castShadows = true;
        grass.applyFog = true;
        changed = true;
    }
    Dev::PanelSameLine();
    if (Dev::Button("Max coverage"))
    {
        // Deliberately expensive: the top of every coverage control, so the cost
        // of the new density ceiling can be measured in the table below rather
        // than guessed at. Radius is short because the candidate grid is finite --
        // at this spacing a longer radius is what hits the cap.
        grass.enabled = true;
        grass.density = 1.0f;
        grass.densityBoost = 16.0f;
        grass.spacing = 0.10f;
        grass.radius = 60.0f;
        grass.midRadius = 400.0f;
        grass.farRadius = 1200.0f;
        grass.densityNoiseStrength = 0.20f;
        grass.lodBlend = 30.0f;
        grass.nearDensity = 1.0f;
        grass.midDensity = 1.0f;
        grass.farDensity = 1.0f;
        changed = true;
    }
    Dev::PanelSameLine();
    if (Dev::Button("Disable"))
    {
        grass.enabled = false;
        changed = true;
    }

    if (changed)
        GEngine->SetGrassSettings(grass);

    // GRS-A — grass benchmark panel, mirroring the Water tab's WTR-002 table.
    // Both GPU timings and instance counts are harvested asynchronously, so they
    // lag the displayed frame by the readback ring depth (~2-3 frames).
    Dev::PanelSeparator();
    Dev::PanelHeading("Benchmark (GRS-A)");

    Engine::GrassStatsOut stats;
    if (GEngine->GetGrassStats(stats) &&
        ImGui::BeginTable("grsCounts", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("LOD");
        ImGui::TableSetupColumn("instances");
        ImGui::TableSetupColumn("candidates");
        ImGui::TableSetupColumn("accepted");
        ImGui::TableSetupColumn("vertices");
        ImGui::TableHeadersRow();
        struct Row
        {
            const char* name;
            unsigned instances, candidates, vertices;
        };
        const Row rows[] = {
            {"Near", stats.nearInstances, stats.nearCandidates, stats.nearVertices},
            {"Mid", stats.midInstances, stats.midCandidates, stats.midVertices},
            {"Far", stats.farInstances, stats.farCandidates, stats.farVertices},
        };
        unsigned totalInstances = 0, totalVertices = 0;
        for (const Row& r : rows)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Dev::PanelHeading(r.name);
            ImGui::TableNextColumn();
            ImGui::Text("%u", r.instances);
            ImGui::TableNextColumn();
            ImGui::Text("%u", r.candidates);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f%%", r.candidates ? 100.0 * r.instances / r.candidates : 0.0);
            ImGui::TableNextColumn();
            ImGui::Text("%u", r.vertices);
            totalInstances += r.instances;
            totalVertices += r.vertices;
        }
        ImGui::EndTable();
        ImGui::Text("Total: %u instances, %u vertices submitted", totalInstances, totalVertices);
        Dev::PanelTooltip("Vertices are the geometry the colour pass submits. The prepass "
                          "re-submits near+mid, and the shadow pass re-submits near, so the "
                          "frame's true grass vertex load is higher than this row.");
        if (stats.farInstances == 0 && grass.farRadius > 0.0f)
            Dev::PanelHelp("Far ring is on but empty — check geography exclusions for this map.");
    }

    float gpuMs[64];
    const int gpuRegions = GEngine->GetWaterGpuTimings(gpuMs, 64);
    if (gpuRegions <= 0)
    {
        Dev::PanelHelp("GPU timings unavailable (adapter lacks TIMESTAMP_QUERY / non-wgpu backend).");
    }
    else if (ImGui::BeginTable("grsGpuTimings", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        float gpuTotal = 0.0f;
        const int last = std::min(gpuRegions, (int)Engine::kGrassGpuRegionEnd);
        for (int i = (int)Engine::kGrassGpuRegionBegin; i < last; ++i)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Dev::PanelHeading(GEngine->GetWaterGpuTimingName(i));
            ImGui::TableNextColumn();
            if (gpuMs[i] < 0.0f)
                ImGui::TextDisabled("n/a");
            else
            {
                ImGui::Text("%.3f ms", gpuMs[i]);
                gpuTotal += gpuMs[i];
            }
        }
        ImGui::EndTable();
        ImGui::Text("Measured grass total: %.3f ms", gpuTotal);
        Dev::PanelTooltip("Placement rows are exact (standalone compute passes). The draw "
                          "rows need TIMESTAMP_QUERY_INSIDE_PASSES and read \"n/a\" without "
                          "it, because grass draws share a render pass with the rest of the "
                          "3D plan. Passes can overlap on the GPU, so this is not wall-clock.");
    }

    Dev::PanelHelp("A/B a change: note instances + ms here, apply the change, compare. "
                   "Keep the camera still -- placement is camera-relative.");
}

void DrawFoliageTab()
{
    if (!GEngine)
    {
        Dev::PanelHelp("No engine.");
        return;
    }

    Dev::PanelHelp("Emulated leaf subsurface scattering for alpha-tested vegetation (wgpu).");
    Dev::PanelHelp("Evens out the hard lit/dark split on low-poly canopy at harsh sun angles.");
    Dev::PanelHelp("Stage 1: applies to every alpha-tested cutout section.");
    Dev::PanelSeparator();

    Engine::FoliageSettings f = GEngine->GetFoliageSettings();
    bool changed = false;
    if (ResetToDefaultsButton(f))
    {
        // ResetToDefaultsButton copies a default-constructed struct; the dusk curve lives
        // outside that struct (see FoliageDusk.hpp), so it is reset by hand here.
        Poseidon::SetFoliageDuskCurve(Poseidon::kFoliageDuskCurveDefault);
        changed = true;
    }

    changed |= Dev::SliderFloat("Transmission", &f.transScale, 0.0f, 2.0f, "%.2f");
    Dev::PanelHelp("  DICE fast-SSS: light through the leaf, lifting the dark/backlit side (0 = off)");
    changed |= Dev::SliderFloat("Distortion", &f.distortion, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  bends the transmitted light toward the normal; higher = broader wrap-around");
    changed |= Dev::SliderFloat("Transmission power", &f.transPower, 1.0f, 16.0f, "%.1f");
    Dev::PanelHelp("  lobe tightness; higher = a smaller, sharper backlit glow near the sun");
    changed |= Dev::SliderFloat("Wrap", &f.wrap, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  softens the front terminator; 0 = hard Lambert (lit side stays unchanged)");
    // Range starts at 0, not 0.5: on DayZ-era canopy zero is a real look the owner prefers
    // (see FoliageSettings::ambientBoost), and a slider that cannot reach it hides that.
    changed |= Dev::SliderFloat("Ambient boost", &f.ambientBoost, 0.0f, 4.0f, "%.2f");
    Dev::PanelHelp("  sky-ambient multiplier for foliage only (1 = off); fades with distance");
    // FOLIAGE-DUSK. Sits next to Ambient boost because it decides WHEN that boost (and the
    // foliage night floor) is allowed to fade: both are gated on `daylight`, which collapses
    // before the geometric sunset and used to black out trees in the golden hour. Not part of
    // FoliageSettings only because Engine.hpp was out of bounds for this change --
    // Poseidon::FoliageDuskCurve() is the storage, and the reset buttons below cover it.
    float duskCurve = Poseidon::FoliageDuskCurve();
    if (Dev::SliderFloat("Dusk curve", &duskCurve, 0.05f, 1.0f, "%.2f"))
    {
        Poseidon::SetFoliageDuskCurve(duskCurve);
        changed = true;
    }
    Dev::PanelHelp("  exponent on `daylight` for the two foliage-only ambient terms above.");
    Dev::PanelHelp("  1.00 = the pre-2026-09 look exactly (trees go dark during golden hour);");
    Dev::PanelHelp("  lower holds the evening up. Night is identical at every setting (0^k = 0).");
    changed |= Dev::SliderFloat("GI (ambient x light)", &f.giStrength, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  scale ambient by terrain light level so shadowed foliage stops glowing (0 = off)");
    changed |= Dev::SliderFloat("Fill fade end (m)", &f.fillFadeEnd, 0.0f, 1000.0f, "%.0f");
    Dev::PanelHelp("  distance where fill + ambient boost fade out (distant foliage -> plain sky-ambient; 0 = never)");

    Dev::PanelSeparator();
    Dev::PanelHelp("Spherical canopy normals (GPU-driven path; leaf sections only)");
    changed |= Dev::SliderFloat("Bush bend", &f.normalBend, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  bend bush leaf normals toward a radial crown normal so the blob shades round (0 = off)");
    changed |= Dev::SliderFloat("Bush crown Y (m)", &f.crownYOffset, -5.0f, 5.0f, "%.2f");
    Dev::PanelHelp("  lift the bush crown centre up into the canopy (bounding-sphere centre sits a bit low)");
    changed |= Dev::SliderFloat("Tree bend", &f.treeBend, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  same, for trees — applies only to leaf/canopy sections; the solid trunk is untouched");
    changed |= Dev::SliderFloat("Tree crown Y (m)", &f.treeCrownY, -2.0f, 12.0f, "%.2f");
    Dev::PanelHelp("  lift the tree crown centre up above the trunk (the centre sits mid-trunk)");

    Dev::PanelSeparator();
    if (Dev::Button("Reset foliage to defaults"))
    {
        f = Engine::FoliageSettings{};
        Poseidon::SetFoliageDuskCurve(Poseidon::kFoliageDuskCurveDefault);
        changed = true;
    }
    Dev::PanelSameLine();
    if (Dev::Button("Disable (zero strengths)"))
    {
        f.transScale = 0.0f;
        f.wrap = 0.0f;
        f.ambientBoost = 1.0f;
        f.normalBend = 0.0f;
        // "Disable" means the legacy response, not the softened one: k = 1 is the untouched
        // daylight ramp the shader had before FOLIAGE-DUSK.
        Poseidon::SetFoliageDuskCurve(1.0f);
        changed = true;
    }

    if (changed)
        GEngine->SetFoliageSettings(f);

    Dev::PanelSeparator();
    Dev::PanelHelp("Current tuning (copy back to share):");
    char summary[256];
    snprintf(summary, sizeof(summary),
             "foliage: trans=%.2f dist=%.2f transPow=%.1f wrap=%.2f amb=%.2f dusk=%.2f gi=%.2f fadeEnd=%.0f | "
             "bush=%.2f/%.2f tree=%.2f/%.2f",
             f.transScale, f.distortion, f.transPower, f.wrap, f.ambientBoost, Poseidon::FoliageDuskCurve(),
             f.giStrength, f.fillFadeEnd, f.normalBend, f.crownYOffset, f.treeBend, f.treeCrownY);
    ImGui::SetNextItemWidth(-1.0f);
    Dev::InputText("##foliageSummary", summary, sizeof(summary), ImGuiInputTextFlags_ReadOnly);
    if (Dev::Button("Copy foliage summary to clipboard"))
        ImGui::SetClipboardText(summary);
}

void DrawShadowsTab()
{
    if (!GEngine)
    {
        Dev::PanelHelp("No engine.");
        return;
    }

    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    bool changed = false;
    changed |= ResetToDefaultsButton(t);

    Dev::PanelHelp("Depth-buffer shadow maps (durable fix).");
    Dev::PanelHelp("OFF = legacy projected shadows. ON = light-space shadow map (no flicker).");
    Dev::PanelSeparator();

    changed |= Dev::Checkbox("Enabled (shadow maps)", &t.enabled);
    Dev::PanelSameLine();
    Dev::PanelHelp(t.enabled ? "(projected path skipped)" : "(projected path active)");

    Dev::PanelSeparator();

    changed |= Dev::SliderFloat("Darkness", &t.darkness, 0.0f, 1.0f, "%.3f");
    Dev::PanelHelp("  lit-colour multiplier where shadowed; lower = darker (1.0 = no shadow)");

    changed |= Dev::SliderInt("Cascades", &t.cascadeCount, 1, 4);
    Dev::PanelHelp("  total tiers (omni + frustum); more = crisper across distance");

    changed |= Dev::SliderFloat("Distance coef", &t.distanceCoef, 0.05f, 1.0f, "%.3f");
    Dev::PanelHelp("  frustum-tier far distance as a fraction of view distance (1.0 = full VD)");

    changed |= Dev::SliderFloat("Shadow distance (m)", &t.shadowDistance, 0.0f, 1500.0f, "%.0f");
    Dev::PanelHelp("  explicit cascade reach, decoupled from the 250 m clamp (0 = use game slider)");

    changed |= Dev::SliderFloat("Split coef", &t.splitCoef, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  PSSM blend: 0 = uniform splits, 1 = logarithmic (FP 0.95)");

    Dev::PanelSeparator();
    Dev::PanelHelp("Omni near tiers — camera-centred spheres, all-direction coverage");
    Dev::PanelHelp("(so a caster behind/beside you still casts a shadow into view)");
    changed |= Dev::SliderInt("Omni tiers", &t.omniCount, 0, t.cascadeCount);
    Dev::PanelHelp("  leading tiers fit a sphere around the camera (0 = pure frustum)");
    changed |= Dev::SliderFloat("Omni radius 0", &t.omniCoef0, 0.02f, 0.5f, "%.3f");
    changed |= Dev::SliderFloat("Omni radius 1", &t.omniCoef1, 0.02f, 0.8f, "%.3f");
    Dev::PanelHelp("  sphere radii as a fraction of the shadow range (ascending)");

    changed |= Dev::SliderFloat("Bias base", &t.biasBase, 0.0f, 0.0005f, "%.6f");
    Dev::PanelHelp("  per-cascade depth bias base*(i+1)^2; raise to kill acne");

    changed |= Dev::SliderFloat("Normal offset", &t.normalOffset, 0.0f, 4.0f, "%.2f");
    Dev::PanelHelp("  receiver push toward the light in world texels; raise to kill acne (wgpu)");

    changed |= Dev::SliderFloat("PCF spread", &t.pcf, 0.0f, 3.0f, "%.2f");
    Dev::PanelHelp("  Fixed mode: < 0.5 = bilinear tap; otherwise a 3x3 tent (wgpu)");
    changed |= Dev::SliderInt("Contact shadows: 0 fixed / 1 full / 2 budget", &t.contactShadows, 0, 2);
    changed |= Dev::SliderFloat("Sun angular radius (degrees)", &t.sunAngularRadius, 0.0f, 8.0f, "%.3f");
    Dev::PanelHelp("0.266 = physical sun; larger values are experimental artistic area-source settings.");
    Dev::PanelHelp("  Experimental PCSS modes follow caster/receiver separation. Reset restores budget mode 2.");

    changed |= Dev::SliderFloat("Caster LOD bias", &t.casterLodBias, 1.0f, 8.0f, "%.1f");
    Dev::PanelHelp("  casters pick their LOD as if this many times farther away");

    changed |= Dev::SliderFloat("Far fade (m)", &t.fadeRange, 1.0f, 120.0f, "%.1f");
    Dev::PanelHelp("  distant shadows dissolve over this band instead of a hard cut-off");

    static const int resOptions[] = {512, 1024, 2048, 4096};
    int resIdx = 2;
    for (int i = 0; i < 4; ++i)
        if (resOptions[i] == t.resolution)
            resIdx = i;
    if (Dev::Combo("Resolution", &resIdx,
                   "512\0"
                   "1024\0"
                   "2048\0"
                   "4096\0"))
    {
        t.resolution = resOptions[resIdx];
        changed = true;
    }
    Dev::PanelHelp("  per-cascade depth-map size; higher = sharper, more VRAM");

    Dev::PanelSeparator();
    Dev::PanelHelp("Terrain sun-shadows (wgpu)");
    changed |= Dev::Checkbox("Enabled (terrain sun-shadow)", &t.terrainShadowEnabled);
    changed |= Dev::SliderFloat("Strength", &t.terrainShadowStrength, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  occlusion scale; 1 = physical");
    changed |= Dev::SliderInt("Mask supersample", &t.terrainShadowScale, 1, 8);
    Dev::PanelHelp("  mask resolution vs heightmap");
    changed |= Dev::SliderInt("March steps", &t.terrainShadowSteps, 16, 2048);
    Dev::PanelHelp("  hard range cap");
    changed |= Dev::SliderFloat("Penumbra (deg)", &t.terrainShadowPenumbra, 0.0f, 8.0f, "%.2f");
    Dev::PanelHelp("  soft-edge half-width; 0 = hard, larger = softer");

    Dev::PanelSeparator();
    Dev::PanelHelp("Terrain sky-visibility AO (wgpu) — darkens AMBIENT in valleys/gorges/coves");
    changed |= Dev::Checkbox("Enabled (sky-visibility AO)", &t.terrainSkyVisEnabled);
    changed |= Dev::SliderFloat("SkyVis strength", &t.terrainSkyVisStrength, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  how strongly occluded columns lose ambient (0 = off)");
    changed |= Dev::SliderFloat("SkyVis contrast", &t.terrainSkyVisContrast, 1.0f, 12.0f, "%.1f");
    Dev::PanelHelp("  occ = 1-pow(V,contrast); smooth terrain gives V~1, so raise this to see it");
    changed |= Dev::SliderFloat("SkyVis floor", &t.terrainSkyVisFloor, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  minimum ambient kept where fully occluded (never fully black)");
    changed |= Dev::SliderFloat("SkyVis radius (m)", &t.terrainSkyVisRadius, 50.0f, 2000.0f, "%.0f");
    Dev::PanelHelp("  horizon-scan reach; larger = distant ridges occlude too (re-runs the scan)");
    changed |= Dev::SliderInt("SkyVis azimuths", &t.terrainSkyVisAzimuths, 4, 32);
    Dev::PanelHelp("  scan direction count; more = smoother (re-runs the scan)");
    changed |= Dev::SliderInt("SkyVis downsample", &t.terrainSkyVisDownsample, 1, 4);
    Dev::PanelHelp("  mask coarseness vs heightmap; 1 = sharpest cliffs, slower (re-runs the scan)");
    changed |= Dev::Checkbox("SkyVis debug (greyscale factor)", &t.terrainSkyVisDebug);
    Dev::PanelHelp("  terrain shows the contrast-shaped sky-view factor for tuning");

    Dev::PanelSeparator();
    if (Dev::Button("Reset knobs to defaults"))
    {
        const bool keepEnabled = t.enabled;
        t = Engine::ShadowMapTuning{};
        t.enabled = keepEnabled;
        changed = true;
    }

    if (changed)
        GEngine->SetShadowMapTuning(t);

    // Read-back: a one-line summary the user can copy and paste back so the
    // values they tuned by eye can be baked into the engine defaults.
    Dev::PanelSeparator();
    Dev::PanelHelp("Current tuning (copy back to share):");
    char summary[512];
    snprintf(summary, sizeof(summary),
             "shadows: enabled=%s darkness=%.3f cascades=%d omni=%d/%.3f/%.3f dist=%.3f shadowDist=%.0f split=%.2f "
             "bias=%.6f normOfs=%.2f pcf=%.2f lodBias=%.1f fade=%.1f res=%d | terrain: on=%s str=%.2f scale=%d "
             "steps=%d pen=%.2f | skyvis: on=%s str=%.2f contrast=%.1f floor=%.2f radius=%.0f az=%d ds=%d",
             t.enabled ? "true" : "false", t.darkness, t.cascadeCount, t.omniCount, t.omniCoef0, t.omniCoef1,
             t.distanceCoef, t.shadowDistance, t.splitCoef, t.biasBase, t.normalOffset, t.pcf, t.casterLodBias,
             t.fadeRange, t.resolution, t.terrainShadowEnabled ? "true" : "false", t.terrainShadowStrength,
             t.terrainShadowScale, t.terrainShadowSteps, t.terrainShadowPenumbra,
             t.terrainSkyVisEnabled ? "true" : "false", t.terrainSkyVisStrength, t.terrainSkyVisContrast,
             t.terrainSkyVisFloor, t.terrainSkyVisRadius, t.terrainSkyVisAzimuths, t.terrainSkyVisDownsample);
    ImGui::SetNextItemWidth(-1.0f);
    Dev::InputText("##shadowSummary", summary, sizeof(summary), ImGuiInputTextFlags_ReadOnly);
    if (Dev::Button("Copy summary to clipboard"))
        ImGui::SetClipboardText(summary);
}

// Screen-space ambient occlusion (GTAO) — its own tab rather than a section buried under
// Shadows, because the two things you actually do here are A/B the effect on and off and flip
// between the lit result and the raw buffer, and both need to be one click away.
void DrawAmbientOcclusionTab()
{
    if (!GEngine)
    {
        return;
    }
    Engine::AoSettings ao = GEngine->GetAoSettings();
    bool changed = false;
    changed |= ResetToDefaultsButton(ao);

    Dev::PanelHelp("Short-range AO from the depth+normal prepass: local folds, corners,");
    Dev::PanelHelp("and the contact between objects and the ground. Ambient term only.");
    Dev::PanelSeparator();

    changed |= Dev::Checkbox("Enabled", &ao.enabled);
    Dev::PanelHelp("  the A/B: toggle this and watch corners and object bases");
    changed |= Dev::Checkbox("Directional ambient (bent normal)", &ao.bentNormal);
    Dev::PanelHelp("  Stage 2: light the ambient from the direction that is actually OPEN,");
    Dev::PanelHelp("  not from the surface normal. Changes where light comes from, not just");
    Dev::PanelHelp("  how much — this is the one that gives shaded surfaces form.");
    changed |= Dev::Combo("Raw buffer view", &ao.debugMode, "Off (lit scene)\0AO (greyscale)\0Bent normal (RGB)\0");
    Dev::PanelHelp("  Off        = the normal lit scene in colour, AO folded into ambient");
    Dev::PanelHelp("  AO         = the visibility buffer; white = open, dark = occluded");
    Dev::PanelHelp("  Bent normal= the direction light arrives from, as colour. Use this to");
    Dev::PanelHelp("               see what 'Directional ambient' is actually doing.");

    Dev::PanelSeparator();
    Dev::PanelHelp("Coverage");
    changed |= Dev::SliderFloat("Radius (m)", &ao.radius, 0.1f, 10.0f, "%.2f");
    Dev::PanelHelp("  world-space reach. ~1 m = tight contact shadow, 2-4 m = room corners");
    changed |= Dev::SliderFloat("Max radius (px)", &ao.maxRadiusPixels, 8.0f, 512.0f, "%.0f");
    Dev::PanelHelp("  cost clamp. WATCH THIS: whenever it bites it SHORTENS the radius above,");
    Dev::PanelHelp("  so a low value looks exactly like 'AO does nothing' up close");
    changed |= Dev::SliderFloat("Strength", &ao.strength, 0.1f, 4.0f, "%.2f");
    Dev::PanelHelp("  exponent on visibility; 1 = physical, higher = deeper");

    Dev::PanelSeparator();
    Dev::PanelHelp("Sample budget (no TAA here, so this is the whole per-frame budget)");
    changed |= Dev::SliderInt("Slices", &ao.slices, 1, 8);
    Dev::PanelHelp("  azimuthal directions per pixel");
    changed |= Dev::SliderInt("Steps", &ao.steps, 2, 32);
    Dev::PanelHelp("  horizon march steps per slice. Raise this BEFORE widening the blur;");
    Dev::PanelHelp("  a wide radius with few steps steps over small occluders");
    changed |= Dev::SliderInt("Mip march (0 = off)", &ao.maxMip, 0, 6);
    Dev::PanelHelp("  0 = every tap full-res (stable). Higher = more reach close to a");
    Dev::PanelHelp("  surface, but FLICKERS while moving — no TAA here to absorb it.");
    changed |= Dev::SliderFloat("Thickness", &ao.thickness, 0.05f, 8.0f, "%.2f");
    Dev::PanelHelp("  falloff past the radius; too low and thin poles shadow the sky behind them");

    Dev::PanelSeparator();
    Dev::PanelHelp("Bilateral denoise (this replaces temporal filtering entirely)");
    changed |= Dev::SliderFloat("Blur radius", &ao.blurRadius, 0.0f, 16.0f, "%.1f");
    Dev::PanelHelp("  too wide washes out the contact darkening that is the point");
    changed |= Dev::SliderFloat("Depth reject", &ao.blurDepthScale, 1.0f, 128.0f, "%.0f");
    Dev::PanelHelp("  higher = stops harder at silhouettes");
    changed |= Dev::SliderFloat("Normal reject", &ao.blurNormalPower, 1.0f, 32.0f, "%.0f");
    Dev::PanelHelp("  higher = stops harder at creases (keeps wall/floor contact)");

    Dev::PanelSeparator();
    Dev::PanelHelp("GPU cost (last completed frame)");
    {
        float gpuMs[64];
        const int regions = GEngine->GetWaterGpuTimings(gpuMs, 64);
        if (regions <= (int)Engine::kGtaoGpuRegionBegin)
        {
            Dev::PanelHelp("Unavailable (adapter lacks TIMESTAMP_QUERY / non-wgpu backend).");
        }
        else if (ImGui::BeginTable("aoGpuTimings", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            float total = 0.0f;
            const int end = std::min(regions, (int)Engine::kGtaoGpuRegionEnd);
            for (int i = (int)Engine::kGtaoGpuRegionBegin; i < end; ++i)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                Dev::PanelHeading(GEngine->GetWaterGpuTimingName(i));
                ImGui::TableNextColumn();
                if (gpuMs[i] < 0.0f)
                    ImGui::TextDisabled("n/a");
                else
                {
                    ImGui::Text("%.3f ms", gpuMs[i]);
                    total += gpuMs[i];
                }
            }
            const float frame = gpuMs[(int)Engine::kFrameGpuRegionTotal];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Dev::PanelHeading("GPU frame total");
            ImGui::TableNextColumn();
            if (frame < 0.0f)
                ImGui::TextDisabled("n/a");
            else
                ImGui::Text("%.3f ms", frame);
            ImGui::EndTable();
            ImGui::Text("AO: %.3f ms", total);
            if (frame > 0.0f)
            {
                Dev::PanelSameLine();
                Dev::PanelHelp("(%.1f%% of the GPU frame)", 100.0f * total / frame);
            }
            Dev::PanelTooltip("Split three ways because they answer different questions: prep "
                              "is fixed setup (partly shared with occlusion culling), the "
                              "horizon march scales with slices x steps, and the blur scales "
                              "with its radius. Toggle Enabled off and watch the frame total "
                              "for the honest delta — passes can overlap, so this sum is an "
                              "upper bound on what disabling AO gives back.");
        }
    }

    Dev::PanelSeparator();
    if (Dev::Button("Reset AO to defaults"))
    {
        const bool keepEnabled = ao.enabled;
        ao = Engine::AoSettings{};
        ao.enabled = keepEnabled;
        changed = true;
    }

    if (changed)
    {
        GEngine->SetAoSettings(ao);
    }
}

// Interior sky visibility (LIT-020) — its own tab for the same reason AO has one: the two things
// you actually do here are A/B the effect and flip to the raw reach buffer, and both have to be
// one click away. Judging this through full lighting is much harder than looking at the buffer.
// REN-GI-001: the irradiance probe volume.
void DrawGiTab()
{
    if (!GEngine)
    {
        return;
    }
    Engine::GiSettings gi = GEngine->GetGiSettings();
    bool changed = false;
    changed |= ResetToDefaultsButton(gi);
    Dev::PanelHelp("Hybrid GI, stage 1: a grid of light probes around the camera (32 x 8 x 32,");
    Dev::PanelHelp("spacing below). Each probe integrates sky light through the interior-sky");
    Dev::PanelHelp("dome maps and the sun's first bounce off the terrain; shading blends it");
    Dev::PanelHelp("over the analytic sky-dome ambient. Direct sun is untouched. WGR_GI=0 off.");
    Dev::PanelSeparator();
    changed |= Dev::Checkbox("Enabled", &gi.enabled);
    {
        // REN-GI-009. SH-2 is the default: measured better on every shaded surface and
        // identical on every lit one, for about 0.2 ms. SH-1 is kept only because it is a
        // recorded negative result (REN-GI-008) -- it loses 4.9% on an outdoor shaded wall.
        const char* kBasis[] = {"Ambient cube (6 faces)", "SH-1 (worse -- see REN-GI-008)",
                                "SH-2 (default)"};
        int basis = std::clamp(gi.basis, 0, 2);
        if (ImGui::Combo("Probe basis", &basis, kBasis, IM_ARRAYSIZE(kBasis)))
        {
            gi.basis = basis;
            changed = true;
        }
        Dev::PanelHelp("  how each probe stores the light it gathered, and what the shading can");
        Dev::PanelHelp("  read back out of it for a given surface normal");
    }
    changed |= ImGui::SliderFloat("Weight", &gi.weight, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  0 = the old ambient exactly, 1 = the probes alone");
    changed |= ImGui::SliderFloat("Interior AO share", &gi.interiorMix, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  how much of the per-pixel interior-sky darkening still applies on top");
    changed |= ImGui::SliderFloat("Indoor sky occlusion", &gi.indoorSky, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  REN-GI-010. A probe's rays are tested against the terrain and, going UP,");
    Dev::PanelHelp("  against the interior-sky dome maps -- never sideways against a wall. So a");
    Dev::PanelHelp("  probe in a room took full sky from every horizontal direction and lit the");
    Dev::PanelHelp("  interior almost as brightly as the field outside. This darkens each probe");
    Dev::PanelHelp("  by how much sky it can see at its own position. 0 = the old look exactly.");
    changed |= ImGui::SliderFloat("Spacing (m)", &gi.spacing, 1.0f, 16.0f, "%.1f");
    changed |= ImGui::SliderInt("Rays per probe update", &gi.rays, 8, 64);
    changed |= ImGui::SliderFloat("Hysteresis", &gi.hysteresis, 0.05f, 1.0f, "%.2f");
    Dev::PanelHelp("  per-update blend toward the fresh integral; low = smoother, slower");
    changed |= ImGui::ColorEdit3("Ground albedo", gi.groundAlbedo);
    changed |= ImGui::SliderFloat("Ground bounce gain", &gi.groundGain, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("Wall/roof albedo", &gi.wallAlbedo, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Ray length (m)", &gi.rayLength, 8.0f, 256.0f, "%.0f");
    Dev::PanelSeparator();
    Dev::PanelHelp("Sun proxy (stage 2): a map of every sunlit surface seen from the sun, with");
    Dev::PanelHelp("colour; each probe gathers the first sun bounce from a disc of it.");
    changed |= ImGui::SliderInt("Sun-proxy samples", &gi.rsmSamples, 0, 64);
    Dev::PanelHelp("  0 turns the sun bounce off");
    changed |= ImGui::SliderFloat("Sun-proxy radius (m)", &gi.rsmRadius, 1.0f, 40.0f, "%.0f");
    changed |= ImGui::SliderFloat("Sun-bounce gain", &gi.rsmGain, 0.0f, 4.0f, "%.2f");
    if (changed)
    {
        GEngine->SetGiSettings(gi);
    }
}

void DrawInteriorSkyTab()
{
    if (!GEngine)
    {
        return;
    }
    Engine::InteriorSkySettings is = GEngine->GetInteriorSkySettings();
    bool changed = false;
    changed |= ResetToDefaultsButton(is);

    Dev::PanelHelp("Tells the renderer it is INDOORS: a top-down depth map of the object");
    Dev::PanelHelp("scene. Geometry above a surface removes its SKY AMBIENT, toward a floor.");
    Dev::PanelHelp("Direct sun (shadow maps) and local lights are never touched.");
    Dev::PanelSeparator();

    changed |= Dev::Checkbox("Enabled (per-frame maps, Stage 1)", &is.enabled);
    Dev::PanelHelp("  the A/B: stand in a doorway and toggle. Hotkey: Ctrl+Shift+I");
    changed |= Dev::Checkbox("Baked volumes (Stage 2)", &is.baked);
    Dev::PanelHelp("  the other implementation: occlusion resolved per MODEL, so its edges");
    Dev::PanelHelp("  follow the building instead of a camera-space grid — which is what");
    Dev::PanelHelp("  caused the shadow patches. Hotkey: Ctrl+Shift+B.");
    Dev::PanelHelp("  Needs WGR_SKY_BAKE_VOLUMES=1 at STARTUP to produce the volumes; this");
    Dev::PanelHelp("  switch only decides whether shading reads them. Both can be on, but");
    Dev::PanelHelp("  compare them one at a time.");
    changed |= Dev::Checkbox("Reach buffer (greyscale)", &is.debug);
    Dev::PanelHelp("  white = open sky above, black = fully roofed. Applies to whichever of");
    Dev::PanelHelp("  the two is on. Tune against THIS,");
    Dev::PanelHelp("  not against the lit scene. Hotkey: Ctrl+Shift+O");
    Dev::PanelHelp("  (both work with this panel closed — Ctrl+` reopens it)");

    Dev::PanelSeparator();
    Dev::PanelHelp("Look (Strength and Floor apply to BOTH implementations)");
    changed |= Dev::SliderFloat("Strength", &is.strength, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  0 = inert, 1 = full attenuation down to the floor");
    changed |= Dev::SliderFloat("Floor", &is.floorLevel, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  minimum ambient in a sealed room. NOT optional — OFP interiors carry");
    Dev::PanelHelp("  almost no local lights, so 0 here is a black box you cannot play in.");
    changed |= Dev::SliderFloat("Kernel (m)", &is.kernel, 0.0f, 8.0f, "%.2f");
    Dev::PanelHelp("  softening radius: roughly how far light appears to reach in past an");
    Dev::PanelHelp("  opening. This is what grades a porch instead of drawing a hard line.");
    changed |= Dev::SliderFloat("Directional", &is.directional, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("  0 = the room just gets darker. 1 = the ambient arrives FROM the");
    Dev::PanelHelp("  opening, so the wall facing the window is brighter than the wall");
    Dev::PanelHelp("  beside it. This is the knob that reads as 'light through the window'.");

    Dev::PanelSeparator();
    Dev::PanelHelp("Map (cost + resolving power)");
    changed |= Dev::SliderInt("Resolution", &is.resolution, 256, 4096);
    changed |= Dev::SliderFloat("Extent (m, half-box)", &is.extent, 32.0f, 512.0f, "%.0f");
    Dev::PanelHelp("  %.2f m per texel. Roofs and walls need well under a metre; window",
                   is.resolution > 0 ? (2.0f * is.extent / float(is.resolution)) : 0.0f);
    Dev::PanelHelp("  reveals are NOT reachable here at any setting (that is the Stage 2 bake).");
    changed |= Dev::SliderFloat("Height (m)", &is.height, 32.0f, 1024.0f, "%.0f");
    Dev::PanelHelp("  how far above/below the camera the box reaches; must clear the tallest");
    Dev::PanelHelp("  roof you can stand under");
    // The tilted maps look along a ~50 deg slant, so a point `extent` away laterally sits
    // extent*sin(50) along their view axis and falls out of the depth slab once that passes
    // `height`. The zenith map is unaffected, so the symptom is subtle: window light quietly
    // stops working at range while the roofs still darken correctly.
    const float tiltReach = is.extent * 0.766f; // sin(50 deg)
    if (tiltReach > is.height)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "  ! extent too large for height: the TILTED maps clip past %.0f m", is.height / 0.766f);
        Dev::PanelHelp("  raise Height above %.0f, or lower Extent, or the window-light", tiltReach);
        Dev::PanelHelp("  directions silently stop contributing at range.");
    }
    changed |= Dev::SliderFloat("Bias (m)", &is.bias, 0.0f, 4.0f, "%.2f");
    Dev::PanelHelp("  too low: open ground occludes itself (the whole world dims). Too high:");
    Dev::PanelHelp("  light leaks in under thin roofs.");

    Dev::PanelSeparator();
    Dev::PanelHelp("GPU cost (last completed frame)");
    {
        float gpuMs[64];
        const int regions = GEngine->GetWaterGpuTimings(gpuMs, 64);
        if (regions <= (int)Engine::kInteriorSkyGpuRegionBegin)
        {
            Dev::PanelHelp("Unavailable (adapter lacks TIMESTAMP_QUERY / non-wgpu backend).");
        }
        else if (ImGui::BeginTable("isGpuTimings", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            float total = 0.0f;
            const int end = std::min(regions, (int)Engine::kInteriorSkyGpuRegionEnd);
            for (int i = (int)Engine::kInteriorSkyGpuRegionBegin; i < end; ++i)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                Dev::PanelHeading(GEngine->GetWaterGpuTimingName(i));
                ImGui::TableNextColumn();
                if (gpuMs[i] < 0.0f)
                    ImGui::TextDisabled("n/a");
                else
                {
                    ImGui::Text("%.3f ms", gpuMs[i]);
                    total += gpuMs[i];
                }
            }
            // The frame total is the number that decides whether this is affordable, so show it
            // next to the feature's own cost rather than making the reader hunt another tab.
            const float frame = gpuMs[(int)Engine::kFrameGpuRegionTotal];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Dev::PanelHeading("GPU frame total");
            ImGui::TableNextColumn();
            if (frame < 0.0f)
                ImGui::TextDisabled("n/a");
            else
                ImGui::Text("%.3f ms", frame);
            ImGui::EndTable();
            ImGui::Text("Interior sky: %.3f ms", total);
            if (frame > 0.0f)
            {
                Dev::PanelSameLine();
                Dev::PanelHelp("(%.1f%% of the GPU frame)", 100.0f * total / frame);
            }
            Dev::PanelTooltip("Sum of the rows above, from the last completed frame. Toggle "
                              "Enabled off and watch the frame total to get the honest "
                              "delta: passes can overlap on the GPU, so this sum is an "
                              "upper bound on what disabling the feature gives back.");
        }
    }

    Dev::PanelSeparator();
    Dev::PanelHelp("Is the map SEEING anything? An empty map and a working one look identical");
    Dev::PanelHelp("in the picture: an untouched texel means 'open sky', so a map that drew");
    Dev::PanelHelp("nothing simply leaves every surface unshaded and reads as success.");
    if (Dev::Button("Measure map coverage now (to the log)"))
    {
        GEngine->ProbeInteriorSkyMap();
    }
    Dev::PanelHelp("  writes two lines: 'Interior sky probe at [x z y]' (how many retained");
    Dev::PanelHelp("  instances the ENGINE has inside the box, and the nearest three), and");
    Dev::PanelHelp("  '[wgr] interior sky maps (requested)' (what fraction of each of the five");
    Dev::PanelHelp("  depth layers holds an occluder). Together they say which half is at");
    Dev::PanelHelp("  fault: no instances = the building never reached the GPU-driven path;");
    Dev::PanelHelp("  instances but 0.00%% coverage = the cull or the draw. The startup line");
    Dev::PanelHelp("  fires ~2 s in and measures the LOADING SCREEN -- press this where you");
    Dev::PanelHelp("  are standing instead.");

    Dev::PanelSeparator();
    if (Dev::Button("Reset interior sky to defaults"))
    {
        const bool keepEnabled = is.enabled;
        is = Engine::InteriorSkySettings{};
        is.enabled = keepEnabled;
        changed = true;
    }

    if (changed)
    {
        GEngine->SetInteriorSkySettings(is);
    }
}

// Live anti-aliasing knobs — MSAA sample count, SSAA render scale and
// alpha-to-coverage apply at the next frame boundary, so the effect is
// visible immediately while hunting for the shipped default.
// Live frame-phase breakdown from the always-on FrameProfiler ring
// (World::Simulate marks setup/draw/hud/ai+veh/sound/swap each frame).
void DrawPerfTab()
{
    Dev::FrameProfiler& perf = Dev::GFrameProfiler();
    const int frames = perf.FrameCount();
    if (frames == 0)
    {
        Dev::PanelHelp("no frames recorded yet");
        return;
    }

    const Dev::FrameProfiler::PhaseStats total = perf.TotalStats();
    ImGui::Text("FPS %.1f", perf.AvgFps());
    Dev::PanelSameLine();
    Dev::PanelHelp("frame %.2f ms avg / %.2f p95 / %.2f max (last %d frames)", total.avgMs, total.p95Ms, total.maxMs,
                   frames);

    // REN-THR-015 -- the render thread, switchable and watchable. The switch takes effect at
    // the next frame boundary, never mid-frame: the engine will not start or stop a worker
    // while one holds the renderer handle.
    if (GEngine)
    {
        const Engine::RenderThreadInfo rt = GEngine->GetRenderThreadInfo();
        if (rt.available)
        {
            ImGui::Separator();
            bool on = rt.enabled;
            if (Dev::Checkbox("Render thread", &on))
                GEngine->SetRenderThreadEnabled(on);
            Dev::PanelSameLine();
            Dev::PanelHelp("Runs the drains, the frame publish and wgr_render_frame on a worker. LOCKSTEP: "
                           "the producer still WAITS for each frame, so this is a stack change and not "
                           "overlap -- expect the same framerate, not a better one. Proven equivalent: "
                           "901 of 901 scene frames identical to the inline path.");

            const float simMs = total.avgMs - rt.workerBusyMs + rt.overlapMs;
            ImGui::Text("Simulation         %6.1f ms", simMs < 0.0f ? 0.0f : simMs);
            ImGui::Text("Render thread      %6.1f ms", rt.workerBusyMs);
            if (rt.overlapAvailable)
            {
                bool ov = rt.overlapEnabled;
                if (Dev::Checkbox("Overlap sim and render", &ov))
                    GEngine->SetRenderOverlapEnabled(ov);
                Dev::PanelSameLine();
                Dev::PanelHelp("Lets the simulation run ahead instead of waiting for each frame. The ring "
                               "is two deep, so it may lead by at most one frame; past that the producer "
                               "blocks and that block shows up as Queue waiting below.");
            }
            ImGui::Text("Sim/Render overlap %6.1f ms", rt.overlapMs);
            Dev::PanelSameLine();
            Dev::PanelHelp("Worker time the producer did NOT wait for. Zero in lockstep by construction "
                           "-- it is derived as busy minus wait, so it cannot disagree with the two "
                           "numbers above it.");
            ImGui::Text("Render lag         %4.0f / %d frames", rt.lagFrames, 1);
            ImGui::Text("Queue waiting      %6.1f ms", rt.queueWaitMs);
            ImGui::Text("Window opened late %6.2f ms, %4.2f/frame by %s", rt.lazyWaitMs, rt.lazyWaitsPerFrame,
                        rt.lazyOpener != nullptr ? rt.lazyOpener : "none");
            Dev::PanelSameLine();
            Dev::PanelHelp("How long the producer BLOCKED. In lockstep that is the whole render, because "
                           "it waits for all of it. With overlap on it is only what the renderer could "
                           "not absorb -- so above zero here means the renderer cannot keep up, and lag "
                           "sitting at 1 with waiting at 0 means it is a frame behind but keeping pace.");
            ImGui::Spacing();
            ImGui::Text("Render thread      %5.0f %%", 100.0f * rt.workerBusyFrac);
            ImGui::Text("Main thread        %5.0f %%", 100.0f * rt.mainBusyFrac);
            Dev::PanelSameLine();
            Dev::PanelHelp("Main EXCLUDES the time it sits blocked on the worker, so in lockstep it reads "
                           "low and the render thread reads high. That is the mode working, not a stall.");
            {
                float     gpuMs[Engine::kGpuRegionEnd] = {};
                const int gpuCount = GEngine->GetWaterGpuTimings(gpuMs, Engine::kGpuRegionEnd);
                // ONE region, not a sum. The first version of this line added every region
                // together, and several of them are CONTAINERS that already include their
                // children -- so it counted the same GPU work two and three times and
                // reported 209%. A percentage above 100 is not a small inaccuracy, it is a
                // readout saying it does not know what it is measuring. `kFrameGpuRegionTotal`
                // is all submitted frame work and needs no arithmetic.
                const int frameRegion = static_cast<int>(Engine::kFrameGpuRegionTotal);
                if (gpuCount > frameRegion && gpuMs[frameRegion] > 0.0f && total.avgMs > 0.001f)
                {
                    ImGui::Text("GPU                %5.0f %%", 100.0f * gpuMs[frameRegion] / total.avgMs);
                    Dev::PanelSameLine();
                    Dev::PanelHelp("Submitted GPU work as a share of the CPU frame. It can legitimately "
                                   "exceed 100%% when the GPU is the slower side and the CPU is waiting "
                                   "on it -- that is what being GPU-bound looks like from here.");
                }
            }
            ImGui::Separator();
        }
    }

    static float history[Dev::FrameProfiler::kRingSize];
    const int n = frames;
    for (int i = 0; i < n; i++)
        history[i] = perf.Frame(n - 1 - i).totalMs; // oldest → newest
    ImGui::PlotLines("##frametimes", history, n, 0, "frame ms", 0.f, total.maxMs * 1.2f, ImVec2(-1, 64));

    if (ImGui::BeginTable("phases", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("phase");
        ImGui::TableSetupColumn("avg ms");
        ImGui::TableSetupColumn("p95 ms");
        ImGui::TableSetupColumn("max ms");
        ImGui::TableSetupColumn("% frame");
        ImGui::TableHeadersRow();
        for (int p = 0; p < Dev::FrameProfiler::PhaseCount; p++)
        {
            const auto s = perf.Stats(static_cast<Dev::FrameProfiler::Phase>(p));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Dev::PanelHeading(Dev::FrameProfiler::PhaseName(p));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", s.avgMs);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", s.p95Ms);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", s.maxMs);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f%%", total.avgMs > 0.001f ? 100.f * s.avgMs / total.avgMs : 0.f);
        }
        ImGui::EndTable();
    }
    ImGui::Text("draw calls %.0f avg", perf.AvgDrawCalls());
    {
        const Dev::LightCounters& lights = Dev::GLightCounters();
        ImGui::Text("local lights %u / %u candidates (cap %u)", lights.selected, lights.candidates, lights.cap);
    }
    if (GEngine)
    {
        const int swapInterval = GEngine->GetSwapInterval();
        ImGui::Text("Presentation: %s", swapInterval == 0  ? "VSync off"
                                        : swapInterval < 0 ? "Adaptive VSync"
                                                           : "VSync on");
        float gpuMs[64];
        const int gpuRegions = GEngine->GetWaterGpuTimings(gpuMs, 64);
        if (gpuRegions > Engine::kFrameGpuRegionTotal && gpuMs[Engine::kFrameGpuRegionTotal] >= 0.0f)
            ImGui::Text("GPU submitted frame %.2f ms (excludes present wait)", gpuMs[Engine::kFrameGpuRegionTotal]);
        if (gpuRegions >= Engine::kTerrainGpuRegionEnd)
        {
            const float terrainPrepass = gpuMs[Engine::kTerrainGpuRegionBegin];
            const float terrainColor = gpuMs[Engine::kTerrainGpuRegionBegin + 1];
            if (terrainPrepass >= 0.0f)
                ImGui::Text("Main terrain prepass %.3f ms", terrainPrepass);
            else
                Dev::PanelHelp("Main terrain prepass n/a");
            if (terrainColor >= 0.0f)
                ImGui::Text("Main terrain colour %.3f ms", terrainColor);
            else
                Dev::PanelHelp("Main terrain colour n/a");
        }
    }
    Dev::PanelSameLine();
    if (Dev::Button("Reset window"))
        perf.Reset();
}
void DrawMaterialDebugTab()
{
    if (!GEngine || !GEngine->SupportsMaterialDebug())
    {
        Dev::PanelHelp("Material Debug is available on the WGPU renderer only.");
        return;
    }

    Engine::MaterialDebugSettings settings = GEngine->GetMaterialDebugSettings();
    bool changed = false;
    changed |= ResetToDefaultsButton(settings);
    int view = static_cast<int>(settings.view);
    if (view < 0 || view > 6)
        view = 0;
    changed |= Dev::Combo("Rendering mode", &view,
                          "Full Material\0Base Color Only\0Visualize Normal Map\0"
                          "Screen AO (not authored _AS)\0Specular / Gloss\0UV Source\0Lighting Only\0");
    settings.view = static_cast<Engine::MaterialDebugSettings::View>(view);
    Dev::PanelHelp(
        "Base Color Only is the controlled reference. Normal visualization decodes NOHQ as X=A, Y=G, Z reconstructed. "
        "Screen AO is the engine's screen-space ambient term; it does not claim an authored Reforger _AS map.");
    Dev::PanelSeparator();
    changed |= Dev::Checkbox("Disable Normal Map", &settings.disableNormalMap);
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Applies to the retained GPU-driven set as well as the per-draw path, which is\n"
                          "what makes it an A/B of the whole world rather than of a handful of sections.\n"
                          "Toggling drops the retained model cache, so shapes re-register as you look at\n"
                          "them; expect a brief hitch, and nothing else.");
    changed |= Dev::Checkbox("Compose Multi layers", &settings.composeMultiLayers);
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("The Multi shader family is a four-layer masked blend, each layer at its own UV scale. "
                          "Off binds layer 0 alone, which is how these surfaces rendered before MAT-039 -- a brick "
                          "house drawn as the rock underneath it. On costs one mask fetch plus three texture "
                          "fetches, and only on sections that are actually layered, so it is on by default. "
                          "Toggling drops the retained model cache; expect a brief hitch.");
    changed |= Dev::Checkbox("Legacy OFP/CWA enhancement", &settings.legacyEnhancement);
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Original CWA assets have no RVMAT, so they normally render as they did in 2001.\n"
                          "When a sibling <texture>_nohq.paa exists it is bound as that section's normal\n"
                          "map through the same slot the Arma 3 materials use. Turn this off to see the\n"
                          "unenhanced original in the same frame.\n"
                          "Stock content has no such files, so this changes nothing without an\n"
                          "enhancement pack present.");
    changed |= Dev::Checkbox("Netting draws as a depth-writing cutout (MAT-NET-001)", &settings.coverageMaskDepth);
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip(
            "Chain-link, barbed wire and camo net are punch-through MASKS whose edges were\n"
            "antialiased and then quantised into ARGB4444's 16 alpha levels. The texture\n"
            "histogram reads those edge texels as partial alpha and calls the whole texture\n"
            "BLEND, so the section goes to the back-to-front revisit pass -- which draws\n"
            "depth-test-only. Measured on stock CWA: pletivo_opak.paa (the chain-link panel)\n"
            "is 88.0%% fully clear and 10.6%% partial; barbedwire_plot 85.1/8.7; the camo net\n"
            "maskovaci_sit_new 39.9/32.7. mask_sit_2x1_ridka even ships a DXT1 twin that\n"
            "already classifies CUTOUT -- the same mask, routed two ways by container alone.\n"
            "\n"
            "A surface missing from the depth buffer is not just a sorting question here. The\n"
            "temporal upscaler reconstructs its motion vectors from DEPTH ALONE, so a netting\n"
            "pixel is handed the motion of whatever is behind it -- a fence at 3 m in front of\n"
            "a 100 m background parallaxes about 30x faster than the velocity it is given, and\n"
            "the history-control (reactive) mask covers only sky and water, so DLSS is told to\n"
            "trust that history fully. The wire pattern then smears over whatever is seen\n"
            "through it, worst at the 67%% render scale the DLSS route enables by default.\n"
            "Screen-space AO has the same shape of error: it is fetched by screen position, so\n"
            "the wire shades with the ambient occlusion of the geometry behind it.\n"
            "\n"
            "On: hard discard at alpha 0.5, opaque blend, depth WRITE restored (MSAA\n"
            "alpha-to-coverage still antialiases the wire) -- the same treatment the Cutout\n"
            "class already gets. Off: the old blend, for the A/B. Per-draw and CPU-side, so it\n"
            "applies from the next frame; no reload.\n"
            "\n"
            "The rule is the mirror of the MAT-051 glass rule: >=25%% fully clear AND <=40%%\n"
            "partial. Glass (uh60_skla_ca: <5%% clear, 99%% partial) fails both and keeps its\n"
            "blend, as do smoke and material-driven fades.");
    const bool tintWas = settings.enfusionLayerTint;
    changed |= Dev::Checkbox("Enfusion shared-tile colour (Color_N)", &settings.enfusionLayerTint);
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip(
            "Reforger's MatPBRMulti materials have no albedo of their own: they name a BCR_N,\n"
            "which is a SHARED one-metre library tile out of Assets/_SharedData. 436 distinct\n"
            "tiles serve 2,889 of Everon's structure materials, so unrelated walls came out the\n"
            "same pale grey because they were the same square metre of texture.\n"
            "\n"
            "What makes a wall its own colour is the per-layer Color_N beside the tile, and 90.3%%\n"
            "of those materials declare one. On, the tile is rescaled so its mean IS that colour\n"
            "(mask-weighted across the layers) -- the same rule the xob exporter bakes into its\n"
            ".paa. Off binds the raw tile, which is what this path did before.\n"
            "\n"
            "NOT a per-fragment switch: it decides the texture NAME the world loader hands the\n"
            "renderer, so it applies to what is loaded AFTER the flip. Reload the world for a\n"
            "clean A/B. Inert on every non-Enfusion world.\n"
            "The load's own count is in the log: 'materials -- ... a shared layer tile (of those\n"
            "N author a Color_N tint: M applied ...)'.");
    const bool tintMulWas = settings.enfusionTintMultiply;
    changed |= Dev::Checkbox("Enfusion Color_N multiplies (linear)", &settings.enfusionTintMultiply);
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip(
            "WHAT the switch above's colour means, once it has been applied.\n"
            "\n"
            "Off is what shipped: Color_N is read as the surface's LINEAR mean and the tile is\n"
            "rescaled so its sRGB mean is linearToSrgb(Color_N). On, Color_N MULTIPLIES the tile\n"
            "in linear space -- out = linearToSrgb(srgbToLinear(texel) * C) -- which is what a\n"
            "colour tint physically is.\n"
            "\n"
            "Measured against the albedo Enfusion's own toolchain bakes from the SAME layered\n"
            "material (*_MLOD_BCR.edds), over 35 materials where the pairing is unambiguous:\n"
            "off predicts 1.82x the authored mean (mean |log ratio| 0.644) and a flatter surface\n"
            "(chroma 1.040 against the bake's 1.132); on predicts 1.29x (0.403) at chroma 1.088.\n"
            "1.82x in an 8-bit sRGB mean is ~3.6x the radiance -- the white farmhouse.\n"
            "\n"
            "NOT a per-fragment switch, and a stronger form of the one above: it decides the\n"
            "TEXELS a tinted tile is uploaded with, and both settings build the same texture\n"
            "NAME, so a cached upload is not re-decoded. RELAUNCH for a clean A/B; the value\n"
            "persists to graphics.cfg. Inert wherever no Color_N is applied.");
    const bool multiWas = settings.enfusionMultiLayers;
    changed |= Dev::Checkbox("Enfusion Multi layers (mask on UV set 2, Color_N per layer)", &settings.enfusionMultiLayers);
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip(
            "RFG-072. A native MatPBRMulti is a mask-blended stack of up to four shared one-metre\n"
            "tiles. Off (the state before): the mask samples the model's FIRST UV set -- the\n"
            "metre-scale tiling unwrap, u[-16..32] on Church_01's walls -- so it repeats ~47\n"
            "times across the wall as dark patches, and every layer is its raw colourless tile.\n"
            "On: the mask samples the SECOND UV set the material asks for (UVSrcGlobalMaps\n"
            "\"UV set 2\", the [0..1] unwrap it was painted in) and each layer multiplies its tile\n"
            "by its own Color_N in linear space.\n"
            "\n"
            "Registration-time, like the two switches above: the material record is baked when\n"
            "a shape registers, so reload the world for a clean A/B. The value persists to\n"
            "graphics.cfg. The log counts it: 'Wgpu material: MatPBRMulti census -- ...'.\n"
            "Inert on every non-Enfusion world.");
    const bool compressedWas = settings.compressedEnfusionTextures;
    changed |= Dev::Checkbox("Enfusion compressed textures (BC7/BC5/BC4)", &settings.compressedEnfusionTextures);
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip(
            "Reforger's albedos are BC7 and its normal maps BC7/BC5. The 2001 Pac container has no\n"
            "name for those, so they used to be DECODED to 32-bit on the way in -- 4x the bytes of\n"
            "BC7, 8x those of BC5 -- which is what exhausted GPU memory on native Everon at ~1,200\n"
            "models. On, the blocks go to the GPU untouched, at the same resolution.\n"
            "\n"
            "NOT a per-fragment switch: it changes what is uploaded, so it applies to textures\n"
            "loaded AFTER the flip, and the shader flag for a compressed normal map's channel\n"
            "order is baked when a model registers. Reload the world for a clean A/B.\n"
            "Greyed out with no effect if the adapter cannot take BC textures.");
    changed |= Dev::Checkbox("Invert Normal Map Y (diagnostic)", &settings.invertNormalY);
    Dev::PanelHelp("Compare only under fixed lighting; this does not edit the imported material.");
    changed |= Dev::Checkbox("Disable SMDI / Specular-Gloss", &settings.disableSpecularGloss);
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Shade every section with its flat specular constant, ignoring the bound SMDI map.\n"
                          "The A/B for whether a highlight is the map or the constant.");
    changed |= Dev::Checkbox("Disable Fresnel / Environment", &settings.disableFresnelEnvironment);
    Dev::PanelHelp("Stages the renderer binds: base, Stage1 normal, SMDI specular, Multi mask/layers and their\n"
                   "normals, Fresnel/environment. Detail, macro and ambient-shadow stages are not bound, so\n"
                   "there is nothing to switch off; the checkboxes that suggested otherwise were removed.");
    if (changed)
        GEngine->SetMaterialDebugSettings(settings);
    // RFG-047: persisted the moment it is toggled, with no Save button.
    //
    // Every other control on this tab is a per-fragment switch that takes effect on the
    // next frame, so a session-only value is honest for them. This one only fully applies
    // to what is loaded AFTER it, which makes "relaunch and look" the real A/B -- and a
    // switch you have to set again after every relaunch is a switch that silently measures
    // the wrong thing. SeedRendererEnvFromGraphicsConfig reads it back at boot.
    if (compressedWas != settings.compressedEnfusionTextures)
    {
        const std::string cfgPath = GamePaths::Instance().UserDir() + "graphics.cfg";
        GraphicsConfig cfg;
        cfg.Load(cfgPath);
        cfg.enfusionCompressedTextures = settings.compressedEnfusionTextures ? 1 : 0;
        if (cfg.Save(cfgPath))
            LOG_INFO(Graphics, "Dev panel: Enfusion compressed textures {} (persisted to graphics.cfg)",
                     settings.compressedEnfusionTextures ? "ON" : "OFF");
        else
            LOG_ERROR(Graphics, "Dev panel: saving '{}' failed", cfgPath);
    }
    // RFG-070: persisted for the same reason, and pushed to the WORLD LOADER rather than
    // through the renderer -- the switch is read where the texture name is built, in
    // LandLoadEnfusion, which the graphics engine never sees.
    if (tintWas != settings.enfusionLayerTint)
    {
        Poseidon::Enfusion::SetLayerTintEnabled(settings.enfusionLayerTint);
        const std::string cfgPath = GamePaths::Instance().UserDir() + "graphics.cfg";
        GraphicsConfig cfg;
        cfg.Load(cfgPath);
        cfg.enfusionLayerTint = settings.enfusionLayerTint ? 1 : 0;
        if (cfg.Save(cfgPath))
            LOG_INFO(Graphics,
                     "Dev panel: Enfusion shared-tile colour {} (persisted to graphics.cfg; reload the world)",
                     settings.enfusionLayerTint ? "ON" : "OFF");
        else
            LOG_ERROR(Graphics, "Dev panel: saving '{}' failed", cfgPath);
    }

    // RFG-071: written to the same file and pushed to the same place -- the texture source
    // reads it while the object stream decodes, which is neither the renderer nor the world
    // loader, so the atomic in EnfusionTextureName is the only thing both sides share.
    if (tintMulWas != settings.enfusionTintMultiply)
    {
        Poseidon::Enfusion::SetLayerTintLinearMultiply(settings.enfusionTintMultiply);
        const std::string cfgPath = GamePaths::Instance().UserDir() + "graphics.cfg";
        GraphicsConfig cfg;
        cfg.Load(cfgPath);
        cfg.enfusionTintMultiply = settings.enfusionTintMultiply ? 1 : 0;
        if (cfg.Save(cfgPath))
            LOG_INFO(Graphics,
                     "Dev panel: Enfusion Color_N linear multiply {} (persisted to graphics.cfg; relaunch for a "
                     "clean A/B -- both settings build the same texture name)",
                     settings.enfusionTintMultiply ? "ON" : "OFF");
        else
            LOG_ERROR(Graphics, "Dev panel: saving '{}' failed", cfgPath);
    }

    // RFG-072: same file, same push -- the shape registration reads the atomic.
    if (multiWas != settings.enfusionMultiLayers)
    {
        Poseidon::Enfusion::SetMultiLayersEnabled(settings.enfusionMultiLayers);
        const std::string cfgPath = GamePaths::Instance().UserDir() + "graphics.cfg";
        GraphicsConfig cfg;
        cfg.Load(cfgPath);
        cfg.enfusionMultiLayers = settings.enfusionMultiLayers ? 1 : 0;
        if (cfg.Save(cfgPath))
            LOG_INFO(Graphics, "Dev panel: Enfusion Multi layers {} (persisted to graphics.cfg; reload the world)",
                     settings.enfusionMultiLayers ? "ON" : "OFF");
        else
            LOG_ERROR(Graphics, "Dev panel: saving '{}' failed", cfgPath);
    }

    const Engine::MaterialDebugInfo info = GEngine->GetMaterialDebugInfo();
    Dev::PanelSeparator();
    Dev::PanelHelp("Active production section");
    if (!info.active)
    {
        Dev::PanelHelp("No RVMAT-backed section has been drawn yet.");
        return;
    }
    ImGui::Text("RVMAT: %s", info.rvmatPath.Data());
    ImGui::Text("Shader: %s", info.shaderFamily.Data());
    ImGui::Text("Stage1 role: NormalMap (source stage 1)");
    ImGui::Text("Texture: %s", info.normalTexturePath.Data());
    ImGui::Text("UV source: %s", info.normalUvSource.Data());
    ImGui::Text("Resolution: %s", info.normalStatus.Data());
}

void DrawRenderTab()
{
    if (!GEngine)
    {
        Dev::PanelHelp("engine not up");
        return;
    }

    // MSAA + render scale moved to the Graphics tab (2026-08-30) — this tab carried
    // the GL33-era controls, and once the wgpu engine actually honoured the setters
    // both tabs showed competing knobs for the same state.
    Dev::PanelHelp("MSAA and render scale moved to the \"Graphics\" tab");

    bool a2c = GEngine->GetAlphaToCoverage();
    if (Dev::Checkbox("Alpha-to-coverage (cutout AA; needs MSAA)", &a2c))
        GEngine->SetAlphaToCoverage(a2c);

    bool flat = GEngine->GetDebugFlatColor();
    if (Dev::Checkbox("Flat shading (objects -> solid red; shading-vs-geometry probe)", &flat))
        GEngine->SetDebugFlatColor(flat);

    Dev::PanelSeparator();
    ImGui::Text("window  %d x %d", GEngine->Width(), GEngine->Height());
    ImGui::Text("target  scale %.2fx, %dx MSAA", GEngine->GetRenderScale(), GEngine->GetMsaaSamples());
    // Roads and decals. Here rather than on the Sky tab, where these spent one afternoon
    // because that was the push path that already existed: they describe how ground-conformed
    // geometry wins its depth test, which has nothing to do with the atmosphere.
    Dev::PanelSeparator();
    auto road = GEngine->GetRoadSettings();
    bool roadChanged = ResetToDefaultsButton(road, "Reset roads to defaults",
                                             "(only the road/decal conform below)");
    Dev::PanelHeading("Roads and decals (per-pixel ground conform)");
    roadChanged |= Dev::SliderFloat("Road lift, flat", &road.liftFlat, 0.0f, 0.5f, "%.3f m");
    Dev::PanelTooltip("Fixed part of the pull toward the camera, in metres. Acts at every distance and\n"
                      "every angle, so this is the one to raise if a road flickers right in front of you.");
    roadChanged |= Dev::SliderFloat("Road lift, per metre", &road.liftPerMetre, 0.0f, 0.2f, "%.4f /m",
                                    ImGuiSliderFlags_Logarithmic);
    Dev::PanelTooltip("THE one that matters, and the one that was wrong. Divided by the grazing angle in the\n"
                      "shader, because the error it covers is a height error and a height error becomes\n"
                      "error/|dir.y| along the view ray -- about 6x looking down a road from standing height.\n"
                      "Too small and the road breaks into a chequer of tiles that win and lose the depth test\n"
                      "alternately: continuous close up, continuous from above, in pieces in between.\n"
                      "Swept on Malden: 0.0015 closes most of it, 0.0040 closes it completely.");
    roadChanged |= Dev::SliderFloat("Road lift ceiling", &road.liftMaxFrac, 0.0005f, 2.0f, "%.4f x dist",
                                    ImGuiSliderFlags_Logarithmic);
    Dev::PanelTooltip("Upper bound on the total pull, as a fraction of the distance. Its job is not to keep\n"
                      "the lift small but to keep it FINITE: the grazing division above runs away as the ray\n"
                      "flattens toward the horizon.\n"
                      "The DEFAULT stops at 0.0400; this slider does not. Past 0.0400 the road is pulled far\n"
                      "enough toward the camera to be drawn OVER HOUSES and other objects standing on it --\n"
                      "measured at 0.1263 on Malden, where the pull is ~8.7 m at 80 m and ~25 m at 200 m.\n"
                      "Push it if you want to; you will see that failure long before you see a benefit.");
    if (roadChanged)
        GEngine->SetRoadSettings(road);
    Dev::PanelHelp("settings are session-only; persist via graphics.cfg");
}
// HDR tonemap / look tuning (wgpu HDR path). The Hable curve is fixed; these are
// exposure + a colour-grade block. "Auto (time of day)" drives the grade from the
// per-ToD preset keyframes; uncheck to override and tune a keyframe by eye, then
// copy the preset line back. See engine/WgpuRenderer/docs/hdr-pipeline-plan.md.
// REN-TEMP-001 §6.7 — the owner's live tuning surface: every temporal/DLSS knob next
// to a live frame-time readout, so the sweet spot is found by eye and measured in fps.
void DrawTemporalTab()
{
    if (!GEngine)
    {
        Dev::PanelHelp("engine not up");
        return;
    }
    if (!GEngine->SupportsTemporalTuning())
    {
        Dev::PanelHelp("wgpu HDR path required (WGR_HDR=1)");
        return;
    }

    const auto info = GEngine->GetTemporalInfo();
    const float fps = ImGui::GetIO().Framerate;
    Dev::PanelHelp("%.1f fps  (%.2f ms)", fps, fps > 0.0f ? 1000.0f / fps : 0.0f);
    Dev::PanelHelp("render %dx%d -> output %dx%d", info.renderWidth, info.renderHeight,
                   info.outputWidth, info.outputHeight);
    Dev::PanelHelp("temporal %s | DLSS route %s | DLSS %s (quality %d)",
                   info.temporalActive ? "on" : "off", info.dlssRoute ? "up" : "down",
                   info.dlssActive ? "active" : "inactive", info.dlssQuality);
    static const char* kUpscalerNames[] = {"native (no upscaler)", "DLSS", "FSR 1", "bilinear"};
    const std::string dlssVer = DlssRuntimeVersionString();
    Dev::PanelHelp("active upscaler: %s%s%s%s",
                   kUpscalerNames[info.activeUpscaler >= 0 && info.activeUpscaler <= 3 ? info.activeUpscaler : 0],
                   dlssVer.empty() ? "" : "  (DLSS runtime ",
                   dlssVer.c_str(),
                   dlssVer.empty() ? "" : ", FSR 1.0)");
    Dev::PanelHelp("jitter (%.3f, %.3f) px | mip bias %.3f%s", info.jitterX, info.jitterY,
                   info.mipBiasEffective, info.resetThisFrame ? " | HISTORY RESET" : "");
    // WHY DLSS is inactive, in the renderer's own words (dlss_status.rs): the missing
    // snippet, the iGPU that was picked, the driver, the NGX code. A tester who cannot
    // say why cannot be helped, and until 2026-09-02 this line was a bare "inactive".
    if (!info.dlssActive)
    {
        const std::string why = GEngine->DlssStatusReason();
        Dev::PanelHelp("DLSS inactive: %s",
                       why.empty() ? "(this renderer DLL reports no reason - redeploy exe + DLL together)"
                                   : why.c_str());
    }

    Dev::PanelSeparator();
    auto t = GEngine->GetTemporalTuning();
    bool changed = false;
    changed |= Dev::Checkbox("Temporal (jitter + motion vectors)", &t.temporalOn);
    changed |= Dev::Checkbox("DLSS Super Resolution", &t.dlssOn);
    Dev::PanelTooltip("Needs the NGX route (NVIDIA, detected at launch). Off/unavailable -> FSR 1 takes over.");
    changed |= Dev::Checkbox("FSR 1 (vendor-neutral upscaler)", &t.fsrOn);
    Dev::PanelTooltip("AMD FidelityFX EASU+RCAS (MIT-licensed, runs on any GPU). Active when DLSS is off.\nBoth off = soft bilinear.");
    changed |= Dev::Checkbox("Sharpen DLSS output (RCAS)", &t.dlssSharpen);
    Dev::PanelTooltip("Runs the RCAS sharpener over the DLSS result (NVIDIA removed DLSS's own\n"
                      "sharpening; an external pass is the standard answer). Strength = the slider below.");
    // The knob feeds FSR's RCAS and the optional post-DLSS RCAS; grey it when neither
    // consumer is live so nobody wiggles a dead slider (owner did exactly that).
    const bool sharpnessLive = info.activeUpscaler == 2 || (info.activeUpscaler == 1 && t.dlssSharpen);
    if (!sharpnessLive)
        ImGui::BeginDisabled();
    changed |= Dev::SliderFloat("Sharpness (0 = max)", &t.fsrSharpness, 0.0f, 2.0f, "%.2f");
    if (!sharpnessLive)
    {
        ImGui::EndDisabled();
        Dev::PanelHelp("  (inactive - needs FSR 1, or DLSS with \"Sharpen DLSS output\")");
    }
    Dev::PanelTooltip("RCAS sharpening in stops: 0 = sharpest, 2 = mildest.\n"
                      "Applies to FSR 1 and, when enabled above, to the DLSS output.");
    changed |= Dev::SliderInt("Render scale %", &t.renderScalePct, 50, 200);
    Dev::PanelTooltip("<100%% = upscaling (67%% ~ DLSS Quality). 100%% + DLSS = DLAA. >100%% = SSAA (supersampling, expensive).");
    if (Dev::Button("Native (everything off)"))
    {
        t.temporalOn = false;
        t.dlssOn = false;
        t.fsrOn = false;
        t.renderScalePct = 100;
        changed = true;
    }
    Dev::PanelTooltip("Temporal + DLSS + FSR off, scale 100%% - the untouched native pipeline.\n"
                      "Applies INSTANTLY, no restart. MSAA is classic anti-aliasing, not upscaling,\n"
                      "and stays as set above. Watch the \"active upscaler\" line flip to native.");
    Dev::PanelSameLine();
    if (Dev::Button("Auto defaults"))
    {
        // Fresh shipped defaults: DLSS-if-supported (else FSR), Quality scale, all
        // the owner-validated tuning values.
        t = Engine::TemporalSettings{};
        changed = true;
    }
    Dev::PanelTooltip("Back to the shipped automatic setup: DLSS on (when the route is up, else FSR 1),\n"
                      "render scale 67%%, default sharpness/bias. Applies instantly.");

    {
        const int active = info.msaaSamples <= 1 ? 0 : info.msaaSamples;
        static int msaaChoice = -1;
        if (msaaChoice < 0)
            msaaChoice = active == 0 ? 0 : active == 2 ? 1 : active == 4 ? 2 : 3;
        if (Dev::Combo("MSAA (restart)", &msaaChoice, "Off\0" "2x\0" "4x\0" "8x\0"))
        {
            const int samples[] = {0, 2, 4, 8};
            GEngine->SetMsaaSamples(samples[msaaChoice]);
        }
        const int chosen[] = {0, 2, 4, 8};
        if (chosen[msaaChoice] != active)
            Dev::PanelHelp("MSAA %s applies on the next launch (active: %dx)",
                           msaaChoice == 0 ? "Off" : chosen[msaaChoice] == 2 ? "2x" : chosen[msaaChoice] == 4 ? "4x" : "8x",
                           active);
        Dev::PanelTooltip("Stored in graphics.cfg (also settable in the Options menu); the renderer pipelines are fixed per launch.");

        // Persist the panel's graphics choices so they survive a restart — the same
        // file the Options page writes. DLSS off is remembered (dlssMode 0 seeds
        // WGR_DLSS=0 at boot); scale 100% persists as AUTO (the renderer may then
        // still pick its DLSS default), any other scale as an explicit pin.
        if (Dev::Button("Save as default"))
        {
            const std::string cfgPath = GamePaths::Instance().UserDir() + "graphics.cfg";
            GraphicsConfig cfg;
            cfg.Load(cfgPath);
            // The COMPLETE panel state, mapped onto the Options-menu semantics so both
            // surfaces stay coupled: upscaler choice from the DLSS/FSR switches,
            // sub-100% scale into upscalerQuality vs SSAA into renderScale, plus
            // sharpness and the DLSS sharpen pass.
            cfg.dlssMode = t.dlssOn && t.fsrOn ? -1 : t.dlssOn ? 1 : t.fsrOn ? 2 : 0;
            const int samplesForChoice[] = {0, 2, 4, 8};
            cfg.msaaSamples = samplesForChoice[msaaChoice];
            if (t.renderScalePct < 100)
            {
                cfg.upscalerQuality = t.renderScalePct;
                cfg.renderScale = 1.0f;
            }
            else
            {
                cfg.upscalerQuality = -1;
                cfg.renderScale = t.renderScalePct == 100 ? 1.0f : t.renderScalePct / 100.0f;
            }
            cfg.fsrSharpness = t.fsrSharpness;
            cfg.dlssSharpen = t.dlssSharpen ? 1 : 0;
            cfg.jitterPhases = t.jitterPhases;
            if (cfg.Save(cfgPath))
                LOG_INFO(Graphics,
                         "Dev panel: graphics.cfg saved (dlssMode={}, quality={}, msaa={}, renderScale={:.2f}, "
                         "sharpness={:.2f}, dlssSharpen={}, jitterPhases={})",
                         cfg.dlssMode, cfg.upscalerQuality, cfg.msaaSamples, cfg.renderScale, cfg.fsrSharpness,
                         cfg.dlssSharpen, cfg.jitterPhases);
            else
                LOG_ERROR(Graphics, "Dev panel: saving '{}' failed", cfgPath);
        }
        Dev::PanelTooltip("Writes the whole tab (upscaler, quality, MSAA, scale, sharpness, DLSS sharpen, jitter phases) to "
                          "graphics.cfg - the same file the Options menu edits - applied on every launch.");
    }

    Dev::PanelSeparator();
    Dev::PanelHeading("Image tuning");
    changed |= Dev::Checkbox("Mip bias auto (half log2(scale))", &t.mipBiasAuto);
    if (!t.mipBiasAuto)
    {
        changed |= Dev::SliderFloat("Mip bias", &t.mipBias, -2.0f, 0.0f, "%.2f");
        Dev::PanelTooltip("More negative = sharper textures but more distant shimmer.");
    }
    changed |= Dev::SliderFloat("Reactive: sky/clouds", &t.reactiveSky, 0.0f, 1.0f, "%.2f");
    changed |= Dev::SliderFloat("Reactive: water", &t.reactiveWater, 0.0f, 1.0f, "%.2f");
    int phases = t.jitterPhases;
    if (Dev::SliderInt("Jitter phases (0 = auto)", &phases, 0, 64))
    {
        t.jitterPhases = phases;
        changed = true;
    }
    Dev::PanelTooltip("Auto picks per setup: frozen offset at native scale (no shimmer),\n"
                      "cycling when upscaling so DLSS/FSR get distinct samples (8+ at Quality).\n"
                      "1 freezes always: use it if an upscaled image still shimmers (softer reconstruction).\n"
                      "Saved by \"Save as default\".");

    if (ImGui::CollapsingHeader("Conventions (A/B probes - leave alone)"))
    {
        changed |= Dev::Checkbox("DLSS auto exposure", &t.dlssAutoExposure);
        Dev::PanelTooltip("Owner A/B winner. Off = feed the engine exposure chain instead.");
        changed |= Dev::Checkbox("Jitter Y flip", &t.jitterYFlip);
        changed |= Dev::Checkbox("MV render-space scale", &t.mvRenderSpace);
        changed |= Dev::Checkbox("MV sign flip", &t.mvFlip);
    }

    if (changed)
        GEngine->SetTemporalTuning(t);
}

void DrawTonemapTab()
{
    if (!GEngine)
    {
        Dev::PanelHelp("engine not up");
        return;
    }
    if (!GEngine->SupportsTonemap())
    {
        Dev::PanelHelp("HDR path off (run the wgpu backend with WGR_HDR=1)");
        return;
    }

    bool autoTod = GEngine->GetTonemapAuto();
    if (Dev::Checkbox("Auto (time of day)", &autoTod))
        GEngine->SetTonemapAuto(autoTod);
    Dev::PanelSameLine();
    Dev::PanelHelp("ToD %.2f h", Glob.clock.GetTimeOfDay() * 24.0f);
    if (autoTod)
        Dev::PanelHelp("grade driven by per-ToD presets; uncheck to override + tune");

    // In auto mode the sliders reflect the live interpolated grade but are read-only
    // (UpdateAutoTonemap overwrites them each frame).
    auto t = GEngine->GetTonemapSettings();
    bool changed = false;

    ImGui::BeginDisabled(autoTod);

    changed |= Dev::Checkbox("Hable filmic", &t.hable);
    Dev::PanelSameLine();
    changed |= Dev::Checkbox("sRGB encode", &t.encode);
    Dev::PanelTooltip("Off = passthrough clamp / write-as-is (debug only)");

    changed |= Dev::SliderFloat("Exposure", &t.exposure, 0.05f, 8.0f, "%.3f", ImGuiSliderFlags_Logarithmic);

    Dev::PanelSeparator();

    Dev::PanelHeading("Grade");
    changed |= Dev::SliderFloat("Temperature (warm+/cool-)", &t.temperature, -1.0f, 1.0f, "%.3f");
    changed |= Dev::SliderFloat("Tint (magenta+/green-)", &t.tint, -1.0f, 1.0f, "%.3f");
    changed |= Dev::SliderFloat("Contrast", &t.contrast, 0.5f, 2.0f, "%.3f");
    changed |= Dev::SliderFloat("Saturation", &t.saturation, 0.0f, 2.0f, "%.3f");
    changed |= Dev::SliderFloat("Shadow lift", &t.lift, 0.0f, 0.3f, "%.3f");
    changed |= Dev::SliderFloat("Gain", &t.gain, 0.1f, 4.0f, "%.3f");

    if (Dev::Button("Reset to defaults"))
    {
        t = decltype(t){};
        changed = true;
    }

    if (changed && !autoTod)
        GEngine->SetTonemapSettings(t);

    ImGui::EndDisabled();

    // Bloom is a global look setting (not per-ToD keyframed), so it stays editable even
    // in auto mode — its values are preserved across the per-frame preset overwrite.
    Dev::PanelSeparator();
    Dev::PanelHeading("Bloom");
    bool bloomChanged = false;
    bloomChanged |= Dev::SliderFloat("Intensity##bloom", &t.bloomIntensity, 0.0f, 0.3f, "%.3f");
    Dev::PanelTooltip("Linear weight of the bloom added to the scene (0 = off).");
    bloomChanged |= Dev::SliderFloat("Threshold##bloom", &t.bloomThreshold, 0.0f, 4.0f, "%.3f");
    Dev::PanelTooltip("Scene-referred luminance where bloom begins (soft knee).");
    bloomChanged |= Dev::SliderFloat("Knee##bloom", &t.bloomKnee, 0.0f, 2.0f, "%.3f");
    if (bloomChanged)
        GEngine->SetTonemapSettings(t);

    // Auto-exposure / eye adaptation. Separate from the grade (its own setter), off by
    // default so it doesn't fight manual per-ToD exposure. Independent of auto/manual.
    Dev::PanelSeparator();
    Dev::PanelHeading("Auto exposure (eye adaptation)");
    auto ex = GEngine->GetExposureSettings();
    bool exChanged = false;
    exChanged |= ResetToDefaultsButton(ex, "Reset auto exposure to defaults", "");
    exChanged |= Dev::Checkbox("Enabled##exposure", &ex.enabled);
    Dev::PanelTooltip("ON by default since 2026-08-16. Exposure is scaled toward\n"
                      "key / scene-average luminance, so a bright sky no longer\n"
                      "burns out (measured: 23.2% of pixels clipped -> 0.6%).\n\n"
                      "If the scene visibly PUMPS as you turn the camera, narrow\n"
                      "Min/Max scale before turning this off — 0.55..1.10 measured\n"
                      "within 0.2% of the full 0.25..4.0 range on the test pose.");
    ImGui::BeginDisabled(!ex.enabled);
    exChanged |=
        Dev::SliderFloat("Key (target grey)##exposure", &ex.key, 0.02f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    exChanged |= Dev::SliderFloat("Min scale##exposure", &ex.minScale, 0.05f, 1.0f, "%.3f");
    Dev::PanelTooltip("Lower bound on the exposure multiplier. Raising this toward 1.0\n"
                      "is the first thing to try against pumping — it bounds how dark\n"
                      "the iris can close, which is what makes a turn visible.");
    exChanged |= Dev::SliderFloat("Max scale##exposure", &ex.maxScale, 1.0f, 16.0f, "%.3f");
    exChanged |= Dev::SliderFloat("Adapt time (s)##exposure", &ex.rateTau, 0.0f, 3.0f, "%.2f");
    Dev::PanelTooltip("Adaptation TIME CONSTANT in seconds: 1 - exp(-dt / tau).\n"
                      "Framerate-independent, so the same setting adapts at the same\n"
                      "real speed on a 20 fps world and a 45 fps one.\n"
                      "Set to 0 to fall back to the legacy per-frame rate below.");
    ImGui::BeginDisabled(ex.rateTau > 0.0f);
    exChanged |=
        Dev::SliderFloat("Adapt rate (legacy)##exposure", &ex.rate, 0.005f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic);
    Dev::PanelTooltip("Per-frame ease toward the target. FRAMERATE-DEPENDENT: the same\n"
                      "value adapts twice as fast on a 45 fps world as on a 20 fps one,\n"
                      "so anything tuned with it is tuned against one world's framerate.\n"
                      "Only used when Adapt time is 0.");
    ImGui::EndDisabled();
    exChanged |= Dev::SliderFloat("Sky weight##exposure", &ex.skyWeight, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("Metering weight of the top of the frame (sky) vs the bottom (ground).\n"
                      "1.0 = uniform; lower biases exposure toward the ground so a bright\n"
                      "sky in view doesn't over-darken the scene.");
    ImGui::EndDisabled();
    if (exChanged)
        GEngine->SetExposureSettings(ex);
    // Live scale the resolve is applying (blocking GPU readback — diagnostic). 1.0 =
    // neutral; if this never budges across scenes the reduction/adapt isn't feeding it.
    ImGui::Text("Current scale: %.3f", GEngine->GetAutoExposureScale());

    Dev::PanelSeparator();
    Dev::PanelHelp("Preset (copy back to bake into the ToD keyframes):");
    char preset[512];
    snprintf(preset, sizeof(preset),
             "tonemap: exposure=%.3f temp=%.3f tint=%.3f contrast=%.3f sat=%.3f lift=%.3f gain=%.3f "
             "hable=%s encode=%s",
             t.exposure, t.temperature, t.tint, t.contrast, t.saturation, t.lift, t.gain, t.hable ? "true" : "false",
             t.encode ? "true" : "false");
    ImGui::SetNextItemWidth(-1.0f);
    Dev::InputText("##tonemapPreset", preset, sizeof(preset), ImGuiInputTextFlags_ReadOnly);
    if (Dev::Button("Copy preset to clipboard"))
        ImGui::SetClipboardText(preset);
    Dev::PanelHelp("session-only; paste back to bake into the kTonemapPresets keyframes");
}

// Procedural sky tuning (wgpu). Celestial inputs (sun/moon direction, night factor)
// come live from LightSun; these are the authored atmosphere + look knobs. Writes
// immediately (a renderer-param setter, like the Tonemap tab). See
// engine/WgpuRenderer/docs/procedural-sky-plan.md.
void DrawSkyTab()
{
    if (!GEngine)
    {
        Dev::PanelHelp("engine not up");
        return;
    }
    if (!GEngine->SupportsSky())
    {
        Dev::PanelHelp("procedural sky unavailable (run the wgpu backend)");
        return;
    }

    auto s = GEngine->GetSkySettings();
    bool changed = false;
    changed |= ResetToDefaultsButton(s);

    changed |= Dev::Checkbox("Enabled", &s.enabled);
    Dev::PanelTooltip("Off = skip the sky pass and restore the legacy skydome");
    Dev::PanelSameLine();
    Dev::PanelHelp("ToD %.2f h", Glob.clock.GetTimeOfDay() * 24.0f);

    changed |= Dev::Checkbox("Auto (time-of-day presets)", &s.autoToD);
    Dev::PanelTooltip(
        "On = drive the atmosphere look (exposure, sun intensity, rayleigh, mie, ozone, "
        "turbidity, sun radius, night intensity) from the per-ToD preset table each frame, "
        "interpolated like the tonemap grade — the sliders below show the live values but "
        "edits are overwritten next frame. Off = hold your manual values so you can tune "
        "(then copy the preset). The toggles (sky lighting, aerial shadow, fog falloff) stay live either way.");

    ImGui::BeginDisabled(!s.enabled);

    changed |= Dev::SliderFloat("Exposure", &s.exposure, 0.05f, 8.0f, "%.3f", ImGuiSliderFlags_Logarithmic);

    Dev::PanelSeparator();
    Dev::PanelHeading("Sun");
    changed |= Dev::SliderFloat("Intensity", &s.sunIntensity, 1.0f, 60.0f, "%.2f");
    float sunDeg = s.sunAngularRadius * 180.0f / 3.14159265f;
    if (Dev::SliderFloat("Angular radius (deg)", &sunDeg, 0.1f, 5.0f, "%.2f"))
    {
        s.sunAngularRadius = sunDeg * 3.14159265f / 180.0f;
        changed = true;
    }

    Dev::PanelSeparator();
    Dev::PanelHeading("Moon");
    Dev::PanelHelp("position, phase and moonlight from a real ephemeris (date + world lat/long)");

    // Live readout. The point of showing all four together is that a wrong-looking moon is
    // almost always one of them disagreeing with the others — a phase that does not match the
    // elongation means the sun and the moon are on different models.
    if (GScene && GScene->MainLight())
    {
        const LightSun* ml = GScene->MainLight();
        const Vector3 toMoon = -ml->MoonDirection();
        const float moonEl = asinf(std::clamp(static_cast<float>(toMoon.Y()), -1.0f, 1.0f)) * 180.0f / 3.14159265f;
        float moonAz = atan2f(static_cast<float>(toMoon.X()), static_cast<float>(toMoon.Z())) * 180.0f / 3.14159265f;
        if (moonAz < 0.0f)
        {
            moonAz += 360.0f;
        }
        char dateBuf[64] = {0};
        Glob.clock.FormatDate("%Y-%m-%d", dateBuf);
        const float tod = Glob.clock.GetTimeOfDay();
        Dev::PanelHelp("sim date %s  %02d:%02d", dateBuf, static_cast<int>(tod * 24.0f),
                       static_cast<int>(tod * 1440.0f) % 60);
        Dev::PanelHelp("az %.1f  el %+.1f  lit %.0f%%  size %.3f deg", moonAz, moonEl, ml->MoonIllumination() * 100.0f,
                       ml->MoonAngularRadius() * 2.0f * 180.0f / 3.14159265f);
        Dev::PanelHelp("phase brightness %.3f x full   moonlight %.3f", ml->MoonBrightness(), ml->MoonLightAmount());
    }

    changed |= Dev::Checkbox("Realistic position (ephemeris)", &s.moonRealistic);
    Dev::PanelTooltip("On: position, phase, illuminated fraction and apparent size from a low-precision\n"
                      "lunar ephemeris (accurate to ~0.015 deg -- a thirty-fifth of the moon's own\n"
                      "diameter), driven by the simulation date and the world's latitude/longitude.\n"
                      "Off: the 2001 model -- a 28-day month with no eccentricity and no perturbations,\n"
                      "which puts the moon tens of degrees from where it belongs.");

    changed |= Dev::Checkbox("Realistic SUN position too", &s.sunRealistic);
    Dev::PanelTooltip("MOVES DAYLIGHT. Solar transit shifts by up to the +/-16 min equation of time and\n"
                      "the altitude track through the day changes shape, and every time-of-day preset\n"
                      "was authored against the legacy sun. Measured divergence is small (<5 deg in\n"
                      "azimuth all year) but it is not zero. The moon's PHASE and terminator always come\n"
                      "from the ephemeris sun regardless of this switch, so they never disagree.");

    changed |= Dev::Checkbox("Manual placement (screenshots)", &s.moonManual);
    Dev::PanelTooltip("Ignore the astronomy and put the moon where you want it. Position only -- the "
                      "phase and brightness stay whatever the date says, so framing a shot does not "
                      "silently change the look.");
    ImGui::BeginDisabled(!s.moonManual);
    changed |= Dev::SliderFloat("  azimuth (deg)", &s.moonManualAzimuthDeg, 0.0f, 360.0f, "%.1f");
    Dev::PanelTooltip("From NORTH, increasing toward EAST. 90 = due east, 180 = due south.");
    changed |= Dev::SliderFloat("  elevation (deg)", &s.moonManualElevationDeg, -20.0f, 90.0f, "%.1f");
    ImGui::EndDisabled();

    Dev::PanelSeparator();
    Dev::PanelHelp("Observer / date");
    changed |= Dev::Checkbox("Override date", &s.moonDateOverride);
    Dev::PanelTooltip("Celestial date ONLY -- it does not move the simulation clock, so it is local, "
                      "not replicated, and safe to scrub while a mission runs. Use it to find a phase; "
                      "use the mission Intel / setDate to actually move the world.");
    ImGui::BeginDisabled(!s.moonDateOverride);
    changed |= Dev::InputInt("  year", &s.moonDateYear);
    changed |= Dev::SliderInt("  month", &s.moonDateMonth, 1, 12);
    changed |= Dev::SliderInt("  day", &s.moonDateDay, 1, 31);
    Dev::PanelHelp("  a full lunation is 29.53 days -- step the day to walk the phases");
    ImGui::EndDisabled();

    changed |= Dev::Checkbox("Override latitude", &s.moonLatitudeOverride);
    Dev::PanelTooltip("Off = the world config's `latitude`. NOTE the sign: CfgWorlds stores NEGATIVE for "
                      "the northern hemisphere; the slider below is plain GEOGRAPHIC latitude, north "
                      "positive, so 45 means 45 N.");
    ImGui::BeginDisabled(!s.moonLatitudeOverride);
    changed |= Dev::SliderFloat("  latitude (deg N)", &s.moonLatitudeDeg, -90.0f, 90.0f, "%.1f");
    ImGui::EndDisabled();

    Dev::PanelSeparator();
    Dev::PanelHelp("Look");
    changed |= Dev::SliderFloat("Moon size scale", &s.moonSizeScale, 0.25f, 6.0f, "%.2f");
    Dev::PanelTooltip("Multiplies the TRUE angular radius (~0.26 deg, which is genuinely tiny -- the "
                      "huge moon of photographs is a long lens, not the sky). The disc is drawn "
                      "energy-conserving, so inflating it lowers its radiance and total power holds.");
    changed |= Dev::SliderFloat("Moon brightness scale", &s.moonBrightnessScale, 0.0f, 8.0f, "%.2f");
    Dev::PanelTooltip("Artistic gain on BOTH the drawn disc and the moonlight, on top of the physical "
                      "phase falloff. 0 turns the moon off entirely.");
    changed |= Dev::SliderFloat("Moon daylight scale", &s.moonDaylightScale, 0.0f, 1.0f, "%.3f");
    Dev::PanelTooltip("Fraction of the moon disc's NIGHT brightness that survives at noon.\n"
                      "The disc carries a ~24000x artistic inflation so it is visible at all under a\n"
                      "non-scotopic tonemap; kept in daylight that draws a disc at 6% of the SUN's\n"
                      "radiance, which is the reported second sun.\n"
                      "1.0 restores the old behaviour. Night is unaffected at any value.");
    changed |=
        Dev::SliderFloat("Moonlight intensity", &s.moonIntensity, 0.0f, 0.30f, "%.4f", ImGuiSliderFlags_Logarithmic);
    Dev::PanelTooltip("Full-moon irradiance as a FRACTION of the sun's. Drives BOTH the moonlight and the\n"
                      "disc's radiance, because they are the same body: the disc/ground ratio is fixed by\n"
                      "the moon's solid angle, so one knob is the honest number of knobs.\n"
                      "\n"
                      "The physical ratio is 2.5e-6, which renders as black -- night vision is scotopic and\n"
                      "this tonemap is not. What it has to beat is the engine's night AMBIENT floor, which\n"
                      "on the HDR sky-lit path works out at ~0.023 radiance at the 22:00 preset. The moon's\n"
                      "directional term is roughly intensity * 0.41, so:\n"
                      "  0.0025  moon =  4% of ambient -- present in the buffer, invisible on screen\n"
                      "  0.02    moon =  35% of ambient -- a hint of directional shading\n"
                      "  0.06    moon = 1.1x ambient -- clear moonlight, readable shadows  <- default\n"
                      "  0.11    moon = 2.0x ambient -- matches the legacy/GL33 path's moonlit look\n"
                      "Scaled by the phase curve on top, so a quarter moon is ~11x dimmer than these.");
    changed |= Dev::Checkbox("Moonlight (directional + shadows)", &s.moonLighting);
    Dev::PanelTooltip("On: the moon becomes the scene's directional light once the sun is below -2 deg "
                      "and the moon is above the horizon, and casts shadows. Off: the pre-existing "
                      "behaviour -- night lit by flat ambient only, moon purely scenery.");

    bool forcePhase = s.moonPhaseOverride >= 0.0f;
    if (Dev::Checkbox("Force phase", &forcePhase))
    {
        s.moonPhaseOverride = forcePhase ? 1.0f : -1.0f;
        changed = true;
    }
    Dev::PanelTooltip("Pin the illuminated fraction for a screenshot. Position is unchanged -- only the "
                      "look. Brightness follows the forced phase through the same curve.");
    ImGui::BeginDisabled(!forcePhase);
    {
        float forcedK = forcePhase ? s.moonPhaseOverride : 1.0f;
        if (Dev::SliderFloat("  illuminated fraction", &forcedK, 0.0f, 1.0f, "%.2f"))
        {
            s.moonPhaseOverride = forcedK;
            changed = true;
        }
        Dev::PanelHelp("  0 = new, 0.5 = quarter, 1 = full");
    }
    ImGui::EndDisabled();

    Dev::PanelSeparator();
    Dev::PanelHeading("Atmosphere");
    // Rayleigh/Mie coeffs are tiny (1/m); edit in convenient 1e-6 units.
    float rayleigh[3] = {s.rayleigh[0] * 1e6f, s.rayleigh[1] * 1e6f, s.rayleigh[2] * 1e6f};
    if (ImGui::SliderFloat3("Rayleigh (x1e-6)", rayleigh, 0.0f, 60.0f, "%.2f"))
    {
        s.rayleigh[0] = rayleigh[0] * 1e-6f;
        s.rayleigh[1] = rayleigh[1] * 1e-6f;
        s.rayleigh[2] = rayleigh[2] * 1e-6f;
        changed = true;
    }
    float mie = s.mie * 1e6f;
    if (Dev::SliderFloat("Mie (x1e-6)", &mie, 0.0f, 100.0f, "%.2f"))
    {
        s.mie = mie * 1e-6f;
        changed = true;
    }
    changed |= Dev::SliderFloat("Mie anisotropy g", &s.mieG, 0.0f, 0.99f, "%.3f");
    changed |= Dev::SliderFloat("Rayleigh height (m)", &s.rayleighHeight, 1000.0f, 16000.0f, "%.0f");
    changed |= Dev::SliderFloat("Mie height (m)", &s.mieHeight, 200.0f, 4000.0f, "%.0f");
    changed |= Dev::SliderFloat("Turbidity", &s.turbidity, 0.5f, 10.0f, "%.2f");
    changed |= Dev::SliderFloat("Ozone", &s.ozone, 0.0f, 4.0f, "%.2f");
    Dev::PanelTooltip("Ozone absorption strength — higher keeps twilight blue (the blue-hour knob)");
    changed |= Dev::ColorEdit3("Ground albedo", s.ground);
    changed |= Dev::SliderFloat("Horizon haze", &s.horizonHaze, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("Blend the sky toward the scene fog colour at the horizon so it meets the fogged terrain");
    changed |= Dev::SliderFloat("Aerial sun shadow", &s.aerialShadow, 0.0f, 4.0f, "%.2f");
    Dev::PanelTooltip("Terrain occlusion of the froxel fog: 0 = off (sun lights the haze everywhere, for A/B), "
                      "1 = physical, >1 exaggerated to make the shadowed fog / god-ray shafts obvious");
    changed |= Dev::SliderFloat("Fog closes at", &s.fogFarClose, 0.3f, 1.0f, "%.2f");
    Dev::PanelTooltip("WHERE the fog finishes, as a fraction of the draw distance.\n"
                      "1.00 = fog reaches full only exactly at the far plane -- which is also exactly where "
                      "the terrain grid stops, where the object cull ring sits, and where the map ends. "
                      "Anything still visible out there reads as the world being cut off.\n"
                      "Lower closes the horizon INSIDE all three, so everything past it is uniformly "
                      "sky-coloured airlight and there is no edge left to see.\n"
                      "Different question from the falloff below: that shapes how fast the ramp climbs, so "
                      "using it to thicken the far distance also thickens the near and mid field. This one "
                      "only ever adds cover at the far end.");
    changed |= Dev::SliderFloat("Fog falloff", &s.fogFalloff, 0.5f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    Dev::PanelTooltip("Aerial fog distance ramp exponent. High (default 3) = clear near/mid, fog only near the "
                      "draw edge. Low (~1) = dense fog throughout the view, which makes the volumetric terrain "
                      "sun-shadowing visible. Drop this to see the aerial-shadow effect.");

    Dev::PanelSeparator();
    Dev::PanelHeading("Scene lighting (experimental, HDR only)");
    Dev::PanelHelp("Fog mode and altitude layers are in the Weather tab.");
    changed |= Dev::Checkbox("Sky-based lighting", &s.skyLighting);
    Dev::PanelTooltip("Light terrain + objects FROM the atmosphere: sun = sunIntensity*exposure*transmittance "
                      "(reddens at dusk, fades below the horizon), on the physical scale. Off = legacy GL33 sun. "
                      "Toggle for A/B; the whole scene shifts scale, so exposure/grade will need re-tuning.");
    changed |= Dev::SliderFloat("Sky ambient", &s.skyAmbient, 0.0f, 2.0f, "%.2f");
    Dev::PanelTooltip("Ambient scale for sky-based lighting (Stage 1 bootstrap: the engine's ToD ambient scaled "
                      "to the physical range; real sky irradiance later).");

    Dev::PanelSeparator();
    Dev::PanelHeading("Quality");
    changed |= Dev::SliderInt("View samples", &s.viewSamples, 4, 64);
    changed |= Dev::SliderInt("Light samples", &s.lightSamples, 2, 32);

    Dev::PanelSeparator();
    Dev::PanelHeading("Clouds");
    Dev::PanelHelp("raymarched cloud shell in the sky (also reflected in water + SH ambient)");
    changed |= Dev::Checkbox("Cheap clouds in water reflection", &s.cloudReflectionCheap);
    Dev::PanelHelp("  REN-SKY-004: the reflection marches clouds with its own 32-step pipeline");
    Dev::PanelHelp("  instead of the sky's 128. Measured 2.55 -> 1.20 ms on open sea, no visible");
    Dev::PanelHelp("  difference. Off puts the sky's own march back in the reflection.");
    changed |= Dev::Checkbox("Coverage follows weather", &s.cloudCoverageFromWeather);
    Dev::PanelTooltip("On: cloud cover is driven by the world's overcast, so Zeus, the "
                      "`weather` console command and mission weather all move the sky. "
                      "Off: the Coverage slider below authors it directly.");
    ImGui::BeginDisabled(s.cloudCoverageFromWeather);
    changed |= Dev::SliderFloat("Coverage", &s.cloudCoverage, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    if (s.cloudCoverageFromWeather)
    {
        changed |= Dev::SliderFloat("  clear cover", &s.cloudCoverageClear, 0.0f, 1.0f, "%.2f");
        changed |= Dev::SliderFloat("  overcast cover", &s.cloudCoverageFull, 0.0f, 1.0f, "%.2f");
        Dev::PanelHelp("  cover at overcast 0 and 1; the Zeus slider lerps between them");
    }
    changed |= Dev::SliderFloat("Evolve (m/s)", &s.cloudEvolve, 0.0f, 60.0f, "%.1f");
    Dev::PanelHelp("  how fast clouds FORM and DISSOLVE (0 = frozen shapes that only drift");
    Dev::PanelHelp("  with the wind). Slow on purpose: ~8 turns the field over in ~20 min.");
    Dev::PanelTooltip("0 = clear sky; low = isolated cumulus; high = solid overcast deck. Also dims the "
                      "directional sun / lifts ambient as it rises (overcast reads flat).");
    changed |= Dev::SliderFloat("Density", &s.cloudDensity, 0.005f, 0.3f, "%.3f", ImGuiSliderFlags_Logarithmic);
    Dev::PanelTooltip("Cloud extinction (1/m): higher = more opaque / darker undersides");
    changed |= Dev::SliderFloat("Base altitude (m)", &s.cloudBottom, 200.0f, 6000.0f, "%.0f");
    changed |= Dev::SliderFloat("Top altitude (m)", &s.cloudTop, 400.0f, 10000.0f, "%.0f");
    changed |= ImGui::SliderFloat2("Wind (m/s)", s.cloudWind, -30.0f, 30.0f, "%.1f");
    changed |= Dev::SliderFloat("Shape size (m)", &s.cloudShapeSize, 2000.0f, 20000.0f, "%.0f");
    Dev::PanelTooltip("World size of the base cloud blobs — LARGER = less visible tiling across the map");
    changed |= Dev::SliderFloat("Detail size (m)", &s.cloudDetailSize, 400.0f, 5000.0f, "%.0f");
    Dev::PanelTooltip("Edge detail tile — keep INCOMMENSURATE with shape (not a simple multiple) so the "
                      "combined pattern's visual period is long");
    changed |= Dev::SliderFloat("Warp amount (m)", &s.cloudWarpAmount, 0.0f, 3000.0f, "%.0f");
    Dev::PanelTooltip("Domain-warp displacement — the single highest-impact anti-repetition knob (breaks "
                      "the grid regularity that makes tiling legible)");
    changed |= Dev::SliderFloat("Warp size (m)", &s.cloudWarpSize, 2000.0f, 20000.0f, "%.0f");
    changed |= Dev::SliderFloat("Weather amount", &s.cloudWeatherAmount, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("How much coverage DRIFTS across the sky (0 = uniform everywhere, which reads same-y)");
    changed |= Dev::SliderFloat("Weather size (m)", &s.cloudWeatherSize, 5000.0f, 40000.0f, "%.0f");
    Dev::PanelTooltip("World scale of the coverage drift — big, so cloudy/clear regions span the map");
    changed |= Dev::SliderFloat("Forward scatter g", &s.cloudHgG, 0.0f, 0.9f, "%.2f");
    Dev::PanelTooltip("Henyey-Greenstein anisotropy: higher = brighter silver lining toward the sun");
    changed |= Dev::SliderFloat("Powder", &s.cloudPowder, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("Beer-Powder dark-edge term (the fluffy look)");
    changed |= Dev::SliderFloat("Ambient fill", &s.cloudAmbient, 0.0f, 2.0f, "%.2f");
    Dev::PanelTooltip("Sky-ambient scale on the shadowed cloud sides");
    changed |= Dev::SliderFloat("Max distance (m)", &s.cloudMaxDist, 5000.0f, 80000.0f, "%.0f");
    Dev::PanelTooltip("March / visibility cap; the far deck dissolves into the horizon haze");

    Dev::PanelSeparator();
    Dev::PanelHeading("Second layer (high cirrus, ~7 km)");
    Dev::PanelHelp("ice cloud ABOVE the deck, so the sky has two altitudes instead of one slab");
    changed |= Dev::Checkbox("Cirrus layer", &s.cirrusEnabled);
    Dev::PanelTooltip("Off = bare upper sky. It follows the deck's coverage, so clear weather is "
                      "still clear with this on.");
    ImGui::BeginDisabled(!s.cirrusEnabled);
    changed |= Dev::Checkbox("Volumetric (thin shell march)", &s.cirrusVolumetric);
    Dev::PanelTooltip("On: 6 samples through a 900 m shell, the noise field walking its third axis "
                      "with height, so the top and bottom of the layer are different cloud and a "
                      "grazing ray travels kilometres THROUGH it.\n"
                      "Off: one ray/sheet intersection at 7 km -- cheaper, but a sheet is pierced, "
                      "never travelled through, so it has no parallax and no soft grazing edge.");
    changed |= Dev::SliderFloat("Puffiness", &s.cirrusPuffiness, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("What KIND of ice cloud it is, which up there is one question: is the air being "
                      "sheared, or is it convecting?\n"
                      "0 = cirrus fibratus -- shear draws the ice out into long parallel fibres: "
                      "elongated, heavily striated, flat.\n"
                      "1 = cirrocumulus -- shallow convection breaks the sheet into a raft of nearly "
                      "round cells: short, lumpy, and genuinely deeper (the marched shell runs 500 m "
                      "at the fibrous end, 900 m at the default and 1400 m at the puffy one, always at "
                      "the SAME step count, so the whole slider is free).\n"
                      "0.5 is exactly the look this layer shipped with. Total opacity is held roughly "
                      "constant across the range on purpose -- this is a shape control, not a "
                      "brightness one.");
    changed |= Dev::SliderFloat("Puffiness variation", &s.cirrusPuffVariation, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("How far the layer wanders from that on its own, so a still day is sometimes "
                      "flatter and sometimes lumpier without anyone touching it.\n"
                      "0 = perfectly steady (exactly the look at the puffiness above).\n"
                      "Driven by the world's weather-drift clock -- the same one the cumulus shapes "
                      "form and dissolve on -- so it is continuous, moves with sim time, freezes when "
                      "the sim does, and speeds up with the Evolve slider. A full cycle is the better "
                      "part of an hour at Evolve 8, so this is weather, not animation.\n"
                      "Neither slider can pop: the renderer eases toward the value they imply.");
    changed |= Dev::SliderFloat("Amount", &s.cirrusAmount, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("How MUCH second layer there is -- the control the on/off checkbox above was not.\n"
                      "0 = a handful of separated wisps in an otherwise empty upper sky.\n"
                      "0.5 = exactly the coverage this layer shipped with.\n"
                      "1 = a continuous cirrostratus veil with only thin breaks in it.\n"
                      "Coverage moves furthest; optical depth follows only a little (0.8x .. 1.25x), "
                      "because a sky filling with cirrus really does thicken as it fills. It still "
                      "multiplies the deck's own coverage, so clear weather stays clear at any setting.");
    changed |= Dev::SliderFloat("Match main layer", &s.cirrusMatchDeck, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("How far this layer takes on the FIRST layer's character instead of high ice cloud's.\n"
                      "0 = cirrus at 7 km, which is what it has always been.\n"
                      "1 = as close to the cumulus deck as a shell march gets: it descends to ~1.2 km above "
                      "the deck's top, features shrink to the deck's own shape scale, the 4:1 wind-shear "
                      "stretch and the fibre striation relax to isotropic cells, the shell deepens, optical "
                      "depth rises, the ice-halo phase gives way to the deck's, and a vertical self-shading "
                      "term appears so it reads as solid cloud rather than glowing fog.\n"
                      "Every target is read from the deck's LIVE settings, so it tracks the weather.\n"
                      "It can never sink into or below the deck. Costs nothing at 0.");
    changed |= Dev::SliderFloat("Edge softness", &s.cirrusSoftness, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("How definite this layer's cloud edges are.\n"
                      "0 = the silhouette it has always had: the sheet field is remapped linearly about the "
                      "coverage threshold into a hard clamp, so a cloud reaches useful density close to its "
                      "boundary and reads as solid.\n"
                      "1 = density is held down through a much wider band either side of that threshold, so "
                      "the cloud fades out over a long path and gains a deep fringe. High cloud is ice and "
                      "its edges sublimate rather than condense, so a fringe is what it should have.\n"
                      "Weighted by local coverage, so the BIG merged banks soften and isolated wisps stay "
                      "defined. Mean density is restored as it rises, so this is a shape control, not a "
                      "brightness one.");
    Dev::PanelHelp("  capture/benchmark override: WGR_CIRRUS_PUFF=<puff>[,<variation>[,<phase turns>]], "
                   "WGR_CIRRUS_LOOK=<amount>[,<match>], WGR_CIRRUS_SOFT=<softness>");
    ImGui::EndDisabled();

    Dev::PanelSeparator();
    Dev::PanelHeading("Ground shadows (CLD-020)");
    changed |= Dev::SliderFloat("Cloud shadow strength", &s.cloudShadowStrength, 0.0f, 1.0f, "%.2f");
    Dev::PanelHelp("How much the deck dims direct sun on terrain, objects, grass and water. 0 = off, and "
                   "the pass then writes fully-lit texels rather than being skipped, so switching it off "
                   "clears the shadows instead of freezing the last ones on the ground.");
    Dev::PanelHelp("Ambient is untouched, so shaded ground settles toward sky ambient rather than black. "
                   "Two limits worth knowing: the map is evaluated at sea level, so a hillside and the "
                   "valley below it get the same shadow (fine for something this soft), and it covers a "
                   "4 km square around the camera -- outside that, surfaces read fully lit rather than "
                   "dark, because missing data must never invent shadow.");

    Dev::PanelSeparator();
    Dev::PanelHeading("God rays (crepuscular shafts)");
    Dev::PanelHelp("sunlight scattering in the air between the camera and whatever is shading it");
    changed |= Dev::Checkbox("God rays", &s.godRays);
    Dev::PanelTooltip("A world-space volumetric march, NOT a radial blur around the sun on screen. Each "
                      "low-res view ray is walked and asked whether the sun is visible from that point, "
                      "against three maps that already exist: the cloud sun-transmittance map above, the "
                      "long-range terrain shadow ceiling, and the cascade shadow map (buildings, tree "
                      "crowns). So the shafts are shaped by the clouds and by real geometry, an off-screen "
                      "sun cannot smear a halo into the frame edge, and the effect fades out on its own "
                      "below the horizon and behind the camera.\n"
                      "Composited into the linear HDR scene BEFORE bloom and eye adaptation, so shafts "
                      "bloom and roll off like any other light rather than sitting on top as white paint.\n"
                      "Capture/benchmark override: WGR_GODRAYS=<on>[,<intensity>[,<density x1e-6>"
                      "[,<steps>[,<res divisor>]]]] -- WGR_GODRAYS=0 is the A/B ablation.");
    ImGui::BeginDisabled(!s.godRays);
    // Range tops out where the renderer's own clamp does (GodRaySettings::sanitized), so the
    // default of 4 sits mid-scale and can be pushed up as well as down.
    changed |= Dev::SliderFloat("Shaft intensity", &s.godRayIntensity, 0.0f, 8.0f, "%.2f");
    Dev::PanelTooltip("Multiplier on the physical single-scatter estimate. 1 = as computed from the sun "
                      "radiance, the density below and the phase function.");
    changed |= Dev::SliderFloat("Cloud shaping", &s.godRayCloudInfluence, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("How much the cloud deck carves the shafts. 1 = full, 0 = terrain and objects only, "
                      "which is the A/B for 'is this actually following the clouds'.\n"
                      "Reads the same CLD-020 map as the ground shadows above, with that slider's strength "
                      "divided back out, so dialling ground cloud shadows DOWN does not flatten the shafts. "
                      "At Cloud shadow strength = 0 the map is uniformly lit and there is no cloud shape "
                      "left to read, so this control goes inert -- keep that slider above 0.");
    float rayDensity = s.godRayDensity * 1e6f;
    // Headroom above the default of 13 (the renderer clamps at 200), so the default is not pinned
    // to the end of the track and float round-trip through *1e6 cannot land outside the range.
    if (Dev::SliderFloat("Air density (x1e-6)", &rayDensity, 0.0f, 120.0f, "%.2f"))
    {
        s.godRayDensity = rayDensity * 1e-6f;
        changed = true;
    }
    Dev::PanelTooltip("Scattering coefficient of the air the shafts live in (1/m). The engine's clear-day "
                      "Mie is 6; the default sits above it because the march is capped at the reach below "
                      "instead of running to the horizon, so the truncated tail has to be paid for here.");
    changed |= Dev::SliderFloat("Shaft reach (m)", &s.godRayDistance, 500.0f, 20000.0f, "%.0f");
    Dev::PanelTooltip("March cap. Past this the aerial-perspective froxel already owns the look; raising "
                      "it spends the SAME number of samples over a longer path, so the far shafts get "
                      "coarser rather than the pass getting slower.");
    changed |= Dev::SliderFloat("Forward scatter g", &s.godRayG, 0.0f, 0.95f, "%.2f");
    Dev::PanelTooltip("Henyey-Greenstein anisotropy: higher = a tighter, brighter beam toward the sun and "
                      "a faster fade away from it. This is also what makes the shafts vanish when you turn "
                      "your back on the sun, so it is not purely a look knob.");
    Dev::PanelHelp("  cost knobs -- the march is the whole feature's budget:");
    changed |= Dev::SliderInt("Ray steps", &s.godRaySteps, 4, 48);
    Dev::PanelTooltip("Samples per ray, distributed with the square of distance (dense near the camera, "
                      "where a shaft edge covers many pixels). Cost is linear in this.");
    changed |= Dev::SliderInt("Ray resolution divisor", &s.godRayResDiv, 1, 4);
    Dev::PanelTooltip("Screen-resolution divisor for the march. 2 = half in each axis (a quarter of the "
                      "pixels), the default; 4 quarters each axis and costs a sixteenth. The result is "
                      "upsampled with a depth-aware filter, so raising this softens the shafts rather than "
                      "haloing them around silhouettes -- but it will eventually show as steppiness on a "
                      "shaft edge that crosses the screen at a shallow angle.");
    ImGui::EndDisabled();

    Dev::PanelSeparator();
    Dev::PanelHeading("Stars");
    changed |= Dev::SliderFloat("Star brightness", &s.starIntensity, 0.0f, 4.0f, "%.2f");
    Dev::PanelTooltip("Master gain for the whole night sky: the catalogue stars AND the Milky Way "
                      "band. 0 turns both off outright (the shader early-outs), which is the "
                      "switch for the feature as a whole.");
    // NOTE: this help block used to sit below, under "Lens flare", where it read as documentation
    // for the flare. It describes the stars; it belongs here.
    Dev::PanelHelp("A bright-star CATALOGUE, not a texture and no longer a hash: each star carries a real "
                   "right ascension, declination, visual magnitude and B-V colour index, so the "
                   "constellations, the relative brightnesses and the star colours are the real ones and "
                   "stay sharp at any resolution. Magnitude is logarithmic and is compressed to about 16:1 "
                   "on screen -- the raw 1000:1 flux range would hide the faint majority and blow out "
                   "Sirius. Brighter stars also draw slightly larger discs, which is what makes a "
                   "constellation pop out of the field.\n"
                   "The diffuse Milky Way band is a separate term (points cannot make a continuous glow) "
                   "placed on the real galactic great circle. It is a PROCEDURAL PLACEHOLDER until the "
                   "NASA SVS Deep Star Maps texture is in the tree -- the band is in the right place, its "
                   "internal shape is invented. WGR_MILKYWAY=<0..3> weights it against the stars.\n"
                   "Gated to night by sun altitude, so it cannot affect a daytime sky, and added before "
                   "the cloud composite so a deck covers the stars the way it covers the sky behind them. "
                   "Being ahead of the composite was necessary and not sufficient: the cloud march is lit "
                   "by the sky radiance at its own pixel, so while that included the stars, every star "
                   "re-lit the cloud standing in front of it and came back through a solid overcast at "
                   "about 0.65 of its clear-sky brightness. The march is now handed a STAR-FREE sky, so "
                   "an overcast night has no stars in it and a broken deck keeps only the ones in its "
                   "gaps. WGR_STAR_OCCLUSION=<0..1> is the strength (1 = default, 0 = the old look "
                   "exactly); WGR_ABLATE=clouds removes the deck instead.\n"
                   "The field turns about the CELESTIAL POLE (a fixed mid-northern latitude), not about "
                   "the zenith, so stars rise in the east and arc over instead of spinning flat like a "
                   "plate. The sun's bearing is the clock -- it advances at the same 15 deg an hour the "
                   "real sky turns at. No twinkle: that needs a per-frame clock the sky UBO does not "
                   "carry, and faking it from the sun's bearing would change over hours, not seconds.");

    Dev::PanelSeparator();
    Dev::PanelHeading("Lens flare");
    bool flareOn = s.lensFlare > 0.0f;
    if (Dev::Checkbox("Lens Flare", &flareOn))
    {
        s.lensFlare = flareOn ? 1.0f : 0.0f;
        changed = true;
    }
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Occluded by terrain and objects via the scene depth the cloud pass already\n"
                          "reads, and dimmed by cloud through that ray's own transmittance -- so a thick\n"
                          "cloud crossing the sun fades it out continuously rather than popping.\n"
                          "Off costs nothing: the shader early-outs on the gain.");
    if (flareOn)
        changed |= Dev::SliderFloat("Flare intensity", &s.lensFlare, 0.05f, 4.0f, "%.2f");

    Dev::PanelSeparator();
    Dev::PanelHeading("Night floor");
    Dev::PanelHelp("authored deep-blue that fills in as the sun sets (the physical model goes near-black)");
    // Colours are normalised (click the swatch for the picker); intensity scales them.
    changed |= Dev::ColorEdit3("Zenith colour", s.nightZenith);
    changed |= Dev::ColorEdit3("Horizon colour", s.nightHorizon);
    changed |= Dev::SliderFloat("Night intensity", &s.nightIntensity, 0.0f, 0.2f, "%.4f", ImGuiSliderFlags_Logarithmic);
    changed |= Dev::SliderFloat("Day at sun elev (deg)", &s.nightStartDeg, -10.0f, 20.0f, "%.1f");
    Dev::PanelTooltip("Sun elevation at/above which it's full day (night floor off)");
    changed |= Dev::SliderFloat("Night at sun elev (deg)", &s.nightEndDeg, -20.0f, 5.0f, "%.1f");
    Dev::PanelTooltip("Sun elevation at/below which it's full night (night floor at full intensity)");

    ImGui::EndDisabled();

    if (changed)
    {
        GEngine->SetSkySettings(s);
    }

    // Copy the full authored sky state so it can be pasted into per-ToD keyframes
    // (no auto-interpolation for the sky yet; this is the hand-authoring hook).
    Dev::PanelSeparator();
    Dev::PanelHelp("Preset (copy to hand-author keyframes):");
    char preset[768];
    snprintf(preset, sizeof(preset),
             "sky: exposure=%.3f sunInt=%.2f sunRad=%.4f rayleigh=%.2f,%.2f,%.2f mie=%.2f mieG=%.3f "
             "ozone=%.2f turbidity=%.2f ground=%.3f,%.3f,%.3f haze=%.2f "
             "night=%.3f,%.3f,%.3f/%.3f,%.3f,%.3f int=%.4f band=%.1f,%.1f",
             s.exposure, s.sunIntensity, s.sunAngularRadius, s.rayleigh[0] * 1e6f, s.rayleigh[1] * 1e6f,
             s.rayleigh[2] * 1e6f, s.mie * 1e6f, s.mieG, s.ozone, s.turbidity, s.ground[0], s.ground[1], s.ground[2],
             s.horizonHaze, s.nightZenith[0], s.nightZenith[1], s.nightZenith[2], s.nightHorizon[0], s.nightHorizon[1],
             s.nightHorizon[2], s.nightIntensity, s.nightStartDeg, s.nightEndDeg);
    ImGui::SetNextItemWidth(-1.0f);
    Dev::InputText("##skyPreset", preset, sizeof(preset), ImGuiInputTextFlags_ReadOnly);
    if (Dev::Button("Copy preset to clipboard"))
        ImGui::SetClipboardText(preset);
}
void DrawCullingTab()
{
    if (!GEngine)
    {
        Dev::PanelHelp("engine not up");
        return;
    }
    if (!GEngine->SupportsCullDebug())
    {
        Dev::PanelHelp("GPU-driven rendering off (run the wgpu backend with WGR_GPU_DRIVEN=1)");
        return;
    }

    auto s = GEngine->GetCullDebugSettings();
    bool changed = false;
    changed |= ResetToDefaultsButton(s);

    changed |= Dev::Checkbox("Draw cull spheres", &s.drawSpheres);
    Dev::PanelTooltip("Green wireframe of each retained instance's frustum-cull sphere "
                      "(centre = Object Position, radius = GetRadius), drawn on top of the "
                      "scene. Shows whether a sphere sits on its object and how high it floats.");
    // Shown as "Enable" (checked = on, like the occlusion toggle below) for a coherent tab; the
    // stored flag is still disableFrustum, so invert around the checkbox.
    bool enableFrustum = !s.disableFrustum;
    if (Dev::Checkbox("Enable frustum culling", &enableFrustum))
    {
        s.disableFrustum = !enableFrustum;
        changed = true;
    }
    Dev::PanelTooltip("GPU frustum test. Turn OFF to draw everything in range: if objects that "
                      "vanish at certain pitches reappear with this off, the frustum cull is the "
                      "cause; if they still vanish, the cull is innocent and it's the draw/LOD path.");
    changed |= Dev::Checkbox("Enable occlusion culling", &s.occlusion);
    Dev::PanelTooltip("Cull retained objects hidden behind the depth-prepass occluders "
                      "(terrain + drawn objects) via a Hi-Z depth pyramid. When on, the "
                      "engine's built-in software occlusion is disabled (GPU Hi-Z replaces it).");

    Dev::PanelSeparator();
    if (Dev::Button("Dump nearby instances to log"))
    {
        s.dumpNearby = true;
        changed = true;
    }
    Dev::PanelTooltip("Log every GPU-driven instance within 60 m: live Position vs the "
                      "position captured at registration vs the terrain surface. Stand next "
                      "to a misbehaving object first. above >> 0 = floating placement; "
                      "stale > 0 = the retained buffer holds an outdated transform.");

    if (changed)
    {
        GEngine->SetCullDebugSettings(s);
    }
}

// WTR-003 water debug view names — file scope so both the Water tab combo and the
// Ctrl+Shift+W cycle hotkey can reference them. Index maps 1:1 onto WgrWaterDebugView.
static const char* const kWaterDebugViews[] = {
    "Off (normal shading)",        // 0
    "FFT displacement",            // 1
    "FFT horizontal",              // 2
    "FFT vertical",                // 3
    "FFT slope",                   // 4
    "Jacobian",                    // 5
    "Compression",                 // 6
    "Curvature",                   // 7
    "Crest energy",                // 8
    "Slope variance",              // 9
    "Material coordinate",         // 10
    "Displaced world coordinate",  // 11
    "Interaction height",          // 12
    "Interaction velocity",        // 13
    "Interaction foam/aeration",   // 14
    "Persistent foam source",      // 15
    "Persistent foam history",     // 16
    "Surface velocity",            // 17
    "Water-column depth",          // 18
    "Camera-to-surface distance",  // 19
    "SSR colour",                  // 20
    "SSR confidence",              // 21
    "Planar colour",               // 22
    "Planar geometry validity",    // 23
    "Directional sky/cloud refl.", // 24
    "Reflection-source selection", // 25
    "Refraction ray",              // 26
    "Refraction hit validity",     // 27
    "Refraction path length",      // 28
    "RGB transmittance",           // 29
    "Underwater extinction",       // 30 (reserved)
    "Underwater in-scattering",    // 31 (reserved)
    "God-ray shadow visibility",   // 32 (reserved)
    "Caustic intensity",           // 33 (reserved)
    "Whitewater particle state",   // 34 (reserved)
    "Whitewater pool occupancy",   // 35 (reserved)
    "Particle overflow",           // 36 (reserved)
    "Interaction velocity",        // 37 (WTR-012)
    "Interaction height",          // 38 (WTR-012)
    "WTR-040: Directional sky",    // 39
    "WTR-040: Directional clouds", // 40
    "WTR-040: Planar sky",         // 41
    "WTR-040: Planar clouds",      // 42
    "WTR-040: Planar geom only",   // 43
    "WTR-040: Planar validity",    // 44
    "WTR-040: SSR only",           // 45
    "WTR-040: Reflection owner",   // 46 (R=SSR, B=planar, G=directional)
};
static constexpr int kWaterDebugViewCount = static_cast<int>(std::size(kWaterDebugViews));
static_assert(kWaterDebugViewCount == 47, "Water debug view names must match WgrWaterDebugView (0..46)");

// WTR-004 standard test scene definitions
static const char* const kWaterTestScenes[] = {
    "None (Custom / Authored Defaults)",                      // 0
    "WTR-Test-01 — Seabed checkerboard (Refraction)",         // 1
    "WTR-Test-02 — Cloud pitch (Reflection pitch stability)", // 2
    "WTR-Test-03 — Ocean altitude (Cascade filtering)",       // 3
    "WTR-Test-04 — Projectile grid (Interaction solver)",     // 4
    "WTR-Test-05 — Boat wake (Vessel wake propagation)",      // 5
    "WTR-Test-06 — Explosion (Impulse & aeration)",           // 6
    "WTR-Test-07 — Underwater light (God rays & volumetric)", // 7
    "WTR-Test-08 — Waterline (Near-field submersion)",        // 8
    "WTR-Test-09 — Shoreline (Swash, foam & wet band)",       // 9
    "WTR-Test-10 — Weather transition (Calm/storm spectrum)"  // 10
};
static constexpr int kWaterTestSceneCount = static_cast<int>(std::size(kWaterTestScenes));

static void ApplyWtrTestScenePreset(Poseidon::Engine::WaterSettings& s, int index)
{
    s.testScene = index;
    switch (index)
    {
        case 1: // WTR-Test-01 — Seabed checkerboard
            s.enabled = true;
            s.alpha = 0.35f;
            s.colorExt = 0.05f;
            s.coastFade = 0.05f;
            s.foamWidth = 0.0f;
            s.foamIntensity = 0.0f;
            s.freeze.freezeTime = true;
            s.freeze.fixedTime = 12.0f;
            s.debugView = 18; // Water-column depth
            break;
        case 2: // WTR-Test-02 — Cloud pitch
            s.enabled = true;
            s.waveAmp = 0.0f; // Calm water
            s.freeze.freezeTime = true;
            s.freeze.fixedTime = 42.0f;
            s.freeze.freezeClouds = true;
            s.debugView = 24; // Directional sky/cloud reflection
            break;
        case 3: // WTR-Test-03 — Ocean altitude
            s.enabled = true;
            s.fadeStart = 1000.0f;
            s.fadeEnd = 10000.0f;
            s.freeze.freezeTime = true;
            s.freeze.fixedTime = 100.0f;
            s.debugView = 0;
            break;
        case 4: // WTR-Test-04 — Projectile grid
            s.enabled = true;
            s.freeze.freezeInteraction = false;
            s.freeze.fixedDelta = 1.0f / 60.0f;
            s.debugView = 12; // Interaction height
            break;
        case 5: // WTR-Test-05 — Boat wake
            s.enabled = true;
            s.debugView = 17; // Surface velocity
            break;
        case 6: // WTR-Test-06 — Explosion
            s.enabled = true;
            s.debugView = 14; // Interaction foam/aeration
            break;
        case 7: // WTR-Test-07 — Underwater light
            s.enabled = true;
            s.debugView = 31; // Underwater in-scattering
            break;
        case 8: // WTR-Test-08 — Waterline
            s.enabled = true;
            s.debugView = 29; // RGB transmittance
            break;
        case 9: // WTR-Test-09 — Shoreline
            s.enabled = true;
            s.swashAmp = 0.50f;
            s.swashSpeed = 0.05f;
            s.coastFade = 1.50f;
            s.foamWidth = 4.00f;
            s.foamIntensity = 1.00f;
            s.wetHeight = 0.50f;
            s.wetDarken = 0.40f;
            s.debugView = 0;
            break;
        case 10: // WTR-Test-10 — Weather transition
            s.enabled = true;
            s.freeze.freezeWeather = false;
            s.debugView = 0;
            break;
        default:
            break;
    }
}

void DrawWaterTab()
{
    if (!GEngine)
    {
        Dev::PanelHelp("engine not up");
        return;
    }
    if (!GEngine->SupportsWater())
    {
        Dev::PanelHelp("GPU water unavailable (run the wgpu backend with WGR_GPU_WATER)");
        return;
    }

    auto s = GEngine->GetWaterSettings();
    SetRifleWaterImpactSprayEnabled(s.rifleImpactSpray);
    bool changed = false;
    changed |= ResetToDefaultsButton(s);

    // TW-WATER W1 — water backend selector (Tidewater Water Plan v5 §4.3). Current OP is the
    // default and stays so until the owner accepts Tidewater Native (plan §9). Saved per map in the
    // water profile; WGR_WATER_BACKEND=0|1 in the environment overrides it for a whole session.
    {
        static constexpr bool kTidewaterPorted = true; // W2: open sea, W3a: shore waves
        const char* backendNames[] = {"Current OP", "Tidewater Native"};
        const int current = (s.waterBackend == 1) ? 1 : 0;
        if (Dev::BeginCombo("Water backend", backendNames[current]))
        {
            if (ImGui::Selectable(backendNames[0], current == 0) && current != 0)
            {
                s.waterBackend = 0;
                changed = true;
            }
            const int tidewaterFlags = kTidewaterPorted ? 0 : ImGuiSelectableFlags_Disabled;
            if (ImGui::Selectable(kTidewaterPorted ? backendNames[1] : "Tidewater Native (not ported yet)",
                                  current == 1, tidewaterFlags) &&
                current != 1)
            {
                s.waterBackend = 1;
                changed = true;
            }
            ImGui::EndCombo();
        }
        Dev::PanelTooltip("Which water implementation draws. Current OP is today's FFT/CDLOD water and the "
                          "default. Tidewater Native is the port of dgreenheck/tidewater; it replaces the water "
                          "in its mode (no layering) and switching takes effect on the next frame.");
        if (const char* env = std::getenv("WGR_WATER_BACKEND"); env != nullptr && (env[0] == '0' || env[0] == '1'))
        {
            Dev::PanelHelp("WGR_WATER_BACKEND=%c overrides this selection for the session.", env[0]);
        }
    }

    // TW-WATER — the Tidewater Native group. Every control here has a consumer in the renderer
    // (water_tw); the parts of Tidewater not ported yet are listed greyed, not faked.
    if (s.waterBackend == 1)
    {
        auto& t = s.tidewater;
        Dev::PanelSeparator();
        Dev::PanelHeading("Tidewater Native (W2 open sea, W3a shore waves)");
        Dev::PanelHelp("Weather wind changes wave amplitude only. Spectrum heading, speed, fetch, "
                       "foam and materials use the authored sea below. The Current OP controls further down do not affect this mode, except "
                       "Cascade Preset's reference seas, which map onto this group.");
        const char* twSea[] = {"Weather (Breezy shape, wind amplitude)", "Calm", "Breezy (Tidewater default)",
                               "Choppy", "Storm", "Custom (sliders)"};
        changed |= Dev::Combo("TW sea conditions", &t.seaConditions, twSea, IM_ARRAYSIZE(twSea));
        Dev::PanelTooltip("Tidewater's own sea presets set wind, fetch, chop, swell, surf and whitecaps. "
                          "Weather keeps Breezy's shape and changes only amplitude from the mean weather wind. "
                          "Custom uses authored Breezy wind and fetch with the sliders below.");
        const bool twCustom = t.seaConditions == 5;
        changed |= Dev::SliderFloat("TW amplitude", &t.amplitude, 0.0f, 3.0f, "%.2f");
        Dev::PanelTooltip("Displacement scale of all four cascades (WaterSurface amplitude).");
        ImGui::BeginDisabled(!twCustom);
        changed |= Dev::SliderFloat("TW choppiness", &t.choppiness, 0.0f, 2.0f, "%.2f");
        Dev::PanelTooltip("Horizontal displacement (lambda). Tidewater default 0.9. (Custom sea only.)");
        changed |= Dev::SliderFloat("TW swell energy", &t.swellScale, 0.0f, 2.0f, "%.2f");
        changed |= Dev::SliderFloat("TW whitecaps", &t.whitecaps, 0.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();
        changed |= Dev::SliderFloat("TW swell heading", &t.swellHeadingDeg, -180.0f, 180.0f, "%.0f deg");
        Dev::PanelTooltip("Direction the distant swell travels toward, world degrees (0 = +X, 90 = +Z).");
        changed |= Dev::SliderFloat("TW cascade scale", &t.cascadeScale, 0.25f, 4.0f, "%.2f");
        Dev::PanelTooltip("x the Tidewater cascade tiles 733 / 157 / 33.3 / 7.1 m.");
        changed |= Dev::SliderFloat("TW foam coverage", &t.foamCoverage, 0.0f, 3.0f, "%.2f");
        changed |= Dev::SliderFloat("TW foam intensity", &t.foamIntensity, 0.0f, 3.0f, "%.2f");
        changed |= Dev::Checkbox("TW screen-space reflections", &t.ssr);
        changed |= Dev::SliderFloat("TW reflection strength", &t.reflectionStrength, 0.0f, 2.0f, "%.2f");
        changed |= Dev::SliderFloat("TW water glow", &t.waterGlow, 0.0f, 1.0f, "%.2f");
        Dev::PanelTooltip("Light scattered inside the water toward the eye (W3j). 0 = the surface's reflection "
                          "only (navy, as debug view 20); 1 = Tidewater's formula on OP's light (pale cyan). "
                          "Default 0 (owner's pick): the reflection-led navy.");
        changed |= Dev::SliderFloat("TW roughness", &t.roughness, 0.0f, 0.3f, "%.3f");
        changed |= Dev::SliderFloat("TW crest subsurface", &t.sss, 0.0f, 3.0f, "%.2f");
        changed |= Dev::SliderFloat("TW backscatter", &t.backscatter, 0.0f, 0.3f, "%.3f");
        changed |= Dev::SliderFloat("TW gusts", &t.gust, 0.0f, 2.0f, "%.2f");
        changed |= Dev::SliderFloat("TW slicks", &t.slick, 0.0f, 2.0f, "%.2f");
        changed |= Dev::SliderFloat("TW windrows", &t.windrow, 0.0f, 1.0f, "%.2f");
        const char* twDebug[] = {"Off", "Back faces", "Normals", "Foam", "(4 unused)", "Lattice", "Path length",
                                 "Seabed seen through", "Sea depth", "(9 unused)", "SSR weight"};
        changed |= Dev::Combo("TW debug view", &t.debugView, twDebug, IM_ARRAYSIZE(twDebug));
        Dev::PanelSeparator();
        Dev::PanelHeading("Tidewater shore (W3a waves, W3b-c simulation + foam, W3d lips)");
        Dev::PanelHelp("Waves run in along the travel-time field of the whole map (computed in the background "
                       "a few seconds after the map loads, and again when the swell heading or sea level "
                       "moves), shoal, plunge, turn into bores and run up the sand. Height follows TW swell "
                       "energy.");
        changed |= Dev::Checkbox("TW shore waves", &t.shoreWaves);
        ImGui::BeginDisabled(!t.shoreWaves);
        ImGui::BeginDisabled(!twCustom);
        changed |= Dev::SliderFloat("TW shore period", &t.shorePeriod, 3.0f, 20.0f, "%.1f s");
        changed |= Dev::SliderFloat("TW shore amplitude", &t.shoreAmplitude, 0.0f, 3.0f, "%.2f m");
        Dev::PanelTooltip("Offshore amplitude (half the wave height) at the default swell energy 0.48. (Custom "
                          "sea only; the presets set the surf themselves.)");
        ImGui::EndDisabled();
        changed |= Dev::SliderFloat("TW shore variation", &t.shoreVariation, 0.0f, 1.5f, "%.2f");
        changed |= Dev::SliderFloat("TW breaker index", &t.shoreGamma, 0.3f, 1.5f, "%.2f");
        Dev::PanelTooltip("A wave breaks where its height exceeds this x the depth (gamma).");
        changed |= Dev::SliderFloat("TW break span", &t.shoreBreakSpan, 0.03f, 0.6f, "%.2f");
        changed |= Dev::SliderFloat("TW curl", &t.shoreCurl, 0.0f, 1.5f, "%.2f");
        changed |= Dev::SliderFloat("TW run-up", &t.shoreRunup, 0.0f, 3.0f, "%.2f");
        changed |= Dev::SliderFloat("TW surf turbidity", &t.shoreTurbidity, 0.0f, 1.0f, "%.2f");
        changed |= Dev::Checkbox("TW shore simulation (W3b, W3c)", &t.shoreSim);
        Dev::PanelTooltip("Foam carried and stranded by the water, sand that stays wet and dries, and the surf "
                          "foam look (whitewater lumps, flowing lace), in two 380 m regions on the coast nearest "
                          "the camera (they fade in over 3 s when placed). Off: the shore waves' own foam only, "
                          "and OP's coast wet band.");
        ImGui::BeginDisabled(!t.shoreSim);
        changed |= Dev::SliderFloat("TW sand drying time", &t.shoreDryTime, 2.0f, 120.0f, "%.0f s");
        changed |= Dev::SliderFloat("TW breaker lips", &t.shoreLips, 0.0f, 2.0f, "%.2f");
        Dev::PanelTooltip("Opacity of the thrown lip of plunging breakers (W3d), along the shoreline inside the "
                          "simulated regions. 0 = off.");
        changed |= Dev::SliderFloat("TW breaker spray", &t.spray, 0.0f, 3.0f, "%.2f");
        Dev::PanelTooltip("Spray thrown by the breakers (W4): drops and torn strands off the lip, the splash-up "
                          "where it lands, the roller's spray and mist drifting off with the wind. Emission gain, "
                          "Tidewater default 1; 0 = off. The rate adapts so the 32768-particle ring holds ~2 s.");
        changed |= Dev::SliderFloat("TW spray intensity", &t.sprayIntensity, 0.0f, 3.0f, "%.2f");
        Dev::PanelTooltip("Opacity of the spray sprites (Tidewater default 1).");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::BeginDisabled(true);
        bool notPorted = false;
        Dev::Checkbox("TW boat spray + refraction pass (not ported: W4b)", &notPorted);
        Dev::Checkbox("TW underwater + caustics (not ported: W5)", &notPorted);
        Dev::Checkbox("TW boat wakes (not ported: W6)", &notPorted);
        Dev::Checkbox("TW inland lakes / rivers (not drawn: owner decision pending)", &notPorted);
        ImGui::EndDisabled();
    }

    changed |= Dev::Checkbox("Enabled", &s.enabled);
    Dev::PanelTooltip("Off = draw no water surface (the seabed shows through), for A/B");

    // Keep this immediately below the master Water switch: it controls the old CPU
    // impact presentation and must be easy to find during gameplay testing.
    changed |= Dev::Checkbox("Water splash particles", &s.rifleImpactSpray);
    Dev::PanelTooltip("On by default at restrained activity. Enables/disables GPU whitewater "
                      "and water-impact particle billboards. Ripples and foam remain active.");
    SetRifleWaterImpactSprayEnabled(s.rifleImpactSpray);
    ImGui::BeginDisabled(!s.rifleImpactSpray);
    changed |= Dev::SliderFloat("Splash particle activity", &s.waterSplashParticleActivity, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    Dev::PanelTooltip("Strength of the GPU water-spray emitter when enabled. 0.25 is the restrained default; 1.00 "
                      "restores the original full effect.");

    ImGui::BeginDisabled(!s.enabled);

    Dev::PanelSeparator();
    Dev::PanelHeading("Waves (cosmetic — buoyancy stays on the flat plane)");

    const char* cascadePresets[] = {"Production Non-Harmonic (97m, 257m, 683m, 1777m domains)",
                                    "GodotOceanWaves Reference Style (88m, 57m, 16m - 3 cascades)",
                                    "Legacy Harmonic (48m, 144m, 432m, 1296m - 1296m repeat)",
                                    "Reference: Sheltered water",
                                    "Reference: Exposed coast / incoming swell",
                                    "Reference: Rough open water"};
    if (Dev::Combo("Cascade Preset", &s.cascadePreset, cascadePresets, IM_ARRAYSIZE(cascadePresets)))
    {
        if (s.cascadePreset >= 3)
        {
            s.waveScale = 1.0f;
            s.waveSpeed = 1.0f;
            s.waveAmp = 1.0f;
            s.seaStateCoupling = false;
            s.shoreWaveGain = 0.0f;
        }
        changed = true;
    }
    Dev::PanelTooltip("WTR-036C / WTR-037: Toggle between production non-harmonic coprime cascades, "
                      "GodotOceanWaves reference parity preset, and legacy harmonic cascades.");
    const char* fftResolutionPresets[] = {"256 (Performance)", "512 (Optimized default)", "1024 (Godot reference)"};
    int fftResolutionIndex = s.fftResolution == 256 ? 0 : (s.fftResolution == 1024 ? 2 : 1);
    if (Dev::Combo("FFT resolution", &fftResolutionIndex, fftResolutionPresets, IM_ARRAYSIZE(fftResolutionPresets)))
    {
        static constexpr int resolutions[] = {256, 512, 1024};
        s.fftResolution = resolutions[fftResolutionIndex];
        changed = true;
    }
    Dev::PanelTooltip("Live spectral-map resolution. 1024 matches GodotOceanWaves; 512 retains "
                      "the important long-wave modes at roughly one quarter of its FFT cost. "
                      "Changing this rebuilds only the water FFT resources.");

    changed |= Dev::SliderFloat("Amplitude", &s.waveAmp, 0.0f, 4.0f, "%.2f");
    Dev::PanelTooltip("Overall wave height scale. Kept gentle so boats never float in air.");
    changed |= Dev::SliderFloat("Choppiness", &s.waveChoppy, 0.0f, 1.5f, "%.2f");
    Dev::PanelTooltip("Horizontal steepness of the crests (Gerstner Q).");
    changed |= Dev::SliderFloat("Speed", &s.waveSpeed, 0.0f, 3.0f, "%.2f");
    changed |= Dev::SliderFloat("Scale (wavelength)", &s.waveScale, 0.25f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    Dev::PanelTooltip("Multiplies every wavelength: >1 makes larger, farther-apart waves — the main "
                      "knob for how the field reads from a distance.");

    Dev::PanelSeparator();
    Dev::PanelHeading("Distance detail (kills far-field moiré / repetition)");
    changed |= Dev::SliderFloat("Fade start (m)", &s.fadeStart, 0.0f, 4000.0f, "%.0f");
    Dev::PanelTooltip("Distance at which wave detail begins to flatten.");
    changed |= Dev::SliderFloat("Fade end (m)", &s.fadeEnd, 0.0f, 20000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    Dev::PanelTooltip("Distance by which the water is fully flat (a smooth horizon mirror). Lower this "
                      "if the airplane view still shimmers or looks tiled; raise it if distant water "
                      "looks too dead.");
    changed |= Dev::SliderFloat("De-tile warp (m)", &s.warpAmp, 0.0f, 20.0f, "%.2f");
    Dev::PanelTooltip("Low-frequency domain warp that bends the wave field off the regular grid.");

    Dev::PanelSeparator();
    Dev::PanelHeading("Shading");
    changed |= Dev::SliderFloat("Specular power", &s.specPower, 8.0f, 2000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    Dev::PanelTooltip("Sun-glint sharpness (higher = tighter highlight).");
    changed |= Dev::SliderFloat("Specular intensity", &s.specIntensity, 0.0f, 60.0f, "%.2f");
    Dev::PanelTooltip("Sun-glint brightness. Un-clamped on HDR so it blooms.");
    changed |= Dev::SliderFloat("Opacity", &s.alpha, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("Legacy composite: base opacity looking straight down (grazing angles go opaque "
                      "via Fresnel). Shared optical model: 1 = the physical transmission; lower lets "
                      "more of the background through than the water's extinction would.");
    changed |= Dev::SliderFloat("Shadow dim", &s.shadowDim, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("Terrain + CSM sun-shadow always removes the glint/direct sun on shadowed water; "
                      "this additionally darkens the shadowed surface (0 = physical sun-only removal).");

    Dev::PanelSeparator();
    Dev::PanelHeading("Surface look (energy model)");
    changed |= Dev::Checkbox("Physical composite", &s.physicalLook);
    Dev::PanelTooltip("ON: Fresnel runs uncapped, the sun lobe is evaluated at the variance-filtered "
                      "roughness at full radiance, and subsurface scattering gets its own light path. "
                      "OFF: the legacy composite (Fresnel capped at 0.43-0.72, specular scaled to 0.12x, "
                      "SSS multiplied by the near-black deep colour). Toggle for a direct A/B.");
    ImGui::BeginDisabled(!s.physicalLook);
    changed |= Dev::Checkbox("Shared optical model (WRL-002)", &s.sharedOptics);
    Dev::PanelTooltip("ON: one absorption + scattering extinction, derived from the Deep colour and "
                      "Colour clarity, drives the seabed transmission, the body glow AND the underwater "
                      "compositor, so the water seen from above and from inside is one liquid. The "
                      "background is composited in the shader at full coverage, so Opacity now lifts the "
                      "transmission instead of alpha-blending the background a second time.\n"
                      "OFF: the previous physical composite (separate hard-coded absorption curve, "
                      "seabed-visibility falloff and shallow->deep lerp) for a same-frame A/B.");
    changed |= Dev::SliderFloat("Sun glitter gain", &s.glitterGain, 0.0f, 3.0f, "%.2f");
    Dev::PanelTooltip("Sun-specular gain; 1 = the model's own energy. This is the sparkle path — raise it "
                      "if the sun track looks dull, lower it if crests fire white specks.");
    changed |= Dev::SliderFloat("Subsurface gain", &s.sssGain, 0.0f, 3.0f, "%.2f");
    Dev::PanelTooltip("Backlit-crest glow (the turquoise scatter through a wave with the sun behind it). "
                      "1 = the reference's energy mapped onto our HDR sun radiance.");
    changed |= Dev::SliderFloat("Reflection gain", &s.reflectionGain, 0.0f, 1.5f, "%.2f");
    changed |= Dev::SliderFloat("Reflection FOV padding", &s.reflectionFovPad, 1.0f, 3.0f, "%.2fx");
    changed |= Dev::SliderFloat("Reflection edge fade", &s.reflectionEdgeFade, 0.02f, 0.49f, "%.2f");
    Dev::PanelHelp("Fraction of the reflection target over which the planar reflection hands back to the "
                   "sky/environment sample. Some water cannot be covered by a planar reflection at all: "
                   "tilt down and the water beneath you maps outside the mirrored camera's frustum at ANY "
                   "field of view, so padding cannot reach it and this fade is what carries those pixels. "
                   "At 0.03 the swap read as a line across the sea, because planar has parallax-correct "
                   "clouds and the environment sample does not. Wider = the reflection loses parallax "
                   "gradually instead of ending; too wide and you lose planar parallax over most of the "
                   "water.");
    Dev::PanelHelp("How much wider the planar reflection renders than the screen's field of view. The "
                   "reflected camera otherwise inherits the main projection exactly, so a grazing "
                   "reflection needs directions that were never rendered -- the lookup runs off the edge "
                   "of the reflection target and the reflected clouds end in a visible line across the "
                   "water. 1.00 restores that. Padding trades angular resolution for coverage; the planar "
                   "sample is mip-filtered by design, so a little softness costs less than a hard edge.");
    Dev::PanelTooltip("Scales the physical Fresnel reflection weight. 1 = uncapped (correct); lower only "
                      "if the sky/planar reflection itself is wrong and you need to hide it.");
    ImGui::EndDisabled();

    Dev::PanelSeparator();
    Dev::PanelHeading("Sea state");
    changed |= Dev::Checkbox("Physical sea-state coupling", &s.seaStateCoupling);
    Dev::PanelTooltip("ON: the amplitude slider sets a wind speed, so the JONSWAP peak moves with "
                      "it and a rougher sea grows LONGER waves (height linear in the slider, "
                      "wavelength ~amp^0.75). OFF: the legacy behaviour — the whole spectrum is "
                      "scaled uniformly, so waves only get taller at the same wavelength, which "
                      "reads as short steep chop.");
    changed |= Dev::SliderFloat("Calm wave amplitude", &s.seaCalmScale, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("Wave amplitude left at DEAD CALM, as a fraction of the reference sea.\n"
                      "Was effectively 0.61 at the calm-weather mean of 1.5 m/s -- a flat day carried\n"
                      "nearly two thirds of a blustery one. Not zero: a literal ratio flattens the sea\n"
                      "to a mirror, which reads as broken water rather than calm water.\n"
                      "The authored look stays at 12 m/s for Current OP and 7 m/s for Tidewater.");
    changed |= Dev::SliderFloat("Shore breaker gain", &s.shoreWaveGain, 0.0f, 3.0f, "%.2f");
    Dev::PanelTooltip("Strength of the shoaling swell that runs in toward the beach. The train "
                      "grows (Green's law) and its crests sharpen as the water shallows.");
    changed |= Dev::Checkbox("Underwater effect", &s.underwaterEffect);
    Dev::PanelTooltip("ON by default (owner decision 2026-09-06). ON: metric "
                      "underwater extinction and in-scattering using the Water tab's "
                      "shallow/deep colours, classifying each view ray separately near the "
                      "surface so a half-submerged view keeps its above-water part, plus "
                      "world-anchored caustics on nearby seabed geometry. This also gates the "
                      "water shader's own underwater distance fog, so OFF means a submerged "
                      "view has no volume and no fog at all.");
    {
        // Shown even when the effect is off, greyed rather than hidden. Hiding them made the
        // section look like it had no settings at all, which is not what "off" should mean.
        ImGui::BeginDisabled(!s.underwaterEffect);
        ImGui::Indent();
        Dev::PanelHelp("Thresholds");
        changed |= Dev::SliderFloat("Enter depth (m)", &s.underwaterEnterDepth, 0.0f, 1.0f, "%.3f");
        Dev::PanelTooltip("How far the eye must sink BELOW the local wave-displaced surface "
                          "before the effect engages.");
        changed |= Dev::SliderFloat("Exit depth (m)", &s.underwaterExitDepth, 0.0f, 1.0f, "%.3f");
        Dev::PanelTooltip("How far the eye must rise ABOVE the surface before it releases. Keep "
                          "this larger than Enter depth — equal values make the effect flicker "
                          "while the eye rides a moving crest.");
        changed |= Dev::SliderFloat("Engage band (m)", &s.underwaterEngageBand, 0.0f, 6.0f, "%.2f");
        Dev::PanelTooltip("How far above sea level the compositor keeps running. It must run a "
                          "little while dry so a half-submerged view can be classified ray by "
                          "ray. Past the crest height it only costs the froxel and caustic "
                          "dispatches for a frame that resolves to no water.");
        Dev::PanelHelp("Colour");
        changed |= Dev::SliderFloat("Density", &s.underwaterDensity, 0.0f, 4.0f, "%.2f");
        Dev::PanelTooltip("Absorption density multiplier. Lower is clearer water and a longer "
                          "view; higher closes the view down faster. 1.0 is the tuned default.");
        changed |= Dev::SliderFloat("Colour bias", &s.underwaterColorBias, 0.0f, 1.0f, "%.2f");
        Dev::PanelTooltip("1 = absorption hue taken from the Deep colour above, so submerging "
                          "keeps the same substance you swam into. 0 = the fixed curve the "
                          "effect used before, which was unrelated to the water's own colour "
                          "and read as a more turquoise liquid. Drag between the two to "
                          "compare.");
        changed |= Dev::SliderFloat("Caustic gain", &s.underwaterCausticGain, 0.0f, 4.0f, "%.2f");
        Dev::PanelTooltip("Strength of the caustic pattern on nearby seabed geometry. Only "
                          "visible where there is geometry to receive it.");
        Dev::PanelHelp("  Shallow/Deep colour and Extinction above also drive this.");
        ImGui::Unindent();
        ImGui::EndDisabled();
    }
    // WRL-007: two named presets. Low keeps body boundaries, optics and depth correctness and
    // drops reflection/detail work; Balanced is the shipped default.
    Dev::PanelHelp("Presets (WRL-007):");
    Dev::PanelSameLine();
    if (Dev::Button("Low"))
    {
        s.lowQuality = true;
        s.geometryQuality = 0;
        s.fftResolution = 256;
        s.rifleImpactSpray = false;
        s.underwaterEffect = true;
        changed = true;
    }
    Dev::PanelSameLine();
    if (Dev::Button("Balanced"))
    {
        const Engine::WaterSettings d{};
        s.lowQuality = d.lowQuality;
        s.geometryQuality = d.geometryQuality;
        s.fftResolution = d.fftResolution;
        s.rifleImpactSpray = d.rifleImpactSpray;
        s.underwaterEffect = d.underwaterEffect;
        changed = true;
    }
    Dev::PanelTooltip("Low: no SSR/planar, Performance mesh range, 256 FFT, no spray billboards; bodies, "
                      "optics and the underwater volume stay correct. Balanced: the shipped defaults.");
    changed |= Dev::Checkbox("Low water quality (performance)", &s.lowQuality);
    Dev::PanelTooltip("Drops SSR, planar reflection, bicubic filtering and the two smallest wave "
                      "cascades. The reflected camera and its sky, terrain, objects, clouds and mip "
                      "passes are not rendered, saving their full GPU cost.");
    const char* geometryPresets[] = {"Performance (4x CDLOD range)", "Balanced (6x CDLOD range)",
                                     "Reference High (8x CDLOD range)", "Ultra (12x CDLOD range)"};
    changed |= Dev::Combo("Wave mesh quality", &s.geometryQuality, geometryPresets, IM_ARRAYSIZE(geometryPresets));
    Dev::PanelTooltip("Live coast-aware equivalent of GodotOceanWaves' clipmap mesh-quality selector. "
                      "Higher settings retain dense wave geometry farther from the camera. Balanced "
                      "is the default; Performance roughly halves visible water triangles, while "
                      "Ultra is intended for screenshots or fast GPUs. Shoreline pruning and the ocean "
                      "horizon remain active at every setting.");

    Dev::PanelSeparator();
    Dev::PanelHeading("Coast (depth-based colour + soft shoreline)");
    changed |= Dev::ColorEdit3("Shallow colour", s.shallowColor);
    Dev::PanelTooltip("Body tint of shallow water (near the coast).");
    changed |= Dev::ColorEdit3("Deep colour", s.deepColor);
    Dev::PanelTooltip("Body tint of deep water; the surface blends shallow -> deep with the water "
                      "column depth reconstructed from the opaque-depth prepass.");
    changed |= Dev::SliderFloat("Colour clarity", &s.colorExt, 0.02f, 3.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    Dev::PanelTooltip("Extinction (1/m): higher = the tint reaches the deep colour in shallower water, "
                      "so the depth colouring reads stronger. Lower = subtler, more uniform colour.\n"
                      "Shared optical model: absorption = 2.5x this (hue from the Deep colour, floor "
                      "0.12/m) and scattering = 1.6x this, for the surface AND the underwater volume.");
    changed |= Dev::SliderFloat("Soft edge width (m)", &s.coastFade, 0.0f, 3.0f, "%.2f");
    Dev::PanelTooltip("Metres of water depth over which the shoreline fades transparent -> opaque. "
                      "Large values look misty/foggy at the coast; lower it for a crisper waterline.");
    changed |= Dev::SliderFloat("Foam width (m)", &s.foamWidth, 0.0f, 8.0f, "%.2f");
    Dev::PanelTooltip("Column-depth band the churning foam spans (peaks ~1/4 in). 0 = no foam.");
    changed |= Dev::SliderFloat("Foam intensity", &s.foamIntensity, 0.0f, 2.0f, "%.2f");
    Dev::PanelTooltip("Brightness / coverage of the shoreline foam.");

    Dev::PanelSeparator();
    Dev::PanelHeading("Water bodies (WRL-003, synthetic fixtures)");
    {
        WaterBodyRegistry& registry = GetWaterBodies();
        ImGui::Text("Registered bodies: %d   generation: %u", static_cast<int>(registry.Bodies().size()),
                    registry.Generation());
        Dev::PanelHelp("Stock worlds have no inland water and imported ponds carry no elevation or extent, so "
                       "lakes and rivers are fixtures until a loader supplies them. A body is an ellipse with "
                       "its own mean level (a river reach slopes), its own small wave scale, and a profile. The "
                       "surface draws inside it at that level, the underwater compositor engages against it, "
                       "and hull points inside it float on it (Collisions.cpp). Also settable for captures "
                       "with WGR_WATER_LAKE=x,z,rx,rz,level[,wave[,profile]] and "
                       "WGR_WATER_RIVER=x,z,rx,rz,level,gradX,gradZ[,wave[,profile]].");
        const bool haveCamera = GScene && GScene->GetCamera() && GLandscape;
        ImGui::BeginDisabled(!haveCamera);
        if (Dev::Button("Add lake at camera"))
        {
            const Vector3 cam = GScene->GetCamera()->Position();
            WaterBody lake;
            lake.kind = WaterBodyKind::Lake;
            lake.profile = WaterBodyProfile::ClearLake;
            lake.centreX = cam.X();
            lake.centreZ = cam.Z();
            lake.radiusX = 80.0f;
            lake.radiusZ = 60.0f;
            // 1.5 m above the ground under the camera: the parts of the ellipse whose terrain
            // rises above that stay dry (the terrain occludes the surface), the rest floods.
            lake.level = GLandscape->SurfaceYAboveWater(cam.X(), cam.Z()) + 1.5f;
            lake.waveScale = 0.08f;
            registry.Add(lake);
        }
        Dev::PanelSameLine();
        if (Dev::Button("Add river at camera"))
        {
            const Vector3 cam = GScene->GetCamera()->Position();
            const Vector3 dir = GScene->GetCamera()->Direction();
            WaterBody river;
            river.kind = WaterBodyKind::River;
            river.profile = WaterBodyProfile::SlowRiver;
            // A reach along the camera's heading (rotated ellipse), falling downstream the way
            // the camera looks at 4 mm per metre.
            const float hl = std::sqrt(dir.X() * dir.X() + dir.Z() * dir.Z());
            const float hx = hl > 1e-4f ? dir.X() / hl : 1.0f;
            const float hz = hl > 1e-4f ? dir.Z() / hl : 0.0f;
            river.centreX = cam.X() + hx * 200.0f;
            river.centreZ = cam.Z() + hz * 200.0f;
            river.radiusX = 220.0f;
            river.radiusZ = 25.0f;
            river.rotation = std::atan2(hz, hx);
            river.level = GLandscape->SurfaceYAboveWater(cam.X(), cam.Z()) + 1.0f;
            river.gradientX = -0.004f * hx;
            river.gradientZ = -0.004f * hz;
            river.waveScale = 0.05f;
            river.bedDepth = 0.6f;
            river.flowX = hx;
            river.flowZ = hz;
            river.flowSpeed = 1.2f;
            registry.Add(river);
        }
        ImGui::EndDisabled();
        Dev::PanelSameLine();
        if (Dev::Button("Clear bodies"))
        {
            registry.Clear();
        }
        for (const WaterBody& b : registry.Bodies())
        {
            ImGui::Text("  #%u %s at (%.0f, %.0f) r=(%.0f, %.0f) level %.2f wave x%.2f", b.id,
                        b.kind == WaterBodyKind::Lake ? "lake" : "river", b.centreX, b.centreZ, b.radiusX, b.radiusZ,
                        b.level, b.waveScale);
        }
    }

    Dev::PanelSeparator();
    Dev::PanelHeading("Ripple diagnostics");
    ImGui::Text("Player water depth: %.3f m", static_cast<double>(GetPlayerWaterDepth()));
    ImGui::Text("Events submitted (total): %u   drained last frame: %u", TotalWaterInteractionsSubmitted(),
                LastWaterInteractionsDrained());
    Dev::PanelHelp("Walk into water and watch these three. Depth stays 0 = ground collision is not "
                   "reporting water under the player, and nothing downstream can help. Depth rises but "
                   "the submitted total does not = the emit conditions in Man::Simulate are not met "
                   "(it needs depth > 0.05 m, and the continuous ripple also needs horizontal speed > "
                   "0.15 m/s). Both rise but the water is flat = the events reach the renderer and the "
                   "solver or its display is the problem -- check debug view 12 (interaction height).");

    Dev::PanelSeparator();
    Dev::PanelHeading("Wave foam (whitecaps)");
    Dev::PanelHelp("Crests breaking on open water, separate from the shoreline band above. These are "
                   "different phenomena -- water breaking on land versus a crest collapsing under its own "
                   "steepness -- and 'Foam intensity' used to scale both, so calming the ocean also "
                   "stripped the surf.");
    changed |= Dev::Checkbox("Whitecaps need wind (WRL-002)", &s.whitecapWindGate);
    Dev::PanelTooltip("ON: whitecap foam, its persistent history and wind-torn spray are scaled by the "
                      "latched wind speed -- none below ~4 m/s, full at the 12 m/s reference sea -- at all "
                      "three sources. OFF: crest geometry alone decides, so a calm sea still carries a lace "
                      "of foam and floating spray sprites (the previous behaviour, for A/B). Impact and wake "
                      "splashes are not gated.");
    changed |= Dev::SliderFloat("Wave foam intensity", &s.waveFoamIntensity, 0.0f, 2.0f, "%.2f");
    Dev::PanelTooltip("Whitecaps and persistent breaker foam. 0 = none; the shoreline band is unaffected.");
    changed |= Dev::SliderFloat("Deep-water falloff", &s.waveFoamDeepFalloff, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("How strongly deep water suppresses whitecaps. 0 = waves break the same everywhere "
                      "(the old behaviour, which is why the open ocean read as too foamy). 1 = open water "
                      "stays nearly smooth and breaking concentrates in shoaling water near the coast, "
                      "which is where a real sea breaks. Depth ramp is 6 m to 45 m.");
    changed |= Dev::SliderFloat("Swash amplitude (m)", &s.swashAmp, 0.0f, 1.0f, "%.2f");
    Dev::PanelTooltip("How far the near-shore waterline oscillates in/out over the wet beach "
                      "(cosmetic — buoyancy stays on the flat plane).");
    changed |= Dev::SliderFloat("Swash speed (Hz)", &s.swashSpeed, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    Dev::PanelTooltip("Swash cycles per second (slow = long, lazy wash).");
    changed |= Dev::SliderFloat("Wet band height (m)", &s.wetHeight, 0.0f, 4.0f, "%.2f");
    Dev::PanelTooltip("Terrain side: metres above sea level the damp/darkened intertidal band "
                      "reaches, on near-flat ground only (cliffs stay dry).");
    changed |= Dev::SliderFloat("Wet darkening", &s.wetDarken, 0.3f, 1.0f, "%.2f");
    Dev::PanelTooltip("Albedo multiplier for wet sand (lower = darker). 1 = off.");

    // WTR-001 — deterministic water debug controls (dev / capture / A-B / shader-diff use only).
    // All freezes are renderer-local substitutions: they replace the UBO time/dt/seed the water,
    // interaction, foam, cloud, and underwater caustic shaders see, WITHOUT touching Glob.time
    // (gameplay + net clock) or any non-water subsystem other than the cloud wind offset (which
    // rides the same water sim clock by design). Leave "Freeze time" off to retain live animation.
    Dev::PanelSeparator();
    Dev::PanelHeading("Debug (WTR-001 — deterministic capture / A-B)");
    Dev::PanelTooltip("Holds the water-sim clock, FFT, interaction solver, foam, or clouds "
                      "at a fixed value so the same frame reproduces across launches for "
                      "before/after captures and shader-diff work. Dev-only.");
    auto& fz = s.freeze;
    bool freezeFft = fz.freezeFft;
    if (Dev::Checkbox("Freeze FFT##bool", &freezeFft))
    {
        fz.freezeFft = freezeFft;
        changed = true;
    }
    Dev::PanelTooltip("Skip Fft::dispatch: the wave-spectrum holds at its last computed state. "
                      "Combine with Freeze time to capture one frame's spectrum exactly.");
    bool freezeInteraction = fz.freezeInteraction;
    if (Dev::Checkbox("Freeze interaction solver##bool", &freezeInteraction))
    {
        fz.freezeInteraction = freezeInteraction;
        changed = true;
    }
    Dev::PanelTooltip("dt = 0 + skip Interaction::dispatch: the local ripple field holds its "
                      "last state (no decay, no propagation, no event injection).");
    bool freezeFoam = fz.freezeFoam;
    if (Dev::Checkbox("Freeze foam##bool", &freezeFoam))
    {
        fz.freezeFoam = freezeFoam;
        changed = true;
    }
    Dev::PanelTooltip("Skip Foam::dispatch: persistent foam stops advection + ageing at the "
                      "last state (use with Freeze time so the advecting surface velocity is 0).");
    bool freezeClouds = fz.freezeClouds;
    if (Dev::Checkbox("Freeze clouds##bool", &freezeClouds))
    {
        fz.freezeClouds = freezeClouds;
        changed = true;
    }
    Dev::PanelTooltip("Hold the cloud wind world offset at fixed time, so the cloud shell does "
                      "not drift between captures. Implicit when Freeze time is on.");
    bool freezeWeather = fz.freezeWeather;
    if (Dev::Checkbox("Freeze weather##bool", &freezeWeather))
    {
        fz.freezeWeather = freezeWeather;
        changed = true;
    }
    Dev::PanelTooltip("Reserve bit for future weather threading (no per-frame weather "
                      "recomputation today). Implicit when Freeze time is on, since the "
                      "interaction weather vector recomputes off the frozen time.");
    bool freezeTime = fz.freezeTime;
    if (Dev::Checkbox("Freeze water-sim clock##bool", &freezeTime))
    {
        fz.freezeTime = freezeTime;
        changed = true;
    }
    Dev::PanelTooltip("Hold the water-sim clock passed to the FFT, interaction, foam and "
                      "underwater caustic shaders at fixed time. Clouds honour this too.");
    changed |= Dev::SliderFloat("Fixed time (s)", &fz.fixedTime, 0.0f, 3600.0f, "%.2f");
    Dev::PanelTooltip("Seconds (replaces Glob.time when Freeze time or Freeze clouds is on). "
                      "One value keeps the four sim clocks (water, interaction, cloud, "
                      "underwater caustic) coherent for a single reproducible test frame.");
    changed |= Dev::SliderInt("FFT seed override", &fz.fftSeed, -1, 0x00ff'ffff);
    Dev::PanelTooltip("Replaces fft_control[1] (authored default 1337). -1 = use 1337 (no "
                      "swap). Any non-negative value rewrites the spectrum's random field on "
                      "the next dispatch; two runs with the same seed reproduce h0 bit-for-bit.");
    changed |= Dev::SliderFloat("Fixed delta (s)", &fz.fixedDelta, 0.0f, 1.0f / 30.0f, "%.4f");
    Dev::PanelTooltip("Fixes the interaction-solver step regardless of render FPS (0 = use the "
                      "live frame delta clamped to 1/30). For WTR-063 fixed-timestep validation; "
                      "leave 0 for capture mode (Freeze interaction is the standard freeze).");
    changed |= Dev::SliderInt("Camera path frame", &fz.cameraPathFrame, -1, 100000);
    Dev::PanelTooltip("WTR-001 foundation only: when >= 0 the renderer tags each frame's water "
                      "UBO digest with this integer so two runs compare frame-by-frame. The "
                      "camera-path recorder itself is a separate WTR-004 work package; here we "
                      "expose just the integer index for manual capture-then-replay audits.");

    ImGui::EndDisabled();

    // WTR-003 — water debug views. Replaces the water surface shading with a single diagnostic
    // (WgrWaterDebugView). Kept outside the disabled block so it works even with the water
    // surface toggled off. Reserved slots (underwater/god-ray/caustic/whitewater) render black
    // until their passes exist. The combo index maps 1:1 onto WgrWaterDebugView.
    Dev::PanelSeparator();
    Dev::PanelHeading("Debug views (WTR-003)  [Ctrl+Shift+W cycles]");
    // kWaterDebugViews / kWaterDebugViewCount are at file scope (shared with the hotkey).
    int debugView = (s.debugView >= 0 && s.debugView < kWaterDebugViewCount) ? s.debugView : 0;
    if (Dev::Combo("Debug view", &debugView, kWaterDebugViews, (int)std::size(kWaterDebugViews)))
    {
        s.debugView = debugView;
        changed = true;
    }
    Dev::PanelTooltip("Replaces the water surface output with the selected diagnostic. FFT / "
                      "interaction / foam views aggregate the four cascades; interaction & foam "
                      "fields read zero outside the 256 m camera domain. Reserved entries have no "
                      "backing pass yet and render black. wgpu backend only.");

    // WTR-004 — Standard test harness (deterministic animation, frame-stepping, snapshot/restore)
    Dev::PanelSeparator();
    Dev::PanelHeading("Standard test harness (WTR-004)");
    auto& harness = Poseidon::WtrTestHarness::Instance();
    int testScene = harness.IsActive() ? harness.GetCurrentPresetId()
                                       : ((s.testScene >= 0 && s.testScene < kWaterTestSceneCount) ? s.testScene : 0);
    if (Dev::Combo("Test scene preset", &testScene, kWaterTestScenes, kWaterTestSceneCount))
    {
        s.testScene = testScene;
        harness.SelectPreset(testScene, s, s.debugView);
        changed = true;
    }
    Dev::PanelTooltip("Selects a standard WTR-Test-01..10 test scene preset.");

    if (testScene > 0)
    {
        const auto* info = harness.GetPresetInfo(testScene);
        if (info)
        {
            if (info->availability == Poseidon::WtrTestAvailability::Available)
            {
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "Status: Available");
            }
            else if (info->availability == Poseidon::WtrTestAvailability::Partial)
            {
                ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), "Status: %s", info->statusReason);
            }
            else
            {
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.2f, 1.0f), "Status: %s", info->statusReason);
            }
        }

        ImGui::Spacing();
        if (!harness.IsActive())
        {
            if (Dev::Button("Start Test Harness"))
            {
                harness.Start(s, s.debugView);
                changed = true;
            }
        }
        else
        {
            if (Dev::Button(harness.IsPaused() ? "Resume" : "Pause"))
            {
                harness.Pause();
            }
            Dev::PanelSameLine();
            if (Dev::Button("Step Frame"))
            {
                harness.StepFrame(s);
                changed = true;
            }
            Dev::PanelSameLine();
            if (Dev::Button("Restart"))
            {
                harness.Restart(s);
                changed = true;
            }
            Dev::PanelSameLine();
            if (Dev::Button("Stop & Restore Settings"))
            {
                int restoredDebugView = s.debugView;
                harness.Stop(s, restoredDebugView);
                s.debugView = restoredDebugView;
                changed = true;
            }

            ImGui::Text("Active Frame: %llu | Time: %.3f s | Triggers: %u",
                        static_cast<unsigned long long>(harness.GetFrameIndex()),
                        static_cast<double>(harness.GetFrameIndex() * harness.GetFixedDeltaTime()),
                        harness.GetTriggeredEventCount());

            if (Dev::Button("Copy Metadata Log JSON"))
            {
                Vector3 dummyPos(100.0f, 5.0f, 100.0f);
                Vector3 dummyRot(0.0f, 0.0f, 0.0f);
                std::string logJson = harness.GenerateMetadataLog(s, dummyPos, dummyRot);
                ImGui::SetClipboardText(logJson.c_str());
            }
        }
    }

    if (changed)
        GEngine->SetWaterSettings(s);

    // Copy the full authored water look so the tuned values can be pasted back as the
    // Engine::WaterSettings defaults (like the Sky / Tonemap tabs).
    Dev::PanelSeparator();
    Dev::PanelHelp("Preset (copy to persist as defaults):");
    char preset[720];
    snprintf(preset, sizeof(preset),
             "water: amp=%.2f choppy=%.2f speed=%.2f scale=%.2f fade=%.0f,%.0f warp=%.2f "
             "spec=%.0f,%.2f alpha=%.2f shadowDim=%.2f shallow=%.3f,%.3f,%.3f deep=%.3f,%.3f,%.3f "
             "clarity=%.3f coastFade=%.2f foam=%.2f,%.2f swash=%.2f,%.3f wet=%.2f,%.2f",
             s.waveAmp, s.waveChoppy, s.waveSpeed, s.waveScale, s.fadeStart, s.fadeEnd, s.warpAmp, s.specPower,
             s.specIntensity, s.alpha, s.shadowDim, s.shallowColor[0], s.shallowColor[1], s.shallowColor[2],
             s.deepColor[0], s.deepColor[1], s.deepColor[2], s.colorExt, s.coastFade, s.foamWidth, s.foamIntensity,
             s.swashAmp, s.swashSpeed, s.wetHeight, s.wetDarken);
    ImGui::SetNextItemWidth(-1.0f);
    Dev::InputText("##waterPreset", preset, sizeof(preset), ImGuiInputTextFlags_ReadOnly);
    if (Dev::Button("Copy preset to clipboard"))
        ImGui::SetClipboardText(preset);

    // WTR-002 — per-region GPU pass timings (timestamp queries; the renderer harvests the
    // readback asynchronously, so values lag the displayed frame by the ring depth, ~2-3
    // frames). "n/a" rows are reserved spec slots (no standalone pass yet) or passes that
    // haven't run since launch (e.g. frozen dispatches, spectrum init after the first frame).
    Dev::PanelSeparator();
    Dev::PanelHeading("GPU timings (WTR-002)");
    float gpuMs[64];
    const int gpuRegions = GEngine->GetWaterGpuTimings(gpuMs, 64);
    if (gpuRegions <= 0)
    {
        Dev::PanelHelp("Unavailable (adapter lacks TIMESTAMP_QUERY / non-wgpu backend).");
    }
    else if (ImGui::BeginTable("wtrGpuTimings", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        float gpuTotal = 0.0f;
        // Grass shares the region array; its rows live in the Grass tab.
        const int waterRegions = std::min(gpuRegions, (int)Engine::kWaterGpuRegionEnd);
        for (int i = 0; i < waterRegions; ++i)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Dev::PanelHeading(GEngine->GetWaterGpuTimingName(i));
            ImGui::TableNextColumn();
            if (gpuMs[i] < 0.0f)
                ImGui::TextDisabled("n/a");
            else
            {
                ImGui::Text("%.3f ms", gpuMs[i]);
                gpuTotal += gpuMs[i];
            }
        }
        ImGui::EndTable();
        ImGui::Text("Measured total: %.3f ms", gpuTotal);
        Dev::PanelTooltip("Sum of the rows above (last completed frame). Not the water "
                          "pipeline's wall-clock cost: passes may overlap on the GPU and "
                          "reserved rows are folded into their host pass (SSR/refraction "
                          "inside Water draw, caustics inside Underwater composite).");
    }
}
void DrawMouseTab()
{
    // Plain field writes into live GInput.mouse — no Defer needed (cf. DrawCheatsTab).
    auto& sub = InputSubsystem::Instance();
    MouseTuning& t = sub.GetMouseTuning();

    Dev::PanelHeading("Player settings (final)");
    Dev::PanelSeparator();

    int dpiIdx = 0; // Off
    if (t.dpiNormalize)
    {
        int bestDiff = 1 << 30;
        for (int i = 1; i < kMouseDpiPresetCount; ++i)
        {
            int d = t.mouseDpi - kMouseDpiPresets[i];
            if (d < 0)
                d = -d;
            if (d < bestDiff)
            {
                bestDiff = d;
                dpiIdx = i;
            }
        }
    }
    if (Dev::Combo("Mouse DPI", &dpiIdx, kMouseDpiLabels, kMouseDpiPresetCount))
    {
        t.dpiNormalize = dpiIdx > 0;
        if (dpiIdx > 0)
            t.mouseDpi = kMouseDpiPresets[dpiIdx];
    }
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Set this to your mouse's DPI. The game then feels like %d DPI at any hardware.\n"
                          "Off = classic (no compensation).",
                          t.referenceDpi);

    float sx = sub.GetMouseSensitivityX();
    if (Dev::SliderFloat("Sensitivity X", &sx, t.SensMin(), t.SensMax(), "%.3f"))
        sub.SetMouseSensitivityX(sx);
    float sy = sub.GetMouseSensitivityY();
    if (Dev::SliderFloat("Sensitivity Y", &sy, t.SensMin(), t.SensMax(), "%.3f"))
        sub.SetMouseSensitivityY(sy);

    bool rev = sub.IsReverseMouse();
    if (Dev::Checkbox("Invert Y axis", &rev))
        sub.SetReverseMouse(rev);
    bool swap = sub.IsMouseButtonsReversed();
    if (Dev::Checkbox("Swap mouse buttons", &swap))
        sub.SetMouseButtonsReversed(swap);

    ImGui::Spacing();
    Dev::PanelHeading("Final values (live)");
    Dev::PanelSeparator();

    // Cursor math mirrors MouseState::Update (kCursorScaleX = 1/200; screen = 2 NDC).
    const float dpiF = t.DpiFactor();
    const float perCountX = sx * t.baseScale * dpiF / 200.0f;
    ImGui::Text("Reference DPI: %d   |   DPI factor: %.3f", t.referenceDpi, dpiF);
    ImGui::Text("Effective sensitivity X (x baseScale): %.3f", sx * t.baseScale * dpiF);
    if (perCountX > 0.0f)
    {
        const float countsCrossX = 2.0f / perCountX;
        ImGui::Text("Counts to cross screen (X): %.0f", countsCrossX);
        if (t.dpiNormalize && t.mouseDpi > 0)
        {
            // Normalized: physical hand travel is DPI-independent (mouseDpi cancels).
            const float inch = countsCrossX / static_cast<float>(t.mouseDpi);
            ImGui::Text("Hand travel to cross screen (X): %.2f in / %.1f cm", inch, inch * 2.54f);
            Dev::PanelHelp("  same physical feel at every DPI — normalization working");
        }
        else
        {
            Dev::PanelHelp("  Off: raw counts — physical feel depends on your hardware DPI");
        }
    }
    if (s_window)
        ImGui::Text("SDL display scale: %.2f   pixel density: %.2f", SDL_GetWindowDisplayScale(s_window),
                    SDL_GetWindowPixelDensity(s_window));
    Dev::PanelHelp("Mouse moves over this panel are captured by ImGui — close it to feel changes in game.");

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Advanced (dev only)"))
    {
        Dev::SliderFloat("Base scale", &t.baseScale, 0.1f, 3.0f, "%.3f");
        if (Dev::PanelItemHovered())
            ImGui::SetTooltip("Master look scale (was the hard-coded 1.5).");
        Dev::SliderInt("Reference DPI", &t.referenceDpi, 100, 3200);
        Dev::SliderFloat("Smoothing", &t.smoothing, 0.0f, 0.95f, "%.2f");
        Dev::Checkbox("Acceleration", &t.acceleration);
        ImGui::BeginDisabled(!t.acceleration);
        Dev::SliderFloat("Accel exponent", &t.accelExponent, 1.0f, 2.0f, "%.2f");
        ImGui::EndDisabled();
        Dev::SliderFloat("Menu cursor scale", &t.menuCursorScale, 0.1f, 4.0f, "%.2f");
        if (Dev::Checkbox("Extended sensitivity range", &t.extendedRange))
        {
            sub.SetMouseSensitivityX(std::clamp(sub.GetMouseSensitivityX(), t.SensMin(), t.SensMax()));
            sub.SetMouseSensitivityY(std::clamp(sub.GetMouseSensitivityY(), t.SensMin(), t.SensMax()));
        }
        if (Dev::PanelItemHovered())
            ImGui::SetTooltip("Off = legacy 0.5..2.0 sensitivity range. On = 0.05..3.0.");
    }

    ImGui::Spacing();
    Dev::PanelSeparator();
    if (Dev::Button("Reset tuning to classic"))
        t = MouseTuning{};
    Dev::PanelSameLine();
    if (Dev::Button("Save to mouse.cfg"))
        Defer([] { InputSubsystem::Instance().SaveKeys(); });
    if (Dev::PanelItemHovered())
        ImGui::SetTooltip("Persist sensitivity + tuning to mouse.cfg (also written for old builds).");
}

void DrawMainWindow()
{
    ImGui::SetNextWindowSize(ImVec2(560, 480), ImGuiCond_FirstUseEver);
    ImGui::Begin("Poseidon Dev Panel");

    // Above the tab bar, so it filters whichever tab is open -- and because it
    // also resets the per-frame attribution state the wrappers use, it has to
    // run before any of them. ImGui executes only the selected tab's body, so
    // once per frame is once per tab.
    Dev::DrawPanelSearch();

    // Reset-all, beside the search because both act on the same set: every
    // control the panel has drawn. Behind a confirmation, since a dev session can
    // be an hour of tuning and there is no undo.
    Dev::PanelSameLine();
    if (Dev::SmallButton("Reset settings"))
    {
        ImGui::OpenPopup("Reset dev settings?");
    }
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip(
            "Named settings only -- the dialog lists exactly which.\n"
            "\n"
            "Not every setting: a tab is reset where it says how, and most tabs\n"
            "do not yet. The pointer-based version that WOULD have covered\n"
            "everything wrote into stack addresses that die with the frame.");
    }
    if (ImGui::BeginPopupModal("Reset dev settings?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Restore these to their defaults?");
        ImGui::BulletText("Physics probes, world colliders, penetration, ray audit");
        ImGui::BulletText("Grenade fuse and bounce tuning");
        ImGui::BulletText("Loose-object physics");
        ImGui::BulletText("Fixed step");
        ImGui::BulletText("Ballistics recorder and aim model");
        ImGui::BulletText("Picture Mode / depth of field");
        ImGui::BulletText("Street lamps, bulb glow, local shadows and beam shape");
        ImGui::BulletText("Sky, water and road appearance");
        ImGui::BulletText("Fog appearance and amount (snow's automatic fog still applies)");
        ImGui::BulletText("Snow settings (existing cover and tracks are retained)");
        ImGui::Separator();
        ImGui::TextDisabled("Everything else is untouched. A tab is reset only where it says\n"
                            "how, and most do not yet -- so this list IS the coverage, not a\n"
                            "summary of it. There is no undo.");
        ImGui::Separator();
        if (Dev::Button("Reset"))
        {
            Dev::ResetPanelSettings();
            ImGui::CloseCurrentPopup();
        }
        Dev::PanelSameLine();
        if (Dev::Button("Cancel"))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginTabBar("DevPanelTabs"))
    {
        if (Dev::PanelTabItem("Zeus"))
        {
            DrawZeusTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Cheats"))
        {
            DrawCheatsTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Mods"))
        {
            DrawModsTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Game"))
        {
            DrawGameTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Mouse"))
        {
            DrawMouseTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Console"))
        {
            DrawConsoleTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Profile"))
        {
            DrawProfileTab();
            ImGui::EndTabItem();
        }
        // Fully qualified: the tab body lives in BallisticsTab.cpp, so this call does not
        // depend on where this file's anonymous namespace happens to sit.
        if (Dev::PanelTabItem("Ballistics"))
        {
            Poseidon::Dev::DrawBallisticsTab();
            ImGui::EndTabItem();
        }
        // Screenshot controls, kept apart from the diagnostics they have nothing to do with.
        if (Dev::PanelTabItem("Physics"))
        {
            Poseidon::Dev::DrawPhysicsTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Map Editor"))
        {
            const bool available = GWorld && GLandscape && GWorld->GetMode() != GModeNetware;
            ImGui::BeginDisabled(!available);
            const auto editorAction = Dev::DrawMapEditorTab();
            if (editorAction == Dev::MapEditorAction::RestoreTerrain)
            {
                s_terrainBrushDragging = false;
                Defer([] {
                    s_zeusStatus = Dev::RestoreEditedTerrain() ? "Terrain restored." :
                        "Terrain restore refused: baseline/grid mismatch or collision creation failed.";
                });
            }
            if (editorAction == Dev::MapEditorAction::UndoCave)
                Defer([] { Dev::RemoveLastEditorCave(); s_zeusStatus = Dev::EditorCaveStatus(); });
            if (editorAction == Dev::MapEditorAction::RotateCave)
            {
                const auto id = Dev::MapEditorActionId();
                const float heading = Dev::MapEditorActionHeading();
                Defer(
                    [id, heading]
                    {
                        Dev::RotateEditorCave(id, heading);
                        s_zeusStatus = Dev::EditorCaveStatus();
                    });
            }
            if (editorAction == Dev::MapEditorAction::DeleteCave)
            {
                const auto id = Dev::MapEditorActionId();
                Defer(
                    [id]
                    {
                        Dev::DeleteEditorCave(id);
                        s_zeusStatus = Dev::EditorCaveStatus();
                    });
            }
            if (editorAction == Dev::MapEditorAction::StartCave)
            {
                EnableZeusCamera();
                if (s_zeusCamera)
                {
                    Dev::TerrainBrush().enabled = false;
                    Dev::CaveEditor().enabled = true;
                    SetVisible(false);
                }
            }
            if (editorAction == Dev::MapEditorAction::StartPainting)
            {
                EnableZeusCamera();
                if (s_zeusCamera)
                {
                    Dev::CaveEditor().enabled = false;
                    Dev::TerrainBrush().enabled = true;
                    SetVisible(false);
                }
            }
            ImGui::EndDisabled();
            if (!available) ImGui::TextWrapped("Load a single-player mission to edit terrain.");
            ImGui::EndTabItem();
        }
        // Not a diagnostic: this tab CHANGES what the AI does. See AITab.cpp.
        if (Dev::PanelTabItem("AI"))
        {
            Poseidon::Dev::DrawAITab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Fixed Step"))
        {
            Poseidon::Dev::DrawFixedStepTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Picture Mode"))
        {
            Poseidon::Dev::DrawPictureModeTab();
            ImGui::EndTabItem();
        }
        // Same arrangement: bodies in WeatherTab.cpp / SmokeTab.cpp.
        if (Dev::PanelTabItem("Weather"))
        {
            Poseidon::Dev::DrawWeatherTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Smoke"))
        {
            Poseidon::Dev::DrawSmokeTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Flight"))
        {
            Poseidon::Dev::DrawFlightTab();
            ImGui::EndTabItem();
        }
        ImGuiTabItemFlags memoryFlags = 0;
        if (s_selectMemoryTab)
        {
            memoryFlags = ImGuiTabItemFlags_SetSelected;
            s_selectMemoryTab = false;
        }
        if (Dev::PanelTabItem("Memory", nullptr, memoryFlags))
        {
            DrawMemoryTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Perf"))
        {
            DrawPerfTab();
            ImGui::EndTabItem();
        }
        // Body in StreamingTab.cpp, same arrangement as Ballistics/Weather.
        if (Dev::PanelTabItem("Streaming"))
        {
            Poseidon::Dev::DrawStreamingTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Font"))
        {
            DrawFontTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Aspect"))
        {
            DrawAspectTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Render"))
        {
            DrawRenderTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Material Debug"))
        {
            DrawMaterialDebugTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Graphics"))
        {
            DrawTemporalTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Tonemap"))
        {
            DrawTonemapTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Sky"))
        {
            DrawSkyTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Water"))
        {
            DrawWaterTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Culling"))
        {
            DrawCullingTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Foliage"))
        {
            DrawFoliageTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Amb. Occlusion"))
        {
            DrawAmbientOcclusionTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Interior Sky"))
        {
            DrawInteriorSkyTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("GI"))
        {
            DrawGiTab();
            ImGui::EndTabItem();
        }
        if (Dev::PanelTabItem("Grass"))
        {
            DrawGrassTab();
            ImGui::EndTabItem();
        }
        ImGuiTabItemFlags shadowFlags = 0;
        if (s_selectShadowsTab)
        {
            shadowFlags = ImGuiTabItemFlags_SetSelected;
            s_selectShadowsTab = false;
        }
        if (Dev::PanelTabItem("Shadows", nullptr, shadowFlags))
        {
            DrawShadowsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    Dev::PanelSeparator();
    Dev::PanelHelp("Ctrl+` / Ctrl+; to hide");
    ImGui::End();
}
} // namespace

namespace
{
void CreateSharedContext(SDL_Window* window)
{
    s_window = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // no imgui.ini side-effects
    ImGui::StyleColorsDark();
}

void UpdateEngineTextures(ImVector<ImTextureData*>* textures)
{
    if (!textures || !GEngine || !GEngine->SupportsOverlayRenderer())
        return;
    for (ImTextureData* tex : *textures)
    {
        if (tex->Status == ImTextureStatus_WantCreate)
        {
            IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
            const uint64_t id =
                GEngine->OverlayTextureCreate(tex->Width, tex->Height, static_cast<const uint8_t*>(tex->GetPixels()));
            if (id == 0)
                continue;
            tex->SetTexID(static_cast<ImTextureID>(id));
            tex->SetStatus(ImTextureStatus_OK);
        }
        else if (tex->Status == ImTextureStatus_WantUpdates)
        {
            // Full re-upload; the FFI has no sub-rect update and atlas textures are small.
            GEngine->OverlayTextureUpdate(static_cast<uint64_t>(tex->TexID), tex->Width, tex->Height,
                                          static_cast<const uint8_t*>(tex->GetPixels()));
            tex->SetStatus(ImTextureStatus_OK);
        }
        else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
        {
            GEngine->OverlayTextureDestroy(static_cast<uint64_t>(tex->TexID));
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

// Flatten ImGui's draw lists into one vertex/index pool + scissored draw
// records and hand them to the engine for composition over the frame.
void RenderDrawDataEngine(ImDrawData* dd)
{
    static_assert(sizeof(ImDrawIdx) == 2, "engine overlay indices are 16-bit");
    static_assert(sizeof(Engine::OverlayVertex) == sizeof(ImDrawVert), "OverlayVertex must mirror ImDrawVert");

    UpdateEngineTextures(dd->Textures);
    if (!GEngine || !GEngine->SupportsOverlayRenderer())
        return;

    const ImVec2 off = dd->DisplayPos;
    const ImVec2 scale = dd->FramebufferScale;

    static std::vector<Engine::OverlayVertex> verts;
    static std::vector<uint16_t> indices;
    static std::vector<Engine::OverlayDrawCmd> cmds;
    verts.clear();
    indices.clear();
    cmds.clear();
    verts.reserve(dd->TotalVtxCount);
    indices.reserve(dd->TotalIdxCount);

    for (int li = 0; li < dd->CmdListsCount; li++)
    {
        const ImDrawList* dl = dd->CmdLists[li];
        const uint32_t vtxBase = static_cast<uint32_t>(verts.size());
        const uint32_t idxBase = static_cast<uint32_t>(indices.size());
        for (const ImDrawVert& v : dl->VtxBuffer)
        {
            verts.push_back({(v.pos.x - off.x) * scale.x, (v.pos.y - off.y) * scale.y, v.uv.x, v.uv.y, v.col});
        }
        indices.insert(indices.end(), dl->IdxBuffer.begin(), dl->IdxBuffer.end());
        for (const ImDrawCmd& c : dl->CmdBuffer)
        {
            if (c.UserCallback)
            {
                if (c.UserCallback != ImDrawCallback_ResetRenderState)
                    c.UserCallback(dl, &c);
                continue;
            }
            if (c.ElemCount == 0)
                continue;
            Engine::OverlayDrawCmd oc;
            oc.clip[0] = (c.ClipRect.x - off.x) * scale.x;
            oc.clip[1] = (c.ClipRect.y - off.y) * scale.y;
            oc.clip[2] = (c.ClipRect.z - off.x) * scale.x;
            oc.clip[3] = (c.ClipRect.w - off.y) * scale.y;
            if (oc.clip[2] <= oc.clip[0] || oc.clip[3] <= oc.clip[1])
                continue;
            oc.texture = static_cast<uint64_t>(c.GetTexID());
            oc.firstIndex = idxBase + c.IdxOffset;
            oc.indexCount = c.ElemCount;
            oc.baseVertex = vtxBase + c.VtxOffset;
            cmds.push_back(oc);
        }
    }

    GEngine->SubmitOverlay(verts.data(), static_cast<int>(verts.size()), indices.data(),
                           static_cast<int>(indices.size()), cmds.data(), static_cast<int>(cmds.size()));
}
} // namespace

void Init(SDL_Window* window, void* glContext)
{
    if (s_initialized)
        return;
    CreateSharedContext(window);

    if (!ImGui_ImplSDL3_InitForOpenGL(window, glContext))
    {
        LOG_ERROR(Graphics, "DebugOverlay: ImGui_ImplSDL3_InitForOpenGL failed");
        return;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 330"))
    {
        LOG_ERROR(Graphics, "DebugOverlay: ImGui_ImplOpenGL3_Init failed");
        return;
    }

    s_backend = RenderBackend::OpenGL3;
    s_initialized = true;
    LOG_INFO(Graphics, "DebugOverlay: ImGui initialized (press Ctrl+` / Ctrl+; to toggle)");
}

void InitForEngine(SDL_Window* window)
{
    if (s_initialized)
        return;
    CreateSharedContext(window);

    if (!ImGui_ImplSDL3_InitForOther(window))
    {
        LOG_ERROR(Graphics, "DebugOverlay: ImGui_ImplSDL3_InitForOther failed");
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "imgui_impl_poseidon_overlay";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    s_backend = RenderBackend::Engine;
    s_initialized = true;
    LOG_INFO(Graphics, "DebugOverlay: ImGui initialized on the engine overlay backend "
                       "(press Ctrl+` / Ctrl+; to toggle)");
}

void Shutdown()
{
    if (!s_initialized)
        return;
    if (s_backend == RenderBackend::OpenGL3)
    {
        ImGui_ImplOpenGL3_Shutdown();
    }
    else
    {
        // Release engine textures while the engine is still alive; if it is
        // already gone the renderer teardown frees them anyway.
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        {
            if (tex->RefCount != 1)
                continue;
            if (GEngine && GEngine->SupportsOverlayRenderer() && tex->TexID != ImTextureID_Invalid)
                GEngine->OverlayTextureDestroy(static_cast<uint64_t>(tex->TexID));
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    s_initialized = false;
}

void ProcessEvent(const SDL_Event& event)
{
    if (SuspendMultiplayerTools())
        return;
    if (!s_initialized)
        return;
    s_zeusConsumeMouseEvent = false;
    s_zeusConsumeKeyboardEvent = false;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) s_terrainBrushDragging = false;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED && s_zeusCamera && !s_visible)
    {
        // SDL can restore the desktop cursor when a window regains focus even
        // though the game remains in absolute mouse mode for Zeus editing.
        SDL_HideCursor();
    }
    if (!s_visible && s_zeusCamera)
    {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT)
        {
            s_zeusConsumeMouseEvent = true;
            const SDL_Keymod modifiers = SDL_GetModState();
            if (Dev::CaveEditor().enabled)
            {
                const ZeusPoint cursor = ZeusCursorPixel();
                s_zeusSuppressNextMouseUp = true;
                Defer([cursor] {
                    Dev::CreateEditorCaveAtPixel(cursor.x,cursor.y);
                    s_zeusStatus = Dev::EditorCaveStatus();
                });
            }
            else if (Dev::TerrainBrush().enabled)
            {
                s_terrainBrushDragging = true;
                s_terrainBrushTickMs = SDL_GetTicks();
                s_zeusSuppressNextMouseUp = true;
                s_zeusStatus = "Terrain brush: hold left mouse to paint.";
            }
            else if (Dev::PictureModeClickFocusEnabled())
            {
                const ZeusPoint cursor = ZeusCursorPixel();
                s_zeusSuppressNextMouseUp = true;
                Defer([cursor] {
                    s_zeusStatus = Dev::FocusPictureModeAtPixel(cursor.x, cursor.y)
                        ? "Picture Mode: focus set." : "Picture Mode: no surface hit; focus unchanged.";
                });
            }
            else if ((modifiers & SDL_KMOD_SHIFT) != 0 && !s_zeusSelection.empty())
            {
                s_zeusRotateDrag = true;
                s_zeusStatus = "Drag left/right to rotate the selected Zeus object(s).";
            }
            else if (s_zeusClickPlacement)
            {
                s_zeusSuppressNextMouseUp = true;
                Defer([] { SpawnZeusAtClick(); });
            }
            else
            {
                // Absolute SDL event coordinates are not the space the visible
                // in-game cursor lives in; resolve every Zeus position from the
                // engine cursor instead.  See ZeusCursorPixel().
                const ZeusPoint cursor = ZeusCursorPixel();
                SelectZeusAtCursor(cursor.x, cursor.y);
                if (s_zeusSelection.empty())
                {
                    // With click placement disabled, an empty left-drag is a
                    // lasso by default. This keeps ordinary click-selection
                    // and dragging an existing selection to move it intact.
                    s_zeusLassoDrag = true;
                    s_zeusLassoStartX = s_zeusLassoEndX = cursor.x;
                    s_zeusLassoStartY = s_zeusLassoEndY = cursor.y;
                    s_zeusStatus = "Drag to lasso Zeus-spawned objects.";
                }
                else
                    BeginZeusMoveDrag();
            }
        }
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT &&
                 (s_zeusSuppressNextMouseUp || s_zeusRotateDrag || s_zeusMoveDrag || s_zeusLassoDrag))
        {
            s_zeusConsumeMouseEvent = true;
            const ZeusPoint cursor = ZeusCursorPixel();
            if (s_terrainBrushDragging)
            {
                s_terrainBrushDragging = false;
            }
            if (s_zeusLassoDrag)
                SelectZeusInRect(s_zeusLassoStartX, s_zeusLassoStartY, cursor.x, cursor.y);
            else if (s_zeusMoveDrag)
                MoveZeusSelectionAtPixel(cursor.x, cursor.y);
            s_zeusSuppressNextMouseUp = false;
            s_zeusRotateDrag = false;
            s_zeusMoveDrag = false;
            s_zeusLassoDrag = false;
            s_zeusMoveOffsets.clear();
        }
        // Zeus drags must NOT consume mouse motion.  Every Zeus position now
        // comes from the engine cursor, and that cursor is advanced by
        // SDLInput_BufferMouseMotion — which SDLEventWindow::HandleEvents skips
        // for any event WantsMouse() claims.  Consuming motion here therefore
        // freezes the cursor for the whole drag: the lasso stays a zero-area
        // rectangle and selects nothing.  Motion reaching the free-fly camera is
        // harmless, because that camera only looks while the right button is
        // held (SetMouseLookRequiresRightButton).
        else if (event.type == SDL_EVENT_MOUSE_MOTION && s_zeusRotateDrag)
        {
            if (event.motion.xrel != 0.0f)
                RotateZeusSelectionBy(event.motion.xrel * 0.5f);
        }
        // Raise/lower the selection with the wheel.  Page Up / Page Down cannot
        // serve here: they are the alternate bindings for the MoveUp/MoveDown
        // user actions (see InputSubsystem's action table), so they already fly
        // the free-fly camera vertically and a Zeus binding would fight it.
        // The wheel is only claimed while something is selected, leaving it to
        // the game otherwise.
        else if (event.type == SDL_EVENT_MOUSE_WHEEL && Dev::CaveEditor().enabled && event.wheel.y != 0.0f)
        {
            s_zeusConsumeMouseEvent = true;
            const float steps = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            if ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0)
                Dev::CaveEditor().length = std::clamp(Dev::CaveEditor().length * std::pow(1.15f,steps),1.0f,64.0f);
            else Dev::ResizeEditorCave(steps);
        }
        else if (event.type == SDL_EVENT_MOUSE_WHEEL && Dev::TerrainBrush().enabled && event.wheel.y != 0.0f)
        {
            s_zeusConsumeMouseEvent = true;
            const float steps = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            auto& brush = Dev::TerrainBrush();
            brush.radius = TerrainBrushWheelRadius(brush.radius, steps);
        }
        else if (event.type == SDL_EVENT_MOUSE_WHEEL && !s_zeusSelection.empty() && event.wheel.y != 0.0f)
        {
            s_zeusConsumeMouseEvent = true;
            const bool shiftDown = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
            const float step = shiftDown ? 5.0f : 0.5f;
            const float deltaY = event.wheel.y * step;
            Defer([deltaY] { MoveZeusSelectionVertical(deltaY); });
        }
        else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
        {
            const bool ctrlDown = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
            if (event.key.scancode == SDL_SCANCODE_DELETE)
            {
                s_zeusConsumeKeyboardEvent = true;
                Defer([] { DeleteZeusSelection(); });
            }
            else if (ctrlDown && event.key.scancode == SDL_SCANCODE_C)
            {
                s_zeusConsumeKeyboardEvent = true;
                s_zeusConsumeShortcutKeyUp = true;
                s_zeusClipboard = s_zeusSelection;
                s_zeusStatus = "Copied " + std::to_string(s_zeusClipboard.size()) + " Zeus object(s).";
            }
            else if (ctrlDown && event.key.scancode == SDL_SCANCODE_V)
            {
                s_zeusConsumeKeyboardEvent = true;
                s_zeusConsumeShortcutKeyUp = true;
                Defer([] { PasteZeusAtCursor(); });
            }
        }
        else if (event.type == SDL_EVENT_KEY_UP && s_zeusConsumeShortcutKeyUp &&
                 (event.key.scancode == SDL_SCANCODE_C || event.key.scancode == SDL_SCANCODE_V))
        {
            s_zeusConsumeKeyboardEvent = true;
            s_zeusConsumeShortcutKeyUp = false;
        }
    }
    ImGui_ImplSDL3_ProcessEvent(&event);

    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
    {
        // Ctrl+Grave + F5 are dev-only hotkeys (toggle dev panel +
        // role-slot flicker) gated by --dev.
        if (!AppConfig::Instance().DevMode())
            return;
        // Ctrl+` (US) / Ctrl+; (CZ) — toggle the dev panel.  Bound by physical
        // scancode (GRAVE = the key above Tab) so the same key works regardless
        // of keyboard layout.  Ctrl is required so the unmodified key stays
        // available to the game (it's used in radio/chat commands).
        const bool ctrlDown = (event.key.mod & SDL_KMOD_CTRL) != 0;
        if (ctrlDown && (event.key.scancode == SDL_SCANCODE_GRAVE || event.key.scancode == SDL_SCANCODE_SEMICOLON ||
                         event.key.scancode == SDL_SCANCODE_F8))
        {
            ToggleVisible();
            return;
        }
        // ZEUS-THROW: G while the Zeus free-fly camera is active, Ctrl+G at any time (plain G
        // is the gear key when playing). Raw SDL scancode like the other panel shortcuts, so
        // it works whether or not the panel is visible; a focused text field keeps its keys.
        if (event.key.scancode == SDL_SCANCODE_G && s_zeusThrowHotkey && (s_zeusCamera || ctrlDown) &&
            !ImGui::GetIO().WantTextInput)
        {
            ZeusThrow();
            s_zeusConsumeKeyboardEvent = true;
            return;
        }
        const bool shiftDown = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        // Ctrl+Shift+I / Ctrl+Shift+O — interior sky visibility: toggle the EFFECT, and toggle
        // its greyscale reach view. Panel-free like the water debug view below, because the A/B
        // that actually decides whether this feature looks right is "stand in a doorway and
        // flip it", and doing that through a tab is hopeless while the screen is grey.
        if (event.key.scancode == SDL_SCANCODE_I && ctrlDown && shiftDown && GEngine)
        {
            auto is = GEngine->GetInteriorSkySettings();
            is.enabled = !is.enabled;
            GEngine->SetInteriorSkySettings(is);
            LOG_INFO(Core, "Interior sky visibility: {}", is.enabled ? "ON" : "off");
            return;
        }
        if (event.key.scancode == SDL_SCANCODE_O && ctrlDown && shiftDown && GEngine)
        {
            auto is = GEngine->GetInteriorSkySettings();
            is.debug = !is.debug;
            // The reach view is a view OF the effect: with the effect off there is no map and
            // the renderer forces debug back to 0, so the key would silently do nothing. Turn
            // the effect on rather than leave the user pressing a dead key.
            if (is.debug)
                is.enabled = true;
            GEngine->SetInteriorSkySettings(is);
            LOG_INFO(Core, "Interior sky reach view: {}", is.debug ? "ON (greyscale)" : "off (lit scene)");
            return;
        }
        if (event.key.scancode == SDL_SCANCODE_B && ctrlDown && shiftDown && GEngine)
        {
            auto is = GEngine->GetInteriorSkySettings();
            is.baked = !is.baked;
            GEngine->SetInteriorSkySettings(is);
            LOG_INFO(Core, "Interior sky BAKED volumes: {}", is.baked ? "ON" : "off");
            return;
        }
        // Ctrl+Shift+W — cycle the WTR-003 water debug view (works without
        // opening the dev panel).  Wraps 0→1→…→36→0.
        if (event.key.scancode == SDL_SCANCODE_W && ctrlDown && shiftDown && GEngine && GEngine->SupportsWater())
        {
            auto ws = GEngine->GetWaterSettings();
            ws.debugView = (ws.debugView + 1) % kWaterDebugViewCount;
            GEngine->SetWaterSettings(ws);
            LOG_INFO(Core, "Water debug view: [{}] {}", ws.debugView, kWaterDebugViews[ws.debugView]);
            return;
        }
    }
}

void NewFrame()
{
    if (!s_initialized)
        return;
    if (SuspendMultiplayerTools())
    {
        // Keep ImGui's backend lifecycle balanced without drawing any dev UI.
        ImGui::GetIO().MouseDrawCursor = false;
        if (s_backend == RenderBackend::OpenGL3)
            ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        return;
    }
    // LGT-013: the head torch follows the camera every frame, not only while its tab is
    // open. A light that goes out when you close the panel would be a torch you cannot use
    // while looking at anything.
    UpdateHeadlamp();
    Dev::UpdatePictureModeFocus();
    if (s_visible || !s_zeusCamera || !Dev::TerrainBrush().enabled)
        s_terrainBrushDragging = false;
    if (s_terrainBrushDragging && SDL_GetTicks()-s_terrainBrushTickMs >= 250)
    {
        const float seconds = std::min(0.25f,float(SDL_GetTicks()-s_terrainBrushTickMs)*0.001f);
        s_terrainBrushTickMs = SDL_GetTicks();
        const ZeusPoint cursor = ZeusCursorPixel();
        const bool invert = (SDL_GetModState() & SDL_KMOD_LALT) != 0;
        const bool fast = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        Defer([cursor,seconds,invert,fast] {
            s_zeusStatus = Dev::PaintTerrainAtPixel(cursor.x,cursor.y,seconds,invert,fast)
                ? "Terrain brush: applied (mission-local)." : "Terrain brush: no editable land hit.";
        });
    }
    if (!AppConfig::Instance().DevMode() && s_visible)
        SetVisible(false);
    // Toggle the software cursor with panel visibility.  When the panel is
    // shown, the engine's UI cursor renders BEHIND ImGui (we composite ImGui
    // after the game render), so we draw our own cursor as part of ImGui's
    // drawlist to stay on top.  When hidden, fall back to the engine cursor.
    ImGui::GetIO().MouseDrawCursor = s_visible;
    if (s_backend == RenderBackend::OpenGL3)
    {
        ImGui_ImplOpenGL3_NewFrame();
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (!s_visible)
    {
        DrawZeusInteractionOverlay();
        DrawPositionHudOverlay();
        if (s_zeusCursor)
            s_zeusCursor->DrawCursor();
    }
    // POSEIDON_PANEL_SEARCH opens the panel as well as seeding the filter, so a
    // filtered tab can be captured by --auto-screenshot. An auto-capture has no
    // hands: without this the search could only ever be checked by a person.
    if (s_visible || Dev::PanelSearchPreseeded())
        DrawMainWindow();
}

void Render()
{
    if (!s_initialized)
        return;
#if POSEIDON_DIAG
    if (!MultiplayerSession())
        Dev::OpDiag::RenderOverlay(); // DIAG-001: shot paths, labels, watched units (--diag-draw)
#endif
    ImGui::Render();
    if (MultiplayerSession())
    {
        s_pendingActions.clear();
        return;
    }
    if (s_backend == RenderBackend::OpenGL3)
    {
        // Make sure we draw to the default framebuffer in case the engine left
        // an FBO bound — happens with post-FX in GL33.  Other state (blend,
        // scissor, vao, depth) is saved/restored inside RenderDrawData.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    else
    {
        RenderDrawDataEngine(ImGui::GetDrawData());
    }

    // Drain deferred actions queued by UI click handlers.  See the
    // s_pendingActions comment for the why — running cheats here
    // (after ImGui::Render returns) means engine code in the cheat
    // can freely realloc / clean up without trashing ImGui state.
    if (!s_pendingActions.empty())
    {
        auto local = std::move(s_pendingActions);
        s_pendingActions.clear();
        for (auto& fn : local)
            if (!MultiplayerSession())
                fn();
    }
}

bool IsVisible()
{
    return !MultiplayerSession() && s_visible;
}
void SetVisible(bool v)
{
    if (v && (!AppConfig::Instance().DevMode() || MultiplayerSession()))
        v = false;
    s_visible = v;
    ApplyDevPanelMouseState();
}
void ToggleVisible()
{
    SetVisible(!s_visible);
}
void SelectShadowsTab()
{
    s_selectShadowsTab = true;
}
void SelectMemoryTab()
{
    s_selectMemoryTab = true;
}

void RequestDeferredReload(const char* modPath)
{
    if (MultiplayerSession())
        return;
    // Route through the Application's between-frames re-mount request (serviced at the top
    // of AppIdle, before any simulate/draw). Running the reload inside Render()/BackToFront
    // instead — mid-frame, after Simulate — left the rebuilt world's first Simulate touching
    // a torn-down sensor list (null SensorList, SensorList::CheckPos crash).
    Poseidon::GApp->RequestRemountWithMods(modPath);
}

bool WantsKeyboard()
{
    if (MultiplayerSession())
        return false;
    if (!s_initialized || (!s_visible && !s_zeusConsumeKeyboardEvent))
        return false;
    if (s_zeusConsumeKeyboardEvent)
        return true;

    // WantTextInput, not WantCaptureKeyboard. The latter is true merely because a
    // panel window has focus, which meant opening the panel froze the player in
    // place -- you could look at a tower and not walk to it. WantTextInput is true
    // only while a field is actually being typed into, so the model-name box still
    // works and WASD reaches the game the rest of the time.
    const ImGuiIO& io = ImGui::GetIO();
    return io.WantTextInput;
}

bool WantsMouse()
{
    if (MultiplayerSession())
        return false;
    if (!s_initialized)
        return false;
    if (s_zeusConsumeMouseEvent)
        return true;
    if (!s_visible)
        return false;

    // Only while the cursor is actually over a panel window, or a widget is being
    // dragged. Claiming every event whenever the panel was open meant the camera
    // could not be turned at all with it up, so aiming at the thing you were about
    // to spawn required closing the tool first. WantCaptureMouse stays true for
    // the whole of a slider drag, so dragging off a window does not snap the view.
    return ImGui::GetIO().WantCaptureMouse;
}

void AdoptFreeFlyCamera(CameraVehicle* camera)
{
    if (!camera || !MultiplayerSession())
        AdoptFreeFlyCameraImpl(camera);
}

bool SetFreeFlyTestPose(const char* coordinates)
{
    if (MultiplayerSession())
        return false;
    const auto pose = ParseZeusGoto(coordinates);
    if (!coordinates || std::strlen(coordinates) >= sizeof(s_zeusGotoBuf) || pose.count != 5 ||
        !std::isfinite(pose.x) || !std::isfinite(pose.z) || !std::isfinite(pose.altitude) ||
        !std::isfinite(pose.azimuth) || !std::isfinite(pose.elevation) ||
        std::abs(pose.x) > 1000000 || std::abs(pose.z) > 1000000 || std::abs(pose.altitude) > 1000000)
        return false;
    snprintf(s_zeusGotoBuf, sizeof(s_zeusGotoBuf), "%s", coordinates);
    GoToZeusCoordinates();
    return s_zeusCamera != nullptr;
}

} // namespace DebugOverlay
} // namespace Poseidon::Dev
