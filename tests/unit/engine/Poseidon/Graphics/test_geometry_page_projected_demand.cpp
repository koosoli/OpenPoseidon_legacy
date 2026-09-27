#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageProjectedDemand.hpp>
#include <limits>
#include <initializer_list>
using namespace Poseidon::GeometryPages;
namespace {
constexpr ProjectedDemandBinding Pair{1,2,3,4};
ProjectedDemandObservation Bound(uint64_t generation,double upper){return {Pair,generation,true,upper};}
}
TEST_CASE("Projected dwell and retry require actual new camera observations", "[geometry-page-projected-demand]") {
    ProjectedPageDemand demand(Pair);
    demand.Observe(100,Bound(1,2));
    for(uint64_t now=101;now<=400;++now)demand.Observe(now,Bound(1,2));
    REQUIRE_FALSE(demand.DesiredFinePreparation());REQUIRE_FALSE(demand.CanAttempt(400,1,false,false,false));
    demand.Observe(401,Bound(2,2));REQUIRE(demand.DesiredFinePreparation());
    const auto revision=demand.Revision();REQUIRE(demand.CanAttempt(401,2,false,false,false));demand.Attempted(401);
    REQUIRE_FALSE(demand.CanAttempt(401,2,false,false,false));REQUIRE_FALSE(demand.CanAttempt(651,2,false,false,false));
    demand.Observe(650,Bound(3,2));REQUIRE_FALSE(demand.CanAttempt(650,3,false,false,false));
    demand.Observe(651,Bound(4,2));REQUIRE(demand.CanAttempt(651,4,false,false,false));
    REQUIRE_FALSE(demand.CanAttempt(652,4,false,false,false));REQUIRE_FALSE(demand.CanAttempt(651,3,false,false,false));
    demand.Observe(700,Bound(5,1));demand.Observe(800,Bound(6,1.5));
    demand.Observe(900,Bound(7,1));REQUIRE_FALSE(demand.Observe(1099,Bound(8,1)));
    REQUIRE(demand.Observe(1100,Bound(9,1)));REQUIRE_FALSE(demand.DesiredFinePreparation());REQUIRE(demand.Revision()==revision+1);
}
TEST_CASE("Rejected generation is fenced even if later replay appears valid", "[geometry-page-projected-demand]") {
    ProjectedPageDemand demand(Pair,{2,1,0,50});demand.Observe(10,Bound(1,3));REQUIRE(demand.DesiredFinePreparation());
    auto refused=Bound(2,3);refused.validatedIdealPairBound=false;
    REQUIRE(demand.Observe(11,refused));REQUIRE(demand.LastGeneration()==2);
    demand.Observe(12,Bound(1,3));demand.Observe(13,Bound(2,3));
    REQUIRE_FALSE(demand.DesiredFinePreparation());REQUIRE_FALSE(demand.CanAttempt(13,2,false,false,false));
    demand.Observe(14,Bound(3,3));REQUIRE(demand.DesiredFinePreparation());
    for(unsigned field=0;field<4;++field){
        auto different=Bound(4+field*2,3);
        if(field==0)++different.binding.fixtureEpoch;
        if(field==1)++different.binding.sourceAdmissionEpoch;
        if(field==2)++different.binding.certificateGeneration;
        if(field==3)++different.binding.coarseModelBirth;
        REQUIRE(demand.Observe(20+field*3,different));
        demand.Observe(21+field*3,Bound(4+field*2,3));REQUIRE_FALSE(demand.DesiredFinePreparation());
        demand.Observe(22+field*3,Bound(5+field*2,3));REQUIRE(demand.DesiredFinePreparation());
    }
}
TEST_CASE("Projected attempts remain lifetime bounded and never duplicate prepared work", "[geometry-page-projected-demand]") {
    ProjectedPageDemand demand(Pair,{2,1,0,50});demand.Observe(10,Bound(1,3));
    REQUIRE_FALSE(demand.CanAttempt(10,1,true,false,false));REQUIRE_FALSE(demand.CanAttempt(10,1,false,true,false));REQUIRE_FALSE(demand.CanAttempt(10,1,false,false,true));
    for(uint32_t i=0;i<8;++i){const auto now=10+uint64_t(i)*50;demand.Observe(now,Bound(1+i,3));REQUIRE(demand.CanAttempt(now,1+i,false,false,false));demand.Attempted(now);}
    REQUIRE(demand.Attempts()==8);demand.Observe(10000,Bound(9,3));REQUIRE_FALSE(demand.CanAttempt(10000,9,false,false,false));
    auto invalid=Bound(10,0);invalid.validatedIdealPairBound=false;REQUIRE(demand.Observe(10001,invalid));
    demand.Observe(10002,Bound(11,3));REQUIRE_FALSE(demand.CanAttempt(10002,11,false,false,false));
}
TEST_CASE("Projected policy refuses malformed values time and binding without promotion", "[geometry-page-projected-demand]") {
    for(double upper:{-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
        ProjectedPageDemand demand(Pair,{2,1,0,50});demand.Observe(100,Bound(1,3));REQUIRE(demand.Observe(101,Bound(2,upper)));demand.Observe(102,Bound(2,3));REQUIRE_FALSE(demand.DesiredFinePreparation());
    }
    ProjectedPageDemand backwards(Pair,{2,1,0,50});backwards.Observe(100,Bound(1,3));REQUIRE(backwards.Observe(99,Bound(2,3)));backwards.Observe(101,Bound(2,3));REQUIRE_FALSE(backwards.DesiredFinePreparation());
    backwards.Observe(102,Bound(3,3));REQUIRE(backwards.Observe(UINT64_MAX,Bound(4,3)));backwards.Observe(103,Bound(5,3));REQUIRE_FALSE(backwards.DesiredFinePreparation());
    ProjectedPageDemand empty({},{});empty.Observe(1,Bound(1,3));REQUIRE_FALSE(empty.DesiredFinePreparation());
    for(auto settings:{ProjectedDemandSettings{1,1,0,50},ProjectedDemandSettings{2,-1,0,50},ProjectedDemandSettings{2,1,2001,50},ProjectedDemandSettings{2,1,0,49}}){ProjectedPageDemand invalid(Pair,settings);invalid.Observe(1,Bound(1,3));REQUIRE_FALSE(invalid.CanAttempt(1,1,false,false,false));}
    ProjectedPageDemand generations(Pair,{2,1,0,50});generations.Observe(1,Bound(1,3));REQUIRE(generations.Observe(2,Bound(0,3)));generations.Observe(3,Bound(2,3));REQUIRE(generations.Observe(4,Bound(UINT64_MAX,3)));generations.Observe(5,Bound(3,3));REQUIRE_FALSE(generations.DesiredFinePreparation());
}
