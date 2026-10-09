// AI-AIM-001: see AICombatPace.hpp.

#include <Poseidon/Core/Application.hpp>
#include <Poseidon/AI/AICombatPace.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>

#include <algorithm>

namespace Poseidon::CombatPace
{
namespace
{
struct Settings
{
    bool shootFirst = false;
    float proneSpeed = 1.0f;
};

const Settings& Get()
{
    static Settings s;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        if (const ParamEntry* cls = Pars.FindEntry("CfgAIFork"))
        {
            if (const ParamEntry* e = cls->FindEntry("shootFirst"))
                s.shootFirst = static_cast<float>(*e) != 0;
            if (const ParamEntry* e = cls->FindEntry("proneSpeed"))
                s.proneSpeed = std::clamp(static_cast<float>(*e), 0.25f, 4.0f);
        }
    }
    return s;
}
} // namespace

bool ShootFirstEnabled()
{
    return Get().shootFirst;
}

float ProneSpeed()
{
    return Get().proneSpeed;
}
} // namespace Poseidon::CombatPace
