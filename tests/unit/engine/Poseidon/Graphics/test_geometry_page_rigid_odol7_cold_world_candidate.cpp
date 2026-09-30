#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageRigidOdol7ColdWorldCandidate.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

using namespace Poseidon;
using namespace Poseidon::GeometryPages;

namespace
{
struct RetailSourceFlag
{
    std::optional<std::string> previous;
    explicit RetailSourceFlag(const char* value)
    {
        if (const char* old = std::getenv("WGR_GEOMETRY_PAGE_RETAIL_SOURCE")) previous = old;
#ifdef _WIN32
        REQUIRE(_putenv_s("WGR_GEOMETRY_PAGE_RETAIL_SOURCE", value) == 0);
#else
        REQUIRE(setenv("WGR_GEOMETRY_PAGE_RETAIL_SOURCE", value, 1) == 0);
#endif
    }
    ~RetailSourceFlag()
    {
#ifdef _WIN32
        _putenv_s("WGR_GEOMETRY_PAGE_RETAIL_SOURCE", previous ? previous->c_str() : "");
#else
        if (previous) setenv("WGR_GEOMETRY_PAGE_RETAIL_SOURCE", previous->c_str(), 1);
        else unsetenv("WGR_GEOMETRY_PAGE_RETAIL_SOURCE");
#endif
    }
};
}

TEST_CASE("Cold world candidate refuses absent parser birth without touching prior snapshot",
          "[geometry-page-cold-world-candidate]")
{
    Foundation::CaptureMainThread();
    RigidOdol7ColdWorldCandidateSnapshot previous;
    previous.candidate.source.canonicalPath = "keep-prior-source";
    previous.shapeBirthId = 71;
    previous.shapeQueryRevision = 83;
    previous.knownCandidateCapacityBytes = 97;

    {
        RetailSourceFlag disabled("0");
        REQUIRE(CaptureRigidOdol7ColdWorldCandidate(nullptr, {}, 0, previous) ==
                RigidOdol7ColdWorldStatus::Disabled);
    }
    {
        RetailSourceFlag enabled("1");
        REQUIRE(CaptureRigidOdol7ColdWorldCandidate(nullptr, {}, 0, previous) ==
                RigidOdol7ColdWorldStatus::MissingBirth);
        LODShapeWithShadow ordinary;
        REQUIRE(CaptureRigidOdol7ColdWorldCandidate(&ordinary, {}, 1, previous) ==
                RigidOdol7ColdWorldStatus::MissingBirth);
        REQUIRE_FALSE(RigidOdol7ColdWorldDetail::SameBirth(&ordinary, {}, 1, 1));
    }
    REQUIRE(previous.candidate.source.canonicalPath == "keep-prior-source");
    REQUIRE(previous.shapeBirthId == 71);
    REQUIRE(previous.shapeQueryRevision == 83);
    REQUIRE(previous.knownCandidateCapacityBytes == 97);
}

TEST_CASE("Cold world candidate recognizes wrong owner before inspecting Shape birth",
          "[geometry-page-cold-world-candidate]")
{
    Foundation::CaptureMainThread();
    RetailSourceFlag enabled("1");
    RigidOdol7ColdWorldCandidateSnapshot prior;
    prior.candidate.source.canonicalPath = "untouched";
    RigidOdol7ColdWorldStatus status = RigidOdol7ColdWorldStatus::Captured;
    std::thread worker([&] {
        status = CaptureRigidOdol7ColdWorldCandidate(nullptr, {}, 0, prior);
    });
    worker.join();
    REQUIRE(status == RigidOdol7ColdWorldStatus::WrongOwner);
    REQUIRE(prior.candidate.source.canonicalPath == "untouched");
}

TEST_CASE("Cold world candidate charges every retained vector capacity transactionally",
          "[geometry-page-cold-world-candidate]")
{
    RigidOdol7Candidate candidate;
    candidate.source.canonicalPath = "data3d\\skala_new.p3d";
    candidate.visual.emplace_back();
    candidate.visual[0].texturePath = "data\\skala_piskovec2.pac";
    candidate.visual[0].positions.reserve(16);
    candidate.visual[0].vertices.reserve(16);
    candidate.visual[0].indices.reserve(48);
    candidate.visual[0].triangleMaterials.reserve(16);
    uint64_t known = 0;
    REQUIRE(RigidOdol7ColdWorldDetail::CandidateCapacityBytes(candidate, known));
    REQUIRE(known > sizeof(RigidOdol7ColdWorldCandidateSnapshot));
    REQUIRE(known <= RigidOdol7ColdWorldDetail::MaxCandidateCapacityBytes);

    const uint64_t accepted = known;
    candidate.visual[0].positions.reserve(11000); // 132 KiB from this array alone
    REQUIRE_FALSE(RigidOdol7ColdWorldDetail::CandidateCapacityBytes(candidate, known));
    REQUIRE(known == accepted);

    RigidOdol7Candidate other;
    other.lodEvidence.reserve(10000);
    REQUIRE_FALSE(RigidOdol7ColdWorldDetail::CandidateCapacityBytes(other, known));
    REQUIRE(known == accepted);
}
