#include "WgpuRenderer/EarthWorldgen.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
using Worldgen = Poseidon::EarthWorldgen;
namespace Earth = Poseidon::Earth;
Worldgen::Object Object(uint32_t id, const char* model, float x, float y, float z)
{
    return {id, model, {1, .2f, 0, 0, 1, 0, 0, 0, 1, x, y, z}};
}
void Fixture(const std::filesystem::path& directory, const std::vector<Worldgen::Object>& objects,
             bool terminate = true)
{
    const double half = 128 / Earth::Radius * 180 / Earth::Pi;
    {
        std::ofstream out(directory / "manifest.json");
        out << std::setprecision(17)
            << "{\"height_scale\":0.05,\"world\":{\"name\":\"fixture\",\"cells\":16,\"cell_size\":16,\"bbox\":["
            << -half << "," << -half << "," << half << "," << half << "]}}";
    }
    std::ofstream out(directory / "source/fixture.wrp", std::ios::binary);
    out.write("4WVR", 4);
    const int32_t cells = 16;
    out.write(reinterpret_cast<const char*>(&cells), 4);
    out.write(reinterpret_cast<const char*>(&cells), 4);
    const std::vector<int16_t> heights(256, 200), textures(256, 0);
    out.write(reinterpret_cast<const char*>(heights.data()), 512);
    out.write(reinterpret_cast<const char*>(textures.data()), 512);
    const std::string table(512 * 32, '\0');
    out.write(table.data(), table.size());
    for (const auto& object : objects)
    {
        out.write(reinterpret_cast<const char*>(object.matrix.data()), 48);
        out.write(reinterpret_cast<const char*>(&object.id), 4);
        char model[76]{};
        assert(object.model.size() < 76);
        std::copy(object.model.begin(), object.model.end(), model);
        out.write(model, 76);
    }
    if (terminate)
    {
        const std::string empty(128, '\0');
        out.write(empty.data(), empty.size());
    }
}
int main(int argc, char** argv)
{
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("op-earth-worldgen-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory / "source");
    const std::vector<Worldgen::Object> objects{Object(1, "data3d\\str smrk_medium.p3d", 128, 19, 128),
                                                Object(2, "o\\road\\sil25.p3d", 144, 10.08f, 128),
                                                Object(3, "o\\hous\\domek_sedy.p3d", 128, 10, 144)};
    Fixture(directory, objects);
    auto source = Worldgen::Load(directory / "manifest.json");
    assert(source.error.empty() && source.objects.size() == 3 && source.heights.size() == 256);
    Poseidon::EarthBuildingStream::Ground ground{6272, 6272, 1, 0, 0, 5, 7, std::vector<float>(257 * 257, 300)};
    auto snapshot = Worldgen::Build(source, ground, 6400, 6400);
    assert(snapshot.error.empty() && snapshot.revision == 5 && snapshot.generation == 7 &&
           snapshot.coveredObjects == 3 && snapshot.objects.size() == 3);
    assert(snapshot.objects[0].model == objects[0].model && std::abs(snapshot.objects[0].matrix[10] - 309) < .0001);
    assert(snapshot.objects[0].matrix[9] == 6400 && snapshot.objects[0].matrix[11] == 6400);
    assert(std::abs(snapshot.objects[0].matrix[1] - .2) < .00001); // Authored forest shear survives.
    for (const auto& object : snapshot.objects)
    {
        for (float v : object.matrix)
            assert(std::isfinite(v));
        if (object.id == 2)
            assert(std::abs(object.matrix[10] - 300.08f) < .0001f);
        if (object.id == 3)
            assert(std::abs(object.matrix[10] - 300) < .0001f);
    }
    auto moved = ground;
    moved.latitude = -.3;
    moved.longitude = -.2;
    const auto centre = Earth::ToLocal(moved.latitude, moved.longitude, 0, 0);
    moved.x = float(centre.x - 128);
    moved.z = float(centre.z - 128);
    const auto rebased = Worldgen::Build(source, moved, float(centre.x), float(centre.z));
    assert(rebased.objects.size() == 3);
    const auto& tree = rebased.objects[0];
    const auto normal = Earth::GlobeNormal(moved.latitude, moved.longitude, 0, 0);
    assert(std::abs(tree.matrix[9] - (centre.x + 9 * normal.x)) < .003);
    assert(std::abs(tree.matrix[10] - (300 + 9 * normal.y)) < .0001);
    const auto restored =
        Earth::RotateFrame({moved.latitude, moved.longitude}, {0, 0}, {tree.matrix[0], tree.matrix[1], tree.matrix[2]});
    assert(std::abs(restored.x - 1) < .00001 && std::abs(restored.y - .2) < .00001 && std::abs(restored.z) < .00001);
    auto remote = ground;
    remote.x = remote.z = 100000;
    assert(Worldgen::Build(source, remote, 100000, 100000).objects.empty());
    assert(Worldgen::Build(source, ground, 6400, 6400, [] { return true; }).error == "request cancelled");
    auto invalidGround = ground;
    invalidGround.heights.clear();
    assert(!Worldgen::Build(source, invalidGround, 6400, 6400).error.empty());
    auto many = source;
    many.objects.clear();
    for (uint32_t id = 0; id < 3000; ++id)
    {
        auto object = objects[0];
        object.id = id;
        many.objects.push_back(object);
    }
    const auto capped = Worldgen::Build(many, ground, 6400, 6400);
    assert(capped.objects.size() == Worldgen::MaxInstances && capped.objects.back().id == Worldgen::MaxInstances - 1);
    many.objects.resize(100);
    for (size_t i = 0; i < many.objects.size(); ++i)
        many.objects[i].model = "fixture\\model" + std::to_string(i) + ".p3d";
    const auto models = Worldgen::Build(many, ground, 6400, 6400);
    assert(models.objects.size() == Worldgen::MaxModels && models.modelLimitSkipped == 100 - Worldgen::MaxModels);
    {
        Worldgen stream(directory / "manifest.json");
        Poseidon::EarthTerrainStream::Patch patch{};
        patch.x = ground.x;
        patch.z = ground.z;
        patch.gridMetres = ground.grid;
        patch.originLatitude = 0;
        patch.originLongitude = 0;
        patch.originRevision = 5;
        patch.generation = 7;
        patch.heights = ground.heights;
        stream.Request(patch, 6400, 6400);
        stream.Relocate();
        patch.originRevision = 6;
        patch.generation = 8;
        stream.Request(patch, 6400, 6400);
        std::unique_ptr<Worldgen::Snapshot> ready;
        for (int i = 0; i < 200 && !ready; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ready = stream.TakeReady();
        }
        assert(ready && ready->revision == 6 && ready->generation == 8 && ready->objects.size() == 3);
        stream.Relocate();
        assert(!stream.TakeReady());
    }
    Fixture(directory, {objects[0], objects[0]});
    assert(!Worldgen::Load(directory / "manifest.json").error.empty());
    Fixture(directory, objects, false);
    assert(!Worldgen::Load(directory / "manifest.json").error.empty());
    auto corrupt = objects;
    corrupt[0].matrix[0] = NAN;
    Fixture(directory, corrupt);
    assert(!Worldgen::Load(directory / "manifest.json").error.empty());
    corrupt = objects;
    corrupt[0].model = "..\\outside.p3d";
    Fixture(directory, corrupt);
    assert(!Worldgen::Load(directory / "manifest.json").error.empty());
    {
        std::ofstream out(directory / "manifest.json");
        out << "{\"world\":{\"name\":\"../outside\"}}";
    }
    assert(!Worldgen::Load(directory / "manifest.json").error.empty());
    std::filesystem::remove(directory / "source/fixture.wrp");
    std::filesystem::remove(directory / "source");
    std::filesystem::remove(directory / "manifest.json");
    std::filesystem::remove(directory);
    if (argc > 1)
    {
        const auto actual = Worldgen::Load(argv[1]);
        assert(actual.error.empty() && !actual.objects.empty());
        const double lat = (actual.south + actual.north) * .5, lon = (actual.west + actual.east) * .5;
        ground = {3200, 3200, 25, lat, lon, 1, 1, std::vector<float>(257 * 257, 300)};
        snapshot = Worldgen::Build(actual, ground, 6400, 6400);
        assert(snapshot.error.empty() && !snapshot.objects.empty());
        std::cout << "Actual Worldgen CPU: source=" << actual.objects.size() << " covered=" << snapshot.coveredObjects
                  << " selected=" << snapshot.objects.size() << " modelsSkipped=" << snapshot.modelLimitSkipped << "\n";
    }
    std::cout << "PASS Worldgen actual RVW4 wire parsing, stock model identities, resident grounding/relative lift, "
                 "affine shear/georebase, bounded selection, malformed input, stale worker rejection\n";
}
