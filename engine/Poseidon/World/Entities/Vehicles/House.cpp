#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Core/Application.hpp>

#include <Poseidon/World/Entities/Vehicles/House.hpp>
#include <Poseidon/World/Effects/BuildingInterior.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/Graphics/Rendering/Effects/Smokes.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/Game/UiActions.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>

#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <math.h>
#include <stdio.h>
#include <cstdlib>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/BoolArray.hpp>
#include <Poseidon/Foundation/Containers/StreamArray.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Memory/FastAlloc.hpp>
#include <Poseidon/Foundation/Memory/MemAlloc.hpp>
#include <Poseidon/Foundation/platform.hpp>

using namespace Poseidon;
namespace Poseidon
{
using Poseidon::Foundation::MemAllocSS;
using Poseidon::Foundation::MStorage;

} // namespace Poseidon
namespace Poseidon::Foundation
{
template class Ref<LightPointOnVehicle>;
} // namespace Poseidon::Foundation
BuildingType::BuildingType(const ParamEntry* param) : base(param)
{
    _scopeLevel = 1;
}

void BuildingType::Load(const ParamEntry& par)
{
    base::Load(par);
    _coefInside = par >> "coefInside";
    _coefInsideHeur = par >> "coefInsideHeur";
    const ParamEntry* remove = par.FindEntry("removeOnDestruction");
    _removeOnDestruction = remove && int(*remove) != 0;
}

void BuildingType::InitShape()
{
    if (!_shape)
    {
        return;
    }
    _scopeLevel = 2;

    _exits.Clear();
    _positions.Clear();

    Shape* paths = _shape->PathsLevel();
    if (paths)
    {
        char buffer[256];
        for (int i = 1; true; i++)
        {
            snprintf(buffer, sizeof(buffer), "In%d", i);
            int index = paths->PointIndex(buffer);
            if (index < 0)
            {
                break;
            }
            int pt = paths->VertexToPoint(index);
            DoAssert(pt >= 0);
            DoAssert(paths->PointToVertex(pt) >= 0);
            _exits.Add(pt);
        }
        for (int i = 1; true; i++)
        {
            snprintf(buffer, sizeof(buffer), "Pos%d", i);
            int index = paths->PointIndex(buffer);
            if (index < 0)
            {
                break;
            }
            int pt = paths->VertexToPoint(index);
            DoAssert(pt >= 0);
            DoAssert(paths->PointToVertex(pt) >= 0);
            _positions.Add(pt);
        }
        _connections.Resize(0);
        for (Offset f = paths->BeginFaces(), e = paths->EndFaces(); f < e; paths->NextFace(f))
        {
            const Poly& face = paths->Face(f);
            int n = face.N();
            if (n < 2)
            {
                continue;
            }
            for (int v = 0; v < n; v++)
            {
                int index = paths->VertexToPoint(face.GetVertex(v));
                DoAssert(index >= 0);
                DoAssert(paths->PointToVertex(index) >= 0);
                if (_connections.Size() <= index)
                {
                    _connections.Resize(index + 1);
                }
                FindArray<int>& array = _connections[index];
                if (v == 0)
                {
                    int pindex = paths->VertexToPoint(face.GetVertex(n - 1));
                    DoAssert(pindex >= 0);
                    DoAssert(paths->PointToVertex(pindex) >= 0);
                    array.AddUnique(pindex);
                }
                else
                {
                    int pindex = paths->VertexToPoint(face.GetVertex(v - 1));
                    DoAssert(pindex >= 0);
                    DoAssert(paths->PointToVertex(pindex) >= 0);
                    array.AddUnique(pindex);
                }
                if (v == n - 1)
                {
                    int pindex = paths->VertexToPoint(face.GetVertex(0));
                    DoAssert(pindex >= 0);
                    DoAssert(paths->PointToVertex(pindex) >= 0);
                    array.AddUnique(pindex);
                }
                else
                {
                    int pindex = paths->VertexToPoint(face.GetVertex(v + 1));
                    DoAssert(pindex >= 0);
                    DoAssert(paths->PointToVertex(pindex) >= 0);
                    array.AddUnique(pindex);
                }
            }
        }
    }

    const ParamEntry& par = (*_par);

    Shape* mem = _shape->MemoryLevel();
    _ladders.Clear();
    if (mem)
    {
        const ParamEntry& ladders = par >> "ladders";
        for (int i = 0; i < ladders.GetSize(); i++)
        {
            RStringB ladderBottomName = ladders[i][0];
            RStringB ladderTopName = ladders[i][1];
            Ladder& ladder = _ladders.Append();
            ladder._top = mem->PointIndex(ladderTopName);
            ladder._bottom = mem->PointIndex(ladderBottomName);
        }
    }

    for (int level = 0; level < _shape->NLevels(); level++)
    {
        WeaponProxy& info = _proxies[level];
        Shape* shape = _shape->LevelOpaque(level);

        // convert shape proxies to my proxies
        for (int i = 0; i < shape->NProxies(); i++)
        {
            const ProxyObject& proxy = shape->Proxy(i);
            Object* obj = proxy.obj;
            const VehicleNonAIType* type = obj->GetVehicleType();
            if (!type)
            {
                continue;
            }
            RString simulation = type->_simName;
            if (stricmp(simulation, "proxyweapon") == 0 || stricmp(simulation, "proxysecweapon") == 0)
            {
                info.obj = obj;
                info.selection = proxy.selection;
                break;
            }
        }
    }

    base::InitShape();
}

