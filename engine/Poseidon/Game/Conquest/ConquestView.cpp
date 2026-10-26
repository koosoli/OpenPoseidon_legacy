// SPDX-License-Identifier: GPL-3.0-or-later
// The Conquest commander view: see ConquestView.hpp.
//
// This is a player-facing screen, not a dev tool: it keeps its own look (system typefaces,
// dark glass cards, side-coloured accents) instead of the dev panel's default ImGui style.

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/platform.hpp>

// The PCH pulls in Logging.hpp, which #defines DebugLog() as a macro and
// collides with the ImGui::DebugLog() method. Same dance as DebugOverlay.cpp.
#ifdef DebugLog
#undef DebugLog
#endif

#include <imgui.h>
#include <SDL3/SDL.h>

#include <Poseidon/Game/Conquest/ConquestView.hpp>
#include <Poseidon/Game/Conquest/ConquestReferee.hpp>
#include <Poseidon/Dev/NpcChat/NpcChat.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSecrets.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSettings.hpp>

#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Modules/Modules.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Vehicles/Air/Airplane.hpp>
#include <Poseidon/World/Entities/Vehicles/Air/Helicopter.hpp>
#include <Poseidon/World/Entities/Vehicles/Ground/Tank.hpp>
#include <Poseidon/UI/InGame/InGameUI.hpp>
#include <Poseidon/World/Entities/Vehicles/Misc/Ship.hpp>
#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Scene/Camera/CamEffects.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Camera/CameraHold.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>
#include <Evaluator/express.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <array>
#include <string>
#include <vector>

// The cutscene letterbox state, defined at global scope in WorldSetup.cpp (see DebugOverlay.cpp).
extern bool showCinemaBorder;
void ShowCinemaBorder(bool show);

namespace Poseidon::Conquest::View
{
namespace
{
constexpr float Pi = 3.14159265f;
constexpr float PickPixels = 20.0f;
constexpr float MinDistance = 3.0f;       // down among the soldiers
constexpr float MaxDistance = 3500.0f;
constexpr float FollowMinDistance = 2.2f; // a followed soldier: close, never inside him
constexpr float MinPitch = 2.0f;          // degrees: the camera can come down to the ground
constexpr float MaxPitch = 88.0f;

// ---- look ----

constexpr ImU32 ColPanel = IM_COL32(13, 17, 24, 226);
constexpr ImU32 ColEdge = IM_COL32(255, 255, 255, 24);
constexpr ImU32 ColCard = IM_COL32(255, 255, 255, 10);
constexpr ImU32 ColCardHover = IM_COL32(255, 255, 255, 22);
constexpr ImU32 ColText = IM_COL32(236, 240, 246, 255);
constexpr ImU32 ColMuted = IM_COL32(146, 157, 173, 255);
constexpr ImU32 ColGood = IM_COL32(96, 214, 132, 255);
constexpr ImU32 ColWarn = IM_COL32(255, 186, 82, 255);
constexpr ImU32 ColBad = IM_COL32(240, 92, 82, 255);
constexpr ImU32 ColSelect = IM_COL32(110, 226, 255, 255);

struct Fonts
{
    bool tried = false;
    ImFont* body = nullptr;    // Segoe UI
    ImFont* bold = nullptr;    // Segoe UI Semibold
    ImFont* display = nullptr; // Bahnschrift: headings and numbers
};

Fonts& Font()
{
    static Fonts f;
    return f;
}

// System typefaces, read where the system keeps them (nothing is shipped); the default font
// stands in where they are missing.
void LoadFonts()
{
    Fonts& f = Font();
    if (f.tried || !ImGui::GetCurrentContext()) // no overlay (a headless run): nothing to load into
        return;
    f.tried = true;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    const auto load = [&](const char* path, float size) -> ImFont*
    {
        std::error_code ec;
        return std::filesystem::exists(path, ec) ? atlas->AddFontFromFileTTF(path, size) : nullptr;
    };
#ifdef _WIN32
    f.body = load("C:/Windows/Fonts/segoeui.ttf", 16.0f);
    f.bold = load("C:/Windows/Fonts/seguisb.ttf", 16.0f);
    f.display = load("C:/Windows/Fonts/bahnschrift.ttf", 22.0f);
#else
    f.body = load("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16.0f);
    f.bold = load("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 16.0f);
#endif
    if (!f.bold)
        f.bold = f.body;
    if (!f.display)
        f.display = f.bold;
}

ImFont* Pick(ImFont* font)
{
    return font ? font : ImGui::GetFont();
}

ImVec2 Measure(ImFont* font, float size, const char* text)
{
    return Pick(font)->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

void Put(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text, bool shadow = false)
{
    if (shadow)
        draw->AddText(Pick(font), size, ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 170), text);
    draw->AddText(Pick(font), size, at, colour, text);
}

void Label(ImFont* font, float size, ImU32 colour, const char* text)
{
    ImGui::PushFont(Pick(font), size);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

ImU32 WithAlpha(ImU32 colour, float alpha)
{
    const int a = static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    return (colour & 0x00FFFFFFu) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

std::string CallsignFull(int id)
{
    return Callsign(id);
}

// ---- state ----

struct ScreenPoint
{
    float x = 0.0f;
    float y = 0.0f;
};

struct Flash
{
    float x = 0.0f;
    float y = 0.0f;
    Uint64 ticks = 0;
};

struct State
{
    bool active = false;
    OLink<CameraVehicle> camera;
    bool prevCinema = true;
    bool prevGrab = true;
    float focusX = 0.0f;
    float focusY = 0.0f;
    float yaw = 0.0f;    // radians; 0 looks north
    float pitch = 55.0f; // degrees below the horizon
    float distance = 450.0f;
    std::vector<int> selected;
    OLink<Person> unit;      // the one soldier clicked, inside the selected squad
    OLink<EntityAI> follow;  // the camera keeps him (or his vehicle) centred
    bool dragPan = false;    // Ctrl + left drag moves the view
    bool targeting = false;  // the next left click places the mortar barrage
    bool targetAir = false;  // ... or the air strike
    bool optionsOpen = false;
    bool deployOpen = false; // the DEPLOY / SUPPORT panel (B, or the top bar's Deploy)
    bool helpOpen = false;   // the controls row (H)
    bool voiceOpen = false;  // the AI & voice panel (HQ conversation, the LLM, voice in and out)
    std::vector<ImU32> miniColours; // the minimap's ground, MiniCells x MiniCells, north up
    int miniCells = 0;
    bool setupGrab = false;  // the setup screen freed the mouse
    bool endGrab = false;    // the end screen freed the mouse
    bool endClosed = false;  // ... and the player closed it to look on
    std::string setupStart = "soldier";
    bool boxDrag = false;
    float boxX0 = 0.0f;
    float boxY0 = 0.0f;
    bool rotating = false;
    bool consumeMouse = false;
    bool consumeKeyboard = false;
    Uint64 lastTicks = 0;
    std::string status;
    ImU32 statusColour = ColGood;
    Uint64 statusTicks = 0;
    std::vector<Flash> flashes;
    int deployBase = -1;
    bool possessionToggle = true; // the HUD's "Allow taking control of soldiers"
};

State& St()
{
    static State s;
    return s;
}

// One of the player's squads as the view sees it this frame.
struct SquadDraw
{
    LiveGroup group;
    std::vector<ScreenPoint> members;
    std::vector<Person*> soldiers; // members[i] is soldiers[i]
    std::vector<ScreenPoint> vehicles;
    std::vector<const Transport*> vehicleObjects;
    ScreenPoint leader;
    bool leaderVisible = false;
    int men = 0;
    float worldX = 0.0f;
    float worldY = 0.0f;
};

// The Esc menu, a mission dialog or the end screen is over the game. Not GetOptions(): that is the
// whole menu stack (DisplayMain), set during play too. A menu over a single-player game pauses
// it, so the simulation being off is the signal.
bool MenuOpen()
{
    return GWorld && (!GWorld->IsSimulationEnabled() || GWorld->UserDialog());
}

void SetStatus(const std::string& text, ImU32 colour = ColGood)
{
    St().status = text;
    St().statusColour = colour;
    St().statusTicks = SDL_GetTicks();
}

// ---- projection (framebuffer pixels, as Zeus does it) and ImGui coordinates ----

float PixelToUiX()
{
    return GEngine && GEngine->Width() > 0 ? ImGui::GetIO().DisplaySize.x / static_cast<float>(GEngine->Width()) : 1.0f;
}

float PixelToUiY()
{
    return GEngine && GEngine->Height() > 0 ? ImGui::GetIO().DisplaySize.y / static_cast<float>(GEngine->Height())
                                            : 1.0f;
}

bool Project(Vector3Par world, ScreenPoint& out)
{
    const Camera* camera = GScene ? GScene->GetCamera() : nullptr;
    if (!camera)
        return false;
    const Vector3 projected = GScene->ScaledInvTransform() * world;
    if (projected.Z() < camera->Near())
        return false;
    const Matrix4& projection = camera->Projection();
    const float invZ = 1.0f / projected.Z();
    out.x = (projection(0, 2) + projection(0, 0) * projected[0] * invZ) * PixelToUiX();
    out.y = (projection(1, 2) + projection(1, 1) * projected[1] * invZ) * PixelToUiY();
    return true;
}

// The ground (or sea) under an ImGui-space point, by inverting Project.
bool GroundAt(float uiX, float uiY, float& worldX, float& worldY)
{
    CameraVehicle* cam = St().camera;
    const Camera* camera = GScene ? GScene->GetCamera() : nullptr;
    if (!cam || !camera || !GLandscape)
        return false;
    const Matrix4& projection = camera->Projection();
    if (projection(0, 0) == 0.0f || projection(1, 1) == 0.0f)
        return false;
    const float ndcX = (uiX / PixelToUiX() - projection(0, 2)) / projection(0, 0);
    const float ndcY = (uiY / PixelToUiY() - projection(1, 2)) / projection(1, 1);
    const Vector3 direction =
        (cam->Direction() + cam->DirectionAside() * ndcX * camera->Left() + cam->DirectionUp() * ndcY * camera->Top())
            .Normalized();
    Vector3 hit;
    if (GLandscape->IntersectWithGroundOrSea(&hit, cam->Position(), direction, 0.0f, 20000.0f) < 0.0f)
        return false;
    worldX = hit.X();
    worldY = hit.Z();
    return true;
}

float Ground(float x, float y)
{
    return GLandscape ? GLandscape->SurfaceYAboveWater(x, y) : 0.0f;
}

float WorldSize()
{
    return GLandscape ? (GLandscape->GetTerrainRange() - 1) * GLandscape->GetTerrainGrid() : 12800.0f;
}

// ---- the camera ----

// POSEIDON_CONQUEST_FOLLOW_TICKS=1: follow by tick position and let the camera be interpolated, as
// before 2026-10-26 (the A/B for the follow camera's smoothness; see FollowTrace).
bool FollowByTicks()
{
    static const bool on = std::getenv("POSEIDON_CONQUEST_FOLLOW_TICKS") != nullptr;
    return on;
}

// What the camera follows, alive and still worth following.
EntityAI* Followed()
{
    State& s = St();
    EntityAI* target = s.follow;
    if (!target || target->IsDammageDestroyed())
    {
        s.follow = nullptr;
        return nullptr;
    }
    return target;
}

void ApplyCamera()
{
    State& s = St();
    CameraVehicle* cam = s.camera;
    if (!cam || !GLandscape)
        return;
    const float size = WorldSize();
    float focusHeight = 0.0f;
    if (EntityAI* target = Followed())
    {
        // Where it is DRAWN this frame (between simulation ticks), not its tick position: a driving
        // car followed by its tick position steps 60 times a second and judders against the scene.
        const Vector3 p = FollowByTicks() ? target->Position() : target->RenderWorldTransform().Position();
        s.focusX = p.X();
        s.focusY = p.Z();
        // Men are looked at chest-high, vehicles a little above their centre.
        focusHeight = p.Y() + (dyn_cast<Person>(target) ? 1.3f : 1.0f);
        s.distance = std::max(s.distance, std::max(FollowMinDistance, target->GetRadius() * 1.1f));
    }
    s.focusX = std::clamp(s.focusX, 0.0f, size);
    s.focusY = std::clamp(s.focusY, 0.0f, size);
    if (!Followed())
        focusHeight = Ground(s.focusX, s.focusY) + std::min(1.5f, s.distance * 0.1f);
    const float p = s.pitch * Pi / 180.0f;
    const Vector3 forward(std::sin(s.yaw), 0.0f, std::cos(s.yaw));
    const Vector3 focus(s.focusX, focusHeight, s.focusY);
    Vector3 pos = focus - forward * (std::cos(p) * s.distance) + Vector3(0.0f, std::sin(p) * s.distance, 0.0f);
    // Never below the ground: tilted all the way down, the camera stands on it.
    const float floor = Ground(pos.X(), pos.Z()) + 0.6f;
    if (pos.Y() < floor)
        pos[1] = floor;
    cam->SetPosition(pos);
    cam->SetDirectionAndUp((focus - pos).Normalized(), VUp);
    cam->ResetTargets();
    // The camera is placed every rendered frame: drawing must not blend it with where it stood at
    // the last tick (REN-INTERP-001 interpolates every object between ticks).
    if (!FollowByTicks())
        cam->CaptureRenderPrev();
}

// POSEIDON_CONQUEST_FOLLOW_TRACE=1: where the followed unit is drawn on screen, once per frame. A
// smooth follow keeps it still; judder shows as frame-to-frame jumps.
void FollowTrace()
{
    static const bool on = std::getenv("POSEIDON_CONQUEST_FOLLOW_TRACE") != nullptr;
    EntityAI* target = on ? Followed() : nullptr;
    ScreenPoint p;
    if (target && Project(target->RenderWorldTransform().Position(), p))
        LOG_INFO(Mission, "Conquest follow trace: {:.2f} {:.2f} speed {:.1f}", p.x, p.y, target->Speed().Size());
}

void FocusOn(float x, float y)
{
    St().focusX = x;
    St().focusY = y;
    St().follow = nullptr;
}

int HomeBase()
{
    const Snapshot& snap = GetSnapshot();
    const int me = PlayerSide();
    for (int pass = 0; pass < 2; pass++)
        for (size_t o = 0; o < snap.war.defs.size(); o++)
            if (snap.war.states[o].owner == me && (pass == 1 || snap.war.defs[o].kind == ObjectiveKind::HQ))
                return static_cast<int>(o);
    return -1;
}

void InitialFocus()
{
    const Snapshot& snap = GetSnapshot();
    if (const int home = HomeBase(); home >= 0)
        FocusOn(snap.war.defs[home].x, snap.war.defs[home].y);
    else if (GWorld && GWorld->CameraOn())
        FocusOn(GWorld->CameraOn()->Position().X(), GWorld->CameraOn()->Position().Z());
}

// Metres of ground per screen pixel at the focus, for dragging the view.
float MetresPerPixel()
{
    const ImGuiIO& io = ImGui::GetIO();
    return io.DisplaySize.y > 0.0f ? St().distance * 1.25f / io.DisplaySize.y : 1.0f;
}

void Pan(float dt)
{
    State& s = St();
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (!keys)
        return;
    float ahead = 0.0f, aside = 0.0f, turn = 0.0f, tilt = 0.0f;
    if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])
        ahead += 1.0f;
    if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])
        ahead -= 1.0f;
    if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT])
        aside += 1.0f;
    if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])
        aside -= 1.0f;
    if (keys[SDL_SCANCODE_E])
        turn += 1.0f;
    if (keys[SDL_SCANCODE_Q])
        turn -= 1.0f;
    if (keys[SDL_SCANCODE_R])
        tilt += 1.0f;
    if (keys[SDL_SCANCODE_F])
        tilt -= 1.0f;
    // The screen edge pans too, as in any strategy game (not while over a panel).
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const float edge = 6.0f;
    if (!io.WantCaptureMouse && !s.boxDrag && !s.dragPan && mouse.x >= 0.0f && mouse.y >= 0.0f &&
        mouse.x <= io.DisplaySize.x && mouse.y <= io.DisplaySize.y)
    {
        if (mouse.x < edge)
            aside -= 1.0f;
        if (mouse.x > io.DisplaySize.x - edge)
            aside += 1.0f;
        if (mouse.y < edge)
            ahead += 1.0f;
        if (mouse.y > io.DisplaySize.y - edge)
            ahead -= 1.0f;
    }
    if (ahead != 0.0f || aside != 0.0f)
        s.follow = nullptr; // moving the view lets go of the soldier it followed
    const float speed = std::max(s.distance, 20.0f) * 1.1f *
                        ((keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) ? 2.5f : 1.0f);
    s.focusX += (std::sin(s.yaw) * ahead + std::cos(s.yaw) * aside) * speed * dt;
    s.focusY += (std::cos(s.yaw) * ahead - std::sin(s.yaw) * aside) * speed * dt;
    s.yaw += turn * 1.6f * dt;
    s.pitch = std::clamp(s.pitch + tilt * 45.0f * dt, MinPitch, MaxPitch);
}

