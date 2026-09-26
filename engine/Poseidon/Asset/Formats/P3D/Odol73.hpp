#pragma once

#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/Asset/Formats/BISStructures.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <Poseidon/Foundation/Algorithms/Lzo1x.hpp>
#include <cstdint>
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Poseidon::Asset::Formats::P3D
{

// AST-012B: the first deliberately narrow later-ODOL reader.  Revision 73 puts
// ModelInfo and the LOD directory at the beginning of the file, unlike v7's
// sequential LOD bodies; it must never be sent to readModel().
struct Odol73Preamble
{
    uint32_t appId = 0;
    std::string muzzleFlash;
    std::vector<float> resolutions;
    int32_t special = 0;
    float boundingSphere = 0.0f;
    float geometrySphere = 0.0f;
    int32_t remarks = 0;
    int32_t andHints = 0;
    int32_t orHints = 0;
    Vector3 aimingCenter;
    uint32_t color = 0;
    uint32_t colorType = 0;
    float viewDensity = 0.0f;
    Vector3 bboxMin;
    Vector3 bboxMax;

    // COL-001: the physical body. These sit in the same ModelInfo prefix every
    // later revision (40, 48-54, 73) reads, and every one of those readers used
    // to consume them and throw them away. That mattered more than it looked:
    // Object::IsPassable() is `GetMass() < 10`, and GetMass() is the shape's
    // stored mass, so a model that arrives with mass 0 is something a soldier
    // walks straight through. That is the whole of "I can walk through walls in
    // every game except OFP" -- the geometry LODs and their convex components
    // were loading fine, and the collision code then discarded every hit because
    // the wall weighed nothing.
    Vector3 centerOfMass;
    Vector3 invInertia[3];
    // The geometry LOD's per-point mass table (compressed float array, one entry
    // per point of the geometry LOD), then the model totals as the binariser
    // computed them.
    std::vector<float> massArray;
    float mass = 0.0f;
    float invMass = 0.0f;
    float armor = 0.0f;
    float invArmor = 0.0f;
    // The special-LOD index table exactly as stored: 12 signed bytes at 40-52
    // (memory, geometry, geometryFire, geometryView, viewPilot, viewGunner,
    // viewCommander, viewCargo, landContact, roadway, paths, hitpoints), 13 at
    // 54 and 14 at 73, where the extra bytes are later-engine slots whose position
    // in the run is not measured here. Kept raw for a census; the converter
    // derives the runtime indices from the LOD resolutions instead, which is
    // what LODShape::ScanShapes does anyway.
    std::vector<int8_t> lodIndexTable;
    std::string propertyClass;
    std::string propertyDamage;
};

struct Odol73Bone
{
    std::string name;
    std::string parent;
};

// One animated source declared by the model. The payload after `sourceAddress`
// depends on `type`, so it is stored in the shape the type implies rather than as
// an opaque blob: the fields are what a later runtime animation layer needs, and
// reading them is what makes the block's length correct.
struct Odol73AnimationClass
{
    uint32_t type = 0;
    std::string name;
    std::string source;
    float minPhase = 0.0f, maxPhase = 0.0f;
    float minValue = 0.0f, maxValue = 0.0f;
    float animPeriod = 0.0f, initPhase = 0.0f;
    uint32_t sourceAddress = 0;

    // Types 0-3, rotation.
    float angle0 = 0.0f, angle1 = 0.0f;
    // Types 4-7, translation.
    float offset0 = 0.0f, offset1 = 0.0f;
    // Type 8, direct.
    Vector3 axisPos;
    Vector3 axisDir;
    float angle = 0.0f, axisOffset = 0.0f;
    // Type 9, hide.
    float hideValue = 0.0f;
    float unused = 0.0f;

    // Types 8 and 9 carry their axis inline above, so they take no per-bone axis
    // pair in the lookup tables. Getting this wrong shifts every later offset.
    bool CarriesPerBoneAxis() const { return type != 8 && type != 9; }
};

// The model's animation tables. Kept rather than skipped: the handover's rule for
// animated assets is to load the static representation and preserve the animation
// information in canonical form, not to discard it.
struct Odol73Animations
{
    std::vector<Odol73AnimationClass> classes;
    // [resolution][bone] -> the animations that drive that bone.
    std::vector<std::vector<std::vector<uint32_t>>> bonesToAnimations;
    // [resolution][animation] -> bone index, or -1 where the animation does not
    // bind a bone at that resolution.
    std::vector<std::vector<int32_t>> animationsToBones;
    // Parallel to animationsToBones. Present only where the animation binds a bone
    // and its type takes a per-bone axis; { position, direction }.
    std::vector<std::vector<std::array<Vector3, 2>>> axes;
    std::vector<std::vector<bool>> hasAxis;
};

struct Odol73LodDirectory
{
    Odol73Preamble model;
    // Declared by ModelInfo. Present on plenty of models whose LODs carry no
    // per-vertex weights at all, so it is read rather than treated as a refusal.
    std::string skeletonName;
    bool skeletonIsDiscrete = false;
    std::vector<Odol73Bone> bones;
    // Absent on the majority of world props; present on doors, containers, wind
    // socks and lights. Its presence no longer refuses the model.
    bool hasAnimations = false;
    Odol73Animations animations;
    std::vector<uint32_t> starts;
    std::vector<uint32_t> ends;
    std::vector<bool> permanent;
};

// An attachment point: another model placed into this one at a named selection.
// Read, not resolved -- following `model` would pull in a second asset, and the
// world census counts what a file declares, not what its dependencies do.
struct Odol73Proxy
{
    std::string model;
    Matrix4x3 transform;
    int32_t sequenceId = 0;
    int32_t namedSelectionIndex = 0;
    int32_t boneIndex = 0;
    int32_t sectionIndex = 0;
};

struct Odol73StaticLodHeader
{
    std::vector<Odol73Proxy> proxies;
    // Two halves of the same mapping between this LOD's reduced bone set and the
    // model's skeleton. Present on animated props whose LOD drives only some bones.
    std::vector<int32_t> subSkeletonsToSkeleton;
    std::vector<std::vector<int32_t>> skeletonToSubSkeleton;
    uint32_t vertexCount = 0;
    float faceArea = 0.0f;
    int32_t orHints = 0;
    int32_t andHints = 0;
    Vector3 bboxMin;
    Vector3 bboxMax;
    Vector3 bboxCenter;
    float bboxRadius = 0.0f;
    std::vector<std::string> textures;
};

struct Odol73EmbeddedMaterial
{
    std::string name;
    uint32_t version = 0;
    uint32_t pixelShader = 0;
    uint32_t vertexShader = 0;
    uint32_t renderFlags = 0;
    std::array<std::array<float, 4>, 6> colours{};
    std::vector<std::string> stageTextures;
    // The stage transform is a 3x4 affine matrix (three basis rows then the
    // translation row), twelve floats -- not a padded 4x4.  Reading sixteen here
    // silently consumed 128 bytes too many per material and put every later
    // offset in the LOD body onto a face record that happened to resynchronise.
    struct TexGen
    {
        uint32_t uvSource = 0;
        std::array<float, 12> matrix{};
    };
    std::vector<TexGen> texGens;
};

struct Odol73Polygons
{
    // faceDataSize is the size of the face stream in the units the section
    // bounds use: four bytes for the vertex count plus four per index.  That is
    // not this file's on-disk face encoding (one byte for the count), so it is
    // kept under its own name and cross-checked rather than assumed.
    uint32_t faceDataSize = 0;
    uint16_t unused = 0;
    // Face-section bounds use revision-specific units. v73 uses four bytes for
    // the count and each index; v49 uses the aligned 16-bit form.
    uint32_t faceStreamHeaderBytes = 4;
    uint32_t faceStreamIndexBytes = 4;
    std::vector<std::vector<uint32_t>> faces;
};

struct Odol73StaticSection
{
    int32_t faceLower = 0, faceUpper = 0;
    int32_t minBone = 0, boneCount = 0;
    int16_t textureIndex = 0;
    // MAT-046: CommonFaceFlags. Read, not skipped -- 0x10000000 is IsHiddenProxy,
    // and dropping this word is why proxy marker triangles draw. The file says
    // the section is a hidden proxy marker (measured 0x10002000 on all 38 marker
    // sections of the Takistan set); discarding it left Section::hints at 0, so
    // neither RegisterGpuModel's skip nor Shape::Draw's had anything to skip.
    //
    // The comment in ShapeAdapter.cpp claiming Oxygen emits sections before
    // marking proxy faces hidden, so section hints lack IsHiddenProxy, is false
    // for this generation -- and believing it is why the fix was attempted
    // through named selections, which are EMPTY on the drawing LOD (717 of 717
    // proxy selections report zero faces on LOD 0).
    uint32_t commonFaceFlags = 0;
    int32_t materialIndex = 0;
};

struct Odol73StaticPreRestData
{
    int32_t colorTop = 0;
    int32_t color = 0;
    int32_t special = 0;
    bool vertexBoneRefIsSimple = false;
    uint32_t restDataSize = 0;
};

struct Odol73NamedSelection
{
    std::string name;
    // AST-019: FACE INDICES into `Odol73Polygons::faces`. Deliberately NOT the
    // same unit as a section's faceLower/faceUpper, which are face-stream byte
    // offsets -- the two sit in the same LOD and disagree, which is exactly how
    // the converter came to run these through the offset table and get nothing.
    // Measured on both generations; see the note in ODOLLoader::convertLOD.
    std::vector<int32_t> selectedFaces;
    bool isSectional = false;
    // The selection's own section list. Present in the format, but EMPTY in
    // practice: 1,171 of 1,171 selections in an ODOL 49 mosque and 414 of 414 in
    // an ODOL 73 hospital carry none, and only 218 of 36,135 across 120
    // structures_e models do. It is not a usable fallback for the face list.
    std::vector<int32_t> sections;
    std::vector<int32_t> selectedVertices;
    // Parallel to selectedVertices when present, and absent entirely when the
    // selection is unweighted -- the payload is sized by its own field, not by
    // the vertex count.
    std::vector<uint8_t> vertexWeights;
};

struct Odol73UvSet
{
    float minU = 0.0f, minV = 0.0f, maxU = 0.0f, maxV = 0.0f;
    // Decoded to floats here: the wire form is a pair of 16-bit fractions of the
    // per-set range, which is meaningless without the range it belongs to.
    std::vector<std::array<float, 2>> uv;
};

// Everything behind the LOD's rest-data size field.  Nothing in here is skipped:
// the reader consumes each array and then requires its own position to match the
// declared size, so a layout mistake cannot pass as a successful read.
struct Odol73StaticRestData
{
    std::vector<int32_t> clip;
    Odol73UvSet uv0;
    uint32_t uvSetCount = 0;
    // Sets 1..n, decoded and kept. Only set 0 and set 1 have a home on Vertex;
    // the rest are preserved here rather than dropped for want of a slot.
    std::vector<Odol73UvSet> extraUvSets;
    std::vector<Vector3> positions;
    std::vector<Vector3> normals;
    // Packed tangent/binormal pairs, retained unconverted: no consumer needs
    // them yet and guessing their basis convention would be a claim, not a read.
    std::vector<std::array<uint32_t, 2>> stCoords;
    // The same data at revision 40, where the pair is 24 bytes rather than 8 and
    // so cannot be carried by the field above. Kept separate rather than widened
    // in place: an A1 entry and an A2/A3 entry are not the same encoding, and a
    // consumer must not be able to read one as the other by accident.
    std::vector<std::array<uint8_t, 24>> stCoordsFull;
    // Per-vertex skinning: a count followed by eight bytes of (bone, weight)
    // pairs. Retained, not applied -- the static path ignores it, but discarding
    // it would throw away the only record that these vertices are skinned at all.
    struct BoneWeights
    {
        int32_t count = 0;
        std::array<uint8_t, 8> data{};
    };
    std::vector<BoneWeights> vertexBoneRefs;
    std::vector<std::array<uint8_t, 32>> neighbourBoneRefs;
    uint32_t trailing = 0;
};

struct Odol73StaticLod
{
    uint32_t observedKeyframes = 0;
    bool keyframesObserved = false;
    Odol73StaticLodHeader header;
    std::vector<Odol73EmbeddedMaterial> materials;
    std::vector<int32_t> pointToVertex;
    std::vector<int32_t> vertexToPoint;
    Odol73Polygons polygons;
    std::vector<Odol73StaticSection> sections;
    std::vector<Odol73NamedSelection> namedSelections;
    std::vector<std::pair<std::string, std::string>> namedProperties;
    Odol73StaticPreRestData endData;
    Odol73StaticRestData rest;
    uint8_t trailingByte = 0;
};

struct Odol73StaticModel
{
    // Only the complete declared-boundary readers establish this proof.
    bool declaredLodBodiesDecoded = false;
    uint32_t decodedRevision = 0;
    Odol73LodDirectory directory;
    std::vector<Odol73StaticLod> lods;
};

inline std::string ReadBoundedAsciiz(BinaryReader& reader, const char* field)
{
    std::string value;
    while (true)
    {
        const char c = reader.read<char>();
        if (c == '\0')
            return value;
        if (value.size() == 4096)
            throw std::runtime_error(std::string("ODOL 73 ") + field + " exceeds 4096 bytes");
        value.push_back(c);
    }
}

// ODOL 73 puts an explicit compression flag before each LZO payload.  The
// decoder reports its explicit LZO end marker, so restore the stream to that
// boundary; no following array is swallowed.
inline std::vector<uint8_t> ReadOdol73CompressedPayload(BinaryReader& reader, size_t expected, bool compressed)
{
    std::vector<uint8_t> output(expected);
    if (!compressed)
    {
        reader.readBytes(output.data(), expected);
        return output;
    }
    const int start = reader.tell();
    const int remaining = reader.remaining();
    std::vector<uint8_t> source(static_cast<size_t>(remaining));
    reader.readBytes(source.data(), source.size());
    size_t consumed = 0;
    if (Foundation::Lzo1x::Decompress(source.data(), source.size(), output.data(), output.size(), &consumed) ==
            expected &&
        consumed != 0)
    {
        reader.seek(start + static_cast<int>(consumed));
        return output;
    }
    throw std::runtime_error("ODOL 73 LZO array did not terminate at the expected size");
}

inline std::vector<uint8_t> ReadOdol73Compressed(BinaryReader& reader, size_t expected)
{
    // A zero-length payload carries no flag byte at all; reading one would eat
    // the count of whatever array follows.
    if (expected == 0)
        return {};
    return ReadOdol73CompressedPayload(reader, expected, reader.read<bool>());
}

// count, then a (possibly compressed) payload of exactly count elements.
template <typename T>
inline std::vector<T> ReadOdol73CompressedArray(BinaryReader& reader, const char* field, uint32_t limit)
{
    const int32_t count = reader.read<int32_t>();
    if (count < 0 || static_cast<uint32_t>(count) > limit)
        throw std::runtime_error(std::string("ODOL 73 ") + field + " count is out of range");
    const auto bytes = ReadOdol73Compressed(reader, static_cast<size_t>(count) * sizeof(T));
    std::vector<T> result(static_cast<size_t>(count));
    if (count != 0)
        std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

// As above, but with a leading "every element is this value" flag.
template <typename T>
inline std::vector<T> ReadOdol73CondensedArray(BinaryReader& reader, const char* field, uint32_t limit)
{
    const int32_t count = reader.read<int32_t>();
    if (count < 0 || static_cast<uint32_t>(count) > limit)
        throw std::runtime_error(std::string("ODOL 73 ") + field + " count is out of range");
    if (reader.read<bool>())
    {
        T value{};
        reader.readBytes(&value, sizeof(T));
        return std::vector<T>(static_cast<size_t>(count), value);
    }
    const auto bytes = ReadOdol73Compressed(reader, static_cast<size_t>(count) * sizeof(T));
    std::vector<T> result(static_cast<size_t>(count));
    if (count != 0)
        std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

inline Odol73Preamble ReadOdol73Preamble(BinaryReader& reader)
{
    if (reader.remaining() < 13)
        throw std::runtime_error("ODOL 73 header truncated");
    char signature[4] = {};
    reader.readBytes(signature, sizeof(signature));
    if (std::memcmp(signature, "ODOL", 4) != 0)
        throw std::runtime_error("ODOL 73 signature expected");
    if (reader.read<uint32_t>() != 73)
        throw std::runtime_error("ODOL 73 reader received another revision");

    Odol73Preamble result;
    result.appId = reader.read<uint32_t>();
    result.muzzleFlash = ReadBoundedAsciiz(reader, "muzzle-flash string");
    const uint32_t lodCount = reader.read<uint32_t>();
    if (lodCount == 0 || lodCount > 100 || static_cast<uint64_t>(lodCount) * sizeof(float) > reader.remaining())
        throw std::runtime_error("ODOL 73 invalid LOD-resolution directory");
    result.resolutions.resize(lodCount);
    for (float& resolution : result.resolutions)
        resolution = reader.read<float>();

    // The stable prefix of v73 ModelInfo.  Later fields include skeleton and
    // address tables and are intentionally not consumed until the LOD-body slice
    // is implemented; keeping the reader bounded prevents a partial parser from
    // claiming it understood an arbitrary remainder.
    result.special = reader.read<int32_t>();
    result.boundingSphere = reader.read<float>();
    result.geometrySphere = reader.read<float>();
    result.remarks = reader.read<int32_t>();
    result.andHints = reader.read<int32_t>();
    result.orHints = reader.read<int32_t>();
    reader.readBytes(&result.aimingCenter, sizeof(result.aimingCenter));
    result.color = reader.read<uint32_t>();
    result.colorType = reader.read<uint32_t>();
    result.viewDensity = reader.read<float>();
    reader.readBytes(&result.bboxMin, sizeof(result.bboxMin));
    reader.readBytes(&result.bboxMax, sizeof(result.bboxMax));
    return result;
}

// The model-level animation block. Reading it rather than refusing it is what
// lets an animated world prop contribute its static geometry: on Stratis this is
// 35 of the 350 referenced models -- doors, shipping containers, runway lights --
// which are ordinary static meshes that merely declare a source.
//
// The block sits immediately before the LOD byte-offset table, so a layout mistake
// here cannot pass silently: the very next thing the directory reader does is
// require every LOD range to be ordered and inside the file.
//
// The table itself is a shared Real Virtuality structure rather than a revision-73
// one: the Arma 2 revisions carry the same classes and the same two lookup tables,
// and differ only in two fields that later revisions added. `revision` selects
// those, so the A2-family reader shares this code instead of copying it.
inline Odol73Animations ReadOdol73Animations(BinaryReader& reader, uint32_t revision = 73)
{
    Odol73Animations result;

    const uint32_t classCount = reader.read<uint32_t>();
    if (classCount > 4096)
        throw std::runtime_error("ODOL 73 model declares too many animation classes");
    result.classes.reserve(classCount);
    for (uint32_t i = 0; i < classCount; ++i)
    {
        Odol73AnimationClass animation;
        animation.type = reader.read<uint32_t>();
        animation.name = ReadBoundedAsciiz(reader, "animation name");
        animation.source = ReadBoundedAsciiz(reader, "animation source");
        animation.minPhase = reader.read<float>();
        animation.maxPhase = reader.read<float>();
        animation.minValue = reader.read<float>();
        animation.maxValue = reader.read<float>();
        // Two version gates: revision 56 added the period/phase pair, revision 55
        // the hide animation's second value. Revision 73 is past both; the Arma 2
        // revisions (49, 50, 52) are before both, and reading the later fields
        // there would shift every subsequent offset in the file.
        if (revision >= 56)
        {
            animation.animPeriod = reader.read<float>();
            animation.initPhase = reader.read<float>();
        }
        animation.sourceAddress = reader.read<uint32_t>();

        switch (animation.type)
        {
            case 0:
            case 1:
            case 2:
            case 3: // rotation about x/y/z and its bounded forms
                animation.angle0 = reader.read<float>();
                animation.angle1 = reader.read<float>();
                break;
            case 4:
            case 5:
            case 6:
            case 7: // translation
                animation.offset0 = reader.read<float>();
                animation.offset1 = reader.read<float>();
                break;
            case 8: // direct: carries its own axis
                reader.readBytes(&animation.axisPos, sizeof(animation.axisPos));
                reader.readBytes(&animation.axisDir, sizeof(animation.axisDir));
                animation.angle = reader.read<float>();
                animation.axisOffset = reader.read<float>();
                break;
            case 9: // hide
                animation.hideValue = reader.read<float>();
                if (revision >= 55)
                    animation.unused = reader.read<float>();
                break;
            default:
                // Every later offset depends on this payload's width, so an
                // unknown type is a refusal, not something to step over.
                throw std::runtime_error("ODOL 73 animation type " + std::to_string(animation.type) +
                                         " has no known payload layout");
        }
        result.classes.push_back(std::move(animation));
    }

    const uint32_t resolutions = reader.read<uint32_t>();
    if (resolutions > 1024)
        throw std::runtime_error("ODOL 73 animation tables declare too many resolutions");
    result.bonesToAnimations.resize(resolutions);
    for (uint32_t lod = 0; lod < resolutions; ++lod)
    {
        const uint32_t bones = reader.read<uint32_t>();
        if (bones > 65536)
            throw std::runtime_error("ODOL 73 animation table declares too many bones");
        result.bonesToAnimations[lod].resize(bones);
        for (uint32_t bone = 0; bone < bones; ++bone)
        {
            const uint32_t count = reader.read<uint32_t>();
            if (count > classCount)
                throw std::runtime_error("ODOL 73 bone references more animations than the model declares");
            result.bonesToAnimations[lod][bone].resize(count);
            for (uint32_t i = 0; i < count; ++i)
                result.bonesToAnimations[lod][bone][i] = reader.read<uint32_t>();
        }
    }

    // The reverse table is not length-prefixed: it is exactly one entry per
    // (resolution, animation class), and each entry is followed by an axis pair
    // only when it binds a bone and the animation's type needs one.
    result.animationsToBones.resize(resolutions);
    result.axes.resize(resolutions);
    result.hasAxis.resize(resolutions);
    for (uint32_t lod = 0; lod < resolutions; ++lod)
    {
        result.animationsToBones[lod].resize(classCount);
        result.axes[lod].resize(classCount);
        result.hasAxis[lod].assign(classCount, false);
        for (uint32_t i = 0; i < classCount; ++i)
        {
            const int32_t bone = reader.read<int32_t>();
            result.animationsToBones[lod][i] = bone;
            if (bone != -1 && result.classes[i].CarriesPerBoneAxis())
            {
                reader.readBytes(&result.axes[lod][i][0], sizeof(Vector3));
                reader.readBytes(&result.axes[lod][i][1], sizeof(Vector3));
                result.hasAxis[lod][i] = true;
            }
        }
    }

    return result;
}

// Static ModelInfo + LOD directory for the selected 3den rectangle fixture.
// Skeletons are read; animations are read as of WLD-012, so an animated prop
// contributes its static geometry instead of being refused. Anything whose layout
// is still unimplemented remains a refusal -- accepting bytes blind would make
// every later offset untrustworthy.
inline Odol73LodDirectory ReadOdol73StaticDirectory(BinaryReader& reader, int fileSize)
{
    Odol73LodDirectory result;
    result.model = ReadOdol73Preamble(reader);
    auto skipFloat = [&reader]() { (void)reader.read<float>(); };
    auto skipVec = [&reader]()
    {
        Vector3 ignored;
        reader.readBytes(&ignored, sizeof(ignored));
    };
    skipFloat();
    skipFloat(); // density coefficient, draw importance
    skipVec();
    skipVec(); // visual bounds
    // Bounding centre and geometry centre are read but deliberately NOT surfaced
    // yet: the runtime has been drawing these models with a zero bounding centre
    // and every proxy / grounding offset is measured against that. Centre of
    // mass and the inertia rows are physics-only (COL-001).
    skipVec();
    skipVec();
    reader.readBytes(&result.model.centerOfMass, sizeof(result.model.centerOfMass));
    for (int i = 0; i < 3; ++i)
        reader.readBytes(&result.model.invInertia[i], sizeof(Vector3)); // inverse inertia rows
    for (int i = 0; i < 5; ++i)
        (void)reader.read<bool>();
    for (int i = 0; i < 6; ++i)
        skipFloat();
    (void)reader.read<bool>();
    (void)reader.read<int32_t>();
    (void)reader.read<bool>();
    skipFloat();
    (void)reader.read<bool>(); // animated
    // A skeleton in ModelInfo is a declaration, not evidence that any LOD is
    // skinned: the per-LOD vertex- and neighbour-bone arrays are what carry
    // weights, and those are still required to be empty. Reading the names here
    // is what lets a rigged-but-static model through; refusing on the name alone
    // rejected models whose geometry this reader handles perfectly well.
    result.skeletonName = ReadBoundedAsciiz(reader, "skeleton name");
    if (!result.skeletonName.empty())
    {
        result.skeletonIsDiscrete = reader.read<bool>();
        const uint32_t boneCount = reader.read<uint32_t>();
        if (boneCount > 4096)
            throw std::runtime_error("ODOL 73 skeleton declares too many bones");
        result.bones.reserve(boneCount);
        for (uint32_t bone = 0; bone < boneCount; ++bone)
        {
            std::string name = ReadBoundedAsciiz(reader, "bone name");
            std::string parent = ReadBoundedAsciiz(reader, "parent bone name");
            result.bones.push_back({std::move(name), std::move(parent)});
        }
        (void)ReadBoundedAsciiz(reader, "obsolete pivots name");
    }
    (void)reader.read<uint8_t>(); // map type
    // COL-001: the geometry LOD's per-point masses and the model totals. Read
    // into the preamble instead of discarded; see Odol73Preamble for why a
    // discarded mass is a wall a soldier walks through.
    result.model.massArray = reader.readCompressedArray<float>();
    result.model.mass = reader.read<float>();
    result.model.invMass = reader.read<float>();
    result.model.armor = reader.read<float>();
    result.model.invArmor = reader.read<float>();
    skipFloat(); // explosion shielding
    result.model.lodIndexTable.resize(14);
    for (int i = 0; i < 14; ++i)
        result.model.lodIndexTable[i] = static_cast<int8_t>(reader.read<uint8_t>());
    (void)reader.read<uint32_t>();
    (void)reader.read<bool>();
    result.model.propertyClass = ReadBoundedAsciiz(reader, "class name");
    result.model.propertyDamage = ReadBoundedAsciiz(reader, "damage name");
    (void)reader.read<bool>();
    (void)reader.read<uint32_t>();
    const size_t count = result.model.resolutions.size();
    for (int array = 0; array < 3; ++array)
        for (size_t i = 0; i < count; ++i)
            (void)reader.read<int32_t>();
    result.hasAnimations = reader.read<bool>();
    if (result.hasAnimations)
        result.animations = ReadOdol73Animations(reader);
    result.starts.resize(count);
    result.ends.resize(count);
    result.permanent.resize(count);
    for (uint32_t& offset : result.starts)
        offset = reader.read<uint32_t>();
    for (uint32_t& offset : result.ends)
        offset = reader.read<uint32_t>();
    for (size_t i = 0; i < count; ++i)
        result.permanent[i] = reader.read<bool>();
    for (size_t i = 0; i < count; ++i)
        if (result.starts[i] >= result.ends[i] || result.ends[i] > static_cast<uint32_t>(fileSize))
            throw std::runtime_error("ODOL 73 LOD directory has an invalid byte range");
    return result;
}

inline Odol73StaticLodHeader ReadOdol73StaticLodHeader(BinaryReader& reader, uint32_t start, uint32_t end)
{
    if (start >= end || end > static_cast<uint32_t>(reader.tell() + reader.remaining()))
        throw std::runtime_error("ODOL 73 LOD header range is invalid");
    reader.seek(static_cast<int>(start));
    Odol73StaticLodHeader result;

    // Three variable-length blocks precede the LOD's own fields. They used to be
    // required empty, which refused every fence, wall and animated prop on
    // Stratis; they are read now (WLD-012). Nothing here is resolved or applied --
    // reading them is what makes the rest of the LOD land on the right offsets,
    // and `end` below is the check that it did.
    auto boundedCount = [&reader, end](const char* name, uint32_t limit)
    {
        const uint32_t count = reader.read<uint32_t>();
        if (count > limit || reader.tell() > static_cast<int>(end))
            throw std::runtime_error(std::string("ODOL 73 ") + name + " count is out of range");
        return count;
    };

    const uint32_t proxyCount = boundedCount("proxy", 4096);
    result.proxies.reserve(proxyCount);
    for (uint32_t i = 0; i < proxyCount; ++i)
    {
        Odol73Proxy proxy;
        proxy.model = ReadBoundedAsciiz(reader, "proxy model");
        for (int row = 0; row < 4; ++row)
            reader.readBytes(&proxy.transform.rows[row], sizeof(Vector3));
        proxy.sequenceId = reader.read<int32_t>();
        proxy.namedSelectionIndex = reader.read<int32_t>();
        proxy.boneIndex = reader.read<int32_t>();
        proxy.sectionIndex = reader.read<int32_t>();
        result.proxies.push_back(std::move(proxy));
    }

    const uint32_t subSkeletonCount = boundedCount("sub-skeleton mapping", 65536);
    result.subSkeletonsToSkeleton.resize(subSkeletonCount);
    for (uint32_t i = 0; i < subSkeletonCount; ++i)
        result.subSkeletonsToSkeleton[i] = reader.read<int32_t>();

    const uint32_t subSkeletonSets = boundedCount("sub-skeleton sets", 65536);
    result.skeletonToSubSkeleton.resize(subSkeletonSets);
    for (uint32_t i = 0; i < subSkeletonSets; ++i)
    {
        const uint32_t members = boundedCount("sub-skeleton set member", 65536);
        result.skeletonToSubSkeleton[i].resize(members);
        for (uint32_t member = 0; member < members; ++member)
            result.skeletonToSubSkeleton[i][member] = reader.read<int32_t>();
    }

    result.vertexCount = reader.read<uint32_t>();
    result.faceArea = reader.read<float>();
    result.orHints = reader.read<int32_t>();
    result.andHints = reader.read<int32_t>();
    reader.readBytes(&result.bboxMin, sizeof(result.bboxMin));
    reader.readBytes(&result.bboxMax, sizeof(result.bboxMax));
    reader.readBytes(&result.bboxCenter, sizeof(result.bboxCenter));
    result.bboxRadius = reader.read<float>();
    const uint32_t textureCount = reader.read<uint32_t>();
    if (textureCount > 64)
        throw std::runtime_error("ODOL 73 LOD has too many texture references");
    result.textures.reserve(textureCount);
    for (uint32_t i = 0; i < textureCount; ++i)
        result.textures.push_back(ReadBoundedAsciiz(reader, "texture reference"));
    if (reader.tell() > static_cast<int>(end))
        throw std::runtime_error("ODOL 73 LOD header exceeds its declared range");
    return result;
}

// One stage texture, with the exact version gates the BIS reference applies:
// the filter appears at material version 5, the stage id only at 8, and the
// world-env-map flag only at 11. The stage id used to be read unconditionally,
// which is four bytes too many on a version-6 or -7 material.
inline std::string ReadOdol73StageTexture(BinaryReader& reader, uint32_t version, const char* field)
{
    if (version >= 5)
        (void)reader.read<uint32_t>(); // texture filter
    std::string texture = ReadBoundedAsciiz(reader, field);
    if (version >= 8)
        (void)reader.read<uint32_t>(); // stage id
    if (version >= 11)
        (void)reader.read<bool>(); // use world environment map
    return texture;
}

inline Odol73EmbeddedMaterial::TexGen ReadOdol73StageTransform(BinaryReader& reader)
{
    Odol73EmbeddedMaterial::TexGen transform;
    transform.uvSource = reader.read<uint32_t>();
    for (float& element : transform.matrix)
        element = reader.read<float>();
    return transform;
}

inline std::vector<Odol73EmbeddedMaterial> ReadOdol73EmbeddedMaterials(BinaryReader& reader)
{
    const uint32_t count = reader.read<uint32_t>();
    // 64 was below the data. AST-013 measured the whole Arma 1 corpus and found
    // characters.pbo : material_wounds.p3d carrying 154 embedded materials -- a
    // wounds material library, not a corrupt file: every one of the 154 is a
    // valid version-9 entry. The guard is here to refuse a desynced stream, so
    // it only has to stay well under an implausible u32.
    if (count > 256)
        throw std::runtime_error("ODOL 73 LOD has too many embedded materials");
    std::vector<Odol73EmbeddedMaterial> materials;
    materials.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        Odol73EmbeddedMaterial material;
        material.name = ReadBoundedAsciiz(reader, "embedded material name");
        material.version = reader.read<uint32_t>();
        if (material.version < 6 || material.version > 32)
            throw std::runtime_error("ODOL 73 embedded material revision is unsupported");
        for (auto& colour : material.colours)
            for (float& component : colour)
                component = reader.read<float>();
        // DayZ's material version 20 widens the record in two places: eight colour
        // quads where every Arma revision writes six, and eighteen further words
        // between the specular power and the shader ids.
        //
        // Measured, not guessed. Each embedded material names its source .rvmat, so
        // the rvmat is independent ground truth: across 300 cross-checked materials
        // the specular power always lands 128 bytes into the colour block (eight
        // quads, not six) and the surface path always at +220, and reading it this
        // way makes PixelShaderID/VertexShaderID resolve to the classic enum values
        // their rvmat names imply -- Normal->0, Super->102, NormalMapSpecularMap->18,
        // Basic->0, Super->23, NormalMap->1, with no name ever mapping to two numbers.
        //
        // The gate is version >= 12 rather than == 20 because the two populations
        // are far apart: every Arma material observed is <= 11 and every DayZ one is
        // exactly 20. Nothing in between has been seen, so where the change actually
        // landed is untested -- this only has to separate the two known families.
        const bool wideMaterial = material.version >= 12;
        if (wideMaterial)
            for (int i = 0; i < 8; ++i)
                (void)reader.read<float>(); // two further colour quads
        (void)reader.read<float>(); // specular power
        if (wideMaterial)
            for (int i = 0; i < 18; ++i)
                (void)reader.read<uint32_t>();
        material.pixelShader = reader.read<uint32_t>();
        material.vertexShader = reader.read<uint32_t>();
        (void)reader.read<uint32_t>();
        (void)reader.read<uint32_t>(); // light + fog
        (void)ReadBoundedAsciiz(reader, "embedded material surface");
        (void)reader.read<uint32_t>();
        material.renderFlags = reader.read<uint32_t>();
        const uint32_t stages = material.version > 6 ? reader.read<uint32_t>() : 0;
        const uint32_t texGens = material.version > 8 ? reader.read<uint32_t>() : stages;
        if (stages > 32 || texGens > 32)
            throw std::runtime_error("ODOL 73 material stage count is invalid");
        material.stageTextures.resize(stages);
        material.texGens.resize(texGens);
        if (material.version < 8)
        {
            // AST-013: below version 8 the reference INTERLEAVES the pair, one
            // transform then its texture, instead of writing all textures and
            // then all transforms. At v<8 texGens is not an independent count --
            // it equals the stage count -- so the two arrays are the same length
            // here by construction.
            //
            // Latent, not live: every Arma 1 material is version 9 (6,932 of
            // 6,932 sampled) and Arma 2/3 are 9 or above, so no file in any local
            // corpus reaches this branch. It matches the reference because a
            // reader that silently disagrees with it is a trap for whoever meets
            // the first v6/v7 file.
            for (uint32_t stage = 0; stage < stages; ++stage)
            {
                material.texGens[stage]       = ReadOdol73StageTransform(reader);
                material.stageTextures[stage] = ReadOdol73StageTexture(reader, material.version, "embedded stage texture");
            }
        }
        else
        {
            for (uint32_t stage = 0; stage < stages; ++stage)
                material.stageTextures[stage] =
                    ReadOdol73StageTexture(reader, material.version, "embedded stage texture");
            for (uint32_t texGen = 0; texGen < texGens; ++texGen)
                material.texGens[texGen] = ReadOdol73StageTransform(reader);
        }
        if (material.version >= 10)
            (void)ReadOdol73StageTexture(reader, material.version, "thermal stage texture");
        materials.push_back(std::move(material));
    }
    return materials;
}

// The point/vertex maps that precede the face stream.  Revision 73 is past the
// v69 boundary, so both are 32-bit indices rather than the older 16-bit form.
inline std::vector<int32_t> ReadOdol73VertexIndexArray(BinaryReader& reader, const char* field)
{
    return ReadOdol73CompressedArray<int32_t>(reader, field, 1'000'000);
}

inline Odol73Polygons ReadOdol73Polygons(BinaryReader& reader, uint32_t vertexCount, uint32_t end)
{
    Odol73Polygons result;
    const int32_t count = reader.read<int32_t>();
    // The smallest face record is five bytes, so a count the range cannot hold
    // is rejected before it drives an allocation.
    if (count < 0 || static_cast<uint64_t>(count) * 5 > static_cast<uint64_t>(end) - reader.tell())
        throw std::runtime_error("ODOL 73 LOD face count does not fit its declared range");
    result.faceDataSize = reader.read<uint32_t>();
    result.unused = reader.read<uint16_t>();
    result.faces.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i)
    {
        const uint8_t vertices = reader.read<uint8_t>();
        // Triangles and quads are the only faces the Model IR carries; a larger
        // polygon would have to be triangulated, which is a decision this narrow
        // reader is not entitled to make silently.
        if (vertices != 3 && vertices != 4)
            throw std::runtime_error("ODOL 73 LOD has a face that is neither a triangle nor a quad");
        std::vector<uint32_t> face(vertices);
        for (uint32_t& index : face)
        {
            index = reader.read<uint32_t>();
            if (index >= vertexCount)
                throw std::runtime_error("ODOL 73 LOD face references a vertex outside the LOD");
        }
        result.faces.push_back(std::move(face));
    }
    if (reader.tell() > static_cast<int>(end))
        throw std::runtime_error("ODOL 73 face stream exceeds its declared LOD range");
    return result;
}

inline std::vector<Odol73StaticSection> ReadOdol73StaticSections(BinaryReader& reader, uint32_t end)
{
    const uint32_t count = reader.read<uint32_t>();
    // 64 was a fixture-sized guess. Real world geometry exceeds it -- a Stratis
    // building's top LOD carries more sections than that -- so the bound is now
    // one that only a desynchronised stream can reach, and the LOD's own declared
    // `end` remains the check that the sections landed where they should.
    if (count > 8192)
        throw std::runtime_error("ODOL 73 LOD has too many sections");
    std::vector<Odol73StaticSection> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        Odol73StaticSection section;
        section.faceLower = reader.read<int32_t>();
        section.faceUpper = reader.read<int32_t>();
        section.minBone = reader.read<int32_t>();
        section.boneCount = reader.read<int32_t>();
        (void)reader.read<uint32_t>();
        section.textureIndex = reader.read<int16_t>();
        section.commonFaceFlags = reader.read<uint32_t>();
        section.materialIndex = reader.read<int32_t>();
        if (section.materialIndex == -1)
            (void)ReadBoundedAsciiz(reader, "section material");
        const uint32_t areas = reader.read<uint32_t>();
        if (areas > 64)
            throw std::runtime_error("ODOL 73 section area array is too large");
        for (uint32_t area = 0; area < areas; ++area)
            (void)reader.read<float>();
        const int32_t flag67 = reader.read<int32_t>();
        if (flag67 >= 1)
            for (int j = 0; j < 11; ++j)
                (void)reader.read<float>();
        if (reader.tell() > static_cast<int>(end))
            throw std::runtime_error("ODOL 73 section exceeds LOD range");
        result.push_back(section);
    }
    return result;
}

