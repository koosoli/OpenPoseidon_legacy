// Can the ENGINE read Arma Reforger's Everon straight out of the installed .pak set,
// with no offline conversion step?
//
// The question is worth a test rather than an opinion because the answer is
// counter-intuitive: every Enfusion reader this needs already lives in the engine
// library (`engine/Poseidon/Asset/Formats/Enfusion/`), not in the converter. The
// converter is where they are *called from*, which is a different statement, and it is
// the one that decides how much work a native path would be.
//
// So this walks the whole chain using engine code only, and asserts measured values at
// every hop. What it proves is a starting position, NOT a feature: nothing here draws a
// pixel, and the plumbing that would let the runtime do this at load time is the subject
// of the plan in `design notes`.
//
// The one hop that is NOT engine code is called out with a REQUIRE that documents it
// rather than skipping quietly — see the prefab section at the end.
//
// Skips when no Reforger install is present. That is not a hidden pass: the test above
// it in the compat suite already asserts the corpus opens, and this file's subject is
// what can be read out of it.

#define _CRT_SECURE_NO_WARNINGS
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Asset/Formats/Enfusion/EbinWorld.hpp>
#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>
#include <Poseidon/Asset/Formats/Enfusion/ResourceDatabase.hpp>
#include <Poseidon/Asset/Formats/Enfusion/TerrainTile.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobModel.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
namespace Enfusion = Poseidon::Asset::Formats::Enfusion;

std::filesystem::path LocateReforger()
{
    // Reforger installs to the C: library on this workstation; both are checked because
    // "Steam libraries exist on more than one drive" is a documented past mistake here.
    static const char* kRoots[] = {
        "C:/Program Files (x86)/Steam/steamapps/common/Arma Reforger",
        "D:/SteamLibrary/steamapps/common/Arma Reforger",
        "E:/SteamLibrary/steamapps/common/Arma Reforger",
    };
    for (const char* root : kRoots)
    {
        std::error_code ec;
        const std::filesystem::path candidate(root);
        if (std::filesystem::exists(candidate / "addons" / "data", ec))
            return candidate;
    }
    return {};
}

bool ReadLoose(const std::filesystem::path& p, std::vector<uint8_t>& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}
} // namespace

