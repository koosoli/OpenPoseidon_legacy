#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/Path/AIDefs.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/Core/Version.hpp>

#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/Audio/DynSound.hpp>
#include <Poseidon/World/Detection/Detector.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/World/Scene/Camera/CamEffects.hpp>

#include <Poseidon/UI/Locale/Languages.hpp>

#include <Poseidon/Foundation/Algorithms/Qsort.hpp>

#include <Poseidon/AI/ArcadeTemplate.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>

#include <Poseidon/Game/Commands/GameStateExt.hpp>

#include <Poseidon/Network/Network.hpp>
#include <Poseidon/Game/Chat.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Foundation/Platform/GamePaths.hpp>

#include <Poseidon/Foundation/Enums/EnumNames.hpp>

#include <Poseidon/Game/UiActions.hpp>

#include <Poseidon/Foundation/Strings/Mbcs.hpp>
#include <Poseidon/Foundation/Strings/Bstring.hpp>
#include <Poseidon/Foundation/Strings/StrFormat.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <float.h>
#include <string.h>
#include <string>
#include <utility>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Memtype.h>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>

using namespace Poseidon;
extern SRef<EntityAI> GDummyVehicle;

int PlayersNumber(TargetSide side);

namespace Poseidon
{
void PositionToAA11(Vector3Val pos, char* buffer);
}

using Poseidon::Foundation::EnumName;
using Poseidon::Foundation::QSort;
using Poseidon::Foundation::Time;

// no of updates in AICenter::Think
#define FieldsPerCycle 10
#define GroupsPerCycle 1

#define TargetsPerCycle 8

#define EXPOSURE_COEF 50.0F
#define INV_EXPOSURE_COEF (1.0F / EXPOSURE_COEF)

const float AccuracyLast = 7200;   // how long we can remmember the information
const float ExposureTimeout = 450; // how long exposure persist when target is not visible

bool IsEnemy(const AICenter* center, TargetSide side)
{
    return center->IsEnemy(side);
}

bool IsUnknown(const AICenter* center, TargetSide side)
{
    return side == TSideUnknown;
}

#define LOG_TARGETS 0

union AITimeRelative
{
  private:
    WORD _packed;
    struct
    {
        signed _mantisa : 7;    // singed mantisa,r range -0x3f, +0x3f
        unsigned _exponent : 3; // binary exponent divided by 3
        // max. represented value is (0x3f)<<(7*3) = 132120576
        // max. valid AITime value is 365*24*60*60 = 31536000
        unsigned _owner : 6;
    };

  public:
    AITimeRelative() : _mantisa(0), _exponent(0), _owner(0x3f) {}
    explicit AITimeRelative(AITime time, int owner = 0);
    operator AITime() const { return _mantisa << (_exponent * 4); }

    int GetOwner() const { return _owner; }
    void SetOwner(int owner) { _owner = owner; }
};

AITimeRelative::AITimeRelative(AITime delta, int owner)
{
    _owner = owner;
    // delta can be up to 25 bits (secPerYear<2^25)
    bool negative = delta < 0;
    if (negative)
    {
        delta = -delta;
    }
    int exponent = 0;
    while (delta >= 0x40)
    {
        delta >>= 4, exponent++;
    }
    AI_ERROR(exponent < 8);
    AI_ERROR(delta < 0x40);
    if (negative)
    {
        delta = -delta;
    }
    _exponent = exponent;
    _mantisa = delta;
}

// Lazy initialization to avoid Static Initialization Order Fiasco (SIOF)
// The array is initialized on first use (after WinMain), not during static init
template <>
const EnumName* Poseidon::Foundation::GetEnumNames(AI::Semaphore dummy)
{
    static const EnumName SemaphoreNames[] = {EnumName(-1, "NO CHANGE"),
                                              EnumName(AI::SemaphoreBlue, "BLUE"),
                                              EnumName(AI::SemaphoreGreen, "GREEN"),
                                              EnumName(AI::SemaphoreWhite, "WHITE"),
                                              EnumName(AI::SemaphoreYellow, "YELLOW"),
                                              EnumName(AI::SemaphoreRed, "RED"),
                                              EnumName()};
    return SemaphoreNames;
}

template <>
const EnumName* Poseidon::Foundation::GetEnumNames(AI::Formation dummy)
{
    static const EnumName FormationNames[] = {EnumName(-1, "NO CHANGE"),
                                              EnumName(AI::FormColumn, "COLUMN"),
                                              EnumName(AI::FormStaggeredColumn, "STAG COLUMN"),
                                              EnumName(AI::FormWedge, "WEDGE"),
                                              EnumName(AI::FormEcholonLeft, "ECH LEFT"),
                                              EnumName(AI::FormEcholonRight, "ECH RIGHT"),
                                              EnumName(AI::FormVee, "VEE"),
                                              EnumName(AI::FormLine, "LINE"),
                                              // Arma 2 formations read as their nearest CWA formation
                                              EnumName(AI::FormWedge, "DIAMOND"),
                                              EnumName(AI::FormColumn, "FILE"),
                                              EnumName()};
    return FormationNames;
}

template <>
const EnumName* Poseidon::Foundation::GetEnumNames(Rank dummy)
{
    static const EnumName RankNames[] = {
        EnumName(RankUndefined, "UNDEFINED"), EnumName(RankPrivate, "PRIVATE"),     EnumName(RankCorporal, "CORPORAL"),
        EnumName(RankSergeant, "SERGEANT"),   EnumName(RankLieutnant, "LIEUTNANT"), EnumName(RankCaptain, "CAPTAIN"),
        EnumName(RankMajor, "MAJOR"),         EnumName(RankColonel, "COLONEL"),
        // Arma 1/2 missions spell it out; read only (a value is written with its first name).
        EnumName(RankLieutnant, "LIEUTENANT"), EnumName()};
    return RankNames;
}

AITime AI::GetActualTime()
{
    float time = Glob.clock.GetTimeInYear();
    // convert in-day time and in-year time to single int represetation
    const float secPerYear = 365 * 24 * 60 * 60;
    return toIntFloor(secPerYear * time);
}

