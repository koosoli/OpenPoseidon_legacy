#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <cstring>
#include <string_view>

namespace Poseidon
{
namespace
{
bool SupportedDiagnosticIdentity(const RStringB& name)
{
    const char* text = name;
    // RStringB::GetLength calls strlen. Reject at the factory's 128-byte
    // boundary without scanning an arbitrarily long inventory token first.
    for (int i = 0; i < 128; ++i)
    {
        if (!text[i]) return i != 0;
        if (static_cast<unsigned char>(text[i]) < 32 || static_cast<unsigned char>(text[i]) > 126) return false;
    }
    return false;
}
}

bool Landscape::ModernSourceDiagnosticInventoryMatches(const ModernSourceDiagnosticEntry& entry,
    const ObjectStreamPreparer::SourceEnvelopeSnapshot& latest) const
{
    return _modernObjectPreparer && entry.index < _modernObjectModels.size() &&
        entry.index < _modernObjectPreparer->ModelCount() &&
        SupportedDiagnosticIdentity(_modernObjectModels[entry.index]) &&
        std::string_view(static_cast<const char*>(_modernObjectModels[entry.index])) == entry.identity.data() &&
        latest.generation != 0 && latest.generation == entry.inventoryGeneration &&
        latest.modelIdentity == entry.identity.data();
}

ObjectStreamPreparer::SourceEnvelopeRequest Landscape::RequestModernSourceDiagnostic(uint32_t index)
{
    using Result = ObjectStreamPreparer::SourceEnvelopeRequest;
    if (!Foundation::IsMainThread()) return Result::WrongThread;
    if (!_modernObjectPreparer || !_modernObjectPreparer->Running() || index >= _modernObjectModels.size() ||
        index >= _modernObjectPreparer->ModelCount() || !SupportedDiagnosticIdentity(_modernObjectModels[index]))
        return Result::Unavailable;
    MaintainModernSourceDiagnostics();
    const auto now = std::chrono::steady_clock::now();
    ModernSourceDiagnosticEntry* slot = nullptr;
    for (auto& entry : _modernSourceDiagnostics)
    {
        if (entry.occupied && entry.index == index)
        {
            if (entry.pending && now < entry.expires) return entry.requestResult; // No TTL renewal/spin.
            slot = &entry; break;
        }
        if (!slot && (!entry.occupied || now >= entry.expires)) slot = &entry;
    }
    if (!slot) return Result::Capacity;
    const auto before = _modernObjectPreparer->QueryStaticSourceEnvelope(index);
    if (!before.generation || before.modelIdentity != static_cast<const char*>(_modernObjectModels[index]))
        return Result::Unavailable;
    *slot = {};
    slot->occupied = true; slot->index = index; slot->inventoryGeneration = before.generation;
    std::memcpy(slot->identity.data(), before.modelIdentity.data(), before.modelIdentity.size());
    slot->expires = now + std::chrono::seconds(ModernSourceDiagnosticTtlSeconds);
    slot->requestResult = _modernObjectPreparer->RequestWithStaticSourceEnvelope(index);
    slot->diagnosticParseToken = _modernObjectPreparer->QueryStaticSourceEnvelope(index).diagnosticParseToken;
    slot->pending = slot->requestResult == Result::Requested;
    _modernSourceDiagnosticsActive = true;
    return slot->requestResult;
}

Landscape::ModernSourceDiagnosticSnapshot Landscape::SnapshotModernSourceDiagnostic(uint32_t index) const
{
    ModernSourceDiagnosticSnapshot out;
    using Status = ModernSourceDiagnosticStatus;
    if (!Foundation::IsMainThread()) { out.status = Status::WrongThread; return out; }
    if (!_modernObjectPreparer || index >= _modernObjectModels.size() || index >= _modernObjectPreparer->ModelCount() ||
        !SupportedDiagnosticIdentity(_modernObjectModels[index])) { out.status = Status::Invalid; return out; }
    const ModernSourceDiagnosticEntry* entry = nullptr;
    for (const auto& candidate : _modernSourceDiagnostics)
        if (candidate.occupied && candidate.index == index) { entry = &candidate; break; }
    if (!entry) return out;
    out.requestResult = entry->requestResult;
    if (std::chrono::steady_clock::now() >= entry->expires) { out.status = Status::Expired; return out; }
    out.latestSnapshot = _modernObjectPreparer->QueryStaticSourceEnvelope(index);
    if (!ModernSourceDiagnosticInventoryMatches(*entry, out.latestSnapshot))
    { out.latestSnapshot = {}; out.status = Status::StaleInventory; return out; }
    if (out.latestSnapshot.diagnosticParseToken != entry->diagnosticParseToken)
    { out.latestSnapshot = {}; out.status = Status::Interrupted; return out; }
    const auto state = _modernObjectPreparer->Query(index);
    if (state == ObjectStreamPreparer::State::Failed || state == ObjectStreamPreparer::State::NotLoose)
    { out.status = Status::Refused; return out; } // No original parsed snapshot exists.
    if (out.latestSnapshot.attempted)
    {
        out.status = Status::LatestSnapshot;
        out.classNow = Streaming::ProbeStaticPlainRouteNow(out.latestSnapshot.plainSource,
            out.latestSnapshot.generation, out.latestSnapshot.modelIdentity, Pars);
        return out;
    }
    if (entry->pending && (state == ObjectStreamPreparer::State::Queued || state == ObjectStreamPreparer::State::Parsing))
    { out.status = Status::Pending; out.pendingDemand = true; }
    else if (entry->requestResult == ObjectStreamPreparer::SourceEnvelopeRequest::Requested)
        out.status = Status::Interrupted;
    else out.status = Status::Refused;
    return out;
}

void Landscape::MaintainModernSourceDiagnostics()
{
    if (!_modernSourceDiagnosticsActive || !Foundation::IsMainThread()) return;
    const auto now = std::chrono::steady_clock::now();
    bool active = false;
    for (auto& entry : _modernSourceDiagnostics)
    {
        if (!entry.occupied || now >= entry.expires) { entry.pending = false; continue; }
        active = true;
        if (!entry.pending) continue;
        if (!_modernObjectPreparer) { entry.pending = false; continue; }
        const auto latest = _modernObjectPreparer->QueryStaticSourceEnvelope(entry.index);
        if (!ModernSourceDiagnosticInventoryMatches(entry, latest) || latest.attempted ||
            latest.diagnosticParseToken != entry.diagnosticParseToken)
        { entry.pending = false; continue; }
        const auto state = _modernObjectPreparer->Query(entry.index);
        if (state != ObjectStreamPreparer::State::Queued && state != ObjectStreamPreparer::State::Parsing)
            entry.pending = false;
    }
    _modernSourceDiagnosticsActive = active;
}

void Landscape::PreserveModernSourceDiagnosticRequests(std::vector<uint32_t>& epochs, uint32_t epoch) const
{
    if (!_modernSourceDiagnosticsActive || !Foundation::IsMainThread() || !_modernObjectPreparer) return;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& entry : _modernSourceDiagnostics)
    {
        if (!entry.occupied || !entry.pending || now >= entry.expires || entry.index >= epochs.size()) continue;
        const auto latest = _modernObjectPreparer->QueryStaticSourceEnvelope(entry.index);
        if (!ModernSourceDiagnosticInventoryMatches(entry, latest) || latest.attempted ||
            latest.diagnosticParseToken != entry.diagnosticParseToken) continue;
        const auto state = _modernObjectPreparer->Query(entry.index);
        if (state == ObjectStreamPreparer::State::Queued || state == ObjectStreamPreparer::State::Parsing)
            epochs[entry.index] = epoch;
    }
}

void Landscape::ResetModernSourceDiagnostics()
{
    if (!Foundation::IsMainThread()) return;
    _modernSourceDiagnostics = {};
    _modernSourceDiagnosticsActive = false;
}
}
