#pragma once

#include <Poseidon/World/Model/GeometryBasis.hpp>
#include <Poseidon/World/Model/LodPurpose.hpp>
#include <Poseidon/World/Model/ModelFlags.hpp>
#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <utility>

namespace Poseidon {
namespace Model {

struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vector3() = default;
    Vector3(float x, float y, float z) : x(x), y(y), z(z) {}
};

struct Vector2 {
    float u = 0.0f;
    float v = 0.0f;

    Vector2() = default;
    Vector2(float u, float v) : u(u), v(v) {}
};

struct Matrix4x3 {
    float m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};  // Identity + zero translation

    Matrix4x3() = default;

    void Set(float m00, float m01, float m02, float m03,
             float m10, float m11, float m12, float m13,
             float m20, float m21, float m22, float m23) {
        m[0][0] = m00; m[0][1] = m01; m[0][2] = m02; m[0][3] = m03;
        m[1][0] = m10; m[1][1] = m11; m[1][2] = m12; m[1][3] = m13;
        m[2][0] = m20; m[2][1] = m21; m[2][2] = m22; m[2][3] = m23;
    }

    Vector3 GetTranslation() const {
        return Vector3(m[0][3], m[1][3], m[2][3]);
    }
};

struct BoundingBox {
    Vector3 min;
    Vector3 max;

    BoundingBox() = default;
    BoundingBox(const Vector3& bmin, const Vector3& bmax) : min(bmin), max(bmax) {}

    Vector3 GetCenter() const {
        return Vector3(
            (min.x + max.x) * 0.5f,
            (min.y + max.y) * 0.5f,
            (min.z + max.z) * 0.5f
        );
    }

    Vector3 GetExtent() const {
        return Vector3(
            (max.x - min.x) * 0.5f,
            (max.y - min.y) * 0.5f,
            (max.z - min.z) * 0.5f
        );
    }
};

struct BoundingSphere {
    Vector3 center;
    float radius = 0.0f;

    BoundingSphere() = default;
    BoundingSphere(const Vector3& center, float radius) 
        : center(center), radius(radius) {}
};

struct Vertex {
    Vector3 position;
    Vector3 normal;
    // Optional authored tangent frame.  ODOL stores its S/T pair separately
    // from the normal stream; retaining it prevents normal-mapped assets from
    // falling back to a derivative frame at coarse geometry LODs.
    Vector3 tangent;
    Vector3 binormal;
    bool hasTangentFrame = false;
    Vector2 uv;
    // Second UV channel (AST-011C). P3DM models routinely carry more than one
    // #UVSet#, and folding the second into `uv` would destroy exactly the data a
    // material's uvSource selects between. Zero for SP3X, which has no UV sets.
    Vector2 uv1;
    VertexFlags flags = VertexFlags::None;  // Clip flags, lighting, surface attributes

    Vertex() = default;
    Vertex(const Vector3& pos, const Vector3& norm, const Vector2& uv, VertexFlags flags = VertexFlags::None)
        : position(pos), normal(norm), uv(uv), flags(flags) {}
};

struct Triangle {
    uint32_t indices[3] = {0, 0, 0};
    uint32_t materialIndex = 0;
    FaceFlags flags = FaceFlags::None;
    uint32_t originalIndex = 0;       // preserves file order for face stream reconstruction

    Triangle() = default;
    Triangle(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t matIdx = 0, FaceFlags flags = FaceFlags::None)
        : indices{i0, i1, i2}, materialIndex(matIdx), flags(flags) {}
};

struct Quad {
    uint32_t indices[4] = {0, 0, 0, 0};
    uint32_t materialIndex = 0;
    FaceFlags flags = FaceFlags::None;
    uint32_t originalIndex = 0;          // preserves file order for face stream reconstruction

    Quad() = default;
    Quad(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t i3, uint32_t matIdx = 0, FaceFlags flags = FaceFlags::None)
        : indices{i0, i1, i2, i3}, materialIndex(matIdx), flags(flags) {}
};

// Authored RVMAT stage data, carried independently of a renderer binding.
// `uvSource` is the source's numeric selector; mapping it to a vertex UV set is
// a separate, evidence-backed rendering decision.
struct MaterialStage {
    uint32_t    sourceStage = 0;
    std::string texturePath;
    uint32_t    uvSource = 0;
};

struct Material {
    std::string name;
    std::string texturePath;
    // The RVMAT the source face named. Kept separate from texturePath because it
    // is a shader description, not an image; MAT-020/MAT-030 interpret it. SP3X
    // has no equivalent and leaves this empty.
    std::string materialPath;
    std::vector<MaterialStage> embeddedStages;
    float metallic = 0.0f;
    float roughness = 1.0f;
    float emissive = 0.0f;
    FaceFlags flags = FaceFlags::None;

