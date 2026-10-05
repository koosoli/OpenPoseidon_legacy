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
#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Poseidon
{
namespace
{
constexpr int ListId = 62001, FolderId = 62002, ScanId = 62003, OpenId = 62004, StatusId = 62005, BackId = 62006,
              MapsId = 62008, VehiclesId = 62009;
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
    bool _vehicleMode = false;
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
    void Status(const std::string& text)
    {
        if (auto* ctrl = dynamic_cast<CStatic*>(GetCtrl(StatusId)))
            ctrl->SetText(text.c_str());
    }
    void Scan()
    {
        if (_scan.valid())
            return;
        std::string folder;
        if (auto* edit = dynamic_cast<CEdit*>(GetCtrl(FolderId)))
            folder = edit->GetText().Data();
        Status("Searching installed Steam games... (archive directories only)");
        _scan = std::async(std::launch::async, [folder] { return ScanLocalMaps(folder); });
    }
    void Open()
    {
        auto* list = dynamic_cast<CListBox*>(GetCtrl(ListId));
        const int index = list ? list->GetCurSel() : -1;
        if (index < 0 || index >= static_cast<int>(_vehicleMode ? _vehicles.size() : _catalog.maps.size()))
            return;
        const LocalMap* map = _vehicleMode ? nullptr : &_catalog.maps[index];
        const LocalVehicle* vehicle = _vehicleMode ? &_vehicles[index] : nullptr;
        if ((map && !map->supported) || (vehicle && !vehicle->ready))
        {
            Status("This selected content is not supported or its local bridge is incomplete.");
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
                             (_vehicleMode ? "LocalVehicleBrowser.Eden" : "LocalMapBrowser.Eden");
        std::filesystem::create_directories(mission, ec);
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
        if (ec || !sqm)
        {
            Status("Cannot write the local map preview mission.");
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
                                         std::to_string(GetCurrentProcessId()),
                                         "--test-mission",
                                         mission.string()};
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
        AddControl("RscText", "Title", 62007, "LOCAL CONTENT", 0.06f, 0.15f, 0.40f, 0.05f);
        AddControl("RscButton", "MapsTab", MapsId, "Maps", 0.51f, 0.15f, 0.20f, 0.05f);
        AddControl("RscButton", "VehiclesTab", VehiclesId, "Vehicles", 0.73f, 0.15f, 0.21f, 0.05f);
        AddControl("RscEdit", "Folder", FolderId, "", 0.06f, 0.22f, 0.65f, 0.05f);
        AddControl("RscButton", "Scan", ScanId, "Search / refresh", 0.73f, 0.22f, 0.21f, 0.05f);
        AddControl("RscListBox", "Maps", ListId, "", 0.06f, 0.29f, 0.88f, 0.38f);
        AddControl("RscText", "Status", StatusId, "", 0.06f, 0.69f, 0.88f, 0.08f);
        AddControl("RscButton", "Open", OpenId, "Open selected", 0.06f, 0.78f, 0.35f, 0.05f);
        AddControl("RscButton", "Back", BackId, "Back", 0.74f, 0.78f, 0.20f, 0.05f);
        _vehicles = ScanLocalVehicles(std::filesystem::current_path());
        Scan();
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
            if (_vehicleMode)
                for (const auto& vehicle : _vehicles)
                    list->AddString((vehicle.label + (vehicle.ready ? " (local bridge)" : " (incomplete)")).c_str());
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
        Status(_vehicleMode ? "Local vehicle bridges use inherited OFP physics; original animations/features may "
                              "differ. Open restarts engine."
                            : std::to_string(_catalog.maps.size()) +
                                  " maps found. Open in the OFP mission editor in this window. Current format support; "
                                  "place OFP units and use Preview to play.");
    }
    void OnButtonClicked(int idc) override
    {
        if (idc == BackId)
            Exit(2);
        else if (idc == ScanId)
            Scan();
        else if (idc == OpenId)
            Open();
        else if (idc == MapsId || idc == VehiclesId)
        {
            _vehicleMode = idc == VehiclesId;
            Fill();
        }
        else
            Display::OnButtonClicked(idc);
    }
    void OnSimulate(EntityAI* vehicle) override
    {
        if (_scan.valid() && _scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            try
            {
                _catalog = _scan.get();
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
} // namespace Poseidon
