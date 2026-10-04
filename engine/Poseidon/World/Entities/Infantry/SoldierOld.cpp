#include <Poseidon/World/Entities/Infantry/SoldierOldCommon.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/Physics/CorpseArticulation.hpp>
#include <Poseidon/World/Simulation/Animation/StockCorpseRig.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Poseidon/World/Weather/RainWaterField.hpp>
#include <array>
#include <chrono>
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
    if (_automaticCorpsePose && UseCorpsePose()) return ManPosDead;
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
Physics::PhysicsWorld* g_corpsePreparedWorld = nullptr;
std::uint64_t g_corpsePreparedEpoch = 0, g_corpsePreparedTerrainSerial = 0;
float g_corpsePreparedOriginX = 0, g_corpsePreparedOriginZ = 0;
std::vector<OLink<Man>> g_automaticCorpses;
int g_automaticInfantryDeathsOverride = -1;

int StockRigidProxyBone(const AnimationRTWeight& weight, Skeleton* skeleton)
{
    if (!skeleton || skeleton->NBones()!=33) return -1;
    return StockCorpseRigidProxyBone(weight.Size(),
        [&](int w) { return weight[w].GetSel(); },
        [&](int w) { return weight[w].GetWeight(); },
        [&](int bone) { return StockCorpseProxyPart(static_cast<const char*>(skeleton->GetBone(bone))); });
}

bool StockCorpseCapability(LODShape* shape, Skeleton* skeleton, RString& refusal)
{
    if (!shape) { refusal = "stock-corpse-model-unavailable"; return false; }
    if (!StockCorpseModelKnown(shape->Name()))
    { refusal = "original-stock-corpse-model-required"; return false; }
    if (!skeleton) { refusal = "stock-corpse-skeleton-unavailable"; return false; }
    const char* rigRefusal = StockCorpseSkeletonRefusal(skeleton->NBones(),
        [&](int bone, std::string_view expected) { return std::strcmp(skeleton->GetBone(bone), expected.data()) == 0; });
    if (rigRefusal) { refusal = rigRefusal; return false; }
    if (shape->NLevels() <= 0 || shape->NLevels() > CorrectedCorpsePose::MaximumLevels)
    { refusal = "bounded-stock-lod-count-required"; return false; }
    const int roles[] = {0, shape->FindMemoryLevel(), shape->FindGeometryLevel(),
        shape->FindFireGeometryLevel(), shape->FindViewGeometryLevel(), shape->FindLandContactLevel()};
    for (int role : roles)
        if (role < 0 || role >= shape->NLevels())
        { refusal = "stock-special-lod-missing"; return false; }
    for (int level = 0; level < shape->NLevels(); ++level)
        if (!shape->Level(level) || shape->Level(level)->NPos() <= 0 ||
            shape->Level(level)->NPos() > CorrectedCorpsePose::MaximumPoints)
        { refusal = "bounded-stock-points-required"; return false; }
    return true;
}

bool AutomaticRagdollEnabled()
{
    if (g_automaticInfantryDeathsOverride >= 0) return g_automaticInfantryDeathsOverride != 0;
    const char* enabled = std::getenv("POSEIDON_AUTOMATIC_RAGDOLL");
    return !enabled || std::strcmp(enabled,"0") != 0;
}

bool AutomaticRagdollTerrain(RString& refusal)
{
    const auto began = std::chrono::steady_clock::now();
    if (!GLandscape) return false;
    auto& world = Physics::EnsurePhysicsWorld();
    if (!world.Create()) { refusal = "physics-world-unavailable"; return false; }
    if (world.GetStats().terrainRegistered) return true;
    // Elevation belongs to the terrain grid, which the renderer may subdivide
    // independently of object/texture LAND space. Downsampling land corners
    // removes real terrain ridges, and is not an admissible ragdoll floor.
    const int range = GLandscape->GetTerrainRange();
    const float grid = GLandscape->GetTerrainGrid();
    if (range <= 1 || !std::isfinite(grid) || grid <= 0) return false;
    const size_t width = size_t(range);
    if (width*width > 4u*1024u*1024u) { refusal = "bounded-terrain-allocation-refused"; return false; }
    const float spacing = grid;
    std::vector<float> heights(width*width);
    for (size_t z = 0; z < width; ++z) for (size_t x = 0; x < width; ++x)
    {
        // SurfaceY requires an entire cell and returns outside-map sea at the
        // last row/column. Preserve those real closing vertices explicitly.
        const float y = (x+1 < width && z+1 < width)
            ? GLandscape->SurfaceY(float(x)*spacing,float(z)*spacing)
            : GLandscape->ClippedData(int(z),int(x));
        if (!std::isfinite(y)) { refusal = "nonfinite-map-terrain"; return false; }
        heights[z*width+x] = y;
    }
    if (!world.SetTerrain(heights.data(),int(width),int(width),spacing,0,0))
    { refusal = "physics-terrain-registration-refused"; return false; }
    LOG_INFO(World,"AUTORAGDOLL: registered shared actual terrain width={} spacing={} mapGrid={} landGrid={} bytes={} milliseconds={}",
        width,spacing,grid,GLandscape->GetLandGrid(),heights.size()*sizeof(float),
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count());
    return true;
}
}

bool Man::AutomaticInfantryDeathsEnabled() { return AutomaticRagdollEnabled(); }

void Man::SetAutomaticInfantryDeathsEnabled(bool enabled)
{
    g_automaticInfantryDeathsOverride = enabled ? 1 : 0;
    LOG_INFO(World,"AUTORAGDOLL: runtime new-death admission enabled={} existingOwnersRetained=1",enabled);
    if (enabled) PrepareAutomaticInfantryTerrain();
}

void Man::PrepareAutomaticInfantryTerrain()
{
    if (!AutomaticRagdollEnabled() || !GWorld || !GLandscape || GWorld->GetMode()!=GModeArcade ||
        GLandscape->GetTerrainRange()<=1) return;
    auto* world=Physics::GetPhysicsWorld();
    static const void* attemptedWorld=nullptr;
    static const void* attemptedLandscape=nullptr;
    static std::uint64_t attemptedEpoch=~std::uint64_t(0);
    static std::uint64_t attemptedTerrainSerial=~std::uint64_t(0);
    const std::uint64_t epoch=world?world->Generation():0;
    const std::uint64_t serial=world?world->TerrainMutationSerial():0;
    if (attemptedWorld==GWorld && attemptedLandscape==GLandscape && attemptedEpoch==epoch && attemptedTerrainSerial==serial) return;
    attemptedWorld=GWorld; attemptedLandscape=GLandscape; attemptedEpoch=epoch; attemptedTerrainSerial=serial;
    if (world && world->GetStats().terrainRegistered) return;
    RString refusal; const bool ready=AutomaticRagdollTerrain(refusal);
    world=Physics::GetPhysicsWorld(); attemptedEpoch=world?world->Generation():0;
    attemptedTerrainSerial=world?world->TerrainMutationSerial():0;
    LOG_INFO(World,"AUTORAGDOLL: mission terrain preparation ready={} reason={} beforeDamage=1",ready,refusal.Data());
}

