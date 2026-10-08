#include "EarthFeatureStream.hpp"
#include "EarthContentSource.hpp"
#include <cjson/cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <unordered_set>

namespace Poseidon
{
namespace
{
using Features = EarthFeatureStream;
struct Point
{
    double x, z;
};
const char* Text(const cJSON* object, const char* key)
{
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}
bool Is(const cJSON* object, const char* key, const char* value)
{
    return std::string(Text(object, key)) == value;
}
double Number(const cJSON* object, const char* key)
{
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) ? value->valuedouble : NAN;
}
double Metres(const cJSON* tags, const char* key, double fallback, double low, double high)
{
    const char* value = Text(tags, key);
    char* end = nullptr;
    const double n = std::strtod(value, &end);
    while (end && *end == ' ')
        ++end;
    return end && end != value && (!*end || std::string(end) == "m") && std::isfinite(n) && n >= low && n <= high
               ? n
               : fallback;
}
Point Local(double lat, double lon, const Features::Ground& g, Features::Cell cell)
{
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::abs(lat - (cell.latitude + .5) * .02) > .06 ||
        std::abs(Earth::WrapLongitude(lon - (cell.longitude + .5) * .02)) > .06)
        throw std::out_of_range("feature outside cell margin");
    const auto local = Earth::ToLocal(g.latitude, g.longitude, lat, lon);
    if (!local.visible) throw std::out_of_range("feature behind horizon");
    return {local.x, local.z};
}
float Height(const Features::Ground& g, Point p)
{
    const double x = (p.x - g.x) / g.grid, z = (p.z - g.z) / g.grid;
    if (x < 0 || z < 0 || x > 256 || z > 256)
        throw std::out_of_range("outside resident ground");
    const int ix = std::min(255, int(x)), iz = std::min(255, int(z));
    const float a = g.heights[iz * 257 + ix], b = g.heights[iz * 257 + ix + 1], c = g.heights[(iz + 1) * 257 + ix],
                d = g.heights[(iz + 1) * 257 + ix + 1];
    const double fx = x - ix, fz = z - iz;
    // Resident terrain indices are {a,c,b,b,c,d}; interpolate its actual
    // triangles rather than the bilinear surface between four heights.
    const float y = float(fx + fz <= 1 ? a + (b - a) * fx + (c - a) * fz : d + (c - d) * (1 - fx) + (b - d) * (1 - fz));
    if (!std::isfinite(y))
        throw std::out_of_range("invalid ground");
    return y;
}
size_t Triangles(const Features::Mesh& m)
{
    size_t n = 0;
    for (const auto& b : m.batches)
        n += b.indices.size() / 3;
    return n;
}
void Triangle(Features::Batch& b, Features::Vertex a, Features::Vertex c, Features::Vertex d)
{
    const float ux = c.x - a.x, uy = c.y - a.y, uz = c.z - a.z, vx = d.x - a.x, vy = d.y - a.y, vz = d.z - a.z;
    const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx,
                l = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (l < 1e-6f)
        return;
    const auto base = uint32_t(b.vertices.size());
    for (auto v : {a, c, d})
    {
        v.nx = nx / l;
        v.ny = ny / l;
        v.nz = nz / l;
        b.vertices.push_back(v);
    }
    b.indices.insert(b.indices.end(), {base, base + 1, base + 2});
}
Features::Vertex V(Point p, float y)
{
    return {float(p.x), y, float(p.z), 0, 1, 0};
}
void Ribbon(Features::Mesh& result, const Features::Ground& g, const std::vector<Point>& p, Features::Kind kind,
            double width)
{
    auto& batch = result.batches[kind];
    bool emitted = false;
    for (size_t i = 1; i < p.size(); ++i)
    {
        const auto a = p[i - 1], b = p[i];
        const double dx = b.x - a.x, dz = b.z - a.z, length = std::hypot(dx, dz);
        if (length < .1 || length > 1500)
            continue;
        const double ox = -dz / length * width * .5, oz = dx / length * width * .5;
        const int steps = std::max(1, int(std::ceil(length / 8))); // Follow resident height grid between OSM nodes.
        for (int j = 0; j < steps; ++j)
        {
            if (Triangles(result) + 2 > Features::MaxTriangles)
                return;
            const double t0 = double(j) / steps, t1 = double(j + 1) / steps;
            const Point l0{a.x + dx * t0 + ox, a.z + dz * t0 + oz}, r0{a.x + dx * t0 - ox, a.z + dz * t0 - oz},
                l1{a.x + dx * t1 + ox, a.z + dz * t1 + oz}, r1{a.x + dx * t1 - ox, a.z + dz * t1 - oz};
            try
            {
                const float lift = kind == Features::InlandWater ? .12f : .08f;
                const auto vl0 = V(l0, Height(g, l0) + lift), vr0 = V(r0, Height(g, r0) + lift),
                           vl1 = V(l1, Height(g, l1) + lift), vr1 = V(r1, Height(g, r1) + lift);
                Triangle(batch, vl0, vl1, vr0);
                Triangle(batch, vr0, vl1, vr1);
                emitted = true;
            }
            catch (const std::out_of_range&)
            {
            } // A long way can cross the resident patch.
        }
    }
    if (emitted)
        ++batch.features;
}
double Cross(Point a, Point b, Point c)
{
    return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}