struct BRoadNode
{
    BRoadNode* parent;
    int index;
    Vector3 pos;
    float cost;
    float heur;
    bool open;

    USE_FAST_ALLOCATOR
};

DEFINE_FAST_ALLOCATOR(BRoadNode);

bool operator<(BRoadNode& a, BRoadNode& b)
{
    return a.cost + a.heur < b.cost + b.heur;
}

bool operator<=(BRoadNode& a, BRoadNode& b)
{
    return a.cost + a.heur <= b.cost + b.heur;
}

typedef HeapArray<BRoadNode*, MemAllocSS> BRoadOpenList;

class BRoadNodeContainer : public AutoArray<SRef<BRoadNode>>
{
  public:
    BRoadNode* Find(int index) const;
};

BRoadNode* BRoadNodeContainer::Find(int index) const
{
    int i, n = Size();
    for (i = 0; i < n; i++)
    {
        BRoadNode* node = Get(i);
        if (node->index == index)
        {
            return node;
        }
    }
    return nullptr;
}

inline float BRoadCost(Vector3Par from, Vector3Par to)
{
    return to.Distance(from);
}

const V3& BuildingType::GetPosition(int index) const
{
    PoseidonAssert(_shape);
    Shape* paths = _shape->PathsLevel();
    PoseidonAssert(paths);
    return paths->Pos(paths->PointToVertex(index));
}

#define DIAGS 0
bool BuildingType::SearchPath(int from, int to, HousePathArrayIndexed& path) const
{
    // maintain "open" list
    // (close list?)

    // search from...to given points
    BRoadNodeContainer container;

    BRoadOpenList openList;
    static StaticStorage<BRoadNode*> openListStorage;
    // use static storage to contain typical searches
    openList.SetStorage(openListStorage.Init(256));
    BRoadNode *best = nullptr, *cur;

#if DIAGS
    LOG_DEBUG(Physics, "Searching path in building from {} to {}", from, to);
#endif

    Vector3Val startPos = GetPosition(from);

    cur = new BRoadNode;
    container.Add(cur);
    cur->parent = nullptr;
    cur->index = to;
    cur->pos = GetPosition(to);
    cur->cost = 0;
    cur->heur = BRoadCost(cur->pos, startPos);
    cur->open = true;
    openList.HeapInsert(cur);

    // A* algorithm
    int iter = 0;
    while (true) // search cycle
    {
        iter++;
        bool ok = openList.HeapRemoveFirst(best);
        if (!ok)
        {
#if DIAGS
            LOG_DEBUG(Physics, "  path not found at {} iters", iter);
#endif
            path.Clear();
            return false;
        }

#if DIAGS >= 2
        LOG_DEBUG(Physics, "  best = {}", best->index);
#endif

        if (best->index == from)
        {
            // best is the first node of result path
            goto PathFound;
        }

        best->open = false;

        const FindArray<int>& array = _connections[best->index];
        int i, n = array.Size();
        for (i = 0; i < n; i++) // generate successors
        {
            int index = array[i];
            Vector3Val pos = GetPosition(index);
            float cost = best->cost + BRoadCost(best->pos, pos);

            cur = container.Find(index);
#if DIAGS >= 2
            LOG_DEBUG(Physics, "   current = {} (found {})", index, cur != nullptr);
#endif
            if (cur)
            {
                //			heuristic doesn't change
                if (cur->open && (cost < cur->cost))
                {
                    cur->parent = best;
                    cur->cost = cost;
                    openList.HeapUpdateUp(cur);
#if DIAGS >= 2
                    LOG_DEBUG(Physics, "    update cost {:.3f}", cost);
#endif
                }
                else
                {
                    // there is better path into item
#if DIAGS >= 2
                    LOG_DEBUG(Physics, "    worse cost {:.3f}", cost);
#endif
                    continue;
                }
            }
            else
            {
                cur = new BRoadNode;
                container.Add(cur);
                cur->parent = best;
                cur->index = index;
                cur->pos = pos;
                cur->cost = cost;
                cur->heur = BRoadCost(pos, startPos);
                cur->open = true;
                openList.HeapInsert(cur);
#if DIAGS >= 2
                LOG_DEBUG(Physics, "    cost {:.3f}, heuristic {:.3f}", cost, cur->heur);
#endif
            }
        } // end of generate successors
    } // end of search cycle

PathFound:
    // build result list
    // best is the first node of result path
#if DIAGS
    LOG_DEBUG(Physics, "  path found at {} iters:", iter);
#endif

    path.Clear();
    cur = best;
    while (cur)
    {
        path.Add(cur->index);
#if DIAGS
        LOG_DEBUG(Physics, "  - {} ({:.1f}, {:.1f}, {:.1f})", cur->index, cur->pos.X(), cur->pos.Y(), cur->pos.Z());
#endif
        cur = cur->parent;
    }
    return true;
}

