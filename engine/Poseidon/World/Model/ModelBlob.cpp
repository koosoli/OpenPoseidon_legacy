#include <Poseidon/World/Model/ModelBlob.hpp>

#include <Poseidon/World/Model/Model.hpp>

#include <cstring>
#include <limits>

namespace Poseidon
{
namespace ModelBlob
{
namespace
{

using namespace Poseidon::Model;

// ---------------------------------------------------------------------------
// Writer. Little-endian, fixed widths, every field named.
// ---------------------------------------------------------------------------
struct Writer
{
    std::vector<uint8_t>& out;

    void Raw(const void* p, size_t n)
    {
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
    void U8(uint8_t v) { out.push_back(v); }
    void U32(uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
    void I8(int8_t v) { U8(static_cast<uint8_t>(v)); }
    void Bool(bool v) { U8(v ? 1 : 0); }
    // IEEE-754 bit pattern, not a raw float write: the point is that the byte
    // order is decided here rather than inherited.
    void F32(float v)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, 4);
        U32(bits);
    }
    void Str(const std::string& s)
    {
        U32(static_cast<uint32_t>(s.size()));
        if (!s.empty())
            Raw(s.data(), s.size());
    }
    template <typename E>
    void Enum8(E v)
    {
        U8(static_cast<uint8_t>(v));
    }
    template <typename E>
    void Enum32(E v)
    {
        U32(static_cast<uint32_t>(v));
    }
    void Vec3(const Vector3& v)
    {
        F32(v.x);
        F32(v.y);
        F32(v.z);
    }
    void Vec2(const Vector2& v)
    {
        F32(v.u);
        F32(v.v);
    }
    void Box(const BoundingBox& b)
    {
        Vec3(b.min);
        Vec3(b.max);
    }
    void Sphere(const BoundingSphere& s)
    {
        Vec3(s.center);
        F32(s.radius);
    }
    void Mat(const Matrix4x3& m)
    {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                F32(m.m[r][c]);
    }
    void U32Vec(const std::vector<uint32_t>& v)
    {
        U32(static_cast<uint32_t>(v.size()));
        for (uint32_t x : v)
            U32(x);
    }
    void U16Vec(const std::vector<uint16_t>& v)
    {
        U32(static_cast<uint32_t>(v.size()));
        for (uint16_t x : v)
        {
            out.push_back(static_cast<uint8_t>(x));
            out.push_back(static_cast<uint8_t>(x >> 8));
        }
    }
    void U8Vec(const std::vector<uint8_t>& v)
    {
        U32(static_cast<uint32_t>(v.size()));
        if (!v.empty())
            Raw(v.data(), v.size());
    }
    void F32Vec(const std::vector<float>& v)
    {
        U32(static_cast<uint32_t>(v.size()));
        for (float x : v)
            F32(x);
    }
};

// ---------------------------------------------------------------------------
// Reader. Bounds-checked at every step; `ok` latches false and every accessor
// short-circuits, so one bad count cannot be followed by a wild read.
// ---------------------------------------------------------------------------
struct Reader
{
    const uint8_t* p   = nullptr;
    size_t         n   = 0;
    size_t         at  = 0;
    bool           ok  = true;

    size_t Remaining() const { return n - at; }

