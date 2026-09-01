#pragma once

// `poseidon timeline diff A B` -- compare two recorded simulation timelines and name the
// FIRST tick, record and field at which they stop agreeing.
//
// The recordings come from POSEIDON_AI_TIMELINE (Poseidon/AI/AITimeline.hpp). The point
// of having this as a tool rather than a test is that the two runs are whole missions in
// separate processes; nothing in-process can hold both.

#include <CLI/CLI.hpp>

namespace PoseidonTools
{

class TimelineCommand
{
  public:
    static void Setup(CLI::App& app);
};

} // namespace PoseidonTools
