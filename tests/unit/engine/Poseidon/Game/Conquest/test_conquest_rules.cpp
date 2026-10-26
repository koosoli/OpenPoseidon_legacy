// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include <Poseidon/Game/Conquest/ConquestCommander.hpp>
#include <Poseidon/Game/Conquest/ConquestRules.hpp>

using namespace Poseidon::Conquest;

namespace
{
// A line of five: HQ(0) - A(1) - B(2) - C(3) - HQ(4). Side 0 owns 0, side 1 owns 4.
War LineWar()
{
    War war;
    war.sideCount = 2;
    const char* names[] = {"HQ0", "A", "B", "C", "HQ1"};
    for (int i = 0; i < 5; i++)
    {
        ObjectiveDef d;
        d.name = names[i];
        d.x = 1000.0f * static_cast<float>(i);
        d.y = 0.0f;
        d.kind = (i == 0 || i == 4) ? ObjectiveKind::HQ : ObjectiveKind::Town;
        d.value = d.kind == ObjectiveKind::HQ ? 10.0f : 5.0f;
        if (i > 0)
            d.neighbours.push_back(i - 1);
        if (i < 4)
            d.neighbours.push_back(i + 1);
        war.defs.push_back(d);
    }
    war.defs[0].initialOwner = 0;
    war.defs[4].initialOwner = 1;
    StartWar(war, Tuning(), 300.0f);
    return war;
}

Presence Men(float side0, float side1)
{
    Presence p;
    p.strength[0] = side0;
    p.strength[1] = side1;
    return p;
}

// Runs the capture for `seconds` in one-second steps; returns the first event.
CaptureResult Hold(War& war, int index, const Presence& p, float seconds, float& now)
{
    CaptureResult first;
    for (float t = 0.0f; t < seconds; t += 1.0f)
    {
        now += 1.0f;
        const CaptureResult r = StepCapture(war, index, p, 1.0f, now, Tuning());
        if (first.event == CaptureEvent::None && r.event != CaptureEvent::None)
            first = r;
    }
    return first;
}
} // namespace

TEST_CASE("conquest: a squad captures a neutral objective on its front in 45 s", "[conquest]")
{
    War war = LineWar();
    float now = 0.0f;
    CHECK(CaptureSeconds(8.0f, Tuning()) == Catch::Approx(45.0f));
    CHECK(CaptureSeconds(2.0f, Tuning()) == Catch::Approx(180.0f));
    CHECK(CaptureSeconds(16.0f, Tuning()) == Catch::Approx(22.5f)); // more soldiers, faster
    CHECK(CaptureSeconds(100.0f, Tuning()) == Catch::Approx(15.0f)); // but only up to 24 of them
    Hold(war, 1, Men(8, 0), 44.0f, now);
    CHECK(war.states[1].owner == NoSide);
    const CaptureResult r = Hold(war, 1, Men(8, 0), 2.0f, now);
    CHECK(r.event == CaptureEvent::Captured);
    CHECK(r.side == 0);
    CHECK(war.states[1].owner == 0);
}

TEST_CASE("conquest: no progress off the front line", "[conquest]")
{
    War war = LineWar();
    float now = 0.0f;
    Hold(war, 2, Men(8, 0), 120.0f, now); // B is not next to anything side 0 owns
    CHECK(war.states[2].owner == NoSide);
    CHECK(war.states[2].level == 0.0f);
    CHECK_FALSE(CanAttack(war, 0, 2));
    CHECK(CanAttack(war, 0, 1));
}

TEST_CASE("conquest: an owned objective is neutralised, then captured", "[conquest]")
{
    War war = LineWar();
    float now = 0.0f;
    Hold(war, 3, Men(0, 8), 50.0f, now); // side 1 takes C
    REQUIRE(war.states[3].owner == 1);
    war.states[2].owner = 0; // pretend side 0 holds B, so C is on its front
    war.states[2].holder = 0;
    war.states[2].level = 1.0f;
    const CaptureResult drained = Hold(war, 3, Men(8, 0), 46.0f, now);
    CHECK(drained.event == CaptureEvent::Neutralised);
    CHECK(war.states[3].owner == NoSide);
    const CaptureResult taken = Hold(war, 3, Men(8, 0), 46.0f, now);
    CHECK(taken.event == CaptureEvent::Captured);
    CHECK(war.states[3].owner == 0);
}

TEST_CASE("conquest: the side with more soldiers in the zone captures; a tie freezes it", "[conquest]")
{
    War war = LineWar();
    float now = 0.0f;
    Hold(war, 1, Men(3, 3), 30.0f, now);
    CHECK(war.states[1].contested);
    CHECK(war.states[1].level == 0.0f);
    Hold(war, 1, Men(4, 3), 10.0f, now);
    CHECK_FALSE(war.states[1].contested);
    CHECK(war.states[1].holder == 0);
    CHECK(war.states[1].level > 0.0f);
}

