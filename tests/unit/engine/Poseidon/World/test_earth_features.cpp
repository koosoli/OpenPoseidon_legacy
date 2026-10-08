#include "WgpuRenderer/EarthFeatureStream.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
using Features = Poseidon::EarthFeatureStream;
using Buildings = Poseidon::EarthBuildingStream;
using Point = std::pair<double, double>;
std::string Way(uint64_t id, const char* tags, const std::vector<Point>& points)
{
    std::ostringstream out;
    out << std::setprecision(12) << "{\"type\":\"way\",\"id\":" << id << ",\"tags\":{" << tags << "},\"geometry\":[";
    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto c = Poseidon::Earth::FromLocal(0, 0, points[i].first, points[i].second);
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
            assert(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z));
        for (size_t i = 0; i < b.indices.size(); i += 3)
        {
            assert(b.indices[i] < b.vertices.size() && b.indices[i + 1] < b.vertices.size() &&
                   b.indices[i + 2] < b.vertices.size());
            const auto a = b.vertices[b.indices[i]], c = b.vertices[b.indices[i + 1]], d = b.vertices[b.indices[i + 2]];
            const float ux = c.x - a.x, uy = c.y - a.y, uz = c.z - a.z, vx = d.x - a.x, vy = d.y - a.y, vz = d.z - a.z;
            assert((uy * vz - uz * vy) * a.nx + (uz * vx - ux * vz) * a.ny + (ux * vy - uy * vx) * a.nz > 0);
        }
    }
    assert(triangles <= Features::MaxTriangles && mesh.batches[Features::Trees].features <= Features::MaxTrees &&
           mesh.forestSamples <= 16000);
}
int main(int argc, char** argv)
{
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
        assert(std::abs(low - (offset == .25 ? 1.08f : 51.08f)) < .001f);
    }
    const Features::Ground wide{3200, 3200, 25, 0, 0, 1, 1, std::vector<float>(257 * 257, 10)};
    mesh = Features::BuildCell(
        Json(Way(47, "\"natural\":\"wood\"", {{5000, 5000}, {7600, 5000}, {7600, 7600}, {5000, 7600}, {5000, 5000}})),
        wide, cell);
    assert(mesh.batches[Features::Trees].features == Features::MaxTrees);
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
                 "water, geometry winding, caps, neighborhood/deduplication, cancellation, revision-safe worker\n";
}
