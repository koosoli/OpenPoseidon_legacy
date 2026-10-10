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

constexpr int PlanId = 62120, PlannerMapId = 62130, PlannerListId = 62131, PlannerStatusId = 62132,
              ZoomInId = 62133, ZoomOutId = 62134, ClearId = 62135, DoneId = 62136, PlannerPanelId = 62137,
              PlannerHelpId = 62237;

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
        const bool multiLine = id == StatusId || id == PlannerStatusId || id == PlannerHelpId;
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
              id == PanelId || id == PlannerPanelId ? 0.94f : (button || edit ? 1.0f : 0.0f));
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

// ---- Mission planner: a satellite map of the area to place units on; they become the launched mission ----


struct UnitType
{
    const char* label;
    const char* className;
    const char* side; // mission.sqm side; EMPTY = an unmanned vehicle
};
constexpr UnitType UnitTypes[] = {
    {"BLUFOR rifleman", "SoldierWB", "WEST"},       {"BLUFOR machine gunner", "SoldierWMG", "WEST"},
    {"BLUFOR AT soldier", "SoldierWAT", "WEST"},    {"BLUFOR M1A1 tank", "M1Abrams", "WEST"},
    {"BLUFOR M113 APC", "M113", "WEST"},            {"BLUFOR UH-60 helicopter", "UH60", "WEST"},
    {"BLUFOR jeep", "Jeep", "WEST"},                {"OPFOR rifleman", "SoldierEB", "EAST"},
    {"OPFOR machine gunner", "SoldierEMG", "EAST"}, {"OPFOR AT soldier", "SoldierEAT", "EAST"},
    {"OPFOR T-72 tank", "T72", "EAST"},             {"OPFOR BMP", "BMP", "EAST"},
    {"OPFOR Mi-24 helicopter", "Mi24", "EAST"},     {"OPFOR UAZ", "UAZ", "EAST"},
    {"Resistance soldier", "SoldierGB", "GUER"},    {"Empty truck", "Truck5t", "EMPTY"},
    {"Empty jeep", "Jeep", "EMPTY"},
};

struct PlannedUnit
{
    double latitude, longitude;
    int type;
};

// The unit types the loaded data actually has (a demo install lacks some).
std::vector<int> AvailableUnitTypes()
{
    std::vector<int> result;
    const ParamEntry* vehicles = Pars.FindEntry("CfgVehicles");
    for (int i = 0; i < int(std::size(UnitTypes)); ++i)
        if (vehicles && vehicles->FindEntry(UnitTypes[i].className))
            result.push_back(i);
    return result;
}

class PlannerMapCtrl : public CStatic
{
  public:
    struct Marker
    {
        float u, v;
        PackedColor color;
        std::string label;
        bool player;
    };
    Ref<Texture> map;
    std::vector<Marker> markers;
    std::function<void(float, float)> onLeft, onRight;
    std::function<void(float)> onWheel;