// Named selections. The weight payload is sized by its own preceding field
// rather than by the vertex count, so an unweighted selection carries no bytes
// at all -- deriving the size from selectedVertices would desynchronise the
// whole remainder of the LOD.
inline std::vector<Odol73NamedSelection> ReadOdol73NamedSelections(BinaryReader& reader, uint32_t end)
{
    const uint32_t count = reader.read<uint32_t>();
    if (count > 4096)
        throw std::runtime_error("ODOL 73 LOD has too many named selections");
    std::vector<Odol73NamedSelection> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        Odol73NamedSelection selection;
        selection.name = ReadBoundedAsciiz(reader, "named selection");
        selection.selectedFaces = ReadOdol73VertexIndexArray(reader, "named selection faces");
        (void)reader.read<int32_t>(); // unused
        selection.isSectional = reader.read<bool>();
        selection.sections = ReadOdol73CompressedArray<int32_t>(reader, "named selection sections", 1'000'000);
        selection.selectedVertices = ReadOdol73VertexIndexArray(reader, "named selection vertices");
        const int32_t weightBytes = reader.read<int32_t>();
        if (weightBytes < 0 || static_cast<uint32_t>(weightBytes) > 1'000'000)
            throw std::runtime_error("ODOL 73 named selection weight size is out of range");
        const auto weights = ReadOdol73Compressed(reader, static_cast<size_t>(weightBytes));
        selection.vertexWeights.assign(weights.begin(), weights.end());
        if (reader.tell() > static_cast<int>(end))
            throw std::runtime_error("ODOL 73 named selections exceed their declared LOD range");
        result.push_back(std::move(selection));
    }
    return result;
}

