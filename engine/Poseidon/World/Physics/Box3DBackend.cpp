// The Box3D half. NOTHING in this file is visible to the engine: it is reached
// only through PhysicsBackend, and it is the file a move to Jolt would replace.
//
// PHY-001 measured that the library-specific part of a physics integration is
// 250-300 lines. This is that part. If it starts growing, something that belongs
// in PhysicsWorld has drifted down here.

#include <Poseidon/World/Physics/PhysicsBackend.hpp>

#include <box3d/box3d.h>

#include <Poseidon/Foundation/Framework/Log.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Poseidon::Physics
{
namespace
{

/// Rotation matrix to quaternion, Shepperd's method: pick the largest of the four
/// components to divide by, so the square root is never taken of a value near
/// zero. The naive w-first form loses precision at 180 degrees, which for a world
/// full of buildings placed at arbitrary yaw is not an edge case.
/// Box3D filters collision AND queries with the same category/mask pair, so the
/// collider flags map straight onto `categoryBits` and a query's `maskBits`. That
/// is why the filter is expressible at the seam at all: nothing here walks a hit
/// list afterwards.
///
/// The one bit that is ours rather than the engine's. A shape that exists only to
/// answer queries -- a separately authored Fire or View Geometry LOD -- must never
/// take part in contact, or a dropped crate would land on a building's line-of-
/// sight box. Box3D pairs two shapes only when EACH one's mask accepts the other's
/// category, so giving those shapes a mask of this bit alone is enough: no body
/// carries it, so no pair ever forms. Queries set it in their own category, so
/// they still see them.
constexpr std::uint64_t QueryOnlyBit = 1ull << 63;

/// What a body the physics world owns outright is, as far as a query is
/// concerned. A dropped barrel is one solid thing: it stops a shot and it blocks
/// sight, and it is emphatically not a roadway. Fixed rather than taken from the
/// pieces, because a model probe is built from the model's Geometry LOD and would
/// otherwise be invisible to a bullet-filtered ray purely because of which LOD it
/// was cut from.
constexpr ColliderFlags DynamicBodyFlags =
    ColliderFlags::Solid | ColliderFlags::BulletCollision | ColliderFlags::ViewBlocking;

b3Filter ContactFilterFor(ColliderFlags flags)
{
    b3Filter filter = b3DefaultFilter();
    filter.categoryBits = static_cast<std::uint64_t>(flags);
    // Solid means the Geometry LOD, which is the collider the world already had;
    // it keeps the mask it had, so contact behaviour is bit-for-bit what it was
    // before flags existed.
    filter.maskBits = HasAny(flags, ColliderFlags::Solid) ? ~0ull : QueryOnlyBit;
    return filter;
}

b3QueryFilter QueryFilterFor(const QueryFilter& filter)
{
    b3QueryFilter q = b3DefaultQueryFilter();
    // Everything, so the query is accepted by any shape's mask -- including the
    // query-only shapes above, whose mask is deliberately narrow.
    q.categoryBits = ~0ull;
    q.maskBits = static_cast<std::uint64_t>(filter.any);
    return q;
}

b3Quat QuatFromOrientation(const Matrix4& m)
{
    // Columns as the engine stores them: aside = X, up = Y, dir = Z.
    const Vector3 x = m.DirectionAside();
    const Vector3 y = m.DirectionUp();
    const Vector3 z = m.Direction();

    const float m00 = x[0], m01 = y[0], m02 = z[0];
    const float m10 = x[1], m11 = y[1], m12 = z[1];
    const float m20 = x[2], m21 = y[2], m22 = z[2];

    // b3Quat splits into a vector part `v` and a scalar `s` rather than x/y/z/w.
    b3Quat      q{};
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f)
    {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.s = 0.25f * s;
        q.v.x = (m21 - m12) / s;
        q.v.y = (m02 - m20) / s;
        q.v.z = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.s = (m21 - m12) / s;
        q.v.x = 0.25f * s;
        q.v.y = (m01 + m10) / s;
        q.v.z = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.s = (m02 - m20) / s;
        q.v.x = (m01 + m10) / s;
        q.v.y = 0.25f * s;
        q.v.z = (m12 + m21) / s;
    }
    else
    {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.s = (m10 - m01) / s;
        q.v.x = (m02 + m20) / s;
        q.v.y = (m12 + m21) / s;
        q.v.z = 0.25f * s;
    }
    return q;
}

b3Vec3 ToB3(Vector3Par v) { return b3Vec3{v[0], v[1], v[2]}; }

bool RigidArticulationFrame(const Matrix4& frame)
{
    if (!frame.IsFinite()) return false;
    const Vector3 x = frame.DirectionAside(), y = frame.DirectionUp(), z = frame.Direction();
    return std::abs(x.SquareSize()-1) < 1e-4f && std::abs(y.SquareSize()-1) < 1e-4f &&
        std::abs(z.SquareSize()-1) < 1e-4f && std::abs(x*y) < 1e-4f &&
        std::abs(y*z) < 1e-4f && std::abs(z*x) < 1e-4f && x*y.CrossProduct(z) > 0.9999f;
}

/// Cap on hull vertices. A Geometry LOD component is authored small -- a crate, a
/// wall segment, a wheel arch -- so this is a guard against pathological input
/// rather than a budget anyone should hit.
constexpr int MaxHullVertices = 64;

class Box3DBackend final : public PhysicsBackend
{
    struct TerrainTile
    {
        b3BodyId body = b3_nullBodyId;
        b3HeightFieldData* field = nullptr;
        std::vector<float> heights;
    };

    static void DestroyTerrainTile(TerrainTile& tile)
    {
        if (b3Body_IsValid(tile.body)) b3DestroyBody(tile.body);
        if (tile.field) b3DestroyHeightField(tile.field);
        tile = TerrainTile{};
    }

public:
    ~Box3DBackend() override { Destroy(); }

    bool Create() override
    {
        if (b3World_IsValid(_world))
        {
            return true;
        }
        b3WorldDef def = b3DefaultWorldDef();
        _world = b3CreateWorld(&def);
        return b3World_IsValid(_world);
    }

    void Destroy() override
    {
        // ORDER MATTERS, and only for the height field. b3CreateHeightFieldShape
        // carries an explicit "@warning this holds reference to the input height
        // field which must remain valid for the lifetime of this shape", so the
        // world -- which owns the shape -- has to go FIRST. This was the wrong way
        // round when the file was written.
        //
        // Hulls need no such care: b3CreateHullShape copies. Measured rather than
        // assumed, because the headers are ambiguous (mesh and height field carry
        // the reference warning, the plain hull variant does not, and the "world
        // hull database" note belongs to the TRANSFORMED variant). A cube hull was
        // built, its shape created, the source hull destroyed, thirty-two further
        // hulls allocated and freed over the same region, and a ray still hit the
        // shape afterwards while a control ray still missed. So hulls are freed at
        // creation time now and none are retained here.
        // A live recording holds a pointer INTO the world; b3World_StopRecording
        // must run while the world still exists, and the buffer must be freed
        // whether or not anybody asked for the audit. Destroying a world with a
        // recording attached and then leaking the buffer is the obvious way to get
        // this wrong, so both are handled here rather than only on the happy path.
        if (_recording != nullptr)
        {
            if (b3World_IsValid(_world))
            {
                b3World_StopRecording(_world);
            }
            b3DestroyRecording(_recording);
            _recording = nullptr;
        }

        if (b3World_IsValid(_world))
        {
            b3DestroyWorld(_world);
        }
        _world = b3_nullWorldId;

        // The world has already released every tile shape's reference.
        for (auto& tile : _terrainTiles)
            if (tile.field) b3DestroyHeightField(tile.field);
        _terrainTiles.clear();

        if (_heightField)
        {
            b3DestroyHeightField(_heightField);
            _heightField = nullptr;
        }

        // Every piece of slot state, not just the slots. Leaving _free populated
        // while _slots is emptied means the next Store() pops a stale index and
        // indexes an empty vector -- a Destroy/Create cycle away from memory
        // corruption. Same shape of lifecycle bug as the accumulator static that
        // outlived its World.
        // Probes are bodies in the world that is about to go, so the vector is
        // dropped rather than destroyed body by body.
        _probes.clear();
        _proxyValid = false;
        _proxy = b3_nullBodyId;
        _slots.clear();
        _joints.clear();
        _articulatedBodies.clear();
        _free.clear();
        _terrainBody = b3_nullBodyId;
        _terrainBodyValid = false;
        _heights.clear();
        _shapeCount = 0;
        _queryOnlyShapes = 0;
        _hullFailures = 0;
        _terrainWidth = _terrainHeight = 0;
    }

    void Step(float deltaSeconds) override
    {
        if (!b3World_IsValid(_world) || deltaSeconds <= 0.0f)
        {
            return;
        }
        // Wind first, so the force acts during the step it was set for rather than
        // the one after. Box3D computes the relative air speed itself, so nothing
        // here subtracts the body's own velocity -- doing that too would apply the
        // drag twice.
        if (_wind.enabled)
        {
            const b3Vec3 wind = ToB3(_wind.velocity);
            for (const Probe& probe : _probes)
            {
                if (b3Shape_IsValid(probe.shape))
                {
                    b3Shape_ApplyWind(probe.shape, wind, _wind.drag, _wind.lift, _wind.maxSpeed, true);
                }
            }
        }
        ApplyProbeWater(deltaSeconds);
        b3World_Step(_world, deltaSeconds, SubSteps);
    }

    void SetWaterEnvironment(const ProbeWaterEnvironment& water) override { _water = water; }

    void ApplyProbeWater(float dt)
    {
        if (!_water.sample) return;
        int emitted = 0;
        const float gravity = std::max(-b3World_GetGravity(_world).y, 0.0f);
        for (Probe& probe : _probes)
        {
            const bool firstWaterSample = !probe.waterObserved;
            probe.waterObserved = true;
            probe.waterCooldown = std::max(0.0f, probe.waterCooldown - dt);
            const auto bounds = b3Body_ComputeAABB(probe.body);
#if defined(POSEIDON_BOX3D_PINNED_954CF87)
            const auto p = b3Body_GetWorldCenter(probe.body);
#else
            const auto p = b3Body_GetWorldCenterOfMass(probe.body);
#endif
            ProbeWaterSample water;
            if (!_water.sample(Vector3(p.x, p.y, p.z), water))
            {
                probe.wet = false;
                continue;
            }
            const float height = std::max(bounds.upperBound.y - bounds.lowerBound.y, 0.001f);
            float fraction = std::clamp((water.height - bounds.lowerBound.y) / height, 0.0f, 1.0f);
            // Exact spherical-cap volume; other hulls use their oriented world
            // bounds for partial immersion, but their actual volume at full depth.
            if (probe.kind == ProbeShape::Sphere) fraction = fraction * fraction * (3.0f - 2.0f * fraction);
            if (fraction <= 0.0f)
            {
                // Surface bobbing is not a fresh impact. Rearm only after the
                // bottom has genuinely cleared the water, not at float epsilon.
                if (bounds.lowerBound.y > water.height + std::max(0.03f, height * 0.1f))
                    probe.wet = false;
                continue;
            }
            const float mass = std::max(b3Body_GetMass(probe.body), 0.001f);
            const auto v = b3Body_GetLinearVelocity(probe.body);
            const float radius = std::max(0.02f, 0.5f * std::max(bounds.upperBound.x - bounds.lowerBound.x,
                                                               bounds.upperBound.z - bounds.lowerBound.z));
            const bool entering = !probe.wet && !firstWaterSample;
            const Vector3 relative(v.x - water.velocity[0], v.y - water.velocity[1], v.z - water.velocity[2]);
            if (_water.impact && emitted < 8 && probe.waterCooldown <= 0.0f &&
                relative.Size() > 0.25f && (entering || fraction < 0.98f))
            {
                _water.impact(Vector3(p.x, water.height, p.z), relative, radius, mass, entering);
                probe.waterCooldown = entering ? 0.3f : 0.15f;
                ++emitted;
            }
            probe.wet = true;
            const float displacedMass = 1000.0f * probe.waterVolume * fraction;
            // Integrate quadratic drag implicitly so a light probe or hard throw
            // cannot reverse velocity/explode from one explicit drag step.
            Vector3 predicted = relative + Vector3(0, (displacedMass / mass - 1.0f) * gravity * dt, 0);
            const float area = 3.14159265f * radius * radius * fraction;
            predicted *= 1.0f / (1.0f + 250.0f * area * predicted.Size() * dt / mass);
            // Bound buoyancy-generated rise by the remaining immersion this tick.
            // Extremely low-density probes otherwise skip out of the surface in
            // one step. Preserve any genuine pre-existing upward launch velocity.
            predicted[1] = std::min(predicted[1], std::max(relative[1],
                std::max(0.0f, water.height - bounds.lowerBound.y) * 0.5f / dt));
            const Vector3 impulse = (predicted - relative + Vector3(0, gravity * dt, 0)) * mass;
            b3Body_ApplyLinearImpulseToCenter(probe.body, ToB3(impulse), true);
            const auto angular = b3Body_GetAngularVelocity(probe.body);
            const float damping = 1.0f / (1.0f + 3.0f * fraction * dt);
            b3Body_SetAngularVelocity(probe.body, b3Vec3{angular.x * damping, angular.y * damping, angular.z * damping});
        }
    }

    BodyId AddStaticBody(const ConvexPiece* pieces, int pieceCount, const Matrix4& transform) override
    {
        if (!b3World_IsValid(_world) || !pieces || pieceCount <= 0)
        {
            return {};
        }

        b3BodyDef bodyDef = b3DefaultBodyDef();
        bodyDef.type = b3_staticBody;
        bodyDef.position = ToB3(transform.Position());
        bodyDef.rotation = QuatFromOrientation(transform);

        const b3BodyId body = b3CreateBody(_world, &bodyDef);

        b3ShapeDef shapeDef = b3DefaultShapeDef();
        int        attached = 0;
        int        queryOnly = 0;
        for (int i = 0; i < pieceCount; ++i)
        {
            const ConvexPiece& piece = pieces[i];
            if (!piece.points || piece.count < 4)
            {
                ++_hullFailures;
                continue;
            }
            // b3Vec3 and Vector3 are both three floats but not the same type, so
            // this converts rather than reinterpret_casts. A cast here would be
            // correct today and silently wrong the day either side gains padding.
            _scratch.clear();
            _scratch.reserve(static_cast<std::size_t>(piece.count));
            for (int p = 0; p < piece.count; ++p)
            {
                _scratch.push_back(ToB3(piece.points[p]));
            }

            // Per PIECE, not per body: a model that authored a separate Fire
            // Geometry LOD hands both sets down in one call, and they must not end
            // up answering each other's questions.
            shapeDef.filter = ContactFilterFor(piece.flags);

            b3HullData* hull = b3CreateHull(_scratch.data(), static_cast<int>(_scratch.size()), MaxHullVertices);
            if (!hull)
            {
                ++_hullFailures;
                continue;
            }
            const b3ShapeId shape = b3CreateHullShape(body, &shapeDef, hull);
            // The shape owns a copy (see Destroy), so the source hull is temporary.
            // Retaining one per Geometry component would hold a second copy of every
            // collider on the map for as long as the world lives.
            b3DestroyHull(hull);

            // b3CreateHullShape can still refuse -- a hull that constructed may be
            // unusable as a collider. Counting before checking would report shapes
            // that do not exist, which is the same lie as skipping a failure.
            if (!b3Shape_IsValid(shape))
            {
                ++_hullFailures;
                continue;
            }
            ++_shapeCount;
            if (!HasAny(piece.flags, ColliderFlags::Solid))
            {
                ++_queryOnlyShapes;
                ++queryOnly;
            }
            ++attached;
        }

        if (attached == 0)
        {
            b3DestroyBody(body);
            return {};
        }

        return Store(body, static_cast<std::uint32_t>(attached), static_cast<std::uint32_t>(queryOnly));
    }

    void RemoveBody(BodyId id) override
    {
        Slot* slot = Find(id);
        if (!slot)
        {
            return;
        }
        // A DYNAMIC body is in two places -- the slot table and `_probes` -- and
        // this only ever cleared the first. `_probes` is not a debug convenience:
        // `Step` applies wind through it and `GetProbeSamples` calls
        // b3Body_GetTransform on every entry, unguarded. So an individually
        // removed probe left a dangling b3BodyId that the next readback
        // dereferences.
        //
        // Reachable, not theoretical. `LooseObjects::Release` -> `PhysicsWorld::
        // Remove` is exactly this call on a `SpawnModelProbe` body, and it runs
        // from `Thing::~Thing`, from switching the feature off, and from an object
        // ceasing to be local in multiplayer. Measured: three probes, remove the
        // middle one, and the next `GetProbeSamples()` is a SIGSEGV.
        //
        // The second failure is quieter and worse. Box3D reuses body ids, so once
        // a later body took the freed id, the stale entry ALIASED it -- and
        // `ClearProbes`, which destroys every body in `_probes`, would then delete
        // a building's collider that nothing had asked it to touch. Erasing here
        // also fixes `ClearProbes`'s double-destroy of an already-removed probe
        // and the `probes` stat, which counted removals as still present.
        // erase-remove, not std::erase_if: this library is built as C++17.
        const b3BodyId dying = slot->body;
        _articulatedBodies.erase(std::remove(_articulatedBodies.begin(),_articulatedBodies.end(),id),_articulatedBodies.end());
        // Articulated constraints are owned independently of probes. Invalidate
        // their generation BEFORE Box3D destroys attached joints with this body.
        for (JointSlot& joint : _joints)
            if (joint.generation && (joint.first == id || joint.second == id))
            {
                if (b3Joint_IsValid(joint.joint)) b3DestroyJoint(joint.joint, false);
                joint.generation = 0;
            }
        _probes.erase(std::remove_if(_probes.begin(), _probes.end(),
                                     [&](const Probe& probe) {
                                         return probe.body.index1 == dying.index1 &&
                                                probe.body.world0 == dying.world0;
                                     }),
                      _probes.end());

        b3DestroyBody(slot->body);
        slot->body = b3_nullBodyId;
        // b3DestroyBody takes the body's shapes with it, so the running total has
        // to give them back. Without this, `shapes` only counts up and any
        // streaming or mission reload inflates it past what exists.
        _shapeCount -= std::min(_shapeCount, slot->shapes);
        _queryOnlyShapes -= std::min(_queryOnlyShapes, slot->queryShapes);
        slot->shapes = 0;
        slot->queryShapes = 0;
        // Bump the generation so the caller's copy of this id stops resolving.
        // Reusing the slot without this is how a stale handle silently addresses
        // whatever was created next.
        ++slot->generation;
        _free.push_back(slot->index);
    }

    void SetBodyTransform(BodyId id, const Matrix4& transform) override
    {
        if (Slot* slot = Find(id))
        {
            b3Body_SetTransform(slot->body, ToB3(transform.Position()), QuatFromOrientation(transform));
        }
    }

    bool SetTerrain(const TerrainField& field) override
    {
        if (!b3World_IsValid(_world) || !field.heights || field.width < 2 || field.height < 2)
        {
            return false;
        }
        // Copied because b3HeightFieldDef takes a non-const pointer and we must
        // not hand the landscape's own storage to a library that may hold it.
        std::vector<float> heights(field.heights, field.heights + static_cast<std::size_t>(field.width) * field.height);

        const auto minMax = std::minmax_element(heights.begin(), heights.end());

        b3HeightFieldDef def{};
        def.heights = heights.data();
        def.materialIndices = nullptr;
        // Heights are unscaled and quantised against the global range below, so
        // the y scale stays 1 and the grid spacing goes in x and z.
        def.scale = b3Vec3{field.cellSize, 1.0f, field.cellSize};
        def.countX = field.width;
        def.countZ = field.height;
        def.globalMinimumHeight = *minMax.first;
        def.globalMaximumHeight = *minMax.second;
        def.clockwiseWinding = false;

        auto* replacement = b3CreateHeightField(&def);
        if (!replacement)
        {
            return false;
        }

        b3BodyDef bodyDef = b3DefaultBodyDef();
        bodyDef.type = b3_staticBody;
        bodyDef.position = b3Vec3{field.originX, 0.0f, field.originZ};
        const auto replacementBody = b3CreateBody(_world, &bodyDef);
        if (!b3Body_IsValid(replacementBody))
        {
            b3DestroyHeightField(replacement);
            return false;
        }

        b3ShapeDef shapeDef = b3DefaultShapeDef();
        // The ground answers EVERY question: a shot is stopped by it, sight is
        // blocked by it, and it is the roadway of last resort. Tagging it Solid
        // alone is the trap -- a bullet-filtered ray would then pass through the
        // hillside it is aimed at, which reads as "no collider there" rather than
        // as a filter mistake.
        shapeDef.filter = ContactFilterFor(ColliderFlags::All);
        const auto replacementShape = b3CreateHeightFieldShape(replacementBody, &shapeDef, replacement);
        if (!b3Shape_IsValid(replacementShape))
        {
            b3DestroyBody(replacementBody);
            b3DestroyHeightField(replacement);
            return false;
        }
        // Publish only after creation succeeded, retaining the old ground on failure.
        for (auto& tile : _terrainTiles) DestroyTerrainTile(tile);
        _terrainTiles.clear();
        if (_terrainBodyValid) b3DestroyBody(_terrainBody);
        if (_heightField) b3DestroyHeightField(_heightField);
        _terrainBody = replacementBody;
        _terrainBodyValid = true;
        _heightField = replacement;
        _heights.swap(heights);
        for (const Slot& slot : _slots)
            if (b3Body_IsValid(slot.body) && b3Body_GetType(slot.body) == b3_dynamicBody)
                b3Body_SetAwake(slot.body, true);

        _terrainWidth = static_cast<std::uint32_t>(field.width);
        _terrainHeight = static_cast<std::uint32_t>(field.height);
        _terrainCellSize = field.cellSize;
        _terrainOriginX = field.originX;
        _terrainOriginZ = field.originZ;
        return true;
    }

    bool PatchTerrain(int x, int z, int width, int height, const float* samples) override
    {
        const int columns = static_cast<int>(_terrainWidth);
        const int rows = static_cast<int>(_terrainHeight);
        if (!b3World_IsValid(_world) || !samples || columns < 2 || rows < 2 ||
            columns > 2049 || rows > 2049 || x < 0 || z < 0 || width < 1 || height < 1 ||
            width > columns || height > rows || x > columns-width || z > rows-height)
            return false;
        for (int i = 0; i < width*height; ++i)
            if (!std::isfinite(samples[i])) return false;

        constexpr int cells = 128;
        const int tileColumns = (columns-2)/cells+1;
        const int tileRows = (rows-2)/cells+1;
        const bool partition = _terrainTiles.empty();
        std::vector<TerrainTile> staged(static_cast<size_t>(tileColumns)*tileRows);
        for (int tz = 0; tz < tileRows; ++tz)
        {
            for (int tx = 0; tx < tileColumns; ++tx)
            {
                const int left = tx*cells, top = tz*cells;
                const int w = std::min(cells+1, columns-left);
                const int h = std::min(cells+1, rows-top);
                // Shared boundary vertices must rebuild both adjacent tiles.
                if (!partition && (left > x+width-1 || top > z+height-1 || left+w-1 < x || top+h-1 < z))
                    continue;
                auto& tile = staged[static_cast<size_t>(tz)*tileColumns+tx];
                tile.heights.resize(static_cast<size_t>(w)*h);
                for (int iz = 0; iz < h; ++iz)
                    for (int ix = 0; ix < w; ++ix)
                    {
                        const int gx = left+ix, gz = top+iz;
                        tile.heights[static_cast<size_t>(iz)*w+ix] =
                            gx >= x && gx < x+width && gz >= z && gz < z+height
                            ? samples[(gz-z)*width+gx-x] : _heights[static_cast<size_t>(gz)*columns+gx];
                    }
                const auto limits = std::minmax_element(tile.heights.begin(), tile.heights.end());
                b3HeightFieldDef def{};
                def.heights = tile.heights.data();
                def.scale = b3Vec3{_terrainCellSize, 1, _terrainCellSize};
                def.countX = w;
                def.countZ = h;
                def.globalMinimumHeight = *limits.first;
                def.globalMaximumHeight = *limits.second;
                def.clockwiseWinding = false;
                tile.field = b3CreateHeightField(&def);
                if (tile.field)
                {
                    auto body = b3DefaultBodyDef();
                    body.type = b3_staticBody;
                    body.position = b3Vec3{_terrainOriginX+left*_terrainCellSize, 0,
                                           _terrainOriginZ+top*_terrainCellSize};
                    tile.body = b3CreateBody(_world, &body);
                    if (b3Body_IsValid(tile.body))
                    {
                        auto shape = b3DefaultShapeDef();
                        shape.filter = ContactFilterFor(ColliderFlags::All);
                        if (b3Shape_IsValid(b3CreateHeightFieldShape(tile.body, &shape, tile.field)))
                            continue;
                    }
                }
                for (auto& candidate : staged) DestroyTerrainTile(candidate);
                return false;
            }
        }
        // Publish only once every replacement exists; failed edits retain old ground.
        if (partition)
        {
            if (_terrainBodyValid) b3DestroyBody(_terrainBody);
            if (_heightField) b3DestroyHeightField(_heightField);
            _heightField = nullptr;
            _terrainBody = b3_nullBodyId;
            _terrainBodyValid = false;
            _terrainTiles = std::move(staged);
        }
        else
            for (size_t i = 0; i < staged.size(); ++i)
                if (staged[i].field)
                {
                    DestroyTerrainTile(_terrainTiles[i]);
                    _terrainTiles[i] = std::move(staged[i]);
                }
        for (int iz = 0; iz < height; ++iz)
            std::copy_n(samples+iz*width, width, _heights.data()+static_cast<size_t>(z+iz)*columns+x);
        for (const Slot& slot : _slots)
            if (b3Body_IsValid(slot.body) && b3Body_GetType(slot.body) == b3_dynamicBody)
                b3Body_SetAwake(slot.body, true);
        return true;
    }

    BodyId SpawnSphereProbe(const SphereProbeDef& def) override
    {
        if (!b3World_IsValid(_world) || def.radius <= 0.0f)
        {
            return {};
        }

        b3BodyDef bodyDef = b3DefaultBodyDef();
        bodyDef.type = b3_dynamicBody;
        bodyDef.position = ToB3(def.position);
        // Yaw about Y, as a quaternion: (0, sin(y/2), 0, cos(y/2)).
        bodyDef.rotation = b3Quat{{0.0f, std::sin(def.yaw * 0.5f), 0.0f}, std::cos(def.yaw * 0.5f)};
        const b3BodyId body = b3CreateBody(_world, &bodyDef);

        b3ShapeDef shapeDef = b3DefaultShapeDef();
        // The dev UI speaks in kilograms because that is the unit a person has
        // intuition about; Box3D wants density, so convert through the sphere's
        // own volume rather than making the slider mean something nobody can
        // picture.
        const float volume = (4.0f / 3.0f) * 3.14159265f * def.radius * def.radius * def.radius;
        shapeDef.filter = ContactFilterFor(DynamicBodyFlags);
        shapeDef.density = volume > 0.0f ? std::max(def.mass, 0.001f) / volume : 1000.0f;
        shapeDef.baseMaterial.friction = def.friction;
        shapeDef.baseMaterial.restitution = def.restitution;
        shapeDef.baseMaterial.rollingResistance = def.rollingResistance;

        const float halfLength =
            (def.shape == ProbeShape::Capsule || def.shape == ProbeShape::Cylinder)
                ? std::max(def.halfLength, 0.001f)
                : 0.0f;
        b3ShapeId   shape;
        b3HullData* ownedHull = nullptr;
        if (def.shape == ProbeShape::Box)
        {
            // b3MakeBoxHull returns the hull BY VALUE with its arrays embedded, and
            // must not be destroyed. The shape copies (measured -- see Destroy), so
            // letting the stack value die here is correct.
            b3BoxHull box = b3MakeBoxHull(std::max(def.halfExtents[0], 0.001f),
                                          std::max(def.halfExtents[1], 0.001f),
                                          std::max(def.halfExtents[2], 0.001f));
            shape = b3CreateHullShape(body, &shapeDef, &box.base);
        }
        else if (def.shape == ProbeShape::Cylinder)
        {
            // Upright: a tower block stands on its end. 16 sides is round enough to
            // roll believably and cheap enough to stack a lot of.
            ownedHull = b3CreateCylinder(2.0f * halfLength, def.radius, 0.0f, 16);
            if (!ownedHull)
            {
                b3DestroyBody(body);
                ++_hullFailures;
                return {};
            }
            shape = b3CreateHullShape(body, &shapeDef, ownedHull);
        }
        else if (def.shape == ProbeShape::Capsule)
        {
            // Laid along the body's local X so it starts horizontal, like something
            // that has fallen over rather than a post standing on end.
            b3Capsule capsule{};
            capsule.center1 = b3Vec3{-halfLength, 0.0f, 0.0f};
            capsule.center2 = b3Vec3{halfLength, 0.0f, 0.0f};
            capsule.radius = def.radius;
            shape = b3CreateCapsuleShape(body, &shapeDef, &capsule);
        }
        else
        {
            b3Sphere sphere{};
            sphere.center = b3Vec3{0.0f, 0.0f, 0.0f};
            sphere.radius = def.radius;
            shape = b3CreateSphereShape(body, &shapeDef, &sphere);
        }
        if (ownedHull)
        {
            b3DestroyHull(ownedHull);
        }
        if (!b3Shape_IsValid(shape))
        {
            b3DestroyBody(body);
            return {};
        }

        b3Body_SetLinearVelocity(body, ToB3(def.velocity));

        // Diagnostic: ask the physics world itself what is underneath the spawn
        // point. If the answer is "nothing" while the engine says there is ground
        // there, the colliders are not where the registration thinks they are --
        // which a falling ball cannot distinguish from a stepping problem.
        {
            const b3Pos  from = ToB3(def.position);
            const b3Vec3 down{0.0f, -500.0f, 0.0f};
            const b3RayResult hit = b3World_CastRayClosest(_world, from, down, QueryFilterFor(QueryFilter{}));
            if (hit.hit)
            {
                LOG_WARN(World, "PHYSPROBE: ray down from y={:.2f} hits y={:.2f} (fraction {:.4f})",
                         def.position[1], hit.point.y, hit.fraction);
            }
            else
            {
                LOG_WARN(World, "PHYSPROBE: ray down from y={:.2f} hits NOTHING within 500 m", def.position[1]);
            }
        }

        _probes.push_back(Probe{body, shape, def.radius, halfLength, def.shape, def.halfExtents});
        _probes.back().waterVolume = b3Shape_ComputeMassData(shape).mass / b3Shape_GetDensity(shape);
        // The initial density used a sphere estimate even for boxes/capsules.
        // Make the UI's kilograms authoritative for every primitive shape.
        b3Shape_SetDensity(shape, std::max(def.mass, 0.001f) / std::max(_probes.back().waterVolume, 1e-9f), true);
        ++_shapeCount;
        return Store(body, 1);
    }

    BodyId SpawnDynamicPieces(const ConvexPiece* pieces, int pieceCount, const Matrix4& transform, float mass,
                              float friction, float restitution, Vector3Par velocity) override
    {
        if (!b3World_IsValid(_world) || !pieces || pieceCount <= 0)
        {
            return {};
        }

        b3BodyDef bodyDef = b3DefaultBodyDef();
        bodyDef.type = b3_dynamicBody;
        bodyDef.position = ToB3(transform.Position());
        bodyDef.rotation = QuatFromOrientation(transform);
        const b3BodyId body = b3CreateBody(_world, &bodyDef);

        // Density is derived from the model's own bounding volume rather than
        // asked for, so the dev UI can keep speaking in kilograms. It only has to
        // be the right order of magnitude: what matters is that a jeep is heavy
        // enough not to be flicked away by a footstep.
        Vector3 lo(0, 0, 0);
        Vector3 hi(0, 0, 0);
        bool    first = true;
        for (int i = 0; i < pieceCount; ++i)
        {
            for (int p = 0; p < pieces[i].count; ++p)
            {
                const Vector3& v = pieces[i].points[p];
                if (first)
                {
                    lo = hi = v;
                    first = false;
                    continue;
                }
                for (int a = 0; a < 3; ++a)
                {
                    lo[a] = std::min(lo[a], v[a]);
                    hi[a] = std::max(hi[a], v[a]);
                }
            }
        }
        const float volume = std::max((hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]), 0.001f);

        b3ShapeDef shapeDef = b3DefaultShapeDef();
        shapeDef.filter = ContactFilterFor(DynamicBodyFlags);
        shapeDef.density = std::max(mass, 0.1f) / volume;
        shapeDef.baseMaterial.friction = friction;
        shapeDef.baseMaterial.restitution = restitution;

        int attached = 0;
        for (int i = 0; i < pieceCount; ++i)
        {
            if (!pieces[i].points || pieces[i].count < 4)
            {
                ++_hullFailures;
                continue;
            }
            _scratch.clear();
            _scratch.reserve(static_cast<std::size_t>(pieces[i].count));
            for (int p = 0; p < pieces[i].count; ++p)
            {
                _scratch.push_back(ToB3(pieces[i].points[p]));
            }
            b3HullData* hull = b3CreateHull(_scratch.data(), static_cast<int>(_scratch.size()), MaxHullVertices);
            if (!hull)
            {
                ++_hullFailures;
                continue;
            }
            const b3ShapeId shape = b3CreateHullShape(body, &shapeDef, hull);
            b3DestroyHull(hull);
            if (!b3Shape_IsValid(shape))
            {
                ++_hullFailures;
                continue;
            }
            ++_shapeCount;
            ++attached;
        }

        if (attached == 0)
        {
            b3DestroyBody(body);
            return {};
        }

        // Density above came from the BOUNDING BOX, which overstates the volume of
        // anything that is not a brick -- a jeep would have come out several times
        // too light. Box3D has already computed the true compound mass properties
        // from the hulls, so rescale them to the mass that was actually asked for
        // and keep the centre of mass and inertia it derived. Verified separately
        // that those properties are volume-weighted and correct.
        const float actual = b3Body_GetMass(body);
        if (actual > 0.0f)
        {
            b3MassData   data = b3Body_GetMassData(body);
            const float  scale = std::max(mass, 0.1f) / actual;
            data.mass *= scale;
            // b3Matrix3 is three COLUMN VECTORS (cx, cy, cz), not a packed
            // xx/xy/... run. Inertia is linear in mass, so every element scales.
            const b3Vec3* in = &data.inertia.cx;
            b3Vec3*       out = &data.inertia.cx;
            for (int c = 0; c < 3; ++c)
            {
                out[c] = b3Vec3{in[c].x * scale, in[c].y * scale, in[c].z * scale};
            }
            b3Body_SetMassData(body, data);
        }

        b3Body_SetLinearVelocity(body, ToB3(velocity));

        Probe probe;
        probe.body = body;
        probe.kind = ProbeShape::Model;
        std::vector<b3ShapeId> waterShapes(b3Body_GetShapeCount(body));
        const int waterShapeCount = b3Body_GetShapes(body, waterShapes.data(), static_cast<int>(waterShapes.size()));
        for (int i = 0; i < waterShapeCount; ++i)
        {
            const float density = b3Shape_GetDensity(waterShapes[i]);
            if (density > 0.0f) probe.waterVolume += b3Shape_ComputeMassData(waterShapes[i]).mass / density;
        }
        _probes.push_back(probe);
        return Store(body, static_cast<std::uint32_t>(attached));
    }

    void ApplyImpulse(BodyId id, Vector3Par point, Vector3Par impulse) override
    {
        if (Slot* slot = Find(id))
        {
            b3Body_ApplyLinearImpulse(slot->body, ToB3(impulse), ToB3(point), true);
        }
    }

    BodyId SpawnArticulatedPiece(const ConvexPiece& piece, const Matrix4& frame, float mass,
        const ArticulatedInitialMotion& motion = {}) override
    {
        if (!RigidArticulationFrame(frame) || !std::isfinite(mass) || mass <= 0 ||
            !motion.linear.IsFinite() || !motion.angular.IsFinite() ||
            motion.collisionFamily >= 0 || motion.collisionFamily < -32767 ||
            !piece.points || piece.count < 4 || piece.count > MaxHullVertices) return {};
        for (int p = 0; p < piece.count; ++p) if (!piece.points[p].IsFinite()) return {};
        BodyId id = SpawnDynamicPieces(&piece, 1, frame, mass, 0.65f, 0, motion.linear);
        Slot* slot = Find(id);
        if (!slot) return {};
        const b3BodyId body = slot->body;
        b3Body_SetAngularVelocity(body,ToB3(motion.angular));
        // Passive rotational energy loss belongs only to articulated limbs.
        // The upstream default is zero damping. Rotating contact/limit loops
        // otherwise persist after the supported torso stops. Preserve initial
        // velocities and immediate impact momentum; no linear damping is added,
        // so blast translation and gravity retain their ballistic trajectory.
        b3Body_SetAngularDamping(body,1.0f);
        // A corpse is not a probe: no probe clear, wind or water controller owns
        // it. Only this bounded corpse's owner destroys these articulated bodies.
        _probes.erase(std::remove_if(_probes.begin(), _probes.end(), [&](const Probe& probe)
        { return probe.body.index1 == body.index1 && probe.body.world0 == body.world0; }), _probes.end());
        std::vector<b3ShapeId> shapes(b3Body_GetShapeCount(body));
        const int count = b3Body_GetShapes(body, shapes.data(), static_cast<int>(shapes.size()));
        for (int i = 0; i < count; ++i)
        {
            b3Filter filter = b3Shape_GetFilter(shapes[i]);
            filter.groupIndex = motion.collisionFamily;
            b3Shape_SetFilter(shapes[i], filter, true);
        }
        _articulatedBodies.push_back(id);
        return id;
    }

    bool SetArticulatedMotion(BodyId id, Vector3Par linear, Vector3Par angular) override
    {
        if (!linear.IsFinite() || !angular.IsFinite()) return false;
        if (std::none_of(_articulatedBodies.begin(),_articulatedBodies.end(),[&](BodyId body) { return body == id; }))
            return false;
        Slot* slot = Find(id);
        if (!slot) return false;
        b3Body_SetLinearVelocity(slot->body,ToB3(linear));
        b3Body_SetAngularVelocity(slot->body,ToB3(angular));
        b3Body_SetAwake(slot->body,true);
        return true;
    }

    JointId AddArticulatedJoint(const ArticulatedJointDef& definition) override
    {
        Slot* first = Find(definition.first); Slot* second = Find(definition.second);
        if (!first || !second || definition.first == definition.second ||
            !RigidArticulationFrame(definition.firstFrame) || !RigidArticulationFrame(definition.secondFrame))
            return {};
        b3JointDef base = b3DefaultSphericalJointDef().base;
        base.bodyIdA = first->body; base.bodyIdB = second->body;
        base.localFrameA = {ToB3(definition.firstFrame.Position()), QuatFromOrientation(definition.firstFrame)};
        base.localFrameB = {ToB3(definition.secondFrame.Position()), QuatFromOrientation(definition.secondFrame)};
        base.collideConnected = false;
        b3JointId joint = b3_nullJointId;
        if (definition.kind == ArticulatedJointKind::Hinge)
        {
            constexpr float limit = 0.99f * 3.14159265358979323846f;
            if (!std::isfinite(definition.lowerAngle) || !std::isfinite(definition.upperAngle) ||
                definition.lowerAngle < -limit || definition.upperAngle > limit ||
                definition.lowerAngle >= definition.upperAngle) return {};
            b3RevoluteJointDef def = b3DefaultRevoluteJointDef();
            def.base = base; def.enableLimit = true;
            def.lowerAngle = definition.lowerAngle; def.upperAngle = definition.upperAngle;
            // Passive body: targetAngle is a spring target, not a limit offset.
            def.enableSpring = false; def.enableMotor = false;
            joint = b3CreateRevoluteJoint(_world, &def);
        }
        else if (definition.kind == ArticulatedJointKind::Spherical)
        {
            if (!std::isfinite(definition.coneAngle) || definition.coneAngle <= 0 || definition.coneAngle > 3.1415926f ||
                !std::isfinite(definition.twistAngle) || definition.twistAngle <= 0 || definition.twistAngle > 3.0f) return {};
            b3SphericalJointDef def = b3DefaultSphericalJointDef();
            def.base = base; def.enableConeLimit = true; def.coneAngle = definition.coneAngle;
            def.enableTwistLimit = true;
            def.lowerTwistAngle = -definition.twistAngle; def.upperTwistAngle = definition.twistAngle;
            joint = b3CreateSphericalJoint(_world, &def);
        }
        else return {};
        if (!b3Joint_IsValid(joint)) return {};
        if (++_nextJointGeneration == 0) ++_nextJointGeneration;
        std::uint32_t index = 0;
        while (index < _joints.size() && _joints[index].generation) ++index;
        JointSlot slot{joint, definition.first, definition.second, _nextJointGeneration};
        if (index == _joints.size()) _joints.push_back(slot); else _joints[index] = slot;
        return {index, slot.generation};
    }

    void RemoveJoint(JointId id) override
    {
        if (!id.IsValid() || id.index >= _joints.size()) return;
        JointSlot& slot = _joints[id.index];
        if (slot.generation != id.generation) return;
        if (b3Joint_IsValid(slot.joint)) b3DestroyJoint(slot.joint, false);
        slot.generation = 0;
    }

    BodyId CastRay(Vector3Par from, Vector3Par to, Vector3& hitPoint, const QueryFilter& filter) const override
    {
        if (!b3World_IsValid(_world))
        {
            return {};
        }
        const b3Vec3      translation{to[0] - from[0], to[1] - from[1], to[2] - from[2]};
        const b3RayResult hit = b3World_CastRayClosest(_world, ToB3(from), translation, QueryFilterFor(filter));
        if (!hit.hit)
        {
            return {};
        }
        hitPoint = Vector3(hit.point.x, hit.point.y, hit.point.z);

        // Map the hit body back to a handle. Linear over the slot table: probe
        // counts are small, and a map would have to be kept in step with every
        // create and destroy for no measurable gain.
        const b3BodyId body = b3Shape_GetBody(hit.shapeId);
        for (const Slot& slot : _slots)
        {
            // NO PARITY TEST. This used to also require `slot.generation % 2 != 0`,
            // reading the generation counter as a live/dead flag: slots start at 1
            // and `RemoveBody` increments, so a freed slot is even. But `Store`
            // REUSES a freed slot without incrementing again, so the body that
            // moves in keeps the even generation -- and this was the only place
            // that treated even as dead. The effect: after any removal, the next
            // body to take that slot was hit by the ray and then reported as "no
            // body", every time, for as long as it lived. `Find()` never agreed
            // with this reading; it compares the generation exactly, which is what
            // a generation counter is for.
            //
            // Liveness is already implied by the match itself. `RemoveBody` and
            // `ClearProbes` both set `slot.body = b3_nullBodyId`, whose `index1`
            // is 0, and a body Box3D just handed back from a ray hit never is.
            if (slot.body.index1 == body.index1 && slot.body.world0 == body.world0)
            {
                return BodyId{slot.index, slot.generation};
            }
        }
        return {};
    }

    bool RayHitAnything(Vector3Par from, Vector3Par to, const QueryFilter& filter) const override
    {
        if (!b3World_IsValid(_world))
        {
            return false;
        }
        const b3Vec3 translation{to[0] - from[0], to[1] - from[1], to[2] - from[2]};
        return b3World_CastRayClosest(_world, ToB3(from), translation, QueryFilterFor(filter)).hit;
    }

    void ClearProbes() override
    {
        for (const Probe& probe : _probes)
        {
            // Drop the slot entry too, or the body count keeps counting probes
            // that no longer exist.
            for (Slot& slot : _slots)
            {
                if (slot.body.index1 == probe.body.index1 && slot.body.world0 == probe.body.world0)
                {
                    _shapeCount -= std::min(_shapeCount, slot.shapes);
                    _queryOnlyShapes -= std::min(_queryOnlyShapes, slot.queryShapes);
                    slot.shapes = 0;
                    slot.queryShapes = 0;
                    slot.body = b3_nullBodyId;
                    ++slot.generation;
                    _free.push_back(slot.index);
                    break;
                }
            }
            b3DestroyBody(probe.body);
        }
        _probes.clear();
    }

    void GetProbeSamples(std::vector<ProbeSample>& out) const override
    {
        out.reserve(_probes.size());
        for (const Probe& probe : _probes)
        {
            const b3WorldTransform t = b3Body_GetTransform(probe.body);
            ProbeSample            sample;
            sample.shape = probe.kind;
            for (const Slot& slot : _slots)
            {
                if (slot.body.index1 == probe.body.index1 && slot.body.world0 == probe.body.world0)
                {
                    sample.id = BodyId{slot.index, slot.generation};
                    break;
                }
            }
            sample.position = Vector3(t.p.x, t.p.y, t.p.z);
            sample.radius = probe.radius;
            sample.halfLength = probe.halfLength;
            sample.halfExtents = probe.halfExtents;
            const b3Vec3 ax = b3RotateVector(t.q, b3Vec3{1.0f, 0.0f, 0.0f});
            const b3Vec3 ay = b3RotateVector(t.q, b3Vec3{0.0f, 1.0f, 0.0f});
            const b3Vec3 az = b3RotateVector(t.q, b3Vec3{0.0f, 0.0f, 1.0f});
            sample.axisX = Vector3(ax.x, ax.y, ax.z);
            sample.axisY = Vector3(ay.x, ay.y, ay.z);
            sample.axisZ = Vector3(az.x, az.y, az.z);
            sample.axis = probe.kind == ProbeShape::Capsule ? sample.axisX * probe.halfLength : Vector3(0, 0, 0);
            out.push_back(sample);
        }
    }

    bool GetBodyMotion(BodyId id, BodyMotion& out) const override
    {
        // Find() is non-const because it hands back a mutable slot for the write
        // paths. Reading through a const_cast here is narrower than making the
        // whole lookup const-correct for one caller, and the alternative -- a
        // second near-identical finder -- is the kind of duplication that drifts.
        const Slot* slot = const_cast<Box3DBackend*>(this)->Find(id);
        if (!slot)
        {
            return false;
        }
        const b3WorldTransform t = b3Body_GetTransform(slot->body);
        out.position = Vector3(t.p.x, t.p.y, t.p.z);
        const b3Vec3 ax = b3RotateVector(t.q, b3Vec3{1.0f, 0.0f, 0.0f});
        const b3Vec3 ay = b3RotateVector(t.q, b3Vec3{0.0f, 1.0f, 0.0f});
        const b3Vec3 az = b3RotateVector(t.q, b3Vec3{0.0f, 0.0f, 1.0f});
        out.axisX = Vector3(ax.x, ax.y, ax.z);
        out.axisY = Vector3(ay.x, ay.y, ay.z);
        out.axisZ = Vector3(az.x, az.y, az.z);
        const b3Vec3 lv = b3Body_GetLinearVelocity(slot->body);
        const b3Vec3 av = b3Body_GetAngularVelocity(slot->body);
        out.linearVelocity = Vector3(lv.x, lv.y, lv.z);
        out.angularVelocity = Vector3(av.x, av.y, av.z);
        out.awake = b3Body_IsAwake(slot->body);
        return true;
    }

    bool GetArticulatedGroundSupport(BodyId id, Vector3& point) const override
    {
#if !defined(POSEIDON_BOX3D_PINNED_954CF87)
        (void)id; (void)point;
        return false; // No certified contact witness on a different dependency.
#else
        const Slot* slot = const_cast<Box3DBackend*>(this)->Find(id);
        if (!slot || std::find(_articulatedBodies.begin(),_articulatedBodies.end(),id) == _articulatedBodies.end()) return false;
        constexpr int Capacity = 64;
        if (b3Body_GetContactCapacity(slot->body) > Capacity) return false;
        b3ContactData contacts[Capacity];
        const int count = b3Body_GetContactData(slot->body,contacts,Capacity);
        for (int i = 0; i < count; ++i)
        {
            const auto& contact = contacts[i];
            const bool first = B3_ID_EQUALS(b3Shape_GetBody(contact.shapeIdA),slot->body);
            const b3ShapeId other = first ? contact.shapeIdB : contact.shapeIdA;
            if (!b3Shape_IsValid(other) || b3Body_GetType(b3Shape_GetBody(other)) != b3_staticBody ||
                !(b3Shape_GetFilter(other).categoryBits & static_cast<std::uint64_t>(ColliderFlags::Solid))) continue;
            const auto center = b3Body_GetWorldCenter(slot->body);
            for (int m = 0; m < contact.manifoldCount; ++m)
            {
                const auto& manifold = contact.manifolds[m];
                const float up = first ? -manifold.normal.y : manifold.normal.y;
                if (!std::isfinite(up) || up <= .5f) continue;
                for (int p = 0; p < manifold.pointCount; ++p)
                {
                    const auto& support = manifold.points[p];
                    if (!std::isfinite(support.separation) || support.separation > .015f) continue;
                    const b3Vec3 anchor = first ? support.anchorA : support.anchorB;
                    point = Vector3(center.x+anchor.x,center.y+anchor.y,center.z+anchor.z);
                    if (point.IsFinite()) return true;
                }
            }
        }
        return false;
#endif
    }

    void SetWind(const WindSettings& wind) override { _wind = wind; }

    void SetKinematicProxy(Vector3Par position, float radius, float height) override
    {
        if (!b3World_IsValid(_world))
        {
            return;
        }
        if (height <= 0.0f)
        {
            if (_proxyValid)
            {
                b3DestroyBody(_proxy);
                _proxyValid = false;
            }
            return;
        }
        if (!_proxyValid)
        {
            b3BodyDef def = b3DefaultBodyDef();
            // Kinematic: moved by us, never by the solver. A dynamic proxy would
            // be shoved around by whatever it touched and would need the result
            // written back into the player -- exactly the ownership step this
            // avoids.
            def.type = b3_kinematicBody;
            def.position = ToB3(position);
            _proxy = b3CreateBody(_world, &def);

            const float half = std::max(height * 0.5f - radius, 0.01f);
            b3Capsule   capsule{};
            capsule.center1 = b3Vec3{0.0f, -half, 0.0f};
            capsule.center2 = b3Vec3{0.0f, half, 0.0f};
            capsule.radius = radius;

            b3ShapeDef shapeDef = b3DefaultShapeDef();
            shapeDef.filter = ContactFilterFor(DynamicBodyFlags);
            shapeDef.baseMaterial.friction = 0.8f;
            b3CreateCapsuleShape(_proxy, &shapeDef, &capsule);
            _proxyValid = true;
        }
        // SetTransform rather than a velocity: the player's motion comes from the
        // 2001 simulation and is not ours to model. Box3D resolves the sweep.
        b3Body_SetTransform(_proxy, ToB3(position), b3Quat{{0.0f, 0.0f, 0.0f}, 1.0f});
    }

    [[nodiscard]] PhysicsStats GetStats() const override
    {
        PhysicsStats stats;
        stats.bodies = static_cast<std::uint32_t>(_slots.size() - _free.size());
        stats.shapes = _shapeCount;
        stats.queryOnlyShapes = _queryOnlyShapes;
        stats.hullFailures = _hullFailures;
        stats.probes = static_cast<std::uint32_t>(_probes.size());
        stats.articulatedBodies = static_cast<std::uint32_t>(_articulatedBodies.size());
        if (b3World_IsValid(_world))
        {
            const auto counters = b3World_GetCounters(_world);
            stats.solverBodies = static_cast<std::uint32_t>(counters.bodyCount);
            stats.solverJoints = static_cast<std::uint32_t>(counters.jointCount);
        }
        stats.kinematicProxyRegistered = _proxyValid;
        for (const JointSlot& joint : _joints)
            if (joint.generation && b3Joint_IsValid(joint.joint)) ++stats.articulatedJoints;
        stats.terrainRegistered = _heightField != nullptr || !_terrainTiles.empty();
        stats.terrainWidth = _terrainWidth;
        stats.terrainHeight = _terrainHeight;
        stats.terrainCellSize = stats.terrainRegistered ? _terrainCellSize : 0;
        stats.terrainOriginX = stats.terrainRegistered ? _terrainOriginX : 0;
        stats.terrainOriginZ = stats.terrainRegistered ? _terrainOriginZ : 0;
        // Asked of the LIVE world rather than of b3DefaultWorldDef, because the
        // question 8.4 cares about is what this world will actually do inside a
        // step -- and b3World_SetWorkerCount can change that after creation. A
        // value above 1 means Box3D creates its own threads and its own internal
        // scheduler (types.h: "Otherwise Box3D will create threads and use an
        // internal scheduler"), which would be a thread pool the engine neither
        // owns nor FP-conforms.
        stats.workerCount =
            b3World_IsValid(_world) ? static_cast<std::uint32_t>(b3World_GetWorkerCount(_world)) : 0u;
        return stats;
    }

    [[nodiscard]] const char* Name() const override
    {
#if defined(POSEIDON_BOX3D_PINNED_954CF87)
        return "box3d 0.1.0+954cf87";
#else
        return "box3d 0.1.0";
#endif
    }

    // ---- box3d's own record/replay. SIM-808. ------------------------------
    //
    // box3d records every API call made against a world, seeded by a full world
    // snapshot, and stamps a StateHash op after every b3World_Step -- FNV-1a over
    // the transform and the linear/angular velocity of every live body, bitwise.
    // Replaying the stream into a fresh world re-issues the same calls and
    // recomputes that hash per step, so a mismatch names the FRAME.
    //
    // What makes it worth wiring at all, given the engine already compares its own
    // readbacks: the replay world can be built at a DIFFERENT worker count.
    // box3d's header says replaying at another count "re-partitions the constraint
    // graph", so this reaches a configuration the engine cannot otherwise reach --
    // SIM-804 measured our live worker count as 1, which means the multi-worker
    // solver has no coverage from any test that steps the live world.
    //
    // What it can NOT see, and why it is a second witness and not the oracle: the
    // engine's own calls are BAKED INTO the recording. Replay reproduces box3d
    // given inputs the engine already chose, so a divergence upstream of
    // SetBodyTransform -- in AI, in scripting, in an event order -- is invisible
    // here by construction. That is the engine timeline's job.

    bool BeginReplayRecording() override
    {
        if (!b3World_IsValid(_world) || _recording != nullptr)
        {
            return false;
        }
        // A hint, not a cap: b3RecBufAppend doubles on overflow. 1 MiB covers a
        // few hundred steps of this size of world without a reallocation.
        _recording = b3CreateRecording(1 << 20);
        if (_recording == nullptr)
        {
            return false;
        }
        b3World_StartRecording(_world, _recording);
        return true;
    }

    ReplayAudit EndReplayRecordingAndValidate(int replayWorkerCount) override
    {
        ReplayAudit audit;
        audit.supported = true;
        if (_recording == nullptr)
        {
            return audit;
        }

        if (b3World_IsValid(_world))
        {
            b3World_StopRecording(_world);
        }

        const uint8_t* data = b3Recording_GetData(_recording);
        const int      size = b3Recording_GetSize(_recording);
        audit.recordedBytes = size;

        const int workers = std::clamp(replayWorkerCount, 1, B3_MAX_WORKERS);
        audit.replayWorkerCount = workers;

        if (data != nullptr && size > 0)
        {
            // b3ValidateReplay would answer this with a bool. The player is used
            // instead for one reason: it exposes the diverge FRAME, which is the
            // whole point of the roadmap item. A bool would tell us no more than
            // the whole-run hash SIM-804 already has.
#if defined(POSEIDON_BOX3D_PINNED_954CF87)
            if (b3RecPlayer* player = b3CreatePlayer(data, size, workers))
#else
            if (b3RecPlayer* player = b3RecPlayer_Create(data, size, workers))
#endif
            {
                while (b3RecPlayer_StepFrame(player))
                {
                    if (b3RecPlayer_HasDiverged(player))
                    {
                        break;
                    }
                }
                audit.ran = true;
                audit.diverged = b3RecPlayer_HasDiverged(player);
                audit.divergentFrame = b3RecPlayer_GetDivergeFrame(player);
                audit.frameCount = b3RecPlayer_GetFrame(player);
#if defined(POSEIDON_BOX3D_PINNED_954CF87)
                b3DestroyPlayer(player);
#else
                b3RecPlayer_Destroy(player);
#endif
            }
        }

        b3DestroyRecording(_recording);
        _recording = nullptr;
        return audit;
    }

private:
    /// Sub-steps per call. Box3D's solver takes these internally; four is its
    /// documented default territory and PHY-010 has no reason to tune it.
    static constexpr int SubSteps = 4;

    struct Slot
    {
        b3BodyId      body = b3_nullBodyId;
        std::uint32_t generation = 1;
        std::uint32_t index = 0;
        /// Shapes this body owns. Kept per body because b3DestroyBody takes them
        /// with it silently, and a total that only ever counts up stops being a
        /// diagnostic the moment anything is streamed out.
        std::uint32_t shapes = 0;
        /// Of `shapes`, the query-only ones. Same reason as `shapes`: a running
        /// total that is never given back is not a measurement.
        std::uint32_t queryShapes = 0;
    };

    BodyId Store(b3BodyId body, std::uint32_t shapes, std::uint32_t queryShapes = 0)
    {
        if (!_free.empty())
        {
            const std::uint32_t index = _free.back();
            _free.pop_back();
            Slot& slot = _slots[index];
            slot.body = body;
            slot.shapes = shapes;
            slot.queryShapes = queryShapes;
            return BodyId{index, slot.generation};
        }
        Slot slot;
        slot.body = body;
        slot.shapes = shapes;
        slot.queryShapes = queryShapes;
        slot.index = static_cast<std::uint32_t>(_slots.size());
        _slots.push_back(slot);
        return BodyId{slot.index, slot.generation};
    }

    Slot* Find(BodyId id)
    {
        if (!id.IsValid() || id.index >= _slots.size())
        {
            return nullptr;
        }
        Slot& slot = _slots[id.index];
        return slot.generation == id.generation ? &slot : nullptr;
    }

    b3WorldId                _world = b3_nullWorldId;
    /// Non-null only between BeginReplayRecording and EndReplayRecordingAndValidate.
    /// A test throws this switch; a frame never does.
    b3Recording* _recording = nullptr;
    std::vector<Slot>        _slots;
    std::vector<std::uint32_t> _free;
    std::vector<b3Vec3>      _scratch;

    b3BodyId _proxy = b3_nullBodyId;
    bool     _proxyValid = false;

    b3HeightFieldData* _heightField = nullptr;
    b3BodyId           _terrainBody = b3_nullBodyId;
    bool               _terrainBodyValid = false;
    std::vector<float> _heights;
    std::vector<TerrainTile> _terrainTiles;
    float _terrainCellSize = 1, _terrainOriginX = 0, _terrainOriginZ = 0;

    /// PHY-020 probes, kept apart from the static world so ClearProbes cannot
    /// take a building with it and the debug draw never has to guess which body
    /// is a probe.
    struct Probe
    {
        b3BodyId  body = b3_nullBodyId;
        b3ShapeId shape = b3_nullShapeId;
        float     radius = 0.0f;
        /// 0 for a sphere.
        float halfLength = 0.0f;
        /// `kind`, not `shape`: the b3ShapeId above already owns that name, and a
        /// second member called shape reads as the same thing.
        ProbeShape kind = ProbeShape::Sphere;
        Vector3    halfExtents{0, 0, 0};
        float waterVolume = 0.0f;
        float waterCooldown = 0.0f;
        bool wet = false;
        bool waterObserved = false;
    };
    std::vector<Probe> _probes;
    struct JointSlot
    {
        b3JointId joint;
        BodyId first, second;
        std::uint32_t generation;
    };
    std::vector<JointSlot> _joints;
    std::vector<BodyId> _articulatedBodies;
    std::uint32_t _nextJointGeneration = 0;
    WindSettings       _wind;
    ProbeWaterEnvironment _water;

    std::uint32_t _shapeCount = 0;
    std::uint32_t _queryOnlyShapes = 0;
    std::uint32_t _hullFailures = 0;
    std::uint32_t _terrainWidth = 0;
    std::uint32_t _terrainHeight = 0;
};

} // namespace

std::unique_ptr<PhysicsBackend> CreatePhysicsBackend() { return std::make_unique<Box3DBackend>(); }

} // namespace Poseidon::Physics
