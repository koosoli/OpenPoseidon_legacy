// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <Poseidon/Dev/NpcChat/NpcChatReply.hpp>
#include <Poseidon/Game/Conquest/ConquestLlm.hpp>

using namespace Poseidon::Conquest;

namespace
{
const std::vector<std::string> Towns{"Morton", "Figari", "Montignac", "Airfield"};
const std::vector<std::string> Squads{"Rifle squad", "Tank"};
} // namespace

TEST_CASE("LLM commander: a well-formed answer becomes priorities, posture and a purchase", "[conquest][llm]")
{
    Llm::Advice advice;
    std::string error;
    const std::string text = R"({"priorities": [{"objective": "Figari", "weight": 3}, {"objective": "montignac", "weight": -1}],
        "posture": "attack", "buy": "tank", "barrage": "figari", "reason": "Figari first, it opens the road north."})";
    REQUIRE(Llm::ParseAdvice(text, Towns, Squads, advice, error));
    REQUIRE(advice.priorities.size() == 2);
    CHECK(advice.priorities[0].first == "Figari");
    CHECK(advice.priorities[0].second == Catch::Approx(3.0f));
    CHECK(advice.priorities[1].first == "Montignac"); // matched without regard to case
    CHECK(advice.posture == "attack");
    CHECK(advice.buy == "Tank");
    CHECK(advice.barrage == "Figari"); // the mortars, on a town the report named
    CHECK(advice.reason == "Figari first, it opens the road north.");
}

TEST_CASE("LLM commander: prose and fences round the JSON are tolerated", "[conquest][llm]")
{
    Llm::Advice advice;
    std::string error;
    const std::string text = "Here is my plan:\n```json\n{\"priorities\": [{\"objective\": \"Morton\", \"weight\": 1}],"
                             " \"posture\": \"defend\"}\n```\nGood luck.";
    REQUIRE(Llm::ParseAdvice(text, Towns, Squads, advice, error));
    CHECK(advice.posture == "defend");
    REQUIRE(advice.priorities.size() == 1);
    CHECK(advice.buy.empty());
}

TEST_CASE("LLM commander: invented towns, wild weights and odd postures are cleaned up", "[conquest][llm]")
{
    Llm::Advice advice;
    std::string error;
    const std::string text = R"({"priorities": [{"objective": "Paris", "weight": 3}, {"objective": "Airfield", "weight": 99}],
        "posture": "blitzkrieg", "buy": "Battleship"})";
    REQUIRE(Llm::ParseAdvice(text, Towns, Squads, advice, error));
    REQUIRE(advice.priorities.size() == 1); // Paris dropped
    CHECK(advice.priorities[0].second == Catch::Approx(3.0f)); // clamped
    CHECK(advice.posture == "balanced");
    CHECK(advice.buy.empty());
}

TEST_CASE("LLM commander: an answer with nothing usable is a failure, so the standard commander carries on",
          "[conquest][llm]")
{
    Llm::Advice advice;
    std::string error;
    CHECK_FALSE(Llm::ParseAdvice("I would attack the nearest town.", Towns, Squads, advice, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(Llm::ParseAdvice("{\"priorities\": [", Towns, Squads, advice, error)); // truncated
    CHECK_FALSE(Llm::ParseAdvice(R"({"priorities": [{"objective": "Paris", "weight": 2}]})", Towns, Squads, advice, error));
    CHECK_FALSE(Llm::ParseAdvice("", Towns, Squads, advice, error));
}

TEST_CASE("NPC chat: a request to HQ for a main effort parses as a priority order", "[conquest][llm]")
{
    using namespace Poseidon::NpcChat;
    Reply reply;
    REQUIRE(ParseReply(R"({"say": [{"speaker": "HQ", "text": "Understood, Figari it is."}],
        "orders": [{"to": "all", "order": "priority", "value": "Figari"}]})",
                       "HQ", reply));
    REQUIRE(reply.orders.size() == 1);
    CHECK(reply.orders[0].kind == OrderKind::Priority);
    CHECK(reply.orders[0].value == "Figari"); // the name as given, not upper-cased
    CHECK(reply.rejected.empty());
}