int BuildingType::FindNearestExit(Vector3Par pos, Vector3& ret) const
{
    int best = -1;
    float dist2Min = FLT_MAX;
    for (int i = 0; i < _exits.Size(); i++)
    {
        Vector3Val pt = GetPosition(_exits[i]);
        float dist2 = pt.Distance2(pos);
        if (dist2 < dist2Min)
        {
            dist2Min = dist2;
            best = _exits[i];
            ret = pt;
        }
    }
    return best;
}

int BuildingType::FindNearestPosition(Vector3Par pos, Vector3& ret) const
{
    int best = -1;
    float dist2Min = FLT_MAX;
    for (int i = 0; i < _positions.Size(); i++)
    {
        Vector3Val pt = GetPosition(_positions[i]);
        float dist2 = pt.Distance2(pos);
        if (dist2 < dist2Min)
        {
            dist2Min = dist2;
            best = _positions[i];
            ret = pt;
        }
    }
    return best;
}

Vector3 IPaths::GetPosition(int index) const
{
    const Object* object = GetObject();
    int paths = object->GetShape()->FindPaths();
    PoseidonAssert(paths >= 0);
    Shape* pathsLevel = object->GetShape()->PathsLevel();
    int vIndex = pathsLevel->PointToVertex(index);
    return object->AnimatePoint(paths, vIndex);
}

bool IPaths::SearchPath(int from, int to, HousePathArray& path) const
{
    const BuildingType* type = GetBType();
    HOUSE_PATH_ARRAY_INDEXED(indexed, 128);

#if DIAGS
    LOG_DEBUG(Physics, "Searching path in {}", (const char*)GetObject()->GetDebugName());
#endif

    if (type->SearchPath(from, to, indexed))
    {
        const Object* object = GetObject();
        int level = object->GetShape()->FindPaths();
        path.Resize(indexed.Size());
        for (int i = 0; i < path.Size(); i++)
        {
            int index = indexed[i];
            int vIndex = object->GetShape()->PathsLevel()->PointToVertex(index);
            path[i].pos = object->AnimatePoint(level, vIndex);
            path[i].index = index;
        }
        return true;
    }
    return false;
}

int IPaths::FindNearestExit(Vector3Par pos, Vector3& ret) const
{
    Vector3 posModel, retModel;

    const Object* object = GetObject();
    Matrix4Val invTrans = object->GetInvTransform();
    posModel = invTrans.FastTransform(pos);

    const BuildingType* type = GetBType();
    int index = type->FindNearestExit(posModel, retModel);
    if (index < 0)
    {
        ret = VZero;
    }
    else
    {
        int paths = object->GetShape()->FindPaths();
        int vIndex = object->GetShape()->PathsLevel()->PointToVertex(index);
        ret = object->AnimatePoint(paths, vIndex);
    }
    return index;
}

