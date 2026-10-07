// test_compat_smoke.cpp -- REN-GL33-002: the cross-generation compatibility smoke.
//
// Why this file exists
// --------------------
// The WGPU sole-renderer gate (roadmap Phase 2.2) has a bullet reading "Arma
// 1/2/3/DayZ/Reforger compatibility smoke tests pass". REN-GL33-001 measured that
// bullet and found the tests did not exist: every compat claim in the project was a
// hand-run capture written up in design notes, and a gate bullet that
// cannot be *failed* by a run is not a gate. This file is that run.
//
// What a compat smoke test can honestly be, here
// ----------------------------------------------
// It cannot launch the game -- these are unit tests, and this machine allows one
// game instance at a time. So it does not assert pixels. What it does assert is the
// thing every capture silently depends on and no existing test covers: that the
// readers for each generation's *real shipped containers* still open the real
// corpora and produce the counts they produced when the compat work landed.
//
// This is deliberately different from the rest of tests/unit/engine/Poseidon/Asset/
// Formats/, which is almost entirely synthetic -- fixtures built byte by byte so
// they can live in the repo and run on CI. Those tests pin what the format IS. They
// cannot notice that a reader stopped reading Chernarus, because no Chernarus is
// ever handed to them. The one existing exception is the local-corpus case at the
// bottom of test_oprw25.cpp, and this file follows exactly its pattern -- resolve by
// walking up from the executable, skip when the data is absent.
//
// Where every expected number came from
// -------------------------------------
// Each number below is annotated in place with its provenance. There are three
// kinds, and they are never mixed silently:
//
//   MEASURED   read off the corpus on this workstation on 2026-08-31, by the reader
//              under test and cross-checked against an independent second witness
//              (a standalone Python header/PBO probe that shares no code with the
//              engine). Both witnesses are named at the assertion.
//   RECORDED   taken from a decision document that measured it earlier, cited by
//              filename. Re-measured here; the citation says who first established
//              it.
//   STRUCTURAL derived from the format definition rather than from any one file
//              (e.g. terrainRange must be a whole multiple of landRange).
//
// No number here was chosen to make the test pass.
//
// Licensing note: not one byte of any corpus is committed. packages/ and
// inspiration/ are gitignored; the Steam installs are read where they lie. Every
// case reports itself SKIPped when its corpus is absent, so this file is a no-op on
// CI rather than a failure.
//
// Deliberately NOT tagged [GameData]. That tag is excluded from the whole ctest run
// whenever packages/Remaster is missing (tests/unit/engine/Poseidon/CMakeLists.txt,
// OFPR_CATCH_TEST_SPEC), and Remaster is missing on every machine that nevertheless
// has these corpora -- including the one this was written on. Tagging it [GameData]
// would have made the compat smoke silently never run, which is the exact failure
// mode it was written to end.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/Asset/Formats/World/Oprw20.hpp>
#include <Poseidon/Asset/Formats/World/Oprw24.hpp>
#include <Poseidon/Asset/Formats/World/Oprw25.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>

#include "test_fixtures.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace Poseidon;
using namespace Poseidon::Asset::Formats;
using namespace Poseidon::Asset::Formats::World;
namespace Enfusion = Poseidon::Asset::Formats::Enfusion;
namespace P3D = Poseidon::Asset::Formats::P3D;

