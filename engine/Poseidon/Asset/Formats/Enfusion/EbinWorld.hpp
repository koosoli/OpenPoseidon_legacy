#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Enfusion's `EBIN` container -- the binarised form of the same brace syntax the
// `.emat`/`.et`/`.conf` reader already parses, and what Arma Reforger's worlds
// (`.ent`) are stored in.
//
//   "EBIN"                            magic
//   u32   version                     1 across the whole corpus
//   u16   nameCount
//   u16   guidCount
//   nameCount x (u16 len, len bytes)  interned class AND property names, in order
//                                     of first appearance; ":" is interned as an
//                                     ordinary name
//   guidCount x 8 bytes               interned asset ids, LITTLE-endian -- the
//                                     textual {7037BF6589EEFD5D} form is these
//                                     bytes reversed and hex-uppercased
//   <record stream>                   to EOF, exactly
//
// A record is a declaration -- a run of decl tokens -- followed by a value or a
// block:
//
//   0x0D u16 nameIdx     identifier: a class name, a property name, or ":"
//   0x43 u16 len + bytes instance name
//   0x44 8 bytes         inline GUID (the declared type / prefab)
//   0x45 u16 guidIdx     the same, interned
//
// Value tags, with their occurrence counts over the 103-file corpus:
//
//   0x01 i32 [18,347]        0x02 two u32 flags, EIGHT bytes [2,201,432]
//   0x05 u16 guidIdx [39,138]  0x06 f32 [2,651,199]
//   0x07 f32[2] [6]          0x08 f32[3] [5,808,811]
//   0x09 f32[4] [247]        0x0B u8 bool [17,885]
//   0x0C u16 len + bytes [32,132]
//   0x0E sized block         0x80|T array (u32 count, then elements of tag T)
//
// `0x02` being eight bytes and not four is the field that decided the census: with
// it read as four, 44 of 100 files closed. It is corroborated by the textual form,
// where the same property reads `Flags 0 0x3` -- two values, not one.
//
// A `0x0E` block's u32 size is SELF-INCLUSIVE of its own four size bytes, so the
// payload is `size - 4`. That too was forced by arithmetic rather than guessed.
// The one non-self-describing point in the format is what a block's payload is,
// and it is decided by its first byte:
//
//   0x00                          an entity block: three flag bytes (zero in all
//                                 3,012,727 of them) then a record stream
//   0x0D/0x43/0x44/0x45/0x0E      a bare record stream (component lists, children)
//   0x80|T                        an array value
//   anything else                 an opaque blob (19 in the corpus, every one of
//                                 them a world's ASCII `BSP` partition tree)
//
// Byte accounting: 103/103 files, 168,860,053 / 168,860,053 bytes, no gaps and no
// overlaps, from 189 B to 84 MB.
//
// `$grp` is the structural trap. It reads `$grp <ClassName> : {prefabGUID} { ... }`
// and is a GROUPING node, not a transform level -- it exists so that N instances of
// one prefab can share a single class name and prefab reference. Treating it as a
// transform, or failing to inherit its prefab reference down to its children, is
// what makes the prefab look "missing" on the instances.
//
// Tags 0x03, 0x04, 0x0A and 0x0F are accepted with guessed widths and were NEVER
// observed in the corpus. They are unverified and marked so at the point of use.

namespace Poseidon::Asset::Formats::Enfusion
{

//! One placed object: what it is, and where.
struct EbinPlacement
{
    std::string className;                  //!< the entity's class, or its group's
    std::string prefabGuid;                 //!< uppercase hex, as Enfusion writes it in {braces}
    float position[3] = {0.0f, 0.0f, 0.0f}; //!< metres; index 1 is up
    float angles[3] = {0.0f, 0.0f, 0.0f};   //!< degrees; index 1 is YAW
    float scale = 1.0f;
    bool hasPosition = false;
    //! The entity block carried a `Flags` property of its own.
    //!
    //! It is recorded because it is the only field in the file that separates an
    //! ABSOLUTE `coords` Y from one stored as an offset above the terrain. Measured
    //! over Everon's 1,228,065 in-bounds placements against the assembled
    //! heightfield:
    //!
    //!   Flags present  872,185  96.3% sit within 0.5 m of the terrain, median Y 54.99
    //!   Flags absent   355,880  15.4%                                 median Y  0.00
    //!
    //! `Tree` alone splits 97.9% / 7.4% on the same test, so this is not a property
    //! of one entity class. `SCR_DestructibleBuildingEntity` is 93% flagless, which
    //! is why taking every Y as absolute buries Everon's houses a median 29 m under
    //! the ground while its vegetation, rocks, walls and props all look correct.
    //!
    //! The reader states the fact and nothing more; the rule that turns it into a
    //! height lives with the exporter that has a terrain to resolve against
    //! (`IsTerrainRelative` in XobCommand.cpp).
    bool hasFlags = false;