    PlannerMapCtrl(ControlsContainer* parent, int idc, const ParamEntry& cls) : CStatic(parent, idc, cls)
    {
        EnableCtrl(true);
    }
    void OnLButtonDown(float x, float y) override
    {
        if (onLeft)
            onLeft((x - _x) / _w, (y - _y) / _h);
    }
    void OnRButtonDown(float x, float y) override
    {
        if (onRight)
            onRight((x - _x) / _w, (y - _y) / _h);
    }
    void OnMouseZChanged(float dz) override
    {
        if (onWheel)
            onWheel(dz);
    }
    void OnDraw(float alpha) override
    {
        const float w = GLOB_ENGINE->Width2D(), h = GLOB_ENGINE->Height2D();
        const Rect2DPixel rect(_x * w, _y * h, _w * w, _h * h);
        Draw2DPars pars;
        pars.mip = GLOB_ENGINE->TextBank()->UseMipmap(map, 0, 0);
        pars.SetColor(PackedColor(Color(map ? 1.0f : 0.1f, map ? 1.0f : 0.12f, map ? 1.0f : 0.15f, alpha)));
        pars.spec = NoZBuf | IsAlpha | IsAlphaFog;
        pars.SetU(0, 1);
        pars.SetV(0, 1);
        GLOB_ENGINE->Draw2D(pars, rect);
        // centre cross: the chosen place
        const PackedColor cross(Color(1, 1, 1, 0.6f * alpha));
        const float cx = rect.x + rect.w * 0.5f, cy = rect.y + rect.h * 0.5f;
        GLOB_ENGINE->DrawLine(Line2DPixel(cx - 8, cy, cx + 8, cy), cross, cross);
        GLOB_ENGINE->DrawLine(Line2DPixel(cx, cy - 8, cx, cy + 8), cross, cross);
        const MipInfo plain = GLOB_ENGINE->TextBank()->UseMipmap(nullptr, 0, 0);
        const Rect2DFloat clip(_x, _y, _w, _h);
        for (const auto& marker : markers)
        {
            if (marker.u < 0 || marker.u > 1 || marker.v < 0 || marker.v > 1)
                continue;
            const float px = rect.x + marker.u * rect.w, py = rect.y + marker.v * rect.h;
            const float r = marker.player ? 7.0f : 5.0f;
            if (marker.player)
                GLOB_ENGINE->Draw2D(plain, PackedColor(Color(1, 1, 1, alpha)), Rect2DPixel(px - r - 2, py - r - 2, 2 * r + 4, 2 * r + 4));
            GLOB_ENGINE->Draw2D(plain, PackedColor(Color(marker.color.R8() / 255.0f, marker.color.G8() / 255.0f, marker.color.B8() / 255.0f, alpha)), Rect2DPixel(px - r, py - r, 2 * r, 2 * r));
            GLOB_ENGINE->DrawText(Point2DFloat(_x + (marker.u * rect.w + r + 4) / w, _y + (marker.v * rect.h - r) / h),
                                  _size * 0.8f, clip, _font, PackedColor(Color(1, 1, 1, alpha)), marker.label.c_str());
        }
    }
};

class WholeWorldPlanner : public WholeWorldScreen
{
    std::filesystem::path _cache;
    double _latitude, _longitude;
    std::vector<PlannedUnit>& _plan;
    std::vector<int> _types;
    int _zoom = 1;
    static constexpr double HalfSizes[] = {1000, 2500, 5000, 10000}; // metres from centre to map edge
    double _south = 0, _west = 0, _north = 0, _east = 0;
    std::future<std::pair<bool, std::string>> _build;
    std::shared_ptr<std::atomic<bool>> _cancel = std::make_shared<std::atomic<bool>>(false);
    std::filesystem::path _pending;
    int _generation = 0;