float AI::ExpForRank(Rank rank, bool ingame)
{
    if (rank < RankPrivate)
    {
        return 0;
    }

    if (rank > RankColonel)
    {
        return FLT_MAX;
    }

    if (ingame)
    {
        return 0.8 * ExperienceTable[rank];
    }

    return ExperienceTable[rank];
}

Rank AI::RankFromExp(float exp, bool ingame)
{
    int i;
    for (i = RankPrivate + 1; i <= RankColonel; i++)
    {
        if (exp < ExpForRank((Rank)i, ingame))
        {
            return (Rank)(i - 1);
        }
    }
    return RankColonel;
}

namespace Poseidon
{
AutoArray<float> ExperienceTable;
AutoArray<ExperienceDestroyInfo> ExperienceDestroyTable;

float ExperienceDestroyEnemy;
float ExperienceDestroyFriendly;
float ExperienceDestroyCivilian;

float ExperienceKilled;

float ExperienceRenegadeLimit;

// for subordinate soldier only
float ExperienceCommandCompleted;
float ExperienceCommandFailed;
float ExperienceFollowMe;

// for leadership only
float ExperienceDestroyYourUnit;
float ExperienceMissionCompleted;
float ExperienceMissionFailed;
} // namespace Poseidon

float Poseidon::ExperienceForDestroyedCost(float cost)
{
    int n = ExperienceDestroyTable.Size();
    if (n <= 0)
    {
        return 0;
    }
    // Costs above the last sized bucket fall into the final (catch-all) entry;
    // otherwise return the first bucket whose maxCost the cost fits under. The
    // outer test is against maxCost[n-2] (not n-1) so a cost in the band between
    // the last two buckets is covered, not left at 0.
    if (n < 2 || cost > ExperienceDestroyTable[n - 2].maxCost)
    {
        return ExperienceDestroyTable[n - 1].exp;
    }
    for (int i = 0; i < n - 1; i++)
    {
        if (cost <= ExperienceDestroyTable[i].maxCost)
        {
            return ExperienceDestroyTable[i].exp;
        }
    }
    return ExperienceDestroyTable[n - 1].exp;
}

void AI::InitTables()
{
    const ParamEntry& cls = Pars >> "CfgExperience";

    int n = (cls >> "ranks").GetSize();
    AI_ERROR(n == NRanks);

    ExperienceTable.Resize(n);
    for (int i = 0; i < n; i++)
    {
        ExperienceTable[i] = (cls >> "ranks")[i];
    }

    n = (cls >> "destroyUnit").GetSize();
    ExperienceDestroyTable.Resize(n);
    for (int i = 0; i < n; i++)
    {
        RString name = (cls >> "destroyUnit")[i];
        ExperienceDestroyTable[i].maxCost = (cls >> name)[0];
        ExperienceDestroyTable[i].exp = (cls >> name)[1];
    }

    ExperienceDestroyEnemy = cls >> "destroyEnemy";
    ExperienceDestroyFriendly = cls >> "destroyFriendly";
    ExperienceDestroyCivilian = cls >> "destroyCivilian";

    ExperienceRenegadeLimit = cls >> "renegadeLimit";

    ExperienceKilled = cls >> "playerKilled";

    // for subordinate soldier only
    ExperienceCommandCompleted = cls >> "commandCompleted";
    ExperienceCommandFailed = cls >> "commandFailed";
    ExperienceFollowMe = cls >> "followMe";

    // for leadership only
    ExperienceDestroyYourUnit = cls >> "destroyYourUnit";
    ExperienceMissionCompleted = cls >> "missionCompleted";
    ExperienceMissionFailed = cls >> "missionFailed";
}

template <>
const EnumName* Poseidon::Foundation::GetEnumNames(AIStatsEventType dummy)
{
    static const EnumName EventTypeNames[] = {EnumName(SETUnitLost, "LOST"),
                                              EnumName(SETKillsEnemyInfantry, "ENEMY INF"),
                                              EnumName(SETKillsEnemySoft, "ENEMY SOFT"),
                                              EnumName(SETKillsEnemyArmor, "ENEMY ARMOR"),
                                              EnumName(SETKillsEnemyAir, "ENEMY AIR"),
                                              EnumName(SETKillsFriendlyInfantry, "FRIEND INF"),
                                              EnumName(SETKillsFriendlySoft, "FRIEND SOFT"),
                                              EnumName(SETKillsFriendlyArmor, "FRIEND ARMOR"),
                                              EnumName(SETKillsFriendlyAir, "FRIEND AIR"),
                                              EnumName(SETKillsCivilInfantry, "CIVIL INF"),
                                              EnumName(SETKillsCivilSoft, "CIVIL SOFT"),
                                              EnumName(SETKillsCivilArmor, "CIVIL ARMOR"),
                                              EnumName(SETKillsCivilAir, "CIVIL AIR"),
                                              EnumName(SETUser, "USER"),
                                              EnumName()};
    return EventTypeNames;
}

template <>
const EnumName* Poseidon::Foundation::GetEnumNames(AIStatsKills dummy)
{
    static const EnumName StatsKillsNames[] = {EnumName(SKEnemyInfantry, "ENEMY INF"),
                                               EnumName(SKEnemySoft, "ENEMY SOFT"),
                                               EnumName(SKEnemyArmor, "ENEMY ARMOR"),
                                               EnumName(SKEnemyAir, "ENEMY AIR"),
                                               EnumName(SKFriendlyInfantry, "FRIEND INF"),
                                               EnumName(SKFriendlySoft, "FRIEND SOFT"),
                                               EnumName(SKFriendlyArmor, "FRIEND ARMOR"),
                                               EnumName(SKFriendlyAir, "FRIEND AIR"),
                                               EnumName(SKCivilianInfantry, "CIVIL INF"),
                                               EnumName(SKCivilianSoft, "CIVIL SOFT"),
                                               EnumName(SKCivilianArmor, "CIVIL ARMOR"),
                                               EnumName(SKCivilianAir, "CIVIL AIR"),
                                               EnumName()};
    return StatsKillsNames;
}

