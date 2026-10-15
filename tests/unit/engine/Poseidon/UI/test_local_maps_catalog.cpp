#include <catch2/catch_test_macros.hpp>
#include <Poseidon/UI/LocalMapsCatalog.hpp>
#include <Poseidon/World/Terrain/TerrainPreviewStart.hpp>
#include <limits>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

TEST_CASE("Local mission scan finds campaign missions and marks the playable ones", "[local-maps]")
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
                      ("op-mission-catalog-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "Campaigns");
    fs::create_directories(root / "MPMissions");
    struct Cleanup
    {
        fs::path path;
        ~Cleanup()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{root};
    // A PBO with a "prefix" header and two zero-length mission.sqm entries.
    auto write = [&](const fs::path& path)
    {
        std::ofstream f(path, std::ios::binary);
        const auto u32 = [&](uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                f.put(static_cast<char>(v >> (8 * i)));
        };
        f.put(0);
        u32(0x56657273);
        for (int i = 0; i < 4; ++i)
            u32(0);
        const char header[] = "prefix\0campaigns\\ca\0";
        f.write(header, sizeof(header)); // includes the terminating empty key
        for (const char* name : {"missions\\m09convoyattack.sara\\mission.sqm", "missions\\m99test.intro\\mission.sqm"})
        {
            f.write(name, static_cast<std::streamsize>(std::strlen(name) + 1));
            for (int i = 0; i < 5; ++i)
                u32(0);
        }
        f.put(0);
        for (int i = 0; i < 5; ++i)
            u32(0);
    };
    write(root / "Campaigns" / "ca.pbo");
    write(root / "MPMissions" / "mp.pbo"); // multiplayer: never listed
    const auto catalog = Poseidon::ScanLocalMapsFolder(root.string());
    REQUIRE(catalog.diagnostics.empty());
    REQUIRE(catalog.missions.size() == 2);
    for (const auto& mission : catalog.missions)
    {
        REQUIRE(mission.collection == "ca");
        REQUIRE(mission.installRoot == root.string());
        REQUIRE(mission.virtualPath == "campaigns\\ca\\missions\\" + mission.name);
        // A local folder is not an Arma 1 install, so nothing it holds is playable.
        REQUIRE_FALSE(mission.supported);
    }
    // With Arma 1's executable beside it the same folder is an Arma 1 install: the Sahrani
    // mission becomes playable, the one on an unregistered terrain does not.
    std::ofstream(root / "ArmA.exe") << "";
    REQUIRE(Poseidon::DetectLocalGame(root.string(), "Local folder") == "Arma 1");
    const auto arma1 = Poseidon::ScanLocalMapsFolder(root.string());
    REQUIRE(arma1.missions.size() == 2);
    REQUIRE(arma1.missions[0].supported); // playable first
    REQUIRE(arma1.missions[0].name == "m09convoyattack.sara");
    REQUIRE(arma1.missions[0].game == "Arma 1");
    REQUIRE_FALSE(arma1.missions[1].supported);
    std::string reason;
    REQUIRE(Poseidon::LocalMissionSupported("Arma 1", "sara", reason));
    REQUIRE(reason.empty());
    REQUIRE(Poseidon::LocalMissionSupported("Arma 1", "Sara_DBE1", reason));
    REQUIRE_FALSE(Poseidon::LocalMissionSupported("Arma 1", "intro", reason));
    REQUIRE_FALSE(reason.empty());
    // Arma 3 missions too, experimentally; other games' missions stay refused.
    REQUIRE(Poseidon::LocalMissionSupported("Arma 3", "altis", reason));
    REQUIRE_FALSE(reason.empty());
    REQUIRE_FALSE(Poseidon::LocalMissionSupported("DayZ", "chernarusplus", reason));
    // Arma 2 and OA missions are offered experimentally, on any terrain, with the reason shown
    REQUIRE(Poseidon::LocalMissionSupported("Arma 2", "chernarus", reason));
    REQUIRE_FALSE(reason.empty());
    REQUIRE(Poseidon::LocalMissionSupported("Arma 2 OA", "takistan", reason));
}

// Manual probe, hidden from normal runs: OP_CONTENT_SCAN=<folder> PoseidonTests "[.content-probe]"
// lists what the ARMA CONTENT browser would show for that folder.
TEST_CASE("Content scan probe", "[.content-probe]")
{
    const char* folder = std::getenv("OP_CONTENT_SCAN");
    if (!folder)
        return;
    const auto catalog = Poseidon::ScanLocalMapsFolder(folder);
    for (const auto& map : catalog.maps)
        std::printf("MAP %s | %s | rev %d | %s\n", map.game.c_str(), map.name.c_str(), map.revision,
                    map.supported ? "supported" : "unsupported");
    for (const auto& mission : catalog.missions)
        std::printf("MISSION %s | %s | %s | %s | %s\n", mission.game.c_str(), mission.collection.c_str(),
                    mission.name.c_str(), mission.supported ? "playable" : mission.reason.c_str(),
                    mission.virtualPath.c_str());
    for (const auto& line : catalog.diagnostics)
        std::printf("DIAG %s\n", line.c_str());
}