TEST_CASE("conquest: capture speed follows the margin, not the head count", "[conquest]")
{
    // 8 against 3 is as fast as 5 alone; 8 alone is faster.
    War a = LineWar(), b = LineWar(), c = LineWar();
    float t1 = 0.0f, t2 = 0.0f, t3 = 0.0f;
    Hold(a, 1, Men(8, 3), 10.0f, t1);
    Hold(b, 1, Men(5, 0), 10.0f, t2);
    Hold(c, 1, Men(8, 0), 10.0f, t3);
    CHECK(a.states[1].level == Catch::Approx(b.states[1].level));
    CHECK(c.states[1].level > a.states[1].level);
}

TEST_CASE("conquest: abandoned progress decays back", "[conquest]")
{
    War war = LineWar();
    float now = 0.0f;
    Hold(war, 1, Men(8, 0), 20.0f, now);
    const float partial = war.states[1].level;
    REQUIRE(partial > 0.0f);
    Hold(war, 1, Men(0, 0), 60.0f, now);
    CHECK(war.states[1].level < partial);
    Hold(war, 1, Men(0, 0), 600.0f, now);
    CHECK(war.states[1].level == 0.0f);
    CHECK(war.states[1].holder == NoSide);
}

TEST_CASE("conquest: an HQ falls last", "[conquest]")
{
    War war = LineWar();
    war.states[3].owner = 0; // side 0 reached C; side 1 still owns C? no: give side 1 B to keep it alive
    war.states[3].holder = 0;
    war.states[3].level = 1.0f;
    war.states[2].owner = 1;
    war.states[2].holder = 1;
    war.states[2].level = 1.0f;
    CHECK_FALSE(CanAttack(war, 0, 4)); // side 1 still owns B
    war.states[2].owner = 0;
    war.states[2].holder = 0;
    CHECK(CanAttack(war, 0, 4));
}

TEST_CASE("conquest: spawn eligibility", "[conquest]")
{
    War war = LineWar();
    Tuning tuning;
    std::string why;
    CHECK(SpawnEligible(war, 0, 0, 1000.0f, tuning, &why));
    CHECK_FALSE(SpawnEligible(war, 1, 0, 1000.0f, tuning, &why));
    CHECK(why == "not ours");
    war.states[0].enemiesNear[0] = 1;
    CHECK_FALSE(SpawnEligible(war, 0, 0, 1000.0f, tuning, &why));
    CHECK(why == "enemy near the base");
    war.states[0].enemiesNear[0] = 0;
    war.states[0].lastHitAt = 980.0f;
    CHECK_FALSE(SpawnEligible(war, 0, 0, 1000.0f, tuning, &why));
    CHECK(why == "taking fire");
    war.states[0].lastHitAt = -1.0e9f;
    war.states[0].capturedAt = 990.0f;
    CHECK_FALSE(SpawnEligible(war, 0, 0, 1000.0f, tuning, &why));
    CHECK(why == "just captured");
}

TEST_CASE("conquest: income, score drain and defeat", "[conquest]")
{
    War war = LineWar();
    Tuning tuning;
    CHECK(IncomePerMinute(war, 0) == Catch::Approx(10.0f));
    war.states[1].owner = 0;
    war.states[2].owner = 0;
    CHECK(IncomePerMinute(war, 0) == Catch::Approx(20.0f));
    CHECK(StepEconomy(war, 60.0f, tuning) == NoSide);
    CHECK(war.pool[0] == Catch::Approx(320.0f));
    CHECK(war.score[1] == Catch::Approx(tuning.startScore - 10.0f)); // two objectives behind
    CHECK(war.score[0] == Catch::Approx(tuning.startScore));
    war.states[4].owner = NoSide; // side 1 lost everything
    CHECK(StepEconomy(war, 1.0f, tuning) == 0);
    CHECK(war.defeated[1]);
}

namespace
{
SideView ViewOf(const War& war, int side)
{
    SideView v;
    v.side = side;
    v.war = &war;
    v.knownEnemy.assign(war.defs.size(), 0.0f);
    v.canSpawn.assign(war.defs.size(), false);
    for (size_t i = 0; i < war.defs.size(); i++)
        v.canSpawn[i] = war.states[i].owner == side;
    v.pool = 0.0f;
    v.cap = 60;
    return v;
}
} // namespace

TEST_CASE("conquest commander: attacks the front, never the rear", "[conquest]")
{
    War war = LineWar();
    SideView v = ViewOf(war, 0);
    v.groups.push_back({1, 0.0f, 0.0f, 8, TaskKind::None, -1, false});
    const auto targets = RankTargets(v);
    REQUIRE(targets.size() == 1);
    CHECK(targets[0].first == 1);
    const auto intents = PlanStandard(v, CommanderTuning());
    REQUIRE(intents.size() == 1);
    CHECK(intents[0].kind == Intent::Kind::Assign);
    CHECK(intents[0].task == TaskKind::Attack);
    CHECK(intents[0].objective == 1);
}