AIStatsMPRow::AIStatsMPRow()
{
    order = 0;
    side = TSideUnknown;
    killsInfantry = 0;
    killsSoft = 0;
    killsArmor = 0;
    killsAir = 0;
    killsPlayers = 0;
    customScore = 0;
    killsTotal = 0;
    killed = 0;
}

void AIStatsMPRow::RecalculateTotal()
{
    killsTotal = killsInfantry + 2 * killsSoft + 3 * killsArmor + 5 * killsAir + customScore;
}

AIStatsMission::AIStatsMission()
{
    _size = 0;
    _reportWritten = false;
}

void AIStatsMission::Init()
{
    const ParamEntry& cls = Pars >> "CfgInGameUI" >> "MPTable";
    _color = GetPackedColor(cls >> "color");
    _colorBg = GetPackedColor(cls >> "colorBg");
    _colorSelected = GetPackedColor(cls >> "colorSelected");
    _colorWest = GetPackedColor(cls >> "colorWest");
    _colorEast = GetPackedColor(cls >> "colorEast");
    _colorCiv = GetPackedColor(cls >> "colorCiv");
    _colorRes = GetPackedColor(cls >> "colorRes");
}

const int maxMPTableRows = 10;

void AIStatsMission::Clear()
{
    _events.Clear();
    _tableMP.Clear();
    _reportHeader.Clear();
    _reportEvents.Clear();
    _reportFooter.Clear();
    _lives = -1;
    _start = Glob.time;
    _reportWritten = false;

// caution: GNetworkManager must be initialized
#if 1
    const AutoArray<PlayerIdentity>* identities = GetNetworkManager().GetIdentities();
    if (identities)
    {
        int n = identities->Size();
        if (n > 0)
        {
            _tableMP.Resize(n);
            for (int i = 0; i < n; i++)
            {
                _tableMP[i].player = identities->Get(i).name;
                _tableMP[i].order = i + 1;
                int dpnid = identities->Get(i).dpnid;
                for (int j = 0; j < GetNetworkManager().NPlayerRoles(); j++)
                {
                    const PlayerRole* info = GetNetworkManager().GetPlayerRole(j);
                    if (info && info->player == dpnid)
                    {
                        _tableMP[i].side = info->side;
                        break;
                    }
                }
            }
            // ensure local player statistics are visible
            int index = -1;
            for (int i = 0; i < _tableMP.Size(); i++)
            {
                if (_tableMP[i].player == GetLocalPlayerName())
                {
                    index = i;
                    break;
                }
            }
            if (index >= maxMPTableRows)
            {
                swap(_tableMP[index], _tableMP[maxMPTableRows - 1]);
            }
        }
    }
#endif
}

void AIStatsMission::AddEvent(AIStatsEventType type, Vector3Par position, RString message,
                              const VehicleType* killedType, RString killedName, bool killedPlayer,
                              const VehicleType* killerType, RString killerName, bool killerPlayer)
{
    int index = _events.Add();
    _events[index].type = type;
    _events[index].time = Glob.clock.GetTimeInYear();
    _events[index].position = position;
    _events[index].message = message;

    _events[index].killedType = killedType;
    _events[index].killedName = killedName;
    _events[index].killedPlayer = killedPlayer;
    _events[index].killerType = killerType;
    _events[index].killerName = killerName;
    _events[index].killerPlayer = killerPlayer;
}

Person* GetPlayerPerson(EntityAI* vehicle)
{
    if (!vehicle)
    {
        return nullptr;
    }
    Person* person = dyn_cast<Person>(vehicle);
    if (person)
    {
        return person;
    }
    Transport* trans = dyn_cast<Transport>(vehicle);
    if (!trans)
    {
        return nullptr; // may be Thing or Building
    }

    // check if player is inside
    Person* commander = trans->Commander();
    if (commander && commander->IsNetworkPlayer())
    {
        return commander;
    }
    Person* gunner = trans->Gunner();
    if (gunner && gunner->IsNetworkPlayer())
    {
        return gunner;
    }
    Person* driver = trans->Driver();
    if (driver && driver->IsNetworkPlayer())
    {
        return driver;
    }

    // check if anybody is inside
    if (commander)
    {
        return commander;
    }
    if (gunner)
    {
        return gunner;
    }
    return driver;
}

void AIStatsMission::RecordReportKill(EntityAI* killed, EntityAI* killer)
{
    AI_ERROR(killed);
    AI_ERROR(killer);

    Person* killedPerson = GetPlayerPerson(killed);
    if (!killedPerson)
        return;
    Person* killerPerson = GetPlayerPerson(killer);
    if (!killerPerson)
        return;

    int index = _reportEvents.Add();
    MPReportEvent& event = _reportEvents[index];

    event.type = MPRETKill;
    event.t = Glob.time;

    // killed
    event.unit1.name = killedPerson->GetInfo()._name;
    event.unit1.type = killed->GetType()->GetName();
    event.unit1.position = killed->Position();
    event.unit1.side = killedPerson->Vehicle::GetTargetSide();
    event.unit1.player = killedPerson->IsNetworkPlayer();

    // killer
    event.unit2.name = killerPerson->GetInfo()._name;
    event.unit2.type = killer->GetType()->GetName();
    event.unit2.position = killer->Position();
    event.unit2.side = killerPerson->Vehicle::GetTargetSide();
    event.unit2.player = killerPerson->IsNetworkPlayer();
}

