#pragma once

// Exact static-model reader for ODOL revisions 49, 50 and 52. It deliberately
// does not accept v48 or later Arma 3 layouts.

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

inline std::vector<uint8_t> ReadOdol49Compressed(BinaryReader& reader, size_t expected)
{
    if (expected == 0) return {};
    std::vector<uint8_t> output(expected);
    // This generation precedes ODOL's per-payload compression flag: decoded
    // buffers below 1024 bytes are raw, larger ones are LZO.
    if (expected < 1024)
    {
        reader.readBytes(output.data(), expected);
        return output;
    }
    const int start = reader.tell();
    std::vector<uint8_t> source(static_cast<size_t>(reader.remaining()));
    reader.readBytes(source.data(), source.size());
    size_t consumed = 0;
    if (Foundation::Lzo1x::Decompress(source.data(), source.size(), output.data(), output.size(), &consumed) == expected && consumed != 0)
    {
        reader.seek(start + static_cast<int>(consumed));
        return output;
    }
    throw std::runtime_error("ODOL A2 LZO array did not terminate at the expected size");
}

template <typename T>
inline std::vector<T> ReadOdol49CompressedArray(BinaryReader& reader, const char* field, uint32_t limit)
{
    const int32_t count = reader.read<int32_t>();
    if (count < 0 || static_cast<uint32_t>(count) > limit)
        throw std::runtime_error(std::string("ODOL A2 ") + field + " count is out of range");
    const auto bytes = ReadOdol49Compressed(reader, static_cast<size_t>(count) * sizeof(T));
    std::vector<T> values(static_cast<size_t>(count));
    if (count != 0) std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
}

template <typename T>
inline std::vector<T> ReadOdol49CondensedArray(BinaryReader& reader, const char* field, uint32_t limit)
{
    const int32_t count = reader.read<int32_t>();
    if (count < 0 || static_cast<uint32_t>(count) > limit)
        throw std::runtime_error(std::string("ODOL A2 ") + field + " count is out of range");
    if (reader.read<bool>())
    {
        T value{};
        reader.readBytes(&value, sizeof(T));
        return std::vector<T>(static_cast<size_t>(count), value);
    }
    const auto bytes = ReadOdol49Compressed(reader, static_cast<size_t>(count) * sizeof(T));
    std::vector<T> values(static_cast<size_t>(count));
    if (count != 0) std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
}

inline void RequireOdol49Range(const BinaryReader& reader, uint32_t end, const char* field)
{
    if (reader.tell() > static_cast<int>(end))
        throw std::runtime_error(std::string("ODOL A2 ") + field + " exceeds its declared LOD range");
}

