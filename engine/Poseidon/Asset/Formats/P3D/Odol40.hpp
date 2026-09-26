#pragma once

// AST-013 -- the Arma 1 reader.  Revision 40 is the *only* revision Arma 1 ships:
// 2,580 ODOL models across the 95 PBOs of the local A1 corpus, without exception.
// Until this file existed nothing of it parsed, so all 893 models sara.wrp
// references failed at the container.
//
// It is a port of a Python reference parser that closes 18,697 of 18,697 LOD
// bodies EXACTLY on their declared byte boundary across that whole corpus, with
// 18,379 embedded materials read.  The layout below is therefore measured rather
// than inferred, and the per-LOD boundary check at the bottom of
// ReadOdol40StaticLod is the same check that produced that figure.
//
// What separates revision 40 from the Arma 2 family (Odol49.hpp) is small but
// fatal if guessed:
//
//   * Bulk arrays are LZSS (SSCompress), not LZO.  Revision 23 of the world
//     format made the same switch later; ODOL made it after 40.
//   * ModelInfo carries eleven bytes of flags where the A2 layout carries four
//     bools, six floats and a further bool/int/bool/float/bool, and it has no
//     trailing "obsolete pivots" string after the bone table.
//   * UVs and normals are FULL FLOATS -- an 8-byte (u,v) pair and a 12-byte
//     normal -- not the quantised 16-bit UV pair and 10-bit-per-axis packed
//     normal every later revision uses.  Both arrive in "condensed" arrays that
//     may declare a single repeated element.
//   * ST coordinates are 24 bytes per vertex, not 8.
//   * The clip flags live in the LOD *header*, as they do below revision 50.
//   * A LOD body ends at its declared boundary with no trailing byte.