inline std::vector<std::pair<std::string, std::string>> ReadOdol73NamedProperties(BinaryReader& reader, uint32_t end)
{
    const uint32_t count = reader.read<uint32_t>();
    if (count > 4096)
        throw std::runtime_error("ODOL 73 LOD has too many named properties");
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        std::string name = ReadBoundedAsciiz(reader, "named property name");
        std::string value = ReadBoundedAsciiz(reader, "named property value");
        result.emplace_back(std::move(name), std::move(value));
    }
    if (reader.tell() > static_cast<int>(end))
        throw std::runtime_error("ODOL 73 named properties exceed their declared LOD range");
    return result;
}

inline Odol73StaticPreRestData ReadOdol73StaticPreRestData(BinaryReader& reader, uint32_t end)
{
    // Keyframes remain unimplemented: an animated LOD is not the static shape
    // this reader claims, and skipping a non-empty array would make every later
    // offset untrustworthy.
    if (reader.read<uint32_t>() != 0)
        throw std::runtime_error("ODOL 73 keyframes are not implemented");
    Odol73StaticPreRestData result;
    result.colorTop = reader.read<int32_t>();
    result.color = reader.read<int32_t>();
    result.special = reader.read<int32_t>();
    result.vertexBoneRefIsSimple = reader.read<bool>();
    result.restDataSize = reader.read<uint32_t>();
    if (reader.tell() > static_cast<int>(end))
        throw std::runtime_error("ODOL 73 pre-rest data exceeds LOD range");
    return result;
}