namespace
{

// ---------------------------------------------------------------------------
// Corpus location
// ---------------------------------------------------------------------------

// Repo-relative corpora (packages/, inspiration/) resolved by walking up from the
// executable, not from the working directory, which CTest does not guarantee. The
// walk also reaches the parent checkout from inside worktrees/<name>/, which
// is where the gitignored corpora actually live.
std::filesystem::path LocateRelative(const char* relative)
{
    for (std::filesystem::path at(TestFixtures::GetExecutableDirectory()); !at.empty(); at = at.parent_path())
    {
        std::error_code ec;
        const std::filesystem::path candidate = at / relative;
        if (std::filesystem::exists(candidate, ec))
            return candidate;
        if (at.parent_path() == at)
            break;
    }
    return {};
}

// Installed games are absolute. Both Steam library roots are tried: assuming one
// drive is a documented past mistake in this project (the development guidelines rule 0b).
std::filesystem::path LocateInstalled(const char* suffix)
{
    static const char* kSteamRoots[] = {
        "D:/SteamLibrary/steamapps/common",
        "C:/Program Files (x86)/Steam/steamapps/common",
        "E:/SteamLibrary/steamapps/common",
    };
    for (const char* root : kSteamRoots)
    {
        std::error_code ec;
        const std::filesystem::path candidate = std::filesystem::path(root) / suffix;
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return {};
}

// ---------------------------------------------------------------------------
// A read-only PBO index, sufficient to reach a stored entry's first bytes
// ---------------------------------------------------------------------------
//
// Deliberately local to this file and deliberately tiny. The engine's own PBO path
// is the FileBank/FileServer machinery, which wants a mounted world and a running
// file server; this smoke needs eight bytes from the head of each `.p3d` in one
// archive, and nothing else. Keeping it here also keeps it honest: it is a *second*
// implementation, so when it and the engine's ODOL dispatch agree on a revision
// histogram, two independent readers agree.
//
// Only stored (mime 0) entries are indexed. Every `.p3d` in every corpus archive
// this file names is stored -- verified by the Python probe, which reports zero
// compressed `.p3d` entries across all five archives.
struct PboEntry
{
    std::string name;
    uint32_t mime = 0;
    uint64_t offset = 0;
    uint32_t size = 0;
};

class PboIndex
{
  public:
    bool Open(const std::filesystem::path& path)
    {
        file_ = std::fopen(path.string().c_str(), "rb");
        if (!file_)
            return false;

        std::vector<PboEntry> headers;
        for (;;)
        {
            std::string name;
            if (!Asciiz(name))
                return false;
            uint32_t rec[5];
            if (std::fread(rec, sizeof(rec), 1, file_) != 1)
                return false;
            // A "Vers" product entry is followed by an asciiz key/value list, not by
            // payload. Missing this shifts every subsequent data offset and makes a
            // whole archive look like garbage -- which is exactly how it presented
            // before it was handled.
            if (rec[0] == 0x56657273u) // 'Vers'
            {
                for (;;)
                {
                    std::string key;
                    if (!Asciiz(key))
                        return false;
                    if (key.empty())
                        break;
                    std::string value;
                    if (!Asciiz(value))
                        return false;
                }
                continue;
            }
            if (name.empty() && rec[0] == 0 && rec[1] == 0 && rec[4] == 0)
                break; // terminating record; payload starts here
            PboEntry entry;
            entry.name = name;
            entry.mime = rec[0];
            entry.size = rec[4];
            headers.push_back(entry);
        }

        const long dataStart = std::ftell(file_);
        if (dataStart < 0)
            return false;
        uint64_t at = static_cast<uint64_t>(dataStart);
        for (PboEntry& entry : headers)
        {
            entry.offset = at;
            at += entry.size;
            entries_.push_back(entry);
        }
        return true;
    }

    ~PboIndex()
    {
        if (file_)
            std::fclose(file_);
    }

    const std::vector<PboEntry>& Entries() const { return entries_; }

    // The first `count` bytes of an entry, or an empty vector.
    std::vector<uint8_t> Head(const PboEntry& entry, size_t count) const
    {
        std::vector<uint8_t> out;
        if (entry.mime != 0 || entry.size < count)
            return out;
        if (std::fseek(file_, static_cast<long>(entry.offset), SEEK_SET) != 0)
            return out;
        out.resize(count);
        if (std::fread(out.data(), 1, count, file_) != count)
            out.clear();
        return out;
    }

  private:
    bool Asciiz(std::string& out)
    {
        out.clear();
        for (;;)
        {
            const int c = std::fgetc(file_);
            if (c == EOF)
                return false;
            if (c == 0)
                return true;
            out.push_back(static_cast<char>(c));
            if (out.size() > 1024)
                return false;
        }
    }

    std::FILE* file_ = nullptr;
    std::vector<PboEntry> entries_;
};

bool EndsWithNoCase(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    if (s.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i])))
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// World expectations
// ---------------------------------------------------------------------------

struct WorldCase
{
    const char* generation;
    const char* relative; // repo-relative, under packages/
    int32_t revision;     // OPRW container revision this generation ships
    int32_t landRange;    // square
    int32_t terrainRange;
    float landCellSize;
    size_t models;    // distinct referenced .p3d paths
    size_t objects;   // placements
    size_t materials; // terrain surface rvmats, including the empty index 0
};

