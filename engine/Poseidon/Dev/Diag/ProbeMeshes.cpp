#include <Poseidon/Dev/Diag/ProbeMeshes.hpp>

#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/World/Scene/Scene.hpp>

#include <cmath>
#include <vector>

namespace Poseidon::Dev
{
namespace
{

// Same recipe ObjectLine::CreateShape uses, generalised. IsColored is what lets
// SetConstantColor tint the result, which is how a probe stays distinguishable
// from the scenery it is resting on.
// NO NoShadow. That flag was copied from ObjectLine::CreateShape, where a debug
// line has no business casting one -- but a crate resting on the ground without a
// shadow reads as floating, and the shadow is half of what tells you it is
// actually touching.
constexpr int ProbeSpecial = IsColored;
Ref<LODShapeWithShadow> s_boxMesh, s_sphereMesh, s_cylinderMesh;
int s_appliedSpecial = -1;

/// Builds one shape from flat-shaded quads and triangles. `indices` holds 3 or 4
/// entries per face; `counts` says which.
///
/// WINDING: the object pipelines cull with front_face = Cw, so a face must read
/// CLOCKWISE from outside or it is culled and you see straight through the model
/// to its far side.
///
/// `reverse` is PER MESH and not a global correction. Flipping it for everything
/// fixed the cylinder and broke the box, which had been wound correctly all
/// along -- so the box then showed only its inside faces. Each generator states
/// its own convention rather than sharing a guess.
LODShapeWithShadow* Build(const std::vector<Vector3>& positions, const std::vector<Vector3>& normals,
                          const std::vector<int>& indices, const std::vector<int>& counts, bool reverse)
{
    LODShapeWithShadow* lShape = new LODShapeWithShadow;
    lShape->SetAutoCenter(false);
    Shape* shape = new Shape;
    lShape->AddShape(shape, 0);
    lShape->SetSpecial(ProbeSpecial);

    shape->ReallocTable(static_cast<int>(positions.size()));
    for (std::size_t i = 0; i < positions.size(); ++i)
    {
        shape->SetPos(static_cast<int>(i)) = positions[i];
        // Shape stores inward normals; MeshBuild negates them for lighting.
        // The procedural builders below compute outward geometric normals.
        shape->SetNorm(static_cast<int>(i)) = -normals[i];
        shape->SetClip(static_cast<int>(i), ClipAll);
    }

    int cursor = 0;
    for (const int n : counts)
    {
        Poly face;
        face.Init();
        face.SetN(n);
        for (int i = 0; i < n; ++i)
        {
            const int source = reverse ? (n - 1 - i) : i;
            face.Set(i, indices[static_cast<std::size_t>(cursor + source)]);
        }
        face.SetTexture(GScene->Preloaded(TextureWhite));
        face.SetSpecial(ProbeSpecial);
        shape->AddFace(face);
        cursor += n;
    }

    shape->SetSpecial(ProbeSpecial);
    shape->FindSections();
    shape->CalculateHints();
    lShape->CalculateMinMax(true);
    return lShape;
}

} // namespace

LODShapeWithShadow* ProbeBoxMesh()
{
    if (!s_boxMesh) s_boxMesh = []
    {
        // 24 vertices, not 8: a shared corner would average three face normals and
        // give a cube soft, rounded-looking shading. Four own vertices per face
        // keeps every face flat, which is what makes the edges readable.
        static const float axis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        std::vector<Vector3> positions;
        std::vector<Vector3> normals;
        std::vector<int>     indices;
        std::vector<int>     counts;

        for (int a = 0; a < 3; ++a)
        {
            const int b = (a + 1) % 3;
            const int c = (a + 2) % 3;
            for (int sign = -1; sign <= 1; sign += 2)
            {
                const Vector3 n(axis[a][0] * sign, axis[a][1] * sign, axis[a][2] * sign);
                const Vector3 u(axis[b][0], axis[b][1], axis[b][2]);
                const Vector3 v(axis[c][0], axis[c][1], axis[c][2]);
                const int     base = static_cast<int>(positions.size());
                // Wound so the face points outwards for either sign.
                const float   su = static_cast<float>(sign);
                positions.push_back(n - u * su - v);
                positions.push_back(n + u * su - v);
                positions.push_back(n + u * su + v);
                positions.push_back(n - u * su + v);
                for (int i = 0; i < 4; ++i)
                {
                    normals.push_back(n);
                    indices.push_back(base + i);
                }
                counts.push_back(4);
            }
        }
        // Already clockwise from outside.
        return Build(positions, normals, indices, counts, false);
    }();
    return s_boxMesh;
}

LODShapeWithShadow* ProbeSphereMesh()
{
    if (!s_sphereMesh) s_sphereMesh = []
    {
        // A UV sphere. 16x8 is round enough at the sizes a probe is looked at and
        // cheap enough that a hundred of them cost nothing.
        constexpr int Segments = 16;
        constexpr int Rings = 8;
        std::vector<Vector3> positions;
        std::vector<Vector3> normals;
        std::vector<int>     indices;
        std::vector<int>     counts;

        auto point = [](int ring, int seg)
        {
            const float phi = 3.14159265f * static_cast<float>(ring) / static_cast<float>(Rings);
            const float theta = 6.28318531f * static_cast<float>(seg % Segments) / static_cast<float>(Segments);
            return Vector3(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
        };

        for (int r = 0; r < Rings; ++r)
        {
            for (int sgm = 0; sgm < Segments; ++sgm)
            {
                const Vector3 p[4] = {point(r, sgm), point(r, sgm + 1), point(r + 1, sgm + 1), point(r + 1, sgm)};
                const int     base = static_cast<int>(positions.size());
                // Poles collapse to a triangle; a quad there would be degenerate.
                const int n = (r == 0 || r == Rings - 1) ? 3 : 4;
                for (int i = 0; i < 4; ++i)
                {
                    if (n == 3 && ((r == 0 && i == 1) || (r == Rings - 1 && i == 2)))
                    {
                        continue;
                    }
                    positions.push_back(p[i]);
                    normals.push_back(p[i]); // unit sphere: position IS the normal
                    indices.push_back(base + static_cast<int>(positions.size()) - base - 1);
                }
                counts.push_back(n);
            }
        }
        // Same convention as the box.
        return Build(positions, normals, indices, counts, false);
    }();
    return s_sphereMesh;
}

LODShapeWithShadow* ProbeCylinderMesh()
{
    if (!s_cylinderMesh) s_cylinderMesh = []
    {
        constexpr int Sides = 16;
        std::vector<Vector3> positions;
        std::vector<Vector3> normals;
        std::vector<int>     indices;
        std::vector<int>     counts;

        auto ring = [](int i)
        {
            const float t = 6.28318531f * static_cast<float>(i % Sides) / static_cast<float>(Sides);
            return Vector3(std::cos(t), 0.0f, std::sin(t));
        };

        for (int i = 0; i < Sides; ++i)
        {
            const Vector3 a = ring(i);
            const Vector3 b = ring(i + 1);
            const int     base = static_cast<int>(positions.size());
            positions.push_back(Vector3(a[0], -1.0f, a[2]));
            positions.push_back(Vector3(b[0], -1.0f, b[2]));
            positions.push_back(Vector3(b[0], 1.0f, b[2]));
            positions.push_back(Vector3(a[0], 1.0f, a[2]));
            normals.push_back(a);
            normals.push_back(b);
            normals.push_back(b);
            normals.push_back(a);
            for (int k = 0; k < 4; ++k)
            {
                indices.push_back(base + k);
            }
            counts.push_back(4);
        }

        // Caps as triangle fans from the centre.
        for (int sign = -1; sign <= 1; sign += 2)
        {
            const Vector3 n(0.0f, static_cast<float>(sign), 0.0f);
            const int     centre = static_cast<int>(positions.size());
            positions.push_back(Vector3(0.0f, static_cast<float>(sign), 0.0f));
            normals.push_back(n);
            for (int i = 0; i < Sides; ++i)
            {
                const Vector3 a = ring(sign < 0 ? i : i + 1);
                const Vector3 b = ring(sign < 0 ? i + 1 : i);
                const int     base = static_cast<int>(positions.size());
                positions.push_back(Vector3(a[0], static_cast<float>(sign), a[2]));
                positions.push_back(Vector3(b[0], static_cast<float>(sign), b[2]));
                normals.push_back(n);
                normals.push_back(n);
                indices.push_back(centre);
                indices.push_back(base);
                indices.push_back(base + 1);
                counts.push_back(3);
            }
        }
        // The ring is generated anticlockwise in XZ, so these come out the wrong
        // way round and need the flip. Measured, not assumed: the cylinder was the
        // one shape the owner could see through.
        return Build(positions, normals, indices, counts, true);
    }();
    return s_cylinderMesh;
}

void ReleaseProbeMeshes()
{
    s_boxMesh.Free();
    s_sphereMesh.Free();
    s_cylinderMesh.Free();
    s_appliedSpecial = -1;
}

void SetProbeMeshSelfIllum(bool unlit)
{
    // The meshes are function-local statics built on first use, so asking for them here
    // is what guarantees all three exist before the flag is applied to them -- a mesh
    // built later picks up ProbeSpecial and would silently miss the flag otherwise.
    LODShapeWithShadow* meshes[] = {ProbeBoxMesh(), ProbeSphereMesh(), ProbeCylinderMesh()};
    const int special = unlit ? (ProbeSpecial | IsLight) : ProbeSpecial;
    if (s_appliedSpecial == special)
    {
        return; // nothing to do, and SetSpecial walks every face
    }
    s_appliedSpecial = special;
    for (LODShapeWithShadow* lShape : meshes)
    {
        if (lShape == nullptr)
        {
            continue;
        }
        lShape->SetSpecial(special);
        for (int level = 0; level < lShape->NLevels(); level++)
        {
            Shape* shape = lShape->Level(level);
            if (shape == nullptr)
            {
                continue;
            }
            shape->SetSpecial(special);
            for (Offset f = shape->BeginFaces(); f < shape->EndFaces(); shape->NextFace(f))
            {
                shape->Face(f).SetSpecial(special);
            }
            shape->FindSections();
        }
    }
}

} // namespace Poseidon::Dev