bool Inside(Point p, const std::vector<Point>& polygon)
{
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
    {
        const auto a = polygon[i], b = polygon[j];
        if ((a.z > p.z) != (b.z > p.z) && p.x < (b.x - a.x) * (p.z - a.z) / (b.z - a.z) + a.x)
            inside = !inside;
    }
    return inside;
}
void Tree(Features::Mesh& result, const Features::Ground& g, Point p, double h)
{
    auto& batch = result.batches[Features::Trees];
    if (batch.features >= Features::MaxTrees || Triangles(result) + 8 > Features::MaxTriangles)
        return;
    const float y = Height(g, p), radius = float(h * .26);
    const auto top = V(p, y + float(h)), bottom = V(p, y + float(h * .12));
    const Point ring[4] = {{p.x - radius, p.z}, {p.x, p.z + radius}, {p.x + radius, p.z}, {p.x, p.z - radius}};
    for (int i = 0; i < 4; ++i)
    {
        Triangle(batch, top, V(ring[i], y + float(h * .35)), V(ring[(i + 1) % 4], y + float(h * .35)));
        Triangle(batch, bottom, V(ring[(i + 1) % 4], y + float(h * .35)), V(ring[i], y + float(h * .35)));
    }
    ++batch.features;
}
void Sign(Features::Mesh& result, const Features::Ground& g, Point p)
{
    auto& b = result.batches[Features::Signs];
    if (Triangles(result) + 16 > Features::MaxTriangles)
        return;
    const float y = Height(g, p);
    for (const auto dimensions :
         std::array<std::array<float, 4>, 2>{{{.045f, 0, 2.2f, .045f}, {.35f, 1.55f, 2.2f, .045f}}})
    {
        const float w = dimensions[0], low = y + dimensions[1], high = y + dimensions[2], d = dimensions[3];
        const Point corners[4] = {{p.x - w, p.z - d}, {p.x + w, p.z - d}, {p.x + w, p.z + d}, {p.x - w, p.z + d}};
        for (int i = 0; i < 4; ++i)
        {
            const auto a = corners[i], c = corners[(i + 1) % 4];
            Triangle(b, V(a, low), V(a, high), V(c, low));
            Triangle(b, V(c, low), V(a, high), V(c, high));
        }
    }
    ++b.features;
}
void Water(Features::Mesh& result, const Features::Ground& g, std::vector<Point> p)
{
    // Simple closed way only: reject self-intersections rather than fill holes,
    // multipolygons or unknown connectivity incorrectly.
    if (p.size() < 4 || p.size() > 256 || std::hypot(p.front().x - p.back().x, p.front().z - p.back().z) > .01)
        return;
    p.pop_back();
    double area = 0;
    for (size_t i = 0; i < p.size(); ++i)
        area += p[i].x * p[(i + 1) % p.size()].z - p[(i + 1) % p.size()].x * p[i].z;
    if (std::abs(area) < 2 || std::abs(area) > 500000)
        return;
    if (area < 0)
        std::reverse(p.begin(), p.end());
    for (size_t i = 0; i < p.size(); ++i)
        for (size_t j = i + 1; j < p.size(); ++j)
        {
            if (j == i + 1 || (i == 0 && j == p.size() - 1))
                continue;
            const auto a = p[i], b = p[(i + 1) % p.size()], c = p[j], d = p[(j + 1) % p.size()];
            if (Cross(a, b, c) * Cross(a, b, d) <= 0 && Cross(c, d, a) * Cross(c, d, b) <= 0)
                return;
        }
    float low = INFINITY, high = -INFINITY;
    for (auto v : p)
    {
        const float y = Height(g, v);
        low = std::min(low, y);
        high = std::max(high, y);
    }
    if (high - low > 8)
        return; // No hydrological flattening in this prototype.
    const float y = low + .16f;
    std::vector<uint32_t> active(p.size()), indices;
    std::iota(active.begin(), active.end(), 0);
    while (active.size() > 3)
    {
        bool cut = false;
        for (size_t i = 0; i < active.size(); ++i)
        {
            const auto a = active[(i + active.size() - 1) % active.size()], b = active[i],
                       c = active[(i + 1) % active.size()];
            if (Cross(p[a], p[b], p[c]) <= 1e-7)
                continue;
            bool contains = false;
            for (auto v : active)
                if (v != a && v != b && v != c && Cross(p[a], p[b], p[v]) >= 0 && Cross(p[b], p[c], p[v]) >= 0 &&
                    Cross(p[c], p[a], p[v]) >= 0)
                {
                    contains = true;
                    break;
                }
            if (contains)
                continue;
            indices.insert(indices.end(), {a, c, b});
            active.erase(active.begin() + i);
            cut = true;
            break;
        }
        if (!cut)
            return;
    }
    indices.insert(indices.end(), {active[0], active[2], active[1]});
    if (Triangles(result) + indices.size() / 3 > Features::MaxTriangles)
        return;
    auto& batch = result.batches[Features::InlandWater];
    for (size_t i = 0; i < indices.size(); i += 3)
        Triangle(batch, V(p[indices[i]], y), V(p[indices[i + 1]], y), V(p[indices[i + 2]], y));
    ++batch.features;
}
void Append(Features::Mesh& result, const std::string& json, const Features::Ground& g, Features::Cell cell,
            std::unordered_set<std::string>& ids, const std::function<bool()>& cancelled)
{
    if (json.size() > Features::MaxBytes || g.heights.size() != 257 * 257 || !std::isfinite(g.grid) || g.grid <= 0 ||
        !std::isfinite(g.latitude) || !std::isfinite(g.longitude) || std::abs(g.latitude) > 84)
    {
        result.error = "invalid bounded feature cell/ground";
        return;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_ParseWithLength(json.data(), json.size()), cJSON_Delete);
    const auto* elements = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "elements") : nullptr;
    if (!cJSON_IsArray(elements))
    {
        result.error = "invalid feature elements";
        return;
    }
    size_t visited = 0;
    const cJSON* element = nullptr;
    cJSON_ArrayForEach(element, elements)
    {
        if (++visited > Features::MaxElements || Triangles(result) >= Features::MaxTriangles)
            break;
        if (cancelled && cancelled())
        {
            result.error = "request cancelled";
            return;
        }
        const auto* tags = cJSON_GetObjectItemCaseSensitive(element, "tags");
        const std::string type = Text(element, "type");
        const double id = Number(element, "id");
        std::string key;
        if (std::isfinite(id) && id > 0 && id <= 9007199254740991.0 && std::floor(id) == id)
            key = type + std::to_string(uint64_t(id));
        if (!key.empty() && !ids.insert(key).second)
        {
            ++result.skipped;
            continue;
        }
        try
        {
            if (type == "node")
            {
                const auto p = Local(Number(element, "lat"), Number(element, "lon"), g, cell);
                if (Is(tags, "natural", "tree"))
                    Tree(result, g, p, Metres(tags, "height", 9, 2, 40));
                else if (*Text(tags, "traffic_sign") || Is(tags, "highway", "stop") || Is(tags, "highway", "give_way"))
                    Sign(result, g, p);
                else
                    ++result.skipped;
                continue;
            }
            const auto* geometry = cJSON_GetObjectItemCaseSensitive(element, "geometry");
            const int count = cJSON_GetArraySize(geometry);
            if (type != "way" || count < 2 || count > int(Features::MaxPoints))
            {
                ++result.skipped;
                continue;
            }
            std::vector<Point> p;
            for (int i = 0; i < count; ++i)
            {
                const auto* node = cJSON_GetArrayItem(geometry, i);
                p.push_back(Local(Number(node, "lat"), Number(node, "lon"), g, cell));
            }
            if (*Text(tags, "highway"))
            {
                const std::string highway = Text(tags, "highway"), surface = Text(tags, "surface");
                if (Is(tags, "area", "yes") || (*Text(tags, "bridge") && !Is(tags, "bridge", "no")) ||
                    (*Text(tags, "tunnel") && !Is(tags, "tunnel", "no")) || highway == "construction" ||
                    highway == "proposed" || highway == "steps")
                {
                    ++result.skipped;
                    continue;
                }
                const bool path = highway == "path" || highway == "footway" || highway == "cycleway" ||
                                  highway == "track" || highway == "bridleway";
                const bool dirt = path || surface == "unpaved" || surface == "gravel" || surface == "dirt" ||
                                  surface == "ground" || surface == "sand";
                const double fallback = path                    ? 2.5
                                        : highway == "motorway" ? 12
                                        : highway == "primary"  ? 9
                                        : highway == "service"  ? 4
                                                                : 6;
                Ribbon(result, g, p, dirt ? Features::UnpavedRoad : Features::PavedRoad,
                       Metres(tags, "width", fallback, 1, 30));
            }
            else if (Is(tags, "waterway", "river") || Is(tags, "waterway", "stream") || Is(tags, "waterway", "canal") ||
                     Is(tags, "waterway", "drain") || Is(tags, "waterway", "ditch"))
                Ribbon(result, g, p, Features::InlandWater,
                       Metres(tags, "width", Is(tags, "waterway", "river") ? 8 : 2, 1, 80));
            else if (Is(tags, "natural", "water") || Is(tags, "landuse", "reservoir") ||
                     Is(tags, "waterway", "riverbank"))
                Water(result, g, p);
            else if (Is(tags, "landuse", "forest") || Is(tags, "natural", "wood"))
            {
                if (p.size() < 4 || p.size() > 256 ||
                    std::hypot(p.front().x - p.back().x, p.front().z - p.back().z) > .01)
                {
                    ++result.skipped;
                    continue;
                }
                p.pop_back();
                double xmin = INFINITY, zmin = INFINITY, xmax = -INFINITY, zmax = -INFINITY;
                for (auto v : p)
                {
                    xmin = std::min(xmin, v.x);
                    xmax = std::max(xmax, v.x);
                    zmin = std::min(zmin, v.z);
                    zmax = std::max(zmax, v.z);
                }
                xmin = std::max(xmin, double(g.x) + 4);
                xmax = std::min(xmax, double(g.x + g.grid * 256) - 4);
                zmin = std::max(zmin, double(g.z) + 4);
                zmax = std::min(zmax, double(g.z + g.grid * 256) - 4);
                // Fixed origin-relative spacing and no PRNG: repeated builds match.
                for (double z = std::ceil(zmin / 32) * 32;
                     z <= zmax && result.forestSamples < 16000 &&
                     result.batches[Features::Trees].features < Features::MaxTrees;
                     z += 32)
                    for (double x = std::ceil(xmin / 32) * 32;
                         x <= xmax && result.forestSamples < 16000 &&
                         result.batches[Features::Trees].features < Features::MaxTrees;
                         x += 32)
                    {
                        ++result.forestSamples;
                        if (Inside({x, z}, p))
                            Tree(result, g, {x, z}, 9);
                    }
            }
            else
                ++result.skipped;
        }
        catch (const std::out_of_range&)
        {
            ++result.skipped;
        }
    }
}
} // namespace
EarthFeatureStream::Mesh EarthFeatureStream::BuildCell(const std::string& json, const Ground& ground, Cell cell)
{
    Mesh result;
    result.cell = cell;
    result.revision = ground.revision;
    result.generation = ground.generation;
    std::unordered_set<std::string> ids;
    Append(result, json, ground, cell, ids, {});
    return result;
}
EarthFeatureStream::Mesh EarthFeatureStream::BuildNeighborhood(const std::filesystem::path& cache, const Ground& ground,
                                                               Cell centre, const std::function<bool()>& cancelled)
{
    Mesh result;
    result.cell = centre;
    result.revision = ground.revision;
    result.generation = ground.generation;
    std::unordered_set<std::string> ids;
    constexpr int offsets[9][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    bool found = false;
    for (const auto& offset : offsets)
    {
        if (cancelled && cancelled())
        {
            result.error = "request cancelled";
            return result;
        }
        const Cell cell{centre.latitude + offset[0], ((centre.longitude + offset[1] + 27000) % 18000) - 9000};
        if (std::abs((cell.latitude + .5) * .02) > 84)
            continue;
        auto path = cache / "features" / EarthBuildingStream::FileName(cell);
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error))
            path = EarthContentSource::StreamPath(cache, "features", cell.latitude, cell.longitude);
        const auto bytes = std::filesystem::file_size(path, error);
        if (error)
            continue;
        found = true;
        if (bytes > MaxBytes)
        {
            result.error = "feature cell exceeds 4MiB";
            continue;
        }
        std::string json(size_t(bytes), '\0');
        std::ifstream input(path, std::ios::binary);
        if (!input.read(json.data(), std::streamsize(bytes)))
        {
            result.error = "feature cell read failed";
            continue;
        }
        Append(result, json, ground, cell, ids, cancelled);
        if (result.error == "request cancelled" || Triangles(result) >= MaxTriangles)
            break;
    }
    if (!found)
        result.error = "no cached feature cells in 3x3 window";
    return result;
}
EarthFeatureStream::EarthFeatureStream(std::filesystem::path cache) : _cache(std::move(cache))
{
    _worker = std::thread(&EarthFeatureStream::Run, this);
}
EarthFeatureStream::~EarthFeatureStream()
{
    {
        std::lock_guard lock(_mutex);
        _stop = true;
        ++_request;
    }
    _wake.notify_one();
    if (_worker.joinable())
        _worker.join();
}
void EarthFeatureStream::Relocate()
{
    std::lock_guard lock(_mutex);
    ++_request;
    _pending = false;
    _requested = false;
    _ready.reset();
}
void EarthFeatureStream::Request(const EarthTerrainStream::Patch& p, float x, float z)
{
    if (!std::isfinite(x) || !std::isfinite(z))
        return;
    const auto coordinate = Earth::FromLocal(p.originLatitude, p.originLongitude, x, z);
    if (!std::isfinite(coordinate.latitude) || std::abs(coordinate.latitude) > 84)
        return;
    const auto cell = EarthBuildingStream::CellAt(coordinate.latitude, coordinate.longitude);
    std::lock_guard lock(_mutex);
    if (_requested && cell == _cell && p.generation == _ground.generation && p.originRevision == _ground.revision)
        return;
    _cell = cell;
    _ground = {p.x, p.z, p.gridMetres, p.originLatitude, p.originLongitude, p.originRevision, p.generation, p.heights};
    ++_request;
    _requested = true;
    _pending = true;
    _ready.reset();
    _wake.notify_one();
}
std::unique_ptr<EarthFeatureStream::Mesh> EarthFeatureStream::TakeReady()
{
    std::lock_guard lock(_mutex);
    return std::move(_ready);
}
void EarthFeatureStream::Run()
{
    EarthContentSource source;
    for (;;)
    {
        Ground ground;
        Cell cell;
        uint64_t request;
        {
            std::unique_lock lock(_mutex);
            _wake.wait(lock, [&] { return _stop || _pending; });
            if (_stop)
                return;
            ground = _ground;
            cell = _cell;
            request = _request;
            _pending = false;
        }
        const auto cancelled = [&] { std::lock_guard lock(_mutex); return _stop || request != _request; };
        auto mesh = std::make_unique<Mesh>(BuildNeighborhood(_cache, ground, cell, cancelled));
        if (source.Enabled())
        {
            // Present resident cached neighbors immediately while new cells
            // transfer. The render thread never waits for network readiness.
            { std::lock_guard lock(_mutex); if (!_stop && request == _request) _ready = std::move(mesh); }
            if (!source.EnsureNeighborhood(_cache, "features", cell.latitude, cell.longitude, cancelled)) continue;
            mesh = std::make_unique<Mesh>(BuildNeighborhood(_cache, ground, cell, cancelled));
        }
        {
            std::lock_guard lock(_mutex);
            if (!_stop && request == _request)
                _ready = std::move(mesh);
        }
    }
}
} // namespace Poseidon
