#include <Poseidon/Foundation/PoseidonPCH.hpp>

#include <Poseidon/AI/AITimeline.hpp>

#include <Poseidon/AI/AICenter.hpp>
#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/AI/EntityAI.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/Core/StateTimeline.hpp>
#include <Poseidon/World/World.hpp>

#include <cstdlib>
#include <unordered_map>

namespace Poseidon::AIDiag
{

using Poseidon::Determinism::FieldKind;
using Poseidon::Determinism::NameId;
using Poseidon::Determinism::StateTimeline;

namespace
{

/// One recording per process. The alternative -- a timeline owned by the World -- loses
/// the recording when a mission ends, which is exactly when it wants writing.
struct Recorder
{
    StateTimeline timeline;
    std::string   path;
    bool          enabled = false;
    bool          flushed = false;
    bool          atexitArmed = false;

    NameId sGroup = 0;
    NameId sUnit = 0;
    NameId fCentre = 0;
    NameId fNetId = 0;
    NameId fUnits = 0;
    NameId fTargets = 0;
    NameId fLife = 0;
    NameId fNearestEnemy = 0;
    NameId fTargetNetId = 0;
    NameId fSlot = 0;
};

Recorder& Rec()
{
    static Recorder r = []
    {
        Recorder rr;
        const char* p = std::getenv("POSEIDON_AI_TIMELINE");
        if (p != nullptr && *p != '\0')
        {
            rr.path = p;
            rr.enabled = true;
            rr.sGroup = rr.timeline.Intern("group");
            rr.sUnit = rr.timeline.Intern("unit");
            rr.fCentre = rr.timeline.Intern("centre");
            rr.fNetId = rr.timeline.Intern("netId");
            rr.fUnits = rr.timeline.Intern("nUnits");
            rr.fTargets = rr.timeline.Intern("nTargets");
            rr.fLife = rr.timeline.Intern("lifeState");
            rr.fNearestEnemy = rr.timeline.Intern("nearestEnemyDist2");
            rr.fTargetNetId = rr.timeline.Intern("targetNetId");
            rr.fSlot = rr.timeline.Intern("slot");
        }
        return rr;
    }();
    if (r.enabled && !r.atexitArmed)
    {
        // Armed here rather than at a shutdown call site, because the paths that end a
        // run are several and none of them is guaranteed: the --check harness exits after
        // its screenshot, a mission can end on its own, and a crash ends nothing. An
        // atexit registered just after this static is constructed runs BEFORE the static
        // is destroyed, so the recording is still intact when it writes.
        r.atexitArmed = true;
        std::atexit([] { (void)FlushAITimeline(); });
    }
    return r;
}

/// Identity WITHOUT a pointer and without a network id.
///
/// The first attempt used `GetNetworkId()`. It compiled, it ran, and it recorded the
/// same value -- zero -- for every unit in the run: network ids are assigned by the
/// network layer and a single-player mission never assigns one. The oracle then held
/// 937,200 fields of which the identity columns were constant, and two runs under
/// DIFFERENT RNG seeds compared EQUIVALENT. A recording that cannot tell two units
/// apart cannot tell two runs apart either.
///
/// So identity is built here: every entity reachable through the sweep gets the index
/// at which the sweep first met it. The sweep order is deterministic (centres in a
/// fixed order, groups by index, slots ascending), so the index means the same thing in
/// both runs -- and if the order itself changes, that is a real divergence and shows up
/// as one rather than being hidden by a key that survived the change.
///
/// A target outside the sweep (a vehicle, an object, an entity of another side that has
/// no group) cannot get a sweep index, so it gets its class name hashed instead, tagged
/// so the two spaces cannot collide. Coarser, and deterministic, which is the property
/// that matters.
constexpr std::uint64_t kOutsideSweep = 1ull << 40;

std::uint64_t HashName(const char* s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (; s != nullptr && *s != '\0'; s++)
    {
        h ^= static_cast<unsigned char>(*s);
        h *= 1099511628211ull;
    }
    return h & 0xffffffffull;
}

std::uint64_t IdentityOf(const EntityAI* e, const std::unordered_map<const EntityAI*, std::uint32_t>& sweep)
{
    if (e == nullptr)
    {
        return 0;
    }
    const auto it = sweep.find(e);
    if (it != sweep.end())
    {
        // +1 so that "in the sweep at index 0" is distinguishable from "absent".
        return static_cast<std::uint64_t>(it->second) + 1ull;
    }
    const EntityAIType* type = const_cast<EntityAI*>(e)->GetType();
    return kOutsideSweep | HashName(type != nullptr ? type->GetName().Data() : "");
}

/// Pass one: walk the sweep and number what it meets. Nothing is recorded here -- the
/// numbering has to exist before the first unit's target can be written down, because a
/// unit may target someone the sweep has not reached yet.
void NumberSweep(AICenter* centre, std::unordered_map<const EntityAI*, std::uint32_t>& sweep, std::uint32_t& next)
{
    if (centre == nullptr)
    {
        return;
    }
    for (int g = 0; g < centre->NGroups(); g++)
    {
        AIGroup* group = centre->GetGroup(g);
        if (group == nullptr)
        {
            continue;
        }
        for (int u = 0; u < MAX_UNITS_PER_GROUP; u++)
        {
            AIUnit* unit = group->UnitWithID(u + 1);
            if (unit == nullptr || unit->GetVehicle() == nullptr)
            {
                continue;
            }
            sweep.emplace(unit->GetVehicle(), next++);
        }
    }
}

void RecordCentre(Recorder& r, AICenter* centre, std::uint32_t centreTag, std::uint32_t& key,
                  const std::unordered_map<const EntityAI*, std::uint32_t>& sweep)
{
    if (centre == nullptr)
    {
        return;
    }
    // Groups by INDEX, unit slots ASCENDING. `CompareTimelines` compares positionally, so
    // this order is part of the recorded state -- an iteration whose order varied between
    // runs would be reported as a divergence that is really just container order.
    for (int g = 0; g < centre->NGroups(); g++)
    {
        AIGroup* group = centre->GetGroup(g);
        if (group == nullptr)
        {
            continue;
        }
        const std::uint32_t groupKey = key++;
        r.timeline.U32(r.sGroup, groupKey, r.fCentre, centreTag);
        r.timeline.U32(r.sGroup, groupKey, r.fUnits, static_cast<std::uint32_t>(group->NUnits()));
        r.timeline.U32(r.sGroup, groupKey, r.fTargets,
                       static_cast<std::uint32_t>(group->GetTargetList().Size()));

        for (int u = 0; u < MAX_UNITS_PER_GROUP; u++)
        {
            // `UnitWithID` is 1-BASED, and AIGroup has no GetUnit -- the GetUnit in this
            // header belongs to AISubgroup. Slot is still recorded 0-based so it reads
            // like the array it is.
            AIUnit* unit = group->UnitWithID(u + 1);
            if (unit == nullptr)
            {
                continue;
            }
            const std::uint32_t unitKey = key++;
            r.timeline.U32(r.sUnit, unitKey, r.fSlot, static_cast<std::uint32_t>(u));
            r.timeline.U64(r.sUnit, unitKey, r.fNetId, IdentityOf(unit->GetVehicle(), sweep));
            r.timeline.U32(r.sUnit, unitKey, r.fLife, static_cast<std::uint32_t>(unit->GetLifeState()));

            // The two fields the tracking ration would move if it moves anything: which
            // target a unit has settled on, and how close it believes the nearest enemy
            // to be. `targetNetId` is Discrete -- a different target is a different
            // decision, never a rounding artefact, and no diagnostic may soften it.
            const Target* target = unit->GetTargetAssigned();
            r.timeline.U64(r.sUnit, unitKey, r.fTargetNetId,
                           target != nullptr ? IdentityOf(target->idExact, sweep) : 0ull);
            r.timeline.F32(r.sUnit, unitKey, r.fNearestEnemy, unit->GetNearestEnemyDist2());
        }
    }
}

} // namespace

bool AITimelineEnabled()
{
    return Rec().enabled;
}

int AITimelineTickCount()
{
    return Rec().timeline.TickCount();
}

void RecordAITick(World* world)
{
    Recorder& r = Rec();
    if (!r.enabled || world == nullptr)
    {
        return;
    }

    std::unordered_map<const EntityAI*, std::uint32_t> sweep;
    std::uint32_t                                      next = 0;
    NumberSweep(world->GetEastCenter(), sweep, next);
    NumberSweep(world->GetWestCenter(), sweep, next);
    NumberSweep(world->GetGuerrilaCenter(), sweep, next);
    NumberSweep(world->GetCivilianCenter(), sweep, next);
    NumberSweep(world->GetLogicCenter(), sweep, next);

    r.timeline.BeginTick();
    std::uint32_t key = 0;
    // Fixed centre order, and the tag is recorded so a centre that is absent in one run
    // shifts the labels rather than silently aligning against another centre's groups.
    // ALL of them, not just the two that fight. A civilian or guerrilla group whose
    // behaviour changed would otherwise be invisible to the oracle, and "we only looked
    // at the sides we expected to move" is not a gate.
    RecordCentre(r, world->GetEastCenter(), 1, key, sweep);
    RecordCentre(r, world->GetWestCenter(), 2, key, sweep);
    RecordCentre(r, world->GetGuerrilaCenter(), 3, key, sweep);
    RecordCentre(r, world->GetCivilianCenter(), 4, key, sweep);
    RecordCentre(r, world->GetLogicCenter(), 5, key, sweep);
    r.timeline.EndTick();
}

std::string FlushAITimeline()
{
    Recorder& r = Rec();
    if (!r.enabled || r.flushed)
    {
        return {};
    }
    r.flushed = true;
    return Determinism::WriteTimeline(r.timeline, r.path);
}

} // namespace Poseidon::AIDiag