inline Odol73LodDirectory ReadOdol49StaticDirectory(BinaryReader& reader, int fileSize, uint32_t expectedRevision)
{
    char signature[4] = {};
    reader.readBytes(signature, sizeof(signature));
    if (std::memcmp(signature, "ODOL", 4) != 0 || reader.read<uint32_t>() != expectedRevision)
        throw std::runtime_error("ODOL A2-family signature and exact revision expected");
    Odol73LodDirectory result;
    const uint32_t lodCount = reader.read<uint32_t>();
    if (lodCount == 0 || lodCount > 100 || static_cast<uint64_t>(lodCount) * sizeof(float) > reader.remaining())
        throw std::runtime_error("ODOL A2 invalid LOD-resolution directory");
    result.model.resolutions.resize(lodCount);
    for (float& resolution : result.model.resolutions) resolution = reader.read<float>();
    auto readVector = [&reader](Vector3& value) { reader.readBytes(&value, sizeof(value)); };
    auto skipVector = [&reader]() { Vector3 unused; reader.readBytes(&unused, sizeof(unused)); };
    result.model.special = reader.read<int32_t>(); result.model.boundingSphere = reader.read<float>();
    result.model.geometrySphere = reader.read<float>(); result.model.remarks = reader.read<int32_t>();
    result.model.andHints = reader.read<int32_t>(); result.model.orHints = reader.read<int32_t>();
    readVector(result.model.aimingCenter); result.model.color = reader.read<uint32_t>();
    result.model.colorType = reader.read<uint32_t>(); result.model.viewDensity = reader.read<float>();
    readVector(result.model.bboxMin); readVector(result.model.bboxMax);
    if (expectedRevision >= 52) { skipVector(); skipVector(); }
    // Bounding centre, geometry centre (still skipped -- see the revision-73
    // reader for why), then centre of mass and the inverse inertia rows (COL-001).
    skipVector(); skipVector();
    readVector(result.model.centerOfMass);
    for (int i = 0; i < 3; ++i) readVector(result.model.invInertia[i]);
    // Revision 54 (DayZ) widens three fixed-size runs by six bytes in total: one
    // more bool here, one more float after them, and one more filler byte after
    // the mass array. Measured, not inferred from the revision number -- the
    // three counts were fitted against the LOD byte-offset table, which is
    // located independently by its tiling constraint, and the fit closes the
    // directory exactly on the first byte of LOD data for 8,203 of 8,203 models
    // in the DayZ corpus. Getting any one of them wrong moves that landing point.
    const bool dayz = expectedRevision >= 54;
    for (int i = 0; i < (dayz ? 5 : 4); ++i) (void)reader.read<bool>();
    for (int i = 0; i < (dayz ? 7 : 6); ++i) (void)reader.read<float>();
    (void)reader.read<bool>(); (void)reader.read<int32_t>(); (void)reader.read<bool>();
    (void)reader.read<float>(); (void)reader.read<bool>();
    result.skeletonName = ReadBoundedAsciiz(reader, "skeleton name");
    if (!result.skeletonName.empty())
    {
        result.skeletonIsDiscrete = reader.read<bool>(); const uint32_t bones = reader.read<uint32_t>();
        if (bones > 4096) throw std::runtime_error("ODOL A2 skeleton declares too many bones");
        result.bones.reserve(bones);
        for (uint32_t i = 0; i < bones; ++i)
            result.bones.push_back({ReadBoundedAsciiz(reader, "bone name"), ReadBoundedAsciiz(reader, "bone parent")});
        (void)ReadBoundedAsciiz(reader, "obsolete pivots name");
    }
    (void)reader.read<uint8_t>(); // map type
    // COL-001: the mass table and totals, kept instead of discarded. A model that
    // arrives with mass 0 is Object::IsPassable() -- a soldier walks through it.
    result.model.massArray = ReadOdol49CompressedArray<float>(reader, "mass array", 1'000'000);
    result.model.mass = reader.read<float>(); result.model.invMass = reader.read<float>();
    result.model.armor = reader.read<float>(); result.model.invArmor = reader.read<float>();
    result.model.lodIndexTable.resize(dayz ? 13 : 12);
    for (int i = 0; i < (dayz ? 13 : 12); ++i) result.model.lodIndexTable[i] = static_cast<int8_t>(reader.read<uint8_t>());
    (void)reader.read<uint32_t>(); (void)reader.read<bool>();
    result.model.propertyClass = ReadBoundedAsciiz(reader, "class name");
    result.model.propertyDamage = ReadBoundedAsciiz(reader, "damage name"); (void)reader.read<bool>(); (void)reader.read<uint32_t>();
    result.hasAnimations = reader.read<bool>();
    // The animation block is the same Real Virtuality structure revision 73 reads,
    // minus the fields revisions 55 and 56 added. Reading it rather than refusing it
    // is what lets an animated prop contribute its static geometry: on Takistan that
    // is 54 of the world's 534 referenced models -- houses, minarets, street lamps --
    // ordinary static meshes whose only sin is declaring a door or a light source.
    if (result.hasAnimations) result.animations = ReadOdol73Animations(reader, expectedRevision);
    result.starts.resize(lodCount); result.ends.resize(lodCount); result.permanent.resize(lodCount);
    for (uint32_t& offset : result.starts) offset = reader.read<uint32_t>();
    for (uint32_t& offset : result.ends) offset = reader.read<uint32_t>();
    for (size_t i = 0; i < lodCount; ++i) result.permanent[i] = reader.read<bool>();
    for (size_t i = 0; i < lodCount; ++i) if (!result.permanent[i])
    {
        (void)reader.read<int32_t>(); (void)reader.read<uint32_t>(); (void)reader.read<int32_t>();
        (void)reader.read<uint32_t>(); (void)reader.read<bool>();
        if (expectedRevision >= 51) { (void)reader.read<int32_t>(); (void)reader.read<float>(); }
    }
    for (size_t i = 0; i < lodCount; ++i)
        if (result.starts[i] >= result.ends[i] || result.ends[i] > static_cast<uint32_t>(fileSize))
            throw std::runtime_error("ODOL A2 LOD directory has an invalid byte range");
    return result;
}

