// test_odol40_material_layout.cpp -- AST-020, the negative controls.
//
// test_odol40_arma1.cpp proves that the CORRECT embedded-material layout parses.
// This file proves that the two WRONG readings the investigation actually held do
// not, which is the part that stops the question being reopened.
//
// The field in question is the version-9 embedded material, and it was the last
// unresolved piece of revision 40. The reference parser appeared to show that a
// separate `texGens` count was present on some version-9 materials and absent on
// others, which cannot both be true of one version. It was neither: the reading
// carried TWO compensating errors.
//
//   wrong reading A -- an extra 4-byte field after `version`
//   wrong reading B -- no independent `texGens` count at version 9
//
// Each alone desynchronises. Together they are equal and opposite in size -- the
// extra field eats four bytes and the missing count gives four back -- so a
// reference parser holding both consumed the right NUMBER of bytes and closed
// many LODs, diverging only where the displaced colour block made it pick up a
// different stage count. That is why the field appeared to be present on some
// version-9 materials and absent on others, and it is exactly the class of defect
// a boundary check catches and eyeballing does not. Measured by config search
// over 120 A1 models, 1,039 LODs:
//
//   extra field, texGens count      737 of 1039 LODs closed
//   extra field, no texGens count  1029 of 1039
//   NO extra field, texGens count  1039 of 1039   <- the layout
//   no extra field, no count        632 of 1039
//
// The sentinel after the material is what makes each case decisive: a reading
// that ends anywhere but exactly at the end of the material fails to find it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol40.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace Poseidon::Asset::Formats;
namespace P3D = Poseidon::Asset::Formats::P3D;

