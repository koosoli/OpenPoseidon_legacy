// row index is brain (sensor) handle
// column index is target handle

#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/World/Terrain/Visibility.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <stdio.h>
#include <string.h>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>

namespace Poseidon
{
using namespace Foundation;
#define DIAGS 0

SensorRow::SensorRow() = default;

void SensorRow::Init(Person* vehicle, int size)
{
    _info.Realloc(size);
    _info.Resize(size);
    for (int i = 0; i < size; i++)
    {
        _info[i].Init();
    }
    _vehicle = vehicle;
}

void SensorRow::Free()
{
    _info.Clear();
    _vehicle = nullptr;
}

template <class VehicleType>
LSError SensorInfo<VehicleType>::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(ar.SerializeRef("Vehicle", _vehicle, 1))
    return LSOK;
}

void SensorUpdate::SetVisibility(float vis)
{
    int iVis = (int)(vis * 255);
    saturate(iVis, 0, 255);
    vis8 = iVis;
}

void SensorUpdate::Init()
{
    // full dirty
    rowPos = VZero;
    colPos = VZero;
    time = TIME_MIN;
    lastVisible = TIMESEC_MIN;
    vis8 = 0;
}

RString SensorUpdate::DiagText() const
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%.2f (%.1fs, lv %.1fs, %.1fs)", GetVisibility(), Glob.time.Diff(time),
             Glob.time.Diff(lastVisible), Glob.time.Diff(GetLastVisibilityTime()));
    return buf;
}

#define DEF_STRB(x)                        \
    inline const RStringB& GetString_##x() \
    {                                      \
        static RStringB String_##x(#x);    \
        return String_##x;                 \
    }
#define USE_STRB(x) GetString_##x()

DEF_STRB(lastVisible)
DEF_STRB(vis8)
DEF_STRB(time)
DEF_STRB(rowPos)
DEF_STRB(colPos)

bool SensorUpdate::IsDefaultValue(ParamArchive& ar) const
{
    if (Time(lastVisible) < Glob.time - 120)
    {
        // it was not visible for very long time and can be considered never visible
        // - use default values
        return true;
    }
    return false;
}

void SensorUpdate::LoadDefaultValues(ParamArchive& ar)
{
    // if there are no values, use default
    lastVisible = ar.GetArVersion() >= 12 ? TIMESEC_MIN : TIMESEC_MAX;
    vis8 = 0;
    time = Glob.time;
    rowPos = VZero;
    colPos = VZero;
}

LSError SensorUpdate::Serialize(ParamArchive& ar)
{
    // visibility matrix can be huge
    // many items in visibility matrix are "never visible"
    // we do not save such values
    // when loading them, we use some reasonable default
    if (ar.IsSaving())
    {
        if (Time(lastVisible) < Glob.time - 120)
        {
            // it was not visible for very long time and can be considered never visible
            // - use default values
            return LSOK;
        }
    }
    TimeSec defLastVisible = ar.GetArVersion() >= 12 ? TIMESEC_MIN : TIMESEC_MAX;
    PARAM_CHECK(ar.Serialize(USE_STRB(lastVisible), lastVisible, 1, defLastVisible))
    // if there are no values, use default
    PARAM_CHECK(ar.Serialize(USE_STRB(vis8), vis8, 1, 0))
    // if not saved, load as MAX (most recent possible)
    PARAM_CHECK(ar.Serialize(USE_STRB(time), time, 1, Glob.time))
    PARAM_CHECK(ar.Serialize(USE_STRB(rowPos), rowPos, 1, VZero))
    PARAM_CHECK(ar.Serialize(USE_STRB(colPos), colPos, 1, VZero))

    return LSOK;
}

LSError SensorRow::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeDef("Vis", _info, 1))
    return LSOK;
}

