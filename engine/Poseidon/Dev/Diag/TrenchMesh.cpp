#include <Poseidon/Dev/Diag/TrenchMesh.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

namespace Poseidon::Dev
{
namespace
{
using IrVertex = Model::Vertex;
using IrPoint = decltype(IrVertex::position);
using Mesh = Model::Mesh;
using Slab = std::array<IrPoint, 8>;
std::atomic<std::uint64_t> s_nextTrench{1};

// Same inward-plane/clockwise-exterior convention as the canonical cave box.
constexpr int kFaces[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
constexpr float kInward[6][3] = {{0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {1, 0, 0}};

Slab Box(float xmin, float xmax, float ymin, float ymax, float zmin, float zmax)
{
    return {{{xmin, ymin, zmin},
             {xmax, ymin, zmin},
             {xmax, ymin, zmax},
             {xmin, ymin, zmax},
             {xmin, ymax, zmin},
             {xmax, ymax, zmin},
             {xmax, ymax, zmax},
             {xmin, ymax, zmax}}};
}

void AddSolid(Mesh& mesh, const Slab& points, int component)
{
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    Model::NamedSelection selection("component0" + std::to_string(component));
    for (const auto& point : points)
    {
        selection.vertexIndices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
        mesh.vertices.emplace_back(point, IrPoint{0, 1, 0}, Model::Vector2{}, Model::VertexFlags::ClipAll);
        mesh.vertexMass.push_back(100.0f); // non-passable ordinary stationary Object
    }
    for (const auto& face : kFaces)
    {
        Model::Quad quad(base + face[0], base + face[1], base + face[2], base + face[3]);
        quad.originalIndex = static_cast<std::uint32_t>(mesh.quads.size());
        selection.triangleIndices.push_back(quad.originalIndex);
        mesh.quads.push_back(quad);
    }
    mesh.selections.push_back(std::move(selection));
}

void AddEarthVisual(Mesh& mesh, const Slab& points, float facetDepth, bool floor)
{
    for (int f = 0; f < 6; ++f)
    {
        IrPoint centre;
        for (int c : kFaces[f])
        {
            centre.x += points[c].x * 0.25f;
            centre.y += points[c].y * 0.25f;
            centre.z += points[c].z * 0.25f;
        }
        // Visual earth recedes into the solids, leaving full inner clearance.
        // Ramp and bed top remain planar and coincide with roadway/collision.
        const float recess = floor && f == 1 ? 0.0f : facetDepth;
        centre.x += kInward[f][0] * recess;
        centre.y += kInward[f][1] * recess;
        centre.z += kInward[f][2] * recess;
        for (int edge = 0; edge < 4; ++edge)
        {
            const IrPoint a = points[kFaces[f][edge]], b = points[kFaces[f][(edge + 1) % 4]];
            const float ax = centre.x - a.x, ay = centre.y - a.y, az = centre.z - a.z;
            const float bx = b.x - a.x, by = b.y - a.y, bz = b.z - a.z;
            IrPoint normal{ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx};
            const float inv = 1.0f / std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
            normal.x *= inv;
            normal.y *= inv;
            normal.z *= inv; // Shape stores inward lighting normals
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.emplace_back(a, normal, Model::Vector2{0, 0}, Model::VertexFlags::ClipAll);
            mesh.vertices.emplace_back(b, normal, Model::Vector2{1, 0}, Model::VertexFlags::ClipAll);
            mesh.vertices.emplace_back(centre, normal, Model::Vector2{0.5f, 1}, Model::VertexFlags::ClipAll);
            Model::Triangle triangle(base, base + 1, base + 2, static_cast<std::uint32_t>((f + edge) % 3));
            triangle.originalIndex = static_cast<std::uint32_t>(mesh.triangles.size());
            mesh.triangles.push_back(triangle);
        }
    }
}

void AddRoadway(Mesh& roadway, float w, float z0, float y0, float z1, float y1)
{
    IrPoint inward{0, -1, (y1 - y0) / (z1 - z0)};
    const float inv = 1.0f / std::sqrt(1.0f + inward.z * inward.z);
    inward.y *= inv;
    inward.z *= inv;
    const auto base = static_cast<std::uint32_t>(roadway.vertices.size());
    roadway.vertices.emplace_back(IrPoint{-w, y0, z0}, inward, Model::Vector2{0, 0}, Model::VertexFlags::ClipAll);
    roadway.vertices.emplace_back(IrPoint{-w, y1, z1}, inward, Model::Vector2{0, 1}, Model::VertexFlags::ClipAll);
    roadway.vertices.emplace_back(IrPoint{w, y1, z1}, inward, Model::Vector2{1, 1}, Model::VertexFlags::ClipAll);
    roadway.vertices.emplace_back(IrPoint{w, y0, z0}, inward, Model::Vector2{1, 0}, Model::VertexFlags::ClipAll);
    Model::Quad face(base, base + 1, base + 2, base + 3);
    face.originalIndex = static_cast<std::uint32_t>(roadway.quads.size());
    roadway.quads.push_back(face);
}
} // namespace

bool ValidTrenchMeshParams(const TrenchMeshParams& p)
{
    if (!std::isfinite(p.width) || p.width < 0.1f || p.width > 12.0f || !std::isfinite(p.depth) || p.depth < 0.1f ||
        p.depth > 4.0f || !std::isfinite(p.length) || p.length > 64.0f || !std::isfinite(p.rampLength) ||
        p.rampLength < p.depth * 3.0f || p.rampLength > p.length || !std::isfinite(p.wallThickness) ||
        p.wallThickness < 0.05f || p.wallThickness > 2.0f)
        return false;
    // Avoid a nearly-zero-volume flat slab. Exact ramp-only geometry is valid;
    // a requested flat bed must be at least 5 cm long (no silent dimension edit).
    return p.rampLength == p.length || p.length - p.rampLength >= 0.05f;
}

TrenchMeshParams TankTrenchMeshParams()
{
    return {5.0f, 24.0f, 2.0f, 12.0f, 0.4f};
}

Model::Model BuildTrenchModel(const TrenchMeshParams& p)
{
    Model::Model model;
    if (!ValidTrenchMeshParams(p))
        return model;
    model.sourceFormat = "MLOD";
    model.sourceVersion = 11;
    model.sourcePath =
        "generated\\trench-shell-" + std::to_string(s_nextTrench.fetch_add(1, std::memory_order_relaxed)) + ".p3d";
    model.autoCenterEnabled = 0;
    const float w = p.width * 0.5f, d = p.depth, l = p.length, r = p.rampLength, t = p.wallThickness;
    Mesh geometry, visual, roadway, memory;
    visual.materials.emplace_back("earth-dark", "#(argb,8,8,3)color(0.25,0.17,0.10,1,DT)");
    visual.materials.emplace_back("earth-mid", "#(argb,8,8,3)color(0.31,0.23,0.14,1,DT)");
    visual.materials.emplace_back("earth-light", "#(argb,8,8,3)color(0.36,0.28,0.18,1,DT)");
    int component = 0;
    const auto add = [&](const Slab& slab, bool floor)
    {
        AddSolid(geometry, slab, ++component);
        AddEarthVisual(visual, slab, std::min(0.04f, t * 0.1f), floor);
    };
    add(Box(-w - t, -w, -d - t, 0.3f, 0, l), false);
    add(Box(w, w + t, -d - t, 0.3f, 0, l), false);
    Slab ramp = Box(-w - t, w + t, -t, 0, 0, r);
    for (int i : {2, 3, 6, 7})
        ramp[i].y -= d;
    add(ramp, true); // closed convex parallelepiped, never a solid trench hull
    AddRoadway(roadway, w, 0, 0, r, -d);
    if (r < l)
    {
        add(Box(-w - t, w + t, -d - t, -d, r, l), true);
        AddRoadway(roadway, w, r, -d, l, -d);
    }
    add(Box(-w - t, w + t, -d - t, 0.3f, l, l + t), false);
    geometry.properties.emplace_back("autocenter", "0");
    geometry.properties.emplace_back("map", "rock"); // stationary, no vegetation sway
    geometry.properties.emplace_back("dammage", "No"); // earth must not fall like a low-mass tree under tracks
    visual.properties = geometry.properties;
    Model::NamedSelection hole("terrain_hole");
    const std::array<IrPoint, 4> footprint = {{{-w, 0, 0}, {w, 0, 0}, {w, 0, l}, {-w, 0, l}}};
    for (const auto& point : footprint)
    {
        hole.vertexIndices.push_back(static_cast<std::uint32_t>(memory.vertices.size()));
        memory.vertices.emplace_back(point, IrPoint{0, 1, 0}, Model::Vector2{});
    }
    memory.selections.push_back(std::move(hole));
    model.lodLevels.emplace_back(0.0f, visual);
    model.lodLevels.emplace_back(1.0e13f, geometry);
    model.lodLevels.emplace_back(1.0e15f, memory);
    model.lodLevels.emplace_back(3.0e15f, roadway);
    geometry.vertexMass.clear();
    model.lodLevels.emplace_back(6.0e15f, geometry);
    model.lodLevels.emplace_back(7.0e15f, geometry);
    model.geometryIdx = 1;
    model.memoryIdx = 2;
    model.roadwayIdx = 3;
    model.geometryViewIdx = 4;
    model.geometryFireIdx = 5;
    return model;
}

LODShapeWithShadow* BuildTrenchMesh(const TrenchMeshParams& p)
{
    if (!Foundation::IsMainThread() || !ValidTrenchMeshParams(p))
        return nullptr;
    return Model::ShapeAdapter::convertToLODShape(BuildTrenchModel(p));
}
} // namespace Poseidon::Dev
