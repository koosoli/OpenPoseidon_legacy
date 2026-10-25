// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Whole world ground drawn from the elevation pyramid (docs/earth-elevation-pyramid.md, phase 2): CDLOD over the
// web-mercator quadtree. Each frame the nodes the camera needs are selected (a node splits while its distance is
// under SplitFactor times its width), each is one draw of a shared 33 x 33 grid that the vertex shader places on the
// globe and lifts by EarthHeightCache's heights (shader3d conform mode 3), and the data tiles they will read are
// asked for. The window patches' near and horizon meshes are neither built nor drawn while this is on (the default;
// POSEIDON_EARTH_HEIGHT_PYRAMID=0 brings them back); the patch still feeds gameplay and the heightmap.
#include "EarthHeightCache.hpp"
#include "include/wgpu_renderer.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
class EngineWgpu;

class EarthGroundNodes
{
  public:
    static bool Enabled(); // unless POSEIDON_EARTH_HEIGHT_PYRAMID=0
    EarthGroundNodes(EngineWgpu& engine, WgrRenderer* renderer, const std::filesystem::path& cache);
    ~EarthGroundNodes();
    bool Ready() const { return _heights && _heights->Ready() && _grid != 0; }
    // Main thread, every frame (not at the moon): select, ask for data, install it, submit the draws. Camera in the
    // origin's world frame; `groundY` the ground's world y under it (nodes are measured as if at that height: a
    // camera on a plateau is near its ground, not 3 km over the sea); `horizon` the radius the ground is drawn to (m).
    // The main view, for dropping nodes it cannot see (Earth ground draws only into it: not a shadow caster).
    // Directions are world unit vectors; tanX, tanY the half-angle tangents (Camera::Left, Top). `valid` false: draw all.
    struct View
    {
        bool valid = false;
        double forward[3], up[3], aside[3];
        double tanX, tanY;
    };
    // `gameplayWindow`: the installed terrain heightmap is this origin's patch -- inside it the nodes take its heights
    // (shader3d conform mode 3.5), so the drawn ground is the ground feet stand on.
    void Draw(double cameraX, double cameraY, double cameraZ, double groundY, double originLatitude, double originLongitude,
              double horizon, bool gameplayWindow, const View& view);
    size_t LastNodes() const { return _lastNodes; }
    // Could node `n` be in `view`? Its middle at (cx, cz) and its heights from the data under it.
    bool Visible(const View& view, int z, int x, int y, double cx, double cz, double width);

  private:
    struct Node
    {
        int z, x, y;
        double distance, radius;
    };
    // A frame's per-row and per-column trigonometry: selection visits ~1700 nodes but only some ten rows and columns
    // of each zoom, and recomputing them per node was most of its cost (2026-10-24: 0.45-1.07 ms a frame)
    struct Row
    {
        int y;
        double latitude, sinP, cosP, width;
    };
    struct Column
    {
        int x;
        double sinDl, cosDl;
    };
    const Row& RowOf(int z, int y);
    const Column& ColumnOf(int z, int x);
    void Select(int z, int x, int y, std::vector<Node>& out);

    EngineWgpu& _engine;
    WgrRenderer* _renderer;
    std::unique_ptr<EarthHeightCache> _heights;
    WgrMesh _grid = 0;
    uint32_t _gridIndices = 0;
    // this frame's selection inputs
    double _cam[3] = {}, _groundY = 0, _originLat = 0, _originLon = 0, _horizon = 0, _sinP0 = 0, _cosP0 = 1;
    std::vector<Row> _rows[20];
    std::vector<Column> _columns[20];
    std::vector<Node> _nodes;
    std::vector<EarthHeightCache::Key> _wanted;
    std::unordered_map<uint64_t, std::pair<float, float>> _ranges; // this frame's HeightRange answers, by data tile
    size_t _lastNodes = 0;
    std::chrono::steady_clock::time_point _statSince{};
    int _statFrames = 0;
    double _statMs = 0.0, _statMax = 0.0, _statPart[5] = {};
};
} // namespace Poseidon