// Every row below was produced on 2026-08-31 by running this very reader over the
// file named, and cross-checked field by field against a standalone Python probe
// that reads the OPRW header independently (scratchpad `pboscan.py` +
// header probe; see REN-GL33-002 for the exact commands). The revisions are also
// RECORDED: 20 for Arma 1 and 24 for Arma 2 are stated by Oprw25.hpp's own revision
// table, 25 for Arma 3 by WLD-010/011/012, and 29 for DayZ by
// DZ-001-dayz-is-an-arma-2-descendant-20260814.md.
//
// The counts are the load-bearing part. A model count, a placement count and a
// material count together fail on any field-width or codec regression anywhere in
// the reader: get one field wrong early and the object table's declared byte size
// no longer matches, get the compression gate wrong and the string tables come out
// as noise. They are cheap to assert and impossible to satisfy by accident.
// Three of these counts had an independent witness in the tree BEFORE they were
// measured here, which is the strongest provenance available without a second
// implementation, and they agree exactly:
//
//   Sahrani  893 models   -- test_odol40_arma1.cpp:4 ("every one of the 893 models
//                            sara.wrp references failed at the container")
//   Takistan 534 / 438906 -- memory note `takistan-oa-compat-harness` ("534 of 534
//                            referenced models over 438,906 placements")
//   Altis    1779908      -- test_oprw25.cpp:396 ("Altis has 1.78 million of them")
//   ChernarusPlus 2971218 -- memory note `dayz-compat-harness` ("2.97M placements")
//
const WorldCase kWorldCases[] = {
    // -- Arma 1 (Armed Assault) ------------------------------------------------
    // Extracted from packages/a1-compat/addons/{sara,saralite,Sara_DBE1}.pbo; see
    // REN-GL33-002 for the extraction command. Sahrani is 512 land cells at 40 m,
    // i.e. 20480 m. Sahrani Lite is revision *18*, not 20 -- the two Arma 1 worlds
    // in the same game do not share a container revision, and a compat smoke that
    // only ever saw sara.wrp would never have found that out.
    {"Arma 1", "packages/a1-compat/world/sara/sara.wrp", 20, 512, 2048, 40.0f, 893, 602481, 5126},
    {"Arma 1", "packages/a1-compat/world/saralite/saralite.wrp", 18, 256, 1024, 40.0f, 687, 168817, 1731},
    // -- Arma 2 / Operation Arrowhead, OPRW 24 -------------------------------
    {"Arma 2", "packages/a2-compat/world/chernarus/chernarus.wrp", 24, 512, 2048, 30.0f, 1245, 1055157, 1025},
    {"Arma 2 OA", "packages/a2-compat/world/takistan/takistan.wrp", 24, 256, 2048, 50.0f, 534, 438906, 3293},
    {"Arma 2 OA", "packages/a2-compat/world/zargabad/zargabad.wrp", 24, 256, 2048, 32.0f, 406, 93516, 1351},
    // -- Arma 3, OPRW 25 ------------------------------------------------------
    {"Arma 3", "packages/a3-compat/world/stratis/stratis.wrp", 25, 256, 2048, 32.0f, 350, 163953, 479},
    {"Arma 3", "packages/a3-compat/world/altis/altis.wrp", 25, 1024, 4096, 30.0f, 963, 1779908, 14731},
    // -- DayZ, OPRW 29 --------------------------------------------------------
    {"DayZ", "packages/dayz-compat/world/chernarusplus/chernarusplus.wrp", 29, 256, 2048, 60.0f, 2352, 2971218, 7193},
    {"DayZ", "packages/dayz-compat/world/enoch/enoch.wrp", 29, 256, 2048, 50.0f, 2484, 2932069, 9432},
};

Oprw25World ReadWorldByRevision(BinaryReader& reader, int32_t revision)
{
    switch (revision)
    {
        // 18 has no named profile entry point either; Sahrani Lite is the only
        // world in any local corpus that ships it.
        case 18:
            return ReadOprwModern(reader, 18);
        case 20:
            return ReadOprw20(reader);
        case 24:
            return ReadOprw24(reader);
        case 25:
            return ReadOprw25(reader);
        // 29 is DayZ. It has no named profile entry point of its own -- DZ-001
        // established that it rides the later-generation profile -- so it is read
        // through ReadOprwModern directly, at its own revision.
        case 29:
            return ReadOprwModern(reader, 29);
        default:
            throw std::runtime_error("no profile for OPRW revision");
    }
}

} // namespace

// ===========================================================================
// 1. Worlds
// ===========================================================================

