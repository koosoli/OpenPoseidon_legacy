
#include <Poseidon/Core/Application.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/Path/AIDefs.hpp>
#include <Poseidon/Game/OperMap.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Dev/Debug/DebugWin.hpp>
#include <stdlib.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/HashMap.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Memory/FastAlloc.hpp>
#include <Poseidon/Foundation/Memory/MemAlloc.hpp>

using namespace Poseidon;
namespace Poseidon
{
} // namespace Poseidon

using namespace Poseidon::Dev;
#include <Poseidon/World/Entities/Vehicles/House.hpp>

#include <Poseidon/Dev/Diag/DiagModes.hpp>

using Poseidon::Foundation::MemAllocD;
using Poseidon::Foundation::MemAllocSS;
using Poseidon::Foundation::MStorage;

#define USE_NEW_ASTAR 1

#if USE_NEW_ASTAR
#include <Poseidon/AI/Path/AStar.hpp>

struct ASOField
{
    union
    {
        int key;
        struct
        {
            short int x;
            short int z;
        } coord;
    } u;
    BYTE dir;
    bool house;
    ASOField(int x, int z, int d, bool h)
    {
        u.coord.x = x;
        u.coord.z = z;
        dir = d;
        house = h;
    }
    bool operator==(const ASOField& with) const { return with.u.key == u.key; }
    int GetKey() const { return u.key; }
};

typedef AStarNode<ASOField> ASONode;
template <>
DEFINE_FAST_ALLOCATOR(ASONode)

class ASOCostFunction
{
  protected:
    OperMap* _map;
    bool _locks;
    bool _soldier;
    OLink<EntityAI> _vehicle;

  public:
    ASOCostFunction(OperMap* map, bool locks, EntityAI* vehicle, bool soldier)
    {
        _map = map;
        _locks = locks;
        _vehicle = vehicle;
        _soldier = soldier;
    }
    float operator()(const ASOField& field1, const ASOField& field2) const;

  protected:
    float GetFieldCost(int x, int z) const;
};

float ASOCostFunction::operator()(const ASOField& field1, const ASOField& field2) const
{
    int xf = field1.u.coord.x;
    int zf = field1.u.coord.z;
    int xt = field2.u.coord.x;
    int zt = field2.u.coord.z;

    if (field2.house)
    {
        // special case - path inside house
        Object* house = _map->FindDoor(xf, zf, xt, zt);
        PoseidonAssert(house);
        PoseidonAssert(house->GetIPaths());

        return house->GetIPaths()->GetBType()->GetCoefInsideHeur() * OperItemGrid *
               sqrt(Square(xt - xf) + Square(zt - zf)) * _vehicle->GetType()->GetMinCost();
    }
    if (field1.house)
    {
        // special case - exitting doors
        return OperItemGrid * sqrt(Square(xt - xf) + Square(zt - zf)) * _vehicle->GetType()->GetMinCost();
    }

    const float H_SQRT5 = 2.2360679775;
    const float H_SQRT5_4 = 0.25 * H_SQRT5;
    const float H_SQRT2_2 = 0.5 * H_SQRT2;

    int dx = xt - xf;
    int dz = zt - zf;

    float result = GetFieldCost(xf, zf) + GetFieldCost(xt, zt);
    switch (dx)
    {
        case -2:
            switch (dz)
            {
                case -1:
                    result += GetFieldCost(xf - 1, zf - 1);
                    result += GetFieldCost(xf - 1, zf);
                    result *= H_SQRT5_4;
                    break;
                case 0:
                    result *= 0.5;
                    result += GetFieldCost(xf - 1, zf);
                    break;
                case 1:
                    result += GetFieldCost(xf - 1, zf);
                    result += GetFieldCost(xf - 1, zf + 1);
                    result *= H_SQRT5_4;
                    break;
                case -2:
                case 2:
                    Fail("Unused");
                    break;
            }
            break;
        case -1:
            switch (dz)
            {
                case -2:
                    result += GetFieldCost(xf - 1, zf - 1);
                    result += GetFieldCost(xf, zf - 1);
                    result *= H_SQRT5_4;
                    break;
                case -1:
                    result *= H_SQRT2_2;
                    break;
                case 0:
                    result *= 0.5;
                    break;
                case 1:
                    result *= H_SQRT2_2;
                    break;
                case 2:
                    result += GetFieldCost(xf - 1, zf + 1);
                    result += GetFieldCost(xf, zf + 1);
                    result *= H_SQRT5_4;
                    break;
            }
            break;
        case 0:
            switch (dz)
            {
                case -2:
                    result *= 0.5;
                    result += GetFieldCost(xf, zf - 1);
                    break;
                case -1:
                    result *= 0.5;
                    break;
                case 1:
                    result *= 0.5;
                    break;
                case 2:
                    result *= 0.5;
                    result += GetFieldCost(xf, zf + 1);
                    break;
                case 0:
                    Fail("Unused");
                    break;
            }
            break;
        case 1:
            switch (dz)
            {
                case -2:
                    result += GetFieldCost(xf, zf - 1);
                    result += GetFieldCost(xf + 1, zf - 1);
                    result *= H_SQRT5_4;
                    break;
                case -1:
                    result *= H_SQRT2_2;
                    break;
                case 0:
                    result *= 0.5;
                    break;
                case 1:
                    result *= H_SQRT2_2;
                    break;
                case 2:
                    result += GetFieldCost(xf, zf + 1);
                    result += GetFieldCost(xf + 1, zf + 1);
                    result *= H_SQRT5_4;
                    break;
            }
            break;
        case 2:
            switch (dz)
            {
                case -1:
                    result += GetFieldCost(xf + 1, zf - 1);
                    result += GetFieldCost(xf + 1, zf);
                    result *= H_SQRT5_4;
                    break;
                case 0:
                    result *= 0.5;
                    result += GetFieldCost(xf + 1, zf);
                    break;
                case 1:
                    result += GetFieldCost(xf + 1, zf);
                    result += GetFieldCost(xf + 1, zf + 1);
                    result *= H_SQRT5_4;
                    break;
                case -2:
                case 2:
                    Fail("Unused");
                    break;
            }
            break;
    }

    int dirD = field2.dir - field1.dir;
    if (dirD >= 8)
    {
        dirD -= 16;
    }
    else if (dirD < -8)
    {
        dirD += 16;
    }
    if (dirD != 0)
    {
        result += _vehicle->GetCostTurn(dirD);
    }

    if (_soldier && result >= GET_UNACCESSIBLE)
    {
        // search for doors
        OperDoor* door = _map->FindDoor(xt, zt);
        if (door)
        {
            // special case - entering doors
            result = OperItemGrid * sqrt(Square(xt - xf) + Square(zt - zf)) * _vehicle->GetType()->GetMinCost();
        }
    }

    return result;
}

float ASOCostFunction::GetFieldCost(int x, int z) const
{
    PoseidonAssert(_map);
    return _map->GetFieldCost(x, z, _locks, _vehicle, _soldier);
}

class ASOHeuristicFunction
{
  protected:
    float _coef;
    OLink<EntityAI> _vehicle;

  public:
    ASOHeuristicFunction(float coef, EntityAI* vehicle)
    {
        _coef = coef;
        _vehicle = vehicle;
    }
    float operator()(const ASOField& field1, const ASOField& field2) const
    {
        if (field1 == field2)
        {
            return 0;
        }

        int xs = field1.u.coord.x;
        int zs = field1.u.coord.z;
        int xe = field2.u.coord.x;
        int ze = field2.u.coord.z;
        int dx = abs(xs - xe);
        int dz = abs(zs - ze);
        float dif = fabs(dx - dz);
        float result = _coef * (dif + (0.5f * H_SQRT2) * (dx + dz - dif));

        // direction penalty
        if (field1.dir != 0xee)
        {
            int dirD = AI::CalcDirection(Vector3(xe - xs, 0, ze - zs)) - field1.dir;
            if (dirD >= 8)
            {
                dirD -= 16;
            }
            else if (dirD < -8)
            {
                dirD += 16;
            }
            if (dirD != 0)
            {
                result += _vehicle->GetCostTurn(1) * abs(dirD);
            }
        }

        return result;
    }
};