void Man::ClearCorpsePose() const
{
    if (_corpseArticulation && _corpseArticulation->state == CorpseSolverState::Frozen && GScene)
        GScene->GetShadowCache().ShadowChanged(const_cast<Man*>(this));
    _corpseArticulation.reset();
    _automaticCorpsePose.reset();
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
    if (_automaticCorpsePose)
    {
        const bool identity = _corpsePoseLease.world == GWorld && _shape.GetRef() == _corpsePoseModel.GetRef() &&
            Type()->GetWeights().GetName().skeleton.GetTypeRef() == _corpsePoseSkeleton.GetRef() &&
            &Type()->GetWeights() == _corpsePoseWeights.GetRef();
        if (!identity || (!_isDead && !IsDammageDestroyed()) || !IsLocal() || !GWorld ||
            GWorld->GetMode() != GModeArcade || _hideBody != 0 || _hideBodyWanted != 0 ||
            GetHierachyParent() || _ladderBuilding || ToDelete() || ToMoveOut() || !IsInLandscape() ||
            (!_automaticCorpsePose->frozen && !_automaticCorpsePose->pending &&
                (!_corpseArticulation || !_corpseArticulation->Ready())))
        { ClearCorpsePose(); return false; }
        return true;
    }
    if (_corpseArticulation && !_corpseArticulation->Ready()) { ClearCorpsePose(); return false; }
    // Reject removal/world changes before touching type-owned animation tables.
    if (_corpsePoseLease.world != GWorld || CorpsePoseRefusal()) { ClearCorpsePose(); return false; }
    Skeleton* skeleton = Type()->GetWeights().GetName().skeleton.GetTypeRef();
    const bool frozen = _corpseArticulation && _corpseArticulation->state == CorpseSolverState::Frozen;
    const bool leaseMatches = skeleton && (frozen ?
        _corpsePoseLease.MatchesIdentity(GWorld,_shape.GetRef(),skeleton,skeleton->NBones(),
            int(_primaryMove.id),int(_secondaryMove.id),_primaryFactor,Glob.time.toInt()) :
        _corpsePoseLease.Matches(GWorld,_shape.GetRef(),skeleton,skeleton->NBones(),
            int(_primaryMove.id),int(_secondaryMove.id),_primaryFactor,Glob.time.toInt()));
    bool retainedTransform = true;
    if (frozen)
        for (int row=0;row<3;++row) for (int col=0;col<4;++col)
            if (Transform()(row,col) != _corpseArticulation->object(row,col)) retainedTransform = false;
    if (!skeleton || _corpsePoseWeights.GetRef() != &Type()->GetWeights() ||
        !leaseMatches || !retainedTransform ||
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
    if (g_corpsePoseOwner && g_corpsePoseOwner.GetLink() != this && !g_corpsePoseOwner->_automaticCorpsePose)
        g_corpsePoseOwner->ClearCorpsePose();
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

bool Man::InspectCorpsePose(CorpsePoseDiagnostic& result, RString& refusal, bool hingeEndpoints) const
{
    Skeleton* skeleton = Type()->GetWeights().GetName().skeleton.GetTypeRef();
    if (!StockCorpseCapability(_shape, skeleton, refusal)) return false;
    refusal = "finite-correction-or-world-transform-required";
    if (!_headTrans.IsFinite() || !_gunTrans.IsFinite() || !_legTrans.IsFinite() || !Transform().IsFinite()) return false;
    if (!GLandscape || !skeleton || skeleton->NBones() > 128 || _shape->NLevels() > MAX_LOD_LEVELS) return false;
    CorpsePoseDiagnostic candidate;
    candidate.articulated = _corpseArticulation && _corpseArticulation->state == CorpseSolverState::Simulating;
    candidate.frozen = _corpseArticulation && _corpseArticulation->state == CorpseSolverState::Frozen;
    candidate.object = Transform();
    if (_corpseArticulation)
    {
        candidate.retainedBounds = true;
        candidate.retainedMinimum = _corpseArticulation->minimum;
        candidate.retainedMaximum = _corpseArticulation->maximum;
        candidate.retainedRadius = _corpseArticulation->radius;
    }
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
        measured.faceEvaluated = _automaticCorpsePose ? true :
            _head.EvaluateDeadFaceBones(Type()->_head, weights, level, palette, _isDead);
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
            measured.palettePoints.push_back(projected);
            if (p == 0) measured.paletteMin = measured.paletteMax = projected;
            for (int axis = 0; axis < 3; ++axis)
            {
                measured.paletteMin[axis] = std::min(measured.paletteMin[axis], projected[axis]);
                measured.paletteMax[axis] = std::max(measured.paletteMax[axis], projected[axis]);
            }
            if (weight.Size() <= 0) ++measured.unweighted;
            bool automaticPaletteOnly = false;
            if (_automaticCorpsePose) for (int w = 0; w < weight.Size(); ++w)
                if (weight[w].GetSel() >= 25) automaticPaletteOnly = true;
            if (automaticPaletteOnly || (_corpsePosePrimary.Valid() && !_corpsePosePrimary.SupportsPoint(weight)) ||
                (_corpsePoseSecondary.Valid() && !_corpsePoseSecondary.SupportsPoint(weight)))
            { ++measured.skippedPointTails; continue; }
            const Vector3 actual = AnimatePoint(level, p);
            if (!actual.IsFinite()) { refusal = "nonfinite-special-point"; return false; }
            measured.consumerPoints.push_back(actual);
            measured.consumerPointIndices.push_back(p);
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
            const auto& proxyWeights=GetProxyWeights(level, proxy);
            if (proxyWeights.Size() <= 0) continue;
            if (_automaticCorpsePose && StockRigidProxyBone(proxyWeights,skeleton)<0)
            { refusal="single-part-inspected-equipment-proxy-required"; return false; }
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
        {"pstehno", "pholen"}, {"lstehno", "lholen"},
        {"ploket", "pruka"}, {"lloket", "lruka"}, {"pholen", "pchodidlo"}, {"lholen", "lchodidlo"},
        {"pchodidlo", "pprsty"}, {"lchodidlo", "lprsty"}};
    graphical->SaveOriginalPos();
    auto fireBoundary = [&](CorpseAnchorDiagnostic& anchor) {
        if (hingeEndpoints || !StockCorpseModelNameEqual(_shape->Name(),"data3d\\angelina.p3d")) return false;
        const int firstPart=StockCorpseAnatomicalPart(anchor.first), secondPart=StockCorpseAnatomicalPart(anchor.second);
        if (firstPart<0 || secondPart<0 || firstPart==secondPart) return false;
        const int level=_shape->FindFireGeometryLevel(); Shape* fireShape=_shape->Level(level);
        fireShape->SaveOriginalPos();
        std::vector<Vector3> modelA,modelB,worldA,worldB;
        for (int component=1;component<=12;++component)
        {
            char name[32]; std::snprintf(name,sizeof(name),"component%02d",component);
            const int selection=fireShape->FindNamedSel(name);
            if (selection<0) return false;
            const auto& selected=fireShape->NamedSel(selection);
            for (int vertex=0;vertex<selected.Size();++vertex)
            {
                const int p=selected[vertex];
                if (p<0 || p>=fireShape->NPos()) return false;
                const auto& binding=weights[level][p];
                if (binding.Size()!=1 || binding[0].GetWeight()!=1 || binding[0].GetSel()<0 || binding[0].GetSel()>=25) return false;
                const int part=StockCorpseAnatomicalPart(static_cast<const char*>(skeleton->GetBone(binding[0].GetSel())));
                if (part!=firstPart && part!=secondPart) continue;
                auto& model=part==firstPart?modelA:modelB; auto& world=part==firstPart?worldA:worldB;
                model.push_back(fireShape->OrigPos(p));
                // Consume this actual FireLOD's palette/affine pose, including
                // its own correction; never apply the graphical front palette.
                world.push_back(PositionModelToWorld(AnimatePoint(level,p)));
            }
        }
        if (!CorpseMeasureFireBoundary(modelA,modelB,worldA,worldB,anchor)) return false;
        anchor.sourceLevel=level;
        LOG_INFO(World,"CORPSEANCHOR: model={} first={} second={} source={} sourceLevel={} sourcePairCount={} sharedVertices=0 separation={}",
            _shape->Name(),anchor.first,anchor.second,anchor.source,level,anchor.sourcePairCount,anchor.separation);
        return true;
    };
    for (int boundaryIndex = 0; boundaryIndex < (hingeEndpoints ? 15 : 9); ++boundaryIndex)
    {
        const auto& boundary = boundaries[boundaryIndex];
        CorpseAnchorDiagnostic anchor;
        anchor.first = boundary[0]; anchor.second = boundary[1];
        int first = graphical->FindNamedSel(boundary[0]), second = graphical->FindNamedSel(boundary[1]);
        if (first < 0 || second < 0)
        {
            if (!fireBoundary(anchor)) { refusal = "stock-anchor-selection-missing"; return false; }
            candidate.anchors.push_back(std::move(anchor)); continue;
        }
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
        else if (!fireBoundary(anchor)) { refusal = "shared-geometry-anchor-not-admitted"; return false; }
        candidate.anchors.push_back(std::move(anchor));
    }
    if (!UseCorpsePose()) { refusal = "hold-retired-during-measurement"; return false; }
    result = std::move(candidate);
    refusal = "";
    return true;
}

bool Man::StartCorpseArticulation(RString& refusal, bool anatomicalHinges)
{
    if (!UseCorpsePose() || _corpseArticulation) { refusal = "captured-authored-corpse-required"; return false; }
    if (std::abs(CorpseMatrixDeterminant(Transform())-1) > 1e-4f)
    { refusal = "unit-scale-world-frame-required"; return false; }
    if (!StockCorpseCapability(_shape, _corpsePoseSkeleton.GetRef(), refusal)) return false;
    if (_automaticCorpsePose && anatomicalHinges && !StockCorpseHasAuthoredHingeEndpoints(_shape->Name()))
    {
        anatomicalHinges = false;
        LOG_INFO(World,"AUTORAGDOLL: model={} jointMode=spherical reason=stock-anatomical-hinge-endpoints-unavailable physicalJointAnchorGatesUnchanged=1",
            _shape->Name());
    }
    if (!_automaticCorpsePose && (CorpseMatrixIdentityError(_headTrans) > 1e-6f || CorpseMatrixIdentityError(_gunTrans) > 1e-6f ||
        CorpseMatrixIdentityError(_legTrans) > 1e-6f))
    { refusal = "identity-post-animation-corrections-required"; return false; }
    auto water = GRainWater().At(Position().X(), Position().Z());
    if (water.valid && water.depth > 0.003f) { refusal = "dry-retained-water-support-required"; return false; }
    Physics::PhysicsWorld* world = Physics::GetPhysicsWorld();
    if (!world || !world->IsCreated() || !world->GetStats().terrainRegistered)
    { refusal = "registered-physics-terrain-required"; return false; }
    const bool groundedContactRecovery = CorpseGroundedContactRecoveryAdmitted(
        _automaticCorpsePose && _automaticCorpsePose->pending && _automaticDeathEligible && _landContact,
        Position().Y()-GLandscape->RoadSurfaceY(Position()+VUp*.5f));
    Skeleton* skeleton = _corpsePoseSkeleton.GetRef();
    if (!skeleton || skeleton->NBones() != 33) { refusal = "complete-stock-33-bone-palette-required"; return false; }
    const char* names[][6] = {{"pzadek","lzadek","bricho",nullptr},
        {"zebra","hrudnik","krk","prameno","lrameno",nullptr}, {"hlava",nullptr},
        {"pbiceps",nullptr}, {"ploket","pruka",nullptr}, {"lbiceps",nullptr}, {"lloket","lruka",nullptr},
        {"pstehno",nullptr}, {"pholen","pchodidlo","pprsty",nullptr},
        {"lstehno",nullptr}, {"lholen","lchodidlo","lprsty",nullptr}};
    auto state = std::make_unique<CorpseArticulation>();
    state->world = world; state->epoch = world->Generation(); state->object = Transform();
    if (_automaticCorpsePose) state->collisionFamily = _automaticCorpsePose->collisionFamily;
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
    if (_automaticCorpsePose)
        for (int level=0;level<_shape->NLevels();++level)
        {
            const auto& captured=_automaticCorpsePose->levels[level];
            for (size_t p=0;p<captured.proxySelections.size();++p)
            {
                const auto& weight=GetSelWeights(level,captured.proxySelections[p]);
                const int representative=StockCorpseRigidProxyBone(weight.Size(),
                    [&](int w) { return weight[w].GetSel(); },
                    [&](int w) { return weight[w].GetWeight(); },
                    [&](int bone) { return state->boneParts[bone]; });
                if (representative < 0 || representative != captured.proxyBones[p])
                { refusal="captured-proxy-solver-part-disagrees"; return false; }
            }
        }

    CorpsePoseDiagnostic diagnostic;
    if (!InspectCorpsePose(diagnostic, refusal, anatomicalHinges)) return false;
    for (const auto& hull : diagnostic.hulls)
        if (hull.externalCenterRayHits != 0)
        { refusal = "open-ground-no-external-center-ray-contact-required"; return false; }
    for (const auto& anchor : diagnostic.anchors)
        if ((anchor.sharedVertices <= 0 && !(anchor.source=="authored-fire-boundary-pair" && anchor.sourcePairCount>0 &&
            !anatomicalHinges && StockCorpseModelNameEqual(_shape->Name(),"data3d\\angelina.p3d"))) || anchor.separation > 0.12f)
        { refusal = "shared-geometry-anchor-not-admitted"; return false; }

    std::array<CorpseHingeMeasurement, 4> hinges;
    if (anatomicalHinges)
    {
        // Bilateral shoulder/hip and actual ankle/toe boundaries establish the
        // neutral body's plane and its forward sign. No fixed model/world axis.
        auto neutral = [&](int index) { return diagnostic.anchors[index].modelAnchor; };
        Vector3 spine = neutral(0)-(neutral(5)+neutral(6))*.5f;
        Vector3 lateral = (neutral(2)-neutral(1))+(neutral(6)-neutral(5));
        Vector3 toes = (neutral(13)-neutral(11))+(neutral(14)-neutral(12));
        if (spine.Size() < .25f || lateral.Size() < .15f || toes.Size() < .1f)
        { refusal = "hinge-neutral-body-frame-degenerate"; return false; }
        spine.Normalize(); lateral -= spine*(spine*lateral);
        if (lateral.Size() < .15f) { refusal = "hinge-neutral-body-frame-degenerate"; return false; }
        lateral.Normalize(); Vector3 forward = spine.CrossProduct(lateral); forward.Normalize();
        const float toeAgreement = forward*toes/toes.Size();
        if (std::abs(toeAgreement) < .5f) { refusal = "hinge-neutral-forward-sign-ambiguous"; return false; }
        if (toeAgreement < 0) forward *= -1;
        const int endpointIndices[][3] = {{1,3,9},{2,4,10},{5,7,11},{6,8,12}};
        const char* proximalBones[] = {"pbiceps","lbiceps","pstehno","lstehno"};
        const char* distalBones[] = {"ploket","lloket","pholen","lholen"};
        const auto& palette = diagnostic.levels.front().palette;
        auto vectorText = [](Vector3Val vector) {
            char text[128]; std::snprintf(text,sizeof(text),"(%.9g,%.9g,%.9g)",vector.X(),vector.Y(),vector.Z());
            return std::string(text);
        };
        for (int limb = 0; limb < 4; ++limb)
        {
            const auto& endpoints = endpointIndices[limb];
            const int proximalBone = skeleton->FindBone(proximalBones[limb]);
            const int distalBone = skeleton->FindBone(distalBones[limb]);
            if (proximalBone < 0 || distalBone < 0) { refusal = "hinge-bone-binding-required"; return false; }
            const bool admitted = CorpseMeasureHinge(neutral(endpoints[0]), neutral(endpoints[1]), neutral(endpoints[2]),
                forward, Transform()*palette[proximalBone], Transform()*palette[distalBone], limb >= 2, hinges[limb]);
            const auto& h = hinges[limb];
            LOG_INFO(World,"CORPSEHINGE: limb={} bone={} admitted={} refusal={} neutralProx={} neutralJoint={} neutralDist={} neutralForward={} worldProx={} worldDist={} reference={} axis={} angle={} agreement={} limits=({},{}); shared=({},{},{}) separations=({},{},{})",
                limb,distalBones[limb],admitted,h.refusal,vectorText(neutral(endpoints[0])),vectorText(neutral(endpoints[1])),vectorText(neutral(endpoints[2])),vectorText(forward),
                vectorText(h.proximal),vectorText(h.distal),vectorText(h.reference),vectorText(h.axis),h.capturedAngle,h.referenceAgreement,h.lower,h.upper,
                diagnostic.anchors[endpoints[0]].sharedVertices,diagnostic.anchors[endpoints[1]].sharedVertices,diagnostic.anchors[endpoints[2]].sharedVertices,
                diagnostic.anchors[endpoints[0]].separation,diagnostic.anchors[endpoints[1]].separation,diagnostic.anchors[endpoints[2]].separation);
            if (!admitted) { refusal = "measured-anatomical-hinge-frame-not-admitted"; return false; }
        }
    }

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
    float pelvisFireMinimum=std::numeric_limits<float>::infinity(),pelvisSkinMinimum=pelvisFireMinimum;
    for (const auto& point:vertices[0])
        pelvisFireMinimum=std::min(pelvisFireMinimum,point.Y()-GLandscape->RoadSurfaceY(point+VUp*.5f));
    for (int vertex = 0; vertex < graphical->NPos(); ++vertex)
    {
        const auto& weight = weights[0][vertex]; bool pelvis = false, proxy = false;
        for (int w = 0; w < weight.Size(); ++w)
            if (weight[w].GetSel() < 25 && state->boneParts[weight[w].GetSel()] == 0) pelvis = true;
        if (!pelvis) continue;
        for (int s = 0; s < graphical->NNamedSel(); ++s)
            if (std::strncmp(graphical->NamedSel(s).Name(), "proxy:", 6) == 0 &&
                graphical->NamedSel(s).IsSelected(vertex)) { proxy = true; break; }
        if (!proxy)
        {
            const Vector3 point=PositionModelToWorld(AnimatePoint(0,vertex));
            pelvisSkinMinimum=std::min(pelvisSkinMinimum,point.Y()-GLandscape->RoadSurfaceY(point+VUp*.5f));
            vertices[0].push_back(point);
        }
    }
    const float masses[] = {12,24,5,3,2,3,2,8,4,8,4};
    state->minimumClearance = 1000;
    RString fitRefusal;
    float maximumRootDistance = 0;
    int fittedVertices = 0;
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
            float actualMinimum=std::numeric_limits<float>::infinity();
            for (const auto& point:vertices[0])
                actualMinimum=std::min(actualMinimum,point.Y()-GLandscape->RoadSurfaceY(point+VUp*.5f));
            float cornersMinimum=std::numeric_limits<float>::infinity();
            for (int corner=0;corner<8;++corner)
            {
                const Vector3 point((corner&1)?hi.X():lo.X(),(corner&2)?hi.Y():lo.Y(),(corner&4)?hi.Z():lo.Z());
                cornersMinimum=std::min(cornersMinimum,point.Y()-GLandscape->RoadSurfaceY(point+VUp*.5f));
            }
            if (_automaticCorpsePose)
                LOG_INFO(World,"CORPSEFIT: pelvis-source model={} vertices={} actualMinimum={} fireMinimum={} skinMinimum={} generatedAabbMinimum={} rootY={} sourceBoundsY=({},{}) actualPoseUnchanged=1",
                    _shape->Name(),vertices[0].size(),actualMinimum,pelvisFireMinimum,pelvisSkinMinimum,cornersMinimum,Position().Y(),lo.Y(),hi.Y());
            vertices[0].clear();
            for (int corner = 0; corner < 8; ++corner)
                vertices[0].emplace_back((corner&1)?hi.X():lo.X(), (corner&2)?hi.Y():lo.Y(), (corner&4)?hi.Z():lo.Z());
            center = (lo+hi)*0.5f;
        }
        state->initial[part] = MIdentity; state->initial[part].SetPosition(center);
        if (groundedContactRecovery)
        {
            // Before admitting overlap, every actual body origin must remain
            // above both positively identified floors. This is not an airborne
            // capture or an attempt to rescue a body origin buried in terrain.
            const float roadY=GLandscape->RoadSurfaceY(center+VUp*.5f);
            const auto filter=Physics::QueryFilter{Physics::ColliderFlags::Roadway};
            Vector3 floor;
            if (!center.IsFinite() || !std::isfinite(roadY) || center.Y()<=roadY ||
                !world->RayHitAnything(center+VUp*.8f,center-VUp*3.2f,filter))
            { refusal="grounded-contact-recovery-body-origin-not-above-ground"; return false; }
            world->CastRay(center+VUp*.8f,center-VUp*3.2f,floor,filter);
            if (!floor.IsFinite() || std::abs(floor.Y()-roadY)>.025f || center.Y()<=floor.Y())
            { refusal="grounded-contact-recovery-body-origin-floor-disagrees"; return false; }
        }
        for (auto& point : vertices[part]) point = (point-center)*(_automaticCorpsePose ? .90f : .40f);
        float proxyClearance = std::numeric_limits<float>::infinity();
        float nativeProxyClearance = std::numeric_limits<float>::infinity();
        for (const auto& point : vertices[part])
        {
            const Vector3 supported = center+point;
            if (!supported.IsFinite()) { proxyClearance = std::numeric_limits<float>::quiet_NaN(); break; }
            const float roadY = GLandscape->RoadSurfaceY(supported+VUp*0.5f);
            if (!std::isfinite(roadY)) { proxyClearance = std::numeric_limits<float>::quiet_NaN(); break; }
            nativeProxyClearance = std::min(nativeProxyClearance,supported.Y()-roadY);
            proxyClearance = std::min(proxyClearance,supported.Y()-roadY);
            if (_automaticCorpsePose)
            {
                // Heightfields quantise their vertices. Calibrate the same
                // bounded proxy against BOTH admitted floors, preserving the
                // authored pose, body origin and 10 mm maximum collider offset.
                const auto filter = Physics::QueryFilter{Physics::ColliderFlags::Roadway};
                const Vector3 from = supported+VUp*.8f, to = supported-VUp*3.2f;
                Vector3 floor;
                if (!world->RayHitAnything(from,to,filter))
                { refusal = "solver-terrain-floor-not-present"; return false; }
                world->CastRay(from,to,floor,filter);
                if (!floor.IsFinite() || std::abs(floor.Y()-roadY) > .025f)
                { refusal = "solver-floor-disagrees-with-authored-contact"; return false; }
                proxyClearance = std::min(proxyClearance,supported.Y()-floor.Y());
            }
        }
        float proxyLift = 0;
        if (CorpseProxyContactLift(proxyClearance,proxyLift) && proxyLift > 0)
        {
            // Keep the body origin, skin, joint anchors and proxy dimensions.
            // This small local collider offset stays inside the inset anatomy;
            // every original support/admission check below still must pass.
            for (auto& point : vertices[part]) point += VUp*proxyLift;
            LOG_INFO(World,"CORPSEFIT: contact-proxy part={} name={} lift={} originalClearance={} nativeClearance={} bodyOriginUnchanged=1",
                part,names[part][0],proxyLift,proxyClearance,nativeProxyClearance);
        }
        int vertex = 0;
        for (auto& point : vertices[part])
        {
            const Vector3 supported = center+point;
            const bool finite = point.IsFinite() && supported.IsFinite() && Position().IsFinite();
            const float roadY = finite ? GLandscape->RoadSurfaceY(supported+VUp*0.5f) : std::numeric_limits<float>::quiet_NaN();
            const float clearance = supported.Y()-roadY;
            const float distance = (supported-Position()).Size();
            if (std::isfinite(clearance)) state->minimumClearance = std::min(state->minimumClearance,clearance);
            if (std::isfinite(distance)) maximumRootDistance = std::max(maximumRootDistance,distance);
            ++fittedVertices;
            const bool ordinaryBoot = _automaticCorpsePose && (part == 8 || part == 10);
            if (!finite || !CorpseInitialContactAdmitted(ordinaryBoot,clearance,groundedContactRecovery) ||
                clearance > (_automaticCorpsePose ? 2.8f : .8f) || distance > 2.5f)
            {
                if (fitRefusal.GetLength() == 0)
                {
                    Vector3 floor = VZero;
                    const auto filter = Physics::QueryFilter{Physics::ColliderFlags::Roadway};
                    const bool hit = finite && world->RayHitAnything(supported+VUp*.8f,supported-VUp,filter);
                    if (hit) world->CastRay(supported+VUp*.8f,supported-VUp,floor,filter);
                    const float terrainY = finite ? GLandscape->SurfaceY(supported.X(),supported.Z()) : std::numeric_limits<float>::quiet_NaN();
                    char detail[768];
                    std::snprintf(detail,sizeof(detail),"part=%d name=%s vertex=%d supported=(%.9g,%.9g,%.9g) center=(%.9g,%.9g,%.9g) terrainY=%.9g roadY=%.9g solverHit=%d solverY=%.9g clearance=%.9g rootDistance=%.9g localFinite=%d worldFinite=%d rootFinite=%d",
                        part,names[part][0],vertex,supported.X(),supported.Y(),supported.Z(),center.X(),center.Y(),center.Z(),
                        terrainY,roadY,int(hit),hit?floor.Y():std::numeric_limits<float>::quiet_NaN(),clearance,distance,
                        int(point.IsFinite()),int(supported.IsFinite()),int(Position().IsFinite()));
                    fitRefusal = detail;
                }
            }
            ++vertex;
        }
    }
    if (fitRefusal.GetLength() != 0)
    {
        char detail[1024];
        std::snprintf(detail,sizeof(detail),"inset-collider-contact-or-bounds-not-admitted %s minimumClearance=%.9g maximumRootDistance=%.9g fittedVertices=%d",
            static_cast<const char*>(fitRefusal),state->minimumClearance,maximumRootDistance,fittedVertices);
        LOG_WARN(World,"CORPSEFIT: {}",detail);
        refusal = detail;
        return false;
    }
    // Check the ACTUAL registered solver floor before creating any dynamic part.
    // A different heightfield/road support is refused, never silently corrected.
    for (int part = 0; part < 11; ++part)
    {
        const Vector3 center = state->initial[part].Position();
        for (const auto& point : vertices[part])
        {
            const Vector3 supported = center+point, from = supported+VUp*0.8f,
                to = supported-VUp*(_automaticCorpsePose ? 3.2f : 1.0f);
            Vector3 floor;
            if (!world->RayHitAnything(from,to,Physics::QueryFilter{Physics::ColliderFlags::Roadway}))
            { refusal = "solver-terrain-floor-not-present"; return false; }
            world->CastRay(from,to,floor,Physics::QueryFilter{Physics::ColliderFlags::Roadway});
            if (!floor.IsFinite() || std::abs(floor.Y()-GLandscape->RoadSurfaceY(supported+VUp*0.5f)) > 0.025f)
            { refusal = "solver-floor-disagrees-with-authored-contact"; return false; }
            state->minimumClearance = std::min(state->minimumClearance,supported.Y()-floor.Y());
            const bool ordinaryBoot = _automaticCorpsePose && (part == 8 || part == 10);
            if (!CorpseInitialContactAdmitted(ordinaryBoot,supported.Y()-floor.Y(),groundedContactRecovery))
            { refusal = "inset-collider-penetrates-solver-floor"; return false; }
        }
    }
    if (_automaticCorpsePose && state->minimumClearance < .003f)
        LOG_INFO(World,"AUTORAGDOLL: bounded initial contact minimumClearance={} maximumPenetration={} groundedRecovery={} skinAndOriginsUnchanged=1",
            state->minimumClearance,groundedContactRecovery?.1f:.025f,groundedContactRecovery);
    for (int part = 0; part < 11; ++part)
    {
        Physics::ConvexPiece piece{vertices[part].data(), static_cast<int>(vertices[part].size()), Physics::ColliderFlags::Solid};
        if (_automaticCorpsePose)
        { state->retainedHulls[part]=vertices[part]; state->retainedMasses[part]=masses[part]; }
        Physics::ArticulatedInitialMotion motion;
        if (_automaticCorpsePose)
        {
            motion.collisionFamily = state->collisionFamily;
            motion.angular = _automaticCorpsePose->initialAngularVelocity;
            motion.linear = _automaticCorpsePose->initialSpeed + motion.angular.CrossProduct(
                state->initial[part].Position()-PositionModelToWorld(_shape->CenterOfMass()));
        }
        state->bodies[part] = world->SpawnArticulatedPiece(piece, state->initial[part], masses[part],motion);
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
        // Only the spherical root uses a generated midpoint; its complete
        // measured body separation provides the same anatomical direction.
        if (!anatomicalHinges && j==0)
        {
            if (!CorpseMeasureSphericalRootAxis(state->initial[a].Position(),state->initial[b].Position(),pivot,axis))
            { refusal="spherical-root-centre-separation-required"; return false; }
        }
        if (!axis.IsFinite() || axis.Size() < 0.025f)
        {
            refusal = "joint-geometry-axis-degenerate";
            LOG_WARN(World,"AUTORAGDOLL: joint frame refused model={} joint={} firstPart={} secondPart={} anchor={} axisLength={} firstCenter=({},{},{}) secondCenter=({},{},{}) pivot=({},{},{}) hinges={}",
                _shape->Name(),j,a,b,anchors[j],axis.Size(),
                state->initial[a].Position().X(),state->initial[a].Position().Y(),state->initial[a].Position().Z(),
                state->initial[b].Position().X(),state->initial[b].Position().Y(),state->initial[b].Position().Z(),
                pivot.X(),pivot.Y(),pivot.Z(),anatomicalHinges);
            return false;
        }
        axis.Normalize(); Vector3 up = std::abs(axis*VUp) < 0.95f ? VUp : VAside;
        const bool distal = j == 3 || j == 5 || j == 7 || j == 9;
        const int hingeIndex = j == 3 ? 0 : j == 5 ? 1 : j == 7 ? 2 : 3;
        if (anatomicalHinges && distal) { axis = hinges[hingeIndex].axis; up = hinges[hingeIndex].proximal; }
        Matrix4 joint(MDirection, axis, up); joint.SetPosition(pivot);
        Physics::ArticulatedJointDef definition;
        definition.first = state->bodies[a]; definition.second = state->bodies[b];
        definition.firstFrame = state->initial[a].InverseRotation()*joint;
        definition.secondFrame = state->initial[b].InverseRotation()*joint;
        definition.coneAngle = distal ? 0.30f : 0.9f;
        definition.twistAngle = distal ? 0.20f : 0.6f;
        if (anatomicalHinges && distal)
        {
            definition.kind = Physics::ArticulatedJointKind::Hinge;
            definition.lowerAngle = hinges[hingeIndex].lower; definition.upperAngle = hinges[hingeIndex].upper;
        }
        state->joints[j] = world->AddArticulatedJoint(definition);
        if (_automaticCorpsePose) state->retainedJoints[j]=definition;
        if (!state->joints[j].IsValid()) { refusal = "backend-refused-anatomical-joint"; return false; }
        if (definition.kind == Physics::ArticulatedJointKind::Hinge) ++state->hingeCount;
    }
    if (_automaticCorpsePose)
    { state->wakeRecipe=true; state->terrainSerial=world->TerrainMutationSerial(); }
    _corpseArticulation = std::move(state);
    refusal = "";
    if (!UpdateCorpseArticulation()) { refusal = "initial-body-readback-refused"; return false; }
    return true;
}

