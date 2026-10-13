// exitWith inside a loop body ends the loop (the body runs in a context of its own).
#include <catch2/catch_test_macros.hpp>
#include "eval_fixture.hpp"

TEST_CASE("exitWith ends a while loop", "[loop-exit]")
{
    EvalFixture f;
    GameValue result =
        f.state().EvaluateMultiple("loopExitN = 0; while {true} do {loopExitN = loopExitN + 1; if (loopExitN >= 3) exitWith {}}; loopExitN");
    REQUIRE(f.state().GetLastError() == EvalOK);
    REQUIRE((float)result == 3.0f);
}

TEST_CASE("exitWith ends a for-from-to loop", "[loop-exit]")
{
    EvalFixture f;
    GameValue result = f.state().EvaluateMultiple(
        "loopExitR = 0; for \"_i\" from 1 to 10 do {loopExitR = _i; if (_i == 4) exitWith {}}; loopExitR");
    REQUIRE(f.state().GetLastError() == EvalOK);
    REQUIRE((float)result == 4.0f);
}

TEST_CASE("a loop without exitWith runs to its end", "[loop-exit]")
{
    EvalFixture f;
    GameValue result =
        f.state().EvaluateMultiple("loopExitS = 0; for \"_i\" from 1 to 5 do {loopExitS = loopExitS + _i}; loopExitS");
    REQUIRE(f.state().GetLastError() == EvalOK);
    REQUIRE((float)result == 15.0f);
}