class ASOIterator
{
  protected:
    int _x, _z;
    int _index;
    int _doorFrom;
    int _doorTo;
    OperMap* _map;

  public:
    ASOIterator(const ASOField& field, void* context);
    operator bool() const { return _index < 8; }
    void operator++();
    operator ASOField();
};

ASOIterator::ASOIterator(const ASOField& field, void* context)
{
    _x = field.u.coord.x;
    _z = field.u.coord.z;
    _map = reinterpret_cast<OperMap*>(context);
    _doorFrom = 0;
    _doorTo = -1;
    if (_map)
    {
        _index = -1;
        ++(*this);
    }
    else
    {
        _index = 0;
    }
}

void ASOIterator::operator++()
{
    if (_index >= 0)
    {
        _index++;
    }
    else
    {
        PoseidonAssert(_map);
        while (_doorFrom < _map->_doors.Size())
        {
            OperDoor& doorFrom = _map->_doors[_doorFrom];
            if (!doorFrom.house || doorFrom.x != _x || doorFrom.z != _z)
            {
                _doorFrom++;
                continue;
            }
            while (++_doorTo < _map->_doors.Size())
            {
                if (_doorTo == _doorFrom)
                {
                    continue;
                }
                OperDoor& doorTo = _map->_doors[_doorTo];
                if (doorTo.house == doorFrom.house)
                {
                    return;
                }
            }
            _doorFrom++;
            _doorTo = -1;
        };
        _index = 0;
    }
}

ASOIterator::operator ASOField()
{
    if (_index >= 0)
    {
        return ASOField(_x + direction_delta[2 * _index][0], _z + direction_delta[2 * _index][1],
                        direction_delta[2 * _index][2], false);
    }
    else
    {
        PoseidonAssert(_map);
        OperDoor& doorTo = _map->_doors[_doorTo];
        return ASOField(doorTo.x, doorTo.z, 0xee, true);
    }
}

struct ASOOpenListTraits
{
    static bool IsLess(const ASONode* a, const ASONode* b) { return a->_f < b->_f; }
    static bool IsLessOrEqual(const ASONode* a, const ASONode* b) { return a->_f <= b->_f; }
};
class ASOOpenList : public HeapArray<ASONode*, MemAllocD, ASOOpenListTraits>
{
    typedef HeapArray<ASONode*, MemAllocD, ASOOpenListTraits> base;

  public:
    void UpdateUp(ASONode* node) { base::HeapUpdateUp(node); }
    void Add(ASONode* node) { base::HeapInsert(node); }
    bool RemoveFirst(ASONode*& node) { return base::HeapRemoveFirst(node); }
    const ASONode* GetFirst() const { return this->Size() > 0 ? (*this)[0] : nullptr; }
};

class ASONodeRef : public SRef<ASONode>
{
    typedef SRef<ASONode> base;

  public:
    ASONodeRef() = default;
    ASONodeRef(ASONode* node) : base(node) {}
    int GetKey() const { return (*this)->_field.u.key; }
};

struct ASOClosedListTraits
{
    //! key type
    typedef int KeyType;
    //! calculate hash value
    static unsigned int CalculateHashValue(KeyType key) { return (unsigned int)key; }

    //! compare keys, return negative when k1<k2, positive when k1>k2, zero when equal
    static int CmpKey(KeyType k1, KeyType k2) { return k1 - k2; }
    static KeyType GetKey(const ASONodeRef& item) { return item.GetKey(); }
};
class ASOClosedList : public MapStringToClass<ASONodeRef, AutoArray<ASONodeRef>, ASOClosedListTraits>
{
    typedef MapStringToClass<ASONodeRef, AutoArray<ASONodeRef>, ASOClosedListTraits> base;

  public:
    int Add(ASONode* node) { return base::Add(node); }
};

typedef AStar<ASOField, ASOCostFunction, ASOHeuristicFunction, ASOIterator, ASOClosedList, ASOOpenList> AStarOperative;
#endif

inline float OperHeuristic(float dx, float dz)
{
    dx = fabs(dx);
    dz = fabs(dz);
    float sum = dx + dz;
    float adif = fabs(dx - dz);
    float minD2 = (sum - adif);
    float maxD2 = (sum + adif);
    return (maxD2 - minD2) * 0.5 + (H_SQRT2 * 0.5) * minD2;
}

int AI::CalcDirection(Vector3 direction)
{
    // 16 directions (+-8)

    float angle = atan2(direction.X(), direction.Z()) + H_PI;
    // angle is from 0 to 2*pi

    angle *= 8 / H_PI;
    int dir = toInt(angle);
    if (dir > 16)
    {
        dir -= 16;
    }
    if (dir < 0)
    {
        dir += 16;
    }
    return dir;
}

#if _ENABLE_CHEATS

#include <Poseidon/Dev/Debug/DebugWinImpl.hpp>

class DebugWindowOperMap : public DebugWindow
{
  protected:
    OperMap* _map;
    OLink<AIUnit> _unit;
    int _xs, _zs, _xe, _ze;
    int _searchID;
    bool _unitAssigned;
    float _heurCost;

  public:
    DebugWindowOperMap();
    void Attach(OperMap* map, AIUnit* unit, int xs, int zs, int xe, int ze, float heurCost);
    void Detach();
    void AssignUnit(AIUnit* unit)
    {
        _unit = unit;
        _unitAssigned = true;
    }
    void UnassignUnit() { _unitAssigned = false; }
    void SetSearchID(int id) { _searchID = id; }
    bool IsAttached() const { return _map != nullptr; }

    void OnPaint(const OnPaintContext& dc);
};

DebugWindowOperMap::DebugWindowOperMap() : DebugWindow("Debug - Operative Map")
{
    _map = nullptr;
    _unit = nullptr;
    _unitAssigned = false;
}

#include <Poseidon/Core/Global.hpp>

void DebugWindowOperMap::Attach(OperMap* map, AIUnit* unit, int xs, int zs, int xe, int ze, float heurCost)
{
    if (_unitAssigned)
    {
        if (_unit != unit)
            return;
    }
    else
        _unit = unit;
    _map = map;
    _xs = xs;
    _zs = zs;
    _xe = xe;
    _ze = ze;
    _heurCost = heurCost;
    _searchID = -1;
    if (unit)
    {
        EntityAI* veh = unit->GetVehicle();
        RString title =
            (RString("Operative map for ") + veh->GetType()->GetDisplayName() + RString(" ") + unit->GetDebugName());
        char buf[256];
        snprintf(buf, sizeof(buf), ",t: %.2f, iter %d", Glob.time - Time(0), unit->GetIter());
        title = title + RString(buf);
        float invMinCost = veh->GetType()->GetMaxSpeedMs();
        snprintf(buf, sizeof(buf), ",h: %.2f", heurCost * invMinCost);
        title = title + RString(buf);
        HWND hwnd = GetWindowHandle(this);
        SetWindowText(hwnd, title);
    }
}

void DebugWindowOperMap::Detach()
{
    if (_map)
    {
        char text[1024];
        HWND hwnd = GetWindowHandle(this);
        GetWindowText(hwnd, text, sizeof(text));
        strncat(text, "[Finished]", sizeof(text) - strlen(text) - 1);
        SetWindowText(hwnd, text);
        _map = nullptr;
    }
}

