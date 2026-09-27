#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageCameraDemand.hpp>
using namespace Poseidon::GeometryPages;
TEST_CASE("Private camera prefetch follows real dwell progress and hysteresis", "[geometry-page-camera-demand]") {
    CameraPageDemand demand({30,50,200,250});
    REQUIRE_FALSE(demand.Observe(100,true,60));REQUIRE_FALSE(demand.CanAttempt(100,false,false,false));
    demand.Observe(200,true,20);demand.Observe(399,true,20);REQUIRE_FALSE(demand.DesiredFine());
    demand.Observe(400,true,20);REQUIRE(demand.DesiredFine());REQUIRE(demand.CanAttempt(400,false,false,false));
    const auto revision=demand.Revision();demand.Attempted(400);
    for(uint64_t now=401;now<650;++now){demand.Observe(now,true,40);REQUIRE(demand.DesiredFine());REQUIRE(demand.Revision()==revision);
        REQUIRE_FALSE(demand.CanAttempt(now,false,false,false));}
    REQUIRE(demand.CanAttempt(650,false,false,false));
    REQUIRE_FALSE(demand.CanAttempt(650,true,false,false));REQUIRE_FALSE(demand.CanAttempt(650,false,true,false));
    REQUIRE_FALSE(demand.CanAttempt(650,false,false,true));
    demand.Observe(700,true,60);demand.Observe(800,true,40); // middle rail breaks far dwell
    demand.Observe(900,true,60);REQUIRE_FALSE(demand.Observe(1099,true,60));
    REQUIRE(demand.Observe(1100,true,60));REQUIRE_FALSE(demand.DesiredFine());REQUIRE(demand.Revision()==revision+1);
}
TEST_CASE("Private camera prefetch caps attempts and revokes unavailable or backward observations", "[geometry-page-camera-demand]") {
    CameraPageDemand demand({1,2,0,50});demand.Observe(10,true,.5);
    for(uint32_t i=0;i<CameraPageDemand::MaxAttempts;++i){const auto now=10+uint64_t(i)*50;REQUIRE(demand.CanAttempt(now,false,false,false));demand.Attempted(now);}
    REQUIRE_FALSE(demand.CanAttempt(10000,false,false,false));REQUIRE(demand.Attempts()==8);
    REQUIRE(demand.Observe(10001,false,0));REQUIRE_FALSE(demand.DesiredFine());
    CameraPageDemand backward({1,2,0,50});backward.Observe(100,true,.5);REQUIRE(backward.DesiredFine());
    REQUIRE(backward.Observe(99,true,.5));REQUIRE_FALSE(backward.DesiredFine());REQUIRE_FALSE(backward.CanAttempt(99,false,false,false));
    backward.Observe(101,true,.5);REQUIRE(backward.DesiredFine());REQUIRE(backward.Observe(UINT64_MAX,true,.5));
    REQUIRE_FALSE(backward.CanAttempt(UINT64_MAX,false,false,false));
    CameraPageDemand invalid({2,1,0,50});invalid.Observe(1,true,0);REQUIRE_FALSE(invalid.DesiredFine());
    CameraPageDemand nan({1,2,0,50});nan.Observe(1,true,.5);REQUIRE(nan.Observe(2,true,std::numeric_limits<double>::quiet_NaN()));
}
TEST_CASE("Prepared fine record proof requires a successful complete private renderer cut", "[geometry-page-camera-demand]") {
    REQUIRE(PreparedFineRecordCut(true,true,true,true,true,7,0)); // Renderer ID0 is valid.
    REQUIRE_FALSE(PreparedFineRecordCut(false,true,true,true,true,7,0));
    REQUIRE_FALSE(PreparedFineRecordCut(true,false,true,true,true,7,0));
    REQUIRE_FALSE(PreparedFineRecordCut(true,true,false,true,true,7,0));
    REQUIRE_FALSE(PreparedFineRecordCut(true,true,true,false,true,7,0));
    REQUIRE_FALSE(PreparedFineRecordCut(true,true,true,true,false,7,0));
    REQUIRE_FALSE(PreparedFineRecordCut(true,true,true,true,true,UINT32_MAX,0));
    REQUIRE_FALSE(PreparedFineRecordCut(true,true,true,true,true,7,UINT32_MAX));
}
