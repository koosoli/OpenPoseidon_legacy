#include <Poseidon/World/Entities/Infantry/SoldierOldCommon.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/Weather/RainWaterField.hpp>
#include <array>
#include <limits.h>
#include <utility>
#include <cstdio>
#include <cstring>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/BankArray.hpp>
#include <Poseidon/Foundation/Containers/BoolArray.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Memory/FastAlloc.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

#pragma warning(disable : 4065)

using namespace Poseidon;
ActionContextBase* CreateGetInActionContext(Transport* veh, UIActionType pos)
{
    using namespace Poseidon;
    return new ActionContextGetIn(veh, pos);
}
namespace Poseidon
{

// One explicit local corpse; owned body/joint handles never enter saves or model
// assets. Epoch mismatch must not remove a new world's reused backend handles.
struct CorpseArticulation
{
    Physics::PhysicsWorld* world = nullptr;
    std::uint64_t epoch = 0;
    std::array<Physics::BodyId, 11> bodies{};
    std::array<Physics::JointId, 10> joints{};
    std::array<Matrix4, 11> initial{};
    Matrix4 object = MIdentity;
    std::vector<int> boneParts;
    float minimumClearance = 0;
    int updates = 0;
    float maximumTravel = 0;
    Vector3 minimum = VZero, maximum = VZero;
    float radius = 0;
    bool Ready() const
    {
        return world && Physics::GetPhysicsWorld() == world && world->Generation() == epoch;
    }
    ~CorpseArticulation()
    {
        if (!Ready()) return;
        for (auto joint : joints) world->RemoveJoint(joint);
        for (auto body : bodies) world->Remove(body);
    }
};

DEFINE_FAST_ALLOCATOR(ActionContextDefault)

ActionContextDefault::ActionContextDefault()
{
    static const RStringB empty = "";
    param = 0;
    param2 = 0;
    param3 = empty;
}

ActionContextDefault::~ActionContextDefault() = default;

DEFINE_FAST_ALLOCATOR(ActionContextUIAction)

ActionContextUIAction::ActionContextUIAction(const UIAction& uiAction) : action(uiAction)
{
    function = MFUIAction;
}

DEFINE_FAST_ALLOCATOR(ActionContextGetIn)

ActionContextGetIn::ActionContextGetIn(Transport* veh, UIActionType pos)
{
    vehicle = veh;
    position = pos;
}

const static MotionEdge NoEdge;

MotionPath::MotionPath() {}

BankArray<AnimationRT> AnimationRTBank;
BankArray<WeightInfo> WeigthBank;

MotionPath::~MotionPath() = default;

MotionType::MotionType()
{
    _entry = nullptr;
    _skeleton = new Skeleton("");
}
MotionType::~MotionType() = default;

void MotionType::AssignSkeleton(RStringB name)
{
    _skeleton = new Skeleton(name);
}

RStringB MotionType::GetMoveName(MoveId id) const
{
    if (id == MoveIdNone)
    {
        return RStringB("<none>");
    }
    return _moveIds.GetName(id);
}

MoveId MotionType::GetMoveId(RStringB name) const
{
    int value = _moveIds.GetValue(name);
    if (value < 0 && name.GetLength() > 0)
    {
        LOG_DEBUG(Physics, "Warning: unknown value '{}'", (const char*)name);
    }
    return MoveId(value);
}

struct UsedContext
{
    AutoArray<bool> _indexUsed;
};

const MotionEdge& MotionType::Edge(MoveId a, MoveId b) const
{
    // find edge in edge list
    PoseidonAssert(a >= 0);
    PoseidonAssert(b >= 0);
    const MotionEdges& edges = _vertex[a];
    for (int i = 0; i < edges.Size(); i++)
    {
        if (edges[i].target == b)
        {
            return edges[i];
        }
    }
    return NoEdge;
}

void MotionType::AddEdge(MoveId a, MoveId b, MotionEdgeType type, float cost)
{
    int iCost = toInt(cost * 50);
    if (iCost > SHRT_MAX || iCost < SHRT_MIN)
    {
        LOG_WARN(Physics, "Move transition cost from {} to {} is out of range: {}", (const char*)GetMoveName(a),
                 (const char*)GetMoveName(b), cost);
    }
    saturate(iCost, SHRT_MIN, SHRT_MAX);
    MotionEdges& edges = _vertex[a];
    for (int i = 0; i < edges.Size(); i++)
    {
        MotionEdge& edge = edges[i];
        if (edge.target == b)
        {
            edge.type = type;
            edge.cost = iCost;
            return;
        }
    }
    edges.Add(MotionEdge(b, iCost, type));
}

void MotionType::DeleteEdge(MoveId a, MoveId b)
{
    MotionEdges& edges = _vertex[a];
    for (int i = 0; i < edges.Size(); i++)
    {
        MotionEdge& edge = edges[i];
        if (edge.target == b)
        {
            edges.Delete(i);
            return;
        }
    }
}

#define LOG_PATH 0

bool MotionType::FindPath(MotionPath& path, MoveId from, MotionPathItem to) const
{
    path.Resize(0);
    int nVerts = _vertex.Size();
    if (from < 0 || from >= nVerts || to.id < 0 || to.id >= nVerts)
    {
        return false;
    }

    const MotionEdge& edge = Edge(from, to.id);
    if ((MotionEdgeType)edge.type != MEdgeNone)
    {
        path.Add(to);
#if LOG_PATH
        LOG_DEBUG(Physics, "Direct path from {} to {}", NAME(from), NAME(to.id));
        LOG_DEBUG(Physics, "  insert {}, cost {}", NAME(to.id), edge.cost);
#endif
        return true;
    }

    AUTO_STATIC_ARRAY(int, vs, 1024);
    AUTO_STATIC_ARRAY(int, d, 1024);
    AUTO_STATIC_ARRAY(int, p, 1024);
    d.Resize(nVerts);
    p.Resize(nVerts);
    for (int i = 0; i < nVerts; i++)
    {
        d[i] = INT_MAX, p[i] = -1;
    }
    for (int i = 0; i < nVerts; i++)
    {
        vs.Add(i);
    }
    d[from] = 0;
    const int unaccessible = INT_MAX / 16;

    for (;;)
    {
        // extract cheapest: may be faster using heap sort, but we do not care
        int minI = -1, minVal = unaccessible;
        for (int i = 0; i < vs.Size(); i++)
        {
            int index = vs[i];
            int val = d[index];
            if (minVal > val)
            {
                minVal = val, minI = i;
            }
        }
        if (minI < 0)
        {
            break; // nothing to add
        }
        int v = vs[minI];
        vs.Delete(minI);
        int vCost = d[v];
        const MotionEdges& e = _vertex[v];
        for (int i = 0; i < e.Size(); i++)
        {
            const MotionEdge& ei = e[i];
            int target = ei.target;
            if (target < 0 || target >= nVerts)
            {
                continue;
            }
            int costToTarget = vCost + ei.cost;
            if (costToTarget < d[target])
            {
                d[target] = costToTarget, p[target] = v;
            }
        }
    }

    if (p[to.id] < 0)
    {
        return false;
    }

    path.Add(to);
#if LOG_PATH
    LOG_DEBUG(Physics, "State search from {} to {}", NAME(from), NAME(to.id));
    LOG_DEBUG(Physics, "  insert {}, cost {}", NAME(to.id), d[to.id]);
#endif
    for (;;)
    {
        int predecessor = p[to.id];
        if (predecessor < 0 || predecessor >= nVerts)
        {
            path.Resize(0);
            return false;
        }
        to = MotionPathItem((MoveId)predecessor);
        if (to.id == from)
        {
#if LOG_PATH
            LOG_DEBUG(Physics, "  found {}, cost {}", NAME(from), d[from]);
#endif
            return true;
        }
        path.Insert(0, to);
#if LOG_PATH
        LOG_DEBUG(Physics, "  insert {}, cost {}", NAME(to.id), d[to.id]);
#endif
    }
}

ActionVehMap::ActionVehMap() = default;

void ActionVehMap::Load(const MotionType* motion, const ParamEntry& map)
{
    RString prefix = "ManAct";
    int nAct = GActionVehNames.FirstInvalidValue();
    _actionMoves.Realloc(nAct);
    _actionMoves.Resize(nAct);
    for (int id = 0; id < nAct; id++)
    {
        _actionMoves[id] = MoveIdNone;
    }

    for (int i = 0; i < map.GetEntryCount(); i++)
    {
        const ParamEntry& entry = map.GetEntry(i);
        RStringB name = entry.GetName();
        RStringB value = entry;
        RString fullName = prefix + name;
        const char* str = fullName;
        int idInt = GActionVehNames.GetValue(str);
        if (idInt < 0)
        {
            RptF("Bad action %s in %s", (const char*)str, (const char*)map.GetName());
            continue;
        }
        ManVehAction id = (ManVehAction)idInt;
        PoseidonAssert(_actionMoves.Size() > id);
        _actionMoves[id] = motion->GetMoveId(value);
        if (_actionMoves[id] == MoveIdNone)
        {
            RptF("Invalid move %s in action %s (%s)", (const char*)value, (const char*)name,
                 (const char*)map.GetContext());
        }
    }
}

} // namespace Poseidon
void InitMan()
{
    using namespace Poseidon;
    const ParamEntry& map = Pars >> "CfgVehicleActions";
    RString prefix = "ManAct";
    for (int i = 0; i < map.GetEntryCount(); i++)
    {
        const ParamEntry& entry = map.GetEntry(i);
        RStringB name = entry.GetName();
        GActionVehNames.AddValue(prefix + name);
    }
}
namespace Poseidon
{

ActionMap* MotionType::NewActionMap(const ParamEntry* cfg)
{
    ActionMapName name;
    name.motion = this;
    name.entry = cfg;
    return _actionMaps.New(name);
}

BlendAnimType* MotionType::NewBlendAnimType(const ParamEntry& cfg)
{
    BlendAnimTypeName name;
    name.motion = this;
    name.cfg = &cfg;
    return _blendAnimTypes.New(name);
}

void MotionType::InitNoActions(const ParamEntry* cfg)
{
    _noActions = NewActionMap(cfg);
}

MoveId MotionType::GetDefaultMove(int upDegree) const
{
    for (int i = 0; i < _actionMaps.Size(); i++)
    {
        const ActionMap* map = _actionMaps[i];
        if (map->GetUpDegree() == upDegree)
        {
            return map->GetAction(ManActDefault);
        }
    }
    return MoveIdNone;
}

MoveId MotionType::GetMove(int upDegree, ManAction action) const
{
    for (int i = 0; i < _actionMaps.Size(); i++)
    {
        const ActionMap* map = _actionMaps[i];
        if (map->GetUpDegree() == upDegree)
        {
            return map->GetAction(action);
        }
    }
    return MoveIdNone;
}

static void CheckArrayMod(const ParamEntry& entry, int mod, const ParamEntry* context = nullptr)
{
    if (entry.GetSize() % mod != 0)
    {
        RptF("%s: item count not multiple of %d (is %d)",
             context ? (const char*)context->GetContext(entry.GetName()) : (const char*)entry.GetName(), mod,
             entry.GetSize());
    }
}

static void BadMove(const ParamEntry& entry, const char* name, const ParamEntry* context = nullptr)
{
    RptF("  %s: Bad move %s",
         context ? (const char*)context->GetContext(entry.GetName()) : (const char*)entry.GetName(), (const char*)name);
}

void MotionType::Load(const ParamEntry& entry)
{
    _skeleton = Skeletons.New(entry.GetName());
    _entry = &entry;
    const ParamEntry& states = entry >> "States";

    _moveIds.Clear();
    for (int i = 0; i < states.GetEntryCount(); i++)
    {
        const ParamEntry& sub = states.GetEntry(i);
        _moveIds.AddValue(sub.GetName());
    }
    _moveIds.Close();

    int moveIdN = MoveIdN();
    _vertex.Realloc(moveIdN);
    _vertex.Resize(moveIdN);

    // load interpolations information
    const ParamEntry& inter = entry >> "Interpolations";
    for (int i = 0; i < inter.GetEntryCount(); i++)
    {
        const ParamEntry& group = inter.GetEntry(i);
        float fCost = group[0];
        // insert all possible pairs of states
        for (int s = 1; s < group.GetSize(); s++)
        {
            RStringB sName = group[s];
            MoveId sMove = GetMoveId(sName);
            if (sMove == MoveIdNone)
            {
                continue;
            }

            for (int r = 1; r < group.GetSize(); r++)
            {
                if (r != s)
                {
                    RStringB rName = group[r];
                    MoveId rMove = GetMoveId(rName);
                    if (rMove == MoveIdNone)
                    {
                        continue;
                    }

                    AddEdge(rMove, sMove, MEdgeInterpol, fCost);
                }
            }
        }
    }

    { // interpolated transitions
        const ParamEntry& trans = entry >> "transitionsInterpolated";
        CheckArrayMod(trans, 3, &entry);
        for (int i = 0; i < trans.GetSize(); i += 3)
        {
            RStringB a = trans[i];
            RStringB b = trans[i + 1];
            float fCost = trans[i + 2];
            MoveId aId = GetMoveId(a);
            MoveId bId = GetMoveId(b);
            if (aId == MoveIdNone || bId == MoveIdNone)
            {
                RptF("Bad ipol transition from %s to %s", (const char*)a, (const char*)b);
                continue;
            }
            AddEdge(aId, bId, MEdgeInterpol, fCost);
        }
    }
    { // simple transitions
        const ParamEntry& trans = entry >> "transitionsSimple";

        CheckArrayMod(trans, 3, &entry);
        for (int i = 0; i < trans.GetSize(); i += 3)
        {
            RStringB a = trans[i];
            RStringB b = trans[i + 1];
            float fCost = trans[i + 2];
            MoveId aId = GetMoveId(a);
            MoveId bId = GetMoveId(b);
            if (aId == MoveIdNone || bId == MoveIdNone)
            {
                RptF("Bad simple transition from %s to %s", (const char*)a, (const char*)b);
                continue;
            }
            AddEdge(aId, bId, MEdgeSimple, fCost);
        }
    }

    for (int i = 0; i < moveIdN; i++)
    {
        const ParamEntry& state = states.GetEntry(i);
        const ParamEntry& conFrom = state >> "connectFrom";
        CheckArrayMod(conFrom, 2, &state);
        for (int s = 0; s < conFrom.GetSize() - 1; s += 2)
        {
            RStringB name = conFrom[s];
            float cost = conFrom[s + 1];
            MoveId id = GetMoveId(name);
            if (id == MoveIdNone)
            {
                BadMove(conFrom, name, &state);
            }
            else
            {
                AddEdge(id, (MoveId)i, MEdgeSimple, cost);
            }
        }
        const ParamEntry& conTo = state >> "connectTo";
        CheckArrayMod(conTo, 2, &state);
        for (int s = 0; s < conTo.GetSize() - 1; s += 2)
        {
            RStringB name = conTo[s];
            float cost = conTo[s + 1];
            MoveId id = GetMoveId(name);
            if (id == MoveIdNone)
            {
                BadMove(conTo, name, &state);
            }
            else
            {
                AddEdge((MoveId)i, id, MEdgeSimple, cost);
            }
        }

        const ParamEntry& ipolW = state >> "interpolateWith";
        CheckArrayMod(ipolW, 2, &state);
        for (int s = 0; s < ipolW.GetSize() - 1; s += 2)
        {
            RStringB name = ipolW[s];
            float cost = ipolW[s + 1];
            MoveId id = GetMoveId(name);
            if (id == MoveIdNone)
            {
                BadMove(ipolW, name, &state);
            }
            else
            {
                AddEdge((MoveId)i, id, MEdgeInterpol, cost);
                AddEdge(id, (MoveId)i, MEdgeInterpol, cost);
            }
        }

        const ParamEntry& ipolT = state >> "interpolateTo";
        CheckArrayMod(ipolT, 2, &state);
        for (int s = 0; s < ipolT.GetSize() - 1; s += 2)
        {
            RStringB name = ipolT[s];
            float cost = ipolT[s + 1];
            MoveId id = GetMoveId(name);
            if (id == MoveIdNone)
            {
                BadMove(ipolT, name, &state);
            }
            else
            {
                AddEdge((MoveId)i, id, MEdgeInterpol, cost);
            }
        }

        const ParamEntry& ipolF = state >> "interpolateFrom";
        CheckArrayMod(ipolF, 2, &state);
        for (int s = 0; s < ipolF.GetSize() - 1; s += 2)
        {
            RStringB name = ipolF[s];
            float cost = ipolF[s + 1];
            MoveId id = GetMoveId(name);
            if (id == MoveIdNone)
            {
                BadMove(ipolF, name, &state);
            }
            else
            {
                AddEdge(id, (MoveId)i, MEdgeInterpol, cost);
            }
        }
    }

    for (int i = 0; i < moveIdN; i++)
    {
        const ParamEntry& state = states.GetEntry(i);
        const RStringB& asState = state >> "connectAs";
        if (asState.GetLength() <= 0)
        {
            continue;
        }
        MoveId as = GetMoveId(asState);
        if (as == MoveIdNone)
        {
            continue;
        }
        for (int j = 0; j < moveIdN; j++)
        {
            const MotionEdge& sEdge = Edge(as, (MoveId)j);
            if ((MotionEdgeType)sEdge.type == MEdgeNone)
            {
                continue;
            }
            const MotionEdge& tEdge = Edge((MoveId)i, (MoveId)j);
            if ((MotionEdgeType)tEdge.type != MEdgeNone)
            {
                continue;
            }
            AddEdge((MoveId)i, (MoveId)j, sEdge.type, sEdge.GetCost());
        }
        for (int j = 0; j < moveIdN; j++)
        {
            const MotionEdge& sEdge = Edge((MoveId)j, as);
            if ((MotionEdgeType)sEdge.type == MEdgeNone)
            {
                continue;
            }
            const MotionEdge& tEdge = Edge((MoveId)j, (MoveId)i);
            if ((MotionEdgeType)tEdge.type != MEdgeNone)
            {
                continue;
            }
            AddEdge((MoveId)j, (MoveId)i, sEdge.type, sEdge.GetCost());
        }
    }

    { // disabled transitions
        const ParamEntry& trans = entry >> "transitionsDisabled";
        CheckArrayMod(trans, 2, &entry);

        for (int i = 0; i < trans.GetSize(); i += 2)
        {
            RStringB a = trans[i];
            RStringB b = trans[i + 1];
            MoveId aId = GetMoveId(a);
            MoveId bId = GetMoveId(b);
            if (aId == MoveIdNone || bId == MoveIdNone)
            {
                RptF("Bad disabled transition from %s to %s", (const char*)a, (const char*)b);
                continue;
            }
            DeleteEdge(aId, bId);
        }
    }

    _vertex.Compact();
    for (int i = 0; i < _vertex.Size(); i++)
    {
        _vertex[i].Compact();
    }

    RStringB vehicleActionsName = entry >> "vehicleActions";
    _actionVehMap.Load(this, Pars >> vehicleActionsName);
}

void MotionType::Unload()
{
    _moveIds.Clear();
    _vertex.Clear();
    _entry = nullptr;
}

const float TimeToRun = 6;
const float TimeToCrawl = 8;
const float WaitBeforeStandUp = TimeToRun + TimeToCrawl;

int Man::GetActUpDegree() const
{
    ActionMap* map = Type()->GetActionMap(_primaryMove.id);
    return map ? map->GetUpDegree() : ManPosStand;
}

DEFINE_CASTING(Man)

Man::Man(VehicleType* name, bool fullCreate)
    : Person(name, fullCreate),