bool Man::CaptureCorrectedCorpsePose(RString& refusal)
{
    refusal = "stock-corrected-live-pose-required";
    const float worldDeterminant=CorpseMatrixDeterminant(Transform());
    if (!_shape || !_headTrans.IsFinite() || !_gunTrans.IsFinite() || !_legTrans.IsFinite() ||
        std::abs(worldDeterminant-1) > 1e-4f || !GLandscape || !GWorld)
    {
        refusal=!_shape?"stock-live-model-unavailable":!_headTrans.IsFinite()?"nonfinite-live-head-correction":
            !_gunTrans.IsFinite()?"nonfinite-live-gun-correction":!_legTrans.IsFinite()?"nonfinite-live-leg-correction":
            !GLandscape || !GWorld?"stock-live-world-unavailable":"unit-scale-live-world-frame-required";
        LOG_INFO(World,"AUTORAGDOLL: live capture refused model={} reason={} headFinite={} gunFinite={} legFinite={} rootFinite={} worldDeterminant={} headDeterminant={} gunDeterminant={} legDeterminant={} rootAxisLengths=({},{},{}) rootAxisDots=({},{},{})",
            _shape?_shape->Name():"unavailable",refusal.Data(),_headTrans.IsFinite(),_gunTrans.IsFinite(),_legTrans.IsFinite(),Transform().IsFinite(),
            worldDeterminant,CorpseMatrixDeterminant(_headTrans),CorpseMatrixDeterminant(_gunTrans),CorpseMatrixDeterminant(_legTrans),
            DirectionAside().Size(),DirectionUp().Size(),Direction().Size(),DirectionAside()*DirectionUp(),DirectionUp()*Direction(),Direction()*DirectionAside());
        return false;
    }
    Skeleton* skeleton = Type()->GetWeights().GetName().skeleton.GetTypeRef();
    if (!StockCorpseCapability(_shape, skeleton, refusal)) return false;
    AffinePhasePose primary, secondary;
    AnimationRT* pri = Type()->GetAnimation(_primaryMove.id);
    AnimationRT* sec = Type()->GetAnimation(_secondaryMove.id);
    if (!std::isfinite(_primaryFactor) || _primaryFactor < 0 || _primaryFactor > 1)
    { refusal="finite-live-animation-factor-required"; return false; }
    if (_primaryFactor > .01f && (!pri || !pri->CapturePhasePose(primary,_primaryTime)))
    { refusal="actual-live-primary-phase-required"; return false; }
    if (_primaryFactor < .99f && (!sec || !sec->CapturePhasePose(secondary,_secondaryTime)))
    { refusal="actual-live-secondary-phase-required"; return false; }
    auto pose = std::make_unique<CorrectedCorpsePose>();
    pose->capturedRoot = pose->root = Transform();
    pose->initialSpeed = Speed(); pose->initialAngularVelocity = AngVelocity();
    if (!pose->initialSpeed.IsFinite() || !pose->initialAngularVelocity.IsFinite())
    { refusal="finite-live-inherited-motion-required"; return false; }
    const auto& weights = Type()->GetWeights();
    for (int level = 0; level < _shape->NLevels(); ++level)
    {
        Shape* shape = _shape->Level(level);
        if (!shape || shape->NPos() <= 0 || shape->NPos() > CorrectedCorpsePose::MaximumPoints)
        { refusal="bounded-live-lod-points-required"; return false; }
        CorrectedCorpseLevel captured;
        MATRIX_4_ARRAY(palette,128);
        PrepareCorrectedPose(level,palette);
        _head.EvaluateDeadFaceBones(Type()->_head,weights,level,palette,true);
        if (palette.Size() != 33) { refusal="complete-live-corrected-palette-required"; return false; }
        for (int bone = 0; bone < palette.Size(); ++bone) captured.authoredPalette.push_back(palette[bone]);
        for (int point = 0; point < shape->NPos(); ++point)
        {
            const auto& weight = weights[level][point];
            CorpsePointBinding binding;
            for (int w = 0; w < weight.Size(); ++w)
            { binding.bones.push_back(weight[w].GetSel()); binding.weights.push_back(weight[w].GetWeight()); }
            const bool supports = (!primary.Valid() || primary.SupportsPoint(weight)) &&
                (!secondary.Valid() || secondary.SupportsPoint(weight));
            if (!supports && (level == _shape->FindMemoryLevel() || shape->NPos() == 1))
            {
                refusal="actual-phase-bone-required-for-memory-point";
                LOG_INFO(World,"AUTORAGDOLL: live point capture refused model={} level={} point={} memory={} primaryMatrices={} secondaryMatrices={} weightCount={} firstBone={}",
                    _shape->Name(),level,point,level==_shape->FindMemoryLevel(),primary.PhaseMatrices(),secondary.PhaseMatrices(),weight.Size(),weight.Size()?weight[0].GetSel():-1);
                return false;
            }
            // Reserved facial tails are palette-only in the authored path.
            captured.authoredPoints.push_back(supports ? AnimatePoint(level,point) :
                AnimationRT::ApplyMatricesPoint(weights[level],_shape,level,palette,point));
            captured.bindings.push_back(std::move(binding));
        }
        for (int p = 0; p < shape->NProxies(); ++p)
        {
            const auto& proxy = shape->Proxy(p);
            if (proxy.selection < 0 || proxy.selection >= shape->NNamedSel() ||
                shape->NamedSel(proxy.selection).Size() <= 0) continue;
            const auto& weight = GetProxyWeights(level,proxy);
            if (weight.Size() <= 0) continue;
            // Preserve the native proxy blend when every influence receives
            // exactly the same existing articulated body delta.
            const int proxyBone=StockRigidProxyBone(weight,skeleton);
            if (proxyBone < 0)
            {
                refusal="single-part-live-equipment-proxy-required";
                LOG_INFO(World,"AUTORAGDOLL: live proxy capture refused model={} level={} proxy={} selection={} weightCount={} firstBone={} firstWeight={}",
                    _shape->Name(),level,p,shape->NamedSel(proxy.selection).Name(),weight.Size(),weight.Size()?weight[0].GetSel():-1,weight.Size()?weight[0].GetWeight():0);
                return false;
            }
            captured.proxySelections.push_back(proxy.selection); captured.proxyBones.push_back(proxyBone);
            captured.authoredProxies.push_back(AnimateProxyMatrix(level,proxy));
        }
        captured.palette = captured.authoredPalette; captured.points = captured.authoredPoints;
        captured.proxies = captured.authoredProxies; pose->levels.push_back(std::move(captured));
    }
    if (!pose->Valid()) { refusal = "invalid-corrected-live-pose"; return false; }
    ClearCorpsePose();
    _corpsePosePrimary = std::move(primary); _corpsePoseSecondary = std::move(secondary);
    _corpsePoseModel = _shape; _corpsePoseSkeleton = skeleton; _corpsePoseWeights = &Type()->GetWeights();
    _corpsePosePrimaryAnimation = pri; _corpsePoseSecondaryAnimation = sec;
    _corpsePoseLease = {GWorld,_shape.GetRef(),skeleton,33,int(_primaryMove.id),int(_secondaryMove.id),
        Glob.time.toInt(),_primaryFactor};
    _automaticCorpsePose = std::move(pose);
    bool exact = true;
    int comparedPalettes = 0, comparedPoints = 0, comparedProxies = 0;
    _corpsePoseBypass = true;
    for (int level = 0; level < _shape->NLevels(); ++level)
    {
        const auto& captured = _automaticCorpsePose->levels[level];
        MATRIX_4_ARRAY(palette,128);
        PrepareCorrectedPose(level,palette);
        _head.EvaluateDeadFaceBones(Type()->_head,weights,level,palette,true);
        exact &= palette.Size() == int(captured.palette.size());
        for (int bone = 0; bone < palette.Size(); ++bone)
        { exact &= std::memcmp(&palette[bone],&captured.palette[bone],sizeof(Matrix4)) == 0; ++comparedPalettes; }
        for (int point = 0; point < int(captured.points.size()); ++point)
        {
            const auto& weight = weights[level][point];
            bool automaticPaletteOnly = false;
            if (_automaticCorpsePose) for (int w = 0; w < weight.Size(); ++w)
                if (weight[w].GetSel() >= 25) automaticPaletteOnly = true;
            if (automaticPaletteOnly || (_corpsePosePrimary.Valid() && !_corpsePosePrimary.SupportsPoint(weight)) ||
                (_corpsePoseSecondary.Valid() && !_corpsePoseSecondary.SupportsPoint(weight))) continue;
            const Vector3 actual = AnimatePoint(level,point);
            exact &= std::memcmp(&actual,&captured.points[point],sizeof(Vector3)) == 0; ++comparedPoints;
        }
        Shape* shape = _shape->Level(level);
        for (int p = 0; p < shape->NProxies(); ++p)
        {
            const auto& proxy = shape->Proxy(p);
            for (size_t i = 0; i < captured.proxySelections.size(); ++i)
                if (captured.proxySelections[i] == proxy.selection)
                {
                    const Matrix4 actual = AnimateProxyMatrix(level,proxy);
                    exact &= std::memcmp(&actual,&captured.proxies[i],sizeof(Matrix4)) == 0; ++comparedProxies;
                }
        }
    }
    _corpsePoseBypass = false;
    LOG_INFO(World,"AUTORAGDOLL: actual zero-motion consumers exact={} matrices={} points={} proxies={}",
        exact,comparedPalettes,comparedPoints,comparedProxies);
    if (!exact) { ClearCorpsePose(); refusal = "corrected-consumer-zero-motion-mismatch"; return false; }
    refusal = "";
    return true;
}