// ---- selection and orders ----

std::vector<SquadDraw> CollectSquads()
{
    std::vector<SquadDraw> out;
    const int me = PlayerSide();
    ForEachGroup(
        [&](const LiveGroup& g)
        {
            if (g.side != me)
                return;
            SquadDraw d;
            d.group = g;
            for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
            {
                AIUnit* unit = g.group->UnitWithID(id);
                if (!unit || unit->GetLifeState() != AIUnit::LSAlive)
                    continue;
                d.men++;
                if (!unit->IsFreeSoldier())
                    continue; // inside a vehicle: the vehicle's symbol stands for him
                ScreenPoint p;
                if (Project(unit->Position() + Vector3(0.0f, 2.1f, 0.0f), p))
                {
                    d.members.push_back(p);
                    d.soldiers.push_back(unit->GetPerson());
                }
            }
            for (int v = 0; v < g.group->NVehicles(); v++)
            {
                const Transport* veh = g.group->GetVehicle(v);
                ScreenPoint p;
                if (veh && !veh->IsDammageDestroyed() && Project(veh->Position() + Vector3(0.0f, 3.2f, 0.0f), p))
                {
                    d.vehicles.push_back(p);
                    d.vehicleObjects.push_back(veh);
                }
            }
            const Vector3 lp = g.group->Leader()->Position();
            d.worldX = lp.X();
            d.worldY = lp.Z();
            d.leaderVisible = Project(lp + Vector3(0.0f, 3.0f, 0.0f), d.leader);
            out.push_back(d);
        });
    return out;
}

bool IsSelected(int id)
{
    const auto& sel = St().selected;
    return std::find(sel.begin(), sel.end(), id) != sel.end();
}

void ToggleSelected(int id)
{
    auto& sel = St().selected;
    const auto it = std::find(sel.begin(), sel.end(), id);
    if (it == sel.end())
        sel.push_back(id);
    else
        sel.erase(it);
}

// A click selects the squad of the soldier (or vehicle) under the pointer, and that soldier: the
// camera follows him and "Take control" becomes him.
void ClickSelect(float x, float y, bool add)
{
    State& s = St();
    int best = 0;
    Person* soldier = nullptr;
    const Transport* vehicle = nullptr;
    float bestD2 = PickPixels * PickPixels;
    for (const SquadDraw& d : CollectSquads())
    {
        for (size_t i = 0; i < d.members.size(); i++)
        {
            const float d2 = (d.members[i].x - x) * (d.members[i].x - x) + (d.members[i].y - y) * (d.members[i].y - y);
            if (d2 < bestD2)
            {
                bestD2 = d2;
                best = d.group.id;
                soldier = d.soldiers[i];
                vehicle = nullptr;
            }
        }
        for (size_t i = 0; i < d.vehicles.size(); i++)
        {
            const float d2 = (d.vehicles[i].x - x) * (d.vehicles[i].x - x) + (d.vehicles[i].y - y) * (d.vehicles[i].y - y);
            if (d2 < bestD2)
            {
                bestD2 = d2;
                best = d.group.id;
                soldier = nullptr;
                vehicle = d.vehicleObjects[i];
            }
        }
    }
    if (!add)
        s.selected.clear();
    if (!best)
    {
        if (!add)
        {
            s.unit = nullptr;
            s.follow = nullptr;
        }
        return;
    }
    if (add)
        ToggleSelected(best);
    else
        s.selected.push_back(best);
    s.unit = soldier;
    if (soldier)
        s.follow = soldier;
    else if (vehicle)
        s.follow = const_cast<Transport*>(vehicle);
    if (s.follow && s.distance > 120.0f)
        s.distance = 120.0f; // come down to the soldier picked
}

void BoxSelect(float x0, float y0, float x1, float y1, bool add)
{
    const float left = std::min(x0, x1), right = std::max(x0, x1);
    const float top = std::min(y0, y1), bottom = std::max(y0, y1);
    if (!add)
        St().selected.clear();
    St().unit = nullptr;
    for (const SquadDraw& d : CollectSquads())
    {
        bool inside = false;
        for (const ScreenPoint& p : d.members)
            inside = inside || (p.x >= left && p.x <= right && p.y >= top && p.y <= bottom);
        for (const ScreenPoint& p : d.vehicles)
            inside = inside || (p.x >= left && p.x <= right && p.y >= top && p.y <= bottom);
        if (inside && !IsSelected(d.group.id))
            St().selected.push_back(d.group.id);
    }
}

void OrderAt(float uiX, float uiY)
{
    State& s = St();
    if (s.selected.empty())
    {
        SetStatus("Select a squad first: click it, or drag a box around several.", ColWarn);
        return;
    }
    float x = 0.0f, y = 0.0f;
    // A known enemy under the cursor is the target; otherwise the ground there.
    bool onEnemy = false;
    float bestD2 = PickPixels * PickPixels;
    for (const Contact& c : KnownEnemies(PlayerSide()))
    {
        ScreenPoint p;
        if (!Project(Vector3(c.x, c.height + 1.0f, c.y), p))
            continue;
        const float d2 = (p.x - uiX) * (p.x - uiX) + (p.y - uiY) * (p.y - uiY);
        if (d2 < bestD2)
        {
            bestD2 = d2;
            x = c.x;
            y = c.y;
            onEnemy = true;
        }
    }
    if (!onEnemy && !GroundAt(uiX, uiY, x, y))
        return;
    const int n = OrderGroups(s.selected, x, y);
    if (n == 0)
        return;
    s.flashes.push_back({x, y, SDL_GetTicks()});
    const Snapshot& snap = GetSnapshot();
    std::string what = onEnemy ? "Engaging the enemy contact" : "Moving to the position";
    for (size_t o = 0; o < snap.war.defs.size(); o++)
        if (std::hypot(snap.war.defs[o].x - x, snap.war.defs[o].y - y) <= snap.war.defs[o].radius * 1.2f)
            what = (snap.war.states[o].owner == PlayerSide() ? "Holding " : "Attacking ") + snap.war.defs[o].name;
    SetStatus(what + (n == 1 ? "  -  1 squad" : "  -  " + std::to_string(n) + " squads"));
}

void FireBarrageAt(float uiX, float uiY)
{
    State& s = St();
    s.targeting = false;
    float x = 0.0f, y = 0.0f;
    if (!GroundAt(uiX, uiY, x, y))
        return;
    std::string why;
    if (s.targetAir)
    {
        s.targetAir = false;
        if (!OrderAirStrike(x, y, &why))
        {
            SetStatus("No air strike: " + why, ColWarn);
            return;
        }
        s.flashes.push_back({x, y, SDL_GetTicks()});
        SetStatus("Air support inbound. Bombs on target in about forty seconds.", ColWarn);
        return;
    }
    if (!OrderBarrage(x, y, &why))
    {
        SetStatus("No barrage: " + why, ColWarn);
        return;
    }
    s.flashes.push_back({x, y, SDL_GetTicks()});
    SetStatus("Mortars firing. Rounds land in about seven seconds.", ColWarn);
}

void TakeControl()
{
    State& s = St();
    std::string why;
    Person* soldier = s.unit;
    const bool ok = soldier ? PossessUnit(soldier, &why) : !s.selected.empty() && Possess(s.selected.front(), &why);
    if (!ok)
    {
        if (!why.empty())
            SetStatus("Cannot take control: " + why, ColWarn);
        return;
    }
    SetActive(false);
}

// ---- the world overlay ----

const char* Task(const LiveGroup& g, const Snapshot& snap, std::string& buffer)
{
    if (g.playerLed)
        return "You are leading this squad";
    if (g.stalled)
        return "Held up, finding another way";
    const bool onObjective = g.objective >= 0 && g.objective < static_cast<int>(snap.war.defs.size());
    switch (g.task)
    {
        case TaskKind::Attack:
            buffer = "Attacking " + (onObjective ? snap.war.defs[g.objective].name : std::string("the enemy"));
            return buffer.c_str();
        case TaskKind::Defend:
            buffer = "Holding " + (onObjective ? snap.war.defs[g.objective].name : std::string("position"));
            return buffer.c_str();
        case TaskKind::Move:
            return "Moving to position";
        case TaskKind::None:
            break;
    }
    return "Awaiting orders";
}

// 0 blue (WEST), 1 red (EAST), 2 green (resistance), -1 neutral grey.
int Family(int side)
{
    const Snapshot& snap = GetSnapshot();
    if (side < 0 || side >= static_cast<int>(snap.sideNames.size()))
        return -1;
    const std::string& n = snap.sideNames[side];
    return n == "WEST" ? 0 : n == "EAST" ? 1 : 2;
}

ImU32 SideColour(int side, float alpha = 1.0f)
{
    static const ImU32 base[] = {IM_COL32(74, 152, 255, 255), IM_COL32(236, 76, 64, 255), IM_COL32(86, 196, 98, 255)};
    const int f = Family(side);
    return WithAlpha(f < 0 ? IM_COL32(190, 196, 205, 255) : base[f], alpha);
}

// Every squad its own shade of its side's colour, so neighbouring squads read apart on the ground.
ImU32 SquadShade(int side, int id, float alpha = 1.0f)
{
    static const ImU32 shades[3][8] = {
        {IM_COL32(74, 152, 255, 255), IM_COL32(40, 104, 228, 255), IM_COL32(128, 186, 255, 255), IM_COL32(28, 74, 186, 255),
         IM_COL32(96, 214, 255, 255), IM_COL32(104, 128, 236, 255), IM_COL32(170, 208, 255, 255), IM_COL32(52, 140, 200, 255)},
        {IM_COL32(236, 76, 64, 255), IM_COL32(196, 34, 40, 255), IM_COL32(255, 126, 102, 255), IM_COL32(160, 26, 52, 255),
         IM_COL32(255, 96, 140, 255), IM_COL32(222, 108, 60, 255), IM_COL32(255, 164, 150, 255), IM_COL32(180, 60, 90, 255)},
        {IM_COL32(86, 196, 98, 255), IM_COL32(46, 148, 70, 255), IM_COL32(144, 222, 120, 255), IM_COL32(36, 118, 58, 255),
         IM_COL32(120, 210, 170, 255), IM_COL32(170, 200, 80, 255), IM_COL32(180, 236, 160, 255), IM_COL32(70, 160, 120, 255)}};
    const int f = Family(side);
    if (f < 0)
        return SideColour(side, alpha);
    return WithAlpha(shades[f][static_cast<size_t>(std::max(id, 1) - 1) % 8], alpha);
}

std::string SideTitle(int side)
{
    const Snapshot& snap = GetSnapshot();
    if (side < 0 || side >= static_cast<int>(snap.sideNames.size()))
        return "Neutral";
    const std::string& n = snap.sideNames[side];
    return n == "WEST" ? "BLUFOR" : n == "EAST" ? "OPFOR" : "INDEP";
}

void Pill(ImDrawList* draw, ImVec2 centre, ImFont* font, float size, const char* text, ImU32 fill, ImU32 colour)
{
    const ImVec2 t = Measure(font, size, text);
    const ImVec2 a(centre.x - t.x * 0.5f - 9.0f, centre.y - t.y * 0.5f - 3.0f);
    const ImVec2 b(centre.x + t.x * 0.5f + 9.0f, centre.y + t.y * 0.5f + 3.0f);
    draw->AddRectFilled(a, b, fill, (b.y - a.y) * 0.5f);
    Put(draw, font, size, ImVec2(centre.x - t.x * 0.5f, centre.y - t.y * 0.5f), colour, text);
}

// ---- military symbols (after APP-6 / MIL-STD-2525: a frame for the affiliation, a glyph for the arm) ----

enum class Arm
{
    Infantry,
    Motorised, // wheeled: cars, trucks
    Armour,    // tracked, no troops: tanks
    Mechanised, // tracked with troops: APCs and IFVs
    Rotary,    // helicopters
    FixedWing,
    Naval,
};

Arm ArmOf(const EntityAI* e)
{
    if (!e || dynamic_cast<const Person*>(e))
        return Arm::Infantry;
    if (dynamic_cast<const Helicopter*>(e))
        return Arm::Rotary;
    if (dynamic_cast<const Airplane*>(e))
        return Arm::FixedWing;
    if (dynamic_cast<const Ship*>(e))
        return Arm::Naval;
    if (const Tank* tank = dynamic_cast<const Tank*>(e))
        return tank->Type()->GetMaxManCargo() >= 4 ? Arm::Mechanised : Arm::Armour;
    return Arm::Motorised;
}

