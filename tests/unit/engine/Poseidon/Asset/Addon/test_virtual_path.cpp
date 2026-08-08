// test_virtual_path.cpp - AST-015: canonical Real Virtuality virtual paths.
//
// The same logical resource is spelled inconsistently across a real corpus: mixed
// case, either slash, sometimes a leading separator. AST-007 found 1,560 models
// referencing `ca\...` and 30 referencing `a3\...`, and anything keying on the raw
// string treats those spellings as different resources.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Addon/VirtualPath.hpp>
#include <string>
#include <unordered_set>

using Poseidon::Asset::AddonNamespace;
using Poseidon::Asset::VirtualPath;

TEST_CASE("VirtualPath: spellings of one resource canonicalise together", "[asset][virtualpath][ast-015]")
{
    const std::string expected = "ca\\air\\data\\ataka_co.paa";
    for (const char* spelling : {
             "ca\\air\\data\\ataka_co.paa",
             "CA\\Air\\Data\\Ataka_CO.paa",     // mixed case is normal in RVMATs
             "ca/air/data/ataka_co.paa",        // forward slashes appear in configs
             "\\ca\\air\\data\\ataka_co.paa",   // optional leading root separator
             "ca\\\\air\\data\\ataka_co.paa",   // doubled separator
             "ca\\air\\data\\ataka_co.paa\\",   // trailing separator
         })
    {
        REQUIRE(VirtualPath::Parse(spelling).canonical() == expected);
    }
}

TEST_CASE("VirtualPath: the original spelling survives for diagnostics", "[asset][virtualpath][ast-015]")
{
    // A message quoting only the lowercased form makes a case-sensitivity problem
    // in someone else's tooling invisible in the log.
    auto path = VirtualPath::Parse("CA\\Air\\Data\\Ataka_CO.paa");
    REQUIRE(path.canonical() == "ca\\air\\data\\ataka_co.paa");
    REQUIRE(path.original() == "CA\\Air\\Data\\Ataka_CO.paa");
}

TEST_CASE("VirtualPath: identity is usable as a key", "[asset][virtualpath][ast-015]")
{
    std::unordered_set<VirtualPath, VirtualPath::Hash> seen;
    seen.insert(VirtualPath::Parse("ca\\air\\x.p3d"));
    seen.insert(VirtualPath::Parse("CA/Air/X.P3D"));
    REQUIRE(seen.size() == 1);
}

TEST_CASE("VirtualPath: prefix and extension", "[asset][virtualpath][ast-015]")
{
    auto path = VirtualPath::Parse("a3\\plants_f\\tree\\data\\t_pinusp3s_f_mlod.rvmat");
    REQUIRE(path.prefix() == "a3");
    REQUIRE(path.extension() == ".rvmat");

    // A dot in a directory name must not be mistaken for an extension.
    REQUIRE(VirtualPath::Parse("ca\\my.addon\\model").extension().empty());
    REQUIRE(VirtualPath::Parse("ca").prefix() == "ca");
    REQUIRE(VirtualPath::Parse("").empty());
    REQUIRE(VirtualPath::Parse("\\\\\\").empty());
}

TEST_CASE("VirtualPath: source references map to the shipped resource", "[asset][virtualpath][ast-015]")
{
    // RVMAT stages name the artist's .tga, which never ships. Verified against the
    // Arma 3 install: t_PinusP3s_F.p3d's dependencies resolve only after this
    // substitution, so a resolver without it reports every texture as missing.
    REQUIRE(VirtualPath::Parse("ca\\air\\data\\ataka_co.tga").shipped().canonical() ==
            "ca\\air\\data\\ataka_co.paa");
    REQUIRE(VirtualPath::Parse("a3\\x\\y.tiff").shipped().canonical() == "a3\\x\\y.paa");

    // Anything already shipped, or not an image, is left alone -- rewriting a .p3d
    // into a .paa would turn a model reference into a texture that cannot exist.
    REQUIRE(VirtualPath::Parse("ca\\air\\data\\ataka_co.paa").shipped().canonical() ==
            "ca\\air\\data\\ataka_co.paa");
    REQUIRE(VirtualPath::Parse("ca\\air\\ataka.p3d").shipped().canonical() == "ca\\air\\ataka.p3d");
    REQUIRE(VirtualPath::Parse("ca\\air\\data\\x.rvmat").shipped().canonical() == "ca\\air\\data\\x.rvmat");
}

TEST_CASE("AddonNamespace: longest declared prefix wins", "[asset][virtualpath][ast-015]")
{
    // Prefixes are hierarchical in the real corpus: takistan_data.pbo declares
    // `ca\takistan\data` while takistan.pbo declares `ca\takistan`. Matching on the
    // first component alone would send every data path to the wrong archive.
    AddonNamespace ns;
    REQUIRE(ns.Declare("ca\\takistan", "takistan.pbo"));
    REQUIRE(ns.Declare("ca\\takistan\\data", "takistan_data.pbo"));

    REQUIRE(ns.Owner(VirtualPath::Parse("ca\\takistan\\data\\layers\\x.paa")) == "takistan_data.pbo");
    REQUIRE(ns.Owner(VirtualPath::Parse("ca\\takistan\\other\\x.paa")) == "takistan.pbo");
    REQUIRE(ns.Owner(VirtualPath::Parse("a3\\plants_f\\x.paa")).empty());
}

TEST_CASE("AddonNamespace: a clashing prefix is recorded, not silently rebound", "[asset][virtualpath][ast-015]")
{
    // Letting a later declaration win makes which archive a path resolves to depend
    // on scan order -- a difference that stays invisible until content is wrong.
    AddonNamespace ns;
    REQUIRE(ns.Declare("ca\\air", "air.pbo"));
    REQUIRE_FALSE(ns.Declare("CA/Air", "air_replacement.pbo"));

    REQUIRE(ns.Owner(VirtualPath::Parse("ca\\air\\x.p3d")) == "air.pbo");
    REQUIRE(ns.collisions().size() == 1);
    REQUIRE(ns.collisions()[0].prefix == "ca\\air");
    REQUIRE(ns.collisions()[0].existingSource == "air.pbo");
    REQUIRE(ns.collisions()[0].newSource == "air_replacement.pbo");

    // Re-declaring the same prefix with the same source is not a clash: scanning
    // the same archive twice is ordinary, and reporting it as a conflict would
    // bury the real ones.
    REQUIRE_FALSE(ns.Declare("ca\\air", "air.pbo"));
    REQUIRE(ns.collisions().size() == 1);
}

TEST_CASE("VirtualPath: a leaked host path is not an addon prefix", "[asset][virtualpath][ast-015]")
{
    // Real references in the indexed corpus, baked in from a developer's drive.
    // Treating the drive letter as an addon prefix makes the engine report a
    // missing addon named "p" rather than a bad reference.
    REQUIRE(VirtualPath::Parse("P:\\ca\\ca_e\\data\\default.rvmat").looksHostAbsolute());
    REQUIRE(VirtualPath::Parse("J:/bistudio/_helpers/x_co.tga").looksHostAbsolute());

    REQUIRE_FALSE(VirtualPath::Parse("ca\\air\\data\\ataka_co.paa").looksHostAbsolute());
    REQUIRE_FALSE(VirtualPath::Parse("a3\\plants_f\\x.paa").looksHostAbsolute());
}