    bool Need(size_t bytes)
    {
        if (!ok || Remaining() < bytes)
        {
            ok = false;
            return false;
        }
        return true;
    }
    uint8_t U8()
    {
        if (!Need(1))
            return 0;
        return p[at++];
    }
    uint32_t U32()
    {
        if (!Need(4))
            return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(p[at + i]) << (8 * i);
        at += 4;
        return v;
    }
    int32_t I32() { return static_cast<int32_t>(U32()); }
    int8_t  I8() { return static_cast<int8_t>(U8()); }
    bool    Bool() { return U8() != 0; }
    float   F32()
    {
        const uint32_t bits = U32();
        float          v    = 0;
        std::memcpy(&v, &bits, 4);
        return v;
    }
    // A count is only believable if the SMALLEST possible element still fits in
    // what is left. This is what stops a damaged length from sizing a multi-
    // gigabyte allocation before the read fails.
    bool Count(uint32_t& outCount, size_t minElementBytes)
    {
        outCount = U32();
        if (!ok)
            return false;
        if (minElementBytes > 0 && static_cast<uint64_t>(outCount) * minElementBytes > Remaining())
        {
            ok = false;
            return false;
        }
        return true;
    }
    std::string Str()
    {
        uint32_t len = 0;
        if (!Count(len, 1))
            return std::string();
        std::string s(reinterpret_cast<const char*>(p + at), len);
        at += len;
        return s;
    }
    template <typename E>
    E Enum8()
    {
        return static_cast<E>(U8());
    }
    template <typename E>
    E Enum32()
    {
        return static_cast<E>(U32());
    }
    Vector3 Vec3()
    {
        Vector3 v;
        v.x = F32();
        v.y = F32();
        v.z = F32();
        return v;
    }
    Vector2 Vec2()
    {
        Vector2 v;
        v.u = F32();
        v.v = F32();
        return v;
    }
    BoundingBox Box()
    {
        BoundingBox b;
        b.min = Vec3();
        b.max = Vec3();
        return b;
    }
    BoundingSphere Sphere()
    {
        BoundingSphere s;
        s.center = Vec3();
        s.radius = F32();
        return s;
    }
    Matrix4x3 Mat()
    {
        Matrix4x3 m;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                m.m[r][c] = F32();
        return m;
    }
    void U32Vec(std::vector<uint32_t>& v)
    {
        uint32_t c = 0;
        if (!Count(c, 4))
            return;
        v.resize(c);
        for (uint32_t i = 0; i < c; ++i)
            v[i] = U32();
    }
    void U16Vec(std::vector<uint16_t>& v)
    {
        uint32_t c = 0;
        if (!Count(c, 2))
            return;
        v.resize(c);
        for (uint32_t i = 0; i < c; ++i)
        {
            if (!Need(2))
                return;
            v[i] = static_cast<uint16_t>(p[at] | (static_cast<uint16_t>(p[at + 1]) << 8));
            at += 2;
        }
    }
    void U8Vec(std::vector<uint8_t>& v)
    {
        uint32_t c = 0;
        if (!Count(c, 1))
            return;
        v.assign(p + at, p + at + c);
        at += c;
    }
    void F32Vec(std::vector<float>& v)
    {
        uint32_t c = 0;
        if (!Count(c, 4))
            return;
        v.resize(c);
        for (uint32_t i = 0; i < c; ++i)
            v[i] = F32();
    }
};

// ---- element writers -------------------------------------------------------

void WriteVertex(Writer& w, const Vertex& v)
{
    w.Vec3(v.position);
    w.Vec3(v.normal);
    w.Vec3(v.tangent);
    w.Vec3(v.binormal);
    w.Bool(v.hasTangentFrame);
    w.Vec2(v.uv);
    w.Vec2(v.uv1);
    w.Enum32(v.flags);
}
void ReadVertex(Reader& r, Vertex& v)
{
    v.position        = r.Vec3();
    v.normal          = r.Vec3();
    v.tangent         = r.Vec3();
    v.binormal        = r.Vec3();
    v.hasTangentFrame = r.Bool();
    v.uv              = r.Vec2();
    v.uv1             = r.Vec2();
    v.flags           = r.Enum32<VertexFlags>();
}

void WriteTriangle(Writer& w, const Triangle& t)
{
    for (uint32_t i : t.indices)
        w.U32(i);
    w.U32(t.materialIndex);
    w.Enum32(t.flags);
    w.U32(t.originalIndex);
}
void ReadTriangle(Reader& r, Triangle& t)
{
    for (uint32_t& i : t.indices)
        i = r.U32();
    t.materialIndex = r.U32();
    t.flags         = r.Enum32<FaceFlags>();
    t.originalIndex = r.U32();
}

void WriteQuad(Writer& w, const Quad& q)
{
    for (uint32_t i : q.indices)
        w.U32(i);
    w.U32(q.materialIndex);
    w.Enum32(q.flags);
    w.U32(q.originalIndex);
}
void ReadQuad(Reader& r, Quad& q)
{
    for (uint32_t& i : q.indices)
        i = r.U32();
    q.materialIndex = r.U32();
    q.flags         = r.Enum32<FaceFlags>();
    q.originalIndex = r.U32();
}

void WriteMaterial(Writer& w, const Material& m)
{
    w.Str(m.name);
    w.Str(m.texturePath);
    w.Str(m.materialPath);
    w.U32(static_cast<uint32_t>(m.embeddedStages.size()));
    for (const MaterialStage& s : m.embeddedStages)
    {
        w.U32(s.sourceStage);
        w.Str(s.texturePath);
        w.U32(s.uvSource);
    }
    w.F32(m.metallic);
    w.F32(m.roughness);
    w.F32(m.emissive);
    w.Enum32(m.flags);
}
void ReadMaterial(Reader& r, Material& m)
{
    m.name         = r.Str();
    m.texturePath  = r.Str();
    m.materialPath = r.Str();
    uint32_t n     = 0;
    if (r.Count(n, 12)) // sourceStage + len + uvSource is the smallest stage
    {
        m.embeddedStages.resize(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
        {
            m.embeddedStages[i].sourceStage = r.U32();
            m.embeddedStages[i].texturePath = r.Str();
            m.embeddedStages[i].uvSource    = r.U32();
        }
    }
    m.metallic  = r.F32();
    m.roughness = r.F32();
    m.emissive  = r.F32();
    m.flags     = r.Enum32<FaceFlags>();
}

void WriteSelection(Writer& w, const NamedSelection& s)
{
    w.Str(s.name);
    w.U32Vec(s.vertexIndices);
    w.U8Vec(s.vertexWeights);
    w.U8Vec(s.sourceVertexWeights);
    w.U32Vec(s.triangleIndices);
    w.U32Vec(s.faceSelectionOffsets);
    w.U32Vec(s.sectionIndices);
    w.Bool(s.needsSections);
}
void ReadSelection(Reader& r, NamedSelection& s)
{
    s.name = r.Str();
    r.U32Vec(s.vertexIndices);
    r.U8Vec(s.vertexWeights);
    r.U8Vec(s.sourceVertexWeights);
    r.U32Vec(s.triangleIndices);
    r.U32Vec(s.faceSelectionOffsets);
    r.U32Vec(s.sectionIndices);
    s.needsSections = r.Bool();
}

void WriteProxy(Writer& w, const Proxy& p)
{
    w.Str(p.name);
    w.Mat(p.transform);
    w.U32(p.selectionIndex);
    w.I32(p.id);
}
void ReadProxy(Reader& r, Proxy& p)
{
    p.name           = r.Str();
    p.transform      = r.Mat();
    p.selectionIndex = r.U32();
    p.id             = r.I32();
}

void WriteSection(Writer& w, const Section& s)
{
    w.U32(s.materialIndex);
    w.U32(s.startTriangle);
    w.U32(s.triangleCount);
    w.Enum32(s.hints);
    w.I32(s.specialMaterial);
    w.U32(s.firstFace);
    w.U32(s.faceCount);
    w.Bool(s.faceRangeKnown);
}
void ReadSection(Reader& r, Section& s)
{
    s.materialIndex   = r.U32();
    s.startTriangle   = r.U32();
    s.triangleCount   = r.U32();
    s.hints           = r.Enum32<RenderHints>();
    s.specialMaterial = r.I32();
    s.firstFace       = r.U32();
    s.faceCount       = r.U32();
    s.faceRangeKnown  = r.Bool();
}

void WriteMesh(Writer& w, const Mesh& m)
{
    w.U32(static_cast<uint32_t>(m.vertices.size()));
    for (const Vertex& v : m.vertices)
        WriteVertex(w, v);
    w.U32(static_cast<uint32_t>(m.triangles.size()));
    for (const Triangle& t : m.triangles)
        WriteTriangle(w, t);
    w.U32(static_cast<uint32_t>(m.quads.size()));
    for (const Quad& q : m.quads)
        WriteQuad(w, q);
    w.U32(static_cast<uint32_t>(m.materials.size()));
    for (const Material& mat : m.materials)
        WriteMaterial(w, mat);
    w.U32(static_cast<uint32_t>(m.selections.size()));
    for (const NamedSelection& s : m.selections)
        WriteSelection(w, s);
    w.U32(static_cast<uint32_t>(m.properties.size()));
    for (const NamedProperty& p : m.properties)
    {
        w.Str(p.name);
        w.Str(p.value);
    }
    w.U32(static_cast<uint32_t>(m.proxies.size()));
    for (const Proxy& p : m.proxies)
        WriteProxy(w, p);
    w.U32(static_cast<uint32_t>(m.sections.size()));
    for (const Section& s : m.sections)
        WriteSection(w, s);
    w.U16Vec(m.edges.mlodIndices);
    w.U16Vec(m.edges.vertexIndices);
    w.U32(static_cast<uint32_t>(m.frames.size()));
    for (const AnimationFrame& f : m.frames)
    {
        w.F32(f.time);
        w.U32(static_cast<uint32_t>(f.positions.size()));
        for (const Vector3& v : f.positions)
            w.Vec3(v);
    }
    w.F32Vec(m.vertexMass);
    w.Box(m.boundingBox);
    w.Sphere(m.boundingSphere);
    w.Vec3(m.bCenter);
    w.F32(m.bRadius);
    w.Enum32(m.orHints);
    w.Enum32(m.andHints);
    w.Enum32(m.special);
    w.U32(m.color);
    w.U32(m.colorTop);
    w.U32(m.iconColor);
    w.U32(m.selectedColor);
}

void ReadMesh(Reader& r, Mesh& m)
{
    uint32_t n = 0;
    if (r.Count(n, 60)) // a Vertex is 61 bytes; 60 is a safe lower bound
    {
        m.vertices.resize(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
            ReadVertex(r, m.vertices[i]);
    }
    if (r.Count(n, 24))
    {
        m.triangles.resize(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
            ReadTriangle(r, m.triangles[i]);
    }
    if (r.Count(n, 28))
    {
        m.quads.resize(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
            ReadQuad(r, m.quads[i]);
    }
    if (r.Count(n, 28))
    {
        m.materials.resize(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
            ReadMaterial(r, m.materials[i]);
    }
    if (r.Count(n, 29))
    {
        m.selections.resize(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
            ReadSelection(r, m.selections[i]);
    }
    if (r.Count(n, 8))
    {
        m.properties.resize(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
        {
            m.properties[i].name  = r.Str();
            m.properties[i].value = r.Str();
        }
    }
    if (r.Count(n, 60))
    {
        m.proxies.resize(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
            ReadProxy(r, m.proxies[i]);
    }
    if (r.Count(n, 33))
    {
        m.sections.resize(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
            ReadSection(r, m.sections[i]);
    }
    r.U16Vec(m.edges.mlodIndices);
    r.U16Vec(m.edges.vertexIndices);
    if (r.Count(n, 8))
    {
        m.frames.resize(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
        {
            m.frames[i].time = r.F32();
            uint32_t pc      = 0;
            if (!r.Count(pc, 12))
                break;
            m.frames[i].positions.resize(pc);
            for (uint32_t j = 0; j < pc && r.ok; ++j)
                m.frames[i].positions[j] = r.Vec3();
        }
    }
    r.F32Vec(m.vertexMass);
    m.boundingBox    = r.Box();
    m.boundingSphere = r.Sphere();
    m.bCenter        = r.Vec3();
    m.bRadius        = r.F32();
    m.orHints        = r.Enum32<RenderHints>();
    m.andHints       = r.Enum32<RenderHints>();
    m.special        = r.Enum32<SpecialFlags>();
    m.color          = r.U32();
    m.colorTop       = r.U32();
    m.iconColor      = r.U32();
    m.selectedColor  = r.U32();
}

void WriteBasis(Writer& w, const GeometryBasis& b)
{
    w.Enum8(b.winding);
    w.Enum8(b.normals);
    w.Enum8(b.tangents);
    w.Enum8(b.origin);
    w.Enum8(b.windingConfidence);
    w.Enum8(b.normalsConfidence);
    w.Enum8(b.tangentsConfidence);
    w.Enum8(b.originConfidence);
    w.Bool(b.centreOfMassAtOrigin);
    w.Enum8(b.centreOfMassConfidence);
}
void ReadBasis(Reader& r, GeometryBasis& b)
{
    b.winding                = r.Enum8<SourceWinding>();
    b.normals                = r.Enum8<NormalOrientation>();
    b.tangents               = r.Enum8<TangentHandedness>();
    b.origin                 = r.Enum8<SourceOrigin>();
    b.windingConfidence      = r.Enum8<BasisConfidence>();
    b.normalsConfidence      = r.Enum8<BasisConfidence>();
    b.tangentsConfidence     = r.Enum8<BasisConfidence>();
    b.originConfidence       = r.Enum8<BasisConfidence>();
    b.centreOfMassAtOrigin   = r.Bool();
    b.centreOfMassConfidence = r.Enum8<BasisConfidence>();
}

void WriteLod(Writer& w, const LODLevel& l)
{
    w.F32(l.resolution);
    w.Enum8(l.purpose);
    WriteMesh(w, l.mesh);
    w.U32(static_cast<uint32_t>(l.uvChannels.size()));
    for (const UVChannel& c : l.uvChannels)
    {
        w.I32(c.id);
        w.U32(static_cast<uint32_t>(c.faceVertexUVs.size()));
        for (const Vector2& uv : c.faceVertexUVs)
            w.Vec2(uv);
    }
    w.Str(l.sourceEncoding);
    WriteBasis(w, l.basis);
    // sourceWinding is derived from the basis by DescribeWinding, but it is
    // written out rather than recomputed: a loader that set it to something else
    // must round-trip as that something else, not as what the derivation would
    // have produced. Recomputing would make the cache quietly REPAIR the IR,
    // which is a difference between a cold load and a warm one.
    w.Str(l.sourceWinding);
    w.I32(l.uvSetCount);
}
void ReadLod(Reader& r, LODLevel& l)
{
    l.resolution = r.F32();
    l.purpose    = r.Enum8<LodPurpose>();
    ReadMesh(r, l.mesh);
    uint32_t n = 0;
    if (r.Count(n, 8))
    {
        l.uvChannels.resize(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
        {
            l.uvChannels[i].id = r.I32();
            uint32_t c         = 0;
            if (!r.Count(c, 8))
                break;
            l.uvChannels[i].faceVertexUVs.resize(c);
            for (uint32_t j = 0; j < c && r.ok; ++j)
                l.uvChannels[i].faceVertexUVs[j] = r.Vec2();
        }
    }
    l.sourceEncoding = r.Str();
    ReadBasis(r, l.basis);
    l.sourceWinding = r.Str();
    l.uvSetCount    = r.I32();
}

} // namespace

void Serialize(const Poseidon::Model::Model& m, std::vector<uint8_t>& out)
{
    Writer w{out};
    w.Raw(kMagic, 4);
    w.U32(kFormatVersion);

    w.U32(static_cast<uint32_t>(m.lodLevels.size()));
    for (const LODLevel& l : m.lodLevels)
        WriteLod(w, l);

    // std::map iterates in key order, so the metadata block is already
    // deterministic without a sort.
    w.U32(static_cast<uint32_t>(m.metadata.size()));
    for (const auto& kv : m.metadata)
    {
        w.Str(kv.first);
        w.Str(kv.second);
    }

    w.Sphere(m.boundingSphere);
    w.Box(m.boundingBox);
    w.Str(m.sourcePath);
    w.Str(m.sourceFormat);
    w.U32(m.sourceVersion);

    w.F32(m.mass);
    w.F32(m.invMass);
    w.F32(m.armor);
    w.F32(m.invArmor);
    w.Vec3(m.centerOfMass);
    for (float v : m.invInertia)
        w.F32(v);
    w.F32Vec(m.massArray);

    w.Vec3(m.aimingCenter);
    w.Vec3(m.autoCenter);
    w.Vec3(m.boundingCenter);
    w.Vec3(m.geometryCenter);
    w.Sphere(m.geometrySphere);

    w.Enum32(m.andHints);
    w.Enum32(m.orHints);
    w.U32(m.canOcclude);
    w.U32(m.canBeOccluded);
    w.U32(m.allowAnimation);
    w.U32(m.lockAutoCenter);
    w.U32(m.autoCenterEnabled);
    w.U32(m.mapType);

    w.I8(m.memoryIdx);
    w.I8(m.geometryIdx);
    w.I8(m.geometryFireIdx);
    w.I8(m.geometryViewIdx);
    w.I8(m.geometryViewPilotIdx);
    w.I8(m.geometryViewGunnerIdx);
    w.I8(m.geometryViewCommanderIdx);
    w.I8(m.geometryViewCargoIdx);
    w.I8(m.landContactIdx);
    w.I8(m.roadwayIdx);
    w.I8(m.pathsIdx);
    w.I8(m.hitpointsIdx);

    w.U32(m.color);
    w.U32(m.colorTop);
    w.F32(m.viewDensity);
    w.F32(m.special);

    w.Str(m.memory);
    w.Str(m.geometry);
    w.Str(m.geometryFire);
    w.Str(m.geometryView);
    w.Str(m.geometryViewPilot);
    w.Str(m.geometryViewGunner);
    w.Str(m.geometryViewCommander);
    w.Str(m.geometryViewCargo);
    w.Str(m.landContact);
    w.Str(m.roadway);
    w.Str(m.paths);
    w.Str(m.hitpoints);
    w.Str(m.remarks);

    w.I32(m.memoryLODIndex);
    w.I32(m.geometryLODIndex);
    w.I32(m.fireGeometryLODIndex);
    w.I32(m.viewGeometryLODIndex);
    w.I32(m.viewPilotLODIndex);
    w.I32(m.viewGunnerLODIndex);
    w.I32(m.viewCommanderLODIndex);
    w.I32(m.viewCargoLODIndex);
    w.I32(m.landContactLODIndex);
    w.I32(m.roadwayLODIndex);
    w.I32(m.pathsLODIndex);
    w.I32(m.hitpointsLODIndex);
    w.I32(m.remarksFlags);

    const auto& audit = m.sourceAudit;
    w.U32(kSourceAuditTag);
    w.U32(kSourceAuditVersion);
    w.U32(kSourceAuditPayloadBytes);
    w.U32(audit.producerVersion);
    w.U32(audit.sourceRevision);
    w.U32(uint32_t(audit.geometryCoverage));
    w.U32(audit.observations);
    w.U32(audit.declaredLods);
    w.U32(audit.decodedLods);
    w.U32(audit.skeletonBones);
    w.U32(audit.directoryAnimationClasses);
    w.U32(audit.keyframeCount);
    w.U32(audit.vertexBoneReferenceCount);
    w.U32(audit.neighbourBoneReferenceCount);
    w.U32((audit.directoryHasAnimations ? 1u : 0u) | (audit.skeletonDeclared ? 2u : 0u) |
          (audit.keyframePayloadDiscarded ? 4u : 0u));
}

std::vector<uint8_t> Serialize(const Poseidon::Model::Model& m)
{
    std::vector<uint8_t> out;
    Serialize(m, out);
    return out;
}

bool Deserialize(const void* data, size_t size, Poseidon::Model::Model& m)
{
    if (!data || size < 8)
        return false;
    Reader r;
    r.p = static_cast<const uint8_t*>(data);
    r.n = size;

    if (std::memcmp(r.p, kMagic, 4) != 0)
        return false;
    r.at = 4;
    if (r.U32() != kFormatVersion)
        return false;

    m = Poseidon::Model::Model{};

    uint32_t n = 0;
    if (r.Count(n, 5)) // resolution + purpose is the smallest a LOD can be
    {
        m.lodLevels.resize(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
            ReadLod(r, m.lodLevels[i]);
    }
    if (r.Count(n, 8))
    {
        for (uint32_t i = 0; i < n && r.ok; ++i)
        {
            std::string k = r.Str();
            std::string v = r.Str();
            if (r.ok)
                m.metadata.emplace(std::move(k), std::move(v));
        }
    }

    m.boundingSphere = r.Sphere();
    m.boundingBox    = r.Box();
    m.sourcePath     = r.Str();
    m.sourceFormat   = r.Str();
    m.sourceVersion  = r.U32();

    m.mass         = r.F32();
    m.invMass      = r.F32();
    m.armor        = r.F32();
    m.invArmor     = r.F32();
    m.centerOfMass = r.Vec3();
    for (float& v : m.invInertia)
        v = r.F32();
    r.F32Vec(m.massArray);

    m.aimingCenter   = r.Vec3();
    m.autoCenter     = r.Vec3();
    m.boundingCenter = r.Vec3();
    m.geometryCenter = r.Vec3();
    m.geometrySphere = r.Sphere();

    m.andHints          = r.Enum32<RenderHints>();
    m.orHints           = r.Enum32<RenderHints>();
    m.canOcclude        = r.U32();
    m.canBeOccluded     = r.U32();
    m.allowAnimation    = r.U32();
    m.lockAutoCenter    = r.U32();
    m.autoCenterEnabled = r.U32();
    m.mapType           = r.U32();

    m.memoryIdx                 = r.I8();
    m.geometryIdx               = r.I8();
    m.geometryFireIdx           = r.I8();
    m.geometryViewIdx           = r.I8();
    m.geometryViewPilotIdx      = r.I8();
    m.geometryViewGunnerIdx     = r.I8();
    m.geometryViewCommanderIdx  = r.I8();
    m.geometryViewCargoIdx      = r.I8();
    m.landContactIdx            = r.I8();
    m.roadwayIdx                = r.I8();
    m.pathsIdx                  = r.I8();
    m.hitpointsIdx              = r.I8();

    m.color       = r.U32();
    m.colorTop    = r.U32();
    m.viewDensity = r.F32();
    m.special     = r.F32();

    m.memory                = r.Str();
    m.geometry              = r.Str();
    m.geometryFire          = r.Str();
    m.geometryView          = r.Str();
    m.geometryViewPilot     = r.Str();
    m.geometryViewGunner    = r.Str();
    m.geometryViewCommander = r.Str();
    m.geometryViewCargo     = r.Str();
    m.landContact           = r.Str();
    m.roadway               = r.Str();
    m.paths                 = r.Str();
    m.hitpoints             = r.Str();
    m.remarks               = r.Str();

    m.memoryLODIndex        = r.I32();
    m.geometryLODIndex      = r.I32();
    m.fireGeometryLODIndex  = r.I32();
    m.viewGeometryLODIndex  = r.I32();
    m.viewPilotLODIndex     = r.I32();
    m.viewGunnerLODIndex    = r.I32();
    m.viewCommanderLODIndex = r.I32();
    m.viewCargoLODIndex     = r.I32();
    m.landContactLODIndex   = r.I32();
    m.roadwayLODIndex       = r.I32();
    m.pathsLODIndex         = r.I32();
    m.hitpointsLODIndex     = r.I32();
    m.remarksFlags          = r.I32();

    if (!r.ok) return false;
    if (r.at == r.n) return true; // Old base v1: sourceAudit stays wholly Unknown.
    if (r.Remaining() != kSourceAuditFooterBytes || r.U32() != kSourceAuditTag ||
        r.U32() != kSourceAuditVersion || r.U32() != kSourceAuditPayloadBytes) return false;
    ModelSourceAudit audit;
    audit.producerVersion = r.U32();
    audit.sourceRevision = r.U32();
    const auto coverage = r.U32();
    audit.observations = r.U32();
    audit.declaredLods = r.U32();
    audit.decodedLods = r.U32();
    audit.skeletonBones = r.U32();
    audit.directoryAnimationClasses = r.U32();
    audit.keyframeCount = r.U32();
    audit.vertexBoneReferenceCount = r.U32();
    audit.neighbourBoneReferenceCount = r.U32();
    const auto flags = r.U32();
    if (!r.ok || coverage > uint32_t(SourceGeometryCoverage::DeclaredLodsDecoded) ||
        audit.producerVersion > 1 || (audit.observations & ~SourceAuditAllObservations) || (flags & ~7u)) return false;
    audit.geometryCoverage = SourceGeometryCoverage(coverage);
    audit.directoryHasAnimations = (flags & 1u) != 0;
    audit.skeletonDeclared = (flags & 2u) != 0;
    audit.keyframePayloadDiscarded = (flags & 4u) != 0;
    if (!audit.producerVersion)
    {
        if (coverage || audit.sourceRevision || audit.observations || audit.declaredLods || audit.decodedLods ||
            audit.skeletonBones || audit.directoryAnimationClasses || audit.keyframeCount ||
            audit.vertexBoneReferenceCount || audit.neighbourBoneReferenceCount || flags) return false;
    }
    else
    {
        const auto revision = audit.sourceRevision;
        if (m.sourceFormat != "ODOL" || revision != m.sourceVersion || (revision != 7 && revision != 40 && revision != 48 && revision != 49 &&
            revision != 50 && revision != 52 && revision != 54 && revision != 73)) return false;
        if (audit.declaredLods > 100 || audit.decodedLods > audit.declaredLods ||
            audit.decodedLods != m.lodLevels.size()) return false;
        if (coverage && (!audit.declaredLods || audit.decodedLods != audit.declaredLods)) return false;
        if ((!audit.Observed(SourceAuditObservation::Skeleton) && (audit.skeletonBones || audit.skeletonDeclared)) ||
            (!audit.Observed(SourceAuditObservation::DirectoryAnimation) && (audit.directoryAnimationClasses || audit.directoryHasAnimations)) ||
            (!audit.Observed(SourceAuditObservation::BoneReferences) && (audit.vertexBoneReferenceCount || audit.neighbourBoneReferenceCount)) ||
            (!audit.Observed(SourceAuditObservation::Keyframes) && (audit.keyframeCount || audit.keyframePayloadDiscarded)) ||
            (audit.keyframePayloadDiscarded && !audit.keyframeCount) ||
            (audit.directoryAnimationClasses && !audit.directoryHasAnimations) ||
            (audit.skeletonBones && !audit.skeletonDeclared)) return false;
    }
    m.sourceAudit = audit;

    // Trailing bytes are as much a sign of a wrong reading as missing ones, so
    // an entry that decodes but does not END where the writer stopped is a miss.
    return r.ok && r.at == r.n;
}

} // namespace ModelBlob
} // namespace Poseidon