    Material() = default;
    explicit Material(const std::string& name) : name(name) {}
    Material(const std::string& name, const std::string& texture)
        : name(name), texturePath(texture) {}
};

struct NamedSelection {
    std::string name;
    std::vector<uint32_t> vertexIndices;
    std::vector<uint8_t> vertexWeights;    // per-vertex weight (0-255), parallel to vertexIndices

    // AST-018: the source's own weight byte, parallel to vertexIndices, carried
    // undecoded.
    //
    // MLOD named selections store a weight per point and the loader was throwing
    // it away, keeping only "is this point in the selection" -- so a soft
    // selection arrived as a hard one and the original bytes were unrecoverable
    // without re-reading the file.
    //
    // It is kept separate from vertexWeights on purpose. vertexWeights is a
    // linear 0-255 weight and ShapeAdapter treats it as one; the MLOD byte is
    // reportedly not that scale (0 means absent, and the remaining range is not
    // known here to be linear or even in the same direction). Decoding it on a
    // guess would silently reweight every skinned MLOD model, so the byte is
    // preserved under a name that does not claim to be a weight yet.
    std::vector<uint8_t> sourceVertexWeights;
    std::vector<uint32_t> triangleIndices;
    // ODOL pre-computed section mapping for animation Hide
    std::vector<uint32_t> faceSelectionOffsets; // byte offsets into face stream (_faceSel)
    std::vector<uint32_t> sectionIndices;       // sections fully within this selection
    bool needsSections = false;

    NamedSelection() = default;
    explicit NamedSelection(const std::string& name) : name(name) {}
};

struct NamedProperty {
    std::string name;
    std::string value;

    NamedProperty() = default;
    NamedProperty(const std::string& name, const std::string& value)
        : name(name), value(value) {}
};

struct Proxy {
    std::string name;           // e.g., "proxy:weapon", "proxy:driver"
    Matrix4x3 transform;
    uint32_t selectionIndex;
    int32_t id = -1;            // sequence id from binary, used for cargo index

    Proxy() = default;
    explicit Proxy(const std::string& name) : name(name), selectionIndex(0) {}
    Proxy(const std::string& name, const Matrix4x3& transform, uint32_t selIdx, int32_t id = -1)
        : name(name), transform(transform), selectionIndex(selIdx), id(id) {}
};

struct Section {
    uint32_t materialIndex;
    // Despite the names, these are the source's own face-stream offsets, not
    // indices into Mesh::triangles: byte offsets on the ODOL v7 path, and
    // offsets in the four-bytes-plus-four-per-index unit on the ODOL 73 path.
    // They are kept because ShapeAdapter's FindSections needs them in source
    // form. A consumer that reads them as triangle indices is wrong on both.
    uint32_t startTriangle;
    uint32_t triangleCount;
    RenderHints hints;
    int32_t specialMaterial;    // ODOL only

    // AST-018: the same boundary as a face range, when the loader could work one
    // out. firstFace indexes source face order -- the same numbering
    // Triangle::originalIndex and Quad::originalIndex carry -- so a consumer can
    // find a section's faces without knowing the source's offset unit.
    //
    // faceRangeKnown is the whole point of the field. It is false wherever the
    // conversion has not been established (the ODOL v7 path), and a false there
    // must be read as "ask elsewhere", not as "this section has no faces".
    uint32_t firstFace = 0;
    uint32_t faceCount = 0;
    bool faceRangeKnown = false;

    Section() : materialIndex(0), startTriangle(0), triangleCount(0), hints(RenderHints::None), specialMaterial(0) {}
    Section(uint32_t matIdx, uint32_t start, uint32_t count, RenderHints hints = RenderHints::None)
        : materialIndex(matIdx), startTriangle(start), triangleCount(count), hints(hints), specialMaterial(0) {}
};

struct EdgeData {
    std::vector<uint16_t> mlodIndices;    // compact point space (MLOD)
    std::vector<uint16_t> vertexIndices;  // expanded vertex space

    bool IsEmpty() const { return mlodIndices.empty() && vertexIndices.empty(); }
};

struct AnimationFrame {
    float time;
    std::vector<Vector3> positions;

    AnimationFrame() = default;
    AnimationFrame(float time, std::vector<Vector3> positions)
        : time(time), positions(std::move(positions)) {}
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
    std::vector<Quad> quads;
    std::vector<Material> materials;
    std::vector<NamedSelection> selections;
    std::vector<NamedProperty> properties;
    std::vector<Proxy> proxies;
    std::vector<Section> sections;
    EdgeData edges;
    std::vector<AnimationFrame> frames;