int IPaths::FindNearestPosition(Vector3Par pos, Vector3& ret) const
{
    Vector3 posModel, retModel;

    const Object* object = GetObject();
    Matrix4Val invTrans = object->GetInvTransform();
    posModel = invTrans.FastTransform(pos);

    const BuildingType* type = GetBType();
    int index = type->FindNearestPosition(posModel, retModel);
    if (index < 0)
    {
        ret = VZero;
    }
    else
    {
        int paths = object->GetShape()->FindPaths();
        int vIndex = object->GetShape()->PathsLevel()->PointToVertex(index);
        ret = object->AnimatePoint(paths, vIndex);
    }
    return index;
}

int IPaths::FindNearestPoint(Vector3Par pos, Vector3& ret, float maxDist2) const
{
    Vector3 posModel, retModel;
    const Object* object = GetObject();
    Matrix4Val invTrans = object->GetInvTransform();
    posModel = invTrans.FastTransform(pos);

    const BuildingType* type = GetBType();

    int paths = object->GetShape()->FindPaths();
    Shape* pathsLevel = object->GetShape()->PathsLevel();

    int index = -1;

    int best1 = -1, best2 = -1;
    float dist2Min = maxDist2;
    int n = type->_connections.Size();
    for (int i = 0; i < n; i++)
    {
        // note: i is "point" index
        // specific point index need not be used
        int vIndex = pathsLevel->PointToVertex(i);
        if (vIndex < 0)
        {
            continue;
        }
        const FindArray<int>& array = type->_connections[i];
        int m = array.Size();
        if (m <= 0)
        {
            continue;
        }
        Vector3 b = object->AnimatePoint(paths, vIndex);
        Vector3 p = pos - b;
        for (int j = 0; j < m; j++)
        {
            int index = array[j];
            Vector3 e = GetPosition(index) - b;
            float t = (e * p) / e.SquareSize();
            saturate(t, 0, 1);
            Vector3 nearest = b + t * e;
            float dist2 = nearest.Distance2(pos);
            if (dist2 < dist2Min)
            {
                dist2Min = dist2;
                best1 = i;
                best2 = index;
            }
        }
    }
    if (best1 < 0 || best2 < 0)
    {
        return -1;
    }
    Vector3 nearest1 = GetPosition(best1);
    Vector3 nearest2 = GetPosition(best2);
    if (nearest2.Distance2(pos) < nearest1.Distance2(pos))
    {
        ret = nearest2;
        index = best2;
    }
    else
    {
        ret = nearest1;
        index = best1;
    }

    if (index < 0)
    {
        ret = VZero;
    }
    else
    {
        int vIndex = pathsLevel->PointToVertex(index);
        int paths = object->GetShape()->FindPaths();
        ret = object->AnimatePoint(paths, vIndex);
    }
    return index;
}

CameraBuilding::CameraBuilding(VehicleType* name, int id, LODShapeWithShadow* shape) : base(name, nullptr)
{
    SetSimulationPrecision(0.5);
    if (shape)
    {
        _shape = shape;
    }
    PoseidonAssert(_shape);
    _destrType = GetType()->GetDestructType();
    if ((DestructType)_destrType == DestructDefault)
    {
        _destrType = DestructBuilding;
    }
    _static = true;
    SetType(Primary);
    SetID(id);
}

DEFINE_CASTING(Building)

Building::Building(VehicleType* name, int id, LODShapeWithShadow* shape) : base(name)
{
    SetSimulationPrecision(0.5);
    if (shape)
    {
        _shape = shape;
    }
    PoseidonAssert(_shape);
    _destrType = GetType()->GetDestructType();
    if ((DestructType)_destrType == DestructDefault)
    {
        _destrType = DestructBuilding;
    }
    _static = true;
    SetType(Primary);
    SetID(id);

    int n = NPos();
    _locks.Resize(n);
    for (int i = 0; i < n; i++)
    {
        _locks[i] = 0;
    }
}

Matrix4 Building::InsideCamera(CameraType camType) const
{
    return MIdentity;
}

