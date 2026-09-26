#pragma once

#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Terrain/StaticPlainSourceSummary.hpp>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace PoseidonTools
{
// Raw IR evidence only. These limits bound output, not parser allocations.
inline constexpr size_t SimulationInfoMaxLods = 64;
inline constexpr size_t SimulationInfoMaxProperties = 64;
inline constexpr size_t SimulationInfoMaxStringBytes = 256;

inline std::string SimulationInfoQuoted(std::string_view value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : value.substr(0, SimulationInfoMaxStringBytes))
    {
        if (ch == '"' || ch == '\\') out << '\\' << char(ch);
        else if (ch < 32 || ch == 127)
            out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(ch) << std::dec;
        else out << char(ch);
    }
    out << '"';
    if (value.size() > SimulationInfoMaxStringBytes)
        out << " omittedBytes=" << value.size() - SimulationInfoMaxStringBytes;
    return out.str();
}

inline std::string FormatModelSimulationSourceInfo(const Poseidon::Model::Model& model)
{
    std::ostringstream out;
    out << "Simulation source info: parsed IR evidence only\n"
           "Parser coverage: absent fields describe this parser representation; source completeness is not certified.\n"
           "No Shape adaptation, constructor/config resolution, or simulation Ready certification.\n";
    out << "Source: " << SimulationInfoQuoted(model.sourcePath) << '\n';
    out << "Format: " << SimulationInfoQuoted(model.sourceFormat) << " version=" << model.sourceVersion << '\n';
    const auto& audit = model.sourceAudit;
    out << "Source audit: producer=" << audit.producerVersion << " revision=" << audit.sourceRevision
        << " geometryCoverage=" << uint32_t(audit.geometryCoverage) << " observations=" << audit.observations
        << " declaredLods=" << audit.declaredLods << " decodedLods=" << audit.decodedLods << '\n';
    out << "  directoryHasAnimations=" << audit.directoryHasAnimations
        << " animationClasses=" << audit.directoryAnimationClasses << " skeletonDeclared=" << audit.skeletonDeclared
        << " skeletonBones=" << audit.skeletonBones << " keyframes=" << audit.keyframeCount
        << " keyframePayloadDiscarded=" << audit.keyframePayloadDiscarded
        << " vertexBoneReferences=" << audit.vertexBoneReferenceCount
        << " neighbourBoneReferences=" << audit.neighbourBoneReferenceCount << '\n';
    out << "  Zero counts establish absence only when their corresponding observed bits are set.\n";
    // Explicit offline inspection only. Caller supplies the original immutable
    // raw input; these synthetic tokens cannot serve as runtime inventory proof.
    using namespace Poseidon::Streaming;
    constexpr uint64_t diagnosticGeneration = 1;
    const auto envelope = BuildStaticSourceEnvelope(model, diagnosticGeneration, StaticSourceCoverage::FullCompiledIR);
    const auto summary = BuildStaticPlainSourceSummary(model, envelope);
    out << "Offline static source diagnostic: generation=" << diagnosticGeneration
        << " (synthetic; not world/inventory identity or content freshness)\n"
           "  Scope: original immutable raw input only; no live config verdict, constructor, final bounds or Ready proof.\n"
           "  Identity policy uses sourcePath as supplied; extracted disk paths are not resolved to world inventory names.\n";
    out << "  Source envelope: state="
        << (envelope.State() == StaticSourceEnvelopeState::SourceEvidence ? "SourceEvidence" : "Unknown")
        << " payloadBytes=" << envelope.PayloadBytes() << " reasons=[";
    using Reason = StaticSourceEnvelopeReason;
    const std::pair<Reason, const char*> reasons[] = {
        {Reason::Generation,"Generation"}, {Reason::Format,"Format"}, {Reason::Capacity,"Capacity"},
        {Reason::NonFinite,"NonFinite"}, {Reason::NoVertices,"NoVertices"}, {Reason::MissingRole,"MissingRole"},
        {Reason::Proxy,"Proxy"}, {Reason::Animation,"Animation"}, {Reason::LandConform,"LandConform"},
        {Reason::InvalidFace,"InvalidFace"}, {Reason::InconsistentPurpose,"InconsistentPurpose"},
        {Reason::IncompleteSource,"IncompleteSource"}
    };
    bool firstReason = true;
    for (const auto& [reason, name] : reasons)
        if (envelope.HasReason(reason)) { out << (firstReason ? "" : ",") << name; firstReason = false; }
    out << ']';
    if (const auto radius = envelope.RadiusForGeneration(diagnosticGeneration))
        out << " sourceRadius=" << std::setprecision(9) << *radius;
    out << '\n';
    const char* summaryState = "Unknown";
    const char* summaryReason = "envelope-not-source-evidence-or-incomplete-source-audit";
    switch (summary.State())
    {
        case StaticPlainSourceSummaryState::SourceFacts:
            summaryState = "SourceFacts"; summaryReason = "none"; break;
        case StaticPlainSourceSummaryState::Unsupported:
            summaryState = "Unsupported"; summaryReason = "source-path-or-property-policy"; break;
        case StaticPlainSourceSummaryState::Unknown: break;
    }
    out << "  Plain source summary: state=" << summaryState << " reason=" << summaryReason
        << " identity=" << SimulationInfoQuoted(summary.ModelIdentity())
        << " configModelName=" << SimulationInfoQuoted(summary.ConfigModelName()) << '\n';
    out << "allowAnimation: " << model.allowAnimation << '\n';
    out << "LOD count: " << model.lodLevels.size() << '\n';
    const size_t count = std::min(model.lodLevels.size(), SimulationInfoMaxLods);
    for (size_t i = 0; i < count; ++i)
    {
        const auto& lod = model.lodLevels[i];
        const auto& mesh = lod.mesh;
        size_t proxySelections = 0, weightedSelections = 0, nonUnitSourceWeights = 0, nonfinitePositions = 0;
        for (const auto& selection : mesh.selections)
        {
            proxySelections += selection.name.starts_with("proxy:");
            weightedSelections += !selection.vertexWeights.empty();
            for (auto weight : selection.sourceVertexWeights) nonUnitSourceWeights += weight != 1;
        }
        for (const auto& vertex : mesh.vertices)
            nonfinitePositions += !std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) ||
                                  !std::isfinite(vertex.position.z);
        out << "LOD " << i << ": resolution=" << std::setprecision(9) << lod.resolution
            << " purpose=" << Poseidon::Model::LodPurposeName(lod.purpose)
            << " encoding=" << SimulationInfoQuoted(lod.sourceEncoding) << '\n';
        out << "  vertices=" << mesh.vertices.size() << " triangles=" << mesh.triangles.size()
            << " quads=" << mesh.quads.size() << " nonfinitePositions=" << nonfinitePositions << '\n';
        out << "  proxies=" << mesh.proxies.size() << " frames=" << mesh.frames.size()
            << " selections=" << mesh.selections.size() << " proxySelections=" << proxySelections
            << " weightedSelections=" << weightedSelections << " nonUnitSourceWeights=" << nonUnitSourceWeights << '\n';
        out << "  properties=" << mesh.properties.size() << '\n';
        const size_t properties = std::min(mesh.properties.size(), SimulationInfoMaxProperties);
        for (size_t p = 0; p < properties; ++p)
            out << "    [" << p << "] name=" << SimulationInfoQuoted(mesh.properties[p].name)
                << " value=" << SimulationInfoQuoted(mesh.properties[p].value) << '\n';
        if (properties < mesh.properties.size()) out << "  omittedProperties=" << mesh.properties.size() - properties << '\n';
    }
    if (count < model.lodLevels.size()) out << "omittedLods=" << model.lodLevels.size() - count << '\n';
    return out.str();
}
} // namespace PoseidonTools
