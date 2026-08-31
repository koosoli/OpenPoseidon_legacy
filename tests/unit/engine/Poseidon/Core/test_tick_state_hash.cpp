// SIM-815 -- unit tests for the per-tick state recorder (Core/TickStateHash).
//
// The recorder's job: two runs' logs, diffed by tick, name the FIRST divergent
// tick and the FIELD CLASS that moved -- one level below what the frame hash
// (EngineWgpu::HashPublishedFrame) can see. These tests exercise the fold, the
// field registry and the anti-vacuity counter on a synthetic model of a run,
// with the same rules SIM-808 and SIM-812 imposed on their harnesses:
//
//  * the equivalence case must produce a healthy DISTINCT hash count -- a
//    model whose ticks all hash alike would agree with a wrong implementation
//    forever (SIM-813 hit exactly this with a periodic model and the check
//    caught it);
//  * a perturbation of ONE field at ONE tick must be reported at that tick and
//    in that field class, and NOT in the others;
//  * the perturbation is also placed at the LAST tick, so "first divergent
//    tick" is shown to track the perturbation rather than defaulting early.

#include <Poseidon/Core/TickStateHash.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace Poseidon::Determinism;

namespace
{

constexpr int kTicks = 96;
constexpr int kEntities = 12;

struct ModelOptions
{
    // Tick at which to nudge entity 3's position by one ulp; -1 for none.
    int perturbPosTick = -1;
    // Tick at which the RNG draw counter takes one extra draw; -1 for none.
    // Cumulative from that tick on, as a real draw-count divergence would be.
    int perturbRngTick = -1;
    // Freeze the world: every tick records identical state (the vacuous run).
    bool frozen = false;
};

// A well-mixed, fully deterministic state evolution -- NOT periodic, per the
// trap SIM-813 recorded: its first model gave 8 distinct tick hashes over 96
// ticks and would have agreed with a wrong implementation on the other 88.
float Mix(int entity, int tick, int axis)
{
    const float a = std::sin(0.7f * static_cast<float>(entity) + 0.31f * static_cast<float>(tick) +
                             1.13f * static_cast<float>(axis));
    return a + 0.01f * static_cast<float>(tick) * static_cast<float>(entity + 1);
}

std::vector<TickStateRow> RunModel(const ModelOptions& opt)
{
    TickStateHash rec;
    std::vector<TickStateRow> rows;
    std::uint64_t rngDraws = 0;
    for (int t = 0; t < kTicks; t++)
    {
        rec.BeginTick();
        for (int e = 0; e < kEntities; e++)
        {
            const int mt = opt.frozen ? 0 : t;
            float px = Mix(e, mt, 0);
            const float py = Mix(e, mt, 1);
            const float pz = Mix(e, mt, 2);
            if (t == opt.perturbPosTick && e == 3)
            {
                // One ulp -- the smallest representable nudge, which is what a
                // real cross-run float divergence first looks like (SIM-808
                // measured 2 ulp as the first observable consequence of an
                // impulse).
                px = std::nextafter(px, px + 1.0f);
            }
            rec.FoldVec3(TickFieldClass::Positions, px, py, pz);
            rec.FoldVec3(TickFieldClass::Velocities, Mix(e, mt, 3), Mix(e, mt, 4), Mix(e, mt, 5));
        }
        rec.FoldVec3(TickFieldClass::Camera, Mix(99, opt.frozen ? 0 : t, 0), Mix(99, opt.frozen ? 0 : t, 1),
                     Mix(99, opt.frozen ? 0 : t, 2));

        // Draw-count model: a handful of draws per tick, deterministic. A
        // frozen world takes no draws -- its counter must hold still so the
        // vacuity case really is vacuous.
        if (!opt.frozen)
        {
            rngDraws += 5;
        }
        if (opt.perturbRngTick >= 0 && t >= opt.perturbRngTick)
        {
            rngDraws += 1; // the extra draw, cumulative like the real counter
        }
        rec.SetCounter(TickCounter::RngDraws, rngDraws);
        rec.SetCounter(TickCounter::ShapeCount, 100);
        rec.SetCounter(TickCounter::EntityCount, kEntities);
        rec.SetCounter(TickCounter::Cloudlets, 0);
        rec.SetCounter(TickCounter::TimeMs, static_cast<std::uint64_t>(t) * 16u);
        rows.push_back(rec.EndTick());
    }
    return rows;
}

struct RowDiff
{
    int  tick = -1; // first divergent tick, -1 if none
    bool pos = false, vel = false, cam = false;
    bool rng = false, shapes = false, ents = false, clouds = false;
};

RowDiff FirstDivergence(const std::vector<TickStateRow>& a, const std::vector<TickStateRow>& b)
{
    RowDiff d;
    REQUIRE(a.size() == b.size());
    for (std::size_t t = 0; t < a.size(); t++)
    {
        if (a[t].all == b[t].all)
        {
            continue;
        }
        d.tick = static_cast<int>(t);
        auto cls = [&](TickFieldClass c)
        { return a[t].classHash[static_cast<std::size_t>(c)] != b[t].classHash[static_cast<std::size_t>(c)]; };
        auto ctr = [&](TickCounter c)
        { return a[t].counter[static_cast<std::size_t>(c)] != b[t].counter[static_cast<std::size_t>(c)]; };
        d.pos = cls(TickFieldClass::Positions);
        d.vel = cls(TickFieldClass::Velocities);
        d.cam = cls(TickFieldClass::Camera);
        d.rng = ctr(TickCounter::RngDraws);
        d.shapes = ctr(TickCounter::ShapeCount);
        d.ents = ctr(TickCounter::EntityCount);
        d.clouds = ctr(TickCounter::Cloudlets);
        return d;
    }
    return d;
}

} // namespace