      _turnToDo(0), _walkToggle(false), _inBuilding(false),

      _doSoundStep(false), _freeFallUntil(Glob.time - 60), _stillMoveQueueEnd(MoveIdNone), _variantTime(Glob.time),

      _aimInaccuracyX(0), _aimInaccuracyY(0), _aimInaccuracyDist(0), _lastInaccuracyTime(Glob.time),
      _lastInaccuracyDistTime(Glob.time),

      _lastObjectContactTime(Glob.time), _lastMovementTime(Glob.time),

      _waterDepth(0), _hydroWaterDepth(0), _waterBuoyancyContact(false), _unitPos(UPAuto),

      _hideBody(0), _hideBodyWanted(0),

      _primaryMove((MoveId)MoveIdNone), _secondaryMove((MoveId)MoveIdNone), _externalMove((MoveId)MoveIdNone),
      _externalMoveFinished(false), _forceMove((MoveId)MoveIdNone), _hasNVG(false),

      _upDegreeChangeTime(Glob.time - 5), _upDegreeStable(ManPosStand),

      _posWanted(ManPosStand), _posWantedTime(TIME_MAX),

      _primaryFactor(1), _primaryTime(0), _secondaryTime(0),

      _walkSpeedWanted(0),

      _aimingPositionWorld(VZero), _cameraPositionWorld(VZero),

      _whenKilled(0), _whenScreamed(TIME_MAX),

      _tired(0), _canMoveFast(true),

      _showPrimaryWeapon(true), _showSecondaryWeapon(true), _showHead(true), _manScale(1),

      _nvg(false),

      _gunTrans(MIdentity), _headTrans(MIdentity), _headTransIdent(true), _gunTransIdent(true),

      _correctBankSin(0), _correctBankCos(1), _legTrans(MIdentity),

      _gunYRot(0), _gunYRotWanted(0), _gunXRot(0), _gunXRotWanted(0), _gunXSpeed(0), _gunYSpeed(0),

      _headYRot(0), _headYRotWanted(0), _headXRot(0), _headXRotWanted(0),

      _lookForwardTimeLeft(0), _lookTargetTimeLeft(0),

      _ladderBuilding(nullptr), _ladderIndex(-1), _ladderPosition(0),

      _handGun(false),