void DebugWindowOperMap::OnPaint(const OnPaintContext& pc)
{
    HDC dc = pc.dc;
    if (!_map)
        return;
    if (!_unit)
        return;

    if (_map->_fields.Size() == 0)
        return;

    int xMin = INT_MAX;
    int xMax = INT_MIN;
    int zMin = INT_MAX;
    int zMax = INT_MIN;

    for (int i = 0; i < _map->_fields.Size(); i++)
    {
        OperField* field = _map->_fields[i];
        saturateMin(xMin, field->_x);
        saturateMax(xMax, field->_x);
        saturateMin(zMin, field->_z);
        saturateMax(zMax, field->_z);
    }

    const int itemSize = 8;
    const int fieldSize = OperItemRange * itemSize;

    HWND hwnd = GetWindowHandle(this);

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);

    int w = fieldSize * (xMax - xMin + 1);
    int h = fieldSize * (zMax - zMin + 1);

    if (clientRect.bottom > h)
    {
        RECT rect;
        rect.left = 0;
        rect.right = clientRect.right;
        rect.top = h;
        rect.bottom = clientRect.bottom;
        HBRUSH hbrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
        FillRect(dc, &rect, hbrush);
        clientRect.bottom = h;
    }
    if (clientRect.right > w)
    {
        RECT rect;
        rect.left = w;
        rect.right = clientRect.right;
        rect.top = 0;
        rect.bottom = clientRect.bottom;
        HBRUSH hbrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
        FillRect(dc, &rect, hbrush);
        clientRect.right = w;
    }

    EntityAI* veh = _unit->GetVehicle();
    bool soldier = _unit->IsFreeSoldier();

    float unaccLimit = veh->GetType()->GetKind() == VArmor ? GET_UNACCESSIBLE : 4.0;
    float invHeurCost = 1.0 / _heurCost;

    // field costs and locks
    HPEN lockPen = CreatePen(PS_SOLID, 0, RGB(255, 0, 255));
    HGDIOBJ oldPen = SelectObject(dc, lockPen);
    for (int i = 0; i < _map->_fields.Size(); i++)
    {
        OperField* field = _map->_fields[i];
        LockField* locks = GLandscape->LockingCache()->FindLockField(field->_x, field->_z);
        float baseCost = field->_baseCost;

        RECT rect;
        float left = fieldSize * (field->_x - xMin);
        rect.bottom = fieldSize * (zMax + 1 - field->_z);
        for (int z = 0; z < OperItemRange; z++)
        {
            rect.top = rect.bottom - itemSize;
            rect.left = left;
            for (int x = 0; x < OperItemRange; x++)
            {
                OperItem& item = field->_items[z][x];
                rect.right = rect.left + itemSize;
                OperItemType type = soldier ? item._typeSoldier : item._type;
                float cost = veh->GetTypeCost(type);
                COLORREF color;
                if (cost >= unaccLimit)
                    color = RGB(255, 0, 0);
                else
                {
                    cost *= baseCost;
                    if (cost <= _heurCost)
                    {
                        int green = toIntFloor(cost * invHeurCost * 256.0);
                        saturate(green, 0, 255);
                        color = RGB(0, (BYTE)green, 0);
                    }
                    else
                    {
                        int red = toIntFloor((cost - _heurCost) * invHeurCost * 64.0);
                        saturate(red, 0, 127);
                        color = RGB(255, (BYTE)(255 - red), 0);
                    }
                }
                HBRUSH hbrush = CreateSolidBrush(color);
                FillRect(dc, &rect, hbrush);
                DeleteObject(hbrush);

                if (locks && locks->IsLocked(x, z, soldier))
                {
                    MoveToEx(dc, rect.left, rect.top, nullptr);
                    LineTo(dc, rect.right, rect.bottom);
                    MoveToEx(dc, rect.left, rect.bottom, nullptr);
                    LineTo(dc, rect.right + 1, rect.top);
                }

                rect.left = rect.right;
            }
            rect.bottom = rect.top;
        }
    }
    SelectObject(dc, oldPen);
    DeleteObject(lockPen);

    // doors
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    oldPen = SelectObject(dc, GetStockObject(WHITE_PEN));
    for (int i = 0; i < _map->_doors.Size(); i++)
    {
        OperDoor& door = _map->_doors[i];
        int left = (door.x - OperItemRange * xMin) * itemSize;
        int bottom = (OperItemRange * (zMax + 1) - door.z) * itemSize;
        int right = left + itemSize;
        int top = bottom - itemSize;
        Ellipse(dc, left, top, right, bottom);
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);

    // path
    HPEN openPen = CreatePen(PS_SOLID, 0, RGB(191, 191, 255));
    HPEN closedPen = CreatePen(PS_SOLID, 0, RGB(0, 0, 0));
    oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    for (int i = 0; i < _map->_fields.Size(); i++)
    {
        OperField* field = _map->_fields[i];
        for (int z = 0; z < OperItemRange; z++)
            for (int x = 0; x < OperItemRange; x++)
            {
                OperItem& item = field->_items[z][x];
                if (item._searchID != _searchID)
                    continue;
                if (!item._parent)
                    continue;
                int x1 = (item._x - OperItemRange * xMin) * itemSize + itemSize / 2;
                int z1 = (OperItemRange * (zMax + 1) - item._z) * itemSize - itemSize / 2;
                int x2 = (item._parent->_x - OperItemRange * xMin) * itemSize + itemSize / 2;
                int z2 = (OperItemRange * (zMax + 1) - item._parent->_z) * itemSize - itemSize / 2;
                SelectObject(dc, item._open ? openPen : closedPen);
                MoveToEx(dc, x1, z1, nullptr);
                LineTo(dc, x2, z2);
            }
    }
    SelectObject(dc, oldPen);
    DeleteObject(closedPen);
    DeleteObject(openPen);

    // start & end point
    oldPen = SelectObject(dc, GetStockObject(WHITE_PEN));
    HBRUSH sbrush = CreateSolidBrush(RGB(255, 0, 255));
    HBRUSH ebrush = CreateSolidBrush(RGB(0, 255, 255));
    oldBrush = SelectObject(dc, sbrush);
    {
        int left = (_xs - OperItemRange * xMin) * itemSize;
        int bottom = (OperItemRange * (zMax + 1) - _zs) * itemSize;
        int right = left + itemSize;
        int top = bottom - itemSize;
        Ellipse(dc, left + 1, top + 1, right - 1, bottom - 1);
    }
    SelectObject(dc, ebrush);
    {
        int left = (_xe - OperItemRange * xMin) * itemSize;
        int bottom = (OperItemRange * (zMax + 1) - _ze) * itemSize;
        int right = left + itemSize;
        int top = bottom - itemSize;
        Ellipse(dc, left + 1, top + 1, right - 1, bottom - 1);
    }
    SelectObject(dc, oldBrush);
    DeleteObject(ebrush);
    DeleteObject(sbrush);
    SelectObject(dc, oldPen);
}

static Link<DebugWindowOperMap> GOperMapDebug;
static bool DebugAutoHide;

void DebugOperMap()
{
    if (!GOperMapDebug)
    {
        GOperMapDebug = new DebugWindowOperMap();
        DebugAutoHide = false;
    }
    GOperMapDebug->UnassignUnit();
}

void DebugOperMap(AIUnit* unit)
{
    if (!GOperMapDebug)
    {
        GOperMapDebug = new DebugWindowOperMap();
        DebugAutoHide = false;
    }
    GOperMapDebug->AssignUnit(unit);
}

static bool showTrouble = false;
void DebugOperMapTrouble()
{
    showTrouble = !showTrouble;
    GlobalShowMessage(500, "Show path troubles %s", showTrouble ? "On" : "Off");
}

#endif

OperMap::OperMap()
{
    InitStaticStorage();
    _alternateGoal = false;
}

OperMap::~OperMap() = default;

void OperMap::InitStaticStorage()
{
    static StaticStorage<Ref<OperField>> fieldsStorage;
    static StaticStorage<OperInfo> pathStorage;
    static StaticStorage<OperDoor> doorStorage;
    _fields.SetStorage(fieldsStorage.Init(256));
    _doors.SetStorage(doorStorage.Init(256));
    _path.SetStorage(pathStorage.Init(64));
}

OperItem* OperMap::Item(int x, int z)
{
    if (x < 0 || x >= OperItemRange * LandRange || z < 0 || z >= OperItemRange * LandRange)
    {
        return nullptr;
    }

    int x0 = x / OperItemRange;
    int z0 = z / OperItemRange;
    for (int i = 0; i < _fields.Size(); i++)
    {
        if (_fields[i]->_x == x0 && _fields[i]->_z == z0)
        {
            x0 = x - x0 * OperItemRange;
            z0 = z - z0 * OperItemRange;
            return &_fields[i]->_items[z0][x0];
        }
    }
    return nullptr;
}

#define FC(x, z) GetFieldCost(x, z, locks, veh, soldier)