void Man::QueueAutomaticRagdoll()
{
    if (!AutomaticRagdollEnabled() || !_automaticDeathEligible || _corpsePoseLease.world ||
        !GWorld || GWorld->GetMode() != GModeArcade || !IsLocal() ||
        (!_isDead && !IsDammageDestroyed()) || !_landContact || _waterContact || _waterBuoyancyContact ||
        _waterDepth > 0 || _hydroWaterDepth > 0 || GetHierachyParent() || _ladderBuilding ||
        _hideBody != 0 || _hideBodyWanted != 0 || ToDelete() || ToMoveOut() || !IsInLandscape()) return;
    g_automaticCorpses.erase(std::remove_if(g_automaticCorpses.begin(),g_automaticCorpses.end(),
        [](const OLink<Man>& owner) { return !owner.GetLink() || !owner->_automaticCorpsePose; }),g_automaticCorpses.end());
    bool used[4] = {}; int retained = 0;
    for (const auto& owner : g_automaticCorpses)
        if (Man* man = owner.GetLink(); man && man->_automaticCorpsePose && man->UseCorpsePose())
        {
            ++retained;
            if (!man->_automaticCorpsePose->frozen)
            {
                const int slot = -man->_automaticCorpsePose->collisionFamily-2;
                if (slot >= 0 && slot < 4) used[slot] = true;
            }
        }
    int slot = 0; while (slot < 4 && used[slot]) ++slot;
    if (slot == 4 || retained >= 64)
    { LOG_INFO(World,"AUTORAGDOLL: authored fallback reason=corpse-budget activeSlots={} retained={}",slot,retained); return; }
    RString refusal;
    // KilledBy can run before HitBy for script damage. This is a damage death,
    // never an animation-only request, and retains the existing scoring callback.
    if (IsDammageDestroyed()) _isDead = true;
    if (!CaptureCorrectedCorpsePose(refusal))
    { LOG_INFO(World,"AUTORAGDOLL: authored fallback reason={} model={}",refusal.Data(),_shape ? _shape->Name() : "unavailable"); return; }
    _automaticCorpsePose->collisionFamily = -slot-2;
    g_automaticCorpses.emplace_back(this);
    g_corpsePoseOwner = this; // diagnostic selection only; previous corpse stays owned
    LOG_INFO(World,"AUTORAGDOLL: source posture primary={} secondary={} factor={} primaryTime={} upDegree={}",
        int(_primaryMove.id),int(_secondaryMove.id),_primaryFactor,_primaryTime,GetActUpDegree());
    LOG_INFO(World,"AUTORAGDOLL: captured actual death owner={} family={} levels={} model={} speed=({},{},{}) angular=({},{},{}) corrections=({},{},{})",
        static_cast<const void*>(this),-slot-2,_automaticCorpsePose->levels.size(),_shape->Name(),Speed().X(),Speed().Y(),Speed().Z(),
        AngVelocity().X(),AngVelocity().Y(),AngVelocity().Z(),CorpseMatrixIdentityError(_headTrans),
        CorpseMatrixIdentityError(_gunTrans),CorpseMatrixIdentityError(_legTrans));
}

