#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyState.hpp>
#include <Poseidon/World/Terrain/SimulationFireCollision.hpp>
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/AI/EntityAI.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <Poseidon/IO/ParamFileExt.hpp>

namespace Poseidon
{
using namespace Streaming;
namespace
{
using State = SimulationResidencyState;
using Status = SimulationResidencyStatus;
bool PositiveColdEnabled()
{
    static const bool enabled = [] { const char* v = std::getenv("WGR_SIMULATION_POSITIVE_COLD"); return v && std::strcmp(v, "1") == 0; }();
    return enabled;
}
CoveragePoint Point(Vector3Par p) { return {p.X(), p.Y(), p.Z()}; }
bool ValidRegion(const CoveragePoint& a, const CoveragePoint& b, double radius)
{
    if (!std::isfinite(radius) || radius < 0 || radius > EngineBroadphaseSafeMagnitude()) return false;
    for (size_t i = 0; i < 3; ++i)
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]) ||
            std::abs(a[i]) > EngineBroadphaseSafeMagnitude() || std::abs(b[i]) > EngineBroadphaseSafeMagnitude()) return false;
    return true;
}
template<class Budget>
Status MetadataStatus(const State& s, const std::vector<RStringB>& paths, Budget&& budget)
{
    if (s.stats.status == Status::CapacityExceeded || s.watchCapacityExceeded) return Status::CapacityExceeded;
    if (s.inventoryInvalid) return Status::Unknown;
    if (!s.inventoryComplete) return Status::Pending;
    if (s.activeModelCapacityExceeded)
    {
        // Preserve the historical census refusal until a complete actual bank
        // observation exists, without retaining/loading the full inventory.
        if (!s.registeredMetadataEverComplete) return Status::CapacityExceeded;
        const auto status = s.ValidateRegisteredMetadata(Shapes, paths, Pars, budget);
        if (status != Status::Ready) return status;
        return s.registeredMetadataDirty ? Status::Pending : Status::Ready;
    }
    return s.ValidateAdmittedMetadata(budget);
}
bool WatchMatches(const State& s, const State::Watch& watch)
{
    Object* object = static_cast<Object*>(watch.object);
    if (!object) return true; // Joined owner cleanup retires the tombstone later.
    if (!watch.authored || watch.model >= s.groups.size()) return false;
    const auto& g = s.groups[watch.model];
    auto* expected = s.activeModelCapacityExceeded ? static_cast<LODShapeWithShadow*>(g.registeredShape.GetRef()) : g.shape.GetRef();
    return watch.MatchesImmutableObject(expected, s.activeModelCapacityExceeded ? SimulationFactoryRoute::Plain : g.route);
}
template<class Budget>
Status ObservedStatus(const State& s, const std::vector<RStringB>& paths, Budget&& budget)
{
    const auto metadata = MetadataStatus(s, paths, budget);
    if (metadata != Status::Ready) return metadata;
    // Complete immediate owner read, bounded by16384 registered weak instances.
    // No stale incremental prefix can establish a Ready query.
    for (const auto& [index, watch] : s.watches)
    {
        if (!budget())
        { if (s.activeModelCapacityExceeded) ++s.registeredValidationBudgetRefusals; return Status::Pending; }
        if (!WatchMatches(s, watch)) return Status::Unknown;
    }
    return Status::Ready;
}
Status ObservedStatus(const State& s, const std::vector<RStringB>& paths)
{
    const auto started = std::chrono::steady_clock::now();
    return ObservedStatus(s, paths, [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() < 2; });
}
struct RegisteredBorrowRollback
{
    State* state = nullptr;
    uint32_t model = 0;
    ~RegisteredBorrowRollback() { if (state) state->ReleaseUnusedRegisteredBorrow(model); }
};
bool ActorUnchanged(const State::Region& r, const Object& actor)
{
    const auto speed = actor.ObjectSpeed();
    return static_cast<Object*>(r.actor) == &actor && r.actorId == actor.ID() && r.actorFrame == SimulationFrameSample(actor.Transform()) &&
        r.actorSpeed == std::array<float, 3>{speed.X(), speed.Y(), speed.Z()} && r.actorRadius == actor.GetRadius();
}
Status RegionRecordStatus(const State& s, const State::Region& r, const Object& actor)
{
    if (!ActorUnchanged(r, actor)) return Status::Pending;
    if (r.status != Status::Ready || r.planGeneration != s.generation)
        return r.status == Status::Ready ? Status::Pending : r.status;
    for (auto index : r.active)
    {
        const auto entry = s.entries.find(index);
        const auto watch = s.watches.find(index);
        if (entry == s.entries.end() || watch == s.watches.end()) return Status::Unknown;
        Object* object = static_cast<Object*>(entry->second.object);
        if (!object || !s.MatchesQueryBorrow(entry->second) || object != static_cast<Object*>(watch->second.object) ||
            object->GetShape() != entry->second.shape.GetRef() ||
            SimulationFrameSample(object->Transform()) != entry->second.frame || object->GetRadius() != entry->second.radius)
            return Status::Unknown;
    }
    return Status::Ready;
}
bool Overlap(const CoverageEnvelope& box, const State::Region& r)
{
    // Conservative swept AABB: over-admission is permitted; omission is not.
    for (size_t axis = 0; axis < 3; ++axis)
        if (box.max[axis] < std::min(r.from[axis], r.to[axis]) - r.radius ||
            box.min[axis] > std::max(r.from[axis], r.to[axis]) + r.radius) return false;
    return true;
}
}

