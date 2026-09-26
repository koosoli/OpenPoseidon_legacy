#include <string.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/StreamArray.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Memory/MemAlloc.hpp>
#if defined(_M_X64) || defined(_M_AMD64)
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Simulation/Animation/Animation.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/Foundation/Strings/StrFormat.hpp>

#include <Poseidon/IO/Streams/SerializeBin.hpp>

#include <Poseidon/Graphics/Rendering/Primitives/Edges.hpp>
#include <Poseidon/Graphics/Rendering/Draw/SpecLods.hpp>

#include <Poseidon/Foundation/Algorithms/Qsort.hpp>
#include <Poseidon/Foundation/Common/Filenames.hpp>

#include <Poseidon/Core/Data3D.h>

#include <Poseidon/World/MapTypes.hpp>

#include <Poseidon/Graphics/Rendering/Shape/ShapeShared.hpp>

namespace Poseidon
{
using Poseidon::Foundation::QSort;

#ifdef _MSC_VER
#pragma warning(disable : 4355)
#endif

void Shape::Clear()
{
    _plane.Clear();
    _textures.Clear();

    _sel.Clear();
    _prop.Clear();
    _phase.Clear();

    _face.Clear();
    VertexTable::ReleaseTables();
}

void Shape::CalculateColor()
{
    // calculate average of all texture colors
    // scan all faces and weight by area
    Color sum(HBlack), sumTop(HBlack);
    float sumA = 0, sumATop = 0;
    float sumArea = 0, sumAreaTop = 0;
    for (Offset i = BeginFaces(), e = EndFaces(); i < e; NextFace(i))
    {
        Poly& face = _face[i];
        Texture* texture = face.GetTexture();
        //  calculate area
        float area = face.GetArea(*this);
        sumArea += area;
        Color color = texture ? texture->GetColor() : Color(1, 1, 1, 1);
        sumA += color.A() * area;
        sum += color * (color.A() * area);
        // calculate area from top
        float areaTop = face.GetAreaTop(*this);
        sumAreaTop += areaTop;
        sumATop += color.A() * areaTop;
        sumTop += color * (color.A() * areaTop);
    }
    if (sumA > 0)
    {
        sum = sum * (1 / sumA);
    }
    if (sumArea > 0)
    {
        sum.SetA(sumA / sumArea);
    }
    _color = PackedColor(sum);
    if (sumATop > 0)
    {
        sumTop = sumTop * (1 / sumATop);
    }
    if (sumAreaTop > 0)
    {
        sumTop.SetA(sumATop / sumAreaTop);
    }
    _colorTop = PackedColor(sumTop);
}

void Shape::AutoClamp()
{
    //  make clamping flags per texture
    AUTO_STATIC_ARRAY(bool, tileU, 256);
    AUTO_STATIC_ARRAY(bool, tileV, 256);
    tileU.Resize(_textures.Size());
    tileV.Resize(_textures.Size());
    for (int i = 0; i < _textures.Size(); i++)
    {
        tileU[i] = tileV[i] = false;
    }
    for (Offset f = BeginFaces(); f < EndFaces(); NextFace(f))
    {
        const Poly& face = Face(f);
        // ignore explicit faces
        if (face.Special() & (NoClamp | ClampU | ClampV))
        {
            continue;
        }
#define CLAMP_EPS (1.0 / 128) // when should we clamp
        // #define CLAMP_EPS ( 0.1 ) // when should we clamp
        Texture* texture = face.GetTexture();
        if (!texture)
        {
            continue;
        }
        int textureIndex = _textures.Find(texture);
        if (textureIndex < 0)
        {
            Fail("Texture?");
            continue;
        }
        float minU = 1e10, maxU = -1e10;
        float minV = 1e10, maxV = -1e10;
        for (int vi = 0; vi < face.N(); vi++)
        {
            int vv = face.GetVertex(vi);
            float u = 0, v = 0;
            if (texture)
            {
                u = texture->UToLogical(U(vv));
                v = texture->VToLogical(V(vv));
            }
            saturateMin(minU, u);
            saturateMax(maxU, u);
            saturateMin(minV, v);
            saturateMax(maxV, v);
        }
        // force clamping if necessary
        if (minU < -CLAMP_EPS || maxU > 1 + CLAMP_EPS)
        {
            tileU[textureIndex] = true;
        }
        if (minV < -CLAMP_EPS || maxV > 1 + CLAMP_EPS)
        {
            tileV[textureIndex] = true;
        }
    }

    for (Offset f = BeginFaces(); f < EndFaces(); NextFace(f))
    {
        Poly& face = Face(f);
        if (face.Special() & (NoClamp | ClampU | ClampV))
        {
            continue;
        }
        if (!face.GetTexture())
        {
            face.OrSpecial(ClampU | ClampV);
            continue;
        }
        int textureIndex = _textures.Find(face.GetTexture());
        if (textureIndex < 0)
        {
            Fail("Texture?");
            continue;
        }
        int spec = 0;
        if (!tileU[textureIndex])
        {
            spec |= ClampU;
        }
        if (!tileV[textureIndex])
        {
            spec |= ClampV;
        }
        if (!spec)
        {
            spec |= NoClamp;
        }
        face.OrSpecial(spec);
    }
}

void Shape::BuildFaceIndexToOffset() const
{
    // already buildded
    if (NFaces() == _faceIndexToOffset.Size())
    {
        return;
    }
    // building neccessary
    _faceIndexToOffset.Realloc(NFaces());
    _faceIndexToOffset.Resize(NFaces());
    int i = 0;
    for (Offset o = BeginFaces(); o < EndFaces(); NextFace(o), i++)
    {
        _faceIndexToOffset[i] = o;
    }
}

void Shape::SetPoints(const Vector3* point, const ClipFlags* clip, int nPoints, const Vector3* normal, int nNormals
                      // const VertexMesh &mesh
)
{
    PoseidonAssert(nPoints == nNormals);
    ReallocTable(nPoints);
    int i;
    for (i = 0; i < nPoints; i++)
    {
        SetPos(i) = point[i];
        SetClip(i, clip[i]);
    }
    for (i = 0; i < nNormals; i++)
    {
        SetNorm(i) = normal[i];
    }
    CalculateHints(); // precalculate hints for possible optimizations
}

#ifdef _DEBUG
Offset OffsetChangeBase(Offset a, int b)
{
    a.Advance(b);
    return a;
}
#else
Offset OffsetChangeBase(Offset a, int b)
{
    return a + b;
}
#endif

void Shape::Merge(const Shape* with, const Matrix4& transform)
{
    // assume this and with have the same coordinate space
    // merge points and normals
    // create a permutation

    if (with->NPos() == with->NNorm() && NPos() == NNorm())
    {
        enum
        {
            maxTable = 256
        };
        AUTO_STATIC_ARRAY(int, pointWithToThis, maxTable);
        pointWithToThis.Resize(with->NPos());

        Reserve(with->NPos() + NPos());
        Vector3 pos, norm;
        for (int i = 0; i < with->NPos(); i++)
        {
            pos.SetFastTransform(transform, with->Pos(i));
            norm.SetRotate(transform, with->Norm(i));
            ClipFlags clip = with->Clip(i);
            int vertex = AddVertex(pos, norm, clip, with->U(i), with->V(i));
            //  no vertex information possible on merged shapes
            _vertexToPoint.Access(vertex);
            _vertexToPoint[vertex] = -1;
            pointWithToThis[i] = vertex;
        }
        PoseidonAssert(_face.VerifyStructure());
        PoseidonAssert(with->_face.VerifyStructure());
        int offsetDiff = OffsetDiff(EndFaces(), BeginFaces());
        _face.ReserveRaw(_face.RawSize() + with->_face.RawSize());
        // merge sections
        for (int s = 0; s < with->NSections(); s++)
        {
            const ShapeSection& sec = with->_face._sections[s];
            for (Offset f = sec.beg, e = sec.end; f < e; with->NextFace(f))
            {
                Offset index = _face.Add(with->Face(f));
                Poly& face = _face[index];
                for (int v = 0; v < face.N(); v++)
                {
                    face.Set(v, pointWithToThis[face.GetVertex(v)]);
                }
            }
            // merge faces
            ShapeSection dstSec = sec;
            dstSec.beg = OffsetChangeBase(sec.beg, offsetDiff);
            dstSec.end = OffsetChangeBase(sec.end, offsetDiff);
            AddSection(dstSec);
        }
        PoseidonAssert(_face.VerifyStructure());
    }
    else
    {
        Fail("Merge with old points/normals");
    }
    // merge textures
    for (int i = 0; i < with->_textures.Size(); i++)
    {
        int index = _textures.AddUnique(with->_textures[i]);
        if (index < 0)
        {
            index = _textures.Find(with->_textures[i]);
        }
        if (index < _areaOTex.Size())
        {
            saturateMax(_areaOTex[index], with->_areaOTex[i]);
        }
        else
        {
            _areaOTex.Access(index);
            _areaOTex[index] = with->_areaOTex[i];
        }
    }
    _special |= with->_special;
}

void Shape::MergeFast(const Shape* with, const Matrix4& transform)
{
    // assume this and with have the same coordinate space
    // no duplicate points/normals assumed
    int i;
    // create a permutation
    int oldNPos = NPos();
    //  reserve space for with->NPos()...

    if (with->NPos() == with->NNorm() && NPos() == NNorm())
    {
        Reserve(with->NPos() + NPos());
        Vector3 pos, norm;
        for (int i = 0; i < with->NPos(); i++)
        {
            pos.SetFastTransform(transform, with->Pos(i));
            norm.SetRotate(transform, with->Norm(i));
            ClipFlags clip = with->Clip(i);
            int vertex = AddVertexFast(pos, norm, clip, with->U(i), with->V(i));
            // no vertex information possible on merged shapes
            _vertexToPoint.Access(vertex);
            _vertexToPoint[vertex] = -1;
        }

        // merge meshes
        // merge faces
        _face.ReserveRaw(_face.RawSize() + with->_face.RawSize());
        for (Offset f = with->BeginFaces(), e = with->EndFaces(); f < e; with->NextFace(f))
        {
            Offset index = _face.Add(with->Face(f));
            Poly& face = _face[index];
            for (int v = 0; v < face.N(); v++)
            {
                face.Set(v, face.GetVertex(v) + oldNPos);
            }
        }
    }
    else
    {
        Fail("Merge with old points/normals");
    }
    // merge textures
    for (i = 0; i < with->_textures.Size(); i++)
    {
        int index = _textures.AddUnique(with->_textures[i]);
        if (index < _areaOTex.Size())
        {
            saturateMax(_areaOTex[index], with->_areaOTex[i]);
        }
        else
        {
            _areaOTex.Access(index);
            _areaOTex[index] = with->_areaOTex[i];
        }
    }
    _special |= with->_special;
}

struct MergedVertex
{
    Vector3 pos;
    int n;
    int sel;
    int pointIndex;
};

Shape* Shape::ExtractPath() // optimize this shape as path lod
{
    // scan all faces
    // replace vertices with degenerate
    // create a new shape with 2-vertex (degenerate) faces
    Shape* res = new Shape;

    res->_pointToVertex.Realloc(_pointToVertex.Size());
    res->_pointToVertex.Resize(_pointToVertex.Size());
    for (int i = 0; i < res->_pointToVertex.Size(); i++)
    {
        res->_pointToVertex[i] = -1;
    }

    // first of all convert all points to their respective "merged" variants
    // find longest of the shortest edges

    float maxMerge2 = 0;
    for (Offset o = BeginFaces(); o < EndFaces(); NextFace(o))
    {
        float shortestEdge2 = 1e10;
        const Poly& face = Face(o);
        for (int v = 0, p = face.N() - 1; v < face.N(); p = v++)
        {
            Vector3Val vpos = Pos(face.GetVertex(v));
            Vector3Val ppos = Pos(face.GetVertex(p));
            float dist2 = vpos.Distance2(ppos);
            saturateMin(shortestEdge2, dist2);
        }
        if (shortestEdge2 < Square(1.5))
        {
            saturateMax(maxMerge2, shortestEdge2);
        }
    }
    maxMerge2 *= 1.01;
    LOG_DEBUG(Graphics, "  longest shortest edge {:.1f}", sqrt(maxMerge2));
    saturate(maxMerge2, Square(0.5), Square(1.5));
    // or use property from model

    AutoArray<MergedVertex> vertex;
    Temp<int> vertexReplacedBy(NPos());

    res->_pointToVertex.Realloc(_pointToVertex.Size());
    res->_pointToVertex.Resize(_pointToVertex.Size());
    for (int i = 0; i < res->_pointToVertex.Size(); i++)
    {
        res->_pointToVertex[i] = -1;
    }
    for (int i = 0; i < NPos(); i++)
    {
        Vector3Val pos = Pos(i);
        // search if the point is already present
        // check i point selection
        // check i
        int pointI = VertexToPoint(i);
        int selI = -1;
        const char* name = "";
        for (int s = 0; s < NNamedSel(); s++)
        {
            if (NamedSel(s).IsSelected(i))
            {
                PoseidonAssert(selI < 0);
                selI = s;
                name = NamedSel(s).Name();
            }
        }
        float minDist2 = 1e10;
        int minJ = -1;
        for (int j = 0; j < vertex.Size(); j++)
        {
            float dist2 = pos.Distance2(vertex[j].pos);
            if (dist2 < minDist2)
            {
                minJ = j;
                minDist2 = dist2;
            }
        }
        if (minJ >= 0 && minDist2 <= maxMerge2)
        {
            MergedVertex& vtx = vertex[minJ];
            vtx.pos = (vtx.pos * vtx.n + pos) * (1 / (vtx.n + 1));
            vtx.n++;

            if (selI >= 0)
            {
                if (vtx.sel >= 0 && vtx.sel != selI)
                {
                    LOG_DEBUG(Graphics, "Double selection {}, {}", NamedSel(selI).Name(), NamedSel(vtx.sel).Name());
                }
                vtx.sel = selI;
                vtx.pointIndex = pointI;
            }
            vertexReplacedBy[i] = minJ;
        }
        else
        {
            int j = vertex.Add();
            vertex[j].pos = pos;
            vertex[j].n = 1;
            vertex[j].pointIndex = pointI;
            vertex[j].sel = selI;

            vertexReplacedBy[i] = j;
        }
    }
    //

    for (int j = 0; j < vertex.Size(); j++)
    {
        const MergedVertex& vtx = vertex[j];
        int vIndex = res->AddVertexFast(vtx.pos, VUp, ClipAll, 0, 0);
        res->_vertexToPoint.Access(vIndex);
        res->_vertexToPoint[vIndex] = vtx.pointIndex;
        res->_pointToVertex[vtx.pointIndex] = vIndex;
        if (vtx.sel >= 0)
        {
            NamedSelection oSel = NamedSel(vtx.sel);
            SelInfo info(vIndex, 255);
            NamedSelection sel(oSel.GetName(), &info, 1, nullptr, 0);
            res->AddNamedSel(sel);
        }
    }

    for (Offset o = BeginFaces(); o < EndFaces(); NextFace(o))
    {
        const Poly& face = Face(o);
        int n = face.N();
        //
        Poly resFace;
        resFace.Init();
        int resN = 0;
        // replace vertices
        for (int v = 0; v < n; v++)
        {
            // check edge pv,v
            int vIndex = face.GetVertex(v);
            int replaceBy = vertexReplacedBy[vIndex];
            // check if vertex is already present
            bool present = false;
            for (int p = 0; p < resN; p++)
            {
                if (resFace.GetVertex(p) == replaceBy)
                {
                    present = true;
                    break;
                }
            }
            if (!present)
            {
                resFace.Set(resN, replaceBy);
                ++resN;
            }
        }
        resFace.SetSpecial(NoClamp);
        resFace.SetN(resN);
        // longest edge is from p to v
        // vertices to merge are: p-1 with p and v with v+1

        if (resN > 1)
        {
            // note: resN should be 2
            res->AddFace(resFace);
        }
    }
    res->CalculateMinMax();
    res->CalculateHints();
    res->StoreOriginalMinMax();
    res->Compact();
    return res;
}

void Shape::SortVertices()
{
    // make permutation of vertices to that _vertexToPoint is sorted
    AUTO_STATIC_ARRAY(SortVertex, sort, 2048);
    sort.Resize(_vertexToPoint.Size());
    for (int i = 0; i < _vertexToPoint.Size(); i++)
    {
        sort[i].vertex = i;
        sort[i].point = _vertexToPoint[i];
        //  sort first by UserMask (Material index), then by LightMask
        sort[i].prior = _clip[i] & (ClipUserMask | ClipLightMask);
    }

    QSort(sort.Data(), sort.Size(), CmpSortVertex);
    // save current state
    AutoArray<ClipFlags> oClip = _clip;
    AutoArray<Vector3> oPos = _pos;
    AutoArray<Vector3> oNorm = _norm;
    AutoArray<VertexIndex> oVertexToPoint = _vertexToPoint;
    AutoArray<UVPair> oTex = _tex;
    AutoArray<UVPair> oTex1 = _tex1;
    const bool hasTex1 = _tex1.Size() >= _vertexToPoint.Size() && _tex1.Size() > 0;

    // change vertices to respect a new permutation
    for (int i = 0; i < _vertexToPoint.Size(); i++)
    {
        int newIndex = i;
        int oldIndex = sort[i].vertex;
        _clip[newIndex] = oClip[oldIndex];
        _pos[newIndex] = oPos[oldIndex];
        _norm[newIndex] = oNorm[oldIndex];
        _tex[newIndex] = oTex[oldIndex];
        if (hasTex1)
            _tex1[newIndex] = oTex1[oldIndex];
        _vertexToPoint[newIndex] = oVertexToPoint[oldIndex];
    }

    AUTO_STATIC_ARRAY(int, invSort, 2048);
    invSort.Resize(sort.Size());
    for (int i = 0; i < invSort.Size(); i++)
    {
        invSort[sort[i].vertex] = i;
    }
    // change all existing references to vertices
    for (int i = 0; i < _pointToVertex.Size(); i++)
    {
        int oldI = _pointToVertex[i];
        _pointToVertex[i] = invSort[oldI];
    }
    for (Offset o = BeginFaces(), e = EndFaces(); o < e; NextFace(o))
    {
        Poly& face = Face(o);
        for (int v = 0; v < face.N(); v++)
        {
            face.Set(v, invSort[face.GetVertex(v)]);
        }
    }
    // note: some references may be in selections
    DoAssert(_sel.Size() == 0);
}

class CompareTextureR
{
  public:
    int operator()(const Poly* p0, const Poly* p1);
};

int CompareTextureR::operator()(const Poly* p0, const Poly* p1)
{
    // check if some texture is alpha
    bool t0Alpha = (p0->Special() & IsAlphaOrdered) != 0;
    bool t1Alpha = (p0->Special() & IsAlphaOrdered) != 0;
    if (t0Alpha || t1Alpha)
    {
        if (t0Alpha)
        {
            if (t1Alpha)
            {
                return 0; // both alpha - keep order
            }
            return +1;
        }
        return -1;
    }
    const Texture* t0 = p0->GetTexture();
    const Texture* t1 = p1->GetTexture();
    return (char*)t0 - (char*)t1;
}

static int CompareTextureROffset(const StreamSortSegment* s1, const StreamSortSegment* s2, FaceArray* face)
{
    CompareTextureR functor;
    return functor(&face->Get(s1->beg), &face->Get(s2->beg));
}

void Shape::Optimize()
{
    PoseidonAssert(_sel.Size() == 0);
    // note: face indices will become invalid
    CompareTextureR functor;
    _face.QSortSeq(functor);
}

void Shape::AddSection(const ShapeSection& sec)
{
    // add section, respect selection boundaries
    // scan if we match any selection
    // mark faces that need to be added
    // check if section may be split

    // faces: all face offsets from the shape
    AUTO_STATIC_ARRAY(Offset, faces, 1024);
    int i = 0;
    faces.Resize(NFaces());
    for (Offset o = BeginFaces(); o < EndFaces(); NextFace(o), i++)
    {
        faces[i] = o;
    }

    // start with having one section
    AUTO_STATIC_ARRAY(ShapeSection, sections, 256);
    sections.Add(sec);

    AUTO_STATIC_ARRAY(ShapeSection, selections, 256);
    // selections are "sections" representing section-aware selections

    // convert all section-aware selections to section representation
    for (int i = 0; i < NNamedSel(); i++)
    {
        const NamedSelection& sel = NamedSel(i);
        if (!sel.GetNeedsSections())
        {
            continue;
        }
        // convert selection to section list
        Offset beg = Offset(0), end = Offset(0);
        bool open = false;
        for (int ff = 0; ff < sel.Faces().Size(); ff++)
        {
            int fi = sel.Faces()[ff];
            Offset fo = faces[fi];
            Offset fn = fo;
            NextFace(fn);
            if (!open)
            {
                beg = fo;
                end = fn;
                open = true;
            }
            else if (fo == end)
            {
                // extend section
                end = fn;
            }
            else
            {
                // flush section
                ShapeSection& selSec = selections.Append();
                selSec.beg = beg;
                selSec.end = end;
                // open a new section
                beg = fo;
                end = fn;
            }
        }
        if (open)
        {
            ShapeSection& selSec = selections.Append();
            selSec.beg = beg;
            selSec.end = end;
        }
    }

    // we must split the section by all selections that are section aware
    // note: in most cases this will mean no splitting, but generally
    // when splitting by n sections, the result might be x^n parts

    for (int i = 0; i < selections.Size(); i++)
    {
        const ShapeSection& sel = selections[i];
        // use sel to split all existing sections
        // each section
        int n = sections.Size(); // do not split sections added during splitting
        for (int s = 0; s < n; s++)
        {
            ShapeSection& sec = sections[s];
            // nothing to do if sec and sel do not overlap
            if (sec.beg >= sel.end || sec.end <= sel.beg)
            {
                continue;
            }

            // split sec.beg...sec.end by two boundaries: sel.beg and sel.end
            if (sel.beg > sec.beg && sel.beg < sec.end)
            {
                // sel.beg inside sec - split neccessary
                ShapeSection& secNew = sections.Append();
                secNew.properties = sec.properties;
                secNew.beg = sec.beg;
                secNew.end = sel.beg;
                sec.beg = sel.beg;
                //
            }
            if (sel.end > sec.beg && sel.end < sec.end)
            {
                // sel.end inside sec - split neccessary
                ShapeSection& secNew = sections.Append();
                secNew.properties = sec.properties;
                secNew.beg = sel.end;
                secNew.end = sec.end;
                sec.end = sel.end;
            }
            // splitting is necessary
        }
    }
    // add all constructed sections
    for (int i = 0; i < sections.Size(); i++)
    {
        _face._sections.Add(sections[i]);
    }
}

static int CmpSection(const ShapeSection* s1, const ShapeSection* s2)
{
    if (s1->beg > s2->beg)
    {
        return +1;
    }
    if (s1->beg < s2->beg)
    {
        return -1;
    }
    return 0;
}

// integer range - interval
/*
    used to represent set of integers: i>=beg && i<end
*/

struct Interval
{
    int beg, end;

