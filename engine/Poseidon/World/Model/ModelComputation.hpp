#pragma once

#include <Poseidon/World/Model/Model.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace Poseidon::Model
{
namespace ModelComputation {

inline BoundingBox computeBoundingBox(const std::vector<Vertex>& vertices) {
    if (vertices.empty()) {
        return BoundingBox();
    }
    
    BoundingBox box;
    box.min = Vector3(
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    );
    box.max = Vector3(
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()
    );
    
    for (const auto& v : vertices) {
        box.min.x = std::min(box.min.x, v.position.x);
        box.min.y = std::min(box.min.y, v.position.y);
        box.min.z = std::min(box.min.z, v.position.z);
        
        box.max.x = std::max(box.max.x, v.position.x);
        box.max.y = std::max(box.max.y, v.position.y);
        box.max.z = std::max(box.max.z, v.position.z);
    }
    
    return box;
}

inline BoundingSphere computeBoundingSphere(const BoundingBox& box) {
    BoundingSphere sphere;
    sphere.center = box.GetCenter();
    
    Vector3 extent = box.GetExtent();
    sphere.radius = std::sqrt(
        extent.x * extent.x + 
        extent.y * extent.y + 
        extent.z * extent.z
    );
    
    return sphere;
}

inline BoundingSphere computeBoundingSphere(const std::vector<Vertex>& vertices) {
    if (vertices.empty()) {
        return BoundingSphere();
    }

    BoundingBox box = computeBoundingBox(vertices);
    return computeBoundingSphere(box);
}

inline std::vector<uint32_t> extractUniqueMaterialIndices(const std::vector<Triangle>& triangles) {
    std::vector<uint32_t> uniqueIndices;
    
    for (const auto& tri : triangles) {
        if (std::find(uniqueIndices.begin(), uniqueIndices.end(), tri.materialIndex) == uniqueIndices.end()) {
            uniqueIndices.push_back(tri.materialIndex);
        }
    }
    
    std::sort(uniqueIndices.begin(), uniqueIndices.end());
    return uniqueIndices;
}

inline std::vector<Section> generateSections(const std::vector<Triangle>& triangles) {
    if (triangles.empty()) {
        return {};
    }
    
    std::vector<Section> sections;
    
    uint32_t currentMaterial = triangles[0].materialIndex;
    uint32_t sectionStart = 0;
    uint32_t sectionCount = 0;
    
    for (size_t i = 0; i < triangles.size(); ++i) {
        if (triangles[i].materialIndex != currentMaterial) {
            Section sec;
            sec.materialIndex = currentMaterial;
            sec.startTriangle = sectionStart;
            sec.triangleCount = sectionCount;
            sec.hints = RenderHints::None;
            sections.push_back(sec);

            currentMaterial = triangles[i].materialIndex;
            sectionStart = static_cast<uint32_t>(i);
            sectionCount = 1;
        } else {
            ++sectionCount;
        }
    }
    
    // Add final section
    Section sec;
    sec.materialIndex = currentMaterial;
    sec.startTriangle = sectionStart;
    sec.triangleCount = sectionCount;
    sec.hints = RenderHints::None;
    sections.push_back(sec);
    
    return sections;
}

// Source MLOD face streams freely mix triangles and quads.  A section is a run
// in that original stream, not a run in either of the two convenience vectors
// the canonical mesh uses.  Keeping the original face order is essential: it
// is what carries an authored RVMAT through ShapeAdapter to the renderer.
inline std::vector<Section> generateSections(const std::vector<Triangle>& triangles,
                                             const std::vector<Quad>& quads) {
    struct FaceMaterial {
        uint32_t originalIndex;
        uint32_t materialIndex;
    };
    std::vector<FaceMaterial> faces;
    faces.reserve(triangles.size() + quads.size());
    for (const auto& triangle : triangles)
        faces.push_back({triangle.originalIndex, triangle.materialIndex});
    for (const auto& quad : quads)
        faces.push_back({quad.originalIndex, quad.materialIndex});
    if (faces.empty())
        return {};

    std::stable_sort(faces.begin(), faces.end(),
                     [](const FaceMaterial& left, const FaceMaterial& right) { return left.originalIndex < right.originalIndex; });

    std::vector<Section> sections;
    uint32_t currentMaterial = faces.front().materialIndex;
    uint32_t sectionStart = 0;
    for (uint32_t face = 1; face <= static_cast<uint32_t>(faces.size()); ++face) {
        if (face != faces.size() && faces[face].materialIndex == currentMaterial)
            continue;
        Section section;
        section.materialIndex = currentMaterial;
        section.startTriangle = sectionStart; // historical field name; MLOD uses face-stream units.
        section.triangleCount = face - sectionStart;
        section.hints = RenderHints::None;
        sections.push_back(section);
        if (face < faces.size()) {
            currentMaterial = faces[face].materialIndex;
            sectionStart = face;
        }
    }
    return sections;
}

// AST-018: the proxy frame, built the way the engine's own proxy code builds it.
//
// This previously took the three selection vertices in index order and made
// v1-v0 the aside axis. Both halves were wrong. MLOD selection membership is
// gathered by iterating point indices, so the order the three vertices arrive in
// is an artefact of point numbering, not of the triangle -- and LODShape's proxy
// scan (ShapeLOD.cpp) first sorts the points so that p0p1 is the shortest edge
// and p0p2 the second shortest, precisely to make the frame independent of that
// order. It then builds the frame with p1-p0 as the *direction* and p2-p0 as up,
// not as aside.
//
// A proxy transform that disagrees with the engine's puts every attached object
// -- weapons, crew, cargo -- at the wrong orientation, so this follows the
// engine step for step rather than deriving something equivalent-looking.
inline Matrix4x3 computeProxyTransform(
    const std::vector<Vertex>& vertices,
    const NamedSelection& selection
) {
    Matrix4x3 transform;

    if (selection.vertexIndices.size() != 3) {
        return transform;  // identity
    }

    uint32_t i0 = selection.vertexIndices[0];
    uint32_t i1 = selection.vertexIndices[1];
    uint32_t i2 = selection.vertexIndices[2];

    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
        return transform;  // identity
    }

    const Vector3* p0 = &vertices[i0].position;
    const Vector3* p1 = &vertices[i1].position;
    const Vector3* p2 = &vertices[i2].position;

    auto distance2 = [](const Vector3* a, const Vector3* b) {
        const float dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
        return dx * dx + dy * dy + dz * dz;
    };
    float d01 = distance2(p0, p1), d02 = distance2(p0, p2), d12 = distance2(p1, p2);
    // Canonical ordering, identical to LODShape's: p0p1 shortest, p0p2 second.
    if (d01 > d02) { std::swap(p1, p2); std::swap(d01, d02); }
    if (d01 > d12) { std::swap(p0, p2); std::swap(d01, d12); }
    if (d02 > d12) { std::swap(p0, p1); std::swap(d02, d12); }

    auto sub = [](const Vector3* a, const Vector3* b) {
        return Vector3(a->x - b->x, a->y - b->y, a->z - b->z);
    };
    auto length = [](const Vector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
    auto cross = [](const Vector3& a, const Vector3& b) {
        return Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    };

    // Matrix3P::SetDirectionAndUp: normalise the direction, project up into the
    // plane perpendicular to it, then aside = up x direction.
    Vector3 direction = sub(p1, p0);
    float directionLength = length(direction);
    if (directionLength <= 1e-6f) {
        return transform;  // degenerate proxy triangle; identity rather than a guess
    }
    direction = Vector3(direction.x / directionLength, direction.y / directionLength,
                        direction.z / directionLength);

    Vector3 up = sub(p2, p0);
    const float projection = up.x * direction.x + up.y * direction.y + up.z * direction.z;
    up = Vector3(up.x - direction.x * projection, up.y - direction.y * projection,
                 up.z - direction.z * projection);
    float upLength = length(up);
    if (upLength <= 1e-6f) {
        return transform;  // the three points are colinear
    }
    up = Vector3(up.x / upLength, up.y / upLength, up.z / upLength);

    const Vector3 aside = cross(up, direction);

    // Row 0 aside, row 1 up, row 2 direction, last column the position -- the
    // layout ShapeAdapter reads back.
    transform.Set(
        aside.x,     aside.y,     aside.z,     p0->x,
        up.x,        up.y,        up.z,        p0->y,
        direction.x, direction.y, direction.z, p0->z
    );

    return transform;
}

inline std::vector<Proxy> generateProxiesFromSelections(
    const std::vector<Vertex>& vertices,
    const std::vector<NamedSelection>& selections
) {
    std::vector<Proxy> proxies;
    
    for (size_t i = 0; i < selections.size(); ++i) {
        const auto& sel = selections[i];
        
        if (sel.name.find("proxy:") == 0 && sel.vertexIndices.size() == 3) {
            Proxy proxy;
            // "proxy:<model>.<id>" -- the prefix and the id are not part of the
            // model name. ShapeAdapter hands Proxy::name straight to
            // NewProxyObject, so leaving either attached would look up a shape
            // that does not exist. Same split as LODShape's proxy scan.
            std::string body = sel.name.substr(6);
            const size_t dot = body.find('.');
            if (dot != std::string::npos) {
                proxy.id = std::atoi(body.c_str() + dot + 1);
                body.resize(dot);
            }
            proxy.name = body;
            proxy.selectionIndex = static_cast<uint32_t>(i);
            proxy.transform = computeProxyTransform(vertices, sel);
            proxies.push_back(proxy);
        }
    }
    
    return proxies;
}

inline bool validateMesh(const Mesh& mesh) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) {
        return false;
    }
    if (mesh.boundingSphere.radius <= 0.0f) {
        return false;
    }
    for (const auto& tri : mesh.triangles) {
        for (int i = 0; i < 3; ++i) {
            if (tri.indices[i] >= mesh.vertices.size()) {
                return false;
            }
        }
    }
    
    return true;
}

} // namespace ModelComputation
} // namespace Poseidon::Model
