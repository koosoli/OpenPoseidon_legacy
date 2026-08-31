// SIM-814 -- the gate that makes the stage graph's resource declarations fail when they
// are wrong.
//
// SIM-811 shipped six stages with declared reads and writes and wrote down, honestly, that
// "a stage that touches something it did not declare will not be caught". These cases are
// what catches it.
//
// The check is against the SOURCE TEXT, not against a running engine, and that is a choice
// rather than a compromise. No unit test in this repo can construct a `World` -- there is
// no game data on a build machine -- so an instrumented runtime recorder would report zero
// violations in CI while meaning "did not run". This project has a written history of
// exactly that diagnostic. A source check runs on every build, on every machine, and its
// failure mode is a red test naming a stage, a resource and a file.
//
// Three gates, and they fail for three different reasons:
//
//   LIVENESS  every hop of every recorded call path still exists. This is the one that
//             trips when somebody deletes or renames a call five layers under a stage --
//             the mechanism by which a hand-written declaration goes stale.
//   COVERAGE  every declared bit has a path or a named widening, and every path's bit is
//             declared. Neither side may drift from the other in silence.
//   CENSUS    for the three resources whose access points are rare enough to enumerate,
//             every occurrence in engine/Poseidon is classified. A NEW call inside a
//             stage's reach fails until the declaration widens to cover it.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/World/SimStageEvidence.hpp>
#include <Poseidon/World/SimStageGraph.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Poseidon::Sim;

namespace
{

std::filesystem::path PoseidonDir()
{
    return std::filesystem::path(TESTS_ROOT_DIR).parent_path() / "engine" / "Poseidon";
}

const std::string& SourceOf(std::string_view relative)
{
    static std::map<std::string, std::string> cache;
    const std::string key{relative};
    auto it = cache.find(key);
    if (it != cache.end())
    {
        return it->second;
    }

    std::filesystem::path p = PoseidonDir();
    std::string part;
    std::istringstream parts{key};
    while (std::getline(parts, part, '/'))
    {
        p /= part;
    }

    std::string text;
    std::ifstream f(p, std::ios::binary);
    if (f.is_open())
    {
        std::ostringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    return cache.emplace(key, std::move(text)).first->second;
}

/// The definition whose line begins with `symbol`, as [first, last) over the body.
///
/// "Begins with" is deliberate: every symbol in the evidence table is a definition at
/// column zero, so an indented declaration in a header cannot be mistaken for one.
struct BodyRange
{
    bool found = false;
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t line = 0;
};

std::size_t LineOf(const std::string& text, std::size_t pos)
{
    return static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(pos), '\n')) +
           1;
}

BodyRange FindBody(const std::string& text, std::string_view symbol)
{
    BodyRange out;
    std::size_t at = 0;
    while ((at = text.find(symbol, at)) != std::string::npos)
    {
        if (at == 0 || text[at - 1] == '\n')
        {
            break;
        }
        at += 1;
    }
    if (at == std::string::npos)
    {
        return out;
    }

    const std::size_t open = text.find('{', at);
    if (open == std::string::npos)
    {
        return out;
    }

    // Brace matching that does not count braces inside comments, strings or character
    // literals. Without this a body containing "{" in a log format string ends early and
    // the gate produces a false failure, which would be worse than no gate.
    int depth = 0;
    std::size_t i = open;
    for (; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
            i = text.find('\n', i);
            if (i == std::string::npos)
            {
                break;
            }
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
        {
            i = text.find("*/", i + 2);
            if (i == std::string::npos)
            {
                break;
            }
            i += 1;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            const char quote = c;
            ++i;
            while (i < text.size() && text[i] != quote)
            {
                if (text[i] == '\\')
                {
                    ++i;
                }
                ++i;
            }
            continue;
        }
        if (c == '{')
        {
            ++depth;
        }
        else if (c == '}')
        {
            if (--depth == 0)
            {
                out.found = true;
                out.begin = open;
                out.end = i;
                out.line = LineOf(text, at);
                return out;
            }
        }
    }
    return out;
}

/// The identifier a hop calls: the last identifier before the first `(` in its text.
std::string CalleeName(std::string_view contains)
{
    const std::size_t paren = contains.find('(');
    if (paren == std::string::npos || paren == 0)
    {
        return {};
    }
    std::size_t end = paren;
    while (end > 0 && (contains[end - 1] == ' '))
    {
        --end;
    }
    std::size_t start = end;
    while (start > 0)
    {
        const char c = contains[start - 1];
        const bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ident)
        {
            break;
        }
        --start;
    }
    return std::string{contains.substr(start, end - start)};
}