inline Odol73StaticLodHeader ReadOdol49StaticLodHeader(BinaryReader& reader, uint32_t start, uint32_t end,
                                                        std::vector<int32_t>& oldClip, uint32_t revision)
{
    if (start >= end) throw std::runtime_error("ODOL A2 LOD header range is invalid");
    reader.seek(static_cast<int>(start)); Odol73StaticLodHeader result;
    const auto readCount = [&reader, end](const char* field, uint32_t limit)
    {
        const uint32_t count = reader.read<uint32_t>();
        if (count > limit || reader.tell() > static_cast<int>(end)) throw std::runtime_error(std::string("ODOL A2 ") + field + " count is out of range");
        return count;
    };
    const uint32_t proxies = readCount("proxy", 4096); result.proxies.reserve(proxies);
    for (uint32_t i = 0; i < proxies; ++i)
    {
        Odol73Proxy proxy; proxy.model = ReadBoundedAsciiz(reader, "proxy model");
        for (int row = 0; row < 4; ++row) reader.readBytes(&proxy.transform.rows[row], sizeof(Vector3));
        proxy.sequenceId = reader.read<int32_t>(); proxy.namedSelectionIndex = reader.read<int32_t>();
        proxy.boneIndex = reader.read<int32_t>(); proxy.sectionIndex = reader.read<int32_t>(); result.proxies.push_back(std::move(proxy));
    }
    const uint32_t mappings = readCount("sub-skeleton mapping", 65536); result.subSkeletonsToSkeleton.resize(mappings);
    for (int32_t& value : result.subSkeletonsToSkeleton) value = reader.read<int32_t>();
    const uint32_t sets = readCount("sub-skeleton sets", 65536); result.skeletonToSubSkeleton.resize(sets);
    for (auto& set : result.skeletonToSubSkeleton) { const uint32_t members = readCount("sub-skeleton set member", 65536); set.resize(members); for (int32_t& value : set) value = reader.read<int32_t>(); }
    if (revision < 50) { oldClip = ReadOdol49CondensedArray<int32_t>(reader, "legacy clip flags", 1'000'000); result.vertexCount = static_cast<uint32_t>(oldClip.size()); }
    else { result.vertexCount = reader.read<uint32_t>(); if (result.vertexCount > 1'000'000) throw std::runtime_error("ODOL A2 LOD vertex count is out of range"); }
    if (revision >= 51) (void)reader.read<float>();
    result.orHints = reader.read<int32_t>(); result.andHints = reader.read<int32_t>();
    reader.readBytes(&result.bboxMin, sizeof(result.bboxMin)); reader.readBytes(&result.bboxMax, sizeof(result.bboxMax));
    reader.readBytes(&result.bboxCenter, sizeof(result.bboxCenter)); result.bboxRadius = reader.read<float>();
    const uint32_t textures = readCount("texture", 4096); result.textures.reserve(textures);
    for (uint32_t i = 0; i < textures; ++i) result.textures.push_back(ReadBoundedAsciiz(reader, "texture reference"));
    RequireOdol49Range(reader, end, "LOD header"); return result;
}

inline std::vector<int32_t> ReadOdol49VertexIndexArray(BinaryReader& reader, const char* field)
{
    const auto raw = ReadOdol49CompressedArray<uint16_t>(reader, field, 1'000'000);
    std::vector<int32_t> values; values.reserve(raw.size()); for (uint16_t value : raw) values.push_back(value); return values;
}