bool Man::BeginAutomaticRagdoll(RString& refusal)
{
    if (!_automaticCorpsePose || !_automaticCorpsePose->pending || !UseCorpsePose()) return false;
    const auto water = GRainWater().At(Position().X(),Position().Z());
    if ((water.valid && water.depth > .003f) || !AutomaticRagdollTerrain(refusal)) return false;
    const Matrix3 inertia = Orientation()*InvInertia()*GetInvTransform().Orientation();
    _automaticCorpsePose->initialSpeed += _impulseForce*(GetInvMass()*Rigid());
    _automaticCorpsePose->initialAngularVelocity += inertia*(_impulseTorque*Rigid());
    if (!_automaticCorpsePose->initialSpeed.IsFinite() || !_automaticCorpsePose->initialAngularVelocity.IsFinite() ||
        _automaticCorpsePose->initialSpeed.Size() > 15 || _automaticCorpsePose->initialAngularVelocity.Size() > 20)
    { refusal = "inherited-death-motion-outside-bounded-admission"; return false; }
    if (!StartCorpseArticulation(refusal,true))
    {
        if (std::strcmp(refusal,"measured-anatomical-hinge-frame-not-admitted")!=0) return false;
        LOG_INFO(World,"AUTORAGDOLL: model={} jointMode=spherical reason={} actualLivePoseRetained=1 physicalJointAnchorGatesUnchanged=1",
            _shape->Name(),refusal.Data());
        // A valid authored stance can lack a safe hinge plane/angle. Retry the
        // existing constrained spherical control, including ALL source-anchor,
        // geometry/contact/bounds admission; never expand hinge safety limits.
        if (!StartCorpseArticulation(refusal,false)) return false;
    }
    _automaticCorpsePose->pending = false;
    _impulseForce = _impulseTorque = VZero;
    _whenKilled = Glob.time; _lastMovementTime = Glob.time;
    IsMoved();
    ActionContextDefault context; context.function = MFDead;
    ProcessMoveFunction(&context);
    LOG_INFO(World,"AUTORAGDOLL: activated owner={} bodies=11 joints=10 hinges={} family={} model={} inheritedImpulseConsumed=1 speed=({},{},{}) angular=({},{},{})",
        static_cast<const void*>(this),_corpseArticulation->hingeCount,_automaticCorpsePose->collisionFamily,_shape->Name(),
        _automaticCorpsePose->initialSpeed.X(),_automaticCorpsePose->initialSpeed.Y(),_automaticCorpsePose->initialSpeed.Z(),
        _automaticCorpsePose->initialAngularVelocity.X(),_automaticCorpsePose->initialAngularVelocity.Y(),_automaticCorpsePose->initialAngularVelocity.Z());
    return true;
}

bool Man::AutomaticRagdollSupported() const
{
    if (!_automaticCorpsePose || !_corpseArticulation || !_corpseArticulation->Ready() || !GLandscape) return false;
    for (auto body : _corpseArticulation->bodies)
    {
        Vector3 support;
        if (!_corpseArticulation->world->GetArticulatedGroundSupport(body,support)) continue;
        const float nativeY = GLandscape->RoadSurfaceY(support+VUp*.5f);
        if (std::isfinite(nativeY) && std::abs(nativeY-support.Y()) <= .025f) return true;
    }
    return false;
}

bool Man::FallBackAutomaticRagdoll(const char* reason)
{
    const Vector3 speed = _automaticCorpsePose ? _automaticCorpsePose->lastSpeed : VZero;
    const Vector3 angular = _automaticCorpsePose ? _automaticCorpsePose->lastAngularVelocity : VZero;
    LOG_WARN(World,"AUTORAGDOLL: authored falling fallback owner={} reason={} noAirborneFreeze=1",static_cast<const void*>(this),reason);
    ClearCorpsePose();
    _speed = speed.IsFinite() ? speed : VZero;
    _angVelocity = angular.IsFinite() ? angular : VZero;
    _landContact = false; _freeFallUntil = Glob.time+.3f; _lastMovementTime = Glob.time;
    IsMoved();
    return false;
}

bool Man::FreezeAutomaticRagdoll()
{
    if (!_automaticCorpsePose || !_corpseArticulation || !_automaticCorpsePose->Valid()) return false;
    if (!_automaticCorpsePose->frozen && !AutomaticRagdollSupported()) return false;
    if (!_automaticCorpsePose->frozen)
        for (auto body : _corpseArticulation->bodies)
        {
            Physics::BodyMotion motion;
            if (!_corpseArticulation->world->GetBodyMotion(body,motion) || !motion.linearVelocity.IsFinite() ||
                !motion.angularVelocity.IsFinite() || motion.linearVelocity.Size() > .3f || motion.angularVelocity.Size() > .6f)
                return false;
        }
    _corpseArticulation->ReleaseHandles();
    _corpseArticulation->state = CorpseSolverState::Frozen;
    _automaticCorpsePose->pending = false; _automaticCorpsePose->frozen = true;
    if (GScene) GScene->GetShadowCache().ShadowChanged(this);
    LOG_INFO(World,"AUTORAGDOLL: frozen owner={} simulatedSeconds={} maximumTravel={} handles=0 supportedGround=1",
        static_cast<const void*>(this),_automaticCorpsePose->simulatedSeconds,_corpseArticulation->maximumTravel);
    return true;
}