float SensorList::Unimportance(EntityAI* from, EntityAI* to, float& defValue)
{
    defValue = 1;
    if (!from)
    {
        return 1e10;
    }
    AIUnit* fromBrain = from->CommanderUnit();
    PoseidonAssert(fromBrain);
    if (!fromBrain)
    {
        return 1e10;
    }
    AIGroup* fromGroup = fromBrain->GetGroup();
    if (!fromGroup)
    {
        return 1e10;
    }
    AICenter* fromCenter = fromGroup->GetCenter();
    // same center units are ignored - thay are always seen and known
    AIUnit* toUnit = to->CommanderUnit();
    if (toUnit)
    {
        AIGroup* toGroup = toUnit->GetGroup();
        if (toGroup)
        {
            AICenter* toCenter = toGroup->GetCenter();
            if (fromCenter == toCenter)
            {
                // it may be renegade, running captive or joined enemy soldier
                if (!fromCenter->IsEnemy(to->GetTargetSide()))
                {
                    if (fromGroup != toGroup)
                    {
                        // unimportant friendly - different group
                        return 1e10;
                    }
                    // unimportant: same group
                    // check visibility only when not alive
                    if (toUnit && toUnit->GetLifeState() == AIUnit::LSAlive)
                    {
                        defValue = 0;
                        return 1e10;
                    }
                }
            }
        }
    }
    // calculate max. allowed dirty
    float unimportance = to->VisibleSize();
    saturateMax(unimportance, 2);
    float dist2 = (from->GetHierachyParent() ? from->WorldTransform().Position() : from->Position()).Distance2(to->Position());
    if (dist2 > Square(200))
    {
        unimportance *= 2;
        if (dist2 > Square(600))
        {
            unimportance *= 2; // cannot be targeted
        }
    }
    return unimportance;
}

SensorList::SensorList() : _lastUpdateRow(0), _lastUpdateCol(0) {}

SensorList::~SensorList() = default;

SensorRowID SensorList::FindRowID(Person* veh)
{
    for (int i = 0; i < _rows.Size(); i++)
    {
        if (_rows[i]._vehicle == veh)
        {
            return SensorRowID(i);
        }
    }
    return SensorRowID(-1);
}
SensorColID SensorList::FindColID(EntityAI* veh)
{
    for (int i = 0; i < _cols.Size(); i++)
    {
        if (_cols[i]._vehicle == veh)
        {
            return SensorColID(i);
        }
    }
    return SensorColID(-1);
}

SensorRowID SensorList::AddRow(Person* veh)
{
    if (!veh->Brain() || veh->Brain()->GetLifeState() == AIUnit::LSDead)
    {
        LOG_DEBUG(World, "Add dead sensor {}", (const char*)veh->GetDebugName());
        // error:
        return (SensorRowID)-1;
    }
    if (veh->IsDammageDestroyed())
    {
        LOG_DEBUG(World, "Add destroyed sensor {}", (const char*)veh->GetDebugName());
        return (SensorRowID)-1;
    }
    // seach for some free slot
    SensorRowID rowID = FindRowID(nullptr);
    if (rowID < 0)
    {
        rowID = SensorRowID(_rows.Add());
    }
    _rows[rowID].Init(veh, _cols.Size());
    //_rows[rowID]._dirty=MaxSensorDirty;
    veh->SetSensorRowID(rowID);
    return rowID;
}

SensorColID SensorList::AddCol(EntityAI* veh)
{
    if (!veh->IsInLandscape())
    {
        Fail("BUG: Adding in-vehicle target.");
    }
    SensorColID colID = FindColID(nullptr);
    if (colID < 0)
    {
        colID = SensorColID(_cols.Add());
        // add this ID to all rows
        for (int row = 0; row < _rows.Size(); row++)
        {
            if (_rows[row]._vehicle != nullptr)
            {
                _rows[row]._info.Access(colID);
            }
        }
    }
    for (int row = 0; row < _rows.Size(); row++)
    {
        if (_rows[row]._vehicle != nullptr)
        {
            _rows[row]._info[colID].Init();
        }
    }
    _cols[colID]._vehicle = veh;
    veh->SetSensorColID(colID);
    return colID;
}
void SensorList::DeleteRow(SensorRowID i)
{
    Person* veh = _rows[i]._vehicle;
    veh->SetSensorRowID(SensorRowID(-1)); // no longer used as sensor
    _rows[i].Free();                      // delete
}

void SensorList::DeleteRow(Person* veh)
{
    SensorRowID index = veh->GetSensorRowID();
    if (index < 0)
    {
        return;
    }
    Log("Removed sensor row %s", (const char*)veh->GetDebugName());
    PoseidonAssert(index == FindRowID(veh));
    _rows[index].Free();                  // delete
    veh->SetSensorRowID(SensorRowID(-1)); // no longer used as sensor
}

void SensorList::DeleteCol(SensorColID i)
{
    EntityAI* veh = _cols[i]._vehicle;
    veh->SetSensorColID(SensorColID(-1)); // no longer used as sensor
    _cols[i]._vehicle = nullptr;          // delete
}

