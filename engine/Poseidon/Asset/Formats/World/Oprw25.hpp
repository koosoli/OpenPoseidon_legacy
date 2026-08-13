#pragma once

#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/Asset/Formats/BISStructures.hpp>
#include <Poseidon/Foundation/Algorithms/Lzo1x.hpp>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace Poseidon::Asset::Formats::World
{

// The shared later-generation world reader. OPRW 18 and 20 (Armed Assault), 24
// (Arma 2 / Operation Arrowhead) and 25 (Arma 3) are one container family with the
// same field order; the four differences between the Arma 1 revisions and the later
// ones are enumerated in kOprwModernRevisions below and gated by revision inside
// ReadOprwModern. Keeping that here, rather than teaching the OFP-era WrpReader
// about a structurally unrelated container, makes the generation boundary explicit
// and keeps each profile entry point independently strict.
//
// Revision 25 is what Arma 3 ships. It differs from the OFP-era OPRW the legacy
// reader knows in every structural respect that matters: the per-cell grids are
// stored as quadtrees rather than dense arrays, the bulk payloads are LZO rather
// than LZSS, elevation is float rather than scaled int16, terrain and "land" use
// two different grid resolutions, and object placement is a flat record table
// addressed by a size in bytes instead of a per-cell list.
//
// Layout confirmed against the two real revision-25 worlds available locally
// (a3 map_vr and map_stratis). The confirmation is not "it did not crash": both
// files are consumed to a byte. The road network's own declared size matches what
// walking its per-cell lists consumes (VR 262,144; Stratis 306,262), and the bytes
// left after the object table match the declared map-info size exactly (VR 0;
// Stratis 1,482,744). A wrong field width anywhere earlier could not land on both.
struct Oprw25Header
{
    int32_t version = 0;
    // Revision 25's distinguishing addition: the Steam app or DLC id the world
    // belongs to. It is zero for revision 24 because the field is not stored.
    int32_t appId = 0;
    // "Land" is the coarse grid the geography/material/object-offset quadtrees are
    // indexed by; "terrain" is the fine elevation grid. On both local worlds these
    // are 256 and 2048 -- eight terrain cells per land cell -- so they must not be
    // conflated even though the OFP-era format had only one grid.
    int32_t landRangeX = 0, landRangeY = 0;
    int32_t terrainRangeX = 0, terrainRangeY = 0;
    // Size of a *land* cell in metres (32.0 on both local worlds). The terrain cell
    // size is this scaled by the grid ratio, i.e. 4.0m; see TerrainCellSize().
    float landCellSize = 0.0f;

    float TerrainCellSize() const
    {
        if (terrainRangeX <= 0)
            return 0.0f;
        return landCellSize * static_cast<float>(landRangeX) / static_cast<float>(terrainRangeX);
    }

    // World edge length in metres. 8192 for both local worlds.
    float WorldExtent() const { return landCellSize * static_cast<float>(landRangeX); }
};

struct Oprw25Object
{
    int32_t objectId = 0;
    // Index into Oprw25World::models.
    int32_t modelIndex = 0;
    // Rows are [aside, up, dir, pos], the engine's usual object basis.
    Matrix4x3 transform;
    int32_t shapeParam = 0;
};

// Named world entities: the subset of placements the world file itself binds to a
// config class rather than leaving as anonymous geometry. Stratis has 1,272, all
// of them Land_* props such as runway lights.
struct Oprw25StaticEntity
{
    std::string className;
    std::string shapeName;
    Vector3 position;
    int32_t objectId = 0;
};

struct Oprw25RoadLink
{
    std::vector<Vector3> positions;
    // Revision 24 added a per-connection type byte, parallel to positions. Empty on
    // revisions 18 and 20, which store the positions and then go straight to the
    // object id.
    std::vector<uint8_t> connectionTypes;
    int32_t objectId = 0;
    std::string p3dPath;
    Matrix4x3 toWorld;
};

struct Oprw25World
{
    Oprw25Header header;

    // Per-land-cell packed geography bits (see Oprw25Geography), expanded from the
    // stored quadtree to a dense landRangeX*landRangeY array in row-major order.
    std::vector<int16_t> geography;
    // Per-land-cell index into `materials` naming the terrain surface RVMAT.
    std::vector<uint16_t> materialIndex;
    // Map peaks, in world space. Their maximum matches the elevation maximum.
    std::vector<Vector3> mountains;

    // Per-land-cell colour index and UV jitter, packed as the engine's own
    // RandomInfo bitfield (colour:8, uOff:4, vOff:4 -- the offsets measured at
    // -5..+5 across Sahrani). Stored in the file on revisions 18 and 20, exactly as
    // the OFP-era container stored it; revision 24 dropped it, and the engine
    // regenerates the equivalent at load time (Landscape::InitRandomization).
    // Empty on revisions 24 and 25.
    std::vector<uint16_t> randomization;

    // Per-terrain-cell bytes.
    std::vector<uint8_t> grassApprox;
    // Second per-terrain-cell byte array, added in revision 24; empty on 18 and 20.
    std::vector<uint8_t> primaryTextureIndex;
    std::vector<uint8_t> subdivisionHints;
    std::vector<uint8_t> persistent; // per land cell

    // Per-terrain-cell elevation in metres, row-major.
    std::vector<float> elevation;

    // Terrain surface material paths (`...\layers\p_XXX-YYY_*.rvmat`). Index 0 is
    // the empty string on both local worlds -- an unassigned cell, not a material.
    std::vector<std::string> materials;
    // Object model paths, referenced by Oprw25Object::modelIndex.
    std::vector<std::string> models;

    std::vector<Oprw25StaticEntity> staticEntities;
    // Indexed by land cell (row-major); most cells carry no roads.
    std::vector<std::vector<Oprw25RoadLink>> roadNet;
    std::vector<Oprw25Object> objects;

    int32_t maxObjectId = 0;

    // Trailing map-info block (2D-map annotations: road segments, building outlines,
    // and so on). Kept as bytes: nothing consumes it yet, and decoding it on the
    // strength of its size alone would be a guess rather than a read.
    std::vector<uint8_t> mapInfo;

    float ElevationAt(int x, int z) const { return elevation[static_cast<size_t>(z) * header.terrainRangeX + x]; }
};

// Bit layout of a geography cell. Named accessors rather than raw bits because the
// water-depth and object-count fields are two bits each and easy to mis-shift.
struct Oprw25Geography
{
    int16_t bits = 0;

    uint8_t MinWaterDepth() const { return static_cast<uint8_t>(bits & 0x3); }
    bool Full() const { return ((bits >> 2) & 0x1) != 0; }
    bool Forest() const { return ((bits >> 3) & 0x1) != 0; }
    bool Road() const { return ((bits >> 4) & 0x1) != 0; }
    uint8_t MaxWaterDepth() const { return static_cast<uint8_t>((bits >> 5) & 0x3); }
    uint8_t ObjectCount() const { return static_cast<uint8_t>((bits >> 7) & 0x3); }
    uint8_t HardObjectCount() const { return static_cast<uint8_t>((bits >> 9) & 0x3); }
    uint8_t Gradient() const { return static_cast<uint8_t>((bits >> 11) & 0x7); }
    bool SomeRoadway() const { return ((bits >> 14) & 0x1) != 0; }
    bool SomeObjects() const { return ((bits >> 15) & 0x1) != 0; }
};

namespace Detail
{

// A per-cell grid stored as a 16-ary tree: each internal node carries a 16-bit mask
// saying which of its children are themselves nodes, and every leaf is exactly four
// bytes covering a small rectangle of cells whose shape depends on the element
// width. Uniform regions -- which is most of a terrain -- collapse to one leaf.
//
// The leaf rectangle is derived from the element width rather than tabulated: a
// four-byte leaf holds 4/elementSize elements, laid out 2x2 for bytes, 2x1 for
// 16-bit elements and 1x1 for 32-bit ones. That reproduces the reference
// implementation's indexing wherever the reference is unambiguous, and gives the
// only layout that fills the leaf in the byte case, where the reference reads
// offset zero for all four cells.
class QuadTree
{
  public:
    QuadTree(BinaryReader& reader, int sizeX, int sizeY, int elementSize)
        : sizeX_(sizeX), sizeY_(sizeY), elementSize_(elementSize)
    {
        if (elementSize != 1 && elementSize != 2 && elementSize != 4)
            throw std::runtime_error("OPRW 25 quadtree element size must be 1, 2 or 4");
        if (sizeX <= 0 || sizeY <= 0)
            throw std::runtime_error("OPRW 25 quadtree dimensions must be positive");

        ComputeDimensions();

        rootIsNode_ = reader.read<bool>();
        rootIndex_ = rootIsNode_ ? ReadNode(reader) : ReadLeaf(reader);
    }

    // Element at (x, y), reassembled from the leaf's four bytes.
    template <typename T>
    T Get(int x, int y) const
    {
        static_assert(std::is_trivially_copyable<T>::value, "quadtree element must be trivially copyable");
        const uint8_t* leaf = FindLeaf(x, y);
        const int inLeafX = x & ((1 << leafLogSizeX_) - 1);
        const int inLeafY = y & ((1 << leafLogSizeY_) - 1);
        const int offset = ((inLeafY << leafLogSizeX_) + inLeafX) * elementSize_;
        T value{};
        std::memcpy(&value, leaf + offset, sizeof(T));
        return value;
    }

    // Dense row-major expansion, which is what every consumer here actually wants.
    template <typename T>
    std::vector<T> Expand() const
    {
        std::vector<T> out(static_cast<size_t>(sizeX_) * sizeY_);
        for (int y = 0; y < sizeY_; ++y)
            for (int x = 0; x < sizeX_; ++x)
                out[static_cast<size_t>(y) * sizeX_ + x] = Get<T>(x, y);
        return out;
    }

  private:
    struct Node
    {
        bool isLeaf = false;
        std::array<uint8_t, 4> leaf{};
        // Child indices into nodes_; meaningful only when !isLeaf.
        std::array<int32_t, 16> children{};
    };

    void ComputeDimensions()
    {
        auto bitsToHold = [](int n)
        {
            int bits = 0;
            for (int v = n - 1; v != 0; v >>= 1)
                ++bits;
            return bits;
        };
        int logTotalX = bitsToHold(sizeX_);
        int logTotalY = bitsToHold(sizeY_);

        // 4 / elementSize cells per leaf, as square as the width allows.
        leafLogSizeX_ = elementSize_ == 4 ? 0 : 1;
        leafLogSizeY_ = elementSize_ == 1 ? 1 : 0;

        // Every level of the tree divides by four in each axis, so both axes must
        // reach their leaf size in the same number of levels.
        const int levelsX = (logTotalX - leafLogSizeX_ + kLogSize - 1) / kLogSize;
        const int levelsY = (logTotalY - leafLogSizeY_ + kLogSize - 1) / kLogSize;
        const int levels = levelsX > levelsY ? levelsX : levelsY;

        logTotalX_ = levels * kLogSize + leafLogSizeX_;
        logTotalY_ = levels * kLogSize + leafLogSizeY_;
    }

    int32_t ReadLeaf(BinaryReader& reader)
    {
        Node node;
        node.isLeaf = true;
        reader.readBytes(node.leaf.data(), node.leaf.size());
        nodes_.push_back(node);
        return static_cast<int32_t>(nodes_.size()) - 1;
    }

    int32_t ReadNode(BinaryReader& reader)
    {
        if (++depth_ > kMaxDepth)
            throw std::runtime_error("OPRW 25 quadtree nesting exceeds the depth the grid can require");
        const int16_t flag = reader.read<int16_t>();
        // Reserve before recursing: children append to nodes_ and would invalidate
        // any reference held across the call.
        const int32_t self = static_cast<int32_t>(nodes_.size());
        nodes_.push_back(Node{});
        std::array<int32_t, 16> children{};
        uint16_t mask = static_cast<uint16_t>(flag);
        for (int i = 0; i < 16; ++i)
        {
            children[i] = (mask & 1) ? ReadNode(reader) : ReadLeaf(reader);
            mask >>= 1;
        }
        nodes_[self].isLeaf = false;
        nodes_[self].children = children;
        --depth_;
        return self;
    }

    const uint8_t* FindLeaf(int x, int y) const
    {
        if (x < 0 || x >= sizeX_ || y < 0 || y >= sizeY_)
            throw std::runtime_error("OPRW 25 quadtree access out of range");
        if (!rootIsNode_)
            return nodes_[rootIndex_].leaf.data();

        // Walk from the most significant index bits down; two bits per axis per level.
        uint32_t shiftedX = static_cast<uint32_t>(x) << (32 - logTotalX_);
        uint32_t shiftedY = static_cast<uint32_t>(y) << (32 - logTotalY_);
        int32_t at = rootIndex_;
        while (true)
        {
            const Node& node = nodes_[at];
            const uint32_t ix = shiftedX >> (32 - kLogSize);
            const uint32_t iy = shiftedY >> (32 - kLogSize);
            const int index = static_cast<int>((iy << kLogSize) + ix);
            const int32_t next = node.children[index];
            if (nodes_[next].isLeaf)
                return nodes_[next].leaf.data();
            at = next;
            shiftedX <<= kLogSize;
            shiftedY <<= kLogSize;
        }
    }

    static constexpr int kLogSize = 2; // 4x4 children per node
    static constexpr int kMaxDepth = 32;

    int sizeX_ = 0, sizeY_ = 0, elementSize_ = 0;
    int logTotalX_ = 0, logTotalY_ = 0;
    int leafLogSizeX_ = 0, leafLogSizeY_ = 0;
    bool rootIsNode_ = false;
    int32_t rootIndex_ = -1;
    int depth_ = 0;
    std::vector<Node> nodes_;
};

// Revision 23 switched the bulk arrays from LZSS to LZO; 18 and 20 predate it and
// use the same SSCompress LZSS every other pre-Arma-2 BIS payload uses. Under
// either codec there is no per-payload compression flag: a payload is compressed
// exactly when its decompressed size reaches the engine's 1024-byte threshold. That
// is not an assumption -- it is what makes every local world consume to the byte,
// and a wrong rule would desynchronise the very next field.
//
// The LZSS payload ends in a checksum the decoder verifies, so a wrong `expected`
// is caught at the payload rather than surfacing as nonsense several fields later.
inline std::vector<uint8_t> ReadCompressed(BinaryReader& reader, size_t expected, bool lzo = true)
{
    std::vector<uint8_t> output(expected);
    if (expected == 0)
        return output;
    if (expected < COMPRESSION_THRESHOLD)
    {
        reader.readBytes(output.data(), expected);
        return output;
    }
    if (!lzo)
    {
        reader.readCompressedBytes(output.data(), expected);
        return output;
    }

    const int start = reader.tell();
    const int remaining = reader.remaining();
    if (remaining <= 0)
        throw std::runtime_error("OPRW 25 compressed payload runs past the end of the file");
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
    throw std::runtime_error("OPRW 25 LZO payload did not terminate at the expected size");
}

inline std::string ReadBoundedAsciiz(BinaryReader& reader, const char* field)
{
    std::string value;
    while (true)
    {
        const char c = reader.read<char>();
        if (c == '\0')
            return value;
        if (value.size() == 4096)
            throw std::runtime_error(std::string("OPRW 25 ") + field + " exceeds 4096 bytes");
        value.push_back(c);
    }
}

inline Vector3 ReadVector3(BinaryReader& reader)
{
    Vector3 v;
    v.x = reader.read<float>();
    v.y = reader.read<float>();
    v.z = reader.read<float>();
    return v;
}

inline Matrix4x3 ReadMatrix4x3(BinaryReader& reader)
{
    Matrix4x3 m;
    for (int i = 0; i < 4; ++i)
        m.rows[i] = ReadVector3(reader);
    return m;
}

// A count that is about to size an allocation, checked against what the file can
// still supply. World files are large but their record widths are fixed, so a
// count needing more bytes than remain is malformed rather than merely surprising.
inline int32_t ReadBoundedCount(BinaryReader& reader, const char* field, size_t elementSize)
{
    const int32_t count = reader.read<int32_t>();
    if (count < 0)
        throw std::runtime_error(std::string("OPRW 25 ") + field + " count is negative");
    if (elementSize != 0 && static_cast<uint64_t>(count) * elementSize > static_cast<uint64_t>(reader.remaining()))
        throw std::runtime_error(std::string("OPRW 25 ") + field + " count exceeds remaining input");
    return count;
}

} // namespace Detail

// Object records are a fixed 60 bytes in revision 25: two ints, a 4x3 matrix, and
// the shape parameter revision 14 added. The file gives the table's size in bytes
// rather than its length, so this is the divisor that recovers the count.
constexpr size_t kOprw25ObjectRecordSize = 60;

// The revisions this reader has been read against real bytes for. 21, 22 and 23 sit
// between the two verified groups and are deliberately absent: the codec switch is
// documented at 23 and the other three differences are only bracketed between 20
// and 24, so accepting an unseen revision would be a guess wearing a version check.
// 29 is DayZ, and it is here on the same terms as the rest: read against real
// bytes, not admitted on the strength of its number. Both worlds DayZ ships --
// ChernarusPlus and Enoch, ~224 MB each -- read through this profile and satisfy
// the reader's own end-of-stream checks, which is what 21/22/23 cannot claim.
constexpr int32_t kOprwModernRevisions[] = {18, 20, 24, 25, 29};

inline bool IsOprwModernRevision(int32_t revision)
{
    for (const int32_t known : kOprwModernRevisions)
        if (revision == known)
            return true;
    return false;
}

// Reads one revision of the later-generation container. Throws std::runtime_error
// on anything it does not recognise; it never returns a partially-populated world,
// because a world whose object table came from a desynchronised stream is worse
// than no world at all.
//
// Everything revision-dependent is decided once, here, from the revision the caller
// asked for -- four booleans, each measured rather than assumed:
//
//   lzo               revision 23 moved the bulk arrays from LZSS to LZO.
//   randomization     18/20 store the per-land-cell colour/UV jitter array the
//                     OFP container also stored. 24 dropped it.
//   second byte array 24 added a second per-terrain-cell byte array before the
//                     elevation grid.
//   road link types   24 added the per-connection type byte.
//
// The road-link one is the trap: the file declares the road network's byte length,
// and walking it lands on that length under *either* rule, because the segment
// path is a null-terminated string that resynchronises whatever came before it.
// The discriminator is the path itself -- read with the wrong rule, Sahrani's
// "ca\roads\..." comes back as "\roads\...", two characters eaten. So the size
// check is necessary and not sufficient, and this gate is set from that.
inline Oprw25World ReadOprwModern(BinaryReader& reader, int32_t expectedVersion)
{
    Oprw25World world;

    char signature[4] = {};
    reader.readBytes(signature, sizeof(signature));
    if (std::memcmp(signature, "OPRW", 4) != 0)
        throw std::runtime_error("Not an OPRW world");

    world.header.version = reader.read<int32_t>();
    if (world.header.version != expectedVersion)
        throw std::runtime_error("OPRW revision " + std::to_string(world.header.version) +
                                 " is not the requested profile revision " + std::to_string(expectedVersion));
    if (!IsOprwModernRevision(world.header.version))
        throw std::runtime_error("OPRW revision " + std::to_string(world.header.version) +
                                 " is not a later-generation world container this build reads");

    const bool lzo = world.header.version >= 23;
    const bool hasRandomization = world.header.version < 24;
    const bool hasPrimaryTextureIndex = world.header.version >= 24;
    const bool hasRoadConnectionTypes = world.header.version >= 24;

    if (world.header.version >= 25)
        world.header.appId = reader.read<int32_t>();
    // Revision 29's app id is not a number, it is the FourCC 'ENF0' -- the Enfusion
    // marker -- and it is followed by a further word and a single byte before the
    // grid dimensions begin. Measured on both worlds DayZ ships: reading it this way
    // yields landRange 256x256 and terrainRange 2048x2048 for each, with cell sizes
    // 60 m and 50 m, i.e. 15360 m for ChernarusPlus and 12800 m for Enoch -- the
    // documented sizes of both maps. Off by even one byte none of that lands.
    if (world.header.version >= 29)
    {
        (void)reader.read<uint32_t>();
        (void)reader.read<uint8_t>();
    }
    world.header.landRangeX = reader.read<int32_t>();
    world.header.landRangeY = reader.read<int32_t>();
    world.header.terrainRangeX = reader.read<int32_t>();
    world.header.terrainRangeY = reader.read<int32_t>();
    world.header.landCellSize = reader.read<float>();

    const auto& h = world.header;
    // Both grids index quadtrees and size LZO payloads, so an absurd value here
    // becomes an allocation before any data is read.
    constexpr int32_t kMaxRange = 1 << 16;
    if (h.landRangeX <= 0 || h.landRangeY <= 0 || h.terrainRangeX <= 0 || h.terrainRangeY <= 0 ||
        h.landRangeX > kMaxRange || h.landRangeY > kMaxRange || h.terrainRangeX > kMaxRange ||
        h.terrainRangeY > kMaxRange)
        throw std::runtime_error("OPRW 25 grid dimensions are out of range");
    if (!(h.landCellSize > 0.0f) || h.landCellSize > 4096.0f)
        throw std::runtime_error("OPRW 25 land cell size is out of range");

    const size_t landCells = static_cast<size_t>(h.landRangeX) * h.landRangeY;
    const size_t terrainCells = static_cast<size_t>(h.terrainRangeX) * h.terrainRangeY;

    world.geography = Detail::QuadTree(reader, h.landRangeX, h.landRangeY, 2).Expand<int16_t>();

    // Both dimensions are landRangeX. Not a transcription slip: the sound map is
    // square on the X range even where the land grid is not, and reading it as
    // (landRangeX, landRangeY) would consume the wrong number of leaves.
    Detail::QuadTree soundMap(reader, h.landRangeX, h.landRangeX, 1);
    (void)soundMap;

    const int32_t mountainCount = Detail::ReadBoundedCount(reader, "mountain", sizeof(float) * 3);
    world.mountains.reserve(static_cast<size_t>(mountainCount));
    for (int32_t i = 0; i < mountainCount; ++i)
        world.mountains.push_back(Detail::ReadVector3(reader));

    world.materialIndex = Detail::QuadTree(reader, h.landRangeX, h.landRangeY, 2).Expand<uint16_t>();

    if (hasRandomization)
    {
        const auto raw = Detail::ReadCompressed(reader, landCells * sizeof(uint16_t), lzo);
        world.randomization.resize(landCells);
        std::memcpy(world.randomization.data(), raw.data(), raw.size());
    }

    world.grassApprox = Detail::ReadCompressed(reader, terrainCells, lzo);
    if (hasPrimaryTextureIndex)
        world.primaryTextureIndex = Detail::ReadCompressed(reader, terrainCells, lzo);

    {
        const auto raw = Detail::ReadCompressed(reader, terrainCells * sizeof(float), lzo);
        world.elevation.resize(terrainCells);
        std::memcpy(world.elevation.data(), raw.data(), raw.size());
    }

    {
        const int32_t count = Detail::ReadBoundedCount(reader, "terrain material", 2);
        world.materials.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i)
        {
            world.materials.push_back(Detail::ReadBoundedAsciiz(reader, "terrain material name"));
            // A "major" flag per material. Unread rather than guessed at: no
            // consumer needs it, and both local worlds set it to zero throughout.
            reader.read<uint8_t>();
        }
    }

    {
        const int32_t count = Detail::ReadBoundedCount(reader, "model", 1);
        world.models.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i)
            world.models.push_back(Detail::ReadBoundedAsciiz(reader, "model path"));
    }

    {
        const int32_t count = Detail::ReadBoundedCount(reader, "static entity", 18);
        world.staticEntities.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i)
        {
            Oprw25StaticEntity entity;
            entity.className = Detail::ReadBoundedAsciiz(reader, "static entity class");
            entity.shapeName = Detail::ReadBoundedAsciiz(reader, "static entity shape");
            entity.position = Detail::ReadVector3(reader);
            entity.objectId = reader.read<int32_t>();
            // Revision 29 carries one more word per entity. Identified positionally
            // rather than by name: walking Chernarus's table, the objectId field
            // increments by one from record to record while this one holds the same
            // value throughout, so it is the id that is the id and this is a
            // separate constant. Four bytes short here and the very next thing --
            // the object-offset quadtree -- reads a nonsense tree.
            if (world.header.version >= 29)
                (void)reader.read<uint32_t>();
            world.staticEntities.push_back(std::move(entity));
        }
    }

    // Per-cell offsets into the object and map-info tables. Not retained: the object
    // table is read whole below, and a spatial index built from the placements we
    // actually keep is more useful than one addressing bytes in a file.
    Detail::QuadTree objectOffsets(reader, h.landRangeX, h.landRangeY, 4);
    (void)objectOffsets;
    const int32_t objectTableBytes = reader.read<int32_t>();
    Detail::QuadTree mapInfoOffsets(reader, h.landRangeX, h.landRangeY, 4);
    (void)mapInfoOffsets;
    const int32_t mapInfoBytes = reader.read<int32_t>();
    if (objectTableBytes < 0 || mapInfoBytes < 0)
        throw std::runtime_error("OPRW 25 table size is negative");
    if (static_cast<size_t>(objectTableBytes) % kOprw25ObjectRecordSize != 0)
        throw std::runtime_error("OPRW 25 object table size is not a whole number of records");

    world.persistent = Detail::ReadCompressed(reader, landCells, lzo);
    world.subdivisionHints = Detail::ReadCompressed(reader, terrainCells, lzo);

    world.maxObjectId = reader.read<int32_t>();
    const int32_t roadNetBytes = reader.read<int32_t>();
    const int roadNetStart = reader.tell();
    if (roadNetBytes < 0 || roadNetBytes > reader.remaining())
        throw std::runtime_error("OPRW 25 road network size exceeds remaining input");

    world.roadNet.resize(landCells);
    for (size_t cell = 0; cell < landCells; ++cell)
    {
        const int32_t linkCount = Detail::ReadBoundedCount(reader, "road link", 2);
        if (linkCount == 0)
            continue;
        auto& links = world.roadNet[cell];
        links.reserve(static_cast<size_t>(linkCount));
        for (int32_t i = 0; i < linkCount; ++i)
        {
            Oprw25RoadLink link;
            const int16_t connections = reader.read<int16_t>();
            if (connections < 0)
                throw std::runtime_error("OPRW 25 road link connection count is negative");
            link.positions.reserve(static_cast<size_t>(connections));
            for (int16_t c = 0; c < connections; ++c)
                link.positions.push_back(Detail::ReadVector3(reader));
            if (hasRoadConnectionTypes)
            {
                link.connectionTypes.resize(static_cast<size_t>(connections));
                for (int16_t c = 0; c < connections; ++c)
                    link.connectionTypes[c] = reader.read<uint8_t>();
            }
            link.objectId = reader.read<int32_t>();
            // Revision 29 adds a word here too, in the same place relative to the
            // object id as the one on a static entity. Decoded by hand on the first
            // link of Chernarus's road net: connections=2, two positions inside the
            // map, two type bytes, objectId=25992, this word, then the segment path
            // and a well-formed 4x3 basis -- and the next link's connection count
            // lands exactly where that predicts.
            if (world.header.version >= 29)
                (void)reader.read<uint32_t>();
            link.p3dPath = Detail::ReadBoundedAsciiz(reader, "road segment model");
            link.toWorld = Detail::ReadMatrix4x3(reader);
            links.push_back(std::move(link));
        }
    }
    // The file declares how many bytes the road network occupies, so walking it is
    // self-checking: any mistake in the per-link layout lands somewhere else.
    if (reader.tell() - roadNetStart != roadNetBytes)
        throw std::runtime_error("OPRW 25 road network did not consume its declared size");

    {
        const size_t count = static_cast<size_t>(objectTableBytes) / kOprw25ObjectRecordSize;
        world.objects.resize(count);
        for (size_t i = 0; i < count; ++i)
        {
            Oprw25Object& object = world.objects[i];
            object.objectId = reader.read<int32_t>();
            object.modelIndex = reader.read<int32_t>();
            object.transform = Detail::ReadMatrix4x3(reader);
            object.shapeParam = reader.read<int32_t>();
            if (object.modelIndex < 0 || static_cast<size_t>(object.modelIndex) >= world.models.size())
                throw std::runtime_error("OPRW 25 object references a model index outside the model table");
        }
    }

    // Whatever is left is the map-info block, and the header already said how much
    // that should be. Checking it turns every preceding field into a tested one.
    const int remaining = reader.remaining();
    if (remaining != mapInfoBytes)
        throw std::runtime_error("OPRW 25 trailing map info is " + std::to_string(remaining) +
                                 " bytes but the file declared " + std::to_string(mapInfoBytes));
    world.mapInfo.resize(static_cast<size_t>(remaining));
    if (remaining > 0)
        reader.readBytes(world.mapInfo.data(), world.mapInfo.size());

    return world;
}

