#pragma once

// Far-field proxy data for a MODEL, read from its P3D header alone.
//
// A modern world names a few hundred distinct models and places them millions of
// times, so anything the far-field tier needs per model must cost a header read,
// not a load. Everything below is already in ModelInfo, which every ODOL revision
// from Arma 1 onward puts at the front of the file: the bounding sphere radius,
// the model-space bounding box, and the shape's average colour. No LOD body is
// touched, no bulk array is decompressed, and no buffer larger than a Vector3 is
// allocated.
//
// The deliberate refusal is OFP/CWA's revision 7, whose ModelInfo sits at the END
// of the file behind every LOD body. Reading it is not a header read at all, so
// this returns valid=false rather than quietly paying for a full parse.

#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/Asset/Formats/BISStructures.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>

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

// Reads ONLY the header preamble.
//
// Returns valid=false -- never throws -- for a revision whose ModelInfo is not at
// the front of the file (OFP-era ODOL 7), for a non-ODOL container, and for any
// truncated, garbage or non-finite header. The reader is restored to the position
// it came in on either way, so a caller can peek proxy bounds off a stream it
// intends to parse properly afterwards.
inline ModelProxyBounds ReadModelProxyBounds(BinaryReader& reader)
{
    const int start = reader.tell();
    try
    {
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
