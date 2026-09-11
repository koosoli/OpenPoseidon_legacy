#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Dev/Diag/FrameProfiler.hpp>

TEST_CASE("Frame capture is bounded, ordered and independent of the rolling ring", "[frame-capture]")
{
    Poseidon::Dev::FrameProfiler perf;
    CHECK_FALSE(perf.StartCapture(0));
    CHECK_FALSE(perf.StartCapture(perf.kMaxCaptureFrames + 1));
    perf.BeginFrame();
    REQUIRE(perf.StartCapture(300));
    perf.EndFrame(-1); // Starting in a script must not include a partial frame.
    CHECK(perf.CapturedFrames().empty());
    for (int i = 0; i < 305; ++i)
    {
        perf.BeginFrame();
        perf.Mark(perf.PhaseSetup);
        perf.EndFrame(i);
    }
    REQUIRE(perf.CapturedFrames().size() == 300);
    CHECK(perf.CaptureDropped() == 5);
    CHECK(perf.FrameCount() == perf.kRingSize);
    for (int i = 0; i < 300; ++i)
    {
        CHECK(perf.CapturedFrames()[i].drawCalls == i);
        CHECK(perf.CapturedFrames()[i].totalMs >= perf.CapturedFrames()[i].ms[perf.PhaseSetup]);
    }
    perf.StopCapture();
    perf.BeginFrame();
    perf.EndFrame(999);
    CHECK(perf.CaptureDropped() == 5);
    REQUIRE(perf.StartCapture(2));
    CHECK(perf.CapturedFrames().empty());
    CHECK(perf.CaptureDropped() == 0);
    perf.BeginFrame();
    perf.StopCapture();
    perf.EndFrame(1000); // Stopping mid-frame excludes the unfinished frame.
    CHECK(perf.CapturedFrames().empty());
    REQUIRE(perf.StartCapture(2));
    perf.Reset();
    perf.BeginFrame();
    perf.EndFrame(1001);
    CHECK(perf.CapturedFrames().empty());
}
