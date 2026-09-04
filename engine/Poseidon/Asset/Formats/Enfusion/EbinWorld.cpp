#include <Poseidon/Asset/Formats/Enfusion/EbinWorld.hpp>

#include <cmath>
#include <cstddef>
#include <cstdio>

namespace Poseidon::Asset::Formats::Enfusion
{
namespace
{

using EbinDetail::Cursor;
using EbinDetail::GuidToText;

//! The declaration accumulated ahead of a value or block: a class or property
//! name, an optional instance name, and an optional prefab reference.
struct Declaration
{
    std::string name;   //!< the last identifier seen -- the property or class name
    std::string prefab; //!< the GUID this entity declares as its type (tag 0x44 / 0x45)
    //! The GUID after a `:` in the declaration -- `Class : {prefabGUID} { ... }`.
    //!
    //! Kept apart from `prefab` because the two are not the same field and the
    //! declared type wins when both are present, which is the rule the textual form
    //! shows: `SCR_IndestructibleEnvironmentalEntity : "{29EA...}Granite_...02.et"`.
    std::string parent;

    //! What a placement here is an instance of.
    const std::string& Prefab() const { return prefab.empty() ? parent : prefab; }
};

class Reader
{
  public:
    Reader(Cursor cursor, EbinWorld& world) : _c(cursor), _world(world) {}

    //! Walks a record stream to `end`. `inherited` carries the enclosing $grp's
    //! class and prefab down, which is what makes a grouped instance resolvable.
    bool Records(size_t end, const Declaration& inherited, int depth)
    {
        if (depth > 64)
            return Refuse("record nesting deeper than 64");

        Declaration decl;
        while (_c.at < end)
        {
            if (!_c.Has(1))
                return Refuse("record stream ran past the buffer");
            const uint8_t tag = _c.U8();

            switch (tag)
            {
                case 0x0D: // identifier
                {
                    if (!_c.Has(2))
                        return Refuse("identifier index truncated");
                    const uint16_t index = _c.U16();
                    if (index >= _world.names.size())
                        return Refuse("identifier index out of range");
                    const std::string& name = _world.names[index];
                    if (name != ":")
                    {
                        decl.name = name;
                        continue;
                    }

                    // `:` is a decl token whose OPERAND is an ordinary value -- almost
                    // always tag 0x05, an interned GUID -- and that operand is the
                    // prefab reference. Reading `:` as a bare token and letting the
                    // operand fall through to the value handler ends the record: the
                    // declaration is cleared before the entity block that follows ever
                    // sees it. That is what left all 1,230,897 of Everon's placements
                    // with an empty prefabGuid while byte accounting still closed on
                    // 103 / 103 files -- consuming the right number of bytes is not the
                    // same as attributing them to the right field.
                    if (!_c.Has(1))
                        return Refuse("':' has no operand");
                    const uint8_t operand = _c.data[_c.at];
                    _c.at += 1;
                    if (operand == 0x05)
                    {
                        if (!_c.Has(2))
                            return Refuse("':' GUID index truncated");
                        const uint16_t guidIndex = _c.U16();
                        if (guidIndex >= _world.guids.size())
                            return Refuse("':' GUID index out of range");
                        if (decl.parent.empty())
                            decl.parent = _world.guids[guidIndex];
                        continue;
                    }
                    // Any other operand is walked for byte accounting and contributes
                    // no prefab reference. The declaration is deliberately NOT reset:
                    // the class name in front of the colon belongs to the record that
                    // follows.
                    if (!Value(operand, end, decl, inherited, depth))
                        return false;
                    continue;
                }
                case 0x43: // instance name
                {
                    if (!_c.Has(2))
                        return Refuse("instance name truncated");
                    const uint16_t length = _c.U16();
                    if (!_c.Has(length))
                        return Refuse("instance name runs past the buffer");
                    _c.Bytes(length);
                    continue;
                }
                case 0x44: // inline GUID
                {
                    if (!_c.Has(8))
                        return Refuse("inline GUID truncated");
                    decl.prefab = GuidToText(_c.data + _c.at);
                    _c.at += 8;
                    continue;
                }
                case 0x45: // interned GUID
                {
                    if (!_c.Has(2))
                        return Refuse("GUID index truncated");
                    const uint16_t index = _c.U16();
                    if (index >= _world.guids.size())
                        return Refuse("GUID index out of range");
                    decl.prefab = _world.guids[index];
                    continue;
                }
                default:
                    break;
            }

            if (!Value(tag, end, decl, inherited, depth))
                return false;
            decl = Declaration{};
        }
        return _c.at == end ? true : Refuse("record stream overran its block");
    }

