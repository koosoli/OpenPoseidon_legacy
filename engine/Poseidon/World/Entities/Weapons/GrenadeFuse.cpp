// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/World/Entities/Weapons/GrenadeFuse.hpp>

namespace Poseidon::GrenadeFuse
{
namespace
{
Settings g_settings;
}

Settings& Get() { return g_settings; }

// Assignment from a fresh instance, so the defaults are the ones written in the
// struct and cannot drift out of step with a second copy of them.
void Reset() { g_settings = Settings{}; }

BounceResult Bounce(Vector3Par speed, Vector3Par surfaceNormal, const Settings& settings)
{
    BounceResult result;

    // Split the velocity into the part heading into the surface and the part
    // sliding along it, and treat them differently -- that separation IS the
    // model. One coefficient decides how much comes back, the other how much
    // carries on, and a grenade differs from a bouncy ball mostly in the second.
    const float into = speed.DotProduct(surfaceNormal);
    const Vector3 normalPart = surfaceNormal * into;
    const Vector3 tangentPart = speed - normalPart;

    result.speed = tangentPart * settings.tangentialKeep - normalPart * settings.restitution;

    if (result.speed.Size() < settings.restSpeed)
    {
        result.speed = VZero;
        result.atRest = true;
    }
    return result;
}

} // namespace Poseidon::GrenadeFuse