bool Landscape::ModernSimulationResidencyEnabled() const
{
    static const bool enabled = [] { const char* v = std::getenv("WGR_SIMULATION_RESIDENCY"); return v && std::strcmp(v, "1") == 0; }();
    return enabled && _modernObjectStreaming;
}
void Landscape::ResetModernSimulationResidency()
{
    ResetModernSourceDiagnostics();
    _modernSimulation.reset();
}
void Landscape::EnsureModernSimulationResidency()
{
    if (!ModernSimulationResidencyEnabled() || !Foundation::IsMainThread() || _modernSimulation) return;
    _modernSimulation = std::make_unique<State>();
    auto& s = *_modernSimulation;
    s.stats.status = Status::Pending;
    s.stats.inventoryTotal = _modernObjectPlacements.size();
    s.stats.models = _modernObjectModels.size();
    const auto preflight = PreflightSimulationMetadata(_modernObjectPlacements.size(), _modernObjectModels.size(), sizeof(State::Group));
    if (preflight.refusal != SimulationMetadataRefusal::None)
    {
        s.stats.status = Status::CapacityExceeded;
        if (preflight.refusal == SimulationMetadataRefusal::PlacementCount) ++s.stats.metadataPlacementRefusals;
        else if (preflight.refusal == SimulationMetadataRefusal::ModelCount) ++s.stats.metadataModelRefusals;
        else ++s.stats.metadataByteRefusals;
        return;
    }
    s.groups.resize(_modernObjectModels.size());
    // Charge actual fixed-record capacity, not requested rows or allocator RSS.
    const auto actual = PreflightSimulationMetadata(_modernObjectPlacements.size(), s.groups.capacity(), sizeof(State::Group));
    if (actual.refusal != SimulationMetadataRefusal::None)
    {
        std::vector<State::Group>().swap(s.groups);
        ++s.stats.metadataByteRefusals; s.stats.status = Status::CapacityExceeded; return;
    }
    s.stats.metadataRecordBytes = actual.recordBytes;
    if (!_modernObjectPreparer) EnsureModernObjectPreparer();
    if (!_modernObjectPreparer) s.inventoryInvalid = true;
}
void Landscape::ObserveModernSimulationPlacement(uint32_t index, Object* object)
{
    if (!ModernSimulationResidencyEnabled() || !Foundation::IsMainThread() || !_modernSimulation || !object) return;
    auto& s = *_modernSimulation;
    if (s.stats.status == Status::CapacityExceeded || index >= _modernObjectPlacements.size()) return;
    auto found = s.watches.find(index);
    if (found != s.watches.end() && static_cast<Object*>(found->second.object) == object) return;
    // Camera/explicit admissions can populate a previously empty selected cell
    // without moving the actor. Invalidate completed positive scans cheaply.
    if (s.positiveAdmissionGeneration == std::numeric_limits<uint64_t>::max())
    { s.inventoryInvalid = true; return; }
    ++s.positiveAdmissionGeneration;
    // Full query-watch refusal remains sticky, but this actual admission must
    // still invalidate positive selection; its independent Entry is bounded.
    if (s.watchCapacityExceeded) return;
    if (found == s.watches.end())
    {
        if (s.watches.size() >= SimulationMaxWatchedPlacements) { s.watchCapacityExceeded = true; return; }
        found = s.watches.try_emplace(index).first; s.watchCleanupQueue.push_back(index);
    }
    const auto& p = _modernObjectPlacements[index];
    auto& watch = found->second;
    watch = State::Watch{}; watch.object = object; watch.shape = object->GetShape();
    watch.model = p.modelIndex; watch.id = p.id; watch.type = object->GetType(); watch.radius = object->GetRadius();
    auto* shape = object->GetShape();
    if (!shape || !object->Static() || (object->GetType() != Primary && object->GetType() != Network)) return;
    const auto route = SimulationRoute(static_cast<const char*>(shape->GetPropertyClass()));
    if (route == SimulationFactoryRoute::Unsupported) return;
    const Matrix4 canonical = RepairWorldObjectPlacementFrame(SimulationPlacementFrame(p.rows), false,
        object->GetType() == Network, (shape->GetOrHints() & ClipLandKeep) != 0);
    watch.frame = SimulationFrameSample(canonical);
    watch.authored = ValidSimulationFrame(canonical) && SimulationFrameSample(object->Transform()) == watch.frame &&
        object->ID() == p.id && std::isfinite(watch.radius) && watch.radius == canonical.Scale() * shape->BoundingSphere();
}
bool Landscape::HasModernSimulationLease(uint32_t index) const
{
    return _modernSimulation && _modernSimulation->HasLease(index);
}
void Landscape::PreserveModernSimulationModelRequests(std::vector<uint32_t>& epochs, uint32_t epoch) const
{
    if (!_modernSimulation || epochs.size() != _modernSimulation->groups.size()) return;
    for (size_t i = 0; i < epochs.size(); ++i)
    {
        const auto& g = _modernSimulation->groups[i];
        if (g.positiveCold)
        {
            if (!g.requestOutstanding || !_modernObjectPreparer) continue;
            const auto current = _modernObjectPreparer->QueryStaticSourceEnvelope(uint32_t(i));
            const auto& owned = g.positiveCold->source;
            if (current.generation == owned.generation && current.diagnosticParseToken == owned.diagnosticParseToken &&
                current.modelIdentity == owned.modelIdentity) epochs[i] = epoch;
        }
        else if (g.requestOutstanding && g.status == State::ModelState::Waiting) epochs[i] = epoch;
    }
}
Status Landscape::QueueModernSimulationRegion(Object& actor, Vector3Par from, Vector3Par to, double radius, bool automatic)
{ return QueueModernSimulationRegionForChannel(actor, from, to, radius, automatic, 0); }

Status Landscape::QueueModernSimulationQuery(Object& actor, SimulationQueryPurpose purpose,
    Vector3Par from, Vector3Par to, double radius)
{
    if (!ModernSimulationResidencyEnabled()) return Status::Disabled;
    if (!Foundation::IsMainThread()) return Status::WrongOwner;
    if (purpose != SimulationQueryPurpose::Fire) return Status::Invalid;
    return QueueModernSimulationRegionForChannel(actor, from, to, radius, false, uint8_t(purpose));
}

Status Landscape::QueueModernSimulationRegionForChannel(Object& actor, Vector3Par from, Vector3Par to,
    double radius, bool automatic, uint8_t channel)
{
    if (!ModernSimulationResidencyEnabled()) return Status::Disabled;
    if (!Foundation::IsMainThread()) return Status::WrongOwner;
    const auto a = Point(from), b = Point(to);
    if (!ValidRegion(a, b, radius) || !ValidSimulationFrame(actor.Transform()) || !std::isfinite(actor.GetRadius())) return Status::Invalid;
    EnsureModernSimulationResidency();
    auto& s = *_modernSimulation;
    if (s.stats.status == Status::CapacityExceeded || (channel != 0 && s.watchCapacityExceeded)) return Status::CapacityExceeded;
    auto found = s.regions.find(State::RegionKey{&actor, channel});
    if (found == s.regions.end())
    {
        if (s.regions.size() >= SimulationMaxRegions) { ++s.stats.queueRefused; return Status::CapacityExceeded; }
        found = s.regions.try_emplace(State::RegionKey{&actor, channel}).first;
        found->second.actor = &actor;
    }
    auto& r = found->second;
    // A stale allocator address must not inherit the prior actor's leases.
    if (static_cast<Object*>(r.actor) != &actor) { s.DropRegion(r); r = State::Region{}; r.actor = &actor; }
    if (automatic && !r.automatic && r.seen != 0) return r.status;
    r.seen = s.tick + 1;
    if (automatic) r.automaticSeenVisit = s.automaticActorVisits;
    if (channel == uint8_t(SimulationQueryPurpose::Fire))
        r.fireHeartbeat.Renew(std::chrono::steady_clock::now());
    const auto frame = SimulationFrameSample(actor.Transform());
    const auto currentSpeed = actor.ObjectSpeed();
    if (r.from == a && r.to == b && r.radius == radius)
    {
        r.actorId = actor.ID(); r.actorFrame = frame;
        r.actorSpeed = {currentSpeed.X(), currentSpeed.Y(), currentSpeed.Z()}; r.actorRadius = actor.GetRadius();
        r.automatic = automatic;
        return r.status;
    }
    s.DropPending(r);
    r.from = a; r.to = b; r.radius = radius; r.automatic = automatic;
    r.actorId = actor.ID(); r.actorFrame = SimulationFrameSample(actor.Transform());
    const auto speed = actor.ObjectSpeed(); r.actorSpeed = {speed.X(), speed.Y(), speed.Z()}; r.actorRadius = actor.GetRadius();
    r.status = Status::Pending;
    return r.status;
}
void Landscape::ReleaseModernSimulationRegion(Object& actor)
{ ReleaseModernSimulationRegionForChannel(actor, 0); }
void Landscape::ReleaseModernSimulationQuery(Object& actor, SimulationQueryPurpose purpose)
{
    if (purpose == SimulationQueryPurpose::Fire) ReleaseModernSimulationRegionForChannel(actor, uint8_t(purpose));
}
void Landscape::ReleaseModernSimulationRegionForChannel(Object& actor, uint8_t channel)
{
    if (!Foundation::IsMainThread() || !_modernSimulation) return;
    auto& s = *_modernSimulation;
    auto it = s.regions.find(State::RegionKey{&actor, channel});
    if (it != s.regions.end()) { s.DropRegion(it->second); s.regions.erase(it); }
}
Status Landscape::ModernSimulationRegionStatus(const Object& actor) const
{ return ModernSimulationRegionStatusForChannel(actor, 0); }

Status Landscape::ModernSimulationQueryStatus(const Object& actor, SimulationQueryPurpose purpose,
    Vector3Par from, Vector3Par to, double radius) const
{
    if (!ModernSimulationResidencyEnabled()) return Status::Disabled;
    if (!Foundation::IsMainThread()) return Status::WrongOwner;
    if (purpose != SimulationQueryPurpose::Fire || !ValidRegion(Point(from), Point(to), radius)) return Status::Invalid;
    const auto status = ModernSimulationRegionStatusForChannel(actor, uint8_t(purpose));
    if (status != Status::Ready) return status;
    const auto found = _modernSimulation->regions.find(State::RegionKey{&actor, uint8_t(purpose)});
    const auto& r = found->second;
    return SimulationCapsuleContains(r.from, r.to, r.radius, Point(from), Point(to), radius) ? Status::Ready : Status::Pending;
}

