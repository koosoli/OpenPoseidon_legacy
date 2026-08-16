#include <Poseidon/Graphics/Rendering/Primitives/MeshBuild.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Poly.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>

#include <climits>

namespace Poseidon::render::mesh
{

int CountIndices(const Shape& src)
{
    int indices = 0;
    for (Offset o = src.BeginFaces(); o < src.EndFaces(); src.NextFace(o))
    {
        const Poly& poly = src.Face(o);
        PoseidonAssert(poly.N() >= 3);
        indices += (poly.N() - 2) * 3;
    }
    return indices;
}

void BuildVertices(const Shape& src, SVertex* out)
{
    // Some shadow LODs retain the tangent-frame flag after their optional
    // tangent streams have been stripped. Treat that as no frame rather than
    // indexing an absent stream while warming the shadow cache.
    const bool hasTangentFrame = src.HasTangentFrame() && src.NTangent() >= src.NVertex() &&
                                 src.NBinormal() >= src.NVertex();
    // Shadow-volume LODs in legacy P3Ds may contain positions and faces but
    // omit UVs and/or normals. The former pointer-based walk dereferenced both
    // streams unconditionally and crashed while the shadow cache built its
    // first WGPU vertex buffer. Supply neutral attributes for those valid,
    // position-only meshes instead.
    const int n = src.NVertex();
    const int normals = src.NNorm();
    const int texcoords = src.NTex();
    const bool hasUv1 = src.HasUV1();
    for (int vertexIndex = 0; vertexIndex < n; ++vertexIndex)
    {
        Vector3Val pos = src.Pos(vertexIndex);
        Vector3Val norm = vertexIndex < normals ? src.Norm(vertexIndex) : V3Up;
        const UVPair uv = vertexIndex < texcoords ? src.UV(vertexIndex) : UVPair{0.0f, 0.0f};
        out->pos = Vector3P(pos.X(), pos.Y(), pos.Z());
        // Normals are negated (matches the D3D convention).
        out->norm = Vector3P(-norm.X(), -norm.Y(), -norm.Z());
        out->t0 = uv;
        out->t1 = hasUv1 ? src.UV1(vertexIndex) : uv;
        // Non-conform path: the per-instance conform mode is 0, so the shader ignores
        // this. Only BuildOrigVertices (the conform path) sets a meaningful value.
        out->conform = 0;
        if (hasTangentFrame)
        {
            Vector3Val tangent = src.Tangent(vertexIndex);
            Vector3Val binormal = src.Binormal(vertexIndex);
            out->tangent = Vector3P(-tangent.X(), -tangent.Y(), -tangent.Z());
            out->binormal = Vector3P(-binormal.X(), -binormal.Y(), -binormal.Z());
        }
        else
        {
            out->tangent = Vector3P(0, 0, 0);
            out->binormal = Vector3P(0, 0, 0);
        }
        out++;
    }
}

void BuildOrigVertices(const Shape& src, SVertex* out)
{
    for (int i = 0; i < src.NVertex(); i++)
    {
        Vector3Val pos = src.OrigPos(i);
        Vector3Val norm = src.OrigNorm(i);
        out->pos = Vector3P(pos.X(), pos.Y(), pos.Z());
        // Normals are negated (matches the D3D convention), same as BuildVertices.
        out->norm = Vector3P(-norm.X(), -norm.Y(), -norm.Z());
        out->t0 = src.UV(i);
        out->t1 = src.HasUV1() ? src.UV1(i) : src.UV(i);
        // Per-vertex conform selector from the ORIGINAL clip (Object::Animate clears
        // ClipLandKeep as it conforms, so the current clip would be wrong): 1 = Keep
        // (SurfaceY + height above surface), 2 = On (pin to SurfaceY), 0 = rigid.
        ClipFlags clip = src.OrigClip(i);
        out->conform = (clip & ClipLandKeep) ? 1u : ((clip & ClipLandOn) ? 2u : 0u);
        if (src.HasTangentFrame())
        {
            Vector3Val tangent = src.Tangent(i);
            Vector3Val binormal = src.Binormal(i);
            out->tangent = Vector3P(-tangent.X(), -tangent.Y(), -tangent.Z());
            out->binormal = Vector3P(-binormal.X(), -binormal.Y(), -binormal.Z());
        }
        else
        {
            out->tangent = Vector3P(0, 0, 0);
            out->binormal = Vector3P(0, 0, 0);
        }
        out++;
    }
}

void BuildIndices(const Shape& src, VertexIndex* out)
{
    for (Offset o = src.BeginFaces(); o < src.EndFaces(); src.NextFace(o))
    {
        const Poly& poly = src.Face(o);
        for (int i = 2; i < poly.N(); i++)
        {
            *out++ = poly.GetVertex(0);
            *out++ = poly.GetVertex(i - 1);
            *out++ = poly.GetVertex(i);
        }
    }
}

void BuildSections(const Shape& src, AutoArray<MeshSection>& out)
{
    out.Realloc(src.NSections());
    out.Resize(src.NSections());
    int start = 0;
    for (int i = 0; i < src.NSections(); i++)
    {
        const ShapeSection& sec = src.GetSection(i);
        int size = 0;
        int minV = INT_MAX;
        int maxV = 0;
        for (Offset o = sec.beg; o < sec.end; src.NextFace(o))
        {
            const Poly& face = src.Face(o);
            PoseidonAssert(face.N() >= 3);
            size += (face.N() - 2) * 3;
            for (int vv = 0; vv < face.N(); vv++)
            {
                int vi = face.GetVertex(vv);
                if (vi < minV)
                    minV = vi;
                if (vi > maxV)
                    maxV = vi;
            }
        }
        out[i].beg = start;
        out[i].end = start + size;
        out[i].begVertex = minV;
        out[i].endVertex = maxV + 1;
        start += size;
    }
}

} // namespace Poseidon::render::mesh