inline Odol73UvSet ReadOdol73UvSet(BinaryReader& reader, uint32_t vertexCount)
{
    Odol73UvSet set;
    set.minU = reader.read<float>();
    set.minV = reader.read<float>();
    set.maxU = reader.read<float>();
    set.maxV = reader.read<float>();
    const uint32_t count = reader.read<uint32_t>();
    if (count != vertexCount)
        throw std::runtime_error("ODOL 73 UV set does not cover the LOD's vertices");
    const bool defaultFill = reader.read<bool>();
    std::vector<uint8_t> raw;
    if (defaultFill)
    {
        raw.resize(4);
        reader.readBytes(raw.data(), raw.size());
        for (uint32_t i = 1; i < count; ++i)
            raw.insert(raw.end(), raw.begin(), raw.begin() + 4);
    }
    else
    {
        raw = ReadOdol73Compressed(reader, static_cast<size_t>(count) * 4);
    }
    // Each component is a 16-bit fraction of the set's own range.  The +32767
    // and 2^-16 scale are the format's, not a normalisation of our choosing.
    const double deltaU = static_cast<double>(set.maxU) - set.minU;
    const double deltaV = static_cast<double>(set.maxV) - set.minV;
    set.uv.resize(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        int16_t u = 0, v = 0;
        std::memcpy(&u, raw.data() + static_cast<size_t>(i) * 4, 2);
        std::memcpy(&v, raw.data() + static_cast<size_t>(i) * 4 + 2, 2);
        set.uv[i][0] = static_cast<float>(1.52587890625e-05 * (u + 32767) * deltaU + set.minU);
        set.uv[i][1] = static_cast<float>(1.52587890625e-05 * (v + 32767) * deltaV + set.minV);
    }
    return set;
}

