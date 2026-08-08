#pragma once

#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/Asset/Formats/BISStructures.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <Poseidon/Foundation/Algorithms/Lzo1x.hpp>
#include <cstdint>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace Poseidon::Asset::Formats::P3D
{

// AST-012B: the first deliberately narrow later-ODOL reader.  Revision 73 puts
// ModelInfo and the LOD directory at the beginning of the file, unlike v7's
// sequential LOD bodies; it must never be sent to readModel().
struct Odol73Preamble
{
    uint32_t appId = 0;
    std::string muzzleFlash;
    std::vector<float> resolutions;
    int32_t special = 0;
    float boundingSphere = 0.0f;
    float geometrySphere = 0.0f;
    int32_t remarks = 0;
    int32_t andHints = 0;
    int32_t orHints = 0;
    Vector3 aimingCenter;
    uint32_t color = 0;
    uint32_t colorType = 0;
    float viewDensity = 0.0f;
    Vector3 bboxMin;
    Vector3 bboxMax;
};

struct Odol73LodDirectory
{
    Odol73Preamble model;
    std::vector<uint32_t> starts;
    std::vector<uint32_t> ends;
    std::vector<bool> permanent;
};

struct Odol73StaticLodHeader
{
    uint32_t vertexCount = 0;
    float faceArea = 0.0f;
    int32_t orHints = 0;
    int32_t andHints = 0;
    Vector3 bboxMin;
    Vector3 bboxMax;
    Vector3 bboxCenter;
    float bboxRadius = 0.0f;
    std::vector<std::string> textures;
};

struct Odol73EmbeddedMaterial
{
    std::string name;
    uint32_t version = 0;
    uint32_t pixelShader = 0;
    uint32_t vertexShader = 0;
    uint32_t renderFlags = 0;
    std::array<std::array<float, 4>, 6> colours{};
    std::vector<std::string> stageTextures;
    // The stage transform is a 3x4 affine matrix (three basis rows then the
    // translation row), twelve floats -- not a padded 4x4.  Reading sixteen here
    // silently consumed 128 bytes too many per material and put every later
    // offset in the LOD body onto a face record that happened to resynchronise.
    struct TexGen { uint32_t uvSource = 0; std::array<float, 12> matrix{}; };
    std::vector<TexGen> texGens;
};

struct Odol73Polygons
{
    // faceDataSize is the size of the face stream in the units the section
    // bounds use: four bytes for the vertex count plus four per index.  That is
    // not this file's on-disk face encoding (one byte for the count), so it is
    // kept under its own name and cross-checked rather than assumed.
    uint32_t faceDataSize = 0;
    uint16_t unused = 0;
    std::vector<std::vector<uint32_t>> faces;
};

struct Odol73StaticSection
{
    int32_t faceLower = 0, faceUpper = 0;
    int32_t minBone = 0, boneCount = 0;
    int16_t textureIndex = 0;
    int32_t materialIndex = 0;
};

struct Odol73StaticPreRestData
{
    int32_t colorTop = 0;
    int32_t color = 0;
    int32_t special = 0;
    bool vertexBoneRefIsSimple = false;
    uint32_t restDataSize = 0;
};

struct Odol73UvSet
{
    float minU = 0.0f, minV = 0.0f, maxU = 0.0f, maxV = 0.0f;
    // Decoded to floats here: the wire form is a pair of 16-bit fractions of the
    // per-set range, which is meaningless without the range it belongs to.
    std::vector<std::array<float, 2>> uv;
};

// Everything behind the LOD's rest-data size field.  Nothing in here is skipped:
// the reader consumes each array and then requires its own position to match the
// declared size, so a layout mistake cannot pass as a successful read.
struct Odol73StaticRestData
{
    std::vector<int32_t> clip;
    Odol73UvSet uv0;
    uint32_t uvSetCount = 0;
    std::vector<Vector3> positions;
    std::vector<Vector3> normals;
    // Packed tangent/binormal pairs, retained unconverted: no consumer needs
    // them yet and guessing their basis convention would be a claim, not a read.
    std::vector<std::array<uint32_t, 2>> stCoords;
    uint32_t trailing = 0;
};

struct Odol73StaticLod
{
    Odol73StaticLodHeader header;
    std::vector<Odol73EmbeddedMaterial> materials;
    std::vector<int32_t> pointToVertex;
    std::vector<int32_t> vertexToPoint;
    Odol73Polygons polygons;
    std::vector<Odol73StaticSection> sections;
    Odol73StaticPreRestData endData;
    Odol73StaticRestData rest;
    uint8_t trailingByte = 0;
};

struct Odol73StaticModel
{
    Odol73LodDirectory directory;
    std::vector<Odol73StaticLod> lods;
};

inline std::string ReadBoundedAsciiz(BinaryReader& reader, const char* field)
{
    std::string value;
    while (true)
    {
        const char c = reader.read<char>();
        if (c == '\0') return value;
        if (value.size() == 4096)
            throw std::runtime_error(std::string("ODOL 73 ") + field + " exceeds 4096 bytes");
        value.push_back(c);
    }
}

// ODOL 73 puts an explicit compression flag before each LZO payload.  The
// decoder reports its explicit LZO end marker, so restore the stream to that
// boundary; no following array is swallowed.
inline std::vector<uint8_t> ReadOdol73CompressedPayload(BinaryReader& reader, size_t expected, bool compressed)
{
    std::vector<uint8_t> output(expected);
    if (!compressed) { reader.readBytes(output.data(), expected); return output; }
    const int start = reader.tell(); const int remaining = reader.remaining();
    std::vector<uint8_t> source(static_cast<size_t>(remaining)); reader.readBytes(source.data(), source.size());
    size_t consumed = 0;
    if (Foundation::Lzo1x::Decompress(source.data(), source.size(), output.data(), output.size(), &consumed) == expected && consumed != 0)
    { reader.seek(start + static_cast<int>(consumed)); return output; }
    throw std::runtime_error("ODOL 73 LZO array did not terminate at the expected size");
}

inline std::vector<uint8_t> ReadOdol73Compressed(BinaryReader& reader, size_t expected)
{
    // A zero-length payload carries no flag byte at all; reading one would eat
    // the count of whatever array follows.
    if (expected == 0) return {};
    return ReadOdol73CompressedPayload(reader, expected, reader.read<bool>());
}

// count, then a (possibly compressed) payload of exactly count elements.
template<typename T>
inline std::vector<T> ReadOdol73CompressedArray(BinaryReader& reader, const char* field, uint32_t limit)
{
    const int32_t count = reader.read<int32_t>();
    if (count < 0 || static_cast<uint32_t>(count) > limit)
        throw std::runtime_error(std::string("ODOL 73 ") + field + " count is out of range");
    const auto bytes = ReadOdol73Compressed(reader, static_cast<size_t>(count) * sizeof(T));
    std::vector<T> result(static_cast<size_t>(count));
    if (count != 0) std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

// As above, but with a leading "every element is this value" flag.
template<typename T>
inline std::vector<T> ReadOdol73CondensedArray(BinaryReader& reader, const char* field, uint32_t limit)
{
    const int32_t count = reader.read<int32_t>();
    if (count < 0 || static_cast<uint32_t>(count) > limit)
        throw std::runtime_error(std::string("ODOL 73 ") + field + " count is out of range");
    if (reader.read<bool>())
    {
        T value{};
        reader.readBytes(&value, sizeof(T));
        return std::vector<T>(static_cast<size_t>(count), value);
    }
    const auto bytes = ReadOdol73Compressed(reader, static_cast<size_t>(count) * sizeof(T));
    std::vector<T> result(static_cast<size_t>(count));
    if (count != 0) std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

inline Odol73Preamble ReadOdol73Preamble(BinaryReader& reader)
{
    if (reader.remaining() < 13)
        throw std::runtime_error("ODOL 73 header truncated");
    char signature[4] = {};
    reader.readBytes(signature, sizeof(signature));
    if (std::memcmp(signature, "ODOL", 4) != 0)
        throw std::runtime_error("ODOL 73 signature expected");
    if (reader.read<uint32_t>() != 73)
        throw std::runtime_error("ODOL 73 reader received another revision");

    Odol73Preamble result;
    result.appId = reader.read<uint32_t>();
    result.muzzleFlash = ReadBoundedAsciiz(reader, "muzzle-flash string");
    const uint32_t lodCount = reader.read<uint32_t>();
    if (lodCount == 0 || lodCount > 100 || static_cast<uint64_t>(lodCount) * sizeof(float) > reader.remaining())
        throw std::runtime_error("ODOL 73 invalid LOD-resolution directory");
    result.resolutions.resize(lodCount);
    for (float& resolution : result.resolutions) resolution = reader.read<float>();

    // The stable prefix of v73 ModelInfo.  Later fields include skeleton and
    // address tables and are intentionally not consumed until the LOD-body slice
    // is implemented; keeping the reader bounded prevents a partial parser from
    // claiming it understood an arbitrary remainder.
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

// Static ModelInfo + LOD directory for the selected 3den rectangle fixture.
// Skeletons and animations are not skipped: accepting bytes whose layout is not
// implemented would make later offsets untrustworthy.
inline Odol73LodDirectory ReadOdol73StaticDirectory(BinaryReader& reader, int fileSize)
{
    Odol73LodDirectory result;
    result.model = ReadOdol73Preamble(reader);
    auto skipFloat = [&reader]() { (void)reader.read<float>(); };
    auto skipVec = [&reader]() { Vector3 ignored; reader.readBytes(&ignored, sizeof(ignored)); };
    skipFloat(); skipFloat();                 // density coefficient, draw importance
    for (int i = 0; i < 5; ++i) skipVec();    // visual bounds, three centres
    for (int i = 0; i < 3; ++i) skipVec();    // inverse inertia rows
    for (int i = 0; i < 5; ++i) (void)reader.read<bool>();
    for (int i = 0; i < 6; ++i) skipFloat();
    (void)reader.read<bool>(); (void)reader.read<int32_t>(); (void)reader.read<bool>(); skipFloat();
    (void)reader.read<bool>();                // animated
    if (!ReadBoundedAsciiz(reader, "skeleton name").empty())
        throw std::runtime_error("ODOL 73 skeletons are not implemented by the static directory reader");
    (void)reader.read<uint8_t>();             // map type
    const auto mass = reader.readCompressedArray<float>();
    (void)mass;
    for (int i = 0; i < 5; ++i) skipFloat();  // mass, inverse mass, armour, inverse armour, explosion shielding
    for (int i = 0; i < 14; ++i) (void)reader.read<uint8_t>();
    (void)reader.read<uint32_t>(); (void)reader.read<bool>();
    (void)ReadBoundedAsciiz(reader, "class name"); (void)ReadBoundedAsciiz(reader, "damage name");
    (void)reader.read<bool>(); (void)reader.read<uint32_t>();
    const size_t count = result.model.resolutions.size();
    for (int array = 0; array < 3; ++array)
        for (size_t i = 0; i < count; ++i) (void)reader.read<int32_t>();
    if (reader.read<bool>())
        throw std::runtime_error("ODOL 73 animations are not implemented by the static directory reader");
    result.starts.resize(count); result.ends.resize(count); result.permanent.resize(count);
    for (uint32_t& offset : result.starts) offset = reader.read<uint32_t>();
    for (uint32_t& offset : result.ends) offset = reader.read<uint32_t>();
    for (size_t i = 0; i < count; ++i) result.permanent[i] = reader.read<bool>();
    for (size_t i = 0; i < count; ++i)
        if (result.starts[i] >= result.ends[i] || result.ends[i] > static_cast<uint32_t>(fileSize))
            throw std::runtime_error("ODOL 73 LOD directory has an invalid byte range");
    return result;
}

inline Odol73StaticLodHeader ReadOdol73StaticLodHeader(BinaryReader& reader, uint32_t start, uint32_t end)
{
    if (start >= end || end > static_cast<uint32_t>(reader.tell() + reader.remaining()))
        throw std::runtime_error("ODOL 73 LOD header range is invalid");
    reader.seek(static_cast<int>(start));
    auto requireEmptyArray = [&reader](const char* name) {
        if (reader.read<uint32_t>() != 0)
            throw std::runtime_error(std::string("ODOL 73 ") + name + " is not implemented by the static LOD reader");
    };
    requireEmptyArray("proxies");
    requireEmptyArray("sub-skeleton mapping");
    requireEmptyArray("sub-skeleton sets");
    Odol73StaticLodHeader result;
    result.vertexCount = reader.read<uint32_t>();
    result.faceArea = reader.read<float>();
    result.orHints = reader.read<int32_t>();
    result.andHints = reader.read<int32_t>();
    reader.readBytes(&result.bboxMin, sizeof(result.bboxMin));
    reader.readBytes(&result.bboxMax, sizeof(result.bboxMax));
    reader.readBytes(&result.bboxCenter, sizeof(result.bboxCenter));
    result.bboxRadius = reader.read<float>();
    const uint32_t textureCount = reader.read<uint32_t>();
    if (textureCount > 64)
        throw std::runtime_error("ODOL 73 LOD has too many texture references");
    result.textures.reserve(textureCount);
    for (uint32_t i = 0; i < textureCount; ++i)
        result.textures.push_back(ReadBoundedAsciiz(reader, "texture reference"));
    if (reader.tell() > static_cast<int>(end))
        throw std::runtime_error("ODOL 73 LOD header exceeds its declared range");
    return result;
}

inline std::vector<Odol73EmbeddedMaterial> ReadOdol73EmbeddedMaterials(BinaryReader& reader)
{
    const uint32_t count = reader.read<uint32_t>();
    if (count > 64) throw std::runtime_error("ODOL 73 LOD has too many embedded materials");
    std::vector<Odol73EmbeddedMaterial> materials; materials.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        Odol73EmbeddedMaterial material;
        material.name = ReadBoundedAsciiz(reader, "embedded material name");
        material.version = reader.read<uint32_t>();
        if (material.version < 6 || material.version > 32)
            throw std::runtime_error("ODOL 73 embedded material revision is unsupported");
        for (auto& colour : material.colours)
            for (float& component : colour) component = reader.read<float>();
        (void)reader.read<float>(); // specular power
        material.pixelShader = reader.read<uint32_t>(); material.vertexShader = reader.read<uint32_t>();
        (void)reader.read<uint32_t>(); (void)reader.read<uint32_t>(); // light + fog
        (void)ReadBoundedAsciiz(reader, "embedded material surface");
        (void)reader.read<uint32_t>(); material.renderFlags = reader.read<uint32_t>();
        const uint32_t stages = material.version > 6 ? reader.read<uint32_t>() : 0;
        const uint32_t texGens = material.version > 8 ? reader.read<uint32_t>() : stages;
        if (stages > 32 || texGens > 32) throw std::runtime_error("ODOL 73 material stage count is invalid");
        for (uint32_t stage = 0; stage < stages; ++stage)
        {
            (void)reader.read<uint32_t>(); // texture filter
            material.stageTextures.push_back(ReadBoundedAsciiz(reader, "embedded stage texture"));
            (void)reader.read<uint32_t>(); // stage id (material revision >= 9 here)
            if (material.version >= 11)
                (void)reader.read<bool>(); // use-world-env-map
        }
        for (uint32_t texGen = 0; texGen < texGens; ++texGen)
        {
            Odol73EmbeddedMaterial::TexGen transform;
            transform.uvSource = reader.read<uint32_t>();
            for (float& element : transform.matrix) element = reader.read<float>();
            material.texGens.push_back(transform);
        }
        if (material.version >= 10)
        {
            (void)reader.read<uint32_t>(); (void)ReadBoundedAsciiz(reader, "thermal stage texture");
            (void)reader.read<uint32_t>();
            if (material.version >= 11)
                (void)reader.read<bool>();
        }
        materials.push_back(std::move(material));
    }
    return materials;
}

// The point/vertex maps that precede the face stream.  Revision 73 is past the
// v69 boundary, so both are 32-bit indices rather than the older 16-bit form.
inline std::vector<int32_t> ReadOdol73VertexIndexArray(BinaryReader& reader, const char* field)
{
    return ReadOdol73CompressedArray<int32_t>(reader, field, 1'000'000);
}

inline Odol73Polygons ReadOdol73Polygons(BinaryReader& reader, uint32_t vertexCount, uint32_t end)
{
    Odol73Polygons result;
    const int32_t count = reader.read<int32_t>();
    // The smallest face record is five bytes, so a count the range cannot hold
    // is rejected before it drives an allocation.
    if (count < 0 || static_cast<uint64_t>(count) * 5 > static_cast<uint64_t>(end) - reader.tell())
        throw std::runtime_error("ODOL 73 LOD face count does not fit its declared range");
    result.faceDataSize = reader.read<uint32_t>();
    result.unused = reader.read<uint16_t>();
    result.faces.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i)
    {
        const uint8_t vertices = reader.read<uint8_t>();
        // Triangles and quads are the only faces the Model IR carries; a larger
        // polygon would have to be triangulated, which is a decision this narrow
        // reader is not entitled to make silently.
        if (vertices != 3 && vertices != 4)
            throw std::runtime_error("ODOL 73 LOD has a face that is neither a triangle nor a quad");
        std::vector<uint32_t> face(vertices);
        for (uint32_t& index : face)
        {
            index = reader.read<uint32_t>();
            if (index >= vertexCount)
                throw std::runtime_error("ODOL 73 LOD face references a vertex outside the LOD");
        }
        result.faces.push_back(std::move(face));
    }
    if (reader.tell() > static_cast<int>(end))
        throw std::runtime_error("ODOL 73 face stream exceeds its declared LOD range");
    return result;
}

inline std::vector<Odol73StaticSection> ReadOdol73StaticSections(BinaryReader& reader, uint32_t end)
{
    const uint32_t count = reader.read<uint32_t>();
    if (count > 64) throw std::runtime_error("ODOL 73 LOD has too many sections");
    std::vector<Odol73StaticSection> result; result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        Odol73StaticSection section;
        section.faceLower = reader.read<int32_t>(); section.faceUpper = reader.read<int32_t>();
        section.minBone = reader.read<int32_t>(); section.boneCount = reader.read<int32_t>();
        (void)reader.read<uint32_t>(); section.textureIndex = reader.read<int16_t>();
        (void)reader.read<uint32_t>(); section.materialIndex = reader.read<int32_t>();
        if (section.materialIndex == -1) (void)ReadBoundedAsciiz(reader, "section material");
        const uint32_t areas = reader.read<uint32_t>();
        if (areas > 64) throw std::runtime_error("ODOL 73 section area array is too large");
        for (uint32_t area = 0; area < areas; ++area) (void)reader.read<float>();
        const int32_t flag67 = reader.read<int32_t>();
        if (flag67 >= 1) for (int j = 0; j < 11; ++j) (void)reader.read<float>();
        if (reader.tell() > static_cast<int>(end)) throw std::runtime_error("ODOL 73 section exceeds LOD range");
        result.push_back(section);
    }
    return result;
}

inline Odol73StaticPreRestData ReadOdol73StaticPreRestData(BinaryReader& reader, uint32_t end)
{
    // The selected static fixture has no named selections, properties or frames.
    // Do not skip a non-empty array: each has its own later-ODOL layout.
    for (const char* field : {"named selections", "named properties", "frames"})
        if (reader.read<uint32_t>() != 0)
            throw std::runtime_error(std::string("ODOL 73 ") + field + " are not implemented");
    Odol73StaticPreRestData result;
    result.colorTop = reader.read<int32_t>(); result.color = reader.read<int32_t>();
    result.special = reader.read<int32_t>(); result.vertexBoneRefIsSimple = reader.read<bool>();
    result.restDataSize = reader.read<uint32_t>();
    if (reader.tell() > static_cast<int>(end)) throw std::runtime_error("ODOL 73 pre-rest data exceeds LOD range");
    return result;
}

inline Odol73UvSet ReadOdol73UvSet(BinaryReader& reader, uint32_t vertexCount)
{
    Odol73UvSet set;
    set.minU = reader.read<float>(); set.minV = reader.read<float>();
    set.maxU = reader.read<float>(); set.maxV = reader.read<float>();
    const uint32_t count = reader.read<uint32_t>();
    if (count != vertexCount)
        throw std::runtime_error("ODOL 73 UV set does not cover the LOD's vertices");
    const bool defaultFill = reader.read<bool>();
    std::vector<uint8_t> raw;
    if (defaultFill)
    {
        raw.resize(4);
        reader.readBytes(raw.data(), raw.size());
        for (uint32_t i = 1; i < count; ++i) raw.insert(raw.end(), raw.begin(), raw.begin() + 4);
    }
    else
    {
        raw = ReadOdol73Compressed(reader, static_cast<size_t>(count) * 4);
    }
    // Each component is a 16-bit fraction of the set's own range.  The +32767
    // and 2^-16 scale are the format's, not a normalisation of our choosing.
    const double deltaU = static_cast<double>(set.maxU) - set.minU;
    const double deltaV = static_cast<double>(set.maxV) - set.minV;
    set.uv.resize(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        int16_t u = 0, v = 0;
        std::memcpy(&u, raw.data() + static_cast<size_t>(i) * 4, 2);
        std::memcpy(&v, raw.data() + static_cast<size_t>(i) * 4 + 2, 2);
        set.uv[i][0] = static_cast<float>(1.52587890625e-05 * (u + 32767) * deltaU + set.minU);
        set.uv[i][1] = static_cast<float>(1.52587890625e-05 * (v + 32767) * deltaV + set.minV);
    }
    return set;
}

// Revision 73 packs a normal into ten bits per axis with a negative scale; that
// sign is the format's, and dropping it would flip every normal in the model.
inline Vector3 DecodeOdol73Normal(uint32_t packed)
{
    auto axis = [](uint32_t value) {
        int component = static_cast<int>(value & 0x3FFu);
        if (component > 511) component -= 1024;
        return static_cast<float>(component) * (-1.0f / 511.0f);
    };
    Vector3 normal;
    normal.x = axis(packed);
    normal.y = axis(packed >> 10);
    normal.z = axis(packed >> 20);
    return normal;
}

inline Odol73StaticRestData ReadOdol73StaticRestData(BinaryReader& reader, const Odol73StaticLodHeader& header,
                                                     const Odol73StaticPreRestData& preRest, uint32_t end)
{
    const int start = reader.tell();
    if (static_cast<uint64_t>(start) + preRest.restDataSize > end)
        throw std::runtime_error("ODOL 73 rest-data range exceeds its declared LOD range");

    Odol73StaticRestData rest;
    rest.clip = ReadOdol73CondensedArray<int32_t>(reader, "clip flags", 1'000'000);
    rest.uv0 = ReadOdol73UvSet(reader, header.vertexCount);
    rest.uvSetCount = reader.read<uint32_t>();
    // A second UV set has its own payload; promoting only the first would drop
    // exactly the channel a material's uvSource selects.
    if (rest.uvSetCount > 1)
        throw std::runtime_error("ODOL 73 additional UV sets are not implemented by the static LOD reader");

    struct Packed3 { float x, y, z; };
    const auto positions = ReadOdol73CompressedArray<Packed3>(reader, "vertex positions", 1'000'000);
    rest.positions.reserve(positions.size());
    for (const auto& position : positions) rest.positions.push_back(Vector3{position.x, position.y, position.z});

    const auto normals = ReadOdol73CondensedArray<uint32_t>(reader, "vertex normals", 1'000'000);
    rest.normals.reserve(normals.size());
    for (uint32_t packed : normals) rest.normals.push_back(DecodeOdol73Normal(packed));

    struct PackedSt { uint32_t s, t; };
    const auto st = ReadOdol73CompressedArray<PackedSt>(reader, "ST coordinates", 1'000'000);
    rest.stCoords.reserve(st.size());
    for (const auto& pair : st) rest.stCoords.push_back({pair.s, pair.t});

    // Skinning: absent in the static subset, and its per-bone layout is not
    // implemented, so a populated array is a refusal rather than a skip.
    if (!ReadOdol73CompressedArray<std::array<uint8_t, 12>>(reader, "vertex bone references", 1'000'000).empty())
        throw std::runtime_error("ODOL 73 vertex bone references are not implemented by the static LOD reader");
    if (!ReadOdol73CompressedArray<std::array<uint8_t, 32>>(reader, "neighbour bone references", 1'000'000).empty())
        throw std::runtime_error("ODOL 73 neighbour bone references are not implemented by the static LOD reader");
    rest.trailing = reader.read<uint32_t>();

    if (rest.positions.size() != header.vertexCount || rest.normals.size() != header.vertexCount ||
        rest.clip.size() != header.vertexCount)
        throw std::runtime_error("ODOL 73 rest-data arrays disagree with the LOD's vertex count");
    // The decisive check: the LOD declared how many bytes this section spans, so
    // a layout error cannot be mistaken for a successful read.
    if (reader.tell() - start != static_cast<int>(preRest.restDataSize))
        throw std::runtime_error("ODOL 73 rest data did not consume its declared size");
    return rest;
}

inline uint8_t ReadOdol73TrailingLodByte(BinaryReader& reader, uint32_t end)
{
    if (reader.tell() != static_cast<int>(end) - 1)
        throw std::runtime_error("ODOL 73 trailing LOD field is not at the declared boundary");
    const uint8_t value = reader.read<uint8_t>();
    if (reader.tell() != static_cast<int>(end))
        throw std::runtime_error("ODOL 73 trailing LOD field did not close the declared range");
    return value;
}

// One complete static LOD body, start to declared end.  Every field between the
// two offsets is consumed; the closing check is what makes the read a fact
// rather than a plausible-looking prefix.
inline Odol73StaticLod ReadOdol73StaticLod(BinaryReader& reader, uint32_t start, uint32_t end)
{
    Odol73StaticLod lod;
    lod.header = ReadOdol73StaticLodHeader(reader, start, end);
    lod.materials = ReadOdol73EmbeddedMaterials(reader);
    lod.pointToVertex = ReadOdol73VertexIndexArray(reader, "point-to-vertex map");
    lod.vertexToPoint = ReadOdol73VertexIndexArray(reader, "vertex-to-point map");
    lod.polygons = ReadOdol73Polygons(reader, lod.header.vertexCount, end);
    lod.sections = ReadOdol73StaticSections(reader, end);
    lod.endData = ReadOdol73StaticPreRestData(reader, end);
    lod.rest = ReadOdol73StaticRestData(reader, lod.header, lod.endData, end);
    lod.trailingByte = ReadOdol73TrailingLodByte(reader, end);
    return lod;
}

inline Odol73StaticModel ReadOdol73StaticModel(BinaryReader& reader, int fileSize)
{
    Odol73StaticModel model;
    model.directory = ReadOdol73StaticDirectory(reader, fileSize);
    model.lods.reserve(model.directory.starts.size());
    for (size_t i = 0; i < model.directory.starts.size(); ++i)
        model.lods.push_back(ReadOdol73StaticLod(reader, model.directory.starts[i], model.directory.ends[i]));
    return model;
}

// Face-stream offsets, in the units the section bounds use: four bytes for the
// vertex count plus four per index.  Returned so section ranges can be turned
// into face indices instead of being carried around as opaque numbers; the
// caller checks the total against the LOD's own declared face-data size.
inline std::vector<uint32_t> Odol73FaceStreamOffsets(const Odol73Polygons& polygons)
{
    std::vector<uint32_t> offsets;
    offsets.reserve(polygons.faces.size() + 1);
    uint32_t offset = 0;
    for (const auto& face : polygons.faces)
    {
        offsets.push_back(offset);
        offset += 4 + 4 * static_cast<uint32_t>(face.size());
    }
    offsets.push_back(offset);
    return offsets;
}

inline Material::RvMaterialSource ToRvMaterialSource(const Odol73EmbeddedMaterial& raw)
{
    Material::RvMaterialSource source;
    source.origin = raw.name; source.embedded = true;
    source.emissive = raw.colours[0]; source.ambient = raw.colours[1]; source.diffuse = raw.colours[2];
    source.forcedDiffuse = raw.colours[3]; source.specular = raw.colours[4];
    source.extra.emplace_back("odolPixelShader", std::to_string(raw.pixelShader));
    source.extra.emplace_back("odolVertexShader", std::to_string(raw.vertexShader));
    source.extra.emplace_back("odolRenderFlags", std::to_string(raw.renderFlags));
    for (size_t i = 0; i < raw.stageTextures.size(); ++i)
    {
        Material::RvStage stage; stage.index = static_cast<int>(i); stage.texture = Material::RvTextureRef::Parse(raw.stageTextures[i]);
        source.stages.push_back(std::move(stage));
    }
    for (const auto& rawGen : raw.texGens)
    {
        Material::RvTexGen gen; gen.index = static_cast<int>(source.texGens.size()); gen.uvSource = std::to_string(rawGen.uvSource);
        gen.uvTransform.present = true;
        for (int c = 0; c < 3; ++c) { gen.uvTransform.aside[c] = rawGen.matrix[c]; gen.uvTransform.up[c] = rawGen.matrix[3 + c]; gen.uvTransform.dir[c] = rawGen.matrix[6 + c]; gen.uvTransform.pos[c] = rawGen.matrix[9 + c]; }
        source.texGens.push_back(std::move(gen));
    }
    return source;
}

} // namespace Poseidon::Asset::Formats::P3D
