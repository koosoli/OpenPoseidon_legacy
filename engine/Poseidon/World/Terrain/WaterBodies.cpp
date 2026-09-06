#include <Poseidon/World/Terrain/WaterBodies.hpp>

#include <Poseidon/World/Terrain/Landscape.hpp>

#include <algorithm>
#include <cmath>

namespace Poseidon
{

float WaterBody::Area() const
{
    return 3.14159265f * std::max(radiusX, 0.0f) * std::max(radiusZ, 0.0f);
}

float WaterBody::ExtentX() const
{
    const float c = std::cos(rotation), s = std::sin(rotation);
    return std::sqrt(radiusX * c * radiusX * c + radiusZ * s * radiusZ * s);
}

float WaterBody::ExtentZ() const
{
    const float c = std::cos(rotation), s = std::sin(rotation);
    return std::sqrt(radiusX * s * radiusX * s + radiusZ * c * radiusZ * c);
}

void WaterBody::ToLocal(float x, float z, float& lx, float& lz) const
{
    const float dx = x - centreX, dz = z - centreZ;
    const float c = std::cos(rotation), s = std::sin(rotation);
    lx = c * dx + s * dz;
    lz = -s * dx + c * dz;
}

bool WaterBody::Contains(float x, float z) const
{
    if (radiusX <= 0.0f || radiusZ <= 0.0f)
    {
        return false;
    }
    float lx, lz;
    ToLocal(x, z, lx, lz);
    const float u = lx / radiusX;
    const float v = lz / radiusZ;
    return u * u + v * v <= 1.0f;
}

float WaterBody::SurfaceLevelAt(float x, float z) const
{
    const float plane = level + gradientX * (x - centreX) + gradientZ * (z - centreZ);
    if (bedDepth > 0.0f && GLandscape != nullptr)
    {
        // Project onto the centreline (local x axis) and read the bed there.
        float lx, lz;
        ToLocal(x, z, lx, lz);
        const float c = std::cos(rotation), s = std::sin(rotation);
        const float cx = centreX + c * lx;
        const float cz = centreZ + s * lx;
        return GLandscape->SurfaceY(cx, cz) + bedDepth;
    }
    return plane;
}

uint32_t WaterBodyRegistry::Add(WaterBody body)
{
    body.id = _nextId++;
    _bodies.push_back(body);
    ++_generation;
    return body.id;
}

bool WaterBodyRegistry::Remove(uint32_t id)
{
    const auto it = std::find_if(_bodies.begin(), _bodies.end(), [id](const WaterBody& b) { return b.id == id; });
    if (it == _bodies.end())
    {
        return false;
    }
    _bodies.erase(it);
    ++_generation;
    return true;
}

void WaterBodyRegistry::Clear()
{
    if (!_bodies.empty())
    {
        ++_generation;
    }
    _bodies.clear();
}

const WaterBody* WaterBodyRegistry::Find(float x, float z) const
{
    const WaterBody* best = nullptr;
    for (const WaterBody& body : _bodies)
    {
        // Box reject first; the ellipse test is the authority.
        if (x < body.MinX() || x > body.MaxX() || z < body.MinZ() || z > body.MaxZ())
        {
            continue;
        }
        if (!body.Contains(x, z))
        {
            continue;
        }
        if (best == nullptr || body.Area() < best->Area() || (body.Area() == best->Area() && body.id < best->id))
        {
            best = &body;
        }
    }
    return best;
}

const WaterBody* WaterBodyRegistry::FindById(uint32_t id) const
{
    for (const WaterBody& body : _bodies)
    {
        if (body.id == id)
        {
            return &body;
        }
    }
    return nullptr;
}

WaterBodyRegistry& GetWaterBodies()
{
    static WaterBodyRegistry registry;
    return registry;
}

} // namespace Poseidon