// Revision 73 packs a normal into ten bits per axis with a negative scale; that
// sign is the format's, and dropping it would flip every normal in the model.
inline Vector3 DecodeOdol73Normal(uint32_t packed)
{
    auto axis = [](uint32_t value)
    {
        int component = static_cast<int>(value & 0x3FFu);
        if (component > 511)
            component -= 1024;
        return static_cast<float>(component) * (-1.0f / 511.0f);
    };
    Vector3 normal;
    normal.x = axis(packed);
    normal.y = axis(packed >> 10);
    normal.z = axis(packed >> 20);
    return normal;
}

inline Odol73StaticRestData ReadOdol73StaticRestData(BinaryReader& reader, const Odol73StaticLodHeader& header,
                                                     const Odol73StaticPreRestData& preRest, uint32_t end)
{
    const int start = reader.tell();
    if (static_cast<uint64_t>(start) + preRest.restDataSize > end)
        throw std::runtime_error("ODOL 73 rest-data range exceeds its declared LOD range");

    Odol73StaticRestData rest;
    rest.clip = ReadOdol73CondensedArray<int32_t>(reader, "clip flags", 1'000'000);
    rest.uv0 = ReadOdol73UvSet(reader, header.vertexCount);
    // The count follows the first set and covers it, so a file that says 0 still
    // has the one already read.
    rest.uvSetCount = std::max(1u, reader.read<uint32_t>());
    if (rest.uvSetCount > 8)
        throw std::runtime_error("ODOL 73 LOD declares more UV sets than the format uses");
    // Each further set carries its own payload and must be consumed here. They
    // are kept rather than skipped: a material's uvSource names one of them, so
    // dropping them would silently discard exactly the channel it selects.
    for (uint32_t set = 1; set < rest.uvSetCount; ++set)
        rest.extraUvSets.push_back(ReadOdol73UvSet(reader, header.vertexCount));

    struct Packed3
    {
        float x, y, z;
    };
    const auto positions = ReadOdol73CompressedArray<Packed3>(reader, "vertex positions", 1'000'000);
    rest.positions.reserve(positions.size());
    for (const auto& position : positions)
        rest.positions.push_back(Vector3{position.x, position.y, position.z});

    const auto normals = ReadOdol73CondensedArray<uint32_t>(reader, "vertex normals", 1'000'000);
    rest.normals.reserve(normals.size());
    for (uint32_t packed : normals)
        rest.normals.push_back(DecodeOdol73Normal(packed));

    struct PackedSt
    {
        uint32_t s, t;
    };
    const auto st = ReadOdol73CompressedArray<PackedSt>(reader, "ST coordinates", 1'000'000);
    rest.stCoords.reserve(st.size());
    for (const auto& pair : st)
        rest.stCoords.push_back({pair.s, pair.t});

    // Skinning. Read and kept rather than refused (WLD-012): on Stratis this alone
    // blocked 38 models. The element is 12 bytes -- a 32-bit count then eight
    // bytes of paired bone and weight -- which is a layout, not a guess: the whole
    // rest-data block is size-checked against its own declared length below.
    {
        struct WireBoneRef
        {
            int32_t count;
            std::array<uint8_t, 8> data;
        };
        static_assert(sizeof(WireBoneRef) == 12, "ODOL 73 bone reference must be 12 bytes on the wire");
        const auto wire = ReadOdol73CompressedArray<WireBoneRef>(reader, "vertex bone references", 1'000'000);
        rest.vertexBoneRefs.reserve(wire.size());
        for (const auto& entry : wire)
            rest.vertexBoneRefs.push_back({entry.count, entry.data});
    }
    // Neighbour bone references, 32 bytes each. Retained unconverted, in the same
    // spirit as stCoords above: the static path has no use for them, and naming
    // their fields on the strength of their width would be a claim rather than a
    // read. Reading them is what the block's length requires.
    rest.neighbourBoneRefs =
        ReadOdol73CompressedArray<std::array<uint8_t, 32>>(reader, "neighbour bone references", 1'000'000);
    rest.trailing = reader.read<uint32_t>();

    if (rest.positions.size() != header.vertexCount || rest.normals.size() != header.vertexCount ||
        rest.clip.size() != header.vertexCount)
        throw std::runtime_error("ODOL 73 rest-data arrays disagree with the LOD's vertex count");
    // The decisive check: the LOD declared how many bytes this section spans, so
    // a layout error cannot be mistaken for a successful read.
    if (reader.tell() - start != static_cast<int>(preRest.restDataSize))
        throw std::runtime_error("ODOL 73 rest data did not consume its declared size");
    return rest;
}

