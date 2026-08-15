#pragma once

// Far-field proxy data for a MODEL, read from as little of its P3D as the
// container allows.
//
// A modern world names a few hundred distinct models and places them millions of
// times, so anything the far-field tier needs per model must cost a header read,
// not a load. For ODOL that is literally true: everything below is already in
// ModelInfo, which every ODOL revision from Arma 1 onward puts at the front of the
// file -- the bounding sphere radius, the model-space bounding box, and the
// shape's average colour. No LOD body is touched, no bulk array is decompressed,
// and no buffer larger than a Vector3 is allocated.
//
// MLOD is the second container, and it has no ModelInfo at all. It is the
// EDITABLE form: a twelve-byte file header and then LOD bodies, with the bounds an
// ODOL carries precomputed nowhere on the wire. That is not a corner case for this
// caller -- the imported Reforger worlds are MLOD 1.1 / P3DM 28.256 without
// exception (measured: 1,132 of 1,132 Everon models), so refusing the container
// refuses the entire world. The MLOD path therefore derives the bounds from the
// FIRST LOD's vertex block and stops there: it never reads a face, a normal, a UV
// set or a TAGG, and never touches the second LOD.
//
// The deliberate refusal is OFP/CWA's revision 7, whose ModelInfo sits at the END
// of the file behind every LOD body. Reading it is not a header read at all, so
// this returns valid=false rather than quietly paying for a full parse.