    //! RFG-027: the shape of a spline entity, in the entity's OWN local frame.
    //!
    //! Everon's roads are not models. There is no baked road geometry anywhere in
    //! the archives -- `pak list -f road` returns shaders, one generator script and
    //! a handful of materials, and nothing else -- because Enfusion builds the strip
    //! at load time from `RoadGeneratorEntity` (2,207 of them on Everon), `RoadEntity`
    //! (189) and `SplineShapeEntity` (73).
    //!
    //! The points are `SplinePoints { ShapePoint { Position } ... }`, read from an
    //! instrumented walk rather than guessed. They are LOCAL to `coords` where the
    //! road declares one and world coordinates where it does not -- a road is usually
    //! declared at the origin, so both readings agree in practice, but the caller
    //! composes rather than assuming.
    std::vector<float> points;  //!< x,y,z triples
    float width = 0.0f;         //!< `Width`, metres; 0 when the road leaves it default
    bool closedSpline = false;  //!< `IsClosedSpline`
    std::string materialGuid;   //!< `Material`, resolvable through the resource database
};

//! Whether a class name is one whose f32[3] arrays are a spline centreline.
//!
//! A closed set rather than a substring test: `PowerlineEntity` and `RiverEntity`
//! are splines too, and sweeping them in here would draw power cables as tarmac.
inline bool IsRoadSplineClass(const std::string& className)
{
    return className == "RoadGeneratorEntity" || className == "RoadEntity";
}

struct EbinWorld
{
    uint32_t version = 0;
    std::vector<std::string> names;
    std::vector<std::string> guids; //!< uppercase hex
    std::vector<EbinPlacement> placements;
    size_t blocks = 0;
    size_t entityBlocks = 0;
    size_t opaqueBlobs = 0;
    size_t consumed = 0; //!< bytes the walk accounted for; must equal the file size
    std::string error;

    bool valid() const { return error.empty(); }
    bool closes() const { return error.empty(); }
};

namespace EbinDetail
{

struct Cursor
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t at = 0;

    bool Has(size_t bytes) const { return at + bytes <= size; }
    uint8_t U8() { return data[at++]; }
    uint16_t U16()
    {
        uint16_t v = 0;
        std::memcpy(&v, data + at, 2);
        at += 2;
        return v;
    }
    uint32_t U32()
    {
        uint32_t v = 0;
        std::memcpy(&v, data + at, 4);
        at += 4;
        return v;
    }
    float F32()
    {
        float v = 0.0f;
        std::memcpy(&v, data + at, 4);
        at += 4;
        return v;
    }
    std::string Bytes(size_t n)
    {
        std::string s(reinterpret_cast<const char*>(data + at), n);
        at += n;
        return s;
    }
};

inline std::string GuidToText(const uint8_t* eight)
{
    // Little-endian: the textual form is these bytes reversed.
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(16);
    for (int i = 7; i >= 0; --i)
    {
        out.push_back(kHex[(eight[i] >> 4) & 0xF]);
        out.push_back(kHex[eight[i] & 0xF]);
    }
    return out;
}

} // namespace EbinDetail

//! Reads an `EBIN` file and collects its placements.
//!
//! Only the transform properties are materialised; everything else is walked for
//! byte accounting and discarded. That keeps a 70 MB world from becoming a
//! multi-million-node tree in memory when the caller wants a scene, not an editor.
EbinWorld ReadEbin(const void* data, size_t size);

} // namespace Poseidon::Asset::Formats::Enfusion