inline Odol73Polygons ReadOdol49Polygons(BinaryReader& reader, uint32_t vertexCount, uint32_t end)
{
    Odol73Polygons result; const int32_t count = reader.read<int32_t>();
    if (count < 0 || static_cast<uint64_t>(count) * 7 > static_cast<uint64_t>(end) - reader.tell()) throw std::runtime_error("ODOL A2 LOD face count does not fit its declared range");
    result.faceDataSize = reader.read<uint32_t>(); result.unused = reader.read<uint16_t>(); result.faceStreamHeaderBytes = 2; result.faceStreamIndexBytes = 2;
    result.faces.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) { const uint8_t vertices = reader.read<uint8_t>(); if (vertices != 3 && vertices != 4) throw std::runtime_error("ODOL A2 LOD has a non-triangle/non-quad face"); std::vector<uint32_t> face(vertices); for (uint32_t& index : face) { index = reader.read<uint16_t>(); if (index >= vertexCount) throw std::runtime_error("ODOL A2 LOD face references a vertex outside the LOD"); } result.faces.push_back(std::move(face)); }
    RequireOdol49Range(reader, end, "face stream"); return result;
}

inline std::vector<Odol73StaticSection> ReadOdol49StaticSections(BinaryReader& reader, uint32_t end)
{
    const uint32_t count = reader.read<uint32_t>(); if (count > 8192) throw std::runtime_error("ODOL A2 LOD has too many sections");
    std::vector<Odol73StaticSection> result; result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) { Odol73StaticSection section; section.faceLower = reader.read<int32_t>(); section.faceUpper = reader.read<int32_t>(); section.minBone = reader.read<int32_t>(); section.boneCount = reader.read<int32_t>(); (void)reader.read<uint32_t>(); section.textureIndex = reader.read<int16_t>(); section.commonFaceFlags = reader.read<uint32_t>(); /* MAT-046: CommonFaceFlags, same slot as rev 73 (Odol73.hpp:918); 0x10000000 is IsHiddenProxy. Dropping it is why proxy markers draw on Arma 2/OA models. ODOLLoader masks it to the hidden bits. */ section.materialIndex = reader.read<int32_t>(); if (section.materialIndex == -1) (void)ReadBoundedAsciiz(reader, "section material"); const uint32_t areas = reader.read<uint32_t>(); if (areas > 64) throw std::runtime_error("ODOL A2 section area array is too large"); for (uint32_t area = 0; area < areas; ++area) (void)reader.read<float>(); RequireOdol49Range(reader, end, "section"); result.push_back(section); }
    return result;
}

inline std::vector<Odol73NamedSelection> ReadOdol49NamedSelections(BinaryReader& reader, uint32_t end)
{
    const uint32_t count = reader.read<uint32_t>(); if (count > 4096) throw std::runtime_error("ODOL A2 LOD has too many named selections");
    std::vector<Odol73NamedSelection> result; result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) { Odol73NamedSelection selection; selection.name = ReadBoundedAsciiz(reader, "named selection"); selection.selectedFaces = ReadOdol49VertexIndexArray(reader, "named selection faces"); (void)reader.read<int32_t>(); selection.isSectional = reader.read<bool>(); selection.sections = ReadOdol49CompressedArray<int32_t>(reader, "named selection sections", 1'000'000); selection.selectedVertices = ReadOdol49VertexIndexArray(reader, "named selection vertices"); const int32_t weights = reader.read<int32_t>(); if (weights < 0 || static_cast<uint32_t>(weights) > 1'000'000) throw std::runtime_error("ODOL A2 named-selection weights are invalid"); const auto bytes = ReadOdol49Compressed(reader, static_cast<size_t>(weights)); selection.vertexWeights.assign(bytes.begin(), bytes.end()); RequireOdol49Range(reader, end, "named selection"); result.push_back(std::move(selection)); }
    return result;
}

inline uint32_t ReadOdol49Keyframes(BinaryReader& reader, uint32_t end)
{
    const uint32_t count = reader.read<uint32_t>(); if (count > 4096) throw std::runtime_error("ODOL A2 LOD has too many keyframes");
    for (uint32_t i = 0; i < count; ++i) { (void)reader.read<float>(); const uint32_t vertices = reader.read<uint32_t>(); if (vertices > 1'000'000 || static_cast<uint64_t>(vertices) * sizeof(Vector3) > reader.remaining()) throw std::runtime_error("ODOL A2 keyframe vertex count is invalid"); reader.seekRelative(static_cast<int>(vertices * sizeof(Vector3))); }
    RequireOdol49Range(reader, end, "keyframe data");
    return count;
}