float OperMap::GetCost(int xf, int zf, int dir, bool locks, EntityAI* veh, bool soldier)
{
    const float H_SQRT5 = 2.2360679775;
    const float H_SQRT5_4 = 0.25 * H_SQRT5;
    const float H_SQRT2_2 = 0.5 * H_SQRT2;
    switch (dir)
    {
        case 0:
            return 0.5 * (FC(xf, zf) + FC(xf, zf - 1));
        case 1:
            return H_SQRT5_4 * (FC(xf, zf) + FC(xf, zf - 1) + FC(xf - 1, zf - 1) + FC(xf - 1, zf - 2));
        case 2:
            return H_SQRT2_2 * (FC(xf, zf) + FC(xf - 1, zf - 1));
        case 3:
            return H_SQRT5_4 * (FC(xf, zf) + FC(xf - 1, zf) + FC(xf - 1, zf - 1) + FC(xf - 2, zf - 1));
        case 4:
            return 0.5 * (FC(xf, zf) + FC(xf - 1, zf));
        case 5:
            return H_SQRT5_4 * (FC(xf, zf) + FC(xf - 1, zf) + FC(xf - 1, zf + 1) + FC(xf - 2, zf + 1));
        case 6:
            return H_SQRT2_2 * (FC(xf, zf) + FC(xf - 1, zf + 1));
        case 7:
            return H_SQRT5_4 * (FC(xf, zf) + FC(xf, zf + 1) + FC(xf - 1, zf + 1) + FC(xf - 1, zf + 2));
        case 8:
            return 0.5 * (FC(xf, zf) + FC(xf, zf + 1));
        case 9:
            return H_SQRT5_4 * (FC(xf, zf) + FC(xf, zf + 1) + FC(xf + 1, zf + 1) + FC(xf + 1, zf + 2));
        case 10:
            return H_SQRT2_2 * (FC(xf, zf) + FC(xf + 1, zf + 1));
        case 11:
            return H_SQRT5_4 * (FC(xf, zf) + FC(xf + 1, zf) + FC(xf + 1, zf + 1) + FC(xf + 2, zf + 1));
        case 12:
            return 0.5 * (FC(xf, zf) + FC(xf + 1, zf));
        case 13:
            return H_SQRT5_4 * (FC(xf, zf) + FC(xf + 1, zf) + FC(xf + 1, zf - 1) + FC(xf + 2, zf - 1));
        case 14:
            return H_SQRT2_2 * (FC(xf, zf) + FC(xf + 1, zf - 1));
        case 15:
            return H_SQRT5_4 * (FC(xf, zf) + FC(xf, zf - 1) + FC(xf + 1, zf - 1) + FC(xf + 1, zf - 2));
        case 16:
            return 0.5 * (FC(xf, zf) + FC(xf, zf - 2)) + FC(xf, zf - 1);
        case 17:
            return 0.5 * (FC(xf, zf) + FC(xf - 2, zf)) + FC(xf - 1, zf);
        case 18:
            return 0.5 * (FC(xf, zf) + FC(xf, zf + 2)) + FC(xf, zf + 1);
        case 19:
            return 0.5 * (FC(xf, zf) + FC(xf + 2, zf)) + FC(xf + 1, zf);
    }
    Fail("Unaccessible for 20 directions.");
    return 0;
}

// A swimmer's water item costs what the soldier says a metre of swimming costs
// (OperItemGrid of them), whatever the 50 m square around it costs to walk: a
// shore square is land to the geography, and multiplying its walking cost made
// the sea look as quick as the beach there and unreachable a square further out.
static float ItemCost(float baseCost, OperItemType type, EntityAI* veh, bool soldier)
{
    float typeCost = veh->GetTypeCost(type);
    if (soldier && type == OITWater && typeCost < GET_UNACCESSIBLE)
    {
        return OperItemGrid * typeCost;
    }
    return baseCost * typeCost;
}

float OperMap::GetFieldCost(int x, int z, bool locks, EntityAI* veh, bool soldier)
{
    if (x < 0 || x >= OperItemRange * LandRange || z < 0 || z >= OperItemRange * LandRange)
    {
        return SET_UNACCESSIBLE;
    }

    if (locks && GLOB_LAND->LockingCache()->IsLocked(x, z, soldier))
    {
        return SET_UNACCESSIBLE;
    }

    int x0 = x / OperItemRange;
    int z0 = z / OperItemRange;
    for (int i = 0; i < _fields.Size(); i++)
    {
        if (_fields[i]->_x == x0 && _fields[i]->_z == z0)
        {
            x0 = x - x0 * OperItemRange;
            z0 = z - z0 * OperItemRange;
            OperItemType type = soldier ? _fields[i]->_items[z0][x0]._typeSoldier : _fields[i]->_items[z0][x0]._type;
            return ItemCost(_fields[i]->_baseCost, type, veh, soldier);
        }
    }
    // field not found
#if FIELD_ON_DEMAND
    int mask = MASK_AVOID_OBJECTS | MASK_PREFER_ROADS | MASK_USE_BUFFER;
    OperField* field = CreateField(x0, z0, mask, veh);
    PoseidonAssert(field);
    x0 = x - x0 * OperItemRange;
    z0 = z - z0 * OperItemRange;
    OperItemType type = soldier ? field->_items[z0][x0]._typeSoldier : field->_items[z0][x0]._type;
    return ItemCost(field->_baseCost, type, veh, soldier);
#else
    return SET_UNACCESSIBLE;
#endif
}

void OperMap::ClearMap()
{
    _fields.Clear();
    _doors.Clear();
}

OperField* OperMap::CreateField(int x, int z, int mask, EntityAI* veh)
{
    // x,z are in LandGrid units
    if (!InRange(z, x))
    {
        return nullptr;
    }

    OperField* fld = nullptr;
    // lookup map
    int n = _fields.Size();
    if (n > 50 * 50)
    {
        LOG_DEBUG(Script, "Very large OperMap");
    }
    for (int i = 0; i < n; i++)
    {
        if (_fields[i]->IsField(x, z))
        {
            fld = _fields[i];
            break; // already in map
        }
    }

    if (fld == nullptr)
    {
        if (mask & MASK_USE_BUFFER)
        {
            fld = GLOB_LAND->OperationalCache()->GetOperField(x, z, mask);
        }
        else
        {
            fld = new OperField(x, z, mask);
        }
        _fields.Add(fld);
        for (int i = 0; i < fld->_doors.Size(); i++)
        {
            _doors.Add(fld->_doors[i]);
        }
    }

    PoseidonAssert(InRange(x, z));
    GeographyInfo geogr = GLOB_LAND->GetGeography(x, z);
    fld->_baseCost = OperItemGrid * veh->GetCost(geogr);
    fld->_heurCost = fld->_baseCost * veh->GetFieldCost(geogr);

    return fld;
}

#include <Poseidon/World/Scene/Scene.hpp>

