#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageEvictionPolicy.hpp>
using namespace Poseidon::GeometryPages;
TEST_CASE("Authored fine retirement preserves historical Present and Absent expectations", "[geometry-page-eviction]")
{
    PageHistoryScope first{3,0,0,true};REQUIRE(first.Valid());
    for(uint32_t i=0;i<3;++i) REQUIRE(first.Expected(i)==1);
    PageHistoryScope evicted{3,1,3,true};REQUIRE(evicted.Expected(0)==1);
    REQUIRE(evicted.Expected(1)==2);REQUIRE(evicted.Expected(2)==2);
    PageHistoryScope refilled{5,1,3,true};REQUIRE(refilled.Expected(0)==1);
    REQUIRE(refilled.Expected(1)==2);REQUIRE(refilled.Expected(2)==2);
    REQUIRE(refilled.Expected(3)==1);REQUIRE(refilled.Expected(4)==1);
    refilled.active=false;for(uint32_t i=0;i<5;++i) REQUIRE(refilled.Expected(i)==2);
}
TEST_CASE("Authored history cannot certify a forgotten retired range or out of bounds prefix", "[geometry-page-eviction]")
{
    for(auto scope:{PageHistoryScope{0,0,0,true},PageHistoryScope{65,1,3,true},
        PageHistoryScope{1,1,3,true},PageHistoryScope{3,3,1,true}}) {
        REQUIRE_FALSE(scope.Valid());REQUIRE(scope.Expected(0)==0);
    }
    PageHistoryScope bound{64,1,3,true};REQUIRE(bound.Valid());REQUIRE(bound.Expected(63)==1);REQUIRE(bound.Expected(64)==0);
}
TEST_CASE("Authored fine consumer refuses an old identity after refill and unsubmitted results", "[geometry-page-eviction]")
{
    constexpr uint32_t old=11,fresh=12,invalid=UINT32_MAX;
    REQUIRE(DecideFineUpdate(true,false,true,old,old,old,1)==FineUpdateDecision::Commit);
    REQUIRE(DecideFineUpdate(true,false,true,old,fresh,fresh,2)==FineUpdateDecision::Drop);
    REQUIRE(DecideFineUpdate(true,true,false,old,fresh,invalid,2)==FineUpdateDecision::Drop);
    REQUIRE(DecideFineUpdate(true,false,true,fresh,fresh,old,2)==FineUpdateDecision::Drop);
    REQUIRE(DecideFineUpdate(true,false,false,fresh,fresh,fresh,2)==FineUpdateDecision::Drop);
    REQUIRE(DecideFineUpdate(true,false,true,fresh,fresh,fresh,0)==FineUpdateDecision::Drop);
    REQUIRE(DecideFineUpdate(false,false,true,fresh,fresh,fresh,2)==FineUpdateDecision::Drop);
    REQUIRE(DecideFineUpdate(true,false,true,invalid,invalid,invalid,2)==FineUpdateDecision::Drop);
    REQUIRE(DecideFineUpdate(true,false,true,0,0,0,2)==FineUpdateDecision::Commit); // ID0 is valid
    REQUIRE(DecideFineUpdate(true,true,false,fresh,fresh,invalid,2)==FineUpdateDecision::Resident);
}
