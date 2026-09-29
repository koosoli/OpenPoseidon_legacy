#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/DeferredProceduralAdmission.hpp>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace admission = Poseidon::render::procedural::admission;

namespace
{
struct PilotFlag
{
    std::string previous;
    bool hadPrevious = false;
    explicit PilotFlag(const char* value)
    {
        if (const char* prior = std::getenv("WGR_OBJECT_STREAM_DAYZ_GENERATED_DEFER"))
        {
            previous = prior;
            hadPrevious = true;
        }
        Set(value);
    }
    ~PilotFlag() { Set(hadPrevious ? previous.c_str() : nullptr); }
    static void Set(const char* value)
    {
#ifdef _WIN32
        _putenv_s("WGR_OBJECT_STREAM_DAYZ_GENERATED_DEFER", value ? value : "");
#else
        if (value) setenv("WGR_OBJECT_STREAM_DAYZ_GENERATED_DEFER", value, 1);
        else unsetenv("WGR_OBJECT_STREAM_DAYZ_GENERATED_DEFER");
#endif
    }
};
}

TEST_CASE("Generated admission scopes require the exact opt-in and selected owner", "[generated-admission]")
{
    auto owner = std::make_shared<admission::Ledger>();
    PilotFlag flag("0");
    {
        admission::CaptureScope capture(owner, true);
        REQUIRE_FALSE(admission::CurrentCapture);
    }
    PilotFlag::Set("1");
    {
        admission::CaptureScope unselected(owner, false);
        REQUIRE_FALSE(admission::CurrentCapture);
    }
    {
        admission::CaptureScope capture(owner, true);
        REQUIRE(admission::CurrentCapture == owner);
        REQUIRE_FALSE(admission::IsOwnerStage(owner.get()));
        {
            admission::StageScope stage(owner);
            REQUIRE(admission::IsOwnerStage(owner.get()));
            admission::CaptureScope nested({}, false);
            REQUIRE_FALSE(admission::CurrentCapture);
        }
        REQUIRE(admission::CurrentCapture == owner);
        REQUIRE_FALSE(admission::IsOwnerStage(owner.get()));
    }
    REQUIRE_FALSE(admission::CurrentCapture);
}

TEST_CASE("Generated admission charges retained bytes until consumed or destroyed", "[generated-admission]")
{
    auto owner = std::make_shared<admission::Ledger>();
    auto charge = owner->Reserve(admission::MaxPendingBytes);
    REQUIRE(charge);
    owner->NoteDeferred();
    REQUIRE(owner->Read().pendingCount == 1);
    REQUIRE(owner->Read().pendingBytes == admission::MaxPendingBytes);
    REQUIRE_FALSE(owner->Reserve(1));
    REQUIRE(owner->Read().immediateFallback == 1);
    REQUIRE(owner->Read().aborted);
    charge.reset();
    REQUIRE(owner->Read().pendingCount == 0);
    REQUIRE(owner->Read().pendingBytes == 0);
    REQUIRE(owner->Read().deferred == 1);
}

TEST_CASE("Generated admission count cap and cross-owner process cap fail closed", "[generated-admission]")
{
    auto first = std::make_shared<admission::Ledger>();
    auto second = std::make_shared<admission::Ledger>();
    std::vector<std::unique_ptr<admission::Ledger::Charge>> charges;
    for (size_t i = 0; i < admission::MaxPendingCount; ++i)
    {
        auto charge = first->Reserve(1);
        REQUIRE(charge);
        charges.push_back(std::move(charge));
    }
    REQUIRE_FALSE(first->Reserve(1));
    REQUIRE_FALSE(second->Reserve(1));
    REQUIRE(first->Read().immediateFallback == 1);
    REQUIRE(second->Read().immediateFallback == 1);
    charges.clear();
    REQUIRE(second->Reserve(1));
}

TEST_CASE("Generated admission witnesses stage, escape, birth mismatch and failure separately", "[generated-admission]")
{
    auto owner = std::make_shared<admission::Ledger>();
    auto foreign = std::make_shared<admission::Ledger>();
    PilotFlag flag("1");
    REQUIRE(admission::SameBirthName("#(argb,8,8,3)color(1,1,1,1)",
                                     "#(argb,8,8,3)color(1,1,1,1)"));
    REQUIRE_FALSE(admission::SameBirthName("#(argb,8,8,3)color(1,1,1,1)",
                                           "#(argb,8,8,3)color(1,1,1,0)"));
    REQUIRE_FALSE(admission::SameBirthName(nullptr, "birth"));
    {
        admission::StageScope stage(owner);
        admission::NoteUploadAttempt(*owner, true);
        admission::NoteUploadAttempt(*foreign, true); // foreign owner is an escape
    }
    REQUIRE_FALSE(owner->Read().aborted);
    admission::NoteUploadAttempt(*owner, true); // after the stage scope is an escape
    owner->NoteNameMismatch();
    {
        admission::StageScope stage(owner);
        admission::NoteUploadAttempt(*owner, false);
    }
    const auto report = owner->Read();
    REQUIRE(report.staged == 2);
    REQUIRE(report.escaped == 1);
    REQUIRE(report.nameMismatch == 1);
    REQUIRE(report.failed == 1);
    REQUIRE(report.aborted);
    REQUIRE(foreign->Read().escaped == 1);
    REQUIRE(foreign->Read().aborted);
}