inline Odol73StaticRestData ReadOdol49StaticRestData(BinaryReader& reader, const Odol73StaticLodHeader& header, const Odol73StaticPreRestData& preRest, uint32_t end, bool hasRestClip)
{
    const int start = reader.tell(); const uint32_t available = end - static_cast<uint32_t>(start);
    const uint32_t effectiveSize = preRest.restDataSize == available + sizeof(uint32_t) ? available : preRest.restDataSize;
    if (effectiveSize > available) throw std::runtime_error("ODOL A2 rest-data range exceeds its LOD");
    Odol73StaticRestData rest; if (hasRestClip) rest.clip = ReadOdol49CondensedArray<int32_t>(reader, "clip flags", 1'000'000);
    const auto readUvSet = [&reader, &header]() { Odol73UvSet set; set.minU = reader.read<float>(); set.minV = reader.read<float>(); set.maxU = reader.read<float>(); set.maxV = reader.read<float>(); const uint32_t vertices = reader.read<uint32_t>(); if (vertices != header.vertexCount) throw std::runtime_error("ODOL A2 UV set does not cover the LOD's vertices"); const bool defaultFill = reader.read<bool>(); std::vector<uint8_t> raw(static_cast<size_t>(vertices) * 4); if (defaultFill) { reader.readBytes(raw.data(), 4); for (uint32_t i = 1; i < vertices; ++i) std::memcpy(raw.data() + static_cast<size_t>(i) * 4, raw.data(), 4); } else raw = ReadOdol49Compressed(reader, raw.size()); const double du = static_cast<double>(set.maxU) - set.minU; const double dv = static_cast<double>(set.maxV) - set.minV; set.uv.resize(vertices); for (uint32_t i = 0; i < vertices; ++i) { int16_t u = 0, v = 0; std::memcpy(&u, raw.data() + static_cast<size_t>(i) * 4, 2); std::memcpy(&v, raw.data() + static_cast<size_t>(i) * 4 + 2, 2); set.uv[i][0] = static_cast<float>(1.52587890625e-05 * (u + 32767) * du + set.minU); set.uv[i][1] = static_cast<float>(1.52587890625e-05 * (v + 32767) * dv + set.minV); } return set; };
    rest.uv0 = readUvSet(); rest.uvSetCount = std::max(1u, reader.read<uint32_t>()); if (rest.uvSetCount > 8) throw std::runtime_error("ODOL A2 LOD declares too many UV sets"); for (uint32_t i = 1; i < rest.uvSetCount; ++i) rest.extraUvSets.push_back(readUvSet());
    struct Packed3 { float x, y, z; }; const auto positions = ReadOdol49CompressedArray<Packed3>(reader, "vertex positions", 1'000'000); rest.positions.reserve(positions.size()); for (const auto& p : positions) rest.positions.push_back({p.x, p.y, p.z});
    const auto normals = ReadOdol49CondensedArray<uint32_t>(reader, "vertex normals", 1'000'000); rest.normals.reserve(normals.size()); for (uint32_t n : normals) rest.normals.push_back(DecodeOdol73Normal(n));
    struct PackedSt { uint32_t s, t; }; const auto st = ReadOdol49CompressedArray<PackedSt>(reader, "ST coordinates", 1'000'000); rest.stCoords.reserve(st.size()); for (const auto& s : st) rest.stCoords.push_back({s.s, s.t});
    struct WireBoneRef { int32_t count; std::array<uint8_t, 8> data; }; static_assert(sizeof(WireBoneRef) == 12, "ODOL A2 bone reference wire size");
    const auto refs = ReadOdol49CompressedArray<WireBoneRef>(reader, "vertex bone references", 1'000'000); rest.vertexBoneRefs.reserve(refs.size()); for (const auto& r : refs) rest.vertexBoneRefs.push_back({r.count, r.data});
    rest.neighbourBoneRefs = ReadOdol49CompressedArray<std::array<uint8_t, 32>>(reader, "neighbour bone references", 1'000'000);
    if (rest.positions.size() != header.vertexCount || rest.normals.size() != header.vertexCount || (hasRestClip && rest.clip.size() != header.vertexCount)) throw std::runtime_error("ODOL A2 rest-data arrays disagree with the LOD's vertex count");
    if (reader.tell() - start != static_cast<int>(effectiveSize)) throw std::runtime_error("ODOL A2 rest data did not consume its declared size"); return rest;
}