void AIStatsMission::RecordReportInjury(EntityAI* injured, EntityAI* killer, float damage, RString ammo)
{
    AI_ERROR(injured);
    Person* person = GetPlayerPerson(injured);
    if (!person)
        return;
    Person* killerPerson = GetPlayerPerson(killer);
    if (!killerPerson)
        return;

    int index = _reportEvents.Add();
    MPReportEvent& event = _reportEvents[index];

    event.type = MPRETInjury;
    event.t = Glob.time;

    // injured
    event.unit1.name = person->GetInfo()._name;
    event.unit1.type = injured->GetType()->GetName();
    event.unit1.position = injured->Position();
    event.unit1.side = person->Vehicle::GetTargetSide();
    event.unit1.player = person->IsNetworkPlayer();

    // killer
    event.unit2.name = killerPerson->GetInfo()._name;
    event.unit2.type = killer->GetType()->GetName();
    event.unit2.position = killer->Position();
    event.unit2.side = killerPerson->Vehicle::GetTargetSide();
    event.unit2.player = killerPerson->IsNetworkPlayer();

    event.value = damage;
    event.text = ammo;
}

void AIStatsMission::AddReportHeader(RString text)
{
    _reportHeader.Add(text);
}

void AIStatsMission::AddReportEvent(RString text)
{
    int index = _reportEvents.Add();
    MPReportEvent& event = _reportEvents[index];

    event.type = MPRETUser;
    event.t = Glob.time;
    event.text = text;
}

void AIStatsMission::AddReportFooter(RString text)
{
    _reportFooter.Add(text);
}

static void ReportOutput(FILE* report, const char* text)
{
    fprintf(report, "%s\n", text);
}

static std::tm LocalTimeSafe(time_t value)
{
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &value);
#else
    localtime_r(&value, &result);
#endif
    return result;
}

