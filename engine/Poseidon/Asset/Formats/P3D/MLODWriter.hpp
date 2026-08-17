#pragma once

#include <Poseidon/Asset/Formats/BISStructures.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Poseidon::Asset::Formats
{
namespace MLOD
{

// The P3DM half of the MLOD container, on the write side.
//
// Only P3DM is emitted. SP3X is the OFP/CWA encoding and AST-007's inventory of
// the owner-provided corpora found 2 SP3X files against 1,657 P3DM ones -- both
// of the two are camera helpers -- so a second writer would exist to serve an
// encoding nothing being imported uses. A reader must handle both because files
// already exist; a writer chooses.
//
// The version pair is not a parameter for the same reason readP3DMHeader rejects
// anything else: P3DM has no headSize, so 28.256 is the only layout the reader
// can resynchronise on.

// One corner of one face. `point` and `normal` index the LOD's own tables; the
// UVs are per-face-corner, which is why a shared point can still carry different
// UVs in different faces.
struct WriteFaceVertex
{
    int32_t point = 0;
    // -1 when the LOD has no normal for this corner. The reader bounds-checks the
    // index and leaves a zero normal rather than failing, so an out-of-range value
    // is the format's way of saying "absent".
    int32_t normal = -1;
    float u = 0.0f;
    float v = 0.0f;
    // Second UV channel. Written only when the LOD sets secondUVChannel, because
    // the reader promotes the *second* #UVSet# block to Vertex::uv1 -- writing one
    // block alone would put this data on channel 0, which the face records already
    // carry.
    float u1 = 0.0f;
    float v1 = 0.0f;
};

struct WriteFace
{
    int32_t vertexCount = 3; // 3 or 4; anything else is rejected at write time
    WriteFaceVertex vertices[4];
    uint32_t flags = 0;
    std::string texture;  // image path, may be empty
    std::string material; // RVMAT path, may be empty -- a different thing from the texture
};

struct WritePoint
{
    Vector3 position;
    // Raw POINT_* bits from Data3D.h, not the ClipFlags the reader converts them
    // into. The conversion is one-way (convertPointFlagsToClipFlags collapses
    // several inputs onto one output and zeroes anything with unknown bits), so
    // feeding a loaded Vertex::flags back in here would not reproduce the source.
    int32_t flags = 0;
};

struct WriteProperty
{
    std::string name; // truncated at 64 bytes on the wire, so longer is rejected
    std::string value;
};

// A named selection: a set of points and a set of faces, both by index into the
// LOD's own tables. On the wire this is one weight byte per POINT followed by one
// flag byte per FACE (readTAGGNamedSelection), the face bytes indexing the file's
// mixed triangle/quad stream -- the same unit Triangle::originalIndex carries.
//
// COL-001: a Geometry LOD's `ComponentXX` selections are what the engine turns into
// convex components (LODShape::InitConvexComponents), and a selection with fewer
// than four faces is dropped there, so a component is a closed convex polytope or
// it is nothing.
struct WriteNamedSelection
{
    std::string name;            // must not be empty and must not start with '#' (that is a tagg keyword)
    std::vector<int32_t> points; // indices into WriteLod::points
    std::vector<int32_t> faces;  // indices into WriteLod::faces
    // The byte written for a selected point. 1 is O2's "fully selected"; the reader
    // treats any non-zero byte as membership and keeps the raw value.
    uint8_t weight = 1;
    // Optional per-point override, parallel to `points`; empty means every point
    // gets `weight`. A zero entry here is rejected (it would deselect the point).
    std::vector<uint8_t> pointWeights;
};

struct WriteLod
{
    std::vector<WritePoint> points;
    std::vector<Vector3> normals;
    std::vector<WriteFace> faces;
    std::vector<WriteProperty> properties;
    std::vector<WriteNamedSelection> selections;
    // COL-001: the `#Mass#` tagg, one float per point. Empty writes no tagg at all;
    // anything else must be exactly points.size() long, because the reader sizes
    // the payload from the point count and nothing else. The engine sums this into
    // LODShape::Mass(), and Object::IsPassable() is `Mass() < 10`.
    std::vector<float> mass;
    // Emit two #UVSet# blocks (channel 0 duplicating the face-record UVs, channel 1
    // carrying u1/v1). Off by default: a LOD with no second channel should carry no
    // UV blocks at all rather than a redundant copy of channel 0.
    bool secondUVChannel = false;
    // Viewing distance in metres for a visual LOD, or a special-LOD sentinel --
    // 1e13 for Geometry, 1e15 * n for the rest. See World/Model/LodPurpose.hpp;
    // the value is written verbatim and classified by the reader.
    float resolution = 1.0f;
};

struct WriteModel
{
    std::vector<WriteLod> lods; // 1-100; the reader rejects a count outside that
};

} // namespace MLOD

class MLODWriter
{
  public:
    // Throws std::runtime_error on anything the reader would reject or silently
    // misread: no LODs, a face that is not a triangle or a quad, a point or normal
    // index outside its table, a string longer than the reader's 1024-byte cap.
    // Refusing here is the point -- every one of those produces a file that loads
    // and is wrong rather than one that fails.
    static std::vector<char> writeToBuffer(const MLOD::WriteModel& model);

    static void write(const MLOD::WriteModel& model, const std::string& filePath);

    // Round-trip helper: turn what MLODLoader produced back into something this
    // writer accepts.
    //
    // Two things are undone here, both of them things the loader does on the way
    // in. Face winding is reversed back (the loader swaps indices 0/1 on triangles
    // and 0/1 + 2/3 on quads), and faces are re-interleaved into source order by
    // originalIndex -- the loader splits them into separate triangle and quad
    // vectors, and writing all triangles then all quads would renumber every face.
    //
    // One vertex becomes one point. The IR has no point table: its vertices are
    // already split per unique (position, normal, uv) combination, so there is no
    // sharing left to reconstruct. The reader re-merges on load, which is what
    // makes the result comparable to the input.
    static MLOD::WriteModel describe(const Poseidon::Model::Model& model);
};

} // namespace Poseidon::Asset::Formats
