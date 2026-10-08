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
             bool terminate = true, const char* validation = nullptr, double latitude = 0, double longitude = 0)
{
    const double half = 128 / Earth::Radius * 180 / Earth::Pi;
    {
        std::ofstream out(directory / "manifest.json");
        out << std::setprecision(17) << "{";
        if (validation)
            out << "\"final_validation\":{\"status\":\"" << validation << "\"},";
        out << "\"height_scale\":0.05,\"world\":{\"name\":\"fixture\",\"cells\":16,\"cell_size\":16,\"bbox\":["
            << latitude - half << "," << longitude - half << "," << latitude + half << "," << longitude + half << "]}}";
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
void IndexTests(const std::filesystem::path& root, const std::vector<Worldgen::Object>& objects)
{
    const auto add = [&](const char* name, uint32_t id, double longitude)
    {
        const auto directory = root / name;
        std::filesystem::create_directories(directory / "source");
        Fixture(directory, objects, true, "failed", 0, longitude);
        const auto source = Worldgen::Load(directory / "manifest.json");
        assert(source.error.empty());
        return Worldgen::Region{id, directory / "manifest.json", source.south, source.west, source.north, source.east};
    };
    const auto a = add("a", 20, 0), b = add("b", 30, .01), far = add("far", 40, .15), overlap = add("overlap", 10, 0);
    const auto write = [&](const std::vector<Worldgen::Region>& regions)
    {
        std::ofstream out(root / "index.json");
        out << std::setprecision(17) << "{\"earth_worldgen_regions\":1,\"regions\":[";
        for (size_t i = 0; i < regions.size(); ++i)
        {
            const auto& r = regions[i];
            out << (i ? "," : "") << "{\"id\":" << r.id << ",\"manifest\":\""
                << r.manifest.lexically_relative(root).generic_string() << "\",\"bbox\":[" << r.south << "," << r.west
                << "," << r.north << "," << r.east << "]}";
        }
        out << "]}";
    };
    write({far, b, a});
    auto catalog = Worldgen::LoadCatalog(root / "index.json");
    assert(catalog.error.empty() && catalog.indexed && catalog.regions.size() == 3);
    Poseidon::EarthBuildingStream::Ground ground{3200, 3200, 25, 0, 0, 5, 7, std::vector<float>(257 * 257, 300)};
    auto selected = Worldgen::SelectRegions(catalog, ground, 6400, 6400);
    assert(selected.size() == 2 && selected[0].id == 20 && selected[1].id == 30);
    auto dateGround = ground;
    dateGround.longitude = -179.99;
    Worldgen::Catalog dateCatalog;
    dateCatalog.indexed = true;
    dateCatalog.regions.push_back({1, {}, -.001, 179.98, .001, 179.999});
    assert(Worldgen::SelectRegions(dateCatalog, dateGround, 6400, 6400).size() == 1);
    auto sourceA = Worldgen::LoadRegion(a), sourceB = Worldgen::LoadRegion(b);
    assert(sourceA.error.empty() && sourceB.error.empty());
    assert(sourceA.objects[0].id == (20u << 24) && sourceA.objects[2].id == ((20u << 24) | 2));
    auto snapshot = Worldgen::Build(std::vector<const Worldgen::Source*>{&sourceA, &sourceB}, ground, 6400, 6400);
    assert(snapshot.objects.size() == 6 && snapshot.sourceObjects == 6 && snapshot.regionIds.size() == 2);
    auto sourceOverlap = Worldgen::LoadRegion(overlap);
    snapshot = Worldgen::Build(std::vector<const Worldgen::Source*>{&sourceA, &sourceOverlap}, ground, 6400, 6400);
    assert(snapshot.objects.size() == 3 && snapshot.coveredObjects == 3);
    for (const auto& object : snapshot.objects)
        assert(object.id >> 24 == 10); // Lower valid resident region owns overlap.
    auto mismatch = overlap;
    mismatch.east += .001;
    const auto invalid = Worldgen::LoadRegion(mismatch);
    assert(!invalid.error.empty() && invalid.objects.empty() && invalid.valueBytes == 0);
    snapshot = Worldgen::Build(std::vector<const Worldgen::Source*>{&sourceA, &invalid}, ground, 6400, 6400);
    assert(snapshot.objects.size() == 3 && !snapshot.error.empty()); // Invalid lower ID cannot blank valid objects.
    auto missing = overlap;
    missing.manifest = root / "missing/manifest.json";
    const auto missingSource = Worldgen::LoadRegion(missing);
    snapshot = Worldgen::Build(std::vector<const Worldgen::Source*>{&sourceA, &missingSource}, ground, 6400, 6400);
    assert(snapshot.objects.size() == 3 && !snapshot.error.empty());
    auto sparseIds = objects;
    sparseIds[0].id = INT32_MAX;
    sparseIds[1].id = 0;
    sparseIds[2].id = 9000;
    Fixture(root / "a", sparseIds);
    auto highRegion = a;
    highRegion.id = 255;
    const auto ranked = Worldgen::LoadRegion(highRegion);
    assert(ranked.error.empty() && ranked.objects[0].model == objects[1].model &&
           ranked.objects[2].model == objects[0].model && ranked.objects[2].id == ((255u << 24) | 2));
    auto reordered = objects;
    std::reverse(reordered.begin(), reordered.end());
    Fixture(root / "a", reordered);
    const auto sourceAgain = Worldgen::LoadRegion(a);
    for (size_t i = 0; i < sourceA.objects.size(); ++i)
        assert(sourceAgain.objects[i].id == sourceA.objects[i].id &&
               sourceAgain.objects[i].model == sourceA.objects[i].model);
    write({a, far, b});
    selected = Worldgen::SelectRegions(Worldgen::LoadCatalog(root / "index.json"), ground, 6400, 6400);
    assert(selected[0].id == 20 && selected[1].id == 30); // Catalog order does not affect residency/IDs.
    int checks = 0;
    const auto cancelled = Worldgen::Load(a.manifest, [&] { return ++checks == 4; });
    assert(cancelled.error == "request cancelled" && cancelled.objects.empty() && cancelled.heights.empty());
    auto manyA = sourceA, manyB = sourceB;
    manyA.objects.resize(3000, sourceA.objects[0]);
    manyB.objects.resize(3000, sourceB.objects[0]);
    for (size_t i = 0; i < 3000; ++i)
    {
        manyA.objects[i].id = (20u << 24) | uint32_t(i);
        manyB.objects[i].id = (30u << 24) | uint32_t(i);
    }
    snapshot = Worldgen::Build(std::vector<const Worldgen::Source*>{&manyA, &manyB}, ground, 6400, 6400);
    assert(snapshot.sourceObjects == 6000 && snapshot.coveredObjects == 6000 && snapshot.objects.size() == 2048);
    manyA.objects.resize(50);
    manyB.objects.resize(50);
    for (size_t i = 0; i < 50; ++i)
    {
        manyA.objects[i].model = "fixture\\a" + std::to_string(i) + ".p3d";
        manyB.objects[i].model = "fixture\\b" + std::to_string(i) + ".p3d";
    }
    snapshot = Worldgen::Build(std::vector<const Worldgen::Source*>{&manyA, &manyB}, ground, 6400, 6400);
    assert(snapshot.objects.size() == 64 && snapshot.modelLimitSkipped == 36);
    manyA.valueBytes = manyB.valueBytes = Worldgen::MaxResidentValueBytes;
    assert(!Worldgen::Build(std::vector<const Worldgen::Source*>{&manyA, &manyB}, ground, 6400, 6400).error.empty());
    // The actual worker crosses to a distant prepared region, then returns with
    // stable IDs. Obsolete requests cannot publish after geographic relocation.
    {
        Worldgen stream(root / "index.json");
        Poseidon::EarthTerrainStream::Patch patch{};
        patch.x = ground.x;
        patch.z = ground.z;
        patch.gridMetres = ground.grid;
        patch.originLatitude = 0;
        patch.originLongitude = 0;
        patch.originRevision = 5;
        patch.generation = 7;
        patch.heights = ground.heights;
        const auto wait = [&](bool clean = true)
        {
            std::unique_ptr<Worldgen::Snapshot> ready;
            for (int i = 0; i < 200 && !ready; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ready = stream.TakeReady();
            }
            assert(ready && (!clean || ready->error.empty()));
            return ready;
        };
        stream.Request(patch, 6400, 6400);
        auto ready = wait();
        assert(ready->regionIds.size() == 2 && ready->sourceObjects == 6 && ready->objects.size() == 6);
        const auto farPoint = Earth::ToLocal(0, 0, 0, .15);
        patch.x = float(farPoint.x - 3200);
        patch.generation = 8;
        stream.Request(patch, float(farPoint.x), 6400);
        ready = wait();
        assert(ready->revision == 5 && ready->generation == 8 && ready->regionIds == std::vector<uint32_t>{40});
        const auto farId = ready->objects[0].id;
        stream.Relocate();
        patch.x = ground.x;
        patch.originLongitude = .15;
        patch.originRevision = 6;
        patch.generation = 9;
        stream.Request(patch, 6400, 6400);
        ready = wait();
        assert(ready->revision == 6 && ready->generation == 9 && ready->regionIds == std::vector<uint32_t>{40});
        assert(ready->objects.size() == 3 && ready->objects[0].id == farId);
        patch.originLongitude = 0;
        patch.originRevision = 7;
        patch.generation = 10;
        stream.Request(patch, 6400, 6400);
        // A newer request cancels the immediately preceding geographic frame.
        patch.originRevision = 8;
        patch.generation = 11;
        stream.Request(patch, 6400, 6400);
        ready = wait();
        assert(ready->revision == 8 && ready->generation == 11 && ready->objects.size() == 6);
        assert(ready->objects[0].id == sourceA.objects[0].id);
        patch.originLongitude = .15;
        patch.originRevision = 9;
        patch.generation = 12;
        stream.Request(patch, 6400, 6400);
        ready = wait();
        // Reload cannot silently change placement identities after an offline
        // generator replaces a previously resident output.
        std::filesystem::last_write_time(a.manifest,
                                         std::filesystem::last_write_time(a.manifest) + std::chrono::seconds(1));
        patch.originLongitude = 0;
        patch.originRevision = 10;
        patch.generation = 13;
        stream.Request(patch, 6400, 6400);
        ready = wait(false);
        assert(ready->error.find("changed during run") != std::string::npos && ready->objects.size() == 3 &&
               ready->regionIds == std::vector<uint32_t>{30});
    }
    write({a, a});
    assert(!Worldgen::LoadCatalog(root / "index.json").error.empty());
    auto bad = a;
    bad.id = 21;
    write({a, bad});
    assert(!Worldgen::LoadCatalog(root / "index.json").error.empty());
    bad.id = 256;
    write({bad});
    assert(!Worldgen::LoadCatalog(root / "index.json").error.empty());
    std::vector<Worldgen::Region> oversized(65, a);
    write(oversized);
    assert(!Worldgen::LoadCatalog(root / "index.json").error.empty());
    {
        std::ofstream out(root / "index.json");
        out << "{\"earth_worldgen_regions\":1,\"regions\":[{\"id\":1,\"manifest\":\"../"
               "outside.json\",\"bbox\":[0,0,1,1]}]}";
    }
    assert(!Worldgen::LoadCatalog(root / "index.json").error.empty());
    for (const auto* name : {"a", "b", "far", "overlap"})
    {
        std::filesystem::remove(root / name / "source/fixture.wrp");
        std::filesystem::remove(root / name / "source");
        std::filesystem::remove(root / name / "manifest.json");
        std::filesystem::remove(root / name);
    }
    std::filesystem::remove(root / "index.json");
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
    IndexTests(directory, objects);
    auto source = Worldgen::Load(directory / "manifest.json");
    assert(source.error.empty() && source.objects.size() == 3 && source.heights.size() == 256);
    assert(source.validation == "unspecified");
    Fixture(directory, objects, true, "failed");
    std::filesystem::create_directories(directory / "source/fixture");
    std::filesystem::rename(directory / "source/fixture.wrp", directory / "source/fixture/fixture.wrp");
    const auto packaged = Worldgen::Load(directory / "manifest.json");
    assert(packaged.error.empty() && packaged.objects.size() == 3 && packaged.validation == "failed");
    std::filesystem::rename(directory / "source/fixture/fixture.wrp", directory / "source/fixture.wrp");
    std::filesystem::remove(directory / "source/fixture");
    Poseidon::EarthBuildingStream::Ground ground{6272, 6272, 1, 0, 0, 5, 7, std::vector<float>(257 * 257, 300)};
    assert(Worldgen::Build(packaged, ground, 6400, 6400).sourceValidation == "failed");
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
    Fixture(directory, objects);
    std::filesystem::resize_file(directory / "source/fixture.wrp",
                                 12 + 16 * 16 * 4 + 512 * 32 + (Worldgen::MaxSourceObjects + 2) * 128);
    assert(Worldgen::Load(directory / "manifest.json").error.find("budget/length") != std::string::npos);
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
        const auto catalog = Worldgen::LoadCatalog(argv[1]);
        if (!catalog.error.empty())
            std::cerr << "Actual Worldgen catalog error: " << catalog.error << "\n";
        assert(catalog.error.empty() && !catalog.regions.empty());
        const auto& first = catalog.regions.front();
        const double lat = (first.south + first.north) * .5, lon = (first.west + first.east) * .5;
        ground = {3200, 3200, 25, lat, lon, 1, 1, std::vector<float>(257 * 257, 300)};
        const auto selected = Worldgen::SelectRegions(catalog, ground, 6400, 6400);
        std::vector<Worldgen::Source> loaded;
        for (const auto& region : selected)
        {
            loaded.push_back(Worldgen::LoadRegion(region));
            if (!loaded.back().error.empty())
                std::cerr << "Actual Worldgen source error: " << loaded.back().error << "\n";
            assert(loaded.back().error.empty());
        }
        std::vector<const Worldgen::Source*> sources;
        for (const auto& source : loaded)
            sources.push_back(&source);
        snapshot = Worldgen::Build(sources, ground, 6400, 6400);
        assert(snapshot.error.empty() && !snapshot.objects.empty());
        std::cout << "Actual Worldgen CPU: upstreamValidation=" << snapshot.sourceValidation
                  << " regions=" << snapshot.regionIds.size() << " source=" << snapshot.sourceObjects
                  << " sourceBytes=" << snapshot.residentSourceBytes << " valueBytes=" << snapshot.residentValueBytes
                  << " covered=" << snapshot.coveredObjects << " selected=" << snapshot.objects.size()
                  << " modelsSkipped=" << snapshot.modelLimitSkipped << "\n";
    }
    std::cout
        << "PASS Worldgen actual RVW4 wire parsing, stock model identities, resident grounding/relative lift, "
           "affine shear/georebase, bounded region residency/overlap/IDs, malformed input, stale worker rejection\n";
}