Status Landscape::TryModernSimulationFireCollision(CollisionBuffer& retVal, const Object& actor,
    Object* with, Object* ignore, Vector3Par from, Vector3Par to, float radius) const
{
    if (!ModernSimulationResidencyEnabled()) return Status::Disabled;
    if (!Foundation::IsMainThread()) return Status::WrongOwner;
    if (!_modernSimulation)
    {
        if (!ValidRegion(Point(from), Point(to), radius)) return Status::Invalid;
        const auto broadphase = SimulationFireBroadphaseStatus(from, to, radius, _invLandGrid, _landRange);
        return broadphase == Status::Ready ? Status::Pending : broadphase;
    }
    auto& s = *_modernSimulation;
    if (!s.fireCollisionConsumption.TryBegin())
    { ++s.stats.fireCollisionWorkRefusals; return Status::Pending; }
    SimulationFireCollisionCharge charge(s.fireCollisionConsumption,
        []() noexcept { return std::chrono::steady_clock::now(); });
    if (!ValidRegion(Point(from), Point(to), radius)) return Status::Invalid;
    const auto broadphase = SimulationFireBroadphaseStatus(from, to, radius, _invLandGrid, _landRange);
    if (broadphase != Status::Ready) return broadphase;
    const double allowance = SimulationFireCollisionConsumption::MaxMilliseconds -
        s.fireCollisionConsumption.ChargedMilliseconds();
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
    const auto validate = [&] { return ModernSimulationQueryStatus(actor, SimulationQueryPurpose::Fire, from, to, radius); };
    const auto finish = [&](Status status) {
        ++s.stats.fireCollisionCalls;
        s.stats.lastFireCollisionMs = elapsed();
        return status;
    };
    const auto status = validate();
    if (status != Status::Ready) return finish(status);
    const auto found = s.regions.find(State::RegionKey{&actor, uint8_t(SimulationQueryPurpose::Fire)});
    if (found == s.regions.end()) return finish(Status::Pending);
    if (found->second.active.size() > SimulationMaxRegionLeases) return finish(Status::CapacityExceeded);
    std::vector<Object*> candidates;
    candidates.reserve(found->second.active.size());
    for (auto index : found->second.active)
    {
        const auto entry = s.entries.find(index);
        if (entry == s.entries.end() || !HasModernSimulationLease(index)) return finish(Status::Unknown);
        Object* object = static_cast<Object*>(entry->second.object);
        if (!object) return finish(Status::Unknown);
        candidates.push_back(object);
    }
    bool workRefused = false;
    const auto budget = [&] {
        const bool allowed = elapsed() < allowance;
        workRefused |= !allowed;
        return allowed;
    };
    // Candidate collection performs only owner reads/allocation, so retain the
    // just-completed initial validation instead of rescanning 16k watchers twice
    // before any geometry work. The post-trace validation is always fresh.
    bool useInitialValidation = true;
    const auto revalidate = [&] {
        if (useInitialValidation) { useInitialValidation = false; return status; }
        return validate();
    };
    const auto result = AppendSimulationFireContacts(retVal, std::span<Object* const>(candidates.data(), candidates.size()), with, ignore,
        [&](CollisionBuffer& staged) { ObjectCollision(staged, with, ignore, from, to, radius, ObjIntersectFire); },
        [&](CollisionBuffer& staged, Object& object) {
            ++s.stats.fireCollisionCandidateTests;
            object.Intersect(staged, from, to, radius, ObjIntersectFire);
        }, revalidate, budget);
    if (workRefused) ++s.stats.fireCollisionWorkRefusals;
    return finish(result);
}

Status Landscape::ModernSimulationRegionStatusForChannel(const Object& actor, uint8_t channel) const
{
    if (!ModernSimulationResidencyEnabled()) return Status::Disabled;
    if (!Foundation::IsMainThread()) return Status::WrongOwner;
    if (!_modernSimulation) return Status::Pending;
    const auto& s = *_modernSimulation;
    const auto found = s.regions.find(State::RegionKey{&actor, channel});
    if (channel == uint8_t(SimulationQueryPurpose::Fire) && found != s.regions.end() &&
        found->second.FireInterestExpired(channel, std::chrono::steady_clock::now()))
        return Status::Pending; // Expiry is effective even before bounded owner retirement.
    const auto metadata = ObservedStatus(s, _modernObjectModels);
    if (metadata != Status::Ready) return metadata;
    if (found == s.regions.end()) return Status::Pending;
    const auto status = RegionRecordStatus(s, found->second, actor);
    // Complete geometry/watch validation can cross the heartbeat deadline.
    return channel == uint8_t(SimulationQueryPurpose::Fire) && status == Status::Ready ?
        found->second.FireStatusAtPublication(channel, status, std::chrono::steady_clock::now()) : status;
}
SimulationResidencyStats Landscape::SnapshotModernSimulationResidency() const
{
    SimulationResidencyStats out;
    if (!ModernSimulationResidencyEnabled()) return out;
    if (!Foundation::IsMainThread()) { out.status = Status::WrongOwner; return out; }
    if (!_modernSimulation) { out.status = Status::Pending; return out; }
    const auto& s = *_modernSimulation;
    out = s.stats; out.status = ObservedStatus(s, _modernObjectModels); out.regions = out.queryRegions = 0; out.coverageGeneration = s.generation;
    out.metadataComplete = s.inventoryComplete; out.referencedModels = s.referencedModels;
    out.registeredBorrowedModels = s.registeredBorrowedModels;
    out.registeredBorrowShapeCharge = s.registeredBorrowShapeCharge;
    // Historical observation, never a substitute for out.status/current proof.
    out.registeredMetadataEverComplete = s.registeredMetadataEverComplete;
    out.registeredValidationBudgetRefusals = s.registeredValidationBudgetRefusals;
    out.activeModels = 0; out.sparseLeaseRecords = s.leaseCounts.size();
    out.positiveColdWorkingModels = s.positiveColdWork.size();
    out.preparedModels = out.unknownModels = out.leasedPlacements = 0;
    out.watchedPlacements = s.watches.size(); out.liveWatchedPlacements = 0;
    for (const auto& [index, watch] : s.watches) if (static_cast<Object*>(watch.object)) ++out.liveWatchedPlacements;
    for (const auto& g : s.groups)
    {
        if (g.shape) ++out.activeModels;
        if (g.queryBorrow) ++out.queryBorrowedModels;
        out.queryBorrowedEntries += g.queryEntries;
        if (g.memberCount && g.status == State::ModelState::Ready) ++out.preparedModels;
        if (g.status == State::ModelState::Unknown || !g.validRows) ++out.unknownModels;
    }
    for (const auto& [i, entry] : s.entries) if (HasModernSimulationLease(i))
    {
        ++out.leasedPlacements;
        if (entry.positiveRevision) ++out.positiveLeasedPlacements;
    }
    const auto queryNow = std::chrono::steady_clock::now();
    for (const auto& [key, r] : s.regions)
    {
        Object* actor = static_cast<Object*>(r.actor);
        auto status = r.FireInterestExpired(key.channel, queryNow) ? Status::Pending : !actor ? Status::Unknown :
            (out.status == Status::Ready ? RegionRecordStatus(s, r, *actor) : out.status);
        if (key.channel == uint8_t(SimulationQueryPurpose::Fire) && status == Status::Ready)
            status = r.FireStatusAtPublication(key.channel, status, std::chrono::steady_clock::now());
        if (key.channel == 0)
        {
            ++out.regions;
            if (r.positiveOnly)
            {
                if (r.positiveScanComplete && r.positiveAdmissionGeneration == s.positiveAdmissionGeneration)
                    ++out.positiveScannedRegions;
            }
            if (status == Status::Ready) ++out.readyRegions;
            else if (status == Status::Pending) ++out.pendingRegions;
            else if (status == Status::Unknown) ++out.unknownRegions;
            else ++out.refusedRegions;
        }
        else
        {
            ++out.queryRegions;
            if (status == Status::Ready) ++out.readyQueries;
            else if (status == Status::Pending) ++out.pendingQueries;
            else if (status == Status::Unknown) ++out.unknownQueries;
            else ++out.refusedQueries;
        }
    }
    return out;
}

