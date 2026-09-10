#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>

#include <Poseidon/World/Effects/BuildingInterior.hpp>
#include <Poseidon/World/Entities/Vehicles/House.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Scene/Object.hpp>

#include <cmath>
#include <cstdlib>
#include <array>

namespace Poseidon
{
bool SmokeCachedBuildingContains(const BuildingInterior* interior, const Object* building, Vector3Par position)
{
    return interior != nullptr && interior->Valid() && building != nullptr && !building->IsDestroyed() &&
           interior->RoomOfWorld(building, position) >= 0;
}

namespace
{

long long GSweepCount = 0;

/// The Building containing `pos`, or null, using the fire/view shell room grid.
/// The last answer is cached in an OLink, which clears itself if the building
/// is deleted: smoke asks about points that barely move between frames, and a
/// full IsInside scan per query would cost what the room lookup was meant to
/// save. Different particles can query adjacent buildings even when neither
/// moved: proximity alone is not evidence that the cached room owns the point.
Object* BuildingContaining(Vector3Par pos)
{
    static OLink<Object> cached;
    static Vector3 cachedPos(VZero);

    Object* obj = cached.GetLink();
    if (obj != nullptr && (pos - cachedPos).SquareSize() < Square(12.0f))
    {
        const Building* building = dyn_cast<const Building>(obj);
        const BuildingInterior* interior = building ? BuildingInterior::GetFor(building->GetBType()) : nullptr;
        if (SmokeCachedBuildingContains(interior, obj, pos))
        {
            return obj;
        }
    }

    if (GLandscape == nullptr)
    {
        return nullptr;
    }

    Object* found = nullptr;
    // IsInside tests solid convex geometry, not the air in a hollow room.
    // Query the room grid directly, as the validated cached path already does.
    // Keep the old search extent/order and reject non-buildings first.
    static const bool filterBuildings = [] {
        const char* value = std::getenv("POSEIDON_SMOKE_BUILDING_FILTER");
        return !(value && value[0]=='0');
    }();
    if (filterBuildings)
    {
        int xMin,xMax,zMin,zMax;
        // Segment overload already adds the legacy 25m object-radius margin.
        ::ObjRadiusRectangle(xMin,xMax,zMin,zMax,pos,pos,0.0f);
        for (int x=xMin; x<=xMax && !found; ++x)
            for (int z=zMin; z<=zMax && !found; ++z)
            {
                const ObjectList& objects=GLandscape->GetObjects(z,x);
                for (int i=0; i<objects.Size(); ++i)
                {
                    Object* candidate=objects[i];
                    const Building* building=dyn_cast<const Building>(candidate);
                    if (!building || building->IsDestroyed() || !building->GetShape())
                        continue;
                    // All-LOD sphere plus room-grid padding: avoid voxelizing
                    // nearby houses which cannot contain this point at all.
                    const float radius=building->GetRadius()+2.0f*building->Scale();
                    if ((pos-building->Position()).SquareSize()>Square(radius))
                        continue;
                    const BuildingInterior* interior=BuildingInterior::GetFor(building->GetBType());
                    if (SmokeCachedBuildingContains(interior,candidate,pos))
                    {
                        found=candidate;
                        break;
                    }
                }
            }
        cached=found;
        cachedPos=pos;
        return found;
    }
    for (int pass = 0; pass < 2 && found == nullptr; ++pass)
    {
        StaticArrayAuto<OLink<Object>> objects;
        GLandscape->IsInside(objects, nullptr, pos, pass == 0 ? ObjIntersectFire : ObjIntersectView);
        for (int i = 0; i < objects.Size(); ++i)
        {
            if (dyn_cast<Building>(objects[i].GetLink()) != nullptr)
            {
                found = objects[i].GetLink();
                break;
            }
        }
    }

    cached = found;
    cachedPos = pos;
    return found;
}

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
    struct SweepCandidate { Object* object; int x, z; };
  public:
    /// One zero-width ray through the world, returning the deepest hit converted
    /// to world space with a real outward normal. Factored out because
    /// SweepSphere below fires several of these: Object::Intersect(beg, end,
    /// radius, ...) IGNORES ITS RADIUS ARGUMENT — the bounding checks test the
    /// line's distance and the clip loop clips the segment; `radius` is never
    /// read. So the "sphere sweep" the interface promises has to be built here
    /// from rays, or a puff's centre has to cross a wall before anything hits.
    bool CastRay(Vector3Par from, Vector3Par to, SmokeSurfaceHit& out,
                 const SweepCandidate* candidates = nullptr, int candidateCount = 0) const
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
        if (!candidates)
            GLandscape->ObjectCollision(buffer, nullptr, nullptr, from, to, 0.0f, ObjIntersectFire);
        else
        {
            int xMin, xMax, zMin, zMax;
            ::ObjRadiusRectangle(xMin, xMax, zMin, zMax, from, to, 0.0f);
            const Vector3 centre = (to + from) * 0.5f;
            const float reach = to.Distance(from) * 0.5f;
            for (int i = 0; i < candidateCount; ++i)
            {
                const auto& candidate = candidates[i];
                if (candidate.x < xMin || candidate.x > xMax || candidate.z < zMin || candidate.z > zMax)
                    continue;
                Object* object = candidate.object;
                if (object->Position().Distance2Inline(centre) > Square(reach + object->GetRadius()))
                    continue;
                object->Intersect(buffer, from, to, 0.0f, ObjIntersectFire);
            }
            static const bool verify = std::getenv("POSEIDON_SMOKE_BATCH_VERIFY") != nullptr;
            if (verify)
            {
                CollisionBuffer reference;
                GLandscape->ObjectCollision(reference, nullptr, nullptr, from, to, 0.0f, ObjIntersectFire);
                bool equal = reference.Size() == buffer.Size();
                for (int i = 0; equal && i < buffer.Size(); ++i)
                    equal = reference[i].object == buffer[i].object &&
                        reference[i].under == buffer[i].under && reference[i].pos == buffer[i].pos &&
                        reference[i].surfaceNormal == buffer[i].surfaceNormal;
                if (!equal) LOG_ERROR(World, "Smoke batch parity mismatch: {} vs {}", buffer.Size(), reference.Size());
                static unsigned checked = 0;
                if (++checked == 1000) LOG_INFO(World, "Smoke batch parity: 1000 exact ray comparisons completed");
            }
        }

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

