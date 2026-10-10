// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/UI/WholeWorldDisplay.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/Controls/UIControlsWidgets.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/World/Terrain/EarthProviders.hpp>
#include <Poseidon/World/Terrain/EarthFrame.hpp>
#include <Poseidon/World/Terrain/EarthPlannerMap.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#undef DrawText
#endif

namespace Poseidon
{
namespace
{
constexpr int ListId = 62101, LatId = 62102, LonId = 62103, AltId = 62104, ImageryId = 62105, ElevationId = 62106,
              KeyId = 62107, StatusId = 62108, EnterId = 62109, BackId = 62110, PanelId = 62111, ContentId = 62117, StartId = 62118, DayCycleId = 62119;

constexpr int PlanId = 62120;

struct Place
{
    const char* name;
    double latitude, longitude, altitude;
};
constexpr Place Places[] = {
    {"Swiss Alps, Jungfrau", 46.537, 7.962, 4500},
    {"Luxembourg City (prepared imagery)", 49.611, 6.130, 2000},
    {"Grand Canyon, USA", 36.100, -112.110, 4000},
    {"Mount Everest, Himalaya", 27.988, 86.925, 9500},
    {"Manhattan, New York", 40.758, -73.985, 1500},
    {"Sognefjord, Norway", 61.150, 6.700, 3000},
    {"Big Island, Hawaii", 19.600, -155.500, 12000},
    {"Earth from orbit", 30.000, 10.000, 18000000},
};

std::string Utf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path WorldStreamCache()
{
    // Same folder the Launch-WholeWorld.ps1 script and the prepared caches use.
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        return std::filesystem::u8path(local) / "OpenPoseidon" / "world-stream";
    return std::filesystem::path(Foundation::GamePaths::Instance().CacheDir()) / "world-stream";
}

class WholeWorldButton : public CButton
{
  public:
    WholeWorldButton(ControlsContainer* parent, int id, const ParamEntry& cls) : CButton(parent, id, cls)
    {
        _color1 = GetPackedColor(cls >> "colorBackground");
        _color2 = GetPackedColor(cls >> "colorActive");
        _color3 = _color1;
        _color4 = _color2;
        _color5 = _color1;
    }
};

// The Whole World screens build their controls in code (the remaster's resource set has no class for them).
class WholeWorldScreen : public Display
{
  protected:
    ParamFile _resources;
    using Display::Display;

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
        const bool multiLine = id == StatusId;
        cls->Add("sizeEx", multiLine ? 0.025f : 0.032f);
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
        const bool button = std::string(base) == "RscButton", edit = std::string(base) == "RscEdit";
        color("colorBackground", button ? 0.12f : 0.04f, button ? 0.19f : 0.05f, button ? 0.25f : 0.065f,
              id == PanelId ? 0.94f : (button || edit ? 1.0f : 0.0f));
        if (multiLine)
        {
            cls->Add("style", ST_MULTI);
            cls->Add("lineSpacing", 1.0f);
        }
        cls->SetBase(inherited->GetClassInterface());
        LoadControl(*cls);
    }
    std::string Text(int id)
    {
        if (auto* edit = dynamic_cast<CEdit*>(GetCtrl(id)))
            return edit->GetText().Data();
        return {};
    }
    void SetEdit(int id, const std::string& text)
    {
        if (auto* edit = dynamic_cast<CEdit*>(GetCtrl(id)))
            edit->SetText(text.c_str());
    }
    void SetLabel(int id, const std::string& text)
    {
        if (auto* button = dynamic_cast<CButton*>(GetCtrl(id)))
            button->SetText(text.c_str());
        else if (auto* label = dynamic_cast<CStatic*>(GetCtrl(id)))
            label->SetText(text.c_str());
    }
    void Status(const std::string& text) { SetLabel(StatusId, text); }
    static std::string Number(double value, int decimals)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
        return buffer;
    }
    Control* OnCreateCtrl(int type, int idc, const ParamEntry& cls) override
    {
        if (type == CT_BUTTON)
            return new WholeWorldButton(this, idc, cls);
        return Display::OnCreateCtrl(type, idc, cls);
    }
};