TEST_CASE("identical runs produce identical tick rows and a healthy distinct count",
          "[determinism][tickhash][sim815]")
{
    const auto a = RunModel({});
    const auto b = RunModel({});

    const RowDiff d = FirstDivergence(a, b);
    CHECK(d.tick == -1);

    // Anti-vacuity, SIM-808's rule: a recorder that reports a constant is
    // measuring nothing. The distinct count rides on the last row.
    CHECK(a.back().distinct >= static_cast<std::uint64_t>(kTicks / 4));
    // And it really is the count of distinct `all` values, monotone per tick.
    CHECK(a.front().distinct == 1);
    CHECK(a.back().distinct <= static_cast<std::uint64_t>(kTicks));
}

TEST_CASE("a frozen world announces its own vacuity", "[determinism][tickhash][sim815]")
{
    ModelOptions opt;
    opt.frozen = true;
    const auto rows = RunModel(opt);
    // Every tick identical -> one distinct hash. The log line carries this
    // number so a vacuous capture is visible on sight, without a second pass.
    CHECK(rows.back().distinct == 1);
}

TEST_CASE("a one-ulp position nudge names its tick and only the position class",
          "[determinism][tickhash][sim815]")
{
    constexpr int kPerturbTick = 47;
    ModelOptions opt;
    opt.perturbPosTick = kPerturbTick;

    const auto clean = RunModel({});
    const auto bent = RunModel(opt);

    const RowDiff d = FirstDivergence(clean, bent);
    INFO("first divergent tick " << d.tick << " pos=" << d.pos << " vel=" << d.vel << " cam=" << d.cam
                                 << " rng=" << d.rng);
    CHECK(d.tick == kPerturbTick);
    CHECK(d.pos);
    // The other classes must NOT be implicated -- attribution is the point.
    CHECK_FALSE(d.vel);
    CHECK_FALSE(d.cam);
    CHECK_FALSE(d.rng);
    CHECK_FALSE(d.shapes);

    // The model does not propagate state between ticks, so a one-tick nudge
    // diverges at exactly one tick -- which also pins that the recorder holds
    // no accidental cross-tick state in the class accumulators.
    int divergent = 0;
    for (std::size_t t = 0; t < clean.size(); t++)
    {
        if (clean[t].all != bent[t].all)
        {
            divergent++;
        }
    }
    CHECK(divergent == 1);
}