void Landscape::PumpModernSimulationResidency(const SimulationResidencyOwnerBoundary&)
{
    // Explicit diagnostic demand is independent of simulation eligibility/caps.
    // Ordinary calls return immediately without scanning or allocating anything.
    MaintainModernSourceDiagnostics();
    if (!ModernSimulationResidencyEnabled() || !Foundation::IsMainThread()) return;
    EnsureModernSimulationResidency();
    auto& s = *_modernSimulation;
    // Joined-owner boundary, including metadata-capacity refusal: consumers
    // never reset this allowance through Queue/status/contact polling.
    s.fireCollisionConsumption.Reset();
    if (s.stats.status == Status::CapacityExceeded) return;
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(); };
    ++s.tick; ++s.stats.ownerPumps; s.stats.lastPumpVisits = 0;
    // This owner boundary is after FinishSecondary, so weak actors and physical
    // object grids are never read/mutated concurrently with background AI.
    const size_t totalActors = _world ? size_t(_world->NVehicles() + _world->NFastVehicles()) : 0;
    for (size_t count = 0; count < std::min(size_t(32), totalActors) && elapsed() < 2; ++count)
    {
        const size_t index = s.actorCursor++ % totalActors;
        s.automaticActorVisits = AdvanceSimulationActorVisit(s.automaticActorVisits);
        Object* actor = index < size_t(_world->NVehicles()) ? _world->GetVehicle(int(index)) :
            _world->GetFastVehicle(int(index - _world->NVehicles()));
        if (!actor) continue;
        const auto quantized = [](Vector3Par p)
        { return Vector3(float(std::floor(p.X() / 10.0) * 10 + 5), float(std::floor(p.Y() / 10.0) * 10 + 5),
                         float(std::floor(p.Z() / 10.0) * 10 + 5)); };
        QueueModernSimulationRegion(*actor, quantized(actor->Position()),
                                    quantized(actor->Position() + actor->ObjectSpeed() * 0.1f),
                                    std::max(75.0, double(actor->GetRadius()) + 25), true);
    }
    size_t expired = 0;
    for (auto it = s.regions.begin(); it != s.regions.end();)
    {
        if ((it->second.FireInterestExpired(it->first.channel, started) ||
             SimulationAutomaticRegionExpired(it->second.automatic, static_cast<Object*>(it->second.actor) != nullptr,
                totalActors, s.automaticActorVisits, it->second.automaticSeenVisit)) &&
            expired < 4 && elapsed() < 2)
        { s.DropRegion(it->second); it = s.regions.erase(it); ++expired; }
        else ++it;
    }
    for (size_t visits = 0; !s.inventoryComplete && visits < 2048 && elapsed() < 2; ++visits)
    {
        if (s.inventoryCursor == _modernObjectPlacements.size())
        {
            // This groundwork does not eagerly expand CPU shapes to cover a
            // production-world model inventory. Finish metadata, then refuse
            // before ANY simulation Request/Take/ShapeBank work when the full
            // referenced active set cannot fit the unchanged256-model bound.
            s.FinishInventory();
            break;
        }
        const uint32_t i = uint32_t(s.inventoryCursor++);
        const auto& p = _modernObjectPlacements[i];
        ++s.stats.lastPumpVisits; s.stats.inventoryRows = s.inventoryCursor;
        if (p.id < 0 || p.modelIndex >= s.groups.size()) { s.inventoryInvalid = true; continue; }
        auto& g = s.groups[p.modelIndex];
        const auto scale = SimulationPlacementMaximumScale(p.rows);
        const CoveragePoint origin = {p.rows[9], p.rows[10], p.rows[11]};
        const double extent = _landRange * double(_landGrid);
        if (!scale || origin[0] < 0 || origin[2] < 0 || origin[0] >= extent || origin[2] >= extent)
            g.validRows = false;
        else
        {
            g.maximumScale = std::max(g.maximumScale, *scale);
            if (!g.memberCount) g.origins = {origin, origin};
            else for (size_t axis = 0; axis < 3; ++axis)
            { g.origins.min[axis] = std::min(g.origins.min[axis], origin[axis]); g.origins.max[axis] = std::max(g.origins.max[axis], origin[axis]); }
        }
        if (!g.memberCount++) ++s.referencedModels;
        ObserveModernSimulationPlacement(i, p.resident);
    }
    // At most ONE bank-table/handoff tail per pump. No synchronous fallback:
    // packed/failed/unsupported models are explicit Unknown, never omitted.
    bool ownerModelHandoffUsed = false;
    if (s.inventoryComplete && !s.inventoryInvalid && !s.activeModelCapacityExceeded && _modernObjectPreparer && elapsed() < 2 && !s.groups.empty())
    {
        for (size_t scan = 0; scan < s.groups.size(); ++scan)
        {
            const uint32_t i = uint32_t(s.modelCursor++ % s.groups.size());
            auto& g = s.groups[i];
            if (g.positiveCold) continue; // Positive jobs never enter the full-coverage producer.
            if (g.status != State::ModelState::Waiting) continue;
            if (!g.validRows) { g.status = State::ModelState::Unknown; continue; }
            const auto state = _modernObjectPreparer->Query(i);
            if (state == ObjectStreamPreparer::State::NotLoose || state == ObjectStreamPreparer::State::Failed)
            { g.status = State::ModelState::Unknown; continue; }
            if (state == ObjectStreamPreparer::State::Unknown)
            { if (_modernObjectPreparer->Request(i)) g.requestOutstanding = true; continue; }
            if (state != ObjectStreamPreparer::State::Ready && state != ObjectStreamPreparer::State::Converted) continue;
            std::shared_ptr<Model::Model> model;
            LODShapeWithShadow* converted = nullptr;
            if (state == ObjectStreamPreparer::State::Converted)
            { auto result = _modernObjectPreparer->TakeConverted(i); converted = result.shape; model = std::move(result.model); }
            else model = _modernObjectPreparer->Take(i);
            if (!model) { delete converted; break; }
            ownerModelHandoffUsed = true;
            // The simulation now owns this handoff even when the original parse
            // was requested by the camera. Preserve only actual owned work.
            g.requestOutstanding = true;
            const uint64_t bytes = Model::ResidentPayloadBytes(*model);
            if (bytes > SimulationMaxModelPayload)
            { delete converted; g.status = State::ModelState::CapacityExceeded; break; }
            const uint64_t charge = bytes * 4 + 1024 * 1024; // multiplication safe after the 4 MiB cap
            if (!SimulationSourceAdmissible(*model)) { delete converted; g.status = State::ModelState::Unknown; break; }
            if (!g.charged)
            {
                if (bytes > SimulationMaxModelPayload || bytes > SimulationMaxPayloadCharge - s.stats.payloadCapacityCharge ||
                    !s.CanScheduleShape(charge))
                { delete converted; g.status = State::ModelState::CapacityExceeded; break; }
                g.charged = true; g.payloadCharge = bytes; g.shapeCharge = charge;
                s.stats.payloadCapacityCharge += bytes; s.stats.shapeSchedulingCharge += charge;
            }
            g.sourceValidated = true;
            try
            {
                ShapeBank::CpuOnlyLoadScope cpuOnly;
                ShapeBank::WorldModelScope worldModel(Shapes);
                if (!converted && !Shapes.Find(_modernObjectModels[i], false, true))
                {
                    std::string lower(static_cast<const char*>(_modernObjectModels[i]));
                    for (char& c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                    model->sourcePath = lower;
                    auto tables = std::make_shared<Model::ShapeAdapter::AdapterBankTables>();
                    Model::ShapeAdapter::BuildAdapterBankTables(*model, *tables);
                    const auto token = _modernObjectPreparer->QueryRadius(i);
                    _modernObjectPreparer->SubmitConvert(i, std::move(model), std::move(tables), &token);
                    break;
                }
                // NewFromModel owns the converted raw pointer on entry, even a cache hit.
                auto* transferred = converted; converted = nullptr;
                const bool hadShape = bool(g.shape);
                g.shape = Shapes.NewFromModel(_modernObjectModels[i], false, true, std::move(model), nullptr, transferred);
                if (!hadShape && g.shape) ++s.legacyShapeSlots;
                if (!g.shape || !SimulationShapeAdmissible(*g.shape)) g.status = State::ModelState::Unknown;
                else
                {
                    g.radius = g.shape->BoundingSphere(); g.revision = g.shape->QueryPolicyRevision();
                    g.route = SimulationRoute(static_cast<const char*>(g.shape->GetPropertyClass()));
                    g.status = State::ModelState::Ready;
                    s.stats.shapeElementBytes += SimulationShapeElementBytes(*g.shape);
                    const auto box = BuildEngineBroadphaseEnvelope(g.origins.max, g.maximumScale, g.radius);
                    if (!box) g.status = State::ModelState::Unknown;
                    else
                    {
                        const auto lowerBox = BuildEngineBroadphaseEnvelope(g.origins.min, g.maximumScale, g.radius);
                        if (!lowerBox) g.status = State::ModelState::Unknown;
                        else for (size_t axis = 0; axis < 3; ++axis)
                            s.placementPadding = std::max({s.placementPadding,
                                box->max[axis] - g.origins.max[axis], g.origins.min[axis] - lowerBox->min[axis]});
                    }
                }
            }
            catch (...) { delete converted; g.status = State::ModelState::Unknown; }
            break;
        }
    }
    // Reconcile a bounded set of owner-observed policy changes, including
    // same-valued changes. This is the scoped query-policy contract, not a
    // certificate for untracked mutable per-LOD vertex APIs.
    bool revised = false;
    // Refused active inventories have no admitted group shapes to reconcile.
    if (!s.activeModelCapacityExceeded)
    for (auto& g : s.groups)
    {
        if (elapsed() >= 2) break;
        if (g.positiveCold || g.status != State::ModelState::Ready || !g.shape || g.shape->QueryPolicyRevision() == g.revision) continue;
        if (!SimulationShapeAdmissible(*g.shape)) g.status = State::ModelState::Unknown;
        else
        {
            g.radius = g.shape->BoundingSphere(); g.revision = g.shape->QueryPolicyRevision();
            g.route = SimulationRoute(static_cast<const char*>(g.shape->GetPropertyClass()));
        }
        revised = true;
    }
    if (revised)
    {
        if (s.generation == std::numeric_limits<uint64_t>::max()) s.inventoryInvalid = true;
        else ++s.generation;
        s.placementPadding = 0;
        for (auto& g : s.groups)
        {
            if (!g.memberCount || g.status != State::ModelState::Ready) continue;
            const auto upper = BuildEngineBroadphaseEnvelope(g.origins.max, g.maximumScale, g.radius);
            const auto lower = BuildEngineBroadphaseEnvelope(g.origins.min, g.maximumScale, g.radius);
            if (!upper || !lower) { g.status = State::ModelState::Unknown; continue; }
            for (size_t axis = 0; axis < 3; ++axis)
                s.placementPadding = std::max({s.placementPadding,
                    upper->max[axis] - g.origins.max[axis], g.origins.min[axis] - lower->min[axis]});
        }
    }
    // Weak owner observations only; an overflowing watch registry already
    // blocks complete queries, so leave production positive warming unchanged.
    if (s.activeModelCapacityExceeded && s.inventoryComplete && !s.inventoryInvalid && !s.watchCapacityExceeded && elapsed() < 2)
    {
        bool changed = false;
        for (size_t n = 0; n < std::min(size_t(32), s.groups.size()) && elapsed() < 2; ++n)
        {
            const uint32_t i = uint32_t(s.registeredCursor++ % s.groups.size());
            if (!s.groups[i].memberCount) continue;
            s.RefreshRegisteredModel(i, Shapes, _modernObjectModels[i], Pars, changed, [&] { return elapsed() < 2; });
        }
        // Staged padding is never a prefix exclusion/completeness certificate.
        if (s.registeredMetadataDirty)
            s.AdvanceRegisteredPadding(Shapes, _modernObjectModels, Pars, [&] { return elapsed() < 2; });
    }
    const bool coldEnabled = PositiveColdEnabled();
    const auto enlistCold = [&](uint32_t modelIndex, uint32_t placementIndex)
    {
        auto& g = s.groups[modelIndex];
        if (!coldEnabled || !g.CanEnlistPositiveCold() || !_modernObjectPreparer ||
            s.legacyShapeSlots + s.positiveColdWork.size() + s.registeredBorrowedModels >= SimulationMaxModels) return;
        const char* path = _modernObjectModels[modelIndex];
        size_t pathBytes = 0;
        while (pathBytes < 128 && path[pathBytes]) ++pathBytes;
        if (!pathBytes || pathBytes >= 128) { g.positiveColdRefused = true; ++s.stats.positiveColdRefused; return; }
        const uint64_t maximumFacts = sizeof(State::PositiveColdFacts) + 128 + SimulationMaxModels * sizeof(uint32_t);
        if (s.stats.metadataRecordBytes > SimulationMaxMetadataRecordBytes ||
            s.stats.positiveColdFactsBytes > SimulationMaxMetadataRecordBytes - s.stats.metadataRecordBytes ||
            maximumFacts > SimulationMaxMetadataRecordBytes - s.stats.metadataRecordBytes - s.stats.positiveColdFactsBytes)
        { ++s.stats.positiveColdRefused; return; }
        if (_modernObjectPreparer->Query(modelIndex) != ObjectStreamPreparer::State::Unknown ||
            Shapes.Find(_modernObjectModels[modelIndex], false, true)) return; // Never steal camera work/cache aliases.
        try
        {
            auto facts = std::make_unique<State::PositiveColdFacts>();
            s.positiveColdWork.reserve(SimulationMaxModels);
            const auto result = _modernObjectPreparer->RequestWithStaticSourceEnvelope(modelIndex);
            if (result != ObjectStreamPreparer::SourceEnvelopeRequest::Requested)
            {
                if (s.positiveColdWork.empty()) std::vector<uint32_t>().swap(s.positiveColdWork);
                return;
            }
            facts->source = _modernObjectPreparer->QueryStaticSourceEnvelope(modelIndex);
            facts->selectedPlacement = placementIndex;
            facts->factCharge = sizeof(State::PositiveColdFacts) + facts->source.modelIdentity.capacity() + 1;
            if (!facts->source.generation || !facts->source.diagnosticParseToken ||
                facts->source.modelIdentity != static_cast<const char*>(_modernObjectModels[modelIndex]) ||
                facts->source.modelIdentity.size() >= 128 ||
                facts->factCharge + s.positiveColdWork.capacity() * sizeof(uint32_t) >
                    SimulationMaxMetadataRecordBytes - s.stats.metadataRecordBytes - s.stats.positiveColdFactsBytes)
            { ++s.stats.positiveColdRefused; facts->retiring = true; } // Drain any successfully enqueued job.
            s.stats.positiveColdFactsBytes += facts->factCharge;
            g.positiveCold = std::move(facts); g.requestOutstanding = !g.positiveCold->retiring;
            s.positiveColdWork.push_back(modelIndex); ++s.stats.positiveColdRequested;
        }
        catch (...)
        {
            ++s.stats.positiveColdRefused;
            if (s.positiveColdWork.empty()) std::vector<uint32_t>().swap(s.positiveColdWork);
        }
    };
    // One selected job per pump, never a whole-model-table sweep. A lost actor
    // withdraws intent; already-running work is drained without bank/adaptation.
    if (coldEnabled && !ownerModelHandoffUsed && !s.positiveColdWork.empty() && elapsed() < 2)
    {
        const uint32_t index = s.positiveColdWork[s.positiveColdCursor++ % s.positiveColdWork.size()];
        auto& g = s.groups[index]; auto& facts = *g.positiveCold;
        const auto& selected = _modernObjectPlacements[facts.selectedPlacement];
        bool desired = false;
        for (const auto& [key, region] : s.regions)
        {
            if (key.channel != 0 || !static_cast<Object*>(region.actor)) continue;
            const double pad = region.radius + _landGrid;
            if (selected.rows[9] >= std::min(region.from[0], region.to[0]) - pad &&
                selected.rows[9] <= std::max(region.from[0], region.to[0]) + pad &&
                selected.rows[11] >= std::min(region.from[2], region.to[2]) - pad &&
                selected.rows[11] <= std::max(region.from[2], region.to[2]) + pad) { desired = true; break; }
        }
        if (!desired && !facts.residentEntries) { facts.retiring = true; g.requestOutstanding = false; }
        const auto state = _modernObjectPreparer->Query(index);
        const auto current = _modernObjectPreparer->QueryStaticSourceEnvelope(index);
        const bool same = current.generation == facts.source.generation &&
            current.diagnosticParseToken == facts.source.diagnosticParseToken && current.modelIdentity == facts.source.modelIdentity;
        const auto retire = [&](bool refused, bool interrupted)
        {
            if (refused) ++s.stats.positiveColdRefused;
            if (interrupted) ++s.stats.positiveColdInterrupted;
            s.RetirePositiveCold(index, refused);
        };
        if (facts.stage == State::PositiveColdStage::Held)
        {
            if (current.generation != facts.source.generation || current.modelIdentity != facts.source.modelIdentity)
                facts.retiring = true;
            if (facts.retiring && !facts.residentEntries) retire(false, false);
        }
        else if (!same || state == ObjectStreamPreparer::State::Failed || state == ObjectStreamPreparer::State::NotLoose)
            retire(state == ObjectStreamPreparer::State::Failed || state == ObjectStreamPreparer::State::NotLoose, !same);
        else if (facts.stage == State::PositiveColdStage::Parse && state == ObjectStreamPreparer::State::Ready)
        {
            // Pair current source facts and THIS Take in one owner operation.
            auto model = _modernObjectPreparer->Take(index);
            if (!model) retire(false, true);
            else if (facts.retiring) retire(false, false);
            else
            {
                const uint64_t bytes = Model::ResidentPayloadBytes(*model);
                const uint64_t charge = bytes <= SimulationMaxModelPayload ? bytes * 4 + 1024 * 1024 : 0;
                // Paired immutable SourceEvidence already validates all source
                // vertices/selections on the worker. Keep the stricter adapter
                // LOD cap here without repeating the full scan on the owner.
                if (!current.attempted || bytes > SimulationMaxModelPayload || model->lodLevels.size() > MAX_LOD_LEVELS ||
                    current.envelope.State() != StaticSourceEnvelopeState::SourceEvidence ||
                    Streaming::ProbeStaticPlainRouteNow(current.plainSource, current.generation, current.modelIdentity, Pars) !=
                        StaticPlainRouteObservation::EmptyClassNow ||
                    bytes > SimulationMaxPayloadCharge - s.stats.payloadCapacityCharge ||
                    !s.CanScheduleShape(charge))
                    retire(true, false);
                else
                {
                    facts.source = current;
                    g.charged = true; g.payloadCharge = bytes; g.shapeCharge = charge;
                    s.stats.payloadCapacityCharge += bytes; s.stats.shapeSchedulingCharge += charge;
                    const auto bankStart = std::chrono::steady_clock::now();
                    bool bankRecorded = false;
                    const auto recordBank = [&]
                    {
                        if (bankRecorded) return;
                        bankRecorded = true;
                        s.stats.positiveColdLastBankMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bankStart).count();
                        s.stats.positiveColdMaxBankMs = std::max(s.stats.positiveColdMaxBankMs, s.stats.positiveColdLastBankMs);
                        ++s.stats.positiveColdBankCalls;
                    };
                    try
                    {
                        ShapeBank::CpuOnlyLoadScope cpuOnly;
                        model->sourcePath = facts.source.modelIdentity;
                        for (char& c : model->sourcePath) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                        auto tables = std::make_shared<Model::ShapeAdapter::AdapterBankTables>();
                        Model::ShapeAdapter::BuildAdapterBankTables(*model, *tables);
                        recordBank(); // Excludes queue/token submission and the later owner tail.
                        const auto radius = _modernObjectPreparer->QueryRadius(index);
                        if (_modernObjectPreparer->SubmitConvert(index, std::move(model), std::move(tables), &radius, &facts.source))
                            facts.stage = State::PositiveColdStage::Convert;
                        else retire(false, true);
                    }
                    catch (...) { recordBank(); retire(true, false); }
                }
            }
        }
        else if (facts.stage == State::PositiveColdStage::Convert && state == ObjectStreamPreparer::State::Converted)
        {
            auto converted = _modernObjectPreparer->TakeConverted(index);
            std::unique_ptr<LODShapeWithShadow> guarded(converted.shape);
            if (!converted.model || !guarded || converted.sourceToken.generation != facts.source.generation ||
                converted.sourceToken.diagnosticParseToken != facts.source.diagnosticParseToken)
                retire(false, true);
            else if (facts.retiring) retire(false, false);
            else if (Streaming::ProbeStaticPlainRouteNow(facts.source.plainSource, facts.source.generation,
                facts.source.modelIdentity, Pars) != StaticPlainRouteObservation::EmptyClassNow ||
                Shapes.Find(_modernObjectModels[index], false, true)) retire(true, false);
            else
            {
                const auto tailStart = std::chrono::steady_clock::now();
                try
                {
                    ShapeBank::CpuOnlyLoadScope cpuOnly;
                    ShapeBank::WorldModelScope worldModel(Shapes);
                    ShapeBank::LoadTiming timing;
                    g.shape = Shapes.NewFromModel(_modernObjectModels[index], false, true,
                        std::move(converted.model), &timing, guarded.release());
                    if (!g.shape || timing.cacheHit || !timing.preparsed || !timing.canonical ||
                        !PositiveColdShapeMatchesSource(*g.shape, facts.source.envelope, facts.source.generation)) retire(true, false);
                    else
                    {
                        facts.stage = State::PositiveColdStage::Held; facts.shapeRevision = g.shape->QueryPolicyRevision();
                        g.requestOutstanding = false; ++s.stats.positiveColdPrepared;
                        if (s.positiveAdmissionGeneration == std::numeric_limits<uint64_t>::max()) s.inventoryInvalid = true;
                        else ++s.positiveAdmissionGeneration;
                    }
                }
                catch (...) { retire(true, false); }
                s.stats.positiveColdLastTailMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tailStart).count();
                s.stats.positiveColdMaxTailMs = std::max(s.stats.positiveColdMaxTailMs, s.stats.positiveColdLastTailMs);
                ++s.stats.positiveColdTailCalls;
            }
        }
        else if (state == ObjectStreamPreparer::State::Unknown ||
            (facts.stage == State::PositiveColdStage::Parse && state != ObjectStreamPreparer::State::Queued && state != ObjectStreamPreparer::State::Parsing) ||
            (facts.stage == State::PositiveColdStage::Convert && state != ObjectStreamPreparer::State::Converting))
            retire(false, true);
    }
    const bool completeCoverage = ObservedStatus(s, _modernObjectModels, [&] { return elapsed() < 2; }) == Status::Ready;
    // Partial positive warming reuses actor channel0 and the SAME sparse leases.
    // Authored origin cells are a selection heuristic here, never a completeness
    // proof: cold/missing/unsupported and off-bin instances are not declared clear.
    if (!completeCoverage && s.inventoryComplete && !s.inventoryInvalid && elapsed() < 2)
    {
        size_t regionIndex = 0;
        const size_t first = s.regions.empty() ? 0 : s.regionCursor++ % s.regions.size();
        for (auto& [key, r] : s.regions)
        {
            if (regionIndex++ != first) continue;
            if (key.channel != 0) break; // Exact Fire remains behind the complete-coverage gate.
            Object* actor = static_cast<Object*>(r.actor);
            if (!actor || !ActorUnchanged(r, *actor)) { r.status = Status::Pending; break; }
            if (r.NeedsPositiveRestart(s.generation, s.positiveAdmissionGeneration))
            {
                s.DropPending(r); r.positiveOnly = true;
                r.positiveAdmissionGeneration = s.positiveAdmissionGeneration;
                r.status = Status::Pending;
            }
            if (r.positiveScanComplete)
            {
                // Amortized weak/policy validation of retained positives; this
                // never publishes readiness from a validated prefix.
                for (size_t n = 0; n < std::min(size_t(32), r.active.size()) && elapsed() < 2; ++n)
                {
                    const auto index = r.active[r.positiveValidationCursor++ % r.active.size()];
                    const auto found = s.entries.find(index);
                    Object* object = static_cast<Object*>(_modernObjectPlacements[index].resident);
                    if (found == s.entries.end() || !object || !found->second.MatchesPositiveResident(*object))
                    { s.DropPending(r); r.status = Status::Pending; break; }
                }
                if (r.positiveScanComplete) break;
            }
            if (r.status == Status::Unknown || r.status == Status::CapacityExceeded) break;
            if (r.planGeneration != s.generation)
            {
                r.plan = PlanSimulationDemand(r.from[0], r.from[2], r.to[0], r.to[2],
                    r.radius, _landGrid, uint32_t(_landRange), 4096);
                r.planGeneration = s.generation;
                if (r.plan.status != DemandStatus::Complete)
                { r.status = r.plan.status == DemandStatus::CapacityExceeded ? Status::CapacityExceeded : Status::Invalid; break; }
            }
            while (r.cell < r.plan.cells.size() && s.stats.lastPumpVisits < 2048 && elapsed() < 2)
            {
                const auto& cell = _modernObjectCells[r.plan.cells[r.cell]];
                if (r.offset == cell.size()) { ++r.cell; r.offset = 0; continue; }
                const uint32_t index = cell[r.offset++]; ++s.stats.lastPumpVisits;
                auto& p = _modernObjectPlacements[index];
                Object* object = static_cast<Object*>(p.resident);
                if (r.pendingSet.contains(index)) continue;
                auto& group = s.groups[p.modelIndex];
                if (!object && coldEnabled)
                {
                    auto* held = group.positiveCold.get();
                    if (!held) enlistCold(p.modelIndex, index);
                    else if (held->stage == State::PositiveColdStage::Held && !held->retiring && group.shape)
                    {
                        const auto inventory = _modernObjectPreparer->QueryStaticSourceEnvelope(p.modelIndex);
                        if (inventory.generation != held->source.generation || inventory.modelIdentity != held->source.modelIdentity ||
                            held->shapeRevision != group.shape->QueryPolicyRevision() ||
                            !PositiveColdShapeMatchesSource(*group.shape, held->source.envelope, held->source.generation) ||
                            Streaming::ProbeStaticPlainRouteNow(held->source.plainSource, held->source.generation,
                                held->source.modelIdentity, Pars) != StaticPlainRouteObservation::EmptyClassNow)
                            held->retiring = true;
                        else if (s.entries.size() < SimulationMaxLeasedPlacements &&
                            r.active.size() + r.pendingNew < SimulationMaxRegionLeases && s.CanAcquireLease(index))
                        {
                            const auto frame = RepairWorldObjectPlacementFrame(SimulationPlacementFrame(p.rows), false, false,
                                (group.shape->GetOrHints() & ClipLandKeep) != 0);
                            const auto candidate = BuildEngineBroadphaseEnvelope(Point(frame.Position()), frame.Scale(), group.shape->BoundingSphere());
                            if (!candidate || !Overlap(*candidate, r)) continue; // No speculative outside-region object.
                            try
                            {
                                object = ObjectCreateModernPlainFromShape(p.id, group.shape.GetRef(), SimulationPlacementFrame(p.rows));
                            }
                            catch (...) { object = nullptr; }
                            if (object)
                            {
                                p.resident = object; _modernLogicalOnlyObjects.insert(p.id);
                                ++s.stats.created; ++s.stats.positiveColdCreated;
                                // Even a refused final observation must retire the
                                // newly created logical object through joined cleanup.
                                if (!s.entries.contains(index))
                                {
                                    s.cleanupQueue.push_back(index);
                                    auto& created = s.entries[index]; created.object = object; created.shape = object->GetShape();
                                    created.frame = SimulationFrameSample(object->Transform()); created.radius = object->GetRadius();
                                    created.positiveColdModel = p.modelIndex; ++held->residentEntries;
                                }
                                ObserveModernSimulationPlacement(index, object);
                                if (_geoAccumEnabled) AccumulateGeographyForObject(object);
                                const size_t ci = size_t(std::floor(p.rows[11] * _invLandGrid)) * _landRange +
                                    size_t(std::floor(p.rows[9] * _invLandGrid));
                                if (_modernObjectCellDesired[ci])
                                { _modernObjectCellActive[ci] = 0; _modernObjectCellCursor[ci] = 0; _modernObjectResidencyPending = true; }
                            }
                        }
                    }
                }
                if (!object) continue;
                auto observed = State::ObservePositiveResident(*object, p.id, p.rows);
                if (!observed) continue;
                const auto box = BuildEngineBroadphaseEnvelope(Point(object->Position()), object->Scale(), object->GetShape()->BoundingSphere());
                if (!box || !Overlap(*box, r)) continue;
                if ((!r.activeSet.contains(index) && r.active.size() + r.pendingNew >= SimulationMaxRegionLeases) ||
                    r.pending.size() >= SimulationMaxRegionLeases ||
                    (!r.activeSet.contains(index) && !s.CanAcquireLease(index)) ||
                    (!s.entries.contains(index) && s.entries.size() >= SimulationMaxLeasedPlacements))
                { r.status = Status::CapacityExceeded; break; }
                uint32_t coldModel = std::numeric_limits<uint32_t>::max();
                if (group.positiveCold && group.shape.GetRef() == object->GetShape()) coldModel = p.modelIndex;
                if (!s.entries.contains(index))
                {
                    s.cleanupQueue.push_back(index);
                    if (coldModel != std::numeric_limits<uint32_t>::max()) ++group.positiveCold->residentEntries;
                }
                else coldModel = s.entries[index].positiveColdModel;
                observed->positiveColdModel = coldModel;
                // Positive re-observation does not acquire query ownership, but
                // must preserve an existing full-producer entry's joined debt.
                if (const auto previous = s.entries.find(index); previous != s.entries.end())
                    observed->queryModel = previous->second.queryModel;
                s.entries[index] = std::move(*observed);
                if (!r.activeSet.contains(index))
                {
                    if (!s.AcquireLease(index)) { r.status = Status::CapacityExceeded; break; }
                    ++r.pendingNew;
                }
                r.pending.push_back(index); r.pendingSet.insert(index);
            }
            if (r.status == Status::Pending && r.cell == r.plan.cells.size())
                s.CompleteReplacement(r, false); // Lease replacement only: NEVER Ready.
            if (r.status != Status::Pending)
            { s.DropPending(r); r.planGeneration = s.generation; }
            break;
        }
    }
    if (completeCoverage && elapsed() < 2)
    {
        size_t regionIndex = 0;
        const size_t first = s.regions.empty() ? 0 : s.regionCursor++ % s.regions.size();
        for (auto& [actorKey, r] : s.regions)
        {
            if (regionIndex++ != first) continue;
            const auto interestExpired = [&] {
                return actorKey.channel == uint8_t(SimulationQueryPurpose::Fire) &&
                    r.FireInterestExpired(actorKey.channel, std::chrono::steady_clock::now());
            };
            if (interestExpired())
                break; // No admissions while retirement quota is exhausted. Keep the retained plan intact for a valid renewed interest.
            Object* actor = static_cast<Object*>(r.actor);
            if (!actor || !ActorUnchanged(r, *actor)) { r.status = Status::Pending; break; }
            if (r.positiveOnly)
            { s.DropPending(r); r.positiveOnly = false; r.status = Status::Pending; }
            if (r.planGeneration != s.generation) r.status = Status::Pending;
            if (r.status == Status::Ready)
            {
                const auto live = RegionRecordStatus(s, r, *actor);
                if (live != Status::Ready) r.status = live;
                else break;
            }
            if (r.status == Status::Unknown || r.status == Status::CapacityExceeded) break;
            if (r.planGeneration != s.generation)
            {
                s.DropPending(r);
                r.planGeneration = s.generation; // refused plans remain sticky in this generation
                r.plan = PlanSimulationDemand(r.from[0], r.from[2], r.to[0], r.to[2],
                    r.radius + s.placementPadding, _landGrid, uint32_t(_landRange), 4096);
                if (r.plan.status != DemandStatus::Complete)
                { r.status = r.plan.status == DemandStatus::CapacityExceeded ? Status::CapacityExceeded : Status::Invalid; break; }
                r.planGeneration = s.generation;
            }
            while (r.cell < r.plan.cells.size() && s.stats.lastPumpVisits < 2048 && elapsed() < 2 && !interestExpired())
            {
                const auto& cell = _modernObjectCells[r.plan.cells[r.cell]];
                if (r.offset == cell.size()) { ++r.cell; r.offset = 0; continue; }
                const uint32_t index = cell[r.offset++]; ++s.stats.lastPumpVisits;
                auto& p = _modernObjectPlacements[index]; auto& g = s.groups[p.modelIndex];
                const bool registered = s.activeModelCapacityExceeded;
                auto* modelShape = registered ? static_cast<LODShapeWithShadow*>(g.registeredShape.GetRef()) : g.shape.GetRef();
                const float modelRadius = registered ? g.registeredRadius : g.radius;
                if (!modelShape) { r.status = Status::Unknown; break; }
                const Matrix4 finalFrame = RepairWorldObjectPlacementFrame(SimulationPlacementFrame(p.rows), false,
                    !registered && g.route == SimulationFactoryRoute::Road, (modelShape->GetOrHints() & ClipLandKeep) != 0);
                const auto box = BuildEngineBroadphaseEnvelope(Point(finalFrame.Position()), finalFrame.Scale(), modelRadius);
                if (!box) { r.status = Status::Unknown; break; }
                if (!Overlap(*box, r) || r.pendingSet.contains(index)) continue;
                if ((!r.activeSet.contains(index) && r.active.size() + r.pendingNew >= SimulationMaxRegionLeases) ||
                    (r.pending.size() >= SimulationMaxRegionLeases) ||
                    (!r.activeSet.contains(index) && !s.CanAcquireLease(index)) ||
                    (!s.entries.contains(index) && s.entries.size() >= SimulationMaxLeasedPlacements))
                { r.status = Status::CapacityExceeded; break; }
                RegisteredBorrowRollback rollback;
                if (registered)
                {
                    // Re-probe immediately before reserving a strong model ref
                    // and before creating anything. No Shapes.New/load fallback.
                    LODShapeWithShadow* current = nullptr;
                    const auto proof = State::ProbeRegisteredPlainNow(Shapes, _modernObjectModels[p.modelIndex], Pars,
                        current, [&] { return elapsed() < 2; });
                    if (proof != Status::Ready || current != modelShape || !s.RegisteredShapeMatches(p.modelIndex, current))
                    { r.status = proof == Status::Pending ? Status::Pending : Status::Unknown; --r.offset; break; }
                    const auto held = s.HoldRegisteredQueryModel(p.modelIndex, current);
                    if (held != State::RegisteredBorrowResult::Held)
                    { r.status = held == State::RegisteredBorrowResult::CapacityExceeded ? Status::CapacityExceeded : Status::Unknown; break; }
                    rollback.state = &s; rollback.model = p.modelIndex;
                }
                if (interestExpired()) { --r.offset; break; }
                Object* object = static_cast<Object*>(p.resident);
                if (!object)
                {
                    try
                    {
                        ShapeBank::CpuOnlyLoadScope cpuOnly;
                        ShapeBank::WorldModelScope worldModel(Shapes);
                        object = registered ? ObjectCreateModernPlainFromShape(p.id, g.queryBorrow, SimulationPlacementFrame(p.rows)) :
                            ObjectCreate(p.id, _modernObjectModels[p.modelIndex], SimulationPlacementFrame(p.rows),
                                         nullptr, nullptr, false, true, false);
                    }
                    catch (...) { object = nullptr; }
                    if (object)
                    {
                        p.resident = object; _modernLogicalOnlyObjects.insert(p.id); ++s.stats.created;
                        ObserveModernSimulationPlacement(index, object);
                        if (_geoAccumEnabled) AccumulateGeographyForObject(object);
                        const size_t x = size_t(std::floor(p.rows[9] * _invLandGrid));
                        const size_t z = size_t(std::floor(p.rows[11] * _invLandGrid));
                        const size_t ci = z * _landRange + x;
                        if (_modernObjectCellDesired[ci])
                        { _modernObjectCellActive[ci] = 0; _modernObjectCellCursor[ci] = 0; _modernObjectResidencyPending = true; }
                    }
                }
                if (object && !s.entries.contains(index))
                {
                    s.cleanupQueue.push_back(index);
                    auto& cleanup = s.entries[index]; cleanup.object = object; cleanup.shape = object->GetShape();
                    cleanup.frame = SimulationFrameSample(object->Transform()); cleanup.radius = object->GetRadius();
                }
                // An indivisible constructor may cross the deadline. Its joined
                // cleanup record remains, but expired interest acquires no lease.
                if (interestExpired()) { --r.offset; break; }
                if (!object || object->GetShape() != modelShape || !object->Static() || object->MustBeSaved() ||
                    (object->GetType() != Primary && object->GetType() != Network) ||
                    SimulationFrameSample(object->Transform()) != SimulationFrameSample(finalFrame) ||
                    object->GetRadius() != finalFrame.Scale() * modelRadius || !SimulationShapeAdmissible(*modelShape))
                { r.status = Status::Unknown; break; }
                // Road constructors can normalize shape query policy in place. Accept
                // that known owner-tail change only after validating the exact result.
                if (!registered)
                    for (auto& group : s.groups) if (group.shape.GetRef() == modelShape) group.revision = modelShape->QueryPolicyRevision();
                auto& e = s.entries[index]; e.object = object; e.shape = object->GetShape();
                e.frame = SimulationFrameSample(object->Transform()); e.radius = object->GetRadius();
                e.positiveRevision = 0; e.positiveId = -1;
                if (!s.AttachQueryBorrow(p.modelIndex, e))
                { r.status = Status::Unknown; break; }
                // Shape validation/borrow attachment may cross the deadline too.
                // Keep any other owner's Entry borrow; joined cleanup handles an
                // unleased new entry, without assigning debt to expired interest.
                if (interestExpired()) { --r.offset; break; }
                if (!r.activeSet.contains(index))
                {
                    if (!s.AcquireLease(index)) { r.status = Status::CapacityExceeded; break; }
                    ++r.pendingNew;
                }
                r.pending.push_back(index); r.pendingSet.insert(index);
            }
            if (r.status == Status::Pending && r.cell == r.plan.cells.size() && !interestExpired())
            {
                // Owner constructors may mutate query state. The same operation
                // must validate the complete inventory and watchers again before
                // publishing Ready; a timed-out prefix leaves pending leases.
                const auto proof = ObservedStatus(s, _modernObjectModels, [&] { return elapsed() < 2; });
                if (proof == Status::Ready && !interestExpired()) s.CompleteReplacement(r);
                else if (interestExpired()) r.status = Status::Pending;
                else r.status = proof;
            }
            if (r.status != Status::Pending && r.status != Status::Ready)
            {
                s.DropPending(r);
                r.planGeneration = s.generation; // failed instance admission is sticky too
            }
            break;
        }
    }
    // Only this joined owner boundary retires logical instances. Changed regions
    // kept their old active leases until a complete replacement was admitted.
    size_t retired = 0;
    const size_t cleanupVisits = std::min(size_t(128), s.cleanupQueue.size());
    for (size_t visit = 0; visit < cleanupVisits && retired < 32 && elapsed() < 2; ++visit)
    {
        const uint32_t index = s.cleanupQueue.front(); s.cleanupQueue.pop_front();
        auto found = s.entries.find(index);
        if (found == s.entries.end()) continue;
        if (HasModernSimulationLease(index)) { s.cleanupQueue.push_back(index); continue; }
        auto& p = _modernObjectPlacements[index]; Object* object = static_cast<Object*>(p.resident);
        if (object && object == static_cast<Object*>(found->second.object) && _modernLogicalOnlyObjects.contains(p.id) &&
            !_modernRequiredObjects.contains(p.id) && !object->MustBeSaved())
        {
            Ref<Object> keepAlive = object; p.resident = nullptr; RemoveObject(keepAlive);
            _objectIds.erase(p.id); _modernLogicalOnlyObjects.erase(p.id); _modernObjectLightOwners.erase(p.id);
            ++s.stats.released; ++retired;
        }
        const auto coldModel = found->second.positiveColdModel;
        if (coldModel < s.groups.size() && s.groups[coldModel].positiveCold && s.groups[coldModel].positiveCold->residentEntries)
            --s.groups[coldModel].positiveCold->residentEntries;
        // Save ownership may keep the Object alive after its final lease. Its
        // query model borrow still ends at this owner cleanup boundary.
        s.ReleaseQueryBorrow(found->second);
        s.entries.erase(found);
    }
    const size_t watchVisits = std::min(size_t(128), s.watchCleanupQueue.size());
    for (size_t visit = 0; visit < watchVisits && elapsed() < 2; ++visit)
    {
        const uint32_t index = s.watchCleanupQueue.front(); s.watchCleanupQueue.pop_front();
        const auto found = s.watches.find(index);
        if (found == s.watches.end()) continue;
        if (static_cast<Object*>(found->second.object)) s.watchCleanupQueue.push_back(index);
        else s.watches.erase(found);
    }
    s.stats.lastPumpMs = elapsed();
}
} // namespace Poseidon
