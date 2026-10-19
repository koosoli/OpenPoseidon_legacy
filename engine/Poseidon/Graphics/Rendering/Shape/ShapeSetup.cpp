#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/Foundation/Containers/StreamArray.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Memory/FastAlloc.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Memtype.h>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>
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

DEFINE_FAST_ALLOCATOR(Shape)

Shape::Shape() : _special(0), _faceNormalsValid(false)
{
    _color = _colorTop = PackedBlack;
    _loadWarning = false;
    _level = -1;
}

// ShapeSection is a group of faces that can be drawn with one call
// this means they share at least texture and flags
// in case of T&L engine they also need to share animation properties

Shape::~Shape()
{
    Clear();
}

Shape::Shape(const Shape& src, bool copyAnimations)
    : VertexTable(src), _face(src._face), _plane(src._plane), _color(src._color), _special(src._special),
      _prop(src._prop),                             // copy named properties
      _sel(src._sel),                               // copy named selections
      _level(src._level), _textures(src._textures), // copy texture list
      // BOOM-002: the per-texture areas belong with the texture list -- PrepareTextures indexes
      // _areaOTex by texture, and a copy without them read past an empty array on its first draw
      _areaOTex(src._areaOTex), _colorTop(src._colorTop),
      _pointToVertex(src._pointToVertex), _vertexToPoint(src._vertexToPoint),
      //_sections(src._sections),
      _faceNormalsValid(src._faceNormalsValid)
{
    if (copyAnimations)
    {
        _phase = src._phase;
    }
    _minMax[0] = src._minMax[0];
    _minMax[1] = src._minMax[1];
    for (int i = 0; i < _proxy.Size(); i++)
    {
        _proxy[i] = new ProxyObject(*src._proxy[i]);
    }
}
#ifdef _MSC_VER
#pragma warning(default : 4355)
#endif

int Shape::AppendVertexCopies(const int* sources, int count)
{
    const int first = _pos.Size();
    const bool tangents = _tangent.Size() == first, binormals = _binormal.Size() == first;
    const bool tex1 = _tex1.Size() == first && first > 0, authored = _authoredTangentVertices.Size() == first;
    const bool orig = _orig.Size() == first, origNorm = _origNorm.Size() == first, origClip = _origClip.Size() == first;
    const bool v2p = _vertexToPoint.Size() == first;
    for (int k = 0; k < count; ++k)
    {
        const int s = sources[k];
        _pos.Add(Vector3(_pos[s]));
        _clip.Add(ClipFlags(_clip[s]));
        _norm.Add(Vector3(_norm[s]));
        _tex.Add(UVPair(_tex[s]));
        if (tangents) _tangent.Add(Vector3(_tangent[s]));
        if (binormals) _binormal.Add(Vector3(_binormal[s]));
        if (tex1) _tex1.Add(UVPair(_tex1[s]));
        if (authored) _authoredTangentVertices.Add((unsigned char)_authoredTangentVertices[s]);
        if (orig) _orig.Add(Vector3(_orig[s]));
        if (origNorm) _origNorm.Add(Vector3(_origNorm[s]));
        if (origClip) _origClip.Add(ClipFlags(_origClip[s]));
        if (v2p) _vertexToPoint.Add(VertexIndex(_vertexToPoint[s]));
    }
    InvalidateBuffer();
    return first;
}

void Shape::CopyProxiesFrom(const Shape& src)
{
    _proxy.Clear();
    for (int i = 0; i < src._proxy.Size(); i++)
    {
        _proxy.Add(new ProxyObject(*src._proxy[i]));
    }
}

struct MTextureItem
{
    RStringB name; // path name (data\sample.pac)
    int x, y;      // position
    int w, h;      // orig. dimensions
    int scale;     // scale (can be negative - zoom out)
    int angle;

    void Load(const ParamEntry& entry);
};

struct MTextureSet : public AutoArray<MTextureItem>
{
    RString filename; // set texture name
    mutable LLink<Texture> tex;
    int w, h;

    void Load(const ParamEntry& entry);
    int Contains(RStringB name) const;
    Ref<Texture> GetTex() const;
};

class MTextureBank : public AutoArray<MTextureSet>
{
    bool _loaded;

  public:
    MTextureBank();
    void Load(QIStream& f);
    int FindBest(const RefArray<Texture>& find) const;
};

struct MTextureMap
{
    Ref<Texture> texture; // texture id
    int set, item;        // coordinades in merged bank
};

// Lazy initialization to avoid SIOF
static const RStringB& GetDataFolder()
{
    static const RStringB DataFolder = "data\\";
    return DataFolder;
}

void MTextureItem::Load(const ParamEntry& entry)
{
    RString lowName = GetDataFolder() + (entry >> "name").GetValue();
    lowName.Lower();
    name = lowName;
    x = entry >> "x";
    y = entry >> "y";
    angle = entry >> "angle";
    scale = entry >> "scale";
    w = entry >> "w";
    h = entry >> "h";
}

void MTextureSet::Load(const ParamEntry& entry)
{
    filename = entry >> "file";
    filename.Lower();
    w = entry >> "w";
    h = entry >> "h";
    const ParamEntry& a = entry >> "Items";
    for (int i = 0; i < a.GetEntryCount(); i++)
    {
        const ParamEntry& e = a.GetEntry(i);
        MTextureItem& item = Set(Add());
        item.Load(e);
    }
}

int MTextureSet::Contains(RStringB name) const
{
    for (int i = 0; i < Size(); i++)
    {
        const MTextureItem& item = Get(i);
        if (item.name == name)
        {
            return i;
        }
    }
    return -1;
}

Ref<Texture> MTextureSet::GetTex() const
{
    if (tex)
    {
        return tex.GetLink();
    }
    // compose name from partial name
    Ref<Texture> texture = GlobLoadTexture(filename);
    Texture* castTemp = texture;
    tex = castTemp;
    return texture;
}