inline uint8_t ReadOdol73TrailingLodByte(BinaryReader& reader, uint32_t end)
{
    if (reader.tell() != static_cast<int>(end) - 1)
        throw std::runtime_error("ODOL 73 trailing LOD field is not at the declared boundary");
    const uint8_t value = reader.read<uint8_t>();
    if (reader.tell() != static_cast<int>(end))
        throw std::runtime_error("ODOL 73 trailing LOD field did not close the declared range");
    return value;
}

// One complete static LOD body, start to declared end.  Every field between the
// two offsets is consumed; the closing check is what makes the read a fact
// rather than a plausible-looking prefix.
inline Odol73StaticLod ReadOdol73StaticLod(BinaryReader& reader, uint32_t start, uint32_t end)
{
    Odol73StaticLod lod;
    lod.header = ReadOdol73StaticLodHeader(reader, start, end);
    lod.materials = ReadOdol73EmbeddedMaterials(reader);
    lod.pointToVertex = ReadOdol73VertexIndexArray(reader, "point-to-vertex map");
    lod.vertexToPoint = ReadOdol73VertexIndexArray(reader, "vertex-to-point map");
    lod.polygons = ReadOdol73Polygons(reader, lod.header.vertexCount, end);
    lod.sections = ReadOdol73StaticSections(reader, end);
    lod.namedSelections = ReadOdol73NamedSelections(reader, end);
    lod.namedProperties = ReadOdol73NamedProperties(reader, end);
    lod.endData = ReadOdol73StaticPreRestData(reader, end);
    lod.keyframesObserved = true; // Nonzero counts are refused by this reader.
    lod.rest = ReadOdol73StaticRestData(reader, lod.header, lod.endData, end);
    lod.trailingByte = ReadOdol73TrailingLodByte(reader, end);
    return lod;
}