class WholeWorldDisplay : public WholeWorldScreen
{
    EarthProviders::Config _config;
    int _imagery = 0, _elevation = 0, _selected = -1;
    bool _onFoot = false; // start as the soldier on the ground instead of the free-flight camera
    bool _dayCycle = false; // test aid: one day per minute
    bool _editor = false; // open the mission editor (Plan mission) instead of playing

    const EarthProviders::Preset& Imagery() const { return EarthProviders::ImageryPresets()[_imagery]; }
    const EarthProviders::Preset& Elevation() const { return EarthProviders::ElevationPresets()[_elevation]; }
    void ShowProviders()
    {
        SetLabel(ImageryId, std::string("Imagery: ") + Imagery().label);
        SetLabel(ElevationId, std::string("Elevation: ") + Elevation().label);
        const bool osm = _config.contentId == "overpass";
        SetLabel(ContentId, osm ? "3D buildings, roads: OpenStreetMap" : "3D buildings, roads: off");
        SetLabel(StartId, _onFoot ? "Start: on foot (soldier)" : "Start: free flight camera");
        SetLabel(DayCycleId, _dayCycle ? "Day cycle: 24 h per minute (test)" : "Day cycle: real time");
        std::string note = "Imagery: ";
        note += Imagery().attribution;
        note += ". Elevation: ";
        note += Elevation().attribution;
        note += ".";
        if (Imagery().needsKey || Elevation().needsKey)
            note += " Enter your own API key; it is stored in providers.cfg in your local app data.";
        if (std::string(Imagery().id) == "esri")
            note += " Check Esri's terms of use for your purpose.";
        if (_config.contentId == "overpass")
            note += " Buildings/roads: OpenStreetMap contributors (ODbL), fetched cell by cell from the Overpass API"
                    " and cached; a busy public server can be slow -- set content_url to your own.";
        if (std::string(Imagery().id) == "custom" && _config.imageryUrl.empty())
            note += " Custom needs imagery_url in providers.cfg ({z}/{x}/{y}, {q}, {key}).";
        Status(note);
    }
    void Pick(int index)
    {
        if (index < 0 || index >= int(std::size(Places)))
            return;
        _selected = index;
        SetEdit(LatId, Number(Places[index].latitude, 4));
        SetEdit(LonId, Number(Places[index].longitude, 4));
        SetEdit(AltId, Number(Places[index].altitude, 0));
    }

    void Enter()
    {
        const auto parse = [&](int id, double& value)
        {
            const std::string text = Text(id);
            char* end = nullptr;
            value = std::strtod(text.c_str(), &end);
            return !text.empty() && end && *end == 0 && std::isfinite(value);
        };
        double lat = 0, lon = 0, alt = 0;
        if (!parse(LatId, lat) || !parse(LonId, lon) || !parse(AltId, alt) || std::abs(lat) > 84 ||
            std::abs(lon) > 180 || alt < 500 || alt > 30000000)
        {
            Status("Latitude must be within +-84, longitude within +-180, altitude 500 m to 30,000 km.");
            return;
        }
        const std::string key = Text(KeyId);
        if ((Imagery().needsKey || Elevation().needsKey) && key.empty())
        {
            Status("The selected provider needs an API key.");
            return;
        }
        const auto cache = WorldStreamCache();
        _config.imageryId = Imagery().id;
        _config.elevationId = Elevation().id;
        if (Imagery().needsKey || !key.empty())
            _config.imageryKey = key;
        if (Elevation().needsKey)
            _config.elevationKey = key;
        if (!EarthProviders::Save(cache, _config))
        {
            Status("Cannot write providers.cfg to " + Utf8(cache));
            return;
        }
        std::string error;
        if (!Launch(cache, lat, lon, alt, error))
            Status(error);
    }

    bool ParsePlace(double& lat, double& lon)
    {
        const auto parse = [&](int id, double& value)
        {
            const std::string text = Text(id);
            char* end = nullptr;
            value = std::strtod(text.c_str(), &end);
            return !text.empty() && end && *end == 0 && std::isfinite(value);
        };
        return parse(LatId, lat) && parse(LonId, lon) && std::abs(lat) <= 84 && std::abs(lon) <= 180;
    }
    // Plan mission: the same whole-world restart, but the start mission opens in OFP's mission editor
    // (units, waypoints, triggers, Preview), whose map shows the provider's satellite image there.
    void OpenPlanner()
    {
        _editor = true;
        Enter();
        _editor = false;
    }