TEST_CASE("a perturbation at the last tick is reported at the last tick",
          "[determinism][tickhash][sim815]")
{
    // The control on the case above (SIM-808's rule): a comparison that always
    // reported an early tick would pass a mid-run perturbation; requiring the
    // answer to move with the perturbation is what makes "first" mean first.
    ModelOptions opt;
    opt.perturbPosTick = kTicks - 1;
    const RowDiff d = FirstDivergence(RunModel({}), RunModel(opt));
    CHECK(d.tick == kTicks - 1);
    CHECK(d.pos);
}

TEST_CASE("an extra RNG draw names the rng counter, not the hashed classes",
          "[determinism][tickhash][sim815]")
{
    constexpr int kPerturbTick = 23;
    ModelOptions opt;
    opt.perturbRngTick = kPerturbTick;

    const RowDiff d = FirstDivergence(RunModel({}), RunModel(opt));
    CHECK(d.tick == kPerturbTick);
    CHECK(d.rng);
    CHECK_FALSE(d.pos);
    CHECK_FALSE(d.vel);
    CHECK_FALSE(d.cam);
}

TEST_CASE("the fold is length-prefixed and count-suffixed", "[determinism][tickhash][sim815]")
{
    // Two items "ab" + "c" must not collide with "a" + "bc" (length prefix)...
    TickStateHash a;
    a.BeginTick();
    a.FoldBytes(TickFieldClass::Positions, "ab", 2);
    a.FoldBytes(TickFieldClass::Positions, "c", 1);
    const auto ra = a.EndTick();

    TickStateHash b;
    b.BeginTick();
    b.FoldBytes(TickFieldClass::Positions, "a", 1);
    b.FoldBytes(TickFieldClass::Positions, "bc", 2);
    const auto rb = b.EndTick();

    CHECK(ra.classHash[0] != rb.classHash[0]);

    // ...and an empty item is distinguishable from no item (count suffix).
    TickStateHash c;
    c.BeginTick();
    c.FoldBytes(TickFieldClass::Positions, nullptr, 0);
    const auto rc = c.EndTick();

    TickStateHash e;
    e.BeginTick();
    const auto re = e.EndTick();

    CHECK(rc.classHash[0] != re.classHash[0]);
}

TEST_CASE("negative zero and zero hash differently -- bits, never values",
          "[determinism][tickhash][sim815]")
{
    TickStateHash a;
    a.BeginTick();
    a.FoldVec3(TickFieldClass::Positions, 0.0f, 1.0f, 2.0f);
    const auto ra = a.EndTick();

    TickStateHash b;
    b.BeginTick();
    b.FoldVec3(TickFieldClass::Positions, -0.0f, 1.0f, 2.0f);
    const auto rb = b.EndTick();

    CHECK(ra.classHash[0] != rb.classHash[0]);
}

TEST_CASE("the formatted row carries every column and the grep key", "[determinism][tickhash][sim815]")
{
    TickStateHash rec;
    rec.BeginTick();
    rec.FoldVec3(TickFieldClass::Positions, 1.0f, 2.0f, 3.0f);
    rec.SetCounter(TickCounter::RngDraws, 42);
    rec.SetCounter(TickCounter::TimeMs, 1600);
    const auto row = rec.EndTick();
    const std::string line = TickStateHash::FormatRow(row);

    CHECK(line.find("TickHash tick=0 ") == 0);
    CHECK(line.find("tms=1600") != std::string::npos);
    CHECK(line.find("rng=42") != std::string::npos);
    CHECK(line.find("pos=") != std::string::npos);
    CHECK(line.find("vel=") != std::string::npos);
    CHECK(line.find("cam=") != std::string::npos);
    CHECK(line.find("shapes=") != std::string::npos);
    CHECK(line.find("ents=") != std::string::npos);
    CHECK(line.find("clouds=") != std::string::npos);
    CHECK(line.find("all=") != std::string::npos);
    CHECK(line.find("distinct=1") != std::string::npos);
}
