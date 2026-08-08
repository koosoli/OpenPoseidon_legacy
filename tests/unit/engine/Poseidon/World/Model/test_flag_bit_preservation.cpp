// test_flag_bit_preservation.cpp - AST-018: raw flag bits plus decoded flags.
//
// The property under test is that nothing is lost and nothing is guessed: a
// source word survives the round trip bit for bit, and a bit this build cannot
// name is reported as unnamed rather than silently absorbed into a known mask.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/GeometryBasis.hpp>
#include <Poseidon/World/Model/ModelComputation.hpp>
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

// AST-018: named selections must carry weighted vertices, not just membership.
TEST_CASE("MLOD named selections keep their source weight byte", "[model][selection][ast-018]")
{
    // The MLOD path read the weight byte only to decide "is this point in the
    // selection", then dropped it. Anything soft arrived hard, and the original
    // bytes could not be recovered without re-reading the file.
    const char* candidates[] = {"p3d/animated_actor.p3d", "p3d/complex_vehicle.p3d",
                               "p3d/multi_lod_vehicle.p3d", "p3d/crew_proxy.p3d"};
    size_t modelsWithSelections = 0;
    for (const char* candidate : candidates)
    {
        Poseidon::Model::Model model;
        try { model = Poseidon::Asset::Formats::MLODLoader::load(GET_FIXTURE(candidate)); }
        catch (const std::exception&) { continue; }
        for (const auto& lod : model.lodLevels)
            for (const auto& selection : lod.mesh.selections)
            {
                if (selection.vertexIndices.empty()) continue;
                ++modelsWithSelections;
                CAPTURE(candidate, selection.name);
                // Parallel to vertexIndices, or the two cannot be read together.
                REQUIRE(selection.sourceVertexWeights.size() == selection.vertexIndices.size());
                // A dropped byte reads as zero, and zero means "not selected" --
                // so a member with weight 0 is proof the byte was lost.
                for (uint8_t weight : selection.sourceVertexWeights)
                    REQUIRE(weight != 0);
            }
    }
    // Guard against the test passing because nothing was examined.
    REQUIRE(modelsWithSelections > 0);
}

TEST_CASE("The MLOD weight byte is preserved, not reinterpreted", "[model][selection][ast-018]")
{
    // vertexWeights is the linear 0-255 scale ShapeAdapter consumes. The MLOD
    // byte is not known to be on that scale, so the loader must not populate it
    // -- doing so would silently reweight every skinned MLOD model. This pins
    // the deliberate emptiness so it reads as a decision rather than an
    // oversight, and fails the day someone decodes it for real.
    Poseidon::Model::Model model;
    try { model = Poseidon::Asset::Formats::MLODLoader::load(GET_FIXTURE("p3d/animated_actor.p3d")); }
    catch (const std::exception&) { SKIP("animated_actor fixture not readable"); }

    size_t checked = 0;
    for (const auto& lod : model.lodLevels)
        for (const auto& selection : lod.mesh.selections)
        {
            if (selection.vertexIndices.empty()) continue;
            ++checked;
            REQUIRE(selection.vertexWeights.empty());
        }
    REQUIRE(checked > 0);
}

// AST-018: the explicit source-geometry basis.
TEST_CASE("The legacy winding label is derived from the typed basis", "[model][basis][ast-018]")
{
    // These three strings are what the SP3X, P3DM and ODOL 73 paths already
    // recorded. Deriving them is what stops the label and the typed record from
    // disagreeing the first time one of them is edited.
    REQUIRE(DescribeWinding(SourceWinding::ClockwiseReversedOnLoad, BasisConfidence::Established) ==
            "CW_REVERSED_ON_LOAD");
    REQUIRE(DescribeWinding(SourceWinding::ClockwiseReversedOnLoad, BasisConfidence::Assumed) ==
            "CW_REVERSED_ON_LOAD_ASSUMED");
    REQUIRE(DescribeWinding(SourceWinding::ClockwiseRelativeToStoredNormals, BasisConfidence::Measured) ==
            "CW_RELATIVE_TO_STORED_NORMALS_MEASURED");
    // An unstated winding produces no label at all rather than a plausible one.
    REQUIRE(DescribeWinding(SourceWinding::Unknown, BasisConfidence::Measured).empty());
}

TEST_CASE("An unstated basis is representable and detectable", "[model][basis][ast-018]")
{
    // The default has to be "nothing established", not a plausible-looking
    // default that a consumer would read and believe.
    GeometryBasis basis;
    REQUIRE(IsBasisUnstated(basis));
    REQUIRE(basis.winding == SourceWinding::Unknown);
    REQUIRE(basis.tangents == TangentHandedness::Unknown);
    REQUIRE_FALSE(basis.centreOfMassAtOrigin);
    REQUIRE(basis.centreOfMassConfidence == BasisConfidence::Unknown);

    basis.windingConfidence = BasisConfidence::Assumed;
    REQUIRE_FALSE(IsBasisUnstated(basis));
}

