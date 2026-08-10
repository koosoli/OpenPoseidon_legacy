#include <catch2/catch_test_macros.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

using namespace Poseidon;

// Which virtual root a bank mounts at. The rule has to serve two generations at
// once: OFP-era archives carry no header-extension block and are addressed by
// filename, while Arma 3 archives declare their own root and it is almost never
// the filename.
TEST_CASE("Bank mount: an archive's declared prefix wins over its filename", "[IO][Banks]")
{
    // rocks_f.pbo declares a3\rocks_f. Mounting it as "rocks_f" makes every path
    // a world references -- all of which begin a3\rocks_f\ -- unresolvable.
    REQUIRE(ResolveBankMountName("a3\\rocks_f", "addons\\", "rocks_f", true) == RString("a3\\rocks_f"));

    // anims_f_data.pbo declares a3\anims_f\data: a multi-segment root that no
    // rule based on the filename could produce, whatever the separator.
    REQUIRE(ResolveBankMountName("a3\\anims_f\\data", "addons\\", "anims_f_data", true) ==
            RString("a3\\anims_f\\data"));
}

TEST_CASE("Bank mount: an archive without a declaration keeps the derived name", "[IO][Banks]")
{
    // The OFP-era path, unchanged. Measured across the local CWA retail and Demo
    // installs, no archive declares a prefix that differs from its filename, so
    // this is the branch all of that content still takes.
    REQUIRE(ResolveBankMountName("", "addons\\", "data3d", true) == RString("data3d"));
    REQUIRE(ResolveBankMountName("", "Campaigns\\", "1985", false) == RString("Campaigns\\1985"));
}

TEST_CASE("Bank mount: a declaration that could escape the namespace is refused", "[IO][Banks]")
{
    // The declaration comes out of a file and becomes the prefix that lookups
    // resolve against, so a traversal or a drive letter falls back to the
    // caller-derived name rather than being mounted.
    REQUIRE(ResolveBankMountName("..\\..\\windows", "addons\\", "evil", true) == RString("evil"));
    REQUIRE(ResolveBankMountName("C:\\windows", "addons\\", "evil", true) == RString("evil"));
    REQUIRE(ResolveBankMountName("a3\\..\\..\\x", "addons\\", "evil", true) == RString("evil"));

    // Length is bounded for the same reason: SetPrefix copies into a fixed buffer.
    RString overlong;
    for (int i = 0; i < 50; ++i)
        overlong = overlong + RString("abcdef");
    REQUIRE(overlong.GetLength() > 240);
    REQUIRE(ResolveBankMountName(overlong, "addons\\", "evil", true) == RString("evil"));
}