      _turnWanted(0), _head(Type()->_head, _shape)
{
    SetSimulationPrecision(1.0f / 5); // 5 times per sec is enough
    RecalcGunTransform();

    _destrType = DestructMan;

    _mGunClouds.Load((*Type()->_par) >> "MGunClouds");
    // Ordinary firearm sprites are visible effects, not dense shadow-casting
    // smoke. Keep the separate muzzle light and intentional smoke sources.
    _mGunClouds.SetSmokeShadow(false);
    _gunClouds.Load((*Type()->_par) >> "GunClouds");
    _mGunFireFrames = 0;
    _mGunFireTime = UITIME_MIN;
    _mGunFirePhase = 0;

    _head.SetMimicMode("neutral");
    ScanNVG();
}

Man::~Man() { ClearCorpsePose(); }

namespace
{
// Weak entity identity only. The actual phase copies belong exclusively to Man.
OLink<Man> g_corpsePoseOwner;
}

void Man::ClearCorpsePose() const
{
    _corpseArticulation.reset();
    _corpsePoseLease = {};
    _corpsePosePrimary.Clear();
    _corpsePoseSecondary.Clear();
    _corpsePoseModel = nullptr;
    _corpsePoseSkeleton = nullptr;
    _corpsePoseWeights = nullptr;
    _corpsePosePrimaryAnimation = nullptr;
    _corpsePoseSecondaryAnimation = nullptr;
}

const char* Man::CorpsePoseRefusal() const
{
    if (!GWorld || GWorld->GetMode() != GModeArcade) return "local-single-player-required";
    if (!IsLocal()) return "locally-owned-corpse-required";
    if (!_isDead || !IsDead() || (!_corpseArticulation && !ShadowPoseFrozen())) return "settled-death-pose-required";
    if (!_landContact || _waterContact || _waterBuoyancyContact || _waterDepth > 0 || _hydroWaterDepth > 0)
        return "dry-ground-contact-required";
    if (_hideBody != 0 || _hideBodyWanted != 0) return "hidden-or-hiding";
    if (_ladderBuilding || GetHierachyParent()) return "attached-or-on-ladder";
    if (!IsInLandscape() || ToDelete() || ToMoveOut()) return "removed-or-deleting";
    if (!_shape || _primaryMove.id == MoveIdNone) return "model-or-move-unavailable";
    if (_corpseArticulation)
    {
        auto water = GRainWater().At(Position().X(), Position().Z());
        if (water.valid && water.depth > 0.003f) return "articulated-corpse-entered-rain-water";
    }
    return nullptr;
}

bool Man::UseCorpsePose() const
{
    if (!_corpsePoseLease.world || _corpsePoseBypass) return false;
    if (_corpseArticulation && !_corpseArticulation->Ready()) { ClearCorpsePose(); return false; }
    // Reject removal/world changes before touching type-owned animation tables.
    if (_corpsePoseLease.world != GWorld || CorpsePoseRefusal()) { ClearCorpsePose(); return false; }
    Skeleton* skeleton = Type()->GetWeights().GetName().skeleton.GetTypeRef();
    if (!skeleton || _corpsePoseWeights.GetRef() != &Type()->GetWeights() ||
        !_corpsePoseLease.Matches(GWorld, _shape.GetRef(), skeleton, skeleton->NBones(),
                                  int(_primaryMove.id), int(_secondaryMove.id), _primaryFactor, Glob.time.toInt()) ||
        (_corpsePosePrimary.Valid() && Type()->GetAnimation(_primaryMove.id) != _corpsePosePrimaryAnimation.GetRef()) ||
        (_corpsePoseSecondary.Valid() && Type()->GetAnimation(_secondaryMove.id) != _corpsePoseSecondaryAnimation.GetRef()))
    {
        ClearCorpsePose();
        return false;
    }
    return true;
}

bool Man::CaptureCorpsePose(RString& refusal)
{
    if (const char* reason = CorpsePoseRefusal()) { refusal = reason; return false; }
    refusal = "blend-or-skeleton-admission";
    if (!std::isfinite(_primaryFactor) || _primaryFactor < 0 || _primaryFactor > 1) return false;
    Skeleton* skeleton = Type()->GetWeights().GetName().skeleton.GetTypeRef();
    if (!skeleton || skeleton->NBones() <= 0 || skeleton->NBones() > 128) return false;
    AffinePhasePose primary, secondary;
    AnimationRT* primaryAnim = nullptr;
    AnimationRT* secondaryAnim = nullptr;
    if (_primaryFactor > 0.01f)
    {
        refusal = "primary-phase-admission";
        primaryAnim = Type()->GetAnimation(_primaryMove.id);
        if (!primaryAnim || primaryAnim->GetSkeleton() != skeleton || !primaryAnim->CapturePhasePose(primary, _primaryTime))
            return false;
    }
    if (_primaryFactor < 0.99f)
    {
        refusal = "secondary-phase-admission";
        secondaryAnim = Type()->GetAnimation(_secondaryMove.id);
        if (!secondaryAnim || secondaryAnim->GetSkeleton() != skeleton || !secondaryAnim->CapturePhasePose(secondary, _secondaryTime))
            return false;
    }
    const WeightInfo& weights = Type()->GetWeights();
    int memory = _shape->FindMemoryLevel();
    refusal = "weight-admission";
    // Admit every special point queried in tree. Palette/proxy tails retain their
    // existing identity fallback, while Point has no such fallback for face bones.
    for (int level = 0; level < _shape->NLevels(); ++level)
    {
        Shape* shape = _shape->Level(level);
        if (!shape) continue;
        for (int p = 0; p < shape->NPos(); ++p)
        {
            const AnimationRTWeight& weight = weights[level][p];
            for (int w = 0; w < weight.Size(); ++w)
                if (weight[w].GetSel() < 0 || weight[w].GetSel() >= skeleton->NBones()) return false;
            if (level == memory || shape->NPos() == 1)
                if ((primary.Valid() && !primary.SupportsPoint(weight)) ||
                    (secondary.Valid() && !secondary.SupportsPoint(weight)))
                {
                    char detail[192];
                    std::snprintf(detail, sizeof(detail), "point-phase-range level=%d point=%d primaryMatrices=%d secondaryMatrices=%d",
                                  level, p, primary.PhaseMatrices(), secondary.PhaseMatrices());
                    refusal = detail;
                    return false;
                }
        }
    }
    if (g_corpsePoseOwner && g_corpsePoseOwner.GetLink() != this) g_corpsePoseOwner->ClearCorpsePose();
    ClearCorpsePose();
    _corpsePosePrimary = std::move(primary);
    _corpsePoseSecondary = std::move(secondary);
    _corpsePoseModel = _shape;
    _corpsePoseSkeleton = skeleton;
    _corpsePoseWeights = &Type()->GetWeights();
    _corpsePosePrimaryAnimation = primaryAnim;
    _corpsePoseSecondaryAnimation = secondaryAnim;
    _corpsePoseLease = {GWorld, _shape.GetRef(), skeleton, skeleton->NBones(),
                        int(_primaryMove.id), int(_secondaryMove.id), Glob.time.toInt(), _primaryFactor};
    g_corpsePoseOwner = this;
    refusal = "";
    return true;
}

void Man::PrepareBasePose(Matrix4Array& matrices) const
{
    const bool held = UseCorpsePose();
    if (_primaryFactor > 0.01f)
    {
        AnimationRT* anim = Type()->GetAnimation(_primaryMove.id);
        if (anim)
        {
            if (held) _corpsePosePrimary.PrepareMatrices(matrices, _primaryFactor);
            else anim->PrepareMatrices(matrices, _primaryTime, _primaryFactor);
        }
    }
    if (_primaryFactor < 0.99f)
    {
        AnimationRT* anim = Type()->GetAnimation(_secondaryMove.id);
        if (anim)
        {
            if (held) _corpsePoseSecondary.PrepareMatrices(matrices, 1 - _primaryFactor);
            else anim->PrepareMatrices(matrices, _secondaryTime, 1 - _primaryFactor);
        }
    }
}

RString Man::CompareCorpsePose() const
{
    if (!UseCorpsePose()) return "REFUSED corpse-compare no-valid-hold";
    // Run the actual Man consumers twice in one main-thread command. No simulation,
    // root transform, shared mesh animation or solver steps occur between them.
    struct BypassReset { bool& flag; ~BypassReset() { flag = false; } } reset{_corpsePoseBypass};
    MATRIX_4_ARRAY(authored, 128);
    MATRIX_4_ARRAY(held, 128);
    _corpsePoseBypass = true;
    PrepareBasePose(authored);
    _corpsePoseBypass = false;
    PrepareBasePose(held);
    bool paletteEqual = authored.Size() == held.Size();
    for (int i = 0; i < authored.Size() && paletteEqual; ++i)
        paletteEqual = std::memcmp(&authored[i], &held[i], sizeof(Matrix4)) == 0;
    int points = 0, skipped = 0, proxies = 0, pointMismatch = 0, proxyMismatch = 0;
    float maxPointError = 0;
    const WeightInfo& weights = Type()->GetWeights();
    for (int level = 0; level < _shape->NLevels(); ++level)
    {
        Shape* shape = _shape->Level(level);
        if (!shape) continue;
        int samples = std::min(64, shape->NPos());
        for (int sample = 0; sample < samples; ++sample)
        {
            int index = sample * shape->NPos() / samples;
            if ((_corpsePosePrimary.Valid() && !_corpsePosePrimary.SupportsPoint(weights[level][index])) ||
                (_corpsePoseSecondary.Valid() && !_corpsePoseSecondary.SupportsPoint(weights[level][index])))
            { ++skipped; continue; }
            _corpsePoseBypass = true;
            Vector3 a = AnimatePoint(level, index);
            _corpsePoseBypass = false;
            Vector3 b = AnimatePoint(level, index);
            ++points;
            if (a.X() != b.X() || a.Y() != b.Y() || a.Z() != b.Z()) ++pointMismatch;
            maxPointError = std::max(maxPointError, (a - b).Size());
        }
        for (int i = 0; i < shape->NProxies(); ++i)
        {
            const ProxyObject& proxy = shape->Proxy(i);
            if (proxy.selection < 0 || proxy.selection >= shape->NNamedSel() ||
                shape->NamedSel(proxy.selection).Size() <= 0) continue;
            const AnimationRTWeight& w = GetProxyWeights(level, proxy);
            if (w.Size() <= 0) continue; // AnimateMatrix(selection) requires w[0].
            _corpsePoseBypass = true;
            Matrix4 a = AnimateProxyMatrix(level, proxy);
            _corpsePoseBypass = false;
            Matrix4 b = AnimateProxyMatrix(level, proxy);
            ++proxies;
            if (std::memcmp(&a, &b, sizeof(Matrix4)) != 0) ++proxyMismatch;
        }
    }
    char result[768];
    std::snprintf(result, sizeof(result),
        "%s corpse-compare entity=%p model=%s paletteMatrices=%d paletteEqual=%d points=%d pointMismatch=%d "
        "maxPointError=%.9g skippedPointTails=%d proxies=%d proxyMismatch=%d holdValid=%d",
        paletteEqual && pointMismatch == 0 && proxyMismatch == 0 && UseCorpsePose() ? "OK" : "MISMATCH",
        static_cast<const void*>(this), _shape->Name(), held.Size(), int(paletteEqual), points, pointMismatch,
        maxPointError, skipped, proxies, proxyMismatch, int(UseCorpsePose()));
    return result;
}

bool Man::GetCorpsePoseDiagnostic(CorpsePoseDiagnostic& result, RString& refusal)
{
    result = {};
    Man* man = g_corpsePoseOwner.GetLink();
    if (!man || !man->UseCorpsePose()) { refusal = "no-valid-hold"; return false; }
    return man->InspectCorpsePose(result, refusal);
}

bool Man::InspectCorpsePose(CorpsePoseDiagnostic& result, RString& refusal) const
{
    refusal = "stock-western-model-required";
    std::string model = _shape->Name();
    std::replace(model.begin(), model.end(), '/', '\\');
    if (strcmpi(model.c_str(), "data3d\\mc vojakw2.p3d") != 0) return false;
    refusal = "finite-correction-or-world-transform-required";
    if (!_headTrans.IsFinite() || !_gunTrans.IsFinite() || !_legTrans.IsFinite() || !Transform().IsFinite()) return false;
    Skeleton* skeleton = Type()->GetWeights().GetName().skeleton.GetTypeRef();
    if (!GLandscape || !skeleton || skeleton->NBones() > 128 || _shape->NLevels() > MAX_LOD_LEVELS) return false;
    CorpsePoseDiagnostic candidate;
    candidate.articulated = bool(_corpseArticulation);
    candidate.model = _shape->Name();
    char identity[64];
    std::snprintf(identity, sizeof(identity), "%p", static_cast<const void*>(this));
    candidate.entity = identity;
    candidate.capturedMs = _corpsePoseLease.capturedMs;
    candidate.measuredMs = Glob.time.toInt();
    candidate.headIdentityFlag = _headTransIdent;
    candidate.gunIdentityFlag = _gunTransIdent;
    candidate.headCorrection = _headTrans;
    candidate.gunCorrection = _gunTrans;
    candidate.legCorrection = _legTrans;
    candidate.headIdentityError = CorpseMatrixIdentityError(_headTrans);
    candidate.gunIdentityError = CorpseMatrixIdentityError(_gunTrans);
    candidate.legIdentityError = CorpseMatrixIdentityError(_legTrans);
    for (int bone = 0; bone < skeleton->NBones(); ++bone)
        candidate.bones.emplace_back(static_cast<const char*>(skeleton->GetBone(bone)));
    const char* names[] = {"pchodidlo", "lchodidlo", "pprsty", "lprsty", "lholen", "pholen", "pstehno", "lstehno",
        "pzadek", "lzadek", "bricho", "zebra", "hrudnik", "krk", "prameno", "lrameno", "hlava", "pbiceps",
        "lbiceps", "ploket", "lloket", "roura", "zbran", "pruka", "lruka"};
    refusal = "complete-stock-bone-names-required";
    for (const char* name : names) if (skeleton->FindBone(name) < 0) return false;
    const WeightInfo& weights = Type()->GetWeights();
    const int sourceLevels[] = {0, _shape->FindMemoryLevel(), _shape->FindGeometryLevel(),
        _shape->FindFireGeometryLevel(), _shape->FindViewGeometryLevel(), _shape->FindLandContactLevel()};
    const char* roles[] = {"graphical0", "memory", "geometry", "fireGeometry", "viewGeometry", "landContact"};
    for (int role = 0; role < 6; ++role)
        if (sourceLevels[role] < 0 || sourceLevels[role] >= _shape->NLevels())
        { refusal = "stock-special-lod-missing"; return false; }
    for (int level = 0; level < _shape->NLevels(); ++level)
    {
        Shape* shape = _shape->Level(level);
        if (!shape || shape->NPos() <= 0 || shape->NPos() > 4096) { refusal = "bounded-stock-points-required"; return false; }
        CorpseLevelDiagnostic measured;
        measured.level = level;
        measured.role = "other";
        for (int role = 0; role < 6; ++role)
            if (sourceLevels[role] == level)
            {
                if (measured.role == "other") measured.role = roles[role];
                else measured.role += std::string("+") + roles[role];
            }
        measured.points = shape->NPos();
        measured.pointOnly = shape->NPos() == 1;
        MATRIX_4_ARRAY(palette, 128);
        PrepareCorrectedPose(level, palette);
        measured.faceEvaluated = _head.EvaluateDeadFaceBones(Type()->_head, weights, level, palette, _isDead);
        if (palette.Size() != skeleton->NBones()) { refusal = "complete-palette-required"; return false; }
        for (int bone = 0; bone < palette.Size(); ++bone)
        {
            if (!palette[bone].IsFinite()) { refusal = "nonfinite-final-palette"; return false; }
            const float determinant = std::abs(CorpseMatrixDeterminant(palette[bone]));
            if (!std::isfinite(determinant)) { refusal = "nonfinite-final-determinant"; return false; }
            if (bone == 0) measured.minAbsDeterminant = determinant;
            else measured.minAbsDeterminant = std::min(measured.minAbsDeterminant, determinant);
            measured.palette.push_back(palette[bone]);
        }
        for (int p = 0; p < shape->NPos(); ++p)
        {
            const AnimationRTWeight& weight = weights[level][p];
            for (int w = 0; w < weight.Size(); ++w)
                if (weight[w].GetSel() < 0 || weight[w].GetSel() >= palette.Size())
                { refusal = "final-point-weight-range"; return false; }
            const Vector3 projected = AnimationRT::ApplyMatricesPoint(weights[level], _shape, level, palette, p);
            if (!projected.IsFinite()) { refusal = "nonfinite-final-point"; return false; }
            if (p == 0) measured.paletteMin = measured.paletteMax = projected;
            for (int axis = 0; axis < 3; ++axis)
            {
                measured.paletteMin[axis] = std::min(measured.paletteMin[axis], projected[axis]);
                measured.paletteMax[axis] = std::max(measured.paletteMax[axis], projected[axis]);
            }
            if (weight.Size() <= 0) ++measured.unweighted;
            if ((_corpsePosePrimary.Valid() && !_corpsePosePrimary.SupportsPoint(weight)) ||
                (_corpsePoseSecondary.Valid() && !_corpsePoseSecondary.SupportsPoint(weight)))
            { ++measured.skippedPointTails; continue; }
            const Vector3 actual = AnimatePoint(level, p);
            if (!actual.IsFinite()) { refusal = "nonfinite-special-point"; return false; }
            const float error = (actual - projected).Size();
            if (!std::isfinite(error)) { refusal = "nonfinite-point-discrepancy"; return false; }
            ++measured.compared;
            measured.maxPointPaletteError = std::max(measured.maxPointPaletteError, error);
        }
        for (int i = 0; i < shape->NProxies(); ++i)
        {
            const ProxyObject& proxy = shape->Proxy(i);
            if (proxy.selection < 0 || proxy.selection >= shape->NNamedSel() ||
                shape->NamedSel(proxy.selection).Size() <= 0) continue;
            const int proxyPoint = shape->NamedSel(proxy.selection)[0];
            if (proxyPoint < 0 || proxyPoint >= shape->NPos()) { refusal = "proxy-point-range"; return false; }
            if (GetProxyWeights(level, proxy).Size() <= 0) continue;
            CorpseProxyDiagnostic item;
            item.level = level;
            item.selection = shape->NamedSel(proxy.selection).Name();
            item.matrix = AnimateProxyMatrix(level, proxy);
            if (!item.matrix.IsFinite()) { refusal = "nonfinite-proxy"; return false; }
            candidate.proxies.push_back(std::move(item));
        }
        if (level == _shape->FindFireGeometryLevel())
        {
            // Inspect actual runtime authored membership, not the offline raw PCA.
            for (int component = 1; component <= 14; ++component)
            {
                char name[32];
                std::snprintf(name, sizeof(name), "component%02d", component);
                const int selection = shape->FindNamedSel(name);
                if (selection < 0) { refusal = "stock-fire-component-missing"; return false; }
                const NamedSelection& selected = shape->NamedSel(selection);
                if (selected.Size() <= 0 || selected.Size() > 64) { refusal = "bounded-fire-component-required"; return false; }
                CorpseHullDiagnostic hull;
                hull.name = name;
                hull.points = selected.Size();
                hull.exclusiveFullWeight = true;
                int bodyBone = -1;
                Vector3 center = VZero;
                for (int i = 0; i < selected.Size(); ++i)
                {
                    const int p = selected[i];
                    if (p < 0 || p >= shape->NPos()) { refusal = "fire-component-point-range"; return false; }
                    const AnimationRTWeight& weight = weights[level][p];
                    if (weight.Size() != 1 || weight[0].GetWeight() != 1.0f) hull.exclusiveFullWeight = false;
                    const int bone = weight.Size() == 1 ? weight[0].GetSel() : -1;
                    if (i == 0) bodyBone = bone;
                    if (bone != bodyBone) hull.exclusiveFullWeight = false;
                    const Vector3 world = PositionModelToWorld(AnimatePoint(level, p));
                    const float support = GLandscape->RoadSurfaceY(world + VUp * 0.5f);
                    if (!world.IsFinite() || !std::isfinite(support)) { refusal = "nonfinite-hull-support"; return false; }
                    center += world;
                    if (i == 0)
                    {
                        hull.worldMin = hull.worldMax = world;
                        hull.minRoadSupportClearance = world.Y() - support;
                    }
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        hull.worldMin[axis] = std::min(hull.worldMin[axis], world[axis]);
                        hull.worldMax[axis] = std::max(hull.worldMax[axis], world[axis]);
                    }
                    hull.minRoadSupportClearance = std::min(hull.minRoadSupportClearance, world.Y() - support);
                }
                if (bodyBone >= 0) hull.bone = candidate.bones[bodyBone];
                center *= 1.0f / selected.Size();
                // Two bounded real geometry rays, ignoring the corpse. This is not
                // a convex-overlap/contact admission: expose that limitation to callers.
                CollisionBuffer external;
                GLandscape->ObjectCollision(external, const_cast<Man*>(this), const_cast<Man*>(this),
                    center - VUp * 0.5f, center + VUp * 0.5f, 0, ObjIntersectGeom);
                hull.externalCenterRayHits = external.Size();
                candidate.hulls.push_back(std::move(hull));
            }
        }
        candidate.levels.push_back(std::move(measured));
    }
    Shape* graphical = _shape->Level(0);
    const auto& final = candidate.levels.front().palette;
    const char* boundaries[][2] = {{"krk", "hlava"}, {"prameno", "pbiceps"}, {"lrameno", "lbiceps"},
        {"pbiceps", "ploket"}, {"lbiceps", "lloket"}, {"pzadek", "pstehno"}, {"lzadek", "lstehno"},
        {"pstehno", "pholen"}, {"lstehno", "lholen"}};
    graphical->SaveOriginalPos();
    for (const auto& boundary : boundaries)
    {
        CorpseAnchorDiagnostic anchor;
        anchor.first = boundary[0]; anchor.second = boundary[1];
        int first = graphical->FindNamedSel(boundary[0]), second = graphical->FindNamedSel(boundary[1]);
        if (first < 0 || second < 0) { refusal = "stock-anchor-selection-missing"; return false; }
        const NamedSelection& a = graphical->NamedSel(first);
        const NamedSelection& b = graphical->NamedSel(second);
        if (a.Size() > 4096 || b.Size() > 4096) { refusal = "bounded-anchor-selection-required"; return false; }
        float total = 0;
        for (int i = 0; i < a.Size(); ++i)
        {
            if (a[i] < 0 || a[i] >= graphical->NPos()) { refusal = "anchor-point-range"; return false; }
            const int other = b.Find(a[i]);
            if (other < 0) continue;
            float weight = float(std::min(a.Weight(i), b.Weight(other)));
            if (weight <= 0) continue;
            bool proxy = false;
            for (int s = 0; s < graphical->NNamedSel(); ++s)
                if (std::strncmp(graphical->NamedSel(s).Name(), "proxy:", 6) == 0 &&
                    graphical->NamedSel(s).IsSelected(a[i])) { proxy = true; break; }
            if (proxy) continue;
            anchor.modelAnchor += graphical->OrigPos(a[i]) * weight;
            total += weight; ++anchor.sharedVertices;
        }
        if (total > 0)
        {
            anchor.modelAnchor *= 1.0f / total;
            anchor.firstWorld = PositionModelToWorld(final[skeleton->FindBone(boundary[0])] * anchor.modelAnchor);
            anchor.secondWorld = PositionModelToWorld(final[skeleton->FindBone(boundary[1])] * anchor.modelAnchor);
            anchor.separation = (anchor.firstWorld - anchor.secondWorld).Size();
            if (!anchor.firstWorld.IsFinite() || !anchor.secondWorld.IsFinite() || !std::isfinite(anchor.separation))
            { refusal = "nonfinite-anchor"; return false; }
        }
        candidate.anchors.push_back(std::move(anchor));
    }
    if (!UseCorpsePose()) { refusal = "hold-retired-during-measurement"; return false; }
    result = std::move(candidate);
    refusal = "";
    return true;
}