    // The engine starts whole-world mode at boot (the terrain renderer is built for it), so like Local maps
    // this restarts the engine: a fresh process with the Earth environment, waiting for this one to close.
    bool Launch(const std::filesystem::path& cache, double lat, double lon, double alt, std::string& error)
    {
#ifdef _WIN32
        std::error_code ec;
        const auto fixture = cache / "fixture";
        const auto mission = fixture / "earth-preview.abel";
        const auto world = fixture / "earth-preview.wrp";
        std::filesystem::create_directories(mission, ec);
        {
            // An authored empty RVW4 container: no retail world, models or placements (as the launch script).
            std::vector<uint8_t> data(12 + 256 * 256 * 4 + 512 * 32 + 128, 0);
            std::memcpy(data.data(), "4WVR", 4);
            const uint32_t size = 256;
            std::memcpy(&data[4], &size, 4);
            std::memcpy(&data[8], &size, 4);
            const int16_t low = -100;
            for (size_t i = 0; i < 256 * 256; ++i)
            {
                std::memcpy(&data[12 + 2 * i], &low, 2);
                data[12 + 256 * 256 * 2 + 2 * i] = 1;
            }
            std::memcpy(&data[12 + 256 * 256 * 4], "landtext\\mo.pac", 15);
            std::memcpy(&data[12 + 256 * 256 * 4 + 32], "o\\pt.paa", 8);
            std::ofstream file(world, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
            std::ofstream sqm(mission / "mission.sqm", std::ios::trunc);
            // The full layout OFP's editor writes: the editor's loader (the Mission editor button opens this
            // file in it) refuses a mission without its empty Waypoints/Sensors/Markers classes, and reads the
            // weather as startWeather/forecastWeather. Starts at overcast 0.3.
            const char* intel = "  class Intel {startWeather=0.3;forecastWeather=0.3;startFog=0;forecastFog=0;"
                                "year=1985;month=6;day=21;hour=12;minute=0;};\n";
            const char* empty = "  class Vehicles {items=0;};\n  class Markers {items=0;};\n  class Sensors {items=0;};\n";
            sqm << "version=11;\nclass Mission\n{\n  addOns[]={};\n  addOnsAuto[]={};\n  randomSeed=42;\n" << intel
                << "  class Groups\n  {\n    items=1;\n    class Item0\n    {\n      side=\"WEST\";\n"
                   "      class Vehicles\n      {\n        items=1;\n        class Item0\n        {\n"
                   "          position[]={6400,0,6400};\n          id=0;\n          side=\"WEST\";\n"
                   "          vehicle=\"SoldierWB\";\n          player=\"PLAYER COMMANDER\";\n          leader=1;\n"
                   "          skill=0.6;\n        };\n      };\n"
                   "      class Waypoints {items=0;};\n      class Sensors {items=0;};\n    };\n  };\n"
                << empty << "};\n";
            for (const char* section : {"Intro", "OutroWin", "OutroLoose"})
                sqm << "class " << section << "\n{\n  addOns[]={};\n  addOnsAuto[]={};\n  randomSeed=1;\n" << intel
                    << "  class Groups {items=0;};\n" << empty << "};\n";
            // Test aid: a whole day in one minute (0.02 h every 0.05 s), to judge sunrise, noon, sunset
            // and night lighting from one spot. Off: no init script at all.
            const auto initScript = mission / "init.sqs";
            if (_dayCycle)
            {
                std::ofstream init(initScript, std::ios::trunc);
                // Broken cloud too, so the decks' lighting through dawn, noon, dusk and night can be judged.
                init << "; OpenPoseidon whole world: one day per minute (Day cycle test option)\n"
                        "0 setOvercast 0.55\n"
                        "#loop\n~0.05\nskipTime 0.02\ngoto \"loop\"\n";
            }
            else
                std::filesystem::remove(initScript, ec);
            if (!file || !sqm)
            {
                error = "Cannot write the whole-world start files to " + Utf8(fixture);
                return false;
            }
        }
        wchar_t exePath[32768]{};
        if (!GetModuleFileNameW(nullptr, exePath, 32768))
        {
            error = "Cannot locate the engine executable.";
            return false;
        }
        const std::string executable = Utf8(std::filesystem::path(exePath));
        const bool orbit = alt > 1000000;
        std::vector<std::string> args = {executable,
                                         "--render=wgpu",
                                         "--wait-for-parent",
                                         std::to_string(GetCurrentProcessId()),
                                         "--test-mission",
                                         Utf8(mission),
                                         "--test-world",
                                         Utf8(world),
                                         "--test-world-hour",
                                         "12",
                                         "--vd",
                                         "20000",
                                         "--log-file",
                                         Utf8((cache / "earth-preview.log"))};
        if (GEngine)
        {
            const WindowMode mode = GEngine->GetCurrentWindowMode();
            args.insert(args.end(), {"--display-mode", mode == WindowMode::Windowed ? "windowed" :
                                     mode == WindowMode::Borderless ? "borderless" : "exclusive",
                                     "--width", std::to_string(GEngine->Width()),
                                     "--height", std::to_string(GEngine->Height())});
        }
        // On foot the mission's soldier stands at map [6400,6400] -- the chosen place -- and is set down on the
        // streamed ground as it arrives (Man::Simulate); the near ground follows him as he moves.
        if (_editor)
            args.push_back("--test-editor");
        else if (!_onFoot)
            args.insert(args.end(), {"--test-world-freefly", "6400", "6400", Number(alt, 0), "30", orbit ? "-89.9" : "-20"});
        if (Foundation::AppConfig::Instance().DevMode())
            args.push_back("--dev"); // the Earth panel (Go to a place, clearance) lives in the dev panel
        std::vector<const char*> argv;
        for (const auto& arg : args)
            argv.push_back(arg.c_str());
        argv.push_back(nullptr);

        SDL_Environment* environment = SDL_CreateEnvironment(true);
        const SDL_PropertiesID properties = SDL_CreateProperties();
        const std::string cacheText = Utf8(cache), latText = Number(lat, 6), lonText = Number(lon, 6);
        const auto nasa = cache / "blue-marble-june-5400.jpg";
        const bool prepared = std::filesystem::exists(cache / "surface" / "lux-ortho-2023.jpg", ec) ||
                              std::filesystem::exists(cache / "surface" / "lux-context-2023.jpg", ec);
        bool configured = environment && properties;
        const auto set = [&](const char* name, const std::string& value)
        { configured = configured && SDL_SetEnvironmentVariable(environment, name, value.c_str(), true); };
        set("POSEIDON_WHOLE_WORLD", "1");
        set("POSEIDON_EARTH_LATITUDE", latText);
        set("POSEIDON_EARTH_LONGITUDE", lonText);
        set("POSEIDON_EARTH_CACHE", cacheText);
        set("POSEIDON_EARTH_IMAGERY", std::filesystem::exists(nasa, ec) ? Utf8(nasa) : std::string());
        set("POSEIDON_EARTH_SURFACE", prepared ? "1" : "0");
        const bool osm = _config.contentId == "overpass";
        set("POSEIDON_EARTH_CONTENT_OVERPASS", osm ? _config.contentUrl : std::string());
        set("POSEIDON_EARTH_BUILDINGS", osm ? "1" : "0");
        set("POSEIDON_EARTH_FEATURES", osm ? "1" : "0");
        set("WGR_GRASS", "0");
        set("WGR_TERRAIN_RESOURCE_REUSE", "1");
        configured = configured && SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data()) &&
                     SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
        SDL_Process* process = configured ? SDL_CreateProcessWithProperties(properties) : nullptr;
        if (!process)
            error = std::string("Cannot start whole-world mode: ") + SDL_GetError();
        if (properties)
            SDL_DestroyProperties(properties);
        if (environment)
            SDL_DestroyEnvironment(environment);
        if (!process)
            return false;
        SDL_DestroyProcess(process);
        Application::Instance().m_validateQuit = true;
        Application::Instance().m_closeRequest = true;
        return true;
#else
        (void)cache, (void)lat, (void)lon, (void)alt;
        error = "Whole-world restart is currently available on Windows.";
        return false;
#endif
    }

