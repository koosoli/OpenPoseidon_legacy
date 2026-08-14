#pragma once

#include <CLI/CLI.hpp>

namespace PoseidonTools
{

//! Enfusion `.pak` archives and the terrain inside them (Arma Reforger).
class PakCommand
{
  public:
    static void Setup(CLI::App& app);
};

} // namespace PoseidonTools