bool Man::StartCorpseArticulation(RString& refusal)
{
    if (!UseCorpsePose() || _corpseArticulation) { refusal = "captured-authored-corpse-required"; return false; }
    if (std::abs(CorpseMatrixDeterminant(Transform())-1) > 1e-4f)
    { refusal = "unit-scale-world-frame-required"; return false; }
    std::string sourceModel = _shape->Name(); std::replace(sourceModel.begin(),sourceModel.end(),'/','\\');
    if (strcmpi(sourceModel.c_str(), "data3d\\mc vojakw2.p3d") != 0)
    { refusal = "stock-western-model-only"; return false; }
    if (CorpseMatrixIdentityError(_headTrans) > 1e-6f || CorpseMatrixIdentityError(_gunTrans) > 1e-6f ||
        CorpseMatrixIdentityError(_legTrans) > 1e-6f)
    { refusal = "identity-post-animation-corrections-required"; return false; }
    auto water = GRainWater().At(Position().X(), Position().Z());
    if (water.valid && water.depth > 0.003f) { refusal = "dry-retained-water-support-required"; return false; }
    Physics::PhysicsWorld* world = Physics::GetPhysicsWorld();
    if (!world || !world->IsCreated() || !world->GetStats().terrainRegistered)
    { refusal = "registered-physics-terrain-required"; return false; }
    Skeleton* skeleton = _corpsePoseSkeleton.GetRef();
    if (!skeleton || skeleton->NBones() != 33) { refusal = "complete-stock-33-bone-palette-required"; return false; }
    const char* names[][6] = {{"pzadek","lzadek","bricho",nullptr},
        {"zebra","hrudnik","krk","prameno","lrameno",nullptr}, {"hlava",nullptr},
        {"pbiceps",nullptr}, {"ploket","pruka",nullptr}, {"lbiceps",nullptr}, {"lloket","lruka",nullptr},
        {"pstehno",nullptr}, {"pholen","pchodidlo","pprsty",nullptr},
        {"lstehno",nullptr}, {"lholen","lchodidlo","lprsty",nullptr}};
    auto state = std::make_unique<CorpseArticulation>();
    state->world = world; state->epoch = world->Generation(); state->object = Transform();
    state->boneParts.assign(33, -1);
    for (int part = 0; part < 11; ++part)
        for (int name = 0; names[part][name]; ++name)
        {
            int bone = skeleton->FindBone(names[part][name]);
            if (bone < 0 || bone >= 25 || state->boneParts[bone] != -1)
            { refusal = "missing-or-ambiguous-stock-bone"; return false; }
            state->boneParts[bone] = part;
        }
    int rifle = skeleton->FindBone("zbran"), launcher = skeleton->FindBone("roura");
    if (rifle < 0 || rifle >= 25 || launcher < 0 || launcher >= 25 || rifle == launcher)
    { refusal = "equipment-bone-binding-required"; return false; }
    state->boneParts[rifle] = 4; state->boneParts[launcher] = 1;
    for (int bone = 25; bone < 33; ++bone)
    {
        if (std::strncmp(skeleton->GetBone(bone), "%face_", 6) != 0)
        { refusal = "unsupported-synthetic-bone"; return false; }
        state->boneParts[bone] = 2;
    }
    for (int part : state->boneParts) if (part < 0) { refusal = "unmapped-stock-bone"; return false; }

    CorpsePoseDiagnostic diagnostic;
    if (!InspectCorpsePose(diagnostic, refusal)) return false;
    for (const auto& hull : diagnostic.hulls)
        if (hull.externalCenterRayHits != 0)
        { refusal = "open-ground-no-external-center-ray-contact-required"; return false; }
    for (const auto& anchor : diagnostic.anchors)
        if (anchor.sharedVertices <= 0 || anchor.separation > 0.12f)
        { refusal = "shared-geometry-anchor-not-admitted"; return false; }

    // The authored Fire hulls are too large to use unchanged: the measured
    // settled pose already penetrates terrain by up to 7.5 cm. Inset each actual
    // component about its own centroid, with support checked at EVERY vertex.
    // Pelvis uses the actual animated graphical anatomical membership, excluding
    // proxy helpers, rather than an RTM matrix translation or a guessed joint.
    std::array<std::vector<Vector3>, 11> vertices;
    const WeightInfo& weights = Type()->GetWeights();
    int fire = _shape->FindFireGeometryLevel(); Shape* fireShape = _shape->Level(fire);
    Shape* graphical = _shape->Level(0);
    if (!fireShape || !graphical) { refusal = "stock-fire-and-graphical-geometry-required"; return false; }
    for (int component = 1; component <= 12; ++component)
    {
        char name[24]; std::snprintf(name, sizeof(name), "component%02d", component);
        int selection = fireShape->FindNamedSel(name);
        if (selection < 0) { refusal = "authored-fire-component-missing"; return false; }
        const NamedSelection& selected = fireShape->NamedSel(selection);
        for (int vertex = 0; vertex < selected.Size(); ++vertex)
        {
            int index = selected[vertex]; const auto& weight = weights[fire][index];
            if (weight.Size() != 1 || weight[0].GetWeight() != 1 || weight[0].GetSel() >= 25)
            { refusal = "exclusive-authored-fire-bone-required"; return false; }
            vertices[state->boneParts[weight[0].GetSel()]].push_back(PositionModelToWorld(AnimatePoint(fire,index)));
        }
    }
    for (int vertex = 0; vertex < graphical->NPos(); ++vertex)
    {
        const auto& weight = weights[0][vertex]; bool pelvis = false, proxy = false;
        for (int w = 0; w < weight.Size(); ++w)
            if (weight[w].GetSel() < 25 && state->boneParts[weight[w].GetSel()] == 0) pelvis = true;
        if (!pelvis) continue;
        for (int s = 0; s < graphical->NNamedSel(); ++s)
            if (std::strncmp(graphical->NamedSel(s).Name(), "proxy:", 6) == 0 &&
                graphical->NamedSel(s).IsSelected(vertex)) { proxy = true; break; }
        if (!proxy) vertices[0].push_back(PositionModelToWorld(AnimatePoint(0,vertex)));
    }
    const float masses[] = {12,24,5,3,2,3,2,8,4,8,4};
    state->minimumClearance = 1000;
    for (int part = 0; part < 11; ++part)
    {
        if (vertices[part].size() < 4) { refusal = "anatomical-part-has-no-volume"; return false; }
        Vector3 center = VZero, lo = vertices[part][0], hi = lo;
        for (const auto& point : vertices[part])
        {
            center += point;
            for (int axis = 0; axis < 3; ++axis) { lo[axis] = std::min(lo[axis], point[axis]); hi[axis] = std::max(hi[axis], point[axis]); }
        }
        center *= 1.0f / vertices[part].size();
        if (part == 0)
        {
            vertices[0].clear();
            for (int corner = 0; corner < 8; ++corner)
                vertices[0].emplace_back((corner&1)?hi.X():lo.X(), (corner&2)?hi.Y():lo.Y(), (corner&4)?hi.Z():lo.Z());
            center = (lo+hi)*0.5f;
        }
        state->initial[part] = MIdentity; state->initial[part].SetPosition(center);
        for (auto& point : vertices[part])
        {
            point = (point-center)*0.40f;
            const Vector3 supported = center+point;
            const float clearance = supported.Y()-GLandscape->RoadSurfaceY(supported+VUp*0.5f);
            state->minimumClearance = std::min(state->minimumClearance, clearance);
            if (!point.IsFinite() || !std::isfinite(clearance) || clearance < 0.003f || clearance > 0.8f ||
                (supported-Position()).Size() > 2.5f)
            { refusal = "inset-collider-contact-or-bounds-not-admitted"; return false; }
        }
    }
    // Check the ACTUAL registered solver floor before creating any dynamic part.
    // A different heightfield/road support is refused, never silently corrected.
    for (int part = 0; part < 11; ++part)
    {
        const Vector3 center = state->initial[part].Position();
        for (const auto& point : vertices[part])
        {
            const Vector3 supported = center+point, from = supported+VUp*0.8f, to = supported-VUp;
            Vector3 floor;
            if (!world->RayHitAnything(from,to,Physics::QueryFilter{Physics::ColliderFlags::Roadway}))
            { refusal = "solver-terrain-floor-not-present"; return false; }
            world->CastRay(from,to,floor,Physics::QueryFilter{Physics::ColliderFlags::Roadway});
            if (!floor.IsFinite() || std::abs(floor.Y()-GLandscape->RoadSurfaceY(supported+VUp*0.5f)) > 0.025f)
            { refusal = "solver-floor-disagrees-with-authored-contact"; return false; }
            state->minimumClearance = std::min(state->minimumClearance,supported.Y()-floor.Y());
            if (supported.Y()-floor.Y() < 0.003f)
            { refusal = "inset-collider-penetrates-solver-floor"; return false; }
        }
    }
    for (int part = 0; part < 11; ++part)
    {
        Physics::ConvexPiece piece{vertices[part].data(), static_cast<int>(vertices[part].size()), Physics::ColliderFlags::Solid};
        state->bodies[part] = world->SpawnArticulatedPiece(piece, state->initial[part], masses[part]);
        if (!state->bodies[part].IsValid()) { refusal = "backend-refused-anatomical-hull"; return false; }
    }
    const int pairs[][2] = {{0,1},{1,2},{1,3},{3,4},{1,5},{5,6},{0,7},{7,8},{0,9},{9,10}};
    const int anchors[] = {-1,0,1,3,2,4,5,7,6,8};
    for (int j = 0; j < 10; ++j)
    {
        const int a = pairs[j][0], b = pairs[j][1];
        Vector3 pivot = (state->initial[a].Position()+state->initial[b].Position())*0.5f;
        if (anchors[j] >= 0)
        {
            const auto& anchor = diagnostic.anchors[anchors[j]];
            pivot = (anchor.firstWorld+anchor.secondWorld)*0.5f;
        }
        Vector3 axis = state->initial[b].Position()-pivot;
        if (axis.Size() < 0.025f) { refusal = "joint-geometry-axis-degenerate"; return false; }
        axis.Normalize(); Vector3 up = std::abs(axis*VUp) < 0.95f ? VUp : VAside;
        Matrix4 joint(MDirection, axis, up); joint.SetPosition(pivot);
        Physics::ArticulatedJointDef definition;
        definition.first = state->bodies[a]; definition.second = state->bodies[b];
        definition.firstFrame = state->initial[a].InverseRotation()*joint;
        definition.secondFrame = state->initial[b].InverseRotation()*joint;
        const bool distal = j == 3 || j == 5 || j == 7 || j == 9;
        definition.coneAngle = distal ? 0.30f : 0.9f;
        definition.twistAngle = distal ? 0.20f : 0.6f;
        state->joints[j] = world->AddArticulatedJoint(definition);
        if (!state->joints[j].IsValid()) { refusal = "backend-refused-anatomical-joint"; return false; }
    }
    _corpseArticulation = std::move(state);
    refusal = "";
    if (!UpdateCorpseArticulation()) { refusal = "initial-body-readback-refused"; return false; }
    return true;
}

