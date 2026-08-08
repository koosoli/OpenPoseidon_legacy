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

} // namespace Poseidon::Asset::Formats::P3D