#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Poseidon::Asset::Formats::P3D
{

// A bulk payload of exactly `expected` decoded bytes.  There is no per-payload
// compression flag at this revision: anything below the 1024-byte threshold is
// stored raw, anything at or above it is LZSS.  The LZSS stream ends in a
// checksum the decoder consumes and verifies, which is the only thing that says
// the payload really was the size the caller claimed.
inline std::vector<uint8_t> ReadOdol40Compressed(BinaryReader& reader, size_t expected)
{
    if (expected == 0)
        return {};
    std::vector<uint8_t> output(expected);
    if (expected < COMPRESSION_THRESHOLD)
    {
        reader.readBytes(output.data(), expected);
        return output;
    }
    reader.readCompressedBytes(output.data(), expected);
    return output;
}

// count, then a payload of exactly count elements.
template <typename T>
inline std::vector<T> ReadOdol40CompressedArray(BinaryReader& reader, const char* field, uint32_t limit)
{
    const int32_t count = reader.read<int32_t>();
    if (count < 0 || static_cast<uint32_t>(count) > limit)
        throw std::runtime_error(std::string("ODOL 40 ") + field + " count is out of range");
    const auto bytes = ReadOdol40Compressed(reader, static_cast<size_t>(count) * sizeof(T));
    std::vector<T> values(static_cast<size_t>(count));
    if (count != 0)
        std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
}

// As above, but with a leading "every element is this value" flag.  When it is
// set, one element follows and the array is that element repeated -- the payload
// is not compressed and the threshold does not apply to it.
template <typename T>
inline std::vector<T> ReadOdol40CondensedArray(BinaryReader& reader, const char* field, uint32_t limit)
{
    const int32_t count = reader.read<int32_t>();
    if (count < 0 || static_cast<uint32_t>(count) > limit)
        throw std::runtime_error(std::string("ODOL 40 ") + field + " count is out of range");
    if (reader.read<bool>())
    {
        T value{};
        reader.readBytes(&value, sizeof(T));
        return std::vector<T>(static_cast<size_t>(count), value);
    }
    const auto bytes = ReadOdol40Compressed(reader, static_cast<size_t>(count) * sizeof(T));
    std::vector<T> values(static_cast<size_t>(count));
    if (count != 0)
        std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
}

inline void RequireOdol40Range(const BinaryReader& reader, uint32_t end, const char* field)
{
    if (reader.tell() > static_cast<int>(end))
        throw std::runtime_error(std::string("ODOL 40 ") + field + " exceeds its declared LOD range");
}

// ModelInfo and the LOD byte-offset table.
inline Odol73LodDirectory ReadOdol40StaticDirectory(BinaryReader& reader, int fileSize)
{
    char signature[4] = {};
    reader.readBytes(signature, sizeof(signature));
    if (std::memcmp(signature, "ODOL", 4) != 0 || reader.read<uint32_t>() != 40)
        throw std::runtime_error("ODOL 40 signature and exact revision expected");

    Odol73LodDirectory result;
    const uint32_t lodCount = reader.read<uint32_t>();
    if (lodCount == 0 || lodCount > 100 || static_cast<uint64_t>(lodCount) * sizeof(float) > reader.remaining())
        throw std::runtime_error("ODOL 40 invalid LOD-resolution directory");
    result.model.resolutions.resize(lodCount);
    for (float& resolution : result.model.resolutions)
        resolution = reader.read<float>();

    auto readVector = [&reader](Vector3& value) { reader.readBytes(&value, sizeof(value)); };
    auto skipVector = [&reader]()
    {
        Vector3 unused;
        reader.readBytes(&unused, sizeof(unused));
    };
    result.model.special        = reader.read<int32_t>();
    result.model.boundingSphere = reader.read<float>();
    result.model.geometrySphere = reader.read<float>();
    result.model.remarks        = reader.read<int32_t>();
    result.model.andHints       = reader.read<int32_t>();
    result.model.orHints        = reader.read<int32_t>();
    readVector(result.model.aimingCenter);
    result.model.color       = reader.read<uint32_t>();
    result.model.colorType   = reader.read<uint32_t>();
    result.model.viewDensity = reader.read<float>();
    readVector(result.model.bboxMin);
    readVector(result.model.bboxMax);
    // Bounding centre, geometry centre (still skipped -- see the revision-73
    // reader for why), centre of mass, then the three rows of the inverse
    // inertia tensor (COL-001).
    skipVector();
    skipVector();
    readVector(result.model.centerOfMass);
    for (int i = 0; i < 3; ++i)
        readVector(result.model.invInertia[i]);
    // Eleven bytes of model flags. The A2 layout has 39 bytes of typed fields
    // here instead; reading those would put the skeleton name eleven bytes into
    // the wrong place and every later offset with it.
    for (int i = 0; i < 11; ++i)
        (void)reader.read<uint8_t>();

    result.skeletonName = ReadBoundedAsciiz(reader, "skeleton name");
    if (!result.skeletonName.empty())
    {
        result.skeletonIsDiscrete = reader.read<bool>();
        const uint32_t bones      = reader.read<uint32_t>();
        if (bones > 4096)
            throw std::runtime_error("ODOL 40 skeleton declares too many bones");
        result.bones.reserve(bones);
        for (uint32_t i = 0; i < bones; ++i)
            result.bones.push_back({ReadBoundedAsciiz(reader, "bone name"), ReadBoundedAsciiz(reader, "bone parent")});
        // No "obsolete pivots" string at this revision -- the A2 and A3 readers
        // consume one here and revision 40 does not carry it.
    }

    (void)reader.read<uint8_t>(); // map type
    // COL-001: the mass table and totals, kept instead of discarded. A model that
    // arrives with mass 0 is Object::IsPassable() -- a soldier walks through it.
    result.model.massArray = ReadOdol40CompressedArray<float>(reader, "mass array", 1'000'000);
    result.model.mass = reader.read<float>();
    result.model.invMass = reader.read<float>();
    result.model.armor = reader.read<float>();
    result.model.invArmor = reader.read<float>();
    result.model.lodIndexTable.resize(12);
    for (int i = 0; i < 12; ++i)
        result.model.lodIndexTable[i] = static_cast<int8_t>(reader.read<uint8_t>()); // the special-LOD index table
    (void)reader.read<uint32_t>();
    (void)reader.read<bool>();
    result.model.propertyClass = ReadBoundedAsciiz(reader, "class name");
    result.model.propertyDamage = ReadBoundedAsciiz(reader, "damage name");
    (void)reader.read<bool>();
    (void)reader.read<uint32_t>();

    result.hasAnimations = reader.read<bool>();
    // The same shared Real Virtuality animation block the A2 family reads: the
    // two fields revisions 55 and 56 added are gated off at 40, exactly as they
    // are at 49. Reading it is what keeps the LOD offset table that follows on
    // the right offset.
    if (result.hasAnimations)
        result.animations = ReadOdol73Animations(reader, 40);

    result.starts.resize(lodCount);
    result.ends.resize(lodCount);
    result.permanent.resize(lodCount);
    for (uint32_t& offset : result.starts)
        offset = reader.read<uint32_t>();
    for (uint32_t& offset : result.ends)
        offset = reader.read<uint32_t>();
    for (size_t i = 0; i < lodCount; ++i)
        result.permanent[i] = reader.read<bool>();
    for (size_t i = 0; i < lodCount; ++i)
        if (!result.permanent[i])
        {
            (void)reader.read<int32_t>();
            (void)reader.read<uint32_t>();
            (void)reader.read<int32_t>();
            (void)reader.read<uint32_t>();
            (void)reader.read<bool>();
        }
    for (size_t i = 0; i < lodCount; ++i)
        if (result.starts[i] >= result.ends[i] || result.ends[i] > static_cast<uint32_t>(fileSize))
            throw std::runtime_error("ODOL 40 LOD directory has an invalid byte range");
    return result;
}

// The LOD header, up to and including its texture list.  `clip` comes back
// through the out-parameter because at this revision the clip flags live here
// rather than in the rest data, and they are what declare the vertex count.
inline Odol73StaticLodHeader ReadOdol40StaticLodHeader(BinaryReader& reader, uint32_t start, uint32_t end,
                                                      std::vector<int32_t>& clip)
{
    if (start >= end || end > static_cast<uint32_t>(reader.tell() + reader.remaining()))
        throw std::runtime_error("ODOL 40 LOD header range is invalid");
    reader.seek(static_cast<int>(start));
    Odol73StaticLodHeader result;

    const auto readCount = [&reader, end](const char* field, uint32_t limit)
    {
        const uint32_t count = reader.read<uint32_t>();
        if (count > limit || reader.tell() > static_cast<int>(end))
            throw std::runtime_error(std::string("ODOL 40 ") + field + " count is out of range");
        return count;
    };

    const uint32_t proxies = readCount("proxy", 4096);
    result.proxies.reserve(proxies);
    for (uint32_t i = 0; i < proxies; ++i)
    {
        Odol73Proxy proxy;
        proxy.model = ReadBoundedAsciiz(reader, "proxy model");
        for (int row = 0; row < 4; ++row)
            reader.readBytes(&proxy.transform.rows[row], sizeof(Vector3));
        proxy.sequenceId          = reader.read<int32_t>();
        proxy.namedSelectionIndex = reader.read<int32_t>();
        proxy.boneIndex           = reader.read<int32_t>();
        proxy.sectionIndex        = reader.read<int32_t>();
        result.proxies.push_back(std::move(proxy));
    }

    const uint32_t mappings = readCount("sub-skeleton mapping", 65536);
    result.subSkeletonsToSkeleton.resize(mappings);
    for (int32_t& value : result.subSkeletonsToSkeleton)
        value = reader.read<int32_t>();
    const uint32_t sets = readCount("sub-skeleton sets", 65536);
    result.skeletonToSubSkeleton.resize(sets);
    for (auto& set : result.skeletonToSubSkeleton)
    {
        const uint32_t members = readCount("sub-skeleton set member", 65536);
        set.resize(members);
        for (int32_t& value : set)
            value = reader.read<int32_t>();
    }

    clip               = ReadOdol40CondensedArray<int32_t>(reader, "clip flags", 1'000'000);
    result.vertexCount = static_cast<uint32_t>(clip.size());
    result.orHints     = reader.read<int32_t>();
    result.andHints    = reader.read<int32_t>();
    reader.readBytes(&result.bboxMin, sizeof(result.bboxMin));
    reader.readBytes(&result.bboxMax, sizeof(result.bboxMax));
    reader.readBytes(&result.bboxCenter, sizeof(result.bboxCenter));
    result.bboxRadius      = reader.read<float>();
    const uint32_t textures = readCount("texture", 4096);
    result.textures.reserve(textures);
    for (uint32_t i = 0; i < textures; ++i)
        result.textures.push_back(ReadBoundedAsciiz(reader, "texture reference"));
    RequireOdol40Range(reader, end, "LOD header");
    return result;
}

inline std::vector<int32_t> ReadOdol40VertexIndexArray(BinaryReader& reader, const char* field)
{
    const auto raw = ReadOdol40CompressedArray<uint16_t>(reader, field, 1'000'000);
    std::vector<int32_t> values;
    values.reserve(raw.size());
    for (uint16_t value : raw)
        values.push_back(value);
    return values;
}

inline Odol73Polygons ReadOdol40Polygons(BinaryReader& reader, uint32_t vertexCount, uint32_t end)
{
    Odol73Polygons result;
    const int32_t  count = reader.read<int32_t>();
    // The smallest face record is seven bytes (a one-byte count and three 16-bit
    // indices), so a count the range cannot hold is rejected before it allocates.
    if (count < 0 || static_cast<uint64_t>(count) * 7 > static_cast<uint64_t>(end) - reader.tell())
        throw std::runtime_error("ODOL 40 LOD face count does not fit its declared range");
    result.faceDataSize          = reader.read<uint32_t>();
    result.unused                = reader.read<uint16_t>();
    // The units the section bounds are expressed in: two bytes for the vertex
    // count and two per index, as on the A2 family. Measured rather than
    // inherited -- rebuilding the stream at 2/2 reproduces the LOD's own declared
    // faceDataSize for all 18,697 LODs of the A1 corpus, and every one of the
    // 52,394 section faceLower bounds lands on a face boundary in that stream.
    result.faceStreamHeaderBytes = 2;
    result.faceStreamIndexBytes  = 2;
    result.faces.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i)
    {
        const uint8_t vertices = reader.read<uint8_t>();
        if (vertices != 3 && vertices != 4)
            throw std::runtime_error("ODOL 40 LOD has a face that is neither a triangle nor a quad");
        std::vector<uint32_t> face(vertices);
        for (uint32_t& index : face)
        {
            index = reader.read<uint16_t>();
            if (index >= vertexCount)
                throw std::runtime_error("ODOL 40 LOD face references a vertex outside the LOD");
        }
        result.faces.push_back(std::move(face));
    }
    RequireOdol40Range(reader, end, "face stream");
    return result;
}

