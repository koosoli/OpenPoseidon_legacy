#pragma once

// Builders: roads laid along a path, forests painted by the cell, bridges spanned
// between two banks, and regions of an island exported and imported.
//
// They place the game's own models the way the game's own islands do; the joining
// points of road pieces come from the models themselves.

#include "GameData.hpp"
#include "Island.hpp"

#include <array>
#include <string>
#include <vector>

namespace island
{
// ---------------------------------------------------------------------------
// roads
// ---------------------------------------------------------------------------

// One road model: where it is entered and left, in its own space (x aside, z forward).
struct RoadPiece
{
    std::string model;
    float begin[2] = {}, beginDir[2] = {0, 1}; // the middle of its begin edge and the way in
    float end[2] = {}, endDir[2] = {0, 1};     // the middle of its end edge and the way out
    float corner[4][2] = {};                   // LB, PB, PE, LE: its outline
    float width = 0, length = 0;
    float turn = 0; // degrees it turns to the right when gone through from begin to end
    bool cap = false; // an end piece: closed at its begin
};

// The pieces of one kind of road: straights of several lengths, curves of several radii.
struct RoadFamily
{
    std::string name;   // "data3d\asf  7.0 m"
    float width = 0;
    std::vector<RoadPiece> straights; // longest first
    std::vector<RoadPiece> curves;    // gentlest first
    bool hasCap = false;              // it has an end piece
    RoadPiece capPiece;
};

// A piece put down: which, which way through, and where.
struct PlacedRoad
{
    const RoadPiece* piece = nullptr;
    bool reversed = false;
    float x = 0, z = 0, yaw = 0; // of the object
    float outline[4][2] = {};    // its corners on the map
};

class Roads
{
  public:
    // Looks through the game's models (and the island's) for road pieces.
    void Find(const GameData& game, const Island& is);
    bool Found() const { return _found; }
    const std::vector<RoadFamily>& Families() const { return _families; }

    // Pieces following `path` (x, z points).  With `heading` the road starts that way
    // (radians from north) instead of towards the second point.
    std::vector<PlacedRoad> Lay(const RoadFamily& family, const std::vector<std::array<float, 2>>& path, bool caps,
                                const float* heading = nullptr) const;
    static Object MakeObject(const PlacedRoad& placed, int id);

    // The open end of a road on the island nearest (x, z) within `reach`: where a new
    // road can carry on from, and which way.
    bool OpenEnd(const Island& is, const GameData& game, float x, float z, float reach, float at[2], float& heading) const;

  private:
    bool _found = false;
    std::vector<RoadFamily> _families;
};

// ---------------------------------------------------------------------------
// forests
// ---------------------------------------------------------------------------

// A forest as the game builds it: every cell two half blocks, and a triangular block in
// the open cells that have forest on two sides next to each other.
struct ForestKind
{
    std::string name;
    std::string half1, half2, corner;
    float off1[2] = {}, off2[2] = {}; // where the half blocks sit from the middle of the cell
    float above1 = 0, above2 = 0, aboveCorner = 0; // their origins above the ground
};

std::vector<ForestKind> FindForests(const GameData& game);

// Makes the cells under the brush forest (or open land) and puts the corner blocks right
// around them.  Returns the number of cells changed.  Call inside an edit.
int PaintForest(Island& is, const ForestKind& kind, float wx, float wz, float radius, bool plant, bool corners);
// the same for a list of cells (x, z)
int SetForestCells(Island& is, const ForestKind& kind, const std::vector<std::array<int, 2>>& cells, bool plant, bool corners);
bool IsForestBlock(const std::string& model); // any half or corner block of any forest

// ---------------------------------------------------------------------------
// bridges
// ---------------------------------------------------------------------------

struct BridgeKind
{
    std::string model;
    float length = 50.0f; // along the way across
    float deck = 0.0f;    // the roadway above the model's origin
};

std::vector<BridgeKind> FindBridges(const GameData& game);

// Spans from one bank to the other with the deck at `deckLevel` (metres above the sea).
// Returns the number of sections placed.  Call inside an edit.
int BuildBridge(Island& is, const BridgeKind& kind, float x0, float z0, float x1, float z1, float deckLevel);

// ---------------------------------------------------------------------------
// regions
// ---------------------------------------------------------------------------

struct RegionInfo
{
    bool marked = false; // the file says which part of it is the region
    int width = 0, height = 0; // cells
    int fromX = 0, fromZ = 0;  // where on its island it was
};

// Cells x0..x1, z0..z1 with their elevations, textures and objects as an island file
// (4WVR, padded to a size the games accept; the editor notes the real extent in it).
bool ExportRegion(const Island& is, int x0, int z0, int x1, int z1, const std::string& file, std::string& error);
RegionInfo ReadRegionInfo(const std::string& file, const Island& loaded);

struct ImportOptions
{
    bool heights = true, textures = true, objects = true;
    bool clearObjects = true; // remove what stands in the way first
};
// Puts the region of `src` down with its south-west corner at cell (dx, dz).  Call inside an edit.
void ImportRegion(Island& is, const Island& src, const RegionInfo& info, int dx, int dz, const ImportOptions& opt);
} // namespace island
