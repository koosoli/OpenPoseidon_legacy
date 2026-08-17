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
    float FloorHeight(float, float) const override { return 0.0f; }
    float FloorHeightBelow(Vector3Par) const override { return 0.0f; }
    bool IsSheltered(Vector3Par, float) const override { return false; }
};

class PoseidonSmokeWorldQuery final : public ISmokeWorldQuery
{
  public:
    /// One zero-width ray through the world, returning the deepest hit converted
    /// to world space with a real outward normal. Factored out because
    /// SweepSphere below fires several of these: Object::Intersect(beg, end,
    /// radius, ...) IGNORES ITS RADIUS ARGUMENT — the bounding checks test the
    /// line's distance and the clip loop clips the segment; `radius` is never
    /// read. So the "sphere sweep" the interface promises has to be built here
    /// from rays, or a puff's centre has to cross a wall before anything hits.
    bool CastRay(Vector3Par from, Vector3Par to, SmokeSurfaceHit& out) const
    {
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
        GLandscape->ObjectCollision(buffer, nullptr, nullptr, from, to, 0.0f, ObjIntersectFire);

        if (buffer.Size() <= 0)
        {
            return false;
        }

        // Nearest entry along the ray wins. `under` on this path is the
        // parametric t of the ENTRY point (see ObjectIntersect.cpp: "ret.under =
        // t" where t = bt), so the smallest value is the surface the ray
        // reached first — which is the one a particle should stop at. Taking
        // the largest, as an earlier revision did, picked the FARTHEST wall on
        // a ray that crossed a room, and reflected the particle off it.
        int best = 0;
        for (int i = 1; i < buffer.Size(); ++i)
        {
            if (buffer[i].under < buffer[best].under)
            {
                best = i;
            }
        }

        const CollisionInfo& info = buffer[best];
        if (info.object == nullptr)
        {
            return false;
        }

        // EVERYTHING IN CollisionInfo IS IN THE HIT OBJECT'S MODEL SPACE.
        out.hit = true;
        out.position = info.object->PositionModelToWorld(info.pos);

        // `surfaceNormal`, not `dirOut`: on this sweep path dirOut is the
        // penetration chord and points ALONG the travel direction.
        Vector3 normal = info.object->DirectionModelToWorld(info.surfaceNormal);
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
        // Convex-component planes face INTO the solid; the caller wants outward.
        if (normal.DotProduct(from - to) < 0.0f)
        {
            normal = -normal;
        }
        out.normal = normal;
        return true;
    }

    SmokeSurfaceHit SweepSphere(Vector3Par from, Vector3Par to, float radius) const override
    {
        SmokeSurfaceHit result;
        if (GLandscape == nullptr)
        {
            return result;
        }

        // Centre ray first: it is the cheapest and, for a particle heading
        // straight at a wall, the one that hits.
        SmokeSurfaceHit best;
        bool haveBest = CastRay(from, to, best);

        // Then a ring of rays offset by `radius` around the axis of travel, plus
        // one straight up and one straight down. The ring catches a wall the
        // particle is grazing; up/down catch a ceiling or floor when the sweep
        // is nearly horizontal, which is exactly the case where a ring around
        // the axis has no vertical member. Seven rays per sweep is the price of
        // an interface that promised a sphere; SmokeVolume rations sweeps per
        // particle so the total stays bounded.
        Vector3 axis = to - from;
        const float axisLength = axis.Size();
        if (radius > 1e-3f)
        {
            Vector3 dir = axisLength > 1e-4f ? axis / axisLength : Vector3(0.0f, 1.0f, 0.0f);
            // Two perpendiculars to the travel direction.
            Vector3 up(0.0f, 1.0f, 0.0f);
            if (std::fabs(dir.DotProduct(up)) > 0.9f)
            {
                up = Vector3(1.0f, 0.0f, 0.0f);
            }
            Vector3 side = dir.CrossProduct(up);
            side = side / std::max(side.Size(), 1e-6f);
            Vector3 lift = side.CrossProduct(dir);
            lift = lift / std::max(lift.Size(), 1e-6f);

            const Vector3 offsets[6] = {
                side * radius,  side * -radius, lift * radius,
                lift * -radius, Vector3(0.0f, radius, 0.0f), Vector3(0.0f, -radius, 0.0f),
            };
            for (const Vector3& offset : offsets)
            {
                SmokeSurfaceHit hit;
                if (!CastRay(from + offset, to + offset, hit))
                {
                    continue;
                }
                // Prefer the contact whose surface is nearest the sweep START:
                // that is the wall the particle reaches first. Measured as the
                // hit point's distance from `from` along the travel axis.
                const float d = (hit.position - from).DotProduct(axisLength > 1e-4f ? axis / axisLength : axis);
                const float bd = haveBest ? (best.position - from).DotProduct(axisLength > 1e-4f ? axis / axisLength : axis)
                                          : 1e9f;
                if (!haveBest || d < bd)
                {
                    // Report the contact at the sphere CENTRE line, not at the
                    // offset ray, so the caller's plane sits where the sphere
                    // touches, not where the ring ray touched.
                    hit.position -= offset;
                    best = hit;
                    haveBest = true;
                }
            }
        }

        if (haveBest)
        {
            result = best;
        }
        return result;
    }

    float GroundHeight(float x, float z) const override
    {
        return GLandscape != nullptr ? GLandscape->SurfaceY(x, z) : 0.0f;
    }

    float FloorHeight(float x, float z) const override
    {
        // "find topmost surface on given x,z coordinates" — terrain OR a
        // roadway LOD (building floor, bridge deck) above it.
        return GLandscape != nullptr ? GLandscape->RoadSurfaceY(x, z) : 0.0f;
    }

    float FloorHeightBelow(Vector3Par pos) const override
    {
        // "find nearest surface under given point": terrain, or the roadway
        // LOD of the storey the point is on.
        return GLandscape != nullptr ? GLandscape->RoadSurfaceY(pos) : 0.0f;
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
