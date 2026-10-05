#include <catch2/catch_test_macros.hpp>
#include <Poseidon/UI/LocalMapsCatalog.hpp>
#include <Poseidon/World/Terrain/TerrainPreviewStart.hpp>
#include <limits>
#include <filesystem>
#include <fstream>
#include <chrono>

TEST_CASE("Steam library discovery accepts old and current metadata", "[local-maps]")
{
    const auto roots = Poseidon::ParseSteamLibraryPaths(
        R"("libraryfolders" { "0" { "path" "C:\\Steam" "apps" { "107410" "123" } } "1" "D:\\SteamLibrary" })");
    REQUIRE(roots == std::vector<std::string>{"C:\\Steam", "D:\\SteamLibrary"});
    REQUIRE(Poseidon::ParseSteamLibraryPaths("missing metadata").empty());
    for (int revision : {18, 20, 24, 25, 29})
        REQUIRE(Poseidon::LocalMapRevisionSupported(revision));
    for (int revision : {0, 21, 22, 23, 26, 73})
        REQUIRE_FALSE(Poseidon::LocalMapRevisionSupported(revision));
}

TEST_CASE("Local map scan uses the archive prefix and guards unsupported worlds", "[local-maps]")
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
                      ("op-map-catalog-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup
    {
        fs::path path;
        ~Cleanup()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{root};
    auto write = [&](const char* name, uint32_t revision)
    {
        std::ofstream f(root / name, std::ios::binary);
        const auto u32 = [&](uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                f.put(static_cast<char>(v >> (8 * i)));
        };
        f.put(0);
        u32(0x56657273);
        for (int i = 0; i < 4; ++i)
            u32(0);
        f.write("prefix\0a3\\test\0\0", 16);
        f.write("test.wrp\0", 9);
        u32(0);
        u32(8);
        u32(0);
        u32(0);
        u32(8);
        f.put(0);
        for (int i = 0; i < 5; ++i)
            u32(0);
        u32(0x5752504f);
        u32(revision);
    };
    write("supported.pbo", 25);
    write("unseen.pbo", 23);
    std::ofstream(root / "broken.pbo") << "broken";
    const auto catalog = Poseidon::ScanLocalMapsFolder(root.string());
    REQUIRE(catalog.maps.size() == 2);
    REQUIRE(catalog.diagnostics.size() == 1);
    int supported = 0;
    for (const auto& map : catalog.maps)
    {
        REQUIRE(map.worldPath == "a3\\test\\test.wrp");
        REQUIRE(map.archiveRoots == std::vector<std::string>{root.string()});
        if (map.supported)
            ++supported;
    }
    REQUIRE(supported == 1);
}

TEST_CASE("Local map preview finds loaded land with a bounded search", "[local-maps]")
{
    int calls = 0;
    auto flat = Poseidon::ChooseTerrainPreviewStart(1000.0f, [&](float, float) { ++calls; return 12.0f; });
    REQUIRE(calls == 82);
    REQUIRE(flat.x == 500.0f);
    REQUIRE(flat.z == 500.0f);
    REQUIRE(flat.height == 12.0f);
    auto island = Poseidon::ChooseTerrainPreviewStart(1000.0f, [](float x, float z) {
        if (x > 290.0f && x < 310.0f && z > 790.0f && z < 810.0f) return 150.0f;
        if (x < 200.0f) return std::numeric_limits<float>::quiet_NaN();
        return 0.0f;
    });
    REQUIRE(island.x == 300.0f);
    REQUIRE(island.z == 800.0f);
    REQUIRE(island.height == 150.0f);
}
