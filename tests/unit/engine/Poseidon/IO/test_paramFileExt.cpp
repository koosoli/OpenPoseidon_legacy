// Regression: mission description.ext entries like `sound[] = {"sound/s01r01.ogg", ...}`
// resolved to `sound\sound/s01r01.ogg` (doubled prefix) and failed in FileCache.
// Without the fix, GetSoundName/GetPictureName below produce "sound\sound/..." /
// "data\pic.paa" -> "data\data\pic.paa". Tolerances are exact-string equality.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <string.h>
#include <Poseidon/Foundation/Strings/RString.hpp>

TEST_CASE("GetDefaultName preserves an existing dir prefix", "[io][paramFileExt]")
{
    SECTION("bare name gets the default dir prepended")
    {
        RString r = GetDefaultName("s01r01", "sound\\", ".wss");
        REQUIRE(strcmp((const char*)r, "sound\\s01r01.wss") == 0);
    }

    SECTION("name with matching forward-slash dir prefix is left alone")
    {
        RString r = GetDefaultName("sound/s01r01.ogg", "sound\\", ".wss");
        REQUIRE(strcmp((const char*)r, "sound/s01r01.ogg") == 0);
    }

    SECTION("name with matching backslash dir prefix is left alone")
    {
        RString r = GetDefaultName("sound\\s01r01.ogg", "sound\\", ".wss");
        REQUIRE(strcmp((const char*)r, "sound\\s01r01.ogg") == 0);
    }

    SECTION("matching prefix is detected case-insensitively")
    {
        RString r = GetDefaultName("SOUND/s01r01.ogg", "sound\\", ".wss");
        REQUIRE(strcmp((const char*)r, "sound/s01r01.ogg") == 0);
    }

    SECTION("leading slash strips and skips the dir prefix")
    {
        RString r = GetDefaultName("\\anim\\walk.rtm", "anim\\", ".rtm");
        REQUIRE(strcmp((const char*)r, "anim\\walk.rtm") == 0);
    }

    SECTION("missing extension is appended")
    {
        RString r = GetDefaultName("walk", "anim\\", ".rtm");
        REQUIRE(strcmp((const char*)r, "anim\\walk.rtm") == 0);
    }

    SECTION("works for the data\\ picture dir too")
    {
        RString r = GetDefaultName("data/pic.paa", "data\\", ".paa");
        REQUIRE(strcmp((const char*)r, "data/pic.paa") == 0);
    }
}

TEST_CASE("GetSoundName accepts mission-relative sound/ paths", "[io][paramFileExt]")
{
    // Real-world case: Missions/01TakeTheCar.ABEL/description.ext writes
    //   sound[] = {"sound/s01r01.ogg", db-10, 1.0};
    // Pre-fix this resolved to "sound\\sound/s01r01.ogg".
    RString r = GetSoundName("sound/s01r01.ogg");
    REQUIRE(strcmp((const char*)r, "sound/s01r01.ogg") == 0);
}

TEST_CASE("SelectMenuInitWorld falls back to demo world when configured init terrain is absent", "[io][paramFileExt]")
{
    SECTION("full package keeps initWorld when its terrain exists")
    {
        RString r = SelectMenuInitWorld("Intro", "Demo", false, true, true);
        REQUIRE(strcmp((const char*)r, "Intro") == 0);
    }

    SECTION("full binary on stripped demo package falls back to demoWorld")
    {
        RString r = SelectMenuInitWorld("Intro", "Demo", false, false, true);
        REQUIRE(strcmp((const char*)r, "Demo") == 0);
    }

    SECTION("demo binary still explicitly prefers demoWorld")
    {
        RString r = SelectMenuInitWorld("Intro", "Demo", true, true, true);
        REQUIRE(strcmp((const char*)r, "Demo") == 0);
    }

    SECTION("missing demoWorld keeps the configured initWorld")
    {
        RString r = SelectMenuInitWorld("Intro", "", false, false, false);
        REQUIRE(strcmp((const char*)r, "Intro") == 0);
    }
}

// Regression: proxy model names carrying an addon path (Arma 2 / DayZ / Reforger-era data)
// were given the OFP model folder as a prefix -- `dz\structures\furniture\beds\postel_panelak1`
// became `data3d\dz\structures\...`, which resolves to nothing. Proxies are how those houses
// carry their furniture, so the shell of every building rendered and the inside was empty, with
// nothing but a FileCache warning to say so. `data3d\` is flat, so a name that already has a
// subdirectory in it is by construction not a name in that folder.
TEST_CASE("GetShapeName does not prefix addon-rooted model paths", "[io][paramFileExt]")
{
    SECTION("bare OFP model name still gets the model folder and the extension")
    {
        RString r = GetShapeName("flag");
        REQUIRE(strcmp((const char*)r, "data3d\\flag.p3d") == 0);
    }

    SECTION("bare name that already has the extension keeps it")
    {
        RString r = GetShapeName("blok.p3d");
        REQUIRE(strcmp((const char*)r, "data3d\\blok.p3d") == 0);
    }

    SECTION("addon path with no leading slash is left rooted where it is")
    {
        RString r = GetShapeName("dz\\structures\\furniture\\beds\\postel_panelak1.p3d");
        REQUIRE(strcmp((const char*)r, "dz\\structures\\furniture\\beds\\postel_panelak1.p3d") == 0);
    }

    SECTION("forward slashes count as a path just as backslashes do")
    {
        RString r = GetShapeName("ca/buildings/misc/bed");
        REQUIRE(strcmp((const char*)r, "ca/buildings/misc/bed.p3d") == 0);
    }

    SECTION("a leading slash still means fully qualified, as it always did")
    {
        RString r = GetShapeName("\\dz\\structures\\proxy\\tent_table.p3d");
        REQUIRE(strcmp((const char*)r, "dz\\structures\\proxy\\tent_table.p3d") == 0);
    }
}