    PlannerMapCtrl* Map() { return dynamic_cast<PlannerMapCtrl*>(GetCtrl(PlannerMapId)); }
    void Bounds()
    {
        const double half = HalfSizes[_zoom];
        const double dLat = half / Earth::Radius * 180.0 / Earth::Pi;
        const double dLon = dLat / std::max(0.05, std::cos(_latitude * Earth::Pi / 180.0));
        _south = _latitude - dLat;
        _north = _latitude + dLat;
        _west = _longitude - dLon;
        _east = _longitude + dLon;
    }
    void RequestMap()
    {
        Bounds();
        RefreshMarkers();
        if (auto* map = Map())
            map->map = nullptr;
        if (!EarthPlannerMap::Hook())
        {
            SetLabel(PlannerStatusId, "The map imagery needs the WGPU renderer (--render wgpu).");
            return;
        }
        if (_build.valid())
            return; // the running build finishes first; OnSimulate restarts for the new zoom
        _pending = _cache / "planner" / ("map-" + std::to_string(++_generation) + "-" +
                                         std::to_string(SDL_GetTicks()) + ".png");
        const auto builder = EarthPlannerMap::Hook();
        const auto cache = _cache, out = _pending;
        const double s = _south, w = _west, n = _north, e = _east;
        const auto cancel = _cancel;
        _build = std::async(std::launch::async, [=]
        {
            std::string message;
            const bool ok = builder(cache, s, w, n, e, 1024, out, message, [cancel] { return cancel->load(); });
            return std::make_pair(ok, message);
        });
        SetLabel(PlannerStatusId, "Loading the satellite map...");
    }
    void RefreshMarkers()
    {
        auto* map = Map();
        if (!map)
            return;
        map->markers.clear();
        for (size_t i = 0; i < _plan.size(); ++i)
        {
            const auto& unit = _plan[i];
            const UnitType& type = UnitTypes[unit.type];
            const std::string side = type.side;
            const Color color = side == "WEST" ? Color(0.2f, 0.45f, 1.0f, 1) :
                                side == "EAST" ? Color(1.0f, 0.25f, 0.2f, 1) :
                                side == "GUER" ? Color(0.2f, 0.85f, 0.3f, 1) : Color(0.95f, 0.85f, 0.2f, 1);
            map->markers.push_back({float((unit.longitude - _west) / (_east - _west)),
                                    float((_north - unit.latitude) / (_north - _south)), PackedColor(color),
                                    (i == 0 ? std::string("YOU: ") : std::string()) + type.label, i == 0});
        }
        std::string text = "Left click: place the selected unit.  Right click: remove the nearest.  Mouse wheel or "
                            "the buttons: zoom.  The first unit placed is you.  ";
        text += std::to_string(_plan.size()) + " unit(s) planned; map " + Number(2 * HalfSizes[_zoom] / 1000, 0) +
                " km across.";
        SetLabel(PlannerHelpId, text);
    }
    int SelectedType()
    {
        auto* list = dynamic_cast<CListBox*>(GetCtrl(PlannerListId));
        const int row = list ? list->GetCurSel() : -1;
        return row >= 0 && row < int(_types.size()) ? _types[row] : -1;
    }
    void Place(float u, float v)
    {
        const int type = SelectedType();
        if (type < 0 || u < 0 || u > 1 || v < 0 || v > 1)
            return;
        _plan.push_back({_north - v * (_north - _south), _west + u * (_east - _west), type});
        RefreshMarkers();
    }
    void Remove(float u, float v)
    {
        int best = -1;
        float bestDistance = 0.03f * 0.03f;
        for (size_t i = 0; i < _plan.size(); ++i)
        {
            const float du = float((_plan[i].longitude - _west) / (_east - _west)) - u;
            const float dv = float((_north - _plan[i].latitude) / (_north - _south)) - v;
            if (du * du + dv * dv < bestDistance)
            {
                bestDistance = du * du + dv * dv;
                best = int(i);
            }
        }
        if (best >= 0)
        {
            _plan.erase(_plan.begin() + best);
            RefreshMarkers();
        }
    }
    void Zoom(int step)
    {
        const int zoom = std::clamp(_zoom + step, 0, int(std::size(HalfSizes)) - 1);
        if (zoom == _zoom)
            return;
        _zoom = zoom;
        RequestMap();
    }

