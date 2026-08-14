#pragma once

#include <Poseidon/Asset/Formats/Enfusion/EnfusionIff.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// Arma Reforger's `.xob` model -- the render geometry only (ARF-002).
//
// The container is the IFF dialect in EnfusionIff.hpp, form type `XOB9`. Chunk
// census over the local install's 15,809 `.xob` files: HEAD 15809, LODS 15807,
// COLL 14771, VOLM 13328, PRB2 199, BSP3 199. HEAD and LODS carry the geometry;
// COLL / VOLM / BSP3 / PRB2 are collision and volume data and are not decoded here,
// and neither is the bone hierarchy beyond skipping over it.
//
// HEAD, little-endian throughout:
//   u32    version           0 on the whole corpus
//   f32[3] bboxMin, bboxMax  model space, metres
//   f32[3] sphereCenter, f32 sphereRadius
//   u16    nMaterials (1..12 observed), u16 nBones (non-zero in 1440 files)
//   u8     nLods (0..9), u8 nPoints (non-zero in 507), u16 pad (always 0)
//   u32    nQuads (non-zero in 146), u32 strTabSize, then NUL-separated strings
//   u16[2*nMaterials]        per material: (nameOrdinal, pathOrdinal)
//   f32[3] * nPoints         loose model-space snap points
//   Bone[nBones]             36 bytes each, skipped
//   Lod[nLods]
//   Quad[nQuads]             56 bytes then f32[3]*n; n == 4 in every observed record
//
// A LOD descriptor lives in HEAD and points at a payload in LODS by ABSOLUTE file
// offset. The coarsest LOD sits FIRST in the file and thresholds run strictly
// descending, LOD0 always 0.5 (571 / 571 multi-LOD files); the per-LOD extents tile
// the LODS chunk with no gap (15809 / 15809 files).
//
// The payload is a chain of LZ4 blocks despite the `LZO4` fourcc on every one of the
// 56,771 LOD descriptors -- LZO1X does not decode these streams and plain LZ4 does,
// byte for byte. Each block is a u32 header whose low 31 bits are the compressed
// size that follows and whose bit 31 marks the LAST block; bit 31 is NOT a
// "compressed" flag, and reading it as one truncates every multi-block LOD. The
// blocks are LINKED -- a match may reach back into the previous block's output --
// which is the same shape DayZ's `.edds` mips use, so Foundation's Lz4Block does the
// work here rather than a second decompressor.
//
// Two traps that cost real time and must not be lost:
//
//   * The triangle count is 32-bit, split across two fields:
//     nTris = nTrisLo | ((flags3 >> 16) << 16). Three corpus parts exceed 65,535
//     triangles and each was short by exactly 12 * 65536 payload bytes until the
//     high half was applied.
//   * One payload blob holds ALL of a LOD's parts back to back, not one blob per
//     part.
//
// Two vertex STREAMS, not two meshes. Index list A runs over the deduplicated
// `vertexCount` stream; list B runs over the `renderVertexCount` stream, and mapping
// every B index through its source index reproduces list A's triangles exactly
// (14,502 / 14,502 parts). A source vertex is repeated only when it needs a second
// tangent frame at a UV seam, which is why renderVertexCount >= vertexCount.
//
// Positions are plain float32 and are NOT quantised. Only the directions (32-bit
// packed) and the UVs (2 x s16 through the set's rect) are.
//
// Closure, measured by the whole-corpus census (ARF-002):
//   container walk closes                     15809 / 15809 files
//   HEAD byte accounting closes               15809 / 15809 files
//   per-LOD extents tile LODS exactly         15809 / 15809 files
//   dataSizeRaw == predicted part sizes       56770 / 56771 LODs
//
// The single LOD that does not close is Billiard_01_ball.xob LOD1, the corpus's only
// part with nUV == 0; it spends 28 bytes per vertex where the formula predicts 20.
// Its layout was never established, so it is REFUSED rather than guessed at.

