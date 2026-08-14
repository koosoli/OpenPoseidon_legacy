#pragma once

#include <CLI/CLI.hpp>

namespace PoseidonTools
{

//! Arma Reforger `.xob` render geometry -> MLOD P3DM `.p3d`, plus a placement
//! manifest for a Reforger world (ARF-003).
//!
//! The runtime accepts MLOD P3DM as-is (ShapeDraw.cpp:376-391 -> ModelCache.cpp:86-98
//! -> MLODLoader.hpp:80-91), so nothing new has to be taught to the model loader --
//! only the container has to be written the way MLODStructures.hpp reads it.
class XobCommand
{
  public:
    static void Setup(CLI::App& app);
};

} // namespace PoseidonTools