  public:
    WholeWorldPlanner(ControlsContainer* parent, std::filesystem::path cache, double latitude, double longitude,
                      std::vector<PlannedUnit>& plan)
        : WholeWorldScreen(parent), _cache(std::move(cache)), _latitude(latitude), _longitude(longitude), _plan(plan)
    {
        _idd = WholeWorldMenuId + 1;
        _types = AvailableUnitTypes();
        // A square map on screen: its width in screen units follows the display's aspect.
        const float mapH = 0.84f;
        const float aspect = GEngine && GEngine->Height() > 0 ? float(GEngine->Width()) / GEngine->Height() : 16.0f / 9.0f;
        const float mapW = std::min(0.62f, mapH / aspect * (4.0f / 3.0f));
        AddControl("RscText", "PlannerPanel", PlannerPanelId, "", 0.01f, 0.03f, 0.98f, 0.94f);
        AddControl("RscText", "PlannerMap", PlannerMapId, "", 0.02f, 0.08f, mapW, mapH);
        const float x = 0.04f + mapW, w = 0.97f - x;
        AddControl("RscText", "PlannerTitle", 62138, "PLAN MISSION", x, 0.05f, w, 0.05f);
        AddControl("RscListBox", "PlannerUnits", PlannerListId, "", x, 0.11f, w, 0.40f);
        AddControl("RscButton", "ZoomIn", ZoomInId, "Zoom in", x, 0.53f, w * 0.48f, 0.05f);
        AddControl("RscButton", "ZoomOut", ZoomOutId, "Zoom out", x + w * 0.52f, 0.53f, w * 0.48f, 0.05f);
        AddControl("RscButton", "Clear", ClearId, "Remove all units", x, 0.59f, w, 0.05f);
        AddControl("RscText", "PlannerHelp", PlannerHelpId, "", x, 0.65f, w, 0.13f);
        AddControl("RscText", "PlannerStatus", PlannerStatusId, "", x, 0.79f, w, 0.06f);
        AddControl("RscButton", "Done", DoneId, "Done", x, 0.87f, w, 0.05f);
        for (int id : {PlannerHelpId, PlannerStatusId})
            if (auto* text = dynamic_cast<CStatic*>(GetCtrl(id)))
                text->EnableCtrl(false);
        if (auto* list = dynamic_cast<CListBox*>(GetCtrl(PlannerListId)))
        {
            for (int type : _types)
                list->AddString(UnitTypes[type].label);
            list->SetCurSel(0);
        }
        if (auto* map = Map())
        {
            map->onLeft = [this](float u, float v) { Place(u, v); };
            map->onRight = [this](float u, float v) { Remove(u, v); };
            map->onWheel = [this](float dz) { Zoom(dz > 0 ? -1 : 1); };
        }
        RequestMap();
    }
    ~WholeWorldPlanner() override
    {
        _cancel->store(true);
        if (_build.valid())
            _build.wait();
    }
    Control* OnCreateCtrl(int type, int idc, const ParamEntry& cls) override
    {
        if (idc == PlannerMapId)
            return new PlannerMapCtrl(this, idc, cls);
        return WholeWorldScreen::OnCreateCtrl(type, idc, cls);
    }
    void OnButtonClicked(int idc) override
    {
        if (idc == DoneId)
            Exit(1);
        else if (idc == ZoomInId)
            Zoom(-1);
        else if (idc == ZoomOutId)
            Zoom(1);
        else if (idc == ClearId)
        {
            _plan.clear();
            RefreshMarkers();
        }
        else
            WholeWorldScreen::OnButtonClicked(idc);
    }
    void OnSimulate(EntityAI* vehicle) override
    {
        if (_build.valid() && _build.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            const auto [ok, message] = _build.get();
            const auto built = _pending;
            Bounds();
            const bool current = built.filename().string().rfind("map-" + std::to_string(_generation) + "-", 0) == 0;
            if (!current)
                RequestMap(); // the zoom changed while it loaded
            else if (ok)
            {
                if (auto* map = Map())
                    map->map = GlobLoadTexture(RStringB(Utf8(built).c_str()));
                SetLabel(PlannerStatusId, message);
            }
            else
                SetLabel(PlannerStatusId, message);
        }
        WholeWorldScreen::OnSimulate(vehicle);
    }
};

