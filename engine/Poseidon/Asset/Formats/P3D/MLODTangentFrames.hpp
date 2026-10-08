#pragma once

#include <Poseidon/World/Model/Model.hpp>
#include <array>
#include <cmath>
#include <vector>

namespace Poseidon::Asset::Formats::MLOD
{
// Optional in-memory import data. Never written into an MLOD file. Kept per
// corner so equal positions/UVs can still have different authored tangent axes.
struct TangentFrame
{
    Poseidon::Model::Vector3 tangent;
    Poseidon::Model::Vector3 binormal;
    bool supplied = false;
};
using FaceTangentFrames = std::array<TangentFrame, 4>;
using LodTangentFrames = std::vector<FaceTangentFrames>;
using TangentFrames = std::vector<LodTangentFrames>;

inline bool ValidTangentFrame(const TangentFrame& f)
{
    const auto valid = [](const auto& v)
    {
        const float n = v.x * v.x + v.y * v.y + v.z * v.z;
        return std::isfinite(n) && n > 1e-8f && n < 4.f;
    };
    if (!f.supplied || !valid(f.tangent) || !valid(f.binormal)) return false;
    const auto& t = f.tangent;
    const auto& b = f.binormal;
    const float x = t.y * b.z - t.z * b.y;
    const float y = t.z * b.x - t.x * b.z;
    const float z = t.x * b.y - t.y * b.x;
    return x * x + y * y + z * z > 1e-8f;
}
} // namespace Poseidon::Asset::Formats::MLOD