int Building::InsideLOD(CameraType camType) const
{
    return 0;
}

void Building::Destroy(EntityAI* owner, float overkill, float minExp, float maxExp)
{
    // Opt-in standalone fragile panels must not run a house-collapse morph.
    // The normal entity-removal path retires both rendering and collision.
    if (Type()->_removeOnDestruction)
    {
        if (IsLocal()) SetDelete();
        return;
    }
    base::Destroy(owner, overkill, minExp, maxExp);
}

void Building::Simulate(float deltaT, SimulationImportance prec)
{
    if (!_isDead && IsDammageDestroyed())
    {
        _isDead = true;
    }
    if (_isDead)
    {
        SmokeSourceVehicle* smoke = dyn_cast<SmokeSourceVehicle>(GetSmoke());
        if (smoke)
        {
            smoke->Explode();
        }
        NeverDestroy();
    }
    base::Simulate(deltaT, prec);
}

bool Building::IsAnimated(int level) const
{
    // appearence changed with Animate
    return Entity::IsAnimated(level);
}
bool Building::IsAnimatedShadow(int level) const
{
    // shadow changed with Animate
    return Entity::IsAnimated(level);
}

void Building::Animate(int level)
{
    Entity::Animate(level);
}

void Building::Deanimate(int level)
{
    Entity::Deanimate(level);
}

void Building::DrawProxies(int level, ClipFlags clipFlags, const Matrix4& transform, const Matrix4& invTransform,
                           float dist2, float z2, const LightList& lights)
{
    const WeaponProxy& proxy = Type()->_proxies[level];
    if (proxy.IsPresent())
    {
        Object* obj = proxy.obj;

        Matrix4 proxyTransform = obj->Transform();
        AnimateMatrix(proxyTransform, level, proxy.selection);

        // smart clipping par of obj->Draw
        Matrix4Val pTransform = transform * proxyTransform;

        // select shape
        LODShapeWithShadow* pshape = nullptr;
        const WeaponType* weapon = nullptr;
        for (int i = 0; i < GetWeaponCargoSize(); i++)
        {
            weapon = GetWeaponCargo(i);
            if (weapon)
            {
                pshape = weapon->_model;
                break;
            }
        }
        if (!weapon)
        {
            for (int i = 0; i < GetMagazineCargoSize(); i++)
            {
                const Magazine* magazine = GetMagazineCargo(i);
                if (magazine)
                {
                    pshape = magazine->_type->_modelMagazine;
                    break;
                }
            }
        }

        if (pshape)
        {
            // LOD detection
            int level = GScene->LevelFromDistance2(pshape, dist2, pTransform.Scale(), pTransform.Direction(),
                                                   GScene->GetCamera()->Direction());
            if (level != LOD_INVISIBLE)
            {
                // construct FrameWithInverse from transform and invTransform
                Matrix4Val invPTransform = pTransform.InverseScaled();
                Shape* shape = pshape->LevelOpaque(level);

                // remove fire selection
                if (weapon)
                {
                    weapon->_animFire.Hide(pshape, level);
                }

                shape->PrepareTextures(z2, shape->Special());
                shape->Draw(this, lights, ClipAll, shape->Special(), pTransform, invPTransform);

                if (weapon)
                {
                    weapon->_animFire.Unhide(pshape, level);
                }
            }
        }
    }
    else
    {
        // skip Transport DrawProxies
        Object::DrawProxies(level, clipFlags, transform, invTransform, dist2, z2, lights);
    }
}

int Building::GetProxyComplexity(int level, const FrameBase& pos, float dist2) const
{
    return Object::GetProxyComplexity(level, pos, dist2);
}

bool Building::CastShadow() const
{
    return IS_SHADOW_OBJECT;
}

