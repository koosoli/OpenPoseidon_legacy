// The dev-panel "Weather" tab — an instrument for the one wind authority
// (World/Weather/WindModel).
//
// The reason this exists: WindModel is a CLOSED FORM in mission time. That is
// exactly the property that makes it deterministic and replication-free, and it
// is also what makes it untestable by hand — you cannot ask the shipping model
// for "9 m/s due east" and watch what answers, because the model's whole job is
// to decide that for itself. The override this tab drives is the escape hatch:
// local, unreplicated, not serialized, and off unless a developer turns it on.
//
// Everything above the override is a pure readout. Nothing in this file caches
// wind state; the sample is re-read every frame from the authority, so what the
// tab shows is what the grass, the sea and the smoke are being handed.

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/platform.hpp>

// The PCH pulls in Logging.hpp, which #defines DebugLog() as a macro and
// collides with the ImGui::DebugLog() method. Same dance as DebugOverlay.cpp.
#ifdef DebugLog
#undef DebugLog
#endif

#include <imgui.h>

#include <Poseidon/Dev/Diag/WeatherTab.hpp>
#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Weather/RainVolume.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/World.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace Poseidon::Dev
{

namespace
{

constexpr float kPi = 3.14159265358979f;

constexpr int kRainBenchmarkFrames = 120;
struct RainBenchmarkHistory
{
    std::array<float, kRainBenchmarkFrames> total{};
    std::array<float, kRainBenchmarkFrames> spawn{};
    std::array<float, kRainBenchmarkFrames> occlusion{};
    std::array<float, kRainBenchmarkFrames> simulate{};
    std::array<float, kRainBenchmarkFrames> draw{};
    int cursor = 0;
    int count = 0;
};
RainBenchmarkHistory GRainBenchmark;

void PushRainBenchmark(const RainStats& stats)
{
    const int i = GRainBenchmark.cursor;
    GRainBenchmark.total[i] = static_cast<float>(stats.lastUpdateUs);
    GRainBenchmark.spawn[i] = static_cast<float>(stats.spawnMicroseconds);
    GRainBenchmark.occlusion[i] = static_cast<float>(stats.occlusionMicroseconds);
    GRainBenchmark.simulate[i] = static_cast<float>(stats.simulateMicroseconds);
    GRainBenchmark.draw[i] = static_cast<float>(stats.drawMicroseconds);
    GRainBenchmark.cursor = (i + 1) % kRainBenchmarkFrames;
    GRainBenchmark.count = std::min(GRainBenchmark.count + 1, kRainBenchmarkFrames);
}

float RainAverage(const std::array<float, kRainBenchmarkFrames>& values)
{
    if (GRainBenchmark.count == 0)
    {
        return 0.0f;
    }
    float sum = 0.0f;
    for (int i = 0; i < GRainBenchmark.count; ++i)
    {
        sum += values[i];
    }
    return sum / static_cast<float>(GRainBenchmark.count);
}

void SetRainBenchmarkPreset(int drops)
{
    RainParams params = GRain.Params();
    params.targetDrops = drops;
    params.maxDrops = drops;
    params.densityOverride = 1.0f;
    params.splashes = false;
    params.occlusion = RainOcclusionRooms;
    GRain.SetParams(params);
    GRain.SetMode(RainParticle);
    GRain.ClearParticles();
    GRain.ResetStats();
    GRainBenchmark = RainBenchmarkHistory{};
}

/// Compass bearing (degrees, 0 = north/+Z, 90 = east/+X, clockwise) from the
/// model's `directionRad` (atan2(z, x), the heading the air travels TOWARD).
/// Both describe the same vector; the model's form is convenient for trig and
/// the compass form is the one a human can check against the in-game map.
float BearingFromRad(float directionRad)
{
    float bearing = 90.0f - directionRad * (180.0f / kPi);
    while (bearing < 0.0f)
    {
        bearing += 360.0f;
    }
    while (bearing >= 360.0f)
    {
        bearing -= 360.0f;
    }
    return bearing;
}

float RadFromBearing(float bearingDegrees)
{
    return (90.0f - bearingDegrees) * (kPi / 180.0f);
}

/// The nearest of the sixteen compass points. Purely so the readout says "WSW"
/// next to the number — reading a bearing off a heading you are flying is much
/// easier than reading it off three digits.
const char* CompassPoint(float bearingDegrees)
{
    static const char* kPoints[] = {"N",  "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                    "S",  "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    const int index = static_cast<int>(std::floor(bearingDegrees / 22.5f + 0.5f)) & 15;
    return kPoints[index];
}

/// Beaufort description of a speed in m/s. The point is not meteorological
/// precision — it is that "12 m/s" means nothing to most people and "strong
/// breeze, whitecaps everywhere" tells you whether the sea outside is right.
const char* BeaufortLabel(float speedMs)
{
    if (speedMs < 0.3f)
        return "calm";
    if (speedMs < 1.6f)
        return "light air";
    if (speedMs < 3.4f)
        return "light breeze";
    if (speedMs < 5.5f)
        return "gentle breeze";
    if (speedMs < 8.0f)
        return "moderate breeze";
    if (speedMs < 10.8f)
        return "fresh breeze";
    if (speedMs < 13.9f)
        return "strong breeze";
    if (speedMs < 17.2f)
        return "near gale";
    if (speedMs < 20.8f)
        return "gale";
    if (speedMs < 24.5f)
        return "strong gale";
    return "storm";
}

/// A compass rose with an arrow along the *instantaneous* heading and a faint
/// second arrow along the mean. Seeing the two separate is the quickest read on
/// whether the gust term is doing anything, which is otherwise a number that
/// wobbles in the third decimal.
void DrawCompass(const WindSample& sample)
{
    const float diameter = 108.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 centre(origin.x + diameter * 0.5f, origin.y + diameter * 0.5f);
    const float radius = diameter * 0.5f - 10.0f;

    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImU32 ring = IM_COL32(140, 140, 150, 180);
    const ImU32 faint = IM_COL32(120, 160, 220, 130);
    const ImU32 strong = IM_COL32(120, 200, 255, 255);
    const ImU32 label = IM_COL32(190, 190, 195, 220);

    draw->AddCircle(centre, radius, ring, 48, 1.5f);
    for (int i = 0; i < 4; ++i)
    {
        const float angle = static_cast<float>(i) * (kPi * 0.5f);
        // Screen Y grows downward, so a north bearing must point to -Y.
        const ImVec2 outer(centre.x + std::sin(angle) * radius, centre.y - std::cos(angle) * radius);
        const ImVec2 inner(centre.x + std::sin(angle) * (radius - 5.0f), centre.y - std::cos(angle) * (radius - 5.0f));
        draw->AddLine(inner, outer, ring, 1.0f);
    }
    draw->AddText(ImVec2(centre.x - 4.0f, centre.y - radius - 14.0f), label, "N");

    // Arrow helper: from the centre outward along a compass bearing.
    const auto arrow = [&](float bearingDegrees, float length, ImU32 colour, float thickness) {
        const float a = bearingDegrees * (kPi / 180.0f);
        const ImVec2 tip(centre.x + std::sin(a) * length, centre.y - std::cos(a) * length);
        draw->AddLine(centre, tip, colour, thickness);
        // Two short barbs, 150 degrees either side of the shaft.
        const float barb = 8.0f;
        for (int side = -1; side <= 1; side += 2)
        {
            const float b = a + static_cast<float>(side) * (150.0f * kPi / 180.0f);
            draw->AddLine(tip, ImVec2(tip.x + std::sin(b) * barb, tip.y - std::cos(b) * barb), colour, thickness);
        }
    };

    // Scale the shaft with speed so the picture also reads as a magnitude, but
    // never let it vanish at 0 m/s or overrun the ring in a storm.
    const float meanBearing = BearingFromRad(sample.meanDirectionRad);
    const float bearing = BearingFromRad(sample.directionRad);
    const float scale = std::clamp(sample.speed / 20.0f, 0.15f, 1.0f);

    arrow(meanBearing, radius * 0.75f, faint, 1.5f);
    arrow(bearing, radius * scale, strong, 2.5f);

    ImGui::Dummy(ImVec2(diameter, diameter));
}

void DrawReadout()
{
    ImGui::SeparatorText("Live wind");

    if (!GWind.IsActive())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Wind authority inactive");
        ImGui::TextDisabled("WindModel::Update has not run. No world is loaded, or this");
        ImGui::TextDisabled("Landscape is being driven outside a World - in which case");
        ImGui::TextDisabled("Landscape::GetWind falls back to the legacy random walk.");
        return;
    }

    const WindSample& sample = GWind.Sample();
    const float bearing = BearingFromRad(sample.directionRad);
    const float meanBearing = BearingFromRad(sample.meanDirectionRad);

    ImGui::BeginGroup();
    DrawCompass(sample);
    ImGui::EndGroup();
    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::Text("Speed      %6.2f m/s  (%s)", sample.speed, BeaufortLabel(sample.speed));
    ImGui::Text("Mean       %6.2f m/s", sample.meanSpeed);
    ImGui::Text("Gust       %+6.0f %%", sample.gustFraction * 100.0f);
    ImGui::Spacing();
    ImGui::Text("Toward     %6.1f deg  %-3s", bearing, CompassPoint(bearing));
    ImGui::Text("Mean tow.  %6.1f deg  %-3s", meanBearing, CompassPoint(meanBearing));
    ImGui::TextDisabled("velocity  (%+.2f, %+.2f) m/s in world X/Z", sample.velocityX, sample.velocityZ);
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::TextDisabled("Overcast %.2f   -   the mean speed and the gust envelope both", GWind.Overcast());
    ImGui::TextDisabled("scale with it, which is what makes a front feel like one.");
}

void DrawOverrideSection()
{
    ImGui::SeparatorText("Override");

    WindOverride current = GWind.Override();
    bool changed = false;

    bool enabled = current.enabled;
    if (ImGui::Checkbox("Override the wind authority", &enabled))
    {
        current.enabled = enabled;
        changed = true;

        // Turning it ON seeds from whatever the mission wind is doing right now,
        // so the first frame after the click looks identical. Without this the
        // world snaps to the default 6 m/s due east and you cannot tell your
        // change apart from the discontinuity.
        if (enabled && GWind.IsActive())
        {
            const WindSample& sample = GWind.Sample();
            current.speed = sample.meanSpeed;
            current.directionRad = sample.meanDirectionRad;
        }
    }

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("LOCAL AND UNREPLICATED. Not serialized, not sent to the server.\n"
                          "In a network game this machine will disagree with everyone\n"
                          "else about the wind. Fine for looking at smoke; never leave\n"
                          "it on for anything that matters.");
    }

    ImGui::BeginDisabled(!current.enabled);

    float speed = current.speed;
    if (ImGui::SliderFloat("Speed (m/s)", &speed, 0.0f, 30.0f, "%.1f"))
    {
        current.speed = speed;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", BeaufortLabel(speed));

    float bearing = BearingFromRad(current.directionRad);
    if (ImGui::SliderFloat("Toward (deg)", &bearing, 0.0f, 360.0f, "%.0f"))
    {
        current.directionRad = RadFromBearing(bearing);
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", CompassPoint(bearing));

    float gustiness = current.gustiness;
    if (ImGui::SliderFloat("Gustiness", &gustiness, 0.0f, 1.0f, "%.2f"))
    {
        current.gustiness = gustiness;
        changed = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("0 keeps the field dead steady, which is what you want when\n"
                          "comparing two smoke systems: a gust that lands differently\n"
                          "in the two runs is not a difference between the systems.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Presets");
    struct Preset
    {
        const char* name;
        float speed;
        float gustiness;
    };
    static const Preset kPresets[] = {
        {"Calm", 0.5f, 0.0f}, {"Breeze", 5.0f, 0.2f}, {"Windy", 12.0f, 0.4f}, {"Gale", 20.0f, 0.7f}};
    for (const Preset& preset : kPresets)
    {
        if (ImGui::Button(preset.name))
        {
            current.speed = preset.speed;
            current.gustiness = preset.gustiness;
            changed = true;
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();

    ImGui::EndDisabled();

    if (changed)
    {
        GWind.SetOverride(current);
    }
}

void DrawConsumerSection()
{
    ImGui::SeparatorText("Consumers");

    ImGui::TextDisabled("Everything below reads the sample above. Nothing invents its");
    ImGui::TextDisabled("own air; that was the point of centralising the model.");
    ImGui::Spacing();

    ImGui::BulletText("Grass sway        renderer, live");
    ImGui::BulletText("Cloud deck drift  renderer, live");
    ImGui::BulletText("Ocean spectrum    renderer, live (driven by MEAN, not gust)");
    ImGui::BulletText("Buoyancy predict  simulation, live");

    bool ballistics = WindModel::BallisticsEnabled();
    if (ImGui::Checkbox("Ballistics (bullet drift)", &ballistics))
    {
        WindModel::SetBallisticsEnabled(ballistics);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("OFF BY DEFAULT AND DELIBERATELY SO. Classic OFP missions were\n"
                          "authored and tested windless; a 9 m/s crosswind changes how a\n"
                          "sniper mission plays. With this off ShotShell::Simulate computes\n"
                          "the byte-identical expression it always did.\n"
                          "Also settable with POSEIDON_WIND_BALLISTICS=1.");
    }
    if (ballistics)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "gameplay-affecting");
    }
}

/// The particle-rain panel. Same philosophy as the wind override above: every
/// control here is local, unreplicated and dev-only. Particle rain is the
/// shipping default; the legacy overlay stays one radio away for comparison.
void DrawRainSection()
{
    ImGui::SeparatorText("Rain");

    // Master switch. One checkbox that produces REAL rain -- forcing the
    // weather overcast up (rain density follows overcast) and switching the
    // renderer to particle rain -- instead of asking anyone to reconcile a
    // weather slider in another section with the mode radio below. Turning it
    // off restores exactly what was there before.
    static bool rainForced = false;
    static float savedOvercast = -1.0f;
    static RainMode savedMode = RainLegacy;
    const bool wasOn = rainForced;
    if (ImGui::Checkbox("Rain ON (forces overcast)", &rainForced))
    {
        if (rainForced && !wasOn)
        {
            savedOvercast = GWorld != nullptr ? GWorld->GetOvercast() : -1.0f;
            savedMode = GRain.Mode();
            GRain.SetMode(RainParticle);
            if (GWorld != nullptr)
            {
                GWorld->SetWeather(0.95f, GWorld->GetFog(), 5.0f);
            }
            // Overcast ALONE is not rain. Landscape's own rain density is a slow
            // random walk bounded by overcast * 1.5 - 1 -- at 0.95 overcast that
            // is a ceiling of 0.425, approached at about 0.01 per second toward a
            // RANDOM target that is often near zero. Every consumer that is not
            // the two render layers reads that number: the sea's rain ripples,
            // visibility, wet surfaces. So a switch that produced a visible
            // downpour also produced an ocean being rained on at a tenth
            // strength, or not at all. Force the density too.
            GRain.SetForceWeatherRain(true);
        }
        else if (!rainForced && wasOn)
        {
            GRain.SetMode(savedMode);
            GRain.ClearParticles();
            GRain.SetForceWeatherRain(false);
            if (GWorld != nullptr && savedOvercast >= 0.0f)
            {
                GWorld->SetWeather(savedOvercast, GWorld->GetFog(), 5.0f);
            }
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ON: overcast 0.95 (transition 5 s), particle rain, and the\n"
                          "weather rain density held at full -- which is what the sea's\n"
                          "ripples and every other non-render consumer actually read.\n"
                          "OFF: restores the overcast and rain mode you had.");
    }

    struct ModeEntry
    {
        const char* label;
        RainMode mode;
    };
    static const ModeEntry kModes[] = {
        {"Off", RainOff}, {"Legacy overlay", RainLegacy}, {"Particle", RainParticle}, {"Both", RainBoth}};

    const RainMode mode = GRain.Mode();
    for (const ModeEntry& entry : kModes)
    {
        if (ImGui::RadioButton(entry.label, mode == entry.mode))
        {
            GRain.SetMode(entry.mode);
        }
        if (ImGui::IsItemHovered())
        {
            switch (entry.mode)
            {
                case RainOff:
                    ImGui::SetTooltip("Mutes both rain systems - even the legacy overlay.\n"
                                      "Legacy overlay is the shipping default.");
                    break;
                case RainBoth:
                    ImGui::SetTooltip("Legacy overlay and particle drops at once, for\n"
                                      "side-by-side comparison.");
                    break;
                default:
                    break;
            }
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::TextDisabled("Particle rain is the shipping default; the legacy overlay");
    ImGui::TextDisabled("stays here for comparison. Density follows weather.");

    const bool particlesOn = mode == RainParticle || mode == RainBoth;
    if (particlesOn)
    {
        // Copy-edit-commit: one SetParams per frame only when something moved.
        RainParams params = GRain.Params();
        bool changed = false;

        ImGui::SeparatorText("Field");
        if (ImGui::SliderInt("Target drops", &params.targetDrops, 100, 30000))
        {
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Steady-state drop count the spawner aims for.");
        }
        if (ImGui::SliderInt("Max drops", &params.maxDrops, 500, 40000))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Area radius (m)", &params.areaRadius, 5.0f, 80.0f, "%.0f"))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Spawn height (m)", &params.spawnHeight, 5.0f, 60.0f, "%.0f"))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Particle density override", &params.densityOverride, -1.0f, 1.0f, "%.2f"))
        {
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("-1 follows live weather; 0..1 pins particle spawn density.\n"
                              "Benchmark presets use 1.0 so runs are repeatable.");
        }

        // One-click intensity presets. The sliders above can do all of this,
        // but "more rain" should not be a hunt: each preset pins the weather
        // density to full and sizes the pool for the look its name promises.
        ImGui::TextDisabled("Intensity:");
        if (ImGui::Button("Drizzle"))
        {
            params.targetDrops = 400;
            params.maxDrops = 900;
            params.densityOverride = 1.0f;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Rain"))
        {
            params.targetDrops = 1500;
            params.maxDrops = 3000;
            params.densityOverride = 1.0f;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Storm"))
        {
            params.targetDrops = 3500;
            params.maxDrops = 5000;
            params.densityOverride = 1.0f;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Deluge"))
        {
            params.targetDrops = 12000;
            params.maxDrops = 24000;
            params.densityOverride = 1.0f;
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Pins particle density to 1 and sizes target/max drops:\n"
                              "Drizzle 400/900, Rain 1500/3000, Storm 3500/5000, Deluge 12000/24000.");
        }

        ImGui::SeparatorText("Motion");
        if (ImGui::SliderFloat("Fall speed (m/s)", &params.fallSpeed, 2.0f, 25.0f, "%.1f"))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Speed jitter", &params.speedJitter, 0.0f, 1.0f, "%.2f"))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Wind response", &params.windResponse, 0.0f, 1.0f, "%.2f"))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Wind acceleration (1/s)", &params.windAcceleration, 0.0f, 20.0f, "%.1f"))
        {
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("How quickly drops already in flight converge on a changed wind.\n"
                              "5 means roughly 63%% of the change in 0.2 seconds.");
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("0 falls straight through gusts; 1 is fully advected --\n"
                              "watch the streaks lean against the Weather tab override\n"
                              "above");
        }

        ImGui::SeparatorText("Look");
        if (ImGui::SliderFloat("Streak length (m)", &params.streakLength, 0.2f, 3.0f, "%.2f"))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Streak width (m)", &params.streakWidth, 0.01f, 0.15f, "%.3f"))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Opacity", &params.opacity, 0.0f, 1.0f, "%.2f"))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Tint R", &params.red, 0.0f, 1.0f, "%.2f"))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Tint G", &params.green, 0.0f, 1.0f, "%.2f"))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Tint B", &params.blue, 0.0f, 1.0f, "%.2f"))
        {
            changed = true;
        }

        ImGui::SeparatorText("Ground");
        if (ImGui::Checkbox("Splashes", &params.splashes))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Splash size (m)", &params.splashSize, 0.05f, 1.5f, "%.2f"))
        {
            changed = true;
        }
        if (ImGui::SliderFloat("Splash lifetime (s)", &params.splashLifetime, 0.05f, 1.0f, "%.2f"))
        {
            changed = true;
        }

        ImGui::SeparatorText("Occlusion");
        struct OcclusionEntry
        {
            const char* label;
            RainOcclusion occlusion;
        };
        static const OcclusionEntry kOcclusions[] = {
            {"None", RainOcclusionNone}, {"Roof probe", RainOcclusionSheltered}, {"Rooms", RainOcclusionRooms}};
        ImGui::BeginGroup();
        for (const OcclusionEntry& entry : kOcclusions)
        {
            if (ImGui::RadioButton(entry.label, params.occlusion == entry.occlusion))
            {
                params.occlusion = entry.occlusion;
                changed = true;
            }
            ImGui::SameLine();
        }
        ImGui::EndGroup();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Rooms is the rooms-and-portals query shared with smoke --\n"
                              "the shipping intent. Roof probe is the cheap IsSheltered\n"
                              "version. None lets rain fall through roofs (diagnostic).");
        }
        ImGui::NewLine();
        if (ImGui::SliderFloat("Probe cadence (s)", &params.occlusionCadence, 0.05f, 1.0f, "%.2f"))
        {
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Seconds between occlusion probes per drop, staggered so\n"
                              "the whole pool never queries on the same frame. This\n"
                              "also sets how far ahead each probe looks.");
        }
        if (ImGui::SliderFloat("Probe lookahead x", &params.probeLookahead, 1.0f, 3.0f, "%.2f"))
        {
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The shelter probe casts along the drop's own path, over this\n"
                              "multiple of the distance it will cover before its next probe.\n"
                              "1.0 probes exactly the next interval and a gust or a frame\n"
                              "spike can still carry a drop past a roof; higher stops rain\n"
                              "slightly further above surfaces but never lets it through.");
        }
        if (ImGui::Checkbox("Probe at spawn", &params.probeAtSpawn))
        {
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("One upward ray per spawned drop. Catches drops BORN under\n"
                              "cover -- bridge decks, balconies, canopies, upper storeys of\n"
                              "buildings with no interior data -- which never cross a roof on\n"
                              "the way down and so no look-ahead can ever see. Costs a ray\n"
                              "per spawn; watch the spawn row in the benchmark table.");
        }
        if (ImGui::SliderFloat("Spawn probe (m)", &params.spawnProbeHeight, 5.0f, 60.0f, "%.0f"))
        {
            changed = true;
        }

        ImGui::SeparatorText("Openings");
        ImGui::TextDisabled("How much slack an opening gets before rain may come in.");
        if (ImGui::Checkbox("Strict: rain never enters", &params.strictIndoor))
        {
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("No opening forgives a shell crossing at all. The blunt\n"
                              "instrument for a building whose portals are simply wrong: no\n"
                              "rain through the door, and none through the wall either.");
        }
        ImGui::BeginDisabled(params.strictIndoor);
        if (ImGui::SliderFloat("Portal radius x", &params.portalRadiusScale, 0.1f, 1.0f, "%.2f"))
        {
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Fraction of an opening's inferred radius a crossing has to\n"
                              "fall within to count as coming IN through it. Portal centres\n"
                              "sit on the wall plane and the inferred radii are generous, so\n"
                              "this is what stands between rain through the door and rain\n"
                              "through the wall beside it. Smaller is stricter.");
        }
        if (ImGui::Checkbox("Windows forgive", &params.portalWindows))
        {
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Breaches forgive", &params.portalBreaches))
        {
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Doors are authored (In1..InN in the Paths LOD) and always\n"
                              "forgive. Windows and breaches come from the heuristic voxel\n"
                              "scan: a window disc can be metres wider than the hole, and a\n"
                              "breach is fire geometry that failed to close at all. Both are\n"
                              "off by default -- turning them on is how rain gets indoors.");
        }
        ImGui::EndDisabled();

        ImGui::SeparatorText("Sea");
        if (ImGui::SliderFloat("Ocean ripple gain", &params.seaRippleGain, 0.0f, 4.0f, "%.2f"))
        {
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Strength of the rain-strike term in the water interaction pass\n"
                              "-- the dimples and rings rain makes on the sea. It is a hashed\n"
                              "branch inside a compute pass that already runs over the whole\n"
                              "field every frame, so this costs nothing either way. 0 turns\n"
                              "sea ripples off.");
        }

        if (changed)
        {
            GRain.SetParams(params);
        }
    }

    if (mode == RainLegacy || mode == RainBoth)
    {
        ImGui::SeparatorText("Legacy comparison");
        float density = GRain.LegacyDensityOverride();
        if (ImGui::SliderFloat("Legacy density##rain", &density, -1.0f, 1.0f, "%.2f"))
        {
            GRain.SetLegacyDensityOverride(density);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("-1 means auto (weather). LOCAL AND UNREPLICATED: lets you\n"
                              "look at the old overlay on a clear day without dragging the\n"
                              "weather about. The real density is still driven by the Weather\n"
                              "state section below.");
        }
        if (density < 0.0f && GLandscape != nullptr)
        {
            ImGui::TextDisabled("Auto density now: %.2f", GLandscape->GetRainDensity());
        }
    }

    ImGui::SeparatorText("Stats");
    const RainStats& stats = GRain.Stats();
    ImGui::Text("Live drops %d   live splashes %d", stats.liveDrops, stats.liveSplashes);
    ImGui::Text("Roof %d   indoor %d   sky-probe %d   ground %d", stats.roofKills, stats.indoorKills,
                stats.probeKills, stats.groundHits);
    ImGui::TextDisabled("Spawned %d   born under cover %d   update %.0f us", stats.spawned,
                        stats.spawnKills, stats.lastUpdateUs);
    ImGui::TextDisabled("Nearby interiors %d   voxel sweeps this frame %d", stats.nearbyBuildings,
                        stats.occlusionTests);
    if (particlesOn && stats.lastUpdateUs > 0.0)
    {
        PushRainBenchmark(stats);
    }

    ImGui::SeparatorText("Particle benchmark");
    const float avgTotal = RainAverage(GRainBenchmark.total);
    const float avgSpawn = RainAverage(GRainBenchmark.spawn);
    const float avgOcclusion = RainAverage(GRainBenchmark.occlusion);
    const float avgSimulate = RainAverage(GRainBenchmark.simulate);
    const float avgDraw = RainAverage(GRainBenchmark.draw);
    if (ImGui::BeginTable("rainCost", 3, ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("phase");
        ImGui::TableSetupColumn("last us");
        ImGui::TableSetupColumn("120-frame avg us");
        ImGui::TableHeadersRow();
        const auto row = [](const char* label, double last, float average)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", last);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", average);
        };
        row("spawn", stats.spawnMicroseconds, avgSpawn);
        row("building gather", stats.occlusionMicroseconds, avgOcclusion);
        row("simulate + collision", stats.simulateMicroseconds, avgSimulate);
        row("draw", stats.drawMicroseconds, avgDraw);
        row("total", stats.lastUpdateUs, avgTotal);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Visible %d/%d drops   %d streak decals   %.3f us/live drop avg",
                        stats.drawnDrops, stats.liveDrops, stats.streakDecals,
                        stats.liveDrops > 0 ? avgTotal / static_cast<float>(stats.liveDrops) : 0.0f);
    if (GWind.IsActive())
    {
        const WindSample& wind = GWind.Sample();
        const RainParams& rain = GRain.Params();
        const float horizontal = wind.speed * rain.windResponse;
        const float lean = std::atan2(horizontal, std::max(rain.fallSpeed, 0.01f)) * (180.0f / kPi);
        ImGui::TextDisabled("Wind input %.1f m/s toward %.0f deg; target rain lean %.1f deg",
                            wind.speed, BearingFromRad(wind.directionRad), lean);
    }
    ImGui::TextDisabled("Presets pin density=1, disable splashes, enable Rooms occlusion,");
    ImGui::TextDisabled("clear the pool/history, and set target=max for a comparable run.");
    if (ImGui::Button("500 drops"))
    {
        SetRainBenchmarkPreset(500);
    }
    ImGui::SameLine();
    if (ImGui::Button("1500 drops"))
    {
        SetRainBenchmarkPreset(1500);
    }
    ImGui::SameLine();
    if (ImGui::Button("4000 drops"))
    {
        SetRainBenchmarkPreset(4000);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset stats"))
    {
        GRain.ResetStats();
        GRainBenchmark = RainBenchmarkHistory{};
    }
    if (!particlesOn)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("particle layer not running");
    }
}

