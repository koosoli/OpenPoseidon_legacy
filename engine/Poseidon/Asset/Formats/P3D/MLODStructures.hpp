#pragma once

#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/Asset/Formats/BISStructures.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace Poseidon::Asset::Formats
{
namespace MLOD
{

// Reject an element count that cannot be backed by the remaining input (each
// element occupies at least one byte on the wire) before it drives a reserve/
// resize into a huge allocation. MLOD counts come straight from the file.
inline void validateCount(BinaryReader& reader, int32_t count, const char* what)
{
    if (count < 0 || count > reader.remaining())
        throw std::runtime_error(std::string("MLOD ") + what + " count out of range");
}

// MLOD file header (12 bytes).
// Signature "MLOD" (0x4D4C4F44), version encoded as major.minor bytes.
struct MLODHeader
{
    char     signature[4] = {0}; // "MLOD"
    uint8_t  versionMajor = 0;
    uint8_t  versionMinor = 0;
    uint16_t padding      = 0;
    uint32_t lodCount     = 0;

    bool isValid() const { return std::memcmp(signature, "MLOD", 4) == 0; }
    bool isMLOD() const { return std::memcmp(signature, "MLOD", 4) == 0; }
};

inline MLODHeader readHeader(BinaryReader& reader)
{
    MLODHeader header;
    uint32_t   sig = reader.read<uint32_t>();
    std::memcpy(header.signature, &sig, 4);
    header.versionMajor = reader.read<uint8_t>();
    header.versionMinor = reader.read<uint8_t>();
    header.padding      = reader.read<uint16_t>();
    header.lodCount     = reader.read<uint32_t>();

    if (std::memcmp(header.signature, "MLOD", 4) != 0)
        throw std::runtime_error("Invalid MLOD signature: expected 'MLOD', got '" + std::string(header.signature, 4) + "'");
    if (header.versionMajor == 0 || header.versionMajor > 10)
        throw std::runtime_error("Invalid MLOD version: " + std::to_string(header.versionMajor) + "." + std::to_string(header.versionMinor));
    if (header.lodCount == 0 || header.lodCount > 100)
        throw std::runtime_error("Invalid LOD count: " + std::to_string(header.lodCount) + " (expected 1-100)");

    return header;
}

// A LOD's own signature, which the file header does not tell you.
//
// "MLOD" names the container, not the per-LOD encoding: an OFP/CWA model stores
// SP3X LODs, every Arma-generation model stores P3DM, and both sit under the
// identical `MLOD 1.1` header. AST-007's inventory of the owner-provided corpora
// measured this -- 1,657 of 1,659 loose models are P3DM, and the two SP3X files
// are camera helpers -- so treating the signature as a constant is not a corner
// case, it is wrong for essentially every Real Virtuality model.
//
// The layouts diverge immediately and incompatibly. SP3X carries a sized header
// and 32-byte fixed texture names in its faces; P3DM has no headSize and stores
// texture and material references as consecutive NUL-terminated strings after the
// vertex block. Their TAGG encodings differ too: SP3X tags are a fixed 64-byte
// name plus a size, P3DM tags are an active byte, an asciiz name, then a size.
// Reading one with the other's layout does not fail cleanly -- it desynchronises
// and reports whatever the misaligned bytes happen to say.
enum class LodSignature
{
    SP3X,
    P3DM,
    Unknown,
};

inline const char* toString(LodSignature signature)
{
    switch (signature)
    {
        case LodSignature::SP3X: return "SP3X";
        case LodSignature::P3DM: return "P3DM";
        default: return "unknown";
    }
}

// Recognised container, unimplemented encoding.
//
// Distinct from the malformed-input runtime_errors above: those mean the bytes
// are not a valid model, this means they are a valid model this build cannot read
// yet. Callers that fall back per-LOD need to tell those apart, and a caller that
// cannot is left guessing from message text.
class UnsupportedLodFormat : public std::runtime_error
{
  public:
    UnsupportedLodFormat(LodSignature signature, uint32_t lodIndex, const std::string& detail)
        : std::runtime_error("MLOD LOD " + std::to_string(lodIndex) + " uses unsupported encoding '" +
                             toString(signature) + "': " + detail),
          signature_(signature),
          lodIndex_(lodIndex)
    {
    }

    LodSignature signature() const { return signature_; }
    uint32_t     lodIndex() const { return lodIndex_; }

  private:
    LodSignature signature_;
    uint32_t     lodIndex_;
};

// Read the next four bytes and rewind, so the chosen reader still sees its own
// signature and the SP3X path stays byte-for-byte what it was.
inline LodSignature peekLodSignature(BinaryReader& reader)
{
    if (reader.remaining() < 4)
        throw std::runtime_error("MLOD LOD signature truncated");

    char     signature[4] = {0};
    uint32_t raw          = reader.read<uint32_t>();
    std::memcpy(signature, &raw, 4);
    reader.seekRelative(-4);

    if (std::memcmp(signature, "SP3X", 4) == 0)
        return LodSignature::SP3X;
    if (std::memcmp(signature, "P3DM", 4) == 0)
        return LodSignature::P3DM;
    return LodSignature::Unknown;
}

// The raw four bytes, for diagnostics that must not lie about what was found.
inline std::string peekLodSignatureBytes(BinaryReader& reader)
{
    if (reader.remaining() < 4)
        return "<truncated>";
    char     signature[4] = {0};
    uint32_t raw          = reader.read<uint32_t>();
    std::memcpy(signature, &raw, 4);
    reader.seekRelative(-4);

    std::string text;
    for (char byte : signature)
        text += (byte >= 0x20 && byte < 0x7F) ? byte : '?';
    return text;
}

// SP3X section header (binary vertex/face data).
// headSize covers all header bytes; fields after the first 28 are skipped.
struct SP3XSection
{
    char    signature[4] = {0}; // "SP3X"
    int32_t headSize     = 0;
    int32_t version      = 0;
    int32_t nPos         = 0; // number of vertex positions
    int32_t nNorm        = 0; // number of normals
    int32_t nFace        = 0; // number of faces
    int32_t flags        = 0;

    bool isValid() const { return std::memcmp(signature, "SP3X", 4) == 0; }
};

inline SP3XSection readSP3XHeader(BinaryReader& reader)
{
    SP3XSection section;
    uint32_t    sig = reader.read<uint32_t>();
    std::memcpy(section.signature, &sig, 4);
    section.headSize = reader.read<int32_t>();
    section.version  = reader.read<int32_t>();
    section.nPos     = reader.read<int32_t>();
    section.nNorm    = reader.read<int32_t>();
    section.nFace    = reader.read<int32_t>();
    section.flags    = reader.read<int32_t>();

    // Skip remaining header bytes beyond the 28 we read
    constexpr int32_t headerStructSize = 28;
    if (section.headSize > headerStructSize)
        reader.seekRelative(section.headSize - headerStructSize);

    if (std::memcmp(section.signature, "SP3X", 4) != 0)
        throw std::runtime_error("Invalid SP3X signature: expected 'SP3X', got '" + std::string(section.signature, 4) + "'");

    return section;
}

struct PointEx
{
    Vector3 position;
    int32_t flags;
};

struct VertexTable
{
    std::vector<PointEx> points;
};

inline VertexTable readVertexTable(BinaryReader& reader, int32_t nPos)
{
    VertexTable vtable;
    validateCount(reader, nPos, "vertex");
    vtable.points.reserve(nPos);
    for (int32_t i = 0; i < nPos; ++i)
    {
        PointEx pt;
        pt.position.x = reader.read<float>();
        pt.position.y = reader.read<float>();
        pt.position.z = reader.read<float>();
        pt.flags      = reader.read<int32_t>();
        vtable.points.push_back(pt);
    }
    return vtable;
}

struct NormalTable
{
    std::vector<Vector3> normals;
};

inline NormalTable readNormalTable(BinaryReader& reader, int32_t nNorm)
{
    NormalTable ntable;
    validateCount(reader, nNorm, "normal");
    ntable.normals.reserve(nNorm);
    for (int32_t i = 0; i < nNorm; ++i)
    {
        Vector3 norm;
        norm.x = reader.read<float>();
        norm.y = reader.read<float>();
        norm.z = reader.read<float>();
        ntable.normals.push_back(norm);
    }
    return ntable;
}

struct DataVertex
{
    int32_t point;
    int32_t normal;
    float   mapU;
    float   mapV;
};

struct DataFaceEx
{
    char       texture[33] = {}; // SP3X: 32 wire bytes + guaranteed NUL terminator
    int32_t    n;       // vertex count (3 or 4)
    DataVertex vs[4];   // up to 4 vertices (MAX_DATA_POLY)
    int32_t    flags;

    // P3DM stores both references as variable-length NUL-terminated strings after
    // the vertex block, and they are routinely longer than SP3X's 32-byte field --
    // a real path like `ca\characters2\civil\worker\data\w1_worker.rvmat` is 46
    // bytes and would be silently truncated into the fixed array above.
    //
    // The two references are also different things, which SP3X had no way to say:
    // `textureName` is an image, `materialName` is an RVMAT describing a whole
    // shader setup. Collapsing them loses the material entirely, so they are kept
    // apart here and MAT-020/MAT-030 decide what the RVMAT means later.
    std::string textureName;
    std::string materialName;
};

struct FaceTable
{
    std::vector<DataFaceEx> faces;
};

inline FaceTable readFaceTable(BinaryReader& reader, int32_t nFace)
{
    FaceTable ftable;
    validateCount(reader, nFace, "face");
    ftable.faces.reserve(nFace);
    for (int32_t i = 0; i < nFace; ++i)
    {
        DataFaceEx face;
        for (int j = 0; j < 32; ++j)
            face.texture[j] = reader.read<char>();
        face.n = reader.read<int32_t>();
        for (int j = 0; j < 4; ++j)
        {
            face.vs[j].point  = reader.read<int32_t>();
            face.vs[j].normal = reader.read<int32_t>();
            face.vs[j].mapU   = reader.read<float>();
            face.vs[j].mapV   = reader.read<float>();
        }
        face.flags = reader.read<int32_t>();
        // Mirror the fixed field into the shared string so downstream conversion
        // has one path for both encodings. SP3X carries no material reference, so
        // materialName stays empty rather than being invented from the texture.
        face.textureName = std::string(face.texture);
        ftable.faces.push_back(face);
    }
    return ftable;
}

// ── P3DM ───────────────────────────────────────────────────────────────────
//
// The Arma-generation MLOD LOD. Same container, different everything else: no
// headSize field, a normal count independent of the vertex count, variable-length
// per-face strings, and a different TAGG encoding.

struct P3DMSection
{
    char    signature[4] = {0}; // "P3DM"
    int32_t majorVersion = 0;
    int32_t minorVersion = 0;
    int32_t nPos         = 0;
    int32_t nNorm        = 0;
    int32_t nFace        = 0;
    int32_t flags        = 0;

    bool isValid() const { return std::memcmp(signature, "P3DM", 4) == 0; }
};

inline P3DMSection readP3DMHeader(BinaryReader& reader)
{
    P3DMSection section;
    uint32_t    sig = reader.read<uint32_t>();
    std::memcpy(section.signature, &sig, 4);
    section.majorVersion = reader.read<int32_t>();
    section.minorVersion = reader.read<int32_t>();
    section.nPos         = reader.read<int32_t>();
    section.nNorm        = reader.read<int32_t>();
    section.nFace        = reader.read<int32_t>();
    section.flags        = reader.read<int32_t>();

    if (!section.isValid())
        throw std::runtime_error("Invalid P3DM signature: expected 'P3DM', got '" + std::string(section.signature, 4) + "'");
    // Unlike SP3X there is no headSize to skip by, so the header size is fixed at
    // 28 bytes and a wrong version would desynchronise everything after it.
    if (section.majorVersion != 28 || section.minorVersion != 256)
        throw std::runtime_error("Unsupported P3DM version " + std::to_string(section.majorVersion) + "." +
                                 std::to_string(section.minorVersion) + " (expected 28.256)");

    return section;
}

// A NUL-terminated string of unbounded length on the wire.
inline std::string readAsciiZ(BinaryReader& reader, const char* what)
{
    std::string text;
    for (;;)
    {
        if (reader.remaining() <= 0)
            throw std::runtime_error(std::string("P3DM ") + what + " string is unterminated");
        char ch = reader.read<char>();
        if (ch == '\0')
            return text;
        // Bound it so a corrupt file cannot grow a string until allocation fails.
        if (text.size() >= 1024)
            throw std::runtime_error(std::string("P3DM ") + what + " string exceeds 1024 bytes");
        text += ch;
    }
}

inline FaceTable readP3DMFaceTable(BinaryReader& reader, int32_t nFace)
{
    FaceTable ftable;
    validateCount(reader, nFace, "face");
    ftable.faces.reserve(nFace);
    for (int32_t i = 0; i < nFace; ++i)
    {
        DataFaceEx face;
        face.n = reader.read<int32_t>();
        if (face.n < 3 || face.n > 4)
            throw std::runtime_error("P3DM face " + std::to_string(i) + " has invalid vertex count " + std::to_string(face.n));
        // Four vertex slots are always stored, even for a triangle; the unused
        // slot is skipped by reading it, not by seeking past a computed size.
        for (int j = 0; j < 4; ++j)
        {
            face.vs[j].point  = reader.read<int32_t>();
            face.vs[j].normal = reader.read<int32_t>();
            face.vs[j].mapU   = reader.read<float>();
            face.vs[j].mapV   = reader.read<float>();
        }
        face.flags        = reader.read<int32_t>();
        face.textureName  = readAsciiZ(reader, "face texture");
        face.materialName = readAsciiZ(reader, "face material");
        ftable.faces.push_back(face);
    }
    return ftable;
}

struct TAGGTag
{
    char    name[65] = {}; // 64 wire bytes + guaranteed NUL terminator
    int32_t size = 0;
};

inline TAGGTag readTAGGTag(BinaryReader& reader)
{
    TAGGTag tag;
    for (int i = 0; i < 64; ++i)
        tag.name[i] = reader.read<char>();
    tag.size = reader.read<int32_t>();
    return tag;
}

struct TAGGHeader
{
    char signature[4];
};

inline TAGGHeader readTAGGHeader(BinaryReader& reader)
{
    TAGGHeader header;
    uint32_t   sig = reader.read<uint32_t>();
    std::memcpy(header.signature, &sig, 4);
    if (std::memcmp(header.signature, "TAGG", 4) != 0)
        throw std::runtime_error("Invalid TAGG signature: expected 'TAGG', got '" + std::string(header.signature, 4) + "'");
    return header;
}

struct TAGGNamedSelection
{
    std::string           name;
    std::vector<uint8_t>  pointWeights; // 0=not selected, >0=selected with weight
    std::vector<bool>     faceFlags;
};

inline TAGGNamedSelection readTAGGNamedSelection(BinaryReader& reader, const TAGGTag& tag, int32_t nPoints, int32_t nFaces)
{
    TAGGNamedSelection sel;
    sel.name = tag.name;

    if (tag.size != nPoints + nFaces)
        throw std::runtime_error("Invalid named selection '" + std::string(tag.name) + "': expected " +
                                 std::to_string(nPoints + nFaces) + " bytes, got " + std::to_string(tag.size));

    validateCount(reader, nPoints, "selection point");
    sel.pointWeights.resize(nPoints);
    reader.readBytes(sel.pointWeights.data(), nPoints);

    validateCount(reader, nFaces, "selection face");
    std::vector<uint8_t> faceBytes(nFaces);
    reader.readBytes(faceBytes.data(), nFaces);
    sel.faceFlags.resize(nFaces);
    for (int32_t i = 0; i < nFaces; ++i)
        sel.faceFlags[i] = (faceBytes[i] != 0);

    return sel;
}

struct TAGGNamedProperty
{
    std::string property;
    std::string value;
};

inline TAGGNamedProperty readTAGGProperty(BinaryReader& reader)
{
    TAGGNamedProperty prop;
    // 64-byte NUL-terminated fields. The +1 byte (zero-initialized) guarantees a
    // terminator so the std::string ctor's strlen can't run off the stack when all
    // 64 wire bytes are non-NUL.
    char propName[65] = {};
    for (int i = 0; i < 64; ++i)
        propName[i] = reader.read<char>();
    prop.property = std::string(propName);

    char propValue[65] = {};
    for (int i = 0; i < 64; ++i)
        propValue[i] = reader.read<char>();
    prop.value = std::string(propValue);

    return prop;
}

struct TAGGMass
{
    std::vector<float> massPerPoint;
};

inline TAGGMass readTAGGMass(BinaryReader& reader, int32_t nPoints)
{
    TAGGMass mass;
    validateCount(reader, nPoints, "mass point");
    mass.massPerPoint.reserve(nPoints);
    for (int32_t i = 0; i < nPoints; ++i)
        mass.massPerPoint.push_back(reader.read<float>());
    return mass;
}

struct TAGGAnimationPhase
{
    float               time;
    std::vector<Vector3> positions; // one per point
};

inline TAGGAnimationPhase readTAGGAnimation(BinaryReader& reader, int32_t nPoints, int32_t tagSize)
{
    TAGGAnimationPhase anim;
    int32_t expectedSize = static_cast<int32_t>(sizeof(float) + sizeof(float) * 3 * nPoints);
    if (tagSize != expectedSize)
        throw std::runtime_error("Invalid #Animation# tag size");

    anim.time = reader.read<float>();
    validateCount(reader, nPoints, "animation point");
    anim.positions.reserve(nPoints);
    for (int32_t i = 0; i < nPoints; ++i)
    {
        Vector3 pos;
        pos.x = reader.read<float>();
        pos.y = reader.read<float>();
        pos.z = reader.read<float>();
        anim.positions.push_back(pos);
    }
    return anim;
}

struct TAGGMaterialIndex
{
    int32_t ambient;
    int32_t diffuse;
    int32_t specular;
    int32_t emissive;
};

inline TAGGMaterialIndex readTAGGMaterialIndex(BinaryReader& reader)
{
    TAGGMaterialIndex mat;
    mat.ambient  = reader.read<int32_t>();
    mat.diffuse  = reader.read<int32_t>();
    mat.specular = reader.read<int32_t>();
    mat.emissive = reader.read<int32_t>();
    return mat;
}

// A decoded #UVSet# payload: a uint32 id followed by one (u,v) pair per ACTUAL
// face vertex -- face.n entries, not the four slots the face record reserves.
// Measured, not assumed: across 3,598 UV-set blocks in the indexed Arma 2/OA
// corpus every payload was exactly 4 + 8 * sum(face.n) bytes, and none matched
// the four-slot reading.
struct UVSetData
{
    int32_t            id = 0;
    std::vector<float> uv;          // 2 floats per face vertex
    bool               decoded = false;  // false when the size did not match
};

struct TAGGData
{
    std::vector<TAGGNamedSelection>  namedSelections;
    std::vector<TAGGNamedProperty>   namedProperties;
    std::vector<TAGGAnimationPhase>  animationPhases;
    TAGGMaterialIndex                materialIndex;
    TAGGMass                         mass;
    // Decoded #UVSet# blocks (AST-011C), in source order.
    std::vector<UVSetData>           uvSets;
    float                            resolution       = 0.0f;
    // How many #UVSet# blocks the LOD carried. Counted by the P3DM reader and left
    // at zero by SP3X, which has no such tagg.
    int32_t                          uvSetCount       = 0;
    bool                             hasMass          = false;
    bool                             hasAnimations    = false;
    bool                             hasMaterialIndex = false;
};

inline TAGGData readTAGGSection(BinaryReader& reader, int32_t nPoints, int32_t nFaces)
{
    TAGGData data;
    readTAGGHeader(reader);

    for (;;)
    {
        TAGGTag tag = readTAGGTag(reader);

        if (std::strcmp(tag.name, "#EndOfFile#") == 0)
            break;
        else if (tag.name[0] != '#')
        {
            if (tag.name[0] != '-' && tag.name[0] != '.')
                data.namedSelections.push_back(readTAGGNamedSelection(reader, tag, nPoints, nFaces));
            else
                reader.seekRelative(tag.size);
        }
        else if (std::strcmp(tag.name, "#Property#") == 0)
            data.namedProperties.push_back(readTAGGProperty(reader));
        else if (std::strcmp(tag.name, "#Mass#") == 0)
        {
            data.mass    = readTAGGMass(reader, nPoints);
            data.hasMass = true;
        }
        else if (std::strcmp(tag.name, "#Animation#") == 0)
        {
            data.animationPhases.push_back(readTAGGAnimation(reader, nPoints, tag.size));
            data.hasAnimations = true;
        }
        else if (std::strcmp(tag.name, "#MaterialIndex#") == 0)
        {
            data.materialIndex    = readTAGGMaterialIndex(reader);
            data.hasMaterialIndex = true;
        }
        else
            reader.seekRelative(tag.size);
    }

    data.resolution = reader.read<float>();
    return data;
}

// P3DM tags are `active byte, asciiz name, uint32 size` -- not SP3X's fixed
// 64-byte name plus size. Reading one with the other's layout desynchronises on
// the first tag, which is why this is a separate function rather than a flag.
inline TAGGData readP3DMTAGGSection(BinaryReader& reader, int32_t nPoints, int32_t nFaces,
                                    int32_t totalFaceVertices)
{
    TAGGData data;
    readTAGGHeader(reader);

    for (;;)
    {
        reader.read<uint8_t>(); // active flag
        std::string name = readAsciiZ(reader, "tagg name");
        int32_t     size = reader.read<int32_t>();
        validateCount(reader, size, "tagg payload");

        if (name == "#EndOfFile#")
            break;

        if (name == "#Property#" && size >= 128)
        {
            data.namedProperties.push_back(readTAGGProperty(reader));
            reader.seekRelative(size - 128);
        }
        else if (name == "#UVSet#")
        {
            ++data.uvSetCount;
            UVSetData set;
            const int32_t expected = 4 + 8 * totalFaceVertices;
            if (size == expected && totalFaceVertices > 0)
            {
                set.id = reader.read<int32_t>();
                set.uv.reserve(static_cast<size_t>(totalFaceVertices) * 2);
                for (int32_t i = 0; i < totalFaceVertices * 2; ++i)
                    set.uv.push_back(reader.read<float>());
                set.decoded = true;
            }
            else
            {
                // Skipped rather than guessed at. A UV block of an unexpected size
                // means this reading of the format does not hold for that file, and
                // decoding it anyway would put plausible-looking wrong UVs on the
                // mesh -- far harder to notice than a channel that is absent.
                reader.seekRelative(size);
            }
            data.uvSets.push_back(std::move(set));
        }
        else if (name == "#Mass#" && size == nPoints * static_cast<int32_t>(sizeof(float)) && nPoints > 0)
        {
            // COL-001: the geometry LOD's per-point mass. SP3X already read it;
            // P3DM (Arma samples, Reforger conversions) skipped it, and a model
            // whose mass never arrives is Object::IsPassable().
            data.mass    = readTAGGMass(reader, nPoints);
            data.hasMass = true;
        }
        else if (!name.empty() && name[0] != '#' && name[0] != '-' && name[0] != '.' &&
                 size == nPoints + nFaces)
        {
            TAGGTag tag{};
            std::snprintf(tag.name, sizeof(tag.name), "%s", name.c_str());
            tag.size = size;
            data.namedSelections.push_back(readTAGGNamedSelection(reader, tag, nPoints, nFaces));
        }
        else
        {
            reader.seekRelative(size);
        }
    }

    data.resolution = reader.read<float>();
    return data;
}


} // namespace MLOD
} // namespace Poseidon::Asset::Formats
