// The Box3D half. NOTHING in this file is visible to the engine: it is reached
// only through PhysicsBackend, and it is the file a move to Jolt would replace.
//
// PHY-001 measured that the library-specific part of a physics integration is
// 250-300 lines. This is that part. If it starts growing, something that belongs
// in PhysicsWorld has drifted down here.

#include <Poseidon/World/Physics/PhysicsBackend.hpp>

#include <box3d/box3d.h>

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

/// Cap on hull vertices. A Geometry LOD component is authored small -- a crate, a
/// wall segment, a wheel arch -- so this is a guard against pathological input
/// rather than a budget anyone should hit.
constexpr int MaxHullVertices = 64;

class Box3DBackend final : public PhysicsBackend
{
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
        if (b3World_IsValid(_world))
        {
            b3DestroyWorld(_world);
        }
        _world = b3_nullWorldId;

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
        _slots.clear();
        _free.clear();
        _terrainBody = b3_nullBodyId;
        _terrainBodyValid = false;
        _heights.clear();
        _shapeCount = 0;
        _hullFailures = 0;
        _terrainWidth = _terrainHeight = 0;
    }

    void Step(float deltaSeconds) override
    {
        if (b3World_IsValid(_world) && deltaSeconds > 0.0f)
        {
            b3World_Step(_world, deltaSeconds, SubSteps);
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
            ++attached;
        }

        if (attached == 0)
        {
            b3DestroyBody(body);
            return {};
        }

        return Store(body, static_cast<std::uint32_t>(attached));
    }

    void RemoveBody(BodyId id) override
    {
        Slot* slot = Find(id);
        if (!slot)
        {
            return;
        }
        b3DestroyBody(slot->body);
        slot->body = b3_nullBodyId;
        // b3DestroyBody takes the body's shapes with it, so the running total has
        // to give them back. Without this, `shapes` only counts up and any
        // streaming or mission reload inflates it past what exists.
        _shapeCount -= std::min(_shapeCount, slot->shapes);
        slot->shapes = 0;
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
        if (_heightField)
        {
            // One terrain per world. Replacing it means the old body goes too.
            if (_terrainBodyValid)
            {
                b3DestroyBody(_terrainBody);
                _terrainBodyValid = false;
            }
            b3DestroyHeightField(_heightField);
            _heightField = nullptr;
        }

        // Copied because b3HeightFieldDef takes a non-const pointer and we must
        // not hand the landscape's own storage to a library that may hold it.
        _heights.assign(field.heights, field.heights + static_cast<std::size_t>(field.width) * field.height);

        const auto minMax = std::minmax_element(_heights.begin(), _heights.end());

        b3HeightFieldDef def{};
        def.heights = _heights.data();
        def.materialIndices = nullptr;
        // Heights are unscaled and quantised against the global range below, so
        // the y scale stays 1 and the grid spacing goes in x and z.
        def.scale = b3Vec3{field.cellSize, 1.0f, field.cellSize};
        def.countX = field.width;
        def.countZ = field.height;
        def.globalMinimumHeight = *minMax.first;
        def.globalMaximumHeight = *minMax.second;
        def.clockwiseWinding = false;

        _heightField = b3CreateHeightField(&def);
        if (!_heightField)
        {
            return false;
        }

        b3BodyDef bodyDef = b3DefaultBodyDef();
        bodyDef.type = b3_staticBody;
        bodyDef.position = b3Vec3{field.originX, 0.0f, field.originZ};
        _terrainBody = b3CreateBody(_world, &bodyDef);
        _terrainBodyValid = true;

        b3ShapeDef shapeDef = b3DefaultShapeDef();
        b3CreateHeightFieldShape(_terrainBody, &shapeDef, _heightField);

        _terrainWidth = static_cast<std::uint32_t>(field.width);
        _terrainHeight = static_cast<std::uint32_t>(field.height);
        return true;
    }

    [[nodiscard]] PhysicsStats GetStats() const override
    {
        PhysicsStats stats;
        stats.bodies = static_cast<std::uint32_t>(_slots.size() - _free.size());
        stats.shapes = _shapeCount;
        stats.hullFailures = _hullFailures;
        stats.terrainRegistered = _heightField != nullptr;
        stats.terrainWidth = _terrainWidth;
        stats.terrainHeight = _terrainHeight;
        return stats;
    }

    [[nodiscard]] const char* Name() const override { return "box3d 0.1.0"; }

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
    };

    BodyId Store(b3BodyId body, std::uint32_t shapes)
    {
        if (!_free.empty())
        {
            const std::uint32_t index = _free.back();
            _free.pop_back();
            Slot& slot = _slots[index];
            slot.body = body;
            slot.shapes = shapes;
            return BodyId{index, slot.generation};
        }
        Slot slot;
        slot.body = body;
        slot.shapes = shapes;
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
    std::vector<Slot>        _slots;
    std::vector<std::uint32_t> _free;
    std::vector<b3Vec3>      _scratch;

    b3HeightFieldData* _heightField = nullptr;
    b3BodyId           _terrainBody = b3_nullBodyId;
    bool               _terrainBodyValid = false;
    std::vector<float> _heights;

    std::uint32_t _shapeCount = 0;
    std::uint32_t _hullFailures = 0;
    std::uint32_t _terrainWidth = 0;
    std::uint32_t _terrainHeight = 0;
};

} // namespace

std::unique_ptr<PhysicsBackend> CreatePhysicsBackend() { return std::make_unique<Box3DBackend>(); }

} // namespace Poseidon::Physics