bool Man::WakeAutomaticRagdoll(RString& refusal)
{
    refusal="frozen-runtime-recipe-required";
    if (!_automaticCorpsePose || !_automaticCorpsePose->frozen || !_corpseArticulation ||
        !_corpseArticulation->wakeRecipe || !UseCorpsePose()) return false;
    const Vector3 linear=_impulseForce*(GetInvMass()*Rigid());
    const Vector3 angular=(Orientation()*InvInertia()*GetInvTransform().Orientation())*(_impulseTorque*Rigid());
    if(!linear.IsFinite() || !angular.IsFinite() || linear.Size()>15 || angular.Size()>20)
    { refusal="bounded-hit-motion-required"; return false; }
    auto& pose=*_automaticCorpsePose; auto& state=*_corpseArticulation;
    const auto water=GRainWater().At(Position().X(),Position().Z());
    if (water.valid && water.depth>.003f) { refusal="dry-frozen-support-required"; return false; }
    bool used[4]={};
    for(const auto& owner:g_automaticCorpses)
        if(Man* man=owner.GetLink();man && man!=this && man->_automaticCorpsePose && !man->_automaticCorpsePose->frozen)
        { const int slot=-man->_automaticCorpsePose->collisionFamily-2; if(slot>=0 && slot<4) used[slot]=true; }
    int slot=0; while(slot<4 && used[slot]) ++slot;
    if(slot==4) { refusal="active-corpse-budget"; return false; }
    // Rebase against the ACTUAL retained consumers; a wake never selects a
    // standing/authored death animation or changes the visible frozen pose.
    if(!state.Wake(-slot-2)) { refusal="stale-or-invalid-frozen-solver-recipe"; return false; }
    for(auto& level:pose.levels)
    { level.authoredPalette=level.palette; level.authoredPoints=level.points; level.authoredProxies=level.proxies; }
    pose.capturedRoot=pose.root; pose.pending=false; pose.frozen=false;
    pose.initialSpeed=pose.initialAngularVelocity=VZero;
    pose.lastSpeed=pose.lastAngularVelocity=VZero; pose.quietSeconds=pose.simulatedSeconds=0;
    pose.collisionFamily=state.collisionFamily;
    ++state.wakeCount;
    if(GScene) GScene->GetShadowCache().ShadowChanged(this);
    LOG_INFO(World,"AUTORAGDOLL: awakened owner={} actualFrozenPoseRetained=1 bodies=11 joints=10 family={}",static_cast<const void*>(this),state.collisionFamily);
    refusal=""; return true;
}

bool Man::UpdateAutomaticRagdoll(float deltaT)
{
    if (!_automaticCorpsePose || !_corpseArticulation || !UseCorpsePose()) return false;
    if (_automaticCorpsePose->frozen)
    {
        if (_impulseForce.SquareSize()>0 || _impulseTorque.SquareSize()>0)
        {
            RString refusal;
            if (!WakeAutomaticRagdoll(refusal))
            {
                LOG_INFO(World,"AUTORAGDOLL: frozen hit retained owner={} reason={}",static_cast<const void*>(this),refusal.Data());
                _impulseForce=_impulseTorque=VZero;
            }
            else TransferAutomaticRagdollImpulse();
            // Consume in the SAME wake call before any later base simulation.
            // Solver velocities change; retained visible consumers do not.
        }
        return true;
    }
    auto& state = *_corpseArticulation;
    auto& pose = *_automaticCorpsePose;
    std::array<Physics::BodyMotion,11> motions;
    bool quiet = true, identity = true;
    for (int part = 0; part < 11; ++part)
    {
        auto& motion = motions[part];
        if (!state.world->GetBodyMotion(state.bodies[part],motion) || !motion.position.IsFinite() ||
            !motion.axisX.IsFinite() || !motion.axisY.IsFinite() || !motion.axisZ.IsFinite() ||
            !motion.linearVelocity.IsFinite() || !motion.angularVelocity.IsFinite())
            return FallBackAutomaticRagdoll("invalid-body-readback");
        state.maximumTravel = std::max(state.maximumTravel,(motion.position-state.initial[part].Position()).Size());
        quiet &= !motion.awake || (motion.linearVelocity.Size() < .06f && motion.angularVelocity.Size() < .12f);
        identity &= motion.position == state.initial[part].Position() && motion.axisX == state.initial[part].DirectionAside() &&
            motion.axisY == state.initial[part].DirectionUp() && motion.axisZ == state.initial[part].Direction();
    }
    pose.lastSpeed = motions[0].linearVelocity;
    pose.lastAngularVelocity = motions[0].angularVelocity;
    Matrix4 root = pose.capturedRoot;
    if (!identity) root.SetPosition(root.Position()+(motions[0].position-state.initial[0].Position()));
    std::array<Matrix4,11> parts;
    for (int part = 0; part < 11; ++part)
    {
        const auto& motion = motions[part];
        Matrix4 current = MIdentity;
        current.SetDirectionAside(motion.axisX); current.SetDirectionUp(motion.axisY);
        current.SetDirection(motion.axisZ); current.SetPosition(motion.position);
        parts[part] = identity ? MIdentity : root.InverseRotation()*current*
            state.initial[part].InverseRotation()*pose.capturedRoot;
    }
    std::vector<Matrix4> deltas;
    for (int part : state.boneParts) deltas.push_back(parts[part]);
    CorrectedCorpsePose candidate = pose;
    candidate.root = root;
    if (!candidate.Apply(deltas)) return FallBackAutomaticRagdoll("invalid-affine-pose");
    bool first = true; float radius = 0;
    Vector3 lo = VZero, hi = VZero;
    const auto& weights = Type()->GetWeights();
    for (int level = 0; level < _shape->NLevels(); ++level)
    {
        Shape* shape = _shape->Level(level);
        MATRIX_4_ARRAY(palette,128);
        palette.Resize(33);
        for (int bone = 0; bone < 33; ++bone) palette[bone] = candidate.levels[level].palette[bone];
        for (int p = 0; p < shape->NPos(); ++p)
        {
            const Vector3 point = AnimationRT::ApplyMatricesPoint(weights[level],_shape,level,palette,p);
            if (!point.IsFinite() || point.Size() > 8) return FallBackAutomaticRagdoll("invalid-render-bounds");
            if (first) { lo = hi = point; first = false; }
            for (int axis = 0; axis < 3; ++axis)
            { lo[axis] = std::min(lo[axis],point[axis]); hi[axis] = std::max(hi[axis],point[axis]); }
            radius = std::max(radius,point.Size());
            const Vector3 cpu = candidate.levels[level].points[p];
            for (int axis = 0; axis < 3; ++axis)
            { lo[axis] = std::min(lo[axis],cpu[axis]); hi[axis] = std::max(hi[axis],cpu[axis]); }
            radius = std::max(radius,cpu.Size());
        }
    }
    if (first) return FallBackAutomaticRagdoll("missing-render-bounds");
    candidate.minimum = lo; candidate.maximum = hi; candidate.radius = radius;
    pose = std::move(candidate); state.minimum = lo; state.maximum = hi; state.radius = radius;
    if(state.wakeRecipe) for(int part=0;part<11;++part)
    {
        auto& retained=state.retainedTransforms[part]; retained=MIdentity;
        retained.SetDirectionAside(motions[part].axisX); retained.SetDirectionUp(motions[part].axisY);
        retained.SetDirection(motions[part].axisZ); retained.SetPosition(motions[part].position);
    }
    state.object = root; ++state.updates;
    if (Transform().Position() != root.Position())
    {
        _automaticCorpseMoving = true;
        Move(root);
        _automaticCorpseMoving = false;
    }
    _lastMovementTime = Glob.time;
    IsMoved();
    const float elapsed = std::isfinite(deltaT) ? std::clamp(deltaT,0.0f,.25f) : 0;
    pose.simulatedSeconds += elapsed;
    const bool supported = AutomaticRagdollSupported();
    pose.quietSeconds = quiet && supported ? pose.quietSeconds+elapsed : 0;
    // Distance from the initial corpse is NOT a pose bound: blast translation
    // can exceed 12 m while the rebased skin/joints remain valid. Neither time,
    // travel nor a ballistic apex establishes a safe frozen ground pose.
    // StartCorpseArticulation performs an initial consumer readback while
    // pending. Its spawn motion already includes this death impulse; only
    // later independent incoming hits may transfer it here.
    const bool pendingImpulse = !pose.pending && (_impulseForce.SquareSize() > 0 || _impulseTorque.SquareSize() > 0);
    if (!pendingImpulse && CorpseAutomaticRetirementAdmitted(supported,quiet,pose.quietSeconds,pose.simulatedSeconds))
        return FreezeAutomaticRagdoll();
    // Existing aggregate impulses arriving after activation are consumed once.
    if (pendingImpulse) TransferAutomaticRagdollImpulse();
    return true;
}

void Man::TransferAutomaticRagdollImpulse()
{
    auto& state=*_corpseArticulation;
    const Vector3 force=_impulseForce,torque=_impulseTorque;
    const Vector3 linear=force*(GetInvMass()*Rigid());
    const Vector3 angular=(Orientation()*InvInertia()*GetInvTransform().Orientation())*(torque*Rigid());
    float appliedLinear,appliedAngular;
    const int part=state.TransferImpulse(COMPosition(),force,torque,linear,angular,Rigid(),appliedLinear,appliedAngular);
    LOG_INFO(World,"AUTORAGDOLL: actual hit transfer owner={} part={} force=({},{},{}) torque=({},{},{}) wholeLinear={} wholeAngular={} localReadback=({},{}) mode={}",
        static_cast<const void*>(this),part,force.X(),force.Y(),force.Z(),torque.X(),torque.Y(),torque.Z(),
        linear.Size(),angular.Size(),appliedLinear,appliedAngular,part>=0?"local":part==-1?"aggregate":"refused");
    _impulseForce=_impulseTorque=VZero;
    _automaticCorpsePose->quietSeconds=0;
}