namespace Poseidon::Asset::Formats::Enfusion
{

struct XobVec3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct XobVec2
{
    float u = 0.0f, v = 0.0f;
};

//! The dequantisation window for one UV set.
struct XobUvSet
{
    float uMin = 0.0f, uMax = 0.0f, vMin = 0.0f, vMax = 0.0f;
    //! A per-set scalar in the 0.03 .. 400 range. Geometry does not need it and its
    //! meaning was never established, so it is carried rather than interpreted.
    float extra = 0.0f;
};

struct XobMaterial
{
    std::string name; //!< display name from the string table
    std::string path; //!< "<path>.emat", GUID stripped -- 38971 / 38971 refs resolve
};

//! Unpacks a 32-bit packed unit direction (normal, tangent or bitangent).
//!
//! The field split was found by brute-forcing every contiguous 3-field and
//! 2-field-octahedral layout against the six exactly-known face normals of an
//! axis-aligned box: this layout fits 0.999996 and the runner-up 0.808. Confirmed
//! independently -- 18,218,121 of 18,218,276 packed normals in the sample decode to
//! unit length within 0.02, and the 155 that do not are authored degenerate vertices
//! inside otherwise clean parts.
inline XobVec3 UnpackXobDirection(uint32_t packed)
{
    XobVec3 out;
    out.z = static_cast<float>(packed & 0x3FFu) / 1023.0f * 2.0f - 1.0f;         // bits 0..9
    out.y = static_cast<float>((packed >> 10) & 0x7FFu) / 2047.0f * 2.0f - 1.0f; // bits 10..20
    out.x = static_cast<float>((packed >> 21) & 0x7FFu) / 2047.0f * 2.0f - 1.0f; // bits 21..31
    const float lengthSq = out.x * out.x + out.y * out.y + out.z * out.z;
    if (lengthSq > 1e-18f)
    {
        const float inverse = 1.0f / std::sqrt(lengthSq);
        out.x *= inverse;
        out.y *= inverse;
        out.z *= inverse;
    }
    return out;
}

//! Unpacks a 32-bit packed UV (2 x s16) through its set's rect. The signed halves
//! span the rect end to end, so the rect -- not the raw value -- is what puts a UV
//! in texture space; ignoring it leaves every atlased part on the wrong tile.
inline XobVec2 UnpackXobUv(uint32_t packed, const XobUvSet& set)
{
    const int16_t rawU = static_cast<int16_t>(packed & 0xFFFFu);
    const int16_t rawV = static_cast<int16_t>((packed >> 16) & 0xFFFFu);
    const float fractionU = (static_cast<float>(rawU) / 32767.0f + 1.0f) * 0.5f;
    const float fractionV = (static_cast<float>(rawV) / 32767.0f + 1.0f) * 0.5f;
    XobVec2 out;
    out.u = set.uMin + (set.uMax - set.uMin) * fractionU;
    out.v = set.vMin + (set.vMax - set.vMin) * fractionV;
    return out;
}

//! One sub-mesh as declared in HEAD, before its payload is decoded. The declared
//! counts are what predict the payload size, so they are kept apart from the decoded
//! geometry: a file is refused on this arithmetic before a byte is decompressed.
struct XobPartDesc
{
    uint16_t flags0 = 0;       //!< purpose not established
    uint8_t flags1 = 0;        //!< 0 or 1; purpose not established
    uint8_t vertexFormat = 0;  //!< 0x80 second UV set, 0x20 colour stream, 0x10 skinned
    XobVec3 bboxMin, bboxMax;
    XobVec3 planeNormal;       //!< non-zero only for planar parts (decals, glass)
    float planeDistance = 0.0f;
    uint32_t triangleCount = 0;      //!< already recombined from both halves
    uint16_t vertexCount = 0;        //!< deduplicated stream, indexed by list A
    uint16_t renderVertexCount = 0;  //!< render stream, indexed by list B
    uint32_t skinEntryCount = 0;
    uint16_t materialIndex = 0; //!< into XobHeader::materials
    uint8_t uvSetCount = 0;     //!< 2 in 69194 parts, 1 in 24403, 0 in exactly 1
    uint32_t flags3 = 0;        //!< low 16 bits small flags, high 16 the triangle count
    std::vector<XobUvSet> uvSets;
    std::vector<uint8_t> boneIds;

    bool skinned() const { return (vertexFormat & 0x10u) != 0; }
    bool hasColours() const { return (vertexFormat & 0x20u) != 0; }

    //! Bytes per deduplicated vertex: position (16 when skinned, the extra u32 being
    //! a skin-table index), packed normal, one packed UV per set, optional colour.
    uint64_t VertexStride() const
    {
        return (skinned() ? 16u : 12u) + 4u + 4u * static_cast<uint64_t>(uvSetCount) + (hasColours() ? 4u : 0u);
    }

    //! Bytes per render vertex: the source index plus a tangent/bitangent pair per set.
    uint64_t RenderVertexStride() const { return 4u + 8u * static_cast<uint64_t>(uvSetCount); }

