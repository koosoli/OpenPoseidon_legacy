#pragma once

namespace Poseidon {
// Presentation calibration of the ordinary rain slider into conserved water
// input. Strength 1 supplies 90 mm per game hour; this is not a claim that the
// visual weather slider measures real rainfall. Both backends share the rate.
inline constexpr double RainWaterMaximumRainMetresPerSecond = 0.000025;
}
