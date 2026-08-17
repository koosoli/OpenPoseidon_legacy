#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>

#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Scene/Object.hpp>

#include <cmath>

namespace Poseidon
{
namespace
{

long long GSweepCount = 0;

/// The no-world fallback: open sky, flat ground, nothing to hit. Not a stub for
/// its own sake — it is what lets SmokeVolume::Simulate be exercised in a unit
/// test with no Landscape, no Scene and no renderer.
class NullSmokeWorldQuery final : public ISmokeWorldQuery
{
  public:
    SmokeSurfaceHit SweepSphere(Vector3Par, Vector3Par, float) const override { return SmokeSurfaceHit{}; }
    float GroundHeight(float, float) const override { return 0.0f; }
    bool IsSheltered(Vector3Par, float) const override { return false; }
};

class PoseidonSmokeWorldQuery final : public ISmokeWorldQuery
{
  public:
    SmokeSurfaceHit SweepSphere(Vector3Par from, Vector3Par to, float radius) const override
    {
        SmokeSurfaceHit result;
        if (GLandscape == nullptr)
        {
            return result;
        }

        ++GSweepCount;

        CollisionBuffer buffer;
        // `with` and `ignore` are both null: there is no smoke Object taking part
        // in this sweep, only a point in space. ObjectCollision already skips
        // Temporary and TypeTempVehicle, which is every smoke entity in the
        // world including the legacy cloudlets — so plumes cannot collide with
        // each other or with themselves, which is both correct and free.
        //
        // ObjIntersectFire, not ObjIntersectView: fire geometry is the solid
        // shell of a building. View geometry includes glass and foliage, which
        // smoke should drift through.
        GLandscape->ObjectCollision(buffer, nullptr, nullptr, from, to, radius, ObjIntersectFire);

        if (buffer.Size() <= 0)
        {
            return result;
        }

        // Deepest penetration wins. The buffer is not sorted along the sweep, and
        // taking the first entry gives whichever object the grid walk happened to
        // reach first — which produced particles that slid along a wall and then
        // popped through a pillar standing in front of it.
        int best = 0;
        for (int i = 1; i < buffer.Size(); ++i)
        {
            if (buffer[i].under > buffer[best].under)
            {
                best = i;
            }
        }

        const CollisionInfo& info = buffer[best];
        if (info.object == nullptr)
        {
            return result;
        }

        // EVERYTHING IN CollisionInfo IS IN THE HIT OBJECT'S MODEL SPACE. Using
        // `pos` and the normal directly as world-space values puts the contact
        // wherever the building happens to sit relative to the map origin and
        // rotates the normal by the building's heading — which looks like smoke
        // reacting to walls that are not there. Convert, as SoldierOldSim and
        // the vehicle collision code do.
        result.hit = true;
        result.position = info.object->PositionModelToWorld(info.pos);

        // `surfaceNormal`, not `dirOut`: on this sweep path dirOut is the
        // penetration chord (the engine says so in a comment) and points ALONG
        // the travel direction, so reflecting off it does nothing at all.
        Vector3 normal = info.object->DirectionModelToWorld(info.surfaceNormal);

        // Zero when the segment lay entirely inside a component and clipped
        // against no plane. Pushing back the way we came is not a real normal
        // but it never leaves a particle stuck inside a wall.
        const float length = normal.Size();
        if (length > 1e-4f)
        {
            normal = normal / length;
        }
        else
        {
            const Vector3 back = from - to;
            const float backLength = back.Size();
            normal = backLength > 1e-4f ? back / backLength : Vector3(0.0f, 1.0f, 0.0f);
        }

        // The plane normal that clipped the segment faces INTO the solid (the
        // convex component's planes have positive distance inside). The caller
        // wants the outward normal to push along, so flip it if it disagrees
        // with the direction we came from.
        if (normal.DotProduct(from - to) < 0.0f)
        {
            normal = -normal;
        }

        result.normal = normal;
        return result;
    }

    float GroundHeight(float x, float z) const override
    {
        return GLandscape != nullptr ? GLandscape->SurfaceY(x, z) : 0.0f;
    }

    bool IsSheltered(Vector3Par pos, float probeHeight) const override
    {
        if (GLandscape == nullptr)
        {
            return false;
        }

        ++GSweepCount;

        CollisionBuffer buffer;
        const Vector3 up = pos + Vector3(0.0f, probeHeight, 0.0f);
        // A thin probe, not a sphere: this asks "is there a roof", and a fat
        // sphere would report a wall two metres to the side as a roof.
        GLandscape->ObjectCollision(buffer, nullptr, nullptr, pos, up, 0.05f, ObjIntersectFire);
        return buffer.Size() > 0;
    }
};

PoseidonSmokeWorldQuery GEngineQuery;
NullSmokeWorldQuery GNullQuery;
const ISmokeWorldQuery* GOverride = nullptr;

} // namespace

const ISmokeWorldQuery& GSmokeWorldQuery()
{
    if (GOverride != nullptr)
    {
        return *GOverride;
    }
    return GLandscape != nullptr ? static_cast<const ISmokeWorldQuery&>(GEngineQuery)
                                 : static_cast<const ISmokeWorldQuery&>(GNullQuery);
}

void SetSmokeWorldQuery(const ISmokeWorldQuery* query)
{
    GOverride = query;
}

long long SmokeSweepCount()
{
    return GSweepCount;
}

void ResetSmokeSweepCount()
{
    GSweepCount = 0;
}

} // namespace Poseidon