    bool IsInside(int i) const { return i >= beg && i < end; }
    bool IsOverlapped(const Interval& j) const { return j.end > beg && j.beg < end; }
};

// memory effective implementation of
// integer set containing many continuos regions

class IntervalArray
{
    AutoArray<Interval> _data;

  public:
    IntervalArray();
    ~IntervalArray();

    void Delete(int i);

    void AddInterval(int beg, int end);
    bool IsPresent(int beg) const;
    bool IsEmpty() const { return _data.Size() == 0; }

    int NIntervals() const { return _data.Size(); }

    const Interval& GetInterval(int i) const { return _data.Get(i); }
    void Add(int i) { AddInterval(i, i + 1); }

    bool CheckIntegrity() const;
};

IntervalArray::IntervalArray() = default;
IntervalArray::~IntervalArray() {}

#define LOG_INTERVAL_OPS 0

void IntervalArray::Delete(int index)
{
    PoseidonAssert(CheckIntegrity());
#if LOG_INTERVAL_OPS
    Log("%08x: delete %d", this, index);
#endif
#if _DEBUG
    bool present = false;
    for (int i = 0; i < _data.Size(); i++)
    {
        Interval& ii = _data[i];
        if (!ii.IsInside(index))
        {
            continue;
        }
        present = true;
    }
    PoseidonAssert(present);
#endif

    for (int i = 0; i < _data.Size();)
    {
        if (!_data[i].IsInside(index))
        {
            i++;
            continue;
        }
#if LOG_INTERVAL_OPS
        int obeg = ii.beg;
        int oend = ii.end;
#endif
        // we found given interval
        // if one of end-points, we may simply remove it
        if (_data[i].beg < index && _data[i].end > index + 1)
        {
            // we need to split given interval
            Interval& ni = _data.Append();
            ni.beg = index + 1; // new interval (index,end)
            ni.end = _data[i].end;
            _data[i].end = index; // old interval <beg,index)
            PoseidonAssert(ni.end > ni.beg);
            PoseidonAssert(_data[i].end > _data[i].beg);
            PoseidonAssert(CheckIntegrity());
#if LOG_INTERVAL_OPS
            Log("  %d..%d split to %d..%d, %d..%d", obeg, oend, ii.beg, ii.end, ni.beg, ni.end);
#endif
        }
        else
        {
            PoseidonAssert(_data[i].end > _data[i].beg);
            if (_data[i].beg == index)
            {
                _data[i].beg++;
            }
            else if (_data[i].end == index + 1)
            {
                _data[i].end--;
            }
            if (_data[i].end == _data[i].beg)
            {
#if LOG_INTERVAL_OPS
                Log("  %d..%d deleted", obeg, oend);
#endif
                _data.Delete(i);
                PoseidonAssert(CheckIntegrity());
                break;
            }
            else
            {
#if LOG_INTERVAL_OPS
                Log("  %d..%d changed to %d..%d", obeg, oend, _data[i].beg, _data[i].end);
#endif
                PoseidonAssert(CheckIntegrity());
                PoseidonAssert(_data[i].end > _data[i].beg);
            }
        }
        // assume any number is contained in max. one interval
        break;
    }
// verify point is not present in any other selection
#if _DEBUG
    for (int i = 0; i < _data.Size(); i++)
    {
        Interval& ii = _data[i];
        if (!ii.IsInside(index))
        {
            continue;
        }
        Fail("Index present twice");
    }
#endif
    PoseidonAssert(CheckIntegrity());
}

void IntervalArray::AddInterval(int beg, int end)
{
#if LOG_INTERVAL_OPS
    Log("%08x: add %d..%d", this, beg, end);
#endif
    PoseidonAssert(end > beg);
    // if interval is overlapping or touching any other, merge it
    for (int i = 0; i < _data.Size(); i++)
    {
        Interval& ii = _data[i];
        if (ii.end > beg)
        {
            continue;
        }
        if (ii.beg < end)
        {
            continue;
        }
#if LOG_INTERVAL_OPS
        int obeg = ii.beg;
        int oend = ii.end;
#endif
        saturateMin(ii.beg, beg);
        saturateMax(ii.end, end);
        PoseidonAssert(ii.end > ii.beg);
#if LOG_INTERVAL_OPS
        Log("  %d..%d changed to %d..%d", obeg, oend, ii.beg, ii.end);
#endif
        PoseidonAssert(CheckIntegrity());
        return;
    }

    PoseidonAssert(end > beg);
    Interval& ni = _data.Append();
    ni.beg = beg;
    ni.end = end;
    PoseidonAssert(CheckIntegrity());
}

bool IntervalArray::IsPresent(int index) const
{
    for (int i = 0; i < _data.Size(); i++)
    {
        const Interval& ii = _data[i];
        if (ii.IsInside(index))
        {
            return true;
        }
    }
    return false;
}

bool IntervalArray::CheckIntegrity() const
{
    for (int i = 0; i < _data.Size(); i++)
    {
        const Interval& ii = _data[i];
        if (ii.beg >= ii.end)
        {
            LOG_ERROR(Graphics, "ii.beg>=ii.end: {},{}", ii.beg, ii.end);
            return false;
        }
    }
    for (int i = 0; i < _data.Size(); i++)
    {
        for (int j = 0; j < i; j++)
        {
            const Interval& ii = _data[i];
            const Interval& jj = _data[j];
            if (ii.IsOverlapped(jj))
            {
                LOG_ERROR(Graphics, "Overlapping {} and {} ({}..{}, {}..{})", i, j, ii.beg, ii.end, jj.beg, jj.end);
                return false;
            }
        }
    }
    return true;
}

struct ShapeSectionWork : public ShapeSectionInfo
{
    // used during section creation
    // FindArray<int> _faces; // set of face indices
    IntervalArray _faces;
};

struct FindShapeSections
{
    Shape* _shape;

