// Island Editor: a standalone editor for the islands (.wrp) of Cold War Assault /
// Operation Flashpoint (from Open Malprave, skinned for Open Poseidon).  Opens 4WVR, 3WVR and the binarized OPRW,
// shapes the land, paints ground textures, moves and places objects, and saves 4WVR,
// which those games load as it is.

#include "Builders.hpp"

// (a tree built for any x64 processor has no such check and needs none)
#if __has_include(<Poseidon/Foundation/Platform/CpuCheck.hpp>)
#include <Poseidon/Foundation/Platform/CpuCheck.hpp>
#define ISLAND_EDITOR_CPU_CHECK 1
#endif
#include "GameData.hpp"
#include "HeightMap.hpp"
#include "Island.hpp"
#include "Renderer.hpp"
#include "Reskin.hpp"

#include <PoseidonD3D12/Dx12Device.hpp>
#include <Poseidon/Foundation/Common/PlatformPaths.hpp>
#include <Poseidon/Graphics/Shared/PNGWriter.hpp>

#include <CLI/CLI.hpp>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#pragma push_macro("DebugLog")
#undef DebugLog
#include <imgui.h>
#include <imgui_freetype.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_sdl3.h>
#pragma pop_macro("DebugLog")

#include <shobjidl.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace island;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Windows file dialogs
// ---------------------------------------------------------------------------

namespace
{
std::string Narrow(const wchar_t* w)
{
    if (!w)
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), 0);
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// kind: 0 open file, 1 save file, 2 pick folder
std::string FileDialog(int kind, const wchar_t* title, const wchar_t* filterName, const wchar_t* filterSpec,
                       const wchar_t* defaultExt)
{
    std::string result;
    const bool inited = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IFileDialog* dlg = nullptr;
    const HRESULT hr = kind == 1 ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))
                                 : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr) && dlg)
    {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (kind == 2 ? FOS_PICKFOLDERS : 0));
        if (title)
            dlg->SetTitle(title);
        if (kind != 2 && filterName && filterSpec)
        {
            const COMDLG_FILTERSPEC spec[2] = {{filterName, filterSpec}, {L"All files", L"*.*"}};
            dlg->SetFileTypes(2, spec);
        }
        if (defaultExt)
            dlg->SetDefaultExtension(defaultExt);
        if (SUCCEEDED(dlg->Show(nullptr)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item)
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                {
                    result = Narrow(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (inited)
        CoUninitialize();
    return result;
}

const float kPi = 3.14159265f;

enum class Tool
{
    Raise,
    Lower,
    Smooth,
    Flatten,
    SetHeight,
    Noise,
    Erode,
    Ramp,
    Paint,
    AutoPaint,
    Road,
    Forest,
    Bridge,
    Region,
    Objects,
    Count
};

const char* const kToolNames[] = {"Raise", "Lower", "Smooth", "Flatten", "Set height",
                                  "Noise", "Erode", "Ramp",   "Paint",   "Auto paint",
                                  "Road",  "Forest", "Bridge", "Region",  "Objects"};
const char* const kToolHelp[] = {
    "Drag to lift the land.  Shift lowers, Ctrl smooths.",
    "Drag to dig.  Shift raises, Ctrl smooths.",
    "Drag to even out bumps (the game's map gets slow on rough land).",
    "Drag to level the land to the height where the stroke began.",
    "Drag to set the land to the height below.",
    "Drag to roughen the land with hills and hollows.",
    "Drag to let steep slopes slump, as weather would.",
    "Press at the start, release at the end: a straight incline between them.",
    "Drag to lay the chosen ground texture.  Alt+click picks one up from the land.",
    "Drag to texture the land by rule: beach, grass, hills, rock and peaks, with transitions.",
    "Click along the way the road goes; Enter or a double click lays it.  Backspace takes a point back, Esc drops the lot.",
    "Drag to plant forest by the cell.  Shift clears it.",
    "Click one bank, then the other.",
    "Drag over the cells to mark a region; then export it, or import one into it.",
    "Click to select, drag to move, R / Shift+R to turn, Del to remove.",
};


// ---------------------------------------------------------------------------
// interface: the pieces every panel is made of
// ---------------------------------------------------------------------------

float g_ui = 1.0f;         // interface scale: 1 at 96 dpi
ImFont* g_bold = nullptr;  // headings

// a size in pixels at this screen's scale
float S(float v)
{
    return std::floor(v * g_ui + 0.5f);
}

const float kLeftWidth = 264.0f, kRightWidth = 336.0f, kMargin = 10.0f, kStatusHeight = 30.0f;
// the highlight of the active tool and primary buttons: the Open Poseidon logo's azure, and its cyan highlight
const ImVec4 kAccent(0.118f, 0.608f, 1.0f, 1.0f), kAccentHover(0.271f, 0.769f, 1.0f, 1.0f);

// a remark in the dim colour, wrapped to the width there is
void Hint(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// the heading of a part of a panel
void Section(const char* title, bool first = false)
{
    if (!first)
        ImGui::Spacing();
    if (g_bold)
        ImGui::PushFont(g_bold, 0.0f);
    ImGui::SeparatorText(title);
    if (g_bold)
        ImGui::PopFont();
}

// the name of a setting at the left; what follows fills the rest of the row
void Row(const char* label, float labelWidth = 84.0f)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(S(labelWidth));
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// a button that stays lit while what it stands for is on
bool Toggle(const char* label, bool on, const ImVec2& size = ImVec2(0, 0))
{
    if (on)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    }
    const bool clicked = ImGui::Button(label, size);
    if (on)
        ImGui::PopStyleColor(2);
    return clicked;
}

// a dialog in the middle of the window, of one width, as tall as it needs
bool BeginDialog(const char* name, float width = 460.0f)
{
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(width), 0), ImVec2(S(width), FLT_MAX));
    if (!ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return false;
    ImGui::PushItemWidth(S(width) * 0.52f);
    return true;
}

void EndDialog()
{
    ImGui::PopItemWidth();
    ImGui::EndPopup();
}

// The tools as the panel shows them: by what they are for.
struct ToolGroup
{
    const char* title;
    int columns, count;
    Tool tools[8];
};
const ToolGroup kToolGroups[] = {
    {"Land", 2, 8, {Tool::Raise, Tool::Lower, Tool::Smooth, Tool::Flatten, Tool::SetHeight, Tool::Noise, Tool::Erode, Tool::Ramp}},
    {"Ground", 2, 2, {Tool::Paint, Tool::AutoPaint}},
    {"Build", 3, 3, {Tool::Road, Tool::Forest, Tool::Bridge}},
    {"Select", 2, 2, {Tool::Objects, Tool::Region}},
};

// a folder as the whole path, however it was given
std::string WholePath(const std::string& folder)
{
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::weakly_canonical(std::filesystem::path(folder), ec);
    return ec || p.empty() ? folder : p.string();
}

// the key of a tool: 1 .. 9 for the first nine, 0 for the objects
char ToolKey(Tool t)
{
    const int i = static_cast<int>(t);
    return t == Tool::Objects ? '0' : i < 9 ? static_cast<char>('1' + i) : 0;
}

// "data3d\\asf  7.0 m wide" as people would say it
std::string RoadLabel(const RoadFamily& f)
{
    const size_t slash = f.name.find_last_of('\\');
    const std::string folder = slash == std::string::npos ? std::string() : f.name.substr(0, slash);
    const std::string code = f.name.substr(slash == std::string::npos ? 0 : slash + 1, 3);
    // the stock roads are named in Czech: asfaltka, silnice, cesta, kostky
    const char* what = code == "asf" ? "Asphalt road" : code == "sil" ? "Main road" : code == "ces" ? "Dirt track" : code == "kos" ? "Cobbled road" : nullptr;
    char label[160];
    snprintf(label, sizeof(label), "%s %.1f m, %s", what ? what : code.c_str(), f.width, folder.c_str());
    return label;
}

std::string ForestLabel(const ForestKind& k)
{
    const size_t slash = k.name.find_last_of('\\');
    const std::string stem = k.name.substr(slash == std::string::npos ? 0 : slash + 1);
    if (stem == "les ctverec pruchozi")
        return "Everon and Kolgujev forest";
    if (stem == "les_su_ctver_pruhozi")
        return "Malden forest";
    if (stem == "les_nw_ctver_pruhozi")
        return "Nogova forest";
    if (stem == "les_nw_jehl")
        return "Nogova conifers";
    return k.name;
}

// ---------------------------------------------------------------------------
// the application
// ---------------------------------------------------------------------------

struct App
{
    Dx12::Device* device = nullptr;
    SDL_Window* window = nullptr;
    Island is;
    GameData game;
    Renderer renderer;
    island::Camera cam;
    ViewOptions view;

    Tool tool = Tool::Raise;
    Brush brush;
    float setHeight = 10.0f;
    float noiseScale = 600.0f;
    int paintSlot = 1;
    bool placing = false;
    std::string placeModel;
    float placeYaw = 0;
    int selected = -1;

    // the stroke in progress
    bool stroke = false;
    float strokeTarget = 0;
    unsigned strokeSeed = 1;
    bool rampActive = false;
    float rampStart[3] = {};
    bool dragging = false;
    float dragOffset[2] = {};

    int showTab = -1;   // a tab of the side panel to bring forward once
    bool hover = false; // the cursor is on the land
    float hit[3] = {};

    bool minimapDirty = true;
    double minimapTime = 0;
    bool running = true;
    std::string status;
    double statusTime = 0;
    std::string settingsFile;

    // dialogs
    enum class Ask
    {
        None,
        New,
        Open,
        Exit
    } pending = Ask::None;
    bool openNew = false, openGenerate = false, openScale = false, openAddTexture = false, openAbout = false;
    bool openImport = false, openKeys = false;
    float viewBarWidth = 0; // as last drawn
    char paletteFilter[64] = "";
    bool palettePlain = false;
    int newSizeIndex = 3;
    float newHeight = -20.0f;
    bool newGenerate = true;
    int genSeed = 1;
    float genPeak = 320.0f, genRough = 0.5f, genSea = 40.0f;
    float scaleFactor = 1.0f, offsetMetres = 0.0f;
    int smoothPasses = 1;
    char textureFilter[128] = "";
    char modelFilter[128] = "";
    bool modelsUsedOnly = true;
    std::vector<std::string> usedModels;
    size_t usedModelsFor = static_cast<size_t>(-1);
    std::string importFile;
    float importLow = 0, importHigh = 300;
    HeightImage heightImage; // the height map chosen for importing, or for a new island
    int importSmooth = 0;
    int newMode = 0; // 0 generated, 1 flat, 2 from a height map

    // texturing by rule
    bool showAuto = false;
    AutoTextureRules rules;
    int ruleSet = -1;
    AutoTextureResult lastAuto;
    bool autoRan = false;
    std::vector<std::string> shownTextures; // the table the atlas was built from

    // builders
    Roads roads;
    int roadFamily = 0;
    bool roadCaps = true, roadSnapped = false;
    float roadHeading = 0;
    std::vector<std::array<float, 2>> roadPath;
    std::vector<PlacedRoad> roadPreview;
    std::vector<ForestKind> forests;
    std::vector<BridgeKind> bridges;
    bool buildersFound = false;
    int forestKind = 0, bridgeKind = 0;
    bool forestCorners = true, forestFloor = true;
    bool bridgeStarted = false, bridgeLevelFromBank = true;
    float bridgeA[3] = {}, bridgeLevel = 10.0f;
    bool regionSet = false, regionDragging = false;
    int regionA[2] = {}, regionB[2] = {};
    Island regionSrc;
    RegionInfo regionInfo;
    ImportOptions importOpt;
    bool openRegionImport = false, regionAtOrigin = false;
    std::string regionFile;
    std::vector<OverlayLine> overlay;

    // reskins
    bool showReskin = false, reskinPreview = true, reskinFilterOn = false;
    ReskinOptions reskin;
    char reskinAddon[16] = "", reskinTitle[64] = "", reskinBase[32] = "";
    std::string reskinMessage;

    void Status(const std::string& s)
    {
        status = s;
        statusTime = ImGui::GetTime();
    }

    // ---- settings -----------------------------------------------------------
    void LoadSettings()
    {
        settingsFile = Poseidon::Foundation::getUserConfigDir("OpenPoseidonIslandEditor") + "/settings.ini";
        std::ifstream f(settingsFile);
        std::string line;
        while (std::getline(f, line))
        {
            if (line.rfind("game=", 0) == 0 && game.Folder().empty())
                MountGame(line.substr(5), false);
        }
    }

    void SaveSettings()
    {
        std::error_code ec;
        fs::create_directories(fs::path(settingsFile).parent_path(), ec);
        std::ofstream f(settingsFile, std::ios::trunc);
        f << "game=" << game.Folder() << "\n";
    }

    bool MountGame(const std::string& folder, bool report)
    {
        if (folder.empty())
            return false;
        const bool ok = game.Mount(folder);
        if (report)
            Status(ok ? "Game folder: " + folder + " (" + std::to_string(game.ArchiveCount()) + " archives)"
                      : "No game archives (DTA, AddOns) found in " + folder);
        if (ok && is.size > 0)
        {
            renderer.SetIsland(&is, &game);
            shownTextures = is.textures;
        }
        ruleSet = -1;
        buildersFound = false;
        return ok;
    }

    // ---- documents ----------------------------------------------------------
    void AfterLoad()
    {
        renderer.SetIsland(&is, &game);
        shownTextures = is.textures;
        ruleSet = -1;
        buildersFound = false;
        roadPath.clear();
        roadPreview.clear();
        bridgeStarted = regionSet = regionDragging = false;
        selected = -1;
        stroke = rampActive = dragging = false;
        minimapDirty = true;
        usedModelsFor = static_cast<size_t>(-1);
        ResetCamera();
        // the first land texture in use is a sensible one to paint with
        paintSlot = 1;
        for (int i = 1; i < kMaxTextures; i++)
            if (!is.textures[i].empty())
            {
                paintSlot = i;
                break;
            }
        UpdateTitle();
    }

    void ResetCamera()
    {
        const float s = is.WorldSize();
        cam.pos[0] = s * 0.5f;
        cam.pos[2] = s * 0.5f - s * 0.55f;
        cam.pos[1] = std::max(is.MaxHeight(), 0.0f) + s * 0.35f;
        cam.yaw = 0;
        cam.pitch = -0.62f;
    }

    void UpdateTitle()
    {
        std::string t = "Open Poseidon Engine - Island Editor";
        if (is.size > 0)
            t += " - " + (is.path.empty() ? std::string("new island") : fs::path(is.path).filename().string()) +
                 (is.modified ? " *" : "");
        SDL_SetWindowTitle(window, t.c_str());
    }

    bool Open(const std::string& file)
    {
        std::string error;
        Island loaded;
        if (!GameData::LoadIsland(file, loaded, error))
        {
            Status("Cannot open " + file + ": " + error);
            return false;
        }
        is = std::move(loaded);
        // an island beside a game folder brings its textures with it
        if (game.Folder().empty())
        {
            const fs::path parent = fs::path(file).parent_path().parent_path();
            if (fs::is_directory(parent / "DTA") || fs::is_directory(parent / "AddOns"))
                MountGame(parent.string(), false);
        }
        AfterLoad();
        Status("Opened " + file + " (" + is.sourceFormat + ", " + std::to_string(is.size) + " x " + std::to_string(is.size) +
               " cells, " + std::to_string(is.objects.size()) + " objects" +
               (is.renumbered ? ", " + std::to_string(is.renumbered) + " repeated object ids renumbered" : "") + ")");
        return true;
    }

    bool Save(bool as)
    {
        if (is.size <= 0)
            return false;
        std::string file = is.path;
        if (as || file.empty())
        {
            file = FileDialog(1, L"Save island", L"Island (*.wrp)", L"*.wrp", L"wrp");
            if (file.empty())
                return false;
        }
        std::string error;
        if (!is.Save(file, error))
        {
            Status("Not saved: " + error);
            return false;
        }
        Status("Saved " + file + " (4WVR)");
        UpdateTitle();
        return true;
    }

    void RequestNew()
    {
        if (is.modified)
            pending = Ask::New;
        else
            openNew = true;
    }

    void RequestOpen()
    {
        if (is.modified)
        {
            pending = Ask::Open;
            return;
        }
        const std::string file = FileDialog(0, L"Open island", L"Islands (*.wrp)", L"*.wrp", nullptr);
        if (!file.empty())
            Open(file);
    }

    void RequestExit()
    {
        if (is.modified)
            pending = Ask::Exit;
        else
            running = false;
    }

    // ---- per frame ------------------------------------------------------------
    void Frame(float dt, int width, int height);
    void HandleView(float dt, int width, int height);
    void HandleTool(float dt);
    void Interface(int width, int height);
    void ToolsPanel(float top, float bottom);
    void ViewBar(float top, int width);
    void SelectTool(Tool t);
    void SidePanel(float top, float bottom, int width);
    void Dialogs();
    int PickObject(int width, int height) const;
    bool ChooseHeightMap();
    void ApplyHeightMap();
    void EnsureRules();
    void SelectRuleSet(int index);
    void RunAutoTexture();
    void AutoTextureWindow();
    void SetReskinDefaults();
    void UpdateReskinPreview();
    void ReskinWindow();
    void SyncTextures();
    void EnsureBuilders();
    void BuildRoad();
    bool HandleBuilders(bool clicked);
    void BuilderPanel();
    void Line(float x0, float z0, float x1, float z1, uint32_t color, float lift = 0.5f, int steps = 1);
    void RotateObject(int index, float radians);
};

int App::PickObject(int width, int height) const
{
    const ImVec2 m = ImGui::GetIO().MousePos;
    int best = -1;

    // The model under the cursor, where models are drawn: the ray through the cursor against
    // each one's box.  Of those it goes through, the smallest: a bush inside a block of
    // forest is what was meant, not the forest.
    {
        float dir[3];
        cam.Ray(m.x, m.y, static_cast<float>(width), static_cast<float>(height), dir);
        const float reach = hover ? std::sqrt((hit[0] - cam.pos[0]) * (hit[0] - cam.pos[0]) + (hit[1] - cam.pos[1]) * (hit[1] - cam.pos[1]) +
                                              (hit[2] - cam.pos[2]) * (hit[2] - cam.pos[2])) + 2.0f
                                  : cam.farZ;
        float smallest = 1e30f;
        for (size_t i = 0; i < is.objects.size(); i++)
        {
            if (!renderer.DrawnAsModel(static_cast<int>(i)))
                continue;
            const island::Object& o = is.objects[i];
            const ModelShape& s = game.Model(o.model);
            if (!s.valid)
                continue;
            const float size[3] = {s.bmax[0] - s.bmin[0], s.bmax[1] - s.bmin[1], s.bmax[2] - s.bmin[2]};
            if (size[1] < 0.5f)
                continue; // flat on the ground (a road): its marker is picked instead
            // the ray in the model's own space
            const float p[3] = {cam.pos[0] - o.X(), cam.pos[1] - is.ObjectY(o), cam.pos[2] - o.Z()};
            float lo[3], ld[3];
            for (int a = 0; a < 3; a++)
            {
                const float* axis = &o.m[a * 3];
                const float len2 = std::max(1e-6f, axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
                lo[a] = (p[0] * axis[0] + p[1] * axis[1] + p[2] * axis[2]) / len2;
                ld[a] = (dir[0] * axis[0] + dir[1] * axis[1] + dir[2] * axis[2]) / len2;
            }
            float t0 = 0.0f, t1 = reach;
            for (int a = 0; a < 3 && t0 <= t1; a++)
            {
                const float mn = s.bmin[a] - 0.2f, mx = s.bmax[a] + 0.2f;
                if (std::fabs(ld[a]) < 1e-6f)
                {
                    if (lo[a] < mn || lo[a] > mx)
                        t1 = -1.0f;
                    continue;
                }
                float a0 = (mn - lo[a]) / ld[a], a1 = (mx - lo[a]) / ld[a];
                if (a0 > a1)
                    std::swap(a0, a1);
                t0 = std::max(t0, a0);
                t1 = std::min(t1, a1);
            }
            if (t0 > t1)
                continue;
            const float volume = size[0] * size[1] * size[2];
            if (volume < smallest)
            {
                smallest = volume;
                best = static_cast<int>(i);
            }
        }
        if (best >= 0)
            return best;
    }

    float bestD = 18.0f * 18.0f; // pixels
    for (size_t i = 0; i < is.objects.size(); i++)
    {
        const island::Object& o = is.objects[i];
        const float dx = o.X() - hit[0], dz = o.Z() - hit[2];
        if (dx * dx + dz * dz > 400.0f * 400.0f)
            continue;
        const float base = is.ObjectY(o);
        for (float lift : {0.0f, 3.0f, 6.0f})
        {
            const float p[3] = {o.X(), base + lift, o.Z()};
            float sx, sy;
            if (!cam.Project(p, static_cast<float>(width), static_cast<float>(height), sx, sy))
                continue;
            const float d = (sx - m.x) * (sx - m.x) + (sy - m.y) * (sy - m.y);
            if (d < bestD)
            {
                bestD = d;
                best = static_cast<int>(i);
            }
        }
    }
    return best;
}

void App::RotateObject(int index, float radians)
{
    if (index < 0 || index >= static_cast<int>(is.objects.size()))
        return;
    island::Object o = is.objects[index];
    const float c = std::cos(radians), s = std::sin(radians);
    for (int col = 0; col < 3; col++)
    {
        float* v = o.m + col * 3;
        const float x = v[0] * c + v[2] * s, z = -v[0] * s + v[2] * c;
        v[0] = x;
        v[2] = z;
    }
    is.BeginEdit("Turn object");
    is.ChangeObject(index, o);
    is.EndEdit();
}

void App::EnsureBuilders()
{
    if (buildersFound)
        return;
    buildersFound = true;
    roads.Find(game, is);
    forests = FindForests(game);
    bridges = FindBridges(game);
    roadFamily = std::clamp(roadFamily, 0, std::max(0, static_cast<int>(roads.Families().size()) - 1));
    forestKind = std::clamp(forestKind, 0, std::max(0, static_cast<int>(forests.size()) - 1));
    bridgeKind = std::clamp(bridgeKind, 0, std::max(0, static_cast<int>(bridges.size()) - 1));
    // the forest the island already has, if it has one
    for (const island::Object& o : is.objects)
    {
        bool found = false;
        for (size_t i = 0; i < forests.size() && !found; i++)
            if (LowerPath(o.model) == forests[i].half1)
            {
                forestKind = static_cast<int>(i);
                found = true;
            }
        if (found)
            break;
    }
}

// a guide line lying on the ground
void App::Line(float x0, float z0, float x1, float z1, uint32_t color, float lift, int steps)
{
    for (int i = 0; i < steps; i++)
    {
        const float t0 = static_cast<float>(i) / steps, t1 = static_cast<float>(i + 1) / steps;
        OverlayLine l;
        l.a[0] = x0 + (x1 - x0) * t0;
        l.a[2] = z0 + (z1 - z0) * t0;
        l.b[0] = x0 + (x1 - x0) * t1;
        l.b[2] = z0 + (z1 - z0) * t1;
        l.a[1] = std::max(0.0f, is.SurfaceY(l.a[0], l.a[2])) + lift;
        l.b[1] = std::max(0.0f, is.SurfaceY(l.b[0], l.b[2])) + lift;
        l.color = color;
        overlay.push_back(l);
    }
}

void App::BuildRoad()
{
    EnsureBuilders();
    if (roadPath.size() < 2 || roadFamily >= static_cast<int>(roads.Families().size()))
        return;
    const std::vector<PlacedRoad> placed = roads.Lay(roads.Families()[roadFamily], roadPath, roadCaps, roadSnapped ? &roadHeading : nullptr);
    if (placed.empty())
        return;
    is.BeginEdit("Road");
    int id = is.NewObjectId();
    for (const PlacedRoad& p : placed)
        is.AddObject(Roads::MakeObject(p, id++));
    is.EndEdit();
    Status("Road laid: " + std::to_string(placed.size()) + " pieces");
    roadPath.clear();
    roadPreview.clear();
    roadSnapped = false;
}

// The tools that are not brushes.  True when the tool in hand is one of them.
bool App::HandleBuilders(bool clicked)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool keys = !io.WantCaptureKeyboard;
    if (tool == Tool::Road)
    {
        EnsureBuilders();
        const bool finish = hover && !io.WantCaptureMouse && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (clicked && !finish)
        {
            std::array<float, 2> p = {hit[0], hit[2]};
            if (roadPath.empty())
            {
                // carry on from the open end of a road, if one is near
                float at[2];
                roadSnapped = roads.OpenEnd(is, game, hit[0], hit[2], 18.0f, at, roadHeading);
                if (roadSnapped)
                    p = {at[0], at[1]};
            }
            if (roadPath.empty() || std::hypot(p[0] - roadPath.back()[0], p[1] - roadPath.back()[1]) > 2.0f)
                roadPath.push_back(p);
        }
        if (keys && ImGui::IsKeyPressed(ImGuiKey_Backspace) && !roadPath.empty())
            roadPath.pop_back();
        if (keys && ImGui::IsKeyPressed(ImGuiKey_Escape))
            roadPath.clear();
        if (finish || (keys && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))))
            BuildRoad();

        // what would be laid, with the cursor as the next point
        roadPreview.clear();
        std::vector<std::array<float, 2>> path = roadPath;
        if (hover && !path.empty() && std::hypot(hit[0] - path.back()[0], hit[2] - path.back()[1]) > 2.0f)
            path.push_back({hit[0], hit[2]});
        if (path.size() >= 2 && roadFamily < static_cast<int>(roads.Families().size()))
            roadPreview = roads.Lay(roads.Families()[roadFamily], path, roadCaps, roadSnapped ? &roadHeading : nullptr);
        for (const PlacedRoad& p : roadPreview)
            for (int c = 0; c < 4; c++)
                Line(p.outline[c][0], p.outline[c][1], p.outline[(c + 1) & 3][0], p.outline[(c + 1) & 3][1], 0xFFFFE040, 0.4f);
        for (size_t i = 0; i + 1 < path.size(); i++)
            Line(path[i][0], path[i][1], path[i + 1][0], path[i + 1][1], 0x80FFFFFF, 1.5f, 8);
        if (roadPath.empty() && hover)
        {
            float at[2], heading;
            if (roads.OpenEnd(is, game, hit[0], hit[2], 18.0f, at, heading))
            {
                // a road to carry on from
                Line(at[0] - 4, at[1], at[0] + 4, at[1], 0xFF40FF40, 1.0f);
                Line(at[0], at[1] - 4, at[0], at[1] + 4, 0xFF40FF40, 1.0f);
            }
        }
        return true;
    }
    if (tool == Tool::Bridge)
    {
        EnsureBuilders();
        if (keys && ImGui::IsKeyPressed(ImGuiKey_Escape))
            bridgeStarted = false;
        if (clicked && bridgeKind < static_cast<int>(bridges.size()))
        {
            if (!bridgeStarted)
            {
                std::memcpy(bridgeA, hit, sizeof(hit));
                bridgeStarted = true;
                if (bridgeLevelFromBank)
                    bridgeLevel = hit[1];
            }
            else
            {
                is.BeginEdit("Bridge");
                const int n = BuildBridge(is, bridges[bridgeKind], bridgeA[0], bridgeA[2], hit[0], hit[2], bridgeLevel);
                is.EndEdit();
                Status("Bridge: " + std::to_string(n) + " sections, deck at " + std::to_string(static_cast<int>(bridgeLevel)) + " m");
                bridgeStarted = false;
            }
        }
        if (bridgeStarted && hover && bridgeKind < static_cast<int>(bridges.size()))
        {
            // the deck, level, and where the sections join
            const BridgeKind& k = bridges[bridgeKind];
            const float dx = hit[0] - bridgeA[0], dz = hit[2] - bridgeA[2];
            const float span = std::hypot(dx, dz);
            if (span > 1.0f)
            {
                const int n = std::max(1, static_cast<int>(std::lround(span / k.length)));
                const float ux = dx / span, uz = dz / span, start = (span - n * k.length) * 0.5f;
                for (int i = 0; i <= n; i++)
                {
                    const float a = start + i * k.length;
                    OverlayLine l;
                    l.color = 0xFFFFE040;
                    l.a[0] = bridgeA[0] + ux * a - uz * 4.0f;
                    l.a[2] = bridgeA[2] + uz * a + ux * 4.0f;
                    l.b[0] = bridgeA[0] + ux * a + uz * 4.0f;
                    l.b[2] = bridgeA[2] + uz * a - ux * 4.0f;
                    l.a[1] = l.b[1] = bridgeLevel;
                    overlay.push_back(l);
                    if (i < n)
                    {
                        OverlayLine d;
                        d.color = 0xFFFFE040;
                        d.a[0] = bridgeA[0] + ux * a;
                        d.a[2] = bridgeA[2] + uz * a;
                        d.b[0] = bridgeA[0] + ux * (a + k.length);
                        d.b[2] = bridgeA[2] + uz * (a + k.length);
                        d.a[1] = d.b[1] = bridgeLevel;
                        overlay.push_back(d);
                    }
                }
            }
        }
        return true;
    }
    if (tool == Tool::Region)
    {
        auto cellOf = [&](int out[2])
        {
            out[0] = std::clamp(static_cast<int>(hit[0] / is.cell), 0, is.size - 1);
            out[1] = std::clamp(static_cast<int>(hit[2] / is.cell), 0, is.size - 1);
        };
        if (clicked)
        {
            cellOf(regionA);
            cellOf(regionB);
            regionDragging = regionSet = true;
        }
        if (regionDragging && hover)
            cellOf(regionB);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            regionDragging = false;
    }
    if (regionSet && (tool == Tool::Region))
    {
        const float x0 = std::min(regionA[0], regionB[0]) * is.cell, x1 = (std::max(regionA[0], regionB[0]) + 1) * is.cell;
        const float z0 = std::min(regionA[1], regionB[1]) * is.cell, z1 = (std::max(regionA[1], regionB[1]) + 1) * is.cell;
        const int sx = std::max(1, static_cast<int>((x1 - x0) / is.cell)), sz = std::max(1, static_cast<int>((z1 - z0) / is.cell));
        Line(x0, z0, x1, z0, 0xFF40E0FF, 1.0f, sx);
        Line(x0, z1, x1, z1, 0xFF40E0FF, 1.0f, sx);
        Line(x0, z0, x0, z1, 0xFF40E0FF, 1.0f, sz);
        Line(x1, z0, x1, z1, 0xFF40E0FF, 1.0f, sz);
    }
    return tool == Tool::Region;
}

