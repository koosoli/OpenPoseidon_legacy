#pragma once

#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/Asset/Formats/BISStructures.hpp>
#include <cstdint>
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

} // namespace Poseidon::Asset::Formats::P3D
