// test_flag_bit_preservation.cpp - AST-018: raw flag bits plus decoded flags.
//
// The property under test is that nothing is lost and nothing is guessed: a
// source word survives the round trip bit for bit, and a bit this build cannot
// name is reported as unnamed rather than silently absorbed into a known mask.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ModelFlags.hpp>
#include "../../test_fixtures.hpp"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace Poseidon::Model;

TEST_CASE("A flag word survives the IR bit for bit", "[model][flags][ast-018]")
{
    // Including bits no enum names. Dropping them at load would make them
    // unrecoverable; the roadmap requires they stay available.
    for (uint32_t raw : {0x00000000u, 0x0000003Fu, 0xFFFFFFFFu, 0x0A0B0C0Du})
    {
        CAPTURE(raw);
        REQUIRE(ToUint32(VertexFlagsFromUint32(raw)) == raw);
        REQUIRE(ToUint32(FaceFlagsFromUint32(raw)) == raw);
    }

    Vertex vertex;
    vertex.flags = VertexFlagsFromUint32(0x08000001u);
    REQUIRE(ToUint32(vertex.flags) == 0x08000001u);

    Triangle triangle;
    triangle.flags = FaceFlagsFromUint32(0x00400001u);
    REQUIRE(ToUint32(triangle.flags) == 0x00400001u);
}

TEST_CASE("Named bits are reported as known", "[model][flags][ast-018]")
{
    REQUIRE_FALSE(HasUnknownBits(VertexFlags::ClipFront));
    REQUIRE_FALSE(HasUnknownBits(VertexFlags::LandOn));
    REQUIRE_FALSE(HasUnknownBits(VertexFlags::SpecialHidden));
    // The ODOL 73 rectangle fixture's clip word: all six clip planes set.
    REQUIRE_FALSE(HasUnknownBits(VertexFlagsFromUint32(63u)));

    REQUIRE_FALSE(HasUnknownBits(FaceFlags::NoLight));
    REQUIRE_FALSE(HasUnknownBits(FaceFlags::BeginStrip));
    REQUIRE_FALSE(HasUnknownBits(FaceFlags::DisableTexMerge));
}

TEST_CASE("User-defined ranges count as known", "[model][flags][ast-018]")
{
    // A user bit is a bit the format reserves for content. Reporting it as
    // unknown would make every model with one look unreadable.
    REQUIRE_FALSE(HasUnknownBits(VertexFlagsFromUint32(static_cast<uint32_t>(VertexFlags::UserMask))));
    REQUIRE_FALSE(HasUnknownBits(FaceFlagsFromUint32(static_cast<uint32_t>(FaceFlags::UserMask))));
}

TEST_CASE("An unnamed bit is reported, not absorbed", "[model][flags][ast-018]")
{
    // Face bit 0x40 is named by nothing in FaceFlags. It must not be swallowed
    // by LightMask (0x3000A7) just because it sits near it.
    REQUIRE(HasUnknownBits(FaceFlagsFromUint32(0x40u)));
    REQUIRE(UnknownBits(FaceFlagsFromUint32(0x40u)) == 0x40u);

    // Mixed: a known lighting bit alongside an unnamed one reports only the
    // unnamed one, so a caller can log exactly what it did not understand.
    const uint32_t mixed = static_cast<uint32_t>(FaceFlags::NoLight) | 0x40u;
    REQUIRE(UnknownBits(FaceFlagsFromUint32(mixed)) == 0x40u);

    // Vertex bit 0x80 sits between ClipUser (0x40) and LandMask (0xF00) and is
    // named by nothing; so does the whole top nibble above SpecialMask.
    REQUIRE(UnknownBits(VertexFlagsFromUint32(0x80u)) == 0x80u);
    REQUIRE(UnknownBits(VertexFlagsFromUint32(0x80000000u)) == 0x80000000u);
    // Note: VertexFlags::UserMask (0xFF0000) overlaps LightMask (0xF0000), so
    // 0x100000 is a user bit rather than an unnamed one. That overlap comes from
    // data3d.h and is left alone here -- this test records it, it does not
    // endorse it.
    REQUIRE_FALSE(HasUnknownBits(VertexFlagsFromUint32(0x100000u)));
}