void App::BuilderPanel()
{
    EnsureBuilders();
    const float half = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    if (game.Folder().empty() && tool != Tool::Region)
    {
        Hint("Set the game folder (Info tab at the right): the models come from it.");
        return;
    }
    if (tool == Tool::Road)
    {
        const std::vector<RoadFamily>& fam = roads.Families();
        if (fam.empty())
        {
            Hint("No road models were found in the game folder.");
            return;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##roadkind", RoadLabel(fam[roadFamily]).c_str()))
        {
            for (int i = 0; i < static_cast<int>(fam.size()); i++)
                if (ImGui::Selectable(RoadLabel(fam[i]).c_str(), i == roadFamily))
                    roadFamily = i;
            ImGui::EndCombo();
        }
        const RoadFamily& f = fam[roadFamily];
        char line[160];
        snprintf(line, sizeof(line), "%d straight pieces, %d curves%s", static_cast<int>(f.straights.size()), static_cast<int>(f.curves.size()),
                 f.hasCap ? ", end piece" : "");
        Hint(line);
        if (f.hasCap)
            ImGui::Checkbox("End pieces at both ends", &roadCaps);
        ImGui::Spacing();
        if (roadPath.empty())
            Hint("No points yet.");
        else
            ImGui::Text("%d points, %d pieces", static_cast<int>(roadPath.size()), static_cast<int>(roadPreview.size()));
        if (roadSnapped && !roadPath.empty())
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Carries on from a road");
        ImGui::BeginDisabled(roadPath.size() < 2);
        if (Toggle("Lay it", roadPath.size() >= 2, ImVec2(half, 0)))
            BuildRoad();
        ImGui::SetItemTooltip("Enter, or a double click on the last point");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(roadPath.empty());
        if (ImGui::Button("Start again", ImVec2(half, 0)))
            roadPath.clear();
        ImGui::SetItemTooltip("Esc");
        ImGui::EndDisabled();
        ImGui::Spacing();
        Hint("Start near the open end of a road to carry on from it. Roads lie on the ground as it is: shape it first.");
    }
    else if (tool == Tool::Forest)
    {
        if (forests.empty())
        {
            Hint("No forest blocks were found in the game folder.");
            return;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##forestkind", ForestLabel(forests[forestKind]).c_str()))
        {
            for (int i = 0; i < static_cast<int>(forests.size()); i++)
            {
                if (ImGui::Selectable(ForestLabel(forests[i]).c_str(), i == forestKind))
                    forestKind = i;
                ImGui::SetItemTooltip("%s", forests[i].name.c_str());
            }
            ImGui::EndCombo();
        }
        Row("Radius");
        ImGui::SliderFloat("##radius", &brush.radius, is.cell * 0.5f, 3000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox("Corner blocks round the edges", &forestCorners);
        ImGui::Checkbox("Forest floor texture under it", &forestFloor);
        ImGui::SetItemTooltip("Textures the ground under and around the stroke again by the auto texture rules");
        ImGui::Spacing();
        Hint("Two blocks to every 50 m cell; none is planted in the sea.");
    }
    else if (tool == Tool::Bridge)
    {
        if (bridges.empty())
        {
            Hint("No bridge models were found in the game folder.");
            return;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##bridgekind", bridges[bridgeKind].model.c_str()))
        {
            for (int i = 0; i < static_cast<int>(bridges.size()); i++)
                if (ImGui::Selectable(bridges[i].model.c_str(), i == bridgeKind))
                    bridgeKind = i;
            ImGui::EndCombo();
        }
        BridgeKind& k = bridges[bridgeKind];
        Row("Length");
        ImGui::Text("%.1f m", k.length);
        Row("Roadway");
        ImGui::DragFloat("##deck", &k.deck, 0.05f, -50.0f, 50.0f, "%.2f m up");
        ImGui::SetItemTooltip("How far the roadway is above the middle of the model");
        ImGui::Checkbox("Deck level with the first bank", &bridgeLevelFromBank);
        ImGui::BeginDisabled(bridgeLevelFromBank && !bridgeStarted);
        Row("Deck at");
        ImGui::DragFloat("##level", &bridgeLevel, 0.1f, -50.0f, 1400.0f, "%.1f m");
        ImGui::SetItemTooltip("Above the sea");
        ImGui::EndDisabled();
        ImGui::Spacing();
        if (bridgeStarted)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Now click the other bank (Esc: start again)");
        Hint("A bridge keeps its height when the land under it is shaped.");
    }
    else if (tool == Tool::Region)
    {
        const int x0 = std::min(regionA[0], regionB[0]), x1 = std::max(regionA[0], regionB[0]);
        const int z0 = std::min(regionA[1], regionB[1]), z1 = std::max(regionA[1], regionB[1]);
        if (regionSet)
        {
            Row("Cells");
            ImGui::Text("%d, %d to %d, %d", x0, z0, x1, z1);
            Row("Size");
            ImGui::Text("%d x %d  (%.2f x %.2f km)", x1 - x0 + 1, z1 - z0 + 1, (x1 - x0 + 1) * is.cell / 1000.0f, (z1 - z0 + 1) * is.cell / 1000.0f);
        }
        else
        {
            Hint("No region marked yet.");
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(!regionSet);
        if (ImGui::Button("Export...", ImVec2(half, 0)))
        {
            const std::string file = FileDialog(1, L"Export region", L"Island (*.wrp)", L"*.wrp", L"wrp");
            std::string error;
            if (!file.empty())
                Status(ExportRegion(is, x0, z0, x1, z1, file, error) ? "Region written: " + file : "Not written: " + error);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Import...", ImVec2(half, 0)))
        {
            const std::string file = FileDialog(0, L"Import region", L"Islands (*.wrp)", L"*.wrp", nullptr);
            std::string error;
            if (!file.empty())
            {
                if (GameData::LoadIsland(file, regionSrc, error))
                {
                    regionInfo = ReadRegionInfo(file, regionSrc);
                    regionFile = file;
                    regionAtOrigin = !regionSet && regionInfo.marked;
                    if (!regionSet)
                    {
                        regionA[0] = regionA[1] = regionB[0] = regionB[1] = 0;
                    }
                    openRegionImport = true;
                }
                else
                {
                    Status("Cannot read " + file + ": " + error);
                }
            }
        }
        if (ImGui::Button("Mark the whole island", ImVec2(-FLT_MIN, 0)))
        {
            regionA[0] = regionA[1] = 0;
            regionB[0] = regionB[1] = is.size - 1;
            regionSet = true;
        }
        ImGui::BeginDisabled(!regionSet);
        if (ImGui::Button("Remove the objects in it", ImVec2(-FLT_MIN, 0)))
        {
            island::ObjectList kept;
            for (const island::Object& o : is.objects)
                if (o.X() < x0 * is.cell || o.X() >= (x1 + 1) * is.cell || o.Z() < z0 * is.cell || o.Z() >= (z1 + 1) * is.cell)
                    kept.push_back(o);
            const size_t gone = is.objects.size() - kept.size();
            is.BeginEdit("Remove objects");
            is.ReplaceObjects(std::move(kept));
            is.EndEdit();
            selected = -1;
            Status(std::to_string(gone) + " objects removed");
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        Hint("A region is saved as a small island file: elevations, textures and objects. Whole islands can be imported too.");
    }
}

bool App::ChooseHeightMap()
{
    const std::string file = FileDialog(0, L"Height map", L"Height maps (PNG, PGM, RAW, ASC ...)", HeightImageFilter(), nullptr);
    if (file.empty())
        return false;
    HeightImage img;
    std::string error;
    if (!LoadHeightImage(file, img, error))
    {
        Status("Cannot read " + file + ": " + error);
        return false;
    }
    heightImage = std::move(img);
    importFile = file;
    importLow = heightImage.metres ? heightImage.low : -40.0f;
    importHigh = heightImage.metres ? heightImage.high : 400.0f;
    importSmooth = heightImage.bits <= 8 ? 2 : 0;
    return true;
}

void App::ApplyHeightMap()
{
    if (heightImage.width <= 0)
        return;
    is.ApplyHeights(heightImage.values.data(), heightImage.width, heightImage.height, importLow, importHigh, heightImage.metres);
    if (importSmooth > 0)
        is.SmoothAll(importSmooth);
}

// The atlas follows the texture table: slots that got another texture are drawn again.
void App::SyncTextures()
{
    if (is.size <= 0 || shownTextures == is.textures)
        return;
    shownTextures.resize(kMaxTextures);
    std::vector<int> changed;
    for (int i = 0; i < kMaxTextures; i++)
        if (shownTextures[i] != is.textures[i])
            changed.push_back(i);
    renderer.UpdateTextures(changed);
    shownTextures = is.textures;
    minimapDirty = true;
}

void App::SelectRuleSet(int index)
{
    std::vector<TextureSet>& sets = game.Sets();
    if (index < 0 || index >= static_cast<int>(sets.size()))
        return;
    ruleSet = index;
    game.LoadSetColours(sets[index]);
    // from how the island itself uses the set, else how the game's own island does, else by
    // colour.  The heights and the rest stay as they were set.
    game.RolesFor(is, sets[index], rules);
}

void App::EnsureRules()
{
    if (ruleSet >= 0 || game.Sets().empty())
        return;
    const int own = game.SetOf(is);
    SelectRuleSet(own >= 0 ? own : 0);
    rules.FitHeights(is.MaxHeight());
}

void App::RunAutoTexture()
{
    EnsureRules();
    if (ruleSet < 0)
    {
        Status("Set the game folder first: the textures come from it");
        return;
    }
    is.BeginEdit("Auto texture");
    lastAuto = is.AutoTexture(rules, game.Textures());
    is.EndEdit();
    autoRan = true;
}

void App::AutoTextureWindow()
{
    if (!showAuto)
        return;
    ImGui::SetNextWindowPos(ImVec2(S(kMargin * 2 + kLeftWidth), S(90)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(470), 0), ImVec2(S(470), FLT_MAX));
    if (ImGui::Begin("Auto texture", &showAuto, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::PushItemWidth(S(220));
        std::vector<TextureSet>& sets = game.Sets();
        if (sets.empty())
        {
            ImGui::TextWrapped("No texture sets were found. Set the game folder (Info tab): the textures and their "
                               "transitions are read from it.");
        }
        else
        {
            EnsureRules();
            ImGui::TextWrapped("Textures the land by height and steepness, with the set's own transition textures "
                               "between the kinds of ground.");
            if (ImGui::BeginCombo("Texture set", ruleSet >= 0 ? sets[ruleSet].folder.c_str() : ""))
            {
                for (int i = 0; i < static_cast<int>(sets.size()); i++)
                {
                    char label[96];
                    snprintf(label, sizeof(label), "%s  (%d plain, %d transitions)", sets[i].folder.c_str(),
                             static_cast<int>(sets[i].plains.size()), sets[i].transitions);
                    if (ImGui::Selectable(label, i == ruleSet))
                        SelectRuleSet(i);
                }
                ImGui::EndCombo();
            }
            const TextureSet& set = sets[ruleSet];
            auto swatch = [&](const std::string& code)
            {
                ImVec4 c(0.25f, 0.25f, 0.25f, 1.0f);
                for (const TextureSet::Plain& p : set.plains)
                    if (p.code == code)
                        c = ImVec4(p.rgb[0], p.rgb[1], p.rgb[2], 1.0f);
                ImGui::ColorButton("##sw", c, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(S(22), S(22)));
            };
            auto role = [&](const char* label, std::string& code)
            {
                ImGui::PushID(label);
                swatch(code);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(S(190));
                if (ImGui::BeginCombo(label, code.empty() ? "(none)" : code.c_str()))
                {
                    if (ImGui::Selectable("(none)", code.empty()))
                        code.clear();
                    for (const TextureSet::Plain& p : set.plains)
                    {
                        ImGui::PushID(p.code.c_str());
                        swatch(p.code);
                        ImGui::SameLine();
                        if (ImGui::Selectable(p.code.c_str(), p.code == code))
                            code = p.code;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            };
            Section("Which texture is what");
            role("Beach and sea bed", rules.beach);
            role("Shore", rules.shore);
            role("Grass", rules.grass);
            role("Grass, in patches", rules.grassAlt);
            role("Hills", rules.hill);
            role("Rock (steep ground)", rules.rock);
            role("Peaks", rules.peak);
            role("Forest floor", rules.forest);
            Hint("Taken from how the island uses them, or guessed by colour: change what is wrong.");
            Section("Where");
            ImGui::SliderFloat("Beach up to", &rules.beachTop, 0.0f, 40.0f, "%.1f m");
            ImGui::SliderFloat("Shore up to", &rules.shoreTop, 0.0f, 60.0f, "%.1f m");
            ImGui::SliderFloat("Hills from", &rules.hillFrom, 5.0f, 1000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Peaks from", &rules.peakFrom, 20.0f, 1474.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Rock steeper than", &rules.rockSlope, 5.0f, 60.0f, "%.0f deg");
            Section("Detail");
            ImGui::SliderFloat("Wandering borders", &rules.wobble, 0.0f, 1.0f);
            ImGui::SliderFloat("Patches of other grass", &rules.patches, 0.0f, 1.0f);
            ImGui::Checkbox("Forest floor under forests", &rules.forestFloor);
            ImGui::Checkbox("Step through the kinds between (smoother borders)", &rules.stepThrough);
            ImGui::Checkbox("Sea texture on the deep sea bed", &rules.seaTexture);
            if (rules.seaTexture)
                ImGui::SliderFloat("Deeper than", &rules.seaBelow, -100.0f, 0.0f, "%.1f m");
            int seed = static_cast<int>(rules.seed);
            if (ImGui::InputInt("Seed", &seed))
                rules.seed = static_cast<unsigned>(std::max(0, seed));
            ImGui::Spacing();
            const float halfW = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
            if (Toggle("Texture the whole island", true, ImVec2(halfW, S(34))))
                RunAutoTexture();
            ImGui::SameLine();
            if (ImGui::Button("Use as a brush", ImVec2(halfW, S(34))))
                tool = Tool::AutoPaint;
            if (autoRan)
            {
                ImGui::Text("%d cells changed, %d of them transitions.", lastAuto.changed, lastAuto.transitions);
                if (lastAuto.noTransition)
                    ImGui::TextDisabled("%d mixed cells the set has no transition for.", lastAuto.noTransition);
                if (lastAuto.tableFull)
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%d cells skipped: all 512 texture slots are used.",
                                       lastAuto.tableFull);
                Hint("Ctrl+Z takes it back.");
            }
        }
        ImGui::PopItemWidth();
    }
    ImGui::End();
}

void App::SetReskinDefaults()
{
    const std::vector<ReskinStyle>& styles = ReskinStyles();
    reskin.style = std::clamp(reskin.style, 0, static_cast<int>(styles.size()) - 1);
    const ReskinStyle& st = styles[reskin.style];
    reskin.removeTrees = st.removeTrees;
    reskin.removeBushes = st.removeBushes;
    // "eden" + "snow"; at most 12 characters, small letters and digits
    std::string stem;
    for (char c : LowerPath(is.path.empty() ? std::string("island") : fs::path(is.path).stem().string()))
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            stem.push_back(c);
    if (stem.empty() || (stem[0] >= '0' && stem[0] <= '9'))
        stem = "island" + stem;
    const std::string suffix = st.suffix;
    stem = stem.substr(0, 12 - suffix.size());
    snprintf(reskinAddon, sizeof(reskinAddon), "%s%s", stem.c_str(), suffix.c_str());
    std::string nice = is.path.empty() ? std::string("Island") : fs::path(is.path).stem().string();
    if (!nice.empty())
        nice[0] = static_cast<char>(toupper(static_cast<unsigned char>(nice[0])));
    snprintf(reskinTitle, sizeof(reskinTitle), "%s (%s)", nice.c_str(), st.name);
    if (!reskinBase[0])
        snprintf(reskinBase, sizeof(reskinBase), "%s", GuessBaseWorld(is.path).c_str());
    if (reskin.outFolder.empty())
    {
        const char* home = getenv("USERPROFILE");
        reskin.outFolder = (fs::path(home ? home : ".") / "Documents" / "Island Editor").string();
    }
}

void App::UpdateReskinPreview()
{
    const bool want = showReskin && reskinPreview;
    if (!want && !reskinFilterOn)
        return;
    reskinFilterOn = want;
    if (want)
    {
        const int style = reskin.style;
        const float strength = reskin.strength;
        renderer.SetTextureFilter([style, strength](uint8_t* rgba, size_t count) { ReskinPixels(style, strength, rgba, count); });
    }
    else
    {
        renderer.SetTextureFilter(nullptr);
    }
    minimapDirty = true;
}

void App::ReskinWindow()
{
    if (!showReskin)
    {
        if (reskinFilterOn)
            UpdateReskinPreview(); // the window was closed: back to the real colours
        return;
    }
    const std::vector<ReskinStyle>& styles = ReskinStyles();
    ImGui::SetNextWindowPos(ImVec2(S(kMargin * 2 + kLeftWidth), S(90)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(470), 0), ImVec2(S(470), FLT_MAX));
    if (ImGui::Begin("Reskin", &showReskin, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::PushItemWidth(S(220));
        ImGui::TextWrapped("The same island in another climate: every ground texture recoloured into an addon of its own.");
        if (ImGui::BeginCombo("Style", styles[reskin.style].name))
        {
            for (int i = 0; i < static_cast<int>(styles.size()); i++)
                if (ImGui::Selectable(styles[i].name, i == reskin.style))
                {
                    reskin.style = i;
                    SetReskinDefaults();
                    UpdateReskinPreview();
                }
            ImGui::EndCombo();
        }
        Hint(styles[reskin.style].description);
        if (ImGui::SliderFloat("Strength", &reskin.strength, 0.2f, 1.0f))
        {
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            UpdateReskinPreview();
        if (ImGui::Checkbox("Show it on the island now", &reskinPreview))
            UpdateReskinPreview();
        float trees = reskin.removeTrees * 100.0f, bushes = reskin.removeBushes * 100.0f;
        if (ImGui::SliderFloat("Trees removed", &trees, 0.0f, 100.0f, "%.0f %%"))
            reskin.removeTrees = trees / 100.0f;
        if (ImGui::SliderFloat("Bushes removed", &bushes, 0.0f, 100.0f, "%.0f %%"))
            reskin.removeBushes = bushes / 100.0f;

        Section("What is made");
        ImGui::InputText("Addon name", reskinAddon, sizeof(reskinAddon));
        ImGui::Checkbox("An island of its own in the game", &reskin.worldConfig);
        if (reskin.worldConfig)
        {
            ImGui::InputText("Its name in the game", reskinTitle, sizeof(reskinTitle));
            ImGui::InputText("Settings from world", reskinBase, sizeof(reskinBase));
            Hint("Eden = Everon, Abel = Malden, Cain = Kolgujev, Noe = Nogova.");
        }
        ImGui::Checkbox("Pack it into a .pbo", &reskin.pack);
        if (reskin.pack && !game.Folder().empty())
            ImGui::Checkbox("Copy the .pbo into the game's AddOns folder", &reskin.install);
        Hint("Made in");
        ImGui::TextWrapped("%s", reskin.outFolder.c_str());
        if (ImGui::Button("Another folder..."))
        {
            const std::string dir = FileDialog(2, L"Where to make the addon", nullptr, nullptr, nullptr);
            if (!dir.empty())
                reskin.outFolder = dir;
        }
        ImGui::Spacing();
        if (Toggle("Make the reskin", true, ImVec2(S(220), S(34))))
        {
            // the real textures have to be read, not the previewed ones
            const bool hadPreview = reskinPreview;
            reskinPreview = false;
            UpdateReskinPreview();
            reskin.addon = reskinAddon;
            reskin.title = reskinTitle;
            reskin.baseWorld = reskinBase;
            ReskinReport report;
            std::string error;
            if (ReskinIsland(is, game, reskin, report, error))
            {
                char msg[700];
                snprintf(msg, sizeof(msg), "%d textures written%s, %d objects removed.\n%s%s\n%s",
                         report.textures, report.failed ? " (some could not be read and were left)" : "", report.objectsRemoved,
                         report.pbo.empty() ? "Folder: " : "Addon: ", report.pbo.empty() ? report.folder.c_str() : report.pbo.c_str(),
                         !report.installed.empty()          ? "Copied into the game's AddOns folder."
                         : (report.pbo.empty() ? "" : "Put the .pbo into the game's AddOns folder."));
                reskinMessage = msg;
                if (reskin.worldConfig)
                    reskinMessage += std::string("\nIn the game the island is \"") + reskinTitle + "\" (missions: name." + reskinAddon + ").";
                selected = -1;
                UpdateTitle();
                Status("Reskin made: " + (report.pbo.empty() ? report.folder : report.pbo));
            }
            else
            {
                reskinMessage = "Not made: " + error;
                reskinPreview = hadPreview;
                UpdateReskinPreview();
            }
        }
        if (!reskinMessage.empty())
            ImGui::TextWrapped("%s", reskinMessage.c_str());
        ImGui::PopItemWidth();
    }
    ImGui::End();
}

void App::HandleView(float dt, int width, int height)
{
    ImGuiIO& io = ImGui::GetIO();
    hover = false;
    if (is.size <= 0)
        return;
    const bool overUi = io.WantCaptureMouse;

    float f[3], r[3], u[3];
    cam.Basis(f, r, u);
    if (!overUi)
    {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f))
        {
            cam.yaw += io.MouseDelta.x * 0.0035f;
            cam.pitch = std::clamp(cam.pitch - io.MouseDelta.y * 0.0035f, -1.55f, 1.2f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
        {
            // the land follows the hand
            const float k = std::max(20.0f, cam.pos[1] - is.SurfaceY(cam.pos[0], cam.pos[2])) * 0.0016f;
            const float flat = std::sqrt(f[0] * f[0] + f[2] * f[2]);
            const float fx = flat > 1e-3f ? f[0] / flat : std::sin(cam.yaw), fz = flat > 1e-3f ? f[2] / flat : std::cos(cam.yaw);
            cam.pos[0] += (-r[0] * io.MouseDelta.x + fx * io.MouseDelta.y) * k;
            cam.pos[2] += (-r[2] * io.MouseDelta.x + fz * io.MouseDelta.y) * k;
        }
    }
    if (!io.WantCaptureKeyboard && !io.KeyCtrl)
    {
        float speed = std::max(60.0f, (cam.pos[1] - std::max(0.0f, is.SurfaceY(cam.pos[0], cam.pos[2]))) * 1.2f);
        if (io.KeyShift)
            speed *= 4.0f;
        float mv[3] = {0, 0, 0};
        auto addv = [&](const float* v, float k)
        {
            mv[0] += v[0] * k;
            mv[1] += v[1] * k;
            mv[2] += v[2] * k;
        };
        if (ImGui::IsKeyDown(ImGuiKey_W))
            addv(f, 1);
        if (ImGui::IsKeyDown(ImGuiKey_S))
            addv(f, -1);
        if (ImGui::IsKeyDown(ImGuiKey_D))
            addv(r, 1);
        if (ImGui::IsKeyDown(ImGuiKey_A))
            addv(r, -1);
        if (ImGui::IsKeyDown(ImGuiKey_E))
            mv[1] += 1;
        if (ImGui::IsKeyDown(ImGuiKey_Q))
            mv[1] -= 1;
        for (int i = 0; i < 3; i++)
            cam.pos[i] += mv[i] * speed * dt;
    }

    if (!overUi)
    {
        float dir[3];
        cam.Ray(io.MousePos.x, io.MousePos.y, static_cast<float>(width), static_cast<float>(height), dir);
        hover = is.Raycast(cam.pos, dir, cam.farZ, hit);
        if (io.MouseWheel != 0.0f)
        {
            if (io.KeyAlt || io.KeyCtrl)
            {
                brush.radius = std::clamp(brush.radius * (io.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f), is.cell * 0.5f, 3000.0f);
            }
            else
            {
                // towards what the cursor is on (or straight ahead over open sea)
                float dist = 800.0f;
                if (hover)
                {
                    const float d[3] = {hit[0] - cam.pos[0], hit[1] - cam.pos[1], hit[2] - cam.pos[2]};
                    dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                }
                const float step = dist * 0.18f * io.MouseWheel;
                for (int i = 0; i < 3; i++)
                    cam.pos[i] += dir[i] * step;
            }
        }
    }
    // never under the ground, never lost
    const float s = is.WorldSize();
    cam.pos[0] = std::clamp(cam.pos[0], -s, s * 2.0f);
    cam.pos[2] = std::clamp(cam.pos[2], -s, s * 2.0f);
    cam.pos[1] = std::clamp(cam.pos[1], std::max(0.0f, is.SurfaceY(cam.pos[0], cam.pos[2])) + 3.0f, 60000.0f);
}

void App::HandleTool(float dt)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool down = hover && !io.WantCaptureMouse && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool clicked = hover && !io.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);

    overlay.clear();
    if (HandleBuilders(clicked))
        return;

    if (tool == Tool::Objects)
    {
        if (clicked)
        {
            if (placing && !placeModel.empty())
            {
                island::Object o;
                o.id = is.NewObjectId();
                o.model = placeModel;
                const float c = std::cos(placeYaw), s = std::sin(placeYaw);
                const float m[12] = {c, 0, -s, 0, 1, 0, s, 0, c, hit[0], 0, hit[2]};
                std::memcpy(o.m, m, sizeof(m));
                o.above = game.Model(placeModel).centre[1]; // the middle of the model is its origin
                is.BeginEdit("Place object");
                selected = is.AddObject(o);
                is.EndEdit();
            }
            else
            {
                selected = PickObject(w, h);
                dragging = false;
                if (selected >= 0)
                {
                    // where on the ground it was taken hold of: that stays under the cursor
                    dragOffset[0] = is.objects[selected].X() - hit[0];
                    dragOffset[1] = is.objects[selected].Z() - hit[2];
                }
            }
        }
        else if (down && selected >= 0 && !placing && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))
        {
            if (!dragging)
            {
                is.BeginEdit("Move object");
                dragging = true;
            }
            island::Object o = is.objects[selected];
            o.m[9] = hit[0] + dragOffset[0];
            o.m[11] = hit[2] + dragOffset[1];
            is.ChangeObject(selected, o);
        }
        if (dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            is.EndEdit();
            dragging = false;
        }
        if (!io.WantCaptureKeyboard && selected >= 0)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete))
            {
                is.BeginEdit("Remove object");
                is.RemoveObject(selected);
                is.EndEdit();
                selected = -1;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_R))
            {
                RotateObject(selected, (io.KeyShift ? -1.0f : 1.0f) * kPi / 12.0f);
            }
        }
        return;
    }

    if (tool == Tool::Ramp)
    {
        if (clicked)
        {
            rampActive = true;
            std::memcpy(rampStart, hit, sizeof(hit));
        }
        if (rampActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            rampActive = false;
            if (hover)
            {
                is.BeginEdit("Ramp");
                is.Ramp(rampStart[0], rampStart[2], rampStart[1], hit[0], hit[2], hit[1], brush);
                is.EndEdit();
            }
        }
        return;
    }

    if (tool == Tool::Paint && clicked && io.KeyAlt)
    {
        const int x = static_cast<int>(hit[0] / is.cell), z = static_cast<int>(hit[2] / is.cell);
        if (is.InRange(x, z))
            paintSlot = is.tex[static_cast<size_t>(z) * is.size + x];
        return;
    }

    if (down)
    {
        if (!stroke)
        {
            is.BeginEdit(kToolNames[static_cast<int>(tool)]);
            stroke = true;
            strokeTarget = hit[1];
            strokeSeed = static_cast<unsigned>(SDL_GetTicks()) | 1u;
        }
        Tool t = tool;
        if ((t == Tool::Raise || t == Tool::Lower || t == Tool::Noise || t == Tool::Erode) && io.KeyCtrl)
            t = Tool::Smooth;
        const bool invert = io.KeyShift;
        switch (t)
        {
            case Tool::Raise:
            case Tool::Lower:
            {
                const float sign = ((t == Tool::Raise) != invert) ? 1.0f : -1.0f;
                is.Raise(hit[0], hit[2], brush, sign * brush.strength * 60.0f * dt);
                break;
            }
            case Tool::Smooth:
                is.Smooth(hit[0], hit[2], brush, brush.strength * 10.0f * dt);
                break;
            case Tool::Flatten:
                is.Flatten(hit[0], hit[2], brush, strokeTarget, brush.strength * 8.0f * dt);
                break;
            case Tool::SetHeight:
                is.Flatten(hit[0], hit[2], brush, setHeight, brush.strength * 8.0f * dt);
                break;
            case Tool::Noise:
                is.Noise(hit[0], hit[2], brush, brush.strength * 40.0f * dt, noiseScale, strokeSeed);
                break;
            case Tool::Erode:
                is.Erode(hit[0], hit[2], brush, brush.strength * 12.0f * dt);
                break;
            case Tool::Paint:
                is.Paint(hit[0], hit[2], brush, static_cast<uint16_t>(paintSlot));
                break;
            case Tool::AutoPaint:
                EnsureRules();
                lastAuto = is.AutoTexture(rules, game.Textures(), &brush, hit[0], hit[2]);
                break;
            case Tool::Forest:
                EnsureBuilders();
                if (forestKind < static_cast<int>(forests.size()) &&
                    PaintForest(is, forests[forestKind], hit[0], hit[2], brush.radius, !io.KeyShift, forestCorners) > 0 && forestFloor)
                {
                    // the floor under the trees, and grass again where they went
                    EnsureRules();
                    if (ruleSet >= 0)
                    {
                        Brush wide = brush;
                        wide.radius = brush.radius + is.cell * 2.5f;
                        is.AutoTexture(rules, game.Textures(), &wide, hit[0], hit[2]);
                    }
                }
                break;
            default:
                break;
        }
    }
    else if (stroke && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        is.EndEdit();
        stroke = false;
    }
}

// ---------------------------------------------------------------------------
// interface
// ---------------------------------------------------------------------------

void SetupTheme()
{
    // The Open Poseidon Engine look, from its banner (owner, 2026-10-11): deep navy night sea, steel-blue panels, the
    // logo's azure and cyan as the accent, silver-white lettering; squared 3 px corners with a thin azure outline,
    // like the banner's bracketed captions. Compact spacing as the other tools have (PoseidonStudio).
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    ImVec4* c = s.Colors;
    const ImVec4 navy(0.027f, 0.063f, 0.125f, 1.0f);    // #07101F  the night sea
    const ImVec4 panel(0.043f, 0.098f, 0.188f, 0.97f);  // #0B1930  panels over the island view
    const ImVec4 raised(0.071f, 0.153f, 0.282f, 1.0f);  // #122748  fields and buttons
    const ImVec4 hover(0.102f, 0.227f, 0.412f, 1.0f);   // #1A3A69
    const ImVec4 line(0.118f, 0.290f, 0.502f, 1.0f);    // #1E4A80  outlines and separators
    const ImVec4 azure = kAccent;                        // #1E9BFF  the logo's wave
    const ImVec4 cyan = kAccentHover;                    // #45C4FF  its highlights
    c[ImGuiCol_Text] = ImVec4(0.906f, 0.941f, 0.980f, 1.0f);          // #E7F0FA silver-white
    c[ImGuiCol_TextDisabled] = ImVec4(0.490f, 0.596f, 0.729f, 1.0f);  // #7D98BA
    c[ImGuiCol_WindowBg] = panel;
    c[ImGuiCol_ChildBg] = ImVec4(0.035f, 0.082f, 0.161f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.043f, 0.098f, 0.188f, 0.99f);
    c[ImGuiCol_MenuBarBg] = navy;
    c[ImGuiCol_TitleBg] = navy;
    c[ImGuiCol_TitleBgActive] = ImVec4(0.055f, 0.125f, 0.235f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = navy;
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_SeparatorHovered] = azure;
    c[ImGuiCol_SeparatorActive] = cyan;
    c[ImGuiCol_FrameBg] = raised;
    c[ImGuiCol_FrameBgHovered] = hover;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.125f, 0.290f, 0.522f, 1.0f);
    c[ImGuiCol_Button] = raised;
    c[ImGuiCol_ButtonHovered] = hover;
    c[ImGuiCol_ButtonActive] = azure;
    c[ImGuiCol_Header] = ImVec4(0.086f, 0.212f, 0.392f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.110f, 0.310f, 0.580f, 1.0f);
    c[ImGuiCol_HeaderActive] = azure;
    c[ImGuiCol_Tab] = ImVec4(0.055f, 0.125f, 0.235f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.110f, 0.310f, 0.580f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.086f, 0.243f, 0.455f, 1.0f);
    c[ImGuiCol_TabSelectedOverline] = cyan;
    c[ImGuiCol_CheckMark] = cyan;
    c[ImGuiCol_SliderGrab] = azure;
    c[ImGuiCol_SliderGrabActive] = cyan;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.102f, 0.227f, 0.412f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.125f, 0.290f, 0.522f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = azure;
    c[ImGuiCol_ResizeGrip] = ImVec4(azure.x, azure.y, azure.z, 0.25f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(azure.x, azure.y, azure.z, 0.60f);
    c[ImGuiCol_ResizeGripActive] = cyan;
    c[ImGuiCol_TextSelectedBg] = ImVec4(azure.x, azure.y, azure.z, 0.40f);
    c[ImGuiCol_NavCursor] = cyan;
    c[ImGuiCol_PlotHistogram] = azure;
    c[ImGuiCol_PlotLines] = cyan;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.02f, 0.06f, 0.60f);
    s.WindowRounding = 3.0f;
    s.ChildRounding = 3.0f;
    s.FrameRounding = 3.0f;
    s.PopupRounding = 3.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.ScrollbarRounding = 3.0f;
    s.WindowBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(7, 4);
    s.ItemSpacing = ImVec2(8, 5);
    s.ScrollbarSize = 12.0f;
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    s.DisabledAlpha = 0.45f;
    // everything above is in pixels at 96 dpi
    s.ScaleAllSizes(g_ui);
    s.FontScaleDpi = g_ui;
}

void App::SelectTool(Tool t)
{
    if (stroke)
    {
        is.EndEdit();
        stroke = false;
    }
    tool = t;
    rampActive = false;
    bridgeStarted = false;
}

void App::ToolsPanel(float top, float bottom)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::SetNextWindowPos(ImVec2(S(kMargin), top + S(kMargin)));
    ImGui::SetNextWindowSize(ImVec2(S(kLeftWidth), std::max(S(200), bottom - top - S(kMargin) * 2)));
    ImGui::Begin("Tools", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoSavedSettings);
    bool first = true;
    for (const ToolGroup& g : kToolGroups)
    {
        Section(g.title, first);
        first = false;
        const float bw = std::floor((ImGui::GetContentRegionAvail().x - st.ItemSpacing.x * (g.columns - 1)) / g.columns);
        for (int i = 0; i < g.count; i++)
        {
            const Tool t = g.tools[i];
            if (i % g.columns)
                ImGui::SameLine();
            if (Toggle(kToolNames[static_cast<int>(t)], tool == t, ImVec2(bw, S(30))))
                SelectTool(t);
            if (ImGui::BeginItemTooltip())
            {
                ImGui::PushTextWrapPos(S(280));
                ImGui::TextUnformatted(kToolHelp[static_cast<int>(t)]);
                if (ToolKey(t))
                    ImGui::TextDisabled("Key %c", ToolKey(t));
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
    }

    Section(kToolNames[static_cast<int>(tool)]);
    Hint(kToolHelp[static_cast<int>(tool)]);
    ImGui::Spacing();

    if (tool == Tool::Road || tool == Tool::Forest || tool == Tool::Bridge || tool == Tool::Region)
    {
        BuilderPanel();
    }
    else if (tool != Tool::Objects)
    {
        Row(tool == Tool::Ramp ? "Half width" : "Radius");
        ImGui::SliderFloat("##radius", &brush.radius, is.cell * 0.5f, 3000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Alt + wheel, or [ and ]");
        if (tool != Tool::Paint && tool != Tool::Ramp && tool != Tool::AutoPaint)
        {
            Row("Strength");
            ImGui::SliderFloat("##strength", &brush.strength, 0.02f, 1.0f, "%.2f");
        }
        if (tool != Tool::Paint && tool != Tool::AutoPaint)
        {
            int fo = static_cast<int>(brush.falloff);
            Row("Edge");
            if (ImGui::Combo("##falloff", &fo, "Smooth\0Linear\0Sharp peak\0Hard\0"))
                brush.falloff = static_cast<Falloff>(fo);
        }
        if (tool == Tool::SetHeight)
        {
            Row("Height");
            ImGui::DragFloat("##height", &setHeight, 0.5f, kMinHeight, kMaxHeight, "%.1f m");
            Hint("H takes the height under the cursor.");
        }
        if (tool == Tool::Noise)
        {
            Row("Feature size");
            ImGui::SliderFloat("##nscale", &noiseScale, 100.0f, 4000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
        }
        if (tool == Tool::Paint)
        {
            Row("Texture");
            ImGui::TextWrapped("%s", paintSlot >= 0 && paintSlot < kMaxTextures && !is.textures[paintSlot].empty() ? is.textures[paintSlot].c_str()
                                                                                                             : "(none chosen)");
            Hint("Choose one under Textures at the right.");
        }
        if (tool == Tool::AutoPaint && ImGui::Button("Rules...", ImVec2(-FLT_MIN, 0)))
        {
            EnsureRules();
            showAuto = true;
        }
    }
    else
    {
        ImGui::Checkbox("Place new objects", &placing);
        if (placing)
        {
            Row("Model");
            ImGui::TextWrapped("%s", placeModel.empty() ? "(choose one under Objects)" : placeModel.c_str());
            float deg = placeYaw * 180.0f / kPi;
            Row("Facing");
            if (ImGui::SliderFloat("##yaw", &deg, 0.0f, 360.0f, "%.0f deg"))
                placeYaw = deg * kPi / 180.0f;
        }
    }
    ImGui::End();
}

// What is shown, and how: a strip over the top of the view.
void App::ViewBar(float top, int width)
{
    const float left = S(kMargin * 2 + kLeftWidth), right = width - S(kMargin * 2 + kRightWidth);
    if (viewBarWidth > 0 && right - left < viewBarWidth)
        return; // a narrow window: the View menu has it all
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(4), S(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(4)));
    ImGui::SetNextWindowPos(ImVec2(std::floor((left + right) * 0.5f), top + S(kMargin)), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::Begin("viewbar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    static const char* const modes[] = {"Textures", "Heights", "Slopes"};
    static const char* const modeHelp[] = {"The ground textures", "Colours by elevation", "Red where the land is steep"};
    for (int i = 0; i < 3; i++)
    {
        if (i)
            ImGui::SameLine();
        if (Toggle(modes[i], view.mode == i))
            view.mode = i;
        ImGui::SetItemTooltip("%s", modeHelp[i]);
    }
    const float gap = S(14);
    ImGui::SameLine(0, gap);
    if (Toggle("Grid", view.grid))
        view.grid = !view.grid;
    ImGui::SetItemTooltip("The 50 m cells (G)");
    ImGui::SameLine();
    if (Toggle("Sea", view.sea))
        view.sea = !view.sea;
    ImGui::SameLine();
    if (Toggle("Objects", view.objects))
        view.objects = !view.objects;
    ImGui::SameLine();
    if (Toggle("Models", view.objects && view.models))
    {
        view.models = !(view.objects && view.models);
        view.objects = view.objects || view.models;
    }
    ImGui::SetItemTooltip("Objects as their models near the camera, as markers beyond; how near is set in the View menu");
    ImGui::SameLine();
    if (Toggle("Contours", view.contour > 0))
        view.contour = view.contour > 0 ? 0.0f : 10.0f;
    ImGui::SetItemTooltip("Contour lines; how far apart is set in the View menu");
    ImGui::SameLine(0, gap);
    if (ImGui::Button("Top"))
        cam.pitch = -1.55f;
    ImGui::SetItemTooltip("Look straight down (T)");
    ImGui::SameLine();
    if (ImGui::Button("Whole island"))
        ResetCamera();
    viewBarWidth = ImGui::GetWindowSize().x;
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void App::SidePanel(float top, float bottom, int width)
{
    const float pw = S(kRightWidth);
    ImGui::SetNextWindowPos(ImVec2(width - pw - S(kMargin), top + S(kMargin)));
    ImGui::SetNextWindowSize(ImVec2(pw, std::max(S(200), bottom - top - S(kMargin) * 2)));
    ImGui::Begin("Island", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::BeginTabBar("side"))
    {
        if (ImGui::BeginTabItem("Textures", nullptr, showTab == 0 ? ImGuiTabItemFlags_SetSelected : 0))
        {
            ImGui::Spacing();
            {
                // the texture in hand
                float uv0[2], uv1[2];
                const bool has = paintSlot >= 0 && paintSlot < kMaxTextures && !is.textures[paintSlot].empty();
                renderer.AtlasUV(has ? paintSlot : 0, uv0, uv1);
                ImGui::Image(static_cast<ImTextureID>(renderer.AtlasHandle()), ImVec2(S(44), S(44)), ImVec2(uv0[0], uv0[1]), ImVec2(uv1[0], uv1[1]));
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextUnformatted(has ? is.textures[paintSlot].c_str() : "(no texture chosen)");
                ImGui::TextDisabled("%d of %d slots used", is.UsedTextures(), kMaxTextures);
                ImGui::EndGroup();
            }
            const float plainW = ImGui::CalcTextSize("Plain").x + ImGui::GetStyle().FramePadding.x * 2;
            const float addW = ImGui::CalcTextSize("Add...").x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - plainW - addW - ImGui::GetStyle().ItemSpacing.x * 2);
            ImGui::InputTextWithHint("##pf", "filter", paletteFilter, sizeof(paletteFilter));
            ImGui::SameLine();
            if (Toggle("Plain", palettePlain))
                palettePlain = !palettePlain;
            ImGui::SetItemTooltip("Only the plain textures, without the transitions between them");
            ImGui::SameLine();
            if (ImGui::Button("Add..."))
                openAddTexture = true;
            ImGui::SetItemTooltip("Add a ground texture from the game");
            ImGui::BeginChild("palette", ImVec2(0, 0), ImGuiChildFlags_None);
            const float padPx = S(3), gapPx = S(4);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gapPx, gapPx));
            // as many to the row as fit at about 56 pixels, then stretched to fill it
            const float rowPx = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - S(2);
            const int perRow = std::max(1, static_cast<int>((rowPx + gapPx) / (S(56) + padPx * 2 + gapPx)));
            const float cellPx = std::floor((rowPx - gapPx * (perRow - 1)) / perRow - padPx * 2);
            const std::string wanted = LowerPath(paletteFilter);
            int shown = 0;
            for (int slot = 0; slot < kMaxTextures; slot++)
            {
                if (is.textures[slot].empty())
                    continue;
                if (!wanted.empty() || palettePlain)
                {
                    const std::string name = LowerPath(is.textures[slot]);
                    if (!wanted.empty() && name.find(wanted) == std::string::npos)
                        continue;
                    // plain textures have two letters to their name, transitions eight
                    const size_t from = name.find_last_of('\\') == std::string::npos ? 0 : name.find_last_of('\\') + 1;
                    const size_t dot = name.find_last_of('.');
                    if (palettePlain && slot != 0 && (dot == std::string::npos ? name.size() : dot) - from > 4)
                        continue;
                }
                float uv0[2], uv1[2];
                renderer.AtlasUV(slot, uv0, uv1);
                ImGui::PushID(slot);
                const bool on = slot == paintSlot;
                ImGui::PushStyleColor(ImGuiCol_Button, on ? kAccent : ImVec4(0, 0, 0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padPx, padPx));
                if (ImGui::ImageButton("t", static_cast<ImTextureID>(renderer.AtlasHandle()), ImVec2(cellPx, cellPx), ImVec2(uv0[0], uv0[1]),
                                       ImVec2(uv1[0], uv1[1])))
                {
                    paintSlot = slot;
                    if (tool != Tool::Paint)
                        tool = Tool::Paint;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%d  %s%s", slot, is.textures[slot].c_str(), renderer.TextureMissing(slot) ? "\n(not found in the game folder)" : "");
                if (ImGui::BeginPopupContextItem("ctx"))
                {
                    if (slot != paintSlot && ImGui::MenuItem("Replace everywhere with the chosen texture"))
                    {
                        is.BeginEdit("Replace texture");
                        is.ReplaceTexture(static_cast<uint16_t>(slot), static_cast<uint16_t>(paintSlot));
                        is.EndEdit();
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                if (++shown % perRow != 0)
                    ImGui::SameLine();
            }
            ImGui::PopStyleVar();
            if (!shown)
            {
                ImGui::NewLine();
                Hint("No texture of the island has that in its name.");
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Objects", nullptr, showTab == 1 ? ImGuiTabItemFlags_SetSelected : 0))
        {
            ImGui::Spacing();
            ImGui::TextDisabled("%d objects on the island", static_cast<int>(is.objects.size()));
            if (selected >= 0 && selected < static_cast<int>(is.objects.size()))
            {
                island::Object o = is.objects[selected];
                Section("Selected");
                ImGui::TextWrapped("%s", o.model.c_str());
                ImGui::TextDisabled("id %d   x %.1f   z %.1f", o.id, o.X(), o.Z());
                float above = o.fixed ? o.m[10] : o.above;
                Row("Height", 70.0f);
                if (ImGui::DragFloat("##above", &above, 0.05f, -500.0f, 1500.0f, o.fixed ? "%.2f m above the sea" : "%.2f m above the ground"))
                {
                    if (o.fixed)
                        o.m[10] = above;
                    else
                        o.above = above;
                    is.BeginEdit("Object height");
                    is.ChangeObject(selected, o);
                    is.EndEdit();
                }
                bool fixed = o.fixed;
                if (ImGui::Checkbox("Keeps its height (a bridge)", &fixed))
                {
                    // either way it stays where it is now
                    const float y = is.ObjectY(o);
                    o.fixed = fixed;
                    o.m[10] = y;
                    o.above = y - is.SurfaceY(o.X(), o.Z());
                    is.BeginEdit("Object height");
                    is.ChangeObject(selected, o);
                    is.EndEdit();
                }
                const float third = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f);
                if (ImGui::Button("Turn left", ImVec2(third, 0)))
                    RotateObject(selected, -kPi / 12.0f);
                ImGui::SetItemTooltip("Shift+R");
                ImGui::SameLine();
                if (ImGui::Button("Turn right", ImVec2(third, 0)))
                    RotateObject(selected, kPi / 12.0f);
                ImGui::SetItemTooltip("R");
                ImGui::SameLine();
                if (ImGui::Button("Remove", ImVec2(third, 0)))
                {
                    is.BeginEdit("Remove object");
                    is.RemoveObject(selected);
                    is.EndEdit();
                    selected = -1;
                }
                if (ImGui::Button("Place more of these", ImVec2(-1, 0)))
                {
                    placeModel = o.model;
                    placing = true;
                    tool = Tool::Objects;
                }
            }
            Section("Models");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##mf", "filter, e.g. dum or tree", modelFilter, sizeof(modelFilter));
            ImGui::Checkbox("Only those on this island", &modelsUsedOnly);
            if (usedModelsFor != is.objects.size())
            {
                usedModels.clear();
                for (const island::Object& o : is.objects)
                    usedModels.push_back(LowerPath(o.model));
                std::sort(usedModels.begin(), usedModels.end());
                usedModels.erase(std::unique(usedModels.begin(), usedModels.end()), usedModels.end());
                usedModelsFor = is.objects.size();
            }
            const std::vector<std::string>& all = modelsUsedOnly ? usedModels : game.Models();
            static std::vector<const std::string*> list;
            list.clear();
            const std::string filter = LowerPath(modelFilter);
            for (const std::string& m : all)
                if (filter.empty() || m.find(filter) != std::string::npos)
                    list.push_back(&m);
            ImGui::BeginChild("models", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(list.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++)
                {
                    const bool on = placing && *list[i] == LowerPath(placeModel);
                    if (ImGui::Selectable(list[i]->c_str(), on))
                    {
                        placeModel = *list[i];
                        placing = true;
                        tool = Tool::Objects;
                    }
                }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Map", nullptr, showTab == 2 ? ImGuiTabItemFlags_SetSelected : 0))
        {
            const float side = ImGui::GetContentRegionAvail().x;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (renderer.MinimapHandle())
                ImGui::Image(static_cast<ImTextureID>(renderer.MinimapHandle()), ImVec2(side, side));
            const float s = is.WorldSize();
            if (s > 0)
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 c(p0.x + cam.pos[0] / s * side, p0.y + (1.0f - cam.pos[2] / s) * side);
                dl->PushClipRect(p0, ImVec2(p0.x + side, p0.y + side), true);
                dl->AddCircleFilled(c, 4.0f, IM_COL32(255, 230, 80, 255));
                dl->AddLine(c, ImVec2(c.x + std::sin(cam.yaw) * 22.0f, c.y - std::cos(cam.yaw) * 22.0f), IM_COL32(255, 230, 80, 255), 2.0f);
                if (hover)
                    dl->AddCircle(ImVec2(p0.x + hit[0] / s * side, p0.y + (1.0f - hit[2] / s) * side), std::max(2.0f, brush.radius / s * side),
                                  IM_COL32(255, 255, 255, 200));
                dl->PopClipRect();
                if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    const ImVec2 m = ImGui::GetIO().MousePos;
                    const float wx = (m.x - p0.x) / side * s, wz = (1.0f - (m.y - p0.y) / side) * s;
                    // keep looking the same way, from the same height above the ground
                    const float lift = cam.pos[1] - is.SurfaceY(cam.pos[0], cam.pos[2]);
                    const float back = lift / std::max(0.2f, std::tan(-cam.pitch));
                    cam.pos[0] = wx - std::sin(cam.yaw) * back;
                    cam.pos[2] = wz - std::cos(cam.yaw) * back;
                    cam.pos[1] = std::max(0.0f, is.SurfaceY(wx, wz)) + lift;
                }
            }
            Hint("Click the map to go there.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Info", nullptr, showTab == 3 ? ImGuiTabItemFlags_SetSelected : 0))
        {
            ImGui::Spacing();
            int missing = 0;
            for (int i = 0; i < kMaxTextures; i++)
                missing += (!is.textures[i].empty() && renderer.TextureMissing(i)) ? 1 : 0;
            char value[96];
            auto line = [&](const char* name, const char* text)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", name);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", text);
            };
            if (ImGui::BeginTable("info", 2))
            {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, S(92));
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
                line("File", is.path.empty() ? "(not saved yet)" : is.path.c_str());
                line("Read as", is.sourceFormat.c_str());
                line("Saved as", "4WVR");
                snprintf(value, sizeof(value), "%d x %d cells of %.0f m", is.size, is.size, is.cell);
                line("Size", value);
                snprintf(value, sizeof(value), "%.1f x %.1f km", is.WorldSize() / 1000.0f, is.WorldSize() / 1000.0f);
                line("", value);
                snprintf(value, sizeof(value), "%.1f m", is.MinHeight());
                line("Lowest", value);
                snprintf(value, sizeof(value), "%.1f m  (limit %.0f)", is.MaxHeight(), kMaxHeight);
                line("Highest", value);
                snprintf(value, sizeof(value), "%d", static_cast<int>(is.objects.size()));
                line("Objects", value);
                snprintf(value, sizeof(value), "%d of %d", is.UsedTextures(), kMaxTextures);
                line("Textures", value);
                ImGui::EndTable();
            }
            if (missing)
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%d textures not found (shown red)", missing);
            Section("Game folder");
            ImGui::TextWrapped("%s", game.Folder().empty() ? "None: textures cannot be shown, and there are no models to build with." : WholePath(game.Folder()).c_str());
            if (ImGui::Button("Choose the game folder...", ImVec2(-FLT_MIN, 0)))
            {
                const std::string dir = FileDialog(2, L"The game's folder (with DTA and AddOns)", nullptr, nullptr, nullptr);
                if (!dir.empty() && MountGame(dir, true))
                    SaveSettings();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        showTab = -1;
    }
    ImGui::End();
}

void App::Dialogs()
{
    if (pending != Ask::None)
        ImGui::OpenPopup("Unsaved changes");
    if (BeginDialog("Unsaved changes"))
    {
        ImGui::TextWrapped("The island has changes that are not saved.");
        ImGui::Spacing();
        const Ask what = pending;
        bool go = false;
        if (ImGui::Button("Save", ImVec2(S(120), 0)))
            go = Save(false);
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(S(120), 0)))
        {
            is.modified = false;
            go = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(120), 0)))
        {
            pending = Ask::None;
            ImGui::CloseCurrentPopup();
        }
        if (go)
        {
            pending = Ask::None;
            ImGui::CloseCurrentPopup();
            if (what == Ask::Exit)
                running = false;
            else if (what == Ask::New)
                openNew = true;
            else if (what == Ask::Open)
                RequestOpen();
        }
        EndDialog();
    }

    static const int kSizes[] = {32, 64, 128, 256, 512, 1024};
    if (openNew)
    {
        ImGui::OpenPopup("New island");
        openNew = false;
    }
    if (BeginDialog("New island"))
    {
        ImGui::Combo("Size", &newSizeIndex,
                     "32 cells (1.6 km)\0""64 cells (3.2 km)\0""128 cells (6.4 km)\0""256 cells (12.8 km, as the game's)\0""512 cells (25.6 km)\0""1024 cells (51.2 km)\0");
        Section("Start with");
        ImGui::RadioButton("Generated hills and coast", &newMode, 0);
        ImGui::RadioButton("Flat land", &newMode, 1);
        ImGui::RadioButton("A height map (picture or grid)", &newMode, 2);
        if (newMode == 0)
        {
            ImGui::InputInt("Seed", &genSeed);
            ImGui::SliderFloat("Highest hills", &genPeak, 20.0f, 1200.0f, "%.0f m");
            ImGui::SliderFloat("Roughness", &genRough, 0.3f, 0.7f);
            ImGui::SliderFloat("Sea depth", &genSea, 5.0f, 200.0f, "%.0f m");
        }
        else if (newMode == 1)
        {
            ImGui::DragFloat("Flat at", &newHeight, 0.5f, kMinHeight, kMaxHeight, "%.1f m");
        }
        else
        {
            if (ImGui::Button("Choose height map...", ImVec2(-1, 0)) && ChooseHeightMap())
            {
                // the island size nearest the picture's
                const int n = std::max(heightImage.width, heightImage.height);
                newSizeIndex = 0;
                while (newSizeIndex < 5 && kSizes[newSizeIndex] < n)
                    newSizeIndex++;
            }
            if (heightImage.width > 0)
            {
                ImGui::TextWrapped("%s", importFile.c_str());
                ImGui::Text("%d x %d points, %d bit%s", heightImage.width, heightImage.height, heightImage.bits,
                            heightImage.metres ? ", in metres" : "");
                if (heightImage.metres)
                {
                    ImGui::Text("Heights %.1f m to %.1f m, as in the file", heightImage.low, heightImage.high);
                }
                else
                {
                    ImGui::DragFloat("Black is", &importLow, 0.5f, kMinHeight, kMaxHeight, "%.1f m");
                    ImGui::DragFloat("White is", &importHigh, 0.5f, kMinHeight, kMaxHeight, "%.1f m");
                }
                ImGui::SliderInt("Smoothing", &importSmooth, 0, 8, importSmooth ? "%d passes" : "none");
                if (heightImage.bits <= 8)
                    Hint("An 8-bit picture has only 256 heights: smoothing hides the steps.");
            }
            else
            {
                Hint("PNG (8 or 16 bit), PGM, RAW 16 bit, ESRI ASC, BMP, TGA, JPG.");
            }
        }
        Hint("Larger islands take longer for the game to load.");
        const bool ready = newMode != 2 || heightImage.width > 0;
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Create", ImVec2(S(120), 0)))
        {
            // the ground of the island open before, if there was one
            std::string ground;
            for (int i = 1; i < static_cast<int>(is.textures.size()) && ground.empty(); i++)
                ground = is.textures[i];
            is.New(kSizes[newSizeIndex], newMode == 1 ? newHeight : 0.0f, ground);
            if (newMode == 0)
                is.Generate(static_cast<unsigned>(genSeed), genPeak, genRough, genSea);
            else if (newMode == 2)
                ApplyHeightMap();
            is.modified = newMode != 1;
            AfterLoad();
            Status("New island: " + std::to_string(is.size) + " x " + std::to_string(is.size) + " cells");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(120), 0)))
            ImGui::CloseCurrentPopup();
        EndDialog();
    }

    if (openGenerate)
    {
        ImGui::OpenPopup("Generate land");
        openGenerate = false;
    }
    if (BeginDialog("Generate land"))
    {
        Hint("Replaces every elevation of the island. It can be undone.");
        ImGui::InputInt("Seed", &genSeed);
        ImGui::SliderFloat("Highest hills", &genPeak, 20.0f, 1200.0f, "%.0f m");
        ImGui::SliderFloat("Roughness", &genRough, 0.3f, 0.7f);
        ImGui::SliderFloat("Sea depth", &genSea, 5.0f, 200.0f, "%.0f m");
        if (ImGui::Button("Generate", ImVec2(S(120), 0)))
        {
            is.BeginEdit("Generate land");
            is.Generate(static_cast<unsigned>(genSeed), genPeak, genRough, genSea);
            is.EndEdit();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(S(120), 0)))
            ImGui::CloseCurrentPopup();
        EndDialog();
    }

    if (openScale)
    {
        ImGui::OpenPopup("Whole island");
        openScale = false;
    }
    if (BeginDialog("Whole island"))
    {
        const float act = S(190);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - act - ImGui::GetStyle().ItemSpacing.x);
        ImGui::SliderInt("##passes", &smoothPasses, 1, 10, smoothPasses == 1 ? "%d pass" : "%d passes");
        ImGui::SameLine();
        if (ImGui::Button("Smooth everything", ImVec2(act, 0)))
        {
            is.BeginEdit("Smooth island");
            is.SmoothAll(smoothPasses);
            is.EndEdit();
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - act - ImGui::GetStyle().ItemSpacing.x);
        ImGui::DragFloat("##factor", &scaleFactor, 0.01f, 0.05f, 10.0f, "x %.2f");
        ImGui::SameLine();
        if (ImGui::Button("Scale every height", ImVec2(act, 0)))
        {
            is.BeginEdit("Scale heights");
            is.ScaleAll(scaleFactor);
            is.EndEdit();
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - act - ImGui::GetStyle().ItemSpacing.x);
        ImGui::DragFloat("##metres", &offsetMetres, 0.25f, -500.0f, 500.0f, "%+.2f m");
        ImGui::SameLine();
        if (ImGui::Button("Raise or lower all", ImVec2(act, 0)))
        {
            is.BeginEdit("Offset heights");
            is.OffsetAll(offsetMetres);
            is.EndEdit();
        }
        Hint("Each can be undone.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(S(120), 0)))
            ImGui::CloseCurrentPopup();
        EndDialog();
    }

    if (openImport)
    {
        ImGui::OpenPopup("Import height map");
        openImport = false;
    }
    if (BeginDialog("Import height map"))
    {
        ImGui::TextWrapped("%s", importFile.c_str());
        ImGui::Text("%d x %d points, %d bit%s", heightImage.width, heightImage.height, heightImage.bits,
                    heightImage.metres ? ", in metres" : "");
        if (heightImage.metres)
        {
            ImGui::Text("Heights %.1f m to %.1f m, as in the file", heightImage.low, heightImage.high);
        }
        else
        {
            ImGui::DragFloat("Black is", &importLow, 0.5f, kMinHeight, kMaxHeight, "%.1f m");
            ImGui::DragFloat("White is", &importHigh, 0.5f, kMinHeight, kMaxHeight, "%.1f m");
        }
        ImGui::SliderInt("Smoothing", &importSmooth, 0, 8, importSmooth ? "%d passes" : "none");
        Hint("It is stretched over the whole island, north at the top.");
        if (ImGui::Button("Import", ImVec2(S(120), 0)))
        {
            is.BeginEdit("Import height map");
            ApplyHeightMap();
            is.EndEdit();
            Status("Height map imported");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(120), 0)))
            ImGui::CloseCurrentPopup();
        EndDialog();
    }

    if (openRegionImport)
    {
        ImGui::OpenPopup("Import a region");
        openRegionImport = false;
    }
    if (BeginDialog("Import a region"))
    {
        ImGui::TextWrapped("%s", regionFile.c_str());
        ImGui::Text("%d x %d cells, %d objects", regionInfo.width, regionInfo.height, static_cast<int>(regionSrc.objects.size()));
        if (regionInfo.marked)
            ImGui::Checkbox("Put it where it came from", &regionAtOrigin);
        const int dx = regionAtOrigin && regionInfo.marked ? regionInfo.fromX : std::min(regionA[0], regionB[0]);
        const int dz = regionAtOrigin && regionInfo.marked ? regionInfo.fromZ : std::min(regionA[1], regionB[1]);
        ImGui::Text("South-west corner at cell %d, %d", dx, dz);
        ImGui::Checkbox("Elevations", &importOpt.heights);
        ImGui::Checkbox("Ground textures", &importOpt.textures);
        ImGui::Checkbox("Objects", &importOpt.objects);
        if (importOpt.objects)
            ImGui::Checkbox("Remove what stands there now", &importOpt.clearObjects);
        if (ImGui::Button("Import", ImVec2(S(120), 0)))
        {
            is.BeginEdit("Import region");
            ImportRegion(is, regionSrc, regionInfo, dx, dz, importOpt);
            is.EndEdit();
            regionA[0] = dx;
            regionA[1] = dz;
            regionB[0] = dx + regionInfo.width - 1;
            regionB[1] = dz + regionInfo.height - 1;
            regionSet = true;
            selected = -1;
            Status("Region imported at cell " + std::to_string(dx) + ", " + std::to_string(dz));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(120), 0)))
            ImGui::CloseCurrentPopup();
        EndDialog();
    }

    if (openAddTexture)
    {
        ImGui::OpenPopup("Add a ground texture");
        openAddTexture = false;
    }
    if (BeginDialog("Add a ground texture", 540.0f))
    {
        if (game.Textures().empty())
            ImGui::TextWrapped("No game folder is set (Info tab), so there is nothing to choose from.");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##tf", "filter, e.g. landtext or abel\\", textureFilter, sizeof(textureFilter));
        static std::vector<const std::string*> list;
        list.clear();
        const std::string filter = LowerPath(textureFilter);
        for (const std::string& t : game.Textures())
            if (filter.empty() || t.find(filter) != std::string::npos)
                list.push_back(&t);
        ImGui::Text("%d textures", static_cast<int>(list.size()));
        ImGui::BeginChild("tl", ImVec2(0, S(380)), ImGuiChildFlags_Borders);
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(list.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++)
                if (ImGui::Selectable(list[i]->c_str()))
                {
                    is.BeginEdit("Add texture");
                    const int slot = is.TextureSlot(*list[i]);
                    is.EndEdit();
                    if (slot < 0)
                    {
                        Status("All 512 texture slots are in use");
                    }
                    else
                    {
                        renderer.UpdateTexture(slot);
                        paintSlot = slot;
                        tool = Tool::Paint;
                        Status("Texture " + *list[i] + " is slot " + std::to_string(slot));
                    }
                    ImGui::CloseCurrentPopup();
                }
        ImGui::EndChild();
        if (ImGui::Button("Close", ImVec2(S(120), 0)))
            ImGui::CloseCurrentPopup();
        EndDialog();
    }

    if (openKeys)
    {
        ImGui::OpenPopup("Keys and mouse");
        openKeys = false;
    }
    if (BeginDialog("Keys and mouse", 520.0f))
    {
        static const char* const keys[][2] = {
            {"Right drag", "look around"},
            {"Middle drag", "slide over the land"},
            {"Wheel", "closer / further, towards the cursor"},
            {"W A S D, Q E", "fly; Shift = fast"},
            {"T", "look straight down"},
            {"G", "cell grid"},
            {"", ""},
            {"1 .. 9, 0", "tools, in the order of the panel; 0 = Objects"},
            {"Alt + wheel, [ ]", "brush size"},
            {"Shift, Ctrl", "Raise / Lower: the other way, smooth"},
            {"Alt + click", "Paint: pick the texture up from the land"},
            {"H", "Set height: take the height under the cursor"},
            {"Enter, Backspace, Esc", "Road: lay it, take a point back, start again"},
            {"R, Shift+R, Del", "Objects: turn, turn back, remove"},
            {"", ""},
            {"Ctrl+Z, Ctrl+Y", "undo, redo (200 steps)"},
            {"Ctrl+S, Ctrl+Shift+S", "save, save as"},
            {"Ctrl+N, Ctrl+O", "new, open"},
        };
        if (ImGui::BeginTable("keys", 2))
        {
            ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, S(180));
            ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
            for (const auto& k : keys)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(k[0]);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", k[1]);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(S(120), 0)))
            ImGui::CloseCurrentPopup();
        EndDialog();
    }

    if (openAbout)
    {
        ImGui::OpenPopup("About");
        openAbout = false;
    }
    if (BeginDialog("About"))
    {
        ImGui::TextColored(kAccentHover, "OPEN POSEIDON ENGINE");
        if (g_bold)
            ImGui::PushFont(g_bold, 0.0f);
        ImGui::TextUnformatted("Island Editor");
        if (g_bold)
            ImGui::PopFont();
        ImGui::TextWrapped("For the islands of Cold War Assault / Operation Flashpoint and Open Poseidon.");
        ImGui::Spacing();
        Hint("Opens 4WVR, 3WVR and binarized (OPRW) islands; saves 4WVR, which the games load directly. The file being replaced is "
             "kept as <name>.wrp.bak.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(S(120), 0)))
            ImGui::CloseCurrentPopup();
        EndDialog();
    }
}