MTextureBank::MTextureBank()
{
    _loaded = false;
}

void MTextureBank::Load(QIStream& f)
{
    if (_loaded)
    {
        return;
    }
    _loaded = true;

    ParamFile file;
    file.Parse(f);
    if (file.GetEntryCount() <= 0)
    {
        return;
    }
    const ParamEntry& a = file >> "InfTexMerge";
    for (int i = 0; i < a.GetEntryCount(); i++)
    {
        const ParamEntry& e = a.GetEntry(i);
        if (!e.IsClass())
        {
            continue;
        }
        MTextureSet& set = Set(Add());
        set.Load(e);
    }
}

int MTextureBank::FindBest(const RefArray<Texture>& find) const
{
    // check all textures
    // consider only sets that contain at least two textures from texture array
    // prefer merged - may be shared between lods
    int maxCount = 0;
    int bestI = -1;
    for (int i = 0; i < Size(); i++)
    {
        const MTextureSet& set = Get(i);
        int count = 0;
        // check by name
        for (int t = 0; t < find.Size(); t++)
        {
            if (!find[t])
            {
                continue; // Note: do not include nullptr texture in the list
            }
            if (set.Contains(find[t]->GetName()) >= 0)
            {
                count++;
            }
        }
        if (count > maxCount)
        {
            maxCount = count;
            bestI = i;
        }
    }
    return bestI;
}

static MTextureBank MergedTextures;

#if _DEBUG
#define VERBOSE 1
#else
#define VERBOSE 0
#endif

static bool LoadTag(QIStream& f, char* name, int& size)
{
    f.read(name, 64);
    f.read((char*)&size, sizeof(size));
    return !(f.fail() || f.eof());
}

// GMergeTextures migrated to ENGINE_CONFIG.gMergeTextures
// convert old point attributes
// to new material indices (introduced on 12 Feb 2001)

const int fullFlags = static_cast<int>(MSFullLighted) * static_cast<int>(ClipUserStep);
const int halfFlags = static_cast<int>(MSHalfLighted) * static_cast<int>(ClipUserStep);
const int ambFlags = static_cast<int>(MSInShadow) * static_cast<int>(ClipUserStep);
const int shineFlags = static_cast<int>(MSShining) * static_cast<int>(ClipUserStep);

struct OxygenLEOriginTag
{
    int prot_componumber;
    int prot_magicnumber;
    int prot_unlockcode;
    int prot_requestcode;
};

