// SPDX-License-Identifier: GPL-3.0-or-later
#include "EarthGroundNodes.hpp"
#include "EarthCoordinates.hpp"
#include "EngineWgpu.hpp"

#include <Poseidon/Foundation/Framework/Log.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Poseidon
{
namespace
{
constexpr double Pi = 3.14159265358979323846;
constexpr double Circumference = 40075016.68557849;
constexpr int GridN = 32;           // quads a node side: shader3d's 32.0
constexpr int MinZoom = 2, MaxZoom = 19;
constexpr double SplitFactor = 3.0; // a node splits while closer than this many of its widths
constexpr double Detail = 0.005;    // shader3d EH_DETAIL
constexpr int GroundDetailZoom = 15; // nodes this fine carry the near ground's detail (sand, grain)

double NodeLatitude(int z, double y) // y: row (may be fractional)
{
    return std::atan(std::sinh(Pi * (1.0 - 2.0 * y / double(1 << z)))) * 180.0 / Pi;
}
double DataLevel(double distance, double latitude)
{
    return std::clamp(std::log2(Circumference * std::max(std::cos(latitude * Pi / 180.0), 0.01) / (256.0 * std::max(distance, 1.0) * Detail)),
                      0.0, double(EarthHeightCache::MaxZoom));
}
float Bits(int v)
{
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}
} // namespace

bool EarthGroundNodes::Enabled()
{
    static const bool on = []
    {
        // the default since 2026-10-24 (phase 3); =0 brings back the window patches' meshes
        const char* v = std::getenv("POSEIDON_EARTH_HEIGHT_PYRAMID");
        return !(v && v[0] == '0');
    }();
    return on;
}

EarthGroundNodes::EarthGroundNodes(EngineWgpu& engine, WgrRenderer* renderer, const std::filesystem::path& cache)
    : _engine(engine), _renderer(renderer)
{
    _heights = std::make_unique<EarthHeightCache>(renderer, cache);
    if (!_heights->Ready())
        return;
    // the shared node grid: (u, 0, v) with u east, v south, both 0..1; its UVs the same
    std::vector<WgrMeshVertex> vertices;
    std::vector<uint32_t> indices;
    for (int j = 0; j <= GridN; ++j)
        for (int i = 0; i <= GridN; ++i)
        {
            WgrMeshVertex v{};
            v.pos = {float(i) / GridN, 0.0f, float(j) / GridN};
            v.normal = {0.0f, 1.0f, 0.0f};
            v.uv = {float(i) / GridN, float(j) / GridN};
            vertices.push_back(v);
        }
    for (int j = 0; j < GridN; ++j)
        for (int i = 0; i < GridN; ++i)
        {
            const uint32_t a = uint32_t(j * (GridN + 1) + i), b = a + 1, c = a + GridN + 1, d = c + 1;
            // clockwise seen from above: the depth prepass culls back faces whatever the draw's double-sided
            // flag (gfx3d build_prepass_pipeline), and wound the other way the nodes were in colour but not in depth --
            // the sea, testing that depth, covered every valley
            indices.insert(indices.end(), {a, b, c, b, d, c});
        }
    // Skirts: each edge again with y = 1, which the shader hangs below the ground. Neighbouring nodes place their
    // shared edge from different f32 origins and the two can part by a hair -- a dotted line of sky through the ground
    // without these. Both windings, as the prepass culls and a skirt is seen from either side.
    const auto skirt = [&](auto edgeIndex)
    {
        const uint32_t first = uint32_t(vertices.size());
        for (int k = 0; k <= GridN; ++k)
        {
            WgrMeshVertex v = vertices[edgeIndex(k)];
            v.pos.y = 1.0f;
            vertices.push_back(v);
        }
        for (int k = 0; k < GridN; ++k)
        {
            const uint32_t a = edgeIndex(k), b = edgeIndex(k + 1), c = first + uint32_t(k), d = c + 1;
            indices.insert(indices.end(), {a, b, c, b, d, c, a, c, b, b, c, d});
        }
    };
    skirt([](int k) { return uint32_t(k); });                               // north
    skirt([](int k) { return uint32_t(GridN * (GridN + 1) + k); });         // south
    skirt([](int k) { return uint32_t(k * (GridN + 1)); });                 // west
    skirt([](int k) { return uint32_t(k * (GridN + 1) + GridN); });         // east
    _grid = wgr_mesh_create(_renderer, WgrSlice<WgrMeshVertex>(vertices), WgrSlice<uint32_t>(indices));
    _gridIndices = uint32_t(indices.size());
    wgr_earth_heights(_renderer, _heights->Atlas(), _heights->Lookup());
    LOG_INFO(Graphics, "Earth ground from the elevation pyramid: on (POSEIDON_EARTH_HEIGHT_PYRAMID=0 for the patch meshes)");
}

EarthGroundNodes::~EarthGroundNodes()
{
    if (_renderer)
    {
        wgr_earth_heights(_renderer, 0, 0);
        if (_grid)
            wgr_mesh_destroy(_renderer, _grid);
    }
}

const EarthGroundNodes::Row& EarthGroundNodes::RowOf(int z, int y)
{
    auto& rows = _rows[z];
    for (const auto& r : rows)
        if (r.y == y)
            return r;
    const double latitude = NodeLatitude(z, y + 0.5);
    const double p = latitude * Pi / 180.0;
    return rows.push_back({y, latitude, std::sin(p), std::cos(p), Circumference * std::cos(latitude * Pi / 180.0) / double(1 << z)}),
           rows.back();
}

const EarthGroundNodes::Column& EarthGroundNodes::ColumnOf(int z, int x)
{
    auto& columns = _columns[z];
    for (const auto& c : columns)
        if (c.x == x)
            return c;
    const double longitude = (x + 0.5) / double(1 << z) * 360.0 - 180.0;
    const double dl = (longitude - _originLon) * Pi / 180.0;
    return columns.push_back({x, std::sin(dl), std::cos(dl)}), columns.back();
}

void EarthGroundNodes::Select(int z, int x, int y, std::vector<Node>& out)
{
    // Earth::ToLocal of the node's middle, from the frame's row and column tables
    const Row& row = RowOf(z, y);
    const Column& column = ColumnOf(z, x);
    if (std::abs(row.latitude) > 90.0 || _sinP0 * row.sinP + _cosP0 * row.cosP * column.cosDl < 0.0)
        return;
    const double cx = Earth::Anchor + Earth::Radius * (row.cosP * column.sinDl);
    const double cz = Earth::Anchor + Earth::Radius * (_cosP0 * row.sinP - _sinP0 * row.cosP * column.cosDl);
    const double cy = _groundY - Earth::CurvatureDrop(cx, cz);
    const double width = row.width;
    const double radius = width * 0.75 + width * 0.25; // the tile's half diagonal, and room for its relief
    const double distance = std::max(0.0, std::hypot(std::hypot(cx - _cam[0], cy - _cam[1]), cz - _cam[2]) - radius);
    if (distance > _horizon)
        return;
    if (z < MaxZoom && distance < SplitFactor * width)
    {
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
                Select(z + 1, x * 2 + dx, y * 2 + dy, out);
        return;
    }
    out.push_back({z, x, y, distance, radius});
}

bool EarthGroundNodes::Visible(const View& view, int z, int x, int y, double cx, double cz, double width)
{
    static const bool cull = [] { const char* v = std::getenv("POSEIDON_EARTH_NODE_CULL"); return !(v && v[0] == '0'); }();
    if (!cull || !view.valid || width > 40000.0)
        return true; // the globe-scale nodes: their curvature outruns a box this simple
    // the data tile under it (nodes finer than the data share one): its heights, asked once a frame
    const int shift = std::max(z - EarthHeightCache::MaxZoom, 0);
    const uint64_t key = (uint64_t(z - shift) << 58) | (uint64_t(uint32_t(x >> shift)) << 29) | uint64_t(uint32_t(y >> shift));
    auto [it, fresh] = _ranges.try_emplace(key, -11000.0f, 9000.0f);
    if (fresh)
        _heights->HeightRange(z - shift, x >> shift, y >> shift, it->second.first, it->second.second);
    const float lo = it->second.first, hi = it->second.second;
    // the node as a vertical segment through its middle, its heights less the curvature drop there, widened by its
    // half diagonal (and the drop's change across it)
    const double drop = Earth::CurvatureDrop(cx, cz);
    const double r = 0.75 * width + width * width / (4.0 * Earth::Radius) + 50.0;
    const double rel[2][3] = {{cx - _cam[0], double(lo) - drop - _cam[1], cz - _cam[2]},
                              {cx - _cam[0], double(hi) - drop - _cam[1], cz - _cam[2]}};
    const auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    double f[2], s[2], u[2];
    for (int i = 0; i < 2; ++i)
    {
        f[i] = dot(rel[i], view.forward);
        s[i] = dot(rel[i], view.aside);
        u[i] = dot(rel[i], view.up);
    }
    // the near side, then the four sides (inward normals, normalised), each with a 10% wider view
    if (std::max(f[0], f[1]) < -r)
        return false;
    const double tx = view.tanX * 1.1, ty = view.tanY * 1.1;
    const double nx = 1.0 / std::sqrt(1.0 + tx * tx), ny = 1.0 / std::sqrt(1.0 + ty * ty);
    for (int sign = -1; sign <= 1; sign += 2)
    {
        if (std::max((tx * f[0] + sign * s[0]) * nx, (tx * f[1] + sign * s[1]) * nx) < -r)
            return false;
        if (std::max((ty * f[0] + sign * u[0]) * ny, (ty * f[1] + sign * u[1]) * ny) < -r)
            return false;
    }
    return true;
}

void EarthGroundNodes::Draw(double cameraX, double cameraY, double cameraZ, double groundY, double originLatitude,
                            double originLongitude, double horizon, bool gameplayWindow, const View& view)
{
    if (!Ready())
        return;
    const auto start = std::chrono::steady_clock::now();
    _groundY = std::isfinite(groundY) ? groundY : 0.0;
    _cam[0] = cameraX;
    _cam[1] = cameraY;
    _cam[2] = cameraZ;
    _originLat = originLatitude;
    _originLon = originLongitude;
    _horizon = horizon;
    _sinP0 = std::sin(originLatitude * Pi / 180.0);
    _cosP0 = std::cos(originLatitude * Pi / 180.0);
    for (int z = 0; z <= MaxZoom; ++z)
    {
        _rows[z].clear();
        _columns[z].clear();
    }
    auto& nodes = _nodes;
    nodes.clear();
    for (int y = 0; y < (1 << MinZoom); ++y)
        for (int x = 0; x < (1 << MinZoom); ++x)
            Select(MinZoom, x, y, nodes);
    _lastNodes = nodes.size();
    const auto tSelect = std::chrono::steady_clock::now();

    // the data the drawn vertices will read: per node the zooms its distances ask for (shader3d earth_node_level)
    auto& wanted = _wanted;
    wanted.clear();
    for (const auto& n : nodes)
    {
        // the level its middle reads, and the next finer for the blend (the shader falls back to ancestors, which
        // Want adds): a range per node asked for more tiles than the atlas holds, and the finest never came
        const double latitude = RowOf(n.z, n.y).latitude;
        const int middle = int(std::floor(DataLevel(n.distance + n.radius, latitude)));
        for (int level = std::max(middle, 0); level <= std::min(middle + 1, EarthHeightCache::MaxZoom); ++level)
        {
            if (level <= n.z)
                wanted.push_back({level, n.x >> (n.z - level), n.y >> (n.z - level)});
            else if (level - n.z <= 2)
                for (int j = 0; j < (1 << (level - n.z)); ++j)
                    for (int i = 0; i < (1 << (level - n.z)); ++i)
                        wanted.push_back({level, (n.x << (level - n.z)) + i, (n.y << (level - n.z)) + j});
        }
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    const auto camera = Earth::FromDirection(originLatitude, originLongitude,
                                             {cameraX - Earth::Anchor, Earth::Radius + cameraY, cameraZ - Earth::Anchor});
    const auto tWanted = std::chrono::steady_clock::now();
    _heights->Want(camera.latitude, camera.longitude, wanted);
    const auto tWant = std::chrono::steady_clock::now();
    _heights->Install();
    const auto tInstall = std::chrono::steady_clock::now();

    const double p0 = originLatitude * Pi / 180.0;
    const float sinP0 = float(std::sin(p0));
    std::vector<EngineWgpu::EarthNodeDraw> draws;
    draws.reserve(nodes.size());
    size_t culled = 0;
    _ranges.clear();
    for (const auto& n : nodes)
    {
        {
            // its middle, as Select placed it
            const Row& row = RowOf(n.z, n.y);
            const Column& column = ColumnOf(n.z, n.x);
            const double cx = Earth::Anchor + Earth::Radius * (row.cosP * column.sinDl);
            const double cz = Earth::Anchor + Earth::Radius * (_cosP0 * row.sinP - _sinP0 * row.cosP * column.cosDl);
            if (!Visible(view, n.z, n.x, n.y, cx, cz, row.width))
            {
                ++culled;
                continue; // its data is still asked for above: turning round finds it resident
            }
        }
        const double scale = double(1 << n.z);
        const double yNorth = Pi * (1.0 - 2.0 * n.y / scale);
        const double latNorth = std::atan(std::sinh(yNorth));
        const double lonWest = n.x / scale * 2.0 * Pi - Pi;
        double dl = lonWest - originLongitude * Pi / 180.0;
        dl -= 2.0 * Pi * std::floor((dl + Pi) / (2.0 * Pi));
        const double width = RowOf(n.z, n.y).width;
        // a node is drawn between SplitFactor and twice SplitFactor of its widths: there it turns into its parent
        const double morphEnd = 2.0 * SplitFactor * width * 0.9, morphStart = 2.0 * SplitFactor * width * 0.6;
        EngineWgpu::EarthNodeDraw draw{};
        draw.conform[0] = float(latNorth - p0);
        draw.conform[1] = float(yNorth);
        draw.conform[2] = float(2.0 * Pi / scale);
        draw.conform[3] = float(dl);
        draw.conform[4] = float(2.0 * Pi / scale);
        draw.conform[5] = sinP0;
        draw.conform[6] = float(morphStart);
        draw.conform[7] = float(morphEnd);
        draw.conform[8] = Bits(n.x);
        draw.conform[9] = Bits(n.y);
        draw.conform[10] = gameplayWindow ? 3.5f : 3.0f;
        draw.conform[11] = float(n.z);
        draw.ref19[0] = float(double(n.x) * double(1 << (19 - n.z)));
        draw.ref19[1] = float(double(n.y) * double(1 << (19 - n.z)));
        draw.groundDetail = n.z >= GroundDetailZoom;
        draws.push_back(draw);
    }
    _engine.SubmitEarthNodes(_grid, _gridIndices, draws);
    // what selecting, asking and submitting costs the main thread
    const auto end = std::chrono::steady_clock::now();
    const auto span = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const double ms = span(start, end);
    _statPart[0] += span(start, tSelect);
    _statPart[1] += span(tSelect, tWanted);
    _statPart[2] += span(tWanted, tWant);
    _statPart[3] += span(tWant, tInstall);
    _statPart[4] += span(tInstall, end);
    _statFrames += 1;
    _statMs += ms;
    _statMax = std::max(_statMax, ms);
    if (start - _statSince > std::chrono::seconds(5))
    {
        LOG_INFO(Graphics,
                 "Earth ground nodes: {} selected, {} out of view, {:.3f} ms a frame (max {:.2f}) over {} frames: select {:.3f}, tiles {:.3f}, "
                 "want {:.3f}, install {:.3f}, submit {:.3f}",
                 nodes.size(), culled, _statMs / _statFrames, _statMax, _statFrames, _statPart[0] / _statFrames,
                 _statPart[1] / _statFrames, _statPart[2] / _statFrames, _statPart[3] / _statFrames, _statPart[4] / _statFrames);
        std::fill(std::begin(_statPart), std::end(_statPart), 0.0);
        _statSince = start;
        _statFrames = 0;
        _statMs = _statMax = 0.0;
    }
}
} // namespace Poseidon