void App::Interface(int width, int height)
{
    ImGuiIO& io = ImGui::GetIO();
    float top = 0;
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New...", "Ctrl+N"))
                RequestNew();
            if (ImGui::MenuItem("Open...", "Ctrl+O"))
                RequestOpen();
            if (ImGui::MenuItem("Save", "Ctrl+S", false, is.size > 0))
                Save(false);
            if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S", false, is.size > 0))
                Save(true);
            ImGui::Separator();
            if (ImGui::MenuItem("Import height map...", nullptr, false, is.size > 0) && ChooseHeightMap())
                openImport = true;
            if (ImGui::MenuItem("Export height map (PGM)...", nullptr, false, is.size > 0))
            {
                const std::string file = FileDialog(1, L"Export height map", L"Greyscale PGM (*.pgm)", L"*.pgm", L"pgm");
                if (!file.empty())
                {
                    float lo, hi;
                    std::string error;
                    char msg[256];
                    if (is.ExportHeightmap(file, lo, hi, error))
                        snprintf(msg, sizeof(msg), "Height map written: black = %.1f m, white = %.1f m", lo, hi);
                    else
                        snprintf(msg, sizeof(msg), "Not written: %s", error.c_str());
                    Status(msg);
                }
            }
            if (IsReskinAddon(is) && ImGui::MenuItem("Save and pack the addon again"))
            {
                std::string pbo, error;
                Status(RepackAddon(is, pbo, error) ? "Packed " + pbo : "Not packed: " + error);
                UpdateTitle();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Game folder..."))
            {
                const std::string dir = FileDialog(2, L"The game's folder (with DTA and AddOns)", nullptr, nullptr, nullptr);
                if (!dir.empty() && MountGame(dir, true))
                    SaveSettings();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit"))
                RequestExit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            char label[160];
            snprintf(label, sizeof(label), "Undo %s", is.UndoName());
            if (ImGui::MenuItem(label, "Ctrl+Z", false, is.CanUndo()))
                is.Undo();
            snprintf(label, sizeof(label), "Redo %s", is.RedoName());
            if (ImGui::MenuItem(label, "Ctrl+Y", false, is.CanRedo()))
                is.Redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Whole island: smooth, scale, offset...", nullptr, false, is.size > 0))
                openScale = true;
            if (ImGui::MenuItem("Generate land...", nullptr, false, is.size > 0))
                openGenerate = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Auto texture...", nullptr, false, is.size > 0))
            {
                EnsureRules();
                showAuto = true;
            }
            if (ImGui::MenuItem("Reskin (snow, desert, ...)...", nullptr, false, is.size > 0))
            {
                SetReskinDefaults();
                showReskin = true;
                UpdateReskinPreview();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            if (ImGui::MenuItem("Textures", nullptr, view.mode == 0))
                view.mode = 0;
            if (ImGui::MenuItem("Heights", nullptr, view.mode == 1))
                view.mode = 1;
            if (ImGui::MenuItem("Slopes", nullptr, view.mode == 2))
                view.mode = 2;
            ImGui::Separator();
            ImGui::MenuItem("Cell grid", "G", &view.grid);
            ImGui::MenuItem("Sea", nullptr, &view.sea);
            ImGui::MenuItem("Objects", nullptr, &view.objects);
            ImGui::MenuItem("Objects as their models", nullptr, &view.models, view.objects);
            bool contours = view.contour > 0;
            if (ImGui::MenuItem("Contour lines", nullptr, &contours))
                view.contour = contours ? 10.0f : 0.0f;
            ImGui::Separator();
            ImGui::PushItemWidth(S(230));
            if (contours)
                ImGui::SliderFloat("##contour", &view.contour, 1.0f, 100.0f, "a contour every %.0f m");
            ImGui::SliderAngle("##sun", &view.sunYaw, 0.0f, 360.0f, "light from %.0f deg");
            ImGui::SliderFloat("##range", &view.objectRange, 500.0f, 12000.0f, "objects shown to %.0f m", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("##mrange", &view.modelRange, 200.0f, 4000.0f, "as models to %.0f m", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("##mfine", &view.modelFine, 50.0f, 1500.0f, "in full detail to %.0f m", ImGuiSliderFlags_Logarithmic);
            ImGui::PopItemWidth();
            ImGui::Separator();
            if (ImGui::MenuItem("Look straight down", "T", false, is.size > 0))
                cam.pitch = -1.55f;
            if (ImGui::MenuItem("Whole island", nullptr, false, is.size > 0))
                ResetCamera();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("Keys and mouse"))
                openKeys = true;
            if (ImGui::MenuItem("About"))
                openAbout = true;
            ImGui::EndMenu();
        }
        // the name at the top of the window, centred in the menu bar (owner, 2026-10-11)
        {
            const char* brand = "OPEN POSEIDON ENGINE";
            const char* tool = "  -  Island Editor";
            const float wide = ImGui::CalcTextSize(brand).x + ImGui::CalcTextSize(tool).x;
            const float x = (ImGui::GetWindowSize().x - wide) * 0.5f;
            if (x > ImGui::GetCursorPosX() + S(24))
            {
                ImGui::SetCursorPosX(x);
                ImGui::TextColored(kAccentHover, "%s", brand);
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(tool);
            }
        }
        top = ImGui::GetWindowSize().y;
        ImGui::EndMainMenuBar();
    }

    // shortcuts
    if (!io.WantTextInput)
    {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            is.Undo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
            is.Redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            Save(io.KeyShift);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
            RequestOpen();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false))
            RequestNew();
        if (!io.KeyCtrl)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_G, false))
                view.grid = !view.grid;
            if (ImGui::IsKeyPressed(ImGuiKey_T, false))
                cam.pitch = -1.55f;
            if (ImGui::IsKeyPressed(ImGuiKey_H, false) && hover)
                setHeight = hit[1];
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
                brush.radius = std::max(is.cell * 0.5f, brush.radius / 1.15f);
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket))
                brush.radius = std::min(3000.0f, brush.radius * 1.15f);
            for (int i = 0; i < 9; i++)
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false))
                    SelectTool(static_cast<Tool>(i));
            if (ImGui::IsKeyPressed(ImGuiKey_0, false))
                SelectTool(Tool::Objects);
        }
    }

    // status bar
    const float barH = S(kStatusHeight);
    ImGui::SetNextWindowPos(ImVec2(0, height - barH));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width), barH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), std::floor((barH - ImGui::GetFontSize()) * 0.5f)));
    ImGui::Begin("status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    // what stands at the right: the last message, or the island in short
    std::string rightText;
    const bool message = !status.empty() && ImGui::GetTime() - statusTime < 8.0;
    if (message)
    {
        rightText = status;
    }
    else if (is.size > 0)
    {
        char summary[160];
        snprintf(summary, sizeof(summary), "%d x %d cells      %d objects%s", is.size, is.size, static_cast<int>(is.objects.size()),
                 is.modified ? "      not saved" : "");
        rightText = summary;
    }
    const float rightW = ImGui::CalcTextSize(rightText.c_str()).x;
    float leftEnd = 0;
    if (is.size <= 0)
    {
        ImGui::TextDisabled("Open an island (File > Open) or make a new one (File > New).");
        leftEnd = ImGui::GetItemRectMax().x;
    }
    else if (hover)
    {
        const int cx = static_cast<int>(hit[0] / is.cell), cz = static_cast<int>(hit[2] / is.cell);
        const int slot = is.InRange(cx, cz) ? is.tex[static_cast<size_t>(cz) * is.size + cx] : 0;
        bool first = true;
        auto part = [&](const char* name, const char* fmt, ...)
        {
            char text[160];
            va_list args;
            va_start(args, fmt);
            vsnprintf(text, sizeof(text), fmt, args);
            va_end(args);
            if (!first)
                ImGui::SameLine(0, S(20));
            first = false;
            ImGui::TextDisabled("%s", name);
            ImGui::SameLine(0, S(6));
            ImGui::TextUnformatted(text);
        };
        part("x", "%.0f", hit[0]);
        part("z", "%.0f", hit[2]);
        part("height", "%.1f m", hit[1]);
        part("cell", "%d, %d", cx, cz);
        part("ground", "%s", is.textures[slot].c_str());
        leftEnd = ImGui::GetItemRectMax().x;
    }
    else
    {
        static const char* const how = "Right drag: look      Middle drag: slide      Wheel: closer / further      W A S D Q E: fly";
        if (ImGui::CalcTextSize(how).x + rightW + S(70) < width)
            ImGui::TextDisabled("%s", how);
        else
            ImGui::TextUnformatted(" ");
        leftEnd = ImGui::GetItemRectMax().x;
    }
    if (!rightText.empty())
    {
        ImGui::SameLine(std::max(leftEnd + S(24), width - rightW - S(16)));
        if (message)
            ImGui::TextColored(ImVec4(0.6f, 0.82f, 1.0f, 1.0f), "%s", rightText.c_str());
        else
            ImGui::TextDisabled("%s", rightText.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    if (is.size > 0)
    {
        ToolsPanel(top, height - barH);
        SidePanel(top, height - barH, width);
        ViewBar(top, width);
        AutoTextureWindow();
        ReskinWindow();
    }
    else
    {
        ImGui::SetNextWindowPos(ImVec2(width * 0.5f, height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(24)));
        ImGui::Begin("welcome", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
        ImGui::TextColored(kAccentHover, "OPEN POSEIDON ENGINE"); // the banner's caption, in its cyan
        if (g_bold)
            ImGui::PushFont(g_bold, S(24) / g_ui);
        ImGui::TextUnformatted("Island Editor");
        if (g_bold)
            ImGui::PopFont();
        ImGui::TextDisabled("Cold War Assault / Operation Flashpoint / Open Poseidon islands");
        ImGui::Spacing();
        ImGui::Spacing();
        const ImVec2 big(std::max(S(320), ImGui::GetItemRectSize().x), S(38));
        if (Toggle("Open an island...", true, big))
            RequestOpen();
        if (ImGui::Button("New island...", big))
            RequestNew();
        if (ImGui::Button(game.Folder().empty() ? "Choose the game folder..." : "Change the game folder...", big))
        {
            const std::string dir = FileDialog(2, L"The game's folder (with DTA and AddOns)", nullptr, nullptr, nullptr);
            if (!dir.empty() && MountGame(dir, true))
                SaveSettings();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("%s", game.Folder().empty() ? "No game folder yet: textures cannot be shown." : WholePath(game.Folder()).c_str());
        ImGui::End();
        ImGui::PopStyleVar();
    }
    Dialogs();
}

void App::Frame(float dt, int width, int height)
{
    HandleView(dt, width, height);
    if (is.size > 0)
        HandleTool(dt);
    Interface(width, height);

    SyncTextures();
    int x0, z0, x1, z1;
    if (is.TakeDirty(x0, z0, x1, z1))
    {
        renderer.TerrainChanged(x0, z0, x1, z1);
        minimapDirty = true;
        UpdateTitle();
    }
    if (minimapDirty && ImGui::GetTime() - minimapTime > 0.4 && !stroke)
    {
        renderer.UpdateMinimap(view);
        minimapDirty = false;
        minimapTime = ImGui::GetTime();
    }

    BrushView bv;
    if (hover && is.size > 0 && tool != Tool::Objects && tool != Tool::Road && tool != Tool::Bridge && tool != Tool::Region)
    {
        bv.shown = true;
        bv.x = hit[0];
        bv.z = hit[2];
        bv.radius = brush.radius;
        const bool down = (tool == Tool::Lower) != ImGui::GetIO().KeyShift;
        bv.r = down && (tool == Tool::Raise || tool == Tool::Lower) ? 1.0f : 0.4f;
        bv.g = (tool == Tool::Paint || tool == Tool::AutoPaint) ? 1.0f : 0.8f;
        bv.b = down && (tool == Tool::Raise || tool == Tool::Lower) ? 0.4f : 1.0f;
        if (rampActive)
        {
            bv.line = true;
            bv.lx0 = rampStart[0];
            bv.lz0 = rampStart[2];
            bv.lx1 = hit[0];
            bv.lz1 = hit[2];
        }
    }
    device->Clear(0, nullptr, GXCLEAR_TARGET | GXCLEAR_ZBUFFER | GXCLEAR_STENCIL,
                  is.size > 0 ? GX_COLOR_XRGB(120, 160, 205) : GX_COLOR_XRGB(7, 16, 32), 1.0f, 0); // sky, or the empty desk (Open Poseidon navy)
    renderer.Draw(cam, view, bv, width, height, tool == Tool::Objects ? selected : -1, &overlay);
}

// ---------------------------------------------------------------------------
// checks that need no window
// ---------------------------------------------------------------------------

int SelfTest(const std::string& file)
{
    int failed = 0;
    auto check = [&](bool ok, const char* what)
    {
        printf("[%s] %s\n", ok ? "ok  " : "FAIL", what);
        failed += ok ? 0 : 1;
    };
    Island a;
    std::string error;
    if (!GameData::LoadIsland(file, a, error))
    {
        printf("cannot load %s: %s\n", file.c_str(), error.c_str());
        return 2;
    }
    printf("%s: %s, %d cells, %d objects, %d textures, heights %.1f .. %.1f\n", file.c_str(), a.sourceFormat.c_str(), a.size,
           static_cast<int>(a.objects.size()), a.UsedTextures(), a.MinHeight(), a.MaxHeight());
    const std::string tmp = (fs::temp_directory_path() / "island_editor_selftest.wrp").string();
    check(a.Save(tmp, error), "save as 4WVR");
    Island b;
    check(GameData::LoadIsland(tmp, b, error), "load what was saved");
    check(b.size == a.size && b.objects.size() == a.objects.size(), "same size and object count");
    // an Arma / DayZ world is not 50 m a cell; 4WVR cannot say so, the .cell file beside it does
    printf("     cells of %.2f m, %.2f m after the reload\n", a.cell, b.cell);
    check(std::fabs(b.cell - a.cell) < 1e-3f, "same cell size");
    float worstH = 0, worstY = 0;
    for (size_t i = 0; i < a.height.size() && i < b.height.size(); i++)
        worstH = std::max(worstH, std::fabs(a.height[i] - b.height[i]));
    for (size_t i = 0; i < a.objects.size() && i < b.objects.size(); i++)
        worstY = std::max(worstY, std::fabs(a.objects[i].m[10] - (b.objects[i].m[10])));
    printf("     largest height difference %.4f m, largest object height difference %.4f m\n", worstH, worstY);
    check(worstH <= kHeightStep * 0.51f, "heights within half a step (2.25 cm)");
    check(worstY <= 0.06f, "objects within 6 cm of where they stood");
    check(a.tex == b.tex, "same texture of every cell");
    bool sameNames = true, sameModels = true;
    for (int i = 0; i < kMaxTextures; i++)
        sameNames = sameNames && LowerPath(a.textures[i]) == LowerPath(b.textures[i]);
    for (size_t i = 0; i < a.objects.size() && i < b.objects.size(); i++)
        sameModels = sameModels && a.objects[i].model == b.objects[i].model && a.objects[i].id == b.objects[i].id;
    check(b.renumbered == 0, "no repeated object ids in what was saved");
    check(sameNames, "same texture table");
    check(sameModels, "same models and ids");

    // every tool, then undo back to the start
    const std::vector<float> before = b.height;
    const std::vector<uint16_t> texBefore = b.tex;
    Brush br;
    br.radius = 300;
    const float c = b.WorldSize() * 0.5f;
    b.BeginEdit("raise");
    b.Raise(c, c, br, 25);
    b.EndEdit();
    check(b.H(b.size / 2, b.size / 2) > before[static_cast<size_t>(b.size / 2) * b.size + b.size / 2] + 20, "raise lifts the centre");
    b.BeginEdit("smooth");
    b.Smooth(c, c, br, 1);
    b.EndEdit();
    b.BeginEdit("flatten");
    b.Flatten(c, c, br, 50, 1);
    b.EndEdit();
    b.BeginEdit("noise");
    b.Noise(c, c, br, 10, 300, 7);
    b.EndEdit();
    b.BeginEdit("erode");
    b.Erode(c, c, br, 1);
    b.EndEdit();
    b.BeginEdit("ramp");
    b.Ramp(c - 400, c, 10, c + 400, c, 80, br);
    b.EndEdit();
    b.BeginEdit("paint");
    b.Paint(c, c, br, 0);
    b.EndEdit();
    b.BeginEdit("object");
    island::Object o;
    o.id = b.NewObjectId();
    o.model = "data3d\\test.p3d";
    const int oi = b.AddObject(o);
    b.EndEdit();
    check(oi == static_cast<int>(a.objects.size()), "object added at the end");
    int undone = 0;
    while (b.CanUndo())
    {
        b.Undo();
        undone++;
    }
    check(undone >= 5 && undone <= 8, "the steps undo one by one");
    check(b.height == before && b.tex == texBefore && b.objects.size() == a.objects.size(), "undo restores everything");
    while (b.CanRedo())
        b.Redo();
    check(b.objects.size() == a.objects.size() + 1, "redo brings the object back");
    {
        // a change of every object at once is one step
        const size_t count = b.objects.size();
        island::ObjectList half(b.objects.begin(), b.objects.begin() + count / 2);
        b.BeginEdit("thin out");
        b.ReplaceObjects(std::move(half));
        b.RenameTexture(1, "test" "\\" "renamed.paa");
        b.EndEdit();
        const bool gone = b.objects.size() == count / 2;
        b.Undo();
        check(gone && b.objects.size() == count && LowerPath(b.textures[1]) == LowerPath(a.textures[1]),
              "replacing all objects and renaming a texture undo together");
    }
    std::error_code ec;
    fs::remove(tmp, ec);
    fs::remove(tmp + ".bak", ec);
    printf(failed ? "%d check(s) FAILED\n" : "all checks passed\n", failed);
    return failed ? 1 : 0;
}

bool SaveScreenshot(Dx12::Device* device, int width, int height, const std::string& file)
{
    Dx12::Surface* back = nullptr;
    Dx12::Surface* sys = nullptr;
    bool ok = false;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, GXBACKBUFFER_TYPE_MONO, &back)) &&
        SUCCEEDED(device->CreateOffscreenPlainSurface(width, height, GXFMT_A8R8G8B8, GXPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(device->GetRenderTargetData(back, sys)))
    {
        GxLockedRect lr = {};
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, GXLOCK_READONLY)))
        {
            std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
            for (int y = 0; y < height; y++)
            {
                const uint8_t* s = static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
                uint8_t* d = &rgba[static_cast<size_t>(y) * width * 4];
                for (int x = 0; x < width; x++)
                {
                    d[x * 4] = s[x * 4 + 2];
                    d[x * 4 + 1] = s[x * 4 + 1];
                    d[x * 4 + 2] = s[x * 4];
                    d[x * 4 + 3] = 255;
                }
            }
            ok = Poseidon::PNGWriter::WriteRGBA(file.c_str(), width, height, rgba.data());
        }
    }
    if (sys)
        sys->Release();
    if (back)
        back->Release();
    return ok;
}