    // COL-001: per-VERTEX mass, in this mesh's (post-sort) vertex order, or empty
    // when the source LOD carried no mass. MLOD stores mass per point in the
    // geometry LOD's `#Mass#` tagg; the loader splits points into vertices at
    // normal/UV seams, so a point's mass is divided evenly across the vertices
    // it became -- the total and the centre of mass are preserved, which is all
    // LODShape::CalculateMass reads. ODOL carries the equivalent table on the
    // model (Model::massArray, per point) and leaves this empty.
    std::vector<float> vertexMass;

    BoundingBox boundingBox;
    BoundingSphere boundingSphere;
    Vector3 bCenter;      // per-LOD bounding center (ODOL binary)
    float bRadius = 0.0f; // per-LOD bounding radius (ODOL binary)

    RenderHints orHints = RenderHints::None;
    RenderHints andHints = RenderHints::None;
    SpecialFlags special = SpecialFlags::None;

    uint32_t color = 0xFFFFFFFF;
    uint32_t colorTop = 0xFFFFFFFF;
    uint32_t iconColor = 0;      // ODOL endData
    uint32_t selectedColor = 0;  // ODOL endData

    uint32_t GetVertexCount() const { return static_cast<uint32_t>(vertices.size()); }
    uint32_t GetTriangleCount() const { return static_cast<uint32_t>(triangles.size()); }
    uint32_t GetMaterialCount() const { return static_cast<uint32_t>(materials.size()); }
    
    bool HasGeometry() const { return !vertices.empty() && !triangles.empty(); }
    bool IsEmpty() const { return vertices.empty() && triangles.empty(); }
};

// One #UVSet# block, kept as the source stored it.
struct UVChannel {
    int32_t id = 0;
    std::vector<Vector2> faceVertexUVs;  // one entry per face vertex, source order
};

struct LODLevel {
    float resolution = 0.0f;  // viewing distance in metres, or a special-LOD sentinel

    // AST-018: what the resolution means, decided once by the loader. Consumers
    // should branch on this rather than re-deriving it from the float, which is
    // how a special LOD nobody's table names ends up drawn as ordinary geometry.
    LodPurpose purpose = LodPurpose::Visual;

    Mesh mesh;

    // Every UV channel the source carried, in source order and indexed by
    // cumulative face-vertex -- not merged into the vertex stream. A material's
    // uvSource names one of these, so the mapping has to survive intact even
    // though only channel 1 is currently promoted onto Vertex::uv1.
    std::vector<UVChannel> uvChannels;

    // Source-format facts, recorded rather than re-derived (AST-011B).
    // "MLOD" names the container, not the per-LOD encoding, so a consumer that
    // needs to know which one it got -- or on what winding assumption the indices
    // were built -- has to be told. sourceWinding ending in _ASSUMED means the
    // convention has not been confirmed against a render.
    std::string sourceEncoding;   // "SP3X" | "P3DM" | "ODOL73"

    // AST-018: the typed form of the same facts, with a confidence per axis so
    // "not established" is representable instead of being implied by a suffix on
    // a string. sourceWinding below is derived from it via DescribeWinding, so
    // the label and the record cannot disagree.
    GeometryBasis basis;
    std::string sourceWinding;
    int32_t uvSetCount = 0;       // #UVSet# blocks present; payload is AST-011C
    
    LODLevel() = default;
    explicit LODLevel(float resolution)
        : resolution(resolution), purpose(ClassifyLodResolution(resolution)) {}
    LODLevel(float resolution, const Mesh& mesh)
        : resolution(resolution), purpose(ClassifyLodResolution(resolution)), mesh(mesh) {}
};

enum class SourceGeometryCoverage : uint32_t { Unknown, DeclaredLodsDecoded };
enum class SourceAuditObservation : uint32_t
{
    DirectoryAnimation = 1 << 0, Skeleton = 1 << 1,
    BoneReferences = 1 << 2, Keyframes = 1 << 3
};
inline constexpr uint32_t SourceAuditAllObservations = 0x0f;

