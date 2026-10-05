#include <Poseidon/UI/LocalVehiclesCatalog.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace
{
struct Fixture
{
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("op-local-vehicles-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { std::filesystem::create_directories(root); }
    ~Fixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    static void U32(std::ostream& out, uint32_t value)
    {
        for (int shift = 0; shift != 32; shift += 8)
            out.put(static_cast<char>(value >> shift));
    }
    std::filesystem::path Model(uint32_t revision = 40, uint32_t method = 0, const std::string& prefix = "ca\\tracked")
    {
        auto path = root / "model.pbo";
        std::ofstream out(path, std::ios::binary);
        out.put(0);
        U32(out, 0x56657273);
        for (int i = 0; i < 4; ++i)
            U32(out, 0);
        out.write("prefix\0", 7);
        out.write(prefix.data(), prefix.size());
        out.put(0);
        out.put(0);
        out.write("t72.p3d\0", 8);
        U32(out, method);
        U32(out, 8);
        U32(out, 0);
        U32(out, 0);
        U32(out, 8);
        out.put(0);
        for (int i = 0; i < 5; ++i)
            U32(out, 0);
        out.write("ODOL", 4);
        U32(out, revision);
        return path;
    }
};
} // namespace

TEST_CASE("local vehicle catalog checks actual revision and virtual namespace", "[ui][local-vehicles]")
{
    Fixture f;
    using Poseidon::LocalVehicleDetail::ModelRevision;
    CHECK(ModelRevision(f.Model(), "t72.p3d", 40, "ca\\tracked\\t72"));
    CHECK_FALSE(ModelRevision(f.Model(73), "t72.p3d", 40, "ca\\tracked\\t72"));
    CHECK_FALSE(ModelRevision(f.Model(40, 0x43707273), "t72.p3d", 40, "ca\\tracked\\t72"));
    CHECK_FALSE(ModelRevision(f.Model(40, 0, "other"), "t72.p3d", 40, "ca\\tracked\\t72"));
    CHECK_FALSE(ModelRevision(f.Model(), "missing.p3d", 40, "ca\\tracked\\missing"));
    auto truncated = f.Model();
    std::filesystem::resize_file(truncated, 15);
    CHECK_FALSE(ModelRevision(truncated, "t72.p3d", 40, "ca\\tracked\\t72"));
}

TEST_CASE("local vehicle catalog refuses incomplete bridge and ignores foreign configs", "[ui][local-vehicles]")
{
    Fixture f;
    std::filesystem::create_directories(f.root / "Arma 3" / "Addons");
    CHECK(Poseidon::ScanLocalVehicles(f.root).empty());
    std::filesystem::create_directories(f.root / "Mods" / "@CWR_T72_A1" / "ADDONS");
    auto rows = Poseidon::ScanLocalVehicles(f.root);
    REQUIRE(rows.size() == 6);
    for (auto& row : rows)
    {
        CHECK_FALSE(row.ready);
        CHECK(row.diagnostic.find("Incomplete local bridge") != std::string::npos);
        CHECK(Poseidon::MakeLocalVehicleMission(row).empty());
    }
}

TEST_CASE("local vehicle mission permits only catalog classes", "[ui][local-vehicles]")
{
    Poseidon::LocalVehicle vehicle;
    vehicle.ready = true;
    vehicle.addonPatch = "CWR_T72_A1";
    vehicle.className = "CWR_T100_A3";
    auto mission = Poseidon::MakeLocalVehicleMission(vehicle);
    CHECK(mission.find("vehicle=\"CWR_T100_A3\"") != std::string::npos);
    CHECK(mission.find("side=\"EMPTY\"") != std::string::npos);
    vehicle.className = "CWR_T100_A3\";malicious=1;";
    CHECK(Poseidon::MakeLocalVehicleMission(vehicle).empty());
    vehicle.className = "CWR_T100_A3";
    vehicle.addonPatch = "a3_foreign";
    CHECK(Poseidon::MakeLocalVehicleMission(vehicle).empty());
}