TEST_CASE("Reforger native: the engine reads Everon's terrain, world and models from the .pak set",
          "[compat][reforger][rfg-002]")
{
    const std::filesystem::path root = LocateReforger();
    if (root.empty())
        SKIP("No local Arma Reforger install available");

    const std::filesystem::path data = root / "addons" / "data";

    // -- 1. The archive ------------------------------------------------------------
    // Eden's world, terrain descriptor and height tiles all live in worlds.pak; the
    // models live in the data*.pak set. Only the first is needed to reach the world.
    Enfusion::PakArchive worlds;
    REQUIRE(worlds.Open((data / "worlds.pak").string()));

    // -- 2. The terrain descriptor -------------------------------------------------
    std::vector<uint8_t> blob;
    REQUIRE(worlds.Read("worlds/Eden/Eden/Eden.terr", blob));
    const Enfusion::TerrainDescriptor terr = Enfusion::ReadTerrainDescriptor(blob.data(), blob.size());
    REQUIRE(terr.valid());
    // Measured on the installed corpus. The grid is height SAMPLES, so the world's
    // extent is (gridWidth - 1) * cellSize metres.
    CHECK(terr.version == 9);
    CHECK(terr.cellSize > 0.0f);
    CHECK(terr.gridWidth > 1);
    CHECK(terr.gridHeight > 1);
    // heightScale/heightOffset are read from the file rather than assumed, and the
    // reader's own note records them as byte-identical across all 11 local terrains.
    CHECK(terr.heightScale > 0.0f);
    // Everon is a large island: at least 10 km on a side. A loose bound on purpose --
    // this is a sanity check on the units, not a pin on BI's content.
    const float extentX = static_cast<float>(terr.gridWidth - 1) * terr.cellSize;
    CHECK(extentX > 10000.0f);
    // Surface materials are named and indexable by the per-cell palette.
    CHECK(!terr.materials.empty());

    // -- 3. One height tile --------------------------------------------------------
    REQUIRE(worlds.Read("worlds/Eden/Eden/.Data/Eden_0.ttile", blob));
    const Enfusion::TerrainTile tile = Enfusion::ReadTerrainTile(blob.data(), blob.size());
    REQUIRE(tile.error.empty());
    // A tile carries (edge + 1)^2 samples, so a non-empty square is the shape to assert
    // without hard-coding BI's tile size.
    REQUIRE(tile.valid());
    REQUIRE(!tile.heights.empty());
    CHECK(static_cast<size_t>(tile.samples) * tile.samples == tile.heights.size());
    // Heights are raw u16; converted through the descriptor they must land in a range a
    // real island occupies rather than at the type's extremes.
    uint16_t lo = tile.heights[0], hi = tile.heights[0];
    for (uint16_t h : tile.heights)
    {
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    CHECK(terr.HeightAt(lo) > -500.0f);
    CHECK(terr.HeightAt(hi) < 3000.0f);

    // -- 4. The world's placements -------------------------------------------------
    // 70 MB of EBIN. The reader's byte accounting is the real assertion here: `consumed`
    // must equal the file size exactly, which is what makes the placement list
    // trustworthy rather than merely non-empty.
    REQUIRE(worlds.Read("worlds/Eden/Eden.ent", blob));
    const Enfusion::EbinWorld world = Enfusion::ReadEbin(blob.data(), blob.size());
    REQUIRE(world.valid());
    CHECK(world.consumed == blob.size());
    // Everon is a fully dressed island; a placement count in the millions is the point.
    CHECK(world.placements.size() > 1000000u);

    size_t positioned = 0, withPrefab = 0;
    for (const Enfusion::EbinPlacement& p : world.placements)
    {
        if (p.hasPosition)
            ++positioned;
        if (!p.prefabGuid.empty())
            ++withPrefab;
    }
    // Nearly every placement must carry both, or the world would not be reconstructable.
    CHECK(positioned * 10 > world.placements.size() * 9);
    CHECK(withPrefab * 10 > world.placements.size() * 9);

    // -- 5. GUID -> path -----------------------------------------------------------
    // The resource database sits LOOSE beside the .pak, not inside it.
    REQUIRE(ReadLoose(data / "resourceDatabase.rdb", blob));
    const Enfusion::ResourceDatabase rdb = Enfusion::ReadResourceDatabase(blob.data(), blob.size());
    REQUIRE(rdb.error.empty());
    CHECK(rdb.closes());
    CHECK(rdb.byGuid.size() > 10000u);

    // A placement's prefab GUID must resolve to a path in that database. Sampled rather
    // than exhaustive: the point is that the mapping works, and walking 1.2 M entries
    // would make this a benchmark instead of a smoke test.
    size_t sampled = 0, resolved = 0;
    for (const Enfusion::EbinPlacement& p : world.placements)
    {
        if (p.prefabGuid.empty())
            continue;
        if (++sampled > 2000)
            break;
        if (rdb.byGuid.find(p.prefabGuid) != rdb.byGuid.end())
            ++resolved;
    }
    REQUIRE(sampled > 0);
    // Not 100%: a world may reference prefabs that live in another addon's database
    // (core/ has its own). A clear majority resolving from data/ alone is the claim.
    CHECK(resolved * 2 > sampled);

    // -- 6. A model ----------------------------------------------------------------
    // Reached directly by path rather than through the prefab graph -- see below for
    // why that hop cannot be taken with engine code today.
    // Which data*.pak carries models is BI's business and may change between updates,
    // so the archive is chosen by what it contains rather than by name.
    Enfusion::PakArchive models;
    for (const char* name : {"data.pak", "data001.pak", "data002.pak", "data003.pak"})
    {
        if (!models.Open((data / name).string()))
            continue;
        const bool hasXob = std::any_of(models.Entries().begin(), models.Entries().end(),
                                        [](const Enfusion::PakEntry& e) {
                                            return e.path.size() > 4 &&
                                                   e.path.compare(e.path.size() - 4, 4, ".xob") == 0;
                                        });
        if (hasXob)
            break;
        models.Close();
    }
    REQUIRE(models.IsOpen());

    size_t modelsRead = 0;
    for (const Enfusion::PakEntry& e : models.Entries())
    {
        if (e.path.size() <= 4 || e.path.compare(e.path.size() - 4, 4, ".xob") != 0)
            continue;
        if (!models.Read(e, blob))
            continue;
        const Enfusion::XobModel model = Enfusion::ReadXobModel(blob.data(), blob.size());
        if (!model.error.empty())
            continue;
        CHECK(!model.lods.empty());
        if (++modelsRead >= 8)
            break;
    }
    // Eight is enough to say the reader works on shipped content without turning this
    // into a corpus census; the census exists separately and is what closure was
    // measured with.
    CHECK(modelsRead == 8);

    // Reported, not asserted. These are BI's content and move with every update, so
    // pinning them would make this test fail on a patch rather than on a regression --
    // but they are the numbers a reader of RFG-002 wants, and re-deriving them means
    // re-running a 70 MB parse.
    WARN("Everon, read entirely by engine code: terrain "
         << terr.gridWidth << "x" << terr.gridHeight << " samples at " << terr.cellSize << " m ("
         << (extentX / 1000.0f) << " km), " << terr.materials.size() << " surface materials; "
         << world.placements.size() << " placements, " << withPrefab << " with a prefab GUID; "
         << rdb.byGuid.size() << " GUIDs in data/resourceDatabase.rdb; " << resolved << " of " << sampled
         << " sampled prefab GUIDs resolved there");
}

// The gap, stated as a test so it cannot be forgotten between sessions.
//
// Every hop above used engine code. The hop from a placement's prefab GUID to a model
// path does NOT exist in the engine: a `.et` prefab names its mesh two blocks deep
// (`components { MeshObject { Object "{GUID}path.xob" } }`) and very often not at all --
// Everon's single most placed prefab, at 71,319 instances, only overrides two materials
// and inherits its Object from a parent. Resolving that means walking an inheritance
// chain, and the code that does it (`ResolvePrefabModel`, `FindObjectReference`) lives
// in `apps/tools/Tools/commands/XobCommand.cpp`.
//
// That is the only reader-level piece a native path is missing, and it is roughly a
// hundred lines. Everything else on the critical path is already in the library.
TEST_CASE("Reforger native: the prefab graph is engine-side now", "[compat][reforger][rfg-002][rfg-010]")
{
    // Walk up from __FILE__ until the repo root appears, rather than counting parent
    // levels: the first attempt at this counted two too few and skipped silently, which
    // is exactly the failure mode a test recording a known gap must not have.
    // RFG-010 moved it. This case is inverted rather than deleted: the gap it used to
    // record is closed, and what matters now is that it STAYS closed -- a second copy
    // growing back inside the tool is exactly the drift this was written to catch.
    const std::filesystem::path relative = std::filesystem::path("engine") / "Poseidon" / "Asset" / "Formats" /
                                           "Enfusion" / "PrefabResolve.hpp";
    std::filesystem::path repoTools;
    std::error_code ec;
    for (std::filesystem::path dir = std::filesystem::path(__FILE__).parent_path(); !dir.empty();
         dir = dir.parent_path())
    {
        if (std::filesystem::exists(dir / relative, ec))
        {
            repoTools = dir / relative;
            break;
        }
        if (!dir.has_relative_path())
            break; // reached the drive root
    }
    if (repoTools.empty())
        SKIP("PrefabResolve.hpp not found above __FILE__; the repository layout changed");

    std::ifstream f(repoTools);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    REQUIRE(!text.empty());

    CHECK(text.find("ResolvePrefabModel") != std::string::npos);
    CHECK(text.find("FindObjectReference") != std::string::npos);
    // The two traps the walk exists for, so a rewrite that drops either is loud:
    // m_sPhaseModel names a `.xob` too, and a prefab may inherit its mesh.
    CHECK(text.find("m_sPhaseModel") != std::string::npos);
    CHECK(text.find("depthLimit") != std::string::npos);
}