    bool Done() const { return _c.at == _c.size; }
    size_t Position() const { return _c.at; }

  private:
    // Named Refuse rather than Fail: the Foundation PCH defines a Fail() logging
    // macro, and a method with that name expands into it.
    bool Refuse(const char* why)
    {
        char message[128];
        std::snprintf(message, sizeof(message), "%s at offset %zu", why, _c.at);
        _world.error = message;
        return false;
    }

    bool Value(uint8_t tag, size_t end, const Declaration& decl, const Declaration& inherited, int depth)
    {
        if ((tag & 0x80u) != 0)
            return Array(static_cast<uint8_t>(tag & 0x7Fu), decl, inherited, depth);

        switch (tag)
        {
            case 0x01: // i32
                return Skip(4);
            case 0x02: // two u32 -- EIGHT bytes; four was the census's decisive error
                // Only its PRESENCE is kept, and only on the entity that declares it:
                // it is what tells an importer whether `coords` Y is absolute or an
                // offset above the terrain. See EbinPlacement::hasFlags for the
                // measurement that establishes this.
                if (decl.name == "Flags")
                    if (EbinPlacement* current = Current())
                        current->hasFlags = true;
                return Skip(8);
            case 0x05: // interned GUID as a value
            {
                if (!_c.Has(2))
                    return Refuse("interned GUID truncated");
                const uint16_t index = _c.U16();
                if (decl.name == "Material" && _roadIndex != kNoPlacement && index < _world.guids.size())
                    _world.placements[_roadIndex].materialGuid = _world.guids[index];
                return true;
            }
            case 0x06: // f32
            {
                if (!_c.Has(4))
                    return Refuse("f32 truncated");
                const float value = _c.F32();
                if (decl.name == "scale")
                    if (EbinPlacement* current = Current())
                        current->scale = value;
                if (decl.name == "Width" && _roadIndex != kNoPlacement)
                    _world.placements[_roadIndex].width = value;
                return true;
            }
            case 0x07: // f32[2]
                return Skip(8);
            case 0x08: // f32[3]
            {
                if (!_c.Has(12))
                    return Refuse("f32[3] truncated");
                float v[3];
                v[0] = _c.F32();
                v[1] = _c.F32();
                v[2] = _c.F32();
                if (EbinPlacement* current = Current())
                {
                    if (decl.name == "coords")
                    {
                        current->position[0] = v[0];
                        current->position[1] = v[1];
                        current->position[2] = v[2];
                        current->hasPosition = true;
                    }
                    else if (decl.name == "angles")
                    {
                        current->angles[0] = v[0];
                        current->angles[1] = v[1];
                        current->angles[2] = v[2];
                    }
                }
                // RFG-027: a road's shape. Measured structure, not a guess:
                //
                //   RoadEntity { coords, SplinePoints { ShapePoint { Position } ... },
                //                IsClosedSpline, Type, Width, Material, VScale }
                //
                // Kept against `_roadIndex` and not `Current()` because a `ShapePoint`
                // is itself an entity block: by the time its `Position` is read, the
                // current entity is the point, not the road. Those point entities are
                // then dropped by the no-`coords` rule below, which is why 2,396 road
                // entities looked shapeless and why the class histogram names no point
                // class at all.
                if (decl.name == "Position" && _roadIndex != kNoPlacement)
                {
                    EbinPlacement& road = _world.placements[_roadIndex];
                    road.points.push_back(v[0]);
                    road.points.push_back(v[1]);
                    road.points.push_back(v[2]);
                }
                return true;
            }
            case 0x09: // f32[4]
                return Skip(16);
            case 0x0B: // bool
            {
                if (!_c.Has(1))
                    return Refuse("bool truncated");
                const uint8_t value = _c.U8();
                if (decl.name == "IsClosedSpline" && _roadIndex != kNoPlacement)
                    _world.placements[_roadIndex].closedSpline = value != 0;
                return true;
            }
            case 0x0C: // string
            {
                if (!_c.Has(2))
                    return Refuse("string length truncated");
                const uint16_t length = _c.U16();
                return Skip(length);
            }
            case 0x0E:
                return Block(decl, inherited, depth);
            // Never observed in the corpus; the widths are unverified guesses and a
            // file that uses one should say so rather than silently mis-walk.
            case 0x03:
            case 0x04:
            case 0x0A:
            case 0x0F:
                return Refuse("value tag never seen in the measured corpus");
            default:
                return Refuse("unknown value tag");
        }
    }

