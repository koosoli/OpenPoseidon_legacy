// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Dev/NpcChat/NpcChatGame.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/AICenter.hpp>
#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/AI/ArcadeTemplate.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Game/Chat.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/UI/Locale/MissionHtmlLocalization.hpp>
#include <Poseidon/UI/Map/UIMap.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>
#include <Evaluator/express.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

std::string Arma2BriefingHtml(const std::string& html); // GameStateExtA2.cpp (global namespace)

namespace Poseidon::NpcChat
{
namespace
{
constexpr float kPeopleRadius = 60.0f;
constexpr float kVehicleRadius = 80.0f;
constexpr int kMaxPeople = 8;
constexpr int kMaxContacts = 10;
constexpr int kMaxVehicles = 5;

struct Registry
{
    std::vector<std::pair<std::string, OLink<Person>>> people;      // P#
    std::vector<std::pair<std::string, OLink<EntityAI>>> contacts;  // E#
    std::vector<std::pair<std::string, Vector3>> contactPositions;  // last known
    std::vector<std::pair<std::string, OLink<EntityAI>>> vehicles;  // V#
    Vector3 lookPoint = VZero;
    bool hasLook = false;
};

std::map<int, Registry> s_registries;
int s_nextRegistry = 1;

// Briefing text is re-read only when the mission's briefing file changes.
std::string s_briefingFile;
std::string s_briefingText;

Registry* FindRegistry(int id)
{
    auto it = s_registries.find(id);
    return it == s_registries.end() ? nullptr : &it->second;
}

float Bearing(Vector3Par from, Vector3Par to)
{
    float deg = std::atan2(to.X() - from.X(), to.Z() - from.Z()) * 57.29578f;
    if (deg < 0)
        deg += 360.0f;
    return deg;
}

const char* Compass(float deg)
{
    static const char* names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return names[static_cast<int>(std::floor((deg + 22.5f) / 45.0f)) & 7];
}

std::string DescribeOffset(Vector3Par from, Vector3Par to)
{
    char buf[64];
    const float b = Bearing(from, to);
    std::snprintf(buf, sizeof(buf), "%.0f m, bearing %03.0f (%s)", (to - from).Size(), b, Compass(b));
    return buf;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char* SideName(TargetSide side)
{
    switch (side)
    {
        case TWest: return "WEST";
        case TEast: return "EAST";
        case TGuerrila: return "RESISTANCE";
        case TCivilian: return "CIVILIAN";
        default: return "UNKNOWN";
    }
}

std::string PersonName(Person* person)
{
    if (!person)
        return {};
    const RString name = person->GetInfo()._name;
    return name.GetLength() > 0 ? std::string(name) : std::string(person->GetDisplayName());
}

/// SQF read of one value about `object`, as "_u" (damage, someAmmo, behaviour ...).
GameValue EvalAbout(Object* object, const char* expression)
{
    GameState* gs = GWorld ? GWorld->GetGameState() : nullptr;
    if (!gs || !object)
        return GameValue();
    GameVarSpace space;
    gs->BeginContext(&space);
    gs->VarSetLocal("_u", GameValueExt(object), true, true);
    GameValue v = gs->EvaluateMultiple(expression);
    gs->EndContext();
    return v;
}

std::string EvalString(Object* object, const char* expression)
{
    GameValue v = EvalAbout(object, expression);
    if (v.GetType() != GameString)
        return {};
    const RString s = v;
    return std::string(s);
}

std::string Health(Person* person)
{
    const float damage = static_cast<float>(EvalAbout(person, "damage _u"));
    if (damage < 0.1f)
        return "healthy";
    if (damage < 0.5f)
        return "lightly wounded";
    return "badly wounded";
}

bool LookPoint(Vector3& out)
{
    if (!GScene || !GScene->GetCamera() || !GLandscape)
        return false;
    const Vector3 origin = GScene->GetCamera()->Position();
    const Vector3 dir = GScene->GetCamera()->Direction();
    const float step = 10.0f;
    float prev = 0.0f;
    for (float t = step; t <= 1500.0f; t += step)
    {
        const Vector3 p = origin + dir * t;
        if (p.Y() <= GLandscape->SurfaceY(p.X(), p.Z()))
        {
            float lo = prev, hi = t;
            for (int i = 0; i < 20; ++i)
            {
                const float mid = 0.5f * (lo + hi);
                const Vector3 pm = origin + dir * mid;
                (pm.Y() > GLandscape->SurfaceY(pm.X(), pm.Z()) ? lo : hi) = mid;
            }
            out = origin + dir * (0.5f * (lo + hi));
            return true;
        }
        prev = t;
    }
    // Looking at the sky: the point 300 m out along the view, on the ground.
    const Vector3 p = origin + dir * 300.0f;
    out = Vector3(p.X(), GLandscape->SurfaceY(p.X(), p.Z()), p.Z());
    return true;
}

Person* PlayerPerson()
{
    return GWorld ? GWorld->PlayerOn() : nullptr;
}

AIUnit* PlayerUnit()
{
    Person* p = PlayerPerson();
    return p ? p->Brain() : nullptr;
}

/// Squad member by number ("2") or by (part of) name ("Miller").
AIUnit* SquadMember(AIGroup* grp, const std::string& id)
{
    if (!grp || id.empty())
        return nullptr;
    if (std::all_of(id.begin(), id.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        return grp->UnitWithID(std::atoi(id.c_str()));
    // Call sign, real name ("James Pound", "Pound") or role ("Grenadier"); see MatchSquadMember.
    std::vector<SquadCandidate> squad;
    AIUnit* me = PlayerUnit();
    for (int i = 1; i <= MAX_UNITS_PER_GROUP; ++i)
    {
        AIUnit* unit = grp->UnitWithID(i);
        if (!unit || unit == me || !unit->GetPerson())
            continue;
        squad.push_back({unit->ID(), PersonName(unit->GetPerson()), std::string(unit->GetPerson()->GetDisplayName()),
                         CurrentSettings().CallsignFor(unit->ID())});
    }
    const int match = MatchSquadMember(squad, id);
    return match ? grp->UnitWithID(match) : nullptr;
}

template <class T>
T* FindLinked(const std::vector<std::pair<std::string, OLink<T>>>& list, const std::string& id)
{
    for (const auto& [key, link] : list)
        if (key == id)
            return link.GetLink();
    return nullptr;
}

std::string Pos(Vector3Par p)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "[%.1f,%.1f,0]", p.X(), p.Z());
    return buf;
}

std::string LoadBriefingText()
{
    const RString file = GetBriefingFile();
    const std::string fileName = file.GetLength() > 0 ? std::string(file) : std::string();
    if (fileName == s_briefingFile && !s_briefingText.empty())
        return s_briefingText;
    s_briefingFile = fileName;
    std::string html = fileName.empty() ? std::string() : std::string(LoadLocalizedMissionHtmlUtf8(file));
    html = ::Arma2BriefingHtml(html); // no-op for OFP content; folds in the Arma 2 diary/tasks
    s_briefingText = HtmlToText(html);
    return s_briefingText;
}

std::string MissionName()
{
    RString name = LoadLocalizedMissionBriefingName(GetMissionDirectory(), CurrentTemplate.intel.briefingName);
    if (name.GetLength() == 0)
        name = Glob.header.filename;
    return std::string(name);
}
} // namespace

std::string HtmlToText(const std::string& html)
{
    std::string out;
    out.reserve(html.size());
    for (size_t i = 0; i < html.size(); ++i)
    {
        const char c = html[i];
        if (c == '<')
        {
            const size_t end = html.find('>', i);
            if (end == std::string::npos)
                break;
            std::string tag = Lower(html.substr(i + 1, end - i - 1));
            const bool closing = !tag.empty() && tag[0] == '/';
            if (closing)
                tag.erase(0, 1);
            const std::string name = tag.substr(0, tag.find_first_of(" \t\r\n/"));
            if (!closing && (name == "head" || name == "title" || name == "script" || name == "style"))
            {
                // Not briefing text: skip to the matching close tag.
                const size_t close = Lower(html).find("</" + name, end);
                i = close == std::string::npos ? html.size() : html.find('>', close);
                if (i == std::string::npos)
                    break;
                continue;
            }
            if (name == "br" || name == "p" || name == "h1" || name == "h2" || name == "h3" || name == "h4" ||
                name == "li" || name == "hr" || name == "tr" || name == "div")
                out += '\n';
            // A named anchor starts an OFP briefing section; keep the name so "Plan" and "OBJ_1" read.
            if (!closing && name == "a")
            {
                const size_t n = tag.find("name=");
                if (n != std::string::npos)
                {
                    std::string anchor = html.substr(i + 1 + n + 5, end - (i + 1 + n + 5));
                    anchor.erase(std::remove(anchor.begin(), anchor.end(), '"'), anchor.end());
                    anchor.erase(std::remove(anchor.begin(), anchor.end(), '\''), anchor.end());
                    if (!anchor.empty() && anchor.rfind("__", 0) != 0)
                        out += "\n[" + anchor + "] ";
                }
            }
            i = end;
            continue;
        }
        if (c == '&')
        {
            static const std::pair<const char*, const char*> entities[] = {
                {"&nbsp;", " "}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}};
            bool matched = false;
            for (const auto& [entity, text] : entities)
            {
                const size_t len = std::char_traits<char>::length(entity);
                if (html.compare(i, len, entity) == 0)
                {
                    out += text;
                    i += len - 1;
                    matched = true;
                    break;
                }
            }
            if (matched)
                continue;
        }
        out += (c == '\r' || c == '\t') ? ' ' : c;
    }
    // Collapse runs of spaces and of blank lines.
    std::string collapsed;
    collapsed.reserve(out.size());
    int newlines = 0;
    bool space = false;
    for (char c : out)
    {
        if (c == '\n')
        {
            while (!collapsed.empty() && collapsed.back() == ' ')
                collapsed.pop_back();
            if (++newlines <= 2 && !collapsed.empty())
                collapsed += '\n';
            space = false;
            continue;
        }
        if (c == ' ')
        {
            space = true;
            continue;
        }
        if (space && !collapsed.empty() && collapsed.back() != '\n')
            collapsed += ' ';
        space = false;
        newlines = 0;
        collapsed += c;
    }
    while (!collapsed.empty() && (collapsed.back() == '\n' || collapsed.back() == ' '))
        collapsed.pop_back();
    return collapsed;
}

bool GameAvailable(std::string* why)
{
    auto fail = [&](const char* reason)
    {
        if (why)
            *why = reason;
        return false;
    };
    if (!GWorld)
        return fail("No world loaded.");
    if (GWorld->GetMode() == GModeNetware)
        return fail("NPC chat is single-player only.");
    Person* player = PlayerPerson();
    if (!player || !player->Brain())
        return fail("Start a mission first: there is no player soldier.");
    if (!GWorld->GetGameState())
        return fail("No script state.");
    return true;
}

std::string MissionKey()
{
    if (!GameAvailable())
        return {};
    return std::string(GetMissionDirectory()) + "|" + std::string(Glob.header.filename);
}

bool PlayerPosition(float& x, float& y, float& z)
{
    Person* p = PlayerPerson();
    if (!p)
        return false;
    const Vector3 pos = p->Position();
    x = pos.X();
    y = pos.Y();
    z = pos.Z();
    return true;
}

ReportResult BuildSituationReport(const Settings& settings)
{
    ReportResult result;
    if (!GameAvailable(&result.error))
        return result;

    Person* player = PlayerPerson();
    AIUnit* me = player->Brain();
    AIGroup* grp = me->GetGroup();
    AICenter* center = grp ? grp->GetCenter() : nullptr;
    const Vector3 pp = player->Position();
    const bool leader = grp && grp->Leader() == me;

    const int registryId = s_nextRegistry++;
    Registry& reg = s_registries[registryId];
    // Keep only the last few reports: a reply never arrives later than that.
    while (s_registries.size() > 8)
        s_registries.erase(s_registries.begin());

    std::string r;
    char line[1536]; // a squad line carries the persona too
    r += "=== SITUATION REPORT (live game state) ===\n";
    r += "Mission: " + MissionName() + "\n";

    if (settings.includeEnvironment)
    {
        GameState* gs = GWorld->GetGameState();
        const float hours = static_cast<float>(gs->EvaluateMultiple("dayTime"));
        const float overcast = static_cast<float>(gs->EvaluateMultiple("overcast"));
        const float fog = static_cast<float>(gs->EvaluateMultiple("fog"));
        const float rain = static_cast<float>(gs->EvaluateMultiple("rain"));
        const int hh = static_cast<int>(hours) % 24;
        const int mm = static_cast<int>((hours - std::floor(hours)) * 60.0f);
        std::snprintf(line, sizeof(line),
                      "Time: %02d:%02d (%s). Weather: overcast %.0f%%, fog %.0f%%, rain %.0f%%. Year 1985.\n", hh, mm,
                      (hh >= 6 && hh < 19) ? "daylight" : "night", overcast * 100, fog * 100, rain * 100);
        r += line;
    }

    // The player.
    {
        std::string vehicle = "on foot";
        if (Transport* veh = me->GetVehicleIn())
        {
            const char* role = veh->Driver() == player ? "driver" : veh->Gunner() == player ? "gunner"
                               : veh->Commander() == player ? "commander" : "passenger";
            vehicle = std::string(role) + " of a " + std::string(veh->GetDisplayName());
        }
        std::snprintf(line, sizeof(line),
                      "PLAYER: %s (%s), squad number %d%s, side %s, %s, %s. Facing %03.0f (%s). Map position "
                      "x=%.0f m east, y=%.0f m north.\n",
                      PersonName(player).c_str(), std::string(player->GetDisplayName()).c_str(), me->ID(),
                      leader ? ", the SQUAD LEADER" : ", NOT the squad leader", SideName(player->GetTargetSide()),
                      Health(player).c_str(), vehicle.c_str(), Bearing(VZero, player->Direction()),
                      Compass(Bearing(VZero, player->Direction())), pp.X(), pp.Z());
        r += line;
        if (!leader && grp && grp->Leader() && grp->Leader()->GetPerson())
            r += "Squad leader: " + PersonName(grp->Leader()->GetPerson()) + " (squad number " +
                 std::to_string(grp->Leader()->ID()) + ").\n";
    }

    reg.hasLook = LookPoint(reg.lookPoint);
    if (reg.hasLook)
        r += "Player is looking at a point " + DescribeOffset(pp, reg.lookPoint) + ".\n";

    // The squad.
    std::string nearestSquad;
    float nearestSquadDist = 1e9f;
    if (settings.includeSquad && grp)
    {
        r += leader ? "\nSQUAD (the player can order these, address by number or name):\n"
                    : "\nSQUAD (the player is not the leader, so they cannot give orders):\n";
        int count = 0;
        for (int i = 1; i <= MAX_UNITS_PER_GROUP; ++i)
        {
            AIUnit* unit = grp->UnitWithID(i);
            if (!unit || unit == me || !unit->GetPerson())
                continue;
            Person* person = unit->GetPerson();
            const bool dead = unit->GetLifeState() == AIUnit::LSDead || person->IsDammageDestroyed();
            std::string extra;
            if (Transport* veh = unit->GetVehicleIn())
                extra = ", in a " + std::string(veh->GetDisplayName());
            if (!dead && !static_cast<bool>(EvalAbout(person, "someAmmo _u")))
                extra += ", OUT OF AMMO";
            const std::string sign = CurrentSettings().CallsignFor(unit->ID());
            const std::string label =
                std::to_string(unit->ID()) + (sign.empty() ? std::string() : " (call sign \"" + sign + "\")");
            extra += " -- persona: " + CurrentSettings().PersonaFor(unit->ID(), std::string(person->GetDisplayName()));
            if (unit->ID() == JokerSlot())
                extra += " -- THE SQUAD JOKER: sarcastic one-liners when something happens";
            std::snprintf(line, sizeof(line), " %s: %s, %s, %s, %s%s\n", label.c_str(), PersonName(person).c_str(),
                          std::string(person->GetDisplayName()).c_str(), DescribeOffset(pp, person->Position()).c_str(),
                          dead ? "DEAD" : Health(person).c_str(), extra.c_str());
            r += line;
            ++count;
            const float d = (person->Position() - pp).Size();
            if (!dead && d < nearestSquadDist)
            {
                nearestSquadDist = d;
                nearestSquad = std::to_string(unit->ID());
            }
        }
        if (count == 0)
            r += " (nobody else, the player is alone)\n";
        std::snprintf(line, sizeof(line), "Squad state: behaviour %s, combat mode %s, formation %s, speed %s.\n",
                      EvalString(player, "behaviour _u").c_str(), EvalString(player, "combatMode group _u").c_str(),
                      EvalString(player, "formation group _u").c_str(),
                      EvalString(player, "speedMode group _u").c_str());
        r += line;
    }

    // People near the player who are not in the squad: friendlies and civilians.
    struct Near
    {
        Person* person;
        float dist;
    };
    std::vector<Near> nearby;
    AICenter* centers[] = {GWorld->GetWestCenter(), GWorld->GetEastCenter(), GWorld->GetGuerrilaCenter(),
                           GWorld->GetCivilianCenter()};
    for (AICenter* c : centers)
    {
        if (!c)
            continue;
        for (int g = 0; g < c->NGroups(); ++g)
        {
            AIGroup* other = c->GetGroup(g);
            if (!other || other == grp)
                continue;
            for (int i = 1; i <= MAX_UNITS_PER_GROUP; ++i)
            {
                AIUnit* unit = other->UnitWithID(i);
                Person* person = unit ? unit->GetPerson() : nullptr;
                if (!person || person->IsDammageDestroyed() || unit->GetLifeState() == AIUnit::LSDead)
                    continue;
                if (center && center->IsEnemy(person->GetTargetSide()))
                    continue; // enemies are contacts, not conversation partners
                const float d = (person->Position() - pp).Size();
                if (d <= kPeopleRadius)
                    nearby.push_back({person, d});
            }
        }
    }
    std::sort(nearby.begin(), nearby.end(), [](const Near& a, const Near& b) { return a.dist < b.dist; });
    if (nearby.size() > kMaxPeople)
        nearby.resize(kMaxPeople);

    // Who the player is looking at: the person (squad or not) closest to the view ray, within 12 degrees
    // and 40 m.
    std::string lookingAt;
    if (GScene && GScene->GetCamera())
    {
        const Vector3 camPos = GScene->GetCamera()->Position();
        const Vector3 camDir = GScene->GetCamera()->Direction();
        float best = 0.978f; // cos 12 degrees
        auto consider = [&](Person* person, const std::string& id)
        {
            const Vector3 to = person->Position() + Vector3(0, 1.4f, 0) - camPos;
            const float d = to.Size();
            if (d < 0.5f || d > 40.0f)
                return;
            const float cosA = (to * (1.0f / d)).DotProduct(camDir);
            if (cosA > best)
            {
                best = cosA;
                lookingAt = id;
            }
        };
        if (grp)
            for (int i = 1; i <= MAX_UNITS_PER_GROUP; ++i)
                if (AIUnit* unit = grp->UnitWithID(i); unit && unit != me && unit->GetPerson())
                    consider(unit->GetPerson(), std::to_string(unit->ID()));
        for (size_t i = 0; i < nearby.size(); ++i)
            consider(nearby[i].person, "P" + std::to_string(i + 1));
    }

    r += "\nPEOPLE NEAR THE PLAYER, NOT IN THE SQUAD (can talk, cannot be ordered):\n";
    if (nearby.empty())
        r += " (none within 60 m)\n";
    for (size_t i = 0; i < nearby.size(); ++i)
    {
        Person* person = nearby[i].person;
        const std::string id = "P" + std::to_string(i + 1);
        reg.people.emplace_back(id, OLink<Person>(person));
        const TargetSide side = person->GetTargetSide();
        const char* relation = side == TCivilian ? "civilian" : "friendly soldier";
        std::snprintf(line, sizeof(line), " %s: %s, %s (%s, %s), %s%s%s\n", id.c_str(), PersonName(person).c_str(),
                      std::string(person->GetDisplayName()).c_str(), relation, SideName(side),
                      DescribeOffset(pp, person->Position()).c_str(), i == 0 ? " -- NEAREST" : "",
                      lookingAt == id ? " -- THE PLAYER IS LOOKING AT THEM" : "");
        r += line;
    }
    if (!lookingAt.empty() && lookingAt[0] != 'P')
        r += "The player is looking at squad member " + lookingAt + ".\n";

    // Known contacts: only what the squad's own target list holds, never ground truth.
    if (settings.includeEnemies && grp && center)
    {
        struct Contact
        {
            Target* target;
            float dist;
        };
        std::vector<Contact> contacts;
        const TargetList& list = grp->GetTargetList();
        for (int i = 0; i < list.Size(); ++i)
        {
            Target* t = list[i];
            if (!t || !t->IsKnown() || t->destroyed || t->vanished || !t->type)
                continue;
            if (!center->IsEnemy(t->side))
                continue;
            contacts.push_back({t, (t->position - pp).Size()});
        }
        std::sort(contacts.begin(), contacts.end(),
                  [](const Contact& a, const Contact& b) { return a.dist < b.dist; });
        if (contacts.size() > kMaxContacts)
            contacts.resize(kMaxContacts);
        r += "\nKNOWN ENEMY CONTACTS (reported to the squad; positions are last known):\n";
        if (contacts.empty())
            r += " (none known)\n";
        for (size_t i = 0; i < contacts.size(); ++i)
        {
            Target* t = contacts[i].target;
            const std::string id = "E" + std::to_string(i + 1);
            reg.contacts.emplace_back(id, OLink<EntityAI>(t->idExact.GetLink()));
            reg.contactPositions.emplace_back(id, t->position);
            const float seen = Glob.time - t->lastSeen;
            std::snprintf(line, sizeof(line), " %s: %s (%s), %s, last seen %.0f s ago\n", id.c_str(),
                          std::string(t->type->GetDisplayName()).c_str(), SideName(t->side),
                          DescribeOffset(pp, t->position).c_str(), seen < 0 ? 0.0f : seen);
            r += line;
        }
    }

    // Vehicles the squad could board.
    {
        struct Veh
        {
            Transport* veh;
            float dist;
        };
        std::vector<Veh> vehicles;
        for (int i = 0; i < GWorld->NVehicles(); ++i)
        {
            Transport* veh = dyn_cast<Transport>(GWorld->GetVehicle(i));
            if (!veh || veh->IsDammageDestroyed())
                continue;
            const float d = (veh->Position() - pp).Size();
            if (d > kVehicleRadius)
                continue;
            Person* crew = veh->Driver() ? veh->Driver() : veh->Commander() ? veh->Commander() : veh->Gunner();
            if (crew && center && !center->IsFriendly(crew->GetTargetSide()))
                continue;
            vehicles.push_back({veh, d});
        }
        std::sort(vehicles.begin(), vehicles.end(), [](const Veh& a, const Veh& b) { return a.dist < b.dist; });
        if (vehicles.size() > kMaxVehicles)
            vehicles.resize(kMaxVehicles);
        if (!vehicles.empty())
        {
            r += "\nVEHICLES NEARBY:\n";
            for (size_t i = 0; i < vehicles.size(); ++i)
            {
                Transport* veh = vehicles[i].veh;
                const std::string id = "V" + std::to_string(i + 1);
                reg.vehicles.emplace_back(id, OLink<EntityAI>(veh));
                int used = 0;
                for (int c = 0; c < veh->GetManCargo().Size(); ++c)
                    if (veh->GetManCargo()[c])
                        ++used;
                const int freeSeats = std::max(0, veh->Type()->GetMaxManCargo() - used) + (veh->Driver() ? 0 : 1);
                std::snprintf(line, sizeof(line), " %s: %s, %s, %s, %d free seats\n", id.c_str(),
                              std::string(veh->GetDisplayName()).c_str(), DescribeOffset(pp, veh->Position()).c_str(),
                              veh->Driver() ? "crewed" : "no driver", freeSeats);
                r += line;
            }
        }
    }

    bool papaBearTaken = false;
    for (const auto& [slot, sign] : settings.callsigns)
        if (Lower(sign) == "papa bear")
            papaBearTaken = true;
    r += papaBearTaken ? "\nHQ (\"Command\" / \"HQ\") can be reached over the radio. \"Papa Bear\" is a squad call "
                         "sign in this mission, NOT HQ.\n"
                       : "\nHQ (\"Command\" / \"HQ\" / \"Papa Bear\") can be reached over the radio.\n";

    if (settings.includeBriefing)
    {
        std::string briefing = LoadBriefingText();
        if (!briefing.empty())
        {
            if (static_cast<int>(briefing.size()) > settings.briefingMaxChars)
                briefing = briefing.substr(0, static_cast<size_t>(std::max(0, settings.briefingMaxChars))) + "\n[...]";
            r += "\n=== MISSION BRIEFING ===\n" + briefing + "\n";
        }
    }

    result.ok = true;
    result.text = std::move(r);
    result.registryId = registryId;
    // Default answerer: who the player looks at, else the nearest squad mate, else the nearest
    // person, else HQ.
    if (!lookingAt.empty())
        result.defaultSpeaker = lookingAt;
    else if (!nearestSquad.empty() && nearestSquadDist < 30.0f)
        result.defaultSpeaker = nearestSquad;
    else if (!nearby.empty())
        result.defaultSpeaker = "P1";
    else if (!nearestSquad.empty())
        result.defaultSpeaker = nearestSquad;
    else
        result.defaultSpeaker = "HQ";
    return result;
}

void ReleaseRegistry(int registryId)
{
    s_registries.erase(registryId);
}

SpeakerInfo ResolveSpeaker(int registryId, const std::string& id)
{
    SpeakerInfo info;
    info.id = id;
    if (id == "HQ")
    {
        info.valid = info.isHq = true;
        info.name = "Command";
        info.distance = 1e6f;
        info.voiceSeed = 0;
        return info;
    }
    Person* player = PlayerPerson();
    AIUnit* me = player ? player->Brain() : nullptr;
    AIGroup* grp = me ? me->GetGroup() : nullptr;
    Person* person = nullptr;
    if (!id.empty() && id[0] == 'P' && id.size() > 1 &&
        std::isdigit(static_cast<unsigned char>(id[1])))
    {
        if (Registry* reg = FindRegistry(registryId))
            person = FindLinked(reg->people, id);
    }
    else if (AIUnit* unit = SquadMember(grp, id))
    {
        if (unit != me)
        {
            person = unit->GetPerson();
            info.inSquad = true;
            info.id = std::to_string(unit->ID());
        }
    }
    if (!person && Lower(id) == "papa bear")
        return ResolveSpeaker(registryId, "HQ"); // HQ's traditional call sign, unless a squad member took it
    if (!person || person->IsDammageDestroyed())
        return info;
    info.valid = true;
    info.name = PersonName(person);
    const Vector3 pos = person->Position();
    info.x = pos.X();
    info.y = pos.Y() + 1.6f;
    info.z = pos.Z();
    info.distance = player ? (pos - player->Position()).Size() : 0.0f;
    unsigned h = 2166136261u;
    for (char c : info.name)
        h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    info.voiceSeed = info.inSquad ? std::atoi(info.id.c_str()) : static_cast<int>(h & 0x7fffffff);
    return info;
}

int JokerSlot()
{
    const int chosen = CurrentSettings().jokerSlot;
    if (chosen >= 0)
        return chosen;
    // Automatic: the second squad mate (the first is usually the senior one).
    int seen = 0;
    for (const SquadSlot& slot : CurrentSquad())
        if (!slot.isPlayer && !slot.dead && ++seen == 2)
            return slot.id;
    return 0;
}

std::vector<std::string> PollSquadEvents()
{
    // What the squad looked like at the last poll: damage per squad number, and the enemy
    // contacts the group knows about (by target pointer, never ground truth).
    static std::string s_mission;
    static std::map<int, float> s_damage;
    static std::map<int, bool> s_dead;
    static std::set<const void*> s_contacts;
    static std::set<const void*> s_destroyed;

    std::vector<std::string> events;
    if (!GameAvailable())
        return events;
    const std::string mission = MissionKey();
    const bool firstLook = mission != s_mission;
    if (firstLook)
    {
        s_mission = mission;
        s_damage.clear();
        s_dead.clear();
        s_contacts.clear();
        s_destroyed.clear();
    }

    Person* player = PlayerPerson();
    AIUnit* me = player->Brain();
    AIGroup* grp = me ? me->GetGroup() : nullptr;
    if (!grp)
        return events;

    for (int i = 1; i <= MAX_UNITS_PER_GROUP; ++i)
    {
        AIUnit* unit = grp->UnitWithID(i);
        if (!unit || !unit->GetPerson())
            continue;
        Person* person = unit->GetPerson();
        const bool dead = unit->GetLifeState() == AIUnit::LSDead || person->IsDammageDestroyed();
        const float damage = static_cast<float>(EvalAbout(person, "damage _u"));
        const std::string who = unit == me ? std::string("The player (squad leader)")
                                           : std::to_string(i) + " " + PersonName(person);
        if (!firstLook)
        {
            if (dead && !s_dead[i])
                events.push_back(who + " was killed.");
            else if (!dead && damage > s_damage[i] + 0.05f)
                events.push_back(who + " was hit (" + (damage < 0.5f ? "lightly wounded" : "badly wounded") + ").");
        }
        s_dead[i] = dead;
        s_damage[i] = damage;
    }

    AICenter* center = grp->GetCenter();
    const TargetList& list = grp->GetTargetList();
    const Vector3 pp = player->Position();
    for (int i = 0; i < list.Size(); ++i)
    {
        Target* t = list[i];
        if (!t || !t->type || !center || !center->IsEnemy(t->side))
            continue;
        const void* id = t;
        if (t->destroyed)
        {
            if (s_contacts.count(id) && s_destroyed.insert(id).second && !firstLook)
                events.push_back("Enemy " + std::string(t->type->GetDisplayName()) + " destroyed.");
            continue;
        }
        if (!t->IsKnown() || t->vanished)
            continue;
        if (s_contacts.insert(id).second && !firstLook)
            events.push_back("New enemy contact: " + std::string(t->type->GetDisplayName()) + ", " +
                             DescribeOffset(pp, t->position) + ".");
    }
    return events;
}

std::vector<SquadSlot> CurrentSquad()
{
    std::vector<SquadSlot> slots;
    if (!GameAvailable())
        return slots;
    AIUnit* me = PlayerUnit();
    AIGroup* grp = me ? me->GetGroup() : nullptr;
    if (!grp)
        return slots;
    for (int i = 1; i <= MAX_UNITS_PER_GROUP; ++i)
    {
        AIUnit* unit = grp->UnitWithID(i);
        if (!unit || !unit->GetPerson())
            continue;
        SquadSlot slot;
        slot.id = unit->ID();
        slot.name = PersonName(unit->GetPerson());
        slot.role = std::string(unit->GetPerson()->GetDisplayName());
        slot.isPlayer = unit == me;
        slot.dead = unit->GetLifeState() == AIUnit::LSDead || unit->GetPerson()->IsDammageDestroyed();
        slots.push_back(std::move(slot));
    }
    return slots;
}

bool SpeakerPosition(int registryId, const std::string& id, float& x, float& y, float& z)
{
    const SpeakerInfo info = ResolveSpeaker(registryId, id);
    if (!info.valid || info.isHq)
        return false;
    x = info.x;
    y = info.y;
    z = info.z;
    return true;
}

std::vector<std::string> ExecuteOrders(int registryId, const std::vector<Order>& orders)
{
    std::vector<std::string> log;
    if (orders.empty())
        return log;
    std::string why;
    if (!GameAvailable(&why))
    {
        log.push_back("orders ignored: " + why);
        return log;
    }
    Person* player = PlayerPerson();
    AIUnit* me = player->Brain();
    AIGroup* grp = me->GetGroup();
    if (!grp || grp->Leader() != me)
    {
        log.push_back("orders ignored: the player is not the squad leader");
        return log;
    }
    Registry* reg = FindRegistry(registryId);
    GameState* gs = GWorld->GetGameState();

    for (const Order& order : orders)
    {
        // Who.
        std::vector<AIUnit*> units;
        if (order.all)
        {
            for (int i = 1; i <= MAX_UNITS_PER_GROUP; ++i)
                if (AIUnit* unit = grp->UnitWithID(i); unit && unit != me && unit->GetPerson() &&
                                                       unit->GetLifeState() != AIUnit::LSDead)
                    units.push_back(unit);
        }
        else
        {
            for (const std::string& id : order.to)
                if (AIUnit* unit = SquadMember(grp, id); unit && unit != me && unit->GetPerson() &&
                                                          unit->GetLifeState() != AIUnit::LSDead &&
                                                          std::find(units.begin(), units.end(), unit) == units.end())
                    units.push_back(unit);
        }
        std::string who;
        for (AIUnit* unit : units)
            who += (who.empty() ? "" : ",") + std::to_string(unit->ID());
        if (units.empty())
        {
            log.push_back(std::string(OrderKindName(order.kind)) + ": no such squad member");
            continue;
        }

        // Where.
        bool hasPos = false;
        Vector3 pos = player->Position();
        if (order.where.Present())
        {
            hasPos = true;
            const std::string& ref = order.where.ref;
            if (ref == "look" && reg && reg->hasLook)
                pos = reg->lookPoint;
            else if (ref == "unit")
            {
                if (AIUnit* u = SquadMember(grp, order.where.id); u && u->GetPerson())
                    pos = u->GetPerson()->Position();
            }
            else if (ref == "target" && reg)
            {
                for (const auto& [key, p] : reg->contactPositions)
                    if (key == order.where.id)
                        pos = p;
            }
            else if (ref == "vehicle" && reg)
            {
                if (EntityAI* v = FindLinked(reg->vehicles, order.where.id))
                    pos = v->Position();
            }
            if (!std::isnan(order.where.bearing) || !std::isnan(order.where.distance))
            {
                const float b = std::isnan(order.where.bearing) ? Bearing(VZero, player->Direction())
                                                                 : order.where.bearing;
                const float d = std::isnan(order.where.distance) ? 50.0f
                                                                  : std::clamp(order.where.distance, 0.0f, 3000.0f);
                pos = pos + Vector3(std::sin(b / 57.29578f) * d, 0, std::cos(b / 57.29578f) * d);
            }
        }
        if (!hasPos && order.kind == OrderKind::Cover && reg && reg->hasLook)
        {
            pos = reg->lookPoint;
            hasPos = true;
        }

        // Target.
        EntityAI* target = nullptr;
        if (reg && !order.target.empty())
        {
            if (order.kind == OrderKind::GetIn)
                target = FindLinked(reg->vehicles, order.target);
            else if (!order.target.empty() && order.target[0] == 'P')
                target = FindLinked(reg->people, order.target); // anyone in the report, not only enemies
            else
                target = FindLinked(reg->contacts, order.target);
        }

        std::string code;
        const std::string p = Pos(pos);
        switch (order.kind)
        {
            case OrderKind::Follow: code = "_us commandFollow _p"; break;
            case OrderKind::Hold: code = "commandStop _us"; break;
            case OrderKind::Move: code = "_us commandMove " + p; break;
            case OrderKind::Cover:
                code = "commandStop _us; {_x setUnitPos \"MIDDLE\"} forEach _us";
                if (hasPos)
                    code += "; _us commandWatch " + p;
                break;
            case OrderKind::Watch: code = "_us commandWatch " + p; break;
            case OrderKind::Engage:
                if (!target)
                {
                    log.push_back(who + ": engage " + order.target + " -- no such target");
                    continue;
                }
                // AI never fires on someone its side does not count as an enemy, whatever it is told.
                // For a person from the report (friendly soldier, civilian), use the game's own renegade
                // mechanism: a rating far below zero makes them hostile to everyone.
                code = order.target[0] == 'P' ? "_t addRating -100000; " : "";
                code += "_g reveal _t; _us commandTarget _t; _us commandFire _t";
                break;
            case OrderKind::Fire:
            {
                // Fire the weapon now. "AIR": aim 60 m straight up over the shooter's own position first.
                if (order.value == "AIR")
                    code = "{_x doWatch [(getPos _x select 0), (getPos _x select 1), 60]} forEach _us; ";
                else if (hasPos)
                    code = "_us doWatch " + p + "; ";
                code += "{_x fire (primaryWeapon _x)} forEach _us";
                break;
            }
            case OrderKind::Stance: code = "{_x setUnitPos \"" + order.value + "\"} forEach _us"; break;
            case OrderKind::CombatMode: code = "_g setCombatMode \"" + order.value + "\""; break;
            case OrderKind::Behaviour: code = "_g setBehaviour \"" + order.value + "\""; break;
            case OrderKind::Formation: code = "_g setFormation \"" + order.value + "\""; break;
            case OrderKind::Speed: code = "_g setSpeedMode \"" + order.value + "\""; break;
            case OrderKind::GetIn:
            {
                Transport* veh = dyn_cast<Transport>(target);
                if (!veh)
                {
                    log.push_back(who + ": get in " + order.target + " -- no such vehicle");
                    continue;
                }
                // Exactly what the player's own "Get in" radio command does (InGameUI): seats are
                // assigned (driver first, the vehicle joins the group), then a GetIn group command goes
                // out. orderGetIn alone only sets a permission flag and nobody moves.
                bool canDriver = veh->Type()->HasDriver() && !veh->QIsDriverIn() && !veh->GetDriverAssigned();
                bool canCommander =
                    veh->Type()->HasCommander() && !veh->QIsCommanderIn() && !veh->GetCommanderAssigned();
                bool canGunner = veh->Type()->HasGunner() && !veh->QIsGunnerIn() && !veh->GetGunnerAssigned();
                PackedBoolArray list;
                for (AIUnit* unit : units)
                {
                    unit->AllowGetIn(true);
                    if (canDriver)
                    {
                        if (veh->GetGroupAssigned() != grp)
                            grp->AddVehicle(veh);
                        unit->AssignAsDriver(veh);
                        canDriver = false;
                    }
                    else if (canCommander)
                    {
                        unit->AssignAsCommander(veh);
                        canCommander = false;
                    }
                    else if (canGunner)
                    {
                        unit->AssignAsGunner(veh);
                        canGunner = false;
                    }
                    else if (unit->VehicleAssigned() != veh)
                    {
                        unit->AssignAsCargo(veh);
                    }
                    list.Set(unit->ID() - 1, true);
                }
                Command cmd;
                cmd._message = Command::GetIn;
                cmd._target = veh;
                cmd._context = Command::CtxUI;
                grp->SendCommand(cmd, list);
                log.push_back(who + ": get in " + order.target + " (" + std::string(veh->GetDisplayName()) + ")");
                continue;
            }
            case OrderKind::GetOut:
            {
                PackedBoolArray list;
                for (AIUnit* unit : units)
                {
                    unit->AllowGetIn(false);
                    list.Set(unit->ID() - 1, true);
                }
                Command cmd;
                cmd._message = Command::GetOut;
                cmd._context = Command::CtxUI;
                grp->SendCommand(cmd, list);
                log.push_back(who + ": get out");
                continue;
            }
            case OrderKind::LineUp:
            {
                // A real line, not a formation: each soldier walks to his own spot, 3 m apart, on a
                // line across the player's view (centred 8 m ahead, or on `where`), and faces the
                // player like at an inspection.
                const Vector3 fwd = player->Direction();
                const float heading = std::atan2(fwd.X(), fwd.Z());
                const Vector3 across(std::cos(heading), 0.0f, -std::sin(heading));
                const Vector3 centre = hasPos ? pos : player->Position() + Vector3(fwd.X(), 0.0f, fwd.Z()) * 8.0f;
                const float spacing = 3.0f;
                const float first = -0.5f * spacing * static_cast<float>(units.size() - 1);
                code.clear();
                for (size_t i = 0; i < units.size(); ++i)
                {
                    const Vector3 spot = centre + across * (first + spacing * static_cast<float>(i));
                    char line[192];
                    std::snprintf(line, sizeof(line), "(_us select %d) commandMove [%.1f,%.1f,0]; ",
                                  static_cast<int>(i), spot.X(), spot.Z());
                    code += line;
                }
                code += "_us commandWatch (getPos _p)";
                break;
            }
            default: continue;
        }

        GameArrayType array;
        for (AIUnit* unit : units)
            array.Add(GameValueExt(static_cast<Object*>(unit->GetPerson())));
        GameVarSpace space;
        gs->BeginContext(&space);
        GameValue unitsValue;
        unitsValue = array;
        gs->VarSetLocal("_us", unitsValue, false, true);
        gs->VarSetLocal("_p", GameValueExt(static_cast<Object*>(player)), true, true);
        gs->VarSetLocal("_g", GameValueExt(grp), true, true);
        if (target)
            gs->VarSetLocal("_t", GameValueExt(static_cast<Object*>(target)), true, true);
        gs->Execute(code.c_str());
        gs->EndContext();

        std::string summary = who + ": " + OrderKindName(order.kind);
        if (!order.value.empty())
            summary += " " + order.value;
        if (!order.target.empty())
            summary += " " + order.target;
        if (hasPos && order.kind != OrderKind::Follow && order.kind != OrderKind::Hold)
            summary += " -> " + DescribeOffset(player->Position(), pos);
        log.push_back(summary);
    }
    return log;
}

void PostChatLine(int registryId, const std::string& speakerId, const std::string& text, bool fromPlayer)
{
    if (!GWorld)
        return;
    const RString message(text.c_str());
    Person* player = PlayerPerson();
    if (fromPlayer)
    {
        if (player && player->Brain())
            GChatList.Add(CCGroup, player->Brain(), message, true, true);
        return;
    }
    if (speakerId == "HQ")
    {
        GChatList.Add(CCSide, RString("Command"), message, false, true);
        return;
    }
    if (speakerId == "ALL")
    {
        GChatList.Add(CCGroup, RString("Squad"), message, false, true);
        return;
    }
    const SpeakerInfo info = ResolveSpeaker(registryId, speakerId);
    AIUnit* sender = nullptr;
    if (info.valid)
    {
        if (info.inSquad)
        {
            AIGroup* grp = player && player->Brain() ? player->Brain()->GetGroup() : nullptr;
            sender = SquadMember(grp, info.id);
        }
        else if (Registry* reg = FindRegistry(registryId))
        {
            if (Person* person = FindLinked(reg->people, speakerId))
                sender = person->Brain();
        }
    }
    if (sender)
        GChatList.Add(info.inSquad ? CCGroup : CCDirect, sender, message, false, true);
    else
        GChatList.Add(CCDirect, RString(info.name.empty() ? speakerId.c_str() : info.name.c_str()), message, false,
                      true);
}

} // namespace Poseidon::NpcChat
