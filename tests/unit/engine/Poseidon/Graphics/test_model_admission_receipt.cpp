#include <catch2/catch_test_macros.hpp>
#include <WgpuRenderer/ModelAdmissionReceipt.hpp>
#include <array>
#include <cstdint>
#include <vector>

using Poseidon::render::ModelAdmissionReceipt;

namespace
{
struct Section { uint64_t mesh; };
struct Lod { uint32_t section_base, section_count; };
constexpr std::array<uint64_t, 2> expected{11, 22};
const auto live = [](uint64_t mesh) { return mesh == 11 || mesh == 22; };
}

TEST_CASE("Selected model receipt matches the entire bounded section graph before translation",
    "[model-admission][receipt]")
{
    ModelAdmissionReceipt receipt(0, 7, 8, 9, expected); // producer model zero is valid
    REQUIRE(receipt.ValidFor(0));
    CHECK_FALSE(receipt.ValidFor(1));
    CHECK(receipt.OwnerEpoch() == 7);
    CHECK(receipt.SourceAdmissionEpoch() == 8);
    CHECK(receipt.RequestId() == 9);
    const std::vector<Section> sections{{11}, {22}, {11}};
    const std::vector<Lod> lods{{0, 2}, {2, 1}};
    CHECK(receipt.MatchesBeforeTranslation(0, sections, lods, live));
    CHECK_FALSE(receipt.MatchesBeforeTranslation(1, sections, lods, live));
    CHECK_FALSE(receipt.MatchesBeforeTranslation(0, sections, lods,
        [](uint64_t mesh) { return mesh == 11; })); // missing renderer mesh
    CHECK_FALSE(receipt.MatchesBeforeTranslation(0, std::vector<Section>{{11}, {11}},
        std::vector<Lod>{{0, 2}}, live)); // expected mesh 22 absent
    CHECK_FALSE(receipt.MatchesBeforeTranslation(0, std::vector<Section>{{11}, {22}, {33}},
        lods, [](uint64_t) { return true; })); // unexpected live mesh
    CHECK_FALSE(receipt.MatchesBeforeTranslation(0, sections,
        std::vector<Lod>{{0, 2}}, live)); // orphan section
    CHECK_FALSE(receipt.MatchesBeforeTranslation(0, sections,
        std::vector<Lod>{{0, 2}, {1, 2}}, live)); // overlapping LOD ranges
    CHECK_FALSE(receipt.MatchesBeforeTranslation(0, sections,
        std::vector<Lod>{{0, 4}}, live)); // out-of-bounds range
    CHECK_FALSE(receipt.MatchesBeforeTranslation(0, std::vector<Section>(257, Section{11}),
        std::vector<Lod>{{0, 257}}, live)); // selected packet hard cap
    receipt.ConsumeRejected();
    CHECK_FALSE(receipt.MatchesBeforeTranslation(0, sections, lods, live)); // no receipt replay
}

TEST_CASE("Model receipt refuses malformed frozen identity and consumes once",
    "[model-admission][receipt]")
{
    const std::array<uint64_t, 2> duplicate{11, 11};
    ModelAdmissionReceipt duplicateReceipt(4, 1, 2, 3, duplicate);
    CHECK_FALSE(duplicateReceipt.ValidFor(4));
    const std::array<uint64_t, 1> zero{0};
    ModelAdmissionReceipt zeroMesh(4, 1, 2, 3, zero);
    CHECK_FALSE(zeroMesh.ValidFor(4));
    ModelAdmissionReceipt missingEpoch(4, 0, 2, 3, expected);
    CHECK_FALSE(missingEpoch.ValidFor(4));
    ModelAdmissionReceipt invalidProducer(ModelAdmissionReceipt::InvalidModel, 1, 2, 3, expected);
    CHECK_FALSE(invalidProducer.ValidFor(ModelAdmissionReceipt::InvalidModel));
    std::array<uint64_t, 17> tooMany{};
    for (size_t i = 0; i < tooMany.size(); ++i) tooMany[i] = i + 1;
    ModelAdmissionReceipt oversized(4, 1, 2, 3, tooMany);
    CHECK_FALSE(oversized.ValidFor(4));
    std::array<uint64_t, 2> mutableSource{11, 22};
    ModelAdmissionReceipt frozen(4, 1, 2, 3, mutableSource);
    mutableSource[1] = 33;
    CHECK(frozen.MatchesBeforeTranslation(4, std::vector<Section>{{11}, {22}},
        std::vector<Lod>{{0, 2}}, live)); // exact copied mesh IDs, no borrowed span

    ModelAdmissionReceipt accepted(4, 1, 2, 3, expected);
    CHECK(accepted.Observe().state == ModelAdmissionReceipt::State::Pending);
    CHECK_FALSE(accepted.ConsumeAccepted(ModelAdmissionReceipt::InvalidModel));
    CHECK(accepted.Observe().state == ModelAdmissionReceipt::State::ConsumedRejected);
    CHECK_FALSE(accepted.ConsumeAccepted(17));
    CHECK(accepted.Observe().rendererModel == ModelAdmissionReceipt::InvalidModel);

    ModelAdmissionReceipt success(4, 1, 2, 3, expected);
    REQUIRE(success.ConsumeAccepted(17));
    const auto observed = success.Observe();
    CHECK(observed.state == ModelAdmissionReceipt::State::ConsumedAccepted);
    CHECK(observed.rendererModel == 17);
    success.ConsumeRejected();
    CHECK(success.Observe().state == ModelAdmissionReceipt::State::ConsumedAccepted);
    CHECK_FALSE(success.ConsumeAccepted(18));
}