void SensorList::DeleteCol(EntityAI* veh)
{
    SensorColID index = veh->GetSensorColID();
    if (index < 0)
    {
        return;
    }
    Log("Removed sensor column %s", (const char*)veh->GetDebugName());
    PoseidonAssert(index == FindColID(veh));
    DeleteCol(index);
}

void SensorList::Compact()
{
    Log("SensorList::Compact not impemented.");
}

int SensorList::UpdateCell(SensorRowID r, SensorColID c)
{
    SensorRow& row = _rows[r];
    Person* fromDriver = row._vehicle;
    if (!fromDriver)
    {
        return 0;
    }
    SensorCol& col = _cols[c];
    EntityAI* to = col._vehicle;
    if (!to)
    {
        return 0;
    }
    //
    AIUnit* fromUnit = fromDriver->CommanderUnit();
    if (!fromUnit)
    {
        return 0;
    }

    Man* passenger = fromUnit->IsInCargo() ? dyn_cast<Man>(fromDriver) : nullptr;
    EntityAI* from = passenger && passenger->GetActiveCargoWeaponSeat() ? fromDriver : fromUnit->GetVehicle();
    const Vector3 fromPosition = from->GetHierachyParent() ? from->WorldTransform().Position() : from->Position();

    // update single
    SensorUpdate& info = row._info[c];
    if (from == to)
    {
        info.SetVisibility1();
        info.SetLastVisible(Glob.time);
        info.rowPos = fromPosition;
        info.colPos = to->Position();
        info.time = Glob.time;
        return 0;
    }
    float dist2 = fromPosition.Distance2(to->Position());
    float irRange = from->GetType()->GetIRScanRange();
    if (!to->GetType()->GetIRTarget())
    {
        irRange = 0;
    }
    float sensorLimit = floatMax(irRange, TACTICAL_VISIBILITY);
    if (dist2 < Square(sensorLimit))
    {
        float defValue = 0;
        float unimportance = Unimportance(from, to, defValue);

        // unimportant: assume default value
        if (unimportance > 1e5)
        {
            info.SetVisibility(defValue);
            info.rowPos = fromPosition;
            info.colPos = to->Position();
            info.time = Glob.time - 30;
            if (defValue >= 0.02f)
            {
                info.SetLastVisible(Glob.time);
            }
            // set time in past
            // to guarantee quick evaluation whenunimportance would drop down
            // (like in case of death)
            return 0;
        }
        // calculate dirty value
        float toMoved = to->Position().Distance(info.colPos);
        float fromMoved = fromPosition.Distance(info.rowPos);
        float timeMoved = timeMoved = Glob.time.Diff(info.time);

        // sophisticated check (4D: 3D + time)
        float dirty = toMoved + fromMoved + timeMoved * 0.1;
        if (dirty < unimportance)
        {
            return 0;
        }

        // check from/to camera/aiming positions
        float visibility = GLOB_LAND->Visible(from, to);

        info.SetVisibility(visibility);
        info.rowPos = fromPosition;
        info.colPos = to->Position();
        info.time = Glob.time;
        if (visibility > 0.02f)
        {
            info.SetLastVisible(Glob.time);
        }
        GLOB_WORLD->SecondaryAllowSwitch();
#if DIAGS
        LOG_DEBUG(World, "Cell update {} to {} ({},{}) {:.2f}, dirty {:.1f}<{:.1f}", (const char*)from->GetDebugName(),
                  (const char*)to->GetDebugName(), r, c, visibility, dirty, unimportance);
#endif
        return 1;
    }
    else
    {
        info.SetVisibility0();
        info.rowPos = fromPosition;
        info.colPos = to->Position();
        info.time = Glob.time;
        return 0;
    }
}

int SensorList::UpdateRow(SensorRowID id)
{
    int countUpdates = 0;
    SensorRow& row = _rows[id];
    Person* fromDriver = row._vehicle;
    PoseidonAssert(fromDriver);
    AIUnit* fromUnit = fromDriver->CommanderUnit();
    // EntityAI *from = fromUnit->GetVehicle();
    if (!fromUnit)
    {
        LOG_ERROR(World, "No unit sensor row {}", (const char*)fromDriver->GetDebugName());
        row.Free(); // delete - it will not be needed for a long time
        fromDriver->SetSensorRowID(SensorRowID(-1));
        return 0;
    }
    Man* passenger = fromUnit->IsInCargo() ? dyn_cast<Man>(fromDriver) : nullptr;
    if (fromUnit->IsInCargo() && !(passenger && passenger->GetActiveCargoWeaponSeat()))
    {
// Fail("Cargo unit sensor present");
// unit loaded in cargo
#if DIAGS
        LOG_DEBUG(World, "Removed cargo sensor row {}", (const char*)fromDriver->GetDebugName());
#endif
        row.Free(); // delete - it will not be needed for a long time
        fromDriver->SetSensorRowID(SensorRowID(-1));
        return 0;
    }
    for (int i = 0; i < _cols.Size(); i++)
    {
        countUpdates += UpdateCell(id, SensorColID(i));
    }
#if DIAGS
    if (countUpdates > 0)
    {
        LOG_DEBUG(World, "Update sensor row {},{} {} updates", id, (const char*)from->GetDebugName(), countUpdates);
    }
#endif
    return countUpdates;
}