bool Man::UpdateCorpseArticulation() const
{
    if (!_corpseArticulation || !UseCorpsePose()) return false;
    auto& state = *_corpseArticulation;
    std::array<Matrix4, 11> parts;
    for (int part = 0; part < 11; ++part)
    {
        Physics::BodyMotion motion;
        if (!state.world->GetBodyMotion(state.bodies[part], motion) || !motion.position.IsFinite() ||
            (motion.position-state.initial[part].Position()).Size() > 0.5f)
        { ClearCorpsePose(); return false; }
        state.maximumTravel = std::max(state.maximumTravel,(motion.position-state.initial[part].Position()).Size());
        if (motion.position == state.initial[part].Position() && motion.axisX == VAside &&
            motion.axisY == VUp && motion.axisZ == VForward)
        { parts[part] = MIdentity; continue; }
        Matrix4 current = MIdentity;
        current.SetDirectionAside(motion.axisX); current.SetDirectionUp(motion.axisY); current.SetDirection(motion.axisZ);
        current.SetPosition(motion.position);
        parts[part] = GetInvTransform()*current*state.initial[part].InverseRotation()*state.object;
    }
    std::vector<Matrix4> deltas;
    deltas.reserve(state.boneParts.size());
    for (int part : state.boneParts) deltas.push_back(parts[part]);
    if ((_corpsePosePrimary.Valid() && !_corpsePosePrimary.ApplyRigidDeltas(deltas)) ||
        (_corpsePoseSecondary.Valid() && !_corpsePoseSecondary.ApplyRigidDeltas(deltas)))
    { ClearCorpsePose(); return false; }
    bool firstPoint = true;
    state.radius = 0;
    const auto& weights = Type()->GetWeights();
    for (int level = 0; level < _shape->NLevels(); ++level)
    {
        Shape* shape = _shape->Level(level);
        MATRIX_4_ARRAY(palette,128);
        PrepareCorrectedPose(level,palette);
        _head.EvaluateDeadFaceBones(Type()->_head,weights,level,palette,_isDead);
        if (!_corpseArticulation || palette.Size() != _corpsePoseLease.bones)
        { ClearCorpsePose(); return false; }
        for (int p = 0; p < shape->NPos(); ++p)
        {
            Vector3 point = AnimationRT::ApplyMatricesPoint(weights[level],_shape,level,palette,p);
            if (!point.IsFinite() || point.Size() > 3.5f) { ClearCorpsePose(); return false; }
            if (firstPoint) {state.minimum = state.maximum = point; firstPoint = false;}
            for (int axis = 0; axis < 3; ++axis)
            {state.minimum[axis] = std::min(state.minimum[axis],point[axis]);state.maximum[axis] = std::max(state.maximum[axis],point[axis]);}
            state.radius = std::max(state.radius,point.Size());
        }
    }
    ++state.updates;
    return true;
}

