#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <unordered_map>

#include <Poseidon/Foundation/platform.hpp>

// Same collision the rest of Dev/Debug hits: the PCH's Logging.hpp #defines
// DebugLog() as a macro, which mangles ImGui::DebugLog().
#ifdef DebugLog
#undef DebugLog
#endif

#include <imgui.h>

#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/Dev/Diag/TerrainBrush.hpp>

#include <Poseidon/Dev/Diag/BallisticsRecorder.hpp>
#include <Poseidon/Dev/Diag/FixedStepStats.hpp>
#include <Poseidon/Dev/Diag/PhysicsProbe.hpp>
#include <Poseidon/Dev/Diag/PhysicsRayAudit.hpp>
#include <Poseidon/Dev/Diag/PictureModeTab.hpp>
#include <Poseidon/Dev/Diag/LightingDiag.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/Entities/Weapons/Ballistics.hpp>
#include <Poseidon/World/Entities/Weapons/GrenadeFuse.hpp>
#include <Poseidon/World/Physics/LooseObjects.hpp>
#include <Poseidon/World/Entities/Weapons/Penetration.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/World.hpp>

#include <map>

namespace Poseidon::Dev
{
namespace
{
ImGuiTextFilter g_filter;

// The control the next PanelHelp() belongs to. Reset every frame by
// DrawPanelSearch(), which is why that has to be called before the tab bar.
ImGuiID g_lastId = 0;
bool g_lastShown = true;

// Explanation text keyed on the control's ImGui ID, captured on a frame where
// the control drew. This is what lets the search reach the help paragraphs
// without restructuring 278 call sites to pass their text up front.
//
// The ID is stable across frames for a given control (it is the label hashed
// against the window and ID stack), so this is a cache, not a registry: an
// entry that never appears again is simply never consulted. It exists only in
// the dev panel and only for as long as the process does.
std::unordered_map<ImGuiID, std::string>& HelpCache()
{
    static std::unordered_map<ImGuiID, std::string> cache;
    return cache;
}

// Counters for the "N of M" readout. Reported one frame late, like the cache.
int g_shown = 0;
int g_total = 0;
int g_shownLast = 0;
int g_totalLast = 0;

/// ASCII case-insensitive compare. ImGui's own ImStricmp is internal, and this
/// is only ever fed tab names typed into an environment variable.
bool EqualsNoCase(const char* a, const char* b)
{
    for (; *a != 0 && *b != 0; ++a, ++b)
    {
        const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a - 'A' + 'a') : *a;
        const char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b - 'A' + 'a') : *b;
        if (ca != cb)
            return false;
    }
    return *a == 0 && *b == 0;
}

/// Add text to what the search will match for the control above.
///
/// Appended rather than replaced, because a control can have both an
/// explanation and a tooltip and both are worth finding it by. Guarded against
/// re-appending the same text, or a control redrawn every frame would grow its
/// entry without bound.
void RememberForSearch(const char* text)
{
    if (g_lastId == 0 || text == nullptr || *text == 0)
        return;
    std::string& stored = HelpCache()[g_lastId];
    if (stored.find(text) != std::string::npos)
        return;
    if (!stored.empty())
        stored += ' ';
    stored += text;
}

/// Does this control survive the filter? Its label first, then whatever
/// explanation was recorded under it on an earlier frame.
bool Match(const char* label)
{
    ++g_total;
    g_lastId = ImGui::GetID(label);
    if (!g_filter.IsActive())
    {
        ++g_shown;
        return true;
    }
    if (g_filter.PassFilter(label))
    {
        ++g_shown;
        return true;
    }
    const auto& cache = HelpCache();
    const auto it = cache.find(g_lastId);
    if (it != cache.end() && g_filter.PassFilter(it->second.c_str()))
    {
        ++g_shown;
        return true;
    }
    return false;
}
} // namespace

bool PanelFilterActive()
{
    return g_filter.IsActive();
}

bool PanelSearchPreseeded()
{
    // POSEIDON_PANEL_SEARCH=<terms> starts the panel with that filter already
    // typed. It exists so a filtered panel can be SCREENSHOT -- an auto-capture
    // has no hands, and "the search works" is otherwise only checkable by a
    // person sitting at the keyboard. DebugOverlay opens the panel when it is
    // set, for the same reason.
    static const bool seeded = []
    {
        // POSEIDON_PANEL_OPEN=1 opens it with no filter, which is the "before"
        // half of any before/after capture.
        const char* open = std::getenv("POSEIDON_PANEL_OPEN");
        const bool wantOpen = open != nullptr && *open != 0 && *open != '0';
        const char* v = std::getenv("POSEIDON_PANEL_SEARCH");
        if (v == nullptr || *v == 0)
            return wantOpen;
        snprintf(g_filter.InputBuf, sizeof(g_filter.InputBuf), "%s", v);
        g_filter.Build();
        return true;
    }();
    return seeded;
}

