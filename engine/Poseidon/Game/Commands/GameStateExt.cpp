
#include <algorithm>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Config/UserConfig.hpp>
#include <Poseidon/Core/Version.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Detection/Detector.hpp>
#include <Poseidon/World/Scene/ObjectClasses.hpp>
#include <Poseidon/World/Scene/Fireplace.hpp>

#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>

#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/AIRadio.hpp>
#include <Poseidon/UI/Map/UIMap.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Entities/Vehicles/Ground/Car.hpp>
#include <Poseidon/World/Entities/Vehicles/Ground/Tank.hpp>
#include <Poseidon/World/Entities/Vehicles/Air/Airplane.hpp>
#include <Poseidon/World/Entities/Vehicles/House.hpp>
#include <Poseidon/Audio/DynSound.hpp>
#include <Poseidon/World/Scene/Camera/CameraHold.hpp>
#include <Poseidon/Network/Network.hpp>
#include <Poseidon/Core/resincl.hpp>

#include <Poseidon/Game/Chat.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>

#include <Poseidon/Game/UiActions.hpp>

#include <ctype.h>
#include <time.h>
#include <string>
#include <vector>
#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

using Poseidon::Foundation::FindEnumName;

using namespace Poseidon;
namespace Poseidon
{
GameValue ParticleDrop(const GameState* state, GameValuePar oper1);
} // namespace Poseidon

#ifdef _WIN32
#include <io.h>
#endif

#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/PreprocC/Preproc.h>

#include <Poseidon/Foundation/Platform/VersionNo.h>

#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/World/Scene/ScriptLight.hpp>

DEFINE_FAST_ALLOCATOR(GameDataObject)

RString GameDataObject::GetText() const
{
    if (_value == nullptr)
    {
        return "<nullptr-object>";
    }
    // Arma 3 writes a named object as its variable name (str, format "%1"), and missions
    // compile code from that: format ["%1 getVariable 'BIS_cleared'", _logic].
    if (Poseidon::Arma1Compat::Active() && Poseidon::Arma1Compat::Generation() >= 3)
    {
        const RString name = _value->GetVarName();
        if (name.GetLength() > 0)
            return name;
    }
    return _value->GetDebugName();
}

bool GameDataObject::IsEqualTo(const GameData* data) const
{
    const Object* val1 = GetObject();
    const Object* val2 = static_cast<const GameDataObject*>(data)->GetObject();
    return val1 == val2;
}

LSError GameDataObject::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar));
    PARAM_CHECK(ar.SerializeRef("value", _value, 1))
    return LSOK;
}

DEFINE_FAST_ALLOCATOR(GameDataGroup)

RString GameDataGroup::GetText() const
{
    if (_value == nullptr)
    {
        return "<nullptr-group>";
    }
    return _value->GetDebugName();
}

bool GameDataGroup::IsEqualTo(const GameData* data) const
{
    const AIGroup* val1 = GetGroup();
    const AIGroup* val2 = static_cast<const GameDataGroup*>(data)->GetGroup();
    return val1 == val2;
}

LSError GameDataGroup::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar));
    PARAM_CHECK(ar.SerializeRef("value", _value, 1))
    return LSOK;
}

DEFINE_FAST_ALLOCATOR(GameDataSide)

RString GameDataSide::GetText() const
{
    return FindEnumName(_value);
}

bool GameDataSide::IsEqualTo(const GameData* data) const
{
    const TargetSide val1 = GetSide();
    const TargetSide val2 = static_cast<const GameDataSide*>(data)->GetSide();
    return val1 == val2;
}

LSError GameDataSide::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar));
    PARAM_CHECK(ar.SerializeEnum("value", _value, 1));
    return LSOK;
}

ConfigFileEntries GFileEntries;

int ConfigFileEntries::Add(const ConfigFileEntry& item)
{
    for (int i = 0; i < Size(); i++)
    {
        if (Get(i).cls == nullptr)
        {
            PoseidonAssert(Get(i).refCount == 0);
            Set(i) = item;
            return i;
        }
    }
    return base::Add(item);
}

static void FileEntryAddRef(ParamClass* cls)
{
    PoseidonAssert(cls);
    const ParamClass* file = cls->GetFile();
    for (int i = 0; i < GFileEntries.Size(); i++)
    {
        if (GFileEntries[i].cls == file)
        {
            GFileEntries[i].refCount++;
            break;
        }
    }
}

static void FileEntryRelease(ParamClass* cls)
{
    PoseidonAssert(cls);
    const ParamClass* file = cls->GetFile();
    for (int i = 0; i < GFileEntries.Size(); i++)
    {
        if (GFileEntries[i].cls == file)
        {
            if (--GFileEntries[i].refCount <= 0)
            {
                for (int j = 0; j < GFileEntries.Size(); j++)
                {
                    if (GFileEntries[j].cls && GFileEntries[j].cls->GetFile() == file)
                    {
                        GFileEntries[j].cls = nullptr;
                        PoseidonAssert(GFileEntries[j].refCount == 0);
                    }
                }
                delete static_cast<const ParamFile*>(file);
            }
            break;
        }
    }
}

GameFileType::GameFileType(const GameFileType& src)
{
    readOnly = src.readOnly;
    _index = -1;
    SetIndex(src.GetIndex());
}

void GameFileType::operator=(const GameFileType& src)
{
    readOnly = src.readOnly;
    SetIndex(src.GetIndex());
}

void GameFileType::SetIndex(int index)
{
    if (_index >= 0)
    {
        ParamClass* cls = GFileEntries[_index].cls;
        if (cls)
        {
            FileEntryRelease(cls);
        }
    }

    _index = index;

    if (index >= 0)
    {
        ParamClass* cls = GFileEntries[index].cls;
        if (cls)
        {
            FileEntryAddRef(cls);
        }
    }
}

GameFileType::~GameFileType()
{
    if (_index >= 0)
    {
        ParamClass* cls = GFileEntries[_index].cls;
        if (cls)
        {
            FileEntryRelease(cls);
        }
    }
}

DEFINE_FAST_ALLOCATOR(GameDataFile)

RString GameDataFile::GetText() const
{
    if (_value.GetIndex() < 0)
    {
        return "No file";
    }
    ParamClass* entry = GFileEntries[_value.GetIndex()].cls;
    if (!entry)
    {
        return "Destructed file";
    }
    return entry->GetName();
}

bool GameDataFile::IsEqualTo(const GameData* data) const
{
    const GameFileType val1 = GetFile();
    const GameFileType val2 = static_cast<const GameDataFile*>(data)->GetFile();
    return val1.GetIndex() == val2.GetIndex();
}

LSError GameDataFile::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar));
    // do not serialize
    if (ar.IsLoading())
    {
        _value.SetIndex(-1);
        _value.readOnly = false;
    }

    return LSOK;
}

const GameType GameObjectOrGroup(GameObject | GameGroup);
const GameType GameObjectOrArray(GameObject | GameArray);

GameData* CreateGameDataObject()
{
    return new GameDataObject();
}
GameData* CreateGameDataGroup()
{
    return new GameDataGroup();
}
GameData* CreateGameDataSide()
{
    return new GameDataSide();
}
GameData* CreateGameDataFile()
{
    return new GameDataFile();
}

namespace Poseidon
{
GameValue CreateGameObject(Object* obj)
{
    return GameValueExt(obj);
}
} // namespace Poseidon

#define OBJECT_NULL GameValueExt((Object*)nullptr)
#define GROUP_NULL GameValueExt((AIGroup*)nullptr)

Object* GetObject(GameValuePar oper)
{
    if (oper.GetNil())
    {
        return nullptr;
    }
    if (oper.GetType() == GameObject)
    {
        return static_cast<GameDataObject*>(oper.GetData())->GetObject();
    }
    if (oper.GetType() == GameGroup)
    {
        AIGroup* grp = static_cast<GameDataGroup*>(oper.GetData())->GetGroup();
        if (!grp)
        {
            return nullptr;
        }
        AIUnit* leader = grp->Leader();
        if (!leader)
        {
            return nullptr;
        }
        return leader->GetPerson();
    }
    return nullptr;
}

AIGroup* GetGroup(GameValuePar oper)
{
    if (oper.GetNil())
    {
        return nullptr;
    }
    if (oper.GetType() == GameGroup)
    {
        return static_cast<GameDataGroup*>(oper.GetData())->GetGroup();
    }
    if (oper.GetType() == GameObject)
    {
        Object* obj = static_cast<GameDataObject*>(oper.GetData())->GetObject();
        EntityAI* vai = dyn_cast<EntityAI>(obj);
        if (!vai)
        {
            return nullptr;
        }
        AIUnit* unit = vai->CommanderUnit();
        if (!unit)
        {
            return nullptr;
        }
        return unit->GetGroup();
    }
    return nullptr;
}

Target* FindTargetReveal(AIGroup* grp, EntityAI* veh)
{
    Target* tgt = grp->FindTarget(veh);
    if (tgt)
    {
        return tgt;
    }
    return grp->AddTarget(veh, 0.3, 0.3, 3.0f);
}

