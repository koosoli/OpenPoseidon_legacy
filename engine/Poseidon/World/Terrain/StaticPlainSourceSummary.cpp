#include <Poseidon/World/Terrain/StaticPlainSourceSummary.hpp>
#include <Poseidon/Foundation/Common/Filenames.hpp>
#include <algorithm>

namespace Poseidon::Streaming
{
namespace
{
bool PrintableAscii(std::string_view text, size_t cap)
{
    if (text.size() > cap) return false;
    for (unsigned char c : text) if (c < 32 || c > 126) return false;
    return true;
}
bool ClassName(std::string_view name)
{
    if (name.size() != 5) return false;
    constexpr std::string_view expected = "class";
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
        if (c != expected[i]) return false;
    }
    return true;
}
}

StaticPlainSourceSummary BuildStaticPlainSourceSummary(
    const Model::Model& source, const StaticSourceEnvelope& evidence)
{
    StaticPlainSourceSummary out;
    out._generation = evidence.SourceGeneration();
    if (!out._generation || evidence.State() != StaticSourceEnvelopeState::SourceEvidence ||
        source.sourceFormat != "ODOL" || !Model::HasStaticOdolSourceAudit(source)) return out;
    out._state = StaticPlainSourceSummaryState::Unsupported;
    if (source.lodLevels.empty() || source.lodLevels.size() > StaticSourceDetail::MaxLods) return out;
    size_t propertyCount = 0;
    for (const auto& lod : source.lodLevels)
    {
        if (lod.mesh.properties.size() > StaticPlainRouteLimits::SourceProperties - propertyCount) return out;
        propertyCount += lod.mesh.properties.size();
        for (const auto& prop : lod.mesh.properties)
        {
            if (prop.name.empty() || !PrintableAscii(prop.name, StaticPlainRouteLimits::StringBytes) ||
                !PrintableAscii(prop.value, StaticPlainRouteLimits::StringBytes)) return out;
            // ALL LODs and duplicate properties count, irrespective of selection.
            if (ClassName(prop.name) && !prop.value.empty()) return out;
        }
    }
    if (source.sourcePath.empty() || !PrintableAscii(source.sourcePath, StaticPlainRouteLimits::PathBytes) ||
        source.sourcePath.size() >= StaticPlainRouteLimits::ShapeBankPathBytes) return out;
    const auto path = std::string_view(source.sourcePath);
    const auto separator = path.find_last_of("/\\");
    const auto basename = path.substr(separator == std::string_view::npos ? 0 : separator + 1);
    if (basename.empty() || basename.size() >= StaticPlainRouteLimits::StringBytes) return out;
    // Entire basename copy fits because total path is below the factory's 128.
    Foundation::GetFilename(out._modelName.data(), source.sourcePath.c_str());
    for (char& c : out._modelName)
        if (c == ' ' || c == '-' || c == '/' || c == '(' || c == ')') c = '_';
    const auto end = std::find(out._modelName.begin(), out._modelName.end(), '\0');
    if (end == out._modelName.begin() || end == out._modelName.end()) return out;
    out._modelNameBytes = uint8_t(end - out._modelName.begin());
    std::copy(path.begin(), path.end(), out._identity.begin());
    out._identityBytes = uint8_t(path.size());
    out._state = StaticPlainSourceSummaryState::SourceFacts;
    return out;
}
}
