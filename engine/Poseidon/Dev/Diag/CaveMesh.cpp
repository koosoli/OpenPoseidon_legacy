#include <Poseidon/Dev/Diag/CaveMesh.hpp>
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
using IrPoint = decltype(IrVertex::position); // immune to the engine's Vector3 macro
using Mesh = Model::Mesh;
std::atomic<std::uint64_t> s_nextCave{1};

struct Slab
{
    IrPoint lo, hi;
};

// Clockwise from outside; Poly's (p2-p0)x(p1-p0) points INTO each solid.
constexpr int kFaces[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
constexpr float kInward[6][3] = {{0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {1, 0, 0}};

std::array<IrPoint, 8> Corners(const Slab& s)
{
    return {{{s.lo.x, s.lo.y, s.lo.z},
             {s.hi.x, s.lo.y, s.lo.z},
             {s.hi.x, s.lo.y, s.hi.z},
             {s.lo.x, s.lo.y, s.hi.z},
             {s.lo.x, s.hi.y, s.lo.z},
             {s.hi.x, s.hi.y, s.lo.z},
             {s.hi.x, s.hi.y, s.hi.z},
             {s.lo.x, s.hi.y, s.hi.z}}};
}

void AddSolid(Mesh& mesh, const Slab& slab, int component)
{
    const auto points = Corners(slab);
    const auto firstVertex = static_cast<std::uint32_t>(mesh.vertices.size());
    Model::NamedSelection selection("component0" + std::to_string(component));
    for (const auto& point : points)
    {
        selection.vertexIndices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
        mesh.vertices.emplace_back(point, IrPoint{0, 1, 0}, Model::Vector2{}, Model::VertexFlags::ClipAll);
        mesh.vertexMass.push_back(100.0f); // 4 tonnes total; Object::IsPassable tests mass < 10
    }
    for (const auto& face : kFaces)
    {
        Model::Quad quad(firstVertex + face[0], firstVertex + face[1], firstVertex + face[2], firstVertex + face[3]);
        quad.originalIndex = static_cast<std::uint32_t>(mesh.quads.size());
        selection.triangleIndices.push_back(quad.originalIndex); // SOURCE face index, including quads
        mesh.quads.push_back(quad);
    }
    mesh.selections.push_back(std::move(selection));
}

void AddRockVisual(Mesh& mesh, const Slab& slab, float facetDepth, bool floor)
{
    const auto points = Corners(slab);
    for (int f = 0; f < 6; ++f)
    {
        IrPoint centre;
        for (int c : kFaces[f])
        {
            centre.x += points[c].x * 0.25f;
            centre.y += points[c].y * 0.25f;
            centre.z += points[c].z * 0.25f;
        }
        // Small recesses stay within the collision slabs; never intrude into
        // the clear corridor. The floor top remains planar for faithful support.
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
            const float inverseLength =
                1.0f / std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
            normal.x *= inverseLength;
            normal.y *= inverseLength;
            normal.z *= inverseLength;
            // Shape normals are inward; MeshBuild negates them for lighting.
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

void AddFootprint(Mesh& memory, const char* name, float halfWidth, float length, float y, bool singlePoint)
{
    Model::NamedSelection selection(name);
    const std::array<IrPoint, 4> points = {
        {{-halfWidth, y, 0}, {halfWidth, y, 0}, {halfWidth, y, length}, {-halfWidth, y, length}}};
    for (int i = 0; i < (singlePoint ? 1 : 4); ++i)
    {
        selection.vertexIndices.push_back(static_cast<std::uint32_t>(memory.vertices.size()));
        memory.vertices.emplace_back(singlePoint ? IrPoint{0, y, length * 0.5f} : points[i], IrPoint{0, 1, 0},
                                     Model::Vector2{});
    }
    memory.selections.push_back(std::move(selection));
}
} // namespace

bool ValidCaveMeshParams(const CaveMeshParams& p)
{
    return std::isfinite(p.width) && p.width >= 0.1f && p.width <= 12.0f && std::isfinite(p.height) &&
           p.height >= 0.1f && p.height <= 8.0f && std::isfinite(p.length) && p.length >= 0.1f && p.length <= 64.0f &&
           std::isfinite(p.wallThickness) && p.wallThickness >= 0.05f && p.wallThickness <= 2.0f;
}

Model::Model BuildCaveModel(const CaveMeshParams& p)
{
    Model::Model model;
    if (!ValidCaveMeshParams(p))
        return model;
    model.sourceFormat = "MLOD";
    model.sourceVersion = 11;
    model.sourcePath =
        "generated\\cave-shell-" + std::to_string(s_nextCave.fetch_add(1, std::memory_order_relaxed)) + ".p3d";
    model.autoCenterEnabled = 0;
    const float w = p.width * 0.5f, t = p.wallThickness, h = p.height, l = p.length;
    const std::array<Slab, 5> slabs = {{{{-w - t, -t, 0}, {-w, h + t, l}},
                                        {{w, -t, 0}, {w + t, h + t, l}},
                                        {{-w - t, h, 0}, {w + t, h + t, l}},
                                        {{-w - t, -t, 0}, {w + t, 0, l}},
                                        {{-w - t, -t, l}, {w + t, h + t, l + t}}}};
    Mesh visual, geometry;
    visual.materials.emplace_back("rock-dark", "#(argb,8,8,3)color(0.27,0.25,0.22,1,DT)");
    visual.materials.emplace_back("rock-mid", "#(argb,8,8,3)color(0.32,0.29,0.25,1,DT)");
    visual.materials.emplace_back("rock-light", "#(argb,8,8,3)color(0.36,0.33,0.28,1,DT)");
    for (int i = 0; i < 5; ++i)
    {
        AddSolid(geometry, slabs[i], i + 1);
        AddRockVisual(visual, slabs[i], std::min(0.04f, t * 0.1f), i == 3);
    }
    geometry.properties.emplace_back("autocenter", "0");
    geometry.properties.emplace_back("dammage", "No"); // generated ground is removed only by editor/world teardown
    geometry.properties.emplace_back("map", "rock");
    visual.properties = geometry.properties;
    model.lodLevels.emplace_back(0.0f, visual);
    model.lodLevels.emplace_back(1.0e13f, geometry);
    Mesh memory;
    AddFootprint(memory, "terrain_hole", w, l, 0.0f, false);
    if (p.includeCeilingSelection)
        AddFootprint(memory, "terrain_hole_ceiling", w, l, h, true);
    model.lodLevels.emplace_back(1.0e15f, memory);
    Mesh roadway;
    roadway.vertices = {{IrPoint{-w, 0, 0}, IrPoint{0, -1, 0}, Model::Vector2{0, 0}},
                        {IrPoint{-w, 0, l}, IrPoint{0, -1, 0}, Model::Vector2{0, 1}},
                        {IrPoint{w, 0, l}, IrPoint{0, -1, 0}, Model::Vector2{1, 1}},
                        {IrPoint{w, 0, 0}, IrPoint{0, -1, 0}, Model::Vector2{1, 0}}};
    roadway.quads.emplace_back(0, 1, 2, 3);
    model.lodLevels.emplace_back(3.0e15f, roadway);
    // Independent collision sets: never a single convex hull around the void.
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

LODShapeWithShadow* BuildCaveMesh(const CaveMeshParams& p)
{
    if (!Foundation::IsMainThread() || !ValidCaveMeshParams(p))
        return nullptr;
    return Model::ShapeAdapter::convertToLODShape(BuildCaveModel(p));
}
} // namespace Poseidon::Dev