    AutoArray<ShapeSectionWork> _sections;
    AutoArray<Offset> _offsets; // face index->offset helper

    FindShapeSections(Shape* shape);

    void CreateOffsets();
    int FindSection(const PolyProperties& prop, int material) const;
    int AssignSection(const PolyProperties& prop, int material);

    void SplitByNamedSelection(ShapeSectionWork& src,
                               ShapeSectionWork& split, // what is split-out
                               const NamedSelection& sel);

    void SplitByNamedSelection(const NamedSelection& sel);

    void Perform(bool forceMaterial0);
};

void FindShapeSections::CreateOffsets()
{
    _offsets.Resize(_shape->NFaces() + 1);
    int i = 0;
    for (Offset o = _shape->BeginFaces(); o < _shape->EndFaces(); _shape->NextFace(o), i++)
    {
        _offsets[i] = o;
    }
    _offsets[i] = _shape->EndFaces();
}

int FindShapeSections::FindSection(const PolyProperties& prop, int material) const
{
    for (int i = 0; i < _sections.Size(); i++)
    {
        const ShapeSectionWork& sw = _sections[i];
        if (sw.material != material)
        {
            continue;
        }
        if (sw.properties.GetTexture() != prop.GetTexture())
        {
            continue;
        }
        if (sw.properties.Special() != prop.Special())
        {
            continue;
        }
        return i;
    }
    return -1;
}

int FindShapeSections::AssignSection(const PolyProperties& prop, int material)
{
    int index = FindSection(prop, material);
    if (index >= 0)
    {
        return index;
    }
    index = _sections.Add();
    ShapeSectionWork& sw = _sections[index];
    sw.material = material;
    sw.properties = prop;
    return index;
}

FindShapeSections::FindShapeSections(Shape* shape) : _shape(shape) {}

void FindShapeSections::SplitByNamedSelection(ShapeSectionWork& src,
                                              ShapeSectionWork& split, // what is split-out
                                              const NamedSelection& sel)
{
    split.material = src.material;
    split.properties = src.properties;
    const Selection& faces = sel.Faces();
    for (int i = 0; i < faces.Size(); i++)
    {
        int fi = faces[i];
        // check if fo is in src
        // if it is in both section and selection, move it to split
        if (src._faces.IsPresent(fi))
        {
            split._faces.Add(fi);
            src._faces.Delete(fi);
        }
    }
    // src and split are now both correct in relation to sel
    // one (both?) of them may be empty now
}

void FindShapeSections::SplitByNamedSelection(const NamedSelection& sel)
{
    int ns = _sections.Size();
    // if _sections will grow, there is no need to test new sections
    for (int i = 0; i < ns; i++)
    {
        ShapeSectionWork& sw = _sections[i];
        ShapeSectionWork split;
        SplitByNamedSelection(sw, split, sel);
        if (split._faces.IsEmpty())
        {
            continue;
        }
        if (sw._faces.IsEmpty())
        {
            // nothing left in sw, use split instead of sw
            sw = split;
        }
        else
        {
            // two part - insert a new one
            _sections.Add(split);
        }
    }
}

struct SortSecPoly
{
    Offset polyBeg, polyEnd;
    const ShapeSectionWork* sec;
    int polyIndexBeg, polyIndexEnd;
};

static int CompareSecPoly(const SortSecPoly* s1, const SortSecPoly* s2)
{
    // first sort by section
    const ShapeSectionWork* sec1 = s1->sec;
    const ShapeSectionWork* sec2 = s2->sec;
    // sections: first sort by texture
    const Texture* t1 = sec1->properties.GetTexture();
    const Texture* t2 = sec2->properties.GetTexture();
    // and must retain given order
    // note: due to nasty trick used in tank cockpits
    // all paa textures should retain in given order
    bool t1Alpha = (sec1->properties.Special() & IsAlphaOrdered) != 0;
    bool t2Alpha = (sec2->properties.Special() & IsAlphaOrdered) != 0;
    if (t1Alpha != t2Alpha)
    {
        // alpha transparent goes last
        return t1Alpha - t2Alpha;
    }
    if (t1Alpha)
    {
        // both alpha - retain order
        if (s1->polyBeg > s2->polyBeg)
        {
            return +1;
        }
        if (s1->polyBeg < s2->polyBeg)
        {
            return -1;
        }
        return 0;
    }
    // non-alpha: may sort by section
    // none alpha: sort by texture
    int d = (const char*)t1 - (const char*)t2;
    if (d)
    {
        return d;
    }
    // next by special
    d = sec1->properties.Special() - sec2->properties.Special();
    if (d)
    {
        return d;
    }
    // next by material
    d = sec1->material - sec2->material;
    if (d)
    {
        return d;
    }
    // sections may have equal properties, but need to be
    // distiguished anyway
    d = (const char*)sec1 - (const char*)sec2;
    if (d)
    {
        return d;
    }
    // any order will do
    return 0;
}

void FindShapeSections::Perform(bool forceMaterial0)
{
    bool neededSplit = false;
    for (int i = 0; i < _shape->NNamedSel(); i++)
    {
        const NamedSelection& sel = _shape->NamedSel(i);
        if (!sel.GetNeedsSections())
        {
            continue;
        }
        neededSplit = true;
    }
    if (!neededSplit)
    {
        // detect singular case: sections already contiguos
        // each section may start only once
        AUTO_FIND_ARRAY(ShapeSection, sections, 256);
        ShapeSection* openSec = nullptr;
        bool singular = true;
        for (Offset o = _shape->BeginFaces(); o < _shape->EndFaces(); _shape->NextFace(o))
        {
            const Poly& poly = _shape->Face(o);
            int mat = forceMaterial0 ? 0 : poly.GetMaterial(*_shape);
            // if aplha sorting is required, we cannot use singular case
            if (poly.Special() & IsAlphaOrdered)
            {
                singular = false;
                break;
            }

            if (!openSec)
            {
                // no section started yet
                openSec = &sections.Append();
                openSec->material = mat;
                openSec->properties = poly;
                openSec->beg = o;
                openSec->end = o;
            }
            else if (openSec->material != mat || openSec->properties.GetTexture() != poly.GetTexture() ||
                     openSec->properties.Special() != poly.Special())
            {
                // check if section was already opened
                ShapeSection info;
                info.properties = poly;
                info.material = mat;
                if (sections.Find(info))
                {
                    singular = false;
                    break;
                }
                // close section
                openSec->end = o;
                _shape->NextFace(openSec->end);
                // section closed  - we need to open next section
                openSec = &sections.Append();
                openSec->material = mat;
                openSec->properties = poly;
                openSec->beg = o;
                openSec->end = o;
            }
        }
        if (singular)
        {
            // perform simplified conversion to sections
            // close last open sections if necessary
            if (openSec)
            {
                openSec->end = _shape->EndFaces();
            }
            // convert information from sections to _sections
            _shape->Faces().SetSections(sections.Data(), sections.Size());

            return;
        }
    }

    CreateOffsets();
    // define sections by properties
    int lastSection = -1;
    int lastSectionStart = -1;
    for (int i = 0; i < _shape->NFaces(); i++)
    {
        Offset fo = _offsets[i];
        const Poly& poly = _shape->Face(fo);
        int mat = forceMaterial0 ? 0 : poly.GetMaterial(*_shape);
        // note: polygons are often almost ordered
        // best candidate to search for section
        // is therefore section of last polygon
        if (lastSection >= 0)
        {
            ShapeSectionWork& ls = _sections[lastSection];
            if (ls.properties.GetTexture() == poly.GetTexture() && ls.properties.Special() == poly.Special() &&
                ls.material == mat)
            {
                continue;
            }
            PoseidonAssert(i > lastSectionStart);
            ls._faces.AddInterval(lastSectionStart, i);
            lastSectionStart = i;
        }
        int sec = AssignSection(poly, mat);
        lastSection = sec;
        lastSectionStart = i;
    }
    if (lastSection >= 0)
    {
        ShapeSectionWork& ls = _sections[lastSection];
        PoseidonAssert(_shape->NFaces() > lastSectionStart);
        ls._faces.AddInterval(lastSectionStart, _shape->NFaces());
    }
    // check all sections against all section-aware selections
    for (int i = 0; i < _shape->NNamedSel(); i++)
    {
        const NamedSelection& sel = _shape->NamedSel(i);
        if (!sel.GetNeedsSections())
        {
            continue;
        }
        if (sel.Faces().Size() <= 0)
        {
            continue;
        }
        // we must check all sections against this one
        SplitByNamedSelection(sel);
    }

    // sort faces to that sections are contiguos
    AUTO_STATIC_ARRAY(SortSecPoly, sortFaces, 256);
    sortFaces.Realloc(_shape->NFaces());
    // insert polygons from all sections
    for (int i = 0; i < _sections.Size(); i++)
    {
        const ShapeSectionWork& sw = _sections[i];
        for (int f = 0; f < sw._faces.NIntervals(); f++)
        {
            const Interval& i = sw._faces.GetInterval(f);
            PoseidonAssert(i.end > i.beg);
            Offset fBeg = _offsets[i.beg];
            Offset fEnd = _offsets[i.end];
            SortSecPoly& sp = sortFaces.Append();
            sp.polyBeg = fBeg;
            sp.polyEnd = fEnd;
            sp.polyIndexBeg = i.beg;
            sp.polyIndexEnd = i.end;
            PoseidonAssert(fEnd > fBeg);
            sp.sec = &sw;
        }
    }
    PoseidonAssert(sortFaces.Size() <= _shape->NFaces());
    // sort by alpha, texture, material ... as neccessary
    QSort(sortFaces.Data(), sortFaces.Size(), CompareSecPoly);
    // sortFaces is sorted
    // we have to reorder faces in shape now

    AutoArray<int> reorder;
    reorder.Resize(_shape->NFaces());
    int order = 0;
    for (int i = 0; i < sortFaces.Size(); i++)
    {
        int siBeg = sortFaces[i].polyIndexBeg;
        int siEnd = sortFaces[i].polyIndexEnd;
        for (int si = siBeg; si < siEnd; si++)
        {
            reorder[si] = order++;
        }
    }

    // this means reordering all named selections
    for (int s = 0; s < _shape->NNamedSel(); s++)
    {
        NamedSelection& sel = _shape->NamedSel(s);
        if (sel.Faces().Size() <= 0)
        {
            continue;
        }
        PoseidonAssert(!sel.FaceOffsetsReady());
        Selection& faceSel = sel.Faces();

        AutoArray<VertexIndex> newSel;
        newSel.Resize(faceSel.Size());
        for (int f = 0; f < faceSel.Size(); f++)
        {
            int of = faceSel[f];
            int nf = reorder[of];
            newSel[f] = nf;
        }
        faceSel = Selection(newSel.Data(), newSel.Size());
    }
    // now we can reorder all faces
    // we may directly create sections during final sort
    FaceArray sorted;
    sorted.RawRealloc(_shape->FacesRawSize());
    ShapeSection sec;
    const ShapeSectionWork* lastSec = nullptr;
    AutoArray<ShapeSection> sections;
    for (int i = 0; i < sortFaces.Size(); i++)
    {
        const SortSecPoly& sp = sortFaces[i];
        Offset oBeg = sorted.End();
        sorted.Merge((const char*)&_shape->Face(sp.polyBeg), OffsetDiff(sp.polyEnd, sp.polyBeg),
                     sp.polyIndexEnd - sp.polyIndexBeg);
        Offset oEnd = sorted.End();

        if (sp.sec != lastSec)
        {
            if (lastSec)
            {
                sections.Add(sec);
            }
            sec.properties = sp.sec->properties;
            sec.material = sp.sec->material;
            sec.beg = oBeg;
            sec.end = oEnd;
            lastSec = sp.sec;
        }
        else
        {
            PoseidonAssert(sec.end == oBeg);
            sec.end = oEnd;
        }
    }
    if (lastSec)
    {
        sections.Add(sec);
    }
    sorted.SetSections(sections.Data(), sections.Size());
    _shape->SetFaces(sorted);
    // now we have to check which selections contain which sections
    // check on by-selection basis
    // it is guranteed now section is either contained whole
    // or not at all in any section-aware selection

    // check all sections against all section-aware selections
    for (int i = 0; i < _shape->NNamedSel(); i++)
    {
        NamedSelection& sel = _shape->NamedSel(i);
        if (!sel.GetNeedsSections())
        {
            continue;
        }
        if (sel.Faces().Size() <= 0)
        {
            continue;
        }
        sel.FaceOffsets(_shape); // get ready face offsets
        sel.RescanSections(_shape);
    }
}

void ShapeSection::SerializeBin(SerializeBinStream& f, Shape* shape)
{
    f.TransferBinary(&beg, sizeof(beg));
    f.TransferBinary(&end, sizeof(end));
    f.Transfer(material);
    // transfer poly properties
    // use Shape to get texture index
    if (f.IsSaving())
    {
        Texture* tex = properties.GetTexture();
        int index = tex ? shape->GetTextureIndex(tex) : -1;
        f.SaveShort(index);
        f.SaveInt(properties.Special());
    }
    else
    {
        int index = f.LoadShort();
        // index off the wire — bound against the texture table (GetTexture is _textures[i]).
        Texture* texture = (index >= 0 && index < shape->NTextures()) ? shape->GetTexture(index) : nullptr;
        properties.SetTexture(texture);
        properties.SetSpecial(f.LoadInt());
        surfMat = GTexMaterialBank.TextureToMaterial(texture);
    }
}

void ShapeSection::PrepareTL(const TLMaterial& mat, const LightList& lights, int spec) const
{
    const render::LegacySpec specTyped = render::SplitLegacy(spec);
    if (surfMat)
    {
        // modulate material by material given in section
        TLMaterial matmod;
        surfMat->Combine(matmod, mat);
        GEngine->SetMaterial(matmod, lights, specTyped);
    }
    else
    {
        // set material given by model material supplier
        GEngine->SetMaterial(mat, lights, specTyped);
    }
    constexpr int inheritedFlags = NoDropdown | FogDisabled | DisableSun | IsColored;
    properties.PrepareTL(properties.GetTexture(), properties.Special() | (spec & inheritedFlags));
}

void Shape::FindSections(bool forceMaterial0)
{
    FindShapeSections find(this);
    find.Perform(forceMaterial0);
}

void LODShape::Optimize()
{
    for_each_alpha for (int level = 0; level < NLevels(); level++)
    {
        Shape* shape = Level(level);
        if (!shape)
        {
            continue;
        }
        shape->Optimize();
    }
}

void LODShape::Translate(Vector3Par offset)
{
    InvalidateQueryPolicy();
    for_each_alpha for (int level = 0; level < NLevels(); level++)
    {
        Shape* shape = Level(level);
        if (!shape)
        {
            continue;
        }
        for (int i = 0; i < shape->_pos.Size(); i++)
        {
            V3& pos = shape->_pos[i];
            pos += offset;
        }
        shape->RecalculateNormals(false);
    }
    _minMax[0] += offset;
    _minMax[1] += offset;
    _boundingCenter -= offset;
    if (_massArray.Size() > 0)
    {
        CalculateMass();
    }
    ScanProperties();
}

void LODShape::InternalTransform(const Matrix4& transform)
{
    InvalidateQueryPolicy();
    for_each_alpha for (int level = 0; level < NLevels(); level++)
    {
        Shape* shape = Level(level);
        if (!shape)
        {
            continue;
        }
        for (int i = 0; i < shape->_pos.Size(); i++)
        {
            V3& pos = shape->_pos[i];
            pos += _boundingCenter; // restore original position
            pos.SetFastTransform(transform, pos);
        }
        for (int p = 0; p < shape->_phase.Size(); p++)
        {
            for (int i = 0; i < shape->_phase[p].Size(); i++)
            {
                Vector3& pos = shape->_phase[p][i];
                pos += _boundingCenter;
                pos.SetFastTransform(transform, pos);
            }
        }
        _boundingCenter = VZero;
        shape->RecalculateNormals(true);
        shape->RecalculateAreas();
    }
    CalculateMinMax(true);
    if (_massArray.Size() > 0)
    {
        CalculateMass();
    }
    ScanProperties();
}

bool Shape::VerifyStructure() const
{
    return true;
}

void Shape::SetFaces(const FaceArray& src)
{
    _face = src;
    // all named selections must be reordered before calling this function
    // any other face offset should be invalidated
    _faceIndexToOffset.Clear();
}
void Shape::SetSpecial(int special)
{
    _special = special;
    for (Offset i = BeginFaces(), e = EndFaces(); i < e; NextFace(i))
    {
        Face(i).SetSpecial(special);
    }
    for (int s = 0; s < _face._sections.Size(); s++)
    {
        _face._sections[s].properties.SetSpecial(special);
    }
}

void Shape::MakeCockpit()
{
    const int flags = NoDropdown;
    _special |= flags;
    for (Offset i = BeginFaces(), e = EndFaces(); i < e; NextFace(i))
    {
        Face(i).OrSpecial(flags);
        // some faces can be with no texture at all
        Texture* tex = Face(i).GetTexture();
        if (tex)
        {
            if (tex->AMaxSize() < ENGINE_CONFIG.maxCockText)
            {
                tex->SetMaxSize(ENGINE_CONFIG.maxCockText);
            }
        }
    }
}

void Shape::OrSpecial(int special)
{
    _special |= special;
    for (Offset i = BeginFaces(), e = EndFaces(); i < e; NextFace(i))
    {
        _face[i].OrSpecial(special);
    }
    for (int i = 0; i < _face._sections.Size(); i++)
    {
        _face._sections[i].properties.OrSpecial(special);
    }
}
void Shape::AndSpecial(int special)
{
    _special &= special;
    for (Offset i = BeginFaces(), e = EndFaces(); i < e; NextFace(i))
    {
        _face[i].AndSpecial(special);
    }
    for (int i = 0; i < _face._sections.Size(); i++)
    {
        _face._sections[i].properties.AndSpecial(special);
    }
}

void Shape::Triangulate()
{
    const FaceArray source = _face;
    _face.Clear();
    for (Offset i = source.Begin(), e = source.End(); i < e; source.Next(i))
    {
        Poly src = source[i];
        int nv = src.N();
        if (nv > 3)
        {
#ifdef __ICL
#pragma novector
#endif
            // this one needs to be trianglulated
            for (int v = 2; v < nv - 1; v++)
            {
                // use triangle 0,v,v+1
                Poly dst;
                dst.Set(0, src.GetVertex(0));
                dst.Set(1, src.GetVertex(v));
                dst.Set(2, src.GetVertex(v + 1));
                dst.CopyProperties(src);
                dst.SetN(3);
            }
            src.SetN(3);
        }
        _face.Add(src);
    }
}

Texture* Shape::FindTexture(const char* name) const
{
    for (int t = 0; t < _textures.Size(); t++)
    {
        Texture* tex = _textures[t];
        if (tex && !strcmp(tex->Name(), name))
        {
            return tex;
        }
    }
    return nullptr;
}

Texture* LODShape::FindTexture(const char* name) const
{
    for (int level = 0; level < NLevels(); level++)
    {
        Texture* tex = Level(level)->FindTexture(name);
        if (tex)
        {
            return tex;
        }
    }
    return nullptr;
}

void Shape::RegisterTexture(Texture* texture, float areaOTex)
{
    if (!texture)
    {
        return;
    }
    // check if texture is present
    for (int t = 0; t < _textures.Size(); t++)
    {
        if (_textures[t] == texture)
        {
            saturateMax(_areaOTex[t], areaOTex);
            return;
        }
    }
    int index = _textures.Add(texture);
    _areaOTex.Access(index);
    _areaOTex[index] = areaOTex;
}

void Shape::RegisterTexture(Texture* texture, Texture* oldTexture)
{
    if (!texture)
    {
        return;
    }
    int index = -1;
    float areaOTex = 0;
    // check if texture is present
    for (int t = 0; t < _textures.Size(); t++)
    {
        if (_textures[t] == texture)
        {
            index = t;
            saturateMax(areaOTex, _areaOTex[t]);
            break;
        }
    }

    for (int t = 0; t < _textures.Size(); t++)
    {
        if (_textures[t] == oldTexture)
        {
            saturateMax(areaOTex, _areaOTex[t]);
            break;
        }
    }
    if (index < 0)
    {
        PoseidonAssert(_textures.Size() == _areaOTex.Size());
        index = _textures.Add(texture);
        _areaOTex.Add(areaOTex);
    }
    else
    {
        _areaOTex[index] = areaOTex;
    }
}

void Shape::RegisterTexture(Texture* texture, int selection)
{
    if (selection < 0)
    {
        return;
    }
    if (!texture)
    {
        return;
    }
    // get normal texture from selection
    const NamedSelection& sel = NamedSel(selection);
    const Selection& faceSel = sel.Faces();
    if (faceSel.Size() < 1)
    {
        return; // empty selection
    }
    int faceIndex = faceSel[0];
    const Poly& face = FaceIndexed(faceIndex);
    Texture* oldTexture = face.GetTexture();
    RegisterTexture(texture, oldTexture);
}

void LODShape::RegisterTexture(Texture* texture, const Animation& anim)
{
    // scan all LOD-levels
    if (!texture)
    {
        return;
    }
    for (int i = 0; i < NLevels(); i++)
    {
        Shape* shape = Level(i);
        shape->RegisterTexture(texture, anim.GetSelection(i));
    }
}

void Shape::RecalculateAreas()
{
    // scan all faces with same texture
    AUTO_STATIC_ARRAY(float, areas, 256);
    AUTO_STATIC_ARRAY(float, texts, 256);
    _areaOTex.Realloc(_textures.Size());
    _areaOTex.Resize(_textures.Size());
    areas.Resize(_textures.Size());
    texts.Resize(_textures.Size());
    for (int i = 0; i < _textures.Size(); i++)
    {
        _areaOTex[i] = 0;
        areas[i] = 0;
        texts[i] = 0;
    }
    const float maxAOT = 1e14;
    for (Offset i = BeginFaces(), e = EndFaces(); i < e; NextFace(i))
    {
        Poly& face = Face(i);
        if (face.N() < 3)
        {
            LOG_ERROR(Graphics, "Degenerate face - {} vertices", face.N());
            continue;
        }
        bool validVertices = true;
        for (int vertex = 0; vertex < face.N(); ++vertex)
        {
            const int index = face.GetVertex(vertex);
            if (index < 0 || index >= NVertex())
            {
                LOG_ERROR(Graphics, "RecalculateAreas: face vertex {} is outside [0,{})", index, NVertex());
                validVertices = false;
                break;
            }
        }
        if (!validVertices)
            continue;
        int index = _textures.Find(face.GetTexture());
        if (index >= 0)
        {
            float area = face.CalculateArea(*this);
            float text = face.CalculateUVArea(*this);

            float aot = maxAOT;
            if (area < maxAOT * text)
            {
                aot = area / text;
            }
            saturateMax(_areaOTex[index], aot);
        }
    }
}

void Shape::InitPlanes()
{
    if (_plane.Size() > 0)
    {
        return;
    }
    _plane.Realloc(_face.Size());
    _plane.Resize(_face.Size());
    RecalculateNormals(true);
}

void Shape::InvalidateNormals()
{
    _faceNormalsValid = false;
    InvalidateMinMax();
}

void Shape::RecalculateNormals(bool full)
{
    if (!full && !_faceNormalsValid)
    {
        return;
    }
    if (_plane.Size() > 0)
    {
        int p = 0;
        for (Offset i = BeginFaces(), e = EndFaces(); i < e; NextFace(i), p++)
        {
            Poly& face = Face(i);
            Plane& plane = GetPlane(p);
            if (full)
            {
                face.CalculateNormal(plane, *this);
            }
            else
            {
                face.CalculateD(plane, *this);
            }
        }
        PoseidonAssert(p == _plane.Size());
    }
    _faceNormalsValid = true;

    // if necessary, recalculate bsphere and minmax box
    if (_minMaxDirty)
    {
        CalculateMinMax();
    }
}

void Shape::Compact()
{
    VertexTable::Compact();
    _face.Compact();
    _sel.Compact();
    _prop.Compact();
    _phase.Compact();
    _textures.Compact();
}

void Shape::AddPhase(const AnimationPhase& phase)
{
    int i = 0;
    for (; i < _phase.Size(); i++)
    {
        if (_phase[i].Time() > phase.Time())
        {
            break;
        }
    }
    _phase.Insert(i, phase);
}

int Shape::PrevAnimationPhase(float time) const
{
    int i, n = _phase.Size();
    int prev = n - 1, next = 0;
    float prevTime = -1e10, nextTime = +1e10;
    for (i = 0; i < n; i++)
    {
        float pTime = _phase[i].Time();
        if (pTime <= time && pTime > prevTime)
        {
            prevTime = pTime, prev = i;
        }
        if (pTime > time && pTime < nextTime)
        {
            nextTime = pTime, next = i;
        }
    }
    return prev;
}

int Shape::NextAnimationPhase(float time) const
{
    int i, n = _phase.Size();
    int prev = n - 1, next = 0;
    float prevTime = -1e10, nextTime = +1e10;
    for (i = 0; i < n; i++)
    {
        float pTime = _phase[i].Time();
        if (pTime <= time && pTime > prevTime)
        {
            prevTime = pTime, prev = i;
        }
        if (pTime > time && pTime < nextTime)
        {
            nextTime = pTime, next = i;
        }
    }
    return next;
}

void Shape::SetPhaseIndex(int index)
{
    const AnimationPhase& pos = _phase[index];
    for (int i = 0; i < NPos(); i++)
    {
        SetPos(i) = pos[i];
    }
}

void Shape::PreparePhase(const AnimationPhase*& prevPos, const AnimationPhase*& nextPos, float& interpol, float time,
                         float baseTime) const
{
    bool looped = baseTime >= 0;
    // interpolate between two nearest phases
    prevPos = nextPos = &_phase[0]; // default to first phasis
    int n = _phase.Size();
    if (n < 2)
    {
        return; // no animation
    }
    int i = 0;
    int prev = 0, next = n - 1;
    float prevTime = -1e10, nextTime = +1e10;
    if (looped)
    {
        // ignore out of range negative phases
        for (i = 0; i < n; i++)
        {
            float pTime = _phase[i].Time();
            if (pTime > baseTime - 0.1)
            {
                break;
            }
        }
        if (i >= n)
        {
            return; // no positive phase
        }
        prev = n - 1, next = i;
        prevTime = -1e10, nextTime = +1e10;
    }
    for (; i < n; i++)
    {
        float pTime = _phase[i].Time();
        if (looped && pTime >= baseTime + 1)
        {
            break;
        }
        if (pTime <= time && pTime > prevTime)
        {
            prevTime = pTime, prev = i;
        }
        if (pTime > time && pTime < nextTime)
        {
            nextTime = pTime, next = i;
        }
    }
    prevPos = &_phase[prev];
    nextPos = &_phase[next];
    nextTime = nextPos->Time();
    prevTime = prevPos->Time();
    // cycled through 0 - we use mod 1 arithmetics
    if (prev == next)
    {
        interpol = 1;
    }
    else
    {
        float nextDelta = nextTime - prevTime;
        if (looped && nextDelta <= 0)
        {
            nextDelta += 1;
        }
        float timeDelta = time - prevTime;
        if (looped && timeDelta < 0)
        {
            timeDelta += 1;
        }
        interpol = timeDelta / nextDelta;
    }
}

void Shape::SetPhase(float time, float baseTime)
{
    if (IsAnimated())
    {
        const AnimationPhase *prevPos, *nextPos;
        float interpol;
        PreparePhase(prevPos, nextPos, interpol, time, baseTime);
        for (int i = 0; i < NPos(); i++)
        {
            SetPos(i) = ((*prevPos)[i] + ((*nextPos)[i] - (*prevPos)[i]) * interpol);
        }
        _faceNormalsValid = false;
    }
}

void Shape::SetPhase(const Selection& anim, float time, float baseTime)
{
    if (IsAnimated())
    {
        const AnimationPhase *prevPos, *nextPos;
        float interpol;
        PreparePhase(prevPos, nextPos, interpol, time, baseTime);
        for (int ai = 0; ai < anim.Size(); ai++)
        {
            int i = anim[ai];
            SetPos(i) = ((*prevPos)[i] + ((*nextPos)[i] - (*prevPos)[i]) * interpol);
        }
        _faceNormalsValid = false;
    }
}

Vector3 Shape::PointPhase(int i, float time, float baseTime) const
{
    if (IsAnimated())
    {
        const AnimationPhase *prevPos, *nextPos;
        float interpol;
        PreparePhase(prevPos, nextPos, interpol, time, baseTime);
        return (*prevPos)[i] + ((*nextPos)[i] - (*prevPos)[i]) * interpol;
    }
    else
    {
        return Pos(i);
    }
}

} // namespace Poseidon