bool OperMap::FindNearestEmpty(int& x, int& z, float xf, float zf, int xbase, int zbase, int xsize, int zsize,
                               bool locks, EntityAI* veh, bool soldier, FindNearestEmptyCallback* isFree, void* context)
{
    // x, z, xbase, zbase are in OperItemGrid units
    // xf,zf have the same meaning as x,z, but are with sub-sqaure precision
    int xrest = x - xbase;
    int zrest = z - zbase;
    int xt, zt;
    int t;
    bool inner;
    int nMax = xrest;
    if (zrest > nMax)
    {
        nMax = zrest;
    }
    t = xsize - 1 - xrest;
    if (t > nMax)
    {
        nMax = t;
    }
    t = zsize - 1 - zrest;
    if (t > nMax)
    {
        nMax = t;
    }

    AssertDebug(nMax < 160);
    if (nMax >= 160)
    {
        return false;
    }

    const float operItemGrid = OperItemGrid;

    // for soldier check also position inside building
    float nearestDist2 = 1e10;
#if 1
    if (soldier)
    {
        float posX = xf * operItemGrid;
        float posZ = zf * operItemGrid;
        Vector3 pos = GLandscape->PointOnSurface(posX, 0, posZ);
        // convert to LandGrid units
        int xLand = x / OperItemRange;
        int zLand = z / OperItemRange;
        for (int xx = xLand - 1; xx <= xLand + 1; xx++)
        {
            for (int zz = zLand - 1; zz <= zLand + 1; zz++)
            {
                if (!InRange(xx, zz))
                {
                    continue;
                }
                const ObjectList& list = GLandscape->GetObjects(zz, xx);
                for (int o = 0; o < list.Size(); o++)
                {
                    const Object* obj = list[o];
                    const IPaths* paths = obj->GetIPaths();
                    if (!paths)
                    {
                        continue;
                    }
                    Vector3 retPos;
                    if (paths->FindNearestPoint(pos, retPos) < 0)
                    {
                        continue;
                    }
                    float dist2 = retPos.Distance2(pos);
                    if (nearestDist2 > dist2)
                    {
                        nearestDist2 = dist2;
                        x = toIntFloor(retPos.X() * InvOperItemGrid);
                        z = toIntFloor(retPos.Z() * InvOperItemGrid);
                    }
                }
            }
        }
        // limit nMax with position already found in house
        if (nearestDist2 < 1e9)
        {
            float maxDist = sqrt(nearestDist2) * H_SQRT2;
            int maxDistGrid = toIntCeil(maxDist * InvOperItemGrid);
            saturateMin(nMax, maxDistGrid);

#if _ENABLE_CHEATS
            if (CHECK_DIAG(DECostMap))
            {
                float xPos = operItemGrid * x;
                float zPos = operItemGrid * z;
                float yPos = GLandscape->SurfaceYAboveWater(xPos, zPos);
                Vector3 dPos(xPos, yPos, zPos);

                Ref<Object> obj = new ObjectColored(GScene->Preloaded(SphereModel), -1);
                obj->SetPosition(dPos);
                obj->SetScale(0.5);
                obj->SetConstantColor(PackedColor(Color(1, 1, 1)));
                GLandscape->ShowObject(obj);
            }
#endif
        }
    }
#endif

    for (int n = 1; n < nMax; n++)
    {
#define IS_FREE(xg, zg) \
    isFree(Vector3((xg) * operItemGrid + 0.5 * operItemGrid, 0, (zg) * operItemGrid + 0.5 * operItemGrid), context)

#define CHECK_EMPTY                                                  \
    float dist2 = Square(xbase + xt - xf) + Square(zbase + zt - zf); \
    if (dist2 < nearestDist2)                                        \
    {                                                                \
        nearestDist2 = dist2;                                        \
        x = xbase + xt;                                              \
        z = zbase + zt;                                              \
        saturateMin(nMax, i + 1);                                    \
    }
        // if something found, limit number of iterations

        for (int i = 0; i <= n; i++)
        {
            inner = 0 < i && i < n;
            xt = xrest - n;
            if (xt >= 0)
            {
                zt = zrest + i;
                if (zt < zsize && FC(xbase + xt, zbase + zt) < GET_UNACCESSIBLE && IS_FREE(xbase + xt, zbase + zt) ||
                    inner && (zt = zrest - i) >= 0 && FC(xbase + xt, zbase + zt) < GET_UNACCESSIBLE &&
                        IS_FREE(xbase + xt, zbase + zt))
                {
                    CHECK_EMPTY
                }
            }
            zt = zrest - n;
            if (zt >= 0)
            {
                xt = xrest + i;
                if (xt < xsize && FC(xbase + xt, zbase + zt) < GET_UNACCESSIBLE && IS_FREE(xbase + xt, zbase + zt) ||
                    inner && (xt = xrest - i) >= 0 && FC(xbase + xt, zbase + zt) < GET_UNACCESSIBLE &&
                        IS_FREE(xbase + xt, zbase + zt))
                {
                    CHECK_EMPTY
                }
            }
            xt = xrest + n;
            if (xt < xsize)
            {
                zt = zrest - i;
                if (zt >= 0 && FC(xbase + xt, zbase + zt) < GET_UNACCESSIBLE && IS_FREE(xbase + xt, zbase + zt) ||
                    inner && (zt = zrest + i) < zsize && FC(xbase + xt, zbase + zt) < GET_UNACCESSIBLE &&
                        IS_FREE(xbase + xt, zbase + zt))
                {
                    CHECK_EMPTY
                }
            }
            zt = zrest + n;
            if (zt < zsize)
            {
                xt = xrest - i;
                if (xt >= 0 && FC(xbase + xt, zbase + zt) < GET_UNACCESSIBLE && IS_FREE(xbase + xt, zbase + zt) ||
                    inner && (xt = xrest + i) < xsize && FC(xbase + xt, zbase + zt) < GET_UNACCESSIBLE &&
                        IS_FREE(xbase + xt, zbase + zt))
                {
                    CHECK_EMPTY
                }
            }
        }
    }
    return nearestDist2 < 1e9;
}

void OperMap::LogMap(int xMin, int xMax, int zMin, int zMax, int xs, int xe, int zs, int ze, bool locks)
{
#if _ENABLE_CHEATS
    int xsize = (xMax - xMin + 1) * OperItemRange;
    int zsize = (zMax - zMin + 1) * OperItemRange;
    int i, j;

    LOG_DEBUG(Script, "Fields: {}..{}, {}..{}", xMax, xMin, zMin, zMax);

    Temp<Temp<char>> map(zsize);
    for (i = 0; i < zsize; i++)
        map[i].Realloc(xsize * 2 + 1), memset(map[i], ' ', xsize * 2);

    for (i = zMin * OperItemRange; i < (zMax + 1) * OperItemRange; i++)
    {
        for (j = xMin * OperItemRange; j < (xMax + 1) * OperItemRange; j++)
        {
            if (!InRange(toIntFloor(i / OperItemRange), toIntFloor(j / OperItemRange)))
                continue;

            OperItem* item = nullptr;
            OperField* fld = nullptr;
            int x0 = j / OperItemRange;
            int z0 = i / OperItemRange;
            for (int k = 0; k < _fields.Size(); k++)
                if (_fields[k]->_x == x0 && _fields[k]->_z == z0)
                {
                    fld = _fields[k];
                    x0 = j - x0 * OperItemRange;
                    z0 = i - z0 * OperItemRange;
                    item = &fld->_items[z0][x0];
                }

            PoseidonAssert(fld && item);

            char ch = '?';
            if (locks && GLOB_LAND->LockingCache()->IsLocked(j, i, false))
                ch = 'L';
            else if (!item)
                ch = ' ';
            else
            {
                switch (item->_typeSoldier)
                {
                    case OITNormal:
                        ch = '.';
                        break;
                    case OITAvoidBush:
                        ch = 'b';
                        break;
                    case OITAvoidTree:
                        ch = 't';
                        break;
                    case OITAvoid:
                        ch = 'x';
                        break;
                    case OITWater:
                        ch = 'W';
                        break;
                    case OITSpaceRoad:
                        ch = 'R';
                        break;
                    case OITRoad:
                        ch = 'r';
                        break;
                    case OITSpaceBush:
                        ch = 'B';
                        break;
                    case OITSpaceTree:
                        ch = 'T';
                        break;
                    case OITSpace:
                        ch = 'X';
                        break;
                    case OITRoadForced:
                        ch = 'F';
                        break;
                }
            }
            map[i - zMin * OperItemRange][(xsize - 1 - (j - xMin * OperItemRange)) * 2] = ch;
        }
        map[i - zMin * OperItemRange][xsize * 2] = 0;
    }
    for (i = 0; i < _path.Size(); i++)
    {
        int iz = _path[i]._z - zMin * OperItemRange;
        if (iz < 0 || iz >= zsize)
            continue;
        int ix = xsize - 1 - (_path[i]._x - xMin * OperItemRange);
        if (ix < 0 || ix >= xsize)
            continue;
        map[iz][2 * ix + 1] = '+';
    }
    map[zs - zMin * OperItemRange][(xsize - 1 - (xs - xMin * OperItemRange)) * 2 + 1] = 'S';
    map[ze - zMin * OperItemRange][(xsize - 1 - (xe - xMin * OperItemRange)) * 2 + 1] = 'E';
    LOG_DEBUG(Script, "Begin loop ... {}, begin at {}, {}", zsize, xsize - 1 - (xs - xMin * OperItemRange),
              zs - zMin * OperItemRange);
    for (i = 0; i < zsize; i++)
        LOG_DEBUG(Script, "{:02}:{}", i, map[i]);
    LOG_DEBUG(Script, "End loop ...");
    Sleep(50);
#endif
}

