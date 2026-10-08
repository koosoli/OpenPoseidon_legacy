#include "WgpuRenderer/EarthFeatureStream.hpp"
#include "WgpuRenderer/EarthContentSource.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
using Features = Poseidon::EarthFeatureStream;
using Buildings = Poseidon::EarthBuildingStream;
using Point = std::pair<double, double>;
std::string Way(uint64_t id, const char* tags, const std::vector<Point>& points, double latitude = 0,
                double longitude = 0)
{
    std::ostringstream out;
    out << std::setprecision(12) << "{\"type\":\"way\",\"id\":" << id << ",\"tags\":{" << tags << "},\"geometry\":[";
    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto c = Poseidon::Earth::FromLocal(latitude, longitude, points[i].first, points[i].second);
        if (i)
            out << ",";
        out << "{\"lat\":" << c.latitude << ",\"lon\":" << c.longitude << "}";
    }
    return out.str() + "]}";
}
std::string Json(const std::string& elements)
{
    return "{\"elements\":[" + elements + "]}";
}
void Valid(const Features::Mesh& mesh)
{
    size_t triangles = 0;
    for (const auto& b : mesh.batches)
    {
        triangles += b.indices.size() / 3;
        for (auto v : b.vertices)
        {
            assert(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z));
            assert(std::abs(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz - 1) < .0001f);
            assert(std::isfinite(v.u) && std::isfinite(v.v) && v.u >= 0 && v.u <= 1 && v.v >= 0 && v.v <= 1);
        }
        for (size_t i = 0; i < b.indices.size(); i += 3)
        {
            assert(b.indices[i] < b.vertices.size() && b.indices[i + 1] < b.vertices.size() &&
                   b.indices[i + 2] < b.vertices.size());
            const auto a = b.vertices[b.indices[i]], c = b.vertices[b.indices[i + 1]], d = b.vertices[b.indices[i + 2]];
            const float ux = c.x - a.x, uy = c.y - a.y, uz = c.z - a.z, vx = d.x - a.x, vy = d.y - a.y, vz = d.z - a.z;
            for (auto v : {a, c, d})
                assert((uy * vz - uz * vy) * v.nx + (uz * vx - ux * vz) * v.ny + (ux * vy - uy * vx) * v.nz > 0);
        }
    }
    assert(triangles <= Features::MaxTriangles && mesh.batches[Features::Trees].features <= Features::MaxTrees &&
           mesh.forestSamples <= 16000);
}
void TreeGeometry()
{
    namespace Trees = Poseidon::EarthTrees;
    assert(Trees::TriangleCount == 28 && Trees::VertexCount == 84);
    const auto seed = Trees::GeographicSeed(49.61, 6.13);
    const auto local = Poseidon::Earth::ToLocal(49.60, 6.12, 49.61, 6.13);
    const auto geo = Poseidon::Earth::FromLocal(49.60, 6.12, local.x, local.z);
    assert(Trees::GeographicSeed(geo.latitude, geo.longitude) == seed);
    assert(Trees::GeographicSeed(0, 180) == Trees::GeographicSeed(0, -180));
    bool varies = false;
    for (double x : {-500000.0, 6400.0, 500000.0})
        for (float ground : {-20000.f, 0.f, 300.f})
            for (double height : {2.0, 9.0, 40.0})
                for (uint64_t n = 0; n < 32; ++n)
                {
                    const auto tree = Trees::Build(x, ground, -x, height, seed + n);
                    const auto again = Trees::Build(x, ground, -x, height, seed + n);
                    Features::Mesh mesh{};
                    auto& batch = mesh.batches[Features::Trees];
                    batch.vertices.assign(tree.begin(), tree.end());
                    for (uint32_t i = 0; i < tree.size(); ++i)
                        batch.indices.push_back(i);
                    batch.features = 1;
                    Valid(mesh);
                    float low = INFINITY, high = -INFINITY;
                    for (size_t i = 0; i < tree.size(); ++i)
                    {
                        const auto a = tree[i], b = again[i];
                        assert(a.x == b.x && a.y == b.y && a.z == b.z && a.nx == b.nx && a.ny == b.ny && a.nz == b.nz &&
                               a.u == b.u);
                        low = std::min(low, a.y);
                        high = std::max(high, a.y);
                        assert(a.v == .5f);
                        assert(i < 24 ? a.u == .125f : (a.u == .375f || a.u == .625f || a.u == .875f));
                        if (i >= 24)
                            for (size_t j = 24; j < i; ++j)
                                if (a.x == tree[j].x && a.y == tree[j].y && a.z == tree[j].z)
                                    assert(a.nx == tree[j].nx && a.ny == tree[j].ny && a.nz == tree[j].nz);
                    }
                    assert(low == ground && high == ground + float(height));
                    // Crown triangles interpolate their shared smooth normals; trunk sides stay flat.
                    assert(tree[24].ny != tree[25].ny);
                    for (size_t i = 0; i < tree.size(); i += 3)
                        assert(tree[i].u == tree[i + 1].u && tree[i].u == tree[i + 2].u);
                    const auto varied = Trees::Build(x, ground, -x, height, seed + n, true);
                    float variedHigh = ground;
                    for (auto v : varied)
                        variedHigh = std::max(variedHigh, v.y);
                    assert(variedHigh >= ground + float(height * .82) - .002f &&
                           variedHigh <= ground + float(height * 1.18) + .002f);
                    varies |= variedHigh != high;
                }
    assert(varies);
    for (int band = 0; band < 4; ++band)
        for (int pixel = 1; pixel < 4; ++pixel)
            for (int channel = 0; channel < 4; ++channel)
                assert(Trees::PalettePixels[(band * 4 + pixel) * 4 + channel] ==
                       Trees::PalettePixels[band * 16 + channel]);
}
void ForestRebase()
{
    namespace Earth = Poseidon::Earth;
    const std::vector<Point> polygon{{6320, 6320}, {6480, 6320}, {6480, 6480}, {6320, 6480}, {6320, 6320}};
    struct Sample
    {
        Earth::Coordinate centre;
        float height, leaf;
    };
    const auto samples = [](const Features::Mesh& mesh, const Features::Ground& ground)
    {
        std::vector<Sample> result;
        const auto& vertices = mesh.batches[Features::Trees].vertices;
        for (size_t i = 0; i < vertices.size(); i += Poseidon::EarthTrees::VertexCount)
        {
            // Each of the four trunk base corners appears three times.
            double x = 0, z = 0;
            int count = 0;
            float high = 300;
            for (size_t j = 0; j < Poseidon::EarthTrees::VertexCount; ++j)
            {
                const auto v = vertices[i + j];
                high = std::max(high, v.y);
                if (v.y == 300)
                {
                    x += v.x;
                    z += v.z;
                    ++count;
                }
            }
            assert(count == 12);
            result.push_back({Earth::FromLocal(ground.latitude, ground.longitude, x / count, z / count), high - 300,
                              vertices[i + 24].u});
        }
        return result;
    };
    for (Earth::Coordinate source : {Earth::Coordinate{0, 0}, {49.61, 6.13}, {83.5, 179.999}})
    {
        const auto json = Json(Way(70, "\"landuse\":\"forest\"", polygon, source.latitude, source.longitude));
        const Features::Ground initial{
            6272, 6272, 1, source.latitude, source.longitude, 1, 1, std::vector<float>(257 * 257, 300)};
        const auto cell = Buildings::CellAt(source.latitude, source.longitude);
        const auto mesh = Features::BuildCell(json, initial, cell);
        Valid(mesh);
        const auto reference = samples(mesh, initial);
        assert(reference.size() >= 16 && reference.size() <= 36); // Approx. one sample per 32x32 metres.
        const auto neighborMesh = Features::BuildCell(json, initial, {cell.latitude, cell.longitude + 1});
        const auto neighborSamples = samples(neighborMesh, initial);
        assert(neighborSamples.size() == reference.size());
        for (size_t i = 0; i < reference.size(); ++i)
            assert(neighborSamples[i].centre.latitude == reference[i].centre.latitude &&
                   neighborSamples[i].centre.longitude == reference[i].centre.longitude);
        auto remote = initial;
        remote.x = remote.z = 100000;
        const auto absent = Features::BuildCell(json, remote, cell);
        assert(absent.forestSamples == 0 && absent.batches[Features::Trees].features == 0);
        for (Earth::Coordinate origin : {Earth::Coordinate{source.latitude - .3, source.longitude - .2},
                                         {source.latitude - 3, source.longitude - 2}})
        {
            const auto centre = Earth::ToLocal(origin.latitude, origin.longitude, source.latitude, source.longitude);
            auto moved = initial;
            moved.latitude = origin.latitude;
            moved.longitude = origin.longitude;
            moved.x = float(centre.x - 128);
            moved.z = float(centre.z - 128);
            const auto rebased = Features::BuildCell(json, moved, cell);
            Valid(rebased);
            const auto actual = samples(rebased, moved);
            assert(actual.size() == reference.size());
            for (size_t i = 0; i < actual.size(); ++i)
            {
                const auto delta = Earth::ToLocal(reference[i].centre.latitude, reference[i].centre.longitude,
                                                  actual[i].centre.latitude, actual[i].centre.longitude);
                assert(std::hypot(delta.x - Earth::Anchor, delta.z - Earth::Anchor) <
                       .05); // Float mesh quantization only.
                assert(actual[i].height == reference[i].height && actual[i].leaf == reference[i].leaf);
            }
        }
        auto reversed = polygon;
        std::reverse(reversed.begin(), reversed.end());
        const auto reverseMesh = Features::BuildCell(
            Json(Way(70, "\"landuse\":\"forest\"", reversed, source.latitude, source.longitude)), initial, cell);
        assert(reverseMesh.batches[Features::Trees].indices == mesh.batches[Features::Trees].indices);
        const auto reversedSamples = samples(reverseMesh, initial);
        assert(reversedSamples.size() == reference.size());
        for (size_t i = 0; i < reference.size(); ++i)
            assert(reversedSamples[i].centre.latitude == reference[i].centre.latitude &&
                   reversedSamples[i].centre.longitude == reference[i].centre.longitude);
    }
}
int main(int argc, char** argv)
{
    TreeGeometry();
    ForestRebase();
    Features::Ground g{6272, 6272, 1, 0, 0, 5, 7, std::vector<float>(257 * 257, 10)};
    const auto cell = Buildings::CellAt(0, 0);
    const auto road = Way(42, "\"highway\":\"residential\",\"width\":\"6\"", {{6350, 6400}, {6450, 6400}});
    auto mesh = Features::BuildCell(Json(road), g, cell);
    assert(mesh.error.empty() && mesh.batches[Features::PavedRoad].features == 1);
    assert(mesh.batches[Features::PavedRoad].indices.size() == 26 * 3);
    Valid(mesh);
    for (auto v : mesh.batches[Features::PavedRoad].vertices)
        assert(std::abs(v.y - 10.08) < .001);
    assert(Features::BuildCell(
               Json(Way(43, "\"highway\":\"primary\",\"bridge\":\"yes\"", {{6350, 6400}, {6450, 6400}})), g, cell)
               .batches[Features::PavedRoad]
               .features == 0);
    mesh = Features::BuildCell(Json(Way(44, "\"highway\":\"track\"", {{6200, 6400}, {6600, 6400}})), g, cell);
    assert(mesh.batches[Features::UnpavedRoad].features == 1);
    Valid(mesh);
    const std::vector<Point> polygon{{6320, 6320}, {6480, 6320}, {6480, 6480}, {6320, 6480}, {6320, 6320}};
    const auto forest = Way(45, "\"landuse\":\"forest\"", polygon);
    mesh = Features::BuildCell(Json(forest), g, cell);
    assert(mesh.batches[Features::Trees].features > 10);
    Valid(mesh);
    const auto again = Features::BuildCell(Json(forest), g, cell);
    assert(again.batches[Features::Trees].indices == mesh.batches[Features::Trees].indices);
    for (size_t i = 0; i < mesh.batches[Features::Trees].vertices.size(); ++i)
    {
        const auto a = mesh.batches[Features::Trees].vertices[i], b = again.batches[Features::Trees].vertices[i];
        assert(a.x == b.x && a.y == b.y && a.z == b.z && a.nx == b.nx && a.ny == b.ny && a.nz == b.nz);
    }
    mesh = Features::BuildCell(Json(Way(46, "\"natural\":\"water\"", polygon)), g, cell);
    assert(mesh.batches[Features::InlandWater].features == 1 &&
           mesh.batches[Features::InlandWater].indices.size() == 6);
    Valid(mesh);
    mesh = Features::BuildCell(Json(Way(48, "\"waterway\":\"dam\"", {{6350, 6400}, {6450, 6400}})), g, cell);
    assert(mesh.batches[Features::InlandWater].features == 0);
    mesh = Features::BuildCell(Json(Way(49, "\"waterway\":\"riverbank\"", polygon)), g, cell);
    assert(mesh.batches[Features::InlandWater].features == 1 &&
           mesh.batches[Features::InlandWater].indices.size() == 6);
    auto dateline = g;
    dateline.longitude = 179.999;
    mesh =
        Features::BuildCell(R"({"elements":[{"type":"node","id":42,"lat":0,"lon":179.999,"tags":{"natural":"tree"}}]})",
                            dateline, {0, 8999});
    assert(mesh.batches[Features::Trees].features == 1);
    Valid(mesh);
    mesh = Features::BuildCell(
        R"({"elements":[{"type":"node","id":42,"lat":0,"lon":0,"tags":{"natural":"tree"}},{"type":"node","id":43,"lat":0,"lon":0,"tags":{"highway":"stop"}}]})",
        g, cell);
    assert(mesh.batches[Features::Trees].features == 1 && mesh.batches[Features::Signs].features == 1);
    Valid(mesh);
    assert(!Features::BuildCell("{}", g, cell).error.empty());
    assert(!Features::BuildCell(std::string(Features::MaxBytes + 1, ' '), g, cell).error.empty());
    auto bad = g;
    bad.grid = NAN;
    assert(!Features::BuildCell(Json(road), bad, cell).error.empty());
    auto slope = g;
    for (int z = 0; z < 257; ++z)
        for (int x = 0; x < 257; ++x)
            slope.heights[z * 257 + x] = 10 + x * .1f;
    mesh = Features::BuildCell(Json(road), slope, cell);
    Valid(mesh);
    for (auto v : mesh.batches[Features::PavedRoad].vertices)
        assert(std::abs(v.y - (10 + (v.x - g.x) * .1f + .08f)) < .001);
    // A non-planar quad distinguishes rendered triangles from bilinear height.
    auto saddle = g;
    saddle.x = 6400;
    saddle.z = 6400;
    saddle.heights.assign(257 * 257, 0);
    saddle.heights[258] = 100;
    for (double offset : {.25, .75})
    {
        const auto c = Poseidon::Earth::FromLocal(0, 0, 6400 + offset, 6400 + offset);
        std::ostringstream node;
        node << std::setprecision(15) << "{\"type\":\"node\",\"lat\":" << c.latitude << ",\"lon\":" << c.longitude
             << ",\"tags\":{\"natural\":\"tree\"}}";
        mesh = Features::BuildCell(Json(node.str()), saddle, cell);
        float low = INFINITY;
        for (auto v : mesh.batches[Features::Trees].vertices)
            low = std::min(low, v.y);
        assert(std::abs(low - (offset == .25 ? 0.f : 50.f)) < .001f);
    }
    const Features::Ground wide{3200, 3200, 25, 0, 0, 1, 1, std::vector<float>(257 * 257, 10)};
    mesh = Features::BuildCell(
        Json(Way(47, "\"natural\":\"wood\"", {{5000, 5000}, {7600, 5000}, {7600, 7600}, {5000, 7600}, {5000, 5000}})),
        wide, cell);
    assert(mesh.batches[Features::Trees].features == Features::MaxTrees);
    assert(mesh.batches[Features::Trees].indices.size() / 3 ==
           Features::MaxTrees * Poseidon::EarthTrees::TriangleCount);
    Valid(mesh);
    std::string many;
    for (int i = 0; i < 2000; ++i)
    {
        if (i)
            many += ",";
        many += Way(100 + i, "\"highway\":\"residential\"", {{6000, 6400}, {7000, 6400}});
    }
    mesh = Features::BuildCell(Json(many), wide, cell);
    Valid(mesh);
    assert(mesh.batches[Features::PavedRoad].indices.size() / 3 == Features::MaxTriangles);
    const auto cache =
        std::filesystem::temp_directory_path() /
        ("op-earth-features-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(cache / "features");
    const auto file = cache / "features" / Buildings::FileName(cell);
    {
        std::ofstream out(file);
        out << Json(road + "," + forest);
    }
    mesh = Features::BuildNeighborhood(cache, g, {0, 1});
    assert(mesh.batches[Features::PavedRoad].features == 1);
    Valid(mesh);
    const auto second = cache / "features" / Buildings::FileName({0, 1});
    {
        std::ofstream out(second);
        out << Json(road);
    }
    mesh = Features::BuildNeighborhood(cache, g, cell);
    assert(mesh.batches[Features::PavedRoad].features == 1 && mesh.skipped == 1);
    assert(Features::BuildNeighborhood(cache, g, cell, [] { return true; }).error == "request cancelled");
    {
        Features stream(cache);
        Poseidon::EarthTerrainStream::Patch p{};
        p.x = g.x;
        p.z = g.z;
        p.gridMetres = g.grid;
        p.originLatitude = 0;
        p.originLongitude = 0;
        p.originRevision = 5;
        p.generation = 7;
        p.heights = g.heights;
        stream.Request(p, 6400, 6400);
        stream.Relocate();
        p.originRevision = 6;
        p.generation = 8;
        stream.Request(p, 6400, 6400);
        std::unique_ptr<Features::Mesh> ready;
        for (int i = 0; i < 200 && !ready; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ready = stream.TakeReady();
        }
        assert(ready && ready->revision == 6 && ready->generation == 8 && ready->batches[Features::Trees].features > 0);
        stream.Relocate();
        assert(!stream.TakeReady());
    }
    std::filesystem::remove(file);
    std::filesystem::remove(second);
    std::filesystem::remove(cache / "features");
    // Offline transport cache is sufficient without a prepared cell/provider.
    const auto generated = Poseidon::EarthContentSource::StreamPath(cache, "features", cell.latitude, cell.longitude);
    std::filesystem::create_directories(generated.parent_path());
    {
        std::ofstream out(generated);
        out << Json(road);
    }
    mesh = Features::BuildNeighborhood(cache, g, cell);
    assert(mesh.error.empty() && mesh.batches[Features::PavedRoad].features == 1);
    Valid(mesh);
    // A prepared cell has priority, even when deliberately empty; no merging
    // with an older generated copy for the same cell.
    std::filesystem::create_directories(file.parent_path());
    {
        std::ofstream out(file);
        out << Json("");
    }
    mesh = Features::BuildNeighborhood(cache, g, cell);
    assert(mesh.error.empty() && mesh.batches[Features::PavedRoad].features == 0);
    std::filesystem::remove(file);
    std::filesystem::remove(file.parent_path());
    std::filesystem::remove(generated);
    std::filesystem::remove(generated.parent_path());
    std::filesystem::remove(cache / "content-stream");
    std::filesystem::remove(cache);
    if (argc > 1)
    {
        std::ifstream input(argv[1], std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(input)), {});
        g = {2400, 2400, 31.25f, 49.61, 6.13, 1, 1, std::vector<float>(257 * 257, 300)};
        mesh = Features::BuildCell(json, g, Buildings::CellAt(49.61, 6.13));
        assert(mesh.error.empty());
        Valid(mesh);
        for (size_t i = 0; i < Features::KindCount; ++i)
            std::cout << "batch " << i << ": " << mesh.batches[i].features << " features, "
                      << mesh.batches[i].indices.size() / 3 << " triangles\n";
        assert(mesh.batches[Features::PavedRoad].features > 0 && mesh.batches[Features::Trees].features > 0);
    }
    std::cout << "PASS Earth features: road terrain sampling/clipping, bridge exclusion, forests, trees/signs, simple "
                 "water, procedural trunk/crown smooth normals and winding, geographic forest placement across "
                 "rebases/dateline, caps, offline transport-cache "
                 "fallback/prepared priority, neighborhood/deduplication, cancellation, revision-safe worker\n";
}
