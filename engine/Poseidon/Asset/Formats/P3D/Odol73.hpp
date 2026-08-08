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
    struct TexGen { uint32_t uvSource = 0; std::array<float, 16> matrix{}; };
    std::vector<TexGen> texGens;
};

struct Odol73StaticPolygonPrefix
{
    // The selected v73 LOD has a one-byte index-stream marker and an adjacent
    // 32-bit value before its polygon count.  Their semantics are intentionally
    // retained as raw data until point/vertex mappings are independently
    // verified; polygons must not silently claim they are vertex indices.
    bool indexStreamMarker = false;
    uint32_t unparsedIndexValue = 0;
    std::vector<std::vector<uint32_t>> polygons;
    uint32_t followingByteOffset = 0;
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
    return ReadOdol73CompressedPayload(reader, expected, reader.read<bool>());
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

// Fixture-led static polygon prefix.  The leading value is retained as raw
// metadata: the selected body changes layout after eleven packed face records,
// so it is not safe to call it a total face count.
inline Odol73StaticPolygonPrefix ReadOdol73StaticPolygonPrefix(BinaryReader& reader, uint32_t end)
{
    Odol73StaticPolygonPrefix result;
    result.indexStreamMarker = reader.read<bool>();
    result.unparsedIndexValue = reader.read<uint32_t>();
    const uint32_t declaredValue = reader.read<uint32_t>();
    if (declaredValue == 0 || declaredValue > 1'000'000)
        throw std::runtime_error("ODOL 73 LOD polygon-prefix value is invalid");
    result.polygons.reserve(declaredValue);
    while (reader.tell() < static_cast<int>(end))
    {
        const uint8_t vertices = reader.read<uint8_t>();
        if (vertices < 3 || vertices > 32)
        {
            reader.seek(reader.tell() - 1);
            break;
        }
        std::vector<uint32_t> polygon(vertices);
        for (uint32_t& index : polygon)
            index = reader.read<uint32_t>();
        result.polygons.push_back(std::move(polygon));
    }
    if (result.polygons.empty())
        throw std::runtime_error("ODOL 73 LOD has no decodable polygon prefix");
    result.followingByteOffset = static_cast<uint32_t>(reader.tell());
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
        for (int c = 0; c < 3; ++c) { gen.uvTransform.aside[c] = rawGen.matrix[c]; gen.uvTransform.up[c] = rawGen.matrix[4 + c]; gen.uvTransform.dir[c] = rawGen.matrix[8 + c]; gen.uvTransform.pos[c] = rawGen.matrix[12 + c]; }
        source.texGens.push_back(std::move(gen));
    }
    return source;
}

} // namespace Poseidon::Asset::Formats::P3D