float Shape::LoadTagged(QIStream& f, bool reversed, int ver, bool geometryOnly, AutoArray<float>& massArray,
                        bool tagged)
{
    _loadWarning = false;
    _face.Clear();

    bool extended = false;
    DataHeaderEx head;
    f.read((char*)&head, sizeof(head));
    if (f.fail() || strncmp(head.magic, "SP3X", 4))
    {
        // is not extended format, try old
        f.seekg(-(int)sizeof(head), QIOS::cur);
        DataHeader oHead;
        f.read((char*)&oHead, sizeof(oHead));
        if (f.fail() || strncmp(oHead.magic, "SP3D", 4))
        {
            char magicName[5];
            strncpy(magicName, head.magic, 4);
            magicName[4] = 0;
            WarningMessage("Bad file format (%s).", magicName);
            return 0; // file input error
        }
        head.version = 0;
        head.nPos = oHead.nPos;
        head.nNorm = oHead.nNorm;
        head.nFace = oHead.nFace;
        head.flags = 0;
        head.headSize = sizeof(oHead);
    }
    else
    {
        extended = true;
        // skip rest of header
        f.seekg(head.headSize - sizeof(head), QIOS::cur);
    }

    _face.Realloc(head.nFace);

    _pos.Realloc(head.nPos);
    _clip.Realloc(head.nPos);
    _norm.Realloc(head.nNorm);

#define maxPoints 2048
    AUTO_STATIC_ARRAY(Vector3, pos, maxPoints);
    AUTO_STATIC_ARRAY(Vector3, norm, maxPoints);
    AUTO_STATIC_ARRAY(ClipFlags, clip, maxPoints);
    AUTO_STATIC_ARRAY(bool, hidden, maxPoints);
    pos.Resize(head.nPos);
    clip.Resize(head.nPos);
    hidden.Resize(head.nPos);
    norm.Resize(head.nNorm);
    _pointToVertex.Resize(head.nPos);
    _vertexToPoint.Realloc(head.nPos);
    for (int i = 0; i < _pointToVertex.Size(); i++)
    {
        _pointToVertex[i] = -1;
    }

    // load points, normals and mapping pairs
    bool reportInvalid = true;
    for (int i = 0; i < head.nPos; i++)
    {
        if (!extended)
        {
            DataPoint src;
            f.read((char*)&src, sizeof(src));
            if (f.fail())
            {
                break;
            }
            pos[i] = Vector3(src.X(), src.Y(), src.Z());
            clip[i] = ClipAll;
            hidden[i] = false;
        }
        else
        {
            DataPointEx src;
            f.read((char*)&src, sizeof(src));
            if (f.fail())
            {
                break;
            }

            ClipFlags hints = ClipAll;
            const int allFlags = (POINT_ONLAND | POINT_UNDERLAND | POINT_ABOVELAND | POINT_KEEPLAND | POINT_DECAL |
                                  POINT_VDECAL | POINT_NOLIGHT | POINT_FULLLIGHT | POINT_HALFLIGHT | POINT_AMBIENT |
                                  POINT_NOFOG | POINT_SKYFOG | POINT_USER_MASK | POINT_SPECIAL_MASK);
            hidden[i] = false;
            // check for valid flags - detect objektiv bug
            if (src.flags & ~allFlags)
            {
                src.flags = 0;
                if (reportInvalid)
                {
                    reportInvalid = false;
                    LOG_DEBUG(Graphics, "Invalid point flags.");
                    _loadWarning = true;
                }
            }
            if (src.flags & allFlags)
            {
                if (src.flags & POINT_ONLAND)
                {
                    hints |= ClipLandOn;
                }
                else if (src.flags & POINT_UNDERLAND)
                {
                    hints |= ClipLandUnder;
                }
                else if (src.flags & POINT_ABOVELAND)
                {
                    hints |= ClipLandAbove;
                }
                else if (src.flags & POINT_KEEPLAND)
                {
                    hints |= ClipLandKeep;
                }

                if (src.flags & POINT_DECAL)
                {
                    hints |= ClipDecalNormal;
                }
                else if (src.flags & POINT_VDECAL)
                {
                    hints |= ClipDecalVertical;
                }

                // convert lighting flags to materials
                if (src.flags & POINT_NOLIGHT)
                {
                    hints |= shineFlags;
                }
                else if (src.flags & POINT_FULLLIGHT)
                {
                    hints |= fullFlags;
                }
                else if (src.flags & POINT_HALFLIGHT)
                {
                    hints |= halfFlags;
                }
                else if (src.flags & POINT_AMBIENT)
                {
                    hints |= ambFlags;
                }

                if (src.flags & POINT_NOFOG)
                {
                    hints |= ClipFogDisable;
                }
                else if (src.flags & POINT_SKYFOG)
                {
                    hints |= ClipFogSky;
                }

                if (src.flags & POINT_USER_MASK)
                {
                    int user = (src.flags & POINT_USER_MASK) / POINT_USER_STEP;
                    hints |= user * ClipUserStep;
                }
                if (src.flags & POINT_SPECIAL_HIDDEN)
                {
                    hidden[i] = true;
                }
            }
            if (reversed)
            {
                src[0] = -src[0], src[2] = -src[2];
            }
            pos[i] = Vector3(src.X(), src.Y(), src.Z());
            clip[i] = hints;
        }
    }

    for (int i = 0; i < head.nNorm; i++)
    {
        DataNormal src;
        f.read((char*)&src, sizeof(src));
        if (f.fail())
        {
            break;
        }
        if (reversed)
        {
            src[0] = -src[0], src[2] = -src[2];
        }
        if (!_finite(src[0]) || _isnan(src[0]) || !_finite(src[1]) || _isnan(src[1]) || !_finite(src[2]) ||
            _isnan(src[2]))
        {
            if (reportInvalid)
            {
                LOG_DEBUG(Graphics, "Invalid normal");
                reportInvalid = false;
            }
            src[0] = 0, src[1] = 1, src[2] = 0;
            _loadWarning = true;
        }
        // normals in geometry ignored to avoid vertex multiplication
        if (geometryOnly)
        {
            src[0] = 0, src[1] = 1, src[2] = 0;
        }
        norm[i] = Vector3(src.X(), src.Y(), src.Z());
    }

    int andSpecial = ~0;
    // load faces
    for (int i = 0; i < head.nFace; i++)
    {
        DataFaceEx srcFace;
        if (!extended)
        {
            DataFace oFace;
            f.read((char*)&oFace, sizeof(oFace));
            if (f.fail())
            {
                break;
            }
            srcFace.DataFace::operator=(oFace);
            srcFace.flags = 0;
        }
        else
        {
            f.read((char*)&srcFace, sizeof(srcFace));
            if (f.fail())
            {
                break;
            }
        }

        int spec = 0;
        Poly poly;
        Ref<Texture> texture;
        if (!*srcFace.texture)
        {
            texture = GetDefaultTexture();
            _textures.AddUnique(texture);
        }
        else
        {
            strlwr(srcFace.texture);
            // search object textures first
            for (int t = 0; t < _textures.Size(); t++)
            {
                Texture* txt = _textures[t];
                if (!txt)
                {
                    continue;
                }
                if (!strcmp(txt->Name(), srcFace.texture))
                {
                    texture = txt;
                    break;
                }
            }
            if (!texture)
            {
                // texture not present in this object - load new
                // detect animated texture
                texture = GlobLoadTexture(srcFace.texture);
                if (texture)
                {
                    _textures.Add(texture);
                }
            }
        }
        PoseidonAssert(!texture || _textures.Find(texture) >= 0);
        poly.SetTexture(texture);
        // vertex count off the wire indexes srcFace.vs[MAX_DATA_POLY] (below) and poly's
        // fixed _vertex buffer; drop a face whose count is out of range.
        if (srcFace.n < 0 || srcFace.n > MAX_DATA_POLY)
        {
            srcFace.n = 0;
        }
        poly.SetN(srcFace.n);
        bool skipPoly = true;

        const int allFlags = (FACE_NOLIGHT | FACE_AMBIENT | FACE_FULLLIGHT | FACE_BOTHSIDESLIGHT | FACE_SKYLIGHT |
                              FACE_REVERSELIGHT | FACE_FLATLIGHT | FACE_ISSHADOW | FACE_NOSHADOW |
                              FACE_DISABLE_TEXMERGE | FACE_USER_MASK | FACE_Z_BIAS_MASK);
        // check for valid flags - detect objektiv bug
        if (srcFace.flags & ~allFlags)
        {
            // some invalid flags
            if (reportInvalid)
            {
                LOG_DEBUG(Graphics, "Invalid face flags");
                reportInvalid = false;
                _loadWarning = true;
            }

            srcFace.flags = 0; // reset - assume it is result of Objektiv bug
        }
        ClipFlags clipLightAttr = 0;
        if (srcFace.flags & allFlags)
        {
            if (srcFace.flags & FACE_NOLIGHT)
            {
                clipLightAttr |= shineFlags;
            }
            else if (srcFace.flags & FACE_AMBIENT)
            {
                clipLightAttr |= ambFlags;
            }
            else if (srcFace.flags & FACE_FULLLIGHT)
            {
                clipLightAttr |= fullFlags;
            }
            if (srcFace.flags & FACE_ISSHADOW)
            {
                spec |= IsShadow;
            }
            if (srcFace.flags & FACE_NOSHADOW)
            {
                spec |= NoShadow;
            }
            if (srcFace.flags & FACE_DISABLE_TEXMERGE)
            {
                spec |= NoTexMerger;
            }
            if (srcFace.flags & FACE_USER_MASK)
            {
                int material = ((srcFace.flags & FACE_USER_MASK) >> FACE_USER_SHIFT) & 0xff;
                clipLightAttr = material * ClipUserStep;
            }
            if (srcFace.flags & FACE_Z_BIAS_MASK)
            {
                int bias = (srcFace.flags & FACE_Z_BIAS_MASK) / FACE_Z_BIAS_STEP;
                spec |= ZBiasStep * bias;
            }
        }

        for (int j = 0; j < srcFace.n; j++)
        {
            int srcPoint = srcFace.vs[j].point;
            int srcNormal = srcFace.vs[j].normal;
            float srcU = srcFace.vs[j].mapU;
            float srcV = srcFace.vs[j].mapV;
#if 1 // always validate UV coordinates
            if (fabs(srcU) > 1e5 || fabs(srcV) > 1e5)
            {
                srcU = srcV = 0;
                if (reportInvalid)
                {
                    LOG_DEBUG(Graphics, "Invalid uv coordinates");
                    reportInvalid = false;
                    _loadWarning = true;
                }
            }

#endif
            // uv in geometry ignored to avoid vertex multiplication
            if (geometryOnly)
            {
                srcU = srcV = 0;
            }

            if (texture)
            {
                srcU = texture->UToPhysical(srcU);
                srcV = texture->VToPhysical(srcV);
            }

            if (!hidden[srcPoint])
            {
                skipPoly = false;
            }

            ClipFlags c = clip[srcPoint];
            if (clipLightAttr)
            {
                // combine material with old flags
                // shining is offset +20
                // inShadow is offset +40
                ClipFlags user = c & ClipUserMask;
                if (user)
                {
                    // note: offsetting is quite a hack
                    // we should rather reserve some bits for special lighting
                    // and some leave to user material definition
                    int offset = 0;
                    if (user == shineFlags)
                    {
                        offset = 20;
                    }
                    else if (user == ambFlags)
                    {
                        offset = 40;
                    }
                    c &= ~ClipUserMask;
                    c |= clipLightAttr + offset * ClipUserStep;
                }
                else
                {
                    c |= clipLightAttr;
                }
            }
            ClipFlags fog = c & ClipFogMask;
            if (fog == ClipFogSky || fog == ClipFogDisable)
            {
                c &= ~ClipBack;
            }

            int vertex = AddVertex(pos[srcPoint], norm[srcNormal], c, srcU, srcV, &_vertexToPoint, srcPoint);

#if VERBOSE > 2
            LOG_DEBUG(Graphics, "    vertex {} from {},{}, clip {:x}", vertex, srcPoint, srcNormal, clip[srcPoint]);
#endif

            poly.Set(j, vertex);

            _vertexToPoint.Access(vertex);
            _vertexToPoint[vertex] = srcPoint;
            _pointToVertex[srcPoint] = vertex; // objektiv point index to vertex index conversion
        }

        poly.Reverse(); // models have reversed geometry

        // lighting attributes must be applied to all vertices as well
        for (int j = 0; j < srcFace.n; j++)
        {
            int srcPoint = srcFace.vs[j].point;
            ClipFlags c = clip[srcPoint];
            ClipFlags land = c & ClipLandMask;
            if (land == ClipLandOn)
            {
                spec |= OnSurface;
            }
        }

        if (texture)
        {
            texture->LoadHeaders();
            if (texture->IsAlpha())
            {
                spec |= IsAlpha | IsAlphaOrdered;
            }
            if (texture->IsTransparent())
            {
                spec |= IsTransparent;
            }
            if (texture->IsAnimated())
            {
                spec |= ::IsAnimated;
            }
            // trick - disable alpha ordering by extension
            if (!strcmpi(GetFileExt(texture->Name()), ".paa"))
            {
                // another trick - merged textures should not be alpha ordered
                if (!strstr(texture->Name(), "\\000"))
                {
                    spec |= IsAlphaOrdered;
                }
            }
            // check special case ".paa"
        }
        if (skipPoly)
        {
            // this is currently used only for proxy objects
            // it is safe to disable texture merging on them
            spec |= IsHidden | NoTexMerger;
        }

        poly.SetSpecial(spec);
        _special |= spec;
        andSpecial &= spec;
        _face.Add(poly);
    }

    // make sure all points are represented with vertices

    for (int i = 0; i < _pointToVertex.Size(); i++)
    {
        if (_pointToVertex[i] < 0)
        {
            int vertex = AddVertex(pos[i], VZero, clip[i], // normal set to zero - should not be used
                                   0, 0,                   // no u-v mapping
                                   &_vertexToPoint, i);
            _pointToVertex[i] = vertex;
            _vertexToPoint.Access(vertex);
            _vertexToPoint[vertex] = i;
        }
    }

#if 1 // enable/disable material optimization

    SortVertices();

#endif

    _special &= IsAlpha | IsTransparent | ::IsAnimated | OnSurface;
    _special |= andSpecial & (NoShadow | ZBiasMask);
    CalculateHints(); // VertexTable optimization
    if (GetOrHints() & ClipLandOn)
    {
        PoseidonAssert(_special & OnSurface);
    }
    if (f.fail())
    {
        WarningMessage("Bad object.");
        Clear();
        return 0; // file input error
    }
    _pos.Compact();
    _norm.Compact();
    _clip.Compact();
    _vertexToPoint.Compact();
    _pointToVertex.Compact();
    AutoClamp();
    RecalculateAreas();
    RecalculateNormals(true);
    CalculateMinMax();
    StoreOriginalMinMax();
    CalculateColor();

    if (!geometryOnly && ENGINE_CONFIG.gMergeTextures)
    {
        MergeTextures(false);
    }

    // skip all tags (if any), terminate at #EndOfFile#
    // check for TAGG magic
    char magic[4];
    f.read(magic, sizeof(magic));
    if (f.fail() || f.eof())
    {
        WarningMessage("Error loading tag");
        _loadWarning = true;
        return 0;
    }
    if (strncmp("TAGG", magic, sizeof(magic)))
    {
        WarningMessage("Bad format");
        _loadWarning = true;
        return 0;
    }

    _sel.Clear(); // clear selections
    for (;;)
    {
        int tagSize;
        char tagName[65] = {}; // 64 wire bytes + guaranteed NUL so the C-string uses below can't over-read
        if (!LoadTag(f, tagName, tagSize))
        {
            break;
        }
        else if (!strcmpi(tagName, "#EndOfFile#"))
        {
            break;
        }
        else if (!strcmpi(tagName, "#Mass#"))
        {
            if (ver == 0)
            {
                LOG_DEBUG(Graphics, "{}: Old mass no longer supported");
                f.seekg(tagSize, QIOS::cur); // skip tag
            }
            else
            {
                // only geometry level stores mass information in 1.xx
                int nMass = _pointToVertex.Size();
                PoseidonAssert(tagSize == (int)sizeof(float) * nMass);
                PoseidonAssert(massArray.Size() == 0);
                if (nMass > 0)
                {
                    massArray.Realloc(nMass);
                    massArray.Resize(nMass);
                    f.read(massArray.Data(), sizeof(float) * nMass);
                }
            }
        }
        else if (!strcmpi(tagName, "#Animation#"))
        {
            int pSize = _pointToVertex.Size();
            Temp<DataVec> animTemp(pSize);
            AnimationPhase anim;
            float time = 0;
            PoseidonAssert((size_t)tagSize == sizeof(animTemp[0]) * pSize + sizeof(time));
            f.read((char*)&time, sizeof(time));
            f.read((char*)&animTemp[0], sizeof(animTemp[0]) * pSize);
            anim.Resize(NPos());
            for (int a = 0; a < NPos(); a++)
            {
                int vp = _vertexToPoint[a];
                PoseidonAssert(vp >= 0);
                const DataVec& at = animTemp[vp];
                anim[a] = Vector3(at.X(), at.Y(), at.Z());
                if (reversed)
                {
                    anim[a][0] = -anim[a][0], anim[a][2] = -anim[a][2];
                }
            }
            anim.SetTime(time);
            AddPhase(anim);
        }
        else if (!strcmpi(tagName, "#Property#"))
        {
            // name + value are 64-byte NUL-terminated fields. Read into +1 buffers so strlwr
            // / the NamedProperty ctor can't run strlen off the end when all 64 bytes are
            // non-NUL (a stack over-read in the runtime model loader).
            char name[65] = {}, value[65] = {};
            PoseidonAssert(tagSize == 128);
            f.read(name, 64);
            f.read(value, 64);
            strlwr(name);
            strlwr(value);
            _prop.Add(NamedProperty(name, value));
        }
        else if (!strcmpi(tagName, "#MaterialIndex#"))
        {
            OxygenLEOriginTag originTag;
            PoseidonAssert((size_t)tagSize == sizeof(originTag));
            f.read(&originTag, sizeof(originTag));
            // convert to named properties

            _prop.Add(NamedProperty("__ambient", Format("08x", originTag.prot_componumber)));
            _prop.Add(NamedProperty("__diffuse", Format("08x", originTag.prot_magicnumber)));
            _prop.Add(NamedProperty("__specular", Format("08x", originTag.prot_unlockcode)));
            _prop.Add(NamedProperty("__emissive", Format("08x", originTag.prot_requestcode)));
        }
        else if (*tagName != '#') // named selection
        {
            int pSize = (int)sizeof(bool) * _pointToVertex.Size();
            int fSize = (int)sizeof(bool) * NFaces();
            if (tagSize != pSize + fSize)
            {
                _loadWarning = true;
                LOG_DEBUG(Graphics, "Invalid named selection {}", tagName);
                f.seekg(tagSize, QIOS::cur); // skip tag
            }
            else if (*tagName == '-' || *tagName == '.')
            {
                // ignore selection
                f.seekg(tagSize, QIOS::cur); // skip tag
            }
            else
            {
                Temp<byte> tempPoints(pSize);
                Temp<bool> tempFaces(NFaces());
                f.read(tempPoints.Data(), pSize);
                f.read(tempFaces.Data(), fSize);
                AUTO_STATIC_ARRAY(SelInfo, selPoints, 2048);
                AUTO_STATIC_ARRAY(VertexIndex, selFaces, 2048);
                int i;
                for (i = 0; i < NPos(); i++)
                {
                    int vp = _vertexToPoint[i];
                    PoseidonAssert(vp >= 0);
                    byte val = tempPoints[vp];
                    if (val)
                    {
                        selPoints.Add(SelInfo(i, -val));
                    }
                }
#if _DEBUG
                if (!strcmpi(tagName, "zasleh"))
                {
#ifdef _WIN32
                    __nop();
#else
                    __asm__ __volatile__("nop");
#endif
                }
#endif
                for (i = 0; i < NFaces(); i++)
                {
                    if (tempFaces[i])
                    {
                        selFaces.Add(i);
                    }
                }
                NamedSelection sel(tagName, selPoints.Data(), selPoints.Size(), selFaces.Data(), selFaces.Size());
                AddNamedSel(sel);
            }
        }
        else
        {
            f.seekg(tagSize, QIOS::cur); // skip tag
        }
    }

    Compact();

    float resolution = 0;
    f.read((char*)&resolution, sizeof(resolution));
    if (f.fail() || f.eof())
    {
        resolution = 0.0f;
    }

    return resolution;
}

