#include <cmath>
#include <utility>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>

#ifdef _MSC_VER
// assume no aliasing
#pragma optimize("a", on)
#endif

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Core/Types.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Poly.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/Entities/Vehicles/Plane.hpp>

#include <Poseidon/Foundation/Math/PolyClip.hpp>

namespace Poseidon
{

void PolyVertices::Reflect(TLVertexTable& mesh)
{
    // reverse if not decal
    if (mesh.GetOrHints() & ClipDecalMask)
    {
        bool allDecal = true;
        for (int i = 0; i < _n; i++)
        {
            ClipFlags hints = mesh.Clip(_vertex[i]);
            if ((hints & ClipDecalMask) != ClipDecalNormal)
            {
                allDecal = false;
            }
        }
        if (allDecal)
        {
            return;
        }
    }
    Reverse();
}

static int Interpolate(TLVertexTable& tlMesh, int in, int out, Coord t, const Camera& camera)
{
    int nTIndex = tlMesh.AddPos(); // can change pointers to TransPos
    Vector3Val iPos = tlMesh.TransPosA(in);
    Vector3Val oPos = tlMesh.TransPosA(out);
    ClipFlags iClip = tlMesh.Clip(in);
    ClipFlags oClip = tlMesh.Clip(out);
    // interpolate vertex user data
    ClipFlags hints = iClip & oClip & ClipHints;
    // interpolate vertex position
    float it = 1 - t;
    Vector3 point = iPos * it + oPos * t;
    tlMesh.SetTransPosA(nTIndex, point);
    tlMesh.SetClip(nTIndex, NeedsClipping(point, camera) | hints);
    // interpolate vertex data
    const TLVertex& iVert = tlMesh.GetVertex(in);
    const TLVertex& oVert = tlMesh.GetVertex(out);
    TLVertex& d = tlMesh.SetVertex(nTIndex);
    // conversion necessary from/to PackedColor
    Color inColor((ColorVal)iVert.color), inSpecu((ColorVal)iVert.specular);
    Color outColor((ColorVal)oVert.color), outSpecu((ColorVal)oVert.specular);
    d.t0.u = iVert.t0.u * it + oVert.t0.u * t;
    d.t0.v = iVert.t0.v * it + oVert.t0.v * t;
    d.t1.u = iVert.t1.u * it + oVert.t1.u * t;
    d.t1.v = iVert.t1.v * it + oVert.t1.v * t;
    d.color = PackedColor(inColor * it + outColor * t);
    d.specular = PackedColor(inSpecu * it + outSpecu * t);
    return nTIndex;
}
static int Interpolate(TLVertexTable& tlMesh, int in, int out, Coord t)
{
    int nTIndex = tlMesh.AddPos(); // can change pointers to TransPos
    const V3& iPos = tlMesh.TransPosA(in);
    const V3& oPos = tlMesh.TransPosA(out);
    ClipFlags iClip = tlMesh.Clip(in);
    ClipFlags oClip = tlMesh.Clip(out);
    // interpolate vertex user data
    ClipFlags hints = iClip & oClip & ClipHints;
    // interpolate vertex position
    float it = 1 - t;
    tlMesh.SetTransPosA(nTIndex, iPos * it + oPos * t);
    tlMesh.SetClip(nTIndex, hints);

    const TLVertex& iVert = tlMesh.GetVertex(in);
    const TLVertex& oVert = tlMesh.GetVertex(out);
    TLVertex& d = tlMesh.SetVertex(nTIndex);
    // conversion necessary from/to PackedColor
    Color inColor((ColorVal)iVert.color), inSpecu((ColorVal)iVert.specular);
    Color outColor((ColorVal)oVert.color), outSpecu((ColorVal)oVert.specular);
    d.t0.u = iVert.t0.u * it + oVert.t0.u * t;
    d.t0.v = iVert.t0.v * it + oVert.t0.v * t;
    d.t1.u = iVert.t1.u * it + oVert.t1.u * t;
    d.t1.v = iVert.t1.v * it + oVert.t1.v * t;
    d.color = PackedColor(inColor * it + outColor * t);
    d.specular = PackedColor(inSpecu * it + outSpecu * t);

    return nTIndex;
}

#pragma warning(disable : 68)

int Interpolate(Shape& mesh, int in, int out, Coord t, bool selections = false);

bool PolyVertices::InsideFromTop(const VertexTable& mesh, const Plane& plane, Vector3Par pos, float* y, float* dX,
                                 float* dZ) const
{
    if (_n < 3)
    {
        return false;
    }
    // perform tests from above
    int i;
    const V3* lPos = &mesh.Pos(_vertex[_n - 1]);
    for (i = 0; i < _n; i++)
    {
        const V3* aPos = &mesh.Pos(_vertex[i]);
        // half-plane equation has form ax+by+c<=0
        // (a,b) is line normal
        // note: Visual C++ removes multiply by Y==0
        Vector3 lineNormal(aPos->Z() - lPos->Z(), 0, lPos->X() - aPos->X());
        float checkIn = lineNormal * (pos - *aPos);
        if (checkIn < 0)
        {
            return false;
        }
        lPos = aPos;
    }

    if (y)
    {
        // planar approach:
        // use face plane equation
        Vector3Val p0 = mesh.Pos(_vertex[0]);
        Vector3Val normal = plane.Normal();
        // c=-normal*p0
        // equation is n*p+c=0
        // nx*px+ny*py+nz*pz+c=0
        // nx*px+nz*pz+c=-ny*py
        // py=(-c-nx*px-nz*pz)/ny
        if (fabs(normal.Y()) < 1e-2)
        {
            return false; // avoid some kind of faces
        }
        float invNY = 1 / normal.Y();
        float yRes = (normal * p0 - normal.X() * pos.X() - normal.Z() * pos.Z()) * invNY;
        *y = yRes;
        // calculate dX and dZ
        if (dX)
        {
            *dX = -normal.X() * invNY;
        }
        if (dZ)
        {
            *dZ = -normal.Z() * invNY;
        }
    }
    return true;
}

float PolyVertices::DistanceFromTop(const VertexTable& mesh, Vector3Par pos) const
{
    // note: returns Square(distance)
    if (_n < 3)
    {
        return 1e10;
    }
    int i;
    // then perform exact check
    const V3* lPos = &mesh.Pos(_vertex[_n - 1]);
    bool orIn = false;
    bool andIn = true;
    for (i = 0; i < _n; i++)
    {
        const V3* aPos = &mesh.Pos(_vertex[i]);
        // half-plane equation has form ax+by+c<=0
        // (a,b) is line normal

        Vector3 lineNormal(aPos->Z() - lPos->Z(), 0, lPos->X() - aPos->X());
        float checkIn = lineNormal * (pos - *aPos);
        if (checkIn < 0)
        {
            andIn = false;
        }
        else
        {
            orIn = true;
        }
        lPos = aPos;
    }
    if (orIn == andIn)
    {
        DoAssert(orIn); // it cannot be outside of all half-spaces
        // is in all half spaces or out of all half spaces
        // this can only mean it is inside (with no respect to face orientation)
        return 0;
    }
    else
    {
        // is outside - distance is distance to the nearest vertex
        float minDist2 = 1e30;
        for (i = 0; i < _n; i++)
        {
            Vector3Val aPos = mesh.Pos(_vertex[i]);
            float dist2 = (aPos - pos).SquareSizeXZ();
            saturateMin(minDist2, dist2);
        }
        return minDist2;
    }
}

bool PolyVertices::Inside(const VertexTable& mesh, const Plane& plane, Vector3Par pos) const
{
    if (_n < 3)
    {
        return false;
    }

    // perform all tests in face plane space
    // use cross product with face normal to get line normal in face plane
    int i;
    const V3* lPos = &mesh.Pos(_vertex[_n - 1]);
    bool orIn = false;
    bool andIn = true;
    for (i = 0; i < _n; i++)
    {
        const V3* aPos = &mesh.Pos(_vertex[i]);
        // half-plane equation has form ax+by+c<=0
        // (a,b) is line normal
        Vector3Val aml = (*aPos - *lPos);
        Vector3 lineNormal = aml.CrossProduct(plane.Normal());
        float checkIn = lineNormal * (pos - *aPos);
        if (checkIn < 0)
        {
            andIn = false;
        }
        else
        {
            orIn = true;
        }
        lPos = aPos;
    }
    if (orIn == andIn)
    {
        // it cannot be outside of all half-spaces
        AssertDebug(orIn);
        return orIn;
    }
    return false;
}

float PolyVertices::SquareDistance(const VertexTable& mesh, const Plane& plane, Vector3Par pos, Vector3* normal) const
{
    if (_n < 3)
    {
        return false;
    }
    if (normal)
    {
        *normal = plane.Normal();
    }
    // perform all tests in face plane space
    // use cross product with face normal to get line normal in face plane
    int i;
    // then perform exact check
    const V3* lPos = &mesh.Pos(_vertex[_n - 1]);
    bool orIn = false;
    bool andIn = true;
    for (i = 0; i < _n; i++)
    {
        const V3* aPos = &mesh.Pos(_vertex[i]);
        // half-plane equation has form ax+by+c<=0
        // (a,b) is line normal
        Vector3Val aml = (*aPos - *lPos);
        Vector3 lineNormal = aml.CrossProduct(plane.Normal());
        float checkIn = lineNormal * (pos - *aPos);
        if (checkIn < 0)
        {
            andIn = false;
        }
        else
        {
            orIn = true;
        }
        lPos = aPos;
    }
    if (orIn == andIn)
    {
        DoAssert(orIn); // it cannot be outside of all half-spaces
        // is in all half spaces or out of all half spaces
        // this can only mean it is inside (with no respect to face orientation)
        // note: Normal() is normalized
        Vector3Val p0 = mesh.Pos(_vertex[0]);
        return Square(plane.Normal() * (pos - p0));
    }
    else
    {
        // is outside - distance is distance to the nearest vertex
        float minDist2 = 1e30;
        for (i = 0; i < _n; i++)
        {
            Vector3Val aPos = mesh.Pos(_vertex[i]);
            float dist2 = aPos.Distance2(pos);
            if (minDist2 > dist2)
            {
                minDist2 = dist2;
            }
        }
        return minDist2;
    }
}

inline int Dominance(Vector3Par a, Vector3Par b)
{
    float d;
    d = a[2] - b[2];
    if (d < 0)
    {
        return -1;
    }
    if (d > 0)
    {
        return +1;
    }
    d = a[1] - b[1];
    if (d < 0)
    {
        return -1;
    }
    if (d > 0)
    {
        return +1;
    }
    d = a[0] - b[0];
    if (d < 0)
    {
        return -1;
    }
    if (d > 0)
    {
        return +1;
    }
    return 0;
}

inline Coord Intersect(Vector3Par in, Vector3Par out, Vector3Par normal, Coord d)
{
    Vector3 AmB = in - out;
    return (in * normal + d) / (AmB * normal);
}

bool PolyVertices::Clip(TLVertexTable& mesh, PolyVertices& res, Vector3Par normal, Coord d, ClipFlags test,
                        const Camera& camera)
{
    // initialize resulting polygon
    if (_n < 3)
    {
        res._n = 0;
        return true;
    }

    // search for first vertex inside clipping half-space
    int i;
    VertexIndex* pVertex; // previous vertex
    VertexIndex* aVertex; // actual vertex

    bool change = false;
    pVertex = _vertex + _n - 1;
    ClipFlags pOut = mesh.Clip(*pVertex) & test;

    int nClipped = 0;

    for (i = 0; i < _n; i++)
    {
        aVertex = _vertex + i;
        ClipFlags aOut = mesh.Clip(*aVertex) & test;
        // four possible situations
        if (aOut != pOut)
        {
            // edge going in or out
            int a = *aVertex;
            int p = *pVertex;
            Vector3 aPos = mesh.TransPosA(a);
            Vector3 pPos = mesh.TransPosA(p);
            if (Dominance(aPos, pPos) < 0)
            { // this should guarantee same rounding error on clipping
                swap(a, p);
                swap(aPos, pPos);
            }
            Coord t = Intersect(aPos, pPos, normal, d);
            res._vertex[nClipped++] = Interpolate(mesh, a, p, t, camera);
            change = true;
        }
        if (!aOut)
        {
            // point in
            res._vertex[nClipped++] = *aVertex;
        }
        else
        {
            change = true;
        }
        pVertex = aVertex;
        pOut = aOut;
    }

    PoseidonAssert(nClipped <= MaxPoly);
    if (nClipped < 3)
    {
        res._n = 0; // polygon completely out
    }
    else
    {
        res._n = nClipped;
    }
    return change;
}

// some operations require that inside test is performed, not pre-calculated
bool PolyVertices::Clip(TLVertexTable& mesh, PolyVertices& res, Vector3Par normal, Coord d)
{
    // initialize resulting polygon
    if (_n < 3)
    {
        res._n = 0;
        return true;
    }

    // search for first vertex inside clipping half-space
    int i;
    VertexIndex* pVertex; // previous vertex
    VertexIndex* aVertex; // actual vertex

    pVertex = _vertex + _n - 1;
    bool pOut = normal * mesh.TransPosA(*pVertex) + d < 0;

    int nClipped = 0;
    bool change = false;

    for (i = 0; i < _n; i++)
    {
        aVertex = _vertex + i;
        bool aOut = normal * mesh.TransPosA(*aVertex) + d < 0;
        // four possible situations
        if (aOut != pOut)
        {
            // edge going in or out
            int a = *aVertex;
            int p = *pVertex;
            Vector3 aPos = mesh.TransPosA(a);
            Vector3 pPos = mesh.TransPosA(p);
            if (Dominance(aPos, pPos) < 0)
            { // this should guarantee same rounding error on clipping
                swap(a, p);
                swap(aPos, pPos);
            }
            Coord t = Intersect(aPos, pPos, normal, d);
            // note: aPos and pPos may become invalid during Interpolate
            res._vertex[nClipped++] = Interpolate(mesh, a, p, t);
            change = true;
        }
        if (!aOut)
        {
            // point in
            res._vertex[nClipped++] = *aVertex;
        }
        else
        {
            change = true;
        }
        pVertex = aVertex;
        pOut = aOut;
    }

    PoseidonAssert(nClipped <= MaxPoly);
    if (nClipped < 3)
    {
        res._n = 0; // polygon completely out
    }
    else
    {
        res._n = nClipped;
    }
    return change;
}

void PolyVertices::Clip(TLVertexTable& mesh, const Camera& camera, ClipFlags clipFlags)
{
    int i;
    // this function should be called only if neccessary
    PoseidonAssert(clipFlags);
    ClipFlags clipOr = 0, clipAnd = clipFlags;
    for (i = 0; i < _n; i++)
    {
        ClipFlags clip = mesh.Clip(_vertex[i]);
        clipOr |= clip;
        clipAnd &= clip;
    }
    if (!clipOr)
    {
        return;
    }
    if (clipAnd)
    {
        _n = 0;
        return;
    }
    clipFlags &= clipOr;

    PolyVertices tempResult;
    PolyVertices* source = this;
    PolyVertices* result = &tempResult;
    if (clipFlags & ClipFront)
    {
        if (source->Clip(mesh, *result, Vector3(0, 0, +1), -camera.ClipNear(), ClipFront, camera))
        {
            swap(source, result);
        }
    }
    if (clipFlags & ClipBack)
    {
        if (source->Clip(mesh, *result, Vector3(0, 0, -1), camera.ClipFar(), ClipBack, camera))
        {
            swap(source, result);
        }
    }

    if (clipFlags & ClipLeft)
    {
        if (source->Clip(mesh, *result, Vector3(+1, 0, +1), 0, ClipLeft, camera))
        {
            swap(source, result);
        }
    }
    if (clipFlags & ClipRight)
    {
        if (source->Clip(mesh, *result, Vector3(-1, 0, +1), 0, ClipRight, camera))
        {
            swap(source, result);
        }
    }

    if (clipFlags & ClipTop)
    {
        if (source->Clip(mesh, *result, Vector3(0, +1, +1), 0, ClipTop, camera))
        {
            swap(source, result);
        }
    }
    if (clipFlags & ClipBottom)
    {
        if (source->Clip(mesh, *result, Vector3(0, -1, +1), 0, ClipBottom, camera))
        {
            swap(source, result);
        }
    }

    if (clipFlags & ClipUser0)
    {
        // we perform user clipping after view frustum clipping
        // therefore we are sure no more view frustum clipping will occur
        // and we may safely use Clip without camera argument
        if (source->Clip(mesh, *result, camera.UserClipDir(), camera.UserClipVal(), ClipUser0, camera))
        {
            swap(source, result);
        }
    }

    if (source != this)
    {
        *this = *source;
    }

    PoseidonAssert(_n <= MaxPoly);
    // Due to rounding errors it might seem that the new vertex must be clipped.
    // We are sure it is clipped - clear clipping flags
    // note: we cannot use SetTransPos, because it invalidates ScreenPos
    for (i = 0; i < _n; i++)
    {
        int index = _vertex[i];
        mesh.SetClip(index, mesh.Clip(index) & ~ClipAll);
    }
}

void PolyVertices::CheckClip(TLVertexTable& mesh, const Camera& camera, ClipFlags clipFlags)
{
    PolyVertices tempResult;
    PolyVertices* source = this;
    PolyVertices* result = &tempResult;
    if (clipFlags & ClipFront)
    {
        if (source->Clip(mesh, *result, Vector3(0, 0, +1), -camera.ClipNear()))
        {
            swap(source, result);
        }
    }
    if (clipFlags & ClipBack)
    {
        if (source->Clip(mesh, *result, Vector3(0, 0, -1), camera.ClipFar()))
        {
            swap(source, result);
        }
    }

    if (clipFlags & ClipLeft)
    {
        if (source->Clip(mesh, *result, Vector3(+1, 0, +1), 0))
        {
            swap(source, result);
        }
    }
    if (clipFlags & ClipRight)
    {
        if (source->Clip(mesh, *result, Vector3(-1, 0, +1), 0))
        {
            swap(source, result);
        }
    }

    if (clipFlags & ClipTop)
    {
        if (source->Clip(mesh, *result, Vector3(0, +1, +1), 0))
        {
            swap(source, result);
        }
    }
    if (clipFlags & ClipBottom)
    {
        if (source->Clip(mesh, *result, Vector3(0, -1, +1), 0))
        {
            swap(source, result);
        }
    }

    // result may be left in temporary
    if (source != this)
    {
        *this = *source;
    }

    if (_n < 3)
    {
        PoseidonAssert(_n == 0);
        return;
    }

    // Due to rounding errors it might seem that the new vertex must be clipped.
    // We are sure it is clipped - clear clipping flags
    for (int i = 0; i < _n; i++)
    {
        int index = _vertex[i];
        mesh.SetClip(index, mesh.Clip(index) & ~ClipAll);
        // verify vertex is within clipping range
        const Vector3& v = mesh.TransPosA(index);
        PoseidonAssert(v * Vector3(0, 0, +1) - camera.ClipNear() >= -1);
        PoseidonAssert(v * Vector3(0, 0, -1) + camera.ClipFar() >= -1);
        PoseidonAssert(v * Vector3(+1, 0, +1) >= -1);
        PoseidonAssert(v * Vector3(-1, 0, +1) >= -1);
        PoseidonAssert(v * Vector3(0, +1, +1) >= -1);
        PoseidonAssert(v * Vector3(0, -1, +1) >= -1);
    }
}

// some operations require that inside test is performed, not pre-calculated
void PolyVertices::Split(TLVertexTable& mesh, PolyVertices& clip, PolyVertices& rest, Vector3Par normal, Coord d)
{
    // initialize resulting polygon
    if (_n < 3)
    {
        rest._n = clip._n = 0;
        return;
    }

    // search for first vertex inside clipping half-space
    int i;
    VertexIndex* pVertex; // previous vertex
    VertexIndex* aVertex; // actual vertex

    pVertex = _vertex + _n - 1;
    bool pOut = normal * mesh.TransPosA(*pVertex) + d < 0;

    int nClipped = 0;
    int nRest = 0;

    for (i = 0; i < _n; i++)
    {
        aVertex = _vertex + i;
        bool aOut = normal * mesh.TransPosA(*aVertex) + d < 0;
        // four possible situations
        if (aOut != pOut)
        {
            // edge going in or out
            Coord t = Intersect(mesh.TransPosA(*aVertex), mesh.TransPosA(*pVertex), normal, d);
            rest._vertex[nRest++] = clip._vertex[nClipped++] = Interpolate(mesh, *aVertex, *pVertex, t);
        }
        if (!aOut)
        {
            clip._vertex[nClipped++] = *aVertex; // point in
        }
        else
        {
            rest._vertex[nRest++] = *aVertex; // point out (in rest)
        }
        pVertex = aVertex;
        pOut = aOut;
    }

    PoseidonAssert(nClipped <= MaxPoly);
    PoseidonAssert(nRest <= MaxPoly);

    if (nClipped < 3)
    {
        clip._n = 0; // polygon completely out
    }
    else
    {
        clip._n = nClipped;
    }
    if (nRest < 3)
    {
        rest._n = 0; // polygon completely out
    }
    else
    {
        rest._n = nRest;
    }
}

void PolyVertices::Split(const Matrix4& toWorld, Shape& mesh, PolyVertices& clip, PolyVertices& rest, Vector3Par normal,
                         Coord d)
{
    // source level splitting is used to create splitted shapes
    if (_n < 3)
    {
        rest._n = clip._n = 0;
        return;
    }

    // search for first vertex inside clipping half-space
    int i;
    VertexIndex* pVertex; // previous vertex
    VertexIndex* aVertex; // actual vertex

    Vector3 pPos, aPos;

    pVertex = _vertex + _n - 1;
    pPos.SetFastTransform(toWorld, mesh.Pos(*pVertex));

    bool pOut = pPos * normal + d < 0;

    int nClipped = 0;
    int nRest = 0;

    for (i = 0; i < _n; i++)
    {
        aVertex = _vertex + i;
        aPos.SetFastTransform(toWorld, mesh.Pos(*aVertex));
        bool aOut = aPos * normal + d < 0;
        // four possible situations
        if (aOut != pOut)
        {
            // edge going in or out
            Coord t = Intersect(aPos, pPos, normal, d);
            rest._vertex[nRest++] = clip._vertex[nClipped++] = Interpolate(mesh, *aVertex, *pVertex, t);
        }
        if (!aOut)
        {
            clip._vertex[nClipped++] = *aVertex; // point in
        }
        else
        {
            rest._vertex[nRest++] = *aVertex; // point out (in rest)
        }
        pVertex = aVertex;
        pOut = aOut;
        pPos = aPos;
    }

    PoseidonAssert(nClipped <= MaxPoly);
    PoseidonAssert(nRest <= MaxPoly);

    if (nClipped < 3)
    {
        clip._n = 0; // polygon completely out
    }
    else
    {
        clip._n = nClipped;
    }
    if (nRest < 3)
    {
        rest._n = 0; // polygon completely out
    }
    else
    {
        rest._n = nRest;
    }
}

bool PolyVertices::InsideFromX(const VertexTable& mesh, Vector3Par pos) const
{
    if (_n < 3)
    {
        return false;
    }

    Vector3 normal(-1, 0, 0); // test only incidence from one direction
    Vector3 pPos(0, pos[1], pos[2]);
    // perform all tests in face plane space
    // use cross product with face normal to get line normal in face plane
    int i;
    const V3* lPos = &mesh.Pos(_vertex[_n - 1]);
    bool andIn = true;
    bool orIn = false;
    for (i = 0; i < _n; i++)
    {
        const V3* aPos = &mesh.Pos(_vertex[i]);
        // half-plane equation has form ax+by+c<=0
        // (a,b) is line normal
        // optimize: CrossProduct with (-1,0,0) is:
        // return Vector3(0,-Z(),+Y());
        Vector3 lineNormal(0, lPos->Z() - aPos->Z(), aPos->Y() - lPos->Y());
        Vector3 aPPos(0, aPos->Y(), aPos->Z());
        float checkIn = lineNormal * (pPos - aPPos);
        if (checkIn < 0)
        {
            andIn = false;
        }
        else
        {
            orIn = true;
        }
        lPos = aPos;
    }
    return andIn == orIn;
}

void PolyVertices::FitToLandscape(TLVertexTable& mesh, Scene& scene, float y)
{
    int i;
    for (i = 0; i < _n; i++)
    {
        int index = _vertex[i];
        ClipFlags clip = mesh.Clip(index);
        Vector3 pos = mesh.TransPosA(index);
        Vector3 tPos(VFastTransform, scene.CamInvTrans(), pos);
        // Malprave terrainHole6: a vertex down in a terrain hole is fitted to the hole's roadway, not the terrain above
        float tY = scene.GetLandscape()->DecalSurfaceY(tPos[0], tPos[2], tPos[1]);
        ClipFlags landClip = clip & ClipLandMask;
        if (landClip == ClipLandOn)
        {
            tPos[1] = tY + y;
        }
        else if (landClip == ClipLandUnder)
        {
            tPos[1] = floatMin(tY, tPos[1]);
        }
        else if (landClip == ClipLandAbove)
        {
            tPos[1] = floatMax(tY + y, tPos[1]);
        }
        pos.SetFastTransform(scene.ScaledInvTransform(), tPos);
        mesh.SetTransPosA(index, pos);
    }
}
void PolyVertices::FitToLandscape(const Matrix4& toWorld, VertexTable& mesh, const Landscape& land, float y)
{
    int i;
    Matrix4 fromWorld(MInverseGeneral, toWorld);
    for (i = 0; i < _n; i++)
    {
        V3& pos = mesh.SetPos(_vertex[i]);
        ClipFlags clip = mesh.Clip(_vertex[i]);
        Vector3 tPos(VFastTransform, toWorld, pos);
        float tY = land.SurfaceY(tPos[0], tPos[2]);
        ClipFlags landClip = clip & ClipLandMask;
        if (landClip == ClipLandOn)
        {
            tPos[1] = tY + y;
        }
        else if (landClip == ClipLandUnder)
        {
            tPos[1] = floatMin(tY, tPos[1]);
        }
        else if (landClip == ClipLandAbove)
        {
            tPos[1] = floatMax(tY + y, tPos[1]);
        }
        pos.SetFastTransform(fromWorld, tPos);
    }
}
} // namespace Poseidon