// What the command line can do to an island before it is shown or saved.
struct CliEdits
{
    std::string heightmap;
    int size = 0;
    float low = -40.0f, high = 400.0f;
    int smooth = -1;
    bool autoTexture = false;
    std::string set;
    std::string reskin, addon, out;
    bool install = false;
    // builders
    std::vector<float> road, forest, bridge;
    std::string roadKind, forestKind, exportRegion, importRegion;
    std::vector<int> region, importAt;
    bool noCaps = false, clearForest = false, replant = false;
    bool Any() const
    {
        return !heightmap.empty() || autoTexture || !reskin.empty() || !road.empty() || !forest.empty() || !bridge.empty() ||
               !exportRegion.empty() || !importRegion.empty() || replant;
    }
};

bool ApplyCliEdits(Island& is, GameData& game, const CliEdits& e, const std::string& sourceFile, std::string& log)
{
    char line[512];
    if (!e.heightmap.empty())
    {
        HeightImage img;
        std::string error;
        if (!LoadHeightImage(e.heightmap, img, error))
        {
            log += "cannot read " + e.heightmap + ": " + error + "\n";
            return false;
        }
        int size = e.size;
        if (size <= 0)
        {
            size = 32;
            while (size < 1024 && size < std::max(img.width, img.height))
                size *= 2;
        }
        is.New(size, 0.0f, std::string());
        is.ApplyHeights(img.values.data(), img.width, img.height, e.low, e.high, img.metres);
        const int smooth = e.smooth >= 0 ? e.smooth : (img.bits <= 8 ? 2 : 0);
        if (smooth > 0)
            is.SmoothAll(smooth);
        is.modified = true;
        snprintf(line, sizeof(line), "height map %d x %d (%d bit) -> island of %d cells, heights %.1f .. %.1f m\n", img.width,
                 img.height, img.bits, size, is.MinHeight(), is.MaxHeight());
        log += line;
    }
    if (e.autoTexture)
    {
        std::vector<TextureSet>& sets = game.Sets();
        if (sets.empty())
        {
            log += "auto texture: no texture sets (is --game given?)\n";
            return false;
        }
        int index = game.SetOf(is);
        for (size_t i = 0; i < sets.size(); i++)
            if (!e.set.empty() && sets[i].folder == LowerPath(e.set + "\\"))
                index = static_cast<int>(i);
        if (index < 0)
            index = 0;
        game.LoadSetColours(sets[index]);
        AutoTextureRules rules;
        static const char* const how[] = {"learnt from the island", "learnt from the game's island of that set", "guessed by colour"};
        log += std::string("roles ") + how[game.RolesFor(is, sets[index], rules)] + "\n";
        rules.FitHeights(is.MaxHeight());
        is.BeginEdit("Auto texture");
        const AutoTextureResult r = is.AutoTexture(rules, game.Textures());
        is.EndEdit();
        snprintf(line, sizeof(line),
                 "auto texture, set %s: beach %s, shore %s, grass %s / %s, hills %s (from %.0f m), rock %s, peaks %s (from %.0f m), forest %s\n"
                 "  %d cells changed, %d transitions, %d without a fitting transition, %d skipped (table full); %d textures in use\n",
                 rules.folder.c_str(), rules.beach.c_str(), rules.shore.c_str(), rules.grass.c_str(), rules.grassAlt.c_str(),
                 rules.hill.c_str(), rules.hillFrom, rules.rock.c_str(), rules.peak.c_str(), rules.peakFrom, rules.forest.c_str(),
                 r.changed, r.transitions, r.noTransition,
                 r.tableFull, is.UsedTextures());
        log += line;
    }
    if (!e.reskin.empty())
    {
        const std::vector<ReskinStyle>& styles = ReskinStyles();
        ReskinOptions opt;
        opt.style = -1;
        for (size_t i = 0; i < styles.size(); i++)
            if (LowerPath(styles[i].name) == LowerPath(e.reskin) || LowerPath(styles[i].suffix) == LowerPath(e.reskin))
                opt.style = static_cast<int>(i);
        if (opt.style < 0)
        {
            log += "reskin: no style called " + e.reskin + " (";
            for (const ReskinStyle& s : styles)
                log += std::string(s.suffix) + " ";
            log += ")\n";
            return false;
        }
        opt.addon = e.addon;
        opt.outFolder = e.out;
        opt.removeTrees = styles[opt.style].removeTrees;
        opt.removeBushes = styles[opt.style].removeBushes;
        opt.baseWorld = GuessBaseWorld(sourceFile);
        opt.title = e.addon + " (" + styles[opt.style].name + ")";
        opt.install = e.install;
        ReskinReport rep;
        std::string error;
        if (!ReskinIsland(is, game, opt, rep, error))
        {
            log += "reskin: " + error + "\n";
            return false;
        }
        snprintf(line, sizeof(line), "reskin %s: %d textures written (%d not found), %d objects removed\n  addon %s\n  island %s\n",
                 styles[opt.style].name, rep.textures, rep.failed, rep.objectsRemoved, rep.pbo.c_str(), rep.wrp.c_str());
        log += line;
        if (!rep.installed.empty())
            log += "  copied to " + rep.installed + "\n";
    }
    if (!e.road.empty())
    {
        Roads roads;
        roads.Find(game, is);
        const RoadFamily* family = nullptr;
        for (const RoadFamily& f : roads.Families())
        {
            snprintf(line, sizeof(line), "  road kind: %-34s %d straights (longest %.1f m), %d curves%s\n", f.name.c_str(),
                     static_cast<int>(f.straights.size()), f.straights.front().length, static_cast<int>(f.curves.size()),
                     f.hasCap ? ", end piece" : "");
            log += line;
            if (!family && (e.roadKind.empty() || f.name.find(LowerPath(e.roadKind)) != std::string::npos))
                family = &f;
        }
        if (!family || e.road.size() < 4)
        {
            log += "road: no such kind of road, or fewer than two points\n";
            return false;
        }
        std::vector<std::array<float, 2>> path;
        for (size_t i = 0; i + 1 < e.road.size(); i += 2)
            path.push_back({e.road[i], e.road[i + 1]});
        float at[2], heading = 0;
        const bool snapped = roads.OpenEnd(is, game, path[0][0], path[0][1], 18.0f, at, heading);
        if (snapped)
            path[0] = {at[0], at[1]};
        const std::vector<PlacedRoad> placed = roads.Lay(*family, path, !e.noCaps, snapped ? &heading : nullptr);
        is.BeginEdit("Road");
        int id = is.NewObjectId();
        for (const PlacedRoad& p : placed)
            is.AddObject(Roads::MakeObject(p, id++));
        is.EndEdit();
        // how far the road strays from the path, and how near its end comes to the last point
        float worst = 0;
        for (const PlacedRoad& p : placed)
        {
            const float mx = (p.outline[0][0] + p.outline[1][0] + p.outline[2][0] + p.outline[3][0]) * 0.25f;
            const float mz = (p.outline[0][1] + p.outline[1][1] + p.outline[2][1] + p.outline[3][1]) * 0.25f;
            float best = 1e30f;
            for (size_t i = 0; i + 1 < path.size(); i++)
            {
                const float ax = path[i][0], az = path[i][1], bx = path[i + 1][0] - ax, bz = path[i + 1][1] - az;
                const float t = std::clamp(((mx - ax) * bx + (mz - az) * bz) / std::max(1e-3f, bx * bx + bz * bz), 0.0f, 1.0f);
                best = std::min(best, std::hypot(mx - ax - bx * t, mz - az - bz * t));
            }
            worst = std::max(worst, best);
        }
        float endGap = 0;
        if (!placed.empty())
        {
            const PlacedRoad& last = placed.back();
            // the far edge of the last piece
            const int a = last.reversed ? 0 : 2, c = last.reversed ? 1 : 3;
            endGap = std::hypot((last.outline[a][0] + last.outline[c][0]) * 0.5f - path.back()[0],
                                (last.outline[a][1] + last.outline[c][1]) * 0.5f - path.back()[1]);
        }
        snprintf(line, sizeof(line), "road %s: %d pieces%s, at most %.1f m off the path, ends %.1f m from the last point\n",
                 family->name.c_str(), static_cast<int>(placed.size()), snapped ? " (carries on from a road)" : "", worst, endGap);
        log += line;
    }
    if (!e.forest.empty() || e.replant)
    {
        const std::vector<ForestKind> kinds = FindForests(game);
        for (const ForestKind& k : kinds)
        {
            snprintf(line, sizeof(line), "  forest kind: %-36s corner %s\n", k.name.c_str(), k.corner.c_str());
            log += line;
        }
        if (kinds.empty())
        {
            log += "forest: no forest blocks in the game folder\n";
            return false;
        }
        if (e.replant)
        {
            // Take the island's forests out and plant them again: the blocks should come back where they were.
            struct Was
            {
                std::string model;
                float x, y, z, yaw;
                bool met = false;
            };
            std::vector<Was> was;
            island::ObjectList rest;
            std::vector<std::vector<std::array<int, 2>>> cells(kinds.size());
            for (const island::Object& o : is.objects)
            {
                if (!IsForestBlock(o.model))
                {
                    rest.push_back(o);
                    continue;
                }
                was.push_back({LowerPath(o.model), o.X(), is.ObjectY(o), o.Z(), std::atan2(o.m[6], o.m[8])});
                for (size_t k = 0; k < kinds.size(); k++)
                    if (LowerPath(o.model) == kinds[k].half1)
                        cells[k].push_back({static_cast<int>(o.X() / is.cell), static_cast<int>(o.Z() / is.cell)});
            }
            is.BeginEdit("Replant");
            is.ReplaceObjects(std::move(rest));
            const size_t before = is.objects.size();
            for (size_t k = 0; k < kinds.size(); k++)
                SetForestCells(is, kinds[k], cells[k], true, true);
            is.EndEdit();
            int same = 0, moved = 0, extra = 0;
            double offXZ = 0, offY = 0;
            for (size_t i = before; i < is.objects.size(); i++)
            {
                const island::Object& o = is.objects[i];
                const std::string model = LowerPath(o.model);
                const float yaw = std::atan2(o.m[6], o.m[8]), y = is.ObjectY(o);
                Was* best = nullptr;
                float bestD = 4.0f; // within 2 m
                for (Was& w : was)
                {
                    if (w.met || w.model != model)
                        continue;
                    const float d = (w.x - o.X()) * (w.x - o.X()) + (w.z - o.Z()) * (w.z - o.Z());
                    if (d < bestD)
                    {
                        bestD = d;
                        best = &w;
                    }
                }
                if (!best)
                {
                    extra++;
                    continue;
                }
                best->met = true;
                float turn = std::fabs(yaw - best->yaw);
                turn = std::min(turn, 6.2831853f - turn);
                if (bestD < 0.05f * 0.05f && turn < 0.01f)
                    same++;
                else
                    moved++;
                offXZ += std::sqrt(bestD);
                offY += std::fabs(y - best->y);
            }
            int lost = 0;
            std::map<std::string, int> lostBy;
            for (const Was& w : was)
                if (!w.met)
                {
                    lost++;
                    lostBy[w.model]++;
                }
            const int met = same + moved;
            snprintf(line, sizeof(line),
                     "replant: %d forest blocks were there; %d came back in place, %d near (on average %.2f m aside, %.2f m in height), %d new, %d "
                     "not planted again\n",
                     static_cast<int>(was.size()), same, moved, met ? offXZ / met : 0.0, met ? offY / met : 0.0, extra, lost);
            log += line;
            for (const auto& kv : lostBy)
            {
                snprintf(line, sizeof(line), "    not planted again: %5d x %s\n", kv.second, kv.first.c_str());
                log += line;
            }
        }
        if (e.forest.size() >= 3)
        {
            const ForestKind* kind = nullptr;
            for (const ForestKind& k : kinds)
                if (!kind && (e.forestKind.empty() || k.name.find(LowerPath(e.forestKind)) != std::string::npos))
                    kind = &k;
            if (!kind)
            {
                log += "forest: no such kind\n";
                return false;
            }
            const size_t before = is.objects.size();
            is.BeginEdit("Forest");
            const int cells = PaintForest(is, *kind, e.forest[0], e.forest[1], e.forest[2], !e.clearForest, true);
            is.EndEdit();
            snprintf(line, sizeof(line), "forest %s: %d cells %s, objects %d -> %d\n", kind->name.c_str(), cells,
                     e.clearForest ? "cleared" : "planted", static_cast<int>(before), static_cast<int>(is.objects.size()));
            log += line;
        }
    }
    if (!e.bridge.empty())
    {
        const std::vector<BridgeKind> kinds = FindBridges(game);
        for (const BridgeKind& k : kinds)
        {
            snprintf(line, sizeof(line), "  bridge section: %-30s %.1f m long, roadway %.2f m above its origin\n", k.model.c_str(), k.length,
                     k.deck);
            log += line;
        }
        if (kinds.empty() || e.bridge.size() < 4)
        {
            log += "bridge: no bridge models in the game folder, or no two points\n";
            return false;
        }
        const float level = e.bridge.size() >= 5 ? e.bridge[4] : is.SurfaceY(e.bridge[0], e.bridge[1]);
        is.BeginEdit("Bridge");
        const int n = BuildBridge(is, kinds[0], e.bridge[0], e.bridge[1], e.bridge[2], e.bridge[3], level);
        is.EndEdit();
        snprintf(line, sizeof(line), "bridge %s: %d sections, deck at %.1f m (banks at %.1f and %.1f m)\n", kinds[0].model.c_str(), n,
                 level, is.SurfaceY(e.bridge[0], e.bridge[1]), is.SurfaceY(e.bridge[2], e.bridge[3]));
        log += line;
    }
    if (!e.importRegion.empty())
    {
        Island src;
        std::string error;
        if (!GameData::LoadIsland(e.importRegion, src, error))
        {
            log += "import: cannot read " + e.importRegion + ": " + error + "\n";
            return false;
        }
        const RegionInfo info = ReadRegionInfo(e.importRegion, src);
        const int dx = e.importAt.size() == 2 ? e.importAt[0] : info.fromX, dz = e.importAt.size() == 2 ? e.importAt[1] : info.fromZ;
        const size_t before = is.objects.size();
        is.BeginEdit("Import region");
        ImportRegion(is, src, info, dx, dz, ImportOptions());
        is.EndEdit();
        snprintf(line, sizeof(line), "import %s: %d x %d cells%s put at cell %d, %d; objects %d -> %d\n", e.importRegion.c_str(),
                 info.width, info.height, info.marked ? "" : " (a whole island)", dx, dz, static_cast<int>(before),
                 static_cast<int>(is.objects.size()));
        log += line;
    }
    if (!e.exportRegion.empty())
    {
        std::string error;
        if (e.region.size() != 4 || !ExportRegion(is, e.region[0], e.region[1], e.region[2], e.region[3], e.exportRegion, error))
        {
            log += "export: " + (error.empty() ? std::string("needs --region x0 z0 x1 z1") : error) + "\n";
            return false;
        }
        snprintf(line, sizeof(line), "region %d, %d .. %d, %d written: %s\n", e.region[0], e.region[1], e.region[2], e.region[3],
                 e.exportRegion.c_str());
        log += line;
    }
    return true;
}


