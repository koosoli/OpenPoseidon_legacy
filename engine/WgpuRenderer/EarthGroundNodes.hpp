// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Whole world ground drawn from the elevation pyramid (docs/earth-elevation-pyramid.md, phase 2): CDLOD over the
// web-mercator quadtree. Each frame the nodes the camera needs are selected (a node splits while its distance is
// under SplitFactor times its width), each is one draw of a shared 33 x 33 grid that the vertex shader places on the
// globe and lifts by EarthHeightCache's heights (shader3d conform mode 3), and the data tiles they will read are
// asked for. The window patches' near and horizon meshes are not drawn while this is on
// (POSEIDON_EARTH_HEIGHT_PYRAMID=1); the patch still feeds gameplay and the heightmap.
#include "EarthHeightCache.hpp"
#include "include/wgpu_renderer.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
#include <vector>

namespace Poseidon
{
class EngineWgpu;

class EarthGroundNodes
{
  public:
    static bool Enabled(); // POSEIDON_EARTH_HEIGHT_PYRAMID=1
    EarthGroundNodes(EngineWgpu& engine, WgrRenderer* renderer, const std::filesystem::path& cache);
    ~EarthGroundNodes();
    bool Ready() const { return _heights && _heights->Ready() && _grid != 0; }
    // Main thread, every frame (not at the moon): select, ask for data, install it, submit the draws. Camera in the
    // origin's world frame; `groundY` the ground's world y under it (nodes are measured as if at that height: a
    // camera on a plateau is near its ground, not 3 km over the sea); `horizon` the radius the ground is drawn to (m).
    void Draw(double cameraX, double cameraY, double cameraZ, double groundY, double originLatitude, double originLongitude,
              double horizon);
    size_t LastNodes() const { return _lastNodes; }

  private:
    struct Node
    {
        int z, x, y;
        double distance, radius;
    };
    void Select(int z, int x, int y, std::vector<Node>& out);

    EngineWgpu& _engine;
    WgrRenderer* _renderer;
    std::unique_ptr<EarthHeightCache> _heights;
    WgrMesh _grid = 0;
    uint32_t _gridIndices = 0;
    // this frame's selection inputs
    double _cam[3] = {}, _groundY = 0, _originLat = 0, _originLon = 0, _horizon = 0;
    size_t _lastNodes = 0;
    std::chrono::steady_clock::time_point _statSince{};
    int _statFrames = 0;
    double _statMs = 0.0, _statMax = 0.0, _statPart[5] = {};
};
} // namespace Poseidon