namespace
{

struct Bytes
{
    std::vector<char> data;
    void raw(const void* p, size_t n)
    {
        const char* c = static_cast<const char*>(p);
        data.insert(data.end(), c, c + n);
    }
    void u8(uint8_t v) { raw(&v, 1); }
    void u32(uint32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void asciiz(const char* s) { raw(s, std::strlen(s) + 1); }
};

// One version-9 embedded material followed by a 0x7f sentinel.
//
// `stages` is deliberately variable. The two errors are equal and opposite in
// SIZE at every stage count -- the extra field adds four bytes, the missing
// texGens count removes four, neither scaling with the stage count -- so length
// alone never separates the layouts. What separates them is where the stage count
// itself is read from, and that only becomes observable once a material has a
// stage count other than the one a shifted reader would happen to pick up. Varying
// it is what stops a single-stage fixture from certifying the wrong layout.
std::vector<char> Version9Material(uint32_t stages, bool extraFieldAfterVersion, bool separateTexGenCount)
{
    Bytes b;
    b.u32(1); // one material
    b.asciiz("ca\\sara\\data\\test.rvmat");
    b.u32(9);
    if (extraFieldAfterVersion)
        b.u32(0);
    // Six colours then the specular power. Distinct values so a window shifted by
    // one float is visible in a debugger rather than being all zeros.
    for (int i = 0; i < 24; ++i)
        b.f32(static_cast<float>(i));
    b.f32(20.0f);
    b.u32(0);
    b.u32(0);
    b.u32(0);
    b.u32(0);
    b.asciiz("");
    b.u32(0);
    b.u32(0);
    b.u32(stages);
    if (separateTexGenCount)
        b.u32(stages);
    for (uint32_t stage = 0; stage < stages; ++stage)
    {
        b.u32(0);
        b.asciiz("ca\\sara\\data\\pisek_detail_co.paa");
        b.u32(stage);
    }
    for (uint32_t stage = 0; stage < stages; ++stage)
    {
        b.u32(7);
        for (int i = 0; i < 12; ++i)
            b.f32(0.0f);
    }
    b.u8(0x7f);
    return b.data;
}

// Reads the material and reports whether the stream ended exactly on the sentinel.
bool ParsesToSentinel(const std::vector<char>& data, size_t expectedStages)
{
    QIStream stream(const_cast<char*>(data.data()), static_cast<int>(data.size()));
    BinaryReader reader(stream);
    try
    {
        const auto materials = P3D::ReadOdol73EmbeddedMaterials(reader);
        if (materials.size() != 1 || materials[0].version != 9)
            return false;
        if (materials[0].stageTextures.size() != expectedStages)
            return false;
        if (materials[0].texGens.size() != expectedStages)
            return false;
        return reader.read<uint8_t>() == 0x7f;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

} // namespace

TEST_CASE("ODOL 40: the version-9 material has no extra field and does carry a texGens count", "[p3d][odol][ast-020]")
{
    // The layout, at one, two and four stages. Sahrani's terrain-adjacent props
    // span that range and the corpus survey covers the rest.
    for (const uint32_t stages : {1u, 2u, 4u})
    {
        INFO("stages " << stages);
        REQUIRE(ParsesToSentinel(Version9Material(stages, /*extra=*/false, /*texGenCount=*/true), stages));
    }
}

TEST_CASE("ODOL 40: an extra field after the material version is refused", "[p3d][odol][ast-020]")
{
    // Wrong reading A on its own. It shifts the whole material by four bytes, so
    // the stage count is read out of the colour block.
    for (const uint32_t stages : {1u, 2u, 4u})
    {
        INFO("stages " << stages);
        REQUIRE_FALSE(ParsesToSentinel(Version9Material(stages, /*extra=*/true, /*texGenCount=*/true), stages));
    }
}

TEST_CASE("ODOL 40: dropping the version-9 texGens count is refused", "[p3d][odol][ast-020]")
{
    // Wrong reading B on its own.
    for (const uint32_t stages : {1u, 2u, 4u})
    {
        INFO("stages " << stages);
        REQUIRE_FALSE(ParsesToSentinel(Version9Material(stages, /*extra=*/false, /*texGenCount=*/false), stages));
    }
}

TEST_CASE("ODOL 40: both wrong readings together are still refused", "[p3d][odol][ast-020]")
{
    // Why the field looked self-contradictory for a whole session, stated as a
    // length identity rather than as a claim about this reader.
    //
    // At ONE stage the two errors are the same size in opposite directions: the
    // extra field adds four bytes, the missing texGens count removes four. A
    // reference parser carrying both therefore consumed exactly the right number
    // of bytes on single-stage materials and closed the LOD, and only diverged
    // where a material had a different stage count -- which is precisely the
    // "present on some materials, absent on others" that could not be true.
    //
    // The identity is asserted because it is the diagnosis. It also says why a
    // fixture pinned at one stage would be worthless here: it cannot separate the
    // two layouts by length at all.
    const auto oneStageWrong = Version9Material(1, /*extra=*/true, /*texGenCount=*/false);
    const auto oneStageRight = Version9Material(1, /*extra=*/false, /*texGenCount=*/true);
    REQUIRE(oneStageWrong.size() == oneStageRight.size());
    for (const uint32_t stages : {2u, 4u})
    {
        INFO("stages " << stages);
        REQUIRE(Version9Material(stages, true, false).size() == Version9Material(stages, false, true).size());
    }

    // Equal length is not equal content: the extra zero word displaces the whole
    // colour block, so this reader -- which holds the measured layout -- refuses
    // the doubly-wrong bytes at every stage count, one included.
    for (const uint32_t stages : {1u, 2u, 4u})
    {
        INFO("stages " << stages);
        REQUIRE_FALSE(ParsesToSentinel(Version9Material(stages, /*extra=*/true, /*texGenCount=*/false), stages));
    }
}

TEST_CASE("ODOL 40: revision 40 is dispatched, not merely recognised", "[p3d][odol][ast-020]")
{
    const auto info = P3D::DescribeOdolRevision(40);
    REQUIRE(info.version == 40);
    REQUIRE(info.support == P3D::OdolSupport::NarrowSubset);
    REQUIRE(std::string(info.generation) == "Armed Assault (Arma 1)");
}