        // The seven offset rays share one conservative broad phase. In open
        // air none can hit: avoid seven identical walks of the object grid.
        static const bool broadPhase = []
        {
            const char* v = std::getenv("POSEIDON_SMOKE_BROADPHASE");
            return !(v && v[0] == '0');
        }();
        std::array<SweepCandidate, 32> candidates;
        int candidateCount = 0;
        bool overflow = false;
        if (broadPhase)
        {
            int xMin, xMax, zMin, zMax;
            ::ObjRadiusRectangle(xMin, xMax, zMin, zMax, from, to, radius);
            const Vector3 centre = (from + to) * 0.5f;
            const float reach = (to - from).Size() * 0.5f + std::max(radius, 0.0f);
            // Preserve ObjectCollision's cell/list order, including equal-distance hits.
            for (int x = xMin; x <= xMax && !overflow; ++x)
            {
                for (int z = zMin; z <= zMax && !overflow; ++z)
                {
                    const ObjectList& objects = GLandscape->GetObjects(z, x);
                    for (int i = 0; i < objects.Size(); ++i)
                    {
                        Object* object = objects[i];
                        if (object->GetType() == Temporary || object->GetType() == TypeTempVehicle)
                            continue;
                        if (object->GetType() == Network && object->GetShape()->FindGeometryLevel() < 0)
                            continue;
                        if ((object->Position() - centre).SquareSize() <= Square(reach + object->GetRadius()))
                        {
                            if (candidateCount == static_cast<int>(candidates.size()))
                            {
                                overflow = true;
                                break;
                            }
                            candidates[candidateCount++] = {object, x, z};
                        }
                    }
                }
            }
            if (candidateCount == 0)
                return result;
        }
        // No per-particle heap allocation. Dense overlap falls back without dropping hits.
        const SweepCandidate* batch = broadPhase && !overflow ? candidates.data() : nullptr;

        // Centre ray first: it is the cheapest and, for a particle heading
        // straight at a wall, the one that hits.
        SmokeSurfaceHit best;
        bool haveBest = CastRay(from, to, best, batch, candidateCount);

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
                if (!CastRay(from + offset, to + offset, hit, batch, candidateCount))
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

    bool ProbeSegment(Vector3Par from, Vector3Par to, Vector3& hit) const override
    {
        if (GLandscape == nullptr)
        {
            return false;
        }

        // The same thin fire-geometry ray IsSheltered fires, aimed along the
        // caller's own path instead of straight up. CastRay already converts
        // the deepest hit to world space, so a rain drop that probes its next
        // few metres of fall learns not just THAT a surface is coming but
        // where -- which is what lets it splash on the roof it would otherwise
        // have fallen through.
        SmokeSurfaceHit contact;
        if (!CastRay(from, to, contact))
        {
            return false;
        }
        hit = contact.position;
        return true;
    }

    bool IndoorRoomAt(Vector3Par pos, int& room) const override
    {
        room = -1;
        const Object* obj = BuildingContaining(pos);
        if (obj == nullptr)
        {
            return false;
        }
        const Building* building = dyn_cast<const Building>(obj);
        const BuildingInterior* interior = BuildingInterior::GetFor(building->GetBType());
        if (interior == nullptr || !interior->Valid())
        {
            return false;
        }
        room = interior->RoomOfWorld(obj, pos);
        return room >= 0;
    }

    float IndoorClearanceAt(Vector3Par pos, int room, float maxDistance) const override
    {
        const Object* obj = BuildingContaining(pos);
        const Building* building = dyn_cast<const Building>(obj);
        if (building == nullptr)
        {
            return maxDistance;
        }
        const BuildingInterior* interior = BuildingInterior::GetFor(building->GetBType());
        if (interior == nullptr || !interior->Valid() || interior->RoomOfWorld(obj, pos) != room)
        {
            return maxDistance;
        }
        return interior->ClearanceModel(obj->GetInvTransform().FastTransform(pos), room, maxDistance);
    }

    SmokeContainStep IndoorContainStep(Vector3Par from, Vector3Par to, int room) const override
    {
        SmokeContainStep result;
        result.position = to;
        result.room = room;

        // The source owns this step. Its endpoint may already lie beyond a
        // solid wall, where looking up a building would lose containment.
        const Object* obj = BuildingContaining(from);
        if (obj == nullptr)
        {
            return result;
        }
        const Building* building = dyn_cast<const Building>(obj);
        const BuildingInterior* interior = BuildingInterior::GetFor(building->GetBType());
        if (interior == nullptr || !interior->Valid())
        {
            return result;
        }

        const InteriorStepResult step = interior->ContainedStepWorld(obj, from, to, room);
        result.allowed = !step.blocked;
        result.position = step.position;
        result.normal = step.normal;
        result.room = step.room;
        return result;
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