#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/Asset/Formats/BISStructures.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODStructures.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace Poseidon::Asset::Formats::P3D
{

struct ModelProxyBounds
{
    float radius = 0.0f; // ModelInfo's bounding-sphere radius
    float minY = 0.0f;   // vertical extent of the model-space bbox
    float maxY = 0.0f;
    float horizontalExtent = 0.0f; // max(bbox x extent, bbox z extent)
    uint32_t color = 0xFFFFFFFFu;  // shape average colour, packed
    bool valid = false;
    // Whether `color` was read or defaulted. MLOD carries no average colour
    // anywhere, so the MLOD path leaves the white default in place -- and says so
    // here rather than handing a caller an invented colour it cannot tell apart
    // from a model that really is white.
    bool colorKnown = false;
};

// ModelInfo's shared prefix for revisions 40 and 48-54.
//
// It is byte-identical across that whole span -- compare Odol40.hpp's directory
// reader (through bboxMax) with Odol49.hpp's -- and the revision-specific
// divergence begins only AFTER bboxMax, at revision 52's two extra vectors. That
// is why this stops there: everything a far-field proxy needs is inside the span
// no revision in the family disagrees about, so one reader covers all of them
// without a single version gate.
//
// The full directory readers in those headers are not reused here on purpose.
// They continue past this point through the mass array, the animation tables and
// the LOD byte-offset table -- work whose only product for this caller is a
// decompressed float array it throws away.
inline Odol73Preamble ReadOdolFamilyPreamble(BinaryReader& reader, uint32_t expectedRevision)
{
    char signature[4] = {};
    reader.readBytes(signature, sizeof(signature));
    if (std::memcmp(signature, "ODOL", 4) != 0 || reader.read<uint32_t>() != expectedRevision)
        throw std::runtime_error("ODOL A1/A2-family signature and exact revision expected");

    Odol73Preamble result;
    const uint32_t lodCount = reader.read<uint32_t>();
    if (lodCount == 0 || lodCount > 100 ||
        static_cast<uint64_t>(lodCount) * sizeof(float) > static_cast<uint64_t>(reader.remaining()))
        throw std::runtime_error("ODOL A1/A2 invalid LOD-resolution directory");
    result.resolutions.resize(lodCount);
    for (float& resolution : result.resolutions)
        resolution = reader.read<float>();

    result.special = reader.read<int32_t>();
    result.boundingSphere = reader.read<float>();
    result.geometrySphere = reader.read<float>();
    result.remarks = reader.read<int32_t>();
    result.andHints = reader.read<int32_t>();
    result.orHints = reader.read<int32_t>();
    reader.readBytes(&result.aimingCenter, sizeof(result.aimingCenter));
    result.color = reader.read<uint32_t>();
    result.colorType = reader.read<uint32_t>();
    result.viewDensity = reader.read<float>();
    reader.readBytes(&result.bboxMin, sizeof(result.bboxMin));
    reader.readBytes(&result.bboxMax, sizeof(result.bboxMax));
    return result;
}

// MLOD bounds, from the FIRST LOD's vertex block and nothing else.
//
// The layout this walks, all of it in MLODStructures.hpp:
//
//   MLODStructures.hpp:39  readHeader          12 bytes: "MLOD", major u8, minor
//                                              u8, padding u16, lodCount u32.
//                                              There is no ModelInfo, no bounding
//                                              sphere and no colour -- the file
//                                              header ends and a LOD begins.
//   MLODStructures.hpp:119 peekLodSignature    the per-LOD encoding, which the
//                                              file header does NOT state. Both
//                                              occur in this corpus: OFP-era
//                                              models are SP3X, every Arma- and
//                                              Reforger-generation model is P3DM.
//   MLODStructures.hpp:167 readSP3XHeader      28 fixed bytes ending in nPos, plus
//                                              headSize-28 more it skips.
//   MLODStructures.hpp:319 readP3DMHeader      28 fixed bytes, no headSize, and a
//                                              hard version gate at 28.256.
//   MLODStructures.hpp:201 readVertexTable     nPos records of Vector3 position +
//                                              int32 flags, immediately after the
//                                              LOD header. That is the block below.
//
// readVertexTable is deliberately not called: it materialises a std::vector of
// every point, and this needs six floats out of a stream it is walking once. The
// record layout is its layout, restated as a stride so a change there is visible
// as a mismatch here rather than as silently shifted bounds.
//
// Why the first LOD and not a cheaper one: MLOD stores LODs in ascending
// resolution, so LOD 0 is the highest-detail visual shape and the special LODs
// (geometry, memory, shadow) sort to the end. Reaching any later LOD means parsing
// this one's faces -- P3DM face records carry two NUL-terminated strings each, so
// the block's end is only knowable by decoding it. One vertex block is the
// cheapest correct answer available in this container.
inline ModelProxyBounds ReadMlodProxyBounds(BinaryReader& reader)
{
    // Validates the signature, the version and the LOD count, and throws on all
    // three. Its return is unused: nothing past lodCount bears on the bounds.
    (void)MLOD::readHeader(reader);

    int32_t pointCount = 0;
    switch (MLOD::peekLodSignature(reader))
    {
        case MLOD::LodSignature::SP3X:
            pointCount = MLOD::readSP3XHeader(reader).nPos;
            break;
        case MLOD::LodSignature::P3DM:
            pointCount = MLOD::readP3DMHeader(reader).nPos;
            break;
        default:
            throw std::runtime_error("MLOD first LOD uses an unrecognised encoding");
    }
    // SP3X's headSize skip is a seek, and a seek past the end sets the fail bit
    // instead of throwing. Reading on from there would report bytes from wherever
    // the cursor stayed, so the failure is converted into one here.
    if (reader.fail())
        throw std::runtime_error("MLOD first LOD header does not fit the input");

    constexpr int64_t pointStride = 16; // Vector3 + int32, per readVertexTable
    if (pointCount <= 0 || static_cast<int64_t>(pointCount) * pointStride > static_cast<int64_t>(reader.remaining()))
        throw std::runtime_error("MLOD first LOD vertex block is empty or larger than the input");

    bool any = false;
    float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f, minZ = 0.0f, maxZ = 0.0f;
    float maxDistanceSquared = 0.0f;
    for (int32_t i = 0; i < pointCount; ++i)
    {
        const float x = reader.read<float>();
        const float y = reader.read<float>();
        const float z = reader.read<float>();
        (void)reader.read<int32_t>(); // point clip flags -- nothing a proxy needs

        // One unusable vertex must not cost a 3,000-vertex model its proxy, so a
        // non-finite point is dropped rather than thrown on. A file with nothing
        // BUT such points still refuses, below.
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            continue;

        if (!any)
        {
            minX = maxX = x;
            minY = maxY = y;
            minZ = maxZ = z;
            any = true;
        }
        else
        {
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
            minZ = std::min(minZ, z);
            maxZ = std::max(maxZ, z);
        }
        maxDistanceSquared = std::max(maxDistanceSquared, x * x + y * y + z * z);
    }
    if (!any)
        throw std::runtime_error("MLOD first LOD has no finite vertex");

    ModelProxyBounds bounds;
    // ODOL's boundingSphere is a radius about the model origin, so this is the
    // same quantity computed rather than read. Coordinates large enough to
    // overflow the square are caught by the finiteness test below, not here.
    bounds.radius = std::sqrt(maxDistanceSquared);
    bounds.minY = minY;
    bounds.maxY = maxY;
    bounds.horizontalExtent = std::max(maxX - minX, maxZ - minZ);
    // color and colorKnown stay at their defaults on purpose: see ModelProxyBounds.
    bounds.valid =
        std::isfinite(bounds.radius) && std::isfinite(bounds.horizontalExtent) && bounds.radius >= 0.0f && maxY >= minY;
    return bounds;
}

// Reads ONLY the header preamble.
//
// ... for ODOL. MLOD goes to the first-LOD vertex walk above, which is the
// cheapest correct answer that container has.
//
// Returns valid=false -- never throws -- for a revision whose ModelInfo is not at
// the front of the file (OFP-era ODOL 7), for a container that is neither ODOL nor
// MLOD, and for any truncated, garbage or non-finite input. The reader is restored
// to the position it came in on either way, so a caller can peek proxy bounds off a
// stream it intends to parse properly afterwards.
inline ModelProxyBounds ReadModelProxyBounds(BinaryReader& reader)
{
    const int start = reader.tell();
    try
    {
        // Which container, before either reader is handed the stream. Both of the
        // revision peeks below throw on a signature they do not own, so dispatching
        // by exception would make MLOD -- the common case for an imported world --
        // the one that costs a throw.
        char container[4] = {};
        if (reader.remaining() >= static_cast<int>(sizeof(container)))
        {
            reader.readBytes(container, sizeof(container));
            reader.seek(start);
        }
        if (std::memcmp(container, "MLOD", 4) == 0)
        {
            const ModelProxyBounds mlod = ReadMlodProxyBounds(reader);
            reader.seek(start);
            return mlod.valid ? mlod : ModelProxyBounds{};
        }

        const OdolRevisionInfo revision = PeekOdolRevision(reader);
        Odol73Preamble preamble;
        switch (revision.version)
        {
            // Arma 3. Its ModelInfo carries an application id and a muzzle-flash
            // string ahead of the LOD-resolution table, so it keeps its own reader.
            case 73:
                preamble = ReadOdol73Preamble(reader);
                break;
            // Arma 1 (40), Arma 2 and OA (48-52), DayZ (54).
            case 40:
            case 48:
            case 49:
            case 50:
            case 52:
            case 54:
                preamble = ReadOdolFamilyPreamble(reader, revision.version);
                break;
            default:
                reader.seek(start);
                return ModelProxyBounds{};
        }

        // A header that parsed is not the same as a header that means something: a
        // run of plausible bytes can decode to NaN or to an inverted box, and a
        // far-field impostor sized from either is worse than none at all.
        //
        // Every SOURCE component is tested, not the derived extents. std::max is
        // not NaN-propagating -- max(4.0f, NaN) is 4.0f, because it returns the
        // first argument unless the second compares greater -- so a NaN in one
        // bbox axis can vanish into a plausible-looking extent.
        const float components[] = {preamble.boundingSphere, preamble.bboxMin.x, preamble.bboxMin.y, preamble.bboxMin.z,
                                    preamble.bboxMax.x,      preamble.bboxMax.y, preamble.bboxMax.z};
        for (float component : components)
            if (!std::isfinite(component))
            {
                reader.seek(start);
                return ModelProxyBounds{};
            }

        ModelProxyBounds bounds;
        bounds.radius = preamble.boundingSphere;
        bounds.minY = preamble.bboxMin.y;
        bounds.maxY = preamble.bboxMax.y;
        bounds.horizontalExtent =
            std::max(preamble.bboxMax.x - preamble.bboxMin.x, preamble.bboxMax.z - preamble.bboxMin.z);
        bounds.color = preamble.color;
        bounds.colorKnown = true;
        bounds.valid = bounds.radius >= 0.0f && bounds.maxY >= bounds.minY &&
                       preamble.bboxMax.x >= preamble.bboxMin.x && preamble.bboxMax.z >= preamble.bboxMin.z;
        reader.seek(start);
        return bounds.valid ? bounds : ModelProxyBounds{};
    }
    catch (const std::exception&)
    {
        // seekg() clears the stream's fail bit on a valid position, so this
        // restores the reader's usability as well as its cursor.
        reader.seek(start);
        return ModelProxyBounds{};
    }
}

// The same, over a raw prefix of a P3D. Offered because that is the shape the
// far-field tier actually has: a few hundred models, each of which need only its
// first kilobyte fetched out of a PBO rather than its whole body.
inline ModelProxyBounds ReadModelProxyBounds(const void* data, int size)
{
    if (data == nullptr || size <= 0)
        return ModelProxyBounds{};
    QIStream stream(data, size);
    BinaryReader reader(stream);
    return ReadModelProxyBounds(reader);
}

} // namespace Poseidon::Asset::Formats::P3D