// ---------------------------------------------------------------------------
// the models, checked: the editor driven by a script of mouse and key events
// ---------------------------------------------------------------------------
//
// --check-models runs the editor with its window hidden and plays a script at it: clicks,
// drags and keys go in as events, the same way the real ones do, and after each step the
// island and what the renderer has built are looked at.

struct ModelCheck
{
    int passed = 0, failed = 0;
    std::string shotPrefix;

    // what the script remembers between frames
    int target = -1, targetId = 0, count0 = 0;
    float sx = 0, sy = 0, startPos[3] = {}, startYaw = 0, grab[3] = {};
    std::string targetModel;
    long long tris0 = 0;
    Renderer::AreaInfo area0;
    float ground0 = 0, objectY0 = 0, raiseAt[3] = {}, base0 = 0;
    std::string placeModel;
    bool placeIsMlod = false;
    int settledAt = -1, arrivedAt = -1, returnedAt = -1;
    int shift = -1; // the script's frame 39 is the frame everything was first there
    unsigned buildsBefore = 0;
    int areasBuilt = 0;
    std::vector<Renderer::AreaInfo> before;
    island::Camera home;
    int placedAreaTris = 0;
    float placeAt[3] = {};
    std::vector<float> ms; // how long each frame took to make

    void Say(bool ok, const char* fmt, ...)
    {
        char text[600];
        va_list args;
        va_start(args, fmt);
        vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        printf("[%s] %s\n", ok ? "ok  " : "FAIL", text);
        (ok ? passed : failed)++;
    }
    void Note(const char* fmt, ...)
    {
        char text[600];
        va_list args;
        va_start(args, fmt);
        vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        printf("       %s\n", text);
    }