bool LODShape::CheckLegalCreator() const
{
    int componumber = strtoul(PropertyValue("__ambient"), nullptr, 16);
    int magicnumber = strtoul(PropertyValue("__diffuse"), nullptr, 16);
    int unlockcode = strtoul(PropertyValue("__specular"), nullptr, 16);
    int requestcode = strtoul(PropertyValue("__emissive"), nullptr, 16);

    return (componumber != ~0 || magicnumber != ~0 || unlockcode != ~0 || requestcode != ~0);
}

void FaceArray::SetSections(const ShapeSection* sec, int nSec)
{
    _sections.Realloc(nSec);
    _sections.Resize(nSec);
    for (int i = 0; i < nSec; i++)
    {
        _sections[i] = sec[i];
    }
}

void Shape::DefineSections(const ParamEntry& cfg)
{
    const ParamEntry& notes = Pars >> "CfgModels";

    _face._sections.Resize(0);

    // find sections - take care of animated textures

    // scan faces, group them to sections
    // some named selections are marked as section-based
    // no section may split across any such selection

    const ParamEntry* cfgI = &cfg;
    bool split = false;
    for (;;)
    {
        const ParamEntry& entry = (*cfgI) >> "sections";
        // check if sections need to be split
        // mark named selections as section aware
        for (int i = 0; i < entry.GetSize(); i++)
        {
            RStringB selName = entry[i];
            int si = FindNamedSel(selName);
            if (si < 0)
            {
                continue;
            }
            NamedSelection& sel = NamedSel(si);
            sel.SetNeedsSections(true);
            split = true;
        }
        const RStringB& inherit = (*cfgI) >> "sectionsInherit";
        if (inherit.GetLength() <= 0)
        {
            break;
        }
        cfgI = &(notes >> inherit);
    }

    Offset beg = BeginFaces();
    Offset end = EndFaces();
    // when detected change, save old section
    ShapeSection sec; // actual section
    bool secOpen = false;

    for (Offset o = beg; o < end; NextFace(o))
    {
        const Poly& face = Face(o);
        if (!secOpen)
        {
            sec.beg = o;
            sec.end = o;
            sec.properties = face; // get properties from the face
            secOpen = true;
        }
        else
        {
            // check if we match section
            if (sec.properties.GetTexture() == face.GetTexture() && sec.properties.Special() == face.Special())
            {
                //  extend section
                sec.end = o;
            }
            else
            {
                // we need to close section
                NextFace(sec.end);
                if (split)
                {
                    AddSection(sec);
                }
                else
                {
                    _face._sections.Add(sec);
                }
                // and immediately open another one

                sec.beg = o;
                sec.end = o;
                sec.properties = face; // get properties from the face
            }
        }
    }
    if (secOpen)
    {
        NextFace(sec.end);
        if (split)
        {
            AddSection(sec);
        }
        else
        {
            _face._sections.Add(sec);
        }
        secOpen = false;
    }
    _face._sections.Compact();
}