    bool Array(uint8_t elementTag, const Declaration& decl, const Declaration& inherited, int depth)
    {
        if (!_c.Has(4))
            return Refuse("array count truncated");
        const uint32_t count = _c.U32();

        for (uint32_t i = 0; i < count; ++i)
            if (!Value(elementTag, _c.size, decl, inherited, depth))
                return false;

        return true;
    }

    bool Block(const Declaration& decl, const Declaration& inherited, int depth)
    {
        if (!_c.Has(4))
            return Refuse("block size truncated");
        const size_t sizeAt = _c.at;
        const uint32_t declared = _c.U32();
        if (declared < 4)
            return Refuse("block declares fewer than its own four size bytes");
        const size_t end = sizeAt + declared; // the size includes itself
        if (end > _c.size)
            return Refuse("block runs past the buffer");
        _world.blocks++;

        if (_c.at >= end)
            return true; // an empty block
        const uint8_t first = _c.data[_c.at];

        if (first == 0x00)
        {
            // An entity block: three flag bytes, then a record stream. A placement
            // takes its class and prefab from its own declaration where it has
            // them and from the enclosing $grp where it does not.
            _world.entityBlocks++;
            if (!_c.Has(3))
                return Refuse("entity block header truncated");
            _c.at += 3;

            EbinPlacement placement;
            placement.className = decl.name.empty() ? inherited.name : decl.name;
            const bool isRoad = IsRoadSplineClass(placement.className);
            placement.prefabGuid = decl.Prefab().empty() ? inherited.Prefab() : decl.Prefab();

            // A nested entity's transform is LOCAL to the entity that contains it,
            // so the enclosing one is captured BY VALUE here and composed onto the
            // child below. Reading a child's `coords` as world coordinates is what
            // stranded 4,186 of Everon's placements -- 671 of them within 30 m of
            // the map ORIGIN, including 128 houses and the church at 5153/3989 that
            // came out at (0, -114.2, 0). It is only 0.34% of the world, and it is
            // the 0.34% a player notices, because whole buildings go missing rather
            // than shifting slightly.
            //
            // Safe to read the parent here: measured over Everon, all 4,186 nested
            // children have their parent's `coords` record already consumed by the
            // time their block opens, so no second pass is needed.
            const size_t outerIndex = _currentIndex;
            bool parentPositioned = false;
            float parentPos[3] = {0.0f, 0.0f, 0.0f};
            float parentYawDeg = 0.0f;
            float parentScale = 1.0f;
            if (outerIndex != kNoPlacement)
            {
                const EbinPlacement& parent = _world.placements[outerIndex];
                parentPositioned = parent.hasPosition;
                parentPos[0] = parent.position[0];
                parentPos[1] = parent.position[1];
                parentPos[2] = parent.position[2];
                parentYawDeg = parent.angles[1];
                parentScale = parent.scale;
            }

            _world.placements.push_back(placement);
            const size_t index = _world.placements.size() - 1;
            _currentIndex = index;
            const size_t outerRoad = _roadIndex;
            const size_t subtreeStart = _world.placements.size();
            if (isRoad)
                _roadIndex = index;

            Declaration pass;
            pass.name = placement.className;
            pass.prefab = placement.prefabGuid;
            const bool ok = Records(end, pass, depth + 1);

            // push_back may have reallocated, so the entry is addressed by index.
            // RFG-027: a spline entity that declares no `coords` of its own sits AT
            // its parent, not at the world origin. That only started to matter once
            // roads stopped being erased for lacking a position -- before this, the
            // six longest roads on Everon came out at coordinates like [-1013, -561],
            // which is not merely wrong but off the island.
            if (parentPositioned && !_world.placements[index].hasPosition && !_world.placements[index].points.empty())
                _world.placements[index].hasPosition = true;
            if (parentPositioned && _world.placements[index].hasPosition)
            {
                EbinPlacement& child = _world.placements[index];
                // Yaw about Y, matching the basis the .wrp writer builds from the
                // same angle: world = x*aside + y*up + z*dir with aside=(c,0,-s)
                // and dir=(s,0,c). Getting the sign wrong here does not fail
                // loudly -- it scatters children in a ring around their parent.
                const float yaw = parentYawDeg * 0.01745329252f;
                const float c = std::cos(yaw);
                const float s = std::sin(yaw);
                const float lx = child.position[0] * parentScale;
                const float ly = child.position[1] * parentScale;
                const float lz = child.position[2] * parentScale;
                child.position[0] = parentPos[0] + c * lx + s * lz;
                child.position[1] = parentPos[1] + ly;
                child.position[2] = parentPos[2] - s * lx + c * lz;
                child.angles[1] += parentYawDeg;
                child.scale *= parentScale;
            }
            if (!_world.placements[index].hasPosition && _world.placements[index].points.empty())
                _world.placements.erase(_world.placements.begin() + static_cast<ptrdiff_t>(index));
            // Restoring an INDEX, not a pointer: the push_back above may have
            // reallocated the vector. `outerIndex` is always < index because the
            // parent was pushed first, so the erase cannot have moved it.
            _currentIndex = outerIndex;

            // RFG-027: a road's surface and width are declared on the OUTER
            // `RoadGeneratorEntity`, and the `RoadEntity` children that carry the
            // spline usually restate neither. Propagating on the way out rather than
            // inheriting on the way in is forced by the record order: `Material` and
            // `Width` come AFTER `SplinePoints`, so at the moment a child is read the
            // parent has not met them yet. Measured on Everon this is the difference
            // between 171 and 2,370 roads knowing their own tarmac.
            //
            // Indices are stable here and only here: the subtree is finished, so every
            // erase it was going to do has happened.
            if (isRoad && index < _world.placements.size())
            {
                const std::string material = _world.placements[index].materialGuid;
                const float width = _world.placements[index].width;
                for (size_t i = subtreeStart; i < _world.placements.size(); ++i)
                {
                    EbinPlacement& child = _world.placements[i];
                    if (child.points.empty())
                        continue;
                    if (child.materialGuid.empty())
                        child.materialGuid = material;
                    if (child.width <= 0.0f)
                        child.width = width;
                }
            }
            _roadIndex = outerRoad;
            return ok;
        }
        if (first == 0x0D || first == 0x43 || first == 0x44 || first == 0x45 || first == 0x0E)
            return Records(end, decl.name.empty() ? inherited : decl, depth + 1);
        if ((first & 0x80u) != 0)
        {
            const uint8_t elementTag = static_cast<uint8_t>(_c.U8() & 0x7Fu);
            if (!Array(elementTag, decl, inherited, depth))
                return false;
            _c.at = end;
            return true;
        }
        // An opaque payload -- every one in the corpus is a world's ASCII BSP tree.
        _world.opaqueBlobs++;
        _c.at = end;
        return true;
    }