inline std::vector<Odol73StaticSection> ReadOdol40StaticSections(BinaryReader& reader, uint32_t end)
{
    const uint32_t count = reader.read<uint32_t>();
    if (count > 8192)
        throw std::runtime_error("ODOL 40 LOD has too many sections");
    std::vector<Odol73StaticSection> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        Odol73StaticSection section;
        section.faceLower = reader.read<int32_t>();
        section.faceUpper = reader.read<int32_t>();
        section.minBone   = reader.read<int32_t>();
        section.boneCount = reader.read<int32_t>();
        (void)reader.read<uint32_t>(); // common point flags
        section.textureIndex = reader.read<int16_t>();
        // MAT-046: CommonFaceFlags, same slot and width as revision 73
        // (`Odol73.hpp:918`). Kept rather than discarded -- 0x10000000 is
        // IsHiddenProxy, and dropping this word is why proxy marker triangles
        // draw as spikes on Arma 1 models. ODOLLoader masks it to the two
        // hidden bits before it becomes Section::hints.
        section.commonFaceFlags = reader.read<uint32_t>();
        section.materialIndex = reader.read<int32_t>();
        if (section.materialIndex == -1)
            (void)ReadBoundedAsciiz(reader, "section material");
        const uint32_t areas = reader.read<uint32_t>();
        if (areas > 64)
            throw std::runtime_error("ODOL 40 section area array is too large");
        for (uint32_t area = 0; area < areas; ++area)
            (void)reader.read<float>();
        RequireOdol40Range(reader, end, "section");
        result.push_back(section);
    }
    return result;
}