inline Odol73StaticModel ReadOdol73StaticModel(BinaryReader& reader, int fileSize)
{
    Odol73StaticModel model;
    model.directory = ReadOdol73StaticDirectory(reader, fileSize);
    model.lods.reserve(model.directory.starts.size());
    for (size_t i = 0; i < model.directory.starts.size(); ++i)
        model.lods.push_back(ReadOdol73StaticLod(reader, model.directory.starts[i], model.directory.ends[i]));
    model.declaredLodBodiesDecoded = true;
    model.decodedRevision = 73;
    return model;
}

// Face-stream offsets, in the units the section bounds use: four bytes for the
// vertex count plus four per index.  Returned so section ranges can be turned
// into face indices instead of being carried around as opaque numbers; the
// caller checks the total against the LOD's own declared face-data size.
inline std::vector<uint32_t> Odol73FaceStreamOffsets(const Odol73Polygons& polygons)
{
    std::vector<uint32_t> offsets;
    offsets.reserve(polygons.faces.size() + 1);
    uint32_t offset = 0;
    for (const auto& face : polygons.faces)
    {
        offsets.push_back(offset);
        offset += polygons.faceStreamHeaderBytes + polygons.faceStreamIndexBytes * static_cast<uint32_t>(face.size());
    }
    offsets.push_back(offset);
    return offsets;
}