    bool Skip(size_t bytes)
    {
        if (!_c.Has(bytes))
            return Refuse("value runs past the buffer");
        _c.at += bytes;
        return true;
    }

    Cursor _c;
    EbinWorld& _world;
    //! Index of the entity currently being read, or kNoPlacement outside one.
    //!
    //! An INDEX and not a pointer: `_world.placements` is a vector that grows while
    //! the walk recurses, so any pointer into it dies at the next push_back. The
    //! child-entity path already addressed its own entry by index for that reason;
    //! the enclosing entity was still being restored through a raw pointer that the
    //! same reallocation could dangle.
    static constexpr size_t kNoPlacement = static_cast<size_t>(-1);
    size_t _currentIndex = kNoPlacement;
    //! The road entity whose subtree is being walked, or kNoPlacement outside one.
    size_t _roadIndex = kNoPlacement;

    EbinPlacement* Current() { return _currentIndex == kNoPlacement ? nullptr : &_world.placements[_currentIndex]; }
};

} // namespace

EbinWorld ReadEbin(const void* data, size_t size)
{
    EbinWorld world;
    if (!data || size < 12)
    {
        world.error = "buffer shorter than an EBIN header";
        return world;
    }
    Cursor c{static_cast<const uint8_t*>(data), size, 0};
    if (std::memcmp(c.data, "EBIN", 4) != 0)
    {
        world.error = "not an EBIN container";
        return world;
    }
    c.at = 4;
    world.version = c.U32();
    const uint16_t nameCount = c.U16();
    const uint16_t guidCount = c.U16();

    world.names.reserve(nameCount);
    for (uint16_t i = 0; i < nameCount; ++i)
    {
        if (!c.Has(2))
        {
            world.error = "name table truncated";
            return world;
        }
        const uint16_t length = c.U16();
        if (!c.Has(length))
        {
            world.error = "name runs past the buffer";
            return world;
        }
        world.names.push_back(c.Bytes(length));
    }
    world.guids.reserve(guidCount);
    for (uint16_t i = 0; i < guidCount; ++i)
    {
        if (!c.Has(8))
        {
            world.error = "GUID table truncated";
            return world;
        }
        world.guids.push_back(GuidToText(c.data + c.at));
        c.at += 8;
    }

    Reader reader(c, world);
    Declaration root;
    if (!reader.Records(size, root, 0))
        return world;
    world.consumed = reader.Position();
    if (world.consumed != size)
        world.error = "record stream consumed " + std::to_string(world.consumed) + " of " + std::to_string(size);
    return world;
}

} // namespace Poseidon::Asset::Formats::Enfusion