inline std::vector<Odol73NamedSelection> ReadOdol40NamedSelections(BinaryReader& reader, uint32_t end)
{
    const uint32_t count = reader.read<uint32_t>();
    if (count > 4096)
        throw std::runtime_error("ODOL 40 LOD has too many named selections");
    std::vector<Odol73NamedSelection> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        Odol73NamedSelection selection;
        selection.name          = ReadBoundedAsciiz(reader, "named selection");
        selection.selectedFaces = ReadOdol40VertexIndexArray(reader, "named selection faces");
        (void)reader.read<int32_t>();
        selection.isSectional      = reader.read<bool>();
        selection.sections         = ReadOdol40CompressedArray<int32_t>(reader, "named selection sections", 1'000'000);
        selection.selectedVertices = ReadOdol40VertexIndexArray(reader, "named selection vertices");
        // Sized by its own field, not by the vertex count: an unweighted
        // selection carries no bytes at all.
        const int32_t weights = reader.read<int32_t>();
        if (weights < 0 || static_cast<uint32_t>(weights) > 1'000'000)
            throw std::runtime_error("ODOL 40 named-selection weights are invalid");
        const auto bytes = ReadOdol40Compressed(reader, static_cast<size_t>(weights));
        selection.vertexWeights.assign(bytes.begin(), bytes.end());
        RequireOdol40Range(reader, end, "named selection");
        result.push_back(std::move(selection));
    }
    return result;
}