bool OperMap::IsIntersection(int xs, int zs, int xe, int ze, float& cost, float& costPerItem, EntityAI* veh,
                             bool soldier)
{
    // returned values:
    // cost is total cost of direct path
    // costPerItem is cost per single item (rectangle of size OperItemGrid)
    if (xs == xe && zs == ze)
    {
        cost = 0;
        costPerItem = 1;
        return false;
    }
    float dx = xe - xs;
    float dz = ze - zs;
    int incz = zs < ze ? 1 : -1;
    int incx = xs < xe ? 1 : -1;
    cost = 0;

    if (fabs(dx) < fabs(dz))
    {
        float invabsdz = 1.0 / fabs(dz);
        dx *= invabsdz;
        float invdx = dx != 0 ? 1.0 / dx : 1e10;
        float coef = sqrt(1 + Square(dx));
        float x = xs + 0.5;
        for (int z = zs; z != ze + incz; z += incz)
        {
            x += 0.5 * dx;
            if (toIntFloor(x) != xs)
            {
                PoseidonAssert(toIntFloor(x) == xs + incx);
                float cost1 = GetFieldCost(xs, z, true, veh, soldier);
                float cost2 = GetFieldCost(xs + incx, z, true, veh, soldier);
                float a;
                if (dx > 0)
                {
                    a = (x - (xs + 1)) * invdx;
                }
                else
                {
                    a = (x - xs) * invdx;
                }
                if (a < 0.99f && cost1 >= GET_UNACCESSIBLE)
                {
                    return true;
                }
                if (a > 0.01f && cost2 >= GET_UNACCESSIBLE)
                {
                    return true;
                }
                if (z != zs)
                {
                    cost += a * cost2 + (1.0 - a) * cost1;
                }
                xs += incx;
            }
            else
            {
                float cost1 = GetFieldCost(xs, z, true, veh, soldier);
                if (cost1 >= GET_UNACCESSIBLE)
                {
                    return true;
                }
                if (z != zs)
                {
                    cost += cost1;
                }
            }
            x += 0.5 * dx;
        }
        costPerItem = cost * invabsdz;
        cost *= coef;
    }
    else
    {
        float invabsdx = 1.0 / fabs(dx);
        dz *= invabsdx;
        float invdz = dz != 0 ? 1.0 / dz : 1e10;
        float coef = sqrt(1 + Square(dz));
        float z = zs + 0.5;
        for (int x = xs; x != xe + incx; x += incx)
        {
            z += 0.5 * dz;
            if (toIntFloor(z) != zs)
            {
                //				PoseidonAssert(toIntFloor(z) == zs + incz);
                float cost1 = GetFieldCost(x, zs, true, veh, soldier);
                float cost2 = GetFieldCost(x, zs + incz, true, veh, soldier);
                float a;
                if (dz > 0)
                {
                    a = (z - (zs + 1)) * invdz;
                }
                else
                {
                    a = (z - zs) * invdz;
                }
                if (a < 0.99f && cost1 >= GET_UNACCESSIBLE)
                {
                    return true;
                }
                if (a > 0.01f && cost2 >= GET_UNACCESSIBLE)
                {
                    return true;
                }
                if (x != xs)
                {
                    cost += a * cost2 + (1.0 - a) * cost1;
                }
                zs += incz;
            }
            else
            {
                float cost1 = GetFieldCost(x, zs, true, veh, soldier);
                if (cost1 >= GET_UNACCESSIBLE)
                {
                    return true;
                }
                if (x != xs)
                {
                    cost += cost1;
                }
            }
            z += 0.5 * dz;
        }
        costPerItem = cost * invabsdx;
        cost *= coef;
    }
    return false;
}

void OperMap::CreateMap(EntityAI* veh, int xMin, int zMin, int xMax, int zMax)
{
    int mask = MASK_AVOID_OBJECTS | MASK_PREFER_ROADS | MASK_USE_BUFFER;

    int i, j;
    for (i = xMin; i <= xMax; i++)
    {
        for (j = zMin; j <= zMax; j++)
        {
            if (!InRange(i, j))
            {
                continue;
            }
            CreateField(i, j, mask, veh);
        }
    }
}

bool OperMap::IsSimplePath(AIUnit* unit, int xs, int zs, int xe, int ze)
{
    float simpleCost, simpleHeurCost;
    return !IsIntersection(xs, zs, xe, ze, simpleCost, simpleHeurCost, unit->GetVehicle(), unit->IsSoldier());
}

#define MAX_ITER 1000

bool operator<(OperItem& a, OperItem& b)
{
    return a._cost + a._heur < b._cost + b._heur;
}

bool operator<=(OperItem& a, OperItem& b)
{
    return a._cost + a._heur <= b._cost + b._heur;
}

typedef HeapArray<OperItem*, MemAllocSS> OperOpenList;

#define PERF_STATS 0

#if PERF_STATS
#endif

#if _DEBUG
const int UpdateSleep = 1000;
const int FinishSleep = 1000;
#else
const int UpdateSleep = 100;  // time to sleep after single interation
const int FinishSleep = 1000; // time to sleep after finishing search
#endif