bool Man::CorpseArticulationBounds(Vector3* minMax, float& radius) const
{
    if (!_corpseArticulation || !UseCorpsePose()) return false;
    const Vector3 padding(.05f,.05f,.05f);
    minMax[0] = _corpseArticulation->minimum-padding;
    minMax[1] = _corpseArticulation->maximum+padding;
    radius = _corpseArticulation->radius+.09f;
    return true;
}

RString Man::CorpsePoseProbe(const char* command)
{
    if (!command) return "REFUSED corpse-command missing";
    if (std::strcmp(command, "corpse-physics-status") == 0)
    {
        auto* world = Physics::GetPhysicsWorld();
        if (!world || !world->IsCreated()) return "REFUSED corpse-physics-status no-physics-world";
        const auto stats = world->GetStats(); char status[512];
        std::snprintf(status,sizeof(status),"OK corpse-physics-status epoch=%llu bodies=%u articulatedBodies=%u articulatedJoints=%u",
            static_cast<unsigned long long>(world->Generation()),stats.bodies,stats.articulatedBodies,stats.articulatedJoints);
        return status;
    }
    if (std::strcmp(command, "corpse-prepare") == 0)
    {
        if (!GWorld || GWorld->GetMode() != GModeArcade || !GLandscape)
            return "REFUSED corpse-prepare local-single-player-landscape-required";
        Man* owner = g_corpsePoseOwner.GetLink();
        if (!owner || !owner->UseCorpsePose() || !owner->IsLocal())
            return "REFUSED corpse-prepare local-held-corpse-required";
        auto* world = Physics::GetPhysicsWorld();
        if (!world || !world->IsCreated())
        {
            // The corpus subdivides the entire object grid twelve times. Native
            // maps can then request gigabytes from the engine's allocator, which
            // rejects a single allocation >=256 MiB. This bounded prototype only
            // needs ground around its captured owner (travel is capped at .5 m).
            // Sample the real rendered surface, never substitute a flat floor.
            constexpr int grid = 129;
            constexpr float spacing = .25f;
            constexpr float halfExtent = (grid-1)*spacing*.5f;
            const int range = GLandscape->GetTerrainRange();
            const float sourceSpacing = GLandscape->GetTerrainGrid();
            const float extent = (static_cast<float>(range)-1)*sourceSpacing;
            const Vector3 position = owner->Position();
            if (range < 2 || !std::isfinite(sourceSpacing) || sourceSpacing <= 0 ||
                !std::isfinite(extent) || extent < halfExtent*2 || extent > 1000000 ||
                !std::isfinite(position.X()) || !std::isfinite(position.Y()) || !std::isfinite(position.Z()) ||
                position.X() < 0 || position.Z() < 0 || position.X() > extent || position.Z() > extent)
                return "REFUSED corpse-prepare terrain-bounds-invalid";
            const float originX = std::clamp(position.X(),halfExtent,extent-halfExtent)-halfExtent;
            const float originZ = std::clamp(position.Z(),halfExtent,extent-halfExtent)-halfExtent;
            std::array<float,grid*grid> heights{};
            for (int z=0; z<grid; ++z)
                for (int x=0; x<grid; ++x)
                {
                    const float y = GLandscape->SurfaceY(originX+x*spacing,originZ+z*spacing);
                    if (!std::isfinite(y)) return "REFUSED corpse-prepare terrain-height-invalid";
                    heights[z*grid+x] = y;
                }
            world = &Physics::EnsurePhysicsWorld();
            if (!world->Create()) return "REFUSED corpse-prepare physics-world-unavailable";
            if (!world->SetTerrain(heights.data(),grid,grid,spacing,originX,originZ))
            {
                world->Destroy();
                return "REFUSED corpse-prepare physics-terrain-unavailable";
            }
            LOG_INFO(World,"CORPSEPREPARE: bounded real terrain {}x{} spacing={} origin=({},{}); source {}x{} spacing={} objectGrid={} objectSpacing={}",
                grid,grid,spacing,originX,originZ,range,range,sourceSpacing,
                GLandscape->GetLandRange(),GLandscape->GetLandGrid());
        }
        if (!world || !world->IsCreated() || !world->GetStats().terrainRegistered)
            return "REFUSED corpse-prepare physics-terrain-unavailable";
        const auto stats = world->GetStats(); char status[512];
        std::snprintf(status,sizeof(status),"OK corpse-prepare solver=%s epoch=%llu terrain=%ux%u spacing=%.9g bodies=%u models=%u",
            world->BackendName(),static_cast<unsigned long long>(world->Generation()),stats.terrainWidth,
            stats.terrainHeight,stats.terrainCellSize,stats.bodies,stats.modelsRegistered);
        return status;
    }
    if (std::strcmp(command, "corpse-articulation-status") == 0)
    {
        Man* man = g_corpsePoseOwner.GetLink();
        if (!man || !man->UseCorpsePose() || !man->_corpseArticulation)
            return "REFUSED corpse-articulation-status no-articulated-corpse";
        auto& state = *man->_corpseArticulation;
        char status[512];
        std::snprintf(status,sizeof(status),"OK corpse-articulation-status entity=%p bodies=11 joints=10 epoch=%llu updates=%d maximumTravel=%.9g minimumClearance=%.9g",
            static_cast<void*>(man),static_cast<unsigned long long>(state.epoch),state.updates,state.maximumTravel,state.minimumClearance);
        return status;
    }
    if (std::strcmp(command, "corpse-articulate") == 0 || std::strcmp(command, "corpse-impulse") == 0)
    {
        Man* man = g_corpsePoseOwner.GetLink();
        if (!man || !man->UseCorpsePose()) return "REFUSED corpse-articulate captured-authored-corpse-required";
        if (std::strcmp(command, "corpse-impulse") == 0)
        {
            if (!man->_corpseArticulation) return "REFUSED corpse-impulse no-articulated-corpse";
            auto& state = *man->_corpseArticulation;
            if (!state.Ready()) return "REFUSED corpse-impulse physics-world-retired";
            state.world->ApplyImpulse(state.bodies[1], state.initial[1].Position(), Vector3(0,12,0));
            return "OK corpse-impulse chest=1 impulseY=12";
        }
        RString reason;
        if (!man->StartCorpseArticulation(reason)) return RString("REFUSED corpse-articulate ")+reason;
        char result[512];
        std::snprintf(result, sizeof(result),
            "OK corpse-articulate entity=%p solver=%s bodies=11 joints=10 epoch=%llu minimumClearance=%.9g expiresMs=30000 default=off",
            static_cast<void*>(man), man->_corpseArticulation->world->BackendName(),
            static_cast<unsigned long long>(man->_corpseArticulation->epoch), man->_corpseArticulation->minimumClearance);
        return result;
    }
    if (std::strcmp(command, "corpse-clear") == 0)
    {
        bool held = g_corpsePoseOwner && g_corpsePoseOwner->_corpsePoseLease.world;
        if (g_corpsePoseOwner) g_corpsePoseOwner->ClearCorpsePose();
        g_corpsePoseOwner = nullptr;
        return held ? "OK corpse-clear cleared=1" : "OK corpse-clear cleared=0";
    }
    if (std::strcmp(command, "corpse-status") == 0 || std::strcmp(command, "corpse-compare") == 0)
    {
        Man* man = g_corpsePoseOwner.GetLink();
        if (!man || !man->UseCorpsePose()) return "REFUSED corpse-status no-valid-hold";
        if (std::strcmp(command, "corpse-compare") == 0) return man->CompareCorpsePose();
        char result[768];
        std::snprintf(result, sizeof(result),
            "OK corpse-status entity=%p model=%s bones=%d primaryMatrices=%d secondaryMatrices=%d remainingMs=%d",
            static_cast<void*>(man), man->_shape->Name(), man->_corpsePoseLease.bones,
            man->_corpsePosePrimary.PhaseMatrices(), man->_corpsePoseSecondary.PhaseMatrices(),
            30000 - (Glob.time.toInt() - man->_corpsePoseLease.capturedMs));
        return result;
    }
    if (std::strcmp(command, "corpse-capture") != 0) return "REFUSED corpse-command unknown";
    if (!GWorld || GWorld->GetMode() != GModeArcade || !GScene || !GScene->GetCamera())
        return "REFUSED corpse-capture local-single-player-camera-required";
    Man* nearest = nullptr;
    float nearestDistance = 50 * 50;
    int corpses = 0, eligible = 0;
    const char* lastRefusal = "no-nearby-corpse";
    Vector3 eye = GScene->GetCamera()->Position();
    for (int i = 0; i < GWorld->NVehicles(); ++i)
    {
        Man* man = dyn_cast<Man>(GWorld->GetVehicle(i));
        if (!man || !man->_isDead || (man->Position() - eye).SquareSize() > 50 * 50) continue;
        ++corpses;
        if (const char* reason = man->CorpsePoseRefusal()) { lastRefusal = reason; continue; }
        ++eligible;
        float distance = (man->Position() - eye).SquareSize();
        if (distance < nearestDistance) { nearest = man; nearestDistance = distance; }
    }
    char result[768];
    RString captureRefusal;
    if (!nearest || !nearest->CaptureCorpsePose(captureRefusal))
    {
        std::snprintf(result, sizeof(result), "REFUSED corpse-capture entity=%p nearbyCorpses=%d eligible=%d reason=%s",
                      static_cast<void*>(nearest), corpses, eligible, nearest ? static_cast<const char*>(captureRefusal) : lastRefusal);
        return result;
    }
    std::snprintf(result, sizeof(result),
        "OK corpse-capture entity=%p model=%s distance=%.3f nearbyCorpses=%d eligible=%d bones=%d "
        "primaryMatrices=%d secondaryMatrices=%d expiresMs=30000 solver=none rootMotion=authored",
        static_cast<void*>(nearest), nearest->_shape->Name(), std::sqrt(nearestDistance), corpses, eligible,
        nearest->_corpsePoseLease.bones, nearest->_corpsePosePrimary.PhaseMatrices(), nearest->_corpsePoseSecondary.PhaseMatrices());
    return result;
}