LSError Man::SerializeAutomaticCorpsePose(ParamArchive& ar)
{
    if (IS_UNIT_STATUS_BRANCH(ar.GetArVersion()) ||
        (ar.IsLoading() && ar.GetPass() != ParamArchive::PassFirst)) return LSOK;
    if (ar.IsSaving() && (!_automaticCorpsePose || !_corpseArticulation || !UseCorpsePose())) return LSOK;
    if (ar.IsSaving() && !_automaticCorpsePose->frozen && !FreezeAutomaticRagdoll()) return LSOK;
    ParamArchive saved;
    if (!ar.OpenSubclass("opAutomaticCorpse",saved)) return LSOK;
    CorrectedCorpsePose loaded;
    CorrectedCorpsePose& pose = ar.IsSaving() ? *_automaticCorpsePose : loaded;
    const auto reject = [&]() {
        ar.CloseSubclass(saved);
        LOG_WARN(World,"AUTORAGDOLL: refused incompatible frozen save; authored death remains available");
        return LSOK;
    };
    int version = 1;
    RString model = ar.IsSaving() ? _shape->Name() : "";
    RString map = ar.IsSaving() && GLandscape ? GLandscape->GetName() : "";
    PARAM_CHECK(saved.Serialize("version",version,1))
    PARAM_CHECK(saved.Serialize("model",model,1))
    PARAM_CHECK(saved.Serialize("map",map,1))
    if (version != 1 || !_shape || !GLandscape || strcmpi(model,_shape->Name()) != 0 ||
        strcmpi(map,GLandscape->GetName()) != 0) return reject();
    Skeleton* skeleton = Type()->GetWeights().GetName().skeleton.GetTypeRef();
    RString capabilityRefusal;
    if (!StockCorpseCapability(_shape, skeleton, capabilityRefusal))
    {
        LOG_WARN(World,"AUTORAGDOLL: refused frozen model={} reason={}",_shape->Name(),capabilityRefusal.Data());
        return reject();
    }
    int bones = ar.IsSaving() ? int(pose.levels[0].palette.size()) : 0;
    PARAM_CHECK(saved.Serialize("bones",bones,1))
    if (!skeleton || bones != 33 || skeleton->NBones() != bones) return reject();
    for (int bone = 0; bone < bones; ++bone)
    {
        char key[32]; std::snprintf(key,sizeof(key),"bone%d",bone);
        RString name = ar.IsSaving() ? skeleton->GetBone(bone) : RString();
        PARAM_CHECK(saved.Serialize(key,name,1))
        if (strcmp(name,skeleton->GetBone(bone)) != 0) return reject();
    }
    int levels = ar.IsSaving() ? int(pose.levels.size()) : 0;
    PARAM_CHECK(saved.Serialize("levels",levels,1))
    PARAM_CHECK(saved.Serialize("root",pose.root,1))
    PARAM_CHECK(saved.Serialize("minimum",pose.minimum,1))
    PARAM_CHECK(saved.Serialize("maximum",pose.maximum,1))
    PARAM_CHECK(saved.Serialize("radius",pose.radius,1))
    if (levels <= 0 || levels > CorrectedCorpsePose::MaximumLevels || levels != _shape->NLevels() ||
        !pose.root.IsFinite() || std::abs(CorpseMatrixDeterminant(pose.root)-1) > .0001f ||
        !pose.minimum.IsFinite() || !pose.maximum.IsFinite() || !std::isfinite(pose.radius) ||
        pose.radius <= 0 || pose.radius > 8) return reject();
    if (ar.IsLoading()) pose.levels.resize(levels);
    const auto& weights = Type()->GetWeights();
    for (int level = 0; level < levels; ++level)
    {
        char key[32]; std::snprintf(key,sizeof(key),"level%d",level);
        ParamArchive entry;
        if (!saved.OpenSubclass(key,entry)) return reject();
        auto& captured = pose.levels[level];
        Shape* shape = _shape->Level(level);
        int points = ar.IsSaving() ? int(captured.points.size()) : 0;
        PARAM_CHECK(entry.Serialize("points",points,1))
        if (!shape || points <= 0 || points > CorrectedCorpsePose::MaximumPoints || points != shape->NPos())
        { saved.CloseSubclass(entry); return reject(); }
        if (ar.IsLoading())
        { captured.palette.resize(bones); captured.points.resize(points); captured.bindings.resize(points); }
        for (int bone = 0; bone < bones; ++bone)
        {
            std::snprintf(key,sizeof(key),"palette%d",bone);
            PARAM_CHECK(entry.Serialize(key,captured.palette[bone],1))
        }
        for (int point = 0; point < points; ++point)
        {
            std::snprintf(key,sizeof(key),"point%d",point);
            PARAM_CHECK(entry.Serialize(key,captured.points[point],1))
            if (ar.IsLoading())
            {
                const auto& weight = weights[level][point];
                for (int w = 0; w < weight.Size(); ++w)
                {
                    captured.bindings[point].bones.push_back(weight[w].GetSel());
                    captured.bindings[point].weights.push_back(weight[w].GetWeight());
                }
            }
        }
        int proxies = ar.IsSaving() ? int(captured.proxies.size()) : 0;
        PARAM_CHECK(entry.Serialize("proxies",proxies,1))
        int expectedProxies = 0;
        for (int p = 0; p < shape->NProxies(); ++p)
        {
            const auto& proxy = shape->Proxy(p);
            if (proxy.selection >= 0 && proxy.selection < shape->NNamedSel() &&
                shape->NamedSel(proxy.selection).Size() > 0 && GetProxyWeights(level,proxy).Size() > 0) ++expectedProxies;
        }
        if (proxies < 0 || proxies > 64 || proxies != expectedProxies)
        { saved.CloseSubclass(entry); return reject(); }
        if (ar.IsLoading())
        { captured.proxies.resize(proxies); captured.proxySelections.resize(proxies); captured.proxyBones.resize(proxies); }
        for (int p = 0; p < proxies; ++p)
        {
            std::snprintf(key,sizeof(key),"proxySelection%d",p);
            PARAM_CHECK(entry.Serialize(key,captured.proxySelections[p],1))
            std::snprintf(key,sizeof(key),"proxyBone%d",p);
            PARAM_CHECK(entry.Serialize(key,captured.proxyBones[p],1))
            std::snprintf(key,sizeof(key),"proxyMatrix%d",p);
            PARAM_CHECK(entry.Serialize(key,captured.proxies[p],1))
            const int selection = captured.proxySelections[p];
            bool actualProxy = false;
            for (int i = 0; i < shape->NProxies(); ++i)
                if (shape->Proxy(i).selection == selection) actualProxy = true;
            if (selection < 0 || selection >= shape->NNamedSel() || shape->NamedSel(selection).Size() <= 0)
            { saved.CloseSubclass(entry); return reject(); }
            if (!actualProxy) { saved.CloseSubclass(entry); return reject(); }
            const auto& weight = GetSelWeights(level,selection);
            const int proxyBone=StockRigidProxyBone(weight,skeleton);
            if (proxyBone < 0 || proxyBone != captured.proxyBones[p])
            { saved.CloseSubclass(entry); return reject(); }
        }
        if (ar.IsLoading())
        {
            captured.authoredPalette = captured.palette; captured.authoredPoints = captured.points;
            captured.authoredProxies = captured.proxies;
        }
        saved.CloseSubclass(entry);
    }
    ar.CloseSubclass(saved);
    if (ar.IsLoading())
    {
        pose.capturedRoot = pose.root; pose.pending = false; pose.frozen = true;
        if (!pose.Valid()) { LOG_WARN(World,"AUTORAGDOLL: nonfinite or unbounded frozen save refused"); return LSOK; }
        bool first = true; Vector3 lo = VZero, hi = VZero; float radius = 0;
        for (int level = 0; level < levels; ++level)
        {
            MATRIX_4_ARRAY(palette,128); palette.Resize(bones);
            for (int bone = 0; bone < bones; ++bone) palette[bone] = pose.levels[level].palette[bone];
            for (int p = 0; p < int(pose.levels[level].points.size()); ++p)
                for (const Vector3 point : {pose.levels[level].points[p],
                    AnimationRT::ApplyMatricesPoint(weights[level],_shape,level,palette,p)})
                {
                    if (!point.IsFinite() || point.Size() > 8) return LSOK;
                    if (first) { lo = hi = point; first = false; }
                    for (int axis = 0; axis < 3; ++axis)
                    { lo[axis] = std::min(lo[axis],point[axis]); hi[axis] = std::max(hi[axis],point[axis]); }
                    radius = std::max(radius,point.Size());
                }
        }
        if (first || (lo-pose.minimum).Size() > .002f || (hi-pose.maximum).Size() > .002f ||
            std::abs(radius-pose.radius) > .002f) return LSOK;
        pose.minimum = lo; pose.maximum = hi; pose.radius = radius;
        // Base deserialization owns the object's actual spatial registration.
        // Refuse a mismatched root rather than moving a half-loaded entity.
        for (int row = 0; row < 3; ++row) for (int col = 0; col < 4; ++col)
            if (std::abs(Transform()(row,col)-pose.root(row,col)) > .0001f) return LSOK;
        _corpsePoseModel = _shape; _corpsePoseSkeleton = skeleton; _corpsePoseWeights = &Type()->GetWeights();
        _corpsePoseLease = {GWorld,_shape.GetRef(),skeleton,bones,int(_primaryMove.id),int(_secondaryMove.id),
            Glob.time.toInt(),_primaryFactor};
        _automaticCorpsePose = std::make_unique<CorrectedCorpsePose>(std::move(pose));
        _corpseArticulation = std::make_unique<CorpseArticulation>();
        _corpseArticulation->state = CorpseSolverState::Frozen;
        _corpseArticulation->object = _automaticCorpsePose->root;
        _corpseArticulation->minimum = _automaticCorpsePose->minimum;
        _corpseArticulation->maximum = _automaticCorpsePose->maximum;
        _corpseArticulation->radius = _automaticCorpsePose->radius;
        g_automaticCorpses.erase(std::remove_if(g_automaticCorpses.begin(),g_automaticCorpses.end(),
            [&](const OLink<Man>& owner) { return !owner.GetLink() || owner.GetLink() == this; }),g_automaticCorpses.end());
        g_automaticCorpses.emplace_back(this); g_corpsePoseOwner = this;
        if (GScene) GScene->GetShadowCache().ShadowChanged(this);
        LOG_INFO(World,"AUTORAGDOLL: restored validated frozen owner={} levels={} handles=0",static_cast<const void*>(this),levels);
    }
    return LSOK;
}

