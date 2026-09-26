#include <Poseidon/World/Terrain/StaticPlainRouteProbe.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <string_view>

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

std::string_view View(const Foundation::RStringB& text)
{
    // GetLength calls strlen. Bound the scan before inspecting config text.
    const char* data = text.Data();
    for (size_t n = 0; n <= StaticPlainRouteLimits::StringBytes; ++n)
        if (!data[n]) return {data, n};
    return {data, StaticPlainRouteLimits::StringBytes + 1};
}

bool EqualAsciiNoCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; };
    for (size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

struct LocalResult { const ParamEntry* entry = nullptr; bool supported = false; };

// Do not invoke recursive FindEntry: a base chain has no public bounded traversal
// seam. Refuse all inheritance, including inheritance on the injected root.
LocalResult FindLocal(const ParamEntry& parent, std::string_view key, int cap)
{
    const auto* cls = parent.GetClassInterface();
    if (!cls || parent.IsError() || cls->HasBase()) return {};
    const int count = parent.GetEntryCount();
    if (count < 0 || count > cap) return {};
    const ParamEntry* found = nullptr;
    for (int i = 0; i < count; ++i)
    {
        const auto& entry = parent.GetEntry(i);
        const auto name = View(entry.GetName());
        if (entry.IsError() || name.empty() || !PrintableAscii(name, StaticPlainRouteLimits::StringBytes)) return {};
        if (EqualAsciiNoCase(name, key))
        {
            if (found) return {}; // ambiguous duplicates are not a route proof
            found = &entry;
        }
    }
    return {found, true};
}
}

StaticPlainRouteObservation ProbeStaticPlainRouteNow(
    const Model::Model& source, const StaticSourceEnvelope& evidence,
    uint64_t currentGeneration, const ParamEntry& liveConfigRoot)
{
    using Result = StaticPlainRouteObservation;
    // Reject before dereferencing the owner-only config (or any source payload).
    if (!Foundation::IsMainThread()) return Result::WrongThread;
    if (!currentGeneration || evidence.SourceGeneration() != currentGeneration) return Result::StaleSource;
    const auto summary = BuildStaticPlainSourceSummary(source, evidence);
    return ProbeStaticPlainRouteNow(summary, currentGeneration, source.sourcePath, liveConfigRoot);
}

StaticPlainRouteObservation ProbeStaticPlainRouteNow(
    const StaticPlainSourceSummary& source, uint64_t currentGeneration,
    std::string_view exactModelIdentity, const ParamEntry& liveConfigRoot)
{
    using Result = StaticPlainRouteObservation;
    if (!Foundation::IsMainThread()) return Result::WrongThread;
    if (!currentGeneration || source.SourceGeneration() != currentGeneration) return Result::StaleSource;
    if (source.State() == StaticPlainSourceSummaryState::Unknown) return Result::Unknown;
    if (source.State() != StaticPlainSourceSummaryState::SourceFacts) return Result::Unsupported;
    if (source.ModelIdentity() != exactModelIdentity) return Result::StaleSource;

    const auto models = FindLocal(liveConfigRoot, "CfgModels", StaticPlainRouteLimits::RootEntries);
    if (!models.supported) return Result::Unsupported;
    if (!models.entry) return Result::EmptyClassNow;
    const auto model = FindLocal(*models.entry, source.ConfigModelName(), StaticPlainRouteLimits::ModelEntries);
    if (!model.supported) return Result::Unsupported;
    if (!model.entry) return Result::EmptyClassNow;
    const auto properties = FindLocal(*model.entry, "properties", StaticPlainRouteLimits::OverrideEntries);
    if (!properties.supported) return Result::Unsupported;
    if (!properties.entry) return Result::EmptyClassNow;
    const auto& pairs = *properties.entry;
    if (!pairs.IsArray() || pairs.IsError()) return Result::Unsupported;
    const int count = pairs.GetSize();
    if (count < 0 || count > StaticPlainRouteLimits::PropertyElements || count % 2) return Result::Unsupported;
    for (int i = 0; i < count; i += 2)
    {
        const auto& key = pairs[i];
        const auto& value = pairs[i + 1];
        if (!key.IsTextValue() || !value.IsTextValue()) return Result::Unsupported;
        const auto keyText = key.GetValue();
        const auto valueText = value.GetValue();
        if (View(keyText).empty() || !PrintableAscii(View(keyText), StaticPlainRouteLimits::StringBytes) ||
            !PrintableAscii(View(valueText), StaticPlainRouteLimits::StringBytes)) return Result::Unsupported;
        if (EqualAsciiNoCase(View(keyText), "class") && !View(valueText).empty()) return Result::Unsupported;
    }
    return Result::EmptyClassNow;
}
}