TEST_CASE("MLOD loaders state their basis with honest confidences", "[model][basis][ast-018]")
{
    Poseidon::Model::Model model;
    try { model = Poseidon::Asset::Formats::MLODLoader::load(GET_FIXTURE("p3d/animated_actor.p3d")); }
    catch (const std::exception&) { SKIP("animated_actor fixture not readable"); }
    REQUIRE_FALSE(model.lodLevels.empty());

    for (const auto& lod : model.lodLevels)
    {
        CAPTURE(lod.sourceEncoding);
        REQUIRE_FALSE(IsBasisUnstated(lod.basis));
        REQUIRE(lod.basis.winding == SourceWinding::ClockwiseReversedOnLoad);
        // Whichever encoding it is, the label must agree with the record.
        REQUIRE(lod.sourceWinding == DescribeWinding(lod.basis.winding, lod.basis.windingConfidence));
        // Neither MLOD encoding supplies a tangent frame, and saying so is the
        // point: a consumer that needs one learns it will not get it here.
        REQUIRE(lod.basis.tangents == TangentHandedness::NotSupplied);
        // Nothing has established where the centre of mass sits.
        REQUIRE(lod.basis.centreOfMassConfidence == BasisConfidence::Unknown);
    }
}

// AST-018: proxy identity, transform and selection.
TEST_CASE("MLOD proxies reach the IR with identity and frame", "[model][proxy][ast-018]")
{
    // Extraction existed but was never called, so MLOD models arrived with no
    // proxies at all -- nothing attached, no weapons, no crew.
    const char* candidates[] = {"p3d/simple_proxy.p3d", "p3d/proxy_structure.p3d", "p3d/crew_proxy.p3d"};
    size_t proxiesSeen = 0;
    for (const char* candidate : candidates)
    {
        Poseidon::Model::Model model;
        try { model = Poseidon::Asset::Formats::MLODLoader::load(GET_FIXTURE(candidate)); }
        catch (const std::exception&) { continue; }
        for (const auto& lod : model.lodLevels)
            for (const auto& proxy : lod.mesh.proxies)
            {
                ++proxiesSeen;
                CAPTURE(candidate, proxy.name, proxy.id);
                // The prefix and the ".NN" id are not part of the model name;
                // ShapeAdapter feeds this string straight to NewProxyObject.
                REQUIRE(proxy.name.find("proxy:") == std::string::npos);
                REQUIRE(proxy.name.find('.') == std::string::npos);
                REQUIRE_FALSE(proxy.name.empty());
                // The selection it came from has to stay referenced.
                REQUIRE(proxy.selectionIndex < lod.mesh.selections.size());
                REQUIRE(lod.mesh.selections[proxy.selectionIndex].name.find("proxy:") == 0);

                // An orthonormal frame, or the attached object is skewed.
                const auto& m = proxy.transform.m;
                for (int row = 0; row < 3; ++row)
                {
                    const float length = std::sqrt(m[row][0] * m[row][0] + m[row][1] * m[row][1] +
                                                   m[row][2] * m[row][2]);
                    CAPTURE(row, length);
                    REQUIRE(length > 0.99f);
                    REQUIRE(length < 1.01f);
                }
                for (int a = 0; a < 3; ++a)
                    for (int b = a + 1; b < 3; ++b)
                    {
                        const float dot = m[a][0] * m[b][0] + m[a][1] * m[b][1] + m[a][2] * m[b][2];
                        CAPTURE(a, b, dot);
                        REQUIRE(std::abs(dot) < 1e-3f);
                    }
            }
    }
    REQUIRE(proxiesSeen > 0);
}

TEST_CASE("The proxy frame does not depend on selection vertex order", "[model][proxy][ast-018]")
{
    // Why the canonical ordering exists. MLOD gathers selection membership by
    // iterating point indices, so the order the three vertices arrive in is an
    // artefact of point numbering. Permuting them must not move the frame.
    std::vector<Vertex> vertices(3);
    vertices[0].position = Poseidon::Model::Vector3(1.0f, 2.0f, 3.0f);          // apex
    vertices[1].position = Poseidon::Model::Vector3(1.5f, 2.0f, 3.0f);          // nearest: direction
    vertices[2].position = Poseidon::Model::Vector3(1.0f, 2.0f, 4.0f);          // second: up
    const uint32_t order[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};

    Matrix4x3 reference;
    for (int permutation = 0; permutation < 6; ++permutation)
    {
        NamedSelection selection("proxy:test.01");
        for (int i = 0; i < 3; ++i) selection.vertexIndices.push_back(order[permutation][i]);
        const auto transform = ModelComputation::computeProxyTransform(vertices, selection);
        if (permutation == 0) { reference = transform; continue; }
        CAPTURE(permutation);
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
                REQUIRE(std::abs(transform.m[row][column] - reference.m[row][column]) < 1e-4f);
    }
    // The apex is the origin of the frame, whatever order it arrived in.
    REQUIRE(reference.m[0][3] == 1.0f);
    REQUIRE(reference.m[1][3] == 2.0f);
    REQUIRE(reference.m[2][3] == 3.0f);
}