// A small symbol whose bottom edge sits at `foot`, so the soldier or vehicle under it stays in view.
// Friendly forces in a rectangle, hostile ones in a diamond.
void DrawSymbol(ImDrawList* draw, ImVec2 foot, float h, Arm arm, bool hostile, ImU32 colour, float alpha, bool selected)
{
    const float w = hostile ? h * 1.25f : h * 1.5f;
    const ImVec2 c(foot.x, foot.y - h * 0.5f - (hostile ? h * 0.15f : 0.0f));
    const ImU32 fill = WithAlpha(colour, 0.5f * alpha);
    const ImU32 edge = WithAlpha(colour, alpha);
    const ImU32 line = IM_COL32(255, 255, 255, static_cast<int>(230 * alpha));
    const float thick = h >= 9.0f ? 1.4f : 1.1f;
    float l, r, t, b; // where the glyph goes
    if (hostile)
    {
        const float hw = w * 0.5f, hh = h * 0.65f;
        draw->AddQuadFilled(ImVec2(c.x, c.y - hh), ImVec2(c.x + hw, c.y), ImVec2(c.x, c.y + hh), ImVec2(c.x - hw, c.y), fill);
        draw->AddQuad(ImVec2(c.x, c.y - hh), ImVec2(c.x + hw, c.y), ImVec2(c.x, c.y + hh), ImVec2(c.x - hw, c.y), edge, thick);
        l = c.x - hw * 0.5f, r = c.x + hw * 0.5f, t = c.y - hh * 0.5f, b = c.y + hh * 0.5f;
    }
    else
    {
        l = c.x - w * 0.5f, r = c.x + w * 0.5f, t = c.y - h * 0.5f, b = c.y + h * 0.5f;
        draw->AddRectFilled(ImVec2(l, t), ImVec2(r, b), fill, 1.0f);
        draw->AddRect(ImVec2(l, t), ImVec2(r, b), edge, 1.0f, 0, thick);
    }
    if (selected)
    {
        const float m = 3.0f;
        draw->AddRect(ImVec2(l - m - (hostile ? w * 0.25f : 0.0f), t - m - (hostile ? h * 0.15f : 0.0f)),
                      ImVec2(r + m + (hostile ? w * 0.25f : 0.0f), b + m + (hostile ? h * 0.15f : 0.0f)), ColSelect, 2.0f, 0, 1.5f);
    }
    const ImVec2 mid((l + r) * 0.5f, (t + b) * 0.5f);
    const auto cross = [&]()
    {
        draw->AddLine(ImVec2(l, t), ImVec2(r, b), line, 1.0f);
        draw->AddLine(ImVec2(l, b), ImVec2(r, t), line, 1.0f);
    };
    const auto track = [&]() { draw->AddEllipse(mid, ImVec2((r - l) * 0.34f, (b - t) * 0.3f), line, 0.0f, 16, 1.0f); };
    switch (arm)
    {
        case Arm::Infantry:
            cross();
            break;
        case Arm::Motorised:
            cross();
            draw->AddLine(ImVec2(mid.x, t), ImVec2(mid.x, b), line, 1.0f);
            break;
        case Arm::Armour:
            track();
            break;
        case Arm::Mechanised:
            cross();
            track();
            break;
        case Arm::Rotary:
            draw->AddTriangleFilled(ImVec2(l + 1.0f, t + 1.0f), ImVec2(l + 1.0f, b - 1.0f), mid, line);
            draw->AddTriangleFilled(ImVec2(r - 1.0f, t + 1.0f), ImVec2(r - 1.0f, b - 1.0f), mid, line);
            break;
        case Arm::FixedWing:
            draw->AddLine(ImVec2(l + 1.0f, mid.y), ImVec2(r - 1.0f, mid.y), line, 1.0f);
            draw->AddLine(ImVec2(mid.x, t + 1.0f), ImVec2(mid.x, b - 1.0f), line, 1.0f);
            break;
        case Arm::Naval:
            draw->AddCircle(mid, (b - t) * 0.3f, line, 10, 1.0f);
            break;
    }
}

// Symbols fade as the camera comes down among the men: up close the 3D soldier is the information.
float SymbolAlpha()
{
    const float d = St().distance;
    return d < 60.0f ? 0.35f + 0.65f * std::clamp((d - 8.0f) / 52.0f, 0.0f, 1.0f) : 1.0f;
}

// The shroud (Fog of war: Shroud): ground no soldier of the player's side has been near is black,
// with soft edges where explored and unexplored cells meet.
void DrawShroud(ImDrawList* draw)
{
    if (GetOptions().fog != FogMode::Shroud || !GLandscape)
        return;
    const State& s = St();
    // A map overlay, for the view from above: as the camera comes down towards the ground (and
    // first person) it fades out, and close to the ground it is not drawn at all (the minimap keeps it).
    const float fade = std::clamp((s.distance - 250.0f) / 300.0f, 0.0f, 1.0f) * std::clamp((s.pitch - 25.0f) / 20.0f, 0.0f, 1.0f);
    if (fade <= 0.0f)
        return;
    constexpr float cell = 100.0f;
    const float range = std::clamp(s.distance * 4.0f, 900.0f, 7000.0f);
    const int x0 = static_cast<int>(std::floor((s.focusX - range) / cell));
    const int y0 = static_cast<int>(std::floor((s.focusY - range) / cell));
    const int n = static_cast<int>(std::ceil(2.0f * range / cell)) + 1;
    const float size = WorldSize();
    // Corner values: how much of the four cells round a corner is unexplored (0..1), and where it projects.
    std::vector<float> dark(static_cast<size_t>(n + 1) * (n + 1));
    std::vector<ScreenPoint> at(dark.size());
    std::vector<uint8_t> ok(dark.size());
    const auto unexplored = [&](int cx, int cy)
    {
        const float x = (static_cast<float>(cx) + 0.5f) * cell, y = (static_cast<float>(cy) + 0.5f) * cell;
        if (x < 0.0f || y < 0.0f || x > size || y > size)
            return 1.0f;
        return Explored(x, y) ? 0.0f : 1.0f;
    };
    for (int j = 0; j <= n; j++)
        for (int i = 0; i <= n; i++)
        {
            const int cx = x0 + i, cy = y0 + j;
            const size_t k = static_cast<size_t>(j) * (n + 1) + i;
            dark[k] = 0.25f * (unexplored(cx - 1, cy - 1) + unexplored(cx, cy - 1) + unexplored(cx - 1, cy) + unexplored(cx, cy));
            if (dark[k] <= 0.0f)
                continue;
            const float x = static_cast<float>(cx) * cell, y = static_cast<float>(cy) * cell;
            ok[k] = Project(Vector3(x, Ground(x, y) + 0.3f, y), at[k]) ? 1 : 0;
        }
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++)
        {
            const size_t k[4] = {static_cast<size_t>(j) * (n + 1) + i, static_cast<size_t>(j) * (n + 1) + i + 1,
                                 static_cast<size_t>(j + 1) * (n + 1) + i + 1, static_cast<size_t>(j + 1) * (n + 1) + i};
            if (dark[k[0]] + dark[k[1]] + dark[k[2]] + dark[k[3]] <= 0.0f)
                continue;
            if (!ok[k[0]] || !ok[k[1]] || !ok[k[2]] || !ok[k[3]])
                continue;
            draw->PrimReserve(6, 4);
            const ImDrawIdx base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
            draw->PrimWriteIdx(base);
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
            draw->PrimWriteIdx(base);
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
            for (size_t q : k)
                draw->PrimWriteVtx(ImVec2(at[q].x, at[q].y), uv, IM_COL32(4, 6, 9, static_cast<int>(245 * dark[q] * fade)));
        }
}

void DrawObjectives(ImDrawList* draw)
{
    const Snapshot& snap = GetSnapshot();
    const Fonts& f = Font();
    const auto ring = [&](const ObjectiveDef& d, float radius, ImU32 colour, float thickness, bool dashed)
    {
        ScreenPoint prev;
        bool prevOk = false;
        const int steps = radius > 60.0f ? 72 : 40;
        for (int i = 0; i <= steps; i++)
        {
            const float a = static_cast<float>(i) * (2.0f * Pi / static_cast<float>(steps));
            const float x = d.x + std::cos(a) * radius, y = d.y + std::sin(a) * radius;
            ScreenPoint p;
            const bool ok = Project(Vector3(x, Ground(x, y) + 0.4f, y), p);
            if (ok && prevOk && (!dashed || i % 2 == 0))
                draw->AddLine(ImVec2(prev.x, prev.y), ImVec2(p.x, p.y), colour, thickness);
            prev = p;
            prevOk = ok;
        }
    };
    for (size_t o = 0; o < snap.war.defs.size(); o++)
    {
        const ObjectiveDef& d = snap.war.defs[o];
        const ObjectiveState& st = snap.war.states[o];
        // The command post's small capture circle: only soldiers inside it count.
        ring(d, d.captureRadius, SideColour(st.owner, st.contested ? 0.6f : 0.95f), 3.0f, false);
        ScreenPoint c;
        if (!Project(Vector3(d.x, Ground(d.x, d.y) + 14.0f, d.y), c))
            continue;
        std::string name = d.name;
        if (d.kind == ObjectiveKind::HQ)
            name += "  HQ";
        Pill(draw, ImVec2(c.x, c.y), f.bold, 15.0f, name.c_str(), SideColour(st.owner, 0.88f), IM_COL32(255, 255, 255, 255));
        const bool moving = st.owner == NoSide ? st.level > 0.0f : (st.holder != st.owner || st.level < 1.0f);
        if (moving || st.contested)
        {
            const float w = 96.0f, h = 6.0f;
            const ImVec2 a(c.x - w * 0.5f, c.y + 15.0f), b(c.x + w * 0.5f, c.y + 15.0f + h);
            draw->AddRectFilled(a, b, IM_COL32(0, 0, 0, 160), 3.0f);
            draw->AddRectFilled(a, ImVec2(a.x + w * st.level, b.y), SideColour(st.holder), 3.0f);
            if (st.contested)
                Pill(draw, ImVec2(c.x, c.y + 34.0f), f.bold, 13.0f, "CONTESTED", IM_COL32(40, 30, 10, 220), ColWarn);
        }
    }
}

// A human player's mark over his soldier symbol: a white ring round a head, so the commander knows
// that squad answers to a person (orders to it are requests) and that soldier cannot be taken over.
void DrawPlayerBadge(ImDrawList* draw, ImVec2 c, float alpha)
{
    const ImU32 ring = IM_COL32(255, 255, 255, static_cast<int>(240 * alpha));
    draw->AddCircleFilled(c, 5.5f, IM_COL32(20, 24, 30, static_cast<int>(210 * alpha)), 14);
    draw->AddCircle(c, 5.5f, ring, 14, 1.4f);
    draw->AddCircleFilled(ImVec2(c.x, c.y - 1.2f), 1.7f, ring, 8);
    draw->AddLine(ImVec2(c.x - 2.6f, c.y + 3.0f), ImVec2(c.x + 2.6f, c.y + 3.0f), ring, 1.4f);
}

// The commander's mark: a small gold star, discreet, the "commander's uniform" his side can see.
void DrawCommanderMark(ImDrawList* draw, ImVec2 c, float r, float alpha)
{
    ImVec2 pts[10];
    for (int i = 0; i < 10; i++)
    {
        const float a = -1.5707963f + static_cast<float>(i) * 0.6283185f;
        const float rr = i % 2 == 0 ? r : r * 0.45f;
        pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
    }
    draw->AddConcavePolyFilled(pts, 10, IM_COL32(236, 196, 92, static_cast<int>(235 * alpha)));
    draw->AddPolyline(pts, 10, IM_COL32(40, 30, 10, static_cast<int>(200 * alpha)), ImDrawFlags_Closed, 1.0f);
}

// On the ground: his teammates see the star over their commander's soldier, close by only (300 m)
// and fading with distance, so it marks him without becoming a beacon.
void DrawCommanderOnGround()
{
    const int side = PlayerSide();
    const Person* commander = CommanderSoldier(side);
    Person* me = GWorld ? GWorld->GetRealPlayer() : nullptr;
    if (!commander || !me || commander == me || commander->IsDammageDestroyed())
        return;
    const float distance = commander->Position().Distance(me->Position());
    if (distance > 300.0f)
        return;
    ScreenPoint p;
    if (!Project(commander->RenderWorldTransform().Position() + Vector3(0.0f, 2.25f, 0.0f), p))
        return;
    const float alpha = std::clamp(1.0f - distance / 300.0f, 0.25f, 0.85f);
    DrawCommanderMark(ImGui::GetBackgroundDrawList(), ImVec2(p.x, p.y), distance < 60.0f ? 5.0f : 4.0f, alpha);
}

void DrawSquads(ImDrawList* draw, const std::vector<SquadDraw>& squads)
{
    const Snapshot& snap = GetSnapshot();
    const Fonts& f = Font();
    const State& s = St();
    const float alpha = SymbolAlpha();
    const Person* picked = s.unit;
    for (const SquadDraw& d : squads)
    {
        const bool sel = IsSelected(d.group.id);
        const ImU32 colour = SquadShade(d.group.side, d.group.id);
        if (sel && d.group.task != TaskKind::None && d.leaderVisible)
        {
            // Where it was sent: a dashed line from the squad to its destination.
            ScreenPoint dest;
            if (Project(Vector3(d.group.destX, Ground(d.group.destX, d.group.destY) + 1.0f, d.group.destY), dest))
            {
                const float dx = dest.x - d.leader.x, dy = dest.y - d.leader.y;
                const float len = std::sqrt(dx * dx + dy * dy);
                for (float t = 0.0f; t < len; t += 14.0f)
                {
                    const float t1 = std::min(t + 8.0f, len);
                    draw->AddLine(ImVec2(d.leader.x + dx * t / len, d.leader.y + dy * t / len),
                                  ImVec2(d.leader.x + dx * t1 / len, d.leader.y + dy * t1 / len), WithAlpha(colour, 0.8f),
                                  2.0f);
                }
                draw->AddCircle(ImVec2(dest.x, dest.y), 7.0f, colour, 16, 2.0f);
            }
        }
        for (size_t i = 0; i < d.vehicles.size(); i++)
            DrawSymbol(draw, ImVec2(d.vehicles[i].x, d.vehicles[i].y), 10.0f, ArmOf(d.vehicleObjects[i]), false, colour,
                       alpha, sel && !picked);
        bool humans = false;
        for (size_t i = 0; i < d.members.size(); i++)
        {
            DrawSymbol(draw, ImVec2(d.members[i].x, d.members[i].y), 7.0f, Arm::Infantry, false, colour, alpha,
                       sel && (!picked || d.soldiers[i] == picked));
            if (d.soldiers[i] && d.soldiers[i] == CommanderSoldier(d.group.side))
            {
                humans = true;
                DrawCommanderMark(draw, ImVec2(d.members[i].x, d.members[i].y - 14.0f), 6.0f, alpha);
            }
            else if (IsHumanPlayer(d.soldiers[i]))
            {
                humans = true;
                DrawPlayerBadge(draw, ImVec2(d.members[i].x, d.members[i].y - 14.0f), alpha);
            }
        }
        if (!d.leaderVisible)
            continue;
        // A callsign only where it helps: the selected squads, and the one under the pointer.
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool hovered = std::hypot(mouse.x - d.leader.x, mouse.y - d.leader.y) < 36.0f;
        if (!sel && !hovered)
            continue;
        const std::string name = CallsignFull(d.group.id) + "  " + std::to_string(d.men) + (humans ? "  PLAYER" : "");
        const ImVec2 at(d.leader.x, d.leader.y - 24.0f);
        Pill(draw, at, f.bold, 13.0f, name.c_str(), sel ? IM_COL32(18, 60, 72, 235) : IM_COL32(12, 16, 22, 200),
             sel ? ColSelect : colour);
        if (sel)
        {
            std::string buffer;
            const char* task = Task(d.group, snap, buffer);
            const ImVec2 t = Measure(f.body, 13.0f, task);
            Put(draw, f.body, 13.0f, ImVec2(at.x - t.x * 0.5f, at.y - 26.0f), ColText, task, true);
        }
    }
}

// The enemy as the player's side knows it (both fog-of-war modes): fading with the age of the report.
void DrawContacts(ImDrawList* draw)
{
    const int me = PlayerSide();
    int enemy = NoSide;
    const Snapshot& snap = GetSnapshot();
    for (int s = 0; s < snap.war.sideCount; s++)
        if (s != me)
            enemy = s;
    const float alpha = SymbolAlpha();
    for (const Contact& c : KnownEnemies(me))
    {
        ScreenPoint p;
        if (!Project(Vector3(c.x, c.height + (c.vehicle ? 3.2f : 2.1f), c.y), p))
            continue;
        const float fresh = 1.0f - std::clamp(c.age / 120.0f, 0.0f, 0.75f);
        DrawSymbol(draw, ImVec2(p.x, p.y), c.vehicle ? 10.0f : 7.0f, c.vehicle ? Arm::Armour : Arm::Infantry, true,
                   SideColour(enemy), alpha * fresh, false);
    }
}

