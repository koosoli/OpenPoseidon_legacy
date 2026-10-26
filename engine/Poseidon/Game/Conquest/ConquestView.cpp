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

#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Modules/Modules.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
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
#include <filesystem>
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
constexpr float MinDistance = 40.0f;
constexpr float MaxDistance = 3000.0f;

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

const char* Callsign(int id)
{
    static const char* names[] = {"Alpha",  "Bravo",   "Charlie", "Delta",  "Echo",   "Foxtrot", "Golf",
                                  "Hotel",  "India",   "Juliet",  "Kilo",   "Lima",   "Mike",    "November",
                                  "Oscar",  "Papa",    "Quebec",  "Romeo",  "Sierra", "Tango",   "Uniform",
                                  "Victor", "Whiskey", "X-ray",   "Yankee", "Zulu"};
    return names[(id - 1) % 26];
}

std::string CallsignFull(int id)
{
    const int lap = (id - 1) / 26;
    return lap > 0 ? std::string(Callsign(id)) + " " + std::to_string(lap + 1) : std::string(Callsign(id));
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
    std::vector<ScreenPoint> vehicles;
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

void ApplyCamera()
{
    State& s = St();
    CameraVehicle* cam = s.camera;
    if (!cam || !GLandscape)
        return;
    const float size = WorldSize();
    s.focusX = std::clamp(s.focusX, 0.0f, size);
    s.focusY = std::clamp(s.focusY, 0.0f, size);
    const float p = s.pitch * Pi / 180.0f;
    const Vector3 forward(std::sin(s.yaw), 0.0f, std::cos(s.yaw));
    const Vector3 focus(s.focusX, Ground(s.focusX, s.focusY), s.focusY);
    Vector3 pos = focus - forward * (std::cos(p) * s.distance) + Vector3(0.0f, std::sin(p) * s.distance, 0.0f);
    const float floor = Ground(pos.X(), pos.Z()) + 15.0f;
    if (pos.Y() < floor)
        pos[1] = floor;
    cam->SetPosition(pos);
    cam->SetDirectionAndUp((focus - pos).Normalized(), VUp);
    cam->ResetTargets();
}

void FocusOn(float x, float y)
{
    St().focusX = x;
    St().focusY = y;
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

void Pan(float dt)
{
    State& s = St();
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (!keys)
        return;
    float ahead = 0.0f, aside = 0.0f, turn = 0.0f;
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
    // The screen edge pans too, as in any strategy game (not while over a panel).
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const float edge = 6.0f;
    if (!io.WantCaptureMouse && !s.boxDrag && mouse.x >= 0.0f && mouse.y >= 0.0f && mouse.x <= io.DisplaySize.x &&
        mouse.y <= io.DisplaySize.y)
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
    const float speed = s.distance * 1.1f * ((keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) ? 2.5f : 1.0f);
    s.focusX += (std::sin(s.yaw) * ahead + std::cos(s.yaw) * aside) * speed * dt;
    s.focusY += (std::cos(s.yaw) * ahead - std::sin(s.yaw) * aside) * speed * dt;
    s.yaw += turn * 1.6f * dt;
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
                ScreenPoint p;
                if (Project(unit->Position() + Vector3(0.0f, 1.0f, 0.0f), p))
                    d.members.push_back(p);
            }
            for (int v = 0; v < g.group->NVehicles(); v++)
            {
                const Transport* veh = g.group->GetVehicle(v);
                ScreenPoint p;
                if (veh && !veh->IsDammageDestroyed() && Project(veh->Position() + Vector3(0.0f, 1.5f, 0.0f), p))
                    d.vehicles.push_back(p);
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

void ClickSelect(float x, float y, bool add)
{
    int best = 0;
    float bestD2 = PickPixels * PickPixels;
    for (const SquadDraw& d : CollectSquads())
    {
        auto consider = [&](const ScreenPoint& p)
        {
            const float d2 = (p.x - x) * (p.x - x) + (p.y - y) * (p.y - y);
            if (d2 < bestD2)
            {
                bestD2 = d2;
                best = d.group.id;
            }
        };
        for (const ScreenPoint& p : d.members)
            consider(p);
        for (const ScreenPoint& p : d.vehicles)
            consider(p);
    }
    if (!add)
        St().selected.clear();
    if (best)
    {
        if (add)
            ToggleSelected(best);
        else
            St().selected.push_back(best);
    }
}

void BoxSelect(float x0, float y0, float x1, float y1, bool add)
{
    const float left = std::min(x0, x1), right = std::max(x0, x1);
    const float top = std::min(y0, y1), bottom = std::max(y0, y1);
    if (!add)
        St().selected.clear();
    for (const SquadDraw& d : CollectSquads())
        for (const ScreenPoint& p : d.members)
            if (p.x >= left && p.x <= right && p.y >= top && p.y <= bottom)
            {
                if (!IsSelected(d.group.id))
                    St().selected.push_back(d.group.id);
                break;
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

void TakeControl()
{
    State& s = St();
    if (s.selected.empty())
        return;
    const int id = s.selected.front();
    std::string why;
    if (!Possess(id, &why))
    {
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

ImU32 SideColour(int side, float alpha = 1.0f)
{
    const Snapshot& snap = GetSnapshot();
    ImU32 c = IM_COL32(190, 196, 205, 255);
    if (side >= 0 && side < static_cast<int>(snap.sideNames.size()))
    {
        const std::string& n = snap.sideNames[side];
        c = n == "WEST" ? IM_COL32(74, 152, 255, 255) : n == "EAST" ? IM_COL32(236, 76, 64, 255) : IM_COL32(86, 196, 98, 255);
    }
    return WithAlpha(c, alpha);
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

void DrawObjectives(ImDrawList* draw)
{
    const Snapshot& snap = GetSnapshot();
    const Fonts& f = Font();
    for (size_t o = 0; o < snap.war.defs.size(); o++)
    {
        const ObjectiveDef& d = snap.war.defs[o];
        const ObjectiveState& st = snap.war.states[o];
        const ImU32 ring = SideColour(st.owner, st.contested ? 0.45f : 0.75f);
        ScreenPoint prev;
        bool prevOk = false;
        for (int i = 0; i <= 72; i++)
        {
            const float a = static_cast<float>(i) * (2.0f * Pi / 72.0f);
            const float x = d.x + std::cos(a) * d.radius, y = d.y + std::sin(a) * d.radius;
            ScreenPoint p;
            const bool ok = Project(Vector3(x, Ground(x, y) + 0.5f, y), p);
            if (ok && prevOk)
                draw->AddLine(ImVec2(prev.x, prev.y), ImVec2(p.x, p.y), ring, 2.5f);
            prev = p;
            prevOk = ok;
        }
        ScreenPoint c;
        if (!Project(Vector3(d.x, Ground(d.x, d.y) + 25.0f, d.y), c))
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

void DrawSquads(ImDrawList* draw, const std::vector<SquadDraw>& squads)
{
    const Snapshot& snap = GetSnapshot();
    const Fonts& f = Font();
    for (const SquadDraw& d : squads)
    {
        const bool sel = IsSelected(d.group.id);
        const ImU32 colour = SideColour(d.group.side);
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
                                  ImVec2(d.leader.x + dx * t1 / len, d.leader.y + dy * t1 / len), WithAlpha(ColSelect, 0.7f),
                                  2.0f);
                }
                draw->AddCircle(ImVec2(dest.x, dest.y), 7.0f, ColSelect, 16, 2.0f);
            }
        }
        for (const ScreenPoint& p : d.vehicles)
        {
            const ImVec2 a(p.x - 9.0f, p.y - 6.0f), b(p.x + 9.0f, p.y + 6.0f);
            draw->AddRectFilled(a, b, WithAlpha(colour, 0.35f), 3.0f);
            draw->AddRect(a, b, sel ? ColSelect : colour, 3.0f, 0, 2.0f);
        }
        for (const ScreenPoint& p : d.members)
        {
            if (sel)
                draw->AddCircle(ImVec2(p.x, p.y), 8.0f, ColSelect, 16, 2.0f);
            draw->AddCircleFilled(ImVec2(p.x, p.y), 4.0f, colour);
            draw->AddCircle(ImVec2(p.x, p.y), 4.5f, IM_COL32(255, 255, 255, 170), 12, 1.0f);
        }
        if (!d.leaderVisible)
            continue;
        const std::string name = CallsignFull(d.group.id) + "  " + std::to_string(d.men);
        const ImVec2 at(d.leader.x, d.leader.y - 16.0f);
        Pill(draw, at, f.bold, 13.0f, name.c_str(), sel ? IM_COL32(18, 60, 72, 235) : IM_COL32(12, 16, 22, 200),
             sel ? ColSelect : ColText);
        if (sel)
        {
            std::string buffer;
            const char* task = Task(d.group, snap, buffer);
            const ImVec2 t = Measure(f.body, 13.0f, task);
            Put(draw, f.body, 13.0f, ImVec2(at.x - t.x * 0.5f, at.y - 26.0f), ColText, task, true);
        }
    }
}

void DrawContacts(ImDrawList* draw)
{
    const int me = PlayerSide();
    int enemy = NoSide;
    const Snapshot& snap = GetSnapshot();
    for (int s = 0; s < snap.war.sideCount; s++)
        if (s != me)
            enemy = s;
    for (const Contact& c : KnownEnemies(me))
    {
        ScreenPoint p;
        if (!Project(Vector3(c.x, c.height + 1.0f, c.y), p))
            continue;
        // Older knowledge fades: this is what the side believes, not where the enemy is.
        const float fresh = 1.0f - std::clamp(c.age / 120.0f, 0.0f, 0.75f);
        const float r = c.vehicle ? 9.0f : 6.0f;
        draw->AddQuadFilled(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y),
                            SideColour(enemy, 0.85f * fresh));
        draw->AddQuad(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y),
                      IM_COL32(255, 255, 255, static_cast<int>(160 * fresh)), 1.0f);
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

    // Inside an objective: its capture bar, front and centre.
    Person* player = GWorld ? GWorld->GetRealPlayer() : nullptr;
    if (player)
    {
        const Vector3 p = player->Position();
        for (size_t o = 0; o < snap.war.defs.size(); o++)
        {
            const ObjectiveDef& d = snap.war.defs[o];
            if (std::hypot(p.X() - d.x, p.Z() - d.y) > d.radius)
                continue;
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

void DrawSquadList(const Snapshot& snap, const std::vector<SquadDraw>& squads)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (BeginPanel("##conquest_squads", ImVec2(14.0f, 96.0f), ImVec2(0.0f, 0.0f),
                   ImVec2(330.0f, io.DisplaySize.y - 220.0f)))
    {
        char count[32];
        std::snprintf(count, sizeof(count), "%d", static_cast<int>(squads.size()));
        Heading("SQUADS", count);
        ImGui::Spacing();
        const float width = 296.0f;
        for (const SquadDraw& d : squads)
        {
            ImGui::PushID(d.group.id);
            const bool sel = IsSelected(d.group.id);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("card", ImVec2(width, 56.0f)))
            {
                if (io.KeyShift || io.KeyCtrl)
                    ToggleSelected(d.group.id);
                else
                    St().selected = {d.group.id};
            }
            const bool hovered = ImGui::IsItemHovered();
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                FocusOn(d.worldX, d.worldY);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 b(at.x + width, at.y + 56.0f);
            draw->AddRectFilled(at, b, sel ? WithAlpha(ColSelect, 0.16f) : hovered ? ColCardHover : ColCard, 10.0f);
            if (sel)
                draw->AddRect(at, b, WithAlpha(ColSelect, 0.8f), 10.0f, 0, 1.5f);
            draw->AddRectFilled(ImVec2(at.x, at.y + 8.0f), ImVec2(at.x + 4.0f, b.y - 8.0f), SideColour(d.group.side), 2.0f);

            const std::string name = CallsignFull(d.group.id);
            Put(draw, Font().bold, 16.0f, ImVec2(at.x + 16.0f, at.y + 7.0f), ColText, name.c_str());
            const std::string type = d.group.typeName.empty() ? std::string("Squad") : d.group.typeName;
            const float tw = Measure(Font().body, 13.0f, type.c_str()).x;
            Put(draw, Font().body, 13.0f, ImVec2(b.x - tw - 12.0f, at.y + 9.0f), ColMuted, type.c_str());

            std::string buffer;
            const char* task = Task(d.group, snap, buffer);
            const ImU32 taskColour = d.group.stalled ? ColWarn : d.group.playerLed ? ColSelect
                                                             : d.group.task == TaskKind::None ? ColMuted
                                                                                               : ColText;
            Put(draw, Font().body, 13.0f, ImVec2(at.x + 16.0f, at.y + 31.0f), taskColour, task);

            // Strength: one pip per soldier the squad started with.
            const int size = std::max(d.group.size, d.men);
            for (int i = 0; i < size; i++)
            {
                const ImVec2 p(b.x - 14.0f - static_cast<float>(size - 1 - i) * 9.0f, at.y + 39.0f);
                if (i < d.men)
                    draw->AddCircleFilled(p, 3.2f, SideColour(d.group.side));
                else
                    draw->AddCircle(p, 3.2f, IM_COL32(255, 255, 255, 60), 10, 1.0f);
            }
            if (d.group.locked && !d.group.playerLed)
            {
                const ImVec2 t = Measure(Font().bold, 11.0f, "YOUR ORDERS");
                const ImVec2 pa(b.x - tw - t.x - 34.0f, at.y + 9.0f);
                draw->AddRectFilled(pa, ImVec2(pa.x + t.x + 12.0f, pa.y + t.y + 4.0f), WithAlpha(ColSelect, 0.22f), 6.0f);
                Put(draw, Font().bold, 11.0f, ImVec2(pa.x + 6.0f, pa.y + 2.0f), ColSelect, "YOUR ORDERS");
            }
            ImGui::PopID();
        }
        if (squads.empty())
            Label(Font().body, 14.0f, ColMuted, "No squads. Deploy some on the right.");
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
        const float tileW = 156.0f, tileH = 74.0f;
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
            Put(draw, Font().bold, 15.0f, ImVec2(at.x + 12.0f, at.y + 9.0f), text, types[t].name.c_str());
            char line[64];
            std::snprintf(line, sizeof(line), "%d soldiers", types[t].size);
            Put(draw, Font().body, 12.5f, ImVec2(at.x + 12.0f, at.y + 32.0f), ColMuted, line);
            std::snprintf(line, sizeof(line), "%.0f", types[t].cost);
            const ImVec2 ct = Measure(Font().display, 18.0f, line);
            Put(draw, Font().display, 18.0f, ImVec2(b.x - ct.x - 12.0f, b.y - ct.y - 8.0f), can ? ColGood : ColBad, line);
            if (pressed && can)
            {
                std::string refused;
                const int id = BuySquad(s.deployBase, static_cast<int>(t), &refused);
                if (id)
                {
                    St().selected = {id};
                    SetStatus(types[t].name + " deployed at " + snap.war.defs[s.deployBase].name + "  -  " +
                              CallsignFull(id) + " awaits your orders");
                }
                else
                    SetStatus("Cannot deploy: " + refused, ColWarn);
            }
            if (hovered && !can)
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(("Not now: " + reason).c_str());
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Checkbox("Allow taking control of soldiers", &s.possessionToggle))
            SetPossessionAllowed(s.possessionToggle);
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
    if (BeginPanel("##conquest_bar", ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 14.0f), ImVec2(0.5f, 1.0f)))
    {
        if (!s.selected.empty())
        {
            std::string names;
            for (size_t i = 0; i < s.selected.size() && i < 4; i++)
                names += (i ? ", " : "") + CallsignFull(s.selected[i]);
            if (s.selected.size() > 4)
                names += " and " + std::to_string(s.selected.size() - 4) + " more";
            Label(Font().bold, 16.0f, ColSelect, names.c_str());
            ImGui::SameLine(0.0f, 18.0f);
            Label(Font().body, 14.0f, ColMuted, "Right click an objective, an enemy or the ground to give orders.");
            if (PossessionAllowed())
            {
                ImGui::SameLine(0.0f, 18.0f);
                if (ImGui::Button("Take control"))
                    TakeControl();
            }
            ImGui::SameLine(0.0f, 8.0f);
            if (ImGui::Button("Deselect"))
                s.selected.clear();
        }
        else
            Label(Font().body, 14.0f, ColMuted, "Click a squad to select it, or drag a box around several.");
        ImDrawList* draw = ImGui::GetWindowDrawList();
        KeyCap(draw, "LMB", "Select");
        ImGui::SameLine(0.0f, 16.0f);
        KeyCap(draw, "RMB", "Order");
        ImGui::SameLine(0.0f, 16.0f);
        KeyCap(draw, "WASD", "Move view");
        ImGui::SameLine(0.0f, 16.0f);
        KeyCap(draw, "Wheel", "Zoom");
        ImGui::SameLine(0.0f, 16.0f);
        KeyCap(draw, "Q E", "Turn");
        ImGui::SameLine(0.0f, 16.0f);
        KeyCap(draw, "Ctrl M", PossessionAllowed() ? "Leave command / back to command" : "Leave command");
    }
    ImGui::End();
    (void)squads;
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
        if (!GWorld || !GLandscape || !GEngine || PlayerSide() == NoSide)
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
    s.boxDrag = s.rotating = false;
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
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_M &&
        (event.key.mod & SDL_KMOD_CTRL) != 0)
    {
        if (s.active || PlayerSide() != NoSide)
        {
            SetActive(!s.active);
            s.consumeKeyboard = true;
            return true;
        }
        return false;
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
                return false;
            s.consumeKeyboard = true;
            return true;
        case SDL_EVENT_MOUSE_MOTION:
            s.consumeMouse = true;
            if (s.rotating)
            {
                s.yaw += event.motion.xrel * 0.006f;
                s.pitch = std::clamp(s.pitch - event.motion.yrel * 0.25f, 20.0f, 88.0f);
            }
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            s.consumeMouse = true;
            if (!overHud && event.wheel.y != 0.0f)
            {
                const float steps = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
                s.distance = std::clamp(s.distance * std::pow(0.87f, steps), MinDistance, MaxDistance);
            }
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            s.consumeMouse = true;
            if (overHud)
                return true;
            if (event.button.button == SDL_BUTTON_LEFT)
            {
                s.boxDrag = true;
                s.boxX0 = event.button.x;
                s.boxY0 = event.button.y;
            }
            else if (event.button.button == SDL_BUTTON_MIDDLE)
                s.rotating = true;
            else if (event.button.button == SDL_BUTTON_RIGHT)
                OrderAt(event.button.x, event.button.y);
            return true;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            s.consumeMouse = true;
            if (event.button.button == SDL_BUTTON_LEFT && s.boxDrag)
            {
                const bool add = (SDL_GetModState() & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL)) != 0;
                if (std::abs(event.button.x - s.boxX0) < 6.0f && std::abs(event.button.y - s.boxY0) < 6.0f)
                    ClickSelect(event.button.x, event.button.y, add);
                else
                    BoxSelect(s.boxX0, s.boxY0, event.button.x, event.button.y, add);
                s.boxDrag = false;
            }
            else if (event.button.button == SDL_BUTTON_MIDDLE)
                s.rotating = false;
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

bool WantsMouse()
{
    return St().active && St().consumeMouse;
}

bool WantsKeyboard()
{
    return St().consumeKeyboard;
}

void Frame()
{
    State& s = St();
    // The soldier the commander had taken over died: back to the commander view.
    if (TakeReopenViewRequest() && !s.active)
    {
        SetActive(true);
        SetStatus("The soldier you controlled was killed. You are back in command.", ColWarn);
    }
    if (!s.active)
    {
        // The soldier's HUD: the war bar, the capture bar in a zone, capture banners.
        const Snapshot& snap = GetSnapshot();
        if (snap.active && PlayerSide() != NoSide && Font().tried && !MenuOpen())
        {
            CatchBanners(snap);
            DrawSoldierHud(snap);
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

    // Forget squads that no longer exist.
    std::vector<SquadDraw> squads = CollectSquads();
    s.selected.erase(std::remove_if(s.selected.begin(), s.selected.end(),
                                    [&](int id)
                                    {
                                        return std::none_of(squads.begin(), squads.end(),
                                                            [id](const SquadDraw& d) { return d.group.id == id; });
                                    }),
                     s.selected.end());

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
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
    DrawToast();
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

INIT_MODULE(ConquestView, 3)
{
    GGameState.NewFunction(GameFunction(GameBool, "conquestCommanderView", ConquestCommanderViewUnary, GameBool));
};