namespace Poseidon
{
std::string BuildMPReportPathForUserDir(const std::string& userDir, const std::tm& tm)
{
    char timestamp[32];
    std::snprintf(timestamp, sizeof(timestamp), "%04d%02d%02d_%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    const bool hasTrailingSeparator = !userDir.empty() && (userDir.back() == '/' || userDir.back() == '\\');
    return userDir + (hasTrailingSeparator ? "" : "/") + "mpreport_" + timestamp + ".txt";
}
} // namespace Poseidon

static std::string BuildMPReportPath()
{
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm = LocalTimeSafe(tt);
    return Poseidon::BuildMPReportPathForUserDir(GamePaths::Instance().UserDir(), tm);
}

void AIStatsMission::WriteMPReport()
{
    if (_reportWritten)
        return;

    const std::string reportPath = BuildMPReportPath();
    FILE* report = fopen(reportPath.c_str(), "wt");
    if (!report)
        return;

    BString<512> buffer;

    // Header
    ReportOutput(report, "----------**Start**----------");
    sprintf(buffer, "Mission: %s.%s", (const char*)Glob.header.filenameReal, (const char*)Glob.header.worldname);
    ReportOutput(report, buffer);

    RString sides, players;
    for (int i = 0; i < TSideUnknown; i++)
    {
        int count = PlayersNumber((TargetSide)i);
        if (count == 0)
            continue;

        if (sides.GetLength() > 0)
            sides = sides + RString("/");
        if (players.GetLength() > 0)
            players = players + RString("/");

        switch (i)
        {
            case TEast:
                sides = sides + LocalizeString(IDS_EAST);
                break;
            case TWest:
                sides = sides + LocalizeString(IDS_WEST);
                break;
            case TGuerrila:
                sides = sides + LocalizeString(IDS_GUERRILA);
                break;
            case TCivilian:
                sides = sides + LocalizeString(IDS_CIVILIAN);
                break;
        }

        players = players + FormatNumber(count);
    }
    sprintf(buffer, "Players: %s (%s)", (const char*)sides, (const char*)players);
    ReportOutput(report, buffer);

    std::tm time = LocalTimeSafe(_time);
    sprintf(buffer, "Start mission: %d/%d/%d, %d:%02d:%02d", time.tm_year + 1900, time.tm_mon + 1, time.tm_mday,
            time.tm_hour, time.tm_min, time.tm_sec);
    ReportOutput(report, buffer);

    for (int i = 0; i < _reportHeader.Size(); i++)
        ReportOutput(report, _reportHeader[i]);

    // Events
    ReportOutput(report, "----------**Events**----------");
    int totalKills[TSideUnknown], totalKilled[TSideUnknown];
    for (int i = 0; i < TSideUnknown; i++)
    {
        totalKills[i] = 0;
        totalKilled[i] = 0;
    }
    for (int i = 0; i < _reportEvents.Size(); i++)
    {
        MPReportEvent& event = _reportEvents[i];

        int time = toInt(event.t.toFloat());
        int h = time / 3600;
        time -= 3600 * h;
        int m = time / 60;
        time -= 60 * m;
        int s = time;

        switch (event.type)
        {
            case MPRETKill:
            {
                sprintf(buffer, "%2d:%02d:%02d : %s%s killed by %s%s at [%.0f, %.0f]", h, m, s,
                        (const char*)event.unit1.name, event.unit1.player ? "" : " (AI)", (const char*)event.unit2.name,
                        event.unit2.player ? "" : " (AI)", event.unit1.position.X(), event.unit1.position.Z());
                ReportOutput(report, buffer);
                totalKills[event.unit2.side]++;
                totalKilled[event.unit1.side]++;
            }
            break;
            case MPRETInjury:
            {
                BString<256> ammo;
                if (event.text.GetLength() > 0)
                    sprintf(ammo, ", ammo %s", (const char*)event.text);
                sprintf(buffer, "%2d:%02d:%02d : %s%s injured by %s%s at [%.0f, %.0f], damage %.0f%%%s", h, m, s,
                        (const char*)event.unit1.name, event.unit1.player ? "" : " (AI)", (const char*)event.unit2.name,
                        event.unit2.player ? "" : " (AI)", event.unit1.position.X(), event.unit1.position.Z(),
                        100.0f * event.value, (const char*)ammo);
                ReportOutput(report, buffer);
            }
            break;
            case MPRETUser:
            {
                sprintf(buffer, "%2d:%02d:%02d : %s", h, m, s, (const char*)event.text);
                ReportOutput(report, buffer);
            }
            break;
        }
    }

    // Footer
    ReportOutput(report, "----------**Summary**----------");
    RString kills, killed;
    for (int i = 0; i < TSideUnknown; i++)
    {
        int count = PlayersNumber((TargetSide)i);
        if (count == 0)
            continue;

        if (kills.GetLength() > 0)
            kills = kills + RString("/");
        if (killed.GetLength() > 0)
            killed = killed + RString("/");

        kills = kills + FormatNumber(totalKills[i]);
        killed = killed + FormatNumber(totalKilled[i]);
    }
    sprintf(buffer, "Total amount of deaths: %s (%s)", (const char*)sides, (const char*)killed);
    ReportOutput(report, buffer);
    sprintf(buffer, "Total amount of kills: %s (%s)", (const char*)sides, (const char*)kills);
    ReportOutput(report, buffer);
    for (int i = 0; i < _reportFooter.Size(); i++)
        ReportOutput(report, _reportFooter[i]);

    ReportOutput(report, "----------**End**----------");
    ReportOutput(report, "");

    fclose(report);
    _reportWritten = true;
}

void AIStatsMission::OnMissionStart()
{
    time(&_time);
    _reportWritten = false;
}

int CmpMPTableRows(const AIStatsMPRow* row1, const AIStatsMPRow* row2)
{
    int value = row2->killsTotal - row1->killsTotal;
    if (value != 0)
    {
        return value;
    }
    value = row1->killed - row2->killed;
    if (value != 0)
    {
        return value;
    }
    return stricmp(row1->player, row2->player);
}

void AIStatsMission::AddMPKill(RString playerName, TargetSide playerSide, const VehicleType* killedType,
                               bool killedPlayer, bool isEnemy)
{
    // 1. find row in table
    int index = -1;
    for (int i = 0; i < _tableMP.Size(); i++)
    {
        if (strcmp(_tableMP[i].player, playerName) == 0)
        {
            index = i;
            break;
        }
    }
    if (index < 0)
    {
        index = _tableMP.Add();
        _tableMP[index].player = playerName;
        _tableMP[index].side = playerSide;
    }
    AIStatsMPRow& row = _tableMP[index];

    if (killedType)
    {
        // 2. add killed vehicle
        if (killedType->IsKindOf(GWorld->Preloaded(VTypeStatic)))
        {
            return;
        }
        if (killedType->IsKindOf(GWorld->Preloaded(VTypeMan)))
        {
            if (isEnemy)
            {
                row.killsInfantry++;
            }
            else
            {
                row.killsInfantry--;
            }
        }
        else
        {
            switch (killedType->GetKind())
            {
                case VAir:
                    if (isEnemy)
                    {
                        row.killsAir++;
                    }
                    else
                    {
                        row.killsAir--;
                    }
                    break;
                case VArmor:
                    if (isEnemy)
                    {
                        row.killsArmor++;
                    }
                    else
                    {
                        row.killsArmor--;
                    }
                    break;
                case VSoft:
                default:
                    if (isEnemy)
                    {
                        row.killsSoft++;
                    }
                    else
                    {
                        row.killsSoft--;
                    }
                    break;
            }
        }

        if (killedPlayer)
        {
            if (isEnemy)
            {
                row.killsPlayers++;
            }
            else
            {
                row.killsPlayers--;
            }
        }

        row.RecalculateTotal();

        // 3. sort
        QSort(_tableMP.Data(), _tableMP.Size(), CmpMPTableRows);
        for (int i = 0; i < _tableMP.Size(); i++)
        {
            _tableMP[i].order = i + 1;
        }

        // 4. ensure local player statistics are visible
        index = -1;
        for (int i = 0; i < _tableMP.Size(); i++)
        {
            if (_tableMP[i].player == GetLocalPlayerName())
            {
                index = i;
                break;
            }
        }
        if (index >= maxMPTableRows)
        {
            swap(_tableMP[index], _tableMP[maxMPTableRows - 1]);
        }
    }
    else
    {
        // 2b. player killed
        row.killed++;
    }
}

void AIStatsMission::AddMPScore(RString playerName, TargetSide playerSide, int score)
{
    // 1. find row in table
    int index = -1;
    for (int i = 0; i < _tableMP.Size(); i++)
    {
        if (strcmp(_tableMP[i].player, playerName) == 0)
        {
            index = i;
            break;
        }
    }
    if (index < 0)
    {
        const MissionHeader* header = GetNetworkManager().GetMissionHeader();
        if (header && header->aiKills)
        {
            index = _tableMP.Add();
            _tableMP[index].player = playerName;
            _tableMP[index].side = playerSide;
        }
        else
        {
            return;
        }
    }
    AIStatsMPRow& row = _tableMP[index];

    // 2. add score
    row.customScore += score;
    row.RecalculateTotal();

    // 3. sort
    QSort(_tableMP.Data(), _tableMP.Size(), CmpMPTableRows);
    for (int i = 0; i < _tableMP.Size(); i++)
    {
        _tableMP[i].order = i + 1;
    }

    // 4. ensure local player statistics are visible
    index = -1;
    for (int i = 0; i < _tableMP.Size(); i++)
    {
        if (_tableMP[i].player == GetLocalPlayerName())
        {
            index = i;
            break;
        }
    }
    if (index >= maxMPTableRows)
    {
        swap(_tableMP[index], _tableMP[maxMPTableRows - 1]);
    }
}

#define CX(x) (toInt((x) * w) + 0.5)
#define CY(y) (toInt((y) * h) + 0.5)

void AIStatsMission::DrawMPTable(float alpha)
{
    bool isEast = false, isWest = false, isRes = false, isCiv = false;
    AIStatsMPRow rowEast, rowWest, rowRes, rowCiv;
    rowEast.side = TEast;
    rowWest.side = TWest;
    rowRes.side = TGuerrila;
    rowCiv.side = TCivilian;
    for (int i = 0; i < _tableMP.Size(); i++)
    {
        AIStatsMPRow& row = _tableMP[i];
        switch (row.side)
        {
            case TEast:
                isEast = true;
                rowEast.player = LocalizeString(IDS_EAST);
                rowEast.killsInfantry += row.killsInfantry;
                rowEast.killsSoft += row.killsSoft;
                rowEast.killsArmor += row.killsArmor;
                rowEast.killsAir += row.killsAir;
                rowEast.killsPlayers += row.killsPlayers;
                rowEast.killsTotal += row.killsTotal;
                rowEast.killed += row.killed;
                break;
            case TWest:
                isWest = true;
                rowWest.player = LocalizeString(IDS_WEST);
                rowWest.killsInfantry += row.killsInfantry;
                rowWest.killsSoft += row.killsSoft;
                rowWest.killsArmor += row.killsArmor;
                rowWest.killsAir += row.killsAir;
                rowWest.killsPlayers += row.killsPlayers;
                rowWest.killsTotal += row.killsTotal;
                rowWest.killed += row.killed;
                break;
            case TGuerrila:
                isRes = true;
                rowRes.player = LocalizeString(IDS_GUERRILA);
                rowRes.killsInfantry += row.killsInfantry;
                rowRes.killsSoft += row.killsSoft;
                rowRes.killsArmor += row.killsArmor;
                rowRes.killsAir += row.killsAir;
                rowRes.killsPlayers += row.killsPlayers;
                rowRes.killsTotal += row.killsTotal;
                rowRes.killed += row.killed;
                break;
            case TCivilian:
                isCiv = true;
                rowCiv.player = LocalizeString(IDS_CIVILIAN);
                rowCiv.killsInfantry += row.killsInfantry;
                rowCiv.killsSoft += row.killsSoft;
                rowCiv.killsArmor += row.killsArmor;
                rowCiv.killsAir += row.killsAir;
                rowCiv.killsPlayers += row.killsPlayers;
                rowCiv.killsTotal += row.killsTotal;
                rowCiv.killed += row.killed;
                break;
        }
    }
    int nSides = 0;
    if (isEast)
    {
        nSides++;
    }
    if (isWest)
    {
        nSides++;
    }
    if (isRes)
    {
        nSides++;
    }
    if (isCiv)
    {
        nSides++;
    }

    static Ref<Font> font;
    if (!font)
    {
        font = GEngine->LoadFont(GetFontID("tahomaB48"));
        _size = 0.032;
    }

    // GStats.Init() loads these colours at startup, before the resource config that defines
    // CfgInGameUI/MPTable is parsed — so they come back transparent (alpha 0) and the whole
    // table draws invisibly. Re-load them lazily here, once the config is available.
    if (_color.A8() == 0)
    {
        Init();
    }

    const float xb = 0.05;
    const float xe = 0.95;
    const float yb = 0.15;
    const float ye = 0.85;
    const float rowHeight = (ye - yb) / (maxMPTableRows + 5.5);
    const int columns = 9;
    const float colWidths[columns] = {
        0.05, // order
        0.15, // player
        0.10, // killsInfantry
        0.10, // killsSoft
        0.10, // killsArmor
        0.10, // killsAir
        0.10, // killsPlayers
        0.10, // killed
        0.10, // killsTotal
    }; // total (xe - xb) == 0.9
    const bool doubleLines[columns] = {
        false, // order
        true,  // player
        false, // killsInfantry
        false, // killsSoft
        false, // killsArmor
        true,  // killsAir
        true,  // killsPlayers
        true,  // killed
        false, // killsTotal
    };

    PackedColor color(PackedColorRGB(_color, toIntFloor(_color.A8() * alpha)));
    PackedColor bgColor(PackedColorRGB(_colorBg, toIntFloor(_colorBg.A8() * alpha)));
    PackedColor colorSelected(PackedColorRGB(_colorSelected, toIntFloor(_colorSelected.A8() * alpha)));
    PackedColor colorWest(PackedColorRGB(_colorWest, toIntFloor(_colorWest.A8() * alpha)));
    PackedColor colorEast(PackedColorRGB(_colorEast, toIntFloor(_colorEast.A8() * alpha)));
    PackedColor colorRes(PackedColorRGB(_colorRes, toIntFloor(_colorRes.A8() * alpha)));
    PackedColor colorCiv(PackedColorRGB(_colorCiv, toIntFloor(_colorCiv.A8() * alpha)));

    const float w = GEngine->Width2D();
    const float h = GEngine->Height2D();

    const float cxb = CX(xb);
    const float cxe = CX(xe);
    const float cyb = CY(yb);

    float fontHeight = _size;

    int nRows = _tableMP.Size();
    saturateMin(nRows, maxMPTableRows);

    // background
    MipInfo mip = GEngine->TextBank()->UseMipmap(nullptr, 0, 0);
    GEngine->Draw2D(mip, bgColor, Rect2DPixel(cxb, cyb, cxe - cxb, CY(yb + (nRows + 1) * rowHeight) - cyb));

    // lines
    float bottom = yb;
    float cbottom = CY(bottom);
    GEngine->DrawLine(Line2DPixel(cxb, cbottom, cxe, cbottom), color, color);
    for (int i = 0; i < nRows + 1; i++)
    {
        bottom += rowHeight;
        cbottom = CY(bottom);
        GEngine->DrawLine(Line2DPixel(cxb, cbottom, cxe, cbottom), color, color);
    }
    float left = xb;
    float cleft = cxb;
    GEngine->DrawLine(Line2DPixel(cleft, cyb, cleft, cbottom), color, color);
    for (int i = 0; i < columns; i++)
    {
        left += colWidths[i];
        cleft = CX(left);
        if (doubleLines[i])
        {
            GEngine->DrawLine(Line2DPixel(cleft - 1, cyb, cleft - 1, cbottom), color, color);
            GEngine->DrawLine(Line2DPixel(cleft + 1, cyb, cleft + 1, cbottom), color, color);
        }
        else
        {
            GEngine->DrawLine(Line2DPixel(cleft, cyb, cleft, cbottom), color, color);
        }
    }

    // titles
    float top = yb + 0.5 * (rowHeight - fontHeight);
    left = xb + colWidths[0];
    RString str = LocalizeString(IDS_MPTABLE_NAME);
    float x = left + 0.5 * (colWidths[1] - GEngine->GetTextWidth(_size, font, str));
    GEngine->DrawText(Point2DFloat(x, top), _size, font, color, str);
    left += colWidths[1];
    str = LocalizeString(IDS_MPTABLE_INFANTRY);
    x = left + 0.5 * (colWidths[2] - GEngine->GetTextWidth(_size, font, str));
    GEngine->DrawText(Point2DFloat(x, top), _size, font, color, str);
    left += colWidths[2];
    str = LocalizeString(IDS_MPTABLE_SOFT);
    x = left + 0.5 * (colWidths[3] - GEngine->GetTextWidth(_size, font, str));
    GEngine->DrawText(Point2DFloat(x, top), _size, font, color, str);
    left += colWidths[3];
    str = LocalizeString(IDS_MPTABLE_ARMORED);
    x = left + 0.5 * (colWidths[4] - GEngine->GetTextWidth(_size, font, str));
    GEngine->DrawText(Point2DFloat(x, top), _size, font, color, str);
    left += colWidths[4];
    str = LocalizeString(IDS_MPTABLE_AIR);
    x = left + 0.5 * (colWidths[5] - GEngine->GetTextWidth(_size, font, str));
    GEngine->DrawText(Point2DFloat(x, top), _size, font, color, str);
    left += colWidths[5];
    str = LocalizeString(IDS_MPTABLE_PLAYERS);
    x = left + 0.5 * (colWidths[6] - GEngine->GetTextWidth(_size, font, str));
    GEngine->DrawText(Point2DFloat(x, top), _size, font, color, str);
    left += colWidths[6];
    str = LocalizeString(IDS_MPTABLE_KILLED);
    x = left + 0.5 * (colWidths[7] - GEngine->GetTextWidth(_size, font, str));
    GEngine->DrawText(Point2DFloat(x, top), _size, font, color, str);
    left += colWidths[7];
    str = LocalizeString(IDS_MPTABLE_TOTAL);
    x = left + 0.5 * (colWidths[8] - GEngine->GetTextWidth(_size, font, str));
    GEngine->DrawText(Point2DFloat(x, top), _size, font, color, str);

    // values
    TargetSide playerSide = TSideUnknown;
    for (int i = 0; i < nRows; i++)
    {
        AIStatsMPRow& row = _tableMP[i];
        if (row.player == GetLocalPlayerName())
        {
            float top = yb + (i + 1) * rowHeight;
            GEngine->Draw2D(mip, colorSelected, Rect2DPixel(cxb, CY(top), cxe - cxb, CY(top + rowHeight) - CY(top)));
            playerSide = row.side;
        }
        PackedColor colorAct;
        switch (row.side)
        {
            case TWest:
                colorAct = colorWest;
                break;
            case TEast:
                colorAct = colorEast;
                break;
            case TGuerrila:
                colorAct = colorRes;
                break;
            case TCivilian:
                colorAct = colorCiv;
                break;
            default:
                colorAct = color;
                break;
        }

        top += rowHeight;

        left = xb;
        x = left + 0.5 * (colWidths[0] - GEngine->GetTextWidthF(_size, font, "%d", row.order));
        GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.order);

        left += colWidths[0];
        float sizeName = 0.8 * _size;
        x = left + 0.5 * (colWidths[1] - GEngine->GetTextWidth(sizeName, font, row.player));
        GEngine->DrawText(Point2DFloat(x, top), sizeName, Rect2DFloat(left, top, colWidths[1], top + rowHeight), font,
                          colorAct, row.player);

        left += colWidths[1];
        if (row.killsInfantry > 0)
        {
            x = left + 0.5 * (colWidths[2] - GEngine->GetTextWidthF(_size, font, "%d", row.killsInfantry));
            GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killsInfantry);
        }
        left += colWidths[2];
        if (row.killsSoft > 0)
        {
            x = left + 0.5 * (colWidths[3] - GEngine->GetTextWidthF(_size, font, "%d", row.killsSoft));
            GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killsSoft);
        }
        left += colWidths[3];
        if (row.killsArmor > 0)
        {
            x = left + 0.5 * (colWidths[4] - GEngine->GetTextWidthF(_size, font, "%d", row.killsArmor));
            GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killsArmor);
        }
        left += colWidths[4];
        if (row.killsAir > 0)
        {
            x = left + 0.5 * (colWidths[5] - GEngine->GetTextWidthF(_size, font, "%d", row.killsAir));
            GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killsAir);
        }
        left += colWidths[5];
        if (row.killsPlayers > 0)
        {
            x = left + 0.5 * (colWidths[6] - GEngine->GetTextWidthF(_size, font, "%d", row.killsPlayers));
            GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killsPlayers);
        }
        left += colWidths[6];
        if (row.killed > 0)
        {
            x = left + 0.5 * (colWidths[8] - GEngine->GetTextWidthF(_size, font, "%d", row.killed));
            GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killed);
        }
        left += colWidths[7];
        if (row.killsTotal > 0)
        {
            x = left + 0.5 * (colWidths[7] - GEngine->GetTextWidthF(_size, font, "%d", row.killsTotal));
            GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killsTotal);
        }
    }

    // summary for sides
    if (nSides > 1)
    {
        const float yb = ye - 4 * rowHeight;
        const float cyb = CY(yb);

        // background
        GEngine->Draw2D(mip, bgColor, Rect2DPixel(cxb, cyb, cxe - cxb, CY(yb + nSides * rowHeight) - cyb));

        // lines
        float bottom = yb;
        float cbottom = CY(bottom);
        GEngine->DrawLine(Line2DPixel(cxb, cbottom, cxe, cbottom), color, color);
        for (int i = 0; i < nSides; i++)
        {
            bottom += rowHeight;
            cbottom = CY(bottom);
            GEngine->DrawLine(Line2DPixel(cxb, cbottom, cxe, cbottom), color, color);
        }
        float left = xb;
        float cleft = cxb;
        GEngine->DrawLine(Line2DPixel(cleft, cyb, cleft, cbottom), color, color);
        for (int i = 0; i < columns; i++)
        {
            left += colWidths[i];
            cleft = CX(left);
            if (doubleLines[i])
            {
                GEngine->DrawLine(Line2DPixel(cleft - 1, cyb, cleft - 1, cbottom), color, color);
                GEngine->DrawLine(Line2DPixel(cleft + 1, cyb, cleft + 1, cbottom), color, color);
            }
            else
            {
                GEngine->DrawLine(Line2DPixel(cleft, cyb, cleft, cbottom), color, color);
            }
        }

        // values
        float top = yb + 0.5 * (rowHeight - fontHeight);
        int r = 0;
        for (int i = 0; i < nSides; i++)
        {
            PackedColor colorAct;
            AIStatsMPRow* pRow = nullptr;
            switch (i)
            {
                case 0:
                    if (!isWest)
                    {
                        continue;
                    }
                    pRow = &rowWest;
                    colorAct = colorWest;
                    break;
                case 1:
                    if (!isEast)
                    {
                        continue;
                    }
                    pRow = &rowEast;
                    colorAct = colorEast;
                    break;
                case 2:
                    if (!isRes)
                    {
                        continue;
                    }
                    pRow = &rowRes;
                    colorAct = colorRes;
                    break;
                case 3:
                    if (!isCiv)
                    {
                        continue;
                    }
                    pRow = &rowCiv;
                    colorAct = colorCiv;
                    break;
            }

            AIStatsMPRow& row = *pRow;
            if (row.side == playerSide)
            {
                float top = yb + r * rowHeight;
                GEngine->Draw2D(mip, colorSelected,
                                Rect2DPixel(cxb, CY(top), cxe - cxb, CY(top + rowHeight) - CY(top)));
            }

            left = xb + colWidths[0];
            float sizeName = 0.8 * _size;
            x = left + 0.5 * (colWidths[1] - GEngine->GetTextWidth(sizeName, font, row.player));
            GEngine->DrawText(Point2DFloat(x, top), sizeName, Rect2DFloat(left, top, colWidths[1], top + rowHeight),
                              font, colorAct, row.player);

            left += colWidths[1];
            if (row.killsInfantry > 0)
            {
                x = left + 0.5 * (colWidths[2] - GEngine->GetTextWidthF(_size, font, "%d", row.killsInfantry));
                GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killsInfantry);
            }
            left += colWidths[2];
            if (row.killsSoft > 0)
            {
                x = left + 0.5 * (colWidths[3] - GEngine->GetTextWidthF(_size, font, "%d", row.killsSoft));
                GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killsSoft);
            }
            left += colWidths[3];
            if (row.killsArmor > 0)
            {
                x = left + 0.5 * (colWidths[4] - GEngine->GetTextWidthF(_size, font, "%d", row.killsArmor));
                GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killsArmor);
            }
            left += colWidths[4];
            if (row.killsAir > 0)
            {
                x = left + 0.5 * (colWidths[5] - GEngine->GetTextWidthF(_size, font, "%d", row.killsAir));
                GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killsAir);
            }
            left += colWidths[5];
            if (row.killsPlayers > 0)
            {
                x = left + 0.5 * (colWidths[6] - GEngine->GetTextWidthF(_size, font, "%d", row.killsPlayers));
                GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killsPlayers);
            }
            left += colWidths[6];
            if (row.killed > 0)
            {
                x = left + 0.5 * (colWidths[8] - GEngine->GetTextWidthF(_size, font, "%d", row.killed));
                GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killed);
            }
            left += colWidths[7];
            if (row.killsTotal > 0)
            {
                x = left + 0.5 * (colWidths[7] - GEngine->GetTextWidthF(_size, font, "%d", row.killsTotal));
                GEngine->DrawTextF(Point2DFloat(x, top), _size, font, colorAct, "%d", row.killsTotal);
            }

            top += rowHeight;
            r++;
        }
    }
}

