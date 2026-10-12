// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/UI/LocalMapsDisplay.hpp>
#include <Poseidon/UI/LocalMapsCatalog.hpp>
#include <Poseidon/UI/LocalMapEditor.hpp>
#include <Poseidon/UI/LocalVehiclesCatalog.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/Controls/UIControlsWidgets.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Poseidon
{
namespace
{
constexpr int ListId = 62001, FolderId = 62002, ScanId = 62003, OpenId = 62004, StatusId = 62005, BackId = 62006,
              MapsId = 62008, VehiclesId = 62009, MissionsId = 62011, BrowseId = 62012;
enum class BrowserMode { Maps, Vehicles, Missions };
class LocalBrowserButton : public CButton
{
  public:
    LocalBrowserButton(ControlsContainer* parent, int id, const ParamEntry& cls) : CButton(parent, id, cls)
    {
        _color1 = GetPackedColor(cls >> "colorBackground");
        _color2 = GetPackedColor(cls >> "colorActive");
        _color3 = _color1;
        _color4 = _color2;
        _color5 = _color1;
    }
};
class LocalMapsDisplay : public Display
{
    ParamFile _resources;
    std::future<LocalMapsCatalog> _scan;
    LocalMapsCatalog _catalog;
    std::vector<LocalVehicle> _vehicles;
    BrowserMode _mode = BrowserMode::Maps;
    // Missions tab as a collapsible tree in the list box: game -> campaign/collection -> mission.
    // One row per visible list line; a header row toggles, a mission row opens.
    struct MissionRow
    {
        std::string game, collection; // collection empty on a game row
        int mission = -1;             // index into _catalog.missions, -1 on a header row
    };
    std::vector<MissionRow> _missionRows;
    std::set<std::string> _expanded; // "game" and "game|collection" keys

    void FillMissionTree(CListBox* list)
    {
        _missionRows.clear();
        // The catalog is sorted playable-first; group by game and collection in first-seen order.
        std::vector<std::string> games;
        std::map<std::string, std::vector<std::string>> collections;
        std::map<std::string, std::pair<int, int>> totals; // key -> (playable, all)
        for (const LocalMission& mission : _catalog.missions)
        {
            if (std::find(games.begin(), games.end(), mission.game) == games.end())
                games.push_back(mission.game);
            auto& names = collections[mission.game];
            if (std::find(names.begin(), names.end(), mission.collection) == names.end())
                names.push_back(mission.collection);
            for (const std::string& key : {mission.game, mission.game + "|" + mission.collection})
            {
                totals[key].first += mission.supported ? 1 : 0;
                totals[key].second += 1;
            }
        }
        const auto count = [&](const std::string& key)
        {
            const auto [playable, all] = totals[key];
            return "  (" + std::to_string(playable) + " of " + std::to_string(all) + " playable)";
        };
        for (const std::string& game : games)
        {
            const bool open = _expanded.contains(game);
            list->AddString(((open ? "[-] " : "[+] ") + game + count(game)).c_str());
            _missionRows.push_back({game, {}, -1});
            if (!open)
                continue;
            for (const std::string& collection : collections[game])
            {
                const std::string key = game + "|" + collection;
                const bool sub = _expanded.contains(key);
                list->AddString(("      " + std::string(sub ? "[-] " : "[+] ") + collection + count(key)).c_str());
                _missionRows.push_back({game, collection, -1});
                if (!sub)
                    continue;
                for (size_t i = 0; i < _catalog.missions.size(); ++i)
                {
                    const LocalMission& mission = _catalog.missions[i];
                    if (mission.game != game || mission.collection != collection)
                        continue;
                    list->AddString(("              " + mission.name +
                                     (mission.supported ? "" : "   -- " + mission.reason))
                                        .c_str());
                    _missionRows.push_back({game, collection, static_cast<int>(i)});
                }
            }
        }
    }
    // Header row: expand/collapse and keep it selected. Returns true when the row was a header.
    bool ToggleMissionRow(int index)
    {
        if (index < 0 || index >= static_cast<int>(_missionRows.size()) || _missionRows[index].mission >= 0)
            return false;
        const MissionRow& row = _missionRows[index];
        const std::string key = row.collection.empty() ? row.game : row.game + "|" + row.collection;
        if (!_expanded.erase(key))
            _expanded.insert(key);
        Fill();
        if (auto* list = dynamic_cast<CListBox*>(GetCtrl(ListId)))
            list->SetCurSel(index);
        return true;
    }
    void AddControl(const char* base, const char* name, int id, const char* text, float x, float y, float w, float h)
    {
        const ParamEntry* inherited = Res.FindEntry(base);
        if (!inherited || !inherited->IsClass())
            return;
        ParamClass* cls = _resources.AddClass(name);
        cls->Add("idc", id);
        cls->Add("text", RString(text));
        cls->Add("x", x);
        cls->Add("y", y);
        cls->Add("w", w);
        cls->Add("h", h);
        cls->Add("sizeEx", id == StatusId ? 0.025f : 0.032f);
        if (const ParamEntry* inheritedSize = inherited->FindEntry("size"))
            cls->Add("size", static_cast<float>(*inheritedSize) * 1.6f);
        cls->Add("rowHeight", 0.033f);
        const auto color = [&](const char* key, float r, float g, float b, float a)
        {
            ParamEntry* values = cls->AddArray(key);
            for (float value : {r, g, b, a})
                values->AddValue(value);
        };
        color("colorText", 0.94f, 0.95f, 0.97f, 1.0f);
        // A list box draws its selection bar in colorText and the selected row's text in colorSelect,
        // so white here was white-on-white; dark navy reads on the light bar.
        if (id == ListId)
            color("colorSelect", 0.03f, 0.08f, 0.14f, 1.0f);
        else
            color("colorSelect", 1.0f, 1.0f, 1.0f, 1.0f);
        color("colorActive", 0.24f, 0.38f, 0.49f, 1.0f);
        const bool button = std::string(base) == "RscButton";
        color("colorBackground", button ? 0.12f : 0.04f, button ? 0.19f : 0.05f, button ? 0.25f : 0.065f,
              id == 62010 ? 0.94f : (button || id == FolderId ? 1.0f : 0.0f));
        if (id == StatusId)
        {
            cls->Add("style", ST_MULTI);
            cls->Add("lineSpacing", 1.0f);
        }
        cls->SetBase(inherited->GetClassInterface());
        LoadControl(*cls);
    }
    static std::filesystem::path FolderMemory()
    {
        return std::filesystem::path(Foundation::GamePaths::Instance().UserDir()) / "ArmaContentFolder.txt";
    }
    void Status(const std::string& text)
    {
        if (auto* ctrl = dynamic_cast<CStatic*>(GetCtrl(StatusId)))
            ctrl->SetText(text.c_str());
    }
    // The OS folder picker answers on its own thread, possibly after this display is gone, so the
    // answer goes through a process-wide slot that OnSimulate drains.
    struct PickedFolder
    {
        std::mutex lock;
        std::string path;
        bool ready = false;
    };
    static PickedFolder& Picked()
    {
        static PickedFolder picked;
        return picked;
    }
    static void OnFolderPicked(void*, const char* const* files, int)
    {
        if (!files || !files[0])
        {
            LOG_INFO(Core, "Arma content: folder picker {} ({})", files ? "cancelled" : "failed", SDL_GetError());
            return;
        }
        PickedFolder& picked = Picked();
        std::lock_guard guard(picked.lock);
        picked.path = files[0];
        picked.ready = true;
    }
    void Browse()
    {
        std::string start;
        if (auto* edit = dynamic_cast<CEdit*>(GetCtrl(FolderId)))
            start = edit->GetText().Data();
        LOG_INFO(Core, "Arma content: opening the folder picker");
        Status("Choose the folder that holds an Arma install or ALDP_* data packages...");
        SDL_ShowOpenFolderDialog(&OnFolderPicked, nullptr, SDL_GetKeyboardFocus(),
                                 start.empty() ? nullptr : start.c_str(), false);
    }
    // `remember`: an explicit search or browse saves the folder for the next visit (a data package
    // or a non-Steam install lives outside Steam); the automatic scan on opening does not, so an
    // empty box at start-up never wipes a remembered folder.
    // A search asked for while one is running (the automatic scan on opening takes a few seconds)
    // runs as soon as it finishes. Dropping it silently was why a typed or browsed folder never
    // got scanned -- or remembered.
    bool _rescanPending = false;
    bool _rescanRemember = false;
    void Scan(bool remember = true)
    {
        if (_scan.valid())
        {
            _rescanPending = true;
            _rescanRemember = _rescanRemember || remember;
            Status("Search queued; finishing the current scan first...");
            return;
        }
        std::string folder;
        if (auto* edit = dynamic_cast<CEdit*>(GetCtrl(FolderId)))
            folder = edit->GetText().Data();
        if (remember && Foundation::GamePaths::Instance().IsInitialized())
            std::ofstream(FolderMemory(), std::ios::trunc) << folder;
        Status("Searching installed Steam games... (archive directories only)");
        LOG_INFO(Core, "Arma content: scanning Steam libraries{}{}", folder.empty() ? "" : " and ", folder);
        _scan = std::async(std::launch::async, [folder] { return ScanLocalMaps(folder); });
    }
    void Open()
    {
        auto* list = dynamic_cast<CListBox*>(GetCtrl(ListId));
        const int index = list ? list->GetCurSel() : -1;
        const size_t count = _mode == BrowserMode::Vehicles   ? _vehicles.size()
                             : _mode == BrowserMode::Missions ? _missionRows.size()
                                                              : _catalog.maps.size();
        if (index < 0 || index >= static_cast<int>(count))
            return;
        if (_mode == BrowserMode::Missions && ToggleMissionRow(index))
            return;
        const LocalMap* map = _mode == BrowserMode::Maps ? &_catalog.maps[index] : nullptr;
        const LocalVehicle* vehicle = _mode == BrowserMode::Vehicles ? &_vehicles[index] : nullptr;
        const LocalMission* localMission =
            _mode == BrowserMode::Missions ? &_catalog.missions[_missionRows[index].mission] : nullptr;
        if ((map && !map->supported) || (vehicle && !vehicle->ready))
        {
            Status("This selected content is not supported or its local bridge is incomplete.");
            return;
        }
        if (localMission && !localMission->supported)
        {
            Status("This mission is not playable yet: " + localMission->reason + ".");
            return;
        }
        if (map)
        {
            std::string error;
            if (!RequestLocalMapEditor(*map, error)) Status(error);
            return;
        }
#ifdef _WIN32
        // A new process isolates foreign content and waits for THIS process to close before booting.
        // No shell command or foreign configuration is executed; source archives stay read-only.
        std::error_code ec;
        const auto mission = std::filesystem::path(Foundation::GamePaths::Instance().CacheDir()) /
                             (vehicle ? "LocalVehicleBrowser.Eden" : "LocalMapBrowser.Eden");
        std::filesystem::create_directories(mission, ec);
        // A found mission is played from its own archive (--arma1-mission); only maps and vehicles
        // get a generated preview mission.
        if (!localMission)
        {
            std::ofstream sqm(mission / "mission.sqm");
            if (vehicle)
                sqm << MakeLocalVehicleMission(*vehicle);
            else
                sqm << "version=11; class Mission { randomSeed=1; class Intel { resistanceWest=0; resistanceEast=1; "
                       "startWeather=0.25; startFog=0; year=1985; month=6; day=21; hour=12; minute=0; }; "
                       "class Groups { items=1; class Item0 { side=\"WEST\"; class Vehicles { items=1; "
                       "class Item0 { position[]={1000,0,1000}; azimut=0; id=0; side=\"WEST\"; vehicle=\"SoldierWB\"; "
                       "player=\"PLAYER COMMANDER\"; skill=0.6; }; }; }; }; };";
            sqm.close();
            if (!sqm)
                ec = std::make_error_code(std::errc::io_error);
        }
        if (ec)
        {
            Status("Cannot write the local preview mission.");
            return;
        }
        wchar_t exePath[32768]{};
        if (!GetModuleFileNameW(nullptr, exePath, 32768))
        {
            Status("Cannot locate the engine executable.");
            return;
        }
        // SDL accepts UTF-8 arguments, handling spaces without command concatenation.
        const auto executable = std::filesystem::path(exePath).u8string();
        std::string roots;
        if (map)
            for (const auto& root : map->archiveRoots)
            {
                if (!roots.empty())
                    roots += ';';
                roots += root;
            }
        std::vector<std::string> args = {std::string(executable.begin(), executable.end()),
                                         "--render=" + Foundation::AppConfig::Instance().GetRenderBackend(),
                                         "--wait-for-parent",
                                         std::to_string(GetCurrentProcessId())};
        if (localMission)
            // The game's own archives are mounted read-only; nothing is extracted (Arma1Compat).
            args.insert(args.end(), {"--arma1", localMission->installRoot, "--arma1-mission",
                                     localMission->virtualPath});
        else
            args.insert(args.end(), {"--test-mission", mission.string()});
        if (GEngine)
        {
            const WindowMode mode = GEngine->GetCurrentWindowMode();
            args.insert(args.end(), {"--display-mode", mode == WindowMode::Windowed ? "windowed" :
                                     mode == WindowMode::Borderless ? "borderless" : "exclusive",
                                     "--width", std::to_string(GEngine->Width()),
                                     "--height", std::to_string(GEngine->Height())});
        }
        if (Foundation::AppConfig::Instance().DevMode())
            args.push_back("--dev");
        if (const int port = Foundation::AppConfig::Instance().GetHarnessPort(); port > 0)
        {
            args.push_back("--harness");
            args.push_back(std::to_string(port));
        }
        args.push_back("--log-file");
        args.push_back((mission.parent_path() / "local-map-browser.log").string());
        if (vehicle)
        {
            args.push_back("--mod");
            args.push_back(vehicle->modPath);
        }
        if (map)
        {
            args.insert(args.end(), {"--test-world", map->enfusion ? map->archiveRoot : map->worldPath});
            if (map->enfusion && map->name == "Everon")
                // Start in the owner's working village view, also centring nearby object import there.
                args.insert(args.end(), {"--test-world-freefly", "5176", "3985", "20.5", "90", "5"});
            else
                args.insert(args.end(), {"--test-world-land-start", "--test-world-freefly",
                                         "1000", "1000", "250", "0", "-15"});
            if (map->enfusion)
            {
                args.push_back("--reforger-world");
                args.push_back(map->worldPath);
            }
            else
            {
                args.push_back("--map-archive-root");
                args.push_back(roots);
            }
        }
        std::vector<const char*> argv;
        for (const auto& arg : args)
            argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        SDL_Process* process = nullptr;
        std::string launchError;
        if (map && map->enfusion)
        {
            // Object import is opt-in in the native loader. Give this preview a bounded
            // child-only environment; retain explicit overrides and the session's other settings.
            SDL_Environment* environment = SDL_CreateEnvironment(true);
            const SDL_PropertiesID properties = SDL_CreateProperties();
            const auto setDefault = [&](const char* name, const char* value)
            {
                const char* existing = SDL_GetEnvironmentVariable(environment, name);
                return (existing && *existing) || SDL_SetEnvironmentVariable(environment, name, value, true);
            };
            const bool configured = environment && properties &&
                setDefault("POSEIDON_REFORGER_OBJECTS", "1") &&
                setDefault("POSEIDON_REFORGER_MAX_MODELS", "800") &&
                setDefault("POSEIDON_REFORGER_NEAR_RADIUS", "300") &&
                setDefault("POSEIDON_REFORGER_MAX_OBJECTS", "400000") &&
                SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data()) &&
                SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
            if (configured)
            {
                SDL_Log("Local Reforger preview: objects=%s models=%s nearRadius=%s maxObjects=%s",
                        SDL_GetEnvironmentVariable(environment, "POSEIDON_REFORGER_OBJECTS"),
                        SDL_GetEnvironmentVariable(environment, "POSEIDON_REFORGER_MAX_MODELS"),
                        SDL_GetEnvironmentVariable(environment, "POSEIDON_REFORGER_NEAR_RADIUS"),
                        SDL_GetEnvironmentVariable(environment, "POSEIDON_REFORGER_MAX_OBJECTS"));
                process = SDL_CreateProcessWithProperties(properties);
            }
            if (!process)
                launchError = SDL_GetError();
            if (properties)
                SDL_DestroyProperties(properties);
            if (environment)
                SDL_DestroyEnvironment(environment);
        }
        else
        {
            process = SDL_CreateProcess(argv.data(), false);
            if (!process)
                launchError = SDL_GetError();
        }
        if (!process)
        {
            Status("Cannot open map: " + launchError);
            return;
        }
        SDL_DestroyProcess(process);
        Application::Instance().m_validateQuit = true;
        Application::Instance().m_closeRequest = true;
#else
        Status("Local map restart is currently available on Windows.");
#endif
    }

  public:
    explicit LocalMapsDisplay(ControlsContainer* parent) : Display(parent)
    {
        _idd = LocalMapsMenuId;
        AddControl("RscText", "Panel", 62010, "", 0.04f, 0.13f, 0.92f, 0.72f);
        AddControl("RscText", "Title", 62007, "ARMA CONTENT", 0.06f, 0.15f, 0.30f, 0.05f);
        AddControl("RscButton", "MapsTab", MapsId, "Maps", 0.38f, 0.15f, 0.18f, 0.05f);
        AddControl("RscButton", "VehiclesTab", VehiclesId, "Vehicles", 0.57f, 0.15f, 0.18f, 0.05f);
        AddControl("RscButton", "MissionsTab", MissionsId, "Missions", 0.76f, 0.15f, 0.18f, 0.05f);
        AddControl("RscEdit", "Folder", FolderId, "", 0.06f, 0.22f, 0.50f, 0.05f);
        AddControl("RscButton", "Browse", BrowseId, "Browse...", 0.57f, 0.22f, 0.15f, 0.05f);
        AddControl("RscButton", "Scan", ScanId, "Search / refresh", 0.73f, 0.22f, 0.21f, 0.05f);
        AddControl("RscListBox", "Maps", ListId, "", 0.06f, 0.29f, 0.88f, 0.38f);
        AddControl("RscText", "Status", StatusId, "", 0.06f, 0.69f, 0.88f, 0.08f);
        AddControl("RscButton", "Open", OpenId, "Open selected", 0.06f, 0.78f, 0.35f, 0.05f);
        AddControl("RscButton", "Back", BackId, "Back", 0.74f, 0.78f, 0.20f, 0.05f);
        _vehicles = ScanLocalVehicles(std::filesystem::current_path());
        if (Foundation::GamePaths::Instance().IsInitialized())
        {
            std::string remembered;
            std::getline(std::ifstream(FolderMemory()), remembered);
            if (auto* edit = dynamic_cast<CEdit*>(GetCtrl(FolderId)); edit && !remembered.empty())
                edit->SetText(remembered.c_str());
        }
        Scan(false); // opening the screen is not a choice of folder; keep the remembered one
    }
    Control* OnCreateCtrl(int type, int idc, const ParamEntry& cls) override
    {
        if (type == CT_BUTTON)
            return new LocalBrowserButton(this, idc, cls);
        return Display::OnCreateCtrl(type, idc, cls);
    }
    void Fill()
    {
        if (auto* list = dynamic_cast<CListBox*>(GetCtrl(ListId)))
        {
            list->RemoveAll();
            if (_mode == BrowserMode::Vehicles)
                for (const auto& vehicle : _vehicles)
                    list->AddString((vehicle.label + (vehicle.ready ? " (local bridge)" : " (incomplete)")).c_str());
            else if (_mode == BrowserMode::Missions)
                FillMissionTree(list);
            else
                for (const auto& map : _catalog.maps)
                {
                    const auto label =
                        map.game + " | " + map.name + " | " +
                        (map.enfusion ? "native Enfusion preview" : "OPRW " + std::to_string(map.revision)) +
                        (map.supported ? " (preview)" : " (unsupported)");
                    list->AddString(label.c_str());
                }
            list->SetCurSel(0);
        }
        if (_mode == BrowserMode::Vehicles)
            Status("Local vehicle bridges use inherited OFP physics; original animations/features may differ. Open "
                   "restarts engine.");
        else if (_mode == BrowserMode::Missions)
        {
            const auto playable = std::count_if(_catalog.missions.begin(), _catalog.missions.end(),
                                                [](const LocalMission& m) { return m.supported; });
            Status(std::to_string(_catalog.missions.size()) + " missions found, " + std::to_string(playable) +
                   " playable. Double-click a game or campaign to open it. Missions play from the game's own "
                   "archives (nothing is copied); units the engine lacks are stood in for by OFP ones. Open restarts "
                   "engine. Not a Steam install? Browse... to its folder.");
        }
        else
            Status(std::to_string(_catalog.maps.size()) +
                   " maps found. Open in the OFP mission editor in this window. Current format support; "
                   "place OFP units and use Preview to play.");
    }
    void OnButtonClicked(int idc) override
    {
        if (idc == BackId)
            Exit(2);
        else if (idc == ScanId)
            Scan();
        else if (idc == BrowseId)
            Browse();
        else if (idc == OpenId)
            Open();
        else if (idc == MapsId || idc == VehiclesId || idc == MissionsId)
        {
            _mode = idc == VehiclesId   ? BrowserMode::Vehicles
                    : idc == MissionsId ? BrowserMode::Missions
                                        : BrowserMode::Maps;
            Fill();
        }
        else
            Display::OnButtonClicked(idc);
    }
    void OnLBDblClick(int idc, int curSel) override
    {
        if (idc == ListId)
            Open(); // a header row expands/collapses, an item opens
        else
            Display::OnLBDblClick(idc, curSel);
    }
    void OnSimulate(EntityAI* vehicle) override
    {
        {
            PickedFolder& picked = Picked();
            std::lock_guard guard(picked.lock);
            if (picked.ready)
            {
                picked.ready = false;
                if (auto* edit = dynamic_cast<CEdit*>(GetCtrl(FolderId)))
                    edit->SetText(picked.path.c_str());
                _mode = BrowserMode::Missions;
                Scan();
            }
        }
        if (_scan.valid() && _scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            try
            {
                _catalog = _scan.get();
                LOG_INFO(Core, "Arma content: scan found {} maps, {} missions ({} diagnostics)", _catalog.maps.size(),
                         _catalog.missions.size(), _catalog.diagnostics.size());
                if (_rescanPending)
                {
                    _rescanPending = false;
                    const bool remember = std::exchange(_rescanRemember, false);
                    Fill();
                    Scan(remember);
                    Display::OnSimulate(vehicle);
                    return;
                }
            }
            catch (const std::exception& e)
            {
                Status(e.what());
                Display::OnSimulate(vehicle);
                return;
            }
            Fill();
        }
        Display::OnSimulate(vehicle);
    }
};
} // namespace
Display* CreateLocalMapsDisplay(ControlsContainer* parent)
{
    return new LocalMapsDisplay(parent);
}