void Shape::UntileTextures(const MTextureMap* mapping, int nMapping)
{
#if 1
    // prepare textures for merging by untiling them

    FaceArray clipped;

    for (Offset f = _face.Begin(); f < _face.End();)
    {
        Poly& face = _face[f];
        if (face.Special() & NoTexMerger)
        {
            _face.Next(f);
            continue;
        }

        Texture* texture = face.GetTexture();

        // check it there is some mapping ready

        // try to untile texture (if possible)
        float uMin = +1e10, vMin = +1e10;
        float uMax = -1e10, vMax = -1e10;
        for (int ii = 0; ii < face.N(); ii++)
        {
            int vi = face.GetVertex(ii);
            float u = 0, v = 0;
            if (texture)
            {
                u = texture->UToLogical(U(vi));
                v = texture->VToLogical(V(vi));
            }
            saturateMin(uMin, u);
            saturateMin(vMin, v);
            saturateMax(uMax, u);
            saturateMax(vMax, v);
        }
        float uBase = toIntFloor(uMin) - 1;
        float vBase = toIntFloor(vMin) - 1;

        // note: smaller epsilon is more accurate, but requires more vertices
        const float eps = texture->IsTransparent() ? 0.10 : 0.35;

        while (uMax > uBase + 1)
        {
            uBase += 1;
        }
        while (uMin < uBase - eps && uMax < uBase + 1 - eps)
        {
            uBase -= 1;
        }
        uMax -= uBase, uMin -= uBase;

        while (vMax > vBase + 1)
        {
            vBase += 1;
        }
        while (vMin < vBase - eps && vMax < vBase + 1 - eps)
        {
            vBase -= 1;
        }
        vMax -= vBase, vMin -= vBase;

        if (uMin >= -eps && vMin >= -eps && uMax <= 1 + eps && vMax <= 1 + eps)
        {
            // not tiled - no processing required
            _face.Next(f);
            continue;
        }

        int mapIndex = -1;
        for (int i = 0; i < nMapping; i++)
        {
            if (mapping[i].texture == texture)
            {
                mapIndex = i;
                break;
            }
        }
        if (mapIndex < 0)
        {
            // no mapping - no need to untile
            _face.Next(f);
            continue;
        }

        // we need to perform some splitting on the face

        // texture is tiled
        LOG_DEBUG(Graphics, "{} removing tiling ({:.3f}): u={:.3f}..{:.3f}, v={:.3f}..{:.3f}", texture->Name(), eps,
                  uMin + uBase, uMax + uBase, vMin + vBase, vMax + vBase);

        const float epsIgnore = 0.05;
        int ubmin = toIntCeil(uMin + uBase + epsIgnore);
        int ubmax = toIntFloor(uMax + uBase - epsIgnore);

        int vbmin = toIntCeil(vMin + vBase + epsIgnore);
        int vbmax = toIntFloor(vMax + vBase - epsIgnore);

        LOG_DEBUG(Graphics, "                 clip:  u={}..{}, v={}..{}", ubmin, ubmax, vbmin, vbmax);
        if (ubmax < ubmin && vbmax < vbmin)
        {
            // no clipping boundary present
            Log("Late uv-clipping skip");
            _face.Next(f);
            continue;
        }

        // split by all required u boundaries
        Poly sourceU = face;
        // remove original face from the shape
        _face.Delete(f);
        // note: variable face is now invalid - it is only reference
        // include splitted faces in the same selections

        Poly lessU = sourceU, greaterU = sourceU;
        lessU.SetN(0);
        greaterU.SetN(0);
        for (int ub = ubmin; ub <= ubmax; ub++)
        {
            //  a*u + bv + c <0 means vertex is out
            //  we want out (greaterU) to contain ub<u
            //  u>ub  <=> u-ub>0 <=> -u + ub <0 => a = -1, b = 0, c = ub
            sourceU.SplitUV(*this, lessU, greaterU, -1, 0, ub);
            sourceU = greaterU;
            // lessU needs to be clipped against all v-boundaries
            Poly sourceV = lessU;
            Poly lessV = sourceV, greaterV = sourceV;
            lessV.SetN(0);
            greaterV.SetN(0);
            for (int vb = vbmin; vb <= vbmax; vb++)
            {
                sourceV.SplitUV(*this, lessV, greaterV, 0, -1, vb);
                sourceV = greaterV;
                // lessV may be safely added to the shape
                if (lessV.N() >= 3)
                {
                    clipped.Add(lessV);
                }
            }
            if (sourceV.N() >= 3)
            {
                clipped.Add(sourceV);
            }
        }
        if (sourceU.N() >= 3)
        {
            // if anything is left in sourceU, clip it agains v-boundaries

            // lessU needs to be clipped against all v-boundaries
            Poly sourceV = sourceU;
            Poly lessV = sourceV, greaterV = sourceV;
            lessV.SetN(0);
            greaterV.SetN(0);
            for (int vb = vbmin; vb <= vbmax; vb++)
            {
                sourceV.SplitUV(*this, lessV, greaterV, 0, -1, vb);
                sourceV = greaterV;
                // lessV may be safely added to the shape
                if (lessV.N() >= 3)
                {
                    clipped.Add(lessV);
                }
            }
            if (sourceV.N() >= 3)
            {
                clipped.Add(sourceV);
            }
        }
    }
    if (clipped.Size() > 0)
    {
        Log("Merge %d to %d", clipped.Size(), _face.Size());
        _face.Merge(clipped);
    }
    // new vertices might be introduced,
    // which do not correspond to any objektiv point
    // we should mark them
    if (NVertex() > _vertexToPoint.Size())
    {
        int oldSize = _vertexToPoint.Size();
        _vertexToPoint.Resize(NVertex());
        for (int i = oldSize; i < NVertex(); i++)
        {
            _vertexToPoint[i] = -1; // mark as non-corresponding to anything
        }
    }

#endif
}