bool OperMap::FindPath(AIUnit* unit, int dir, int xs, int zs, int xe, int ze, bool locks)
{
#if LOG_MAPS >= 2 || LOG_PROBL
    int xMin = LandRangeMask;
    int xMax = 0;
    int zMin = LandRangeMask;
    int zMax = 0;
    for (int i = 0; i < _fields.Size(); i++)
    {
        OperField* fld = _fields[i];
        if (fld->_x < xMin)
            xMin = fld->_x;
        if (fld->_x > xMax)
            xMax = fld->_x;
        if (fld->_z < zMin)
            zMin = fld->_z;
        if (fld->_z > zMax)
            zMax = fld->_z;
    }
#endif

    _alternateGoal = false;

    // scan for min,max,avg cost and do some arithmetics with results
    float maxCost = 0, minCost = GET_UNACCESSIBLE, avgCost = 0;
    int cntCost = 0;

    int n = _fields.Size();
    for (int i = 0; i < n; i++)
    {
        OperField* fld = _fields[i];
        float cost = fld->HeurCost();
        if (cost < GET_UNACCESSIBLE)
        {
            saturateMax(maxCost, cost);
            saturateMin(minCost, cost);
            avgCost += cost;
            cntCost++;
        }
    }
    avgCost /= cntCost;

    float heurCost;
    switch (unit->GetIter())
    {
        case 0:
            heurCost = avgCost * 0.8 + minCost * 0.2;
            break;
        case 1:
            heurCost = maxCost * 0.4 + avgCost * 0.6;
            break;
        case 2:
        default:
            heurCost = maxCost;
            break;
    }

    EntityAI* veh = unit->GetVehicle();
    bool isSoldier = unit->IsSoldier();

#if _ENABLE_CHEATS
    if (GOperMapDebug)
    {
        GOperMapDebug->Attach(this, unit, xs, zs, xe, ze, heurCost);
        if (GOperMapDebug->IsAttached())
        {
            GOperMapDebug->Update();
            Sleep(UpdateSleep);
        }
    }
#endif

    //	Check if simplified methods can be used
    float simpleCost, simpleHeurCost;
    if (!IsIntersection(xs, zs, xe, ze, simpleCost, simpleHeurCost, veh, isSoldier))
    {
#if LOG_MAPS
        LOG_DEBUG(Script, "Unit {}: Direct path {}, heur {:.3f}, found {:.3f}", (const char*)unit->GetDebugName(),
                  simpleHeurCost <= 1.01f * heurCost ? "found" : "not found", heurCost, simpleHeurCost);
#endif
        // check if we match heuristic estimation
        if (simpleHeurCost <= 1.01f * heurCost)
        {
        SimplePath:
            _path.Resize(2);
            _path[0]._x = xs;
            _path[0]._z = zs;
            _path[0].house = nullptr;
            _path[0].from = -1;
            _path[0].to = -1;
            _path[0]._cost = 0;
            _path[1]._x = xe;
            _path[1]._z = ze;
            _path[1].house = nullptr;
            _path[1].from = -1;
            _path[1].to = -1;
            _path[1]._cost = simpleCost;
#if _ENABLE_CHEATS
            if (GOperMapDebug && GOperMapDebug->IsAttached())
            {
                GOperMapDebug->Update();
                Sleep(FinishSleep);
                GOperMapDebug->Detach();
                if (DebugAutoHide)
                    WaitForClose(GOperMapDebug);
            }
#endif
            return true;
        }
        else
        {
            // Note: evaluate lowering heurCost to simpleHeurCost here.
        }
    }

#if USE_NEW_ASTAR
    OperItem* test = Item(xs, zs);
    if (!test)
    {
        // cannot plan operative path outside map
        LOG_ERROR(Script, "Out of map");

        // error recovery - try to use simple path
        goto SimplePath;
    }

    bool house = isSoldier && FindDoor(xs, zs);

    ASOField start(xs, zs, dir, house);
    ASOField end(xe, ze, 0xff, false);
    ASOCostFunction costFunction(this, locks, veh, isSoldier);
    ASOHeuristicFunction heuristicFunction(heurCost, veh);
    SRef<AStarOperative> algorithm = new AStarOperative(start, end, costFunction, heuristicFunction, GET_UNACCESSIBLE);

    algorithm->Process(MAX_ITER, isSoldier ? this : nullptr);

    const ASONode* last = nullptr;
    if (algorithm->IsDone() && algorithm->IsFound())
    {
        // path found
        last = algorithm->GetLastNode();
    }
    else
    {
        // path not found - try to select alternate goal
        // a soldier heads for the node nearest the goal (what the original search
        // did); the cheapest open node can lead along a beach, away from a goal
        // out in the water, once the iterations run out on the land around it
        last = isSoldier ? algorithm->GetNearestNode() : algorithm->GetBestNode();
        if (!last)
        {
            return false;
        }
        if (last->_field == start)
        {
            return false; // singular path
        }
        if (last->_f - last->_g >= heuristicFunction(start, end))
        {
            return false;
        }
        _alternateGoal = true;
    }

    // export path
    int depth = 0;
    for (const ASONode* cur = last; cur != nullptr; cur = cur->_parent)
    {
        depth++;
    }
    _path.Resize(depth);
    int i = depth - 1;
    for (const ASONode* cur = last; cur != nullptr; cur = cur->_parent)
    {
        OperInfo& info = _path[i--];
        info._cost = cur->_g;
        info._x = cur->_field.u.coord.x;
        info._z = cur->_field.u.coord.z;

        if (isSoldier && cur->_field.house)
        {
            if (cur->_parent)
            {
                PoseidonAssert(FindDoor(info, cur->_parent->_field.u.coord.x, cur->_parent->_field.u.coord.z,
                                        cur->_field.u.coord.x, cur->_field.u.coord.z));
            }
            else
            {
                OperDoor* door = FindDoor(cur->_field.u.coord.x, cur->_field.u.coord.z);
                PoseidonAssert(door);
                if (door)
                {
                    info.house = door->house;
                    info.from = door->exit;
                    info.to = door->exit;
                }
            }
        }
    }
    return true;

#else
    int searchID = GLOB_LAND->LockingCache()->SearchID();
    int xc, zc;
    int xx, zz;
    int iter = 0;

    float cost, heur;

    static StaticStorage<OperItem*> openListStorage;
    OperOpenList openList;
    // use static storage to contain typical searches
    openList.SetStorage(openListStorage.Init(1024));

    OperItem *best, *cur;

    best = Item(xs, zs);
    if (!best)
    {
        // cannot plan operative path outside map
        LOG_ERROR(Script, "Out of map");

        // error recovery - try to use simple path
        goto SimplePath;
    }
    best->_x = xs;
    best->_z = zs;
    best->_dir = dir;
    best->_cost = 0;
    int dirD;
    if (xe == xs && ze == zs)
        best->_heur = 0;
    else
    {
        best->_heur = OperHeuristic(xe - xs, ze - zs) * heurCost;
        dirD = AI::CalcDirection(Vector3(xe - xs, 0, ze - zs)) - dir;
        if (dirD >= 8)
            dirD -= 16;
        else if (dirD < -8)
            dirD += 16;
        if (dirD != 0)
            best->_heur += veh->GetCostTurn(1) * abs(dirD);
    }
    best->_parent = nullptr;
    best->_open = true;
    best->_searchID = searchID;

    bool soldier = unit->IsFreeSoldier(); // use doors only for soldiers
    if (soldier)
    {
        for (int d = 0; d < _doors.Size(); d++)
        {
            OperDoor& doorIn = _doors[d];
            if (!doorIn.house)
                continue;
            if (doorIn.x != xs)
                continue;
            if (doorIn.z != zs)
                continue;
            best->_dir = 0xee; // start in doors
            break;
        }
    }

    openList.Add(best); // openList is empty, HeapInsert == Add

#if _ENABLE_CHEATS
    if (GOperMapDebug && GOperMapDebug->IsAttached())
    {
        GOperMapDebug->SetSearchID(searchID);
    }
#endif

    while (1) // search cycle
    {
#if _ENABLE_CHEATS
        if (GOperMapDebug && GOperMapDebug->IsAttached())
        {
            // do not update on every iteration
            if ((iter & 31) == 0)
            {
                GOperMapDebug->Update();
                Sleep(UpdateSleep);
            }
        }
#endif

        bool ok = openList.HeapRemoveFirst(best);
        if (!ok || ++iter > MAX_ITER)
        {
            //  try to found alternate path (to the point nearest to goal)
            //  found alternate goal at open list
            best = nullptr;
            float minDist2 = Square(xe - xs) + Square(ze - zs);
            for (int i = 0; i < openList.Size(); i++)
            {
                cur = openList[i];
                float dist2 = Square(xe - cur->_x) + Square(ze - cur->_z);
                if (dist2 < minDist2)
                {
                    minDist2 = dist2;
                    best = cur;
                }
            }
            if (best)
            {
#if PERF_STATS
#endif
#if LOG_MAPS
                LOG_DEBUG(Script, "Unit {}: Alternate path found in {} iters.", (const char*)unit->GetDebugName(),
                          iter);
#endif
                float dist2 = Square(xs - best->_x) + Square(zs - best->_z);

                if (Square(veh->GetPrecision()) > dist2 * Square(OperItemGrid))
                {
                    // path to short - do not consider it as solution
                    goto PathNotFound;
                }
                _alternateGoal = true;
                goto PathFound;
            }
            else
            {
            PathNotFound:
#if PERF_STATS
#endif
                LOG_DEBUG(Script, "Unit {}: Path not found in {} iters.", (const char*)unit->GetDebugName(), iter);
#if LOG_PROBL
                LogMap(xMin, xMax, zMin, zMax, xs, xe, zs, ze, locks);
#endif
#if _ENABLE_CHEATS
                if (GOperMapDebug && GOperMapDebug->IsAttached())
                {
                    GOperMapDebug->Update();
                    Sleep(FinishSleep);
                    GOperMapDebug->Detach();
                    if (DebugAutoHide)
                        WaitForClose(GOperMapDebug);
                }
#endif
                return false; // no solution
            }
        }

#if _ENABLE_CHEATS
        if (iter == 700)
        {
#define DIAG_LARGE_SEARCH 0
#if DIAG_LARGE_SEARCH
            char buf[512];
            snprintf(buf, sizeof(buf), "Unit %s: from %d,%d to %d,%d, Time %.2f", (const char*)unit->GetDebugName(), xs,
                     zs, xe, ze, Glob.time - Time(0));
            static char lastBuf[sizeof(buf)];
            if (!strcmp(lastBuf, buf))
            {
                Fail("Same path search twice");
            }
            else
            {
                snprintf(lastBuf, sizeof(lastBuf), "%s", (const char*)buf);
            }
#endif

            if (showTrouble)
            {
                // diagnose pathfinding slowdown
                bool doAttach = !GOperMapDebug;
                if (doAttach)
                {
                    GOperMapDebug = new DebugWindowOperMap();
                    DebugAutoHide = true;
                }
                if (GOperMapDebug)
                {
                    if (doAttach)
                    {
                        GOperMapDebug->Attach(this, unit, xs, zs, xe, ze, heurCost);
                        GOperMapDebug->SetSearchID(searchID);
                    }
                    GOperMapDebug->Update();
                    Sleep(UpdateSleep);
                }
            }
        }
#endif

        best->_open = false;
        xc = best->_x;
        zc = best->_z;
        if (xc == xe && zc == ze)
        {
#if PERF_STATS
#endif
#if LOG_MAPS
            LOG_DEBUG(Script, "Unit {}: Path found in {} iters.", (const char*)unit->GetDebugName(), iter);
#endif

        PathFound:
            cur = best;
            int i = -1;
            while (cur)
            {
                i++;
                cur = cur->_parent;
            }
            _path.Resize(i + 1);
            cur = best;
            while (cur)
            {
                OperInfo& info = _path[i--];
                info._cost = cur->_cost;
                info._x = cur->_x;
                info._z = cur->_z;
                if (soldier && cur->_dir == 0xee)
                {
                    if (cur->_parent)
                    {
                        int x = cur->_parent->_x;
                        int z = cur->_parent->_z;
                        for (int d = 0; d < _doors.Size(); d++)
                        {
                            OperDoor& door = _doors[d];
                            if (door.x == x && door.z == z && door.house)
                            {
                                info.house = door.house;
                                info.from = door.exit;
                                {
                                    int x = cur->_x;
                                    int z = cur->_z;
                                    for (int d = 0; d < _doors.Size(); d++)
                                    {
                                        OperDoor& door = _doors[d];
                                        if (door.x == x && door.z == z && door.house == info.house)
                                        {
                                            info.to = door.exit;
                                            goto TeleportFound;
                                        }
                                    }
                                }
                            }
                        }

                    TeleportFound:;
                        // Nothing to assert here: those values are never set
                    }
                    else
                    {
                        int x = cur->_x;
                        int z = cur->_z;
                        for (int d = 0; d < _doors.Size(); d++)
                        {
                            OperDoor& door = _doors[d];
                            if (door.x == x && door.z == z && door.house)
                            {
                                info.house = door.house;
                                info.from = door.exit;
                                info.to = door.exit;
                                break;
                            }
                        }
                    }
                }

                cur = cur->_parent;
            }
#if _ENABLE_CHEATS
            if (GOperMapDebug && GOperMapDebug->IsAttached())
            {
                GOperMapDebug->Update();
                Sleep(FinishSleep);
                GOperMapDebug->Detach();
                if (DebugAutoHide)
                {
                    GOperMapDebug->Close();
                }
            }
#endif
#if LOG_MAPS
#if LOG_MAPS >= 2
            LogMap(xMin, xMax, zMin, zMax, xs, xe, zs, ze, locks);
#endif
#endif
            return _path.Size() >= 2; // path found - is nontrivial?
        }

        for (int i = 0; i < 16; i += 2) // generate successors - only 8 directions
        {
            xx = xc + direction_delta[i][0];
            zz = zc + direction_delta[i][1];
            int iDir = direction_delta[i][2];

            // cost value of the field
            if (best->_dir == 0xee)
            {
                // from doors (doors may be on unaccessible field)
                cost = OperItemGrid * sqrt(Square(xx - xc) + Square(zz - zc)) * veh->GetType()->GetMinCost();
            }
            else
            {
                cost = GetCost(xc, zc, i, locks, veh, isSoldier);
                // affect by change of direction
                dirD = iDir - best->_dir;
                if (dirD >= 8)
                    dirD -= 16;
                else if (dirD < -8)
                    dirD += 16;
                if (dirD != 0)
                    cost += veh->GetCostTurn(dirD);
            }
            // add to aggregate cost
            PoseidonAssert(best->_cost >= 0);
            if (soldier && best->_cost + cost >= GET_UNACCESSIBLE)
            {
                // search for doors
                bool door = false;
                for (int d = 0; d < _doors.Size(); d++) // generate successors
                {
                    OperDoor& doorIn = _doors[d];
                    if (!doorIn.house)
                        continue;
                    if (doorIn.x != xx)
                        continue;
                    if (doorIn.z != zz)
                        continue;
                    door = true;
                    break;
                }
                // to doors (doors may be on unaccessible field)
                if (door)
                {
                    cost = OperItemGrid * sqrt(Square(xx - xc) + Square(zz - zc)) * veh->GetType()->GetMinCost();
                }
                else
                    continue;
            }
            cost += best->_cost;

            if (xx == xe && zz == ze)
                heur = 0;
            else
            {
                heur = OperHeuristic(xx - xe, zz - ze) * heurCost;
                dirD = AI::CalcDirection(Vector3(xe - xx, 0, ze - zz)) - i;
                if (dirD >= 8)
                    dirD -= 16;
                else if (dirD < -8)
                    dirD += 16;
                if (dirD != 0)
                    heur += veh->GetCostTurn(1) * abs(dirD);
            }
            cur = Item(xx, zz);
            if (!cur)
                continue;
            if (cur->_searchID == searchID)
            {
                if (cur->_open)
                {
                    if (cost + heur < cur->_cost + cur->_heur)
                    {
                        cur->_dir = i;
                        cur->_cost = cost;
                        cur->_heur = heur;
                        cur->_parent = best;
                        openList.HeapUpdateUp(cur);
                    }
                    else
                    {
                        continue;
                    }
                }
                else
                {
                    continue;
                }
            }
            else
            {
                cur->_x = xx;
                cur->_z = zz;
                cur->_dir = i;
                cur->_cost = cost;
                cur->_heur = heur;
                cur->_parent = best;
                cur->_open = true;
                cur->_searchID = searchID;
                openList.HeapInsert(cur);
            }
        } // end of generate successors
        if (!soldier)
            continue;
        for (int i = 0; i < _doors.Size(); i++) // generate successors
        {
            OperDoor& doorIn = _doors[i];
            if (!doorIn.house)
                continue;
            if (doorIn.x != xc)
                continue;
            if (doorIn.z != zc)
                continue;
            for (int j = 0; j < _doors.Size(); j++)
            {
                if (j == i)
                    continue;
                OperDoor& doorOut = _doors[j];
                if (doorOut.house != doorIn.house)
                    continue;

                xx = doorOut.x;
                zz = doorOut.z;
                PoseidonAssert(doorIn.house->GetIPaths());
                cost = doorIn.house->GetIPaths()->GetBType()->GetCoefInsideHeur() * OperItemGrid *
                       sqrt(Square(xx - xc) + Square(zz - zc)) * veh->GetType()->GetMinCost();
                // add to aggregate cost
                PoseidonAssert(best->_cost >= 0);
                cost += best->_cost;
                if (cost >= GET_UNACCESSIBLE)
                    continue;

                if (xx == xe && zz == ze)
                    heur = 0;
                else
                {
                    heur = OperHeuristic(xx - xe, zz - ze) * heurCost;
                    if (best->_dir != 0xee)
                    {
                        dirD = AI::CalcDirection(Vector3(xe - xx, 0, ze - zz)) - best->_dir;
                        if (dirD >= 8)
                            dirD -= 16;
                        else if (dirD < -8)
                            dirD += 16;
                        if (dirD != 0)
                            heur += veh->GetCostTurn(1) * abs(dirD);
                    }
                }
                cur = Item(xx, zz);
                if (!cur)
                    continue;
                if (cur->_searchID == searchID)
                {
                    if (cur->_open)
                    {
                        if (cost + heur < cur->_cost + cur->_heur)
                        {
                            cur->_dir = 0xee;
                            cur->_cost = cost;
                            cur->_heur = heur;
                            cur->_parent = best;
                            openList.HeapUpdateUp(cur);
                        }
                        else
                        {
                            continue;
                        }
                    }
                    else
                    {
                        continue;
                    }
                }
                else
                {
                    cur->_x = xx;
                    cur->_z = zz;
                    cur->_dir = 0xee;
                    cur->_cost = cost;
                    cur->_heur = heur;
                    cur->_parent = best;
                    cur->_open = true;
                    cur->_searchID = searchID;
                    openList.HeapInsert(cur);
                }
            }
        } // end of generate successors
    } // end of search cycle
#endif
}
