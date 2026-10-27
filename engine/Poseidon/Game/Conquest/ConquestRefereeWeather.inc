// SPDX-License-Identifier: GPL-3.0-or-later
// The war's weather (the setup screen's Weather), included by ConquestReferee.cpp inside its anonymous
// namespace. A fixed sky, or one that changes as the war goes -- slowly and by one step at a time, over
// twenty minutes of game time, so a change is noticed late rather than seen happening (owner, 2026-10-27:
// "weather change should be very discreet").

enum WeatherKind
{
    WeatherMission = 0, // the mission's own, untouched
    WeatherClear,
    WeatherCloudy,
    WeatherOvercast,
    WeatherRain,
    WeatherFog,
    WeatherChanging,
};

struct WeatherState
{
    float overcast;
    float fog;
    float rain;
};

constexpr WeatherState WeatherStates[] = {
    {0.0f, 0.0f, 0.0f},   // (the mission's: not applied)
    {0.1f, 0.0f, 0.0f},   // clear
    {0.45f, 0.05f, 0.0f}, // cloudy
    {0.75f, 0.1f, 0.0f},  // overcast
    {0.9f, 0.15f, 0.7f},  // rain
    {0.5f, 0.55f, 0.0f},  // fog
};
constexpr const char* WeatherNames[] = {"the mission's", "clear", "cloudy", "overcast", "rain", "fog", "changing"};
constexpr float WeatherBlendSeconds = 1200.0f;                             // a change takes 20 game minutes
constexpr float WeatherHoldMin = 1500.0f, WeatherHoldMax = 2400.0f;        // and holds 25-40 minutes

void SetWeather(int kind, float seconds)
{
    GameState* gs = GWorld ? GWorld->GetGameState() : nullptr;
    if (!gs || kind <= WeatherMission || kind >= WeatherChanging)
        return;
    const WeatherState& w = WeatherStates[kind];
    char code[160];
    std::snprintf(code, sizeof(code), "%.0f setOvercast %.2f; %.0f setFog %.2f; %.0f setRain %.2f", seconds, w.overcast,
                  seconds, w.fog, seconds, w.rain);
    gs->EvaluateMultiple(code);
}

// The war begins: the chosen sky at once (a changing one starts from clear, cloudy or overcast).
void StartWeather(Referee& r, float now)
{
    if (r.options.weather <= WeatherMission)
        return;
    r.weatherNow = r.options.weather == WeatherChanging ? WeatherClear + static_cast<int>(GRandGen.RandomValue() * 3.0f)
                                                         : r.options.weather;
    r.weatherNow = std::clamp(r.weatherNow, static_cast<int>(WeatherClear), static_cast<int>(WeatherFog));
    SetWeather(r.weatherNow, 0.0f);
    r.weatherNext = now + WeatherHoldMin + GRandGen.RandomValue() * (WeatherHoldMax - WeatherHoldMin);
    WarLog(std::string("weather: ") + WeatherNames[r.weatherNow] + (r.options.weather == WeatherChanging ? ", changing as the war goes" : ""));
}

// A changing sky: now and then one step to a neighbouring weather, clear - cloudy - overcast - rain, and
// sometimes from cloudy into fog and back.
void StepWeather(Referee& r, float now)
{
    if (r.options.weather != WeatherChanging || r.weatherNow <= WeatherMission || now < r.weatherNext)
        return;
    const float roll = GRandGen.RandomValue();
    int next = r.weatherNow;
    if (r.weatherNow == WeatherFog)
        next = WeatherCloudy;
    else if (r.weatherNow == WeatherCloudy && roll < 0.15f)
        next = WeatherFog;
    else if (r.weatherNow == WeatherClear)
        next = WeatherCloudy;
    else if (r.weatherNow == WeatherRain)
        next = WeatherOvercast;
    else
        next = r.weatherNow + (roll < 0.55f ? -1 : 1);
    r.weatherNow = std::clamp(next, static_cast<int>(WeatherClear), static_cast<int>(WeatherFog));
    SetWeather(r.weatherNow, WeatherBlendSeconds);
    r.weatherNext = now + WeatherBlendSeconds + WeatherHoldMin + GRandGen.RandomValue() * (WeatherHoldMax - WeatherHoldMin);
    WarLog(std::string("the weather turns ") + WeatherNames[r.weatherNow] + " over the next 20 minutes");
}
