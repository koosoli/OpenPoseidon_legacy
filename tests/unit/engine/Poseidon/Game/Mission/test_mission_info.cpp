#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Game/Mission/MissionInfo.hpp>
#include <Poseidon/Game/Mission/MissionPathLoader.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <format>
#include <optional>
#include <system_error>

namespace
{
// Helper for setting up the hasEnd[6] array in a single literal.
struct EndFlags
{
    bool e[6] = {false, false, false, false, false, false};
};

bool Contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

std::filesystem::path MakeTempMissionDir(const char* name)
{
    const auto root = std::filesystem::temp_directory_path() / "ofpr-mission-loader-tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    REQUIRE_FALSE(ec);
    return root;
}

void CleanupTempMissionDir(const std::filesystem::path& root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    REQUIRE_FALSE(ec);
}

void WriteMissionFile(const std::filesystem::path& missionDir, const char* name = "mission.sqm")
{
    std::ofstream(missionDir / name) << "version=11;";
}
} // namespace

TEST_CASE("MissionInfo::EndingsFromSensorFlags returns empty when no ASTEnd flags set", "[mission][info]")
{
    EndFlags ef; // all false
    auto r = MissionInfo::EndingsFromSensorFlags(false, ef.e);
    REQUIRE(r.empty());
}

TEST_CASE("MissionInfo::EndingsFromSensorFlags ignores hasLoose flag (universal outcome)", "[mission][info]")
{
    // ASTLoose maps to the universal "lose" outcome wired by the cheat
    // layer, not a script-defined ending — it must NOT appear in this
    // pure helper's output.  The flag stays in the signature for
    // future expansion but does not affect the result today.
    EndFlags ef;
    auto withoutLoose = MissionInfo::EndingsFromSensorFlags(false, ef.e);
    auto withLoose = MissionInfo::EndingsFromSensorFlags(true, ef.e);
    REQUIRE(withoutLoose == withLoose);
    REQUIRE(withoutLoose.empty());
}

TEST_CASE("MissionInfo::EndingsFromSensorFlags maps hasEnd[0] to end1", "[mission][info]")
{
    EndFlags ef;
    ef.e[0] = true;
    auto r = MissionInfo::EndingsFromSensorFlags(false, ef.e);
    REQUIRE(r.size() == 1);
    REQUIRE(r[0] == "end1");
}

TEST_CASE("MissionInfo::EndingsFromSensorFlags preserves declaration order 1..6", "[mission][info]")
{
    EndFlags ef;
    // Mark out-of-order: 4, 1, 3.  Expected canonical order is 1, 3, 4.
    ef.e[3] = true;
    ef.e[0] = true;
    ef.e[2] = true;
    auto r = MissionInfo::EndingsFromSensorFlags(false, ef.e);
    REQUIRE(r.size() == 3);
    REQUIRE(r[0] == "end1");
    REQUIRE(r[1] == "end3");
    REQUIRE(r[2] == "end4");
}

TEST_CASE("MissionInfo::EndingsFromSensorFlags lists all six when all sensors present", "[mission][info]")
{
    EndFlags ef;
    for (int i = 0; i < 6; i++)
        ef.e[i] = true;
    auto r = MissionInfo::EndingsFromSensorFlags(false, ef.e);
    REQUIRE(r.size() == 6);
    for (int i = 0; i < 6; i++)
        REQUIRE(Contains(r, std::string("end") + std::to_string(i + 1)));
}

TEST_CASE("MissionInfo::EndingsFromSensorFlags does not produce names like win/lose/killed", "[mission][info]")
{
    // Those are cheat-layer universals (DebugCheats::Cmd_EndMission::
    // AvailableOutcomes prepends them) — must not leak into the
    // script-ending list reported by MissionInfo itself.
    EndFlags ef;
    for (int i = 0; i < 6; i++)
        ef.e[i] = true;
    auto r = MissionInfo::EndingsFromSensorFlags(true, ef.e);
    REQUIRE_FALSE(Contains(r, "win"));
    REQUIRE_FALSE(Contains(r, "lose"));
    REQUIRE_FALSE(Contains(r, "killed"));
}

