#pragma once

#include <Poseidon/Asset/Formats/Common/FormatDetector.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <string>

namespace Poseidon::Asset::Formats
{

// The P3D readers report malformed input by throwing. Callers that sit on an engine
// entry point cannot let that escape, so this reports the failure instead.
//
// This matters more here than upstream: the asset pipeline deliberately reads models from
// six game generations (OFP/CWA, Arma 1, Arma 2, Arma 3, DayZ, Reforger), several of them
// only partly supported, so a reader hitting a field it cannot decode is an expected
// outcome, not a defect. One such model must cost a log line, not the process.
bool TryLoadP3D(const std::string& path, Poseidon::Model::Model& model, FormatInfo& format, std::string& error);

} // namespace Poseidon::Asset::Formats