class WholeWorldDisplay : public WholeWorldScreen
{
    EarthProviders::Config _config;
    int _imagery = 0, _elevation = 0, _selected = -1;
    bool _onFoot = false; // start as the soldier on the ground instead of the free-flight camera
    bool _dayCycle = false; // test aid: one day per minute
    std::vector<PlannedUnit> _plan; // from the mission planner; empty = the lone default soldier
    double _planLatitude = 0, _planLongitude = 0;

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
    void OpenPlanner()
    {
        double lat = 0, lon = 0;
        if (!ParsePlace(lat, lon))
        {
            Status("Enter a valid latitude and longitude first.");
            return;
        }
        // A plan belongs to its place: choosing another place starts an empty one.
        if (!_plan.empty() && (std::abs(_planLatitude - lat) > 1e-4 || std::abs(_planLongitude - lon) > 1e-4))
            _plan.clear();
        _planLatitude = lat;
        _planLongitude = lon;
        CreateChild(new WholeWorldPlanner(this, WorldStreamCache(), lat, lon, _plan));
    }
    // The planned units as mission.sqm groups (each unit its own group; the first is the player), and the
    // empty vehicles. Positions in the local frame the Earth origin anchors at map [6400,6400].
    void WritePlan(std::ofstream& sqm, double lat, double lon)
    {
        std::vector<std::string> groups, empties;
        int id = 0;
        for (size_t i = 0; i < _plan.size(); ++i)
        {
            const auto& unit = _plan[i];
            const UnitType& type = UnitTypes[unit.type];
            const auto local = Earth::ToLocal(lat, lon, unit.latitude, unit.longitude);
            std::string item = "position[]={" + Number(local.x, 2) + ",0," + Number(local.z, 2) + "};id=" +
                               std::to_string(id++) + ";side=\"" + type.side + "\";vehicle=\"" + type.className +
                               "\";skill=0.6;";
            if (std::string(type.side) == "EMPTY")
            {
                empties.push_back(item);
                continue;
            }
            if (i == 0)
                item += "player=\"PLAYER COMMANDER\";init=\"this allowDamage false;0 setOvercast 0.3\";";
            groups.push_back(std::string("side=\"") + type.side + "\";class Vehicles {items=1;class Item0 {" + item +
                             "leader=1;};};");
        }
        sqm << " class Groups {items=" << groups.size() << ";\n";
        for (size_t i = 0; i < groups.size(); ++i)
            sqm << "  class Item" << i << " {" << groups[i] << "};\n";
        sqm << " };\n";
        if (!empties.empty())
        {
            sqm << " class Vehicles {items=" << empties.size() << ";\n";
            for (size_t i = 0; i < empties.size(); ++i)
                sqm << "  class Item" << i << " {" << empties[i] << "};\n";
            sqm << " };\n";
        }
    }
    bool PlanHasPlayer() const
    {
        return !_plan.empty() && std::string(UnitTypes[_plan.front().type].side) != "EMPTY";
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
            sqm << "version=11;\nclass Mission {\n randomSeed=42;\n"
                   " class Intel {weather=0.3;weatherForecast=0.3;fog=0;fogForecast=0;year=1985;month=6;day=21;hour=12;minute=0;};\n";
            if (PlanHasPlayer())
                WritePlan(sqm, lat, lon);
            else
                sqm << " class Groups {items=1;class Item0 {side=\"WEST\";class Vehicles {items=1;class Item0 {\n"
                       "  position[]={6400,0,6400};id=0;side=\"WEST\";vehicle=\"SoldierWB\";\n"
                       "  player=\"PLAYER COMMANDER\";leader=1;skill=1;"
                       "init=\"this allowDamage false;0 setRain 0;0 setOvercast 0.3;0 setFog 0\";\n"
                       " };};};};\n";
            sqm << "};\n"
                   "class Intro {randomSeed=1;};class OutroWin {randomSeed=1;};class OutroLoose {randomSeed=1;};\n";
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
        if (!_onFoot)
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
        AddControl("RscButton", "Plan", PlanId, "Plan mission (map)", 0.40f, 0.82f, 0.32f, 0.05f);
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
} // namespace Poseidon