void DrawPanelSearch()
{
    PanelSearchPreseeded();
    g_shownLast = g_shown;
    g_totalLast = g_total;
    g_shown = 0;
    g_total = 0;
    g_lastId = 0;
    g_lastShown = true;

    // ImGuiTextFilter::Draw() puts its label to the RIGHT of the box, which at
    // this window's default width pushes the hint off the edge. Drive the same
    // buffer through InputTextWithHint instead: the prompt lives inside the box,
    // and the status gets its own line rather than competing for the row.
    ImGui::SetNextItemWidth(-60.0f);
    if (ImGui::InputTextWithHint("##panelsearch", "Search settings in this tab...", g_filter.InputBuf,
                                 sizeof(g_filter.InputBuf)))
    {
        g_filter.Build();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
    {
        g_filter.Clear();
    }
    if (g_filter.IsActive())
    {
        if (g_shownLast == 0 && g_totalLast > 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "no match in this tab (%d controls)", g_totalLast);
        }
        else
        {
            ImGui::TextDisabled("showing %d of %d controls in this tab", g_shownLast, g_totalLast);
        }
    }
    else
    {
        ImGui::TextDisabled("space separates terms, -word excludes; searches labels and their explanations");
    }
    ImGui::Separator();
}

void ResetFogSettings()
{
    if (GEngine && GEngine->SupportsSky())
    {
        auto settings = GEngine->GetSkySettings();
        const Engine::SkySettings defaults;
        settings.layeredFog = defaults.layeredFog;
        settings.fogFarClose = defaults.fogFarClose;
        settings.fogFalloff = defaults.fogFalloff;
        for (int i = 0; i < 2; ++i)
        {
            settings.fogLayerBase[i] = defaults.fogLayerBase[i];
            settings.fogLayerTop[i] = defaults.fogLayerTop[i];
            settings.fogLayerFeather[i] = defaults.fogLayerFeather[i];
            settings.fogLayerExtinction[i] = defaults.fogLayerExtinction[i];
        }
        settings.fogLayerAlbedo = defaults.fogLayerAlbedo;
        settings.fogLayerG = defaults.fogLayerG;
        settings.fogTerrainFollow = defaults.fogTerrainFollow;
        settings.fogTerrainReference = defaults.fogTerrainReference;
        settings.fogValleyStrength = defaults.fogValleyStrength;
        settings.fogValleyRadius = defaults.fogValleyRadius;
        settings.fogPatchStrength = defaults.fogPatchStrength;
        settings.fogPatchHorizontalScale = defaults.fogPatchHorizontalScale;
        settings.fogPatchVerticalScale = defaults.fogPatchVerticalScale;
        settings.fogCoverageFeather = defaults.fogCoverageFeather;
        GEngine->SetSkySettings(settings);
    }
    if (GWorld && GLandscape)
        GWorld->SetWeather(GWorld->GetOvercast(), 0.0f, 0.0f);
}

void ResetPanelSettings()
{
    // Explicit list. A tab that wants resetting says so here; anything not named
    // is simply left alone, which is a visible gap rather than a silent one.
    // Physics probes and their world
    ResetProbeSettings();
    ResetTerrainBrush(); // Settings only: never erase painted terrain from a global UI reset.
    ResetRocketCraters();
    GSnow().ResetSettings();
    // Picture Mode
    ResetPictureModeSettings();
    ResetLightingSettings();
    // Penetration
    Penetration::SetEnabled(false);
    Penetration::SetResistanceScale(1.0f);
    GrenadeFuse::Reset();
    LooseObjects::Reset();
    // Ray audit
    SetRayAudit(false);
    ResetRayAudit();
    // Fixed step
    ResetFixedStepStats();
    SetFixedStepScripts(true);
    // Sky look, the water look, and the road/decal ground conform. Written out here rather
    // than called through
    // DebugOverlay: that translation unit is not linked into every app that draws these widgets
    // (Tetris is not), and a reset that only exists in some builds is worse than none.
    // Both are settings structs whose own initialisers ARE the defaults, so this cannot drift
    // from the tabs that edit them.
    if (GEngine != nullptr)
    {
        if (GEngine->SupportsSky())
        {
            GEngine->SetSkySettings(Engine::SkySettings{});
        }
        GEngine->SetRoadSettings(Engine::RoadSettings{});
        if (GEngine->SupportsWater())
        {
            GEngine->SetWaterSettings(Engine::WaterSettings{});
        }
    }
    ResetFogSettings();
    // Ballistics recorder
    Ballistics::SetEnabled(false);
    Ballistics::SetDrawTrails(true);
    Ballistics::ClearAll();
    Ballistics::SetSelectedTrack(0);
    Poseidon::Ballistics::SetDragCorrectAim(true);
    Poseidon::Ballistics::SetForcedDragModel(Poseidon::Ballistics::DragModel::Constant, 0.0f);
}