  public:
    explicit WholeWorldDisplay(ControlsContainer* parent) : WholeWorldScreen(parent)
    {
        _idd = WholeWorldMenuId;
        _config = EarthProviders::Load(WorldStreamCache());
        for (int i = 0; i < int(EarthProviders::ImageryPresets().size()); ++i)
            if (_config.imageryId == EarthProviders::ImageryPresets()[i].id)
                _imagery = i;
        for (int i = 0; i < int(EarthProviders::ElevationPresets().size()); ++i)
            if (_config.elevationId == EarthProviders::ElevationPresets()[i].id)
                _elevation = i;
        AddControl("RscText", "Panel", PanelId, "", 0.04f, 0.10f, 0.92f, 0.80f);
        AddControl("RscText", "Title", 62112, "WHOLE WORLD", 0.06f, 0.12f, 0.50f, 0.05f);
        AddControl("RscListBox", "Places", ListId, "", 0.06f, 0.19f, 0.42f, 0.44f);
        AddControl("RscText", "LatLabel", 62113, "Latitude", 0.52f, 0.19f, 0.16f, 0.05f);
        AddControl("RscEdit", "Lat", LatId, "", 0.70f, 0.19f, 0.24f, 0.05f);
        AddControl("RscText", "LonLabel", 62114, "Longitude", 0.52f, 0.25f, 0.16f, 0.05f);
        AddControl("RscEdit", "Lon", LonId, "", 0.70f, 0.25f, 0.24f, 0.05f);
        AddControl("RscText", "AltLabel", 62115, "Altitude (m)", 0.52f, 0.31f, 0.16f, 0.05f);
        AddControl("RscEdit", "Alt", AltId, "", 0.70f, 0.31f, 0.24f, 0.05f);
        AddControl("RscButton", "Imagery", ImageryId, "", 0.52f, 0.39f, 0.42f, 0.05f);
        AddControl("RscButton", "Elevation", ElevationId, "", 0.52f, 0.45f, 0.42f, 0.05f);
        AddControl("RscText", "KeyLabel", 62116, "API key", 0.52f, 0.52f, 0.16f, 0.05f);
        AddControl("RscEdit", "Key", KeyId, "", 0.70f, 0.52f, 0.24f, 0.05f);
        AddControl("RscButton", "Content", ContentId, "", 0.52f, 0.58f, 0.42f, 0.05f);
        AddControl("RscButton", "Start", StartId, "", 0.52f, 0.64f, 0.42f, 0.05f);
        AddControl("RscButton", "DayCycle", DayCycleId, "", 0.06f, 0.64f, 0.42f, 0.05f);
        AddControl("RscText", "Status", StatusId, "", 0.06f, 0.70f, 0.88f, 0.11f);
        AddControl("RscButton", "Enter", EnterId, "Enter whole world", 0.06f, 0.82f, 0.32f, 0.05f);
        AddControl("RscButton", "Plan", PlanId, "Mission editor", 0.40f, 0.82f, 0.32f, 0.05f);
        AddControl("RscButton", "Back", BackId, "Back", 0.74f, 0.82f, 0.20f, 0.05f);
        if (auto* list = dynamic_cast<CListBox*>(GetCtrl(ListId)))
        {
            for (const auto& place : Places)
                list->AddString(place.name);
            list->SetCurSel(0);
        }
        SetEdit(KeyId, !_config.imageryKey.empty() ? _config.imageryKey : _config.elevationKey);
        Pick(0);
        ShowProviders();
    }
    void OnButtonClicked(int idc) override
    {
        if (idc == PlanId)
            OpenPlanner();
        else if (idc == BackId)
            Exit(2);
        else if (idc == EnterId)
            Enter();
        else if (idc == ImageryId)
        {
            _imagery = (_imagery + 1) % int(EarthProviders::ImageryPresets().size());
            ShowProviders();
        }
        else if (idc == DayCycleId)
        {
            _dayCycle = !_dayCycle;
            ShowProviders();
        }
        else if (idc == StartId)
        {
            _onFoot = !_onFoot;
            ShowProviders();
        }
        else if (idc == ContentId)
        {
            _config.contentId = _config.contentId == "overpass" ? "none" : "overpass";
            ShowProviders();
        }
        else if (idc == ElevationId)
        {
            _elevation = (_elevation + 1) % int(EarthProviders::ElevationPresets().size());
            ShowProviders();
        }
        else
            Display::OnButtonClicked(idc);
    }
    void OnSimulate(EntityAI* vehicle) override
    {
        if (auto* list = dynamic_cast<CListBox*>(GetCtrl(ListId)); list && list->GetCurSel() != _selected)
            Pick(list->GetCurSel());
        Display::OnSimulate(vehicle);
    }
};
} // namespace
Display* CreateWholeWorldDisplay(ControlsContainer* parent)
{
    return new WholeWorldDisplay(parent);
}