TEST_CASE("Legacy mission identifiers cannot overwrite the adjacent world name", "[mission][loader]")
{
    struct Fields { char mission[80]; char world[80]; } fields{};
    REQUIRE(MissionPathLoader::CopyLegacyIdentifier(fields.world, "eden"));
    for (const size_t size : {size_t(0), size_t(78), size_t(79), size_t(80), size_t(160)})
    {
        const std::string name(size, 'm');
        CHECK(MissionPathLoader::CopyLegacyIdentifier(fields.mission, name) == (size < 80));
        CHECK(std::string(fields.mission) == name.substr(0, 79));
        CHECK(fields.mission[79] == '\0');
        CHECK(std::string(fields.world) == "eden");
    }
}

TEST_CASE("MissionPathLoader rejects identifiers beyond the legacy header capacity", "[mission][loader]")
{
    for (const size_t size : {size_t(79), size_t(80), size_t(160)})
    {
        const std::string name(size, 'm');
        CHECK(MissionPathLoader::Loader::DescribeMissionFile("missions/" + name + ".eden/mission.sqm").has_value() == (size < 80));
        CHECK(MissionPathLoader::Loader::DescribeMissionFile("missions/test." + name + "/mission.sqm").has_value() == (size < 80));
    }
}

TEST_CASE("MissionPathLoader::DescribeMissionFile parses stock mission paths", "[mission][loader]")
{
    const auto selection = MissionPathLoader::Loader::DescribeMissionFile("missions/Benchmark.Abel/mission.sqm");
    REQUIRE(selection.has_value());
    CHECK(selection->missionFilePath == "missions/Benchmark.Abel/mission.sqm");
    CHECK(selection->missionParentDirectory == "missions/");
    CHECK(selection->missionName == "Benchmark");
    CHECK(selection->worldName == "Abel");
}

TEST_CASE("MissionPathLoader::DescribeMissionFile parses native and Windows separators", "[mission][loader]")
{
    const auto selection =
        MissionPathLoader::Loader::DescribeMissionFile("C:\\tests\\custom\\00training.Noe\\mission.sqm");
    REQUIRE(selection.has_value());
    CHECK(selection->missionFilePath == "C:/tests/custom/00training.Noe/mission.sqm");
    CHECK(selection->missionParentDirectory == "C:/tests/custom/");
    CHECK(selection->missionName == "00training");
    CHECK(selection->worldName == "Noe");
}

TEST_CASE("MissionPathLoader::DescribeMissionFile keeps dots in mission names", "[mission][loader]")
{
    const auto selection =
        MissionPathLoader::Loader::DescribeMissionFile("/tmp/custom/coop.training.v2.Noe/mission.sqm");
    REQUIRE(selection.has_value());
    CHECK(selection->missionParentDirectory == "/tmp/custom/");
    CHECK(selection->missionName == "coop.training.v2");
    CHECK(selection->worldName == "Noe");
}

TEST_CASE("MissionPathLoader::DescribeMissionFile accepts uppercase mission filename", "[mission][loader]")
{
    const auto selection = MissionPathLoader::Loader::DescribeMissionFile("/tmp/custom/00training.Noe/MISSION.SQM");
    REQUIRE(selection.has_value());
    CHECK(selection->missionFilePath == "/tmp/custom/00training.Noe/MISSION.SQM");
    CHECK(selection->missionName == "00training");
    CHECK(selection->worldName == "Noe");
}

TEST_CASE("MissionPathLoader::DescribeMissionFile parses relative mission paths", "[mission][loader]")
{
    const auto selection = MissionPathLoader::Loader::DescribeMissionFile("00training.Noe/mission.sqm");
    REQUIRE(selection.has_value());
    CHECK(selection->missionParentDirectory.empty());
    CHECK(selection->missionName == "00training");
    CHECK(selection->worldName == "Noe");
}

TEST_CASE("MissionPathLoader::DescribeMissionFile rejects folders without world suffix", "[mission][loader]")
{
    CHECK_FALSE(MissionPathLoader::Loader::DescribeMissionFile("/tmp/custom/00training/mission.sqm").has_value());
}

TEST_CASE("MissionPathLoader::DescribeMissionFile rejects non mission filenames", "[mission][loader]")
{
    CHECK_FALSE(
        MissionPathLoader::Loader::DescribeMissionFile("/tmp/custom/00training.Noe/description.ext").has_value());
}

TEST_CASE("MissionPathLoader::DescribeMissionFile rejects missing path separators", "[mission][loader]")
{
    CHECK_FALSE(MissionPathLoader::Loader::DescribeMissionFile("mission.sqm").has_value());
}