inline uint32_t ReadOdol40Keyframes(BinaryReader& reader, uint32_t end)
{
    const uint32_t count = reader.read<uint32_t>();
    if (count > 4096)
        throw std::runtime_error("ODOL 40 LOD has too many keyframes");
    for (uint32_t i = 0; i < count; ++i)
    {
        (void)reader.read<float>();
        const uint32_t vertices = reader.read<uint32_t>();
        if (vertices > 1'000'000 || static_cast<uint64_t>(vertices) * sizeof(Vector3) > reader.remaining())
            throw std::runtime_error("ODOL 40 keyframe vertex count is invalid");
        reader.seekRelative(static_cast<int>(vertices * sizeof(Vector3)));
    }
    RequireOdol40Range(reader, end, "keyframe data");
    return count;
}

// A revision-40 UV set: full-float (u,v) pairs, in a condensed array.  Nothing
// is decoded here because nothing is encoded -- the quantised min/max range that
// every later revision carries does not exist at this revision, so Odol73UvSet's
// four range fields are left at zero rather than filled with a derived value
// that the file never declared.
inline Odol73UvSet ReadOdol40UvSet(BinaryReader& reader, uint32_t vertexCount)
{
    struct WireUv
    {
        float u, v;
    };
    static_assert(sizeof(WireUv) == 8, "ODOL 40 UV pair must be two floats on the wire");
    const auto wire = ReadOdol40CondensedArray<WireUv>(reader, "UV set", 1'000'000);
    if (wire.size() != vertexCount)
        throw std::runtime_error("ODOL 40 UV set does not cover the LOD's vertices");
    Odol73UvSet set;
    set.uv.reserve(wire.size());
    for (const auto& pair : wire)
        set.uv.push_back(std::array<float, 2>{pair.u, pair.v});
    return set;
}

inline Odol73StaticRestData ReadOdol40StaticRestData(BinaryReader& reader, const Odol73StaticLodHeader& header,
                                                     const Odol73StaticPreRestData& preRest, uint32_t end)
{
    const int      start     = reader.tell();
    const uint32_t available = end - static_cast<uint32_t>(start);
    // restDataSize counts its own four-byte field: measured over the whole A1
    // corpus, declared minus consumed is 4 for all 18,697 LODs and never any
    // other value. That is what licenses the exact check at the bottom of this
    // function rather than a "close enough" one.
    if (preRest.restDataSize < 4 || preRest.restDataSize - 4 > available)
        throw std::runtime_error("ODOL 40 rest-data range exceeds its LOD");

    Odol73StaticRestData rest;
    rest.uv0        = ReadOdol40UvSet(reader, header.vertexCount);
    rest.uvSetCount = std::max(1u, reader.read<uint32_t>());
    if (rest.uvSetCount > 8)
        throw std::runtime_error("ODOL 40 LOD declares more UV sets than the format uses");
    for (uint32_t set = 1; set < rest.uvSetCount; ++set)
        rest.extraUvSets.push_back(ReadOdol40UvSet(reader, header.vertexCount));

    struct Packed3
    {
        float x, y, z;
    };
    const auto positions = ReadOdol40CompressedArray<Packed3>(reader, "vertex positions", 1'000'000);
    rest.positions.reserve(positions.size());
    for (const auto& position : positions)
        rest.positions.push_back(Vector3{position.x, position.y, position.z});

    // Full-float normals, condensed.  Not the ten-bit-per-axis packed form later
    // revisions use, so DecodeOdol73Normal must never be applied here -- and in
    // particular the negative scale factor that form carries is not present, so
    // these normals are used exactly as stored.
    const auto normals = ReadOdol40CondensedArray<Packed3>(reader, "vertex normals", 1'000'000);
    rest.normals.reserve(normals.size());
    for (const auto& normal : normals)
        rest.normals.push_back(Vector3{normal.x, normal.y, normal.z});

    // ST coordinates: 24 bytes per vertex here rather than the 8-byte packed pair
    // of later revisions. Retained unconverted, as on the v73 path -- no consumer
    // needs them, and naming their basis convention on the strength of their
    // width would be a claim rather than a read.
    rest.stCoordsFull = ReadOdol40CompressedArray<std::array<uint8_t, 24>>(reader, "ST coordinates", 1'000'000);

    struct WireBoneRef
    {
        int32_t                count;
        std::array<uint8_t, 8> data;
    };
    static_assert(sizeof(WireBoneRef) == 12, "ODOL 40 bone reference wire size");
    const auto refs = ReadOdol40CompressedArray<WireBoneRef>(reader, "vertex bone references", 1'000'000);
    rest.vertexBoneRefs.reserve(refs.size());
    for (const auto& ref : refs)
        rest.vertexBoneRefs.push_back({ref.count, ref.data});
    rest.neighbourBoneRefs =
        ReadOdol40CompressedArray<std::array<uint8_t, 32>>(reader, "neighbour bone references", 1'000'000);

    if (rest.positions.size() != header.vertexCount || rest.normals.size() != header.vertexCount)
        throw std::runtime_error("ODOL 40 rest-data arrays disagree with the LOD's vertex count");
    if (reader.tell() - start != static_cast<int>(preRest.restDataSize) - 4)
        throw std::runtime_error("ODOL 40 rest data did not consume its declared size");
    return rest;
}