// Neutral SOURCE facts, never inferred from empty IR arrays or compile success.
// Observed bits include known absence. Geometry coverage means declared LOD
// bodies decoded, not correct physics/animation or final object readiness.
// Never enables animation or changes rendering policy. Old blobs and unaudited
// converters leave producerVersion zero.
struct ModelSourceAudit
{
    uint32_t producerVersion = 0, sourceRevision = 0;
    SourceGeometryCoverage geometryCoverage = SourceGeometryCoverage::Unknown;
    uint32_t observations = 0, declaredLods = 0, decodedLods = 0;
    uint32_t skeletonBones = 0, directoryAnimationClasses = 0;
    uint32_t keyframeCount = 0, vertexBoneReferenceCount = 0, neighbourBoneReferenceCount = 0;
    bool directoryHasAnimations = false, skeletonDeclared = false;
    bool keyframePayloadDiscarded = false;
    bool Observed(SourceAuditObservation fact) const { return (observations & uint32_t(fact)) != 0; }
};

struct Model {
    ModelSourceAudit sourceAudit;
    // LOD levels sorted by resolution, highest detail first
    std::vector<LODLevel> lodLevels;

    std::map<std::string, std::string> metadata;

    BoundingSphere boundingSphere;
    BoundingBox boundingBox;

    std::string sourcePath;
    std::string sourceFormat;     // "MLOD" or "ODOL"
    uint32_t sourceVersion = 0;

    // Physics (ODOL)
    float mass = 0.0f;
    float invMass = 0.0f;
    float armor = 0.0f;
    float invArmor = 0.0f;
    Vector3 centerOfMass;
    float invInertia[9] = {0};    // 3x3 inverse inertia tensor
    std::vector<float> massArray;

    // Centers and spheres (ODOL)
    Vector3 aimingCenter;
    Vector3 autoCenter;
    Vector3 boundingCenter;
    Vector3 geometryCenter;
    BoundingSphere geometrySphere;

    // Model-level hints and flags (ODOL)
    RenderHints andHints = RenderHints::None;
    RenderHints orHints = RenderHints::None;
    uint32_t canOcclude = 0;
    uint32_t canBeOccluded = 0;
    uint32_t allowAnimation = 0;
    uint32_t lockAutoCenter = 0;
    uint32_t autoCenterEnabled = 1;
    uint32_t mapType = 0;

    // Signed byte indices into LOD array; -1 = absent
    int8_t memoryIdx = -1;
    int8_t geometryIdx = -1;
    int8_t geometryFireIdx = -1;
    int8_t geometryViewIdx = -1;
    int8_t geometryViewPilotIdx = -1;
    int8_t geometryViewGunnerIdx = -1;
    int8_t geometryViewCommanderIdx = -1;
    int8_t geometryViewCargoIdx = -1;
    int8_t landContactIdx = -1;
    int8_t roadwayIdx = -1;
    int8_t pathsIdx = -1;
    int8_t hitpointsIdx = -1;

    uint32_t color = 0xFFFFFFFF;
    uint32_t colorTop = 0xFFFFFFFF;
    float viewDensity = 1.0f;
    float special = 0.0f;         // stored as float for binary compatibility

    // LOD name strings (ODOL)
    std::string memory;
    std::string geometry;
    std::string geometryFire;
    std::string geometryView;
    std::string geometryViewPilot;
    std::string geometryViewGunner;
    std::string geometryViewCommander;
    std::string geometryViewCargo;
    std::string landContact;
    std::string roadway;
    std::string paths;
    std::string hitpoints;

    std::string remarks;

    int32_t memoryLODIndex = -1;
    int32_t geometryLODIndex = -1;
    int32_t fireGeometryLODIndex = -1;
    int32_t viewGeometryLODIndex = -1;
    int32_t viewPilotLODIndex = -1;
    int32_t viewGunnerLODIndex = -1;
    int32_t viewCommanderLODIndex = -1;
    int32_t viewCargoLODIndex = -1;
    int32_t landContactLODIndex = -1;
    int32_t roadwayLODIndex = -1;
    int32_t pathsLODIndex = -1;
    int32_t hitpointsLODIndex = -1;

    int32_t remarksFlags = 0;

    uint32_t GetLODCount() const { return static_cast<uint32_t>(lodLevels.size()); }
    bool HasLODs() const { return !lodLevels.empty(); }
    bool IsEmpty() const { return lodLevels.empty(); }

    const LODLevel* GetHighestLOD() const {
        return lodLevels.empty() ? nullptr : &lodLevels[0];
    }

    const LODLevel* GetLODAtDistance(float distance) const {
        if (lodLevels.empty()) return nullptr;

        // Special LODs have resolution >= 1e5; always select the last for those distances
        if (distance >= 100000.0f) {
            return &lodLevels.back();
        }

        for (const auto& lod : lodLevels) {
            if (distance <= lod.resolution) {
                return &lod;
            }
        }
        return &lodLevels.back();
    }

    bool compile();
    bool isCompiled() const;
};

} // namespace Model
} // namespace Poseidon