TEST_CASE("MissionPathLoader::DescribeMissionFile rejects empty mission or world tokens", "[mission][loader]")
{
    CHECK_FALSE(MissionPathLoader::Loader::DescribeMissionFile("/tmp/custom/.Noe/mission.sqm").has_value());
    CHECK_FALSE(MissionPathLoader::Loader::DescribeMissionFile("/tmp/custom/Training./mission.sqm").has_value());
}

TEST_CASE("MissionPathLoader::ResolveMissionFile resolves mission folders", "[mission][loader]")
{
    const auto root = MakeTempMissionDir("resolve-folder");
    const auto missionDir = root / "CustomMission.Noe";
    std::error_code ec;
    std::filesystem::create_directories(missionDir, ec);
    REQUIRE_FALSE(ec);

    WriteMissionFile(missionDir);

    const auto resolved = MissionPathLoader::Loader::ResolveMissionFile(missionDir);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == (missionDir / "mission.sqm").lexically_normal().generic_string());

    CleanupTempMissionDir(root);
}

TEST_CASE("MissionPathLoader::ResolveMissionFile resolves direct mission file paths", "[mission][loader]")
{
    const auto root = MakeTempMissionDir("resolve-file");
    const auto missionDir = root / "DirectMission.Noe";
    std::error_code ec;
    std::filesystem::create_directories(missionDir, ec);
    REQUIRE_FALSE(ec);

    WriteMissionFile(missionDir);

    const auto missionFile = missionDir / "mission.sqm";
    const auto resolved = MissionPathLoader::Loader::ResolveMissionFile(missionFile);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == missionFile.lexically_normal().generic_string());

    CleanupTempMissionDir(root);
}

TEST_CASE("MissionPathLoader::ResolveMissionFile normalizes dot segments", "[mission][loader]")
{
    const auto root = MakeTempMissionDir("resolve-normalized");
    const auto missionDir = root / "Normalized.Noe";
    std::error_code ec;
    std::filesystem::create_directories(missionDir, ec);
    REQUIRE_FALSE(ec);

    WriteMissionFile(missionDir);

    const auto nested = missionDir / "subdir";
    std::filesystem::create_directories(nested, ec);
    REQUIRE_FALSE(ec);

    const auto resolved = MissionPathLoader::Loader::ResolveMissionFile(nested / ".." / "mission.sqm");
    REQUIRE(resolved.has_value());
    CHECK(*resolved == (missionDir / "mission.sqm").lexically_normal().generic_string());

    CleanupTempMissionDir(root);
}

TEST_CASE("MissionPathLoader::ResolveMissionFile accepts uppercase mission file names when they exist",
          "[mission][loader]")
{
    const auto root = MakeTempMissionDir("resolve-uppercase");
    const auto missionDir = root / "Uppercase.Noe";
    std::error_code ec;
    std::filesystem::create_directories(missionDir, ec);
    REQUIRE_FALSE(ec);

    WriteMissionFile(missionDir, "MISSION.SQM");

    const auto resolved = MissionPathLoader::Loader::ResolveMissionFile(missionDir / "MISSION.SQM");
    REQUIRE(resolved.has_value());
    CHECK(*resolved == (missionDir / "MISSION.SQM").lexically_normal().generic_string());

    CleanupTempMissionDir(root);
}

TEST_CASE("MissionPathLoader::ResolveMissionFile rejects folders without mission.sqm", "[mission][loader]")
{
    const auto root = MakeTempMissionDir("resolve-missing");
    const auto missionDir = root / "Broken.Noe";
    std::error_code ec;
    std::filesystem::create_directories(missionDir, ec);
    REQUIRE_FALSE(ec);

    CHECK_FALSE(MissionPathLoader::Loader::ResolveMissionFile(missionDir).has_value());

    CleanupTempMissionDir(root);
}

TEST_CASE("MissionPathLoader::ResolveMissionFile rejects non mission files", "[mission][loader]")
{
    const auto root = MakeTempMissionDir("resolve-wrong-file");
    const auto missionDir = root / "WrongFile.Noe";
    std::error_code ec;
    std::filesystem::create_directories(missionDir, ec);
    REQUIRE_FALSE(ec);

    std::ofstream(missionDir / "description.ext") << "class Header{};";

    CHECK_FALSE(MissionPathLoader::Loader::ResolveMissionFile(missionDir / "description.ext").has_value());
    CHECK_FALSE(MissionPathLoader::Loader::ResolveMissionFile(root / "missing.sqm").has_value());

    CleanupTempMissionDir(root);
}