TEST_CASE("Compat smoke: every generation's world container parses to its measured shape",
          "[compat][smoke][ren-gl33-002]")
{
    int covered = 0;
    for (const WorldCase& expected : kWorldCases)
    {
        const std::filesystem::path found = LocateRelative(expected.relative);
        if (found.empty())
            continue;

        INFO(expected.generation << " -- " << found.string());

        QIFStream file;
        file.open(found.string().c_str());
        REQUIRE_FALSE(file.fail());

        BinaryReader reader(file);
        // PeekOprwRevision must name the revision without disturbing the stream.
        // If this drifts, every generation dispatches to the wrong profile and the
        // failure downstream is unreadable.
        REQUIRE(PeekOprwRevision(reader) == expected.revision);

        const Oprw25World world = ReadWorldByRevision(reader, expected.revision);
        ++covered;

        REQUIRE(world.header.version == expected.revision);

        // STRUCTURAL: true of any world of any revision. These catch a grid read
        // that happens to produce plausible-looking numbers.
        REQUIRE(world.header.landRangeX == world.header.landRangeY);
        REQUIRE(world.header.terrainRangeX == world.header.terrainRangeY);
        REQUIRE(world.header.landRangeX > 0);
        REQUIRE(world.header.terrainRangeX % world.header.landRangeX == 0);
        REQUIRE(world.header.landCellSize > 0.0f);
        REQUIRE(world.elevation.size() == static_cast<size_t>(world.header.terrainRangeX) * world.header.terrainRangeY);
        REQUIRE(world.geography.size() == static_cast<size_t>(world.header.landRangeX) * world.header.landRangeY);
        REQUIRE(world.materialIndex.size() == world.geography.size());
        REQUIRE(world.roadNet.size() == world.geography.size());

        // Counted rather than asserted per object: Chernarus+ has millions, and a
        // REQUIRE each would be millions of Catch2 assertions.
        size_t outOfRange = 0;
        for (const Oprw25Object& object : world.objects)
            if (static_cast<size_t>(object.modelIndex) >= world.models.size())
                ++outOfRange;
        REQUIRE(outOfRange == 0);

        // MEASURED, per-world. See kWorldCases.
        REQUIRE(world.header.landRangeX == expected.landRange);
        REQUIRE(world.header.terrainRangeX == expected.terrainRange);
        REQUIRE(world.header.landCellSize == Catch::Approx(expected.landCellSize));
        REQUIRE(world.models.size() == expected.models);
        REQUIRE(world.objects.size() == expected.objects);
        REQUIRE(world.materials.size() == expected.materials);
    }

    if (covered == 0)
        SKIP("No local Arma 1/2/3 or DayZ world corpus available");
}

// ===========================================================================
// 2. Models
// ===========================================================================

struct ModelFamilyCase
{
    const char* generation;
    bool installed; // true: absolute Steam path; false: repo-relative
    const char* path;
    uint32_t revision; // the ONE ODOL revision this archive ships
    size_t models;     // .p3d entries in the archive
};

// One representative archive per generation, chosen for being large enough that an
// off-by-anything in the PBO walk shows up, and for containing only static world
// props (so the revision claim is about world content, not about one vehicle).
//
// MEASURED 2026-08-31 by the Python PBO probe, which shares no code with the engine;
// the assertion below re-measures with the engine's own PeekOdolRevision. The
// per-generation revision is additionally RECORDED in
// engine/Poseidon/Asset/Formats/P3D/OdolRevision.hpp, whose table was established by
// AST-007 over 545 PBOs and by DZ-001 over DayZ's 133.
const ModelFamilyCase kModelCases[] = {
    {"Arma 1", false, "packages/a1-compat/addons/buildings.pbo", 40, 611},
    {"Arma 2", false,
     "inspiration/officialarma/ALDP_A2_PBOs_ADPL-SA_APL-SA_part1/ALDP_A2_PBOs_APL-SA_part1/buildings.pbo", 48, 340},
    {"Arma 2 OA", false,
     "inspiration/officialarma/ALDP_A2OA_PBOs_ADPL-SA_APL-SA/ALDP_A2OA_PBOs_APL-SA/structures_e.pbo", 49, 462},
    {"Arma 3", true, "Arma 3/Addons/structures_f.pbo", 73, 454},
    {"DayZ", true, "DayZ/Addons/structures_industrial.pbo", 54, 318},
};