void DrawFlashes(ImDrawList* draw)
{
    auto& flashes = St().flashes;
    const Uint64 now = SDL_GetTicks();
    flashes.erase(std::remove_if(flashes.begin(), flashes.end(), [&](const Flash& f) { return now - f.ticks > 1200; }),
                  flashes.end());
    for (const Flash& f : flashes)
    {
        ScreenPoint p;
        if (!Project(Vector3(f.x, Ground(f.x, f.y) + 0.5f, f.y), p))
            continue;
        const float t = static_cast<float>(now - f.ticks) / 1200.0f;
        draw->AddCircle(ImVec2(p.x, p.y), 8.0f + 30.0f * t, WithAlpha(ColSelect, 1.0f - t), 32, 3.0f);
        draw->AddCircleFilled(ImVec2(p.x, p.y), 4.0f, WithAlpha(ColSelect, 1.0f - t));
    }
}

void DrawBox(ImDrawList* draw)
{
    const State& s = St();
    if (s.targeting)
    {
        // The barrage's reach under the pointer.
        const ImVec2 m = ImGui::GetIO().MousePos;
        float x = 0.0f, y = 0.0f;
        if (GroundAt(m.x, m.y, x, y))
        {
            const float spread = s.targetAir ? std::max(15.0f, AirStrike().spread * AirStrike().bombs * 0.5f) : 35.0f;
            ScreenPoint prev;
            bool prevOk = false;
            for (int i = 0; i <= 40; i++)
            {
                const float a = static_cast<float>(i) * (2.0f * Pi / 40.0f);
                const float px = x + std::cos(a) * spread, py = y + std::sin(a) * spread;
                ScreenPoint p;
                const bool ok = Project(Vector3(px, Ground(px, py) + 0.4f, py), p);
                if (ok && prevOk)
                    draw->AddLine(ImVec2(prev.x, prev.y), ImVec2(p.x, p.y), ColWarn, 2.0f);
                prev = p;
                prevOk = ok;
            }
        }
        draw->AddLine(ImVec2(m.x - 10.0f, m.y), ImVec2(m.x + 10.0f, m.y), ColWarn, 1.5f);
        draw->AddLine(ImVec2(m.x, m.y - 10.0f), ImVec2(m.x, m.y + 10.0f), ColWarn, 1.5f);
        return;
    }
    if (!s.boxDrag)
        return;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const ImVec2 a(std::min(s.boxX0, m.x), std::min(s.boxY0, m.y)), b(std::max(s.boxX0, m.x), std::max(s.boxY0, m.y));
    draw->AddRectFilled(a, b, WithAlpha(ColSelect, 0.12f), 4.0f);
    draw->AddRect(a, b, WithAlpha(ColSelect, 0.9f), 4.0f, 0, 1.5f);
}

// ---- the HUD ----

void PushTheme()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ColPanel);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ColPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, ColEdge);
    ImGui::PushStyleColor(ImGuiCol_Text, ColText);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 50, 66, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(56, 72, 94, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(72, 94, 122, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(32, 40, 52, 255));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ColSelect);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Separator, ColEdge);
    ImGui::PushFont(Pick(Font().body), 16.0f);
}

void PopTheme()
{
    ImGui::PopFont();
    ImGui::PopStyleColor(11);
    ImGui::PopStyleVar(7);
}

bool BeginPanel(const char* name, ImVec2 pos, ImVec2 pivot, ImVec2 maxSize = ImVec2(0.0f, 0.0f))
{
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    if (maxSize.x > 0.0f)
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), maxSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
    return ImGui::Begin(name, nullptr, flags);
}

void Heading(const char* text, const char* right = nullptr)
{
    Label(Font().display, 15.0f, ColMuted, text);
    if (right)
    {
        ImGui::SameLine();
        const float w = Measure(Font().bold, 14.0f, right).x;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - w);
        Label(Font().bold, 14.0f, ColMuted, right);
    }
}

void Stat(const char* caption, const char* value, ImU32 colour = ColText)
{
    ImGui::BeginGroup();
    Label(Font().body, 12.0f, ColMuted, caption);
    Label(Font().display, 22.0f, colour, value);
    ImGui::EndGroup();
}

void DrawTopBar(const Snapshot& snap)
{
    const ImGuiIO& io = ImGui::GetIO();
    const int me = PlayerSide();
    if (BeginPanel("##conquest_top", ImVec2(io.DisplaySize.x * 0.5f, 12.0f), ImVec2(0.5f, 0.0f)))
    {
        // The side badge.
        const std::string side = SideTitle(me);
        const ImVec2 t = Measure(Font().display, 20.0f, side.c_str());
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(t.x + 26.0f, 44.0f));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(at.x, at.y + 4.0f), ImVec2(at.x + t.x + 26.0f, at.y + 40.0f), SideColour(me, 0.9f), 9.0f);
        Put(draw, Font().display, 20.0f, ImVec2(at.x + 13.0f, at.y + 22.0f - t.y * 0.5f), IM_COL32(255, 255, 255, 255),
            side.c_str());

        char buf[64];
        ImGui::SameLine(0.0f, 22.0f);
        std::snprintf(buf, sizeof(buf), "%.0f", snap.war.pool[me]);
        Stat("SUPPLIES", buf);
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::BeginGroup();
        Label(Font().body, 12.0f, ColMuted, " ");
        std::snprintf(buf, sizeof(buf), "+%.0f/min", IncomePerMinute(snap.war, me));
        Label(Font().bold, 14.0f, ColGood, buf);
        ImGui::EndGroup();

        int owned = 0;
        for (const ObjectiveState& s : snap.war.states)
            if (s.owner == me)
                owned++;
        ImGui::SameLine(0.0f, 26.0f);
        std::snprintf(buf, sizeof(buf), "%d / %d", owned, static_cast<int>(snap.war.states.size()));
        Stat("OBJECTIVES", buf);
        ImGui::SameLine(0.0f, 26.0f);
        std::snprintf(buf, sizeof(buf), "%d / %d", snap.live[me], snap.cap);
        Stat("SOLDIERS", buf, snap.live[me] >= snap.cap ? ColWarn : ColText);

        // The war score of every side, as bars: when one empties, that side has lost.
        for (int s = 0; s < snap.war.sideCount; s++)
        {
            ImGui::SameLine(0.0f, 26.0f);
            ImGui::BeginGroup();
            std::snprintf(buf, sizeof(buf), "%s  %.0f", SideTitle(s).c_str(), snap.war.score[s]);
            Label(Font().body, 12.0f, ColMuted, buf);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(120.0f, 22.0f));
            const float share = snap.tuning.startScore > 0.0f ? std::clamp(snap.war.score[s] / snap.tuning.startScore, 0.0f, 1.0f) : 0.0f;
            draw->AddRectFilled(ImVec2(p.x, p.y + 8.0f), ImVec2(p.x + 120.0f, p.y + 16.0f), IM_COL32(255, 255, 255, 26), 4.0f);
            draw->AddRectFilled(ImVec2(p.x, p.y + 8.0f), ImVec2(p.x + 120.0f * share, p.y + 16.0f), SideColour(s), 4.0f);
            ImGui::EndGroup();
        }

        const int clock = static_cast<int>(snap.now);
        ImGui::SameLine(0.0f, 26.0f);
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", clock / 3600, (clock / 60) % 60, clock % 60);
        Stat("WAR TIME", buf);
        ImGui::SameLine(0.0f, 22.0f);
        ImGui::BeginGroup();
        Label(Font().body, 12.0f, ColMuted, " ");
        if (ImGui::Button(St().deployOpen ? "Close deploy" : "Deploy  B"))
            St().deployOpen = !St().deployOpen;
        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::Button(St().optionsOpen ? "Close options" : "Options"))
            St().optionsOpen = !St().optionsOpen;
        ImGui::EndGroup();
    }
    ImGui::End();
}

// ---- the soldier's HUD (the commander view closed) ----

// War events worth a banner: captures, losses of ground, the end. Picked from the referee's log
// as it grows, so the soldier hears about the war without opening the map.
struct Banner
{
    std::string text;
    ImU32 colour = ColText;
    Uint64 ticks = 0;
};

Banner& LastBanner()
{
    static Banner b;
    return b;
}

void CatchBanners(const Snapshot& snap)
{
    static std::string lastSeen;
    if (snap.log.empty())
        return;
    if (lastSeen.empty())
    {
        lastSeen = snap.log.back(); // the war's history before the HUD first looked is not news
        return;
    }
    for (auto it = snap.log.rbegin(); it != snap.log.rend() && *it != lastSeen; ++it)
    {
        const std::string& line = *it;
        const bool capture = line.find(" captured ") != std::string::npos;
        const bool lost = line.find(" neutralised ") != std::string::npos;
        const bool end = line.find(" wins the war") != std::string::npos;
        if (!capture && !lost && !end)
            continue;
        // "12:34 WEST captured Montignac" -> "BLUFOR captured Montignac", in that side's colour.
        std::string text = line.size() > 6 ? line.substr(6) : line;
        int side = NoSide;
        for (int s = 0; s < static_cast<int>(snap.sideNames.size()); s++)
            if (text.rfind(snap.sideNames[s], 0) == 0)
            {
                side = s;
                text = SideTitle(s) + text.substr(snap.sideNames[s].size());
            }
        LastBanner() = {text, side == NoSide ? ColText : SideColour(side), SDL_GetTicks()};
        break; // the newest one wins
    }
    lastSeen = snap.log.back();
}

void DrawSoldierHud(const Snapshot& snap)
{
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const Fonts& f = Font();
    const int me = PlayerSide();

    // A slim war bar at the top: both scores, objectives held, how to open the commander view.
    {
        char text[160];
        int owned = 0;
        for (const ObjectiveState& s : snap.war.states)
            if (s.owner == me)
                owned++;
        std::snprintf(text, sizeof(text), "Objectives %d / %d", owned, static_cast<int>(snap.war.states.size()));
        const float barW = 90.0f;
        const float width = 2.0f * (barW + 120.0f) + Measure(f.bold, 14.0f, text).x + 150.0f;
        const ImVec2 a(io.DisplaySize.x * 0.5f - width * 0.5f, 10.0f), b(io.DisplaySize.x * 0.5f + width * 0.5f, 44.0f);
        draw->AddRectFilled(a, b, WithAlpha(ColPanel, 0.82f), 17.0f);
        float x = a.x + 18.0f;
        for (int s = 0; s < snap.war.sideCount; s++)
        {
            char score[48];
            std::snprintf(score, sizeof(score), "%s %.0f", SideTitle(s).c_str(), snap.war.score[s]);
            Put(draw, f.bold, 14.0f, ImVec2(x, 19.0f), SideColour(s), score);
            x += Measure(f.bold, 14.0f, score).x + 10.0f;
            const float share = snap.tuning.startScore > 0.0f ? std::clamp(snap.war.score[s] / snap.tuning.startScore, 0.0f, 1.0f) : 0.0f;
            draw->AddRectFilled(ImVec2(x, 24.0f), ImVec2(x + barW, 30.0f), IM_COL32(255, 255, 255, 30), 3.0f);
            draw->AddRectFilled(ImVec2(x, 24.0f), ImVec2(x + barW * share, 30.0f), SideColour(s), 3.0f);
            x += barW + 20.0f;
        }
        Put(draw, f.bold, 14.0f, ImVec2(x, 19.0f), ColText, text);
        x += Measure(f.bold, 14.0f, text).x + 18.0f;
        Put(draw, f.body, 13.0f, ImVec2(x, 20.0f), ColMuted, "Ctrl+M  Command");
    }

    // HQ's order for the player's squad: what, where (bearing and distance).
    Person* player = GWorld ? GWorld->GetRealPlayer() : nullptr;
    if (const PlayerOrder order = CurrentPlayerOrder(); order.valid && player)
    {
        const Vector3 p = player->Position();
        const float dx = order.x - p.X(), dy = order.y - p.Z();
        const float dist = std::hypot(dx, dy);
        float bearing = std::atan2(dx, dy) * 180.0f / Pi;
        if (bearing < 0.0f)
            bearing += 360.0f;
        static const char* compass[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        char where[64];
        if (dist < 1000.0f)
            std::snprintf(where, sizeof(where), "%.0f m %s", dist, compass[static_cast<int>((bearing + 22.5f) / 45.0f) % 8]);
        else
            std::snprintf(where, sizeof(where), "%.1f km %s", dist / 1000.0f, compass[static_cast<int>((bearing + 22.5f) / 45.0f) % 8]);
        const std::string line = "HQ:  " + order.text;
        const float w = Measure(f.bold, 15.0f, line.c_str()).x + Measure(f.body, 14.0f, where).x + 56.0f;
        const ImVec2 a(14.0f, 56.0f), b(14.0f + w, 90.0f);
        draw->AddRectFilled(a, b, WithAlpha(ColPanel, 0.82f), 12.0f);
        draw->AddRectFilled(ImVec2(a.x, a.y + 6.0f), ImVec2(a.x + 4.0f, b.y - 6.0f), SideColour(me), 2.0f);
        Put(draw, f.bold, 15.0f, ImVec2(a.x + 16.0f, a.y + 8.0f), ColText, line.c_str());
        Put(draw, f.body, 14.0f, ImVec2(b.x - Measure(f.body, 14.0f, where).x - 16.0f, a.y + 9.0f), ColMuted, where);
    }

    // Something that happened to the player (killed, carried on as another soldier).
    {
        static std::string notice;
        static Uint64 noticeTicks = 0;
        if (std::string fresh; TakeSoldierNotice(fresh))
        {
            notice = fresh;
            noticeTicks = SDL_GetTicks();
        }
        const Uint64 age = SDL_GetTicks() - noticeTicks;
        if (!notice.empty() && age < 6000)
        {
            const float alpha = age > 5200 ? 1.0f - static_cast<float>(age - 5200) / 800.0f : 1.0f;
            const ImVec2 t = Measure(f.display, 22.0f, notice.c_str());
            const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.38f);
            draw->AddRectFilled(ImVec2(c.x - t.x * 0.5f - 24.0f, c.y - 22.0f), ImVec2(c.x + t.x * 0.5f + 24.0f, c.y + 22.0f),
                                WithAlpha(ColPanel, 0.88f * alpha), 22.0f);
            Put(draw, f.display, 22.0f, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), WithAlpha(ColWarn, alpha), notice.c_str());
        }
    }

    // At a command post: its capture bar, front and centre. In the town but outside the post's
    // circle: how far the post is, since only soldiers by it count.
    if (player)
    {
        const Vector3 p = player->Position();
        for (size_t o = 0; o < snap.war.defs.size(); o++)
        {
            const ObjectiveDef& d = snap.war.defs[o];
            const float dist = std::hypot(p.X() - d.x, p.Z() - d.y);
            if (dist > d.radius)
                continue;
            if (dist > d.captureRadius)
            {
                char text[96];
                std::snprintf(text, sizeof(text), "%s command post  %.0f m", d.name.c_str(), dist - d.captureRadius);
                const ImVec2 t = Measure(f.bold, 15.0f, text);
                const ImVec2 a(io.DisplaySize.x * 0.5f - t.x * 0.5f - 18.0f, 56.0f);
                draw->AddRectFilled(a, ImVec2(a.x + t.x + 36.0f, a.y + 32.0f), WithAlpha(ColPanel, 0.82f), 16.0f);
                Put(draw, f.bold, 15.0f, ImVec2(a.x + 18.0f, a.y + 16.0f - t.y * 0.5f), SideColour(snap.war.states[o].owner), text);
                break;
            }
            const ObjectiveState& st = snap.war.states[o];
            const char* state = st.contested ? "CONTESTED"
                                : st.owner == me && st.holder == me && st.level >= 1.0f ? "SECURE"
                                : st.holder == me ? "CAPTURING"
                                : st.owner == NoSide && st.level <= 0.0f ? "NEUTRAL"
                                                                          : "LOSING GROUND";
            const ImU32 stateColour = st.contested ? ColWarn : st.holder == me ? ColGood : st.owner == NoSide ? ColMuted : ColBad;
            const float w = 340.0f;
            const ImVec2 a(io.DisplaySize.x * 0.5f - w * 0.5f, 56.0f), b(io.DisplaySize.x * 0.5f + w * 0.5f, 122.0f);
            draw->AddRectFilled(a, b, WithAlpha(ColPanel, 0.86f), 14.0f);
            draw->AddRect(a, b, WithAlpha(SideColour(st.owner), 0.7f), 14.0f, 0, 1.5f);
            Put(draw, f.display, 20.0f, ImVec2(a.x + 18.0f, a.y + 10.0f), ColText, d.name.c_str());
            const float sw = Measure(f.bold, 14.0f, state).x;
            Put(draw, f.bold, 14.0f, ImVec2(b.x - sw - 18.0f, a.y + 14.0f), stateColour, state);
            const ImVec2 ba(a.x + 18.0f, a.y + 44.0f), bb(b.x - 18.0f, a.y + 52.0f);
            draw->AddRectFilled(ba, bb, IM_COL32(255, 255, 255, 30), 4.0f);
            draw->AddRectFilled(ba, ImVec2(ba.x + (bb.x - ba.x) * st.level, bb.y), SideColour(st.holder), 4.0f);
            if (st.owner == me && st.holder == me && st.level >= 1.0f)
            {
                // The post's arsenal box (ConquestReferee UpdateArsenals): the inventory restocks from it.
                const ArsenalPrices prices = GetArsenalPrices();
                char priced[160];
                std::snprintf(priced, sizeof(priced), "Arsenal here: open your inventory to restock (weapon %.0f, magazine %.0f of %.0f supplies)",
                              prices.weapon, prices.magazine, snap.war.pool[me]);
                const char* hint = GWorld && GWorld->GetMode() == GModeNetware ? "Arsenal here: use the box's Gear action to restock"
                                                                                : priced;
                const ImVec2 t = Measure(f.body, 13.0f, hint);
                const ImVec2 ha(io.DisplaySize.x * 0.5f - t.x * 0.5f - 14.0f, b.y + 6.0f);
                draw->AddRectFilled(ha, ImVec2(ha.x + t.x + 28.0f, ha.y + 26.0f), WithAlpha(ColPanel, 0.8f), 13.0f);
                Put(draw, f.body, 13.0f, ImVec2(ha.x + 14.0f, ha.y + 13.0f - t.y * 0.5f), ColGood, hint);
            }
            break;
        }
    }

    // The latest capture, loss or ending, for five seconds.
    const Banner& banner = LastBanner();
    const Uint64 age = SDL_GetTicks() - banner.ticks;
    if (!banner.text.empty() && age < 5000)
    {
        const float alpha = age > 4200 ? 1.0f - static_cast<float>(age - 4200) / 800.0f : 1.0f;
        const ImVec2 t = Measure(f.display, 22.0f, banner.text.c_str());
        const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.28f);
        draw->AddRectFilled(ImVec2(c.x - t.x * 0.5f - 24.0f, c.y - 22.0f), ImVec2(c.x + t.x * 0.5f + 24.0f, c.y + 22.0f),
                            WithAlpha(ColPanel, 0.85f * alpha), 22.0f);
        Put(draw, f.display, 22.0f, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), WithAlpha(banner.colour, alpha),
            banner.text.c_str());
    }
}