// The revision of the OPRW this stream holds, or 0 if it is not an OPRW at all.
// Leaves the stream where it found it.
inline int32_t PeekOprwRevision(BinaryReader& reader)
{
    const int start = reader.tell();
    char signature[4] = {};
    int32_t version = 0;
    try
    {
        reader.readBytes(signature, sizeof(signature));
        version = reader.read<int32_t>();
        if (std::memcmp(signature, "OPRW", 4) != 0)
            version = 0;
    }
    catch (const std::exception&)
    {
        version = 0;
    }
    reader.seek(start);
    return version;
}

// The revision to read this stream at, or 0 if this reader does not handle it.
// One call replaces the chain of per-revision probes every dispatch site grew.
inline int32_t PeekOprwModernRevision(BinaryReader& reader)
{
    const int32_t revision = PeekOprwRevision(reader);
    return IsOprwModernRevision(revision) ? revision : 0;
}

// Reads whichever accepted revision the stream declares. The profile entry points
// below stay for callers that mean one specific generation and want anything else
// refused; this one is for the tools and the runtime, which mean "whatever this is".
inline Oprw25World ReadOprwModern(BinaryReader& reader)
{
    const int32_t revision = PeekOprwModernRevision(reader);
    if (revision == 0)
        throw std::runtime_error("Not an OPRW world of a revision this build reads");
    return ReadOprwModern(reader, revision);
}

// Cheap probe for dispatch, leaving the stream where it found it.
inline bool IsOprwRevision(BinaryReader& reader, int32_t expectedVersion)
{
    return PeekOprwRevision(reader) == expectedVersion;
}

inline Oprw25World ReadOprw25(BinaryReader& reader)
{
    return ReadOprwModern(reader, 25);
}

inline bool IsOprw25(BinaryReader& reader)
{
    return IsOprwRevision(reader, 25);
}

} // namespace Poseidon::Asset::Formats::World