int Man::InsideLOD(CameraType camType) const
{
    return Type()->_insideView;
}

void Man::BlendMatrix(Matrix4& mat, const Matrix4& trans, float factor) const
{
    if (factor >= 0.99f)
    {
        mat = trans * mat;
    }
    else if (factor >= 0.01f)
    {
        // interpolation necessary
        mat = (trans * factor + MIdentity * (1 - factor)) * mat;
    }
}

void Man::AnimateMatrix(Matrix4& mat, const AnimationRTWeight& wgt) const
{
    bool held = UseCorpsePose();
    if (held && ((_corpsePosePrimary.Valid() && !_corpsePosePrimary.SupportsMatrix(wgt)) ||
                 (_corpsePoseSecondary.Valid() && !_corpsePoseSecondary.SupportsMatrix(wgt))))
    {
        ClearCorpsePose();
        held = false;
    }
    const ManType* type = Type();
    Matrix4 res = MZero;
    if (_primaryFactor > 0.01f)
    {
        res = mat;
        AnimationRT* anim = type->GetAnimation(_primaryMove.id);
        if (anim)
        {
            if (held) _corpsePosePrimary.Matrix(res, wgt);
            else anim->Matrix(res, _primaryTime, wgt);
        }
        res = res * _primaryFactor;
    }
    if (_primaryFactor < 0.99f)
    {
        AnimationRT* anim = type->GetAnimation(_secondaryMove.id);
        Matrix4 temp = mat;
        if (anim)
        {
            if (held) _corpsePoseSecondary.Matrix(temp, wgt);
            else anim->Matrix(temp, _secondaryTime, wgt);
        }
        res += temp * (1 - _primaryFactor);
    }
    res.Orthogonalize();
    mat = res;
}

const AnimationRTWeight& Man::GetSelWeights(int level, int selection) const
{
    const ManType* type = Type();
    const WeightInfo& weights = type->GetWeights();
    const AnimationRTWeights& wgt = weights[level];

    // check which matrix
    Shape* sShape = _shape->Level(level);
    const NamedSelection& sel = sShape->NamedSel(selection);
    int point = sel[0];
    PoseidonAssert(sel.Size() > 0);
    return wgt[point];
}

const AnimationRTWeight& Man::GetProxyWeights(int level, const ProxyObject& proxy) const
{
    return GetSelWeights(level, proxy.selection);
}

void Man::AnimateMatrix(Matrix4& mat, int level, int selection) const
{
    const AnimationRTWeight& wg = GetSelWeights(level, selection);
    AnimateMatrix(mat, wg);
    int matIndex = wg[0].GetSel();

    if (!_gunTransIdent)
    {
        BLEND_ANIM(aiming);
        const BlendAnimSelections& aimingRes = GetAiming(aiming);

        float aimingFactor = 0;
        for (int bi = 0; bi < aimingRes.Size(); bi++)
        {
            const BlendAnimInfo& blend = aimingRes[bi];
            if (matIndex == blend.matrixIndex)
            {
                aimingFactor = blend.factor;
                break;
            }
        }

        BlendMatrix(mat, _gunTrans, aimingFactor);
    }

    if (!_headTransIdent)
    {
        BLEND_ANIM(head);
        const BlendAnimSelections& headRes = GetHead(head);

        float headFactor = 0;
        for (int bi = 0; bi < headRes.Size(); bi++)
        {
            const BlendAnimInfo& blend = headRes[bi];
            if (matIndex == blend.matrixIndex)
            {
                headFactor = blend.factor;
                break;
            }
        }

        BlendMatrix(mat, _headTrans, headFactor);
    }
}

Vector3 Man::COMPosition() const
{
    return AimingPosition();
}

Vector3 Man::AnimatePoint(int level, int index) const
{
    bool held = UseCorpsePose();
    const ManType* type = Type();
    Vector3 res = VZero;
    const WeightInfo& rtw = type->GetWeights();
    if (held && index >= 0 &&
        ((_corpsePosePrimary.Valid() && !_corpsePosePrimary.SupportsPoint(rtw[level][index])) ||
         (_corpsePoseSecondary.Valid() && !_corpsePoseSecondary.SupportsPoint(rtw[level][index]))))
    {
        ClearCorpsePose();
        held = false;
    }
    auto point = [&](const AffinePhasePose& pose, AnimationRT* anim, float time)
    {
        if (!held) return anim->Point(rtw, _shape, level, time, index);
        if (index < 0) return Vector3(VZero);
        Shape* shape = _shape->LevelOpaque(level);
        if (rtw[level][index].Size() <= 0) return Vector3(shape->Pos(index));
        shape->SaveOriginalPos();
        return pose.Point(rtw[level][index], shape->OrigPos(index), shape->Pos(index));
    };
    if (_primaryFactor > 0.01f)
    {
        AnimationRT* anim = type->GetAnimation(_primaryMove.id);
        if (anim)
        {
            res = point(_corpsePosePrimary, anim, _primaryTime) * _primaryFactor;
        }
    }
    if (_primaryFactor < 0.99f)
    {
        AnimationRT* anim = type->GetAnimation(_secondaryMove.id);
        if (anim)
        {
            res += point(_corpsePoseSecondary, anim, _secondaryTime) * (1 - _primaryFactor);
        }
    }

    if (type->_aimingAxisPoint >= 0 && !_gunTransIdent)
    {
        BLEND_ANIM(aiming);
        const BlendAnimSelections& aimingRes = GetAiming(aiming);
        AnimationRT::TransformPoint(res, rtw[level], _shape->Level(level), _gunTrans, aimingRes.Data(),
                                    aimingRes.Size(), index);
    }

    BLEND_ANIM(legs);
    const BlendAnimSelections& legsRes = GetLegs(legs);
    AnimationRT::TransformPoint(res, rtw[level], _shape->Level(level), _legTrans, legsRes.Data(), legsRes.Size(),
                                index);

    return res;
}

bool Man::HasFlares(CameraType camType) const
{
    if (camType == CamGunner && _currentWeapon >= 0 && _currentWeapon < NMagazineSlots())
    {
        const MagazineSlot& slot = GetMagazineSlot(_currentWeapon);
        if (slot._muzzle)
        {
            return slot._muzzle->_opticsFlare;
        }
    }
    return base::HasFlares(camType);
}

Matrix4 Man::InsideCamera(CameraType camType) const
{
    int level = _shape->FindMemoryLevel();

    if (camType == CamGunner)
    {
        return GetWeaponRelTransform(_currentWeapon);
    }

    int selIndex = Type()->_pilotPoint;
    if (selIndex < 0)
    {
        return MIdentity;
    }

    const Selection& sel = _shape->LevelOpaque(level)->NamedSel(selIndex);
    Vector3Val pilotPos = AnimatePoint(level, sel[0]);
    Matrix4 transform;

    Vector3 relUp = WorldToModel().DirectionUp();

    transform.SetDirectionAndUp(VForward, relUp);
    if (const auto* seat = GetActiveCargoWeaponSeat())
    {
        const float azimuth = seat->azimuth * (H_PI / 180.0f);
        transform.SetDirectionAndUp(Vector3(sin(azimuth), 0, cos(azimuth)),
                                    WorldTransform().InverseRotation().Rotate(VUp));
    }
    transform.SetPosition(pilotPos);
    // calculate recoil camera angle
    if (_recoil)
    {
        _recoil->ApplyRecoil(_recoilTime, transform, 0.2f * _recoilFactor);
        // normalize transform
        transform.Orthogonalize();
    }
    return transform;
}

Vector3 Man::GetCameraDirection(CameraType cam) const
{
    if (const auto* seat = GetActiveCargoWeaponSeat())
    {
        const float azimuth = seat->azimuth * (H_PI / 180.0f);
        return WorldTransform().Rotate(Vector3(sin(azimuth), 0, cos(azimuth)));
    }
    return Direction();
}

Vector3 Man::ExternalCameraPosition(CameraType camType) const
{
    return Type()->_extCameraPosition;
}

Vector3 Man::GetSpeakerPosition() const
{
    return CameraPosition();
}

bool Man::IsVirtual(CameraType camType) const
{
    return true;
}
bool Man::IsVirtualX(CameraType camType) const
{
    if (GetConfiguredCargoWeaponSeat() && camType != CamGunner)
        return true; // A seated soldier cannot turn the carrier to follow the mouse.
    auto& input = InputSubsystem::Instance();
    if (input.IsLookAroundEnabled())
    {
        return true;
    }
    return camType != CamInternal && camType != CamExternal;
}
bool Man::IsGunner(CameraType camType) const
{
    return camType == CamGunner || camType == CamInternal || camType == CamExternal;
}

void Man::OverrideCursor(CameraType camType, Vector3& dir) const {}

void Man::LimitCursor(CameraType camType, Vector3& dir) const
{
    if (!QIsManual())
    {
        return;
    }
    if (GetConfiguredCargoWeaponSeat())
    {
        // AimWeapon clamps the rifle, not the view. Looking outside the seat arc
        // lowers the rifle rather than dragging the player's camera sideways.
        return;
    }
    if (GetCursorRelMode(camType) != CKeyboard)
    {
        return;
    }

    switch (camType)
    {
        case CamInternal:
        case CamExternal:
        case CamGunner:
            break;
        default:
            return;
    }

    auto& input = InputSubsystem::Instance();
    if (!input.IsLookAroundEnabled())
    {
        float xRot = _gunXRotWanted;
        float yRot = _gunYRotWanted;
        saturate(yRot, Type()->_minGunTurn, Type()->_maxGunTurn);
        saturate(xRot, Type()->_minGunElev, Type()->_maxGunElev);
        Vector3 relDir = (Matrix3(MRotationY, yRot) * Matrix3(MRotationX, -xRot).Direction());
        DirectionModelToWorld(dir, relDir);
    }
}

bool Man::IsCommander(CameraType camType) const
{
    return true;
}
bool Man::ShowAim(int weapon, CameraType camType) const
{
    if (WeaponsDisabled())
    {
        return false;
    }
    if (_laserTargetOn)
    {
        return true;
    }
    if (!ShowWeaponAim())
    {
        return false;
    }
    return camType != CamGunner;
}
bool Man::ShowCursor(int weapon, CameraType camType) const
{
#if _ENABLE_CHEATS
    if (CHECK_DIAG(DECombat))
        return true;
#endif
    return camType != CamGunner;
}

void Man::InitVirtual(CameraType camType, float& heading, float& dive, float& fov) const
{
    if (camType == CamExternal)
    {
        camType = CamInternal;
    }
    base::InitVirtual(camType, heading, dive, fov);
    switch (camType)
    {
        case CamGunner:
            fov = 0.21f;
            dive = 0;
            break;
    }
}