std::string WholeWorldEditorMission()
{
    const char* cache = std::getenv("POSEIDON_EARTH_CACHE");
    const auto base = cache && *cache ? std::filesystem::u8path(cache) : WorldStreamCache();
    return Utf8(base / "fixture" / "earth-preview.abel" / "mission.sqm");
}

bool RelaunchWholeWorldEditor(double latitude, double longitude, std::string& error)
{
#ifdef _WIN32
    const char* cacheText = std::getenv("POSEIDON_EARTH_CACHE");
    const auto cache = cacheText && *cacheText ? std::filesystem::u8path(cacheText) : WorldStreamCache();
    const auto fixture = cache / "fixture";
    wchar_t exePath[32768]{};
    if (!GetModuleFileNameW(nullptr, exePath, 32768))
    {
        error = "Cannot locate the engine executable.";
        return false;
    }
    char lat[64], lon[64];
    std::snprintf(lat, sizeof(lat), "%.6f", latitude);
    std::snprintf(lon, sizeof(lon), "%.6f", longitude);
    std::vector<std::string> args = {Utf8(std::filesystem::path(exePath)), "--render=wgpu", "--wait-for-parent",
                                     std::to_string(GetCurrentProcessId()), "--test-mission",
                                     Utf8(fixture / "earth-preview.abel"), "--test-world",
                                     Utf8(fixture / "earth-preview.wrp"), "--test-world-hour", "12", "--vd", "20000",
                                     "--log-file", Utf8(cache / "earth-preview.log"), "--test-editor"};
    if (GEngine)
    {
        const WindowMode mode = GEngine->GetCurrentWindowMode();
        args.insert(args.end(), {"--display-mode", mode == WindowMode::Windowed ? "windowed" :
                                 mode == WindowMode::Borderless ? "borderless" : "exclusive",
                                 "--width", std::to_string(GEngine->Width()), "--height", std::to_string(GEngine->Height())});
    }
    if (Foundation::AppConfig::Instance().DevMode())
        args.push_back("--dev");
    std::vector<const char*> argv;
    for (const auto& arg : args)
        argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    // Everything else (providers, content, cache) carries over from this whole-world process.
    SDL_Environment* environment = SDL_CreateEnvironment(true);
    const SDL_PropertiesID properties = SDL_CreateProperties();
    bool configured = environment && properties &&
                      SDL_SetEnvironmentVariable(environment, "POSEIDON_EARTH_LATITUDE", lat, true) &&
                      SDL_SetEnvironmentVariable(environment, "POSEIDON_EARTH_LONGITUDE", lon, true);
    configured = configured && SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data()) &&
                 SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
    SDL_Process* process = configured ? SDL_CreateProcessWithProperties(properties) : nullptr;
    if (!process)
        error = std::string("Cannot restart whole-world mode: ") + SDL_GetError();
    if (properties)
        SDL_DestroyProperties(properties);
    if (environment)
        SDL_DestroyEnvironment(environment);
    if (!process)
        return false;
    SDL_DestroyProcess(process);
    Application::Instance().m_validateQuit = true;
    Application::Instance().m_closeRequest = true;
    return true;
#else
    (void)latitude, (void)longitude;
    error = "Whole-world restart is currently available on Windows.";
    return false;
#endif
}
} // namespace Poseidon