bool Man::UpdateCorpseArticulation() const
{
    if (_automaticCorpsePose) return const_cast<Man*>(this)->UpdateAutomaticRagdoll(0);
    if (!_corpseArticulation || !UseCorpsePose()) return false;
    auto& state = *_corpseArticulation;
    if (state.state == CorpseSolverState::Frozen) return true; // owned pose, no backend reads
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
    if (std::strcmp(command,"corpse-auto-enable") == 0 || std::strcmp(command,"corpse-auto-disable") == 0)
    {
        if (!GWorld || GWorld->GetMode() != GModeArcade) return "REFUSED corpse-auto-toggle local-single-player-required";
        SetAutomaticInfantryDeathsEnabled(std::strcmp(command,"corpse-auto-enable") == 0);
        return "OK corpse-auto-toggle existing-owners-retained";
    }
    if (std::strcmp(command,"corpse-auto-hit-status") == 0)
    {
        Man* man=g_corpsePoseOwner.GetLink();
        if(!man || !man->_automaticCorpsePose || !man->UseCorpsePose() || !man->_corpseArticulation)
            return "REFUSED corpse-auto-hit-status no-automatic-owner";
        const auto& state=*man->_corpseArticulation;
        char status[256]; std::snprintf(status,sizeof(status),
            "OK corpse-auto-hit-status received=%u wakes=%u transfers=%u lastPart=%d recipe=%d",
            state.impulseEvents,state.wakeCount,state.localTransfers,state.lastImpactPart,int(state.wakeRecipe));
        return status;
    }
    if (std::strcmp(command,"corpse-auto-motion") == 0)
    {
        Man* man = g_corpsePoseOwner.GetLink();
        if (!man || !man->_automaticCorpsePose || !man->UseCorpsePose() || !man->_corpseArticulation)
            return "REFUSED corpse-auto-motion no-automatic-owner";
        const auto& pose = *man->_automaticCorpsePose;
        char status[384];
        std::snprintf(status,sizeof(status),"OK corpse-auto-motion frozen=%d supported=%d time=%.9g travel=%.9g root=(%.9g,%.9g,%.9g) speed=(%.9g,%.9g,%.9g)",
            int(pose.frozen),int(!pose.frozen && man->AutomaticRagdollSupported()),pose.simulatedSeconds,
            man->_corpseArticulation->maximumTravel,pose.root.Position().X(),pose.root.Position().Y(),pose.root.Position().Z(),
            pose.lastSpeed.X(),pose.lastSpeed.Y(),pose.lastSpeed.Z());
        return status;
    }
    if (std::strcmp(command,"corpse-auto-bodymotion") == 0)
    {
        Man* man=g_corpsePoseOwner.GetLink();
        if (!man || !man->_automaticCorpsePose || !man->UseCorpsePose() || !man->_corpseArticulation ||
            man->_automaticCorpsePose->frozen || !man->_corpseArticulation->Ready())
            return "REFUSED corpse-auto-bodymotion no-active-automatic-owner";
        const auto& pose=*man->_automaticCorpsePose; const auto& state=*man->_corpseArticulation;
        bool quiet=true,safe=true; float maximumLinear=0,maximumAngular=0;
        std::string parts;
        for (int part=0;part<11;++part)
        {
            Physics::BodyMotion motion;
            if (!state.world->GetBodyMotion(state.bodies[part],motion) ||
                !motion.linearVelocity.IsFinite() || !motion.angularVelocity.IsFinite())
                return "REFUSED corpse-auto-bodymotion invalid-body-readback";
            const float linear=motion.linearVelocity.Size(),angular=motion.angularVelocity.Size();
            if (!std::isfinite(linear) || !std::isfinite(angular))
                return "REFUSED corpse-auto-bodymotion nonfinite-body-speed";
            quiet &= !motion.awake || (linear<.06f && angular<.12f);
            safe &= linear<=.3f && angular<=.6f;
            maximumLinear=std::max(maximumLinear,linear); maximumAngular=std::max(maximumAngular,angular);
            char item[112]; std::snprintf(item,sizeof(item),"%s(%d,%d,%.9g,%.9g)",part?",":"",part,int(motion.awake),linear,angular);
            parts+=item;
        }
        char prefix[320]; std::snprintf(prefix,sizeof(prefix),
            "OK corpse-auto-bodymotion supported=%d time=%.9g quietSeconds=%.9g quiet=%d freezeSafe=%d parts=11 maximumLinear=%.9g maximumAngular=%.9g bodies=",
            int(man->AutomaticRagdollSupported()),pose.simulatedSeconds,pose.quietSeconds,int(quiet),int(safe),maximumLinear,maximumAngular);
        return RString(prefix)+RString(parts.c_str());
    }
    if (std::strcmp(command,"corpse-auto-status") == 0)
    {
        int active = 0, frozen = 0, pending = 0;
        for (const auto& owner : g_automaticCorpses)
            if (Man* man = owner.GetLink(); man && man->_automaticCorpsePose && man->UseCorpsePose())
            {
                if (man->_automaticCorpsePose->pending) ++pending;
                else if (man->_automaticCorpsePose->frozen) ++frozen;
                else ++active;
            }
        char result[192]; std::snprintf(result,sizeof(result),"OK corpse-auto-status enabled=%d active=%d frozen=%d pending=%d activeBudget=4 retainedBudget=64",
            int(AutomaticRagdollEnabled()),active,frozen,pending);
        return result;
    }
    if (std::strncmp(command,"corpse-auto-select:",19) == 0)
    {
        char* end = nullptr; const long index = std::strtol(command+19,&end,10);
        if (!end || *end || index < 0) return "REFUSED corpse-auto-select invalid-index";
        long found = 0;
        for (const auto& owner : g_automaticCorpses)
            if (Man* man = owner.GetLink(); man && man->_automaticCorpsePose && man->UseCorpsePose())
                if (found++ == index) { g_corpsePoseOwner = man; return "OK corpse-auto-select selected"; }
        return "REFUSED corpse-auto-select no-owner";
    }
    if (std::strcmp(command,"corpse-frozen-world-retire") == 0)
    {
        Man* man = g_corpsePoseOwner.GetLink();
        auto* world = Physics::GetPhysicsWorld();
        if (!GWorld || GWorld->GetMode() != GModeArcade || GWorld->GetAcceleratedTime() != 0 || !man || !man->IsLocal() ||
            !man->UseCorpsePose() || !man->_corpseArticulation ||
            man->_corpseArticulation->state != CorpseSolverState::Frozen ||
            !world || !world->IsCreated() || world != g_corpsePreparedWorld ||
            world->Generation() != g_corpsePreparedEpoch ||
            world->TerrainMutationSerial() != g_corpsePreparedTerrainSerial)
            return "REFUSED corpse-frozen-world-retire owned-bounded-fixture-required";
        const auto stats = world->GetStats();
        if (stats.bodies || stats.probes || stats.modelsRegistered || stats.articulatedBodies || stats.articulatedJoints ||
            stats.solverBodies != (stats.kinematicProxyRegistered ? 2u : 1u) || stats.solverJoints ||
            !stats.terrainRegistered || stats.terrainWidth != 129 || stats.terrainHeight != 129 || stats.terrainCellSize != .25f ||
            stats.terrainOriginX != g_corpsePreparedOriginX || stats.terrainOriginZ != g_corpsePreparedOriginZ)
            return "REFUSED corpse-frozen-world-retire empty-unchanged-bounded-fixture-required";
        // A positively identified separately owned test player proxy is the sole
        // allowable extra body. Remove it while paused, without changing settings.
        if (stats.kinematicProxyRegistered) world->SetKinematicProxy(VZero,.35f,0);
        const auto empty = world->GetStats();
        if (empty.solverBodies != 1 || empty.solverJoints || empty.kinematicProxyRegistered ||
            empty.bodies || empty.probes || empty.modelsRegistered || empty.articulatedBodies || empty.articulatedJoints)
            return "REFUSED corpse-frozen-world-retire raw-empty-backend-not-established";
        const auto before = world->Generation();
        world->Destroy(); // Change the real epoch; do not clear the owner first.
        char status[384]; std::snprintf(status,sizeof(status),
            "OK corpse-frozen-world-retire epochBefore=%llu epochAfter=%llu bodies=0 joints=0 rawBodiesBefore=%u rawBodiesBeforeRetire=1 proxyRemoved=%d fixture=bounded-empty",
            static_cast<unsigned long long>(before),static_cast<unsigned long long>(world->Generation()),
            stats.solverBodies,int(stats.kinematicProxyRegistered));
        return status;
    }
    if (std::strcmp(command,"corpse-freeze") == 0 || std::strcmp(command,"corpse-frozen-status") == 0)
    {
        Man* man = g_corpsePoseOwner.GetLink();
        if (!man || !man->UseCorpsePose() || !man->_corpseArticulation)
            return "REFUSED corpse-freeze no-owned-articulated-pose";
        auto& state = *man->_corpseArticulation;
        if (std::strcmp(command,"corpse-freeze") == 0)
        {
            // Retain the last fully validated consumed pose exactly as it is.
            // Do not sample a later physics frame, reblend or reset its deltas.
            if (man->_automaticCorpsePose)
            {
                if (!man->FreezeAutomaticRagdoll()) return "REFUSED corpse-freeze automatic-supported-low-motion-required";
            }
            else
            {
                if (!state.Freeze()) return "REFUSED corpse-freeze valid-simulating-pose-required";
                if (GScene) GScene->GetShadowCache().ShadowChanged(man);
            }
        }
        if (state.state != CorpseSolverState::Frozen) return "REFUSED corpse-frozen-status no-frozen-pose";
        char status[512];
        std::snprintf(status,sizeof(status),"OK corpse-frozen-status entity=%p bodies=0 joints=0 epoch=%llu updates=%d capturedMs=%d nowMs=%d policy=explicit-owned-pose",
            static_cast<void*>(man),static_cast<unsigned long long>(state.epoch),state.updates,
            man->_corpsePoseLease.capturedMs,Glob.time.toInt());
        return status;
    }
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
            g_corpsePreparedWorld = world; g_corpsePreparedEpoch = world->Generation();
            g_corpsePreparedTerrainSerial = world->TerrainMutationSerial();
            g_corpsePreparedOriginX = originX; g_corpsePreparedOriginZ = originZ;
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
        if (!man || !man->UseCorpsePose() || !man->_corpseArticulation ||
            man->_corpseArticulation->state != CorpseSolverState::Simulating)
            return "REFUSED corpse-articulation-status no-articulated-corpse";
        auto& state = *man->_corpseArticulation;
        char status[512];
        std::snprintf(status,sizeof(status),"OK corpse-articulation-status entity=%p bodies=11 joints=10 hinges=%d epoch=%llu updates=%d maximumTravel=%.9g minimumClearance=%.9g",
            static_cast<void*>(man),state.hingeCount,static_cast<unsigned long long>(state.epoch),state.updates,state.maximumTravel,state.minimumClearance);
        return status;
    }
    if (std::strcmp(command, "corpse-articulate") == 0 || std::strcmp(command, "corpse-articulate-hinges") == 0 || std::strcmp(command, "corpse-impulse") == 0)
    {
        Man* man = g_corpsePoseOwner.GetLink();
        if (!man || !man->UseCorpsePose()) return "REFUSED corpse-articulate captured-authored-corpse-required";
        if (std::strcmp(command, "corpse-impulse") == 0)
        {
            if (!man->_corpseArticulation || man->_corpseArticulation->state != CorpseSolverState::Simulating)
                return "REFUSED corpse-impulse no-articulated-corpse";
            auto& state = *man->_corpseArticulation;
            if (!state.Ready()) return "REFUSED corpse-impulse physics-world-retired";
            state.world->ApplyImpulse(state.bodies[1], state.initial[1].Position(), Vector3(0,12,0));
            return "OK corpse-impulse chest=1 impulseY=12";
        }
        RString reason;
        if (!man->StartCorpseArticulation(reason, std::strcmp(command, "corpse-articulate-hinges") == 0)) return RString("REFUSED corpse-articulate ")+reason;
        char result[512];
        std::snprintf(result, sizeof(result),
            "OK corpse-articulate entity=%p solver=%s bodies=11 joints=10 epoch=%llu minimumClearance=%.9g expiresMs=30000 default=off%s",
            static_cast<void*>(man), man->_corpseArticulation->world->BackendName(),
            static_cast<unsigned long long>(man->_corpseArticulation->epoch), man->_corpseArticulation->minimumClearance,
            man->_corpseArticulation->hingeCount == 4 ? " hinges=4" : "");
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
        if (man->_corpseArticulation && man->_corpseArticulation->state == CorpseSolverState::Frozen)
            return CorpsePoseProbe("corpse-frozen-status");
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
    if (_automaticCorpsePose && UseCorpsePose() && level >= 0 && level < int(_automaticCorpsePose->levels.size()))
    {
        const auto& pose = _automaticCorpsePose->levels[level];
        for (size_t p = 0; p < pose.proxySelections.size(); ++p)
            if (pose.proxySelections[p] == selection) { mat = pose.proxies[p]; return; }
    }
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
    if (_automaticCorpsePose && UseCorpsePose() && level >= 0 && level < int(_automaticCorpsePose->levels.size()) &&
        index >= 0 && index < int(_automaticCorpsePose->levels[level].points.size()))
        return _automaticCorpsePose->levels[level].points[index];
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
    if (_corpseArticulation) return _corpseArticulation->state == CorpseSolverState::Frozen;
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
