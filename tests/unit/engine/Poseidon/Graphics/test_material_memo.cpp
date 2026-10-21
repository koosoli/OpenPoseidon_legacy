#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Material.hpp>

using namespace Poseidon;

TEST_CASE("TexMaterial memo computes each slot once and keeps slots apart", "[graphics][material-memo]")
{
    TexMaterial material;
    int computed = 0;
    const auto five = [&]
    {
        ++computed;
        return 5;
    };
    CHECK(material.Memo(TexMaterial::MemoSectionAlphaRoute, five) == 5);
    CHECK(material.Memo(TexMaterial::MemoSectionAlphaRoute, five) == 5);
    CHECK(computed == 1);

    // A remembered zero is still remembered: "known" is its own bit, not a non-zero value.
    const auto zero = [&]
    {
        ++computed;
        return 0;
    };
    CHECK(material.Memo(TexMaterial::MemoSectionCullNone, zero) == 0);
    CHECK(material.Memo(TexMaterial::MemoSectionCullNone, zero) == 0);
    CHECK(computed == 2);

    // Neighbouring slots neither see nor disturb each other.
    CHECK(material.Memo(TexMaterial::MemoGpuEmatCullNone, [] { return 7; }) == 7);
    CHECK(material.Memo(TexMaterial::MemoSectionAlphaRoute, five) == 5);
    CHECK(material.Memo(TexMaterial::MemoSectionCullNone, zero) == 0);
    CHECK(computed == 2);
}

TEST_CASE("TexMaterial serials are unique per constructed material", "[graphics][material-memo]")
{
    TexMaterial a, b;
    CHECK(a.Serial() != 0);
    CHECK(a.Serial() != b.Serial());
}