TEST_CASE("The known masks cover every named enumerator", "[model][flags][ast-018]")
{
    // The masks are hand-written unions, so they can drift from the enum. This
    // fails if an enumerator is added without extending the mask.
    for (VertexFlags value : {VertexFlags::ClipFront, VertexFlags::ClipBack, VertexFlags::ClipLeft,
                              VertexFlags::ClipRight, VertexFlags::ClipBottom, VertexFlags::ClipTop,
                              VertexFlags::ClipUser, VertexFlags::LandOn, VertexFlags::LandUnder,
                              VertexFlags::LandAbove, VertexFlags::LandKeep, VertexFlags::DecalNormal,
                              VertexFlags::DecalVertical, VertexFlags::FogDisable, VertexFlags::FogSky,
                              VertexFlags::FogShadow, VertexFlags::LightSky, VertexFlags::LightCloud,
                              VertexFlags::LightSun, VertexFlags::LightHalf, VertexFlags::SpecialHidden})
    {
        CAPTURE(ToUint32(value));
        REQUIRE_FALSE(HasUnknownBits(value));
    }

    for (FaceFlags value : {FaceFlags::NoLight, FaceFlags::Ambient, FaceFlags::FullLight,
                            FaceFlags::BothSidesLight, FaceFlags::SkyLight, FaceFlags::ReverseLight,
                            FaceFlags::FlatLight, FaceFlags::IsShadow, FaceFlags::NoShadow,
                            FaceFlags::ZBiasStep, FaceFlags::BeginFan, FaceFlags::BeginStrip,
                            FaceFlags::ContinueFan, FaceFlags::ContinueStrip, FaceFlags::DisableTexMerge})
    {
        CAPTURE(ToUint32(value));
        REQUIRE_FALSE(HasUnknownBits(value));
    }
}

// A capability nothing calls is a claim, not a fact. This walks the shipped P3D
// fixtures and reports what the split actually finds, so the answer for this
// corpus is recorded rather than assumed.
TEST_CASE("The shipped P3D corpus is surveyed for unnamed flag bits", "[model][flags][ast-018][corpus]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::path(GET_FIXTURE("p3d/flat_quad.p3d")).parent_path();
    if (!fs::exists(root))
        SKIP("P3D fixture directory not found");

    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(root))
        if (entry.is_regular_file() && entry.path().extension() == ".p3d")
            files.push_back(entry.path().string());
    std::sort(files.begin(), files.end());
    REQUIRE_FALSE(files.empty());

    size_t surveyed = 0, withUnknownVertexBits = 0, withUnknownFaceBits = 0;
    uint32_t vertexBitsSeen = 0, faceBitsSeen = 0;
    for (const auto& file : files)
    {
        Poseidon::Model::Model model;
        try
        {
            // Either container is fine; the flag words are the same either way.
            model = Poseidon::Asset::Formats::MLODLoader::load(file);
        }
        catch (const std::exception&)
        {
            try { model = Poseidon::Asset::Formats::ODOLLoader::load(file); }
            catch (const std::exception&) { continue; }
        }
        ++surveyed;
        bool vertexHit = false, faceHit = false;
        for (const auto& lod : model.lodLevels)
        {
            for (const auto& vertex : lod.mesh.vertices)
                if (UnknownBits(vertex.flags)) { vertexHit = true; vertexBitsSeen |= UnknownBits(vertex.flags); }
            for (const auto& triangle : lod.mesh.triangles)
                if (UnknownBits(triangle.flags)) { faceHit = true; faceBitsSeen |= UnknownBits(triangle.flags); }
            for (const auto& quad : lod.mesh.quads)
                if (UnknownBits(quad.flags)) { faceHit = true; faceBitsSeen |= UnknownBits(quad.flags); }
        }
        if (vertexHit) ++withUnknownVertexBits;
        if (faceHit) ++withUnknownFaceBits;
    }

    CAPTURE(files.size(), surveyed, withUnknownVertexBits, withUnknownFaceBits, vertexBitsSeen, faceBitsSeen);
    // Every fixture must actually have been read. A survey that quietly skipped
    // the files it could not load would report a clean corpus by finding nothing
    // -- which is the failure mode a survey is most likely to have.
    REQUIRE(surveyed == files.size());
    // The recorded result for this corpus. If a fixture is added that carries a
    // bit this build cannot name, this fails and CAPTURE prints exactly which --
    // which is the point: an unnamed bit should be noticed, not absorbed.
    REQUIRE(vertexBitsSeen == 0u);
    REQUIRE(faceBitsSeen == 0u);
}