#define TABLE_COMMAND(xxx, Xxx, RightType)                                                              \
    GameOperator(GameNothing, "command" #xxx, function, VehCommand##Xxx, GameObjectOrArray, RightType), \
        GameOperator(GameNothing, "do" #xxx, function, VehDo##Xxx, GameObjectOrArray, RightType),

#define TABLE_COMMAND_S(xxx, Xxx)                                                  \
    GameFunction(GameNothing, "command" #xxx, VehCommand##Xxx, GameObjectOrArray), \
        GameFunction(GameNothing, "do" #xxx, VehDo##Xxx, GameObjectOrArray),

bool OpenScript(QIFStreamB& in, RString name);

GameValue ScriptExecute(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ScriptExit(const GameState* state);
// Scheduled SQF / Arma 1 campaign commands (Scripts.cpp)
GameValue ScriptSpawn(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ScriptExecVM(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ScriptExecVMUnary(const GameState* state, GameValuePar oper1);
GameValue ScriptTerminate(const GameState* state, GameValuePar oper1);
GameValue ScriptIsDone(const GameState* state, GameValuePar oper1);
GameValue ScriptWaitUntilUnscheduled(const GameState* state, GameValuePar oper1);
GameValue ScriptSleepUnscheduled(const GameState* state, GameValuePar oper1);
GameValue PreloadCameraReady(const GameState* state, GameValuePar oper1);
GameValue PreloadObjectReady(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CampaignKeyActive(const GameState* state, GameValuePar oper1);
GameValue CampaignKeyActivate(const GameState* state, GameValuePar oper1);
GameValue ScriptGoto(const GameState* state, GameValuePar oper1);

GameValue PoolAddWeapon(const GameState* state, GameValuePar oper1);
GameValue PoolAddMagazine(const GameState* state, GameValuePar oper1);
GameValue PoolGetWeapons(const GameState* state, GameValuePar oper1);
GameValue PoolSetWeapons(const GameState* state, GameValuePar oper1);
GameValue PoolClearWeapons(const GameState* state);
GameValue PoolClearMagazines(const GameState* state);
GameValue PoolQueryWeapons(const GameState* state, GameValuePar oper1);
GameValue PoolQueryMagazines(const GameState* state, GameValuePar oper1);

GameValue ShowCinemaBorder(const GameState* state, GameValuePar oper1);

// Forward declarations for split compilation units
// Nular functions
GameValue Benchmark(const GameState* state);
GameValue CadetMode(const GameState* state);
GameValue CameraOn(const GameState* state);
GameValue ConfigListNames(const GameState* state);
GameValue ConfigNew(const GameState* state);
GameValue DayTime(const GameState* state);
GameValue EnableEndDialog(const GameState* state);
GameValue ForceEnd(const GameState* state);
GameValue GameTime(const GameState* state);
GameValue GetAcceleratedTime(const GameState* state);
GameValue GetMusicVolume(const GameState* state);
GameValue GetSoundVolume(const GameState* state);
GameValue GrpNull(const GameState* state);
GameValue IsCheatsEnabled(const GameState* state);
GameValue IsCompassShown(const GameState* state);
GameValue IsDialog(const GameState* state);
GameValue IsGPSShown(const GameState* state);
GameValue IsMapShown(const GameState* state);
GameValue IsNotepadShown(const GameState* state);
GameValue IsServer(const GameState* state);
GameValue IsJIP(const GameState* state);
GameValue ServerPause(const GameState* state);
GameValue ServerResume(const GameState* state);
GameValue IsWalkieTalkieShown(const GameState* state);
GameValue IsWarrantShown(const GameState* state);
GameValue IsWatchShown(const GameState* state);
GameValue MapAnimClear(const GameState* state);
GameValue MapAnimCommit(const GameState* state);
GameValue MapAnimDone(const GameState* state);
GameValue MapHeight(const GameState* state);
GameValue MapLeftTopPos(const GameState* state);
GameValue MapWidth(const GameState* state);
GameValue MissionName(const GameState* state);
GameValue MissionStart(const GameState* state);
GameValue ObjNull(const GameState* state);
GameValue Player(const GameState* state);
GameValue PlayerClearInjured(const GameState* state);
GameValue PoolClearMagazines(const GameState* state);
GameValue PoolClearWeapons(const GameState* state);
GameValue SaveGame(const GameState* state);
GameValue ScriptExit(const GameState* state);
GameValue SideCivilian(const GameState* state);
GameValue SideEast(const GameState* state);
GameValue SideEnemy(const GameState* state);
GameValue SideFriendly(const GameState* state);
GameValue SideLogic(const GameState* state);
GameValue SideResistance(const GameState* state);
GameValue SideWest(const GameState* state);
GameValue UsedVersion(const GameState* state);
GameValue VoiceLanguage(const GameState* state);
GameValue WorldName(const GameState* state);
GameValue WorldSize(const GameState* state);

// Unary functions
GameValue BriefingOnGear(const GameState* state, GameValuePar oper1);
GameValue BriefingOnGroup(const GameState* state, GameValuePar oper1);
GameValue BriefingOnNotes(const GameState* state, GameValuePar oper1);
GameValue BriefingOnPlan(const GameState* state, GameValuePar oper1);
GameValue ButtonGetAction(const GameState* state, GameValuePar oper1);
GameValue ButtonSetAction(const GameState* state, GameValuePar oper1);
GameValue CamCommited(const GameState* state, GameValuePar oper1);
GameValue CamDestroy(const GameState* state, GameValuePar oper1);
GameValue CenterCreate(const GameState* state, GameValuePar oper1);
GameValue CenterDelete(const GameState* state, GameValuePar oper1);
GameValue ConfigLoad(const GameState* state, GameValuePar oper1);
GameValue DatabaseNew(const GameState* state);
GameValue DatabaseLoad(const GameState* state, GameValuePar oper1);
GameValue DatabaseGet(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseSet(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseDelete(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseExists(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseKeys(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseSave(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseListPush(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseListPushMany(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseListPop(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseListGet(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseListSet(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseListRemove(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseListSize(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseHashSet(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseHashSetMany(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseHashGet(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseHashDelete(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseHashExists(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue DatabaseHashKeys(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CtrlEnable(const GameState* state, GameValuePar oper1);
GameValue CtrlEnabled(const GameState* state, GameValuePar oper1);
GameValue CtrlGetText(const GameState* state, GameValuePar oper1);
GameValue CtrlSetText(const GameState* state, GameValuePar oper1);
GameValue CtrlShow(const GameState* state, GameValuePar oper1);
GameValue CtrlVisible(const GameState* state, GameValuePar oper1);
GameValue CutObj(const GameState* state, GameValuePar oper1);
GameValue CutRsc(const GameState* state, GameValuePar oper1);
GameValue CutText(const GameState* state, GameValuePar oper1);
GameValue DBG_Screenshot(const GameState* state, GameValuePar oper1);
GameValue DBG_SwitchLandscape(const GameState* state, GameValuePar oper1);
GameValue DebugShow(const GameState* state, GameValuePar oper1);
GameValue DeleteIdentity(const GameState* state, GameValuePar oper1);
GameValue DeleteStatus(const GameState* state, GameValuePar oper1);
GameValue DialogClose(const GameState* state, GameValuePar oper1);
GameValue DialogCreate(const GameState* state, GameValuePar oper1);
GameValue A1FindDisplay(const GameState* state, GameValuePar oper1);
GameValue A1DisplayCtrl(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1DisplaySetEventHandler(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1CtrlShow(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1CtrlEnable(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1CtrlSetText(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1ButtonSetAction(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1CtrlSetFade(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1CtrlCommit(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1CtrlIgnored(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1CtrlCommitted(const GameState* state, GameValuePar oper1);
GameValue A1CtrlSetPosition(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1CtrlSetTextColor(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1CtrlSetBackgroundColor(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1CtrlPosition(const GameState* state, GameValuePar oper1);
GameValue A1CtrlSetStructuredText(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1CtrlParent(const GameState* state, GameValuePar oper1);
GameValue DisableUserInput(const GameState* state, GameValuePar oper1);
GameValue EnableRadio(const GameState* state, GameValuePar oper1);
GameValue EstimatedTimeLeft(const GameState* state, GameValuePar oper1);
GameValue GameTitleObj(const GameState* state, GameValuePar oper1);
GameValue GameTitleRsc(const GameState* state, GameValuePar oper1);
GameValue GameTitleText(const GameState* state, GameValuePar oper1);
GameValue GetLaserTarget(const GameState* state, GameValuePar oper1);
GameValue GetNearestObject(const GameState* state, GameValuePar oper1);
GameValue GetNearestObjectByDistance(const GameState* state, GameValuePar oper1);
GameValue A1NearObjects(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1GetWeaponCargo(const GameState* state, GameValuePar oper1);
GameValue A1Reload(const GameState* state, GameValuePar oper1);
GameValue A1GetMagazineCargo(const GameState* state, GameValuePar oper1);
GameValue A1ConfigFile(const GameState* state);
GameValue A1MissionConfigFile(const GameState* state);
GameValue A1ConfigChild(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1GetText(const GameState* state, GameValuePar oper1);
GameValue A1GetNumber(const GameState* state, GameValuePar oper1);
GameValue A1GetArray(const GameState* state, GameValuePar oper1);
GameValue A1IsClass(const GameState* state, GameValuePar oper1);
GameValue A1IsArray(const GameState* state, GameValuePar oper1);
GameValue A1IsText(const GameState* state, GameValuePar oper1);
GameValue A1IsNumber(const GameState* state, GameValuePar oper1);
GameValue A1ConfigName(const GameState* state, GameValuePar oper1);
GameValue A1ConfigCount(const GameState* state, GameValuePar oper1);
GameValue A1ConfigSelect(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1InheritsFrom(const GameState* state, GameValuePar oper1);
GameValue A1IsKindOfConfig(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1Min(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1Max(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1TypeName(const GameState* state, GameValuePar oper1);
GameValue A1SurfaceIsWater(const GameState* state, GameValuePar oper1);
GameValue GetNetIdObj(const GameState* state, GameValuePar oper1);
GameValue GetNetworkId(const GameState* state, GameValuePar oper1);
GameValue GetObjFromNetId(const GameState* state, GameValuePar oper1);
GameValue GetObject(const GameState* state, GameValuePar oper1);
GameValue GetUnitById(const GameState* state, GameValuePar oper1);
GameValue GroupCreate(const GameState* state, GameValuePar oper1);
GameValue GroupDelete(const GameState* state, GameValuePar oper1);
GameValue GrpCombatMode(const GameState* state, GameValuePar oper1);
GameValue GrpFormation(const GameState* state, GameValuePar oper1);
GameValue GrpIsNull(const GameState* state, GameValuePar oper1);
GameValue GrpLeader(const GameState* state, GameValuePar oper1);
GameValue GrpSpeedMode(const GameState* state, GameValuePar oper1);
GameValue GrpUnits(const GameState* state, GameValuePar oper1);
GameValue GuardedPointCreate(const GameState* state, GameValuePar oper1);
GameValue LBAdd(const GameState* state, GameValuePar oper1);
GameValue LBClear(const GameState* state, GameValuePar oper1);
GameValue LBDelete(const GameState* state, GameValuePar oper1);
GameValue LBGetColor(const GameState* state, GameValuePar oper1);
GameValue LBGetCurSel(const GameState* state, GameValuePar oper1);
GameValue LBGetData(const GameState* state, GameValuePar oper1);
GameValue LBGetPicture(const GameState* state, GameValuePar oper1);
GameValue LBGetSize(const GameState* state, GameValuePar oper1);
GameValue LBGetText(const GameState* state, GameValuePar oper1);
GameValue LBGetValue(const GameState* state, GameValuePar oper1);
GameValue LBSetColor(const GameState* state, GameValuePar oper1);
GameValue LBSetCurSel(const GameState* state, GameValuePar oper1);
GameValue LBSetData(const GameState* state, GameValuePar oper1);
GameValue LBSetPicture(const GameState* state, GameValuePar oper1);
GameValue LBSetValue(const GameState* state, GameValuePar oper1);
GameValue MapAnimAdd(const GameState* state, GameValuePar oper1);
GameValue MapForce(const GameState* state, GameValuePar oper1);
GameValue MapOnSingleClick(const GameState* state, GameValuePar oper1);
GameValue MarkerCreate(const GameState* state, GameValuePar oper1);
GameValue MarkerDelete(const GameState* state, GameValuePar oper1);
GameValue MarkerGetColor(const GameState* state, GameValuePar oper1);
GameValue MarkerGetDir(const GameState* state, GameValuePar oper1);
GameValue MarkerGetPos(const GameState* state, GameValuePar oper1);
GameValue MarkerGetSize(const GameState* state, GameValuePar oper1);
GameValue MarkerGetType(const GameState* state, GameValuePar oper1);
GameValue ObjAlive(const GameState* state, GameValuePar oper1);
GameValue ObjBehaviour(const GameState* state, GameValuePar oper1);
GameValue ObjCanFire(const GameState* state, GameValuePar oper1);
GameValue ObjCanMove(const GameState* state, GameValuePar oper1);
GameValue ObjCanStand(const GameState* state, GameValuePar oper1);
GameValue ObjCaptive(const GameState* state, GameValuePar oper1);
GameValue ObjClearMagazineCargo(const GameState* state, GameValuePar oper1);
GameValue ObjClearWeaponCargo(const GameState* state, GameValuePar oper1);
GameValue ObjCommander(const GameState* state, GameValuePar oper1);
GameValue ObjDriver(const GameState* state, GameValuePar oper1);
GameValue ObjExperience(const GameState* state, GameValuePar oper1);
GameValue ObjFlee(const GameState* state, GameValuePar oper1);
GameValue ObjFuel(const GameState* state, GameValuePar oper1);
GameValue ObjGetAllMagazines(const GameState* state, GameValuePar oper1);
GameValue ObjGetAllWeapons(const GameState* state, GameValuePar oper1);
GameValue ObjUnitLoadout(const GameState* state, GameValuePar oper1);
GameValue ObjGetDammage(const GameState* state, GameValuePar oper1);
GameValue ObjGetDir(const GameState* state, GameValuePar oper1);
GameValue ObjGetFlag(const GameState* state, GameValuePar oper1);
GameValue ObjGetFlagOwner(const GameState* state, GameValuePar oper1);
GameValue ObjGetHitpointsNames(const GameState* state, GameValuePar oper1);
GameValue ObjGetMass(const GameState* state, GameValuePar oper1);
GameValue ObjGetMove(const GameState* state, GameValuePar oper1);
GameValue ObjGetNearestBuilding(const GameState* state, GameValuePar oper1);
GameValue ObjGetPos(const GameState* state, GameValuePar oper1);
GameValue ObjGetPosASL(const GameState* state, GameValuePar oper1);
GameValue ObjGetPrimaryWeapon(const GameState* state, GameValuePar oper1);
GameValue ObjGetScore(const GameState* state, GameValuePar oper1);
GameValue ObjGetScudState(const GameState* state, GameValuePar oper1);
GameValue ObjGetSecondaryWeapon(const GameState* state, GameValuePar oper1);
GameValue ObjGetSkill(const GameState* state, GameValuePar oper1);
GameValue ObjGetSpeed(const GameState* state, GameValuePar oper1);
GameValue ObjGetTurretDirAndElev(const GameState* state, GameValuePar oper1);
GameValue ObjGetType(const GameState* state, GameValuePar oper1);
GameValue ObjGetVectorDir(const GameState* state, GameValuePar oper1);
GameValue ObjGetVectorUp(const GameState* state, GameValuePar oper1);
GameValue ObjGetVelocity(const GameState* state, GameValuePar oper1);
GameValue ObjGroup(const GameState* state, GameValuePar oper1);
GameValue ObjGroupLeader(const GameState* state, GameValuePar oper1);
GameValue ObjGunner(const GameState* state, GameValuePar oper1);
GameValue ObjHandsHit(const GameState* state, GameValuePar oper1);
GameValue ObjInflamed(const GameState* state, GameValuePar oper1);
GameValue ObjIsLocal(const GameState* state, GameValuePar oper1);
GameValue ObjIsNull(const GameState* state, GameValuePar oper1);
GameValue ObjLeader(const GameState* state, GameValuePar oper1);
GameValue ObjLightSwitched(const GameState* state, GameValuePar oper1);
GameValue ObjList(const GameState* state, GameValuePar oper1);
GameValue ObjListIn(const GameState* state, GameValuePar oper1);
GameValue ObjLocked(const GameState* state, GameValuePar oper1);
GameValue ObjMagazinesArray(const GameState* state, GameValuePar oper1);
GameValue ObjName(const GameState* state, GameValuePar oper1);
GameValue ObjRemoveAllWeapons(const GameState* state, GameValuePar oper1);
GameValue ObjSide(const GameState* state, GameValuePar oper1);
GameValue ObjSomeAmmo(const GameState* state, GameValuePar oper1);
GameValue ObjStopped(const GameState* state, GameValuePar oper1);
GameValue ObjUnassignVehicle(const GameState* state, GameValuePar oper1);
GameValue ObjVehicle(const GameState* state, GameValuePar oper1);
GameValue ObjWeaponsFromPool(const GameState* state, GameValuePar oper1);
GameValue PlayMusic(const GameState* state, GameValuePar oper1);
GameValue PlaySound(const GameState* state, GameValuePar oper1);
GameValue SoundLength(const GameState* state, GameValuePar oper1);
GameValue PlayersNumber(const GameState* state, GameValuePar oper1);
GameValue PoolAddMagazine(const GameState* state, GameValuePar oper1);
GameValue PoolAddWeapon(const GameState* state, GameValuePar oper1);
GameValue PoolGetWeapons(const GameState* state, GameValuePar oper1);
GameValue PoolQueryMagazines(const GameState* state, GameValuePar oper1);
GameValue PoolQueryWeapons(const GameState* state, GameValuePar oper1);
GameValue PoolSetWeapons(const GameState* state, GameValuePar oper1);
GameValue LoadMission(const GameState* state, GameValuePar oper1);
GameValue OnPlayerConnected(const GameState* state, GameValuePar oper1);
GameValue OnPlayerDisconnected(const GameState* state, GameValuePar oper1);
GameValue PublicExec(const GameState* state, GameValuePar oper1);
GameValue RemoteExec(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue RemoteExecCall(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue RemoteExecRemove(const GameState* state, GameValuePar oper1);
GameValue SaveMission(const GameState* state, GameValuePar oper1);
GameValue PublicVariable(const GameState* state, GameValuePar oper1);
GameValue RequiredVersion(const GameState* state, GameValuePar oper1);
GameValue SaveVar(const GameState* state, GameValuePar oper1);
GameValue ScriptGoto(const GameState* state, GameValuePar oper1);
GameValue SetAcceleratedTime(const GameState* state, GameValuePar oper1);
GameValue SetDate(const GameState* state, GameValuePar oper1);
GameValue SetTerrainGrid(const GameState* state, GameValuePar oper1);
GameValue SetViewDistance(const GameState* state, GameValuePar oper1);
GameValue ShellCreate(const GameState* state, GameValuePar oper1);
GameValue ShowCinemaBorder(const GameState* state, GameValuePar oper1);
GameValue ShowCompass(const GameState* state, GameValuePar oper1);
GameValue ShowGPS(const GameState* state, GameValuePar oper1);
GameValue ShowHint(const GameState* state, GameValuePar oper1);
GameValue ShowHintC(const GameState* state, GameValuePar oper1);
GameValue ShowHintCadet(const GameState* state, GameValuePar oper1);
GameValue ShowMap(const GameState* state, GameValuePar oper1);
GameValue ShowNotepad(const GameState* state, GameValuePar oper1);
GameValue ShowWalkieTalkie(const GameState* state, GameValuePar oper1);
GameValue ShowWarrant(const GameState* state, GameValuePar oper1);
GameValue ShowWatch(const GameState* state, GameValuePar oper1);
GameValue SkipDayTime(const GameState* state, GameValuePar oper1);
GameValue SliderGetPosition(const GameState* state, GameValuePar oper1);
GameValue SliderGetRange(const GameState* state, GameValuePar oper1);
GameValue SliderGetSpeed(const GameState* state, GameValuePar oper1);
GameValue SliderSetPosition(const GameState* state, GameValuePar oper1);
GameValue SliderSetRange(const GameState* state, GameValuePar oper1);
GameValue SliderSetSpeed(const GameState* state, GameValuePar oper1);
GameValue StrFormat(const GameState* state, GameValuePar oper1);
GameValue StrLocalize(const GameState* state, GameValuePar oper1);
GameValue StrSize(const GameState* state, GameValuePar oper1);
GameValue StrSub(const GameState* state, GameValuePar oper1);
GameValue StringLoad(const GameState* state, GameValuePar oper1);
GameValue StringPreprocess(const GameState* state, GameValuePar oper1);
GameValue LogInfo(const GameState* state, GameValuePar oper1);
GameValue TextDebugLog(const GameState* state, GameValuePar oper1);
GameValue TextLog(const GameState* state, GameValuePar oper1);
GameValue TriggerCreate(const GameState* state, GameValuePar oper1);
GameValue AddMPReportEvent(const GameState* state, GameValuePar oper1);
GameValue AddMPReportFooter(const GameState* state, GameValuePar oper1);
GameValue AddMPReportHeader(const GameState* state, GameValuePar oper1);
GameValue GetMPReportInjuries(const GameState* state, GameValuePar oper1);
GameValue GetMPReportKilled(const GameState* state, GameValuePar oper1);
GameValue GetMPReportKills(const GameState* state, GameValuePar oper1);
GameValue VehCommandStop(const GameState* state, GameValuePar oper1);
GameValue VehDelete(const GameState* state, GameValuePar oper1);
GameValue VehDoStop(const GameState* state, GameValuePar oper1);
GameValue VehDone(const GameState* state, GameValuePar oper1);
GameValue VehIsEngineOn(const GameState* state, GameValuePar oper1);
GameValue WaypointDelete(const GameState* state, GameValuePar oper1);
GameValue WpGetPos(const GameState* state, GameValuePar oper1);

// Binary functions
GameValue BoolCmpEq(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue BoolCmpNE(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CamCommand(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CamCommit(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CamCreate(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CamSetDive(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CamSetFOV(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CamSetFOVRange(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CamSetPos(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CamSetRelPos(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CamSetTargetObj(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CamSetTargetVec(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CenterSetFriend(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue SetAirportClosed(const GameState* state, GameValuePar oper1);
GameValue SetAirportSide(const GameState* state, GameValuePar oper1);
GameValue ClassAdd(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ClassOpen(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ConfigSave(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue EffectSetCamera(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue EffectSetCondition(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue EffectSetMusic(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue EffectSetSound(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue EffectSetTitle(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpAllowFleeing(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpCmpE(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpCmpNE(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpKnowsAbout(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpLeaveVehicle(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpLockWP(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpMove(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpReveal(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpSetBehaviour(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpSetCombatMode(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpSetFormDir(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpSetFormation(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpSetIdentity(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpSetSpeedMode(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ListAllowGetIn(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ListCountEnemy(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ListCountFriendly(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ListCountSide(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ListCountType(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ListCountUnknown(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ListJoin(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ListOrderGetIn(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetBrush(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetColor(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetDir(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetPos(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetShape(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetSize(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetText(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetType(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAddExperience(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAddMagazine(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAddMagazineCargo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAddMagazinePrecise(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAddScore(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAddWeapon(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAddWeaponCargo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAllowDammage(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAmmo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAmmoArray(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAnimate(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAnimationPhase(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAssignAsCargo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAssignAsCommander(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAssignAsDriver(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjAssignAsGunner(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjCameraEffect(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjChangeModel(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjCmpE(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjCmpNE(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjDisableAI(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjEnableAI(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjDistance(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjFire(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjFireEx(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjGetBuildingPos(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjGetSelectionDammage(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjGlobalChat(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjGlobalRadio(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjGroupChat(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjGroupRadio(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjHasWeapon(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjIn(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjInflame(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjLand(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjLoadIdentity(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjLoadStatus(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjLock(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjMoveInCargo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjMoveInCommander(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjMoveInDriver(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjMoveInGunner(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjMuzzleReloadTime(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjPlayMove(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjRemoveMagazine(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjRemoveMagazines(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjRemoveWeapon(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSaveIdentity(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSaveStatus(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSay(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSelectWeapon(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetUnitLoadout(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetAmmoCargo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetVehicleAmmo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetCaptive(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetDammage(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetDir(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetFace(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetFaceAnimation(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetFlagOwner(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetFlagSide(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetFlagTexture(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetFlyingHeight(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetFuel(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetFuelCargo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetIdentity(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetSpeaker(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetPitch(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetName(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetMimic(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetPos(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetPosASL(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetRepairCargo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetSelectionDammage(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetSkill(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetTexture(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetMaterial(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetUnitPos(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetVectorDir(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetVectorDirectionAndUp(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetVectorUp(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetVelocity(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSideChat(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSideRadio(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjStop(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSwitchCamera(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSwitchLight(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSwitchMove(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjVehicleChat(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjVehicleRadio(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ParamAmmo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ParamArrayAmmo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ParamArrayVehicles(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ParamArrayWeapons(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ParamSubAmmo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ParamSubArrayAmmo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ParamSubArrayVehicles(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ParamSubArrayWeapons(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ParamSubVehicles(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ParamSubWeapons(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ParamVehicles(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ParamWeapons(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ScriptExecute(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue SetFog(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue SetMusicVolume(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue SetObjectiveStatus(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue SetOvercast(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue SetRadioMessage(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue SetRain(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue SetSoundVolume(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue SideCmpE(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue SideCmpNE(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue TriggerAttachObject(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue TriggerAttachVehicle(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue TriggerSetActivation(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue TriggerSetArea(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue TriggerSetStatements(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue TriggerSetText(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue TriggerSetTimeout(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue TriggerSetType(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue TriggerSynchronize(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue UnitCreate(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1UnitCreate(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A2Waypoints(const GameState* state, GameValuePar oper1); // GameStateExtA2.cpp
GameValue ValueAdd(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ValueGet(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehAddEventHandler(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehAddUserAction(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehCommandFire(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehCommandFollow(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehCommandMove(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehCommandTarget(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehCommandWatch(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehCreate(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehDoFire(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehDoFollow(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehDoMove(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehDoTarget(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehDoWatch(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehEngineOn(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehProcessAction(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehRemoveAllEventHandlers(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehRemoveEventHandler(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue VehRemoveUserAction(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointAdd(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointAttachObject(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointAttachVehicle(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointSetBehaviour(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointSetCombatMode(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointSetDescription(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointSetFormation(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointSetHousePos(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointSetPosition(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointSetScript(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointSetSpeed(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointSetStatements(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointSetTimeout(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointSetType(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointShow(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WaypointSynchronize(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue WpSetPos(const GameState* state, GameValuePar oper1, GameValuePar oper2);

// ---- Arma 1 commands with an existing CWA equivalent (used by the Arma 1 campaigns) ----

// pos nearestObject id  /  pos nearestObject "type"
static GameValue A1NearestObject(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (oper2.GetType() == GameScalar)
        return GetObject(state, oper2); // map object ids are unique on the world
    GameArrayType args;
    args.Add(oper1);
    args.Add(oper2);
    return GetNearestObject(state, GameValue(args));
}

// unit lookAt target: the closest CWA order is doWatch
static GameValue A1LookAt(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return VehDoWatch(state, oper1, oper2);
}

static GameValue A1CamPrepareTarget(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return oper2.GetType() == GameObject ? CamSetTargetObj(state, oper1, oper2) : CamSetTargetVec(state, oper1, oper2);
}

// Arma 1's prepared camera state is CWA's pending one: camCommitPrepared commits it.
static GameValue A1CamPreload(const GameState*, GameValuePar, GameValuePar)
{
    return NOTHING; // CWA streams the scene without a preload request
}

static GameValue A1CamPreloaded(const GameState*, GameValuePar)
{
    return GameValue(true);
}

// str: a value as text; a string comes back in quotes
static GameValue A1Str(const GameState*, GameValuePar oper1)
{
    if (oper1.GetType() == GameString)
        return GameValue(RString("\"") + RString(oper1) + RString("\""));
    return GameValue(oper1.GetText());
}

// Arma 1: distance between positions and/or objects
static bool A1WorldPos(const GameState* state, GameValuePar v, Vector3& pos)
{
    if (v.GetType() == GameObject)
    {
        Object* obj = GetObject(v);
        if (!obj)
            return false;
        pos = obj->WorldPosition();
        return true;
    }
    return GetPos(state, pos, v);
}

static GameValue A1Distance(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Vector3 a, b;
    if (!A1WorldPos(state, oper1, a) || !A1WorldPos(state, oper2, b))
        return GameValue(1e10f);
    return GameValue(a.Distance(b));
}

// object isKindOf "class": CWA's class tree, and for an Arma 1 unit also Arma 1's own
static bool CfgDerived(const ParamEntry* vehicles, const char* type, const char* base)
{
    const ParamEntry* t = vehicles ? vehicles->FindEntry(type) : nullptr;
    const ParamEntry* b = vehicles ? vehicles->FindEntry(base) : nullptr;
    const ParamClass* tc = t ? t->GetClassInterface() : nullptr;
    const ParamClass* bc = b ? b->GetClassInterface() : nullptr;
    return tc && bc && (tc == bc || tc->IsDerivedFrom(*bc));
}

RString ObjGetTypeRaw(GameValuePar oper1); // GameStateExtGrp.cpp

// `type` an object's raw class (className false) or a config class name (className true)
bool A1TypeIsKindOf(const RString& type, const RString& base, bool className)
{
    if (type.GetLength() == 0 || base.GetLength() == 0)
        return false;
    if (CfgDerived(Pars.FindEntry("CfgVehicles"), type, base))
        return true;
    if (Poseidon::Arma1Compat::Active())
    {
        const bool generated = _strnicmp(type, "A1x_", 4) == 0;
        if (generated || className)
        {
            const ParamEntry* a1 =
                Poseidon::Arma1Compat::FindConfigClass("CfgVehicles", (const char*)type + (generated ? 4 : 0));
            const ParamEntry* a1Base = Poseidon::Arma1Compat::FindConfigClass("CfgVehicles", base);
            const ParamClass* tc = a1 ? a1->GetClassInterface() : nullptr;
            const ParamClass* bc = a1Base ? a1Base->GetClassInterface() : nullptr;
            if (tc && bc && (tc == bc || tc->IsDerivedFrom(*bc)))
                return true;
        }
        const RString cwaBase = Poseidon::Arma1Compat::ResolveVehicleClass(base);
        if (cwaBase != base && CfgDerived(Pars.FindEntry("CfgVehicles"), type, cwaBase))
            return true;
    }
    return false;
}

static GameValue A1IsKindOf(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    // Arma 2: "className" isKindOf "Base" asks about a config class, not an object.
    const bool className = oper1.GetType() == GameString;
    const RString type = className ? static_cast<RString>(oper1) : ObjGetTypeRaw(oper1);
    return GameValue(A1TypeIsKindOf(type, oper2, className));
}

// switch (v) do { case a: {..}; case b; case c: {..}; default {..}; }
// switch/case yield tagged arrays; "do" runs the body with the switch active, ":" runs the
// first matching case (a case without code falls through to the next one with code).
namespace
{
struct A1Switch
{
    GameValue value;
    bool matched = false; // a case matched, waiting for its code
    bool done = false;
    RString defaultCode;
    GameValue result;
    const void* owner = nullptr; // the scheduled script it runs in (null: unscheduled code)
};
// Every open switch, innermost last. Scheduled scripts interleave, and one dropped while
// suspended inside a switch body never pops its own (its stack, where the switch lives, is
// freed without unwinding: MPE1_HuntersHunted and TE15 crashed in A1Case, run 42). A
// script's case/default reach only its own innermost switch, and a dropped script's
// switches are forgotten (ForgetScriptSwitches).
thread_local std::vector<A1Switch*> gSwitches;
}
namespace Poseidon
{
const void* CurrentScriptFiber(); // Scripts.cpp
void ForgetScriptSwitches(const void* owner);
}
using Poseidon::CurrentScriptFiber;
void Poseidon::ForgetScriptSwitches(const void* owner)
{
    if (!owner)
        return;
    gSwitches.erase(std::remove_if(gSwitches.begin(), gSwitches.end(),
                                   [owner](const A1Switch* s) { return s->owner == owner; }),
                    gSwitches.end());
}
namespace
{
A1Switch* CurrentSwitch()
{
    const void* owner = CurrentScriptFiber();
    for (auto it = gSwitches.rbegin(); it != gSwitches.rend(); ++it)
        if ((*it)->owner == owner)
            return *it;
    return nullptr;
}

GameValue Tagged(const char* tag, GameValuePar v)
{
    GameArrayType a;
    a.Add(GameValue(RString(tag)));
    a.Add(v);
    return GameValue(a);
}

bool IsTagged(GameValuePar v, const char* tag)
{
    if (v.GetType() != GameArray)
        return false;
    const GameArrayType& a = v;
    return a.Size() == 2 && a[0].GetType() == GameString && strcmp(RString(a[0]), tag) == 0;
}

bool SwitchEqual(GameValuePar a, GameValuePar b)
{
    if (a.GetType() != b.GetType())
        return false;
    if (a.GetType() == GameScalar)
        return (float)a == (float)b;
    if (a.GetType() == GameString)
        return strcmp(RString(a), RString(b)) == 0;
    if (a.GetType() == GameBool)
        return (bool)a == (bool)b;
    return a.GetText() == b.GetText();
}
} // namespace

static GameValue A1Switch_(const GameState*, GameValuePar oper1)
{
    return Tagged("__a1switch", oper1);
}

static GameValue A1Case(const GameState*, GameValuePar oper1)
{
    if (A1Switch* sw = CurrentSwitch(); sw && !sw->done && SwitchEqual(sw->value, oper1))
        sw->matched = true; // "case x;" alone falls through to the next code
    return Tagged("__a1case", oper1);
}

static GameValue A1Default(const GameState*, GameValuePar oper1)
{
    if (A1Switch* sw = CurrentSwitch())
        sw->defaultCode = RString(oper1);
    return NOTHING;
}

static GameValue A1CaseCode(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    A1Switch* current = CurrentSwitch();
    if (!current || !IsTagged(oper1, "__a1case"))
        return NOTHING;
    A1Switch& sw = *current;
    if (sw.done || !sw.matched)
        return NOTHING;
    sw.done = true;
    sw.result = state->EvaluateMultiple(RString(oper2));
    return NOTHING;
}

// scopeName names the scope it runs in; breakTo leaves the scopes inside the named one and
// goes on in it after the statement that was running; breakOut leaves the named one too.
static GameValue A1ScopeName(const GameState* state, GameValuePar oper1)
{
    state->SetScopeName(RString(oper1));
    return NOTHING;
}

static GameValue A1BreakTo(const GameState* state, GameValuePar oper1)
{
    state->BreakToScope(RString(oper1), false);
    return NOTHING;
}

static GameValue A1BreakOut(const GameState* state, GameValuePar oper1)
{
    state->BreakToScope(RString(oper1), true);
    return NOTHING;
}

// value breakOut "scope" (Arma 3): leave the scope with that value as its result
static GameValue A1BreakOutWith(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    state->BreakOutWithValue(RString(oper2), oper1);
    return NOTHING;
}

// for [{init}, {condition}, {step}] do {body}
static GameValue A1ForArray(const GameState*, GameValuePar oper1)
{
    return Tagged("__a1for", oper1);
}

static GameValue A1ForArrayDo(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& tag = oper1;
    const GameArrayType& parts = tag[1];
    if (parts.Size() != 3 || parts[0].GetType() != GameString || parts[1].GetType() != GameString ||
        parts[2].GetType() != GameString)
    {
        state->SetError(EvalDim, parts.Size(), 3);
        return NOTHING;
    }
    GameState* gs = const_cast<GameState*>(state);
    gs->Execute(RString(parts[0]));
    for (int i = 0; i < 100000 && !GameState::s_abortLoops; i++)
    {
        const GameValue test = state->EvaluateMultiple(RString(parts[1]));
        if (state->GetLastError() != EvalOK || !(test.GetType() & GameBool) || !(bool)test)
            break;
        gs->Execute(RString(oper2));
        // the body ran in this scope: breakTo/breakOut keeps unwinding from here, exitWith
        // ends the loop, any other error stops it (the step would clear the error)
        if (state->GetLastError() == EvalBreak)
        {
            if (GameState::s_breakTarget.empty())
                gs->SetError(EvalOK);
            break;
        }
        if (state->GetLastError() != EvalOK)
            break;
        gs->Execute(RString(parts[2]));
    }
    return NOTHING;
}

static GameValue A1SwitchDo(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (IsTagged(oper1, "__a1for"))
        return A1ForArrayDo(state, oper1, oper2);
    if (!IsTagged(oper1, "__a1switch"))
        return NOTHING;
    A1Switch sw;
    const GameArrayType& a = oper1;
    sw.value = a[1];
    sw.owner = CurrentScriptFiber();
    gSwitches.push_back(&sw);
    const_cast<GameState*>(state)->Execute(RString(oper2));
    // its own entry, wherever other scripts' switches have come to lie around it
    gSwitches.erase(std::remove(gSwitches.begin(), gSwitches.end(), &sw), gSwitches.end());
    if (!sw.done && sw.defaultCode.GetLength() > 0)
        sw.result = state->EvaluateMultiple(sw.defaultCode);
    return sw.result;
}

// Arma 1 createVehicle(Local): "#lightpoint" makes a script light (LGT-SCRIPT-001,
// World/Scene/ScriptLight.hpp). "#particlesource" has no counterpart yet: objNull, and its
// set* commands are ignored.
static GameValue A1VehCreate(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const RString type = oper1;
    if (type.GetLength() > 0 && type[0] == '#')
    {
        if (stricmp(type, "#lightpoint") == 0)
        {
            Vector3 pos;
            if (!GetPos(state, pos, oper2))
                return GameValueExt((Object*)nullptr);
            return GameValueExt(Poseidon::CreateScriptLight(pos));
        }
        return GameValueExt((Object*)nullptr);
    }
    return VehCreate(state, oper1, oper2);
}

// ---- LGT-SCRIPT-001: setLightColor / setLightAmbient / setLightBrightness /
// lightAttachObject / lightDetachObject on a "#lightpoint" object.
static Poseidon::ScriptLight* ScriptLightOf(GameValuePar oper)
{
    return dyn_cast<Poseidon::ScriptLight>(GetObject(oper));
}

static bool ScriptLightColor(const GameState* state, GameValuePar oper, Color& out)
{
    if (oper.GetType() != GameArray)
        return false;
    const GameArrayType& a = oper;
    if (a.Size() < 3)
    {
        state->SetError(EvalDim, a.Size(), 3);
        return false;
    }
    float c[3];
    for (int i = 0; i < 3; i++)
    {
        if (a[i].GetType() != GameScalar)
        {
            state->TypeError(GameScalar, a[i].GetType());
            return false;
        }
        c[i] = a[i];
    }
    out = Color(c[0], c[1], c[2], 1);
    return true;
}

static GameValue ScriptLightSetColor(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Poseidon::ScriptLight* light = ScriptLightOf(oper1);
    Color c;
    if (light && ScriptLightColor(state, oper2, c))
        light->SetColor(c);
    return NOTHING;
}

static GameValue ScriptLightSetAmbient(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Poseidon::ScriptLight* light = ScriptLightOf(oper1);
    Color c;
    if (light && ScriptLightColor(state, oper2, c))
        light->SetAmbient(c);
    return NOTHING;
}

static GameValue ScriptLightSetBrightness(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Poseidon::ScriptLight* light = ScriptLightOf(oper1);
    if (light)
        light->SetBrightness(static_cast<float>(oper2));
    return NOTHING;
}

// light lightAttachObject [object, [x,y,z]]  -- offset in the object's model space (Arma)
// light lightAttachObject [object, "memory point"]  -- OpenPoseidon extension
static GameValue ScriptLightAttach(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Poseidon::ScriptLight* light = ScriptLightOf(oper1);
    if (!light)
        return NOTHING;
    const GameArrayType& a = oper2;
    if (a.Size() < 2)
    {
        state->SetError(EvalDim, a.Size(), 2);
        return NOTHING;
    }
    Object* target = a[0].GetType() == GameObject ? GetObject(a[0]) : nullptr;
    if (!target)
    {
        light->Detach();
        return NOTHING;
    }
    Vector3 offset = VZero;
    if (a[1].GetType() == GameString)
    {
        const RString name = a[1];
        LODShape* shape = target->GetShape();
        Shape* memory = shape ? shape->MemoryLevel() : nullptr;
        const int index = memory ? memory->PointIndex(name) : -1;
        if (index < 0)
        {
            LOG_WARN(Script, "lightAttachObject: no memory point '{}' on {}", (const char*)name,
                     shape ? (const char*)shape->GetName() : "<no shape>");
            return NOTHING;
        }
        offset = memory->Pos(index);
    }
    else if (a[1].GetType() == GameArray)
    {
        const GameArrayType& p = a[1];
        if (p.Size() < 3)
        {
            state->SetError(EvalDim, p.Size(), 3);
            return NOTHING;
        }
        for (int i = 0; i < 3; i++)
        {
            if (p[i].GetType() != GameScalar)
            {
                state->TypeError(GameScalar, p[i].GetType());
                return NOTHING;
            }
        }
        offset = Vector3(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
    }
    else
    {
        state->TypeError(GameArray, a[1].GetType());
        return NOTHING;
    }
    light->AttachTo(target, offset);
    return NOTHING;
}

// Arma 3: light setLightIntensity n / setLightAttenuation [start, const, lin, quad, hardStart, hardEnd]
// / setLightDayLight bool. Approximated onto the LightPoint (ScriptLight::EffectiveBrightness).
static GameValue ScriptLightSetIntensity(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Poseidon::ScriptLight* light = ScriptLightOf(oper1);
    if (light)
        light->SetIntensity(static_cast<float>(oper2));
    return NOTHING;
}

static GameValue ScriptLightSetAttenuation(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Poseidon::ScriptLight* light = ScriptLightOf(oper1);
    if (!light)
        return NOTHING;
    const GameArrayType& a = oper2;
    // element 5 is the hard cut-off; without it the light keeps its intensity-derived reach
    float hardEnd = -1;
    if (a.Size() >= 6 && a[5].GetType() == GameScalar)
        hardEnd = static_cast<float>(a[5]);
    light->SetAttenuationLimit(hardEnd);
    return NOTHING;
}

static GameValue ScriptLightSetDayLight(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Poseidon::ScriptLight* light = ScriptLightOf(oper1);
    if (light)
        light->SetDayLight(static_cast<bool>(oper2));
    return NOTHING;
}

static GameValue ScriptLightDetach(const GameState*, GameValuePar oper1)
{
    Poseidon::ScriptLight* light = ScriptLightOf(oper1);
    if (light)
        light->Detach();
    return NOTHING;
}

// "title" hintC [texts] / "title" hintC "text": the texts as one hint
static GameValue A1HintC(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    RString text = oper1;
    if (oper2.GetType() == GameString)
        text = text + RString("\n") + RString(oper2);
    else if (oper2.GetType() == GameArray)
    {
        const GameArrayType& a = oper2;
        for (int i = 0; i < a.Size(); i++)
            if (a[i].GetType() == GameString)
                text = text + RString("\n") + RString(a[i]);
    }
    return ShowHintC(state, GameValue(text));
}

// parseText: structured text is plain text here -- tags dropped, <br/> as a line break
static GameValue A1ParseText(const GameState*, GameValuePar oper1)
{
    const std::string in = (const char*)RString(oper1);
    std::string out;
    for (size_t i = 0; i < in.size(); i++)
    {
        if (in[i] != '<')
        {
            out.push_back(in[i]);
            continue;
        }
        const size_t close = in.find('>', i);
        if (close == std::string::npos)
        {
            out.append(in, i, std::string::npos);
            break;
        }
        const std::string tag = in.substr(i + 1, close - i - 1);
        if (_strnicmp(tag.c_str(), "br", 2) == 0)
            out += "\\n";
        i = close;
    }
    return GameValue(RString(out.c_str()));
}

// animationState unit: the current move, lower case (Arma 1 compares it in lower case)
static GameValue A1AnimationState(const GameState*, GameValuePar oper1)
{
    EntityAI* ai = dyn_cast<EntityAI>(GetObject(oper1));
    RString move = ai ? ai->GetCurrentMove() : RString("");
    move.Lower();
    return GameValue(move);
}

// composeText [a, lineBreak, b]: plain text joined; lineBreak is a "\n" the statics wrap on
static GameValue A1ComposeText(const GameState*, GameValuePar oper1)
{
    const GameArrayType& a = oper1;
    RString out;
    for (int i = 0; i < a.Size(); i++)
        out = out + (a[i].GetType() == GameString ? RString(a[i]) : a[i].GetText());
    return GameValue(out);
}

static GameValue A1LineBreak(const GameState*)
{
    return GameValue(RString("\\n"));
}

// group selectLeader unit
static GameValue A1SelectLeader(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* grp = GetGroup(oper1);
    Person* man = dyn_cast<Person>(GetObject(oper2));
    if (grp && man && man->Brain() && man->Brain()->GetGroup() == grp)
        grp->SelectLeader(man->Brain());
    return NOTHING;
}

// selectPlayer unit: the player takes over that unit
static GameValue A1SelectPlayer(const GameState*, GameValuePar oper1)
{
    if (Person* man = dyn_cast<Person>(GetObject(oper1)))
        GWorld->SwitchPlayerTo(man);
    return NOTHING;
}

// object modelToWorld [x, y, z] -> world position (x, z, height above ground as positions are)
static GameValue A1ModelToWorld(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Object* obj = GetObject(oper1);
    const GameArrayType& a = oper2;
    GameArrayType out;
    if (!obj || a.Size() < 3)
        return GameValue(out);
    const Vector3 world = obj->PositionModelToWorld(Vector3((float)a[0], (float)a[2], (float)a[1]));
    out.Add(GameValue(world.X()));
    out.Add(GameValue(world.Z()));
    out.Add(GameValue(world.Y() - GLandscape->SurfaceYAboveWater(world.X(), world.Z())));
    return GameValue(out);
}

// boundingBox object -> [[minX, minY, minZ], [maxX, maxY, maxZ]] in model space (y up as Arma 1: z up)
static GameValue A1BoundingBox(const GameState*, GameValuePar oper1)
{
    Object* obj = GetObject(oper1);
    GameArrayType out;
    const auto corner = [](Vector3Val v)
    {
        GameArrayType c;
        c.Add(GameValue(v.X()));
        c.Add(GameValue(v.Z()));
        c.Add(GameValue(v.Y()));
        return GameValue(c);
    };
    if (obj && obj->GetShape())
    {
        out.Add(corner(obj->GetShape()->Min()));
        out.Add(corner(obj->GetShape()->Max()));
    }
    else
    {
        out.Add(corner(VZero));
        out.Add(corner(VZero));
    }
    return GameValue(out);
}

static GameValue A1AddVehicle(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* grp = GetGroup(oper1);
    Transport* veh = dyn_cast<Transport>(GetObject(oper2));
    if (grp && veh)
        grp->AddVehicle(veh);
    return NOTHING;
}

static GameValue A1EmptyArray(const GameState*, GameValuePar)
{
    return GameValue(GameArrayType());
}

static GameValue A1One(const GameState*, GameValuePar)
{
    return GameValue(1.0f);
}

// isPlayer unit: in single player, the player's unit
static GameValue A1IsPlayer(const GameState* state, GameValuePar oper1)
{
    Object* obj = GetObject(oper1);
    Object* player = GetObject(Player(state));
    return GameValue(obj != nullptr && obj == player);
}

// compile: code is a string here, so compiling is the identity
static GameValue A1Compile(const GameState*, GameValuePar oper1)
{
    // Arma 3 content: compiled text is code ({...}), which typeName and params tell apart.
    if (GameState::s_codeValues)
        return GameValue(new GameDataCode((RString)oper1));
    return oper1;
}

// unit moveInTurret [vehicle, path]: [0] is the gunner, anything else rides as commander or cargo
static GameValue A1MoveInTurret(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper2;
    if (array.Size() < 2 || array[0].GetType() != GameObject || array[1].GetType() != GameArray)
        return NOTHING;
    const GameArrayType& path = array[1];
    const int first = path.Size() > 0 && path[0].GetType() == GameScalar ? toInt((float)path[0]) : -1;
    if (first == 0)
        return ObjMoveInGunner(state, oper1, array[0]);
    if (first == 1)
        return ObjMoveInCommander(state, oper1, array[0]);
    return ObjMoveInCargo(state, oper1, array[0]);
}

// Arma 1: distance preloadObject "className" -- models stream on demand here
static GameValue A1PreloadClass(const GameState*, GameValuePar, GameValuePar)
{
    return GameValue(true);
}

static GameValue A1PreloadSound(const GameState*, GameValuePar)
{
    return GameValue(true); // sounds load when played
}

static GameValue A1Ignored(const GameState*, GameValuePar)
{
    return NOTHING; // setAperture, enableTeamSwitch, ...: no counterpart here
}

namespace Poseidon
{
int GetDaysInMonth(int year, int month);
}

// date: [year, month, day, hour, minute]
static GameValue A1Date(const GameState*)
{
    int day = toIntFloor(Glob.clock.GetTimeInYear() / OneDay);
    const int year = Glob.clock.GetYear();
    int month = 0;
    while (month < 11 && day >= Poseidon::GetDaysInMonth(year, month))
        day -= Poseidon::GetDaysInMonth(year, month++);
    const int minutes = toIntFloor(Glob.clock.GetTimeOfDay() * 24 * 60 + 0.01f);
    GameArrayType date;
    date.Add(GameValue((float)year));
    date.Add(GameValue((float)(month + 1)));
    date.Add(GameValue((float)(day + 1)));
    date.Add(GameValue((float)(minutes / 60)));
    date.Add(GameValue((float)(minutes % 60)));
    return GameValue(date);
}

// unit/units doGetOut: leave the vehicle now, keep the assignment
static GameValue A1DoGetOut(const GameState* state, GameValuePar oper1)
{
    GameValue units = oper1;
    if (oper1.GetType() != GameArray)
    {
        GameArrayType list;
        list.Add(oper1);
        units = GameValue(list);
    }
    return ListOrderGetIn(state, units, GameValue(false));
}

// group enableAttack bool / enableEnvironment bool / reload unit: no CWA state behind them.
static GameValue A1IgnoredBinary(const GameState*, GameValuePar, GameValuePar)
{
    return NOTHING;
}

// isNil "name": true when the variable has no value
static GameValue A1IsNil(const GameState* state, GameValuePar oper1)
{
    RString name = oper1;
    if (name.GetLength() == 0)
        return GameValue(true);
    // isNil "name" asks about a variable; isNil {code} (a string here too) about the
    // value of an expression. Anything that is not a bare identifier is evaluated.
    const char* p = name;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    const char* q = p;
    while (*q && (isalnum(static_cast<unsigned char>(*q)) || *q == '_'))
        q++;
    const char* r = q;
    while (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n')
        r++;
    if (q > p && *r == 0)
        return GameValue(state->VarGet(RString(p, static_cast<int>(q - p))).GetNil());
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    const GameValue value = state->EvaluateMultiple(name);
    state->EndContext();
    return GameValue(value.GetNil() || value.GetType() == GameNothing);
}

static GameValue A1IgnoredUnary(const GameState*, GameValuePar)
{
    return NOTHING;
}

static const GameNular* GetExtNular(int& count)
{
    static const GameNular ExtNular[] = {
        GameNular(GameObject, "player", Player),
        GameNular(GameObject, "objNull", ObjNull),
        GameNular(GameGroup, "grpNull", GrpNull),
        GameNular(GameScalar, "time", GameTime),
        GameNular(GameScalar, "dayTime", DayTime),
        GameNular(GameArray, "date", A1Date),
        GameNular(GameString, "lineBreak", A1LineBreak),
        GameNular(GameString, "configFile", A1ConfigFile),
        GameNular(GameString, "missionConfigFile", A1MissionConfigFile),
        GameNular(GameBool, "cadetMode", CadetMode),
        GameNular(GameScalar, "benchmark", Benchmark),
        GameNular(GameScalar, "accTime", GetAcceleratedTime),

        GameNular(GameBool, "shownMap", IsMapShown),
        GameNular(GameBool, "shownWatch", IsWatchShown),
        GameNular(GameBool, "shownCompass", IsCompassShown),
        GameNular(GameBool, "shownRadio", IsWalkieTalkieShown),
        GameNular(GameBool, "shownPad", IsNotepadShown),
        GameNular(GameBool, "shownWarrant", IsWarrantShown),
        GameNular(GameBool, "shownGps", IsGPSShown),

        GameNular(GameSide, "west", SideWest),
        GameNular(GameSide, "east", SideEast),
        GameNular(GameSide, "civilian", SideCivilian),
        GameNular(GameSide, "resistance", SideResistance),
        GameNular(GameSide, "sideLogic", SideLogic),
        GameNular(GameSide, "sideEnemy", SideEnemy),
        GameNular(GameSide, "sideFriendly", SideFriendly),

        GameNular(GameNothing, "saveGame", SaveGame),
        GameNular(GameNothing, "exit", ScriptExit),

        GameNular(GameNothing, "enableEndDialog", EnableEndDialog),
        GameNular(GameNothing, "forceEnd", ForceEnd),
        GameNular(GameScalar, "musicVolume", GetMusicVolume),
        GameNular(GameScalar, "soundVolume", GetSoundVolume),

        GameNular(GameNothing, "mapAnimClear", MapAnimClear),
        GameNular(GameNothing, "mapAnimCommit", MapAnimCommit),
        GameNular(GameBool, "mapAnimDone", MapAnimDone),

        GameNular(GameNothing, "clearWeaponPool", Poseidon::PoolClearWeapons),
        GameNular(GameNothing, "clearMagazinePool", Poseidon::PoolClearMagazines),

        GameNular(GameNothing, "cheatsEnabled", IsCheatsEnabled),

        GameNular(GameBool, "dialog", IsDialog),

        GameNular(GameObject, "cameraOn", CameraOn),

        GameNular(GameFile, "newConfig", ConfigNew),
        GameNular(GameDatabase, "dbNew", DatabaseNew),
        GameNular(GameArray, "listConfigNames", ConfigListNames),
        GameNular(GameString, "missionName", MissionName),
        GameNular(GameArray, "missionStart", MissionStart),
        GameNular(GameString, "getWorld", WorldName),
        GameNular(GameString, "voiceLanguage", VoiceLanguage),
        GameNular(GameBool, "isServer", IsServer),
        GameNular(GameBool, "isJIP", IsJIP),
        GameNular(GameNothing, "serverPause", ServerPause),
        GameNular(GameNothing, "serverResume", ServerResume),

        GameNular(GameScalar, "worldSize", WorldSize),
        GameNular(GameScalar, "mapWidth", MapWidth),
        GameNular(GameScalar, "mapHeight", MapHeight),
        GameNular(GameArray, "mapLeftTopPos", MapLeftTopPos),

        // remove player "injured" status
        // it is annoying in MP game (especially in CTI) that injured status won't disappear
        // if accidentally activated "injured" channel
        // todo: maybe this shouldn't be designed as a script command
        GameNular(GameNothing, "playerClearInjured", PlayerClearInjured),
        GameNular(GameScalar, "usedVersion", UsedVersion),
    };
    count = sizeof(ExtNular) / sizeof(*ExtNular);
    return ExtNular;
}

static const GameFunction* GetExtUnary(int& count)
{
    static const GameFunction ExtUnary[] = {
        GameFunction(GameBool, "isNull", ObjIsNull, GameObject),
        GameFunction(GameScalar, "execVM", ScriptExecVMUnary, GameString),
        GameFunction(GameBool, "camPreloaded", A1CamPreloaded, GameObject),
        GameFunction(GameBool, "preloadSound", A1PreloadSound, GameString),
        GameFunction(GameString, "str", A1Str, GameVoid),
        GameFunction(GameString, "compile", A1Compile, GameString),
        GameFunction(GameBool, "isPlayer", A1IsPlayer, GameObject),
        GameFunction(GameNothing, "selectPlayer", A1SelectPlayer, GameObject),
        GameFunction(GameNothing, "scopeName", A1ScopeName, GameString),
        GameFunction(GameNothing, "breakOut", A1BreakOut, GameString),
        GameFunction(GameNothing, "breakTo", A1BreakTo, GameString),
        GameFunction(GameArray, "boundingBox", A1BoundingBox, GameObject),
        GameFunction(GameArray, "waypoints", A2Waypoints, GameObjectOrGroup),
        GameFunction(GameScalar, "precision", A1One, GameObject),
        GameFunction(GameNothing, "ctrlSetFocus", A1Ignored, GameScalar),
        GameFunction(GameString, "parseText", A1ParseText, GameString),
        GameFunction(GameString, "getText", A1GetText, GameString),
        GameFunction(GameScalar, "getNumber", A1GetNumber, GameString),
        GameFunction(GameArray, "getArray", A1GetArray, GameString),
        GameFunction(GameBool, "isClass", A1IsClass, GameString),
        GameFunction(GameBool, "isArray", A1IsArray, GameString),
        GameFunction(GameBool, "isText", A1IsText, GameString),
        GameFunction(GameBool, "isNumber", A1IsNumber, GameString),
        GameFunction(GameString, "configName", A1ConfigName, GameString),
        GameFunction(GameScalar, "count", A1ConfigCount, GameString),
        GameFunction(GameString, "inheritsFrom", A1InheritsFrom, GameString),
        GameFunction(GameString, "typeName", A1TypeName, GameVoid),
        GameFunction(GameBool, "surfaceIsWater", A1SurfaceIsWater, GameArray),
        GameFunction(GameArray, "getWeaponCargo", A1GetWeaponCargo, GameObject),
        GameFunction(GameArray, "getMagazineCargo", A1GetMagazineCargo, GameObject),
        GameFunction(GameString, "animationState", A1AnimationState, GameObject),
        GameFunction(GameString, "composeText", A1ComposeText, GameArray),
        GameFunction(GameArray, "switch", A1Switch_, GameVoid),
        GameFunction(GameArray, "for", A1ForArray, GameArray),
        GameFunction(GameArray, "case", A1Case, GameVoid),
        GameFunction(GameNothing, "default", A1Default, GameString),
        GameFunction(GameNothing, "hideBody", A1Ignored, GameObject),
        GameFunction(GameNothing, "setAperture", A1Ignored, GameScalar),
        GameFunction(GameNothing, "enableTeamSwitch", A1Ignored, GameBool),
        GameFunction(GameBool, "isNil", A1IsNil, GameString),
        GameFunction(GameNothing, "doGetOut", A1DoGetOut, GameObjectOrArray),
        GameFunction(GameNothing, "enableEnvironment", A1IgnoredUnary, GameBool),
        GameFunction(GameNothing, "lightDetachObject", ScriptLightDetach, GameObject),
        GameFunction(GameNothing, "reload", A1Reload, GameObject),
        GameFunction(GameNothing, "terminate", ScriptTerminate, GameScalar),
        GameFunction(GameBool, "scriptDone", ScriptIsDone, GameScalar),
        GameFunction(GameBool, "waitUntil", ScriptWaitUntilUnscheduled, GameString),
        GameFunction(GameNothing, "sleep", ScriptSleepUnscheduled, GameScalar),
        GameFunction(GameBool, "preloadCamera", PreloadCameraReady, GameArray),
        GameFunction(GameBool, "isKeyActive", CampaignKeyActive, GameString),
        GameFunction(GameNothing, "activateKey", CampaignKeyActivate, GameString),
        GameFunction(GameBool, "isNull", GrpIsNull, GameGroup),
        GameFunction(GameBool, "alive", ObjAlive, GameObject),
        GameFunction(GameBool, "local", ObjIsLocal, GameObject),
        GameFunction(GameBool, "requiredVersion", RequiredVersion, GameString),
        GameFunction(GameArray, "getPos", ObjGetPos, GameObject),
        GameFunction(GameArray, "position", ObjGetPos, GameObject),
        GameFunction(GameArray, "getPosASL", ObjGetPosASL, GameObject),
        GameFunction(GameScalar, "getDir", ObjGetDir, GameObject),
        GameFunction(GameArray, "getTurretDirAndElev", ObjGetTurretDirAndElev, GameObject),
        GameFunction(GameScalar, "direction", ObjGetDir, GameObject),
        GameFunction(GameArray, "velocity", ObjGetVelocity, GameObject),
        GameFunction(GameScalar, "getDammage", ObjGetDammage, GameObject),
        GameFunction(GameScalar, "getDamage", ObjGetDammage, GameObject), // one-M alias (cf. setDammage/setDamage)
        GameFunction(GameScalar, "damage", ObjGetDammage, GameObject),
        GameFunction(GameScalar, "speed", ObjGetSpeed, GameObject),
        GameFunction(GameArray, "getMarkerPos", MarkerGetPos, GameString),
        GameFunction(GameArray, "markerPos", MarkerGetPos, GameString),
        GameFunction(GameString, "getMarkerType", MarkerGetType, GameString),
        GameFunction(GameString, "markerType", MarkerGetType, GameString),
        GameFunction(GameArray, "getMarkerSize", MarkerGetSize, GameString),
        GameFunction(GameArray, "markerSize", MarkerGetSize, GameString),
        GameFunction(GameString, "getMarkerColor", MarkerGetColor, GameString),
        GameFunction(GameString, "markerColor", MarkerGetColor, GameString),
        GameFunction(GameArray, "getWPPos", WpGetPos, GameArray),
        GameFunction(GameArray, "waypointPosition", WpGetPos, GameArray),
        GameFunction(GameObject, "nearestBuilding", ObjGetNearestBuilding, GameObject),
        GameFunction(GameObject, "nearestObject", GetNearestObject, GameArray),
        GameFunction(GameBool, "canMove", ObjCanMove, GameObject),
        GameFunction(GameBool, "canFire", ObjCanFire, GameObject),
        GameFunction(GameBool, "canStand", ObjCanStand, GameObject),
        GameFunction(GameScalar, "handsHit", ObjHandsHit, GameObject),
        GameFunction(GameBool, "fleeing", ObjFlee, GameObject),
        GameFunction(GameBool, "someAmmo", ObjSomeAmmo, GameObject),
        GameFunction(GameScalar, "fuel", ObjFuel, GameObject),
        GameFunction(GameScalar, "rating", ObjExperience, GameObject),
        GameFunction(GameScalar, "score", ObjGetScore, GameObject),
        GameFunction(GameObject, "formLeader", ObjLeader, GameObject),
        GameFunction(GameObject, "leader", ObjGroupLeader, GameObject),
        GameFunction(GameObject, "vehicle", ObjVehicle, GameObject),
        GameFunction(GameObject, "driver", ObjDriver, GameObject),
        GameFunction(GameObject, "commander", ObjCommander, GameObject),
        GameFunction(GameObject, "gunner", ObjGunner, GameObject),

        GameFunction(GameObject, "leader", GrpLeader, GameGroup),
        GameFunction(GameGroup, "group", ObjGroup, GameObject),
        GameFunction(GameArray, "units", GrpUnits, GameGroup),
        GameFunction(GameArray, "units", GrpUnits, GameObject),

        GameFunction(GameArray, "crew", ObjListIn, GameObject),

        GameFunction(GameObject, "flagOwner", ObjGetFlagOwner, GameObject),
        GameFunction(GameObject, "flag", ObjGetFlag, GameObject),

        GameFunction(GameBool, "inflamed", ObjInflamed, GameObject),
        GameFunction(GameString, "lightIsOn", ObjLightSwitched, GameObject),

        GameFunction(GameScalar, "scudState", ObjGetScudState, GameObject),

        GameFunction(GameArray, "list", ObjList, GameObject),
        GameFunction(GameSide, "side", ObjSide, GameObjectOrGroup),
        GameFunction(GameString, "name", ObjName, GameObject),
        GameFunction(GameString, "behaviour", ObjBehaviour, GameObject),
        GameFunction(GameString, "behavior", ObjBehaviour, GameObject), // US-spelling alias
        GameFunction(GameString, "combatMode", GrpCombatMode, GameObjectOrGroup),
        GameFunction(GameString, "formation", GrpFormation, GameObjectOrGroup),
        GameFunction(GameString, "speedMode", GrpSpeedMode, GameObjectOrGroup),
        GameFunction(GameNothing, "titleCut", CutText, GameArray),

        GameFunction(GameNothing, "cutText", CutText, GameArray),
        GameFunction(GameNothing, "cutRsc", CutRsc, GameArray),
        GameFunction(GameNothing, "cutObj", CutObj, GameArray),

        GameFunction(GameNothing, "titleText", GameTitleText, GameArray),
        GameFunction(GameNothing, "titleRsc", GameTitleRsc, GameArray),
        GameFunction(GameNothing, "titleObj", GameTitleObj, GameArray),

        GameFunction(GameNothing, "playSound", PlaySound, GameString),
        GameFunction(GameScalar, "soundLength", SoundLength, GameString),
        GameFunction(GameNothing, "playMusic", PlayMusic, GameString),
        GameFunction(GameNothing, "playMusic", PlayMusic, GameArray),

        GameFunction(GameBool, "locked", ObjLocked, GameObject),
        GameFunction(GameBool, "stopped", ObjStopped, GameObject),
        GameFunction(GameBool, "captive", ObjCaptive, GameObject),

        GameFunction(GameNothing, "unassignVehicle", ObjUnassignVehicle, GameObject),

        GameFunction(GameNothing, "removeAllWeapons", ObjRemoveAllWeapons, GameObject),

        GameFunction(GameNothing, "clearWeaponCargo", ObjClearWeaponCargo, GameObject),
        GameFunction(GameNothing, "clearMagazineCargo", ObjClearMagazineCargo, GameObject),

        GameFunction(GameNothing, "deleteVehicle", VehDelete, GameObject),

        GameFunction(GameNothing, "showMap", ShowMap, GameBool),
        GameFunction(GameNothing, "showWatch", ShowWatch, GameBool),
        GameFunction(GameNothing, "showCompass", ShowCompass, GameBool),
        GameFunction(GameNothing, "showRadio", ShowWalkieTalkie, GameBool),
        GameFunction(GameNothing, "showPad", ShowNotepad, GameBool),
        GameFunction(GameNothing, "showWarrant", ShowWarrant, GameBool),
        GameFunction(GameNothing, "showGps", ShowGPS, GameBool),
        GameFunction(GameNothing, "enableRadio", EnableRadio, GameBool),

        GameFunction(GameNothing, "showCinemaBorder", ShowCinemaBorder, GameBool),
        GameFunction(GameNothing, "disableUserInput", DisableUserInput, GameBool),
        GameFunction(GameNothing, "loadMission", LoadMission, GameString),
        GameFunction(GameNothing, "onPlayerConnected", OnPlayerConnected, GameString),
        GameFunction(GameNothing, "onPlayerDisconnected", OnPlayerDisconnected, GameString),
        GameFunction(GameNothing, "publicVariable", PublicVariable, GameString),
        GameFunction(GameNothing, "saveMission", SaveMission, GameString),
        GameFunction(GameNothing, "publicVariableArray", PublicVariable, GameString),
        GameFunction(GameNothing, "publicVariableString", PublicVariable, GameString),

        GameFunction(GameNothing, "hint", ShowHint, GameString),
        GameFunction(GameNothing, "hintCadet", ShowHintCadet, GameString),
        GameFunction(GameNothing, "hintC", ShowHintC, GameString),

        GameFunction(GameString, "format", StrFormat, GameArray),
        GameFunction(GameString, "localize", StrLocalize, GameString),

        GameFunction(GameNothing, "skipTime", SkipDayTime, GameScalar),
        GameFunction(GameNothing, "setAirportClosed", SetAirportClosed, GameArray),
        GameFunction(GameNothing, "setAirportSide", SetAirportSide, GameArray),
        GameFunction(GameNothing, "setViewDistance", SetViewDistance, GameScalar),
        GameFunction(GameNothing, "setTerrainGrid", SetTerrainGrid, GameScalar),

        GameFunction(GameNothing, "goto", ScriptGoto, GameString),

        GameFunction(GameNothing, "textLog", TextLog, GameVoid),
        GameFunction(GameNothing, "debugLog", TextDebugLog, GameVoid),
        GameFunction(GameNothing, "logInfo", LogInfo, GameVoid),

        GameFunction(GameNothing, "camDestroy", CamDestroy, GameObject),
        GameFunction(GameBool, "camCommitted", CamCommited, GameObject),

        GameFunction(GameBool, "unitReady", VehDone, GameObjectOrArray),

        GameFunction(GameNothing, "saveVar", SaveVar, GameString),

        GameFunction(GameNothing, "setAccTime", SetAcceleratedTime, GameScalar),

        GameFunction(GameNothing, "forceMap", MapForce, GameBool),
        GameFunction(GameNothing, "mapAnimAdd", MapAnimAdd, GameArray),

        GameFunction(GameNothing, "estimatedTimeLeft", EstimatedTimeLeft, GameScalar),

        GameFunction(GameBool, "createDialog", DialogCreate, GameString),
        GameFunction(GameScalar, "findDisplay", A1FindDisplay, GameScalar),
        GameFunction(GameBool, "ctrlCommitted", A1CtrlCommitted, GameScalar),
        GameFunction(GameArray, "ctrlPosition", A1CtrlPosition, GameScalar),
        GameFunction(GameScalar, "ctrlParent", A1CtrlParent, GameScalar),
        GameFunction(GameNothing, "closeDialog", DialogClose, GameScalar),

        GameFunction(GameBool, "ctrlVisible", CtrlVisible, GameScalar),
        GameFunction(GameBool, "ctrlEnabled", CtrlEnabled, GameScalar),
        GameFunction(GameNothing, "ctrlShow", CtrlShow, GameArray),
        GameFunction(GameNothing, "ctrlEnable", CtrlEnable, GameArray),

        GameFunction(GameString, "ctrlText", CtrlGetText, GameScalar),
        GameFunction(GameNothing, "ctrlSetText", CtrlSetText, GameArray),

        GameFunction(GameString, "buttonAction", ButtonGetAction, GameScalar),
        GameFunction(GameNothing, "buttonSetAction", ButtonSetAction, GameArray),

        GameFunction(GameScalar, "lbSize", LBGetSize, GameScalar),
        GameFunction(GameScalar, "lbCurSel", LBGetCurSel, GameScalar),
        GameFunction(GameNothing, "lbSetCurSel", LBSetCurSel, GameArray),
        GameFunction(GameNothing, "lbClear", LBClear, GameScalar),
        GameFunction(GameScalar, "lbAdd", LBAdd, GameArray),
        GameFunction(GameNothing, "lbDelete", LBDelete, GameArray),
        GameFunction(GameString, "lbText", LBGetText, GameArray),
        GameFunction(GameString, "lbData", LBGetData, GameArray),
        GameFunction(GameNothing, "lbSetData", LBSetData, GameArray),
        GameFunction(GameScalar, "lbValue", LBGetValue, GameArray),
        GameFunction(GameNothing, "lbSetValue", LBSetValue, GameArray),
        GameFunction(GameString, "lbPicture", LBGetPicture, GameArray),
        GameFunction(GameNothing, "lbSetPicture", LBSetPicture, GameArray),
        GameFunction(GameArray, "lbColor", LBGetColor, GameArray),
        GameFunction(GameNothing, "lbSetColor", LBSetColor, GameArray),

        GameFunction(GameScalar, "sliderPosition", SliderGetPosition, GameScalar),
        GameFunction(GameNothing, "sliderSetPosition", SliderSetPosition, GameArray),
        GameFunction(GameArray, "sliderRange", SliderGetRange, GameScalar),
        GameFunction(GameNothing, "sliderSetRange", SliderSetRange, GameArray),
        GameFunction(GameArray, "sliderSpeed", SliderGetSpeed, GameScalar),
        GameFunction(GameNothing, "sliderSetSpeed", SliderSetSpeed, GameArray),

        GameFunction(GameNothing, "addWeaponPool", Poseidon::PoolAddWeapon, GameArray),
        GameFunction(GameNothing, "addMagazinePool", Poseidon::PoolAddMagazine, GameArray),

        GameFunction(GameNothing, "putWeaponPool", Poseidon::PoolGetWeapons, GameObject),
        GameFunction(GameNothing, "pickWeaponPool", Poseidon::PoolSetWeapons, GameObject),

        GameFunction(GameScalar, "queryWeaponPool", Poseidon::PoolQueryWeapons, GameString),
        GameFunction(GameScalar, "queryMagazinePool", Poseidon::PoolQueryMagazines, GameString),

        GameFunction(GameNothing, "drop", Poseidon::ParticleDrop, GameArray),

        GameFunction(GameNothing, "onBriefingPlan", BriefingOnPlan, GameString),
        GameFunction(GameNothing, "onBriefingNotes", BriefingOnNotes, GameString),
        GameFunction(GameNothing, "onBriefingGear", BriefingOnGear, GameString),
        GameFunction(GameNothing, "onBriefingGroup", BriefingOnGroup, GameString),

        GameFunction(GameNothing, "onMapSingleClick", MapOnSingleClick, GameString),

        GameFunction(GameNothing, "fillWeaponsFromPool", ObjWeaponsFromPool, GameObject),

        GameFunction(GameScalar, "skill", ObjGetSkill, GameObject),

        GameFunction(GameString, "primaryWeapon", ObjGetPrimaryWeapon, GameObject),
        GameFunction(GameString, "secondaryWeapon", ObjGetSecondaryWeapon, GameObject),
        GameFunction(GameArray, "weapons", ObjGetAllWeapons, GameObject),
        GameFunction(GameArray, "magazines", ObjGetAllMagazines, GameObject),
        GameFunction(GameArray, "getUnitLoadout", ObjUnitLoadout, GameObject),

        GameFunction(GameObject, "object", GetObject, GameScalar),

        GameFunction(GameBool, "deleteStatus", DeleteStatus, GameString),
        GameFunction(GameBool, "deleteIdentity", DeleteIdentity, GameString),
        GameFunction(GameNothing, "publicExec", PublicExec, GameString),
        GameFunction(GameNothing, "remoteExecRemove", RemoteExecRemove, GameString),

        GameFunction(GameFile, "loadConfig", ConfigLoad, GameString),
        GameFunction(GameDatabase, "dbLoad", DatabaseLoad, GameString),

        GameFunction(GameNothing, "VBS_addHeader", AddMPReportHeader, GameString),
        GameFunction(GameNothing, "VBS_addEvent", AddMPReportEvent, GameString),
        GameFunction(GameNothing, "VBS_addFooter", AddMPReportFooter, GameString),

        GameFunction(GameArray, "VBS_kills", GetMPReportKills, GameString),
        GameFunction(GameArray, "VBS_killed", GetMPReportKilled, GameString),
        GameFunction(GameArray, "VBS_injuries", GetMPReportInjuries, GameString),

        GameFunction(GameString, "loadFile", StringLoad, GameString),
        GameFunction(GameString, "preprocessFile", StringPreprocess, GameString),
        GameFunction(GameString, "preprocessFileLineNumbers", StringPreprocess, GameString),

        GameFunction(GameScalar, "playersNumber", PlayersNumber, GameSide),

        GameFunction(GameBool, "isEngineOn", VehIsEngineOn, GameObject),

        GameFunction(GameString, "netId", GetNetIdObj, GameObject),
        GameFunction(GameObject, "objectFromNetId", GetObjFromNetId, GameString),
        // Array-based netId for compatibility with 2.01-era missions.
        GameFunction(GameArray, "NetworkId", GetNetworkId, GameObject),
        GameFunction(GameObject, "UnitById", GetUnitById, GameArray),
        TABLE_COMMAND_S(stop, Stop)

#if _ENABLE_CHEATS
            GameFunction(GameNothing, "DBG_switchLandscape", DBG_SwitchLandscape, GameString),
        GameFunction(GameNothing, "DBG_screenshot", DBG_Screenshot, GameString),
#endif

        GameFunction(GameNothing, "setDate", SetDate, GameArray),

        GameFunction(GameSide, "createCenter", CenterCreate, GameSide),
        GameFunction(GameNothing, "deleteCenter", CenterDelete, GameSide),
        GameFunction(GameGroup, "createGroup", GroupCreate, GameSide),
        GameFunction(GameNothing, "deleteGroup", GroupDelete, GameGroup),

        GameFunction(GameString, "createMarker", MarkerCreate, GameArray),
        GameFunction(GameNothing, "deleteMarker", MarkerDelete, GameString),
        GameFunction(GameScalar, "getMarkerDir", MarkerGetDir, GameString),
        // todo: add getMarkerText/Shape/Brush
        // todo: markersMap is an AutoArray. Is it possible to realize it by std::map?

        GameFunction(GameObject, "createTrigger", TriggerCreate, GameArray),
        GameFunction(GameNothing, "createGuardedPoint", GuardedPointCreate, GameArray),

        GameFunction(GameNothing, "deleteWaypoint", WaypointDelete, GameArray),
        GameFunction(GameString, "getMove", ObjGetMove, GameObject),

        GameFunction(GameString, "typeOf", ObjGetType, GameObject),

        GameFunction(GameArray, "vectorUp", ObjGetVectorUp, GameObject),
        GameFunction(GameArray, "vectorDir", ObjGetVectorDir, GameObject),

        GameFunction(GameObject, "laserTarget", GetLaserTarget, GameObject),
        GameFunction(GameScalar, "getMass", ObjGetMass, GameObject),
        GameFunction(GameObject, "NearestObjectDistance", GetNearestObjectByDistance, GameArray),
        GameFunction(GameString, "substr", StrSub, GameArray),
        GameFunction(GameScalar, "sizeofstr", StrSize, GameString),
        // todo: add more parameters and adjust parameters to make some of them has default value
        GameFunction(GameObject, "createShell", ShellCreate, GameArray),
        GameFunction(GameArray, "magazinesArray", ObjMagazinesArray, GameObject),
        GameFunction(GameArray, "GetHitpointsNames", ObjGetHitpointsNames, GameObject),
        GameFunction(GameNothing, "showDebug", DebugShow, GameArray),
    };
    count = sizeof(ExtUnary) / sizeof(*ExtUnary);
    return ExtUnary;
}

static const GameOperator* GetExtBinary(int& count)
{
    static const GameOperator ExtBinary[] = {
        GameOperator(GameNothing, "setCaptive", function, ObjSetCaptive, GameObject, GameBool),
        GameOperator(GameNothing, "setIdentity", function, ObjSetIdentity, GameObject, GameString),
        GameOperator(GameNothing, "setSpeaker", function, ObjSetSpeaker, GameObject, GameString),
        GameOperator(GameNothing, "setPitch", function, ObjSetPitch, GameObject, GameScalar),
        GameOperator(GameNothing, "setName", function, ObjSetName, GameObject, GameString | GameArray),
        GameOperator(GameNothing, "setFaceanimation", function, ObjSetFaceAnimation, GameObject, GameScalar),
        GameOperator(GameNothing, "setFace", function, ObjSetFace, GameObject, GameString),
        GameOperator(GameNothing, "setMimic", function, ObjSetMimic, GameObject, GameString),
        GameOperator(GameNothing, "setFlagTexture", function, ObjSetFlagTexture, GameObject, GameString),
        GameOperator(GameNothing, "setFlagSide", function, ObjSetFlagSide, GameObject, GameSide),
        GameOperator(GameNothing, "setFlagOwner", function, ObjSetFlagOwner, GameObject, GameObject),
        GameOperator(GameNothing, "remoteExec", function, RemoteExec, GameAny, GameString | GameArray),
        GameOperator(GameNothing, "remoteExecCall", function, RemoteExecCall, GameAny, GameString | GameArray),
        GameOperator(GameArray, "buildingPos", function, ObjGetBuildingPos, GameObject, GameScalar),
        GameOperator(GameNothing, "switchLight", function, ObjSwitchLight, GameObject, GameString),
        GameOperator(GameNothing, "inflame", function, ObjInflame, GameObject, GameBool),
        GameOperator(GameNothing, "addRating", function, ObjAddExperience, GameObject, GameScalar),
        GameOperator(GameNothing, "addScore", function, ObjAddScore, GameObject, GameScalar),
        GameOperator(GameScalar, "distance", function, ObjDistance, GameObject, GameObject),
        GameOperator(GameScalar, "distance", function, A1Distance, GameArray, GameObject | GameArray),
        GameOperator(GameScalar, "distance", function, A1Distance, GameObject, GameArray),
        GameOperator(GameNothing, "moveInTurret", function, A1MoveInTurret, GameObject, GameArray),
        GameOperator(GameBool, "isKindOf", function, A1IsKindOf, GameObject | GameString, GameString),
        GameOperator(GameBool, "isKindOf", function, A1IsKindOfConfig, GameString, GameArray),
        GameOperator(GameVoid, "do", function, A1SwitchDo, GameArray, GameString),
        GameOperator(GameNothing, ":", function, A1CaseCode, GameArray, GameString),
        GameOperator(GameNothing, "assignTeam", function, A1IgnoredBinary, GameObject, GameString),
        GameOperator(GameNothing, "setPos", function, ObjSetPos, GameObject, GameArray),
        GameOperator(GameNothing, "setPosASL", function, ObjSetPosASL, GameObject, GameArray),
        GameOperator(GameNothing, "setDir", function, ObjSetDir, GameObject, GameScalar),
        GameOperator(GameNothing, "setVelocity", function, ObjSetVelocity, GameObject, GameArray),
        GameOperator(GameNothing, "setFormDir", function, GrpSetFormDir, GameObjectOrGroup, GameScalar),
        GameOperator(GameNothing, "setDammage", function, ObjSetDammage, GameObject, GameScalar),
        GameOperator(GameNothing, "setDamage", function, ObjSetDammage, GameObject, GameScalar),
        GameOperator(GameNothing, "allowDammage", function, ObjAllowDammage, GameObject, GameBool),
        // Grammatically-correct one-M alias resolves to the same handler (cf. setDammage/setDamage).
        GameOperator(GameNothing, "allowDamage", function, ObjAllowDammage, GameObject, GameBool),
        GameOperator(GameNothing, "flyInHeight", function, ObjSetFlyingHeight, GameObject, GameScalar),
        GameOperator(GameNothing, "setMarkerPos", function, MarkerSetPos, GameString, GameArray),
        GameOperator(GameNothing, "setMarkerType", function, MarkerSetType, GameString, GameString),
        GameOperator(GameNothing, "setMarkerSize", function, MarkerSetSize, GameString, GameArray),
        GameOperator(GameNothing, "setMarkerColor", function, MarkerSetColor, GameString, GameString),
        GameOperator(GameNothing, "setWPPos", function, WpSetPos, GameArray, GameArray),
        GameOperator(GameBool, "in", function, ObjIn, GameObject, GameObject),
        GameOperator(GameScalar, "ammo", function, ObjAmmo, GameObject, GameString),
        GameOperator(GameBool, "hasWeapon", function, ObjHasWeapon, GameObject, GameString),
        GameOperator(GameNothing, "addWeapon", function, ObjAddWeapon, GameObject, GameString),
        GameOperator(GameNothing, "removeWeapon", function, ObjRemoveWeapon, GameObject, GameString),
        GameOperator(GameNothing, "addMagazine", function, ObjAddMagazine, GameObject, GameString),
        GameOperator(GameNothing, "removeMagazine", function, ObjRemoveMagazine, GameObject, GameString),
        GameOperator(GameNothing, "removeMagazines", function, ObjRemoveMagazines, GameObject, GameString),
        GameOperator(GameNothing, "selectWeapon", function, ObjSelectWeapon, GameObject, GameString),
        GameOperator(GameNothing, "setUnitLoadout", function, ObjSetUnitLoadout, GameObject, GameArray),
        GameOperator(GameNothing, "fire", function, ObjFire, GameObject, GameString),
        GameOperator(GameNothing, "fire", function, ObjFireEx, GameObject, GameArray),
        GameOperator(GameScalar, "muzzleReloadTime", function, ObjMuzzleReloadTime, GameObject, GameString),
        GameOperator(GameNothing, "land", function, ObjLand, GameObject, GameString),
        GameOperator(GameScalar, "knowsAbout", function, GrpKnowsAbout, GameObjectOrGroup, GameObject),
        GameOperator(GameNothing, "say", function, ObjSay, GameObject, GameString),
        GameOperator(GameNothing, "say", function, ObjSay, GameObject, GameArray),
        GameOperator(GameNothing, "globalRadio", function, ObjGlobalRadio, GameObject, GameString),
        GameOperator(GameNothing, "sideRadio", function, ObjSideRadio, GameObjectOrArray, GameString),
        GameOperator(GameNothing, "groupRadio", function, ObjGroupRadio, GameObject, GameString),
        GameOperator(GameNothing, "vehicleRadio", function, ObjVehicleRadio, GameObject, GameString),
        GameOperator(GameNothing, "globalChat", function, ObjGlobalChat, GameObject, GameString),
        GameOperator(GameNothing, "sideChat", function, ObjSideChat, GameObjectOrArray, GameString),
        GameOperator(GameNothing, "groupChat", function, ObjGroupChat, GameObject, GameString),
        GameOperator(GameNothing, "vehicleChat", function, ObjVehicleChat, GameObject, GameString),
        GameOperator(GameNothing, "playMove", function, ObjPlayMove, GameObject, GameString),
        GameOperator(GameNothing, "switchMove", function, ObjSwitchMove, GameObject, GameString),
        GameOperator(GameNothing, "setRadioMsg", function, SetRadioMessage, GameScalar, GameString),
        GameOperator(GameBool, "==", function, ObjCmpE, GameObject, GameObject),
        GameOperator(GameBool, "!=", function, ObjCmpNE, GameObject, GameObject),
        GameOperator(GameBool, "==", function, GrpCmpE, GameGroup, GameGroup),
        GameOperator(GameBool, "!=", function, GrpCmpNE, GameGroup, GameGroup),
        GameOperator(GameBool, "==", function, SideCmpE, GameSide, GameSide),
        GameOperator(GameBool, "!=", function, SideCmpNE, GameSide, GameSide),
        // EQ/NE for comparing bool. Operator "==" and "!=" are improper because their results are bool
        // and operator has priority. Use explicitly function instead
        GameOperator(GameBool, "boolEq", function, BoolCmpEq, GameBool, GameBool),
        GameOperator(GameBool, "boolNe", function, BoolCmpNE, GameBool, GameBool),
        GameOperator(GameScalar, "countEnemy", function, ListCountEnemy, GameObject, GameArray),
        GameOperator(GameScalar, "countFriendly", function, ListCountFriendly, GameObject, GameArray),
        GameOperator(GameScalar, "countUnknown", function, ListCountUnknown, GameObject, GameArray),
        GameOperator(GameScalar, "countType", function, ListCountType, GameString, GameArray),
        GameOperator(GameScalar, "countSide", function, ListCountSide, GameSide, GameArray),
        GameOperator(GameNothing, "allowGetIn", function, ListAllowGetIn, GameArray, GameBool),
        GameOperator(GameNothing, "orderGetIn", function, ListOrderGetIn, GameArray, GameBool),
        GameOperator(GameNothing, "join", function, ListJoin, GameArray, GameObjectOrGroup),
        GameOperator(GameNothing, "move", function, GrpMove, GameObjectOrGroup, GameArray),
        GameOperator(GameNothing, "setGroupid", function, GrpSetIdentity, GameObjectOrGroup, GameArray),
        GameOperator(GameNothing, "setBehaviour", function, GrpSetBehaviour, GameObjectOrGroup, GameString),
        GameOperator(GameNothing, "setBehavior", function, GrpSetBehaviour, GameObjectOrGroup,
                     GameString), // US-spelling alias
        GameOperator(GameNothing, "setCombatMode", function, GrpSetCombatMode, GameObjectOrGroup, GameString),
        GameOperator(GameNothing, "setFormation", function, GrpSetFormation, GameObjectOrGroup, GameString),
        GameOperator(GameNothing, "setSpeedMode", function, GrpSetSpeedMode, GameObjectOrGroup, GameString),
        GameOperator(GameNothing, "setUnitPos", function, ObjSetUnitPos, GameObject, GameString),
        GameOperator(GameNothing, "lockWp", function, GrpLockWP, GameObjectOrGroup, GameBool),
        GameOperator(GameNothing, "lock", function, ObjLock, GameObject, GameBool),
        GameOperator(GameNothing, "stop", function, ObjStop, GameObject, GameBool),
        GameOperator(GameNothing, "disableAI", function, ObjDisableAI, GameObject, GameString),
        GameOperator(GameNothing, "assignAsCommander", function, ObjAssignAsCommander, GameObject, GameObject),
        GameOperator(GameNothing, "assignAsDriver", function, ObjAssignAsDriver, GameObject, GameObject),
        GameOperator(GameNothing, "assignAsGunner", function, ObjAssignAsGunner, GameObject, GameObject),
        GameOperator(GameNothing, "assignAsCargo", function, ObjAssignAsCargo, GameObject, GameObject),
        GameOperator(GameNothing, "leaveVehicle", function, GrpLeaveVehicle, GameObject, GameObject),
        GameOperator(GameNothing, "leaveVehicle", function, GrpLeaveVehicle, GameGroup, GameObject),
        GameOperator(GameNothing, "moveInCommander", function, ObjMoveInCommander, GameObject, GameObject),
        GameOperator(GameNothing, "moveInDriver", function, ObjMoveInDriver, GameObject, GameObject),
        GameOperator(GameNothing, "moveInGunner", function, ObjMoveInGunner, GameObject, GameObject),
        GameOperator(GameNothing, "moveInCargo", function, ObjMoveInCargo, GameObject, GameObject),
        GameOperator(GameNothing, "allowFleeing", function, GrpAllowFleeing, GameObjectOrGroup, GameScalar),
        GameOperator(GameNothing, "objStatus", function, SetObjectiveStatus, GameString, GameString),
        GameOperator(GameNothing, "exec", function, ScriptExecute, GameVoid, GameString),
        GameOperator(GameScalar, "spawn", function, ScriptSpawn, GameVoid, GameString),
        // Arma 1 commands (see A1* above)
        GameOperator(GameScalar, "displayCtrl", function, A1DisplayCtrl, GameScalar, GameScalar),
        GameOperator(GameNothing, "displaySetEventHandler", function, A1DisplaySetEventHandler, GameScalar, GameArray),
        GameOperator(GameNothing, "ctrlShow", function, A1CtrlShow, GameScalar, GameBool),
        GameOperator(GameNothing, "ctrlEnable", function, A1CtrlEnable, GameScalar, GameBool),
        GameOperator(GameNothing, "ctrlSetText", function, A1CtrlSetText, GameScalar, GameString),
        GameOperator(GameNothing, "buttonSetAction", function, A1ButtonSetAction, GameScalar, GameString),
        GameOperator(GameNothing, "ctrlSetFade", function, A1CtrlSetFade, GameScalar, GameScalar),
        GameOperator(GameNothing, "ctrlCommit", function, A1CtrlCommit, GameScalar, GameScalar),
        GameOperator(GameNothing, "ctrlSetStructuredText", function, A1CtrlSetStructuredText, GameScalar, GameString),
        GameOperator(GameNothing, "ctrlSetPosition", function, A1CtrlSetPosition, GameScalar, GameArray),
        GameOperator(GameNothing, "ctrlSetTextColor", function, A1CtrlSetTextColor, GameScalar, GameArray),
        GameOperator(GameNothing, "ctrlSetBackgroundColor", function, A1CtrlSetBackgroundColor, GameScalar, GameArray),
        GameOperator(GameNothing, "ctrlSetForegroundColor", function, A1CtrlIgnored, GameScalar, GameArray),
        GameOperator(GameObject, "nearestObject", function, A1NearestObject, GameArray, GameScalar | GameString),
        GameOperator(GameArray, "nearObjects", function, A1NearObjects, GameArray | GameObject, GameArray | GameScalar),
        // Config paths bind looser than other binary commands: Arma 3's own functions write
        // `configFile >> "CfgWorlds" >> worldName >> "Armory" >> ["a", "b"] select _this` and
        // `... >> SIDE_COLORS param [_side, "colorUnknown"]`, the select/param first.
        GameOperator(GameString, ">>", comparison, A1ConfigChild, GameString, GameString),
        GameOperator(GameNothing, "breakOut", function, A1BreakOutWith, GameAny, GameString),
        GameOperator(GameString, "select", function, A1ConfigSelect, GameString, GameScalar),
        GameOperator(GameScalar, "min", function, A1Min, GameScalar, GameScalar),
        GameOperator(GameScalar, "max", function, A1Max, GameScalar, GameScalar),
        GameOperator(GameNothing, "lookAt", function, A1LookAt, GameObjectOrArray, GameObject | GameArray),
        GameOperator(GameNothing, "enableAttack", function, A1IgnoredBinary, GameObjectOrGroup, GameBool),
        GameOperator(GameNothing, "camPreparePos", function, CamSetPos, GameObject, GameArray),
        GameOperator(GameNothing, "camPrepareRelPos", function, CamSetRelPos, GameObject, GameArray),
        GameOperator(GameNothing, "camPrepareTarget", function, A1CamPrepareTarget, GameObject, GameObject | GameArray),
        GameOperator(GameNothing, "camPrepareFov", function, CamSetFOV, GameObject, GameScalar),
        GameOperator(GameNothing, "camPrepareFovRange", function, CamSetFOVRange, GameObject, GameArray),
        GameOperator(GameNothing, "camPrepareDir", function, CamSetDive, GameObject, GameScalar),
        GameOperator(GameNothing, "camPrepareDive", function, CamSetDive, GameObject, GameScalar),
        GameOperator(GameNothing, "camPrepareBank", function, CamSetDive, GameObject, GameScalar),
        GameOperator(GameNothing, "camCommitPrepared", function, CamCommit, GameObject, GameScalar),
        GameOperator(GameNothing, "camPreload", function, A1CamPreload, GameObject, GameScalar),
        GameOperator(GameScalar, "execVM", function, ScriptExecVM, GameVoid, GameString),
        GameOperator(GameBool, "preloadObject", function, PreloadObjectReady, GameScalar, GameObject),
        GameOperator(GameBool, "preloadObject", function, A1PreloadClass, GameScalar, GameString),

        GameOperator(GameNothing, "setOvercast", function, SetOvercast, GameScalar, GameScalar),
        GameOperator(GameNothing, "setFog", function, SetFog, GameScalar, GameScalar),
        GameOperator(GameNothing, "setRain", function, SetRain, GameScalar, GameScalar),

        GameOperator(GameNothing, "setFuel", function, ObjSetFuel, GameObject, GameScalar),
        GameOperator(GameNothing, "setFuelCargo", function, ObjSetFuelCargo, GameObject, GameScalar),
        GameOperator(GameNothing, "setRepairCargo", function, ObjSetRepairCargo, GameObject, GameScalar),
        GameOperator(GameNothing, "setAmmoCargo", function, ObjSetAmmoCargo, GameObject, GameScalar),
        GameOperator(GameNothing, "setVehicleAmmo", function, ObjSetVehicleAmmo, GameObject, GameScalar),
        GameOperator(GameNothing, "addWeaponCargo", function, ObjAddWeaponCargo, GameObject, GameArray),
        GameOperator(GameNothing, "addMagazineCargo", function, ObjAddMagazineCargo, GameObject, GameArray),

        GameOperator(GameObject, "createVehicle", function, A1VehCreate, GameString, GameArray),
        GameOperator(GameObject, "createVehicleLocal", function, A1VehCreate, GameString, GameArray),
        GameOperator(GameNothing, "joinSilent", function, ListJoin, GameArray, GameObjectOrGroup),
        GameOperator(GameNothing, "hintC", function, A1HintC, GameString, GameString | GameArray),
        GameOperator(GameNothing, "setRank", function, A1IgnoredBinary, GameObject, GameString),
        GameOperator(GameNothing, "enableAI", function, ObjEnableAI, GameObject, GameString),
        GameOperator(GameNothing, "selectLeader", function, A1SelectLeader, GameGroup, GameObject),
        GameOperator(GameArray, "modelToWorld", function, A1ModelToWorld, GameObject, GameArray),
        GameOperator(GameNothing, "addVehicle", function, A1AddVehicle, GameGroup, GameObject),
        GameOperator(GameNothing, "setCameraInterest", function, A1IgnoredBinary, GameObject, GameScalar),
        GameOperator(GameNothing, "ctrlSetEventHandler", function, A1IgnoredBinary, GameScalar, GameArray),
        GameOperator(GameNothing, "doFSM", function, A1IgnoredBinary, GameObjectOrArray, GameArray),
        // execFSM: GameStateExtA2's (it runs the FSM). An ignoring entry here was matched
        // first and every scripted FSM -- the animals' agent tasks among them -- did nothing.
        GameOperator(GameNothing, "setParticleParams", function, A1IgnoredBinary, GameObject, GameArray),
        GameOperator(GameNothing, "setParticleRandom", function, A1IgnoredBinary, GameObject, GameArray),
        GameOperator(GameNothing, "setParticleCircle", function, A1IgnoredBinary, GameObject, GameArray),
        GameOperator(GameNothing, "setDropInterval", function, A1IgnoredBinary, GameObject, GameScalar),
        GameOperator(GameNothing, "setLightColor", function, ScriptLightSetColor, GameObject, GameArray),
        GameOperator(GameNothing, "setLightAmbient", function, ScriptLightSetAmbient, GameObject, GameArray),
        GameOperator(GameNothing, "setLightBrightness", function, ScriptLightSetBrightness, GameObject, GameScalar),
        GameOperator(GameNothing, "lightAttachObject", function, ScriptLightAttach, GameObject, GameArray),
        GameOperator(GameNothing, "setLightIntensity", function, ScriptLightSetIntensity, GameObject, GameScalar),
        GameOperator(GameNothing, "setLightAttenuation", function, ScriptLightSetAttenuation, GameObject, GameArray),
        GameOperator(GameNothing, "setLightDayLight", function, ScriptLightSetDayLight, GameObject, GameBool),
        GameOperator(GameNothing, "createUnit", function, UnitCreate, GameString, GameArray),
        GameOperator(GameObject, "createUnit", function, A1UnitCreate, GameGroup, GameArray),
        GameOperator(GameNothing, "setVehicleVarName", function, A1IgnoredBinary, GameObject, GameString),
        GameOperator(GameNothing, "camSetFocus", function, A1IgnoredBinary, GameObject, GameArray),

        GameOperator(GameNothing, "fadeMusic", function, SetMusicVolume, GameScalar, GameScalar),
        GameOperator(GameNothing, "fadeSound", function, SetSoundVolume, GameScalar, GameScalar),

        GameOperator(GameNothing, "cameraEffect", function, ObjCameraEffect, GameObject, GameArray),
        GameOperator(GameObject, "camCreate", function, CamCreate, GameString, GameArray),
        GameOperator(GameNothing, "camSetPos", function, CamSetPos, GameObject, GameArray),
        GameOperator(GameNothing, "camSetRelPos", function, CamSetRelPos, GameObject, GameArray),
        GameOperator(GameNothing, "camSetFov", function, CamSetFOV, GameObject, GameScalar),
        GameOperator(GameNothing, "camSetFovRange", function, CamSetFOVRange, GameObject, GameArray),

        GameOperator(GameNothing, "camSetDive", function, CamSetDive, GameObject, GameScalar),
        GameOperator(GameNothing, "camSetBank", function, CamSetDive, GameObject, GameScalar),
        GameOperator(GameNothing, "camSetDir", function, CamSetDive, GameObject, GameScalar),
        GameOperator(GameNothing, "camCommit", function, CamCommit, GameObject, GameScalar),
        GameOperator(GameNothing, "camSetTarget", function, CamSetTargetObj, GameObject, GameObject),
        GameOperator(GameNothing, "camSetTarget", function, CamSetTargetVec, GameObject, GameArray),
        GameOperator(GameNothing, "camCommand", function, CamCommand, GameObject, GameString),

        GameOperator(GameNothing, "switchCamera", function, ObjSwitchCamera, GameObject, GameString),

        TABLE_COMMAND(Move, Move, GameArray) TABLE_COMMAND(Watch, Watch, GameArray)
            TABLE_COMMAND(Watch, Watch, GameObject) TABLE_COMMAND(Target, Target, GameObject)
                TABLE_COMMAND(Follow, Follow, GameObject) TABLE_COMMAND(Fire, Fire, GameObject)

                    GameOperator(GameNothing, "action", function, VehProcessAction, GameObject, GameArray),
        GameOperator(GameNothing | GameScalar, "addAction", function, VehAddUserAction, GameObject, GameArray),
        GameOperator(GameNothing, "removeAction", function, VehRemoveUserAction, GameObject, GameScalar),
        GameOperator(GameNothing, "reveal", function, GrpReveal, GameObjectOrGroup, GameObject),

        GameOperator(GameNothing | GameScalar, "addEventHandler", function, VehAddEventHandler, GameObject, GameArray),
        GameOperator(GameNothing, "removeEventHandler", function, VehRemoveEventHandler, GameObject, GameArray),
        GameOperator(GameNothing, "removeAllEventHandlers", function, VehRemoveAllEventHandlers, GameObject,
                     GameString),

        GameOperator(GameNothing, "engineOn", function, VehEngineOn, GameObject, GameBool),

        GameOperator(GameBool, "saveStatus", function, ObjSaveStatus, GameObject, GameString),
        GameOperator(GameBool, "loadStatus", function, ObjLoadStatus, GameObject, GameString),

        GameOperator(GameBool, "saveIdentity", function, ObjSaveIdentity, GameObject, GameString),
        GameOperator(GameBool, "loadIdentity", function, ObjLoadIdentity, GameObject, GameString),

        GameOperator(GameNothing, "animate", function, ObjAnimate, GameObject, GameArray),
        GameOperator(GameScalar, "animationPhase", function, ObjAnimationPhase, GameObject, GameString),

        GameOperator(GameNothing, "setSkill", function, ObjSetSkill, GameObject, GameScalar),

        GameOperator(GameNothing, "setObjectTexture", function, ObjSetTexture, GameObject, GameArray),
        GameOperator(GameNothing, "setObjectMaterial", function, ObjSetMaterial, GameObject, GameArray),
        GameOperator(GameNothing, "setObjectMaterialGlobal", function, ObjSetMaterial, GameObject, GameArray),

        GameOperator(GameNothing, "saveConfig", function, ConfigSave, GameFile, GameString),
        GameOperator(GameVoid, "dbGet", function, DatabaseGet, GameDatabase, GameArray),
        GameOperator(GameBool, "dbSet", function, DatabaseSet, GameDatabase, GameArray),
        GameOperator(GameBool, "dbDelete", function, DatabaseDelete, GameDatabase, GameString),
        GameOperator(GameBool, "dbExists", function, DatabaseExists, GameDatabase, GameString),
        GameOperator(GameArray, "dbKeys", function, DatabaseKeys, GameDatabase, GameString),
        GameOperator(GameBool, "dbSave", function, DatabaseSave, GameDatabase, GameString),
        GameOperator(GameScalar, "dbListPush", function, DatabaseListPush, GameDatabase, GameArray),
        GameOperator(GameScalar, "dbListPushMany", function, DatabaseListPushMany, GameDatabase, GameArray),
        GameOperator(GameVoid, "dbListPop", function, DatabaseListPop, GameDatabase, GameArray),
        GameOperator(GameVoid, "dbListGet", function, DatabaseListGet, GameDatabase, GameArray),
        GameOperator(GameBool, "dbListSet", function, DatabaseListSet, GameDatabase, GameArray),
        GameOperator(GameBool, "dbListRemove", function, DatabaseListRemove, GameDatabase, GameArray),
        GameOperator(GameScalar, "dbListSize", function, DatabaseListSize, GameDatabase, GameString),
        GameOperator(GameBool, "dbHashSet", function, DatabaseHashSet, GameDatabase, GameArray),
        GameOperator(GameBool, "dbHashSetMany", function, DatabaseHashSetMany, GameDatabase, GameArray),
        GameOperator(GameVoid, "dbHashGet", function, DatabaseHashGet, GameDatabase, GameArray),
        GameOperator(GameBool, "dbHashDelete", function, DatabaseHashDelete, GameDatabase, GameArray),
        GameOperator(GameBool, "dbHashExists", function, DatabaseHashExists, GameDatabase, GameArray),
        GameOperator(GameArray, "dbHashKeys", function, DatabaseHashKeys, GameDatabase, GameString),
        GameOperator(GameFile, "openClass", function, ClassOpen, GameFile, GameString),
        GameOperator(GameFile, "addClass", function, ClassAdd, GameFile, GameString),
        GameOperator(GameVoid, "getValue", function, ValueGet, GameFile, GameString),
        GameOperator(GameNothing, "addValue", function, ValueAdd, GameFile, GameArray),

        GameOperator(GameNothing, "setFriend", function, CenterSetFriend, GameSide, GameArray),

        GameOperator(GameNothing, "setMarkerText", function, MarkerSetText, GameString, GameString),
        GameOperator(GameNothing, "setMarkerShape", function, MarkerSetShape, GameString, GameString),
        GameOperator(GameNothing, "setMarkerBrush", function, MarkerSetBrush, GameString, GameString),
        GameOperator(GameNothing, "setMarkerDir", function, MarkerSetDir, GameString, GameScalar),

        GameOperator(GameNothing, "setTriggerArea", function, TriggerSetArea, GameObject, GameArray),
        GameOperator(GameNothing, "setTriggerActivation", function, TriggerSetActivation, GameObject, GameArray),
        GameOperator(GameNothing, "setTriggerType", function, TriggerSetType, GameObject, GameString),
        GameOperator(GameNothing, "setTriggerTimeout", function, TriggerSetTimeout, GameObject, GameArray),
        GameOperator(GameNothing, "setTriggerText", function, TriggerSetText, GameObject, GameString),
        GameOperator(GameNothing, "setTriggerStatements", function, TriggerSetStatements, GameObject, GameArray),

        GameOperator(GameNothing, "triggerAttachObject", function, TriggerAttachObject, GameObject, GameScalar),
        GameOperator(GameNothing, "triggerAttachVehicle", function, TriggerAttachVehicle, GameObject, GameArray),

        GameOperator(GameNothing, "setEffectCondition", function, EffectSetCondition, GameObject | GameArray,
                     GameString),
        GameOperator(GameNothing, "setCameraEffect", function, EffectSetCamera, GameObject | GameArray, GameArray),
        GameOperator(GameNothing, "setSoundEffect", function, EffectSetSound, GameObject | GameArray, GameArray),
        GameOperator(GameNothing, "setMusicEffect", function, EffectSetMusic, GameObject | GameArray, GameString),
        GameOperator(GameNothing, "setTitleEffect", function, EffectSetTitle, GameObject | GameArray, GameArray),
        GameOperator(GameNothing, "synchronizeWaypoint", function, WaypointSynchronize, GameArray, GameArray),
        GameOperator(GameNothing, "synchronizeWaypoint", function, TriggerSynchronize, GameObject, GameArray),

        GameOperator(GameArray, "addWaypoint", function, WaypointAdd, GameGroup, GameArray),
        GameOperator(GameNothing, "setWaypointPosition", function, WaypointSetPosition, GameArray, GameArray),
        GameOperator(GameNothing, "setWaypointType", function, WaypointSetType, GameArray, GameString),
        GameOperator(GameNothing, "waypointAttachVehicle", function, WaypointAttachVehicle, GameArray, GameObject),
        GameOperator(GameNothing, "waypointAttachObject", function, WaypointAttachObject, GameArray, GameScalar),
        GameOperator(GameNothing, "setWaypointHousePosition", function, WaypointSetHousePos, GameArray, GameScalar),
        GameOperator(GameNothing, "setWaypointCombatMode", function, WaypointSetCombatMode, GameArray, GameString),
        GameOperator(GameNothing, "setWaypointFormation", function, WaypointSetFormation, GameArray, GameString),
        GameOperator(GameNothing, "setWaypointSpeed", function, WaypointSetSpeed, GameArray, GameString),
        GameOperator(GameNothing, "setWaypointBehaviour", function, WaypointSetBehaviour, GameArray, GameString),
        GameOperator(GameNothing, "setWaypointBehavior", function, WaypointSetBehaviour, GameArray,
                     GameString), // US-spelling alias
        GameOperator(GameNothing, "setWaypointDescription", function, WaypointSetDescription, GameArray, GameString),
        GameOperator(GameNothing, "setWaypointStatements", function, WaypointSetStatements, GameArray, GameArray),
        GameOperator(GameNothing, "setWaypointScript", function, WaypointSetScript, GameArray, GameString),
        GameOperator(GameNothing, "setWaypointTimeout", function, WaypointSetTimeout, GameArray, GameArray),
        GameOperator(GameNothing, "showWaypoint", function, WaypointShow, GameArray, GameString),
        GameOperator(GameNothing, "setVectorUp", function, ObjSetVectorUp, GameObject, GameArray),
        GameOperator(GameNothing, "setVectorDir", function, ObjSetVectorDir, GameObject, GameArray),
        GameOperator(GameNothing, "setVectorDirAndUp", function, ObjSetVectorDirectionAndUp, GameObject, GameArray),

        GameOperator(GameAny, "GetAmmoParam", function, ParamAmmo, GameString, GameString),
        GameOperator(GameAny, "GetAmmoParamArray", function, ParamArrayAmmo, GameString, GameString),
        GameOperator(GameAny, "GetAmmoSubParam", function, ParamSubAmmo, GameArray, GameString),
        GameOperator(GameAny, "GetAmmoSubParamArray", function, ParamSubArrayAmmo, GameArray, GameString),
        GameOperator(GameAny, "GetWeaponParam", function, ParamWeapons, GameString, GameString),
        GameOperator(GameAny, "GetWeaponParamArray", function, ParamArrayWeapons, GameString, GameString),
        GameOperator(GameAny, "GetWeaponSubParam", function, ParamSubWeapons, GameArray, GameString),
        GameOperator(GameAny, "GetWeaponSubParamArray", function, ParamSubArrayWeapons, GameArray, GameString),
        GameOperator(GameAny, "GetVehicleParam", function, ParamVehicles, GameString, GameString),
        GameOperator(GameAny, "GetVehicleParamArray", function, ParamArrayVehicles, GameString, GameString),
        GameOperator(GameAny, "GetVehicleSubParam", function, ParamSubVehicles, GameArray, GameString),
        GameOperator(GameAny, "GetVehicleSubParamArray", function, ParamSubArrayVehicles, GameArray, GameString),
        // todo: Special entrance for UserActions/EventHandlers
        // todo: "Cfg..." can be parameter and thus support scripter read config info freely

        GameOperator(GameScalar, "ammoArray", function, ObjAmmoArray, GameObject, GameString),
        GameOperator(GameNothing, "addMagazinePrecise", function, ObjAddMagazinePrecise, GameObject, GameArray),
        GameOperator(GameScalar, "GetSelectionDammage", function, ObjGetSelectionDammage, GameObject, GameString),
        GameOperator(GameScalar, "getSelectionDamage", function, ObjGetSelectionDammage, GameObject,
                     GameString), // one-M alias
        GameOperator(GameNothing, "SetSelectionDammage", function, ObjSetSelectionDammage, GameObject, GameArray),
        GameOperator(GameNothing, "setSelectionDamage", function, ObjSetSelectionDammage, GameObject,
                     GameArray), // one-M alias
    };
    count = sizeof(ExtBinary) / sizeof(*ExtBinary);
    return ExtBinary;
}

#include <Poseidon/Foundation/Modules/Modules.hpp>

INIT_MODULE(GameStateExt, 2)
{
    // stringtable must be loaded
    GGameState.Init();

    GGameState.NewType("OBJECT", GameObject, CreateGameDataObject, LocalizeString(IDS_EVAL_TYPEOBJECT));
    GGameState.NewType("VECTOR", GameVector, nullptr, LocalizeString(IDS_EVAL_TYPEVECTOR));
    GGameState.NewType("TRANS", GameTrans, nullptr, LocalizeString(IDS_EVAL_TYPETRANS));
    GGameState.NewType("ORIENT", GameOrient, nullptr, LocalizeString(IDS_EVAL_TYPEORIENT));
    GGameState.NewType("SIDE", GameSide, CreateGameDataSide, LocalizeString(IDS_EVAL_TYPESIDE));
    GGameState.NewType("GROUP", GameGroup, CreateGameDataGroup, LocalizeString(IDS_EVAL_TYPEGROUP));
    GGameState.NewType("FILE", GameFile, CreateGameDataFile, "File");
    GGameState.NewType("DATABASE", GameDatabase, CreateGameDataDatabase, "Database");

    int extUnaryCount;
    const GameFunction* ExtUnary = GetExtUnary(extUnaryCount);
    for (int i = 0; i < extUnaryCount; i++)
    {
        GGameState.NewFunction(ExtUnary[i]);
    }

    int extBinaryCount;
    const GameOperator* ExtBinary = GetExtBinary(extBinaryCount);
    for (int i = 0; i < extBinaryCount; i++)
    {
        GGameState.NewOperator(ExtBinary[i]);
    }

    int extNularCount;
    const GameNular* ExtNular = GetExtNular(extNularCount);
    for (int i = 0; i < extNularCount; i++)
    {
        GGameState.NewNularOp(ExtNular[i]);
    }
};

// Out-of-line: these touch OLink<AIGroup>/<Object>, needing the complete types.
GameDataObject::GameDataObject() : _value(0) {}
GameDataObject::GameDataObject(GameObjectType value) : _value(value) {}
GameObjectType GameDataObject::GetObject() const
{
    return _value;
}
GameData* GameDataObject::Clone() const
{
    return new GameDataObject(*this);
}
GameDataGroup::GameDataGroup() : _value(0) {}
GameDataGroup::GameDataGroup(GameGroupType value) : _value(value) {}
GameGroupType GameDataGroup::GetGroup() const
{
    return _value;
}
GameData* GameDataGroup::Clone() const
{
    return new GameDataGroup(*this);
}
GameValueExt::GameValueExt(AIGroup* value)
{
    _data = new GameDataGroup(value);
}
GameValueExt::GameValueExt(Object* value)
{
    _data = new GameDataObject(value);
}