int SensorList::UpdateCol(SensorColID id)
{
    // LOG_DEBUG(World, "UpdateCol {}",id);
    //  last column pos. change is col._posUpdatedTime
    int countUpdates = 0;
    // SensorCol &col=_cols[id];
    // EntityAI *to=;
    PoseidonAssert(_cols[id]._vehicle);
    // bool someDirty=false;

    for (int i = 0; i < _rows.Size(); i++)
    {
        countUpdates += UpdateCell(SensorRowID(i), id);
    }

#if DIAGS
    if (countUpdates > 0)
    {
        LOG_DEBUG(World, "Update target col {},{}  {} updates", id, (const char*)to->GetDebugName(), countUpdates);
    }
#endif
    return countUpdates;
}

LSError SensorList::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(ar.Serialize("Cols", _cols, 1))
    PARAM_CHECK(ar.Serialize("Rows", _rows, 1))
    PARAM_CHECK(ar.Serialize("lastUpdateRow", _lastUpdateRow, 1))
    PARAM_CHECK(ar.Serialize("lastUpdateCol", _lastUpdateCol, 1))
    return LSOK;
}

void SensorList::CheckPos()
{
    // verify no targets are inside the vehcile
    // if they are, their position is invalid
    for (int i = 0; i < _cols.Size(); i++)
    {
        SensorCol& col = _cols[i];
        EntityAI* veh = col._vehicle;
        if (!veh)
        {
            continue;
        }
        // vehicle may be person that is inside of another vehicle
        if (veh->IsInLandscape())
        {
            continue;
        }
        LOG_DEBUG(World, "Removing in-vehicle target {}", (const char*)veh->GetDebugName());
        DeleteCol(SensorColID(i));
        // target should be removed when getting in the vehicle
    }
    // remove dead sensors
    for (int i = 0; i < _rows.Size(); i++)
    {
        SensorRow& row = _rows[i];
        Person* veh = row._vehicle;
        if (!veh)
        {
            continue;
        }
        if (veh->IsDammageDestroyed() && !veh->CommanderUnit())
        {
            DeleteRow(SensorRowID(i));
            LOG_DEBUG(World, "Removing dead sensor {}", (const char*)veh->GetDebugName());
        }
    }
}

int SensorList::SmartUpdateAll()
{
    CheckPos();
    int sum = 0;
    // CWR fork: raised from 4/4 (tuned for 2001 CPUs) so line of sight
    // refreshes quickly even with hundreds of units
    const int maxTests = 16;
    const int maxCalcs = 32;
    for (int maxUpdates = maxCalcs, maxRowCols = maxTests, maxChecks = _rows.Size(); --maxChecks >= 0;)
    {
        if (++_lastUpdateRow >= _rows.Size())
        {
            _lastUpdateRow = 0;
        }
        if (_rows.Size() <= _lastUpdateRow)
        {
            break;
        }
        SensorRow& row = _rows[_lastUpdateRow];
        if (row._vehicle != nullptr)
        {
            int updates = UpdateRow(SensorRowID(_lastUpdateRow));
            GLOB_WORLD->SecondaryAllowSwitch();
            if ((maxUpdates -= updates) <= 0)
            {
                break;
            }
            sum += updates;
            if (--maxRowCols < 0)
            {
                break;
            }
        }
    }
    for (int maxUpdates = maxCalcs, maxRowCols = maxTests, maxChecks = _cols.Size(); --maxChecks >= 0;)
    {
        if (++_lastUpdateCol >= _cols.Size())
        {
            _lastUpdateCol = 0;
        }
        if (_cols.Size() <= _lastUpdateCol)
        {
            break;
        }
        SensorCol& col = _cols[_lastUpdateCol];
        if (col._vehicle != nullptr)
        {
            int updates = UpdateCol(SensorColID(_lastUpdateCol));
            GLOB_WORLD->SecondaryAllowSwitch();
            if ((maxUpdates -= updates) <= 0)
            {
                break;
            }
            sum += updates;
            if (--maxRowCols < 0)
            {
                break;
            }
        }
    }
#if DIAGS
    if (sum > 0)
    {
        LOG_DEBUG(World, "{:5f}: Visibility: {} updates", Glob.time - Time(0), sum);
    }
#endif
    // ADD_COUNTER(sCell,sum);
    return sum;
}