bool SliderFloat(const char* label, float* v, float min, float max, const char* format, ImGuiSliderFlags flags)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::SliderFloat(label, v, min, max, format, flags);
}

bool SliderInt(const char* label, int* v, int min, int max, const char* format, ImGuiSliderFlags flags)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::SliderInt(label, v, min, max, format, flags);
}

bool Checkbox(const char* label, bool* v)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::Checkbox(label, v);
}

bool Combo(const char* label, int* currentItem, const char* const items[], int itemsCount, int popupMaxHeightInItems)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::Combo(label, currentItem, items, itemsCount, popupMaxHeightInItems);
}

bool Combo(const char* label, int* currentItem, const char* itemsSeparatedByZeros, int popupMaxHeightInItems)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::Combo(label, currentItem, itemsSeparatedByZeros, popupMaxHeightInItems);
}

bool ColorEdit3(const char* label, float col[3], ImGuiColorEditFlags flags)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::ColorEdit3(label, col, flags);
}

bool Button(const char* label)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::Button(label);
}

bool Button(const char* label, const ImVec2& size)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::Button(label, size);
}

bool SmallButton(const char* label)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::SmallButton(label);
}

bool RadioButton(const char* label, bool active)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::RadioButton(label, active);
}

bool InputText(const char* label, char* buf, size_t bufSize, int flags)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::InputText(label, buf, bufSize, flags);
}

bool InputTextWithHint(const char* label, const char* hint, char* buf, size_t bufSize, int flags)
{
    // The HINT is the only thing a "##hidden label" box shows, so it has to be
    // searchable too -- otherwise the pose box is unfindable by any word on it.
    g_lastShown = Match(label) || (g_filter.IsActive() && hint != nullptr && g_filter.PassFilter(hint));
    return g_lastShown && ImGui::InputTextWithHint(label, hint, buf, bufSize, flags);
}

bool InputInt(const char* label, int* v, int step, int stepFast, int flags)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::InputInt(label, v, step, stepFast, flags);
}

void PanelSameLine()
{
    if (g_lastShown)
        ImGui::SameLine();
}

void PanelHeading(const char* text)
{
    // Not counted in the "N of M controls" tally: a heading is not a setting,
    // and counting it would make the number lie about how much you found.
    if (!g_filter.IsActive())
        ImGui::TextUnformatted(text);
}

bool PanelTabItem(const char* label, bool* open, int flags)
{
    // One shot: after the first frame the user's clicks own the selection, so
    // this must not keep dragging them back to the requested tab. Any flags the
    // caller already passes (the Shadows/Memory jump-to one-shots) are kept.
    static const char* const wanted = std::getenv("POSEIDON_PANEL_TAB");
    static bool applied = false;
    if (!applied && wanted != nullptr && *wanted != 0 && EqualsNoCase(label, wanted))
    {
        flags |= ImGuiTabItemFlags_SetSelected;
        applied = true;
    }
    return ImGui::BeginTabItem(label, open, flags);
}

bool BeginCombo(const char* label, const char* previewValue, int flags)
{
    g_lastShown = Match(label);
    return g_lastShown && ImGui::BeginCombo(label, previewValue, flags);
}

void PanelHelp(const char* fmt, ...)
{
    char text[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    // Remember it for the search even on the frames it is not drawn, so a
    // control whose help is the only match still becomes findable.
    RememberForSearch(text);

    if (g_lastShown)
        ImGui::TextDisabled("%s", text);
}

void PanelTooltip(const char* fmt, ...)
{
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    RememberForSearch(text);

    if (g_lastShown)
        ImGui::SetItemTooltip("%s", text);
}

bool PanelItemHovered(int flags)
{
    return g_lastShown && ImGui::IsItemHovered(flags);
}

void PanelSeparator()
{
    if (!g_filter.IsActive())
        ImGui::Separator();
}
} // namespace Poseidon::Dev