std::string Describe(const SimEvidence& e)
{
    std::ostringstream ss;
    ss << "stage '" << SimStageName(e.stage) << "' " << (e.write ? "WRITES" : "reads") << " resource '"
       << SimResourceName(e.resource) << "'";
    return ss.str();
}

bool IsCommentLine(const std::string& line)
{
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
        ++i;
    }
    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
    {
        return true;
    }
    return i < line.size() && line[i] == '*';
}

/// Every non-comment line under engine/Poseidon that contains `needle`, as
/// (repo-relative file, line number, line text).
struct Occurrence
{
    std::string file;
    std::size_t line = 0;
    std::string text;
};

std::vector<Occurrence> Occurrences(std::string_view needle)
{
    std::vector<Occurrence> out;
    const auto root = PoseidonDir();
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".inc")
        {
            continue;
        }
        const std::string name = entry.path().filename().string();
        // The evidence and graph files quote every witness in their own comments and
        // tables; censusing them would census this test's own data.
        if (name == "SimStageEvidence.cpp" || name == "SimStageEvidence.hpp" || name == "SimStageGraph.cpp" ||
            name == "SimStageGraph.hpp")
        {
            continue;
        }

        std::ifstream f(entry.path());
        if (!f.is_open())
        {
            continue;
        }
        std::string line;
        std::size_t n = 0;
        while (std::getline(f, line))
        {
            ++n;
            if (line.find(needle) == std::string::npos || IsCommentLine(line))
            {
                continue;
            }
            std::string rel = std::filesystem::relative(entry.path(), root).generic_string();
            out.push_back({std::move(rel), n, line});
        }
    }
    return out;
}

std::uint32_t DeclBit(SimStage stage, bool write)
{
    const SimStageDecl& d = SimStageDeclFor(stage);
    return write ? d.writes : d.reads;
}

} // namespace

// --------------------------------------------------------------------- anti-vacuity ---
//
// Everything below is a check over a table and a corpus. If either came back empty the
// checks would all pass while measuring nothing, so the sizes are pinned first.

TEST_CASE("the evidence table and the source corpus are both non-empty", "[determinism][taskgraph][simstage]")
{
    REQUIRE(SimStageEvidence().size() >= 25);
    REQUIRE(SimStageWidenings().size() >= 1);
    REQUIRE(SimResourceCensus().size() == 3);

    for (const SimEvidence& e : SimStageEvidence())
    {
        INFO(Describe(e));
        REQUIRE(e.hopCount >= 1);
        REQUIRE_FALSE(e.note.empty());
    }

    // The chains are not all one hop deep -- if they were, this gate would only be
    // re-reading `StepSimulation`, which SIM-811 already did.
    std::size_t deepest = 0;
    for (const SimEvidence& e : SimStageEvidence())
    {
        deepest = std::max(deepest, e.hopCount);
    }
    INFO("deepest recorded call chain, in hops: " << deepest);
    REQUIRE(deepest >= 9);

    // And the source really is readable from here.
    REQUIRE_FALSE(SourceOf("World/World.cpp").empty());
    REQUIRE_FALSE(SourceOf("World/WorldImpl.cpp").empty());
}

// ------------------------------------------------------------------------- liveness ---

TEST_CASE("every hop of every recorded call path still exists in the source", "[determinism][taskgraph][simstage]")
{
    for (const SimEvidence& e : SimStageEvidence())
    {
        for (std::size_t h = 0; h < e.hopCount; ++h)
        {
            const SimEvidenceHop& hop = e.hops[h];
            INFO(Describe(e) << "\n  because: " << e.note << "\n  hop " << h << " of " << e.hopCount << ": " << hop.file
                             << "  [" << hop.symbol << "]  must contain  [" << hop.contains << "]");

            const std::string& text = SourceOf(hop.file);
            REQUIRE_FALSE(text.empty());

            const BodyRange body = FindBody(text, hop.symbol);
            INFO("  the enclosing definition was " << (body.found ? "found" : "NOT FOUND"));
            REQUIRE(body.found);

            const std::string_view region{text.data() + body.begin, body.end - body.begin};
            INFO("  searched " << hop.file << ":" << body.line << " .. " << LineOf(text, body.end));
            REQUIRE(region.find(hop.contains) != std::string_view::npos);
        }
    }
}