void DrawToast()
{
    const State& s = St();
    const Uint64 age = SDL_GetTicks() - s.statusTicks;
    if (s.status.empty() || age > 5000)
        return;
    const float alpha = age > 4200 ? 1.0f - static_cast<float>(age - 4200) / 800.0f : 1.0f;
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 t = Measure(Font().bold, 16.0f, s.status.c_str());
    const ImVec2 c(io.DisplaySize.x * 0.5f, 112.0f);
    const ImVec2 a(c.x - t.x * 0.5f - 20.0f, c.y - 18.0f), b(c.x + t.x * 0.5f + 20.0f, c.y + 18.0f);
    draw->AddRectFilled(a, b, WithAlpha(ColPanel, 0.9f * alpha), 18.0f);
    draw->AddRect(a, b, WithAlpha(s.statusColour, 0.6f * alpha), 18.0f, 0, 1.5f);
    Put(draw, Font().bold, 16.0f, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), WithAlpha(s.statusColour, alpha),
        s.status.c_str());
}

Arm ArmOfRole(const std::string& role)
{
    if (role == "motorised" || role == "recon")
        return Arm::Motorised;
    if (role == "mechanised")
        return Arm::Mechanised;
    if (role == "armour")
        return Arm::Armour;
    if (role == "air" || role == "gunship")
        return Arm::Rotary;
    return Arm::Infantry;
}

void DrawSquadList(const Snapshot& snap, const std::vector<SquadDraw>& squads)
{
    const ImGuiIO& io = ImGui::GetIO();
    // Above the minimap, never over it.
    if (BeginPanel("##conquest_squads", ImVec2(14.0f, 96.0f), ImVec2(0.0f, 0.0f),
                   ImVec2(260.0f, std::max(120.0f, io.DisplaySize.y - 96.0f - 254.0f))))
    {
        char count[32];
        std::snprintf(count, sizeof(count), "%d", static_cast<int>(squads.size()));
        Heading("SQUADS", count);
        const float width = 228.0f;
        for (const SquadDraw& d : squads)
        {
            ImGui::PushID(d.group.id);
            const bool sel = IsSelected(d.group.id);
            const float h = sel ? 46.0f : 28.0f;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("card", ImVec2(width, h)))
            {
                if (io.KeyShift || io.KeyCtrl)
                    ToggleSelected(d.group.id);
                else
                    St().selected = {d.group.id};
                St().unit = nullptr;
            }
            const bool hovered = ImGui::IsItemHovered();
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                FocusOn(d.worldX, d.worldY);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 b(at.x + width, at.y + h);
            const ImU32 shade = SquadShade(d.group.side, d.group.id);
            draw->AddRectFilled(at, b, sel ? WithAlpha(ColSelect, 0.16f) : hovered ? ColCardHover : ColCard, 7.0f);
            if (sel)
                draw->AddRect(at, b, WithAlpha(ColSelect, 0.8f), 7.0f, 0, 1.2f);
            DrawSymbol(draw, ImVec2(at.x + 16.0f, at.y + 20.0f), 8.0f, ArmOfRole(d.group.role), false, shade, 1.0f, false);
            const std::string name = CallsignFull(d.group.id);
            Put(draw, Font().bold, 14.0f, ImVec2(at.x + 32.0f, at.y + 5.0f), ColText, name.c_str());
            if (d.group.locked && !d.group.playerLed)
            {
                const float nx = at.x + 32.0f + Measure(Font().bold, 14.0f, name.c_str()).x + 6.0f;
                draw->AddCircleFilled(ImVec2(nx + 3.0f, at.y + 14.0f), 3.0f, ColSelect); // under your orders
            }
            // Strength: one pip per soldier the squad started with.
            const int size = std::max(d.group.size, d.men);
            for (int i = 0; i < size; i++)
            {
                const ImVec2 pt(b.x - 10.0f - static_cast<float>(size - 1 - i) * 7.0f, at.y + 14.0f);
                if (i < d.men)
                    draw->AddCircleFilled(pt, 2.5f, shade);
                else
                    draw->AddCircle(pt, 2.5f, IM_COL32(255, 255, 255, 60), 10, 1.0f);
            }
            if (sel)
            {
                std::string buffer;
                const char* task = Task(d.group, snap, buffer);
                Put(draw, Font().body, 12.5f, ImVec2(at.x + 32.0f, at.y + 26.0f),
                    d.group.stalled ? ColWarn : d.group.playerLed ? ColSelect : ColMuted, task);
            }
            else if (hovered)
            {
                std::string buffer;
                ImGui::BeginTooltip();
                ImGui::TextUnformatted((d.group.typeName + "  -  " + Task(d.group, snap, buffer)).c_str());
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        if (squads.empty())
            Label(Font().body, 13.0f, ColMuted, "No squads. Press B to deploy.");
    }
    ImGui::End();
}

const char* RoleTransport(const std::string& role)
{
    if (role == "motorised")
        return "with transport";
    if (role == "mechanised")
        return "with an APC";
    if (role == "air")
        return "with a helicopter";
    return "on foot";
}

void DrawDeploy(const Snapshot& snap)
{
    const ImGuiIO& io = ImGui::GetIO();
    const int me = PlayerSide();
    State& s = St();
    if (!s.deployOpen)
        return;
    if (s.deployBase < 0 || s.deployBase >= static_cast<int>(snap.war.defs.size()) || snap.war.states[s.deployBase].owner != me)
        s.deployBase = HomeBase();
    if (BeginPanel("##conquest_deploy", ImVec2(io.DisplaySize.x - 14.0f, 96.0f), ImVec2(1.0f, 0.0f),
                   ImVec2(360.0f, io.DisplaySize.y - 220.0f)))
    {
        Heading("DEPLOY");
        ImGui::Spacing();
        // The bases this side may deploy at, as chips: green when ready, amber when not.
        float lineWidth = 0.0f;
        for (size_t o = 0; o < snap.war.defs.size(); o++)
        {
            if (snap.war.states[o].owner != me)
                continue;
            std::string why;
            const bool ready = CanBuy(static_cast<int>(o), 0, &why) || why == "not enough supplies" ||
                               why == "manpower cap reached";
            const std::string name = snap.war.defs[o].name;
            const ImVec2 t = Measure(Font().bold, 14.0f, name.c_str());
            const float w = t.x + 34.0f;
            if (lineWidth > 0.0f && lineWidth + w > 320.0f)
                lineWidth = 0.0f;
            else if (lineWidth > 0.0f)
                ImGui::SameLine(0.0f, 8.0f);
            lineWidth += w + 8.0f;
            ImGui::PushID(static_cast<int>(o));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("base", ImVec2(w, 30.0f)))
            {
                s.deployBase = static_cast<int>(o);
                FocusOn(snap.war.defs[o].x, snap.war.defs[o].y);
            }
            const bool chosen = s.deployBase == static_cast<int>(o);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(at, ImVec2(at.x + w, at.y + 30.0f),
                                chosen ? WithAlpha(ColSelect, 0.2f) : ImGui::IsItemHovered() ? ColCardHover : ColCard, 15.0f);
            if (chosen)
                draw->AddRect(at, ImVec2(at.x + w, at.y + 30.0f), WithAlpha(ColSelect, 0.8f), 15.0f, 0, 1.5f);
            draw->AddCircleFilled(ImVec2(at.x + 15.0f, at.y + 15.0f), 4.5f, ready ? ColGood : ColWarn);
            Put(draw, Font().bold, 14.0f, ImVec2(at.x + 25.0f, at.y + 15.0f - t.y * 0.5f), ColText, name.c_str());
            ImGui::PopID();
        }
        ImGui::Spacing();
        if (s.deployBase < 0)
        {
            Label(Font().body, 14.0f, ColMuted, "You hold no base to deploy at.");
            ImGui::End();
            return;
        }
        std::string why;
        const bool open = CanBuy(s.deployBase, 0, &why) || why == "not enough supplies" || why == "manpower cap reached";
        const std::string where = snap.war.defs[s.deployBase].name + (open ? "  -  ready" : "  -  " + why);
        Label(Font().body, 13.0f, open ? ColGood : ColWarn, where.c_str());
        ImGui::Spacing();

        // The squads to buy, as tiles.
        const std::vector<SquadInfo> types = SquadTypes();
        const float tileW = 156.0f, tileH = 50.0f; // compact: the panel and its SUPPORT fit a 720p screen
        for (size_t t = 0; t < types.size(); t++)
        {
            if (t % 2 == 1)
                ImGui::SameLine(0.0f, 10.0f);
            std::string reason;
            const bool can = CanBuy(s.deployBase, static_cast<int>(t), &reason);
            ImGui::PushID(static_cast<int>(t));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const bool pressed = ImGui::InvisibleButton("tile", ImVec2(tileW, tileH));
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 b(at.x + tileW, at.y + tileH);
            draw->AddRectFilled(at, b, can ? (hovered ? IM_COL32(60, 78, 102, 255) : IM_COL32(38, 48, 64, 255))
                                           : IM_COL32(30, 34, 42, 200),
                                10.0f);
            const ImU32 text = can ? ColText : ColMuted;
            Put(draw, Font().bold, 14.0f, ImVec2(at.x + 12.0f, at.y + 7.0f), text, types[t].name.c_str());
            char line[64];
            std::snprintf(line, sizeof(line), "%d soldiers", types[t].size);
            Put(draw, Font().body, 12.0f, ImVec2(at.x + 12.0f, at.y + 28.0f), ColMuted, line);
            std::snprintf(line, sizeof(line), "%.0f", types[t].cost);
            const ImVec2 ct = Measure(Font().display, 16.0f, line);
            Put(draw, Font().display, 16.0f, ImVec2(b.x - ct.x - 10.0f, b.y - ct.y - 6.0f), can ? ColGood : ColBad, line);
            // Short of supplies or a quiet base, a click still orders it: the order waits (2 min) and deploys when it can.
            if (pressed)
            {
                std::string refused;
                const int id = BuySquad(s.deployBase, static_cast<int>(t), &refused);
                if (id < 0) // multiplayer: asked of the server, which answers on the radio
                    SetStatus(types[t].name + " requested at " + snap.war.defs[s.deployBase].name + ".");
                else if (id)
                {
                    St().selected = {id};
                    SetStatus(types[t].name + " deployed at " + snap.war.defs[s.deployBase].name + "  -  " +
                              CallsignFull(id) + " awaits your orders");
                }
                else if (refused.rfind("queued", 0) == 0)
                    SetStatus(types[t].name + " " + refused, ColWarn);
                else
                    SetStatus("Cannot deploy: " + refused, ColWarn);
            }
            if (hovered && !can)
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(("Not now: " + reason + ". Click to queue it: it deploys when it can.").c_str());
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        Heading("SUPPORT");
        ImGui::Spacing();
        {
            const BarrageInfo b = Barrage();
            const bool ready = b.cooldownLeft <= 0.0f && snap.war.pool[me] >= b.cost;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float w = 156.0f, h = 50.0f;
            const bool pressed = ImGui::InvisibleButton("barrage", ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h),
                                s.targeting && !s.targetAir ? WithAlpha(ColWarn, 0.25f)
                                : ready     ? (hovered ? IM_COL32(60, 78, 102, 255) : IM_COL32(38, 48, 64, 255))
                                            : IM_COL32(30, 34, 42, 200),
                                10.0f);
            Put(draw, Font().bold, 14.0f, ImVec2(at.x + 12.0f, at.y + 7.0f), ready ? ColText : ColMuted, "Mortars");
            char line[96];
            if (b.cooldownLeft > 0.0f)
                std::snprintf(line, sizeof(line), "reloading, %.0f s", b.cooldownLeft);
            else
                std::snprintf(line, sizeof(line), "%d rounds, %s", b.shells, s.targeting && !s.targetAir ? "aim" : "ready");
            Put(draw, Font().body, 12.0f, ImVec2(at.x + 12.0f, at.y + 28.0f), ColMuted, line);
            std::snprintf(line, sizeof(line), "%.0f", b.cost);
            const ImVec2 ct = Measure(Font().display, 16.0f, line);
            Put(draw, Font().display, 16.0f, ImVec2(at.x + w - ct.x - 10.0f, at.y + h - ct.y - 6.0f),
                snap.war.pool[me] >= b.cost ? ColGood : ColBad, line);
            if (pressed)
            {
                if (s.targeting && !s.targetAir)
                    s.targeting = false;
                else if (ready)
                {
                    s.targeting = true;
                    s.targetAir = false;
                    SetStatus("Click where the mortars should land. Right click cancels.", ColWarn);
                }
                else
                    SetStatus(b.cooldownLeft > 0.0f ? "The mortars are reloading." : "Not enough supplies for a barrage.", ColWarn);
            }
        }
        ImGui::SameLine(0.0f, 10.0f);
        {
            // Air support: one plane, a tight stick of bombs exactly where it is called.
            const AirStrikeInfo a = AirStrike();
            const bool ready = a.cooldownLeft <= 0.0f && !a.inbound && snap.war.pool[me] >= a.cost;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float w = 156.0f, h = 50.0f;
            const bool pressed = ImGui::InvisibleButton("air", ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h),
                                s.targeting && s.targetAir ? WithAlpha(ColWarn, 0.25f)
                                : ready ? (hovered ? IM_COL32(60, 78, 102, 255) : IM_COL32(38, 48, 64, 255))
                                        : IM_COL32(30, 34, 42, 200),
                                10.0f);
            Put(draw, Font().bold, 14.0f, ImVec2(at.x + 12.0f, at.y + 7.0f), ready ? ColText : ColMuted, "Air strike");
            char line[96];
            if (a.inbound)
                std::snprintf(line, sizeof(line), "plane inbound");
            else if (a.cooldownLeft > 0.0f)
                std::snprintf(line, sizeof(line), "rearming, %.0f s", a.cooldownLeft);
            else
                std::snprintf(line, sizeof(line), "%d bombs, %s", a.bombs, s.targeting && s.targetAir ? "aim" : "ready");
            Put(draw, Font().body, 12.0f, ImVec2(at.x + 12.0f, at.y + 28.0f), ColMuted, line);
            std::snprintf(line, sizeof(line), "%.0f", a.cost);
            const ImVec2 ct = Measure(Font().display, 16.0f, line);
            Put(draw, Font().display, 16.0f, ImVec2(at.x + w - ct.x - 10.0f, at.y + h - ct.y - 6.0f),
                snap.war.pool[me] >= a.cost ? ColGood : ColBad, line);
            if (pressed)
            {
                if (s.targeting && s.targetAir)
                    s.targeting = s.targetAir = false;
                else if (ready)
                {
                    s.targeting = true;
                    s.targetAir = true;
                    SetStatus("Click the target for the bombs. Right click cancels.", ColWarn);
                }
                else
                    SetStatus(a.inbound ? "A plane is already on its way." : a.cooldownLeft > 0.0f ? "The plane is rearming."
                                                                                                 : "Not enough supplies for an air strike.",
                              ColWarn);
            }
        }
    }
    ImGui::End();
}

