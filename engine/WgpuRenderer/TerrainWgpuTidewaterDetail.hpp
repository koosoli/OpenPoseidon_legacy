// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>

namespace Poseidon
{
constexpr int TidewaterDetailSize = 512;

//! Tidewater's tileable terrain detail heights (DetailTextures.js port): RGBA8, TidewaterDetailSize
//! square; R rock plates, G soil, B sand, A fbm. Deterministic; ~0.3 s to build.
std::vector<uint8_t> BuildTidewaterDetail();
} // namespace Poseidon