    static float YawOf(const island::Object& o) { return std::atan2(o.m[6], o.m[8]); }
    int Find(const App& app, int id) const
    {
        for (size_t i = 0; i < app.is.objects.size(); i++)
            if (app.is.objects[i].id == id)
                return static_cast<int>(i);
        return -1;
    }
    void Totals(const App& app, int& areas, long long& tris, unsigned& builds) const
    {
        areas = 0;
        tris = 0;
        builds = 0;
        for (int i = 0; i < app.renderer.AreaCount(); i++)
        {
            const Renderer::AreaInfo a = app.renderer.AreaAt(i);
            builds += a.builds;
            if (a.built >= 0)
            {
                areas++;
                tris += a.triangles;
            }
        }
    }
    // the ground under a pixel
    bool GroundAt(const App& app, float px, float py, int w, int h, float out[3]) const
    {
        float dir[3];
        app.cam.Ray(px, py, static_cast<float>(w), static_cast<float>(h), dir);
        return app.is.Raycast(app.cam.pos, dir, app.cam.farZ, out);
    }
    float MaxOf(int from, int to) const
    {
        float m = 0;
        for (int i = from; i <= to && i < static_cast<int>(ms.size()); i++)
            m = std::max(m, ms[i]);
        return m;
    }
    int Over(int from, int to, float limit) const
    {
        int n = 0;
        for (int i = from; i <= to && i < static_cast<int>(ms.size()); i++)
            n += ms[i] > limit ? 1 : 0;
        return n;
    }