inline Material::RvMaterialSource ToRvMaterialSource(const Odol73EmbeddedMaterial& raw)
{
    Material::RvMaterialSource source;
    source.origin = raw.name;
    source.embedded = true;
    source.emissive = raw.colours[0];
    source.ambient = raw.colours[1];
    source.diffuse = raw.colours[2];
    source.forcedDiffuse = raw.colours[3];
    source.specular = raw.colours[4];
    source.extra.emplace_back("odolPixelShader", std::to_string(raw.pixelShader));
    source.extra.emplace_back("odolVertexShader", std::to_string(raw.vertexShader));
    source.extra.emplace_back("odolRenderFlags", std::to_string(raw.renderFlags));
    for (size_t i = 0; i < raw.stageTextures.size(); ++i)
    {
        Material::RvStage stage;
        stage.index = static_cast<int>(i);
        stage.texture = Material::RvTextureRef::Parse(raw.stageTextures[i]);
        source.stages.push_back(std::move(stage));
    }
    for (const auto& rawGen : raw.texGens)
    {
        Material::RvTexGen gen;
        gen.index = static_cast<int>(source.texGens.size());
        gen.uvSource = std::to_string(rawGen.uvSource);
        gen.uvTransform.present = true;
        for (int c = 0; c < 3; ++c)
        {
            gen.uvTransform.aside[c] = rawGen.matrix[c];
            gen.uvTransform.up[c] = rawGen.matrix[3 + c];
            gen.uvTransform.dir[c] = rawGen.matrix[6 + c];
            gen.uvTransform.pos[c] = rawGen.matrix[9 + c];
        }
        source.texGens.push_back(std::move(gen));
    }
    return source;
}

} // namespace Poseidon::Asset::Formats::P3D