void Man::LimitVirtual(CameraType camType, float& heading, float& dive, float& fov) const
{
    if (camType == CamExternal)
    {
        camType = CamInternal;
    }
    base::LimitVirtual(camType, heading, dive, fov);
    switch (camType)
    {
        case CamGunner:
        {
            int curWeapon = SelectedWeapon();
            if (curWeapon >= 0 && curWeapon < NMagazineSlots())
            {
                const MagazineSlot& slot = GetMagazineSlot(curWeapon);
                const MuzzleType* muzzle = slot._muzzle;
                saturate(fov, muzzle->_opticsZoomMin, muzzle->_opticsZoomMax);
            }
            else
            {
                saturate(fov, 0.21f, 0.21f);
            }
            saturate(heading, 0, 0);
            saturate(dive, -0.7f, +0.8f);
        }
        break;
        case CamInternal:
            break;
    }
}

AnimationRT* MovesType::GetAnimation(MoveId move) const
{
    if (move == MoveIdNone)
    {
        return nullptr;
    }
    return _moves[move];
}

ActionMap* MovesType::GetActionMap(MoveId move) const
{
    if (move == MoveIdNone)
    {
        return nullptr;
    }
    return _moves[move].GetActionMap();
}

const MoveInfo* MovesType::GetMoveInfo(MoveId move) const
{
    if (move == MoveIdNone)
    {
        return nullptr;
    }
    return &_moves[move];
}

MoveId MovesType::GetEquivalent(MoveId move) const
{
    const MoveInfo* info = GetMoveInfo(move);
    if (!info)
    {
        return MoveIdNone;
    }
    MoveId id = info->GetEquivalentTo();
    if (id == MoveIdNone)
    {
        return move;
    }
    return id;
}

bool Man::IsDead() const
{
    return GetActUpDegree() == ManPosDead;
}

bool Man::ShadowPoseFrozen() const
{
    if (_corpseArticulation) return false;
    // Mirror the steady-dead-body test the simulation uses to suspend a
    // corpse: dead pose reached, killed and motionless for 5 s. Any later
    // physics push refreshes _lastMovementTime and un-freezes the shadow.
    return IsDead() && _whenKilled != Foundation::Time(0) && _whenKilled < Glob.time - 5 &&
           _lastMovementTime < Glob.time - 5;
}

void Man::SetPrimaryMove(MotionPathItem item)
{
    int oldUD = GetActUpDegree();
    _primaryMove = item;

    int newUD = GetActUpDegree();
    if (oldUD != newUD)
    {
        _upDegreeChangeTime = Glob.time;
    }
}

#define DIAG_QUEUE 0

void Man::RefreshMoveQueue(bool enableVariants)
{ // select random variants
    if (Glob.time >= _variantTime)
    {
        const MoveInfo* stillInfo = Type()->GetMoveInfo(_stillMoveQueueEnd);
        // we may select another move instead of this one
        // playing move long enough - select variant
        if (stillInfo)
        {
            MoveId id = stillInfo->GetEquivalentTo();
            if (enableVariants)
            {
                id = QIsManual() ? stillInfo->RandomVariantPlayer() : stillInfo->RandomVariantAI();
            }
            const MoveInfo* newInfo = Type()->GetMoveInfo(id);
            if (newInfo)
            {
                float wait = newInfo->GetVariantAfter();
                _variantTime = Glob.time + wait;
                MotionPathItem item(id);
#if DIAG_QUEUE
                LOG_DEBUG(Physics, "{}: Switch to variant {} of {} for {:.2f}", DNAME(), NAME_T(Type(), id),
                          NAME_T(Type(), _stillMoveQueueEnd), _variantTime - Glob.time);
#endif
                ChangeMoveQueue(item);
                return;
            }
        }
        _variantTime = Glob.time + 600;
    }
}

bool Man::ChangeMoveQueue(MotionPathItem item)
{
#if DIAG_QUEUE
    LOG_DEBUG(Physics, "ChangeMoveQueue {}", NAME_T(Type(), item.id));
#endif

    if (item.id == _primaryMove.id)
    {
#if DIAG_QUEUE
        LOG_DEBUG(Physics, "{}: Reset queue {}", DNAME(), NAME_T(Type(), item.id));
#endif
        // we are in target state
        if (item.context)
        {
            _primaryMove = item;
        }

        if (_queueMove.Size() > 0)
        {
            _queueMove.Resize(0);
            OnEvent(EEAnimChanged, GetCurrentMove());
        }
        return true;
    }

    const MoveInfo* info = Type()->GetMoveInfo(_primaryMove.id);
    if (info && info->GetTerminal())
    {
        // we are in terminal state - nowhere to change
        return false;
    }

    if (item.id == _secondaryMove.id && _primaryFactor < 1 && !_primaryMove.context)
    {
        const MotionEdge& edge = Type()->Edge(_secondaryMove.id, _primaryMove.id);
        if ((MotionEdgeType)edge.type == MEdgeInterpol)
        {
            swap(_secondaryMove, _primaryMove);
            swap(_secondaryTime, _primaryTime);
            _primaryFactor = 1 - _primaryFactor;
            _queueMove.Resize(0);

            if (item.context)
            {
                _primaryMove.context = item.context;
            }
            OnEvent(EEAnimChanged, GetCurrentMove());
            return true;
        }
    }

    int qSize = _queueMove.Size();
    if (qSize > 0 && _queueMove[qSize - 1].id == item.id)
    {
#if DIAG_QUEUE
        LOG_DEBUG(Physics, "{}: Queue already valid - {}", DNAME(), NAME_T(Type(), item.id));
#endif
        // current path is valid
        return true;
    }

#if DIAG_QUEUE
    LOG_DEBUG(Physics, "{}: Set queue {}", DNAME(), NAME_T(Type(), item.id));
#endif

    // find shortest path from current primary move to target move
    if (_primaryMove.id == MoveIdNone)
    {
        return false;
    }

    bool ret = Type()->FindPath(_queueMove, _primaryMove.id, item);
    if (ret)
    {
        OnEvent(EEAnimChanged, GetCurrentMove());
    }
    return ret;
}

bool Man::SetMoveQueue(MotionPathItem item, bool enableVariants)
{
#if DIAG_QUEUE >= 2
    LOG_DEBUG(Physics, "SetMoveQueue {}", NAME_T(Type(), item.id));
#endif

    if (item.id == MoveIdNone)
    {
        if (item.context)
        {
            ProcessMoveFunction(item.context);
        }

        _queueMove.Resize(0);
        return false;
    }
    MoveId equivItemId = Type()->_moveType->GetEquivalent(item.id);
    MoveId equivStillId = Type()->_moveType->GetEquivalent(_stillMoveQueueEnd);
    if (equivItemId != equivStillId)
    {
        _stillMoveQueueEnd = item.id;
        const MoveInfo* info = Type()->GetMoveInfo(item.id);
        float wait = info->GetVariantAfter();
        _variantTime = Glob.time + wait;

#if DIAG_QUEUE
        LOG_DEBUG(Physics, "{}: Eq move changed - {} (eq {})", DNAME(), NAME_T(Type(), item.id),
                  NAME_T(Type(), equivItemId));
#endif
    }
    else
    {
#if DIAG_QUEUE >= 2
        LOG_DEBUG(Physics, "{}: Same eq move changed - {} (eq {})", DNAME(), NAME_T(Type(), item.id),
                  NAME_T(Type(), _stillMoveQueueEnd));
#endif
    }
    if (Glob.time >= _variantTime)
    {
        const MoveInfo* stillInfo = Type()->GetMoveInfo(_stillMoveQueueEnd);
        MoveId id = stillInfo->GetEquivalentTo();
        if (enableVariants)
        {
            id = QIsManual() ? stillInfo->RandomVariantPlayer() : stillInfo->RandomVariantAI();
        }
        if (id != MoveIdNone)
        {
            const MoveInfo* newInfo = Type()->GetMoveInfo(id);

            float wait = newInfo->GetVariantAfter();
            _variantTime = Glob.time + wait;
            item.id = id;
#if DIAG_QUEUE
            LOG_DEBUG(Physics, "{}: Switch to variant {} of {} for {:.2f}", DNAME(), NAME_T(Type(), id),
                      NAME_T(Type(), equivItemId), _variantTime - Glob.time);
#endif
        }
        else
        {
            _variantTime = Glob.time + 600;
        }
    }
    else
    {
        if (equivItemId == equivStillId)
        {
#if DIAG_QUEUE >= 2
            LOG_DEBUG(Physics, "  equivalent state {} and {}", NAME_T(Type(), item.id),
                      NAME_T(Type(), _stillMoveQueueEnd));
#endif
            if (item.context)
            {
                if (_queueMove.Size() > 0)
                {
                    MotionPathItem& lastItem = _queueMove[_queueMove.Size() - 1];
                    lastItem.context = item.context;
                }
                else
                {
                    ChangeMoveQueue(item);
                    return true;
                }
            }
            return true;
        }
    }
    return ChangeMoveQueue(item);
}

void Man::NextExternalQueue()
{
    if (_externalQueue.Size() > 0)
    {
        _externalMove = _externalQueue[0];
        _externalMoveFinished = false;
        _externalQueue.Delete(0);
    }
}

void Man::AdvanceExternalQueue()
{
    if (_externalMoveFinished || _externalMove.id == MoveIdNone)
    {
        NextExternalQueue();
    }
}

static Object* DirectHitCheck(Man* man, Vector3Par beg, Vector3& hit, const AmmoType* ammo)
{
    CollisionBuffer retVal;
    GLandscape->ObjectCollision(retVal, man, man, beg, hit, 0);
    // seek first hit
    if (retVal.Size() <= 0)
    {
        return nullptr;
    }
    int minI = -1;
    float minT = 1e10;
    for (int i = 0; i < retVal.Size(); i++)
    {
        if (minT > retVal[i].under)
        {
            minI = i;
            minT = retVal[i].under;
        }
    }
    const CollisionInfo& info = retVal[minI];
    hit = info.object->PositionModelToTop(info.pos);
    return info.object;
}

void Man::ThrowGrenadeAction(int weapon)
{
    if (GetNetworkManager().IsControlsPaused())
    {
        return;
    }
    TargetType* target = nullptr;
    const WeaponModeType* mode = GetWeaponMode(weapon);

    if (!mode)
    {
        return;
    }
    const AmmoType* ammo = mode->_ammo;
    if (!ammo)
    {
        return;
    }
    if (ammo->_simulation == AmmoShotStroke)
    {
        float distance = ammo->indirectHitRange * 5 + 0.1f;
        Vector3Val begPos = AimingPosition();
        Vector3 hitPos = begPos + Direction() * distance;
        Object* directHit = DirectHitCheck(this, begPos, hitPos, ammo);
        GLandscape->ExplosionDammage(this, nullptr, directHit, hitPos, Direction(), ammo);
        return;
    }
    // Animation delay can outlive the clearance checked when the throw began.
    // Recheck before creating a shell or consuming ammunition, never afterwards.
    if (ImprovedAutonomousCombat() &&
        (!GetAIFireEnabled(GetFireTarget()) || !CombatShotSafe(weapon, GetFireTarget())))
        return;
    bool fired = FireShell(weapon, GetWeaponPoint(weapon), GetWeaponRelDirection(weapon), target);
    if (fired)
    {
        EntityAI::FireWeapon(weapon, target);
        if (_forceFireWeapon == weapon)
        {
            _forceFireWeapon = -1;
        }
    }
}

} // namespace Poseidon