    // ---- events in, before the frame is begun
    void Input(App& app, int f)
    {
        ImGuiIO& io = ImGui::GetIO();
        auto key = [&](ImGuiKey k, int at)
        {
            if (f == at)
                io.AddKeyEvent(k, true);
            if (f == at + 1)
                io.AddKeyEvent(k, false);
        };
        auto click = [&](int at)
        {
            if (f == at)
                io.AddMouseButtonEvent(0, true);
            if (f == at + 1)
                io.AddMouseButtonEvent(0, false);
        };
        if (target < 0 || shift < 0)
            return;
        f -= shift;
        // select it
        if (f == 40)
        {
            app.SelectTool(Tool::Objects);
            app.placing = false;
            io.AddMousePosEvent(sx, sy);
        }
        click(42);
        // drag it 120 pixels to the right
        if (f == 46)
            io.AddMouseButtonEvent(0, true);
        if (f >= 47 && f <= 56)
            io.AddMousePosEvent(sx + 12.0f * (f - 46), sy);
        if (f == 57)
            io.AddMouseButtonEvent(0, false);
        key(ImGuiKey_R, 62);
        key(ImGuiKey_Delete, 66);
        // a stroke of the Raise tool where it stood
        if (f == 80)
        {
            app.SelectTool(Tool::Raise);
            app.brush.radius = 60.0f;
            io.AddMousePosEvent(sx, sy);
        }
        if (f == 82)
            io.AddMouseButtonEvent(0, true);
        if (f == 94)
            io.AddMouseButtonEvent(0, false);
        // place another of the same
        if (f == 114)
        {
            app.SelectTool(Tool::Objects);
            app.placing = true;
            app.placeModel = placeModel;
            io.AddMousePosEvent(sx + 60.0f, sy + 40.0f);
        }
        click(116);
    }

    // ---- and what came of them, after the frame.  True when the script is over.
    bool After(App& app, int f, Dx12::Device* device, int w, int h)
    {
        Island& is = app.is;
        const Renderer& r = app.renderer;
        // when everything in sight was built and read
        const bool allThere = r.Models().waiting == 0 && r.Models().areas > 0;
        if (shift < 0)
        {
            if (!allThere || f < 39)
            {
                if (f > 3000)
                {
                    Say(false, "the models were never all built");
                    return true;
                }
                return false;
            }
            settledAt = f;
            shift = f - 39;
        }
        const int real = f;
        f -= shift;
        if (allThere)
        {
            if (f > 127 && f < 175 && arrivedAt < 0)
                arrivedAt = real;
            if (f > 176 && returnedAt < 0)
                returnedAt = real;
        }
        if (f == 39)
        {
            printf("--- models built\n");
            int areas;
            unsigned builds;
            Totals(app, areas, tris0, builds);
            const Renderer::ModelStats& s = r.Models();
            Say(areas > 0 && tris0 > 0 && s.waiting == 0, "%d areas built, %lld triangles, %d draws, %d textures, nothing waiting", areas, tris0,
                s.draws, s.textures);
            count0 = static_cast<int>(is.objects.size());
            home = app.cam;

            printf("--- picking: a click on the middle of each model near the camera\n");
            int tested = 0, self = 0, smaller = 0, other = 0, none = 0, hidden = 0;
            float bestCentre = 1e30f, bestAny = 1e30f;
            int anyTarget = -1;
            float anyX = 0, anyY = 0;
            const ImVec2 mouseWas = ImGui::GetIO().MousePos;
            const bool hoverWas = app.hover;
            float hitWas[3];
            std::memcpy(hitWas, app.hit, sizeof(hitWas));
            for (size_t i = 0; i < is.objects.size(); i++)
            {
                if (!r.DrawnAsModel(static_cast<int>(i)))
                    continue;
                const island::Object& o = is.objects[i];
                const ModelShape& shape = app.game.Model(o.model);
                if (!shape.valid || shape.bmax[1] - shape.bmin[1] < 0.5f)
                    continue; // roads are picked by their markers
                const float p[3] = {o.X(), is.ObjectY(o), o.Z()};
                const float d = std::sqrt((p[0] - app.cam.pos[0]) * (p[0] - app.cam.pos[0]) + (p[1] - app.cam.pos[1]) * (p[1] - app.cam.pos[1]) +
                                          (p[2] - app.cam.pos[2]) * (p[2] - app.cam.pos[2]));
                float px, py;
                if (d > 350.0f || !app.cam.Project(p, static_cast<float>(w), static_cast<float>(h), px, py))
                    continue;
                if (px < 300 || px > w - 370 || py < 90 || py > h - 50)
                    continue;
                ImGui::GetIO().MousePos = ImVec2(px, py);
                app.hover = GroundAt(app, px, py, w, h, app.hit);
                if (app.hover && std::sqrt((app.hit[0] - app.cam.pos[0]) * (app.hit[0] - app.cam.pos[0]) + (app.hit[1] - app.cam.pos[1]) * (app.hit[1] - app.cam.pos[1]) +
                                           (app.hit[2] - app.cam.pos[2]) * (app.hit[2] - app.cam.pos[2])) < d - 1.0f)
                {
                    hidden++; // its middle is behind a rise of the land: the click lands on the land
                    continue;
                }
                const int got = app.PickObject(w, h);
                tested++;
                if (got == static_cast<int>(i))
                {
                    self++;
                    // the house nearest the middle of the view is the one the script works on
                    const float c = (px - w * 0.5f) * (px - w * 0.5f) + (py - h * 0.5f) * (py - h * 0.5f);
                    if (KindOf(o.model) == ObjectKind::Building && d > 40.0f && c < bestCentre)
                    {
                        bestCentre = c;
                        target = static_cast<int>(i);
                        sx = px;
                        sy = py;
                    }
                    if (d > 40.0f && c < bestAny)
                    {
                        bestAny = c;
                        anyTarget = static_cast<int>(i);
                        anyX = px;
                        anyY = py;
                    }
                }
                else if (got < 0)
                {
                    none++;
                }
                else
                {
                    const ModelShape& g = app.game.Model(is.objects[got].model);
                    const float vg = (g.bmax[0] - g.bmin[0]) * (g.bmax[1] - g.bmin[1]) * (g.bmax[2] - g.bmin[2]);
                    const float vs = (shape.bmax[0] - shape.bmin[0]) * (shape.bmax[1] - shape.bmin[1]) * (shape.bmax[2] - shape.bmin[2]);
                    (vg <= vs ? smaller : other)++;
                }
            }
            ImGui::GetIO().MousePos = mouseWas;
            app.hover = hoverWas;
            std::memcpy(app.hit, hitWas, sizeof(hitWas));
            Say(tested > 20 && none == 0 && other == 0,
                "%d models clicked: %d picked themselves, %d a smaller model standing in the way, %d a larger one, %d nothing (%d more had "
                "their middle behind the land)",
                tested, self, smaller, other, none, hidden);
            if (target < 0)
            {
                // no house: whatever is nearest the middle of the view
                target = anyTarget;
                sx = anyX;
                sy = anyY;
            }
            if (target < 0)
            {
                Say(false, "nothing in view to work on");
                return true;
            }
            // the mouse is at whole pixels; far off, a pixel is metres of ground
            sx = std::floor(sx);
            sy = std::floor(sy);
            const island::Object& o = is.objects[target];
            targetId = o.id;
            targetModel = o.model;
            startPos[0] = o.X(), startPos[1] = is.ObjectY(o), startPos[2] = o.Z();
            startYaw = YawOf(o);
            Note("the object worked on: %s at %.1f, %.1f (object %d), on screen at %.0f, %.0f", o.model.c_str(), o.X(), o.Z(), target, sx, sy);
            // what is placed later: a model that is not binarized, if the game folder has one worth looking at
            placeModel = targetModel;
            for (const std::string& name : app.game.Models())
                if (app.game.ModelFormat(name) == "MLOD" && app.game.Mesh(name).fine.triangles > 200)
                {
                    placeModel = name;
                    placeIsMlod = true;
                    break;
                }
        }
        if (target < 0)
            return f > 39;
        if (f == 44)
        {
            printf("--- the mouse: select, drag, turn, remove\n");
            Say(app.selected == target, "a click on it selects it (selected: %d)", app.selected);
            if (!shotPrefix.empty())
                SaveScreenshot(device, w, h, shotPrefix + "1_selected.png");
        }
        if (f == 62)
        {
            const int at = Find(app, targetId);
            float a[3], b[3];
            const bool okA = GroundAt(app, sx, sy, w, h, a), okB = GroundAt(app, sx + 120.0f, sy, w, h, b);
            if (at >= 0 && okA && okB)
            {
                const island::Object& o = is.objects[at];
                const float ex = startPos[0] + (b[0] - a[0]), ez = startPos[2] + (b[2] - a[2]);
                const float off = std::hypot(o.X() - ex, o.Z() - ez), moved = std::hypot(o.X() - startPos[0], o.Z() - startPos[2]);
                Say(moved > 3.0f && off < 0.6f, "dragged 120 pixels: it moved %.1f m and is %.2f m from where the cursor took it", moved, off);
                const Renderer::AreaInfo a1 = r.Area(o.X(), o.Z());
                Say(r.DrawnAsModel(at) && a1.built >= 0 && !a1.dirty, "its model is drawn where it stands now (area built %u times)", a1.builds);
                Say(std::fabs(is.ObjectY(o) - is.SurfaceY(o.X(), o.Z()) - o.above) < 0.01f, "it stands on the ground there, as high above it as before");
            }
            else
            {
                Say(false, "the house is gone after the drag");
            }
            if (!shotPrefix.empty())
                SaveScreenshot(device, w, h, shotPrefix + "2_dragged.png");
        }
        if (f == 65)
        {
            const int at = Find(app, targetId);
            float turn = at >= 0 ? std::fabs(YawOf(is.objects[at]) - startYaw) : 0.0f;
            turn = std::min(turn, 2.0f * kPi - turn) * 180.0f / kPi;
            Say(std::fabs(turn - 15.0f) < 0.1f, "R turns it (by %.1f degrees)", turn);
        }
        if (f == 69)
        {
            Say(static_cast<int>(is.objects.size()) == count0 - 1 && Find(app, targetId) < 0 && app.selected < 0,
                "Del removes it (%d objects, were %d)", static_cast<int>(is.objects.size()), count0);
        }
        if (f == 72)
        {
            is.Undo();
            is.Undo();
            is.Undo();
        }
        if (f == 78)
        {
            const int at = Find(app, targetId);
            int areas;
            long long tris;
            unsigned builds;
            Totals(app, areas, tris, builds);
            const bool back = at >= 0 && std::fabs(is.objects[at].X() - startPos[0]) < 0.001f && std::fabs(is.objects[at].Z() - startPos[2]) < 0.001f &&
                              std::fabs(YawOf(is.objects[at]) - startYaw) < 0.0001f;
            Say(back && static_cast<int>(is.objects.size()) == count0, "three undos put it back where and as it was");
            Say(at >= 0 && r.DrawnAsModel(at) && tris == tris0, "and the models are again what they were (%lld triangles, were %lld)", tris, tris0);
            target = at;

            printf("--- the land: a stroke of the Raise tool under it\n");
            GroundAt(app, sx, sy, w, h, raiseAt);
            ground0 = is.SurfaceY(startPos[0], startPos[2]);
            objectY0 = at >= 0 ? is.ObjectY(is.objects[at]) : 0.0f;
            area0 = r.Area(startPos[0], startPos[2]);
            Totals(app, areasBuilt, tris, buildsBefore);
            if (!r.BakedBase(at, base0))
                Say(false, "the house has no model built");
        }
        if (f == 99)
        {
            const int at = Find(app, targetId);
            const float rose = is.SurfaceY(startPos[0], startPos[2]) - ground0;
            const float objRose = at >= 0 ? is.ObjectY(is.objects[at]) - objectY0 : 0.0f;
            const Renderer::AreaInfo a1 = r.Area(startPos[0], startPos[2]);
            int areas;
            long long tris;
            unsigned builds;
            Totals(app, areas, tris, builds);
            Say(rose > 0.1f, "the ground under it rose %.2f m", rose);
            Say(std::fabs(objRose - rose) < 0.01f, "it rose with the ground (%.2f m)", objRose);
            float base1 = 0;
            const bool has = r.BakedBase(at, base1);
            Say(has && a1.builds > area0.builds && std::fabs(base1 - base0 - rose) < 0.02f,
                "its model as built rose %.2f m too (its area was built %u times during the stroke)", base1 - base0, a1.builds - area0.builds);
            const int rebuilt = static_cast<int>(builds - buildsBefore);
            Note("%d area builds in all during the stroke, of %d areas built: the rest were left alone", rebuilt, areas);
            if (!shotPrefix.empty())
                SaveScreenshot(device, w, h, shotPrefix + "3_raised.png");
            is.Undo();
        }
        if (f == 103)
        {
            const Renderer::AreaInfo a1 = r.Area(startPos[0], startPos[2]);
            Say(std::fabs(a1.minY - area0.minY) < 0.01f && std::fabs(a1.maxY - area0.maxY) < 0.01f && !a1.dirty,
                "undo: the area is back between %.2f and %.2f m (was %.2f and %.2f)", a1.minY, a1.maxY, area0.minY, area0.maxY);

            printf("--- the land: every elevation up 25 m\n");
            before.clear();
            for (int i = 0; i < r.AreaCount(); i++)
                before.push_back(r.AreaAt(i));
            is.BeginEdit("Offset heights");
            is.OffsetAll(25.0f);
            is.EndEdit();
        }
        if (f == 112)
        {
            // building goes by the clock: wait here until every area has been done again
            for (int i = 0; i < r.AreaCount(); i++)
                if (r.AreaAt(i).built >= 0 && r.AreaAt(i).dirty && real < 5000)
                {
                    shift++;
                    return false;
                }
            int checked = 0, wrong = 0;
            float worst = 0;
            for (int i = 0; i < r.AreaCount(); i++)
            {
                const Renderer::AreaInfo a = r.AreaAt(i);
                const Renderer::AreaInfo& b = before[i];
                if (b.built < 0 || b.triangles == 0 || b.minY < 3.0f)
                    continue; // nothing there, or at the sea, which does not move
                checked++;
                const float e = std::max(std::fabs(a.minY - b.minY - 25.0f), std::fabs(a.maxY - b.maxY - 25.0f));
                worst = std::max(worst, e);
                if (a.built < 0 || a.dirty || a.builds == b.builds || e > 0.1f)
                    wrong++;
            }
            Say(checked > 10 && wrong == 0, "%d areas with models: all built again 25 m higher (furthest off: %.3f m)", checked, worst);
            is.Undo();
        }
        if (f == 113)
            GroundAt(app, sx + 60.0f, sy + 40.0f, w, h, placeAt);
        if (f == 115)
            placedAreaTris = r.Area(placeAt[0], placeAt[2]).triangles;
        if (f == 122)
        {
            printf("--- the mouse: place another of the same\n");
            const bool added = static_cast<int>(is.objects.size()) == count0 + 1 && app.selected == count0;
            Say(added, "a click with \"Place new objects\" adds one (%d objects, were %d)", static_cast<int>(is.objects.size()), count0);
            if (added)
            {
                const island::Object& o = is.objects[app.selected];
                const ModelMesh& mesh = app.game.Mesh(o.model);
                const Renderer::AreaInfo a = r.Area(o.X(), o.Z());
                Note("placed: %s (%s)", o.model.c_str(), placeIsMlod ? "not binarized, MLOD" : "binarized: the game folder has no other kind worth trying");
                float base = 0;
                if (!r.BakedBase(app.selected, base))
                    Say(false, "it has no model built");
                else if (placeIsMlod)
                    Say(std::fabs(base - is.SurfaceY(o.X(), o.Z())) < 0.35f, "the lowest point of its model is %.2f m from the ground under its middle",
                        base - is.SurfaceY(o.X(), o.Z()));
                else
                    Note("the lowest point of its model is %.2f m from the ground under its middle (a house has foundations, and the ground slopes)",
                         base - is.SurfaceY(o.X(), o.Z()));
                const int expect = a.built == 1 && !mesh.coarse.Empty() ? mesh.coarse.triangles : mesh.fine.triangles;
                Say(std::hypot(o.X() - placeAt[0], o.Z() - placeAt[2]) < 0.5f && std::fabs(o.above - mesh.centre[1]) < 0.001f,
                    "it stands %.2f m from where the click was, its own origin on the ground (%.2f m below its middle; the model says %.2f)",
                    std::hypot(o.X() - placeAt[0], o.Z() - placeAt[2]), o.above, mesh.centre[1]);
                Say(r.DrawnAsModel(app.selected) && a.triangles - placedAreaTris == expect,
                    "its model is drawn: the area has %d triangles more, the model has %d", a.triangles - placedAreaTris, expect);
                if (!shotPrefix.empty())
                    SaveScreenshot(device, w, h, shotPrefix + "4_placed.png");
                is.Undo();
            }
            app.placing = false;
            app.selected = -1;
        }
        if (f == 126)
        {
            int areas;
            long long tris;
            unsigned builds;
            Totals(app, areas, tris, builds);
            Say(static_cast<int>(is.objects.size()) == count0 && tris == tris0, "undo: %lld triangles, as at the start", tris);

            // somewhere the editor has not been yet: the town furthest from here
            float town[2] = {1400.0f, 5800.0f};
            if (std::hypot(town[0] - app.cam.pos[0], town[1] - app.cam.pos[2]) < 2500.0f)
                town[0] = is.WorldSize() - town[0], town[1] = is.WorldSize() - town[1];
            app.cam.pos[0] = town[0];
            app.cam.pos[2] = town[1] - 300.0f;
            app.cam.pos[1] = std::max(0.0f, is.SurfaceY(app.cam.pos[0], app.cam.pos[2])) + 110.0f;
            app.cam.yaw = 0.0f;
            app.cam.pitch = -0.3f;
        }
        if (f == 175)
            app.cam = home;
        if (f == 215)
        {
            printf("--- how long frames take to make (drawing and building, without the wait for the screen)\n");
            auto total = [&](int from, int to)
            {
                float sum = 0;
                for (int i = from; i <= to && i < static_cast<int>(ms.size()); i++)
                    sum += ms[i];
                return sum;
            };
            Note("opening at the first place:   all built and read after %d frames (%.0f ms of work); longest frame %.1f ms, %d over 16.7 ms",
                 settledAt + 1, total(0, settledAt), MaxOf(0, settledAt), Over(0, settledAt, 16.7f));
            const int a0 = 127 + shift, b0 = 176 + shift;
            if (arrivedAt < 0)
                arrivedAt = b0 - 2;
            if (returnedAt < 0)
                returnedAt = real;
            Note("arriving somewhere new:       all there after %d frames (%.0f ms); longest frame %.1f ms, %d over 16.7 ms", arrivedAt - a0 + 1,
                 total(a0, arrivedAt), MaxOf(a0, arrivedAt), Over(a0, arrivedAt, 16.7f));
            Note("coming back to the first:     all there after %d frames (%.0f ms); longest frame %.1f ms, %d over 16.7 ms", returnedAt - b0 + 1,
                 total(b0, returnedAt), MaxOf(b0, returnedAt), Over(b0, returnedAt, 16.7f));
            const Renderer::ModelStats& cost = r.Models();
            Note("in all: %d area builds took %.0f ms (%.1f ms each), %d textures read took %.0f ms (%.1f ms each)", cost.builds, cost.buildMs,
                 cost.builds ? cost.buildMs / cost.builds : 0.0f, cost.texturesRead, cost.textureMs,
                 cost.texturesRead ? cost.textureMs / cost.texturesRead : 0.0f);
            const float settled = MaxOf(real - 8, real);
            Say(settled < 16.7f, "once built, a frame takes %.1f ms at most", settled);
            const Renderer::ModelStats& s = r.Models();
            Say(s.waiting == 0 && s.areas > 0, "back home: %d areas drawn, nothing waiting", s.areas);
            return true;
        }
        return false;
    }
};

App* g_app = nullptr;
} // namespace