void Shape::MergeTextures(bool untile)
{
#if 1
    if (MergedTextures.Size() <= 0)
    {
        static bool once = true;
        if (once)
        {
            // open from merged file
            QIFStream file;
            file.open("merged\\merged.ptm");
            MergedTextures.Load(file);
            once = false;
        }
        if (MergedTextures.Size() <= 0)
        {
            return;
        }
    }

    RefArray<Texture> needTextures;
    needTextures = _textures;

    AUTO_STATIC_ARRAY(MTextureMap, mapping, 256);

    // check if there are some mergers that could be used
    //
    while (needTextures.Size() > 0)
    {
        int setIndex = MergedTextures.FindBest(needTextures);
        if (setIndex < 0)
        {
            break; // no match - terminate
        }
        const MTextureSet& set = MergedTextures[setIndex];
        for (int i = 0; i < needTextures.Size();)
        {
            Texture* texture = needTextures[i];
            if (!texture)
            {
                i++;
                continue;
            }
            int item = set.Contains(texture->GetName());
            if (item < 0)
            {
                i++; // advance to the next texture
                continue;
            }
            // create mapping
            // replace texture with corresponding part of texture set
            MTextureMap map;
            map.texture = texture;
            map.set = setIndex;
            map.item = item;
            mapping.Add(map);
            needTextures.Delete(i);
        }
    }

    // it we are not able to map any texture, do not attempt untiling
    if (mapping.Size() <= 0)
    {
        return;
    }

    // if we have more than one and are able to map only one, it is not worth doing
    if (mapping.Size() <= 1 && needTextures.Size() > 0)
    {
        return;
    }

    VertexTable old = *this;

    AutoArray<VertexIndex> oldVertexToPoint = _vertexToPoint;
    AutoArray<VertexIndex> oldPointToVertex = _pointToVertex;

    _vertexToPoint.Resize(0);
    for (int i = 0; i < _pointToVertex.Size(); i++)
    {
        _pointToVertex[i] = -1;
    }

    // all vertices must be generated again
    Resize(0);

    // recreate list of textures
    RefArray<Texture> newTextures;
    // scan all faces and try to merge textures
    Texture* lastTexture = (Texture*)-1;
    // when we use non-merged texture
    // we should continue using it as long as possible
    for (Offset f = _face.Begin(); f < _face.End(); _face.Next(f))
    {
        Poly& face = _face[f];
        Texture* texture = face.GetTexture();
        // check it there is some mapping ready
        int mapIndex = -1;
        if (texture != lastTexture && (face.Special() & NoTexMerger) == 0)
        {
            for (int i = 0; i < mapping.Size(); i++)
            {
                if (mapping[i].texture == texture)
                {
                    mapIndex = i;
                    break;
                }
            }
        }
        if (mapIndex < 0)
        {
            if (texture)
            {
                newTextures.AddUnique(texture);
            }
            // copy all required vertices
            for (int ii = 0; ii < face.N(); ii++)
            {
                int vi = face.GetVertex(ii);
                Vector3Val pos = old.Pos(vi);
                Vector3Val norm = old.Norm(vi);
                ClipFlags clip = old.Clip(vi);
                float u = old.U(vi);
                float v = old.V(vi);
                int pindex = oldVertexToPoint[vi];
                int vindex = AddVertex(pos, norm, clip, u, v);
                face.Set(ii, vindex);
                _vertexToPoint.Access(vindex);
                _vertexToPoint[vindex] = pindex;
                if (pindex >= 0)
                {
                    _pointToVertex[pindex] = vindex;
                }
            }
            lastTexture = texture;
            continue;
        }
        lastTexture = (Texture*)-1; // no last texture

        // remap all required vertices
        // try to untile texture (if possible)
        float uMin = +1e10, vMin = +1e10;
        float uMax = -1e10, vMax = -1e10;
        for (int ii = 0; ii < face.N(); ii++)
        {
            int vi = face.GetVertex(ii);
            float u = 0, v = 0;
            if (texture)
            {
                u = texture->UToLogical(old.U(vi));
                v = texture->VToLogical(old.V(vi));
            }
            saturateMin(uMin, u);
            saturateMin(vMin, v);
            saturateMax(uMax, u);
            saturateMax(vMax, v);
        }
        float uBase = toIntFloor(uMin) - 1;
        float vBase = toIntFloor(vMin) - 1;
        //  try best fit in both directions

        const float eps = texture->IsTransparent() ? 0.10 : 0.35;

        while (uMax > uBase + 1)
        {
            uBase += 1;
        }
        while (uMin < uBase - eps && uMax < uBase + 1 - eps)
        {
            uBase -= 1;
        }
        uMax -= uBase, uMin -= uBase;

        while (vMax > vBase + 1)
        {
            vBase += 1;
        }
        while (vMin < vBase - eps && vMax < vBase + 1 - eps)
        {
            vBase -= 1;
        }
        vMax -= vBase, vMin -= vBase;

        if (uMin < -eps || vMin < -eps || uMax > 1 + eps || vMax > 1 + eps)
        {
            // we need to perform some splitting on the faces

            // texture is tiled
            LOG_DEBUG(Graphics, "{} tiling prevents merging ({:.3f}): u={:.3f}..{:.3f}, v={:.3f}..{:.3f}",
                      texture->Name(), eps, uMin, uMax, vMin, vMax);
            if (texture)
            {
                newTextures.AddUnique(texture);
            }
            // copy all required vertices
            for (int ii = 0; ii < face.N(); ii++)
            {
                int vi = face.GetVertex(ii);
                Vector3Val pos = old.Pos(vi);
                Vector3Val norm = old.Norm(vi);
                ClipFlags clip = old.Clip(vi);
                float u = old.U(vi);
                float v = old.V(vi);
                int pindex = oldVertexToPoint[vi];
                int vindex = AddVertex(pos, norm, clip, u, v);
                face.Set(ii, vindex);
                _vertexToPoint.Access(vindex);
                _vertexToPoint[vindex] = pindex;
                if (pindex >= 0)
                {
                    _pointToVertex[pindex] = vindex;
                }
            }
            lastTexture = texture;
            continue;
        }

        // there is some mapping - replace texture
        const MTextureMap& map = mapping[mapIndex];
        const MTextureSet& set = MergedTextures[map.set];
        const MTextureItem& item = set[map.item];

        // note: we must update mapping
        Ref<Texture> mergedTexture = set.GetTex();
        PoseidonAssert(mergedTexture);
        newTextures.AddUnique(mergedTexture);
        face.SetTexture(mergedTexture);
        face.AndSpecial(~NoClamp);
        face.OrSpecial(ClampU | ClampV);
        int at = 0;

        if (mergedTexture->IsTransparent())
        {
            at |= IsTransparent;
        }
        if (mergedTexture->IsAlpha())
        {
            at |= IsAlpha;
        }
        face.AndSpecial(~(IsTransparent | IsAlpha));
        face.OrSpecial(at);
        for (int ii = 0; ii < face.N(); ii++)
        {
            int vi = face.GetVertex(ii);
            Vector3Val pos = old.Pos(vi);
            Vector3Val norm = old.Norm(vi);
            ClipFlags clip = old.Clip(vi);
            float u = old.U(vi);
            float v = old.V(vi);
            if (texture)
            {
                u = texture->UToLogical(u) - uBase;
                v = texture->VToLogical(v) - vBase;
            }

            // u,v are texture coordinates
            // convert them to source area coordinates
            int angle = item.angle & 3;
            // consider angle
            float wr = item.w;
            float hr = item.h;
            while (--angle >= 0)
            {
                // rotate matrix
                // multiply matrix with ( 0,-1)
                // multiply matrix with (+1, 0)
                //  			    0    +1
                // 	  		   -1     0
                // uu uv    uv    -uu
                // vu vv    vv    -vu
                float un = +v;
                float vn = 1 - u;
                u = un;
                v = vn;
                float wrn = hr;
                float hrn = wr;
                wr = wrn;
                hr = hrn;
            }

            int scale1024 = item.scale >= 0 ? 1024 << item.scale : 1024 >> -item.scale;
            float scale = scale1024 * (1.0 / 1024);

            float uit = (u * wr * scale + item.x) / set.w;
            float vit = (v * hr * scale + item.y) / set.h;

            // check if result is in supposed range
            // note: it may be necessary to split some faces
            // in order to avoid tiling

            uit = mergedTexture->UToPhysical(uit);
            vit = mergedTexture->VToPhysical(vit);

            int pindex = oldVertexToPoint[vi];
            int vindex = AddVertex(pos, norm, clip, uit, vit);
            face.Set(ii, vindex);
            _vertexToPoint.Access(vindex);
            _vertexToPoint[vindex] = pindex;
            if (pindex >= 0)
            {
                _pointToVertex[pindex] = vindex;
            }
        }
        // note: only used vertices are inserted to VertexTable
        // so there are no unused vertices now
    }
    _vertexToPoint.Compact();
    _textures = newTextures;
    // note: when using late texture merging, we need to redefine selections now
    RecalculateAreas();
    Compact();
#endif
}

} // namespace Poseidon