TEST_CASE("each hop of a call path calls the next one", "[determinism][taskgraph][simstage]")
{
    std::size_t indirectHops = 0;

    for (const SimEvidence& e : SimStageEvidence())
    {
        for (std::size_t h = 0; h + 1 < e.hopCount; ++h)
        {
            const SimEvidenceHop& from = e.hops[h];
            const SimEvidenceHop& to = e.hops[h + 1];
            if (from.indirect)
            {
                ++indirectHops;
                continue;
            }

            const std::string callee = CalleeName(from.contains);
            INFO(Describe(e) << "\n  hop " << h << " calls [" << from.contains << "] -> callee '" << callee
                             << "'\n  hop " << (h + 1) << " claims to be [" << to.symbol << "]");
            REQUIRE_FALSE(callee.empty());
            REQUIRE(to.symbol.find(callee) != std::string_view::npos);
        }
    }

    // A table that quietly became all-indirect would pass the loop above by skipping it.
    INFO("indirect hops in the table: " << indirectHops);
    REQUIRE(indirectHops >= 1);
    REQUIRE(indirectHops <= 12);
}

// ------------------------------------------------------------------------- coverage ---

TEST_CASE("every declared resource bit has a call path or a named widening", "[determinism][taskgraph][simstage]")
{
    for (const SimStage stage : kSimStageOrder)
    {
        for (std::size_t r = 0; r < kSimResourceCount; ++r)
        {
            const auto resource = static_cast<SimResource>(r);
            for (const bool write : {false, true})
            {
                if ((DeclBit(stage, write) & SimResourceMask(resource)) == 0)
                {
                    continue;
                }

                const bool evidenced =
                    std::any_of(SimStageEvidence().begin(), SimStageEvidence().end(), [&](const SimEvidence& e)
                                { return e.stage == stage && e.resource == resource && e.write == write; });

                const auto widening =
                    std::find_if(SimStageWidenings().begin(), SimStageWidenings().end(), [&](const SimDeclWidening& w)
                                 { return w.stage == stage && w.resource == resource && w.write == write; });
                const bool widened = widening != SimStageWidenings().end();

                INFO("stage '" << SimStageName(stage) << "' declares that it " << (write ? "WRITES" : "reads")
                               << " resource '" << SimResourceName(resource)
                               << "'.\n"
                                  "Nothing justifies that bit: add a call path to "
                                  "SimStageEvidence.cpp, or a widening with a reason.");
                REQUIRE((evidenced || widened));

                if (widened && !evidenced)
                {
                    INFO("a widening must say WHY");
                    REQUIRE_FALSE(widening->reason.empty());
                }
            }
        }
    }
}

TEST_CASE("every recorded call path corresponds to a declared bit", "[determinism][taskgraph][simstage]")
{
    for (const SimEvidence& e : SimStageEvidence())
    {
        INFO("A call path is recorded for " << Describe(e) << "\n  because: " << e.note
                                            << "\nbut the stage does not declare that resource. The declaration in "
                                               "SimStageGraph.cpp must widen to match what the code does.");
        REQUIRE((DeclBit(e.stage, e.write) & SimResourceMask(e.resource)) != 0);
    }
}

TEST_CASE("a widening is not a substitute for evidence somebody could have found", "[determinism][taskgraph][simstage]")
{
    // Widenings are allowed, but only where nobody could write a path down. Each must
    // still be a bit the declaration actually carries -- a widening for an undeclared bit
    // is a leftover.
    for (const SimDeclWidening& w : SimStageWidenings())
    {
        INFO("widening for stage '" << SimStageName(w.stage) << "' " << (w.write ? "WRITES" : "reads") << " '"
                                    << SimResourceName(w.resource) << "': " << w.reason);
        REQUIRE_FALSE(w.reason.empty());
        REQUIRE((DeclBit(w.stage, w.write) & SimResourceMask(w.resource)) != 0);
    }
}

// --------------------------------------------------------------------------- census ---

TEST_CASE("every occurrence of a censused witness is classified", "[determinism][taskgraph][simstage]")
{
    for (const SimResourceCensusEntry& entry : SimResourceCensus())
    {
        const std::vector<Occurrence> found = Occurrences(entry.witness);

        INFO("census witness [" << entry.witness << "] for resource '" << SimResourceName(entry.resource) << "'");
        // A witness that matches nothing would make this whole case vacuous, and that is
        // exactly how a census silently stops censusing.
        REQUIRE(found.size() >= 4);

        for (const Occurrence& occ : found)
        {
            const SimCensusSite* best = nullptr;
            for (const SimCensusSite& site : entry.sites)
            {
                if (site.file != occ.file)
                {
                    continue;
                }
                if (!site.line.empty() && occ.text.find(site.line) == std::string::npos)
                {
                    continue;
                }
                // Longest match wins, so a bare `SimulateScripts();` site cannot swallow
                // the qualified `world.SimulateScripts();` one.
                if (best == nullptr || site.line.size() > best->line.size())
                {
                    best = &site;
                }
            }

            INFO("unclassified use of [" << entry.witness << "] at " << occ.file << ":" << occ.line << "\n  "
                                         << occ.text
                                         << "\nAdd it to SimResourceCensus() in SimStageEvidence.cpp: either as an "
                                            "InTick site naming the stage that reaches it, or as OutOfTick with a "
                                            "reason no stage does.");
            REQUIRE(best != nullptr);
        }
    }
}