bool LaunchIslandEditor(std::string& error)
{
#ifdef _WIN32
    wchar_t exePath[32768]{};
    if (!GetModuleFileNameW(nullptr, exePath, 32768))
    {
        error = "Cannot locate the engine executable.";
        return false;
    }
    const std::filesystem::path folder = std::filesystem::path(exePath).parent_path();
    std::filesystem::path editor = folder / "PoseidonIslandEditor.exe";
    std::error_code ec;
    // the shareable build (exe + dll + assets) carries the editor in assets
    if (!std::filesystem::exists(editor, ec))
        editor = folder / "assets" / "PoseidonIslandEditor.exe";
    if (!std::filesystem::exists(editor, ec))
    {
        error = "PoseidonIslandEditor.exe is not installed next to the game.";
        return false;
    }
    const auto editorUtf8 = editor.u8string();
    const auto folderUtf8 = folder.u8string();
    const std::string editorArg(editorUtf8.begin(), editorUtf8.end());
    const std::string folderArg(folderUtf8.begin(), folderUtf8.end());
    // --game: the islands' ground textures and models are read from this install's archives.
    const char* argv[] = {editorArg.c_str(), "--game", folderArg.c_str(), nullptr};
    SDL_Process* process = SDL_CreateProcess(argv, false);
    if (!process)
    {
        error = std::string("Cannot start the Island Editor: ") + SDL_GetError();
        return false;
    }
    SDL_DestroyProcess(process); // detached: the editor outlives this handle
    LOG_INFO(UI, "Island Editor started: {} --game {}", editorArg, folderArg);
    return true;
#else
    error = "The Island Editor draws with Direct3D 12 and runs on Windows only.";
    return false;
#endif
}
} // namespace Poseidon
