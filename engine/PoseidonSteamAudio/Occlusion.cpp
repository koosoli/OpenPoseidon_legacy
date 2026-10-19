#include <Poseidon/Foundation/Common/Global.hpp>
#include <PoseidonSteamAudio/Occlusion.hpp>

#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Poseidon
{
namespace SteamAudio
{
namespace
{
// Terrain is sampled every this many metres, skipping this much at both ends (a source at a
// soldier's feet must not be "behind" the bump it stands on), up to this far.
constexpr float kTerrainStep = 4.f;
constexpr float kTerrainEndSkip = 3.f;
constexpr float kTerrainMaxDist = 800.f;
// Ground this far above the line is ignored (heightmap noise); this much more blocks fully.
constexpr float kTerrainSlack = 0.3f;
constexpr float kTerrainFull = 3.0f;
// Buildings are swept up to this distance; past it the sound is faint and the sweep long.
constexpr float kBuildingMaxDist = 400.f;

// Per-band gain of a fully blocking ridge (diffraction over it) and of a building wall
// (through windows, doors and the wall itself): low / mid / high, roughly -8/-14/-22 dB and
// -10/-18/-28 dB. Tuned by ear, not measured.
constexpr float kTerrainBands[3] = {0.40f, 0.20f, 0.08f};
constexpr float kBuildingBands[3] = {0.30f, 0.12f, 0.04f};
} // namespace

bool OcclusionEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("POSEIDON_STEAM_OCCLUSION");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

OcclusionResult ComputeOcclusion(Vector3Par listener, Vector3Par sourceIn)
{
    OcclusionResult r;
    if (!GLandscape || !listener.IsFinite() || !sourceIn.IsFinite())
        return r;

    // A source on the ground (footsteps, a prone soldier's position) is heard from just above it.
    Vector3 source = sourceIn;
    const float srcGround = GLandscape->SurfaceY(source.X(), source.Z());
    if (source.Y() < srcGround + 1.0f)
        source[1] = srcGround + 1.0f;

    const Vector3 d = source - listener;
    const float dist = d.Size();
    if (dist < 2.f)
        return r;
    const Vector3 dir = d / dist;

    // Terrain: how far the ground rises above the line of sight.
    if (dist <= kTerrainMaxDist)
    {
        float maxExcess = 0.f;
        for (float t = kTerrainEndSkip; t < dist - kTerrainEndSkip; t += kTerrainStep)
        {
            const Vector3 p = listener + dir * t;
            maxExcess = std::max(maxExcess, GLandscape->SurfaceY(p.X(), p.Z()) - p.Y());
        }
        r.terrain = std::clamp((maxExcess - kTerrainSlack) / kTerrainFull, 0.f, 1.f);
    }

    // Buildings: static fire geometry on the line.
    if (dist <= kBuildingMaxDist)
    {
        CollisionBuffer hits;
        GLandscape->ObjectCollision(hits, nullptr, nullptr, listener + dir * 0.3f, source - dir * 0.3f, 0.f,
                                    ObjIntersectFire);
        for (int i = 0; i < hits.Size(); i++)
        {
            const Object* o = hits[i].object;
            if (o && o->Static() && o->GetDestructType() != DestructTree)
            {
                r.building = true;
                break;
            }
        }
    }

    for (int b = 0; b < 3; b++)
    {
        float g = 1.f - r.terrain + r.terrain * kTerrainBands[b];
        if (r.building)
            g *= kBuildingBands[b];
        r.bands[b] = g;
    }
    return r;
}

} // namespace SteamAudio
} // namespace Poseidon