TEST_CASE("Compat smoke: each generation's models are the revision that generation ships",
          "[compat][smoke][ren-gl33-002]")
{
    int covered = 0;
    for (const ModelFamilyCase& expected : kModelCases)
    {
        const std::filesystem::path found =
            expected.installed ? LocateInstalled(expected.path) : LocateRelative(expected.path);
        if (found.empty())
            continue;

        INFO(expected.generation << " -- " << found.string());

        PboIndex pbo;
        REQUIRE(pbo.Open(found));

        size_t models = 0;
        size_t offRevision = 0;
        size_t unreadable = 0;
        std::string firstOffender;
        for (const PboEntry& entry : pbo.Entries())
        {
            if (!EndsWithNoCase(entry.name, ".p3d"))
                continue;
            ++models;
            const std::vector<uint8_t> head = pbo.Head(entry, 8);
            if (head.size() != 8)
            {
                ++unreadable;
                continue;
            }
            uint32_t version = 0;
            std::memcpy(&version, head.data() + 4, sizeof(version));
            if (std::memcmp(head.data(), "ODOL", 4) != 0 || version != expected.revision)
            {
                if (firstOffender.empty())
                    firstOffender = entry.name;
                ++offRevision;
            }
        }

        INFO("first off-revision entry: " << firstOffender);
        REQUIRE(unreadable == 0);
        // MEASURED: the whole archive is one revision, without exception. This is
        // the claim OdolRevision.hpp's table rests on; if it stops holding, the
        // dispatch table is describing a corpus that no longer exists.
        REQUIRE(offRevision == 0);
        REQUIRE(models == expected.models);

        // And the engine's own dispatch must agree about what that revision means.
        // Reading the table is not enough -- what matters to a caller is that the
        // revision resolves to a reader rather than to Unknown, and that it is
        // labelled with the right generation.
        const P3D::OdolRevisionInfo info = P3D::DescribeOdolRevision(expected.revision);
        REQUIRE(info.version == expected.revision);
        REQUIRE(info.support != P3D::OdolSupport::Unknown);
        REQUIRE(std::string(info.generation) != "unknown");

        ++covered;
    }

    if (covered == 0)
        SKIP("No local Arma 1/2/3 or DayZ model corpus available");
}

// ===========================================================================
// 3. Reforger
// ===========================================================================

TEST_CASE("Compat smoke: Reforger's whole pak corpus opens and closes", "[compat][smoke][ren-gl33-002]")
{
    // Reforger installs to the C: library on this workstation (app 1874880);
    // LocateInstalled tries both roots because assuming one drive is a documented
    // past mistake here.
    const std::filesystem::path root = LocateInstalled("Arma Reforger/addons");
    if (root.empty())
        SKIP("No local Arma Reforger install available");

    std::vector<std::filesystem::path> paks;
    for (std::filesystem::recursive_directory_iterator it(root), end; it != end; ++it)
        if (it->is_regular_file() && it->path().extension() == ".pak")
            paks.push_back(it->path());
    std::sort(paks.begin(), paks.end());

    // RECORDED: ARF-001 measured 16 archives / 222,566 entries on this install, and
    // the memory note `reforger-compat-harness` repeats the 16. An install that is
    // mid-update reports fewer, which is a corpus problem rather than a reader one --
    // so the count is asserted as "at least", and the closure below is asserted for
    // every archive actually present. See REN-GL33-002 for why this one number is
    // the exception to exact-count.
    REQUIRE(paks.size() >= 16);

    size_t entries = 0;
    for (const std::filesystem::path& path : paks)
    {
        INFO(path.string());
        Enfusion::PakArchive archive;
        REQUIRE(archive.Open(path.string()));
        const Enfusion::PakClosure& closure = archive.Closure();
        // Each of the four is reported separately because each catches a different
        // class of mistake; asserting only complete() would hide which one broke.
        REQUIRE(closure.formSizeCloses);
        REQUIRE(closure.chunkWalkCloses);
        REQUIRE(closure.directoryCloses);
        REQUIRE(closure.dataTiles);
        REQUIRE(closure.gapBytes == 0);
        REQUIRE(closure.overlaps == 0);
        REQUIRE(closure.nonZeroReserved == 0);
        entries += archive.Entries().size();
    }

    // MEASURED 2026-08-31 over the 16 archives of this install: 222,566 entries --
    // which is exactly the number ARF-001 recorded, so the reader has not drifted
    // and the install has not moved since that work landed. Asserted as a lower
    // bound rather than as that literal, for the same reason as the archive count:
    // Reforger updates add content, and this test's subject is the reader, not BI's
    // release cadence. A reader regression collapses this by orders of magnitude
    // (or fails a closure assertion above first), which the bound still catches.
    REQUIRE(entries >= 200000);
    WARN("Reforger corpus: " << paks.size() << " archives, " << entries << " entries");
}