LSError AIStatsEvent::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(ar.SerializeEnum("type", type, 1))
    PARAM_CHECK(ar.Serialize("time", time, 1))
    PARAM_CHECK(ar.Serialize("position", position, 1))
    PARAM_CHECK(ar.Serialize("message", message, 1, ""))

    PARAM_CHECK(Poseidon::Serialize(ar, "killedType", killedType, 1, GWorld->Preloaded(VTypeAllVehicles)))
    PARAM_CHECK(ar.Serialize("killedName", killedName, 1, ""))
    PARAM_CHECK(ar.Serialize("killedPlayer", killedPlayer, 1, false))
    PARAM_CHECK(Poseidon::Serialize(ar, "killerType", killerType, 1, GWorld->Preloaded(VTypeAllVehicles)))
    PARAM_CHECK(ar.Serialize("killerName", killerName, 1, ""))
    PARAM_CHECK(ar.Serialize("killerPlayer", killerPlayer, 1, false))
    return LSOK;
}

LSError AIStatsMission::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(ar.Serialize("Events", _events, 1))
    PARAM_CHECK(ar.Serialize("lives", _lives, 1, -1))
    PARAM_CHECK(ar.Serialize("start", _start, 1))
    return LSOK;
}

LSError AIUnitHeader::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(ar.Serialize("index", index, 1))
    PARAM_CHECK(ar.Serialize("experience", experience, 1, 0))
    PARAM_CHECK(ar.SerializeEnum("rank", rank, 1, RankUndefined))
    PARAM_CHECK(ar.SerializeRef("Unit", unit, 1))
    PARAM_CHECK(ar.Serialize("used", used, 1, false))
    return LSOK;
}

AIStatsCampaign::AIStatsCampaign()
{
    Clear();
}

void AIStatsCampaign::Clear()
{
    _playerInfo.index = -1;
    _playerInfo.experience = 0;
    _playerInfo.rank = RankPrivate;
    _playerInfo.unit = nullptr;
    _variables.Clear();
    _casualties = 0;
    _inCombat = 0;
    for (int i = 0; i < SKN; i++)
    {
        _kills[i] = 0;
    }

    _score = 0;
    _lastScore = 0;

    _awards.Clear();
    _penalties.Clear();

    _date = 0;
}