int SensorList::UpdateAll()
{
    CheckPos();
    int sum = 0;
    int index;
    for (index = 0; index < _rows.Size(); index++)
    {
        if (_rows[index]._vehicle != nullptr)
        {
            sum += UpdateRow(SensorRowID(index));
        }
    }
    for (index = 0; index < _cols.Size(); index++)
    {
        if (_cols[index]._vehicle != nullptr)
        {
            sum += UpdateCol(SensorColID(index));
        }
    }
#if DIAGS
    LOG_DEBUG(World, "Visibility: {} updates", sum);
#endif
    // ADD_COUNTER(sCell,sum);
    return sum;
}

float SensorList::GetVisibility(Person* from, EntityAI* to) const
{
    if (from->GetSensorRowID() < 0)
    {
#if DIAGS
        LOG_DEBUG(World, "Added sensor row {}", (const char*)from->GetDebugName());
#endif
        SensorRowID id = const_cast<SensorList*>(this)->AddRow(from);
        if (id < 0)
        {
            return 0;
        }
    }
    if (to->GetSensorColID() < 0)
    {
#if DIAGS
        LOG_DEBUG(World, "Added sensor col {}", (const char*)to->GetDebugName());
#endif
        SensorColID id = const_cast<SensorList*>(this)->AddCol(to);
        if (id < 0)
        {
            return 0;
        }
    }
    if (from->GetSensorRowID() >= _rows.Size())
    {
        LOG_ERROR(World, "{}: bad sensor row {} (>={})", (const char*)from->GetDebugName(), (int)from->GetSensorRowID(),
                  _rows.Size());
        return 0;
    }
    const SensorRow& row = _rows[from->GetSensorRowID()];
    if (to->GetSensorColID() >= row._info.Size())
    {
        LOG_ERROR(World, "{}: bad sensor col {} (>={})", (const char*)to->GetDebugName(), (int)to->GetSensorColID(),
                  row._info.Size());
        return 0;
    }
    return row._info[to->GetSensorColID()].GetVisibility();
}

Time SensorUpdate::GetLastVisibilityTime() const
{
    if (lastVisible <= TIMESEC_MIN)
    {
        return TIME_MIN;
    }
    if (time < TIME_MIN + 1 || Time(lastVisible) >= time - 1)
    {
        return Glob.time;
    }
    return lastVisible;
}

Time SensorList::GetVisibilityTime(Person* from, EntityAI* to) const
{
    // it is therefore optimisitic and if real value is not know, it assumes
    // target is visible now
    if (from->GetSensorRowID() < 0)
    {
        return Glob.time;
    }
    if (to->GetSensorColID() < 0)
    {
        return Glob.time;
    }
    if (from->GetSensorRowID() >= _rows.Size())
    {
        return Glob.time;
    }
    const SensorRow& row = _rows[from->GetSensorRowID()];
    if (to->GetSensorColID() >= row._info.Size())
    {
        return Glob.time;
    }
    const SensorUpdate& upd = row._info[to->GetSensorColID()];
    // if time of last visibility is same as time of last check
    // return it is visible now
    return upd.GetLastVisibilityTime();
}

RString SensorList::DiagText(Person* from, EntityAI* to) const
{
    char buf[256];
    if (from->GetSensorRowID() < 0)
    {
        return "No row";
    }
    if (to->GetSensorColID() < 0)
    {
        return "No col";
    }
    const SensorRow& row = _rows[from->GetSensorRowID()];
    // return row._vis[to->GetSensorColID()]*(1.0/255);
    const SensorUpdate& info = row._info[to->GetSensorColID()];
    snprintf(buf, sizeof(buf), "Cell %d,%d: ", static_cast<int>(from->GetSensorRowID()),
             static_cast<int>(to->GetSensorColID()));
    strncat(buf, info.DiagText(), sizeof(buf) - strlen(buf) - 1);
    return buf;
}

bool SensorList::CheckStructure() const
{
    return true;
}

} // namespace Poseidon