    uint64_t PayloadBytes() const
    {
        return 12ull * triangleCount + static_cast<uint64_t>(vertexCount) * VertexStride() +
               static_cast<uint64_t>(renderVertexCount) * RenderVertexStride() +
               (skinned() ? 16ull * skinEntryCount : 0ull);
    }
};

//! One LOD as declared in HEAD, plus where its compressed payload lives.
struct XobLodDesc
{
    FourCC codec;              //!< "LZO4" on 56771 / 56771 descriptors -- it is LZ4
    uint32_t partCount = 0;
    float threshold = 0.0f;    //!< LOD0 is always 0.5; the sequence descends strictly
    uint32_t skinBoneCount = 0;
    uint32_t dataOffset = 0;   //!< ABSOLUTE file offset, not relative to LODS
    uint32_t dataSize = 0;     //!< compressed
    uint32_t dataSizeRaw = 0;  //!< uncompressed; must equal PredictedRawBytes()
    std::vector<XobPartDesc> parts;

    uint64_t PredictedRawBytes() const
    {
        uint64_t total = 0;
        for (const XobPartDesc& part : parts)
            total += part.PayloadBytes();
        return total;
    }
};

//! HEAD, decoded. `error` empty means every closure below held.
struct XobHeader
{
    uint32_t version = 0;
    XobVec3 bboxMin, bboxMax, sphereCenter;
    float sphereRadius = 0.0f;
    uint16_t boneCount = 0;
    uint32_t quadCount = 0;
    std::vector<std::string> strings;
    std::vector<XobMaterial> materials;
    std::vector<XobVec3> points;
    std::vector<XobLodDesc> lods;

    //! Reported separately because each catches a different class of mistake, and
    //! because the corpus census needs to say which one a file failed.
    bool headCloses = false;    //!< the HEAD walk landed exactly on the chunk end
    bool lodsTileChunk = false; //!< the LOD extents tile LODS with no gap or overlap
    bool sizesClose = false;    //!< every dataSizeRaw equals its predicted part sizes
    std::string error;

    bool valid() const { return error.empty(); }
};

//! One decoded sub-mesh. Both streams are kept: list A over `positions` is the
//! position-only topology, list B over the render stream is what a GPU wants, and
//! discarding either loses information the other cannot recover.
struct XobPart
{
    uint16_t materialIndex = 0;
    uint8_t uvSetCount = 0;
    bool skinned = false;
    XobVec3 bboxMin, bboxMax;
    uint32_t triangleCount = 0;

    // Deduplicated stream -- `indices` runs over this.
    std::vector<XobVec3> positions;
    std::vector<XobVec3> normals;
    std::vector<std::vector<XobVec2>> uvSets; //!< uvSets[set][vertex]
    std::vector<uint32_t> colours;            //!< empty unless the format declares one
    std::vector<uint16_t> indices;            //!< list A, 3 per triangle

    // Render stream -- `renderIndices` runs over this.
    std::vector<uint32_t> renderSource;    //!< render vertex -> index into positions
    std::vector<uint16_t> renderIndices;   //!< list B, 3 per triangle
    std::vector<XobVec3> tangents;         //!< per render vertex, UV set 0
    std::vector<XobVec3> bitangents;

    //! Position of render vertex `i`, i.e. positions[renderSource[i]].
    XobVec3 RenderPosition(size_t i) const { return positions[renderSource[i]]; }
};

struct XobLod
{
    float threshold = 0.0f;
    std::vector<XobPart> parts;
};

struct XobModel
{
    XobVec3 bboxMin, bboxMax, sphereCenter;
    float sphereRadius = 0.0f;
    std::vector<XobMaterial> materials;
    std::vector<XobLod> lods; //!< in file order: the COARSEST LOD is index 0
    std::string error;

    bool valid() const { return error.empty(); }
};

//! Walks the container and decodes HEAD. Cheap -- no payload is decompressed -- so
//! this is what a corpus census or an asset scanner should call.
XobHeader ReadXobHeader(const void* data, size_t size);

//! Decompresses one LOD's payload into `raw`. Fails unless the LZ4 chain consumes
//! its input exactly and produces exactly dataSizeRaw bytes.
bool DecodeXobLodPayload(const void* data, size_t size, const XobLodDesc& lod, std::vector<uint8_t>& raw,
                         std::string& error);

//! Decodes one LOD's geometry. Fails rather than half-reads: the payload walk must
//! consume the decompressed blob exactly.
bool ReadXobLod(const void* data, size_t size, const XobHeader& header, size_t lodIndex, XobLod& out,
                std::string& error);

//! Header plus every LOD's geometry. Pass `decodeGeometry = false` for the header
//! alone with the model's bounds and materials still filled in.
XobModel ReadXobModel(const void* data, size_t size, bool decodeGeometry = true);

} // namespace Poseidon::Asset::Formats::Enfusion