inline Odol73StaticLod ReadOdol49StaticLod(BinaryReader& reader, uint32_t start, uint32_t end, uint32_t revision)
{
    Odol73StaticLod lod; std::vector<int32_t> oldClip; const bool hasRestClip = revision >= 50;
    lod.header = ReadOdol49StaticLodHeader(reader, start, end, oldClip, revision); lod.materials = ReadOdol73EmbeddedMaterials(reader);
    lod.pointToVertex = ReadOdol49VertexIndexArray(reader, "point-to-vertex map"); lod.vertexToPoint = ReadOdol49VertexIndexArray(reader, "vertex-to-point map"); lod.polygons = ReadOdol49Polygons(reader, lod.header.vertexCount, end); lod.sections = ReadOdol49StaticSections(reader, end); lod.namedSelections = ReadOdol49NamedSelections(reader, end); lod.namedProperties = ReadOdol73NamedProperties(reader, end); lod.observedKeyframes = ReadOdol49Keyframes(reader, end); lod.keyframesObserved = true;
    lod.endData.colorTop = reader.read<int32_t>(); lod.endData.color = reader.read<int32_t>(); lod.endData.special = reader.read<int32_t>(); lod.endData.vertexBoneRefIsSimple = reader.read<bool>(); lod.endData.restDataSize = reader.read<uint32_t>();
    lod.rest = ReadOdol49StaticRestData(reader, lod.header, lod.endData, end, hasRestClip); if (!hasRestClip) lod.rest.clip = std::move(oldClip);
    if (reader.tell() != static_cast<int>(end)) throw std::runtime_error("ODOL A2 LOD did not close at its declared boundary"); return lod;
}

inline Odol73StaticModel ReadOdolA2StaticModel(BinaryReader& reader, int fileSize, uint32_t revision)
{
    Odol73StaticModel model; model.directory = ReadOdol49StaticDirectory(reader, fileSize, revision); model.lods.reserve(model.directory.starts.size()); for (size_t i = 0; i < model.directory.starts.size(); ++i) model.lods.push_back(ReadOdol49StaticLod(reader, model.directory.starts[i], model.directory.ends[i], revision)); model.declaredLodBodiesDecoded = true; model.decodedRevision = revision; return model;
}
inline Odol73StaticModel ReadOdol48StaticModel(BinaryReader& reader, int fileSize) { return ReadOdolA2StaticModel(reader, fileSize, 48); }
inline Odol73StaticModel ReadOdol49StaticModel(BinaryReader& reader, int fileSize) { return ReadOdolA2StaticModel(reader, fileSize, 49); }
inline Odol73StaticModel ReadOdol50StaticModel(BinaryReader& reader, int fileSize) { return ReadOdolA2StaticModel(reader, fileSize, 50); }
inline Odol73StaticModel ReadOdol52StaticModel(BinaryReader& reader, int fileSize) { return ReadOdolA2StaticModel(reader, fileSize, 52); }
// DayZ. It belongs to this family and not to revision 73's: its prologue carries
// no appId and no muzzle-flash string, it has none of revision 73's three
// per-LOD index arrays, and it is below both animation gates (55's hide value,
// 56's period/phase pair), so the shared animation reader needs no new case.
// The one structural surprise is that DayZ writes its LODs in descending file
// order, which costs nothing here because every LOD is seeked to by its own
// declared offset.
inline Odol73StaticModel ReadOdol54StaticModel(BinaryReader& reader, int fileSize) { return ReadOdolA2StaticModel(reader, fileSize, 54); }

} // namespace Poseidon::Asset::Formats::P3D