void Building::DrawDiags()
{
    // draw all exits
    if (NExits() > 0 || NPos() > 0)
    {
        LODShapeWithShadow* shape = GScene->Preloaded(SphereModel);
        for (int i = 0; i < NExits(); i++)
        {
            Vector3Val pos = GetPosition(GetExit(i));
            Ref<Object> obj = new ObjectColored(shape, -1);
            obj->SetPosition(pos);
            obj->SetConstantColor(PackedColor(Color(0, 1, 0, 1)));
            GScene->ObjectForDrawing(obj);
        }

        for (int i = 0; i < NPos(); i++)
        {
            Vector3Val pos = GetPosition(IPaths::GetPos(i));
            Ref<Object> obj = new ObjectColored(shape, -1);
            obj->SetPosition(pos);
            obj->SetScale(0.5);
            if (IsLocked(i))
            {
                obj->SetConstantColor(PackedColor(Color(1, 0.5, 0.5, 0.5)));
            }
            else
            {
                obj->SetConstantColor(PackedColor(Color(0, 1, 0, 1)));
            }
            GScene->ObjectForDrawing(obj);
        }
    }
    Shape* paths = _shape->PathsLevel();
    if (paths)
    {
        LODShapeWithShadow* shape = GScene->Preloaded(SphereModel);
        for (int i = 0; i < paths->NPos(); i++)
        {
            Vector3Val pos = PositionModelToWorld(paths->Pos(i));
            Ref<Object> obj = new ObjectColored(shape, -1);
            obj->SetPosition(pos);
            obj->SetScale(0.1);
            obj->SetConstantColor(PackedColor(Color(0, 0.5, 0, 1)));
            GScene->ObjectForDrawing(obj);
        }
        for (Offset o = paths->BeginFaces(); o < paths->EndFaces(); paths->NextFace(o))
        {
            const Poly& face = paths->Face(o);
            // PoseidonAssert( face.N()==2 );
            Vector3 sum = VZero;
            for (int i = 0; i < face.N(); i++)
            {
                sum += paths->Pos(face.GetVertex(i));
            }
            sum /= face.N();

            Vector3Val pos = PositionModelToWorld(sum);
            Ref<Object> obj = new ObjectColored(shape, -1);
            obj->SetPosition(pos);
            obj->SetScale(0.05);
            obj->SetConstantColor(PackedColor(Color(0, 0.25, 0, 1)));
            GScene->ObjectForDrawing(obj);
        }

        DrawLines(_shape->FindPaths(), ClipAll, *this);
    }

    // Room/portal overlay, same POSEIDON_ROOM_OVERLAY switch the smoke volume
    // draws under (SmokeVolume.cpp): the interior view is useful wherever cheat
    // diags are, not only near a live plume. Resolves the building itself, so
    // outdoors this does nothing.
    static const bool roomOverlay = []
    {
        const char* value = std::getenv("POSEIDON_ROOM_OVERLAY");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    if (roomOverlay)
    {
        DebugDrawInteriorNear(Position());
    }
}

bool Building::IsMoveTarget() const
{
    // any "big" building may be move target
    // this is trick to avoid ammo crates etc..
    return GetMass() > (10 * 1000);
}

Vector3 Building::GetLadderPos(int ladder, bool up)
{
    const BuildingType* type = Type();
    const Ladder& lad = type->GetLadder(ladder);
    int mem = _shape->FindMemoryLevel();
    return AnimatePoint(mem, up ? lad._top : lad._bottom);
}

RString Building::GetActionName(const UIAction& action)
{
    switch (action.type)
    {
        case ATLadderOnUp:
        case ATLadderUp:
            return LocalizeString(IDS_ACTION_LADDERUP);
        case ATLadderOnDown:
        case ATLadderDown:
            return LocalizeString(IDS_ACTION_LADDERDOWN);
        case ATLadderOff:
            return LocalizeString(IDS_ACTION_LADDEROFF);
    }
    return base::GetActionName(action);
}

void Building::PerformAction(const UIAction& action, AIUnit* unit)
{
    switch (action.type)
    {
        case ATLadderUp:
        case ATLadderDown:
        case ATLadderOnUp:
        case ATLadderOnDown:
        {
            Person* person = unit->GetPerson();
            person->CatchLadder(this, action.param, action.param2 != 0);
        }
        break;
        case ATLadderOff:
        {
            Person* person = unit->GetPerson();
            person->DropLadder(this, action.param);
        }
        break;
        default:
            base::PerformAction(action, unit);
            break;
    }
}

void Building::GetActions(UIActions& actions, AIUnit* unit, bool now)
{
    // check if we can climb some ladder
    if (unit->IsFreeSoldier())
    {
        Person* person = unit->GetPerson();
        for (int i = 0; i < Type()->_ladders.Size(); i++)
        {
            const Ladder& ladder = Type()->_ladders[i];
            if (now)
            {
                if (person->IsOnLadder(this, i))
                {
                    actions.Add(ATLadderOff, this, 10, i);
                }
                else
                {
                    // check if unit is in effective radius
                    int mem = _shape->FindMemoryLevel();

                    Vector3 bottomPoint = AnimatePoint(mem, ladder._bottom);
                    Vector3 topPoint = AnimatePoint(mem, ladder._top);
                    Vector3 pos = unit->Position();
                    // param is ladder index
                    // param2 is 0 for bottom, 1 for top
                    if (pos.Distance2(bottomPoint) < Square(2))
                    {
                        actions.Add(ATLadderOnUp, this, 10, i, true, true, 0);
                    }
                    else if (pos.Distance2(topPoint) < Square(2))
                    {
                        actions.Add(ATLadderOnDown, this, 10, i, true, true, 1);
                    }
                }
            } // if (now)
            else
            {
                // check if we should climb up or down
                actions.Add(ATLadderUp, this, 1, i, true, true, 0);
                actions.Add(ATLadderDown, this, 1, i, true, true, 1);
            }
        }
    }

    base::GetActions(actions, unit, now);
}

ChurchType::ChurchType(const ParamEntry* param) : base(param)
{
    _scopeLevel = 1;
}

void ChurchType::Load(const ParamEntry& par)
{
    base::Load(par);
}

void ChurchType::InitShape()
{
    if (!_shape)
    {
        return;
    }
    _scopeLevel = 2;

    _hour1.Init(_shape, "hodinova1", nullptr, "osa1");
    _minute1.Init(_shape, "minutova1", nullptr, "osa1");
    _hour2.Init(_shape, "hodinova2", nullptr, "osa2");
    _minute2.Init(_shape, "minutova2", nullptr, "osa2");
    _hour3.Init(_shape, "hodinova3", nullptr, "osa3");
    _minute3.Init(_shape, "minutova3", nullptr, "osa3");
    _hour4.Init(_shape, "hodinova4", nullptr, "osa4");
    _minute4.Init(_shape, "minutova4", nullptr, "osa4");

    base::InitShape();
}

Church::Church(VehicleType* name, int id, LODShapeWithShadow* shape)
    : base(name, id, shape), _ringSmall(0), _ringBig(0), _nextRing(0)
//_object(object)
{
    SetSimulationPrecision(10.0458);
    _badTime = GRandGen.RandomValue() * (1.0 / 24 / 60); // one minute non-exactness
    _static = true;
    SetType(Primary);
    _destrType = DestructBuilding;
}

void Church::Simulate(float deltaT, SimulationImportance prec)
{
    base::Simulate(deltaT, prec);
}

bool Church::IsAnimated(int level) const
{
    return true;
}

bool Church::IsAnimatedShadow(int level) const
{
    return base::IsAnimatedShadow(level);
}

void Church::Animate(int level)
{
    float timeOfDay = Glob.clock.GetTimeOfDay();
    if (timeOfDay >= 1.0)
    {
        timeOfDay--;
    }
    PoseidonAssert(timeOfDay >= 0 && timeOfDay < 1.0);

    const ChurchType* type = Type();

    float angle = 4.0 * H_PI * timeOfDay;
    type->_hour1.Rotate(_shape, angle, level);
    type->_hour2.Rotate(_shape, angle, level);
    type->_hour3.Rotate(_shape, angle, level);
    type->_hour4.Rotate(_shape, angle, level);

    timeOfDay = fmod(24.0 * timeOfDay, 1.0);
    angle = 2.0 * H_PI * timeOfDay;
    type->_minute1.Rotate(_shape, angle, level);
    type->_minute2.Rotate(_shape, angle, level);
    type->_minute3.Rotate(_shape, angle, level);
    type->_minute4.Rotate(_shape, angle, level);
}

void Church::Deanimate(int level)
{
    const ChurchType* type = Type();
    type->_hour1.Restore(_shape, level);
    type->_hour2.Restore(_shape, level);
    type->_hour3.Restore(_shape, level);
    type->_hour4.Restore(_shape, level);
    type->_minute1.Restore(_shape, level);
    type->_minute2.Restore(_shape, level);
    type->_minute3.Restore(_shape, level);
    type->_minute4.Restore(_shape, level);
}

void Church::Sound(bool inside, float deltaT)
{
    float time = Glob.clock.GetTimeOfDay();
#define TIME_OFFSET 4

    time += TIME_OFFSET * (1.0 / (24 * 60)) + _badTime;
    int hour = toIntFloor(time * 24);
    int minute = toIntFloor(time * (24 * 60) - hour * 60);

    while (hour > 12)
    {
        hour -= 12;
    }
    while (hour <= 0)
    {
        hour += 12;
    }
    switch (minute)
    {
        case 15 + TIME_OFFSET - 1:
        case 15 + TIME_OFFSET:
            if (_ringBig == 0)
            {
                _ringSmall = 1, _ringBig = 0;
            }
            break;
        case 30 + TIME_OFFSET - 1:
        case 30 + TIME_OFFSET:
            if (_ringBig == 0)
            {
                _ringSmall = 2, _ringBig = 0;
            }
            break;
        case 45 + TIME_OFFSET - 1:
        case 46 + TIME_OFFSET:
            if (_ringBig == 0)
            {
                _ringSmall = 3, _ringBig = 0;
            }
            break;
            break;
        case 0:
        case 1:
        case 2:
        case 3:
            _ringSmall = 4, _ringBig = hour;
            break;
        default:
            if (_ringSmall > 0 || _ringBig > 0)
            {
                _nextRing -= deltaT;
                if (_nextRing <= 0)
                {
                    if (_ringSmall > 0)
                    {
                        SoundPars pars;
                        GetValue(pars, Pars >> "CfgSFX" >> "Church" >> "smallBell");
                        IWave* sound = GSoundScene->OpenAndPlayOnce(pars.name, Position(), VZero);
                        if (sound)
                        {
                            GSoundScene->AddSound(sound);
                        }
                        _nextRing = 2.0;
                        --_ringSmall;
                    }
                    else if (_ringBig > 0)
                    {
                        SoundPars pars;
                        GetValue(pars, Pars >> "CfgSFX" >> "Church" >> "largeBell");
                        IWave* sound = GSoundScene->OpenAndPlayOnce(pars.name, Position(), VZero);
                        if (sound)
                        {
                            GSoundScene->AddSound(sound);
                        }
                        _nextRing = 2.0;
                        --_ringBig;
                    }
                }
            }
            break;
    }
}

void Church::ResetStatus()
{
    _ringBig = 0;
    _ringSmall = 0;
    base::ResetStatus();
}

void Church::UnloadSound() {}

Fountain::Fountain(VehicleType* name, int id, LODShapeWithShadow* shape) : base(name, id, shape)
{
    _anim = 0;
    _lastAnimation = Glob.time;
    _sound = new SoundObject(Type()->_sound, this, true);
}

void Fountain::Simulate(float deltaT, SimulationImportance prec)
{
    if (_sound)
    {
        _sound->Simulate(deltaT, prec);
    }
}
void Fountain::Sound(bool inside, float deltaT) {}
void Fountain::UnloadSound() {}

bool Fountain::IsAnimated(int level) const
{
    return true;
}
bool Fountain::IsAnimatedShadow(int level) const
{
    return false;
}

void Fountain::Animate(int level)
{
    Shape* shape = _shape->Level(level);
    if (!shape)
    {
        return;
    }

    float deltaT = Glob.time - _lastAnimation;
    _lastAnimation = Glob.time;

    _anim += deltaT * Type()->_animSpeed;
    _anim = fastFmod(_anim, 1);

    // scan all faces
    for (Offset i = shape->BeginFaces(), e = shape->EndFaces(); i < e; shape->NextFace(i))
    {
        Poly& face = shape->Face(i);
        if (!(face.Special() & ::IsAnimated))
        {
            continue;
        }
        face.AnimateTexture(_anim);
    }

    base::Animate(level);
}
void Fountain::Deanimate(int level)
{
    base::Deanimate(level);
}

FountainType::FountainType(const ParamEntry* param) : base(param)
{
    _scopeLevel = 1;
}

void FountainType::Load(const ParamEntry& par)
{
    base::Load(par);
    _sound = par >> "sound";
    float animTime = par >> "animPeriod";
    _animSpeed = animTime > 0 ? 1.0f / animTime : 0;
}

void FountainType::InitShape()
{
    if (!_shape)
    {
        return;
    }
    _scopeLevel = 2;
    _water.Init(_shape, "vlajka", nullptr);
    base::InitShape();
}