// ---- AI & voice: the player-facing face of NPC chat (the dev panel's NPC chat tab writes the same
// npcChat.json and the same key store: one settings struct, two views) ----

const char* LinkWord(const NpcChat::LinkStatus& status)
{
    switch (status.state)
    {
        case NpcChat::LinkState::Ok: return "connected";
        case NpcChat::LinkState::Failed: return "failed";
        case NpcChat::LinkState::Checking: return "checking...";
        case NpcChat::LinkState::Unknown: break;
    }
    return "not checked";
}

ImU32 LinkColour(const NpcChat::LinkStatus& status)
{
    return status.state == NpcChat::LinkState::Ok       ? ColGood
           : status.state == NpcChat::LinkState::Failed ? ColBad
                                                        : ColMuted;
}

template <typename Enum>
bool ProviderCombo(const char* label, Enum& value, const char* (*name)(Enum))
{
    bool changed = false;
    if (ImGui::BeginCombo(label, name(value)))
    {
        for (int i = 0; i < static_cast<int>(Enum::Count); i++)
        {
            const Enum e = static_cast<Enum>(i);
            if (ImGui::Selectable(name(e), e == value))
            {
                value = e;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// One key field: typed keys are held for the session (and stored encrypted only with "Remember").
void KeyField(const char* id, const std::string& slot)
{
    if (slot.empty())
        return;
    static std::map<std::string, std::array<char, 256>> buffers;
    auto& buffer = buffers[slot];
    ImGui::PushID(id);
    const bool hasKey = NpcChat::HasPanelKey(slot) || NpcChat::HasEnvKey(slot);
    ImGui::SetNextItemWidth(250.0f);
    ImGui::InputTextWithHint("##key", hasKey ? "key set (type to replace)" : "paste your API key", buffer.data(),
                             buffer.size(), ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    if (ImGui::Button("Use key") && buffer[0])
    {
        NpcChat::SetKey(slot, NpcChat::SanitizeKey(buffer.data()));
        buffer.fill(0);
        std::string error;
        NpcChat::PersistKeys(NpcChat::CurrentSettings().rememberKeys, error);
    }
    ImGui::PopID();
}

void DrawVoicePanel()
{
    State& s = St();
    if (!s.voiceOpen)
        return;
    const ImGuiIO& io = ImGui::GetIO();
    NpcChat::Settings& cfg = NpcChat::CurrentSettings();
    bool changed = false;
    if (BeginPanel("##conquest_voice", ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImVec2(0.5f, 0.5f)))
    {
        Label(Font().display, 22.0f, ColText, "AI & VOICE");
        Label(Font().body, 13.0f, ColMuted, "Talk to HQ and your squad, and let an AI model command a side.");
        if (GWorld && GWorld->GetMode() == GModeNetware)
            Label(Font().body, 13.0f, ColWarn, "Talking to HQ is single player only; the LLM commander runs on the server.");
        ImGui::Spacing();
        // The settings scroll; Close stays in view at any window height.
        ImGui::BeginChild("##voice_settings", ImVec2(520.0f, std::min(470.0f, io.DisplaySize.y - 190.0f)));

        Heading("CONVERSATION");
        changed |= ImGui::Checkbox("Talk to HQ and the squad (Insert: type, hold Caps Lock: speak)", &cfg.enabled);

        ImGui::Spacing();
        Heading("LANGUAGE MODEL");
        changed |= ProviderCombo("Provider##llm", cfg.llm, NpcChat::LlmProviderName);
        {
            static std::array<char, 128> model{};
            if (model[0] == 0)
                std::snprintf(model.data(), model.size(), "%s", cfg.llmModel.c_str());
            ImGui::SetNextItemWidth(250.0f);
            if (ImGui::InputTextWithHint("Model##llm", cfg.EffectiveLlmModel().c_str(), model.data(), model.size()))
            {
                cfg.llmModel = model.data();
                changed = true;
            }
        }
        if (cfg.llm == NpcChat::LlmProvider::Ollama || cfg.llm == NpcChat::LlmProvider::Custom)
        {
            static std::array<char, 256> url{};
            if (url[0] == 0)
                std::snprintf(url.data(), url.size(), "%s", cfg.llmBaseUrl.c_str());
            ImGui::SetNextItemWidth(250.0f);
            if (ImGui::InputTextWithHint("Address##llm", cfg.EffectiveLlmBaseUrl().c_str(), url.data(), url.size()))
            {
                cfg.llmBaseUrl = url.data();
                changed = true;
            }
        }
        KeyField("llmkey", NpcChat::LlmKeySlot(cfg.llm));
        if (ImGui::Button("Check##llm"))
            NpcChat::CheckLlm();
        ImGui::SameLine();
        Label(Font().body, 13.0f, LinkColour(NpcChat::LlmStatus()), LinkWord(NpcChat::LlmStatus()));
        if (!NpcChat::LlmStatus().message.empty() && NpcChat::LlmStatus().state == NpcChat::LinkState::Failed)
            Label(Font().body, 12.0f, ColMuted, NpcChat::LlmStatus().message.substr(0, 90).c_str());

        ImGui::Spacing();
        Heading("VOICE INPUT (SPEECH TO TEXT)");
        changed |= ProviderCombo("Provider##stt", cfg.stt, NpcChat::SttProviderName);
        if (cfg.stt != NpcChat::SttProvider::Off)
        {
            KeyField("sttkey", NpcChat::SttKeySlot(cfg.stt));
            if (ImGui::Button("Check##stt"))
                NpcChat::CheckStt();
            ImGui::SameLine();
            Label(Font().body, 13.0f, LinkColour(NpcChat::SttStatus()), LinkWord(NpcChat::SttStatus()));
            Label(Font().body, 12.0f, ColMuted, "Hold Caps Lock in game and speak; release to send.");
        }

        ImGui::Spacing();
        Heading("VOICE OUTPUT (TEXT TO SPEECH)");
        changed |= ProviderCombo("Provider##tts", cfg.tts, NpcChat::TtsProviderName);
        if (cfg.tts != NpcChat::TtsProvider::Off)
        {
            if (cfg.tts != NpcChat::TtsProvider::WindowsSapi)
                KeyField("ttskey", NpcChat::TtsKeySlot(cfg.tts));
            ImGui::SetNextItemWidth(250.0f);
            changed |= ImGui::SliderFloat("Volume##tts", &cfg.ttsVolume, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::Checkbox("Radio voice when far away", &cfg.ttsRadioWhenFar);
            if (ImGui::Button("Test voice"))
                NpcChat::TestVoice();
            ImGui::SameLine();
            if (ImGui::Button("Check##tts"))
                NpcChat::CheckTts();
            ImGui::SameLine();
            Label(Font().body, 13.0f, LinkColour(NpcChat::TtsStatus()), LinkWord(NpcChat::TtsStatus()));
        }

        ImGui::Spacing();
        changed |= ImGui::Checkbox("Remember keys on this PC (encrypted for your Windows account)", &cfg.rememberKeys);
        if (!NpcChat::StatusLine().empty())
            Label(Font().body, 12.0f, ColMuted, NpcChat::StatusLine().substr(0, 100).c_str());
        ImGui::EndChild();
        ImGui::Spacing();
        if (ImGui::Button("Close"))
            s.voiceOpen = false;
    }
    ImGui::End();
    if (changed)
    {
        NpcChat::SaveSettings();
        std::string error;
        NpcChat::PersistKeys(cfg.rememberKeys, error);
    }
}

// The war's options, from the top bar's Options button.
void DrawOptions()
{
    State& s = St();
    if (!s.optionsOpen)
        return;
    const ImGuiIO& io = ImGui::GetIO();
    if (BeginPanel("##conquest_options", ImVec2(io.DisplaySize.x * 0.5f, 96.0f), ImVec2(0.5f, 0.0f)))
    {
        Heading("OPTIONS");
        ImGui::Spacing();
        Options o = GetOptions();
        bool changed = false;
        // The fog of war is chosen before the war (the setup screen), never during it: switching it
        // off mid-game would show what the side has not seen.
        Label(Font().body, 13.0f, ColMuted, o.fog == FogMode::Shroud ? "Fog of war: shroud (set before the war)"
                                                                     : "Fog of war: contacts (set before the war)");
        ImGui::Spacing();
        changed |= ImGui::Checkbox("Taking control of soldiers", &o.possession);
        changed |= ImGui::Checkbox("HQ radios its orders when you are not in command", &o.hqRadio);
        if (changed)
            SetOptions(o);
        ImGui::Spacing();
        for (int side = 0; side < static_cast<int>(SideNames().size()); side++)
        {
            const std::string line = SideTitle(side) + " commander: " + (o.llmCommander[side] ? "LLM (Hybrid)" : "Standard AI") +
                                     " (set before the war)";
            Label(Font().body, 13.0f, ColMuted, line.c_str());
        }
        if (ImGui::Button("AI & voice..."))
            s.voiceOpen = true;
        ImGui::Spacing();
        if (ImGui::Button("Close"))
            s.optionsOpen = false;
    }
    ImGui::End();
}

// The mission's opening screen (CfgConquest setup = 1): which side, how to start, the options.
void DrawSetup()
{
    State& s = St();
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(6, 9, 13, 170));
    static Options o;
    static bool initialised = false;
    if (!initialised)
    {
        o = GetOptions();
        initialised = true;
    }
    if (BeginPanel("##conquest_setup", ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImVec2(0.5f, 0.5f)))
    {
        Label(Font().display, 30.0f, ColText, "CONQUEST");
        Label(Font().body, 14.0f, ColMuted, "Take the towns' command posts. The side whose war score runs out loses.");
        ImGui::Spacing();
        ImGui::Spacing();
        // Two columns, so the whole screen fits a 720p window: the side and the start on the left,
        // the options and the commanders on the right, the buttons underneath.
        ImGui::BeginGroup();
        Heading("YOUR SIDE");
        ImGui::Spacing();
        const std::vector<std::string> sides = SideNames();
        const int optional = OptionalSide();
        const auto takesPart = [&](size_t i) { return static_cast<int>(i) != optional || o.resistance; };
        if (!takesPart(static_cast<size_t>(o.side)))
            o.side = 0;
        for (size_t i = 0; i < sides.size(); i++)
        {
            if (!takesPart(i))
                continue;
            if (i)
                ImGui::SameLine(0.0f, 12.0f);
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 size(o.resistance && optional != NoSide ? 170.0f : 200.0f, 76.0f);
            if (ImGui::InvisibleButton("side", size))
                o.side = static_cast<int>(i);
            const bool chosen = o.side == static_cast<int>(i);
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImU32 colour = SideColour(static_cast<int>(i));
            draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y),
                                chosen ? WithAlpha(colour, 0.35f) : hovered ? ColCardHover : ColCard, 12.0f);
            draw->AddRect(at, ImVec2(at.x + size.x, at.y + size.y), WithAlpha(colour, chosen ? 1.0f : 0.4f), 12.0f, 0,
                          chosen ? 2.5f : 1.0f);
            const std::string title = SideTitle(static_cast<int>(i));
            Put(draw, Font().display, 24.0f, ImVec2(at.x + 16.0f, at.y + 12.0f), ColText, title.c_str());
            Put(draw, Font().body, 13.0f, ImVec2(at.x + 16.0f, at.y + 46.0f), ColMuted,
                sides[i] == "WEST" ? "US Army" : sides[i] == "EAST" ? "Soviet Army" : "Resistance");
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::Spacing();
        Heading("START");
        ImGui::Spacing();
        if (ImGui::RadioButton("As a squad leader on the ground", !o.startInCommand))
            o.startInCommand = false;
        if (ImGui::RadioButton("In command, from above", o.startInCommand && o.commanderView))
        {
            o.startInCommand = true;
            o.commanderView = true;
        }
        ImGui::Spacing();
        Heading("THE WAR");
        ImGui::Spacing();
        if (optional != NoSide)
            ImGui::Checkbox("The Resistance fights too (a third side, from its own base)", &o.resistance);
        const int towns = VictoryTownsMax(o.resistance);
        o.victoryTowns = std::clamp(o.victoryTowns, 0, towns);
        ImGui::SetNextItemWidth(260.0f);
        ImGui::SliderInt("Villages to win", &o.victoryTowns, 0, towns,
                         o.victoryTowns == 0 ? "off: the war score decides" : o.victoryTowns == towns ? "all %d" : "%d");
        for (size_t i = 0; i < sides.size(); i++)
        {
            if (!takesPart(i))
                continue;
            ImGui::PushID(static_cast<int>(i) + 200);
            ImGui::SetNextItemWidth(260.0f);
            const std::string label = SideTitle(static_cast<int>(i)) + " supplies";
            ImGui::SliderFloat(label.c_str(), &o.startPool[i], 0.0f, 5000.0f, "%.0f");
            o.startPool[i] = std::round(o.startPool[i] / 50.0f) * 50.0f;
            ImGui::PopID();
        }
        ImGui::EndGroup();
        ImGui::SameLine(0.0f, 40.0f);
        ImGui::BeginGroup();
        Heading("OPTIONS");
        ImGui::Spacing();
        if (ImGui::Checkbox("Commander view (Ctrl+M: the top-down strategy view)", &o.commanderView) && !o.commanderView)
            o.startInCommand = false;
        Label(Font().body, 13.0f, ColMuted, "Fog of war");
        if (ImGui::RadioButton("Contacts: enemies only as your side knows them", o.fog == FogMode::Contacts))
            o.fog = FogMode::Contacts;
        if (ImGui::RadioButton("Shroud: unexplored ground stays black (top-down view only)", o.fog == FogMode::Shroud))
            o.fog = FogMode::Shroud;
        ImGui::Checkbox("Taking control of any of your soldiers", &o.possession);
        ImGui::Checkbox("HQ radios its orders", &o.hqRadio);
        ImGui::Spacing();
        Heading("COMMANDERS");
        Label(Font().body, 12.0f, ColMuted, "An LLM sets the priorities; the standard AI does the rest.");
        for (size_t i = 0; i < sides.size(); i++)
        {
            if (!takesPart(i))
                continue;
            ImGui::PushID(static_cast<int>(i) + 100);
            bool llm = o.llmCommander[i];
            const std::string label = SideTitle(static_cast<int>(i)) + ": LLM commander (Hybrid)";
            if (ImGui::Checkbox(label.c_str(), &llm))
                o.llmCommander[i] = llm;
            ImGui::PopID();
        }
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, SideColour(o.side, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, SideColour(o.side, 1.0f));
        ImGui::PushFont(Pick(Font().display), 20.0f);
        if (ImGui::Button("   Start the war   "))
        {
            if (BeginWar(o))
            {
                initialised = false;
                s.setupGrab = false;
                if (GEngine)
                    GEngine->SetMouseGrab(true);
                SDL_HideCursor();
            }
        }
        ImGui::PopFont();
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::Button("AI & voice..."))
            s.voiceOpen = true;
    }
    ImGui::End();
}

// Multiplayer: the side's vote on its commander, while it runs.
void DrawVote()
{
    const CommandSeat c = Seat();
    if (!c.multiplayer || !c.vote)
        return;
    const ImGuiIO& io = ImGui::GetIO();
    if (BeginPanel("##conquest_vote", ImVec2(io.DisplaySize.x * 0.5f, 72.0f), ImVec2(0.5f, 0.0f)))
    {
        const std::string q = c.voteCaller + " asks: remove " + c.voteTarget + " as commander?";
        Label(Font().bold, 15.0f, ColWarn, q.c_str());
        char line[160];
        std::snprintf(line, sizeof(line), "%.0f s left  -  yes %d, no %d, of %d players  -  Ctrl+Y yes, Ctrl+N no%s",
                      c.voteLeft, c.yes, c.no, c.electorate,
                      c.myVote > 0 ? "  (you: yes)" : c.myVote < 0 ? "  (you: no)" : "");
        Label(Font().body, 13.0f, ColMuted, line);
        if (St().active)
        {
            if (ImGui::Button("Yes, remove"))
                CastVote(true);
            ImGui::SameLine(0.0f, 10.0f);
            if (ImGui::Button("No, keep"))
                CastVote(false);
        }
    }
    ImGui::End();
}

void Notify(const std::string& text)
{
    if (St().active)
        SetStatus(text, ColWarn);
    else if (GWorld && GWorld->UI())
        GWorld->UI()->ShowHint(RString(text.c_str()));
}

// After the war: who won and why; a new war, the menu, or a last look round.
void DrawEnd(const Snapshot& snap)
{
    State& s = St();
    if (snap.winner == NoSide)
    {
        s.endClosed = false;
        s.endGrab = false;
        return;
    }
    if (s.endClosed)
        return;
    if (!s.endGrab && GEngine)
    {
        GEngine->SetMouseGrab(false);
        SDL_ShowCursor();
        s.endGrab = true;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (BeginPanel("##conquest_end", ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f), ImVec2(0.5f, 0.5f)))
    {
        const bool won = snap.winner == PlayerSide();
        Label(Font().display, 34.0f, won ? ColGood : ColBad, won ? "VICTORY" : "DEFEAT");
        const std::string why = SideTitle(snap.winner) + " wins the war: " + snap.winReason + ".";
        Label(Font().body, 15.0f, ColText, why.c_str());
        ImGui::Spacing();
        for (int side = 0; side < snap.war.sideCount; side++)
        {
            char line[160];
            std::snprintf(line, sizeof(line), "%s: holds %d, lost %d men, war score %.0f", SideTitle(side).c_str(),
                          OwnedCount(snap.war, side), snap.losses[side], snap.war.score[side]);
            Label(Font().body, 13.0f, SideColour(side), line);
        }
        Label(Font().body, 12.0f, ColMuted, "The result is written to conquest-history.txt in your profile.");
        ImGui::Spacing();
        ImGui::Spacing();
        const bool single = !GWorld || GWorld->GetMode() != GModeNetware;
        if (single && ImGui::Button("   New war   "))
        {
            if (Active())
                SetActive(false);
            if (NewWar())
            {
                s.endClosed = false;
                s.endGrab = false;
                s.setupGrab = true; // the mouse is already free for the setup screen
            }
        }
        if (single)
            ImGui::SameLine(0.0f, 12.0f);
        if (ImGui::Button("   Main menu   "))
            EndMission();
        ImGui::SameLine(0.0f, 12.0f);
        if (ImGui::Button("Look round"))
        {
            s.endClosed = true;
            if (!s.active && GEngine)
            {
                GEngine->SetMouseGrab(true);
                SDL_HideCursor();
            }
        }
    }
    ImGui::End();
}

void KeyCap(ImDrawList* draw, const char* key, const char* what)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 kt = Measure(Font().bold, 12.5f, key);
    const ImVec2 wt = Measure(Font().body, 13.0f, what);
    ImGui::Dummy(ImVec2(kt.x + 14.0f + 8.0f + wt.x, 24.0f));
    draw->AddRectFilled(ImVec2(at.x, at.y + 2.0f), ImVec2(at.x + kt.x + 14.0f, at.y + 22.0f), IM_COL32(255, 255, 255, 30),
                        5.0f);
    Put(draw, Font().bold, 12.5f, ImVec2(at.x + 7.0f, at.y + 12.0f - kt.y * 0.5f), ColText, key);
    Put(draw, Font().body, 13.0f, ImVec2(at.x + kt.x + 22.0f, at.y + 12.0f - wt.y * 0.5f), ColMuted, what);
}

void DrawCommandBar(const std::vector<SquadDraw>& squads)
{
    const ImGuiIO& io = ImGui::GetIO();
    State& s = St();
    if (BeginPanel("##conquest_bar", ImVec2(io.DisplaySize.x * 0.5f + 120.0f, io.DisplaySize.y - 14.0f), ImVec2(0.5f, 1.0f)))
    {
        if (!s.selected.empty())
        {
            std::string names;
            for (size_t i = 0; i < s.selected.size() && i < 4; i++)
                names += (i ? ", " : "") + CallsignFull(s.selected[i]);
            if (s.selected.size() > 4)
                names += " and " + std::to_string(s.selected.size() - 4) + " more";
            Label(Font().bold, 15.0f, ColSelect, names.c_str());
            ImGui::SameLine(0.0f, 14.0f);
            Label(Font().body, 13.0f, ColMuted, "Right click to order");
            if (PossessionAllowed())
            {
                ImGui::SameLine(0.0f, 14.0f);
                if (ImGui::Button(s.unit ? "Take control  T" : "Lead squad  T"))
                    TakeControl();
            }
            ImGui::SameLine(0.0f, 6.0f);
            if (ImGui::Button("Deselect"))
            {
                s.selected.clear();
                s.unit = nullptr;
            }
        }
        else
            Label(Font().body, 13.0f, ColMuted, "Click a squad, or drag a box.   H  controls");
        if (s.helpOpen)
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            KeyCap(draw, "LMB", "Select");
            ImGui::SameLine(0.0f, 12.0f);
            KeyCap(draw, "RMB", "Order");
            ImGui::SameLine(0.0f, 12.0f);
            KeyCap(draw, "WASD / Ctrl+drag", "Move");
            ImGui::SameLine(0.0f, 12.0f);
            KeyCap(draw, "Wheel", "Zoom");
            ImGui::SameLine(0.0f, 12.0f);
            KeyCap(draw, "Q E / R F", "Turn / tilt");
            ImGui::SameLine(0.0f, 12.0f);
            KeyCap(draw, "B", "Deploy");
            ImGui::SameLine(0.0f, 12.0f);
            KeyCap(draw, "Ctrl M", "Leave");
        }
    }
    ImGui::End();
    (void)squads;
}

// The minimap (bottom left, as in World in Conflict): the whole island north up, its objectives,
// your squads, the enemy you know of and what the camera sees. Left click or drag moves the view
// there; right click sends the selected squads.
constexpr int MiniCells = 72;
constexpr float MiniSize = 230.0f;

void PrepareMinimap()
{
    State& s = St();
    if (s.miniCells == MiniCells || !GLandscape)
        return;
    s.miniCells = MiniCells;
    s.miniColours.assign(static_cast<size_t>(MiniCells) * MiniCells, 0);
    const float size = WorldSize(), cell = size / static_cast<float>(MiniCells);
    for (int j = 0; j < MiniCells; j++)
        for (int i = 0; i < MiniCells; i++)
        {
            const float x = (static_cast<float>(i) + 0.5f) * cell, y = (static_cast<float>(j) + 0.5f) * cell;
            const float h = GLandscape->SurfaceY(x, y) - GLandscape->GetSeaLevel();
            const float slope = std::clamp((GLandscape->SurfaceY(x + cell * 0.5f, y) - GLandscape->SurfaceY(x - cell * 0.5f, y)) /
                                               (cell * 0.3f),
                                           -1.0f, 1.0f);
            ImU32 c;
            if (h < 0.0f)
            {
                const float deep = std::clamp(-h / 40.0f, 0.0f, 1.0f);
                c = IM_COL32(static_cast<int>(36 - 18 * deep), static_cast<int>(70 - 30 * deep), static_cast<int>(104 - 30 * deep), 255);
            }
            else
            {
                const float t = std::clamp(h / 220.0f, 0.0f, 1.0f), light = 1.0f + 0.25f * slope;
                const auto mix = [&](float a, float b) { return static_cast<int>(std::clamp((a + (b - a) * t) * light, 0.0f, 255.0f)); };
                c = IM_COL32(mix(58, 132), mix(82, 120), mix(52, 92), 255);
            }
            s.miniColours[static_cast<size_t>(MiniCells - 1 - j) * MiniCells + i] = c; // north up
        }
}

void DrawMinimap(const Snapshot& snap, const std::vector<SquadDraw>& squads)
{
    State& s = St();
    PrepareMinimap();
    if (s.miniCells == 0)
        return;
    const ImGuiIO& io = ImGui::GetIO();
    const float size = WorldSize();
    ImGui::SetNextWindowPos(ImVec2(14.0f, io.DisplaySize.y - 14.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool open = ImGui::Begin("##conquest_minimap", nullptr,
                                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(2);
    if (!open)
    {
        ImGui::End();
        return;
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("map", ImVec2(MiniSize, MiniSize), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const auto toMap = [&](float x, float y) { return ImVec2(at.x + x / size * MiniSize, at.y + (1.0f - y / size) * MiniSize); };
    const auto toWorld = [&](ImVec2 m, float& x, float& y)
    {
        x = std::clamp((m.x - at.x) / MiniSize, 0.0f, 1.0f) * size;
        y = std::clamp(1.0f - (m.y - at.y) / MiniSize, 0.0f, 1.0f) * size;
    };
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        float x, y;
        toWorld(io.MousePos, x, y);
        FocusOn(x, y);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !s.selected.empty())
    {
        float x, y;
        toWorld(io.MousePos, x, y);
        if (const int n = OrderGroups(s.selected, x, y))
        {
            s.flashes.push_back({x, y, SDL_GetTicks()});
            SetStatus("Moving to the point on the map  -  " + std::to_string(n) + (n == 1 ? " squad" : " squads"));
        }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float cellPx = MiniSize / static_cast<float>(s.miniCells);
    for (int j = 0; j < s.miniCells; j++)
        for (int i = 0; i < s.miniCells; i++)
        {
            const ImVec2 a(at.x + i * cellPx, at.y + j * cellPx);
            draw->AddRectFilled(a, ImVec2(a.x + cellPx + 0.5f, a.y + cellPx + 0.5f), s.miniColours[static_cast<size_t>(j) * s.miniCells + i]);
        }
    // Shroud: what nobody of yours has seen.
    if (GetOptions().fog == FogMode::Shroud)
    {
        const float worldCell = size / static_cast<float>(s.miniCells);
        for (int j = 0; j < s.miniCells; j++)
            for (int i = 0; i < s.miniCells; i++)
                if (!Explored((static_cast<float>(i) + 0.5f) * worldCell, size - (static_cast<float>(j) + 0.5f) * worldCell))
                {
                    const ImVec2 a(at.x + i * cellPx, at.y + j * cellPx);
                    draw->AddRectFilled(a, ImVec2(a.x + cellPx + 0.5f, a.y + cellPx + 0.5f), IM_COL32(4, 6, 9, 235));
                }
    }
    // What the camera looks at: the ground under the screen's corners.
    {
        ImVec2 corners[4];
        int n = 0;
        const ImVec2 screen[4] = {ImVec2(0.0f, io.DisplaySize.y * 0.12f), ImVec2(io.DisplaySize.x, io.DisplaySize.y * 0.12f),
                                  ImVec2(io.DisplaySize.x, io.DisplaySize.y), ImVec2(0.0f, io.DisplaySize.y)};
        for (const ImVec2& c : screen)
        {
            float x, y;
            if (GroundAt(c.x, c.y, x, y))
                corners[n++] = toMap(x, y);
        }
        if (n == 4)
            draw->AddQuad(corners[0], corners[1], corners[2], corners[3], IM_COL32(255, 255, 255, 200), 1.2f);
        else
        {
            const ImVec2 c = toMap(s.focusX, s.focusY);
            draw->AddCircle(c, 6.0f, IM_COL32(255, 255, 255, 200), 12, 1.2f);
        }
    }
    for (size_t o = 0; o < snap.war.defs.size(); o++)
    {
        const ObjectiveDef& d = snap.war.defs[o];
        const ObjectiveState& st = snap.war.states[o];
        const ImVec2 c = toMap(d.x, d.y);
        const float r = d.kind == ObjectiveKind::HQ ? 6.0f : 4.5f;
        draw->AddCircleFilled(c, r, SideColour(st.owner, 0.95f));
        draw->AddCircle(c, r + 1.5f, st.contested ? ColWarn : IM_COL32(255, 255, 255, 180), 16, 1.2f);
    }
    const int me = PlayerSide();
    int enemy = NoSide;
    for (int side = 0; side < snap.war.sideCount; side++)
        if (side != me)
            enemy = side;
    for (const Contact& c : KnownEnemies(me))
    {
        const ImVec2 p = toMap(c.x, c.y);
        draw->AddRectFilled(ImVec2(p.x - 1.5f, p.y - 1.5f), ImVec2(p.x + 1.5f, p.y + 1.5f),
                            SideColour(enemy, 1.0f - std::clamp(c.age / 120.0f, 0.0f, 0.7f)));
    }
    for (const SquadDraw& d : squads)
    {
        const ImVec2 p = toMap(d.worldX, d.worldY);
        const bool sel = IsSelected(d.group.id);
        draw->AddCircleFilled(p, sel ? 3.2f : 2.4f, SquadShade(d.group.side, d.group.id));
        if (sel)
            draw->AddCircle(p, 5.0f, ColSelect, 12, 1.2f);
    }
    ImGui::End();
}
} // namespace

bool Active()
{
    return St().active;
}

void SetActive(bool active)
{
    State& s = St();
    if (active == s.active)
        return;
    if (active)
    {
        if (!GWorld || !GLandscape || !GEngine || PlayerSide() == NoSide || !GetOptions().commanderView)
            return;
        // Back from a soldier the player had taken over: that soldier returns to the AI.
        if (Possessing())
            ReturnToCommander();
        LoadFonts();
        s.possessionToggle = PossessionAllowed();
        InitialFocus();
        auto* camera = new CameraVehicle();
        s.camera = camera;
        camera->SetManual(false);
        camera->SetCrossHairs(false);
        GWorld->AddAnimal(camera);
        ApplyCamera();
        GWorld->SetCameraEffect(CreateCameraEffect(camera, "Internal", CamEffectTop, true));
        s.prevCinema = showCinemaBorder;
        ShowCinemaBorder(false);
        // The OS pointer, free: the HUD's buttons and the box selection need a real cursor.
        s.prevGrab = GEngine->IsMouseGrabbed();
        GEngine->SetMouseGrab(false);
        SDL_ShowCursor();
        s.lastTicks = SDL_GetTicks();
        s.active = true;
        SetHumanCommander(true);
        SetStatus("You have command. Select squads with the left mouse button, order them with the right.");
        LOG_INFO(Mission, "Conquest: commander view on");
        return;
    }
    if (GWorld)
        GWorld->SetCameraEffect(nullptr);
    ShowCinemaBorder(s.prevCinema);
    if (CameraVehicle* cam = s.camera)
        cam->SetDelete();
    s.camera = nullptr;
    if (GEngine)
        GEngine->SetMouseGrab(true);
    SDL_HideCursor();
    s.selected.clear();
    s.unit = nullptr;
    s.follow = nullptr;
    s.boxDrag = s.rotating = s.dragPan = s.targeting = false;
    s.optionsOpen = false;
    s.active = false;
    SetHumanCommander(false);
    LOG_INFO(Mission, "Conquest: commander view off");
}

bool ProcessEvent(const SDL_Event& event)
{
    State& s = St();
    s.consumeMouse = s.consumeKeyboard = false;
    // The HUD's typefaces, loaded between frames (here) rather than inside one.
    if (!Font().tried && PlayerSide() != NoSide)
        LoadFonts();
    // The setup screen: the mouse is the screen's, the keyboard too (Esc still opens the menu).
    if (SetupPending())
    {
        if (!Font().tried)
            LoadFonts();
        if (MenuOpen())
            return false;
        if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
            event.key.scancode == SDL_SCANCODE_ESCAPE)
            return false;
        switch (event.type)
        {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                s.consumeKeyboard = true;
                return true;
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_WHEEL:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                s.consumeMouse = true;
                return true;
            default:
                return false;
        }
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_M &&
        (event.key.mod & SDL_KMOD_CTRL) != 0)
    {
        if (s.active || (PlayerSide() != NoSide && GetOptions().commanderView))
        {
            std::string why;
            if (!s.active && !CanTakeCommand(&why))
                Notify(why); // one commander per side
            else
                SetActive(!s.active);
            s.consumeKeyboard = true;
            return true;
        }
        return false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && (event.key.mod & SDL_KMOD_CTRL) != 0 &&
        PlayerSide() != NoSide && Seat().multiplayer && !MenuOpen() &&
        (event.key.scancode == SDL_SCANCODE_V || event.key.scancode == SDL_SCANCODE_Y || event.key.scancode == SDL_SCANCODE_N))
    {
        if (event.key.scancode == SDL_SCANCODE_V)
        {
            std::string why;
            Notify(CallVote(&why) ? "You called a vote to replace the commander." : "No vote: " + why);
        }
        else if (CastVote(event.key.scancode == SDL_SCANCODE_Y))
            Notify(event.key.scancode == SDL_SCANCODE_Y ? "You voted to remove the commander." : "You voted to keep the commander.");
        s.consumeKeyboard = true;
        return true;
    }
    if (!s.active)
        return false;
    // The Esc menu (and mission dialogs) keep working over the commander view.
    if (MenuOpen())
        return false;
    const bool overHud = ImGui::GetIO().WantCaptureMouse;
    switch (event.type)
    {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (event.key.scancode == SDL_SCANCODE_ESCAPE)
            {
                if (s.targeting && event.type == SDL_EVENT_KEY_DOWN)
                {
                    s.targeting = false; // Esc cancels the barrage before it opens the menu
                    s.consumeKeyboard = true;
                    return true;
                }
                return false;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_T &&
                PossessionAllowed() && !s.selected.empty())
                TakeControl();
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_B)
                s.deployOpen = !s.deployOpen;
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_H)
                s.helpOpen = !s.helpOpen;
            s.consumeKeyboard = true;
            return true;
        case SDL_EVENT_MOUSE_MOTION:
            s.consumeMouse = true;
            if (s.rotating)
            {
                s.yaw += event.motion.xrel * 0.006f;
                s.pitch = std::clamp(s.pitch - event.motion.yrel * 0.25f, MinPitch, MaxPitch);
            }
            if (s.dragPan)
            {
                // The ground follows the pointer: drag forward and the view moves forward.
                const float k = MetresPerPixel();
                const float ahead = event.motion.yrel * k, aside = -event.motion.xrel * k;
                s.focusX += std::sin(s.yaw) * ahead + std::cos(s.yaw) * aside;
                s.focusY += std::cos(s.yaw) * ahead - std::sin(s.yaw) * aside;
                s.follow = nullptr;
            }
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            s.consumeMouse = true;
            if (!overHud && event.wheel.y != 0.0f)
            {
                const float steps = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
                EntityAI* followed = s.follow;
                const float floor = followed ? std::max(FollowMinDistance, followed->GetRadius() * 1.1f) : MinDistance;
                s.distance = std::clamp(s.distance * std::pow(0.85f, steps), floor, MaxDistance);
            }
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            s.consumeMouse = true;
            if (overHud)
                return true;
            if (event.button.button == SDL_BUTTON_LEFT)
            {
                if (s.targeting)
                    FireBarrageAt(event.button.x, event.button.y);
                else if ((SDL_GetModState() & SDL_KMOD_CTRL) != 0)
                    s.dragPan = true;
                else
                {
                    s.boxDrag = true;
                    s.boxX0 = event.button.x;
                    s.boxY0 = event.button.y;
                }
            }
            else if (event.button.button == SDL_BUTTON_MIDDLE)
            {
                // Ctrl + middle drag moves the view like Ctrl + left drag; the middle button alone turns and tilts it.
                if ((SDL_GetModState() & SDL_KMOD_CTRL) != 0)
                    s.dragPan = true;
                else
                    s.rotating = true;
            }
            else if (event.button.button == SDL_BUTTON_RIGHT)
            {
                if (s.targeting)
                {
                    s.targeting = false;
                    SetStatus("Barrage cancelled.", ColMuted);
                }
                else
                    OrderAt(event.button.x, event.button.y);
            }
            return true;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            s.consumeMouse = true;
            if (event.button.button == SDL_BUTTON_LEFT && s.dragPan)
                s.dragPan = false;
            else if (event.button.button == SDL_BUTTON_LEFT && s.boxDrag)
            {
                const bool add = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                if (std::abs(event.button.x - s.boxX0) < 6.0f && std::abs(event.button.y - s.boxY0) < 6.0f)
                    ClickSelect(event.button.x, event.button.y, add);
                else
                    BoxSelect(s.boxX0, s.boxY0, event.button.x, event.button.y, add);
                s.boxDrag = false;
            }
            else if (event.button.button == SDL_BUTTON_MIDDLE)
            {
                s.rotating = false;
                s.dragPan = false;
            }
            return true;
        default:
            break;
    }
    return false;
}

void PrepareFonts()
{
    LoadFonts();
}

bool PlaceCamera(float x, float y, float distance, float pitch, float yawDegrees)
{
    State& s = St();
    if (!s.active)
        return false;
    FocusOn(x, y);
    s.distance = std::clamp(distance, MinDistance, MaxDistance);
    s.pitch = std::clamp(pitch, MinPitch, MaxPitch);
    s.yaw = yawDegrees * Pi / 180.0f;
    return true;
}

bool FollowSquad(int groupId, float distance, float pitch)
{
    State& s = St();
    if (!s.active)
        return false;
    bool found = false;
    ForEachGroup(
        [&](const LiveGroup& g)
        {
            if (g.id != groupId || found || !g.group->Leader())
                return;
            found = true;
            s.selected = {groupId};
            s.unit = g.group->Leader()->GetPerson();
            AIUnit* leader = g.group->Leader();
            s.follow = leader->IsFreeSoldier() ? static_cast<EntityAI*>(leader->GetPerson()) : leader->GetVehicle();
        });
    s.distance = std::clamp(distance, FollowMinDistance, MaxDistance);
    s.pitch = std::clamp(pitch, MinPitch, MaxPitch);
    return found;
}

bool WantsMouse()
{
    return (St().active || SetupPending()) && St().consumeMouse;
}

bool WantsKeyboard()
{
    return St().consumeKeyboard;
}

void Frame()
{
    State& s = St();
    // The setup screen comes first: nothing of the war exists before its Start.
    if (SetupPending())
    {
        if (!Font().tried || MenuOpen())
            return;
        if (!s.setupGrab && GEngine)
        {
            GEngine->SetMouseGrab(false);
            SDL_ShowCursor();
            s.setupGrab = true;
        }
        PushTheme();
        DrawSetup();
        DrawVoicePanel();
        PopTheme();
        return;
    }
    // Voted out, or another player holds command: the view closes and says why.
    if (std::string lost; TakeCommandLoss(lost))
    {
        if (s.active)
            SetActive(false);
        Notify(lost);
    }
    // The war began in command, or the soldier the commander had taken over died: the view opens.
    if (std::string notice; TakeViewRequest(notice) && !s.active)
    {
        SetActive(true);
        if (!notice.empty())
            SetStatus(notice, notice.find("killed") != std::string::npos ? ColWarn : ColGood);
    }
    if (!s.active)
    {
        // The soldier's HUD: the war bar, HQ's order, the capture bar at a post, banners.
        const Snapshot& snap = GetSnapshot();
        if (snap.active && PlayerSide() != NoSide && Font().tried && !MenuOpen())
        {
            CatchBanners(snap);
            DrawSoldierHud(snap);
            DrawCommanderOnGround();
            PushTheme();
            DrawVote();
            DrawEnd(snap);
            PopTheme();
        }
        return;
    }
    if (PlayerSide() == NoSide || !GLandscape || !s.camera)
    {
        SetActive(false);
        return;
    }
    const Uint64 now = SDL_GetTicks();
    const float dt = std::clamp(static_cast<float>(now - s.lastTicks) / 1000.0f, 0.0f, 0.1f);
    s.lastTicks = now;
    if (!MenuOpen())
        Pan(dt);
    ApplyCamera();
    FollowTrace();

    // Forget squads (and a picked soldier) that no longer exist.
    std::vector<SquadDraw> squads = CollectSquads();
    s.selected.erase(std::remove_if(s.selected.begin(), s.selected.end(),
                                    [&](int id)
                                    {
                                        return std::none_of(squads.begin(), squads.end(),
                                                            [id](const SquadDraw& d) { return d.group.id == id; });
                                    }),
                     s.selected.end());
    if (Person* picked = s.unit; picked && (!picked->Brain() || picked->Brain()->GetLifeState() != AIUnit::LSAlive))
        s.unit = nullptr;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    DrawShroud(draw);
    DrawObjectives(draw);
    DrawContacts(draw);
    DrawSquads(draw, squads);
    DrawFlashes(draw);
    DrawBox(ImGui::GetForegroundDrawList());

    const Snapshot& snap = GetSnapshot();
    if (!snap.active)
        return;
    CatchBanners(snap);
    PushTheme();
    DrawTopBar(snap);
    DrawSquadList(snap, squads);
    DrawDeploy(snap);
    DrawCommandBar(squads);
    DrawMinimap(snap, squads);
    DrawOptions();
    DrawVoicePanel();
    DrawToast();
    DrawVote();
    DrawEnd(snap);
    PopTheme();
}
} // namespace Poseidon::Conquest::View

// conquestCommanderView true|false: open or close the commander view from a script (missions,
// the soak harness). The same as Ctrl+M.
static GameValue ConquestCommanderViewUnary(const GameState*, GameValuePar oper1)
{
    Poseidon::Conquest::View::SetActive(static_cast<bool>(oper1));
    return GameValue(Poseidon::Conquest::View::Active());
}

// conquestCamera [x, y, distance, pitch, yaw] or [groupId, distance, pitch]: place the commander
// view's camera, or follow a squad's leader (captures, the feature test).
static GameValue ConquestCameraUnary(const GameState*, GameValuePar oper1)
{
    namespace V = Poseidon::Conquest::View;
    const GameArrayType& a = oper1;
    if (!V::Active() || a.Size() < 3)
        return GameValue(false);
    if (a.Size() >= 5)
        return GameValue(V::PlaceCamera(static_cast<float>(a[0]), static_cast<float>(a[1]), static_cast<float>(a[2]),
                                        static_cast<float>(a[3]), static_cast<float>(a[4])));
    return GameValue(V::FollowSquad(static_cast<int>(static_cast<float>(a[0])), static_cast<float>(a[1]),
                                    static_cast<float>(a[2])));
}

INIT_MODULE(ConquestView, 3)
{
    GGameState.NewFunction(GameFunction(GameBool, "conquestCamera", ConquestCameraUnary, GameArray));
    GGameState.NewFunction(GameFunction(GameBool, "conquestCommanderView", ConquestCommanderViewUnary, GameBool));
};
