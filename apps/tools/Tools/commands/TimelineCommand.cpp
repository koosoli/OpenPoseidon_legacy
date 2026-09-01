#include "TimelineCommand.hpp"

#include <Poseidon/Core/StateTimeline.hpp>

#include <CLI/App.hpp>
#include <CLI/Option.hpp>
#include <iostream>
#include <string>

namespace PoseidonTools
{

namespace
{

using Poseidon::Determinism::CompareTimelines;
using Poseidon::Determinism::Divergence;
using Poseidon::Determinism::ReadTimeline;
using Poseidon::Determinism::StateTimeline;

struct DiffArgs
{
    std::string lhs;
    std::string rhs;
    int         minTicks = 1;
};

int RunDiff(const DiffArgs& args)
{
    StateTimeline a;
    StateTimeline b;

    if (const std::string err = ReadTimeline(a, args.lhs); !err.empty())
    {
        std::cerr << "error: " << err << "\n";
        return 2;
    }
    if (const std::string err = ReadTimeline(b, args.rhs); !err.empty())
    {
        std::cerr << "error: " << err << "\n";
        return 2;
    }

    // ANTI-VACUITY, and it is the whole reason this is not just a byte compare of two
    // files. Two recordings that captured nothing agree perfectly, and so do two that
    // captured a world in which nothing moved. Either would report EQUIVALENT and mean
    // "the harness did not run", which is the failure mode this project keeps hitting
    // from the other side -- a counter that cannot say "did not run".
    std::cout << "left  " << args.lhs << ": " << a.TickCount() << " ticks, " << a.EntryCount() << " fields, "
              << a.DistinctTickHashes() << " distinct tick hashes\n";
    std::cout << "right " << args.rhs << ": " << b.TickCount() << " ticks, " << b.EntryCount() << " fields, "
              << b.DistinctTickHashes() << " distinct tick hashes\n";

    if (a.TickCount() < args.minTicks || b.TickCount() < args.minTicks)
    {
        std::cerr << "error: fewer than " << args.minTicks
                  << " ticks recorded -- this is not a verdict, the recording did not happen\n";
        return 2;
    }
    if (a.DistinctTickHashes() <= 1 || b.DistinctTickHashes() <= 1)
    {
        std::cerr << "error: every tick hashes alike -- nothing in the recording moved, so an "
                     "agreement between these two would be vacuous\n";
        return 2;
    }

    const Divergence d = CompareTimelines(a, b);
    if (!d.Diverged())
    {
        std::cout << "EQUIVALENT: " << a.TickCount() << " ticks, " << a.EntryCount()
                  << " fields, identical entry for entry.\n";
        return 0;
    }

    std::cout << "DIVERGED\n  " << d.Describe() << "\n";
    return 1;
}

} // namespace

void TimelineCommand::Setup(CLI::App& app)
{
    auto* timeline = app.add_subcommand("timeline", "Compare recorded simulation timelines (POSEIDON_AI_TIMELINE)");
    timeline->require_subcommand(1);

    auto  args = std::make_shared<DiffArgs>();
    auto* diff = timeline->add_subcommand("diff", "Report the first tick, record and field at which two runs differ");
    diff->add_option("left", args->lhs, "First recording")->required();
    diff->add_option("right", args->rhs, "Second recording")->required();
    diff->add_option("--min-ticks", args->minTicks,
                     "Refuse a verdict on a recording shorter than this (default 1). A short "
                     "recording agrees with another short one and proves nothing.");
    // Exit codes: 0 equivalent, 1 diverged, 2 could not answer. The third is separate on
    // purpose -- "the recording did not happen" must never be reported as agreement.
    diff->callback([args] { std::exit(RunDiff(*args)); });
}

} // namespace PoseidonTools