TEST_CASE("conquest commander: defends a threatened objective first", "[conquest]")
{
    War war = LineWar();
    war.states[1].owner = 0;
    war.states[1].holder = 0;
    war.states[1].level = 1.0f;
    SideView v = ViewOf(war, 0);
    v.knownEnemy[1] = 3.0f;
    v.groups.push_back({1, 100.0f, 0.0f, 8, TaskKind::None, -1, false});
    const auto intents = PlanStandard(v, CommanderTuning());
    REQUIRE_FALSE(intents.empty());
    CHECK(intents[0].task == TaskKind::Defend);
    CHECK(intents[0].objective == 1);
}

TEST_CASE("conquest commander: a captured target becomes a garrison", "[conquest]")
{
    War war = LineWar();
    war.states[1].owner = 0;
    war.states[1].holder = 0;
    war.states[1].level = 1.0f;
    SideView v = ViewOf(war, 0);
    v.groups.push_back({1, 1000.0f, 0.0f, 8, TaskKind::Attack, 1, false});
    const auto intents = PlanStandard(v, CommanderTuning());
    REQUIRE_FALSE(intents.empty());
    CHECK(intents[0].group == 1);
    CHECK(intents[0].task == TaskKind::Defend);
    CHECK(intents[0].objective == 1);
}

TEST_CASE("conquest commander: buys within pool and cap at an eligible base", "[conquest]")
{
    War war = LineWar();
    SideView v = ViewOf(war, 0);
    v.pool = 1000.0f;
    v.live = 50;
    v.cap = 60;
    auto intents = PlanStandard(v, CommanderTuning());
    int buys = 0;
    for (const Intent& i : intents)
        if (i.kind == Intent::Kind::BuySquad)
        {
            buys++;
            CHECK(i.objective == 0);
        }
    CHECK(buys == 1); // a second squad would pass the cap
    v.canSpawn[0] = false;
    intents = PlanStandard(v, CommanderTuning());
    for (const Intent& i : intents)
        CHECK(i.kind != Intent::Kind::BuySquad);
}

TEST_CASE("conquest commander: a captured objective keeps one garrison, the rest move on", "[conquest]")
{
    War war = LineWar();
    war.states[1].owner = 0; // side 0 took A with three squads
    war.states[1].holder = 0;
    war.states[1].level = 1.0f;
    SideView v = ViewOf(war, 0);
    v.groups.push_back({1, 1000.0f, 0.0f, 8, TaskKind::Defend, 1, false});
    v.groups.push_back({2, 1000.0f, 0.0f, 8, TaskKind::Defend, 1, false});
    v.groups.push_back({3, 1000.0f, 0.0f, 8, TaskKind::Defend, 1, false});
    const auto intents = PlanStandard(v, CommanderTuning());
    int attackingB = 0;
    for (const Intent& i : intents)
    {
        CHECK(i.group != 1); // the first defender stays
        if (i.kind == Intent::Kind::Assign && i.task == TaskKind::Attack && i.objective == 2)
            attackingB++;
    }
    CHECK(attackingB == 2);
}

TEST_CASE("conquest commander: spare squads spread over the front instead of one column", "[conquest]")
{
    // A star: side 0 holds the centre (with its HQ behind it) and may attack three neutral towns.
    War war;
    war.sideCount = 2;
    const float pos[5][2] = {{0, 0}, {0, 1000}, {1000, 1000}, {0, 2000}, {-1000, 1000}};
    for (int i = 0; i < 5; i++)
    {
        ObjectiveDef d;
        d.name = "O" + std::to_string(i);
        d.x = pos[i][0];
        d.y = pos[i][1];
        d.kind = i == 0 ? ObjectiveKind::HQ : ObjectiveKind::Town;
        war.defs.push_back(d);
    }
    war.defs[0].neighbours = {1};
    war.defs[1].neighbours = {0, 2, 3, 4};
    war.defs[2].neighbours = {1};
    war.defs[3].neighbours = {1};
    war.defs[4].neighbours = {1};
    war.defs[0].initialOwner = 0;
    war.defs[1].initialOwner = 0;
    StartWar(war, Tuning(), 0.0f);
    SideView v = ViewOf(war, 0);
    for (int g = 1; g <= 6; g++)
        v.groups.push_back({g, 0.0f, 1000.0f, 8, TaskKind::None, -1, false});
    std::set<int> targets;
    for (const Intent& i : PlanStandard(v, CommanderTuning()))
        if (i.kind == Intent::Kind::Assign && i.task == TaskKind::Attack)
            targets.insert(i.objective);
    CHECK(targets.size() == 3);
}