void DrawFogSection()
{
    if (ImGui::Button("Reset fog settings")) ResetFogSettings();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Restore volumetric fog, both layers, terrain/patch controls and light scattering.\n"
                          "Fog amount returns to zero; snow's automatic fog still applies.");
    if (GWorld)
    {
        float wantedFog = GWorld->GetFog();
        if (ImGui::SliderFloat("Fog amount", &wantedFog, 0.0f, 1.0f, "%.2f"))
            GWorld->SetWeather(GWorld->GetOvercast(), wantedFog, 0.0f);
        ImGui::TextDisabled("Actual fog: %.2f", GWorld->GetFog());
    }
    if (!GEngine || !GEngine->SupportsSky())
    {
        ImGui::TextDisabled("Fog renderer selection requires WGPU.");
        return;
    }
    auto settings = GEngine->GetSkySettings();
    bool changed = false;
    int mode = settings.layeredFog ? 1 : 0;
    if (ImGui::Combo("Fog rendering", &mode, "Legacy fog\0Volumetric layered fog\0"))
    {
        settings.layeredFog = mode == 1;
        changed = true;
    }
    ImGui::TextWrapped("Volumetric fog is the default. Weather fog scales its density; zero fog adds no layered volume. Terrain following and patchiness make the mist vary across the landscape.");
    float ground = 0.0f;
    if (GScene && GScene->GetCamera() && GLandscape)
    {
        const Vector3 camera = GScene->GetCamera()->Position();
        ground = GLandscape->SurfaceY(camera.X(), camera.Z());
    }
    if (settings.layeredFog)
    {
        if (ImGui::Button("Low ground fog"))
        {
            settings.fogTerrainFollow = 1.0f;
            settings.fogTerrainReference = 0.0f;
            settings.fogLayerBase[0] = -3.0f;
            settings.fogLayerTop[0] = 12.0f;
            settings.fogLayerFeather[0] = 3.0f;
            settings.fogLayerExtinction[0] = 0.008f;
            settings.fogLayerExtinction[1] = 0.0f;
            settings.fogValleyStrength = 1.0f;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Two layers"))
        {
            const Engine::SkySettings defaults;
            settings.fogTerrainFollow = defaults.fogTerrainFollow;
            settings.fogTerrainReference = defaults.fogTerrainReference;
            for (int i = 0; i < 2; ++i)
            {
                settings.fogLayerBase[i] = defaults.fogLayerBase[i];
                settings.fogLayerTop[i] = defaults.fogLayerTop[i];
                settings.fogLayerFeather[i] = defaults.fogLayerFeather[i];
                settings.fogLayerExtinction[i] = i == 1 ? 0.003f : defaults.fogLayerExtinction[i];
            }
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Elevated band"))
        {
            settings.fogLayerExtinction[0] = 0.0f;
            settings.fogTerrainFollow = 0.0f;
            settings.fogTerrainReference = 0.0f;
            settings.fogLayerBase[1] = ground + 20.0f;
            settings.fogLayerTop[1] = ground + 60.0f;
            settings.fogLayerFeather[1] = 5.0f;
            settings.fogLayerExtinction[1] = 0.003f;
            changed = true;
        }
        if (ImGui::Button("Valley mist"))
        {
            // A fixed ASL ceiling is what makes ridges emerge above valley fog.
            // Ground-following layers climb every mountain instead.
            float valleyFloor = ground;
            if (GScene && GScene->GetCamera() && GLandscape)
            {
                const Vector3 camera = GScene->GetCamera()->Position();
                for (int x = -2; x <= 2; ++x)
                    for (int z = -2; z <= 2; ++z)
                    {
                        const float candidate = GLandscape->SurfaceY(camera.X() + x * 400.0f, camera.Z() + z * 400.0f);
                        if (candidate > 1.0f) valleyFloor = std::min(valleyFloor, candidate);
                    }
            }
            settings.fogTerrainFollow = 0.0f;
            settings.fogTerrainReference = 0.0f;
            settings.fogLayerBase[0] = -100.0f;
            settings.fogLayerTop[0] = valleyFloor + 25.0f;
            settings.fogLayerFeather[0] = 8.0f;
            settings.fogLayerExtinction[0] = 0.008f;
            settings.fogLayerExtinction[1] = 0.0f;
            settings.fogValleyStrength = 2.0f;
            settings.fogPatchStrength = 0.6f;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Mountain mist"))
        {
            settings.fogTerrainFollow = 0.5f;
            settings.fogTerrainReference = ground;
            settings.fogLayerBase[0] = ground;
            settings.fogLayerTop[0] = ground + 60.0f;
            settings.fogLayerFeather[0] = 15.0f;
            settings.fogLayerExtinction[0] = 0.006f;
            settings.fogLayerExtinction[1] = 0.0f;
            settings.fogValleyStrength = 0.0f;
            settings.fogPatchStrength = 0.5f;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Compact mist band"))
        {
            settings.fogTerrainFollow = 1.0f;
            settings.fogTerrainReference = 0.0f;
            settings.fogLayerBase[0] = 1.0f;
            settings.fogLayerTop[0] = 5.0f;
            settings.fogLayerFeather[0] = 0.35f;
            settings.fogLayerExtinction[0] = 0.018f;
            settings.fogLayerExtinction[1] = 0.0f;
            settings.fogValleyStrength = 1.0f;
            settings.fogPatchStrength = 0.35f;
            changed = true;
        }
        ImGui::TextDisabled("Valley mist sets a fixed ceiling above nearby low ground. Hills above that ceiling stay outside the mist; Mountain mist anchors to this location.");
        ImGui::SeparatorText("Mist height and thickness");
        int reference = settings.fogTerrainFollow == 0.0f ? 0 :
            settings.fogTerrainFollow == 1.0f && settings.fogTerrainReference == 0.0f ? 1 : 2;
        if (ImGui::Combo("Height reference", &reference, "Sea level (ASL)\0Above local ground\0Terrain blend\0"))
        {
            // Keep the visible band at this camera when changing coordinates.
            const float oldShift = settings.fogTerrainFollow * (ground - settings.fogTerrainReference);
            settings.fogTerrainFollow = reference == 0 ? 0.0f : reference == 1 ? 1.0f : 0.5f;
            settings.fogTerrainReference = 0.0f;
            const float newShift = settings.fogTerrainFollow * ground;
            for (int i = 0; i < 2; ++i)
            {
                settings.fogLayerBase[i] += oldShift - newShift;
                settings.fogLayerTop[i] += oldShift - newShift;
            }
            changed = true;
        }
        ImGui::TextWrapped(reference == 1 ? "Bottom/top are metres above the local ground. The band follows hills." :
            reference == 0 ? "Bottom/top are metres above sea level. Mountains intersect the fixed mist band." :
            "Heights use the blended terrain reference. This location's actual altitude range is shown below; adjust terrain following in the advanced controls.");
        for (int i = 0; i < 2; ++i)
        {
            ImGui::PushID(i);
            ImGui::Text("Mist layer %d", i + 1);
            bool enabled = settings.fogLayerExtinction[i] > 0.0f;
            if (ImGui::Checkbox("Enabled", &enabled))
            {
                settings.fogLayerExtinction[i] = enabled ? (i == 0 ? 0.008f : 0.003f) : 0.0f;
                changed = true;
            }
            bool bandChanged = false;
            if (ImGui::SliderFloat("Bottom height", &settings.fogLayerBase[i], -100.0f, 2500.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp))
            {
                settings.fogLayerTop[i] = std::max(settings.fogLayerTop[i], settings.fogLayerBase[i] + 0.25f);
                bandChanged = true;
            }
            if (ImGui::SliderFloat("Top height", &settings.fogLayerTop[i], -99.75f, 3000.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp))
            {
                settings.fogLayerBase[i] = std::min(settings.fogLayerBase[i], settings.fogLayerTop[i] - 0.25f);
                bandChanged = true;
            }
            float thickness = settings.fogLayerTop[i] - settings.fogLayerBase[i];
            if (ImGui::SliderFloat("Mist thickness", &thickness, 0.25f, 500.0f, "%.2f m", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
            {
                settings.fogLayerTop[i] = settings.fogLayerBase[i] + thickness;
                bandChanged = true;
            }
            if (bandChanged)
            {
                settings.fogLayerFeather[i] = std::min(settings.fogLayerFeather[i], 0.5f * (settings.fogLayerTop[i] - settings.fogLayerBase[i]));
                changed = true;
            }
            changed |= ImGui::SliderFloat("Edge softness", &settings.fogLayerFeather[i], 0.1f,
                std::max(0.1f, std::min(100.0f, 0.5f * (settings.fogLayerTop[i] - settings.fogLayerBase[i]))), "%.2f m", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderFloat("Fog density", &settings.fogLayerExtinction[i], 0.0f, 0.05f, "%.4f /m");
            const float shift = settings.fogTerrainFollow * (ground - settings.fogTerrainReference);
            ImGui::TextDisabled("Here: %.2f to %.2f m ASL; thickness %.2f m", settings.fogLayerBase[i] + shift,
                settings.fogLayerTop[i] + shift, settings.fogLayerTop[i] - settings.fogLayerBase[i]);
            ImGui::PopID();
        }
        ImGui::TextWrapped("For valley-only mist use Sea level (ASL), set the top below the hilltops and keep the second layer disabled. Fog amount also shortens the original game visibility range; use Fog density for a thicker bank without closing distant hills. Ctrl+click a height slider for precise metres.");
        if (GWorld && GWorld->GetFog() <= 0.0f)
            ImGui::TextDisabled("Set Fog amount above zero to see a layer.");
    }
    if (settings.layeredFog && ImGui::TreeNode("Fog terrain, patches and lighting"))
    {
        changed |= ImGui::SliderFloat("Follow terrain", &settings.fogTerrainFollow, 0.0f, 1.0f, "%.2f");
        ImGui::TextWrapped("0 keeps fixed sea-level layers. 1 follows the local ground; with reference altitude 0, layer heights are metres above ground. Intermediate values let mist collect below hills.");
        changed |= ImGui::SliderFloat("Reference altitude", &settings.fogTerrainReference, -100.0f, 1000.0f, "%.1f m");
        changed |= ImGui::SliderFloat("Valley density boost", &settings.fogValleyStrength, 0.0f, 3.0f, "%.2f");
        changed |= ImGui::SliderFloat("Valley width", &settings.fogValleyRadius, 25.0f, 1000.0f, "%.0f m");
        changed |= ImGui::SliderFloat("Patchiness", &settings.fogPatchStrength, 0.0f, 0.95f, "%.2f");
        changed |= ImGui::SliderFloat("Patch width", &settings.fogPatchHorizontalScale, 25.0f, 2000.0f, "%.0f m");
        changed |= ImGui::SliderFloat("Patch height", &settings.fogPatchVerticalScale, 5.0f, 1000.0f, "%.0f m");
        changed |= ImGui::SliderFloat("Coverage edge softness", &settings.fogCoverageFeather, 0.0f, 2000.0f, "%.0f m");
        changed |= ImGui::SliderFloat("Light scattering", &settings.fogLayerAlbedo, 0.0f, 1.0f, "%.3f");
        changed |= ImGui::SliderFloat("Sun-ray focus", &settings.fogLayerG, 0.0f, 0.95f, "%.3f");
        ImGui::TextWrapped("Light scattering controls the fog's brightness. Sun-ray focus makes sunlight shafts more prominent when looking toward the sun; terrain, buildings and clouds control where light reaches the mist.");
        ImGui::TextWrapped("Physical sky lighting and current terrain/roof coverage are required. Covered ray segments are excluded. Invalid intervals or overlap with the cloud deck refuse the layer effect.");
        ImGui::TreePop();
    }
    if (changed) GEngine->SetSkySettings(settings);
}

void DrawWeatherStateSection()
{
    ImGui::SeparatorText("Weather state");

    if (GWorld == nullptr)
    {
        ImGui::TextDisabled("No world loaded.");
        return;
    }

    bool frozen = GWorld->IsWeatherFrozen();
    if (ImGui::Checkbox("Freeze forecast", &frozen))
    {
        GWorld->SetFreezeWeather(frozen);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Holds the overcast/fog transition where it is, so scrubbing the\n"
                          "time of day does not roll new weather underneath your test.");
    }

    // Read the shared world value each frame, including changes made in Zeus.
    // A dev slider edits it immediately; it does not keep a stale private target.
    float wantedOvercast = GWorld->GetOvercast();
    if (ImGui::SliderFloat("Overcast", &wantedOvercast, 0.0f, 1.0f, "%.2f"))
        GWorld->SetWeather(wantedOvercast, GWorld->GetFog(), 0.0f);
    ImGui::TextDisabled("Live weather controls share the same state as Zeus.");

    ImGui::TextDisabled("Actual: overcast %.2f, fog %.2f", GWorld->GetOvercast(), GWorld->GetFog());
    ImGui::TextDisabled("Overcast also drives the wind: mean speed and gust envelope");
    ImGui::TextDisabled("both scale with it unless the override above is on.");
    // SKY-002. Overcast is also the night sky's star occlusion, and that is not obvious from
    // either end: the deck is nearly black after dark, so the only visible evidence that raising
    // this did anything to the sky is the stars going out.
    ImGui::TextDisabled("At night, overcast is what hides the STARS: it drives the wgpu deck's");
    ImGui::TextDisabled("coverage (Sky tab -> 'Coverage follows weather'), and the deck's own");
    ImGui::TextDisabled("transmittance is what the star field is multiplied by. Strength:");
    ImGui::TextDisabled("WGR_STAR_OCCLUSION=<0..1> (1 = default, 0 = stars punch through).");
    // SKY-004. The other half of the same question, and the one people ask first: with the stars
    // hidden, what is an overcast night lit BY? The moon, now -- and only when there is one up.
    ImGui::TextDisabled("");
    ImGui::TextDisabled("The deck is lit by the MOON at night (Sky tab -> Moon for position,");
    ImGui::TextDisabled("phase and 'Moonlight intensity'). A full moon over full overcast is a");
    ImGui::TextDisabled("soft grey dome; a broken deck gets lit tops and dark bases. With NO");
    ImGui::TextDisabled("moon up the night stays black, which is correct -- check the Moon");
    ImGui::TextDisabled("readout's elevation before calling a black night a bug. Gain:");
    ImGui::TextDisabled("WGR_MOON_CLOUDS=<0..4> (1 = physical default, 0 = sun-only decks).");
    ImGui::TextDisabled("WGR_MOON_CLOUD_BLUE=<0..1> is a STYLISTIC cold tint, 0 by default:");
    ImGui::TextDisabled("moonlight is reflected sunlight, and film's blue moonlight is an");
    ImGui::TextDisabled("artefact of scotopic vision rather than of the light itself.");
}

} // namespace

void DrawWeatherTab()
{
    if (ImGui::CollapsingHeader("Wind", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawReadout();
        DrawOverrideSection();
    }
    if (ImGui::CollapsingHeader("Fog", ImGuiTreeNodeFlags_DefaultOpen)) DrawFogSection();
    if (ImGui::CollapsingHeader("Rain")) DrawRainSection();
    if (ImGui::CollapsingHeader("Snow", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto& snow = GSnow();
        ImGui::Checkbox("Enable snow", &snow.enabled);
        ImGui::Checkbox("Accumulate new snow", &snow.falling);
        ImGui::Checkbox("Geometric close-up tracks", &snow.detailedGeometry);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refines terrain within 8 m of the camera to resolve real snow depressions. Extra geometry only while snow is enabled; collision remains on bare ground.");
        ImGui::SliderFloat("Snowfall (m/min)", &snow.metresPerMinute, 0.0f, 0.2f, "%.3f");
        ImGui::SliderFloat("Snowflake density", &snow.flakeMultiplier, 0.25f, 8.0f, "%.2fx");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Up to 8x the normal particle budget. Dense storms cost more; roof occlusion and wind remain active.");
        if (ImGui::Button("Snowstorm"))
        {
            snow.ApplySnowstormPreset();
        }
        ImGui::TextWrapped("Snowstorm: dense snowfall and 10 cm/min accumulation. Wind is controlled above; this does not instantly cover the ground.");
        ImGui::Text("Flakes: %d active, %d visible", GSnowFlakes.Stats().liveDrops, GSnowFlakes.Stats().drawnDrops);
        ImGui::TextWrapped("Falling snow adds overcast without forcing rain or thunderstorms; stopping snowfall restores the mission's cloud cover.");
        ImGui::Text("Snow fog: %.2f (automatic 0.15 to 0.60; stronger Fog settings take priority)", snow.RenderFog(GLandscape ? GLandscape->GetFog() : 0.0f));
        ImGui::SliderFloat("Maximum snow depth (m)", &snow.maxDepth, 0.05f, 1.0f, "%.2f");
        if (ImGui::Button("Add 10 cm of snow")) snow.Deposit(0.1f);
        ImGui::SameLine();
        if (ImGui::Button("Clear snow and tracks")) snow.Reset();
        if (ImGui::Button("Reset snow settings (keep tracks)")) snow.ResetSettings();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restores the default settings and disables snow. Re-enabling reveals the preserved cover and tracks. Use Clear snow and tracks to erase them.");
        ImGui::Separator();
        ImGui::Checkbox("Alpine snowline (permanent cover above height)", &snow.snowlineEnabled);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Terrain above the snowline is snow-covered without any snowfall. This changes WHERE snow lies, never the snowfall itself; falling snow still accumulates everywhere.");
        ImGui::SliderFloat("Snowline height (m)", &snow.snowlineHeight, 0.0f, 1500.0f, "%.1f");
        ImGui::SliderFloat("Snowline transition (m)", &snow.snowlineRange, 10.0f, 500.0f, "%.0f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Band over which first flakes ramp to full cover with altitude.");
        float snowlineDepthCm = snow.snowlineDepth * 100.0f;
        if (ImGui::SliderFloat("Snowline snow depth (cm)", &snowlineDepthCm, 1.0f, 30.0f, "%.0f"))
            snow.snowlineDepth = snowlineDepthCm / 100.0f;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How deep the alpine cover itself is. Full white from ~4 cm; more mainly lifts the surface.");
        // Tuning aid: ground height and resulting cover where the camera stands,
        // so the snowline slider can be set against the actual terrain.
        if (GScene != nullptr && GScene->GetCamera() != nullptr && GLandscape != nullptr)
        {
            const Vector3 camPos = GScene->GetCamera()->Position();
            const float groundY = GLandscape->SurfaceY(camPos.X(), camPos.Z());
            ImGui::Text("Ground under camera: %.1f m (camera %.1f m); snowline cover here: %.1f cm",
                        groundY, camPos.Y(), snow.AltitudeDepthAt(camPos.X(), camPos.Z()) * 100.0f);
        }
        if (SnowField::ResolveSnowlineOverride().active)
            ImGui::TextDisabled("POSEIDON_SNOWLINE overrides the height above.");
        ImGui::Text("Depth %.1f cm; persistent chunks %zu / %zu", snow.Depth() * 100.0f, snow.Chunks(), SnowField::MaxChunks);
        if (snow.Chunks() >= SnowField::MaxChunks)
            ImGui::TextWrapped("Track storage full: visible tracks retained. Filled tracks are reclaimed automatically.");
        if (snow.Rejected())
            ImGui::Text("Snow cells previously skipped at storage limit: %zu", snow.Rejected());
        ImGui::TextWrapped("Mission-local terrain experiment. Tracks do not expire with time or camera movement; only new snowfall fills them. No savegame/network replication or changed ground collision yet.");
    }
    if (ImGui::CollapsingHeader("Overcast and forecast", ImGuiTreeNodeFlags_DefaultOpen)) DrawWeatherStateSection();
    if (ImGui::CollapsingHeader("Wind consumers and diagnostics")) DrawConsumerSection();
}

} // namespace Poseidon::Dev