int main(int argc, char* argv[])
{
#ifdef ISLAND_EDITOR_CPU_CHECK
    if (!Poseidon::Foundation::CpuIsSupported(true))
        return 1;
#endif
    CLI::App cli{"Open Poseidon Island Editor - edits the islands (.wrp) of Cold War Assault / Operation Flashpoint"};
    std::string file, gameDir, shot, selfTest;
    int shotFrames = 30, toolIndex = -1;
    bool demoStroke = false;
    cli.add_option("file", file, "Island to open");
    cli.add_option("-g,--game", gameDir, "Game folder (with DTA and AddOns) to read textures from");
    cli.add_option("--selftest", selfTest, "Load an island, save and reload it, try every tool, and report (no window)");
    cli.add_option("--shot", shot, "Write a picture of the window after a few frames, then exit");
    cli.add_option("--frames", shotFrames, "Frames to draw before --shot");
    cli.add_option("--tool", toolIndex, "Tool to start with (0..14), for --shot");
    cli.add_flag("--demo-stroke", demoStroke, "With --shot: raise a hill and paint at the middle of the view first");
    CliEdits edits;
    cli.add_option("--heightmap", edits.heightmap, "Make a new island from this height map (PNG, PGM, RAW, ASC ...)");
    cli.add_option("--size", edits.size, "With --heightmap: cells per side (32 .. 1024); default: nearest the picture's size");
    cli.add_option("--low", edits.low, "With --heightmap: metres that black stands for");
    cli.add_option("--high", edits.high, "With --heightmap: metres that white stands for");
    cli.add_option("--smooth", edits.smooth, "With --heightmap: smoothing passes (default 2 for 8-bit pictures)");
    cli.add_flag("--auto-texture", edits.autoTexture, "Texture the island by height and slope (needs --game)");
    cli.add_option("--set", edits.set, "With --auto-texture: the texture set (folder) to use, e.g. eden");
    cli.add_option("--reskin", edits.reskin, "Reskin: snow, desert, dry, autumn, lush, mud, burnt, ash, mars, moon (needs --game, --addon, --out)");
    cli.add_option("--addon", edits.addon, "With --reskin: name of the addon and of the new island");
    cli.add_option("--out", edits.out, "With --reskin: folder to make the addon in");
    cli.add_flag("--install", edits.install, "With --reskin: also copy the .pbo to the game folder's AddOns");
    cli.add_option("--road", edits.road, "Lay a road through these points: x z x z ... (needs --game)");
    cli.add_option("--road-kind", edits.roadKind, "With --road: part of the name of the kind of road");
    cli.add_flag("--no-caps", edits.noCaps, "With --road: no end pieces");
    cli.add_option("--forest", edits.forest, "Plant forest at x z within this radius (needs --game)")->expected(3);
    cli.add_option("--forest-kind", edits.forestKind, "With --forest: part of the name of the kind of forest");
    cli.add_flag("--clear-forest", edits.clearForest, "With --forest: clear instead of planting");
    cli.add_flag("--replant", edits.replant, "Test: take the island's forests out, plant them again and compare");
    cli.add_option("--bridge", edits.bridge, "Span a bridge from x z to x z, deck at this height (default: the first bank's)")->expected(4, 5);
    cli.add_option("--region", edits.region, "With --export-region: the cells x0 z0 x1 z1")->expected(4);
    cli.add_option("--export-region", edits.exportRegion, "Write the cells of --region as a small island file");
    cli.add_option("--import-region", edits.importRegion, "Put a region file (or a whole island) into this one");
    cli.add_option("--at", edits.importAt, "With --import-region: the cell of its south-west corner (default: where it came from)")->expected(2);
    std::string listModels;
    cli.add_option("--list-models", listModels, "Print the game's models whose name has this in it, with their size and joining points");
    bool checkModels = false, modelFormats = false;
    cli.add_flag("--check-models", checkModels, "Test: drive the editor by script (window hidden) and check what the models do; --shot prefix for pictures");
    cli.add_flag("--model-formats", modelFormats, "Print how many of the game's models are binarized (ODOL) and how many are not (MLOD)");
    bool probeRoads = false;
    cli.add_flag("--probe-roads", probeRoads, "Print the joining points of the island's road models and how well the pieces meet");
    std::string convertTo;
    std::vector<float> raiseArg;
    cli.add_option("--convert", convertTo, "Save the island given as 4WVR under this name and exit (no window)");
    cli.add_option("--raise", raiseArg, "With --convert: first raise the land at x z, radius, metres")->expected(4);
    std::vector<float> camArg;
    int viewMode = 0, sideTab = -1;
    bool gridArg = false;
    cli.add_option("--cam", camArg, "Camera for --shot: x y z yaw pitch (degrees)")->expected(5);
    cli.add_option("--view", viewMode, "0 textures, 1 heights, 2 slopes");
    cli.add_flag("--grid", gridArg, "Show the cell grid");
    cli.add_option("--tab", sideTab, "Side panel tab for --shot: 0 textures, 1 objects, 2 map, 3 info");
    float uiScale = 0, modelsTo = 0;
    std::string showWindow;
    int selectArg = -1;
    cli.add_option("--models-to", modelsTo, "Objects are drawn as their models this far from the camera, in metres (0: as markers only)");
    cli.add_option("--select", selectArg, "For --shot: the object (by its place in the file) shown as selected");
    cli.add_option("--ui-scale", uiScale, "Size of the interface: 1 = 96 dpi (default: the screen's own)");
    cli.add_option("--show", showWindow, "For --shot: new, generate, whole, auto, reskin, region, addtexture, keys, about");
    CLI11_PARSE(cli, argc, argv);

    if (!selfTest.empty())
        return SelfTest(selfTest);
    if (!listModels.empty())
    {
        GameData game;
        if (gameDir.empty() || !game.Mount(gameDir))
        {
            fprintf(stderr, "needs --game\n");
            return 1;
        }
        for (const std::string& name : game.Models())
        {
            if (name.find(LowerPath(listModels)) == std::string::npos)
                continue;
            const ModelShape& s = game.Model(name);
            printf("%-44s %s box x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f centre %.2f %.2f %.2f", name.c_str(), s.valid ? "" : "(unreadable)",
                   s.bmin[0], s.bmax[0], s.bmin[1], s.bmax[1], s.bmin[2], s.bmax[2], s.centre[0], s.centre[1], s.centre[2]);
            if (s.road)
                printf("  road: begin y %.2f, end y %.2f", s.lb[1], s.le[1]);
            printf("\n");
            const ModelMesh& mesh = game.Mesh(name);
            printf("    %s, drawn with %d triangles (%d at a distance)%s\n", game.ModelFormat(name).c_str(), mesh.fine.triangles,
                   mesh.coarse.Empty() ? mesh.fine.triangles : mesh.coarse.triangles, mesh.onSurface ? ", lies on the ground" : "");
            for (const ModelLod::Part& part : mesh.fine.parts)
                printf("      %5d triangles  %-40s %s\n", static_cast<int>(part.indices.size() / 3),
                       part.texture.empty() ? "(no texture)" : part.texture.c_str(),
                       part.texture.empty() ? "" : game.LoadTexture(part.texture).Valid() ? "" : "NOT FOUND");
        }
        return 0;
    }
    if (modelFormats)
    {
        GameData game;
        if (gameDir.empty() || !game.Mount(gameDir))
        {
            fprintf(stderr, "needs --game\n");
            return 1;
        }
        std::map<std::string, int> count;
        std::vector<std::string> mlod;
        for (const std::string& name : game.Models())
        {
            const std::string format = game.ModelFormat(name);
            count[format.empty() ? "(unreadable)" : format]++;
            if (format == "MLOD")
                mlod.push_back(name);
        }
        for (const auto& kv : count)
            printf("%-14s %d\n", kv.first.c_str(), kv.second);
        for (size_t i = 0; i < mlod.size(); i++)
        {
            const ModelShape& s = game.Model(mlod[i]);
            const ModelMesh& mesh = game.Mesh(mlod[i]);
            if (i < 12)
                printf("  MLOD %-40s %s centre %.2f %.2f %.2f  box y %.2f..%.2f  %d triangles\n", mlod[i].c_str(), s.valid ? "" : "(unreadable)",
                       s.centre[0], s.centre[1], s.centre[2], s.bmin[1], s.bmax[1], mesh.fine.triangles);
        }
        return 0;
    }
    if (probeRoads)
    {
        Island is;
        GameData game;
        std::string error;
        if (gameDir.empty() || !game.Mount(gameDir) || !GameData::LoadIsland(file, is, error))
        {
            fprintf(stderr, "needs an island and --game\n");
            return 1;
        }
        struct End
        {
            float p[3][3]; // per hypothesis
            int object;
        };
        std::vector<End> ends;
        std::map<std::string, int> seen;
        static const char* const hyp[3] = {"model origin", "minus bounding centre", "minus box centre"};
        for (size_t i = 0; i < is.objects.size(); i++)
        {
            const island::Object& o = is.objects[i];
            const ModelShape& s = game.Model(o.model);
            if (!s.road)
                continue;
            if (seen[LowerPath(o.model)]++ == 0)
                printf("%-34s LB %.2f %.2f %.2f  PB %.2f %.2f %.2f  LE %.2f %.2f %.2f  PE %.2f %.2f %.2f  centre %.2f %.2f %.2f  box %.2f..%.2f %.2f..%.2f\n",
                       o.model.c_str(), s.lb[0], s.lb[1], s.lb[2], s.pb[0], s.pb[1], s.pb[2], s.le[0], s.le[1], s.le[2], s.pe[0],
                       s.pe[1], s.pe[2], s.centre[0], s.centre[1], s.centre[2], s.bmin[0], s.bmax[0], s.bmin[2], s.bmax[2]);
            for (int which = 0; which < 2; which++)
            {
                End e;
                e.object = static_cast<int>(i);
                const float* a = which ? s.le : s.lb;
                const float* b = which ? s.pe : s.pb;
                for (int h = 0; h < 3; h++)
                {
                    float local[3];
                    for (int k = 0; k < 3; k++)
                    {
                        local[k] = (a[k] + b[k]) * 0.5f;
                        if (h == 1)
                            local[k] -= s.centre[k];
                        if (h == 2)
                            local[k] -= (s.bmin[k] + s.bmax[k]) * 0.5f;
                    }
                    for (int k = 0; k < 3; k++)
                        e.p[h][k] = o.m[9 + k] + o.m[k] * local[0] + o.m[3 + k] * local[1] + o.m[6 + k] * local[2];
                }
                ends.push_back(e);
            }
        }
        printf("%d road ends of %d models\n", static_cast<int>(ends.size()), static_cast<int>(seen.size()));
        {
            int freeBegin = 0, freeEnd = 0;
            for (size_t i = 0; i < ends.size(); i++)
            {
                if (LowerPath(is.objects[ends[i].object].model).find("konec") == std::string::npos)
                    continue;
                float best = 1e9f;
                for (size_t j = 0; j < ends.size(); j++)
                {
                    if (ends[j].object == ends[i].object)
                        continue;
                    const float dx = ends[i].p[0][0] - ends[j].p[0][0], dz = ends[i].p[0][2] - ends[j].p[0][2];
                    best = std::min(best, dx * dx + dz * dz);
                }
                if (best > 1.0f)
                    (i % 2 ? freeEnd : freeBegin)++;
            }
            printf("  end pieces (konec): %d have nothing at their begin, %d nothing at their end\n", freeBegin, freeEnd);
        }
        for (int h = 0; h < 3; h++)
        {
            double sum = 0;
            int close = 0, n = 0;
            for (size_t i = 0; i < ends.size(); i += 7) // a sample is enough
            {
                float best = 1e9f;
                for (size_t j = 0; j < ends.size(); j++)
                {
                    if (ends[j].object == ends[i].object)
                        continue;
                    const float dx = ends[i].p[h][0] - ends[j].p[h][0], dz = ends[i].p[h][2] - ends[j].p[h][2];
                    best = std::min(best, dx * dx + dz * dz);
                }
                best = std::sqrt(best);
                if (best < 30.0f)
                {
                    sum += best;
                    n++;
                    close += best < 0.5f ? 1 : 0;
                }
            }
            printf("  %-22s: ends meet another piece within %.2f m on average; %d of %d within 0.5 m\n", hyp[h], n ? sum / n : 0.0, close, n);
        }
        return 0;
    }
    if (!convertTo.empty())
    {
        Island is;
        GameData game;
        std::string error, log;
        if (!gameDir.empty() && !game.Mount(gameDir))
            fprintf(stderr, "no game archives found in %s\n", gameDir.c_str());
        if (edits.heightmap.empty() && (file.empty() || !GameData::LoadIsland(file, is, error)))
        {
            fprintf(stderr, "cannot load %s: %s", file.c_str(), error.c_str());
            return 1;
        }
        const std::string wasRead = is.sourceFormat;
        const bool edited = ApplyCliEdits(is, game, edits, file, log);
        printf("%s", log.c_str());
        if (!edited)
            return 1;
        if (raiseArg.size() == 4)
        {
            Brush b;
            b.radius = raiseArg[2];
            is.BeginEdit("Raise");
            is.Raise(raiseArg[0], raiseArg[1], b, raiseArg[3]);
            is.EndEdit();
        }
        const std::string was = wasRead.empty() ? is.sourceFormat : wasRead;
        if (!is.Save(convertTo, error))
        {
            fprintf(stderr, "cannot save %s: %s", convertTo.c_str(), error.c_str());
            return 1;
        }
        printf("%s (%s, %d cells, %d objects, %d repeated ids renumbered) saved as 4WVR: %s", file.c_str(), was.c_str(),
               is.size, static_cast<int>(is.objects.size()), is.renumbered, convertTo.c_str());
        return 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    // a picture is taken without the window ever showing: nothing pops up over what else is running
    SDL_Window* window = SDL_CreateWindow("Open Poseidon Engine - Island Editor", 1600, 900,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (shot.empty() && !checkModels ? 0 : SDL_WINDOW_HIDDEN));
    if (!window)
    {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    Dx12::Device* device = Dx12::Device::Create(hwnd, width, height, 0, true);
    if (!device)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Island Editor", "Direct3D 12 could not be started on this computer.", window);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    {
        io.Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());
        // the screen's scale: 1 at 96 dpi
        g_ui = uiScale > 0 ? uiScale : SDL_GetWindowDisplayScale(window);
        if (!(g_ui >= 0.5f && g_ui <= 4.0f))
            g_ui = 1.0f;
        ImFontConfig cfg;
        if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f, &cfg))
            io.Fonts->AddFontDefault();
        std::error_code ec;
        if (std::filesystem::exists("C:\\Windows\\Fonts\\seguisb.ttf", ec))
            g_bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 16.0f, &cfg);
    }
    SetupTheme();
    ImGui_ImplSDL3_InitForD3D(window);
    {
        ImGui_ImplDX12_InitInfo info;
        info.Device = device->D3D();
        info.CommandQueue = device->Queue();
        info.NumFramesInFlight = Dx12::Device::FramesInFlight;
        info.RTVFormat = Dx12::Device::BackBufferFormat;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.SrvDescriptorHeap = device->TextureHeap();
        info.UserData = device;
        info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
        { static_cast<Dx12::Device*>(i->UserData)->AllocExternalTexture(cpu, gpu); };
        info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
        { static_cast<Dx12::Device*>(i->UserData)->FreeExternalTexture(cpu); };
        if (!ImGui_ImplDX12_Init(&info))
        {
            fprintf(stderr, "ImGui could not be started\n");
            return 1;
        }
    }

    App app;
    g_app = &app;
    app.device = device;
    app.window = window;
    if (!app.renderer.Init(device))
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Island Editor", "The editor's shaders could not be created.", window);
        return 1;
    }
    if (!gameDir.empty())
        app.MountGame(gameDir, false);
    app.LoadSettings();
    if (app.game.Folder().empty())
    {
        // beside a game?
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        const fs::path dir = fs::path(exe).parent_path();
        if (fs::is_directory(dir / "DTA") || fs::is_directory(dir / "AddOns"))
            app.MountGame(dir.string(), false);
    }
    if (!file.empty())
        app.Open(file);
    if (edits.Any())
    {
        std::string log;
        ApplyCliEdits(app.is, app.game, edits, file, log);
        printf("%s", log.c_str());
        app.AfterLoad();
    }
    if (toolIndex >= 0 && toolIndex < static_cast<int>(Tool::Count))
        app.tool = static_cast<Tool>(toolIndex);
    if (camArg.size() == 5)
    {
        app.cam.pos[0] = camArg[0];
        app.cam.pos[1] = camArg[1];
        app.cam.pos[2] = camArg[2];
        app.cam.yaw = camArg[3] * kPi / 180.0f;
        app.cam.pitch = camArg[4] * kPi / 180.0f;
    }
    app.view.mode = viewMode;
    app.showTab = sideTab;
    if (cli.count("--models-to"))
    {
        app.view.models = modelsTo > 0;
        if (modelsTo > 0)
            app.view.modelRange = modelsTo;
    }
    if (selectArg >= 0 && selectArg < static_cast<int>(app.is.objects.size()))
    {
        app.selected = selectArg;
        app.tool = Tool::Objects;
    }
    if (showWindow == "new")
        app.openNew = true;
    else if (showWindow == "generate")
        app.openGenerate = true;
    else if (showWindow == "whole")
        app.openScale = true;
    else if (showWindow == "addtexture")
        app.openAddTexture = true;
    else if (showWindow == "keys")
        app.openKeys = true;
    else if (showWindow == "about")
        app.openAbout = true;
    else if (showWindow == "auto" && app.is.size > 0)
    {
        app.EnsureRules();
        app.showAuto = true;
    }
    else if (showWindow == "reskin" && app.is.size > 0)
    {
        app.SetReskinDefaults();
        app.showReskin = true;
        app.UpdateReskinPreview();
    }
    else if (showWindow == "region" && app.is.size > 0)
    {
        app.regionSrc = app.is;
        app.regionInfo = RegionInfo();
        app.regionInfo.width = app.regionInfo.height = app.is.size;
        app.regionFile = file;
        app.openRegionImport = true;
    }
    app.view.grid = gridArg;

    Uint64 last = SDL_GetPerformanceCounter();
    int frames = 0, exitCode = 0;
    ModelCheck check;
    check.shotPrefix = checkModels ? shot : std::string();
    while (app.running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                app.RequestExit();
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data)
                app.Open(e.drop.data);
        }
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w <= 0 || h <= 0 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED))
        {
            SDL_Delay(50);
            continue;
        }
        if (w != width || h != height)
        {
            width = w;
            height = h;
            device->Reset(width, height, 0, true);
        }
        const Uint64 now = SDL_GetPerformanceCounter();
        const float dt = std::min(0.1f, static_cast<float>(now - last) / static_cast<float>(SDL_GetPerformanceFrequency()));
        last = now;

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        if (checkModels)
            check.Input(app, frames);
        ImGui::NewFrame();

        if (!shot.empty() && demoStroke && frames == 5 && app.is.size > 0)
        {
            // something to look at in the picture: a hill and a painted patch ahead of the camera
            float f[3], r[3], u[3], hit[3];
            app.cam.Basis(f, r, u);
            const bool found = app.is.Raycast(app.cam.pos, f, app.cam.farZ, hit);
            if (found && app.tool == Tool::Road)
            {
                // a road being drawn
                app.roadPath = {{hit[0] - 250, hit[2] - 120}, {hit[0] - 40, hit[2] + 60}, {hit[0] + 160, hit[2] - 30}, {hit[0] + 300, hit[2] + 150}};
            }
            else if (found && app.tool == Tool::Region)
            {
                app.regionA[0] = static_cast<int>(hit[0] / app.is.cell) - 6;
                app.regionA[1] = static_cast<int>(hit[2] / app.is.cell) - 4;
                app.regionB[0] = app.regionA[0] + 12;
                app.regionB[1] = app.regionA[1] + 8;
                app.regionSet = true;
            }
            else if (found)
            {
                Brush b;
                b.radius = 500;
                app.is.BeginEdit("Demo");
                app.is.Raise(hit[0], hit[2], b, 180);
                app.is.Paint(hit[0] + 900, hit[2], b, static_cast<uint16_t>(app.paintSlot));
                app.is.EndEdit();
            }
        }
        const Uint64 frameFrom = SDL_GetPerformanceCounter();
        app.Frame(dt, width, height);
        check.ms.push_back(static_cast<float>(SDL_GetPerformanceCounter() - frameFrom) * 1000.0f / static_cast<float>(SDL_GetPerformanceFrequency()));

        ImGui::Render();
        if (device->BindBackBufferForExternalDraw())
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), device->List());
        device->InvalidateBindings();
        if (checkModels && check.After(app, frames, device, width, height))
        {
            printf("%d checks passed, %d failed\n", check.passed, check.failed);
            app.is.modified = false;
            app.running = false;
            exitCode = check.failed ? 1 : 0;
        }
        frames++;
        if (!checkModels && !shot.empty() && frames >= shotFrames && (app.renderer.Models().waiting == 0 || frames >= shotFrames + 900))
        {
            const bool ok = SaveScreenshot(device, width, height, shot);
            const Renderer::ModelStats& ms = app.renderer.Models();
            printf("models: %d areas, %d draws, %d triangles, %d textures, %d waiting; frame %.1f ms\n", ms.areas, ms.draws, ms.triangles,
                   ms.textures, ms.waiting, dt * 1000.0f);
            printf(ok ? "picture written: %s\n" : "picture NOT written: %s\n", shot.c_str());
            app.is.modified = false;
            app.running = false;
        }
        device->Present();
        if (device->DebugLayerActive())
        {
            std::vector<std::string> messages;
            device->DrainDebugMessages(messages);
            for (const std::string& m : messages)
                fprintf(stderr, "D3D12: %s\n", m.c_str());
        }
    }

    // a scripted run leaves no trace: the game folder it was given is not the user's choice
    if (shot.empty() && !checkModels)
        app.SaveSettings();
    app.renderer.Shutdown();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    delete device;
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
