
#include <Poseidon/AI/ArcadeTemplate.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Random/randomGen.hpp>

#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/World/World.hpp>

#include <Poseidon/Network/Network.hpp>

#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <stdio.h>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>

#include <Poseidon/UI/Locale/StringtableExt.hpp>
#include <Poseidon/Game/Chat.hpp>

void SelectLeader(ArcadeGroupInfo& gInfo);

namespace Poseidon
{

float RankToSkill(int rank);

ArcadeUnitInfo* ArcadeTemplate::FindUnit(int id, int& idGroup, int& idUnit)
{
    int i, n = groups.Size();
    for (i = 0; i < n; i++)
    {
        ArcadeGroupInfo& gInfo = groups[i];

        int j, m = gInfo.units.Size();
        for (j = 0; j < m; j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            if (uInfo.id == id)
            {
                idGroup = i;
                idUnit = j;
                return &uInfo;
            }
        }
    }
    n = emptyVehicles.Size();
    for (i = 0; i < n; i++)
    {
        ArcadeUnitInfo& uInfo = emptyVehicles[i];
        if (uInfo.id == id)
        {
            idGroup = -1;
            idUnit = i;
            return &uInfo;
        }
    }
    idGroup = -1;
    idUnit = -1;
    return nullptr;
}

ArcadeUnitInfo* ArcadeTemplate::FindPlayer()
{
    int i, n = groups.Size();
    for (i = 0; i < n; i++)
    {
        ArcadeGroupInfo& gInfo = groups[i];

        int j, m = gInfo.units.Size();
        for (j = 0; j < m; j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            switch (uInfo.player)
            {
                case APPlayerCommander:
                case APPlayerDriver:
                case APPlayerGunner:
                    return &uInfo;
            }
        }
    }

    return nullptr;
}

ArcadeGroupInfo* ArcadeTemplate::FindPlayerGroup()
{
    int i, n = groups.Size();
    for (i = 0; i < n; i++)
    {
        ArcadeGroupInfo& gInfo = groups[i];

        int j, m = gInfo.units.Size();
        for (j = 0; j < m; j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            switch (uInfo.player)
            {
                case APPlayerCommander:
                case APPlayerDriver:
                case APPlayerGunner:
                    return &gInfo;
            }
        }
    }

    return nullptr;
}

ArcadeMarkerInfo* ArcadeTemplate::FindMarker(const char* name)
{
    for (int i = 0; i < markers.Size(); i++)
    {
        ArcadeMarkerInfo& mInfo = markers[i];
        if (stricmp(name, mInfo.name) == 0)
        {
            return &mInfo;
        }
    }

    return nullptr;
}

ArcadeUnitInfo* ArcadeTemplate::FindVehicle(const char* name)
{
    for (int i = 0; i < emptyVehicles.Size(); i++)
    {
        ArcadeUnitInfo& uInfo = emptyVehicles[i];
        if (stricmp(name, uInfo.name) == 0)
        {
            return &uInfo;
        }
    }
    for (int i = 0; i < groups.Size(); i++)
    {
        ArcadeGroupInfo& gInfo = groups[i];
        for (int j = 0; j < gInfo.units.Size(); j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            if (stricmp(name, uInfo.name) == 0)
            {
                return &uInfo;
            }
        }
    }
    return nullptr;
}

ArcadeSensorInfo* ArcadeTemplate::FindSensor(const char* name)
{
    for (int i = 0; i < sensors.Size(); i++)
    {
        ArcadeSensorInfo& sInfo = sensors[i];
        if (stricmp(name, sInfo.name) == 0)
        {
            return &sInfo;
        }
    }
    for (int i = 0; i < groups.Size(); i++)
    {
        ArcadeGroupInfo& gInfo = groups[i];
        for (int j = 0; j < gInfo.sensors.Size(); j++)
        {
            ArcadeSensorInfo& sInfo = gInfo.sensors[j];
            if (stricmp(name, sInfo.name) == 0)
            {
                return &sInfo;
            }
        }
    }
    return nullptr;
}

void ArcadeTemplate::Clear()
{
    groups.Clear();
    emptyVehicles.Clear();
    sensors.Clear();
    markers.Clear();

    showHUD = true;
    showMap = true;
    showWatch = true;
    showCompass = true;
    showNotepad = true;
    showGPS = false;

    nextSyncId = 0;
    nextVehId = 0;

    addOns.Clear();
    missingAddOns.Clear();
}

void ArcadeTemplate::GroupDelete(int ig)
{
    AI_ERROR(ig >= 0);
    if (ig >= 0)
    {
        groups.Delete(ig);
    }
}

void ArcadeTemplate::UnitDelete(int ig, int iu)
{
    if (ig == -1)
    {
        emptyVehicles.Delete(iu);
    }
    else
    {
        ArcadeGroupInfo& gInfo = groups[ig];
        gInfo.units.Delete(iu);
        if (gInfo.units.Size() == 0)
        {
            groups.Delete(ig);
        }
        else
        {
            SelectLeader(gInfo);
        }
    }
}

void ArcadeTemplate::WaypointDelete(int ig, int iw)
{
    ArcadeGroupInfo& gInfo = groups[ig];
    gInfo.waypoints.Delete(iw);
}

bool ArcadeTemplate::UnitChangeGroup(int ig, int iu, int ignew)
{
    if (ignew >= 0)
    {
        if (ignew == ig)
        {
            return false;
        }
        // remove from old group
        ArcadeGroupInfo& gInfoOld = groups[ig];
        ArcadeUnitInfo uInfo = gInfoOld.units[iu];
        gInfoOld.units.Delete(iu);
        if (gInfoOld.units.Size() == 0)
        {
            groups.Delete(ig);
            if (ignew > ig)
            {
                ignew--;
            }
        }
        else
        {
            SelectLeader(gInfoOld);
        }
        // insert into new group
        ArcadeGroupInfo& gInfoNew = groups[ignew];
        gInfoNew.units.Add(uInfo);
        // select leader
        SelectLeader(gInfoNew);
        return true;
    }
    else
    {
        // split group
        ArcadeGroupInfo& gInfoOld = groups[ig];
        if (gInfoOld.units.Size() <= 1)
        {
            return false;
        }
        // remove from old group
        ArcadeUnitInfo uInfo = gInfoOld.units[iu];
        gInfoOld.units.Delete(iu);
        SelectLeader(gInfoOld);
        // insert into new group
        ArcadeGroupInfo gInfoNew;
        gInfoNew.units.Add(uInfo);
        gInfoNew.side = uInfo.side;
        // select leader
        SelectLeader(gInfoNew);

        groups.Add(gInfoNew);
        return true;
    }
}

void ArcadeTemplate::WaypointChangeSynchro(int ig, int iw, int ig1, int iw1)
{
    if (ig1 >= 0)
    {
        if (ig1 != ig)
        {
            // synchronization
            ArcadeGroupInfo& gInfo1 = groups[ig];
            ArcadeWaypointInfo& wInfo1 = gInfo1.waypoints[iw];
            ArcadeGroupInfo& gInfo2 = groups[ig1];
            ArcadeWaypointInfo& wInfo2 = gInfo2.waypoints[iw1];

            // check if synchronization does not exist
            int found = 0;
            for (int i = 0; i < wInfo1.synchronizations.Size(); i++)
            {
                int sync = wInfo1.synchronizations[i];
                for (int j = 0; j < wInfo2.synchronizations.Size(); j++)
                {
                    if (wInfo2.synchronizations[j] == sync)
                    {
                        found++;
                    }
                }
            }

            if (found == 0)
            {
                int sync = nextSyncId++;
                wInfo1.synchronizations.Add(sync);
                wInfo2.synchronizations.Add(sync);
            }
            else
            {
                AI_ERROR(found == 1);
            }
        }
        else
        {
        }
    }
    else
    {
        // split synchronization
        ArcadeGroupInfo& gInfo = groups[ig];
        ArcadeWaypointInfo& wInfo = gInfo.waypoints[iw];
        wInfo.synchronizations.Clear();
        CheckSynchro();
    }
}

bool ArcadeTemplate::UnitChangePosition(int ig, int iu, Vector3Val pos)
{
    ArcadeUnitInfo* uInfo = nullptr;
    if (ig == -1)
    {
        AI_ERROR(iu >= 0 && iu < emptyVehicles.Size());
        if (iu < 0 || iu >= emptyVehicles.Size())
        {
            return false;
        }
        uInfo = &emptyVehicles[iu];
    }
    else
    {
        AI_ERROR(ig >= 0 && ig < groups.Size());
        if (ig < 0 || ig >= groups.Size())
        {
            return false;
        }
        ArcadeGroupInfo& gInfo = groups[ig];

        AI_ERROR(iu >= 0 && iu < gInfo.units.Size());
        if (iu < 0 || iu >= gInfo.units.Size())
        {
            return false;
        }
        uInfo = &gInfo.units[iu];
    }
    AI_ERROR(uInfo);

    bool changedEnough = (uInfo->position - pos).SquareSizeXZ() >= Square(2);
    uInfo->position = pos;
    return changedEnough;
}

bool ArcadeTemplate::GroupChangePosition(int ig, int iu, Vector3Val pos)
{
    AI_ERROR(ig >= 0);
    ArcadeGroupInfo& gInfo = groups[ig];
    ArcadeUnitInfo& uInfo = gInfo.units[iu];

    Vector3 diff = pos - uInfo.position;
    bool changedEnough = diff.SquareSizeXZ() >= Square(2);

    for (int i = 0; i < gInfo.units.Size(); i++)
    {
        ArcadeUnitInfo& uInfo = gInfo.units[i];
        Point3 pos = uInfo.position + diff;
        pos[1] = GLOB_LAND->RoadSurfaceY(pos[0], pos[2]);
        uInfo.position = pos;
    }

    return changedEnough;
}

bool ArcadeTemplate::WaypointChangePosition(int ig, int iw, Vector3Val pos)
{
    ArcadeGroupInfo& gInfo = groups[ig];
    ArcadeWaypointInfo& wInfo = gInfo.waypoints[iw];

    bool changedEnough = (wInfo.position - pos).SquareSizeXZ() >= Square(2);
    wInfo.position = pos;
    return changedEnough;
}

void ArcadeTemplate::UnitUpdate(int& ig, int& iu, ArcadeUnitInfo& uInfo)
{
    ArcadeUnitInfo* info = nullptr;
    if (iu < 0)
    {
        // insert unit
        AI_ERROR(ig < 0);
        if (uInfo.side == TEmpty)
        {
            // empty vehicles
            ig = -1;
            uInfo.id = -1;
            iu = emptyVehicles.Add(uInfo);
            info = &emptyVehicles[iu];
        }
        else
        {
            // Find group to insert into
            float minDist2 = Square(100);

            int i, n = groups.Size();
            for (i = 0; i < n; i++)
            {
                ArcadeGroupInfo& gInfo = groups[i];
                if (gInfo.side != uInfo.side)
                {
                    continue;
                }
                int j, m = gInfo.units.Size(), jLeader = -1;
                for (j = 0; j < m; j++)
                {
                    ArcadeUnitInfo& unit = gInfo.units[j];
                    if (unit.leader)
                    {
                        jLeader = j;
                        break;
                    }
                }
                if (jLeader >= 0)
                {
                    ArcadeUnitInfo& unit = gInfo.units[jLeader];
                    float dist2 = (unit.position - uInfo.position).SquareSizeXZ();
                    if (dist2 <= minDist2)
                    {
                        minDist2 = dist2;
                        ig = i;
                    }
                }
            }
            if (ig < 0)
            {
                ig = groups.Add();
                groups[ig].side = uInfo.side;
            }
            uInfo.id = -1;
            ArcadeGroupInfo& gInfo = groups[ig];
            iu = gInfo.units.Add(uInfo);
            info = &gInfo.units[iu];
        }
    }
    else
    {
        // edit unit
        if (uInfo.side == TEmpty)
        {
            // empty vehicles
            ig = -1;
            emptyVehicles[iu] = uInfo;
            info = &emptyVehicles[iu];
        }
        else
        {
            AI_ERROR(ig >= 0);
            ArcadeGroupInfo& gInfo = groups[ig];
            gInfo.units[iu] = uInfo;
            info = &gInfo.units[iu];
        }
    }

    if (info->player == APPlayerCommander || info->player == APPlayerDriver || info->player == APPlayerGunner)
    {
        AI_ERROR(ig >= 0);
        ArcadeUnitPlayer p = info->player;
        info->player = APNonplayable;
        // reset player flag for previous player
        ArcadeUnitInfo* unit = FindPlayer();
        if (unit)
        {
            unit->player = APNonplayable;
        }
        // set player flag
        info->player = p;

        AI_ERROR(info->player != APNonplayable && info->player != APPlayableCDG);
        if (info->player == p)
        {
            Glob.header.playerSide = (TargetSide)info->side;
        }
    }

    RString iconName = Pars >> "CfgVehicles" >> info->vehicle >> "icon";
    info->icon = GlobLoadTexture(GetPictureName(iconName));
    info->size = Pars >> "CfgVehicles" >> info->vehicle >> "mapSize";
    if (info->id < 0)
    {
        info->id = nextVehId++;
    }
    if (ig >= 0)
    {
        ArcadeGroupInfo& gInfo = groups[ig];
        SelectLeader(gInfo);
    }
}

void ArcadeTemplate::WaypointUpdate(int ig, int iw, int& iwnew, ArcadeWaypointInfo& waypoint)
{
    ArcadeGroupInfo& gInfo = groups[ig];
    if (iw < 0)
    {
        AI_ERROR(iwnew >= 0);
        gInfo.waypoints.Insert(iwnew, waypoint);
    }
    else if (iwnew == iw || iwnew < 0)
    {
        iwnew = iw;
        gInfo.waypoints[iw] = waypoint;
    }
    else
    {
        gInfo.waypoints.Delete(iw);
        if (iwnew > iw)
        {
            iwnew--;
        }
        gInfo.waypoints.Insert(iwnew, waypoint);
    }
}

void ArcadeTemplate::SensorUpdate(int ig, int index, ArcadeSensorInfo& sInfo)
{
    if (ig < 0)
    {
        if (index < 0)
        {
            sensors.Add(sInfo);
        }
        else
        {
            sensors[index] = sInfo;
        }
    }
    else
    {
        ArcadeGroupInfo& gInfo = groups[ig];
        if (index < 0)
        {
            index = gInfo.sensors.Add(sInfo);
        }
        else
        {
            gInfo.sensors[index] = sInfo;
        }
    }
}

void ArcadeTemplate::SensorDelete(int ig, int index)
{
    if (ig < 0)
    {
        sensors.Delete(index);
    }
    else
    {
        ArcadeGroupInfo& gInfo = groups[ig];
        gInfo.sensors.Delete(index);
    }
}

bool ArcadeTemplate::SensorChangePosition(int ig, int index, Vector3Val pos)
{
    ArcadeSensorInfo* sInfo = nullptr;
    if (ig < 0)
    {
        sInfo = &sensors[index];
    }
    else
    {
        ArcadeGroupInfo& gInfo = groups[ig];
        sInfo = &gInfo.sensors[index];
    }

    bool changedEnough = (sInfo->position - pos).SquareSizeXZ() >= Square(2);
    sInfo->position = pos;
    return changedEnough;
}

bool ArcadeTemplate::SensorChangeGroup(int ig, int index, int ignew)
{
    if (ignew == ig)
    {
        return false;
    }

    ArcadeSensorInfo sInfo;
    if (ig >= 0)
    {
        // remove from old group
        ArcadeGroupInfo& gInfoOld = groups[ig];
        sInfo = gInfoOld.sensors[index];
        gInfoOld.sensors.Delete(index);
    }
    else
    {
        // remove from flags
        sInfo = sensors[index];
        sensors.Delete(index);
    }

    if (ignew >= 0)
    {
        // insert into new group
        ArcadeGroupInfo& gInfoNew = groups[ignew];
        sInfo.activationBy = ASAGroup;
        sInfo.idStatic = -1;
        sInfo.idVehicle = -1;
        gInfoNew.sensors.Add(sInfo);
    }
    else
    {
        if (sInfo.activationBy == ASAGroup)
        {
            sInfo.activationBy = ASANone;
        }
        sensors.Add(sInfo);
    }
    return true;
}

void ArcadeTemplate::SensorChangeVehicle(int ig, int index, int id)
{
    ArcadeSensorInfo* sInfo = nullptr;
    if (ig >= 0)
    {
        ArcadeGroupInfo& gInfo = groups[ig];
        sInfo = &gInfo.sensors[index];
    }
    else
    {
        sInfo = &sensors[index];
    }
    sInfo->idStatic = -1;
    sInfo->idVehicle = id;
    sInfo->activationBy = ASAVehicle;
    if (ig >= 0)
    {
        SensorChangeGroup(ig, index, -1);
    }
}

void ArcadeTemplate::SensorChangeStatic(int ig, int index, int id)
{
    ArcadeSensorInfo* sInfo = nullptr;
    if (ig >= 0)
    {
        ArcadeGroupInfo& gInfo = groups[ig];
        sInfo = &gInfo.sensors[index];
    }
    else
    {
        sInfo = &sensors[index];
    }
    sInfo->idStatic = id;
    sInfo->idVehicle = -1;
    sInfo->activationBy = ASAStatic;
    if (ig >= 0)
    {
        SensorChangeGroup(ig, index, -1);
    }
}

void ArcadeTemplate::SensorChangeSynchro(int ig, int index, int ig1, int iw1)
{
    ArcadeSensorInfo* sInfo = nullptr;
    if (ig >= 0)
    {
        ArcadeGroupInfo& gInfo = groups[ig];
        sInfo = &gInfo.sensors[index];
    }
    else
    {
        sInfo = &sensors[index];
    }
    AI_ERROR(sInfo);

    if (ig1 >= 0)
    {
        ArcadeGroupInfo& gInfo = groups[ig1];
        ArcadeWaypointInfo& wInfo = gInfo.waypoints[iw1];

        // check if synchronization does not exist
        int found = 0;
        for (int i = 0; i < wInfo.synchronizations.Size(); i++)
        {
            int sync = wInfo.synchronizations[i];
            for (int j = 0; j < sInfo->synchronizations.Size(); j++)
            {
                if (sInfo->synchronizations[j] == sync)
                {
                    found++;
                }
            }
        }

        if (found == 0)
        {
            int sync = nextSyncId++;
            wInfo.synchronizations.Add(sync);
            sInfo->synchronizations.Add(sync);
        }
        else
        {
            AI_ERROR(found == 1);
        }
    }
    else
    {
        // split synchronization
        sInfo->synchronizations.Clear();
        CheckSynchro();
    }
}

void ArcadeTemplate::MarkerUpdate(int index, ArcadeMarkerInfo& mInfo)
{
    if (index < 0)
    {
        index = markers.Add(mInfo);
    }
    else
    {
        if (index >= markers.Size())
        {
            markers.Resize(index + 1);
        }
        markers[index] = mInfo;
    }

    // a marker type the config lacks (an Arma mission's) keeps no icon instead of failing
    const ParamEntry* all = Pars.FindEntry("CfgMarkers");
    const ParamEntry* cls =
        all && markers[index].type.GetLength() > 0 ? all->FindEntry(markers[index].type) : nullptr;
    if (cls && cls->IsClass() && cls->FindEntry("icon") && cls->FindEntry("size"))
    {
        markers[index].icon = GlobLoadTexture(GetPictureName((*cls) >> "icon"));
        markers[index].size = (*cls) >> "size";
    }
}

void ArcadeTemplate::MarkerDelete(int index)
{
    markers.Delete(index);
}

bool ArcadeTemplate::MarkerChangePosition(int index, Vector3Val pos)
{
    ArcadeMarkerInfo& mInfo = markers[index];

    bool changedEnough = (mInfo.position - pos).SquareSizeXZ() >= Square(2);
    mInfo.position = pos;
    return changedEnough;
}

void ArcadeTemplate::UnitAddMarker(int ig, int index, int indexMarker)
{
    ArcadeUnitInfo* uInfo = nullptr;
    if (ig < 0)
    {
        uInfo = &emptyVehicles[index];
    }
    else
    {
        ArcadeGroupInfo& gInfo = groups[ig];
        uInfo = &gInfo.units[index];
    }
    AI_ERROR(uInfo);
    RString name = markers[indexMarker].name;
    int n = uInfo->markers.Size();
    for (int i = 0; i < n; i++)
    {
        if (stricmp(uInfo->markers[i], name) == 0)
        {
            return; // already in list
        }
    }
    uInfo->markers.Add(name);
}

void ArcadeTemplate::RemoveMarker(int indexMarker)
{
    RString name = markers[indexMarker].name;
    int n = groups.Size();
    for (int i = 0; i < n; i++)
    {
        ArcadeGroupInfo& gInfo = groups[i];
        int m = gInfo.units.Size();
        for (int j = 0; j < m; j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            int o = uInfo.markers.Size();
            for (int k = 0; k < o; k++)
            {
                if (stricmp(uInfo.markers[k], name) == 0)
                {
                    uInfo.markers.Delete(k);
                    break; // max once in each list
                }
            }
        }
    }
    int m = emptyVehicles.Size();
    for (int j = 0; j < m; j++)
    {
        ArcadeUnitInfo& uInfo = emptyVehicles[j];
        int o = uInfo.markers.Size();
        for (int k = 0; k < o; k++)
        {
            if (stricmp(uInfo.markers[k], name) == 0)
            {
                uInfo.markers.Delete(k);
                break; // max once in each list
            }
        }
    }
}

void ArcadeTemplate::AddGroup(const ParamEntry& cls, Vector3Par position)
{
    int ig = groups.Add();
    ArcadeGroupInfo& group = groups[ig];
    for (int i = 0; i < cls.GetEntryCount(); i++)
    {
        const ParamEntry& entry = cls.GetEntry(i);
        if (!entry.IsClass())
        {
            continue;
        }
        int iu = group.units.Add();
        ArcadeUnitInfo& unit = group.units[iu];
        int side = entry >> "side";
        unit.side = (TargetSide)side;
        unit.vehicle = entry >> "vehicle";
        RString iconName = Pars >> "CfgVehicles" >> unit.vehicle >> "icon";
        unit.icon = GlobLoadTexture(GetPictureName(iconName));
        unit.size = Pars >> "CfgVehicles" >> unit.vehicle >> "mapSize";

        RStringB rank = entry >> "rank";
        unit.rank = Foundation::GetEnumValue<Rank>(rank);
        unit.skill = RankToSkill(unit.rank);
        unit.position = position;
        float offset = (entry >> "position")[0];
        unit.position[0] += offset;
        offset = (entry >> "position")[1];
        unit.position[2] += offset;
        offset = (entry >> "position")[2];
        unit.position[1] += offset;
        unit.id = nextVehId++;
    }
    SelectLeader(group);
}

struct ConversionItem
{
    bool used;
    int newIndex;
};

void ArcadeTemplate::Compact()
{
    AutoArray<ConversionItem> table;

    // vehicle IDs
    table.Resize(nextVehId);
    for (int i = 0; i < nextVehId; i++)
    {
        table[i].used = false;
        table[i].newIndex = -1;
    }
    for (int i = 0; i < emptyVehicles.Size(); i++)
    {
        ArcadeUnitInfo& uInfo = emptyVehicles[i];
        AI_ERROR(uInfo.id >= 0);
        AI_ERROR(uInfo.id < nextVehId);
        table[uInfo.id].used = true;
    }
    for (int i = 0; i < groups.Size(); i++)
    {
        ArcadeGroupInfo& gInfo = groups[i];
        for (int j = 0; j < gInfo.units.Size(); j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            AI_ERROR(uInfo.id >= 0);
            AI_ERROR(uInfo.id < nextVehId);
            table[uInfo.id].used = true;
        }
    }
    int index = 0;
    for (int i = 0; i < nextVehId; i++)
    {
        if (table[i].used)
        {
            table[i].newIndex = index++;
        }
    }
    nextVehId = index;
    for (int i = 0; i < emptyVehicles.Size(); i++)
    {
        ArcadeUnitInfo& uInfo = emptyVehicles[i];
        uInfo.id = table[uInfo.id].newIndex;
    }
    for (int i = 0; i < sensors.Size(); i++)
    {
        ArcadeSensorInfo& sInfo = sensors[i];
        if (sInfo.idVehicle >= 0)
        {
            AI_ERROR(sInfo.idVehicle < table.Size());
            sInfo.idVehicle = table[sInfo.idVehicle].newIndex;
        }
    }
    for (int i = 0; i < groups.Size(); i++)
    {
        ArcadeGroupInfo& gInfo = groups[i];
        for (int j = 0; j < gInfo.units.Size(); j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            uInfo.id = table[uInfo.id].newIndex;
        }
        for (int j = 0; j < gInfo.sensors.Size(); j++)
        {
            ArcadeSensorInfo& sInfo = gInfo.sensors[j];
            if (sInfo.idVehicle >= 0)
            {
                AI_ERROR(sInfo.idVehicle < table.Size());
                sInfo.idVehicle = table[sInfo.idVehicle].newIndex;
            }
        }
        for (int j = 0; j < gInfo.waypoints.Size(); j++)
        {
            ArcadeWaypointInfo& wInfo = gInfo.waypoints[j];
            if (wInfo.id >= 0)
            {
                AI_ERROR(wInfo.id < table.Size());
                wInfo.id = table[wInfo.id].newIndex;
            }
        }
    }

    // synchronization IDs
    table.Resize(nextSyncId);
    for (int i = 0; i < nextSyncId; i++)
    {
        table[i].used = false;
        table[i].newIndex = -1;
    }
    for (int i = 0; i < sensors.Size(); i++)
    {
        ArcadeSensorInfo& sInfo = sensors[i];
        for (int s = 0; s < sInfo.synchronizations.Size(); s++)
        {
            int sync = sInfo.synchronizations[s];
            AI_ERROR(sync >= 0);
            AI_ERROR(sync < nextSyncId);
            table[sync].used = true;
        }
    }
    for (int i = 0; i < groups.Size(); i++)
    {
        ArcadeGroupInfo& gInfo = groups[i];
        for (int j = 0; j < gInfo.sensors.Size(); j++)
        {
            ArcadeSensorInfo& sInfo = gInfo.sensors[j];
            for (int s = 0; s < sInfo.synchronizations.Size(); s++)
            {
                int sync = sInfo.synchronizations[s];
                AI_ERROR(sync >= 0);
                AI_ERROR(sync < nextSyncId);
                table[sync].used = true;
            }
        }
        for (int j = 0; j < gInfo.waypoints.Size(); j++)
        {
            ArcadeWaypointInfo& wInfo = gInfo.waypoints[j];
            for (int s = 0; s < wInfo.synchronizations.Size(); s++)
            {
                int sync = wInfo.synchronizations[s];
                AI_ERROR(sync >= 0);
                AI_ERROR(sync < nextSyncId);
                table[sync].used = true;
            }
        }
    }
    index = 0;
    for (int i = 0; i < nextSyncId; i++)
    {
        if (table[i].used)
        {
            table[i].newIndex = index++;
        }
    }
    nextSyncId = index;
    for (int i = 0; i < sensors.Size(); i++)
    {
        ArcadeSensorInfo& sInfo = sensors[i];
        for (int s = 0; s < sInfo.synchronizations.Size(); s++)
        {
            int sync = sInfo.synchronizations[s];
            sInfo.synchronizations[s] = table[sync].newIndex;
        }
    }
    for (int i = 0; i < groups.Size(); i++)
    {
        ArcadeGroupInfo& gInfo = groups[i];
        for (int j = 0; j < gInfo.sensors.Size(); j++)
        {
            ArcadeSensorInfo& sInfo = gInfo.sensors[j];
            for (int s = 0; s < sInfo.synchronizations.Size(); s++)
            {
                int sync = sInfo.synchronizations[s];
                sInfo.synchronizations[s] = table[sync].newIndex;
            }
        }
        for (int j = 0; j < gInfo.waypoints.Size(); j++)
        {
            ArcadeWaypointInfo& wInfo = gInfo.waypoints[j];
            for (int s = 0; s < wInfo.synchronizations.Size(); s++)
            {
                int sync = wInfo.synchronizations[s];
                wInfo.synchronizations[s] = table[sync].newIndex;
            }
        }
    }
}

struct TranslationTableItem
{
    RString from;
    RString to;