// One complete revision-40 LOD, start to declared end.  The closing check is the
// whole point: it is what turned a plausible-looking prefix into the measured
// 18,697-of-18,697 closure the port is based on.
inline Odol73StaticLod ReadOdol40StaticLod(BinaryReader& reader, uint32_t start, uint32_t end)
{
    Odol73StaticLod      lod;
    std::vector<int32_t> clip;
    lod.header = ReadOdol40StaticLodHeader(reader, start, end, clip);
    // Byte-identical to revision 73's embedded material (AST-013): every A1
    // material is version 9, and the v73 reader parses all 18,379 of them.
    lod.materials     = ReadOdol73EmbeddedMaterials(reader);
    lod.pointToVertex = ReadOdol40VertexIndexArray(reader, "point-to-vertex map");
    lod.vertexToPoint = ReadOdol40VertexIndexArray(reader, "vertex-to-point map");
    lod.polygons      = ReadOdol40Polygons(reader, lod.header.vertexCount, end);
    lod.sections      = ReadOdol40StaticSections(reader, end);
    lod.namedSelections = ReadOdol40NamedSelections(reader, end);
    lod.namedProperties = ReadOdol73NamedProperties(reader, end);
    lod.observedKeyframes = ReadOdol40Keyframes(reader, end);
    lod.keyframesObserved = true;
    lod.endData.colorTop             = reader.read<int32_t>();
    lod.endData.color                = reader.read<int32_t>();
    lod.endData.special              = reader.read<int32_t>();
    lod.endData.vertexBoneRefIsSimple = reader.read<bool>();
    lod.endData.restDataSize         = reader.read<uint32_t>();
    lod.rest                         = ReadOdol40StaticRestData(reader, lod.header, lod.endData, end);
    lod.rest.clip                    = std::move(clip);
    if (reader.tell() != static_cast<int>(end))
        throw std::runtime_error("ODOL 40 LOD did not close at its declared boundary");
    return lod;
}

inline Odol73StaticModel ReadOdol40StaticModel(BinaryReader& reader, int fileSize)
{
    Odol73StaticModel model;
    model.directory = ReadOdol40StaticDirectory(reader, fileSize);
    model.lods.reserve(model.directory.starts.size());
    for (size_t i = 0; i < model.directory.starts.size(); ++i)
        model.lods.push_back(ReadOdol40StaticLod(reader, model.directory.starts[i], model.directory.ends[i]));
    model.declaredLodBodiesDecoded = true;
    model.decodedRevision = 40;
    return model;
}

} // namespace Poseidon::Asset::Formats::P3D