TEST_CASE("an in-tick census site's stage declares the resource it touches", "[determinism][taskgraph][simstage]")
{
    std::size_t inTickSites = 0;

    for (const SimResourceCensusEntry& entry : SimResourceCensus())
    {
        for (const SimCensusSite& site : entry.sites)
        {
            INFO("census site " << site.file << " [" << site.line << "] -- " << site.reason);
            REQUIRE_FALSE(site.reason.empty());
            if (!site.inTick)
            {
                continue;
            }
            ++inTickSites;

            INFO("stage '" << SimStageName(site.stage) << "' reaches " << site.file << " and therefore "
                           << (site.write ? "WRITES" : "reads") << " '" << SimResourceName(entry.resource)
                           << "', but does not declare it");
            REQUIRE((DeclBit(site.stage, site.write) & SimResourceMask(entry.resource)) != 0);
        }
    }

    INFO("in-tick census sites: " << inTickSites);
    REQUIRE(inTickSites >= 6);
}

TEST_CASE("the census says which resources it does NOT cover", "[determinism][taskgraph][simstage]")
{
    // The honest half. Clock, VehicleState and AIState are touched through `Glob.time`,
    // `SetPosition`, `Brain()` and a hundred other spellings, at thousands of sites; no
    // census of them is affordable and none is claimed. For those three the declaration is
    // evidence-backed but NOT exhaustive, and a new touch will not be caught.
    const std::uint32_t censused = CensusedResources();

    REQUIRE((censused & SimResourceMask(SimResource::PhysicsState)) != 0);
    REQUIRE((censused & SimResourceMask(SimResource::Visibility)) != 0);
    REQUIRE((censused & SimResourceMask(SimResource::ScriptState)) != 0);

    REQUIRE((censused & SimResourceMask(SimResource::Clock)) == 0);
    REQUIRE((censused & SimResourceMask(SimResource::VehicleState)) == 0);
    REQUIRE((censused & SimResourceMask(SimResource::AIState)) == 0);
}

// ---------------------------------------------------------- what the audit changed ---

TEST_CASE("the corrected declarations imply the previous-tick edges SIM-814 measured",
          "[determinism][taskgraph][simstage]")
{
    // SIM-811 reported four. The audit found six more declaration bits and the set is now
    // eight. Pinned whole rather than by its interesting member, so a declaration edit
    // cannot quietly add or drop one.
    struct Expected
    {
        SimStage producer;
        SimStage consumer;
        SimResource resource;
    };

    const Expected expected[] = {
        // NEW: the wind tick reads helicopter positions written last tick.
        {SimStage::Vehicles, SimStage::Clock, SimResource::VehicleState},
        // NEW: a flare's script, run from inside the vehicle stage.
        {SimStage::Vehicles, SimStage::Scripts, SimResource::ScriptState},
        // The price of running scripts first: everything a script reads is one tick old.
        {SimStage::AI, SimStage::Scripts, SimResource::AIState},
        {SimStage::Vehicles, SimStage::Scripts, SimResource::VehicleState},
        {SimStage::Visibility, SimStage::Scripts, SimResource::Visibility},
        // The one SIM-810 flagged.
        {SimStage::Visibility, SimStage::AI, SimResource::Visibility},
        // NEW: automatic fire reads the same stale sensor results the AI does.
        {SimStage::Visibility, SimStage::Vehicles, SimResource::Visibility},
        // NEW: loose objects take their pose from a solver that has not stepped yet.
        {SimStage::Physics, SimStage::Vehicles, SimResource::PhysicsState},
    };

    const std::vector<SimStageEdge> lagged = PreviousTickEdges(SimStageSpan{kSimStageOrder});

    std::ostringstream got;
    for (const SimStageEdge& e : lagged)
    {
        got << "\n  " << SimStageName(e.producer) << " -> " << SimStageName(e.consumer) << "  ("
            << SimResourceName(e.resource) << ")";
    }
    INFO("previous-tick edges the declarations imply:" << got.str());

    REQUIRE(lagged.size() == std::size(expected));
    for (std::size_t i = 0; i < lagged.size(); ++i)
    {
        INFO("edge " << i);
        CHECK(lagged[i].producer == expected[i].producer);
        CHECK(lagged[i].consumer == expected[i].consumer);
        CHECK(lagged[i].resource == expected[i].resource);
        CHECK_FALSE(lagged[i].sameTick);
    }
}