    TranslationTableItem(RString f, RString t)
    {
        from = f;
        to = t;
    }
};

class TranslationTable : public AutoArray<TranslationTableItem>
{
    typedef AutoArray<TranslationTableItem> base;

  public:
    int Add(RString from, RString to) { return base::Add(TranslationTableItem(from, to)); }
    RString Translate(RString from);
};

RString TranslationTable::Translate(RString from)
{
    if (from.GetLength() == 0)
    {
        return from;
    }
    for (int i = 0; i < Size(); i++)
    {
        const TranslationTableItem& item = Get(i);
        if (stricmp(from, item.from) == 0)
        {
            return item.to;
        }
    }
    return from;
}

void ArcadeTemplate::Merge(ArcadeTemplate& t, Vector3Par offset)
{
    // unique names - ArcadeMarkerInfo::name
    TranslationTable markerNames;
    for (int i = 0; i < t.markers.Size(); i++)
    {
        ArcadeMarkerInfo& mInfo = t.markers[i];
        if (mInfo.name.GetLength() == 0)
        {
            continue;
        }
        if (FindMarker(mInfo.name))
        {
            char name[256];
            for (int l = 1;; l++)
            {
                snprintf(name, sizeof(name), "%s_%d", (const char*)mInfo.name, l);
                if (!FindMarker(name))
                {
                    break;
                }
            }
            markerNames.Add(mInfo.name, name);
        }
    }

    // unique names - ArcadeUnitInfo::text
    TranslationTable unitNames;
    for (int i = 0; i < t.emptyVehicles.Size(); i++)
    {
        ArcadeUnitInfo& uInfo = t.emptyVehicles[i];
        if (uInfo.name.GetLength() == 0)
        {
            continue;
        }
        if (FindVehicle(uInfo.name))
        {
            char name[256];
            for (int l = 1;; l++)
            {
                snprintf(name, sizeof(name), "%s_%d", (const char*)uInfo.name, l);
                if (!FindVehicle(name))
                {
                    break;
                }
            }
            unitNames.Add(uInfo.name, name);
        }
    }
    for (int i = 0; i < t.groups.Size(); i++)
    {
        ArcadeGroupInfo& gInfo = t.groups[i];
        for (int i = 0; i < gInfo.units.Size(); i++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[i];
            if (uInfo.name.GetLength() == 0)
            {
                continue;
            }
            if (FindVehicle(uInfo.name))
            {
                char name[256];
                for (int l = 1;; l++)
                {
                    snprintf(name, sizeof(name), "%s_%d", (const char*)uInfo.name, l);
                    if (!FindVehicle(name))
                    {
                        break;
                    }
                }
                unitNames.Add(uInfo.name, name);
            }
        }
    }

    // unique names - ArcadeSensorInfo::name
    TranslationTable sensorNames;
    for (int i = 0; i < t.sensors.Size(); i++)
    {
        ArcadeSensorInfo& sInfo = t.sensors[i];
        if (sInfo.name.GetLength() == 0)
        {
            continue;
        }
        if (FindSensor(sInfo.name))
        {
            char name[256];
            for (int l = 1;; l++)
            {
                snprintf(name, sizeof(name), "%s_%d", (const char*)sInfo.name, l);
                if (!FindSensor(name))
                {
                    break;
                }
            }
            sensorNames.Add(sInfo.name, name);
        }
    }
    for (int i = 0; i < t.groups.Size(); i++)
    {
        ArcadeGroupInfo& gInfo = t.groups[i];
        for (int i = 0; i < gInfo.sensors.Size(); i++)
        {
            ArcadeSensorInfo& sInfo = gInfo.sensors[i];
            if (sInfo.name.GetLength() == 0)
            {
                continue;
            }
            if (FindSensor(sInfo.name))
            {
                char name[256];
                for (int l = 1;; l++)
                {
                    snprintf(name, sizeof(name), "%s_%d", (const char*)sInfo.name, l);
                    if (!FindSensor(name))
                    {
                        break;
                    }
                }
                sensorNames.Add(sInfo.name, name);
            }
        }
    }

    // merge templates
    for (int i = 0; i < t.groups.Size(); i++)
    {
        int index = groups.Add(t.groups[i]);
        ArcadeGroupInfo& gInfo = groups[index];
        // offset positions
        gInfo.AddOffset(offset);
        // offset synchronizations and vehicle IDs
        for (int j = 0; j < gInfo.units.Size(); j++)
        {
            ArcadeUnitInfo& uInfo = gInfo.units[j];
            AI_ERROR(uInfo.id >= 0);
            AI_ERROR(uInfo.id < t.nextVehId);
            uInfo.id += nextVehId;
            uInfo.player = APNonplayable; // avoid 2 players
            uInfo.name = unitNames.Translate(uInfo.name);
            for (int m = 0; m < uInfo.markers.Size(); m++)
            {
                uInfo.markers[m] = markerNames.Translate(uInfo.markers[m]);
            }
        }
        for (int j = 0; j < gInfo.sensors.Size(); j++)
        {
            ArcadeSensorInfo& sInfo = gInfo.sensors[j];
            if (sInfo.idVehicle >= 0)
            {
                AI_ERROR(sInfo.idVehicle < t.nextVehId);
                sInfo.idVehicle += nextVehId;
            }
            for (int s = 0; s < sInfo.synchronizations.Size(); s++)
            {
                AI_ERROR(sInfo.synchronizations[s] >= 0);
                AI_ERROR(sInfo.synchronizations[s] < t.nextSyncId);
                sInfo.synchronizations[s] += nextSyncId;
            }
            sInfo.name = sensorNames.Translate(sInfo.name);
        }
        for (int j = 0; j < gInfo.waypoints.Size(); j++)
        {
            ArcadeWaypointInfo& wInfo = gInfo.waypoints[j];
            if (wInfo.id >= 0)
            {
                AI_ERROR(wInfo.id < t.nextVehId);
                wInfo.id += nextVehId;
            }
            for (int s = 0; s < wInfo.synchronizations.Size(); s++)
            {
                AI_ERROR(wInfo.synchronizations[s] >= 0);
                AI_ERROR(wInfo.synchronizations[s] < t.nextSyncId);
                wInfo.synchronizations[s] += nextSyncId;
            }
        }
    }
    for (int i = 0; i < t.emptyVehicles.Size(); i++)
    {
        int index = emptyVehicles.Add(t.emptyVehicles[i]);
        ArcadeUnitInfo& uInfo = emptyVehicles[index];
        // offset positions
        uInfo.AddOffset(offset);
        // offset vehicle IDs
        AI_ERROR(uInfo.id >= 0);
        AI_ERROR(uInfo.id < t.nextVehId);
        uInfo.id += nextVehId;
        uInfo.name = unitNames.Translate(uInfo.name);
        for (int m = 0; m < uInfo.markers.Size(); m++)
        {
            uInfo.markers[m] = markerNames.Translate(uInfo.markers[m]);
        }
    }
    for (int i = 0; i < t.sensors.Size(); i++)
    {
        int index = sensors.Add(t.sensors[i]);
        // offset positions
        ArcadeSensorInfo& sInfo = sensors[index];
        sInfo.AddOffset(offset);
        // offset synchronizations and vehicle IDs
        if (sInfo.idVehicle >= 0)
        {
            AI_ERROR(sInfo.idVehicle < t.nextVehId);
            sInfo.idVehicle += nextVehId;
        }
        for (int s = 0; s < sInfo.synchronizations.Size(); s++)
        {
            AI_ERROR(sInfo.synchronizations[s] >= 0);
            AI_ERROR(sInfo.synchronizations[s] < t.nextSyncId);
            sInfo.synchronizations[s] += nextSyncId;
        }
        sInfo.name = sensorNames.Translate(sInfo.name);
    }
    for (int i = 0; i < t.markers.Size(); i++)
    {
        int index = markers.Add(t.markers[i]);
        // offset positions
        ArcadeMarkerInfo& mInfo = markers[index];
        mInfo.AddOffset(offset);
        mInfo.name = markerNames.Translate(mInfo.name);
    }

    nextVehId += t.nextVehId;
    nextSyncId += t.nextSyncId;
}

void ArcadeTemplate::AddOffset(Vector3Par offset)
{
    for (int i = 0; i < emptyVehicles.Size(); i++)
    {
        emptyVehicles[i].AddOffset(offset);
    }
    for (int i = 0; i < sensors.Size(); i++)
    {
        sensors[i].AddOffset(offset);
    }
    for (int i = 0; i < markers.Size(); i++)
    {
        markers[i].AddOffset(offset);
    }
    for (int i = 0; i < groups.Size(); i++)
    {
        groups[i].AddOffset(offset);
    }
}

void ArcadeTemplate::Rotate(Vector3Par center, float angle, bool sel)
{
    for (int i = 0; i < emptyVehicles.Size(); i++)
    {
        emptyVehicles[i].Rotate(center, angle, sel);
    }
    for (int i = 0; i < sensors.Size(); i++)
    {
        sensors[i].Rotate(center, angle, sel);
    }
    for (int i = 0; i < markers.Size(); i++)
    {
        markers[i].Rotate(center, angle, sel);
    }
    for (int i = 0; i < groups.Size(); i++)
    {
        groups[i].Rotate(center, angle, sel);
    }
}

void ArcadeTemplate::CalculateCenter(Vector3& sum, int& count, bool sel)
{
    for (int i = 0; i < emptyVehicles.Size(); i++)
    {
        emptyVehicles[i].CalculateCenter(sum, count, sel);
    }
    for (int i = 0; i < sensors.Size(); i++)
    {
        sensors[i].CalculateCenter(sum, count, sel);
    }
    for (int i = 0; i < markers.Size(); i++)
    {
        markers[i].CalculateCenter(sum, count, sel);
    }
    for (int i = 0; i < groups.Size(); i++)
    {
        groups[i].CalculateCenter(sum, count, sel);
    }
}

void ArcadeTemplate::ClearSelection()
{
    for (int i = 0; i < emptyVehicles.Size(); i++)
    {
        emptyVehicles[i].selected = false;
    }
    for (int i = 0; i < sensors.Size(); i++)
    {
        sensors[i].selected = false;
    }
    for (int i = 0; i < markers.Size(); i++)
    {
        markers[i].selected = false;
    }
    for (int i = 0; i < groups.Size(); i++)
    {
        ArcadeGroupInfo& gInfo = groups[i];
        for (int j = 0; j < gInfo.units.Size(); j++)
        {
            gInfo.units[j].selected = false;
        }
        for (int j = 0; j < gInfo.sensors.Size(); j++)
        {
            gInfo.sensors[j].selected = false;
        }
        for (int j = 0; j < gInfo.waypoints.Size(); j++)
        {
            gInfo.waypoints[j].selected = false;
        }
    }
}

} // namespace Poseidon
