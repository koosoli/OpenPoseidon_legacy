#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Shape/RegistrationCostPolicy.hpp>
#include <array>

using Poseidon::render::RegistrationCost;
using Poseidon::render::ClassifyRegistrationCost;
using Poseidon::render::ChargeRegistrationCost;
using Poseidon::render::ClassifyDirectProxyRegistrationCost;

TEST_CASE("Partial registration inspects only bounded direct proxy work", "[streaming][registration-cost]")
{
    std::array<int, 65> proxies{};
    std::array<bool, 65> normal{}; normal.fill(true);
    size_t inspected = 0, children = 0;
    std::array<RegistrationCost, 128> costs; costs.fill(RegistrationCost::Reusable);
    auto classify = [&](int levels)
    {
        return ClassifyDirectProxyRegistrationCost(levels,
            [&](int level) { ++inspected; return normal.at(level); },
            [&](int level) { return proxies.at(level); },
            [&](int, int index) { ++children; return costs.at(index); });
    };
    CHECK(classify(64) == RegistrationCost::Reusable); CHECK(inspected == 64); CHECK(children == 0);
    inspected = 0;
    CHECK(classify(65) == RegistrationCost::Unknown); CHECK(inspected == 64); CHECK(children == 0);
    CHECK(classify(-1) == RegistrationCost::Unknown);
    proxies[0] = 128;
    CHECK(classify(65) == RegistrationCost::Reusable); CHECK(children == 128);
    // The first proxy-bearing normal LOD is authoritative; later LODs are not visited.
    proxies[1] = 129; children = 0;
    CHECK(classify(64) == RegistrationCost::Reusable); CHECK(children == 128);
    costs[3] = RegistrationCost::NeedsRegistration; children = 0;
    CHECK(classify(64) == RegistrationCost::NeedsRegistration); CHECK(children == 4);
    costs[3] = RegistrationCost::Unknown; children = 0;
    CHECK(classify(64) == RegistrationCost::Unknown); CHECK(children == 4);
    normal[0] = false; children = 0;
    CHECK(classify(64) == RegistrationCost::Unknown); CHECK(children == 0); // 129 direct proxies.
    proxies[1] = 0; proxies[2] = -1;
    CHECK(classify(64) == RegistrationCost::Unknown);
    proxies[2] = 1; costs[0] = RegistrationCost::Reusable;
    CHECK(classify(64) == RegistrationCost::Reusable); // Null/hidden/rejected child can be cheap.
}

TEST_CASE("Registration cost distinguishes reuse from bounded refill work", "[streaming][registration-cost]")
{
    size_t visited = 0;
    auto unavailable = [&](size_t) { ++visited; return false; };
    CHECK(ClassifyRegistrationCost(false, false, false, false, false, 0, unavailable) ==
          RegistrationCost::NeedsRegistration);
    CHECK(ClassifyRegistrationCost(true, true, true, true, false, 999, unavailable) == RegistrationCost::Reusable);
    CHECK(ClassifyRegistrationCost(true, false, false, false, false, 999, unavailable) == RegistrationCost::Reusable);
    CHECK(ClassifyRegistrationCost(true, false, true, false, true, 0, unavailable) == RegistrationCost::Unknown);
    CHECK(ClassifyRegistrationCost(true, false, false, true, false, 0, unavailable) == RegistrationCost::Unknown);
    CHECK(ClassifyRegistrationCost(true, false, false, true, true, 129, unavailable) == RegistrationCost::Unknown);
    CHECK(visited == 0); // Refusals and active registrations never touch texture metadata.

    std::array<bool, 128> live; live.fill(true);
    auto check = [&](size_t index) { ++visited; return live.at(index); };
    CHECK(ClassifyRegistrationCost(true, false, false, true, true, live.size(), check) == RegistrationCost::Reusable);
    CHECK(visited == 128);
    live[3] = false; visited = 0;
    CHECK(ClassifyRegistrationCost(true, false, false, true, true, live.size(), check) == RegistrationCost::NeedsRegistration);
    CHECK(visited == 4);
    CHECK(ClassifyRegistrationCost(true, false, false, true, true, 0, check) == RegistrationCost::Reusable);
}

TEST_CASE("Registration quota charges warm refills without changing legacy cold charging", "[streaming][registration-cost]")
{
    for (const auto cost : {RegistrationCost::Reusable, RegistrationCost::NeedsRegistration, RegistrationCost::Unknown})
    {
        CHECK(ChargeRegistrationCost(false, true, cost));
        CHECK_FALSE(ChargeRegistrationCost(false, false, cost));
        CHECK(ChargeRegistrationCost(true, true, cost));
    }
    CHECK_FALSE(ChargeRegistrationCost(true, false, RegistrationCost::Reusable));
    CHECK(ChargeRegistrationCost(true, false, RegistrationCost::NeedsRegistration));
    CHECK(ChargeRegistrationCost(true, false, RegistrationCost::Unknown));
    // Identical immutable owner observations produce identical count decisions.
    const std::array<RegistrationCost, 5> route{{RegistrationCost::Reusable,
        RegistrationCost::NeedsRegistration, RegistrationCost::Reusable,
        RegistrationCost::Unknown, RegistrationCost::NeedsRegistration}};
    size_t charged = 0, admitted = 0;
    for (const auto cost : route)
    {
        if (charged >= 2) break;
        ++admitted;
        charged += ChargeRegistrationCost(true, false, cost) ? 1 : 0;
    }
    CHECK(charged == 2); CHECK(admitted == 4); // Third refill is deferred, not refused.
}
